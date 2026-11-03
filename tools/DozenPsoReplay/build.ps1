$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products '*' -property installationPath
Import-Module "$vs/Common7/Tools/Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64 -vcvars_ver=14.44' | Out-Null
$out = Join-Path $repo 'Build/DozenPsoReplay'
New-Item -ItemType Directory -Force $out | Out-Null
$headers = Join-Path $repo 'externals/mesa-uwp/subprojects/DirectX-Headers-1.0/include'
$capture = Join-Path $repo 'externals/mesa-uwp/src/microsoft/vulkan'
& cl /nologo /std:c++17 /EHsc /MD /O2 "/I$headers" "/I$capture" (Join-Path $PSScriptRoot 'main.cpp') "/Fe:$out/DozenPsoReplay.exe" "/Fo:$out/main.obj" /link d3d12.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw 'Replay build failed' }
