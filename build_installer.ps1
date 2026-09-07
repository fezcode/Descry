<#
.SYNOPSIS
    Package Descry into dist\Descry-Setup-<version>.exe with Forge.

.PARAMETER Sign
    Authenticode-sign build\descry.exe before packaging, and the finished
    installer afterwards, via tools\sign.ps1. Needs DESCRY_SIGN_THUMBPRINT or
    DESCRY_SIGN_PFX in the environment (see tools\make-dev-cert.ps1).
#>

[CmdletBinding()]
param(
    [switch]$Sign
)

$ErrorActionPreference = "Stop"

$version = "0.87.0"
$forge   = "D:\Workhammer\Forge\build\forge.exe"
$project = $PSScriptRoot
$outDir  = Join-Path $PSScriptRoot "dist"
$output  = Join-Path $outDir "Descry-Setup-$version.exe"
$signer  = Join-Path $PSScriptRoot "tools\sign.ps1"

Write-Host "Building forge.exe (windowsgui) from D:\Workhammer\Forge..."
Push-Location "D:\Workhammer\Forge"
try {
    # -H windowsgui: no console window when the installer runs.
    # Deliberately NOT stripped (-s -w): a stripped, symbol-free Go binary that
    # unpacks an exe plus a pile of DLLs is the exact shape Defender's ML
    # classifier scores as a dropper. Keeping the symbol and DWARF tables costs
    # a few MB and makes the installer look like what it is.
    & go build -tags "desktop,production" -ldflags "-H windowsgui" -o build\forge.exe ./cmd/forge
    if ($LASTEXITCODE -ne 0) { throw "go build forge.exe failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}

# Sign the payload before it is packaged: signing the installer afterwards
# would not reach the exe inside it.
if ($Sign) {
    & $signer -Required (Join-Path $project "build\descry.exe")
    if ($LASTEXITCODE -ne 0) { throw "signing descry.exe failed ($LASTEXITCODE)" }
}

Push-Location $project
try {
    # forge.exe is a GUI-subsystem binary, so PowerShell's call operator (&) does
    # not wait for it and $LASTEXITCODE is unreliable. Use Start-Process -Wait.
    $p = Start-Process -FilePath $forge -ArgumentList @("build", "--out", $outDir) -Wait -PassThru -NoNewWindow
    if ($p.ExitCode -ne 0) { throw "forge build failed with exit code $($p.ExitCode)" }
} finally {
    Pop-Location
}

if ($Sign) {
    & $signer -Required $output
    if ($LASTEXITCODE -ne 0) { throw "signing the installer failed ($LASTEXITCODE)" }
}

Write-Host "Built: $output"
