<#
.SYNOPSIS
    Run a program against the OpenXR Simulator without changing the system runtime.

.DESCRIPTION
    Sets XR_RUNTIME_JSON for the launched process only. Your machine-wide
    ActiveRuntime (Virtual Desktop Streamer) is left completely alone, so this
    is fully reversible -- close the process and nothing has changed.

    Picks the runtime that matches the target's bitness from its PE header: MOHA.exe is
    32-bit and needs OpenXR-Simulator-x86 (build it with rebuild-openxr-simulator.ps1 -Arch x86);
    a 64-bit exe gets OpenXR-Simulator. A 32-bit process cannot load the 64-bit runtime.

    To register the simulator machine-wide instead (needs admin, stashes the
    previous runtime so it can be restored):
        .\OpenXR-Simulator\activate_simulator.ps1
        .\OpenXR-Simulator\deactivate_simulator.ps1

.EXAMPLE
    .\run-with-openxr-sim.ps1 -Exe 'C:\Program Files (x86)\Steam\steamapps\common\Medal of Honor Airborne\UnrealEngine3\Binaries\MOHA.exe'
#>
param(
    [Parameter(Mandatory)][string]$Exe,
    [string[]]$ExeArgs = @()
)

$ErrorActionPreference = 'Stop'

$bytes   = [IO.File]::ReadAllBytes((Resolve-Path $Exe).Path)
$machine = [BitConverter]::ToUInt16($bytes, [BitConverter]::ToInt32($bytes, 0x3c) + 4)
switch ($machine) {
    0x014C { $dir = 'OpenXR-Simulator-x86'; $rebuild = '.\rebuild-openxr-simulator.ps1 -Arch x86' }
    0x8664 { $dir = 'OpenXR-Simulator';     $rebuild = '.\rebuild-openxr-simulator.ps1' }
    default { throw ('Unsupported PE machine 0x{0:X4} in {1}' -f $machine, $Exe) }
}

$manifest = Join-Path $PSScriptRoot "$dir\bin\openxr_simulator.json"
if (-not (Test-Path $manifest)) { throw "Runtime not built. Run $rebuild first." }

$env:XR_RUNTIME_JSON = (Resolve-Path $manifest).Path
Write-Host "XR_RUNTIME_JSON = $env:XR_RUNTIME_JSON" -ForegroundColor Cyan
& $Exe @ExeArgs
