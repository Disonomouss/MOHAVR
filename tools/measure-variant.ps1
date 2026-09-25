<#
.SYNOPSIS
    One measured run of a mod configuration: deploy with ini overrides, drive to gameplay,
    let the scene settle, snapshot the address space (tools/vmmap.py) and a screenshot, quit.

.DESCRIPTION
    Used for A/B comparisons in the fixed test scene (the player's save resumes mid-parachute
    over the flak tower and lands ~15 s in -- ENGINE-NOTES 5c). Leaves the mod deployed; the
    caller undeploys at the end of the session.

    Output: logs\measure\<label>.json (vmmap + mem) and logs\measure\<label>.png

.EXAMPLE
    tools\measure-variant.ps1 -Label on12-off -Set 'Bridge.D3D9On12=0'
    tools\measure-variant.ps1 -Label on12-on  -Set 'Bridge.D3D9On12=1' -SettleSec 20
#>
param(
    [Parameter(Mandatory)] [string] $Label,
    [string[]] $Set = @(),
    [int] $SettleSec = 20
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out  = Join-Path $root 'logs\measure'
New-Item -ItemType Directory -Force $out | Out-Null
$h = Join-Path $PSScriptRoot 'harness.ps1'

& (Join-Path $PSScriptRoot 'deploy.ps1') deploy -Set $Set
try {
    & $h launch
    & $h wait-log 'Direct3DCreate9 #1' 60
    if ($LASTEXITCODE -ne 0) { throw 'Direct3DCreate9 never reached the hook' }
    & $h to-gameplay
    Start-Sleep -Seconds $SettleSec
    $vm = python (Join-Path $PSScriptRoot 'vmmap.py') --json | ConvertFrom-Json
    $vm | Add-Member label $Label
    $vm | Add-Member overrides ($Set -join ';')
    $vm | ConvertTo-Json -Compress | Set-Content -Encoding utf8 (Join-Path $out "$Label.json")
    $shot = & $h shot $Label | Select-Object -Last 1
    Copy-Item $shot (Join-Path $out "$Label.png") -Force
    python (Join-Path $PSScriptRoot 'vmmap.py') --modules
} finally {
    & $h quit
}
