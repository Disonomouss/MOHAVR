<#
.SYNOPSIS
    Back up and restore the player's MOHA user folder (Config\ and Saved\) around a test (D8).

.DESCRIPTION
    The game keeps the player's settings and progress in
        <Documents>\EA Games\Medal of Honor Airborne(tm)\{Config,Saved}
    where <Documents> is the real known folder (redirected to OneDrive on this machine).
    The game rewrites its ini files on exit, so a test must restore AFTER the run, not only
    back up before it.

    Backups go to <repo>\logs\backup\<timestamp>\ -- inside the project, never beside the
    originals (that folder syncs to OneDrive).

.EXAMPLE
    $b = tools\userdata.ps1 backup          # prints and returns the backup folder
    tools\userdata.ps1 diff    -From $b     # what the game changed since the backup
    tools\userdata.ps1 restore -From $b     # put the player's files back exactly
#>
param(
    [Parameter(Mandatory, Position = 0)][ValidateSet('backup', 'restore', 'diff', 'where')] [string] $Action,
    [string] $From
)

$ErrorActionPreference = 'Stop'

$docs    = [Environment]::GetFolderPath('MyDocuments')
$userDir = Join-Path $docs 'EA Games\Medal of Honor Airborne(tm)'
$parts   = 'Config', 'Saved'
$root    = Split-Path $PSScriptRoot -Parent

function Get-Manifest([string] $base) {
    $m = @{}
    foreach ($p in $parts) {
        $d = Join-Path $base $p
        if (Test-Path $d) {
            Get-ChildItem $d -Recurse -File | ForEach-Object {
                $m[$_.FullName.Substring($base.Length + 1)] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
            }
        }
    }
    $m
}

switch ($Action) {
    'where' { $userDir }

    'backup' {
        if (-not (Test-Path $userDir)) { throw "user folder not found: $userDir" }
        $dest = Join-Path $root ('logs\backup\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
        foreach ($p in $parts) {
            $src = Join-Path $userDir $p
            if (Test-Path $src) { Copy-Item $src (Join-Path $dest $p) -Recurse -Force }
        }
        $n = (Get-ChildItem $dest -Recurse -File).Count
        Write-Host "backed up $n files -> $dest"
        $dest
    }

    'diff' {
        if (-not $From) { throw '-From <backup folder> is required' }
        $a = Get-Manifest $From; $b = Get-Manifest $userDir
        foreach ($k in ($a.Keys + $b.Keys | Sort-Object -Unique)) {
            if (-not $b.ContainsKey($k))      { "removed  $k" }
            elseif (-not $a.ContainsKey($k))  { "added    $k" }
            elseif ($a[$k] -ne $b[$k])        { "changed  $k" }
        }
    }

    'restore' {
        if (-not $From -or -not (Test-Path $From)) { throw '-From <backup folder> is required' }
        if (Get-Process MOHA -ErrorAction SilentlyContinue) { throw 'MOHA is running; it would overwrite the restore on exit' }
        foreach ($p in $parts) {
            $src = Join-Path $From $p
            if (-not (Test-Path $src)) { continue }
            $dst = Join-Path $userDir $p
            # Files the game added during the test are removed; everything else is put back byte-exact.
            $keep = @{}
            Get-ChildItem $src -Recurse -File | ForEach-Object { $keep[$_.FullName.Substring($src.Length)] = $true }
            Get-ChildItem $dst -Recurse -File -ErrorAction SilentlyContinue |
                Where-Object { -not $keep.ContainsKey($_.FullName.Substring($dst.Length)) } |
                ForEach-Object { Write-Host "  removing added file $($_.FullName)"; Remove-Item -LiteralPath $_.FullName -Force }
            Copy-Item (Join-Path $src '*') $dst -Recurse -Force
        }
        $left = @(& $PSCommandPath diff -From $From)
        if ($left.Count) { $left; throw 'restore incomplete' }
        Write-Host "restored from $From (verified identical)"
    }
}
