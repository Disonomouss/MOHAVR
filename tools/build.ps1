<#
.SYNOPSIS
    Build the MOHAVR proxy (x86 dinput8.dll) with the Visual Studio toolchain and Ninja.

.DESCRIPTION
    Output: build\x86\dinput8.dll. Verifies the result is PE32 (machine 0x14C) and exports
    DirectInput8Create under its plain name -- on x86, WINAPI (__stdcall) names are decorated
    and the loader would not find a decorated export.

.EXAMPLE
    tools\build.ps1
    tools\build.ps1 -Config Debug
#>
param([ValidateSet('Release', 'RelWithDebInfo', 'Debug')] [string] $Config = 'RelWithDebInfo')

$ErrorActionPreference = 'Stop'

$root  = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build\x86'

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
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x86 -host_arch=x64' | Out-Null

# vcpkg manifest mode (vcpkg.json): dependencies land in build\x86\vcpkg_installed, never in
# the shared vcpkg tree. Static triplet: the mod stays one DLL with a static CRT.
$env:VCPKG_ROOT = $vcpkgRoot
$toolchain = (Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake') -replace '\\', '/'
if (-not (Test-Path $toolchain)) { throw "vcpkg toolchain not found at $toolchain (set VCPKG_ROOT)" }

cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
      "-DVCPKG_TARGET_TRIPLET=x86-windows-static" "-DVCPKG_OVERLAY_TRIPLETS=$($vcpkgRoot -replace '\\','/')/triplets/community"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

$dll = Join-Path $build 'dinput8.dll'
$b = [IO.File]::ReadAllBytes($dll)
$machine = [BitConverter]::ToUInt16($b, [BitConverter]::ToInt32($b, 0x3c) + 4)
if ($machine -ne 0x14C) { throw ('built DLL is machine 0x{0:X4}, not x86' -f $machine) }
$exports = & dumpbin /nologo /exports $dll | Select-String '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)' |
           ForEach-Object { $_.Matches[0].Groups[1].Value }
if ($exports -notcontains 'DirectInput8Create') { throw "DirectInput8Create not exported by its plain name: $($exports -join ', ')" }
Write-Host ("BUILD OK: {0} ({1:N0} bytes, x86) exports: {2}" -f $dll, (Get-Item $dll).Length, ($exports -join ', ')) -ForegroundColor Green
