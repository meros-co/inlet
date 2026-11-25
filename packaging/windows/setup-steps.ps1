# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# What Inlet's installer (inlet.iss) does besides copying files, as
# administrator:
#   -Step Stop       before installing: the app, the service and the PTP helper
#                    of an earlier Inlet stop, and its service is removed;
#   -Step Configure  after: the ASIO driver registered ("Inlet" in DAWs),
#                    inletd as an automatic Windows service ("Inlet") restarted
#                    if it fails, the firewall rules, DSCP EF on the audio;
#   -Step Remove     on uninstall: all of that undone. Settings and the log in
#                    C:\ProgramData\Inlet stay.
# Exits non-zero when a step fails, with the reason on stderr and in -Log.

param(
    [Parameter(Mandatory)][ValidateSet("Stop", "Configure", "Remove")][string]$Step,
    [string]$AppDir = (Split-Path -Parent $PSCommandPath),
    [string]$Log = ""
)
$ErrorActionPreference = "Stop"

function Stop-Inlet {
    Get-Process -Name "Inlet" -ErrorAction SilentlyContinue | Stop-Process -Force
    if (Get-Service -Name "inletd" -ErrorAction SilentlyContinue) {
        Stop-Service -Name "inletd" -Force -ErrorAction SilentlyContinue
        & sc.exe delete inletd | Out-Null
        # The service manager removes it once nothing holds it open.
        for ($i = 0; $i -lt 20 -and (Get-Service -Name "inletd" -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 250 }
    }
    Get-Process -Name "inletd", "gst-ptp-helper" -ErrorAction SilentlyContinue | Stop-Process -Force
}

function Remove-NetworkRules {
    Get-NetFirewallRule -DisplayName "Inlet*" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    Remove-NetQosPolicy -Name "Inlet audio" -Confirm:$false -ErrorAction SilentlyContinue
}

try {
    switch ($Step) {
        "Stop" {
            Stop-Inlet
        }
        "Configure" {
            & regsvr32.exe /s (Join-Path $AppDir "inlet_asio.dll")
            if ($LASTEXITCODE) { throw "Registering the ASIO driver failed ($LASTEXITCODE)." }

            # inletd (NMOS API, streams, SAP, mDNS) and GStreamer's PTP helper receive.
            Remove-NetworkRules
            New-NetFirewallRule -DisplayName "Inlet service" -Direction Inbound -Program (Join-Path $AppDir "inletd.exe") -Action Allow -Profile Any | Out-Null
            New-NetFirewallRule -DisplayName "Inlet PTP" -Direction Inbound -Program (Join-Path $AppDir "gst-ptp-helper.exe") -Action Allow -Profile Any | Out-Null
            # DSCP EF (46) on Inlet's audio, as AES67 asks; Windows applies it from a QoS policy.
            New-NetQosPolicy -Name "Inlet audio" -AppPathNameMatchCondition "inletd.exe" -DSCPAction 46 -NetworkProfile All -ErrorAction SilentlyContinue | Out-Null

            # The service: installed and started by inletd itself, which also
            # lets a signed-in user start it (the app's Start Inlet).
            & (Join-Path $AppDir "inletd.exe") --install-service
            if ($LASTEXITCODE) { throw "Installing the Inlet service failed ($LASTEXITCODE)." }
            & sc.exe failure inletd reset= 86400 actions= restart/5000/restart/5000/restart/30000 | Out-Null
        }
        "Remove" {
            Stop-Inlet
            $dll = Join-Path $AppDir "inlet_asio.dll"
            if (Test-Path $dll) { & regsvr32.exe /s /u $dll }
            Remove-NetworkRules
        }
    }
    exit 0
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    if ($Log) { Set-Content -Path $Log -Value $_.Exception.Message -Encoding UTF8 }
    exit 1
}
