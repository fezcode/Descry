$ErrorActionPreference = "Stop"

$version  = "0.71.0"
$builder  = "D:\Workhammer\DeployPaladin\release\builder\DeployPaladin.Builder.exe"
$base     = "D:\Workhammer\DeployPaladin\release\installer\DeployPaladin.exe"
$payload  = "D:\Workhammer\Downsee"
$output   = "D:\Workhammer\Downsee\Downsee_Installer_$version.exe"

& $builder --payload $payload --base $base --output $output
if ($LASTEXITCODE -ne 0) { throw "Builder failed with exit code $LASTEXITCODE" }

Write-Host "Built: $output"
