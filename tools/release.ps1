<#
.SYNOPSIS
    Package (and optionally publish) a Sundial release with Velopack's vpk tool.

.DESCRIPTION
    Wraps `vpk pack` (and `vpk upload github`). The first run produces a
    Setup.exe that users install once; every later, higher-versioned release is
    what running copies auto-update to.

    Requires the vpk CLI:  dotnet tool install -g vpk

.PARAMETER Version
    The release version, e.g. "1.2.0". Required.

.PARAMETER BuildDir
    Folder containing the built sundial.exe (and the velopack DLL). Defaults to
    build\Release.

.PARAMETER RepoUrl
    GitHub repo URL to publish to (e.g. https://github.com/<owner>/sundial).
    If omitted, packs locally without uploading.

.PARAMETER Token
    GitHub personal access token with repo scope (needed only with -RepoUrl).
    Falls back to the GITHUB_TOKEN environment variable.

.EXAMPLE
    pwsh tools/release.ps1 -Version 1.2.0

.EXAMPLE
    pwsh tools/release.ps1 -Version 1.2.0 -RepoUrl https://github.com/me/sundial -Token $env:GH_PAT
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$BuildDir,
    [string]$RepoUrl,
    [string]$Token = $env:GITHUB_TOKEN
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $root "build\Release" }
$releaseDir = Join-Path $root "releases"

if (-not (Get-Command vpk -ErrorAction SilentlyContinue)) {
    throw "vpk not found. Install it with:  dotnet tool install -g vpk"
}

# Build with the release version baked in, so the About box matches the package.
$cmakeDir = Join-Path $root "build"
Write-Host "Configuring + building $Version ..."
# libultrahdr (Ultra HDR export) vendors libjpeg-turbo 3.0.1, whose
# cmake_minimum_required predates the floor enforced by CMake 4.x. Let that
# nested ExternalProject configure accept the older minimum.
$env:CMAKE_POLICY_VERSION_MINIMUM = "3.5"
# The first configure on a clean tree fetches libultrahdr; its libjpeg-turbo
# dependency isn't detected until the source tree has settled, so the very first
# configure can fail. A second pass (sources now present) succeeds - retry once.
# Quote the whole -D token: Windows PowerShell mangles a bare
# -DSUNDIAL_VERSION=$Version (it drops the dotted parts and doesn't even expand
# $Version inside the bareword), which is what baked a literal "$Version" into
# the About box. Quoting forces normal string interpolation and passes the
# token to cmake intact.
cmake -S $root -B $cmakeDir "-DSUNDIAL_VERSION=$Version"
if ($LASTEXITCODE -ne 0) {
    Write-Host "First configure failed (libultrahdr dependency bootstrap); retrying once ..."
    cmake -S $root -B $cmakeDir "-DSUNDIAL_VERSION=$Version"
}
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)." }
cmake --build $cmakeDir --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)." }

$exe = Join-Path $BuildDir "sundial.exe"
if (-not (Test-Path $exe)) {
    throw "sundial.exe not found in $BuildDir. Build the Release config first."
}

Write-Host "Packing Sundial $Version from $BuildDir ..."
# --shortcuts adds a run-on-startup shortcut (Startup folder) alongside the
# Start menu entry, so Sundial launches at login. vpk can't pass arguments to
# the shortcuts it creates, so the Startup one is argless; Sundial rewrites it
# on launch to add --startup (EnsureStartupShortcutArgs) so login launches stay
# in the tray instead of popping the toolbar.
vpk pack `
    --packId Sundial `
    --packTitle "Sundial" `
    --packVersion $Version `
    --packDir $BuildDir `
    --mainExe sundial.exe `
    --shortcuts StartMenuRoot,Startup `
    --outputDir $releaseDir
if ($LASTEXITCODE -ne 0) { throw "vpk pack failed ($LASTEXITCODE)." }

if ($RepoUrl) {
    if (-not $Token) { throw "-RepoUrl given but no token (pass -Token or set GITHUB_TOKEN)." }
    Write-Host "Uploading to $RepoUrl ..."
    vpk upload github `
        --repoUrl $RepoUrl `
        --token $Token `
        --outputDir $releaseDir `
        --publish `
        --releaseName "Sundial $Version" `
        --tag "v$Version"
    if ($LASTEXITCODE -ne 0) { throw "vpk upload failed ($LASTEXITCODE)." }
    Write-Host "Published Sundial $Version." -ForegroundColor Green
} else {
    Write-Host "Packed to $releaseDir (no -RepoUrl, so nothing was uploaded)." -ForegroundColor Green
    Write-Host "Ship releases\Sundial-win-Setup.exe for first-time installs."
}
