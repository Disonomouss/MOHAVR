<#
.SYNOPSIS
    Put the MOHAVR proxy into MOHA's Binaries folder, or take it out again -- leaving the
    folder exactly as it was.

.DESCRIPTION
    The mod ships only its own files (standing rule 1): dinput8.dll and MOHAVR.ini. At run time
    it adds MOHAVR.log / MOHAVR.prev.log beside them. Nothing that was already in the folder is
    ever modified.

    Safety:
      * refuses to overwrite a dinput8.dll it did not deploy (hash recorded in logs\deploy-state.json);
      * refuses while MOHA is running;
      * undeploy keeps copies of the mod logs in logs\modlogs\, removes only MOHAVR's own files,
        then verifies the folder listing equals the baseline recorded before the first deploy.

.EXAMPLE
    tools\deploy.ps1 deploy                                   # build\x86\dinput8.dll + config\MOHAVR.ini
    tools\deploy.ps1 deploy -Set 'Debug.TestWrongBuild=1'     # same, with an ini override for this test
    tools\deploy.ps1 undeploy
    tools\deploy.ps1 status
#>
param(
    [Parameter(Mandatory, Position = 0)][ValidateSet('deploy', 'undeploy', 'status')] [string] $Action,
    [string[]] $Set = @()
)

$ErrorActionPreference = 'Stop'

$root      = Split-Path $PSScriptRoot -Parent
$gameDir   = (Get-Content (Join-Path $PSScriptRoot 'gamedir.txt') | Where-Object { $_ -and $_ -notmatch '^\s*#' } | Select-Object -First 1).Trim()
if ($env:MOHAVR_GAMEDIR) { $gameDir = $env:MOHAVR_GAMEDIR }
$bin       = Join-Path $gameDir 'UnrealEngine3\Binaries'
$statePath = Join-Path $root 'logs\deploy-state.json'
$modLogs   = Join-Path $root 'logs\modlogs'
$ours      = 'dinput8.dll', 'MOHAVR.ini', 'MOHAVR.log', 'MOHAVR.prev.log'
New-Item -ItemType Directory -Force (Split-Path $statePath), $modLogs | Out-Null

function Load-State { if (Test-Path $statePath) { Get-Content $statePath -Raw | ConvertFrom-Json } }
function Save-State($s) { $s | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 $statePath }
function Hash($p) { (Get-FileHash $p -Algorithm SHA256).Hash }
function Assert-NotRunning { if (Get-Process MOHA -ErrorAction SilentlyContinue) { throw 'MOHA is running -- quit it first' } }

switch ($Action) {
    'deploy' {
        Assert-NotRunning
        $src = Join-Path $root 'build\x86\dinput8.dll'
        if (-not (Test-Path $src)) { throw 'build\x86\dinput8.dll not found -- run tools\build.ps1' }
        $s = Load-State
        $target = Join-Path $bin 'dinput8.dll'
        if (Test-Path $target) {
            if (-not $s -or $s.dllHash -ne (Hash $target)) {
                throw "a dinput8.dll that MOHAVR did not deploy is already in $bin -- refusing to overwrite it"
            }
        }
        if (-not $s -or -not $s.baseline) {
            $baseline = @(Get-ChildItem $bin -Force | ForEach-Object Name | Where-Object { $ours -notcontains $_ } | Sort-Object)
        } else { $baseline = @($s.baseline) }

        Copy-Item $src $target -Force
        $ini = Join-Path $bin 'MOHAVR.ini'
        $text = Get-Content (Join-Path $root 'config\MOHAVR.ini') -Raw
        foreach ($kv in $Set) {
            if ($kv -notmatch '^(\w+)\.(\w+)=(.*)$') { throw "bad -Set '$kv' (use Section.Key=Value)" }
            $sec, $key, $val = $Matches[1], $Matches[2], $Matches[3]
            $pattern = "(?ms)(^\[$sec\][^\[]*?^$key=)[^\r\n]*"
            if ($text -notmatch $pattern) { throw "ini has no [$sec] $key" }
            $text = [regex]::Replace($text, $pattern, "`${1}$val")
        }
        [IO.File]::WriteAllText($ini, $text, (New-Object Text.UTF8Encoding($false)))

        Save-State ([pscustomobject]@{
            deployed = (Get-Date).ToString('o'); dllHash = (Hash $target); iniOverrides = $Set; baseline = $baseline })
        Write-Host ("deployed dinput8.dll + MOHAVR.ini to {0}{1}" -f $bin, $(if ($Set) { " (ini: $($Set -join ', '))" } else { '' }))
    }

    'undeploy' {
        Assert-NotRunning
        $s = Load-State
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        foreach ($l in 'MOHAVR.log', 'MOHAVR.prev.log') {
            $p = Join-Path $bin $l
            if (Test-Path $p) { Copy-Item $p (Join-Path $modLogs "$stamp-$l") }
        }
        $dll = Join-Path $bin 'dinput8.dll'
        if ((Test-Path $dll) -and (-not $s -or $s.dllHash -ne (Hash $dll))) {
            throw "dinput8.dll in $bin is not the one MOHAVR deployed -- leaving it alone"
        }
        foreach ($f in $ours) { $p = Join-Path $bin $f; if (Test-Path $p) { Remove-Item -LiteralPath $p -Force } }

        if ($s -and $s.baseline) {
            $now = @(Get-ChildItem $bin -Force | ForEach-Object Name | Sort-Object)
            $diff = Compare-Object @($s.baseline) $now
            if ($diff) { $diff | ForEach-Object { "  {0} {1}" -f $_.SideIndicator, $_.InputObject }; throw 'Binaries folder does not match its pre-deploy baseline' }
            Write-Host "undeployed; Binaries matches its baseline ($($now.Count) entries); logs kept in logs\modlogs\$stamp-*"
        } else { Write-Host 'undeployed (no baseline recorded)' }
    }

    'status' {
        $s = Load-State
        $dll = Join-Path $bin 'dinput8.dll'
        if (Test-Path $dll) {
            $mine = $s -and $s.dllHash -eq (Hash $dll)
            "dinput8.dll present ($(if ($mine) { 'MOHAVR' } else { 'NOT MOHAVR' })), deployed $($s.deployed), ini overrides: $($s.iniOverrides -join ', ')"
        } else { 'not deployed' }
        foreach ($f in 'MOHAVR.ini', 'MOHAVR.log') { if (Test-Path (Join-Path $bin $f)) { "  $f present" } }
    }
}
