# Fail closed: a valid signature without a trusted timestamp is not releasable.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Path,
    [Parameter(Mandatory)][int]$ExpectedCount
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$files = @(Get-ChildItem -LiteralPath $Path -File -Recurse | Where-Object Extension -eq '.exe')
if ($files.Count -ne $ExpectedCount -or $ExpectedCount -lt 1) {
    throw "Expected $ExpectedCount executables in $Path; found $($files.Count)"
}
$tools = @(Get-ChildItem "${env:ProgramFiles(x86)}/Windows Kits/10/bin/*/x64/signtool.exe" |
    Sort-Object { [version]$_.Directory.Parent.Name } -Descending)
if (!$tools.Count) { throw 'Windows SDK x64 SignTool is required' }
foreach ($file in $files) {
    $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
    if ($signature.Status -ne 'Valid' -or !$signature.SignerCertificate) {
        throw "Invalid Authenticode signature: $($file.FullName): $($signature.Status)"
    }
    if (!$signature.TimeStamperCertificate) {
        throw "Missing authenticated timestamp: $($file.FullName)"
    }
    # /all verifies every embedded signature; /tw warns about absent timestamps.
    # Treat warnings (exit 2), as well as errors, as a publication failure.
    & $tools[0].FullName verify /pa /all /tw /v $file.FullName
    if ($LASTEXITCODE -ne 0) { throw "SignTool verification failed: $($file.FullName)" }
}
