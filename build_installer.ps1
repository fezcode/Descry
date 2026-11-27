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

$version = "0.94.0"
$forge   = "D:\Workhammer\Forge\build\forge.exe"
$project = $PSScriptRoot
$outDir  = Join-Path $PSScriptRoot "dist"
$output  = Join-Path $outDir "Descry-Setup-$version.exe"
$signer  = Join-Path $PSScriptRoot "tools\sign.ps1"

Write-Host "Checking prebuilt Forge 2..."
$forgeRoot = Split-Path (Split-Path $forge -Parent) -Parent
foreach ($relative in @('build/forge.exe', 'build/stub.exe', 'build/wizard/Forge.Wizard.exe', 'build/wizard/libSkiaSharp.dll', 'build/wizard/libHarfBuzzSharp.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $forgeRoot $relative))) { throw "Missing $relative. Run gobake build in $forgeRoot first." }
}
$forgeVersion = & $forge --version
if ($LASTEXITCODE -ne 0 -or $forgeVersion -notmatch '^forge v2\.') { throw 'Forge 2 is required.' }

# forge.toml names every file it ships. A DLL the build bundled next to the exe
# but forge.toml does not list would be left out of the installer, and the
# installed app would fail to start on a machine without MSYS2.
$listed = Select-String -Path (Join-Path $project 'forge.toml') -Pattern 'src\s*=\s*"build/([^"]+\.dll)"' |
    ForEach-Object { $_.Matches[0].Groups[1].Value }
$built  = Get-ChildItem (Join-Path $project 'build\*.dll') | ForEach-Object Name
$unlisted = @($built  | Where-Object { $listed -notcontains $_ })
$absent   = @($listed | Where-Object { $built  -notcontains $_ })
if ($unlisted.Count) { throw "forge.toml does not ship these runtime DLLs: $($unlisted -join ', ')" }
if ($absent.Count)   { throw "forge.toml lists DLLs the build did not produce: $($absent -join ', ')" }

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
