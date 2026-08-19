# test_gui.ps1 - Section F: GUI smoke test (start -> process alive -> stop)
# Usage: powershell -ExecutionPolicy Bypass -File tests\test_gui.ps1
# Note: ASCII-only messages to avoid encoding issues under Windows PowerShell 5.1.
$ErrorActionPreference = 'Continue'
$root = 'C:\Users\Zeneks\Documents\code\creeper'
$fails = 0

foreach ($name in @('creeper_img', 'creeper_audio')) {
    $exe = Join-Path $root "$name.exe"
    if (-not (Test-Path $exe)) {
        Write-Host "[FAIL] $exe missing"
        $fails++
        continue
    }
    $p = Start-Process -FilePath $exe -PassThru
    Start-Sleep -Seconds 3
    $alive = Get-Process -Name $name -ErrorAction SilentlyContinue
    if ($alive) {
        Write-Host "[PASS] $name started, process alive (PID=$($alive.Id), MainWindowTitle='$($alive.MainWindowTitle)')"
    } else {
        Write-Host "[FAIL] $name process not found after 3s (crashed on startup?)"
        $fails++
    }
    Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 800
}

Write-Host "== test_gui: $(2 - $fails) passed, $fails failed =="
exit $fails
