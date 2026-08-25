$ErrorActionPreference='Stop'
$root=(Resolve-Path $PSScriptRoot).Path
if($root -ne 'D:\Projects\DeployTool'){throw "Apply root mismatch: $root"}
Copy-Item (Join-Path $root 'artifacts\small-layout-modified\DeployOverlay.cpp') (Join-Path $root 'DeployOverlay\DeployOverlay.cpp') -Force
Get-FileHash -Algorithm SHA256 (Join-Path $root 'DeployOverlay\DeployOverlay.cpp')
