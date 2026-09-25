# Rebuild the OpenXR Simulator runtime DLL.
#
# All paths derive from this script's location. Uses the Visual Studio C++ toolchain with
# Ninja, and the header-only Vulkan headers in tools\Vulkan-Headers — no Vulkan SDK needed,
# because the runtime resolves every Vulkan entry point at run time and only needs the
# headers to compile.
#
# Output: tools\OpenXR-Simulator\bin\openxr_simulator.dll        (-Arch x64, default)
#         tools\OpenXR-Simulator-x86\bin\openxr_simulator.dll    (-Arch x86, for 32-bit MOHA.exe)
#
# The x86 build needs its own checkout because CMakeLists.txt hard-codes the output to
# <source>\bin and writes the manifest's library_path from it. The checkout is created from
# the x64 one (same commit) on first use.
#
# This does NOT register the runtime. See SETUP.md — registering would replace the
# machine-wide OpenXR runtime. Use run-with-openxr-sim.ps1 instead, which sets
# XR_RUNTIME_JSON for one process and leaves the registry alone.

param([ValidateSet('x64', 'x86')] [string] $Arch = 'x64')

$ErrorActionPreference = 'Stop'

$src = Join-Path $PSScriptRoot 'OpenXR-Simulator'
if ($Arch -eq 'x86') {
    $x64src = $src
    $src    = Join-Path $PSScriptRoot 'OpenXR-Simulator-x86'
    if (-not (Test-Path $src)) {
        git clone --quiet $x64src $src
        if ($LASTEXITCODE -ne 0) { throw 'clone for the x86 build failed' }
        git -C $src remote set-url origin (git -C $x64src remote get-url origin)
    }
}
$build         = Join-Path $src 'build'
$vulkanInclude = (Join-Path $PSScriptRoot 'Vulkan-Headers\include') -replace '\\', '/'

if (-not (Test-Path $src)) { throw "OpenXR-Simulator not found at $src" }
if (-not (Test-Path $vulkanInclude)) { throw "Vulkan headers not found at $vulkanInclude" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere not found at $vswhere" }
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'No Visual Studio installation with the C++ toolchain was found.' }
Write-Host "VS     : $vsRoot"
Write-Host "arch   : $Arch -> $src\bin"
Write-Host "vulkan : $vulkanInclude"

$cmakeDir = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaDir = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
if (Test-Path $cmakeDir) { $env:PATH = "$cmakeDir;$ninjaDir;$env:PATH" }

Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments "-arch=$Arch -host_arch=x64" | Out-Null

# The -D arguments are double-quoted so PowerShell expands the variable and still passes
# each as ONE token. Unquoted, $vulkanInclude was passed literally and the build failed
# with 'vulkan/vulkan.h: No such file'.
#
# x86: XRAPI_CALL is __stdcall there, so the entry point is exported decorated as
# _xrNegotiateLoaderRuntimeInterface@8 and the loader's GetProcAddress of the plain name fails.
# The /EXPORT alias adds the undecorated name without touching the simulator's source.
$extra = @()
if ($Arch -eq 'x86') {
    $extra += '-DCMAKE_SHARED_LINKER_FLAGS=/EXPORT:xrNegotiateLoaderRuntimeInterface=_xrNegotiateLoaderRuntimeInterface@8'
}
cmake -S $src -B $build -G Ninja "-DCMAKE_BUILD_TYPE=Release" "-DSIMXR_VULKAN_INCLUDE_DIR=$vulkanInclude" @extra
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

Write-Host 'BUILD OK' -ForegroundColor Green
