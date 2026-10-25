// LowScale - upscaler + frame generation overlay (mirip Lossless Scaling), Windows 10 2004+ / Windows 11.
//
// Cara kerja (sama seperti OBS): jendela game ditangkap lewat Windows Graphics Capture (WGC),
// diproses di GPU (upscale Catmull-Rom + sharpen CAS + frame generation 2x berbasis motion estimation),
// lalu ditampilkan di jendela overlay "click-through" di atas game.
// Program TIDAK inject DLL, TIDAK membaca/menulis memori game, TIDAK mengubah file game.
//
// Build (Developer Command Prompt for VS):  build.bat
//
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <unknwn.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

// Minta Windows memakai GPU NVIDIA/AMD (bukan iGPU) di laptop hybrid.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

// ------------------------------------------------------------------------------------------------
// HLSL
// ------------------------------------------------------------------------------------------------
static const char* kHlslCommon = R"HLSL(
cbuffer CB : register(b0) { float4 p0; float4 p1; };
SamplerState lin : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
)HLSL";

// Luma setengah resolusi (rata-rata 2x2 lewat bilinear)
static const char* kHlslLuma = R"HLSL(
Texture2D<float4> srcT : register(t0);
float4 PSLuma(VSOut i) : SV_Target
{
    float3 c = srcT.SampleLevel(lin, i.uv, 0).rgb;
    float l = dot(c, float3(0.299, 0.587, 0.114));
    return float4(l, l, l, 1.0);
}
)HLSL";

// Motion estimation: block matching 8x8 (pada luma setengah resolusi), pencarian penuh +-R.
// Output: (mv.x, mv.y dalam piksel resolusi penuh, rata-rata error, 1). mv = lokasi blok cur di frame prev.
static const char* kHlslME = R"HLSL(
Texture2D<float> lumaCur : register(t0);
Texture2D<float> lumaPrev : register(t1);
float4 PSME(VSOut i) : SV_Target
{
    int2 lsz = int2((int)p0.x, (int)p0.y);
    int R = (int)p0.z;
    float lambda = p0.w;
    int2 base = int2((int)i.pos.x, (int)i.pos.y) * 8;

    float cur[32];
    [unroll] for (int y = 0; y < 8; y++)
    {
        [unroll] for (int xi = 0; xi < 4; xi++)
        {
            int2 q = min(base + int2(xi * 2 + (y & 1), y), lsz - int2(1, 1));
            cur[y * 4 + xi] = lumaCur.Load(int3(q, 0));
        }
    }

    float bestCost = 1e30;
    float bestSad = 0.0;
    int2 bestMv = int2(0, 0);
    [loop] for (int dy = -R; dy <= R; dy++)
    {
        [loop] for (int dx = -R; dx <= R; dx++)
        {
            float sad = 0.0;
            [unroll] for (int y2 = 0; y2 < 8; y2++)
            {
                [unroll] for (int xj = 0; xj < 4; xj++)
                {
                    int2 q2 = clamp(base + int2(xj * 2 + (y2 & 1) + dx, y2 + dy), int2(0, 0), lsz - int2(1, 1));
                    sad += abs(cur[y2 * 4 + xj] - lumaPrev.Load(int3(q2, 0)));
                }
            }
            float cost = sad + lambda * (float)(abs(dx) + abs(dy));
            if (cost < bestCost)
            {
                bestCost = cost;
                bestSad = sad;
                bestMv = int2(dx, dy);
            }
        }
    }
    return float4((float)bestMv.x * 2.0, (float)bestMv.y * 2.0, bestSad / 32.0, 1.0);
}
)HLSL";

// Interpolasi frame tengah: untuk tiap piksel coba vektor dari 4 blok terdekat + nol, pilih yang errornya terkecil.
static const char* kHlslInterp = R"HLSL(
Texture2D<float4> curT : register(t0);
Texture2D<float4> prevT : register(t1);
Texture2D<float4> mvT : register(t2);

void Eval(float2 mv, float pen, float2 uv, float2 inv, inout float bestErr, inout float3 bestCol)
{
    float3 a = prevT.SampleLevel(lin, uv + mv * 0.5 * inv, 0).rgb;
    float3 c = curT.SampleLevel(lin, uv - mv * 0.5 * inv, 0).rgb;
    float3 d = abs(a - c);
    float err = d.r + d.g + d.b + pen;
    if (err < bestErr)
    {
        bestErr = err;
        bestCol = 0.5 * (a + c);
    }
}

float2 GetMv(int2 b, int2 bcount)
{
    return mvT.Load(int3(clamp(b, int2(0, 0), bcount - int2(1, 1)), 0)).xy;
}

float4 PSInterp(VSOut i) : SV_Target
{
    float2 srcSize = p0.xy;
    int2 bcount = int2((int)p0.z, (int)p0.w);
    float blockPx = p1.x;
    float2 inv = 1.0 / srcSize;
    float2 p = i.pos.xy;
    float2 uv = p * inv;

    float2 bc = p / blockPx - 0.5;
    float2 fl = floor(bc);
    float2 fr = bc - fl;
    int2 b0 = int2((int)fl.x, (int)fl.y);
    int2 nearB = b0 + int2(fr.x >= 0.5 ? 1 : 0, fr.y >= 0.5 ? 1 : 0);
    int2 farB = b0 + int2(fr.x >= 0.5 ? 0 : 1, fr.y >= 0.5 ? 0 : 1);

    float bestErr = 1e9;
    float3 bestCol = float3(0, 0, 0);
    Eval(GetMv(nearB, bcount), 0.0, uv, inv, bestErr, bestCol);
    Eval(GetMv(int2(farB.x, nearB.y), bcount), 0.03, uv, inv, bestErr, bestCol);
    Eval(GetMv(int2(nearB.x, farB.y), bcount), 0.03, uv, inv, bestErr, bestCol);
    Eval(GetMv(farB, bcount), 0.03, uv, inv, bestErr, bestCol);
    Eval(float2(0, 0), 0.02, uv, inv, bestErr, bestCol);
    return float4(bestCol, 1.0);
}
)HLSL";

// Upscale Catmull-Rom (9 tap bilinear)
static const char* kHlslCubic = R"HLSL(
Texture2D<float4> srcT : register(t0);
float4 PSCubic(VSOut i) : SV_Target
{
    float2 texSize = p0.xy;
    float2 samplePos = i.uv * texSize;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / w12;
    float2 q0 = (texPos1 - 1.0) / texSize;
    float2 q3 = (texPos1 + 2.0) / texSize;
    float2 q12 = (texPos1 + offset12) / texSize;

    float4 r = float4(0, 0, 0, 0);
    r += srcT.SampleLevel(lin, float2(q0.x, q0.y), 0) * w0.x * w0.y;
    r += srcT.SampleLevel(lin, float2(q12.x, q0.y), 0) * w12.x * w0.y;
    r += srcT.SampleLevel(lin, float2(q3.x, q0.y), 0) * w3.x * w0.y;
    r += srcT.SampleLevel(lin, float2(q0.x, q12.y), 0) * w0.x * w12.y;
    r += srcT.SampleLevel(lin, float2(q12.x, q12.y), 0) * w12.x * w12.y;
    r += srcT.SampleLevel(lin, float2(q3.x, q12.y), 0) * w3.x * w12.y;
    r += srcT.SampleLevel(lin, float2(q0.x, q3.y), 0) * w0.x * w3.y;
    r += srcT.SampleLevel(lin, float2(q12.x, q3.y), 0) * w12.x * w3.y;
    r += srcT.SampleLevel(lin, float2(q3.x, q3.y), 0) * w3.x * w3.y;
    return float4(saturate(r.rgb), 1.0);
}
)HLSL";

// Sharpen adaptif ala AMD CAS
static const char* kHlslCAS = R"HLSL(
Texture2D<float4> inT : register(t0);
float3 ld(int2 q, int2 sz)
{
    return inT.Load(int3(clamp(q, int2(0, 0), sz - int2(1, 1)), 0)).rgb;
}
float4 PSCAS(VSOut i) : SV_Target
{
    int2 q = int2((int)i.pos.x, (int)i.pos.y) - int2((int)p0.x, (int)p0.y);
    int2 sz = int2((int)p0.z, (int)p0.w);
    float sharp = p1.x;
    float3 e = ld(q, sz);
    if (sharp <= 0.001) return float4(e, 1.0);
    float3 b = ld(q + int2(0, -1), sz);
    float3 d = ld(q + int2(-1, 0), sz);
    float3 f = ld(q + int2(1, 0), sz);
    float3 h = ld(q + int2(0, 1), sz);
    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mx = max(max(max(d, e), max(f, b)), h);
    float3 amp = saturate(min(mn, 1.0 - mx) / max(mx, 0.0001));
    amp = sqrt(amp);
    float peak = -1.0 / lerp(8.0, 5.0, sharp);
    float3 w = amp * peak;
    float3 rw = 1.0 / (1.0 + 4.0 * w);
    float3 col = saturate((w * (b + d + f + h) + e) * rw);
    return float4(col, 1.0);
}
)HLSL";

// ------------------------------------------------------------------------------------------------
// State global
// ------------------------------------------------------------------------------------------------
enum { HK_TOGGLE = 1, HK_FG, HK_SHARP_UP, HK_SHARP_DOWN, HK_QUIT };

static std::atomic<bool> g_running{true};
static bool g_fg = true;            // frame generation aktif
static float g_sharp = 0.5f;        // 0..1
static bool g_sharpTouched = false; // kalau resolusi native, sharpen hanya jika user mengaturnya
static int g_meRange = 6;           // jangkauan pencarian gerak (piksel luma setengah-res => +-12 px layar)

static ComPtr<ID3D11Device> g_dev;
static ComPtr<ID3D11DeviceContext> g_ctx;
static wgd3d::IDirect3DDevice g_winrtDev{nullptr};
static ComPtr<ID3D11VertexShader> g_vs;
static ComPtr<ID3D11PixelShader> g_psLuma, g_psME, g_psInterp, g_psCubic, g_psCAS;
static ComPtr<ID3D11SamplerState> g_samp;
static ComPtr<ID3D11RasterizerState> g_rs;
static ComPtr<ID3D11Buffer> g_cb;

struct Tex {
    ComPtr<ID3D11Texture2D> t;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};

struct Session {
    HWND target = nullptr;
    HWND overlay = nullptr;
    int W = 0, H = 0;
    int monX = 0, monY = 0, monW = 0, monH = 0;
    int dw = 0, dh = 0, offX = 0, offY = 0;
    bool needScale = false;
    ComPtr<IDXGISwapChain1> sc;
    ComPtr<ID3D11RenderTargetView> bbRTV;

    Tex ring[4];
    Tex luma[2];
    Tex interp, mid, mv;
    int lumaW = 0, lumaH = 0, bw = 0, bh = 0;

    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession sess{nullptr};
    std::thread th;
    std::atomic<bool> stop{false};
    std::atomic<bool> needRestart{false};
    HANDLE evt = nullptr;

    std::mutex mu;
    int prev = -1, cur = -1, pending = -1;

    int lumaIdx = 0;
    int lumaFrames = 0;
    bool visible = true;
    bool clipped = false;
};
static std::unique_ptr<Session> S;

static std::string Narrow(const wchar_t* w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// ------------------------------------------------------------------------------------------------
// D3D helper
// ------------------------------------------------------------------------------------------------
static ComPtr<ID3DBlob> CompileShader(const std::string& src, const char* entry, const char* target)
{
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(src.data(), src.size(), "lowscale.hlsl", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &blob, &err);
    if (FAILED(hr)) {
        std::printf("[ERROR] Shader '%s' gagal dikompilasi:\n%s\n", entry,
                    err ? static_cast<const char*>(err->GetBufferPointer()) : "(tanpa pesan)");
        return nullptr;
    }
    return blob;
}

static bool MakePS(const char* body, const char* entry, ComPtr<ID3D11PixelShader>& out)
{
    std::string src = std::string(kHlslCommon) + body;
    auto blob = CompileShader(src, entry, "ps_5_0");
    if (!blob) return false;
    return SUCCEEDED(g_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out));
}

static bool InitDevice()
{
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory6> f6;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(f6.GetAddressOf())))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                       reinterpret_cast<void**>(adapter.GetAddressOf()));
    }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION,
                                   &g_dev, nullptr, &g_ctx);
    if (FAILED(hr)) {
        std::printf("[ERROR] Gagal membuat D3D11 device (0x%08X)\n", (unsigned)hr);
        return false;
    }
    ComPtr<IDXGIDevice> dxgiDev;
    g_dev.As(&dxgiDev);
    ComPtr<IDXGIAdapter> a;
    dxgiDev->GetAdapter(&a);
    DXGI_ADAPTER_DESC ad{};
    a->GetDesc(&ad);
    std::printf("GPU dipakai: %s\n", Narrow(ad.Description).c_str());

    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(g_ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);

    winrt::com_ptr<::IInspectable> insp;
    winrt::check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.Get(), insp.put()));
    g_winrtDev = insp.as<wgd3d::IDirect3DDevice>();

    // shader
    {
        std::string src = std::string(kHlslCommon);
        auto vsb = CompileShader(src, "VSMain", "vs_5_0");
        if (!vsb) return false;
        if (FAILED(g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs))) return false;
    }
    std::printf("Mengompilasi shader...\n");
    if (!MakePS(kHlslLuma, "PSLuma", g_psLuma)) return false;
    if (!MakePS(kHlslME, "PSME", g_psME)) return false;
    if (!MakePS(kHlslInterp, "PSInterp", g_psInterp)) return false;
    if (!MakePS(kHlslCubic, "PSCubic", g_psCubic)) return false;
    if (!MakePS(kHlslCAS, "PSCAS", g_psCAS)) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_dev->CreateSamplerState(&sd, &g_samp))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(g_dev->CreateRasterizerState(&rd, &g_rs))) return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_cb))) return false;

    // state tetap
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    ID3D11SamplerState* ss[1] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, ss);
    g_ctx->RSSetState(g_rs.Get());
    ID3D11Buffer* cbs[1] = {g_cb.Get()};
    g_ctx->PSSetConstantBuffers(0, 1, cbs);
    return true;
}

static bool MakeTex(Tex& o, int w, int h, DXGI_FORMAT fmt, bool rtv)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv ? D3D11_BIND_RENDER_TARGET : 0);
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &o.t))) return false;
    if (FAILED(g_dev->CreateShaderResourceView(o.t.Get(), nullptr, &o.srv))) return false;
    if (rtv && FAILED(g_dev->CreateRenderTargetView(o.t.Get(), nullptr, &o.rtv))) return false;
    return true;
}

static void SetCB(float a, float b, float c, float d, float e = 0, float f = 0, float g = 0, float h = 0)
{
    float v[8] = {a, b, c, d, e, f, g, h};
    g_ctx->UpdateSubresource(g_cb.Get(), 0, nullptr, v, 0, 0);
}

static void Pass(ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, int vx, int vy, int vw, int vh,
                 std::initializer_list<ID3D11ShaderResourceView*> srvs)
{
    D3D11_VIEWPORT vp{(float)vx, (float)vy, (float)vw, (float)vh, 0.0f, 1.0f};
    g_ctx->RSSetViewports(1, &vp);
    ID3D11RenderTargetView* r[1] = {rtv};
    g_ctx->OMSetRenderTargets(1, r, nullptr);
    g_ctx->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView* arr[4] = {};
    int n = 0;
    for (auto s : srvs) arr[n++] = s;
    g_ctx->PSSetShaderResources(0, 4, arr);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none[4] = {};
    g_ctx->PSSetShaderResources(0, 4, none);
}

// ------------------------------------------------------------------------------------------------
// Overlay window
// ------------------------------------------------------------------------------------------------
static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

static HWND CreateOverlay(int x, int y, int w, int h)
{
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LowScaleOverlay";
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        RegisterClassExW(&wc);
        reg = true;
    }
    HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                              L"LowScaleOverlay", L"LowScale", WS_POPUP, x, y, w, h, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (hw) SetLayeredWindowAttributes(hw, 0, 255, LWA_ALPHA);
    return hw;
}

static void ClipToTarget(Session& s, bool on)
{
    if (!on) {
        if (s.clipped) ClipCursor(nullptr);
        s.clipped = false;
        return;
    }
    if (!s.needScale) return;  // hanya perlu saat di-upscale
    RECT cr{};
    GetClientRect(s.target, &cr);
    POINT a{cr.left, cr.top}, b{cr.right, cr.bottom};
    ClientToScreen(s.target, &a);
    ClientToScreen(s.target, &b);
    RECT r{a.x, a.y, b.x, b.y};
    ClipCursor(&r);
    s.clipped = true;
}

// ------------------------------------------------------------------------------------------------
// Capture thread
// ------------------------------------------------------------------------------------------------
static void CaptureThread(Session* s)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (!s->stop.load()) {
        wgc::Direct3D11CaptureFrame frame{nullptr};
        try {
            frame = s->pool.TryGetNextFrame();
        } catch (...) {
            Sleep(2);
            continue;
        }
        if (!frame) {
            Sleep(1);
            continue;
        }
        try {
            auto csz = frame.ContentSize();

            RECT ext{};
            if (FAILED(DwmGetWindowAttribute(s->target, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext)))) {
                GetWindowRect(s->target, &ext);
            }
            POINT tl{0, 0};
            ClientToScreen(s->target, &tl);
            RECT cr{};
            GetClientRect(s->target, &cr);
            int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
            if (cw != s->W || ch != s->H || cw > csz.Width || ch > csz.Height) {
                s->needRestart = true;
                frame.Close();
                Sleep(30);
                continue;
            }
            int ox = std::max(0, (int)(tl.x - ext.left));
            int oy = std::max(0, (int)(tl.y - ext.top));
            if (ox + cw > csz.Width) ox = csz.Width - cw;
            if (oy + ch > csz.Height) oy = csz.Height - ch;

            auto surface = frame.Surface();
            auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> ft;
            winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(ft.GetAddressOf())));

            int slot = 0;
            {
                std::lock_guard<std::mutex> lk(s->mu);
                for (int k = 0; k < 4; k++) {
                    if (k != s->prev && k != s->cur && k != s->pending) {
                        slot = k;
                        break;
                    }
                }
            }
            D3D11_BOX box{(UINT)ox, (UINT)oy, 0, (UINT)(ox + cw), (UINT)(oy + ch), 1};
            g_ctx->CopySubresourceRegion(s->ring[slot].t.Get(), 0, 0, 0, 0, ft.Get(), 0, &box);
            {
                std::lock_guard<std::mutex> lk(s->mu);
                s->pending = slot;
            }
            SetEvent(s->evt);
        } catch (...) {
            // frame rusak / device hilang: abaikan, coba lagi
        }
        try { frame.Close(); } catch (...) {}
    }
}

static void StopCapture(Session& s)
{
    s.stop = true;
    if (s.th.joinable()) s.th.join();
    try {
        if (s.sess) s.sess.Close();
        if (s.pool) s.pool.Close();
    } catch (...) {}
    s.sess = nullptr;
    s.pool = nullptr;
    s.item = nullptr;
}

// ------------------------------------------------------------------------------------------------
// Start / Stop scaling
// ------------------------------------------------------------------------------------------------
static void StopScaling()
{
    if (!S) return;
    ClipToTarget(*S, false);
    StopCapture(*S);
    if (S->bbRTV) S->bbRTV.Reset();
    if (S->sc) S->sc.Reset();
    if (S->overlay) DestroyWindow(S->overlay);
    if (S->evt) CloseHandle(S->evt);
    if (g_ctx) {
        g_ctx->ClearState();
        // state tetap diset ulang di StartScaling
    }
    S.reset();
    std::printf("Scaling dihentikan.\n");
}

static bool IsShellWindow(HWND h)
{
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 63);
    return !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
           !wcscmp(cls, L"Shell_SecondaryTrayWnd");
}

static void ReapplyFixedState()
{
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    ID3D11SamplerState* ss[1] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, ss);
    g_ctx->RSSetState(g_rs.Get());
    ID3D11Buffer* cbs[1] = {g_cb.Get()};
    g_ctx->PSSetConstantBuffers(0, 1, cbs);
}

static bool StartScaling(HWND target)
{
    if (S) StopScaling();
    if (!target || !IsWindow(target) || IsIconic(target) || !IsWindowVisible(target) || IsShellWindow(target) ||
        target == GetConsoleWindow()) {
        std::printf("Jendela aktif tidak valid. Fokuskan game (mode Windowed/Borderless) lalu tekan Ctrl+Alt+S.\n");
        return false;
    }
    auto s = std::make_unique<Session>();
    s->target = target;

    RECT cr{};
    GetClientRect(target, &cr);
    s->W = cr.right - cr.left;
    s->H = cr.bottom - cr.top;
    if (s->W < 64 || s->H < 64) {
        std::printf("Ukuran jendela terlalu kecil.\n");
        return false;
    }

    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &mi);
    s->monX = mi.rcMonitor.left;
    s->monY = mi.rcMonitor.top;
    s->monW = mi.rcMonitor.right - mi.rcMonitor.left;
    s->monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    float sc = std::min((float)s->monW / s->W, (float)s->monH / s->H);
    if (std::fabs(sc - 1.0f) < 0.002f) {
        s->dw = s->W;
        s->dh = s->H;
        s->needScale = false;
    } else {
        s->dw = std::min(s->monW, (int)std::lround(s->W * sc));
        s->dh = std::min(s->monH, (int)std::lround(s->H * sc));
        s->needScale = true;
    }
    s->offX = (s->monW - s->dw) / 2;
    s->offY = (s->monH - s->dh) / 2;

    s->lumaW = (s->W + 1) / 2;
    s->lumaH = (s->H + 1) / 2;
    s->bw = (s->lumaW + 7) / 8;
    s->bh = (s->lumaH + 7) / 8;

    // texture
    bool ok = true;
    for (int i = 0; i < 4; i++) ok = ok && MakeTex(s->ring[i], s->W, s->H, DXGI_FORMAT_B8G8R8A8_UNORM, false);
    for (int i = 0; i < 2; i++) ok = ok && MakeTex(s->luma[i], s->lumaW, s->lumaH, DXGI_FORMAT_R8_UNORM, true);
    ok = ok && MakeTex(s->interp, s->W, s->H, DXGI_FORMAT_B8G8R8A8_UNORM, true);
    ok = ok && MakeTex(s->mv, s->bw, s->bh, DXGI_FORMAT_R32G32B32A32_FLOAT, true);
    if (s->needScale) ok = ok && MakeTex(s->mid, s->dw, s->dh, DXGI_FORMAT_B8G8R8A8_UNORM, true);
    if (!ok) {
        std::printf("[ERROR] Gagal membuat texture.\n");
        return false;
    }

    // overlay + swap chain
    s->overlay = CreateOverlay(s->monX, s->monY, s->monW, s->monH);
    if (!s->overlay) {
        std::printf("[ERROR] Gagal membuat jendela overlay.\n");
        return false;
    }
    {
        ComPtr<IDXGIDevice> dxgiDev;
        g_dev.As(&dxgiDev);
        ComPtr<IDXGIAdapter> ad;
        dxgiDev->GetAdapter(&ad);
        ComPtr<IDXGIFactory2> fac;
        ad->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(fac.GetAddressOf()));

        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = (UINT)s->monW;
        d.Height = (UINT)s->monH;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = fac->CreateSwapChainForHwnd(g_dev.Get(), s->overlay, &d, nullptr, nullptr, &s->sc);
        if (FAILED(hr)) {
            d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;  // cadangan
            d.BufferCount = 1;
            hr = fac->CreateSwapChainForHwnd(g_dev.Get(), s->overlay, &d, nullptr, nullptr, &s->sc);
        }
        if (FAILED(hr)) {
            std::printf("[ERROR] Gagal membuat swap chain (0x%08X)\n", (unsigned)hr);
            DestroyWindow(s->overlay);
            return false;
        }
        fac->MakeWindowAssociation(s->overlay, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<ID3D11Texture2D> bb;
        s->sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(bb.GetAddressOf()));
        g_dev->CreateRenderTargetView(bb.Get(), nullptr, &s->bbRTV);
    }

    // capture
    try {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(target, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                      winrt::put_abi(s->item)));
        s->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            g_winrtDev, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, s->item.Size());
        s->sess = s->pool.CreateCaptureSession(s->item);
        try { s->sess.IsCursorCaptureEnabled(true); } catch (...) {}
        try { s->sess.IsBorderRequired(false); } catch (...) {}
        s->evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        s->th = std::thread(CaptureThread, s.get());
        s->sess.StartCapture();
    } catch (winrt::hresult_error const& e) {
        std::printf("[ERROR] Gagal memulai capture: %s\n", Narrow(e.message().c_str()).c_str());
        s->stop = true;
        if (s->th.joinable()) s->th.join();
        DestroyWindow(s->overlay);
        return false;
    }

    ReapplyFixedState();
    ShowWindow(s->overlay, SW_SHOWNOACTIVATE);
    SetWindowPos(s->overlay, HWND_TOPMOST, s->monX, s->monY, s->monW, s->monH, SWP_NOACTIVATE);
    S = std::move(s);
    ClipToTarget(*S, true);

    wchar_t title[128] = {};
    GetWindowTextW(target, title, 127);
    std::printf("Scaling aktif: \"%s\"  %dx%d -> %dx%d  (%s)  FrameGen: %s\n", Narrow(title).c_str(), S->W, S->H,
                S->dw, S->dh, S->needScale ? "upscale" : "native", g_fg ? "ON" : "OFF");
    return true;
}

// ------------------------------------------------------------------------------------------------
// Render
// ------------------------------------------------------------------------------------------------
static void ShowTexture(ID3D11ShaderResourceView* src)
{
    Session& s = *S;
    const float clr[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(s.bbRTV.Get(), clr);

    ID3D11ShaderResourceView* casIn = src;
    if (s.needScale) {
        SetCB((float)s.W, (float)s.H, 0, 0);
        Pass(g_psCubic.Get(), s.mid.rtv.Get(), 0, 0, s.dw, s.dh, {src});
        casIn = s.mid.srv.Get();
    }
    float sharp = (s.needScale || g_sharpTouched) ? g_sharp : 0.0f;
    SetCB((float)s.offX, (float)s.offY, (float)s.dw, (float)s.dh, sharp);
    Pass(g_psCAS.Get(), s.bbRTV.Get(), s.offX, s.offY, s.dw, s.dh, {casIn});
    s.sc->Present(1, 0);
}

static void OnNewFrame()
{
    Session& s = *S;
    int prev, cur;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        prev = s.prev;
        cur = s.cur;
    }
    if (cur < 0) return;

    if (g_fg && s.visible) {
        s.lumaIdx ^= 1;
        SetCB(0, 0, 0, 0);
        Pass(g_psLuma.Get(), s.luma[s.lumaIdx].rtv.Get(), 0, 0, s.lumaW, s.lumaH, {s.ring[cur].srv.Get()});
        s.lumaFrames++;
        if (s.lumaFrames >= 2 && prev >= 0) {
            // motion estimation
            SetCB((float)s.lumaW, (float)s.lumaH, (float)g_meRange, 0.015f);
            Pass(g_psME.Get(), s.mv.rtv.Get(), 0, 0, s.bw, s.bh,
                 {s.luma[s.lumaIdx].srv.Get(), s.luma[s.lumaIdx ^ 1].srv.Get()});
            // interpolasi
            SetCB((float)s.W, (float)s.H, (float)s.bw, (float)s.bh, 16.0f);
            Pass(g_psInterp.Get(), s.interp.rtv.Get(), 0, 0, s.W, s.H,
                 {s.ring[cur].srv.Get(), s.ring[prev].srv.Get(), s.mv.srv.Get()});
            ShowTexture(s.interp.srv.Get());
        }
    } else {
        s.lumaFrames = 0;
    }
    if (s.visible) ShowTexture(s.ring[cur].srv.Get());
}

// ------------------------------------------------------------------------------------------------
// Hotkey + main loop
// ------------------------------------------------------------------------------------------------
static void HandleHotkey(int id)
{
    switch (id) {
    case HK_TOGGLE:
        if (S) StopScaling();
        else StartScaling(GetForegroundWindow());
        break;
    case HK_FG:
        g_fg = !g_fg;
        if (S) S->lumaFrames = 0;
        std::printf("Frame generation: %s\n", g_fg ? "ON" : "OFF");
        break;
    case HK_SHARP_UP:
    case HK_SHARP_DOWN:
        g_sharp = std::min(1.0f, std::max(0.0f, g_sharp + (id == HK_SHARP_UP ? 0.1f : -0.1f)));
        g_sharpTouched = true;
        std::printf("Sharpness: %.1f\n", g_sharp);
        break;
    case HK_QUIT:
        g_running = false;
        break;
    }
}

static BOOL WINAPI ConsoleHandler(DWORD)
{
    ClipCursor(nullptr);
    g_running = false;
    return TRUE;
}

int main()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    timeBeginPeriod(1);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    std::printf("=== LowScale ===\n");
    if (!wgc::GraphicsCaptureSession::IsSupported()) {
        std::printf("Windows Graphics Capture tidak didukung (butuh Windows 10 versi 2004 / Windows 11).\n");
        return 1;
    }
    try {
        if (!InitDevice()) return 1;
    } catch (winrt::hresult_error const& e) {
        std::printf("[ERROR] Inisialisasi gagal: %s\n", Narrow(e.message().c_str()).c_str());
        return 1;
    }

    const UINT mod = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    bool hk = RegisterHotKey(nullptr, HK_TOGGLE, mod, 'S') && RegisterHotKey(nullptr, HK_FG, mod, 'F') &&
              RegisterHotKey(nullptr, HK_SHARP_UP, mod, VK_UP) && RegisterHotKey(nullptr, HK_SHARP_DOWN, mod, VK_DOWN) &&
              RegisterHotKey(nullptr, HK_QUIT, mod, 'Q');
    if (!hk) std::printf("[PERINGATAN] Ada hotkey yang gagal didaftarkan (dipakai program lain).\n");

    std::printf("\nHotkey:\n"
                "  Ctrl+Alt+S     mulai/hentikan scaling pada jendela yang sedang aktif\n"
                "  Ctrl+Alt+F     frame generation ON/OFF\n"
                "  Ctrl+Alt+Up/Dn sharpness +/-\n"
                "  Ctrl+Alt+Q     keluar\n\n"
                "Jalankan game dalam mode Windowed / Borderless (bukan Exclusive Fullscreen),\n"
                "klik jendela game supaya aktif, lalu tekan Ctrl+Alt+S.\n\n");

    ULONGLONG lastCheck = 0, lastRestart = 0;
    MSG msg;
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            if (msg.message == WM_HOTKEY) {
                HandleHotkey((int)msg.wParam);
            } else {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (!g_running) break;

        if (!S) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            continue;
        }

        ULONGLONG now = GetTickCount64();
        if (now - lastCheck > 250) {
            lastCheck = now;
            if (!IsWindow(S->target) || IsIconic(S->target)) {
                std::printf("Jendela game ditutup/diminimize.\n");
                StopScaling();
                continue;
            }
            if (S->needRestart && now - lastRestart > 1000) {
                lastRestart = now;
                HWND t = S->target;
                std::printf("Ukuran jendela berubah, memulai ulang...\n");
                StopScaling();
                StartScaling(t);
                continue;
            }
            HWND fg = GetForegroundWindow();
            bool want = (fg == S->target || fg == S->overlay);
            if (want != S->visible) {
                S->visible = want;
                if (want) {
                    ShowWindow(S->overlay, SW_SHOWNOACTIVATE);
                    SetWindowPos(S->overlay, HWND_TOPMOST, S->monX, S->monY, S->monW, S->monH, SWP_NOACTIVATE);
                    ClipToTarget(*S, true);
                } else {
                    ShowWindow(S->overlay, SW_HIDE);
                    ClipToTarget(*S, false);
                }
            }
        }

        DWORD r = MsgWaitForMultipleObjects(1, &S->evt, FALSE, 8, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) {
            bool got = false;
            {
                std::lock_guard<std::mutex> lk(S->mu);
                if (S->pending >= 0) {
                    S->prev = S->cur;
                    S->cur = S->pending;
                    S->pending = -1;
                    got = true;
                }
            }
            if (got) OnNewFrame();
        }
    }

    StopScaling();
    UnregisterHotKey(nullptr, HK_TOGGLE);
    UnregisterHotKey(nullptr, HK_FG);
    UnregisterHotKey(nullptr, HK_SHARP_UP);
    UnregisterHotKey(nullptr, HK_SHARP_DOWN);
    UnregisterHotKey(nullptr, HK_QUIT);
    g_winrtDev = nullptr;
    timeEndPeriod(1);
    return 0;
}
