#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Removes Inlet:
#   sudo "/Library/Application Support/Inlet/uninstall.sh" [--purge]
# Settings (/Library/Application Support/Inlet/inletd.json) and the log stay
# unless --purge is given.

if [ "$(id -u)" != "0" ]; then
    echo "Run it with sudo." >&2
    exit 1
fi

launchctl bootout system/co.meros.inletd 2>/dev/null
user=$(stat -f %u /dev/console)
if [ "$user" != "0" ]; then
    launchctl bootout "gui/$user/co.meros.inlet" 2>/dev/null
fi
pkill -x Inlet 2>/dev/null
pkill -x inletd 2>/dev/null
pkill -x gst-ptp-helper 2>/dev/null

fw=/usr/libexec/ApplicationFirewall/socketfilterfw
bin="/Library/Application Support/Inlet/bin"
if [ -x "$fw" ]; then
    "$fw" --remove "$bin/inletd" >/dev/null 2>&1
    "$fw" --remove "$bin/gst-ptp-helper" >/dev/null 2>&1
fi

rm -f /Library/LaunchDaemons/co.meros.inletd.plist /Library/LaunchAgents/co.meros.inlet.plist
rm -rf /Library/Audio/Plug-Ins/HAL/Inlet.driver /Applications/Inlet.app "$bin"
rm -f "/Library/Application Support/Inlet/uninstall.sh" "/Library/Application Support/Inlet/TESTING.txt"
rm -rf "/Library/Application Support/Inlet/licences" "/Library/Application Support/Inlet/LICENSE.txt" \
       "/Library/Application Support/Inlet/THIRD-PARTY.txt"
killall coreaudiod 2>/dev/null
pkgutil --forget co.meros.inlet >/dev/null 2>&1

if [ "$1" = "--purge" ]; then
    rm -rf "/Library/Application Support/Inlet" /Library/Logs/Inlet
    echo "Inlet is removed, with its settings and log."
else
    echo "Inlet is removed; its settings remain in /Library/Application Support/Inlet."
fi
