# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Installs Inlet from this folder (run install.cmd, which asks for
# administrator rights):
#   - files to C:\Program Files\Inlet;
#   - the ASIO driver registered (DAWs list it as "Inlet");
#   - inletd as an automatic Windows service ("Inlet"), allowed through the firewall;
#   - the Inlet app in the Start menu, and in the tray at every sign-in.
# Settings and the log are in C:\ProgramData\Inlet and survive reinstalling.

$ErrorActionPreference = "Stop"
$src = $PSScriptRoot
$dest = Join-Path $env:ProgramFiles "Inlet"

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run install.cmd (it asks for administrator rights)."
}

Write-Host "Installing Inlet to $dest"

# What is running from an earlier install stops first.
Get-Process -Name "Inlet" -ErrorAction SilentlyContinue | Stop-Process -Force
if (Get-Service -Name "inletd" -ErrorAction SilentlyContinue) {
    Stop-Service -Name "inletd" -Force -ErrorAction SilentlyContinue
    & sc.exe delete inletd | Out-Null
    Start-Sleep -Seconds 2
}
Get-Process -Name "inletd", "gst-ptp-helper" -ErrorAction SilentlyContinue | Stop-Process -Force

New-Item -ItemType Directory -Force $dest | Out-Null
Get-ChildItem $src -Exclude "install.*", "uninstall.*" | Copy-Item -Destination $dest -Recurse -Force
Copy-Item (Join-Path $src "uninstall.ps1"), (Join-Path $src "uninstall.cmd") $dest -Force

# The ASIO driver.
& regsvr32.exe /s (Join-Path $dest "inlet_asio.dll")
if ($LASTEXITCODE) { throw "Registering the ASIO driver failed." }

# The firewall: inletd (NMOS API, streams, SAP, mDNS) and GStreamer's PTP helper.
Get-NetFirewallRule -DisplayName "Inlet*" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName "Inlet service" -Direction Inbound -Program (Join-Path $dest "inletd.exe") -Action Allow -Profile Any | Out-Null
New-NetFirewallRule -DisplayName "Inlet PTP" -Direction Inbound -Program (Join-Path $dest "gst-ptp-helper.exe") -Action Allow -Profile Any | Out-Null

# DSCP EF (46) on Inlet's audio, as AES67 asks; Windows applies it from a QoS policy.
Remove-NetQosPolicy -Name "Inlet audio" -Confirm:$false -ErrorAction SilentlyContinue
New-NetQosPolicy -Name "Inlet audio" -AppPathNameMatchCondition "inletd.exe" -DSCPAction 46 -NetworkProfile All -ErrorAction SilentlyContinue | Out-Null

# The service: installed and started by inletd itself.
& (Join-Path $dest "inletd.exe") --install-service
if ($LASTEXITCODE) { throw "Installing the Inlet service failed." }
# Restart it if it ever stops unexpectedly.
& sc.exe failure inletd reset= 86400 actions= restart/5000/restart/5000/restart/30000 | Out-Null

# The app: Start menu, and the tray at sign-in.
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut((Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Inlet.lnk"))
$lnk.TargetPath = Join-Path $dest "Inlet.exe"
$lnk.WorkingDirectory = $dest
$lnk.Save()
Set-ItemProperty -Path "HKLM:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "Inlet" -Value "`"$(Join-Path $dest 'Inlet.exe')`" --tray"

# Started as the signed-in user, not elevated.
Start-Process explorer.exe (Join-Path $dest "Inlet.exe")

if (-not (Test-Path (Join-Path $env:SystemRoot "System32\ucrtbase.dll"))) {
    Write-Warning "The Universal C Runtime is missing; install the Microsoft Visual C++ Redistributable (x64)."
}
Write-Host "Inlet is installed. The Inlet app is opening; the service log is $env:ProgramData\Inlet\inletd.log"
