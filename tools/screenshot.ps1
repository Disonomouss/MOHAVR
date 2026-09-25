<#
.SYNOPSIS
    Capture the primary display to a PNG. Works for the game because it runs borderless
    (windowed=2 in system.xml); an exclusive-fullscreen surface would capture black.

.EXAMPLE
    tools/screenshot.ps1 shot.png
    tools/screenshot.ps1            # -> $env:TEMP\mohavr_shot.png
#>
param([string] $Out)

if (-not $Out) { $Out = Join-Path $env:TEMP 'mohavr_shot.png' }
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$dir = Split-Path $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$bmp.Save($Out)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $Out ($($b.Width)x$($b.Height))"
