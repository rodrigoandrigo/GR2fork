param([string]$DestinationDirectory = '')
$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
if (!$DestinationDirectory) {
    $DestinationDirectory = Join-Path $repository 'Build\Dependencies\dxc-1.9.2609'
}
New-Item -ItemType Directory -Force $DestinationDirectory | Out-Null
$archive = Join-Path $DestinationDirectory 'dxc_2026_09_29.zip'
if (!(Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip' -OutFile $archive
}
$expectedHash = 'ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1'
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
    throw 'DXC 1.9.2609 archive SHA-256 verification failed.'
}
Expand-Archive -LiteralPath $archive -DestinationPath $DestinationDirectory -Force
$runtimeDirectory = Join-Path $DestinationDirectory 'bin\x64'
foreach ($name in @('dxcompiler.dll', 'dxil.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $runtimeDirectory $name))) {
        throw "DXC runtime is missing $name."
    }
}
Write-Output $runtimeDirectory
