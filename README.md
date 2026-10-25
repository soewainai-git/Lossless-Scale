# LowScale

Utilitas upscaling + frame generation 2x untuk game Windows, mirip Lossless Scaling.
Dibuat untuk GPU kelas GTX 1650 Ti (Direct3D 11, tanpa fitur khusus RTX).

**Status: kode ini belum pernah dikompilasi atau dijalankan oleh pembuatnya** (tidak ada Windows di
lingkungan pembuatan). Bagian di luar WinRT sudah lolos pemeriksaan sintaks, tapi kemungkinan ada
error kompilasi atau bug saat dijalankan. Kirim pesan errornya ke Claude untuk diperbaiki.

## Cara kerja
1. Menangkap jendela game lewat Windows Graphics Capture (sama seperti OBS).
2. Upscale Catmull-Rom + sharpen adaptif (ala AMD CAS) di GPU.
3. Frame generation: block-matching motion estimation, lalu frame tengah diinterpolasi.
4. Hasil ditampilkan di jendela overlay tembus-klik di atas game.

Tidak ada inject DLL, tidak membaca/menulis memori game, tidak mengubah file game.

## Build

**Opsi A, di PC kamu:** pasang "Build Tools for Visual Studio 2022" (gratis), centang
*Desktop development with C++* dan *Windows 11 SDK*. Buka "x64 Native Tools Command Prompt for VS 2022",
masuk ke folder ini, jalankan `build.bat`. Hasilnya `lowscale.exe`.

**Opsi B, tanpa install apa pun:** upload folder ini ke repo GitHub. Tab *Actions* akan
mengompilasi otomatis, lalu unduh `lowscale.exe` dari bagian *Artifacts*.

## Cara pakai
1. Jalankan `lowscale.exe` (jendela console tetap terbuka).
2. Jalankan game dalam mode **Windowed atau Borderless**. Exclusive Fullscreen tidak didukung.
3. Klik jendela game, lalu tekan **Ctrl+Alt+S**.

| Hotkey | Fungsi |
|---|---|
| Ctrl+Alt+S | mulai/hentikan scaling pada jendela aktif |
| Ctrl+Alt+F | frame generation ON/OFF |
| Ctrl+Alt+Up / Down | sharpness naik/turun |
| Ctrl+Alt+Q | keluar |

## Pengaturan yang disarankan (GTX 1650 Ti)
- **Frame generation:** batasi FPS game ke **setengah refresh rate** layar. Contoh: layar 144 Hz, batasi game
  72 FPS. Layar 60 Hz, batasi 30 FPS (hasilnya terasa 60). Tanpa batas FPS, hasilnya patah-patah.
- Frame generation butuh basis FPS yang stabil (minimal sekitar 30). Di bawah itu latency dan artefak terasa.
- **Upscale:** set game ke resolusi lebih rendah (misal 1280x720 di layar 1080p) dalam mode Windowed, lalu Ctrl+Alt+S.
- Di Windows: *Settings > System > Display > Graphics*, tambahkan `lowscale.exe` dan pilih **High performance**
  supaya memakai GTX 1650 Ti, bukan GPU Intel.
- Frame generation memakai GPU yang sama dengan game, jadi turunkan setting grafis game sedikit untuk memberi ruang.

## Batasan yang diketahui
- Saat **upscale** (bukan native), kursor mouse dikunci ke area jendela game kecil. Gambar yang diperbesar tetap
  menunjukkan kursor game di posisi yang benar, tapi kursor Windows asli ikut terlihat di pojok. Untuk game yang
  banyak memakai mouse, pakai mode native (resolusi game = resolusi layar) dengan frame generation saja.
- Elemen UI/HUD yang bergerak cepat bisa menimbulkan artefak (ghosting) di frame hasil interpolasi.
- Gerakan lebih dari sekitar 12 piksel per frame (di resolusi penuh) tidak terdeteksi dengan baik.
- Konten HDR belum didukung.

## Soal anti-cheat
Secara desain program ini memakai cara yang sama dengan Lossless Scaling (menangkap jendela dan menggambar di overlay),
jadi tidak menyentuh proses game. Tapi **tidak ada jaminan**:
- War Thunder memakai BattlEye (kernel) plus anti-cheat internal, dan Heartopia belum diketahui sistemnya.
- Program buatan sendiri belum dikenal vendor anti-cheat, sehingga bisa saja terdeteksi sebagai false positive.
- Pakai dengan risiko sendiri, dan jangan dipakai di akun yang tidak boleh kamu risikokan.
  Untuk game kompetitif, pertimbangkan memakai Lossless Scaling asli yang sudah dipakai banyak orang.
