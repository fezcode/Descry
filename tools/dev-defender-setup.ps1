<#
.SYNOPSIS
    Stop Windows Defender eating your own Descry builds. Run elevated.

.DESCRIPTION
    Defender's ML classifier quarantines fresh, unsigned descry.exe builds as
    Trojan:Win32/Bearfoos.A!ml -- a false positive, but one that deletes the
    binary out from under the build and takes the Start Menu / Desktop / taskbar
    shortcuts with it, since those point at a quarantined target.

    This script is a *local developer* convenience, not a fix and not something
    to ask users to run:

      1. Restores anything Defender already quarantined under that detection.
      2. Excludes the build output directories and the installed location.
      3. Excludes descry.exe as a running process.

    The real fixes live elsewhere: a code-signing certificate (tools\sign.ps1),
    and the VERSIONINFO / DEP / ASLR metadata now baked into the exe by
    CMakeLists.txt.

.PARAMETER InstallDir
    Where Descry is installed, if anywhere. Default: D:\Apps\Fezcode\Descry.

.PARAMETER SkipRestore
    Only add exclusions; do not touch quarantine.

.EXAMPLE
    # from an elevated PowerShell:
    .\tools\dev-defender-setup.ps1
#>

[CmdletBinding()]
param(
    [string]$InstallDir = "D:\Apps\Fezcode\Descry",
    [switch]$SkipRestore
)

$ErrorActionPreference = "Stop"

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script needs an elevated PowerShell (Defender settings are machine-wide)."
}

$root = Split-Path $PSScriptRoot -Parent

# --- 1. restore what Defender already took -------------------------------
if (-not $SkipRestore) {
    $mp = Join-Path $env:ProgramFiles "Windows Defender\MpCmdRun.exe"
    if (Test-Path $mp) {
        Write-Host "Restoring quarantined Descry builds ..." -ForegroundColor Cyan
        # -All restores every item under the named threat, which is what we
        # want: the exe plus the .lnk files that were taken as collateral.
        & $mp -Restore -Name "Trojan:Win32/Bearfoos.A!ml" -All 2>&1 | Write-Host
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  Nothing restored (already restored, or a different threat name)." -ForegroundColor DarkGray
        }
    } else {
        Write-Host "MpCmdRun.exe not found; skipping restore." -ForegroundColor DarkYellow
    }
}

# --- 2. path exclusions --------------------------------------------------
$paths = @(
    (Join-Path $root "build"),
    (Join-Path $root "dist")
)
if ($InstallDir -and (Test-Path $InstallDir)) { $paths += $InstallDir }

foreach ($p in $paths) {
    Write-Host "Excluding path: $p" -ForegroundColor Green
    Add-MpPreference -ExclusionPath $p
}

# --- 3. process exclusion ------------------------------------------------
Write-Host "Excluding process: descry.exe" -ForegroundColor Green
Add-MpPreference -ExclusionProcess "descry.exe"

Write-Host ""
Write-Host "Current exclusions:" -ForegroundColor Cyan
$prefs = Get-MpPreference
$prefs.ExclusionPath    | ForEach-Object { Write-Host "  path    $_" }
$prefs.ExclusionProcess | ForEach-Object { Write-Host "  process $_" }
