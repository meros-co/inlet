# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Removes Inlet (run uninstall.cmd, which asks for administrator rights).
# Settings in C:\ProgramData\Inlet stay unless -Purge is given.

param([switch]$Purge)
$ErrorActionPreference = "Continue"
$dest = Join-Path $env:ProgramFiles "Inlet"

Get-Process -Name "Inlet" -ErrorAction SilentlyContinue | Stop-Process -Force
if (Get-Service -Name "inletd" -ErrorAction SilentlyContinue) {
    Stop-Service -Name "inletd" -Force -ErrorAction SilentlyContinue
    & sc.exe delete inletd | Out-Null
    Start-Sleep -Seconds 2
}
Get-Process -Name "inletd", "gst-ptp-helper" -ErrorAction SilentlyContinue | Stop-Process -Force

if (Test-Path (Join-Path $dest "inlet_asio.dll")) {
    & regsvr32.exe /s /u (Join-Path $dest "inlet_asio.dll")
}
Get-NetFirewallRule -DisplayName "Inlet*" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
Remove-NetQosPolicy -Name "Inlet audio" -Confirm:$false -ErrorAction SilentlyContinue
Remove-ItemProperty -Path "HKLM:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "Inlet" -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Inlet.lnk") -ErrorAction SilentlyContinue

# This script may be running from the folder it removes: copy aside first.
Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
if ($Purge) { Remove-Item -Recurse -Force (Join-Path $env:ProgramData "Inlet") -ErrorAction SilentlyContinue }
Write-Host "Inlet is removed$(if (-not $Purge) { '; its settings remain in ' + (Join-Path $env:ProgramData 'Inlet') })."
