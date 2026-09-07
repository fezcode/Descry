<#
.SYNOPSIS
    Authenticode-sign one or more build artifacts, if a certificate is configured.

.DESCRIPTION
    Descry ships unsigned by default, and an unsigned, never-before-seen exe is
    the single biggest reason Windows Defender's ML classifier quarantines it
    (Trojan:Win32/Bearfoos.A!ml).  A valid signature is the durable fix, so
    build.ps1 and build_installer.ps1 both route their outputs through here.

    The signing identity comes from the environment, so no secret ever lands in
    the repo.  Set ONE of:

        $env:DESCRY_SIGN_THUMBPRINT = "AB12...."      # cert already in a store
        $env:DESCRY_SIGN_PFX        = "C:\path\cert.pfx"
        $env:DESCRY_SIGN_PFX_PASSWORD = "..."         # optional, with -PFX

    Optionally override the RFC-3161 timestamp authority:

        $env:DESCRY_SIGN_TIMESTAMP_URL = "http://timestamp.sectigo.com"

    With nothing configured this script prints a note and exits 0, so an
    unconfigured machine still builds.  Pass -Required to make that an error
    instead (use it in release builds once you have a certificate).

.PARAMETER Path
    One or more files to sign.  Missing files are an error.

.PARAMETER Required
    Fail when no signing identity is configured, instead of skipping.

.EXAMPLE
    .\tools\sign.ps1 build\descry.exe
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Path,
    [switch]$Required
)

$ErrorActionPreference = "Stop"

$thumbprint = $env:DESCRY_SIGN_THUMBPRINT
$pfx        = $env:DESCRY_SIGN_PFX
$timestamp  = if ($env:DESCRY_SIGN_TIMESTAMP_URL) { $env:DESCRY_SIGN_TIMESTAMP_URL }
              else { "http://timestamp.digicert.com" }

if (-not $thumbprint -and -not $pfx) {
    $msg = "No signing identity configured (set DESCRY_SIGN_THUMBPRINT or DESCRY_SIGN_PFX)."
    if ($Required) { throw $msg }
    Write-Host "Skipping code signing: $msg" -ForegroundColor DarkYellow
    Write-Host "  Unsigned builds are what Defender flags. See tools\make-dev-cert.ps1." -ForegroundColor DarkGray
    exit 0
}

# --- locate signtool.exe -------------------------------------------------
# Prefer the newest Windows SDK build; fall back to whatever is on PATH.
$signtool = $null
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
if (Test-Path $kits) {
    $signtool = Get-ChildItem -Path $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\x64\' } |
        Sort-Object { $_.Directory.Parent.Name } -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $signtool) {
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { $signtool = $cmd.Source }
}
if (-not $signtool) {
    throw "signtool.exe not found. Install the Windows SDK (Signing Tools) or put it on PATH."
}

# --- sign ----------------------------------------------------------------
foreach ($file in $Path) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Cannot sign missing file: $file" }
    $full = (Resolve-Path -LiteralPath $file).Path

    # SHA-256 for both the file digest (/fd) and the timestamp digest (/td).
    # /tr is the RFC-3161 timestamp endpoint: without it the signature dies
    # with the certificate instead of outliving it.
    $args = @("sign", "/fd", "sha256", "/tr", $timestamp, "/td", "sha256", "/v")
    if ($thumbprint) {
        $args += @("/sha1", $thumbprint)
    } else {
        if (-not (Test-Path -LiteralPath $pfx)) { throw "DESCRY_SIGN_PFX does not exist: $pfx" }
        $args += @("/f", $pfx)
        if ($env:DESCRY_SIGN_PFX_PASSWORD) { $args += @("/p", $env:DESCRY_SIGN_PFX_PASSWORD) }
    }
    $args += $full

    Write-Host "Signing $full ..." -ForegroundColor Green
    & $signtool @args
    if ($LASTEXITCODE -ne 0) { throw "signtool failed on $full ($LASTEXITCODE)" }
}
