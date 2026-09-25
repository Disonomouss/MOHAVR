<#
.SYNOPSIS
    Bring the MOHA window to the foreground, and verify it got there.

.DESCRIPTION
    Synthetic clicks go to whatever window owns the screen position. If RDR2 is not the
    foreground window the first click is consumed activating it, and the menu never reacts —
    which is exactly what happened on 2026-09-15 once Ghidra's analysis UI started taking
    focus mid-run. The cycle then looked like "the Story button coordinate is wrong" for the
    second time in one evening.

    SetForegroundWindow is restricted by Windows: a process that does not own the foreground
    can be refused. AttachThreadInput to the current foreground thread lifts that restriction,
    which is the documented workaround.

    Exits non-zero if the game did not end up in the foreground, so callers can fail loudly
    instead of clicking into nothing.
#>
param([int] $TimeoutSec = 15)

$ErrorActionPreference = 'Stop'

if (-not ('MOHAVR.Focus' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace MOHAVR {
public static class Focus {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    const int SW_RESTORE = 9;

    public static bool Activate(IntPtr target) {
        IntPtr fg = GetForegroundWindow();
        if (fg == target) return true;
        uint dummy;
        uint fgThread = GetWindowThreadProcessId(fg, out dummy);
        uint me = GetCurrentThreadId();
        // Borrow the foreground thread's input queue so SetForegroundWindow is permitted.
        AttachThreadInput(me, fgThread, true);
        try {
            ShowWindow(target, SW_RESTORE);
            BringWindowToTop(target);
            return SetForegroundWindow(target);
        } finally {
            AttachThreadInput(me, fgThread, false);
        }
    }
    public static uint PidOfForeground() {
        uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid); return pid;
    }
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    // By class, not Process.MainWindowHandle: with OpenXR on, the simulator's preview window is
    // in the same process and would otherwise receive focus (and the keys).
    public static IntPtr FindGameWindow(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid) return true;
            var c = new System.Text.StringBuilder(64); GetClassNameW(h, c, 64);
            if (c.ToString() == "LaunchUnrealUWindowsClient") { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}}
'@
}

$p = Get-Process MOHA -ErrorAction SilentlyContinue
if (-not $p) { Write-Host 'MOHA is not running'; exit 1 }
$hwnd = [MOHAVR.Focus]::FindGameWindow([uint32]$p.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Host 'MOHA has no game window yet'; exit 1 }

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    [void][MOHAVR.Focus]::Activate($hwnd)
    Start-Sleep -Milliseconds 400
    if ([MOHAVR.Focus]::GetForegroundWindow() -eq $hwnd) {
        Write-Host "MOHA (pid $($p.Id)) is foreground"
        exit 0
    }
}
Write-Host "FAILED to bring the MOHA game window to the foreground (foreground pid is $([MOHAVR.Focus]::PidOfForeground()))"
exit 1
