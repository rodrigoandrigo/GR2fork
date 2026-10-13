param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$SDL3Source = 'C:\Users\rodri\Dev1\Projetos\SDL3-uwp',
    [string]$D3D12Source = '',
    [int]$Jobs = 6,
    [string]$PythonExecutable = '',
    [switch]$CoreOnly,
    [switch]$SkipSigning,
    [string]$CertificatePath = ''
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
if (!$D3D12Source) { $D3D12Source = Join-Path $repository 'externals\d3d12-native' }
$buildDirectory = Join-Path $repository "Build\UWP-$Configuration"
$runtimeDirectory = Join-Path $repository "shadPS4-UWP\Runtime\$Configuration"
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$visualStudio) { throw 'Visual Studio C++ tools were not found.' }
Import-Module (Join-Path $visualStudio 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $visualStudio -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$msbuild = Join-Path $visualStudio 'MSBuild\Current\Bin\MSBuild.exe'
$ninja = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$clang = Join-Path $visualStudio 'VC\Tools\Llvm\x64\bin\clang-cl.exe'
if (!(Test-Path $clang)) { $clang = Join-Path $env:ProgramFiles 'LLVM\bin\clang-cl.exe' }
if (!(Test-Path $clang)) { throw 'clang-cl was not found.' }
if (!$PythonExecutable) {
    $pythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($pythonCommand -and $pythonCommand.Source -notlike '*WindowsApps*') {
        $PythonExecutable = $pythonCommand.Source
    } elseif (Test-Path 'C:\msys64\ucrt64\bin\python.exe') {
        $PythonExecutable = 'C:\msys64\ucrt64\bin\python.exe'
    } else {
        throw 'Pass -PythonExecutable with the path to a working Python 3 interpreter.'
    }
}
New-Item -ItemType Directory -Force $buildDirectory, $runtimeDirectory | Out-Null
$sdlOutput = Join-Path $buildDirectory 'SDL3\'
$sdlIntermediate = Join-Path $buildDirectory 'SDL3-obj\'
& $msbuild (Join-Path $SDL3Source 'VisualC-WinRT\SDL-UWP.vcxproj') /m /t:Build "/p:Configuration=$Configuration" /p:Platform=x64 "/p:OutDir=$sdlOutput" "/p:IntDir=$sdlIntermediate" "/p:SolutionDir=$SDL3Source\VisualC-WinRT\" /p:WindowsTargetPlatformVersion=10.0.26100.0 /v:minimal /nologo
if ($LASTEXITCODE) { throw "SDL3 UWP build failed: $LASTEXITCODE" }
$sdlOutput = Join-Path $sdlOutput 'SDL3-UWP'
& (Join-Path $PSScriptRoot 'build-dozen.ps1') -PythonExecutable $PythonExecutable -Jobs $Jobs
$dozenRuntime = Join-Path $repository 'Build\Dozen-UWP\src\microsoft\vulkan\vulkan_dzn.dll'
Copy-Item -LiteralPath $dozenRuntime -Destination $runtimeDirectory
& cmake -S $repository -B $buildDirectory -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_C_COMPILER=$clang" "-DCMAKE_CXX_COMPILER=$clang" "-DCMAKE_BUILD_TYPE=$Configuration" "-DPython3_EXECUTABLE=$PythonExecutable" -DSHADPS4_UWP=ON "-DSHADPS4_D3D12_SOURCE_DIR=$D3D12Source" "-DSHADPS4_SDL3_SOURCE_DIR=$SDL3Source" "-DSHADPS4_SDL3_LIBRARY=$sdlOutput\SDL3.lib" "-DSHADPS4_SDL3_RUNTIME=$sdlOutput\SDL3.dll" "-DSHADPS4_UWP_OUTPUT_DIR=$runtimeDirectory" -DENABLE_TESTS=OFF
if ($LASTEXITCODE) { throw "Core configuration failed: $LASTEXITCODE" }
& cmake --build $buildDirectory --target shadps4 --parallel $Jobs
if ($LASTEXITCODE) { throw "Core build failed: $LASTEXITCODE" }
if (!$CoreOnly) {
    & $msbuild (Join-Path $repository 'shadPS4-UWP\shadPS4-UWP.vcxproj') /m /t:Build "/p:Configuration=$Configuration" /p:Platform=x64 /p:BuildCore=false "/p:D3D12Source=$D3D12Source" /v:minimal /nologo
    if ($LASTEXITCODE) { throw "UWP application build failed: $LASTEXITCODE" }
    if (!$SkipSigning) {
        $bundle = Get-ChildItem (Join-Path $repository 'shadPS4-UWP\AppPackages') -Filter '*.msixbundle' -Recurse |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (!$bundle) { throw 'The UWP package bundle was not generated.' }
        & (Join-Path $PSScriptRoot 'sign-uwp.ps1') -Package $bundle.FullName -CertificatePath $CertificatePath
    }
}
