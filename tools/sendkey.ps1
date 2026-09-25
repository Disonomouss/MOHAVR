<#
.SYNOPSIS
    Press keys into the foreground window with SendInput + hardware scan codes.

.DESCRIPTION
    RDR2 reads the keyboard through raw input, which delivers the SCAN CODE (MakeCode), not
    the virtual key. keybd_event(vk, 0, ...) -- what Cheat Engine's keyDown uses -- arrives
    with MakeCode 0 and the game maps it to nothing; that is why do_key_press(13) never
    selected 'Story' on the main menu. KEYEVENTF_SCANCODE fixes it.

    Each key is held for -HoldMs so a per-frame poll cannot miss it (a down+up inside one
    ~16 ms frame is invisible to code that samples key state once per frame).

.EXAMPLE
    tools/sendkey.ps1 enter                 # one Enter, held 120 ms
    tools/sendkey.ps1 enter,enter -GapMs 1500
    tools/sendkey.ps1 w -HoldMs 2000        # walk forward for 2 s
    tools/sendkey.ps1 -Scan 0x1C            # raw scan code
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)] [string[]] $Keys,
    [int] $Scan = 0,
    [int] $HoldMs = 120,
    [int] $GapMs = 400
)

$ErrorActionPreference = 'Stop'

if (-not ('MOHAVR.Input' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace MOHAVR {
public static class Input {
    [StructLayout(LayoutKind.Sequential)]
    struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)]
    // The union must be MOUSEINPUT-sized (32 bytes) so INPUT is 40 bytes on x64; SendInput
    // rejects any other cbSize with ERROR_INVALID_PARAMETER and returns 0.
    struct INPUTUNION { [FieldOffset(0)] public KEYBDINPUT ki; [FieldOffset(0)] public long pad1; [FieldOffset(8)] public long pad2; [FieldOffset(16)] public long pad3; [FieldOffset(24)] public long pad4; }
    [StructLayout(LayoutKind.Sequential)]
    struct INPUT { public uint type; public INPUTUNION u; }
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    const uint INPUT_KEYBOARD = 1, KEYEVENTF_SCANCODE = 0x0008, KEYEVENTF_KEYUP = 0x0002, KEYEVENTF_EXTENDEDKEY = 0x0001;

    public static uint Key(ushort scan, bool up, bool extended) {
        var i = new INPUT[1];
        i[0].type = INPUT_KEYBOARD;
        i[0].u.ki.wScan = scan;
        i[0].u.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
        return SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
}}
'@
}

# Scan codes (set 1). Arrow keys and a few others are extended (E0 prefix).
$Map = @{
    'enter' = @(0x1C, $false); 'esc' = @(0x01, $false); 'space' = @(0x39, $false); 'tab' = @(0x0F, $false)
    'up' = @(0x48, $true); 'down' = @(0x50, $true); 'left' = @(0x4B, $true); 'right' = @(0x4D, $true)
    'backspace' = @(0x0E, $false); 'lshift' = @(0x2A, $false); 'lctrl' = @(0x1D, $false); 'lalt' = @(0x38, $false)
    'f1' = @(0x3B, $false); 'f2' = @(0x3C, $false); 'f3' = @(0x3D, $false); 'f4' = @(0x3E, $false)
    'a' = @(0x1E, $false); 'b' = @(0x30, $false); 'c' = @(0x2E, $false); 'd' = @(0x20, $false); 'e' = @(0x12, $false)
    'f' = @(0x21, $false); 'g' = @(0x22, $false); 'h' = @(0x23, $false); 'i' = @(0x17, $false); 'j' = @(0x24, $false)
    'k' = @(0x25, $false); 'l' = @(0x26, $false); 'm' = @(0x32, $false); 'n' = @(0x31, $false); 'o' = @(0x18, $false)
    'p' = @(0x19, $false); 'q' = @(0x10, $false); 'r' = @(0x13, $false); 's' = @(0x1F, $false); 't' = @(0x14, $false)
    'u' = @(0x16, $false); 'v' = @(0x2F, $false); 'w' = @(0x11, $false); 'x' = @(0x2D, $false); 'y' = @(0x15, $false)
    'z' = @(0x2C, $false)
    '1' = @(0x02, $false); '2' = @(0x03, $false); '3' = @(0x04, $false); '4' = @(0x05, $false); '5' = @(0x06, $false)
}

function Press([int] $code, [bool] $ext, [string] $name) {
    $r1 = [MOHAVR.Input]::Key([uint16] $code, $false, $ext)
    $e1 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Start-Sleep -Milliseconds $HoldMs
    $r2 = [MOHAVR.Input]::Key([uint16] $code, $true, $ext)
    Write-Host ("pressed {0} (scan 0x{1:X2}) hold={2}ms  sent={3}/{4}{5}" -f $name, $code, $HoldMs, $r1, $r2,
        $(if ($r1 -eq 0) { " win32err=$e1" } else { '' }))
    if ($r1 -eq 0) { Write-Warning 'SendInput returned 0 -- input was NOT delivered (UIPI / integrity level, or a bad INPUT layout)' }
}

if ($Scan) {
    Press $Scan $false ("scan")
} else {
    if (-not $Keys) { throw 'give key names (see $Map) or -Scan' }
    $first = $true
    foreach ($k in $Keys) {
        $kk = $k.ToLower()
        if (-not $Map.ContainsKey($kk)) { throw "unknown key '$k'" }
        if (-not $first) { Start-Sleep -Milliseconds $GapMs }
        Press $Map[$kk][0] $Map[$kk][1] $kk
        $first = $false
    }
}
