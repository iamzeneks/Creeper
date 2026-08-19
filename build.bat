@echo off
rem Build creeper_img.exe / creeper_audio.exe / creeper_cli.exe (ASCII only, no encoding issues)
cd /d %~dp0
set LIBS=-ld3d11 -ldxgi -ld3dcompiler_47 -lgdi32 -limm32 -luser32 -lshell32 -lole32 -luuid -lcomdlg32 -ldwmapi
set WARN=-Wall -Wextra -Wno-missing-field-initializers
set IMGUI_SRC=imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp imgui\imgui_demo.cpp imgui\backends\imgui_impl_win32.cpp imgui\backends\imgui_impl_dx11.cpp

echo === building creeper_img.exe ===
g++ -O2 -std=c++17 -DUNICODE -D_UNICODE %WARN% -mwindows -I. -Iimgui -Iimgui\backends -Istb ^
  img_app.cpp common_ui.cpp crypto.cpp png_steg.cpp mp3_steg.cpp wav_steg.cpp %IMGUI_SRC% -o creeper_img.exe %LIBS%
if errorlevel 1 goto :err

echo === building creeper_audio.exe ===
g++ -O2 -std=c++17 -DUNICODE -D_UNICODE %WARN% -mwindows -I. -Iimgui -Iimgui\backends -Istb ^
  audio_app.cpp common_ui.cpp crypto.cpp png_steg.cpp mp3_steg.cpp wav_steg.cpp %IMGUI_SRC% -o creeper_audio.exe %LIBS%
if errorlevel 1 goto :err

echo === building creeper_cli.exe ===
g++ -O2 -std=c++17 -DUNICODE -D_UNICODE %WARN% -I. -Iimgui -Iimgui\backends -Istb ^
  cli_main.cpp crypto.cpp png_steg.cpp mp3_steg.cpp wav_steg.cpp -o creeper_cli.exe %LIBS%
if errorlevel 1 goto :err

echo.
echo BUILD OK
exit /b 0

:err
echo.
echo BUILD FAILED
exit /b 1
