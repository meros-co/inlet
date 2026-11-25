# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Builds Inlet for Windows (Release, x64) and makes its installer,
# Inlet-<version>-windows-x64-setup.exe (packaging\windows\inlet.iss): inletd as
# a service, the ASIO driver, the Inlet app, the GStreamer runtime inletd needs
# for PTP, and the tools. Every binary and the installer are signed when Azure
# Artifact Signing is configured (see sign.ps1).
#
#   powershell -ExecutionPolicy Bypass -File packaging\windows\build-package.ps1 [-JuceSource <dir>] [-GStreamerRoot <dir>] [-OutDir <dir>]
#
# The installer goes to dist, or to -OutDir.
#
# Needs Visual Studio 2022 or later (C++), CMake 3.24+, Ninja, the GStreamer
# MSVC x86_64 runtime + development installers (1.24 or later) and Inno Setup 6.

param(
    [string]$JuceSource = "",
    [string]$GStreamerRoot = $(if ($env:GSTREAMER_1_0_ROOT_MSVC_X86_64) { $env:GSTREAMER_1_0_ROOT_MSVC_X86_64 } else { "C:\Program Files\gstreamer\1.0\msvc_x86_64" }),
    [string]$BuildDir = "",
    [string]$OutDir = ""
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $BuildDir) { $BuildDir = Join-Path $root "build-release" }
if (-not $OutDir) { $OutDir = Join-Path $root "dist" }
$GStreamerRoot = $GStreamerRoot.TrimEnd("\")

# Inno Setup's compiler: on the PATH, or where its installer puts it.
$iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
    $iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $iscc) { throw "Inno Setup 6 (ISCC.exe) was not found." }

# The MSVC environment, from the newest Visual Studio with C++.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with C++ was not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object { if ($_ -match "^(.*?)=(.*)$") { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }

git -C $root submodule update --init
$configure = @("-S", $root, "-B", $BuildDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF", "-DGSTREAMER_ROOT=$GStreamerRoot")
if ($JuceSource) { $configure += "-DFETCHCONTENT_SOURCE_DIR_JUCE=$JuceSource" }
cmake @configure
if ($LASTEXITCODE) { throw "configure failed" }
cmake --build $BuildDir --target inletd inlet_asio InletApp inlet-asio-loopback inlet-ptp-probe
if ($LASTEXITCODE) { throw "build failed" }

$version = & python (Join-Path $root "tools\set-version.py")
if ($LASTEXITCODE -or -not $version) { throw "the version could not be read" }
$stage = Join-Path $BuildDir "package"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage, "$stage\tools", "$stage\licences" | Out-Null

# Inlet itself.
Copy-Item "$BuildDir\src\inletd\inletd.exe" $stage
Copy-Item "$BuildDir\src\asio\inlet_asio.dll" $stage
Copy-Item "$BuildDir\src\app\InletApp_artefacts\Release\Inlet.exe" $stage
Copy-Item "$BuildDir\tools\inlet-asio-loopback.exe" "$stage\tools"
Copy-Item "$BuildDir\tools\inlet-ptp-probe.exe" $stage   # beside the GStreamer DLLs it needs
Copy-Item "$root\tools\fake_ptp_master.py" "$stage\tools"

# GStreamer's PTP follower and what it links (LGPL; see THIRD-PARTY.txt).
foreach ($dll in "gstnet-1.0-0", "gstreamer-1.0-0", "gstbase-1.0-0", "gio-2.0-0", "gobject-2.0-0", "glib-2.0-0",
                 "gmodule-2.0-0", "intl-8", "ffi-7", "pcre2-8-0", "z-1") {
    Copy-Item "$GStreamerRoot\bin\$dll.dll" $stage
}
Copy-Item "$GStreamerRoot\libexec\gstreamer-1.0\gst-ptp-helper.exe" $stage
# GLib starts the PTP helper through these on Windows.
Copy-Item "$GStreamerRoot\bin\gspawn-win64-helper.exe", "$GStreamerRoot\bin\gspawn-win64-helper-console.exe" $stage
# The C runtime GStreamer's DLLs use (Inlet's own binaries link it statically).
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT\vcruntime140.dll" | Sort-Object FullName | Select-Object -Last 1
Copy-Item $crt.FullName $stage
Copy-Item (Join-Path $crt.DirectoryName "vcruntime140_1.dll") $stage

# The guide and the licenses.
Copy-Item "$PSScriptRoot\TESTING.txt" $stage
Copy-Item "$root\LICENSE" "$stage\LICENSE.txt"
Copy-Item "$PSScriptRoot\THIRD-PARTY.txt" $stage
Copy-Item "$root\src\app\assets\fonts\Inter-OFL.txt" "$stage\licences\Inter-OFL.txt"
Copy-Item "$root\src\app\assets\icons\PHOSPHOR-LICENSE.txt" "$stage\licences\Phosphor-LICENSE.txt"

# Signed: every program and library not signed already (Microsoft's runtime is).
$sign = Join-Path $PSScriptRoot "sign.ps1"
$unsigned = Get-ChildItem $stage -Recurse -Include *.exe, *.dll |
    Where-Object { (Get-AuthenticodeSignature $_.FullName).Status -ne "Valid" } | ForEach-Object FullName
& $sign @unsigned
$signing = [bool]($env:ARTIFACT_SIGNING_ENDPOINT -and $env:ARTIFACT_SIGNING_ACCOUNT -and $env:ARTIFACT_SIGNING_PROFILE)

# The installer; Inno Setup signs it and its uninstaller through sign.ps1 too.
New-Item -ItemType Directory -Force $OutDir | Out-Null
$isccArgs = @("/Q", "/DVersion=$version", "/DStage=$stage", "/DOutDir=$OutDir",
              "/DIcon=$BuildDir\src\app\InletApp_artefacts\JuceLibraryCode\icon.ico")
if ($signing) {
    $isccArgs += "/DSign"
    $isccArgs += "/Sinlet=powershell.exe -NoProfile -ExecutionPolicy Bypass -File `$q$sign`$q `$f"
}
& $iscc @isccArgs (Join-Path $PSScriptRoot "inlet.iss")
if ($LASTEXITCODE) { throw "the installer did not compile" }
$setup = Join-Path $OutDir "Inlet-$version-windows-x64-setup.exe"
Write-Host "Installer: $setup$(if (-not $signing) { ' (unsigned)' })"
