<#
.SYNOPSIS
    Create a self-signed code-signing certificate for local Descry builds.

.DESCRIPTION
    A real OV/EV certificate is the only thing that fixes Windows Defender and
    SmartScreen for *other people's* machines.  This script is the stopgap for
    your own: it mints a self-signed code-signing certificate, trusts it on this
    machine, and prints the thumbprint to feed to tools\sign.ps1.  A locally
    trusted Authenticode signature is enough to stop Defender's ML classifier
    treating every fresh descry.exe as an anonymous unknown binary here.

    It changes nothing on any other computer.  Binaries signed with this cert
    are still unsigned-in-practice for your users, so do not ship them as if
    they were signed.

    What it touches:
      * Cert:\CurrentUser\My            - the certificate and its private key
      * Cert:\CurrentUser\Root          - so the chain validates for you
      * Cert:\CurrentUser\TrustedPublisher
      * Cert:\LocalMachine\Root + TrustedPublisher, only when run elevated,
        so services (Defender included) see the same trust.

.PARAMETER Subject
    Certificate subject. Default: CN=Fezcode (Descry dev).

.PARAMETER Years
    Validity in years. Default: 5.

.EXAMPLE
    .\tools\make-dev-cert.ps1
    # then, in the shell you build from:
    $env:DESCRY_SIGN_THUMBPRINT = "<printed thumbprint>"
    .\build.ps1 -Sign
#>

[CmdletBinding()]
param(
    [string]$Subject = "CN=Fezcode (Descry dev)",
    [int]$Years = 5
)

$ErrorActionPreference = "Stop"

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
$elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

Write-Host "Creating self-signed code-signing certificate: $Subject" -ForegroundColor Cyan
$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -KeyUsage DigitalSignature `
    -KeyAlgorithm RSA `
    -KeyLength 3072 `
    -HashAlgorithm SHA256 `
    -NotAfter (Get-Date).AddYears($Years)

Write-Host "Thumbprint: $($cert.Thumbprint)" -ForegroundColor Green

# --- trust it ------------------------------------------------------------
# Export the public half and import it into the trust stores. A self-signed
# cert is its own root, so it has to sit in Root for the chain to validate.
$tmp = Join-Path $env:TEMP "descry-dev-cert-$($cert.Thumbprint).cer"
Export-Certificate -Cert $cert -FilePath $tmp -Force | Out-Null
try {
    foreach ($store in @("Cert:\CurrentUser\Root", "Cert:\CurrentUser\TrustedPublisher")) {
        Import-Certificate -FilePath $tmp -CertStoreLocation $store | Out-Null
        Write-Host "Trusted in $store" -ForegroundColor DarkGray
    }
    if ($elevated) {
        foreach ($store in @("Cert:\LocalMachine\Root", "Cert:\LocalMachine\TrustedPublisher")) {
            Import-Certificate -FilePath $tmp -CertStoreLocation $store | Out-Null
            Write-Host "Trusted in $store" -ForegroundColor DarkGray
        }
    } else {
        Write-Host "Not elevated: skipped the LocalMachine stores." -ForegroundColor Yellow
        Write-Host "  Re-run this from an admin PowerShell so Defender sees the same trust." -ForegroundColor DarkGray
    }
} finally {
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Next, in the shell you build from:" -ForegroundColor Cyan
Write-Host "  `$env:DESCRY_SIGN_THUMBPRINT = `"$($cert.Thumbprint)`""
Write-Host "  .\build.ps1 -Sign"
Write-Host ""
Write-Host "To make it stick across sessions:" -ForegroundColor Cyan
Write-Host "  [Environment]::SetEnvironmentVariable('DESCRY_SIGN_THUMBPRINT','$($cert.Thumbprint)','User')"
