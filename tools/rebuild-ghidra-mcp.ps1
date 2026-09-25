# Rebuild + redeploy the GhidraMCP Ghidra extension.
#
# Paths are derived, not hardcoded:
#   * the repo is found from this script's own location ($PSScriptRoot),
#   * Ghidra comes from GHIDRA_PATH in tools/ghidra-mcp/.env (or the env var of the same name),
#   * JDK 21 and Maven are discovered, with overrides via JDK21_HOME / MAVEN_HOME.
#
# JDK 21 is pinned for the build on purpose: ghidra-mcp sets maven.compiler.release=21 and
# the system default JDK here is 25.

$ErrorActionPreference = 'Stop'

$repo = Join-Path $PSScriptRoot 'ghidra-mcp'
if (-not (Test-Path $repo)) { throw "ghidra-mcp not found at $repo" }

# --- Ghidra -----------------------------------------------------------------------------
$ghidra = $env:GHIDRA_PATH
if (-not $ghidra) {
    $envFile = Join-Path $repo '.env'
    if (Test-Path $envFile) {
        $line = Select-String -Path $envFile -Pattern '^\s*GHIDRA_PATH\s*=\s*(.+)$' |
                Select-Object -First 1
        if ($line) { $ghidra = $line.Matches[0].Groups[1].Value.Trim() }
    }
}
if (-not $ghidra -or -not (Test-Path $ghidra)) {
    throw "Ghidra not found. Set GHIDRA_PATH in $repo\.env (or as an environment variable)."
}

# --- JDK 21 -----------------------------------------------------------------------------
$jdk = $env:JDK21_HOME
if (-not $jdk) {
    $jdk = Get-ChildItem 'C:\Program Files\Eclipse Adoptium' -Directory -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -like 'jdk-21*' } |
           Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $jdk) { throw 'JDK 21 not found. Set JDK21_HOME, or install EclipseAdoptium.Temurin.21.JDK.' }

# --- Maven ------------------------------------------------------------------------------
$mvnBin = $env:MAVEN_HOME
if ($mvnBin) { $mvnBin = Join-Path $mvnBin 'bin' }
if (-not $mvnBin -or -not (Test-Path $mvnBin)) {
    $onPath = (Get-Command mvn -ErrorAction SilentlyContinue)
    if ($onPath) { $mvnBin = Split-Path $onPath.Source -Parent }
}
if (-not $mvnBin -or -not (Test-Path $mvnBin)) {
    $guess = Get-ChildItem 'E:\Tools','C:\Tools' -Directory -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -like 'apache-maven-*' } |
             Sort-Object Name -Descending | Select-Object -First 1
    if ($guess) { $mvnBin = Join-Path $guess.FullName 'bin' }
}
if (-not $mvnBin -or -not (Test-Path $mvnBin)) { throw 'Maven not found. Set MAVEN_HOME or put mvn on PATH.' }

Write-Host "repo   : $repo"
Write-Host "ghidra : $ghidra"
Write-Host "jdk21  : $jdk"
Write-Host "maven  : $mvnBin"

$env:JAVA_HOME = $jdk
$env:PATH = "$jdk\bin;$mvnBin;$env:PATH"

Set-Location $repo
uv run --no-sync python -m tools.setup build
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
uv run --no-sync python -m tools.setup deploy --ghidra-path $ghidra
if ($LASTEXITCODE -ne 0) { throw "deploy failed ($LASTEXITCODE)" }

Write-Host 'DONE' -ForegroundColor Green
