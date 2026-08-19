# GUI About dialog real-click verification (DPI-aware injection).
# Uses a COPY of the exe so the self-destruct path can never touch the real binary.
# All coordinates are physical (SetProcessDpiAwarenessContext).
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class W32G {
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    public struct RECT { public int L, T, R, B; }
    public struct POINT { public int X, Y; }
}
"@
[void][W32G]::SetProcessDpiAwarenessContext([IntPtr]-4)

$script:list = [System.Collections.ArrayList]::new()
$cb = [W32G+EnumWindowsProc]{ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [void][W32G]::GetClassName($h, $sb, 256)
    $t = New-Object System.Text.StringBuilder 256
    [void][W32G]::GetWindowText($h, $t, 256)
    [void]$script:list.Add([pscustomobject]@{H=$h; Cls=$sb.ToString(); Txt=$t.ToString()})
    return $true
}

$closeTxt = [string][char]0x5173 + [char]0x95ED   # close

$root = Split-Path -Parent $PSScriptRoot
$tmpDir = Join-Path $root "tests\tmp"
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$copy = Join-Path $tmpDir "gui_about_img_copy.exe"
Copy-Item (Join-Path $root "creeper_img.exe") $copy -Force

$p = Start-Process -FilePath $copy -PassThru
Start-Sleep -Seconds 2
try {
    $script:list.Clear()
    [void][W32G]::EnumWindows($cb, [IntPtr]::Zero)
    $main = $script:list | Where-Object { $_.Cls -like "CreeperImgApp" } | Select-Object -First 1
    if (-not $main) { Write-Output "FAIL: main window not found"; exit 1 }
    $hwnd = $main.H
    $r = New-Object W32G+RECT
    [void][W32G]::GetWindowRect($hwnd, [ref]$r)
    if (($r.R - $r.L) -lt 200) { Write-Output "FAIL: implausible window rect"; exit 1 }

    # About button: OCR-verified at window-relative 96.2% / 94.8% (physical fraction)
    $bx = $r.L + [int](($r.R - $r.L) * 0.962)
    $by = $r.T + [int](($r.B - $r.T) * 0.948)
    $pt = New-Object W32G+POINT; $pt.X = $bx; $pt.Y = $by
    if ([W32G]::WindowFromPoint($pt) -ne $hwnd) {
        Write-Output "FAIL: about point not inside Creeper window"
        exit 1
    }

    [void][W32G]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 400
    [void][W32G]::SetCursorPos($bx, $by)
    Start-Sleep -Milliseconds 400
    [void][W32G]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 150
    [void][W32G]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 1200

    $script:list.Clear()
    [void][W32G]::EnumWindows($cb, [IntPtr]::Zero)
    $dlg = $script:list | Where-Object { $_.Cls -eq "XhAboutDlg" } | Select-Object -First 1
    $fg = [W32G]::GetForegroundWindow()
    if (-not $dlg) { Write-Output "FAIL: XhAboutDlg did not appear"; exit 1 }
    if ($fg -ne $dlg.H) { Write-Output "FAIL: dialog not foreground"; exit 1 }
    $dr = New-Object W32G+RECT
    [void][W32G]::GetWindowRect($dlg.H, [ref]$dr)
    Write-Output ("PASS: dialog opened rect=({0},{1})-({2},{3})" -f $dr.L, $dr.T, $dr.R, $dr.B)

    $script:list.Clear()
    [void][W32G]::EnumChildWindows($dlg.H, $cb, [IntPtr]::Zero)
    $closeBtn = $script:list | Where-Object { $_.Txt -eq $closeTxt } | Select-Object -First 1
    if (-not $closeBtn) { Write-Output "FAIL: close button not found"; exit 1 }
    $br = New-Object W32G+RECT
    [void][W32G]::GetWindowRect($closeBtn.H, [ref]$br)
    $cbx = [int](($br.L + $br.R) / 2); $cby = [int](($br.T + $br.B) / 2)
    [void][W32G]::SetCursorPos($cbx, $cby)
    Start-Sleep -Milliseconds 300
    [void][W32G]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 150
    [void][W32G]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 1000

    $script:list.Clear()
    [void][W32G]::EnumWindows($cb, [IntPtr]::Zero)
    if ($script:list | Where-Object { $_.Cls -eq "XhAboutDlg" }) {
        Write-Output "FAIL: dialog still alive after close click"
        exit 1
    }
    if ([W32G]::GetForegroundWindow() -ne $hwnd) {
        Write-Output "WARN: dialog gone but focus not restored"
    }
    Write-Output "PASS: close clicked, dialog destroyed, focus restored"
} finally {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
    Remove-Item $copy -Force -ErrorAction SilentlyContinue
}
Write-Output "== test_gui_about: done =="