$ErrorActionPreference = "Stop"

$version = "0.74.0"
$forge   = "D:\Workhammer\Forge\build\forge.exe"
$project = "D:\Workhammer\Downsee"
$outDir  = "D:\Workhammer\Downsee\dist"
$output  = Join-Path $outDir "Downsee-Setup-$version.exe"

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
    & $forge build --out $outDir
    if ($LASTEXITCODE -ne 0) { throw "forge build failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

Write-Host "Built: $output"

