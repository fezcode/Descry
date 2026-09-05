$ErrorActionPreference = "Stop"

$version = "0.86.1"
$forge   = "D:\Workhammer\Forge\build\forge.exe"
$project = $PSScriptRoot
$outDir  = Join-Path $PSScriptRoot "dist"
$output  = Join-Path $outDir "Descry-Setup-$version.exe"

Write-Host "Building forge.exe (windowsgui) from D:\Workhammer\Forge..."
Push-Location "D:\Workhammer\Forge"
try {
    & go build -tags "desktop,production" -ldflags "-H windowsgui -s -w" -o build\forge.exe ./cmd/forge
    if ($LASTEXITCODE -ne 0) { throw "go build forge.exe failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
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

Write-Host "Built: $output"

