<#
.SYNOPSIS
    Drive MOHA from scripts: launch windowed, detect screens, send keys, measure, quit, restore.

.DESCRIPTION
    The engine writes no log (ENGINE-NOTES 5c), so screen state comes from screenshots matched
    against reference crops (tools/screen_match.py, tools/harness-ref/checks.json) -- D9.

    Every launch backs up the player's Config\ and Saved\ first (tools/userdata.ps1, D8); `quit`
    restores them after the game has exited, verified byte-identical. The backup path lives in
    logs\harness-state.json, so a quit in a later call still restores the right one.

.EXAMPLE
    tools/harness.ps1 launch                 # backup, start via Steam windowed 1920x1080, wait for window
    tools/harness.ps1 cycle                  # launch -> gameplay (from the save) -> mem -> quit + restore
    tools/harness.ps1 to-gameplay            # from the main menu: Campaign -> Continue -> proven in gameplay
    tools/harness.ps1 ingame                 # Esc must open the pause menu (then Esc resumes)
    tools/harness.ps1 wait mainmenu 120      # block until the screen matches a named check
    tools/harness.ps1 state                  # which named check matches right now (or black / unknown)
    tools/harness.ps1 key enter              # focus game, press keys (tools/sendkey.ps1 names)
    tools/harness.ps1 shot name              # logs\shots\<time>-name.png
    tools/harness.ps1 mem                    # working set / private / virtual (2 GB limit)
    tools/harness.ps1 quit                   # WM_CLOSE (kill after 30 s), then restore user data
    tools/harness.ps1 restore                # restore only (e.g. after a crash)
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)] [string] $Action = 'status',
    [Parameter(Position = 1)] [string] $Arg1,
    [Parameter(Position = 2)] [string] $Arg2,
    [int] $Width  = 1920,
    [int] $Height = 1080
)

$ErrorActionPreference = 'Stop'

$Root      = Split-Path $PSScriptRoot -Parent
$Logs      = Join-Path $Root 'logs'
$Shots     = Join-Path $Logs 'shots'
$StatePath = Join-Path $Logs 'harness-state.json'
$SteamExe  = 'C:\Program Files (x86)\Steam\steam.exe'
$AppId     = 24840
$SteamCooldownSec = 20      # after a KILL, so Steam stops thinking the game still runs
New-Item -ItemType Directory -Force $Shots | Out-Null

if (-not ('MohaHarness.Win' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace MohaHarness {
public static class Win {
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
}}
'@
}

function Get-Moha { Get-Process MOHA -ErrorAction SilentlyContinue | Select-Object -First 1 }

function Save-State($s) { $s | ConvertTo-Json | Set-Content -Encoding utf8 $StatePath }
function Load-State { if (Test-Path $StatePath) { Get-Content $StatePath -Raw | ConvertFrom-Json } }

function Take-Shot([string] $name = 'shot') {
    $out = Join-Path $Shots ('{0}-{1}.png' -f (Get-Date -Format 'HHmmss'), $name)
    & (Join-Path $PSScriptRoot 'capture-window.ps1') -Title 'Medal of Honor Airborne' -Out $out -ClientOnly 6>$null | Out-Null
    $out
}

function Get-ScreenState([string] $shot) {
    $lines = python (Join-Path $PSScriptRoot 'screen_match.py') $shot
    ($lines | Select-String '^state: (.+)$').Matches[0].Groups[1].Value
}

function Wait-State([string] $want, [int] $timeout) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $timeout) {
        if (-not (Get-Moha)) { throw "MOHA exited while waiting for '$want'" }
        $shot = Take-Shot 'wait'
        $st = Get-ScreenState $shot
        Remove-Item $shot
        if ($st -eq $want) { Write-Host ("  state '{0}' after {1:N0}s" -f $want, ((Get-Date) - $t0).TotalSeconds); return $true }
        Start-Sleep -Seconds 1
    }
    $last = Take-Shot "timeout-$want"
    Write-Host "  TIMEOUT waiting for '$want' ($timeout s); last screen: $last"
    return $false
}

function Send-Keys([string] $keys) {
    & (Join-Path $PSScriptRoot 'focus-game.ps1') 6>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'could not focus MOHA' }
    & (Join-Path $PSScriptRoot 'sendkey.ps1') ($keys -split ',') -GapMs 600 6>$null | Out-Null
}

# Brightest pixel of a menu row: 255 = highlighted item (measured on both menus).
function Get-RowMax([string] $shot, [int] $y, [int] $x0, [int] $x1) {
    [double](python -c "from PIL import Image; import numpy as np; a=np.asarray(Image.open(r'$shot').convert('L'),dtype=float); print(a[$($y-22):$($y+22),$($x0):$($x1)].max())")
}

# Gameplay has no fixed-crop signature (the HUD is translucent over a moving world), so prove
# it actively: Esc must open the pause menu. Esc again resumes. Only call when not in a menu.
function Test-InGame {
    Send-Keys 'esc'
    Start-Sleep -Milliseconds 1500
    $shot = Take-Shot 'probe'
    $st = Get-ScreenState $shot
    Remove-Item $shot
    if ($st -eq 'pausemenu') { Send-Keys 'esc'; Start-Sleep -Milliseconds 1000; return $true }
    return $false
}

function Invoke-ToGameplay {
    if (-not (Wait-State 'mainmenu' 150)) { throw 'main menu not reached' }
    Send-Keys 'enter'                                   # Campaign is highlighted by default
    if (-not (Wait-State 'campaignmenu' 20)) { throw 'campaign menu not reached' }
    Send-Keys 'down'                                    # New -> Continue
    Start-Sleep -Milliseconds 700
    $shot = Take-Shot 'continue-check'
    $hl = Get-RowMax $shot 293 380 630
    Remove-Item $shot
    if ($hl -lt 240) { throw "Continue is not highlighted (row max $hl) -- refusing to press Enter" }
    $t0 = Get-Date
    Send-Keys 'enter'
    # Loading is black, then gameplay. Probe with Esc only once the screen is neither black nor a menu.
    while (((Get-Date) - $t0).TotalSeconds -lt 120) {
        Start-Sleep -Seconds 2
        $shot = Take-Shot 'loading'; $st = Get-ScreenState $shot; Remove-Item $shot
        if ($st -eq 'unknown' -and (Test-InGame)) {
            Write-Host ("  in gameplay {0:N0}s after Continue" -f ((Get-Date) - $t0).TotalSeconds); return
        }
    }
    $last = Take-Shot 'timeout-gameplay'
    throw "gameplay not reached within 120 s; last screen $last"
}

function Restore-UserData {
    $s = Load-State
    if (-not $s -or -not $s.backup) { Write-Host 'no backup recorded in harness-state.json'; return }
    if ($s.restored) { Write-Host "already restored from $($s.backup)"; return }
    & (Join-Path $PSScriptRoot 'userdata.ps1') restore -From $s.backup
    $s.restored = $true; Save-State $s
}

switch ($Action) {
    'launch' {
        if (Get-Moha) { throw 'MOHA is already running' }
        $s = Load-State
        if ($s -and -not $s.restored) { throw "previous run's backup $($s.backup) was never restored -- run 'restore' first" }
        $backup = (& (Join-Path $PSScriptRoot 'userdata.ps1') backup | Select-Object -Last 1)
        Save-State ([pscustomobject]@{ backup = $backup; restored = $false; launched = (Get-Date).ToString('o'); pid = 0 })
        & $SteamExe -applaunch $AppId -windowed "ResX=$Width" "ResY=$Height" -log
        $t0 = Get-Date; $p = $null
        while (-not $p -and ((Get-Date) - $t0).TotalSeconds -lt 90) { Start-Sleep -Milliseconds 500; $p = Get-Moha }
        if (-not $p) { throw 'MOHA did not start within 90 s (Steam dialog?) -- user data NOT touched, run restore anyway' }
        while (((Get-Date) - $t0).TotalSeconds -lt 120) {
            $p.Refresh(); if ($p.MainWindowHandle -ne [IntPtr]::Zero -and $p.MainWindowTitle -like 'Medal of Honor*') { break }
            Start-Sleep -Milliseconds 500
        }
        $s = Load-State; $s.pid = $p.Id; Save-State $s
        Write-Host ("launched pid {0}, window up after {1:N1}s" -f $p.Id, ((Get-Date) - $t0).TotalSeconds)
    }

    'wait' {
        $timeout = if ($Arg2) { [int]$Arg2 } else { 120 }
        if (Wait-State $Arg1 $timeout) { exit 0 } else { exit 1 }
    }

    'ingame' { if (Test-InGame) { 'in gameplay (pause menu opened and closed)'; exit 0 } else { 'NOT in gameplay'; exit 1 } }

    'to-gameplay' { Invoke-ToGameplay }

    'cycle' {
        # M0 acceptance: cold start -> live gameplay -> out, unattended, user data restored.
        $t0 = Get-Date
        try {
            & $PSCommandPath launch
            Invoke-ToGameplay
            & $PSCommandPath mem
            Take-Shot 'cycle-gameplay' | Out-Null
            $ok = $true
        } catch { Write-Host "CYCLE FAILED: $($_.Exception.Message)"; $ok = $false }
        finally { & $PSCommandPath quit }
        Write-Host ("cycle {0} in {1:N0}s" -f $(if ($ok) { 'OK' } else { 'FAILED' }), ((Get-Date) - $t0).TotalSeconds)
        if (-not $ok) { exit 1 }
    }

    'state' { $shot = Take-Shot 'state'; python (Join-Path $PSScriptRoot 'screen_match.py') $shot; Write-Host $shot }

    'shot'  { Take-Shot ($(if ($Arg1) { $Arg1 } else { 'shot' })) }

    'key' { Send-Keys $Arg1 }

    'mem' {
        $p = Get-Moha; if (-not $p) { throw 'MOHA is not running' }
        '{0:N0} MB working set, {1:N0} MB private, {2:N0} MB virtual (of 2048)' -f ($p.WorkingSet64/1MB), ($p.PrivateMemorySize64/1MB), ($p.VirtualMemorySize64/1MB)
    }

    'quit' {
        $p = Get-Moha
        if ($p) {
            $p.Refresh()
            [void][MohaHarness.Win]::PostMessageW($p.MainWindowHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
            $t0 = Get-Date
            while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 30) { Start-Sleep -Milliseconds 500; $p.Refresh() }
            if ($p.HasExited) { Write-Host ("closed cleanly in {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds) }
            else {
                Write-Host 'no exit after WM_CLOSE in 30 s -> killing'
                Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds $SteamCooldownSec
            }
        } else { Write-Host 'MOHA not running' }
        Restore-UserData
    }

    'restore' { if (Get-Moha) { throw 'quit the game first' }; Restore-UserData }

    'status' {
        $p = Get-Moha
        if ($p) { "running pid $($p.Id)"; & $PSCommandPath mem } else { 'not running' }
        $s = Load-State; if ($s) { "last backup $($s.backup), restored=$($s.restored)" }
    }

    default { throw "unknown action '$Action'" }
}
