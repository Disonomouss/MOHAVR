<#
.SYNOPSIS
    Install MOHAVR (VR for Medal of Honor: Airborne) into the game's Binaries folder.

.DESCRIPTION
    Copies three files next to MOHA.exe: dinput8.dll (the mod), MOHAVR-host.exe (the 64-bit VR host)
    and MOHAVR.ini (settings). Nothing of the game's own is changed or replaced. The mod will not
    install over a dinput8.dll that isn't MOHAVR's (another mod): remove that one first.

    The game is found through Steam (every library folder). Pass -GameDir if it's somewhere else.
    What was installed is recorded in %LOCALAPPDATA%\MOHAVR\install.json, which uninstall.ps1 uses.
    Your in-headset settings (%LOCALAPPDATA%\MOHAVR\MOHAVR.user.ini) are never touched.

.EXAMPLE
    .\install.ps1
    .\install.ps1 -GameDir "D:\Games\Medal of Honor Airborne"
#>
param([string] $GameDir)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$files = 'dinput8.dll', 'MOHAVR-host.exe', 'MOHAVR.ini'
$dataDir = Join-Path $env:LOCALAPPDATA 'MOHAVR'
$manifest = Join-Path $dataDir 'install.json'

function Find-Game {
    $roots = @()
    foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        try {
            $p = Get-ItemProperty $k -ErrorAction Stop
            foreach ($v in 'SteamPath', 'InstallPath') { if ($p.$v) { $roots += ($p.$v -replace '/', '\') } }
        } catch {}
    }
    $libs = @()
    foreach ($r in ($roots | Select-Object -Unique)) {
        $libs += $r
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += ($m.Groups[1].Value -replace '\\\\', '\') }
        }
    }
    foreach ($l in ($libs | Select-Object -Unique)) {
        $g = Join-Path $l 'steamapps\common\Medal of Honor Airborne'
        if (Test-Path (Join-Path $g 'UnrealEngine3\Binaries\MOHA.exe')) { return $g }
    }
    return $null
}

try {
    foreach ($f in $files) { if (-not (Test-Path (Join-Path $here $f))) { throw "$f is missing from this folder -- unpack the whole zip first" } }
    if (-not $GameDir) { $GameDir = Find-Game }
    if (-not $GameDir) { throw 'Medal of Honor Airborne was not found in any Steam library. Run again with -GameDir "<game folder>".' }
    $bin = Join-Path $GameDir 'UnrealEngine3\Binaries'
    if (-not (Test-Path (Join-Path $bin 'MOHA.exe'))) { throw "No UnrealEngine3\Binaries\MOHA.exe under $GameDir" }
    if (Get-Process MOHA -ErrorAction SilentlyContinue) { throw 'The game is running -- quit it first.' }

    $target = Join-Path $bin 'dinput8.dll'
    if (Test-Path $target) {
        # Ours if it carries MOHAVR's own marker string (any MOHAVR version) -- then this is an update.
        $bytes = [IO.File]::ReadAllBytes($target)
        $text = [Text.Encoding]::ASCII.GetString($bytes)
        if ($text -notmatch 'MOHAVR-host\.exe') {
            throw "There is already a dinput8.dll in $bin that is not MOHAVR (another mod?). Remove or rename it first; MOHAVR will not overwrite it."
        }
        Write-Host 'Updating an existing MOHAVR install.'
    }

    New-Item -ItemType Directory -Force $dataDir | Out-Null
    $ini = Join-Path $bin 'MOHAVR.ini'
    if ((Test-Path $ini) -and ((Get-FileHash $ini).Hash -ne (Get-FileHash (Join-Path $here 'MOHAVR.ini')).Hash)) {
        Copy-Item $ini (Join-Path $dataDir 'MOHAVR.ini.previous') -Force
        Write-Host "Your previous MOHAVR.ini was saved to $dataDir\MOHAVR.ini.previous"
    }
    foreach ($f in $files) { Copy-Item (Join-Path $here $f) (Join-Path $bin $f) -Force }

    [pscustomobject]@{
        installed = (Get-Date).ToString('o')
        gameDir   = $GameDir
        files     = @($files | ForEach-Object { [pscustomobject]@{ name = $_; sha256 = (Get-FileHash (Join-Path $bin $_)).Hash } })
    } | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 $manifest

    Write-Host "MOHAVR installed into $bin"
    Write-Host 'Start your VR runtime (e.g. Virtual Desktop), then start the game from Steam as usual.'
    Write-Host 'The monitor shows the headset view while the game is in front (see README).'
    exit 0
}
catch {
    Write-Host "INSTALL FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
