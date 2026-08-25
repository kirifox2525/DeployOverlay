param([ValidateSet('Win32','x64','All')][string]$Platform='All')
$ErrorActionPreference='Stop'
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath=& $vswhere -latest -products * -property installationPath
if (-not $vsPath) { throw 'Visual Studio installation not found.' }
$msbuild=Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
$project=Join-Path $PSScriptRoot 'DeployOverlay\DeployOverlay.vcxproj'
$targets=if($Platform -eq 'All'){@('Win32','x64')}else{@($Platform)}
foreach($target in $targets){
  & $msbuild $project /m /t:Rebuild "/p:Configuration=Release;Platform=$target" /v:minimal
  if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
}

