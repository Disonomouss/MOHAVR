<#
.SYNOPSIS
    Remove MOHAVR from Medal of Honor: Airborne.

.DESCRIPTION
    Deletes only MOHAVR's own files from the game's Binaries folder: dinput8.dll (only if it is
    MOHAVR's), MOHAVR-host.exe, MOHAVR.ini and MOHAVR's log files. The game's files are not touched.
    Your in-headset settings in %LOCALAPPDATA%\MOHAVR are kept unless you pass -RemoveSettings.

.EXAMPLE
    .\uninstall.ps1
    .\uninstall.ps1 -RemoveSettings
#>
param([string] $GameDir, [switch] $RemoveSettings)

$ErrorActionPreference = 'Stop'
$dataDir = Join-Path $env:LOCALAPPDATA 'MOHAVR'
$manifest = Join-Path $dataDir 'install.json'
$ours = 'dinput8.dll', 'MOHAVR-host.exe', 'MOHAVR.ini', 'MOHAVR.log', 'MOHAVR.prev.log', 'MOHAVR-host.log', 'MOHAVR-host.prev.log'

try {
    if (-not $GameDir -and (Test-Path $manifest)) { $GameDir = (Get-Content $manifest -Raw | ConvertFrom-Json).gameDir }
    if (-not $GameDir) { throw 'No install record found. Run again with -GameDir "<game folder>".' }
    $bin = Join-Path $GameDir 'UnrealEngine3\Binaries'
    if (-not (Test-Path $bin)) { throw "$bin does not exist" }
    if (Get-Process MOHA, MOHAVR-host -ErrorAction SilentlyContinue) { throw 'The game (or MOHAVR-host) is running -- quit it first.' }

    $dll = Join-Path $bin 'dinput8.dll'
    if (Test-Path $dll) {
        $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dll))
        if ($text -notmatch 'MOHAVR-host\.exe') { throw "The dinput8.dll in $bin is not MOHAVR's -- leaving it alone. Nothing was removed." }
    }
    $removed = 0
    foreach ($f in $ours) {
        $p = Join-Path $bin $f
        if (Test-Path $p) { Remove-Item -LiteralPath $p -Force; $removed++ }
    }
    if (Test-Path $manifest) { Remove-Item $manifest -Force }
    if ($RemoveSettings -and (Test-Path $dataDir)) { Remove-Item $dataDir -Recurse -Force; Write-Host "Removed $dataDir" }
    Write-Host "MOHAVR removed from $bin ($removed file(s))."
    exit 0
}
catch {
    Write-Host "UNINSTALL FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
