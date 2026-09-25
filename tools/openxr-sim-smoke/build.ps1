# Build smoke.cpp for x86 and x64, then run each against the simulator DLL of the same
# bitness. Exit 0 only if both pass.
#
#   tools\openxr-sim-smoke\build.ps1
$ErrorActionPreference = 'Stop'

$tools   = Split-Path $PSScriptRoot -Parent
$include = Join-Path $tools 'OpenXR-Simulator\include'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot  = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars  = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvarsall.bat'

$failed = 0
foreach ($arch in 'x86', 'x64') {
    $out = Join-Path $PSScriptRoot "smoke-$arch.exe"
    $dll = if ($arch -eq 'x86') { Join-Path $tools 'OpenXR-Simulator-x86\bin\openxr_simulator.dll' }
           else                 { Join-Path $tools 'OpenXR-Simulator\bin\openxr_simulator.dll' }
    $vcArch = if ($arch -eq 'x86') { 'x64_x86' } else { 'x64' }
    cmd /c "`"$vcvars`" $vcArch >nul && cl /nologo /EHsc /O2 /I`"$include`" `"$PSScriptRoot\smoke.cpp`" /Fe`"$out`" /Fo`"$PSScriptRoot\\`" >nul"
    if ($LASTEXITCODE -ne 0) { throw "compile failed for $arch" }
    Write-Host "=== $arch against $dll" -ForegroundColor Cyan
    & $out $dll
    if ($LASTEXITCODE -ne 0) { $failed++ }
}
if ($failed) { Write-Host "$failed FAILED" -ForegroundColor Red; exit 1 }
Write-Host 'ALL PASS' -ForegroundColor Green
