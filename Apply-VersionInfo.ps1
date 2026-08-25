$ErrorActionPreference='Stop'
$root=(Resolve-Path $PSScriptRoot).Path
if($root -ne 'D:\Projects\DeployTool'){throw "Apply root mismatch: $root"}
$project=Join-Path $root 'DeployOverlay'
$snapshot=Join-Path $root 'artifacts\version-modified'
Copy-Item (Join-Path $snapshot 'DeployOverlay.vcxproj') (Join-Path $project 'DeployOverlay.vcxproj') -Force
Copy-Item (Join-Path $snapshot 'DeployOverlay.vcxproj.filters') (Join-Path $project 'DeployOverlay.vcxproj.filters') -Force
Copy-Item (Join-Path $snapshot 'DeployOverlay.rc') (Join-Path $project 'DeployOverlay.rc') -Force
Copy-Item (Join-Path $snapshot 'resource.h') (Join-Path $project 'resource.h') -Force
Copy-Item (Join-Path $snapshot 'README.md') (Join-Path $root 'README.md') -Force
Get-FileHash -Algorithm SHA256 (Join-Path $project 'DeployOverlay.vcxproj'),(Join-Path $project 'DeployOverlay.vcxproj.filters'),(Join-Path $project 'DeployOverlay.rc'),(Join-Path $project 'resource.h'),(Join-Path $root 'README.md')
