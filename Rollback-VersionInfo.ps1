$ErrorActionPreference='Stop'
$root=(Resolve-Path $PSScriptRoot).Path
if($root -ne 'D:\Projects\DeployTool'){throw "Rollback root mismatch: $root"}
$project=Join-Path $root 'DeployOverlay'
$snapshot=Join-Path $root 'artifacts\version-original'
Copy-Item (Join-Path $snapshot 'DeployOverlay.vcxproj.original') (Join-Path $project 'DeployOverlay.vcxproj') -Force
Copy-Item (Join-Path $snapshot 'DeployOverlay.vcxproj.filters.original') (Join-Path $project 'DeployOverlay.vcxproj.filters') -Force
Copy-Item (Join-Path $snapshot 'README.md.original') (Join-Path $root 'README.md') -Force
Remove-Item (Join-Path $project 'DeployOverlay.rc') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $project 'resource.h') -Force -ErrorAction SilentlyContinue
Get-FileHash -Algorithm SHA256 (Join-Path $project 'DeployOverlay.vcxproj'),(Join-Path $project 'DeployOverlay.vcxproj.filters'),(Join-Path $root 'README.md')
