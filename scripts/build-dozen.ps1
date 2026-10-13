param([string]$PythonExecutable = 'C:\msys64\ucrt64\bin\python.exe', [int]$Jobs = 6)
$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
$sourceDirectory = Join-Path $repository 'externals\mesa-uwp'
$buildDirectory = Join-Path $repository 'Build\Dozen-UWP'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $visualStudio 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $visualStudio -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$framework = Get-AppxPackage -Name Microsoft.VCLibs.140.00 | Where-Object Architecture -eq X64 | Sort-Object Version -Descending | Select-Object -First 1
if (!$framework) { throw 'The x64 UWP Visual C++ runtime is required for compiler checks.' }
$env:PATH += ';C:\msys64\usr\bin;C:\msys64\ucrt64\bin;' + $framework.InstallLocation
$store = (Join-Path $env:VCToolsInstallDir 'lib\x64\store').Replace('\', '/')
$arguments = @('setup', $buildDirectory, $sourceDirectory,
    "--native-file=$(Join-Path $PSScriptRoot 'mesa-uwp-x64.ini')",
    '-Dbuildtype=release', '-Ddefault_library=static', '-Duwp=true',
    '-Dplatforms=windows', '-Dopengl=false', '-Dgallium-drivers=',
    '-Dvulkan-drivers=microsoft-experimental', '-Dllvm=disabled',
    '-Dglx=disabled', '-Degl=disabled', '-Dgles1=disabled', '-Dgles2=disabled',
    '-Dgbm=disabled', '-Dshared-glapi=disabled', '-Dmicrosoft-clc=disabled',
    '-Dbuild-tests=false', '-Dzstd=disabled', '-Dspirv-tools=disabled',
    '-Dforce_fallback_for=zlib', '-Db_pch=false',
    "-Dc_link_args=['/APPCONTAINER','WindowsApp.lib','/LIBPATH:$store']",
    "-Dcpp_link_args=['/APPCONTAINER','WindowsApp.lib','/LIBPATH:$store']")
if (Test-Path (Join-Path $buildDirectory 'meson-private\coredata.dat')) { $arguments += '--reconfigure' }
& $PythonExecutable -m mesonbuild.mesonmain @arguments
if ($LASTEXITCODE) { throw "Dozen configuration failed: $LASTEXITCODE" }
& $PythonExecutable -m mesonbuild.mesonmain compile -C $buildDirectory -j $Jobs
if ($LASTEXITCODE) { throw "Dozen build failed: $LASTEXITCODE" }
