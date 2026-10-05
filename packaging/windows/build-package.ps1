# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Builds Inlet for Windows (Release, x64) and assembles a test package: a
# folder and a zip holding inletd, the ASIO driver, the Inlet app, the
# GStreamer runtime inletd needs for PTP, and the install scripts. Unsigned:
# Azure Artifact Signing is not set up yet.
#
#   powershell -ExecutionPolicy Bypass -File packaging\windows\build-package.ps1 [-JuceSource <dir>] [-GStreamerRoot <dir>]
#
# Needs Visual Studio 2022 or later (C++), CMake 3.24+, Ninja, and the GStreamer
# MSVC x86_64 runtime + development installers (1.24 or later).

param(
    [string]$JuceSource = "",
    [string]$GStreamerRoot = $(if ($env:GSTREAMER_1_0_ROOT_MSVC_X86_64) { $env:GSTREAMER_1_0_ROOT_MSVC_X86_64 } else { "C:\Program Files\gstreamer\1.0\msvc_x86_64" }),
    [string]$BuildDir = ""
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $BuildDir) { $BuildDir = Join-Path $root "build-release" }

# The MSVC environment, from the newest Visual Studio with C++.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with C++ was not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object { if ($_ -match "^(.*?)=(.*)$") { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }

git -C $root submodule update --init
$configure = @("-S", $root, "-B", $BuildDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF", "-DGSTREAMER_ROOT=$GStreamerRoot")
if ($JuceSource) { $configure += "-DFETCHCONTENT_SOURCE_DIR_JUCE=$JuceSource" }
cmake @configure
if ($LASTEXITCODE) { throw "configure failed" }
cmake --build $BuildDir --target inletd inlet_asio InletApp inlet-asio-loopback inlet-ptp-probe
if ($LASTEXITCODE) { throw "build failed" }

$version = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern "project\(Inlet VERSION ([0-9.]+)").Matches[0].Groups[1].Value
$name = "Inlet-$version-windows-x64"
$dist = Join-Path $root "dist\$name"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null

# Inlet itself.
Copy-Item "$BuildDir\src\inletd\inletd.exe" $dist
Copy-Item "$BuildDir\src\asio\inlet_asio.dll" $dist
Copy-Item "$BuildDir\src\app\InletApp_artefacts\Release\Inlet.exe" $dist
New-Item -ItemType Directory -Force "$dist\tools" | Out-Null
Copy-Item "$BuildDir\tools\inlet-asio-loopback.exe" "$dist\tools"
Copy-Item "$BuildDir\tools\inlet-ptp-probe.exe" $dist   # beside the GStreamer DLLs it needs
Copy-Item "$root\tools\fake_ptp_master.py" "$dist\tools"

# GStreamer's PTP follower and what it links (LGPL; see THIRD-PARTY.txt).
foreach ($dll in "gstnet-1.0-0", "gstreamer-1.0-0", "gstbase-1.0-0", "gio-2.0-0", "gobject-2.0-0", "glib-2.0-0",
                 "gmodule-2.0-0", "intl-8", "ffi-7", "pcre2-8-0", "z-1") {
    Copy-Item "$GStreamerRoot\bin\$dll.dll" $dist
}
Copy-Item "$GStreamerRoot\libexec\gstreamer-1.0\gst-ptp-helper.exe" $dist
# GLib starts the PTP helper through these on Windows.
Copy-Item "$GStreamerRoot\bin\gspawn-win64-helper.exe", "$GStreamerRoot\bin\gspawn-win64-helper-console.exe" $dist
# The C runtime GStreamer's DLLs use (Inlet's own binaries link it statically).
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT\vcruntime140.dll" | Sort-Object FullName | Select-Object -Last 1
Copy-Item $crt.FullName $dist
Copy-Item (Join-Path $crt.DirectoryName "vcruntime140_1.dll") $dist

# The scripts, the guide and the licences.
Copy-Item "$PSScriptRoot\install.ps1", "$PSScriptRoot\uninstall.ps1", "$PSScriptRoot\install.cmd", "$PSScriptRoot\uninstall.cmd", "$PSScriptRoot\TESTING.txt" $dist
Copy-Item "$root\LICENSE" "$dist\LICENSE.txt"
Copy-Item "$PSScriptRoot\THIRD-PARTY.txt" $dist

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$dist\*" -DestinationPath $zip
Write-Host "Package: $zip"
