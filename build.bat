@echo off
REM Jalankan dari "x64 Native Tools Command Prompt for VS 2022" (atau Developer Command Prompt).
cl /nologo /std:c++17 /EHsc /O2 /W3 /DUNICODE /D_UNICODE lowscale.cpp /Fe:lowscale.exe /link /SUBSYSTEM:CONSOLE
if errorlevel 1 (
  echo.
  echo BUILD GAGAL. Salin pesan error di atas dan kirim ke Claude.
  exit /b 1
)
echo.
echo Berhasil: lowscale.exe
