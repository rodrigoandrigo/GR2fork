param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$CertificatePath = '',
    [string]$SignTool = ''
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
if (!$CertificatePath) {
    $CertificatePath = Join-Path $repository 'shadPS4-UWP\shadPS4-UWP_TemporaryKey.pfx'
}
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$keyPath = (Resolve-Path -LiteralPath $CertificatePath).Path
if (!$SignTool) {
    $SignTool = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
}
$certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
    $keyPath, '', [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
if (!$certificate.HasPrivateKey -or $certificate.NotAfter -le (Get-Date)) {
    throw 'A valid certificate with its private key is required.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Security
$archive = [System.IO.Compression.ZipFile]::OpenRead($packagePath)
try {
    $entry = $archive.GetEntry('AppxMetadata/AppxBundleManifest.xml')
    if (!$entry) { $entry = $archive.GetEntry('AppxManifest.xml') }
    if (!$entry) { throw 'Package manifest not found.' }
    $reader = [System.IO.StreamReader]::new($entry.Open())
    try { [xml]$manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
    $publisher = $manifest.DocumentElement.Identity.Publisher
    if ($publisher -ne $certificate.Subject) {
        throw "Package publisher '$publisher' does not match certificate '$($certificate.Subject)'."
    }
} finally { $archive.Dispose() }
& $SignTool sign /fd SHA256 /f $keyPath $packagePath
if ($LASTEXITCODE) { throw "Package signing failed: $LASTEXITCODE" }
$archive = [System.IO.Compression.ZipFile]::OpenRead($packagePath)
try {
    $entry = $archive.GetEntry('AppxSignature.p7x')
    if (!$entry) { throw 'Package signature not found.' }
    $stream = $entry.Open()
    $buffer = [System.IO.MemoryStream]::new()
    try { $stream.CopyTo($buffer); $signature = $buffer.ToArray() }
    finally { $stream.Dispose(); $buffer.Dispose() }
    if ([System.Text.Encoding]::ASCII.GetString($signature, 0, 4) -ne 'PKCX') {
        throw 'Invalid package signature header.'
    }
    $cms = [System.Security.Cryptography.Pkcs.SignedCms]::new()
    $cms.Decode([byte[]]$signature[4..($signature.Length - 1)])
    $cms.CheckSignature($true)
    if ($cms.SignerInfos.Count -ne 1 -or
        $cms.SignerInfos[0].Certificate.Thumbprint -ne $certificate.Thumbprint) {
        throw 'Unexpected package signer.'
    }
} finally { $archive.Dispose() }
$publicCertificate = Join-Path (Split-Path $packagePath -Parent) 'shadPS4-UWP.cer'
[System.IO.File]::WriteAllBytes($publicCertificate,
    $certificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
Write-Output "SIGNED_PACKAGE=$packagePath SIGNER=$($certificate.Thumbprint) CMS_SIGNATURE=PASS"
