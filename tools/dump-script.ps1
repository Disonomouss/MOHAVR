<#
.SYNOPSIS
    Decompile every UnrealScript class in MOHA's script packages to .uc text files, for grepping.

.DESCRIPTION
    Uses UE Explorer's own library (Eliot.UELib.dll) headless. MOHA's cooked packages are
    LZO-compressed and UELib loads 0 objects from them, so each package is first decompressed
    with Gildor's decompress.exe into work\decompressed (skipped if already there).

    Output: work\script\<Package>\<Class>.uc, plus work\script\<Package>\_errors.txt listing
    classes that failed to decompile.

    Native operators show as __NFUN_nnn__ (cooked builds drop their names).

.EXAMPLE
    tools\dump-script.ps1                                  # Core, Engine, GameFramework, MOHAGame
    tools\dump-script.ps1 -Packages Engine
#>
param(
    [string[]] $Packages = @('Core', 'Engine', 'GameFramework', 'MOHAGame'),
    [string]   $UEExplorer = 'C:\Users\j_tom\Tools\UE-Explorer\ue-explorer',
    [string]   $Decompress = 'C:\Users\j_tom\Tools\umodel\decompress\decompress.exe'
)

$ErrorActionPreference = 'Stop'

$root    = Split-Path $PSScriptRoot -Parent
$gameDir = (Get-Content (Join-Path $PSScriptRoot 'gamedir.txt') | Where-Object { $_ -and $_ -notmatch '^\s*#' } | Select-Object -First 1).Trim()
if ($env:MOHAVR_GAMEDIR) { $gameDir = $env:MOHAVR_GAMEDIR }
$cooked  = Join-Path $gameDir 'UnrealEngine3\MOHAGame\CookedPC'
$decDir  = Join-Path $root 'work\decompressed'
$outRoot = Join-Path $root 'work\script'
New-Item -ItemType Directory -Force $decDir, $outRoot | Out-Null

Get-ChildItem $UEExplorer -Filter *.dll | ForEach-Object { try { [void][Reflection.Assembly]::LoadFrom($_.FullName) } catch {} }
$utf8 = New-Object Text.UTF8Encoding($false)

foreach ($p in $Packages) {
    $dec = Join-Path $decDir "$p.xxx"
    if (-not (Test-Path $dec)) {
        & $Decompress -game=moha "-out=$decDir" (Join-Path $cooked "$p.xxx") | Out-Null
        if (-not (Test-Path $dec)) { throw "decompress failed for $p" }
    }

    # UELib prints its header dump through Console.WriteLine; discard it.
    $pkg = [UELib.UnrealLoader]::LoadFullPackage($dec, [IO.FileAccess]::Read) 6>$null
    $outDir = Join-Path $outRoot $p
    New-Item -ItemType Directory -Force $outDir | Out-Null

    $classes = @($pkg.Objects | Where-Object { $_.GetType().Name -eq 'UClass' })
    $errors  = New-Object Collections.Generic.List[string]
    $ok = 0
    foreach ($c in $classes) {
        try {
            $text = $c.Decompile()
            [IO.File]::WriteAllText((Join-Path $outDir "$($c.Name).uc"), $text, $utf8)
            $ok++
        } catch {
            $errors.Add("$($c.Name): $($_.Exception.InnerException.Message) $($_.Exception.Message)")
        }
    }
    if ($errors.Count) { [IO.File]::WriteAllLines((Join-Path $outDir '_errors.txt'), $errors, $utf8) }
    Write-Host ("{0,-14} {1,5} classes, {2,5} written, {3} failed" -f $p, $classes.Count, $ok, $errors.Count)
}
