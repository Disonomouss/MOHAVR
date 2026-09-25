<#
.SYNOPSIS
    Move the mouse and left-click, via SendInput with absolute coordinates.

.DESCRIPTION
    Needed because RDR2's landing page cannot be driven by Enter alone from a script.
    Measured 2026-09-15:
      * Mouse untouched -> Enter activates whatever the mouse happens to be hovering. A
        cursor left at the bottom-right from earlier work hit "Quit Game" and the game
        exited cleanly (RGL logged exit code 0x0) 2.5 s later.
      * Mouse moved to empty space -> RDR2 switches to mouse mode, nothing is hovered, and
        Enter does nothing at all. The cycle then profiled the menu for 15 minutes.
    Clicking the button directly has neither failure mode.

    Coordinates are in 1920x1080 screen pixels (the game runs borderless at that size).

.EXAMPLE
    tools/click.ps1 1555 1003        # the "STORY" load button, bottom-right of the landing page
    tools/click.ps1 -X 100 -Y 100 -MoveOnly
#>
param(
    [Parameter(Position = 0)] [int] $X,
    [Parameter(Position = 1)] [int] $Y,
    [switch] $MoveOnly,
    [int] $SettleMs = 400
)

if (-not ('MOHAVR.Mouse' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace MOHAVR {
public static class Mouse {
    [StructLayout(LayoutKind.Sequential)]
    struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    // INPUT must be exactly 40 bytes on x64: uint type + 4 pad, then the 32-byte MOUSEINPUT.
    // A trailing pad field makes it 48 and SendInput silently returns 0 without delivering.
    [StructLayout(LayoutKind.Sequential)]
    struct INPUT { public uint type; public MOUSEINPUT mi; }
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int i);
    const uint INPUT_MOUSE = 0;
    const uint MOVE = 0x0001, ABSOLUTE = 0x8000, LEFTDOWN = 0x0002, LEFTUP = 0x0004;

    static uint Send(uint flags, int nx, int ny) {
        var i = new INPUT[1];
        i[0].type = INPUT_MOUSE;
        i[0].mi.dx = nx; i[0].mi.dy = ny; i[0].mi.dwFlags = flags;
        return SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    // Absolute coordinates are normalised to 0..65535 across the virtual desktop.
    public static uint Move(int x, int y) {
        int w = GetSystemMetrics(0), h = GetSystemMetrics(1);
        return Send(MOVE | ABSOLUTE, (x * 65535) / (w - 1), (y * 65535) / (h - 1));
    }
    public static uint Click() { uint a = Send(LEFTDOWN, 0, 0); System.Threading.Thread.Sleep(60); return a + Send(LEFTUP, 0, 0); }
}}
'@
}

$r = [MOHAVR.Mouse]::Move($X, $Y)
Start-Sleep -Milliseconds $SettleMs
# Verify by readback rather than trusting the return value alone -- a wrong INPUT size makes
# SendInput return 0 and deliver nothing, which is invisible without checking.
Add-Type -AssemblyName System.Windows.Forms
$p = [System.Windows.Forms.Cursor]::Position
if ($r -eq 0 -or [Math]::Abs($p.X - $X) -gt 2 -or [Math]::Abs($p.Y - $Y) -gt 2) {
    throw "mouse move NOT delivered (sent=$r, cursor is at $($p.X),$($p.Y), wanted $X,$Y)"
}
if ($MoveOnly) {
    Write-Host "moved to $X,$Y (verified)"
} else {
    $c = [MOHAVR.Mouse]::Click()
    if ($c -eq 0) { throw "click NOT delivered at $X,$Y" }
    Write-Host "clicked $X,$Y (verified at $($p.X),$($p.Y))"
}
