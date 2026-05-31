<#
.SYNOPSIS
    Download the Velopack C/C++ native library (velopack_libc) and lay it out
    under third_party/velopack so CMake picks it up.

.DESCRIPTION
    Downloads the velopack/velopack release archive, then copies the Windows
    files for the chosen architecture into a clean, deterministic layout:

        third_party/velopack/include/   Velopack.h, Velopack.hpp
        third_party/velopack/lib/       velopack_libc_win_<arch>_msvc.dll.lib  (import lib)
        third_party/velopack/bin/       velopack_libc_win_<arch>_msvc.dll       (runtime)

    The dynamic (DLL) build is used on purpose: it links against a small import
    lib and needs no extra Windows system libraries, unlike the static .lib.

    Re-run any time to update. After it succeeds, re-configure CMake and the
    updater compiles in automatically.

.PARAMETER Version
    Velopack version to fetch (e.g. "1.1.1"). Defaults to the latest release.

.PARAMETER Arch
    Target architecture: x64 (default), x86, or arm64.
#>
[CmdletBinding()]
param(
    [string]$Version = "latest",
    [ValidateSet("x64", "x86", "arm64")]
    [string]$Arch = "x64"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$ghHeaders = @{ "User-Agent" = "sundial-setup" }

$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party\velopack"

# Resolve version -> asset URL.
if ($Version -eq "latest") {
    Write-Host "Querying latest velopack/velopack release..."
    $rel = Invoke-RestMethod "https://api.github.com/repos/velopack/velopack/releases/latest" -Headers $ghHeaders
    $Version = $rel.tag_name
}
Write-Host "Velopack version: $Version"
$assetUrl = "https://github.com/velopack/velopack/releases/download/$Version/velopack_libc_$Version.zip"

$tmp = Join-Path $env:TEMP ("velopack_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
$zip = Join-Path $tmp "velopack_libc.zip"

Write-Host "Downloading $assetUrl ..."
Invoke-WebRequest -Uri $assetUrl -OutFile $zip -Headers $ghHeaders

Write-Host "Extracting..."
$extract = Join-Path $tmp "x"
Expand-Archive -Path $zip -DestinationPath $extract -Force

# Reset destination.
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
$incDir = Join-Path $dest "include"
$libDir = Join-Path $dest "lib"
$binDir = Join-Path $dest "bin"
New-Item -ItemType Directory -Path $incDir, $libDir, $binDir -Force | Out-Null

# Headers (shared across platforms).
Copy-Item (Join-Path $extract "include\*") -Destination $incDir -Recurse -Force

# Windows dynamic lib + DLL for the chosen arch.
$stem = "velopack_libc_win_${Arch}_msvc"
$importLib = Join-Path $extract "lib\$stem.dll.lib"
$dll       = Join-Path $extract "lib\$stem.dll"

if (-not (Test-Path $importLib) -or -not (Test-Path $dll)) {
    Write-Host "Files present in archive lib/:" -ForegroundColor Yellow
    Get-ChildItem (Join-Path $extract "lib") -File | ForEach-Object { Write-Host "  - $($_.Name)" }
    throw "Could not find $stem.dll(.lib) in the archive for arch '$Arch'."
}

Copy-Item $importLib -Destination $libDir -Force
# Velopack's import lib references the unsuffixed "velopack_libc.dll", so the
# runtime DLL must be deployed under that exact name (the platform-suffixed
# file name in the archive is NOT what the linked exe looks for at load time).
Copy-Item $dll -Destination (Join-Path $binDir "velopack_libc.dll") -Force

Remove-Item -Recurse -Force $tmp

Write-Host ""
Write-Host "Velopack $Version ($Arch) vendored into third_party/velopack" -ForegroundColor Green
Write-Host "  include: $((Get-ChildItem $incDir -File).Count) file(s)"
Write-Host "  lib:     $stem.dll.lib"
Write-Host "  bin:     velopack_libc.dll (from $stem.dll)"
Write-Host ""
Write-Host "Next: re-run CMake configure; the updater compiles in automatically."
