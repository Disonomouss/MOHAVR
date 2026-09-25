<#
.SYNOPSIS
    Build the player package: dist\MOHAVR-<version>.zip (dinput8.dll, MOHAVR-host.exe, MOHAVR.ini,
    install/uninstall scripts, README).

.DESCRIPTION
    The version is MOHAVR_VERSION in src\mohavr\dllmain.cpp. The shipped MOHAVR.ini is config\MOHAVR.ini
    as it is, and it must use the machine's OpenXR runtime (RuntimeJson empty); a simulator path in it
    fails the package. -NoBuild packages the existing build outputs.

.EXAMPLE
    tools\package.ps1
    tools\package.ps1 -NoBuild
#>
param([switch] $NoBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $NoBuild) {
    & powershell -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}
$ver = if ((Get-Content (Join-Path $root 'src\mohavr\dllmain.cpp') -Raw) -match '#define MOHAVR_VERSION "([^"]+)"') { $Matches[1] } else { throw 'MOHAVR_VERSION not found' }
$ini = Get-Content (Join-Path $root 'config\MOHAVR.ini') -Raw
if ($ini -match '(?m)^RuntimeJson=\S') { throw 'config\MOHAVR.ini has a RuntimeJson set -- a player package must use the system runtime' }

$files = [ordered]@{
    'dinput8.dll'     = 'build\x86\dinput8.dll'
    'MOHAVR-host.exe' = 'build\x64\MOHAVR-host.exe'
    'MOHAVR.ini'      = 'config\MOHAVR.ini'
    'install.ps1'     = 'release\install.ps1'
    'install.cmd'     = 'release\install.cmd'
    'uninstall.ps1'   = 'release\uninstall.ps1'
    'uninstall.cmd'   = 'release\uninstall.cmd'
    'README.md'       = 'release\README.md'
}
$stage = Join-Path $root "dist\MOHAVR-$ver"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($k in $files.Keys) {
    $src = Join-Path $root $files[$k]
    if (-not (Test-Path $src)) { throw "$($files[$k]) is missing" }
    Copy-Item $src (Join-Path $stage $k)
}
$zip = Join-Path $root "dist\MOHAVR-$ver.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$size = [math]::Round((Get-Item $zip).Length / 1KB)
Write-Host "package: $zip ($size KB, version $ver)"
Get-ChildItem $stage | ForEach-Object { Write-Host ("  {0,-16} {1,10:N0} bytes" -f $_.Name, $_.Length) }
