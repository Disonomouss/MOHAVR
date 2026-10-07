<#
.SYNOPSIS
    Build MOHAVR: the x86 game-side mod (dinput8.dll) and the x64 OpenXR host (MOHAVR-host.exe).

.DESCRIPTION
    Visual Studio toolchain + Ninja, vcpkg manifest mode (dependencies in build\<arch>\vcpkg_installed,
    static triplets so each output is a single self-contained binary).

    Outputs and checks:
      build\x86\dinput8.dll      PE32 (machine 0x14C), exports DirectInput8Create by its plain name
                                 (on x86, WINAPI names are decorated and the loader would miss them)
      build\x64\MOHAVR-host.exe  PE32+ (machine 0x8664)

    Each architecture is built in its own child process: the VS dev shell changes the environment
    for the whole process and can't be switched between x86 and x64 in one.

.EXAMPLE
    tools\build.ps1              # both
    tools\build.ps1 -Arch x86    # just the mod
#>
param(
    [ValidateSet('all', 'x86', 'x64')] [string] $Arch = 'all',
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')] [string] $Config = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ($Arch -eq 'all') {
    foreach ($a in 'x86', 'x64') {
        # The VS dev shell prints harmless noise on stderr, which Windows PowerShell 5.1 would turn
        # into a terminating error here; judge the child by its exit code instead.
        $ErrorActionPreference = 'Continue'
        & powershell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Arch $a -Config $Config 2>&1 | ForEach-Object { "$_" }
        $rc = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($rc -ne 0) { throw "$a build failed ($rc)" }
    }
    return
}

$root  = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build\$Arch"

# Resolve vcpkg BEFORE entering the VS dev shell: the dev shell sets VCPKG_ROOT to Visual
# Studio's bundled copy, which would silently replace ours.
$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'C:\dev\vcpkg' }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot  = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'No Visual Studio installation with the C++ toolchain was found.' }
$cmakeDir = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaDir = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
if (Test-Path $cmakeDir) { $env:PATH = "$cmakeDir;$ninjaDir;$env:PATH" }

Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments "-arch=$Arch -host_arch=x64" | Out-Null

# No C:\dev\vcpkg (a fresh machine): Visual Studio's bundled copy, chosen explicitly (it builds the same static triplets).
if (-not (Test-Path $vcpkgRoot) -and (Test-Path (Join-Path $vsRoot 'VC\vcpkg\scripts'))) {
    $vcpkgRoot = Join-Path $vsRoot 'VC\vcpkg'
}
$env:VCPKG_ROOT = $vcpkgRoot
$toolchain = (Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake') -replace '\\', '/'
if (-not (Test-Path $toolchain)) { throw "vcpkg toolchain not found at $toolchain (set VCPKG_ROOT)" }
$triplet = "$Arch-windows-static"

cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
      "-DVCPKG_TARGET_TRIPLET=$triplet" "-DVCPKG_OVERLAY_TRIPLETS=$($vcpkgRoot -replace '\\','/')/triplets/community"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

function Get-Machine($p) { $b = [IO.File]::ReadAllBytes($p); [BitConverter]::ToUInt16($b, [BitConverter]::ToInt32($b, 0x3c) + 4) }

if ($Arch -eq 'x86') {
    $out = Join-Path $build 'dinput8.dll'
    if ((Get-Machine $out) -ne 0x14C) { throw 'dinput8.dll is not x86' }
    $exports = & dumpbin /nologo /exports $out | Select-String '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)' |
               ForEach-Object { $_.Matches[0].Groups[1].Value }
    if ($exports -notcontains 'DirectInput8Create') { throw "DirectInput8Create not exported by its plain name: $($exports -join ', ')" }
    Write-Host ("BUILD OK: {0} ({1:N0} bytes, x86) exports: {2}" -f $out, (Get-Item $out).Length, ($exports -join ', ')) -ForegroundColor Green
} else {
    $out = Join-Path $build 'MOHAVR-host.exe'
    if ((Get-Machine $out) -ne 0x8664) { throw 'MOHAVR-host.exe is not x64' }
    Write-Host ("BUILD OK: {0} ({1:N0} bytes, x64)" -f $out, (Get-Item $out).Length) -ForegroundColor Green
}
