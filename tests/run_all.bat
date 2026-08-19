@echo off
setlocal
rem creeper full regression: run all suites sequentially (they share tests/tmp)
cd /d "%~dp0.."
python -X utf8 tests\test_crypto.py || goto :fail
python -X utf8 tests\test_cross.py || goto :fail
python -X utf8 tests\test_png.py || goto :fail
python -X utf8 tests\test_mp3.py || goto :fail
python -X utf8 tests\test_wav.py || goto :fail
python -X utf8 tests\test_split.py || goto :fail
powershell -ExecutionPolicy Bypass -File tests\test_gui.ps1 || goto :fail
powershell -ExecutionPolicy Bypass -File tests\test_gui_about.ps1 || goto :fail
powershell -ExecutionPolicy Bypass -File tests\test_gui_quality.ps1 || goto :fail
echo.
echo ============ ALL SUITES PASSED ============
exit /b 0
:fail
echo.
echo ============ FAILURE (see logs above) ============
exit /b 1