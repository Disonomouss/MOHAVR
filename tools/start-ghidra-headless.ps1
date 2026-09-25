<#
.SYNOPSIS
    Serve the GhidraMCP REST API from a headless Ghidra -- no GUI -- so the `ghidra` MCP
    server in .mcp.json works without opening CodeBrowser.

.DESCRIPTION
    Runs com.xebyte.headless.GhidraMCPHeadlessServer through Ghidra's own support\launch.bat,
    which builds the classpath (including the GhidraMCP extension already deployed to
    %APPDATA%\ghidra) exactly as analyzeHeadless does. It opens ghidra-projects\MOHAVR.gpr and
    loads MOHA.exe, which was already auto-analyzed, so it is ready as soon as it is listening.

    Same port as the GUI plugin (8089): run one or the other, not both. The project lock also
    means the GUI cannot open MOHAVR.gpr while this runs.

    Foreground process; Ctrl+C (or Stop-Process) stops it. Changes made through the API are
    saved to the project only when a save endpoint is called (e.g. the `save_program` tool).

.EXAMPLE
    tools/start-ghidra-headless.ps1
    tools/start-ghidra-headless.ps1 -MaxMem 24G -Port 8090
#>
param(
    [string] $Program = '/MOHA.exe',   # project path, leading slash required
    [int]    $Port    = 8089,
    [string] $MaxMem  = '16G'
)

$ErrorActionPreference = 'Stop'

$root    = Split-Path $PSScriptRoot -Parent
$project = Join-Path $root 'ghidra-projects\MOHAVR.gpr'
if (-not (Test-Path $project)) { throw "Ghidra project not found: $project" }

# Ghidra install: GHIDRA_PATH from the environment, else from tools\ghidra-mcp\.env.
$ghidra = $env:GHIDRA_PATH
if (-not $ghidra) {
    $line = Select-String -Path (Join-Path $PSScriptRoot 'ghidra-mcp\.env') -Pattern '^\s*GHIDRA_PATH\s*=\s*(.+)$' |
            Select-Object -First 1
    if ($line) { $ghidra = $line.Matches[0].Groups[1].Value.Trim() }
}
if (-not $ghidra -or -not (Test-Path $ghidra)) { throw 'Ghidra not found. Set GHIDRA_PATH.' }

# JDK 21 -- the system default java is 25.
if (-not $env:JAVA_HOME -or $env:JAVA_HOME -notmatch 'jdk-21') {
    $jdk = Get-ChildItem 'C:\Program Files\Eclipse Adoptium' -Directory -ErrorAction SilentlyContinue |
           Where-Object Name -like 'jdk-21*' | Sort-Object Name -Descending | Select-Object -First 1
    if ($jdk) { $env:JAVA_HOME = $jdk.FullName; $env:PATH = "$($jdk.FullName)\bin;$env:PATH" }
}

Write-Host "ghidra  : $ghidra"
Write-Host "project : $project ($Program)"
Write-Host "listen  : http://127.0.0.1:$Port  heap $MaxMem"

# '""' not '': Windows PowerShell drops an empty-string argument to a native command, which
# shifts every later launch.bat argument by one (the JVM then dies on ghidra.GhidraClassLoader).
& (Join-Path $ghidra 'support\launch.bat') fg jdk GhidraMCP-Headless $MaxMem '""' `
    com.xebyte.headless.GhidraMCPHeadlessServer `
    --port $Port --project $project --program $Program
