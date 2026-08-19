@echo off
rem Build creeper_img.exe / creeper_audio.exe / creeper_cli.exe (ASCII only, no encoding issues)
rem Output goes to res/ (release package) so tests/ and delivery scripts keep working.
cd /d %~dp0
if not exist ..\res mkdir ..\res
set LIBS=-ld3d11 -ldxgi -ld3dcompiler_47 -lgdi32 -limm32 -luser32 -lshell32 -lole32 -luuid -lcomdlg32 -ldwmapi
set WARN=-Wall -Wextra -Wno-missing-field-initializers
set IMGUI_SRC=..\third_party\imgui\imgui.cpp ..\third_party\imgui\imgui_draw.cpp ..\third_party\imgui\imgui_tables.cpp ..\third_party\imgui\imgui_widgets.cpp ..\third_party\imgui\imgui_demo.cpp ..\third_party\imgui\backends\imgui_impl_win32.cpp ..\third_party\imgui\backends\imgui_impl_dx11.cpp

echo === building creeper_img.exe ===
g++ -O2 -std=c++17 -DUNICODE -D_UNICODE %WARN% -mwindows -I.. -I..\third_party\imgui -I..\third_party\imgui\backends -I..\third_party ^
  img_app.cpp common_ui.cpp crypto.cpp png_steg.cpp mp3_steg.cpp wav_steg.cpp split_steg.cpp %IMGUI_SRC% -o ..\res\creeper_img.exe %LIBS%
if errorlevel 1 goto :err

echo === building creeper_audio.exe ===
g++ -O2 -std=c++17 -DUNICODE -D_UNICODE %WARN% -mwindows -I.. -I..\third_party\imgui -I..\third_party\imgui\backends -I..\third_party ^
  audio_app.cpp common_ui.cpp crypto.cpp png_steg.cpp mp3_steg.cpp wav_steg.cpp split_steg.cpp %IMGUI_SRC% -o ..\res\creeper_audio.exe %LIBS%
if errorlevel 1 goto :err

echo === building creeper_cli.exe ===
g++ -O2 -std=c++17 -DUNICODE -D_UNICODE %WARN% -I.. -I..\third_party\imgui -I..\third_party\imgui\backends -I..\third_party ^
  cli_main.cpp crypto.cpp png_steg.cpp mp3_steg.cpp wav_steg.cpp split_steg.cpp -o ..\res\creeper_cli.exe %LIBS%
if errorlevel 1 goto :err

echo.
echo BUILD OK
exit /b 0

:err
echo.
echo BUILD FAILED
exit /b 1


