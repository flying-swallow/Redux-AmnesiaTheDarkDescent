# Windows build wrapper for Amnesia64.
# Run from a Developer PowerShell for VS 2022 (or any shell where cmake/msbuild are on PATH).

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

if (-not (Test-Path 'extern/SDL/CMakeLists.txt')) {
    Write-Host "==> Initialising git submodules"
    git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw "submodule init failed" }
}

$cfgName  = if ($Config -eq 'release') { 'Release' } else { 'Debug' }
$buildDir = Join-Path $root 'build'

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "==> Cleaning $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

if (-not $GameDir -and $env:ATDD_DIR) {
    $GameDir = $env:ATDD_DIR
} elseif (-not $GameDir -and $env:AMNESIA_GAME_DIRECTORY) {
    $GameDir = $env:AMNESIA_GAME_DIRECTORY
}

$cmakeArgs = @('-S', $root, '-B', $buildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64')
if ($GameDir) {
    $cmakeArgs += "-DAMNESIA_GAME_DIRECTORY=$GameDir"
}
if ($ExtraArgs) {
    $cmakeArgs += $ExtraArgs
}

Write-Host "==> Configuring ($cfgName)"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& cmake --build $buildDir --config $cfgName
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

if (-not $NoDeploy) {
    & cmake --build $buildDir --config $cfgName --target deploy
    if ($LASTEXITCODE -ne 0) { throw "deploy failed" }
}

Write-Host "==> Build complete: build\bin\"
