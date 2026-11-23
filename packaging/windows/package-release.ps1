# Assemble the release from same-run artifacts, after signing, without rebuilding.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PayloadRoot,
    [string]$OutputPath = 'windows-release',
    [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$payload = (Resolve-Path $PayloadRoot).Path
$executables = @('takclient.exe', 'takserver.exe', 'cartographer.exe')
$expected = @($executables) + @('STREAMING-LICENSES.txt', 'EDITOR-FONT-LICENSE.txt', 'CHAT-FONT-LICENSE.txt', 'TITLE-FONT-LICENSE.txt', 'ACME-LICENSES.txt', 'OPUS-LICENSE.txt')
$versionMatch = [regex]::Match((Get-Content "$root/CMakeLists.txt" -Raw), 'project\(tak-engine VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
if (!$versionMatch.Success) { throw 'Cannot read project version' }
$version = $versionMatch.Groups[1].Value
if ($env:GITHUB_REF -and $env:GITHUB_REF -ne "refs/tags/v$version") {
    throw "Release tag must match project version v$version"
}
$groups = @('takclient-windows-x64', 'takclient-windows-x64-debug',
            'takclient-windows-arm64', 'takclient-windows-arm64-debug')
# Validate the entire input before spending a signing request or publishing files.
if (@(Get-ChildItem $payload -Directory).Count -ne $groups.Count) { throw 'Unexpected artifact set' }
foreach ($group in $groups) {
    $dir = Join-Path $payload $group
    $files = @(Get-ChildItem -LiteralPath $dir -File)
    if ((Compare-Object ($files.Name | Sort-Object) ($expected | Sort-Object)) -or
        @(Get-ChildItem -LiteralPath $dir -Directory).Count) {
        throw "Unexpected or missing files in $group"
    }
    # Signing on x64 must preserve native ARM64 outputs rather than substituting x64.
    $machine = if ($group -like '*arm64*') { 0xaa64 } else { 0x8664 }
    foreach ($exe in $executables) {
        $bytes = [IO.File]::ReadAllBytes((Join-Path $dir $exe))
        $offset = [BitConverter]::ToInt32($bytes, 0x3c)
        if ([BitConverter]::ToUInt32($bytes, $offset) -ne 0x4550 -or
            [BitConverter]::ToUInt16($bytes, $offset + 4) -ne $machine) {
            throw "Unexpected PE architecture: $group/$exe"
        }
    }
}
if ($ValidateOnly) { return }
& "$PSScriptRoot/verify-signatures.ps1" -Path $payload -ExpectedCount 12
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
if (Test-Path $output) { throw "Output folder already exists: $output" }
New-Item -ItemType Directory $output | Out-Null
foreach ($group in $groups) {
    $arch = if ($group -like '*arm64*') { 'arm64' } else { 'x64' }
    $name = if ($group.EndsWith('-debug')) { "takclient-windows-$arch-debug.zip" }
            else { "takclient-$version-windows-$arch.zip" }
    $dir = Join-Path $payload $group
    $zip = Join-Path $output $name
    Compress-Archive -Path "$dir/*" -DestinationPath $zip
    # Verify that packaging preserved every signed byte, including the timestamp.
    $extracted = Join-Path $output "verify-$group"
    Expand-Archive -LiteralPath $zip -DestinationPath $extracted
    foreach ($file in $expected) {
        if ((Get-FileHash "$dir/$file").Hash -ne (Get-FileHash "$extracted/$file").Hash) {
            throw "ZIP payload changed: $group/$file"
        }
    }
    Remove-Item $extracted -Recurse -Force
}
$build = Join-Path $output 'installer-build'
& cmake -S $PSScriptRoot -B $build "-DTAK_VERSION=$version" "-DTAK_SIGNED_PAYLOAD=$payload/takclient-windows-x64"
if ($LASTEXITCODE -ne 0) { throw 'Installer configuration failed' }
& cpack --config "$build/CPackConfig.cmake" -G NSIS -B $build
if ($LASTEXITCODE -ne 0) { throw 'NSIS packaging failed' }
# Check CPack's actual staged input, not just the source files it was given.
$staged = @(Get-ChildItem "$build/_CPack_Packages" -Filter takclient.exe -Recurse)
if ($staged.Count -ne 1) { throw 'Missing or ambiguous NSIS payload' }
foreach ($file in $expected) {
    if ((Get-FileHash "$payload/takclient-windows-x64/$file").Hash -ne
        (Get-FileHash (Join-Path $staged[0].DirectoryName $file)).Hash) {
        throw "NSIS payload changed: $file"
    }
}
Move-Item "$build/tak-engine-$version-windows-x64-setup.exe" $output
Remove-Item $build -Recurse -Force
