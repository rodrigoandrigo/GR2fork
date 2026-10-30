param([string]$PythonExecutable='C:/msys64/ucrt64/bin/python.exe', [string]$Glslang='C:/VulkanSDK/1.4.357.0/Bin/glslangValidator.exe')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root 'Build\DozenDiagnostics'
$payload=Join-Path $output 'Payload'
New-Item -ItemType Directory -Force $output,$payload | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$source=Join-Path $root 'tools\DozenDiagnostics\app.cpp'
$headers=Join-Path $root 'externals\vulkan-headers\include'
$cppwinrt=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include\10.0.26100.0\cppwinrt'
& $PythonExecutable (Join-Path $root 'tools\DozenDiagnostics\generate_shaders.py') (Join-Path $payload 'Shaders') $Glslang (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe')
if($LASTEXITCODE){throw 'Shader generation failed'}
& cl /nologo /std:c++20 /EHsc /MD /O2 /DUNICODE /D_UNICODE /DNOMINMAX /DDOZEN_DIAGNOSTICS_DESKTOP "/I$headers" $source "/Fo$output\desktop.obj" "/Fe$output\DozenDiagnostics-PC.exe" /link windowsapp.lib d3d12.lib dxgi.lib
if($LASTEXITCODE){throw 'Desktop companion compilation failed'}
& cl /nologo /std:c++20 /EHsc /MD /O2 /DUNICODE /D_UNICODE /DNOMINMAX /DWINAPI_FAMILY=WINAPI_FAMILY_APP /D_WIN32_WINNT=0x0A00 "/I$headers" "/I$cppwinrt" $source "/Fo$output\uwp.obj" "/Fe$payload\DozenDiagnostics.exe" /link /APPCONTAINER /SUBSYSTEM:WINDOWS "/LIBPATH:$env:VCToolsInstallDir\lib\x64\store" windowsapp.lib d3d12.lib dxgi.lib
if($LASTEXITCODE){throw 'UWP compilation failed'}
foreach($name in @('vulkan_dzn.dll','spirv_to_dxil.dll','dxcompiler.dll','dxil.dll')){
 Copy-Item -LiteralPath (Join-Path $root "shadPS4-UWP\Runtime\Release\$name") -Destination $payload
}
Copy-Item -LiteralPath (Join-Path $root 'tools\DozenDiagnostics\AppxManifest.xml') -Destination $payload
New-Item -ItemType Directory -Force (Join-Path $payload 'Assets') | Out-Null
foreach($name in @('StoreLogo.png','Square150x150Logo.png','Square44x44Logo.png','SplashScreen.png')){
 Copy-Item -LiteralPath (Join-Path $root ("shadPS4-UWP\Assets\" + $name.Replace('.png','.scale-100.png'))) -Destination (Join-Path $payload "Assets\$name")
}
$kit=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64'
$package=Join-Path $output 'DozenDiagnostics_1.0.18.0_x64.msix'
& (Join-Path $kit 'makeappx.exe') pack /d $payload /p $package /o
if($LASTEXITCODE){throw 'Packaging failed'}
& (Join-Path $PSScriptRoot 'sign-uwp.ps1') -Package $package
Write-Output "DIAGNOSTICS_PACKAGE=$package"
