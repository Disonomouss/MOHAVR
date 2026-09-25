<#
.SYNOPSIS
    Capture one window to a PNG by title substring, even when it is behind another window.

.DESCRIPTION
    `screenshot.ps1` copies the primary display, which only ever shows whatever is in front --
    with the game running borderless-fullscreen that is always the game. The OpenXR Simulator's
    preview window sits behind it, so its contents cannot be read that way, and the simulator's
    own readback path is broken in this environment (`get_frame_info` reports a preview height of
    1 pixel and `capture_screenshot` returns bytes that will not decode).

    PrintWindow asks the window to redraw itself into a device context we own, so occlusion does
    not matter. PW_RENDERFULLCONTENT (0x2) is what makes that work for DirectX/composited
    windows; the simulator renders its D3D12 preview through GDI, so it answers.

.EXAMPLE
    tools/capture-window.ps1 -Title 'OpenXR Simulator' -Out sim.png
    tools/capture-window.ps1 -Title 'OpenXR Simulator' -Out sim.png -Halves   # also write -L / -R
#>
param(
    [Parameter(Mandatory)][string] $Title,
    [Parameter(Mandatory)][string] $Out,
    [switch] $Halves,          # also save the left and right halves separately
    [switch] $ClientOnly,       # crop the window border/title bar away
    [string] $ResizeTo = '',    # 'WxH': give the window that client size first (no focus change)
    [int] $SettleMs = 700       # time to let the window redraw after a resize
)

Add-Type -AssemblyName System.Drawing

if (-not ("WinCap" -as [type])) {
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WinCap {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool AdjustWindowRect(ref RECT r, uint style, bool menu);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int idx);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    public static IntPtr Find(string needle) {
        IntPtr found = IntPtr.Zero;
        string n = needle.ToLowerInvariant();
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            if (!IsWindowVisible(h)) return true;
            StringBuilder t = new StringBuilder(512);
            GetWindowTextW(h, t, t.Capacity);
            StringBuilder c = new StringBuilder(512);
            GetClassNameW(h, c, c.Capacity);
            string title = t.ToString().ToLowerInvariant();
            string cls   = c.ToString().ToLowerInvariant();
            if (title.Contains(n) || cls.Contains(n)) {
                RECT r; GetWindowRect(h, out r);
                // Skip degenerate/hidden shells; take the first window with real area.
                if ((r.Right - r.Left) > 32 && (r.Bottom - r.Top) > 32) { found = h; return false; }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@
}

$h = [WinCap]::Find($Title)
if ($h -eq [IntPtr]::Zero) {
    Write-Host "capture-window: no visible window matching '$Title'"
    exit 2
}

# A window that has been rolled up to its title bar has no client area, so there is nothing
# to capture. That is exactly the state the simulator's preview window is in while the game
# holds the foreground, and it is why the runtime's own readback reports a 1-pixel preview.
# Give it a real size first. SWP_NOACTIVATE|SWP_NOZORDER keeps the game in front.
if ($ResizeTo -match '^(\d+)x(\d+)$') {
    $cw = [int]$Matches[1]; $ch = [int]$Matches[2]
    if ([WinCap]::IsIconic($h)) { [void][WinCap]::ShowWindow($h, 4) }   # SW_SHOWNOACTIVATE
    # Convert the wanted CLIENT size into a window size for this window's style.
    $need = New-Object WinCap+RECT
    $need.Left = 0; $need.Top = 0; $need.Right = $cw; $need.Bottom = $ch
    $style = [WinCap]::GetWindowLong($h, -16)   # GWL_STYLE
    [void][WinCap]::AdjustWindowRect([ref]$need, [uint32]$style, $true)
    $ww = $need.Right - $need.Left; $wh = $need.Bottom - $need.Top
    $cur = New-Object WinCap+RECT
    [void][WinCap]::GetWindowRect($h, [ref]$cur)
    # 0x0010 SWP_NOACTIVATE | 0x0004 SWP_NOZORDER
    [void][WinCap]::SetWindowPos($h, [IntPtr]::Zero, $cur.Left, $cur.Top, $ww, $wh, 0x0014)
    Start-Sleep -Milliseconds $SettleMs
}

$r = New-Object WinCap+RECT
[void][WinCap]::GetWindowRect($h, [ref]$r)
$w = $r.Right - $r.Left
$ht = $r.Bottom - $r.Top

$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
# 0x2 = PW_RENDERFULLCONTENT: required for DirectX-composited windows.
$ok = [WinCap]::PrintWindow($h, $hdc, 2)
$g.ReleaseHdc($hdc)
$g.Dispose()

if (-not $ok) {
    Write-Host "capture-window: PrintWindow failed for '$Title'"
    $bmp.Dispose()
    exit 3
}

$img = $bmp
if ($ClientOnly) {
    # Client area, in screen coordinates, relative to the window rect we captured.
    $cr = New-Object WinCap+RECT
    [void][WinCap]::GetClientRect($h, [ref]$cr)
    $p = New-Object WinCap+POINT
    [void][WinCap]::ClientToScreen($h, [ref]$p)
    $cw = $cr.Right - $cr.Left; $ch = $cr.Bottom - $cr.Top
    if ($cw -gt 0 -and $ch -gt 0) {
        $rect = New-Object System.Drawing.Rectangle (($p.X - $r.Left), ($p.Y - $r.Top), $cw, $ch)
        $img = $bmp.Clone($rect, $bmp.PixelFormat)
    }
}

$dir = Split-Path $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$img.Save($Out)
Write-Host "saved $Out ($($img.Width)x$($img.Height)) from '$Title'"

if ($Halves) {
    $half = [int]($img.Width / 2)
    foreach ($side in @(@{n='L'; x=0}, @{n='R'; x=$half})) {
        $rect = New-Object System.Drawing.Rectangle $side.x, 0, $half, $img.Height
        $part = $img.Clone($rect, $img.PixelFormat)
        $path = [IO.Path]::ChangeExtension($Out, $null) + $side.n + '.png'
        $part.Save($path)
        Write-Host "  saved $path ($($part.Width)x$($part.Height))"
        $part.Dispose()
    }
}

if ($img -ne $bmp) { $img.Dispose() }
$bmp.Dispose()
exit 0
