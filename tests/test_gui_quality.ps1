# Hidden window smoke test: Ctrl+Shift+F must open the metadata window and show
# the 编码质量 (encoding quality) combo. Uses a COPY of the exe. ASCII-only body.
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class W32Q2 {
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType=WindowsRuntime]
$null = [Windows.Globalization.Language, Windows.Globalization, ContentType=WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType=WindowsRuntime]
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType=WindowsRuntime]
function Await($t, $rt) {
    $g = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
    $nt = $g.MakeGenericMethod($rt).Invoke($null, @($t)); $nt.Wait(-1) | Out-Null; $nt.Result
}

[void][W32Q2]::SetProcessDpiAwarenessContext([IntPtr]-4)
$script:list = [System.Collections.ArrayList]::new()
$cb = [W32Q2+EnumWindowsProc]{ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [void][W32Q2]::GetClassName($h, $sb, 256)
    [void]$script:list.Add([pscustomobject]@{H=$h; Cls=$sb.ToString()})
    return $true
}

$root = Split-Path -Parent $PSScriptRoot
$tmpDir = Join-Path $root "tests\tmp"
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$copy = Join-Path $tmpDir "gui_quality_copy.exe"
Copy-Item (Join-Path (Join-Path $root "res") "creeper_img.exe") $copy -Force

$p = Start-Process -FilePath $copy -PassThru
Start-Sleep -Seconds 2
$fail = $false
try {
    $script:list.Clear()
    [void][W32Q2]::EnumWindows($cb, [IntPtr]::Zero)
    $main = $script:list | Where-Object { $_.Cls -like "CreeperImgApp" } | Select-Object -First 1
    if (-not $main) { Write-Output "FAIL: main window not found"; $fail = $true }
    if (-not $fail) {
        [void][W32Q2]::SetForegroundWindow($main.H)
        Start-Sleep -Milliseconds 500
        [void][W32Q2]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)
        [void][W32Q2]::keybd_event(0x10, 0, 0, [UIntPtr]::Zero)
        [void][W32Q2]::keybd_event(0x46, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 100
        [void][W32Q2]::keybd_event(0x46, 0, 2, [UIntPtr]::Zero)
        [void][W32Q2]::keybd_event(0x10, 0, 2, [UIntPtr]::Zero)
        [void][W32Q2]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 1200

        $r = New-Object W32Q2+RECT
        [void][W32Q2]::GetWindowRect($main.H, [ref]$r)
        $shot = Join-Path $tmpDir "gui_quality_shot.png"
        $bmp = New-Object System.Drawing.Bitmap -ArgumentList @(($r.R - $r.L), ($r.B - $r.T))
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
        $g.Dispose()
        $bmp.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()

        $file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($shot)) ([Windows.Storage.StorageFile])
        $stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
        $dec = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
        $soft = Await ($dec.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
        $eng = [Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage([Windows.Globalization.Language]::new("zh-Hans-CN"))
        $res = Await ($eng.RecognizeAsync($soft)) ([Windows.Media.Ocr.OcrResult])
        $plain = $res.Text.Replace([string][char]0x00A0, "").Replace([string][char]0x3000, "").Replace(" ", "")
        $quality = [string][char]0x7F16 + [char]0x7801 + [char]0x8D28 + [char]0x91CF
        if ($plain.IndexOf($quality) -ge 0 -and $plain.IndexOf("EXIF") -ge 0) {
            Write-Output "PASS: hidden window opened, quality combo present"
        } else {
            Write-Output "FAIL: quality combo not found in OCR text"
            $fail = $true
        }
    }
} finally {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
    Remove-Item $copy -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $tmpDir "gui_quality_shot.png") -Force -ErrorAction SilentlyContinue
}
if ($fail) { exit 1 }
Write-Output "== test_gui_quality: done =="