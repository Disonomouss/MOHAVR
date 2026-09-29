<#
.SYNOPSIS
    Capture the OpenXR Simulator's preview window: the composited headset view WITH the host's quad layers
    (menu, reticle). tools/sim_shot.py gets only the projection layer from a D3D11 session.

.EXAMPLE
    tools/sim_window_shot.ps1 -Out logs\shots\x.png
    tools/sim_window_shot.ps1 -Out logs\shots\x.png -Size 1600x900   # restore and resize the window first (it can end
                                                                    # up squeezed to a few pixels behind the game)
#>
param([string]$Class = 'OpenXR Simulator', [string]$Out, [long]$Handle = 0, [string]$Size = '')
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class WS {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
}
'@
$h = [IntPtr]$Handle
if (-not $Handle) {
    # The window belongs to the host (the runtime runs in it); its class is "OpenXR Simulator".
    $hostProc = Get-Process MOHAVR-host -ErrorAction SilentlyContinue | Select-Object -First 1
    Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class WE { public delegate bool P(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(P p, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
public static IntPtr Find(string cls, uint pid) { IntPtr found = IntPtr.Zero; EnumWindows((w, l) => { var c = new StringBuilder(128); GetClassName(w, c, 128); uint p; GetWindowThreadProcessId(w, out p); if (c.ToString() == cls && (pid == 0 || p == pid)) { found = w; return false; } return true; }, IntPtr.Zero); return found; } }
"@
    $h = [WE]::Find($Class, $(if ($hostProc) { [uint32]$hostProc.Id } else { [uint32]0 }))
}
if ($h -eq [IntPtr]::Zero) { throw "no window of class $Class" }
if ($Size -match '^(\d+)x(\d+)$') {
    [void][WS]::ShowWindow($h, 9)                                                                   # SW_RESTORE
    [void][WS]::SetWindowPos($h, [IntPtr]::Zero, 0, 0, [int]$Matches[1], [int]$Matches[2], 0x0006)  # NOMOVE|NOZORDER
    Start-Sleep -Milliseconds 600                                                                   # a few frames
}
$r = New-Object WS+RECT
[void][WS]::GetClientRect($h, [ref]$r)
$bmp = New-Object Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[void][WS]::PrintWindow($h, $hdc, 3)   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)
$bmp.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
"saved $Out ($($bmp.Width)x$($bmp.Height))"
