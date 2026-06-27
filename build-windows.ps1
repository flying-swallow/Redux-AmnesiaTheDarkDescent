# Windows build wrapper for Amnesia64 (premake5 + MSBuild).
# Requires premake5.exe on PATH and a VS 2022 install (MSBuild is auto-located
# via vswhere, so any PowerShell works -- not just a Developer PowerShell).

[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')]
    [string]$Config = 'release',
    [switch]$Clean,
    [switch]$NoDeploy,
    [string]$GameDir,
    [Parameter(ValueFromRemainingArguments=$true)]
    [string[]]$ExtraArgs
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

if (-not (Test-Path 'HPL2/extern/SDL/CMakeLists.txt')) {
    Write-Host "==> Initialising git submodules"
    git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw "submodule init failed" }
}

$cfgName  = if ($Config -eq 'release') { 'Release' } else { 'Debug' }
$buildDir = Join-Path $root 'build-premake'

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "==> Cleaning $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

if (-not $GameDir -and $env:ATDD_DIR) {
    $GameDir = $env:ATDD_DIR
} elseif (-not $GameDir -and $env:AMNESIA_GAME_DIRECTORY) {
    $GameDir = $env:AMNESIA_GAME_DIRECTORY
}

# Extra args go to `premake5 vs2022` (e.g. --with-tools=no, --slangc=...),
# mirroring how build-linux-docker.sh forwards `--`-args to `premake5 gmake2`.
$premakeArgs = @('vs2022')
if ($ExtraArgs) {
    $premakeArgs += $ExtraArgs
}

Write-Host "==> Generating Visual Studio solution"
& premake5 @premakeArgs
if ($LASTEXITCODE -ne 0) { throw "premake5 vs2022 failed" }

$sln = Join-Path $buildDir 'Amnesia.sln'

# Prefer msbuild on PATH (Developer PowerShell); otherwise locate it via vswhere
# so the script also works from a regular PowerShell.
$msbuild = (Get-Command msbuild -ErrorAction SilentlyContinue).Source
if (-not $msbuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "msbuild not on PATH and vswhere not found at $vswhere -- install VS 2022 (or its Build Tools)."
    }
    $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild `
        -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw "vswhere did not return an MSBuild.exe path" }
}

Write-Host "==> Building ($cfgName) with $msbuild"
& $msbuild $sln "/p:Configuration=$cfgName" '/p:Platform=x64' '/m' '/v:m'
if ($LASTEXITCODE -ne 0) { throw "msbuild failed" }

if (-not $NoDeploy) {
    if (-not $GameDir) {
        Write-Host "==> No game dir set; skipping deploy. Pass -GameDir or set ATDD_DIR / AMNESIA_GAME_DIRECTORY."
    } else {
        Write-Host "==> Deploying assets from $GameDir"
        & premake5 deploy "--game-dir=$GameDir"
        if ($LASTEXITCODE -ne 0) { throw "deploy failed" }
    }
}

Write-Host "==> Build complete: build-premake\amnesia\$cfgName\"
