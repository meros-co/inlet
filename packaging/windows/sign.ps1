# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Signs Windows binaries with Azure Artifact Signing: signtool with Microsoft's
# signing client (fetched once from NuGet), timestamped. build-package.ps1
# calls it for Inlet's files and Inno Setup for the installer and uninstaller.
#
#   packaging\windows\sign.ps1 <file>...
#
# Signs when ARTIFACT_SIGNING_ENDPOINT, ARTIFACT_SIGNING_ACCOUNT and
# ARTIFACT_SIGNING_PROFILE are set, as the app registration in AZURE_TENANT_ID,
# AZURE_CLIENT_ID and AZURE_CLIENT_SECRET. Without them the files stay unsigned
# (a build for this computer), unless INLET_REQUIRE_SIGNING is set, as the
# release workflow sets it: then that is an error.

param([Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Files)
$ErrorActionPreference = "Stop"

$ClientPackage = "Microsoft.ArtifactSigning.Client"
$ClientVersion = "1.0.151"

function Test-SigningConfigured {
    return [bool]($env:ARTIFACT_SIGNING_ENDPOINT -and $env:ARTIFACT_SIGNING_ACCOUNT -and $env:ARTIFACT_SIGNING_PROFILE)
}

if (-not (Test-SigningConfigured)) {
    if ($env:INLET_REQUIRE_SIGNING) { throw "Signing is required, but ARTIFACT_SIGNING_ENDPOINT, _ACCOUNT and _PROFILE are not all set." }
    Write-Host "Not signed (no Azure Artifact Signing account configured): $($Files -join ', ')"
    exit 0
}

# signtool from the newest Windows SDK.
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Directory.Parent.Name } -ErrorAction SilentlyContinue | Select-Object -Last 1
if (-not $signtool) { throw "signtool.exe was not found; install the Windows SDK." }

# Microsoft's signing client, the dlib signtool loads.
$cache = Join-Path $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:LOCALAPPDATA }) "inlet-signing\$ClientPackage.$ClientVersion"
$dlib = Get-ChildItem $cache -Recurse -Filter "*.Dlib.dll" -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match "\\x64\\" } | Select-Object -First 1
if (-not $dlib) {
    New-Item -ItemType Directory -Force $cache | Out-Null
    $nupkg = Join-Path $cache "client.zip"
    Invoke-WebRequest "https://www.nuget.org/api/v2/package/$ClientPackage/$ClientVersion" -OutFile $nupkg
    Expand-Archive $nupkg -DestinationPath $cache -Force
    $dlib = Get-ChildItem $cache -Recurse -Filter "*.Dlib.dll" | Where-Object { $_.FullName -match "\\x64\\" } | Select-Object -First 1
    if (-not $dlib) { throw "$ClientPackage $ClientVersion has no x64 signing dlib." }
}

$metadata = Join-Path $cache "metadata.json"
@{
    Endpoint               = $env:ARTIFACT_SIGNING_ENDPOINT
    CodeSigningAccountName = $env:ARTIFACT_SIGNING_ACCOUNT
    CertificateProfileName = $env:ARTIFACT_SIGNING_PROFILE
} | ConvertTo-Json | Set-Content -Path $metadata -Encoding UTF8

& $signtool.FullName sign /v /fd SHA256 /tr "http://timestamp.acs.microsoft.com" /td SHA256 /dlib $dlib.FullName /dmdf $metadata @Files
if ($LASTEXITCODE) { throw "Signing failed ($LASTEXITCODE): $($Files -join ', ')" }
& $signtool.FullName verify /pa @Files | Out-Null
if ($LASTEXITCODE) { throw "A signature did not verify: $($Files -join ', ')" }
