$ErrorActionPreference='Stop'
$toolRoot='C:\Tools\Build'
$msbuild=Join-Path $toolRoot 'VS2022\MSBuild\Current\Bin\MSBuild.exe'
$targets=Join-Path $toolRoot 'VS2022\MSBuild\Microsoft\VC\v150\'
$sdk71=Join-Path $toolRoot 'WindowsSDK\v7.1A\'
$project=Join-Path $PSScriptRoot 'DeployOverlay\DeployOverlay.vcxproj'
& $msbuild $project /m /t:Rebuild '/p:Configuration=Release_XP;Platform=Win32' "/p:VCTargetsPath=$targets" "/p:WindowsSdkDir_71A=$sdk71" /v:minimal
exit $LASTEXITCODE

