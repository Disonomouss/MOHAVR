<#
.SYNOPSIS
    Soak test: launch -> gameplay -> N minutes of periodic injected movement -> quit + restore, recording the
    game's address space every minute (logs\soak-<time>.csv) and scanning the mod logs at the end.

.DESCRIPTION
    Needs a deployed mod with Input.Controllers=1 (tools/pad_cmd.py drives the virtual pad). Every 30 s:
    forward 1 s, back 1 s, turn right 0.5 s, turn left 0.5 s (the player stays about where it was; a death in
    between is fine -- the game reloads the checkpoint, which is part of the test). Every 60 s: tools/vmmap.py
    (used / free / largest free block) and the game's working set.

.EXAMPLE
    tools\soak.ps1 -Minutes 30
#>
param([int] $Minutes = 30)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$harness = Join-Path $PSScriptRoot 'harness.ps1'
$csv = Join-Path $Root ("logs\soak-{0}.csv" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
'minute,used_mb,free_mb,largest_free_mb,working_set_mb' | Set-Content $csv

& $harness launch
& $harness to-gameplay
$t0 = Get-Date
$nextMove = $t0
$nextSample = $t0
try {
    while (((Get-Date) - $t0).TotalMinutes -lt $Minutes) {
        $p = Get-Process MOHA -ErrorAction SilentlyContinue
        if (-not $p) { throw 'MOHA exited during the soak' }
        if ((Get-Date) -ge $nextMove) {
            python (Join-Path $PSScriptRoot 'pad_cmd.py') --seq 'ly=1 dur=1' 'ly=-1 dur=1' 'rx=1 dur=0.5' 'rx=-1 dur=0.5' 'dur=0.2' | Out-Null
            $nextMove = $nextMove.AddSeconds(30)
        }
        if ((Get-Date) -ge $nextSample) {
            $m = [math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
            $v = python (Join-Path $PSScriptRoot 'vmmap.py') --pid $p.Id 2>&1 | Out-String
            $used = if ($v -match 'used ([\d.]+) MB') { $Matches[1] } else { '' }
            $free = if ($v -match 'free ([\d.]+) MB') { $Matches[1] } else { '' }
            $big = if ($v -match 'largest free blocks \(MB\): \[([\d.]+)') { $Matches[1] } else { '' }
            $ws = [math]::Round($p.WorkingSet64 / 1MB)
            "$m,$used,$free,$big,$ws" | Add-Content $csv
            Write-Host ("  {0,5} min: used {1} MB, free {2} MB, largest {3} MB, working set {4} MB" -f $m, $used, $free, $big, $ws)
            $nextSample = $nextSample.AddSeconds(60)
        }
        Start-Sleep -Milliseconds 500
    }
    Write-Host "soak: $Minutes min done"
}
finally {
    & $harness quit
    foreach ($log in @('MOHAVR.log', 'MOHAVR-host.log')) {
        $kept = Get-ChildItem (Join-Path $Root 'logs\modlogs') -Filter ("*-" + $(if ($log -eq 'MOHAVR.log') { 'run' } else { 'host' }) + ".log") |
                Sort-Object LastWriteTime | Select-Object -Last 1
        if ($kept) {
            $bad = Select-String -Path $kept.FullName -Pattern 'FAILED|failed|exception|error|standing down' |
                   Where-Object { $_.Line -notmatch 'not handled' }
            Write-Host ("  {0}: {1} suspicious line(s)" -f $kept.Name, @($bad).Count)
            $bad | Select-Object -First 10 | ForEach-Object { Write-Host "    $($_.Line)" }
        }
    }
    Write-Host "soak: samples in $csv"
}
