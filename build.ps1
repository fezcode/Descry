<#
.SYNOPSIS
    Configure and build Descry (descry.exe) with CMake + Ninja using the
    MSYS2 MinGW-w64 toolchain.

.DESCRIPTION
    Wraps the documented build:

        cmake -G Ninja -B build
        ninja -C build

    The MSYS2 mingw64 bin directory is auto-detected and prepended to PATH so
    cmake finds gcc/ninja/pkg-config and the matching SDL2 / FreeType /
    HarfBuzz / libpng / libjpeg, even from a vanilla PowerShell session. The
    POST_BUILD step in CMakeLists.txt bundles the runtime DLLs next to the exe,
    leaving build\ self-contained for build_installer.ps1 to package.

.PARAMETER Config
    CMAKE_BUILD_TYPE. Default: Release.

.PARAMETER Clean
    Delete the build directory before configuring (forces a fresh reconfigure).

.PARAMETER Run
    Launch build\descry.exe after a successful build.

.PARAMETER Installer
    After building, also build the installer by invoking build_installer.ps1.

.PARAMETER Tests
    Configure with -DDESCRY_TESTS=ON (reconfiguring if the cache disagrees),
    build the unit-test executables and run them with ctest.

.PARAMETER MingwBin
    Path to the MSYS2 mingw64 bin directory. Auto-detected when omitted.

.EXAMPLE
    .\build.ps1
    Release build into .\build.

.EXAMPLE
    .\build.ps1 -Clean -Config Debug -Run
    Fresh Debug build, then launch the app.

.EXAMPLE
    .\build.ps1 -Installer
    Build the app, then produce dist\Descry-Setup-<version>.exe.
#>

[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$Run,
    [switch]$Installer,
    [switch]$Tests,
    [string]$MingwBin
)

$ErrorActionPreference = "Stop"

$root     = $PSScriptRoot
$buildDir = Join-Path $root "build"

# --- locate the MSYS2 MinGW-w64 toolchain --------------------------------
if (-not $MingwBin) {
    # Prefer a gcc already on PATH; otherwise probe the usual install spots.
    $gcc = Get-Command gcc -ErrorAction SilentlyContinue
    if ($gcc) {
        $MingwBin = Split-Path $gcc.Source
    } else {
        $MingwBin = @(
            "D:\Apps\msys64\mingw64\bin",
            "C:\msys64\mingw64\bin",
            "C:\tools\msys64\mingw64\bin"
        ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    }
}

if (-not $MingwBin -or -not (Test-Path $MingwBin)) {
    throw "MSYS2 MinGW-w64 toolchain not found. Pass -MingwBin <path to mingw64\bin>."
}

# Put the toolchain first on PATH so cmake/pkg-config resolve to mingw64.
$env:PATH = "$MingwBin;$env:PATH"

foreach ($tool in @("cmake", "ninja", "gcc", "pkg-config")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required tool '$tool' not found under $MingwBin."
    }
}

Write-Host "Toolchain : $MingwBin" -ForegroundColor Cyan
Write-Host "Config    : $Config"   -ForegroundColor Cyan
Write-Host "Build dir : $buildDir" -ForegroundColor Cyan

# --- clean ---------------------------------------------------------------
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning $buildDir ..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

# --- configure -----------------------------------------------------------
# Re-run configure only when there is no cache (fresh or cleaned tree). Ninja
# is single-config, so changing -Config on an existing tree needs -Clean.
# The one exception is -Tests: if the cache's DESCRY_TESTS value disagrees
# with the switch, reconfigure so the test targets appear (or disappear).
$cache = Join-Path $buildDir "CMakeCache.txt"
$testsWanted = if ($Tests) { "ON" } else { "OFF" }
$needConfigure = -not (Test-Path $cache)
if (-not $needConfigure) {
    $cachedTests = Select-String -Path $cache -Pattern '^DESCRY_TESTS:BOOL=(\w+)' |
        ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -First 1
    if (-not $cachedTests) { $cachedTests = "OFF" }
    if ($cachedTests -ne $testsWanted) {
        Write-Host "DESCRY_TESTS changed ($cachedTests -> $testsWanted); reconfiguring." -ForegroundColor Yellow
        $needConfigure = $true
    }
}
if ($needConfigure) {
    Write-Host "Configuring (cmake -G Ninja) ..." -ForegroundColor Green
    & cmake -G Ninja -B $buildDir -S $root "-DCMAKE_BUILD_TYPE=$Config" "-DDESCRY_TESTS=$testsWanted"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
} else {
    Write-Host "Reusing CMake cache (pass -Clean to reconfigure)." -ForegroundColor DarkGray
}

# --- build ---------------------------------------------------------------
Write-Host "Building (ninja) ..." -ForegroundColor Green
& cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$exe = Join-Path $buildDir "descry.exe"
if (-not (Test-Path $exe)) { throw "build reported success but $exe is missing." }
Write-Host "Built: $exe" -ForegroundColor Green

# --- optional: unit tests ------------------------------------------------
if ($Tests) {
    Write-Host "Running unit tests (ctest) ..." -ForegroundColor Green
    & ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "unit tests failed ($LASTEXITCODE)" }
}

# --- optional: installer -------------------------------------------------
if ($Installer) {
    Write-Host "Building installer ..." -ForegroundColor Green
    & (Join-Path $root "build_installer.ps1")
    if ($LASTEXITCODE -ne 0) { throw "installer build failed ($LASTEXITCODE)" }
}

# --- optional: run -------------------------------------------------------
if ($Run) {
    Write-Host "Launching descry.exe ..." -ForegroundColor Green
    & $exe
}
