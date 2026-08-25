$ErrorActionPreference='Stop'
$root=(Resolve-Path $PSScriptRoot).Path
if($root -ne 'D:\Projects\DeployTool'){throw "Rollback root mismatch: $root"}
Copy-Item (Join-Path $root 'artifacts\small-layout-original\DeployOverlay.cpp.original') (Join-Path $root 'DeployOverlay\DeployOverlay.cpp') -Force
Get-FileHash -Algorithm SHA256 (Join-Path $root 'DeployOverlay\DeployOverlay.cpp')
