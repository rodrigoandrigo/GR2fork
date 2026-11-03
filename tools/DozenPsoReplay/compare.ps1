param(
    [Parameter(Mandatory=$true)][string]$Archive,
    [ValidateRange(0,64)][int]$Adapter = 0,
    [ValidateRange(1,3600)][int]$TimeoutSeconds = 60,
    [switch]$DebugLayer
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$exe = Join-Path $repo 'Build/DozenPsoReplay/DozenPsoReplay.exe'
$archivePath = (Resolve-Path -LiteralPath $Archive).Path
$output = Join-Path $repo ('Build/DozenPsoReplay/run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Force $output | Out-Null
foreach ($api in @('stream','legacy')) {
    $arguments = '"' + $archivePath + '" ' + $api + ' ' + $Adapter
    if ($DebugLayer) { $arguments += ' debug' }
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$output/$api.log" -RedirectStandardError "$output/$api-error.log"
    if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $process.Id
        Write-Output "$api TIMEOUT after $TimeoutSeconds seconds"
    } else {
        $process.Refresh()
        Write-Output "$api EXIT=$($process.ExitCode)"
    }
    Get-Content "$output/$api.log"
    Get-Content "$output/$api-error.log"
}
Write-Output "Results: $output"
