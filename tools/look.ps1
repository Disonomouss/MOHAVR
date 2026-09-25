<#
.SYNOPSIS
    Turn the in-game camera with RELATIVE mouse motion, and walk.

.DESCRIPTION
    click.ps1 moves the cursor to absolute screen coordinates, which is right for menus and
    useless in gameplay: RDR2 reads raw relative motion for mouse-look, so an absolute warp
    turns the camera by whatever delta happens to fall out of it.

    This sends MOUSEEVENTF_MOVE *without* ABSOLUTE, in small steps, which is what a real mouse
    produces.

    WHY THIS EXISTS. The save always loads at a foggy lakeside at night — the worst case for
    detecting a rendering change (dark defeats pixel metrics, drift swamps weak signals, and
    water makes reflection passes masquerade as the main view; Addenda 27, 32, 33). Being able
    to turn and walk means a cycle can relocate to somewhere legible before measuring, without
    needing save-slot menu navigation.

.EXAMPLE
    tools/look.ps1 -Dx 1200            # turn right
    tools/look.ps1 -Dx -1200           # turn left
    tools/look.ps1 -Walk 4             # hold W for 4 s
    tools/look.ps1 -Dx 2400 -Walk 5    # turn about-face, then walk
#>
param(
    [int] $Dx = 0,
    [int] $Dy = 0,
    [int] $Steps = 24,
    [int] $Walk = 0,
    [int] $StepDelayMs = 16
)

$ErrorActionPreference = 'Stop'

if (-not ('MOHAVR.Look' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace MOHAVR {
public static class Look {
    [StructLayout(LayoutKind.Sequential)]
    struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    // INPUT must be exactly 40 bytes on x64 -- a wrong size makes SendInput return 0 and
    // deliver nothing, silently. That cost a cycle earlier in this project.
    [StructLayout(LayoutKind.Sequential)]
    struct INPUT { public uint type; public MOUSEINPUT mi; }
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] i, int size);
    const uint MOVE = 0x0001;   // relative: no ABSOLUTE flag

    public static uint Move(int dx, int dy) {
        var i = new INPUT[1];
        i[0].type = 0;                  // INPUT_MOUSE
        i[0].mi.dx = dx; i[0].mi.dy = dy; i[0].mi.dwFlags = MOVE;
        return SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
}}
'@
}

if ($Dx -ne 0 -or $Dy -ne 0) {
    # Split into steps: one huge jump is often swallowed or clamped by a game's look code.
    $sx = [int][Math]::Round($Dx / [double]$Steps)
    $sy = [int][Math]::Round($Dy / [double]$Steps)
    $sent = 0
    for ($i = 0; $i -lt $Steps; $i++) {
        $sent += [MOHAVR.Look]::Move($sx, $sy)
        Start-Sleep -Milliseconds $StepDelayMs
    }
    if ($sent -eq 0) { throw "mouse-look NOT delivered (SendInput returned 0 for every step)" }
    Write-Host "turned dx=$Dx dy=$Dy in $Steps steps (sent=$sent)"
}

if ($Walk -gt 0) {
    & (Join-Path $PSScriptRoot 'sendkey.ps1') w -HoldMs ($Walk * 1000)
}
