#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# The PipeWire device end to end on one machine: inletd with its receiver 1
# playing its own sender 1, a PipeWire session (PipeWire, WirePlumber,
# pipewire-alsa) with inlet-pipewire in it, and inlet-alsa-loopback playing
# and recording through PipeWire's default sink and source, which are Inlet's.
# Programs on PipeWire then run on Inlet's clock: the samples come back at one
# offset.
#
# Needs root (inletd), PipeWire, WirePlumber, pipewire-alsa and D-Bus.
#
#   tests/linux/pipewire_loopback.sh <build directory> [seconds]

set -eu
build=$1
seconds=${2:-10}
work=$(mktemp -d)
export XDG_RUNTIME_DIR="$work/run"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
pids=()
cleanup() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

printf '{"settings":{"apiPort":8099,"receivers":2,"senders":2,"inputs":16,"outputs":16}}' > "$work/inletd.json"
"$build/src/inletd/inletd" --config "$work/inletd.json" > "$work/inletd.log" 2>&1 &
pids+=($!)
api() {
    exec 3<>/dev/tcp/127.0.0.1/8099 || return 1
    printf '%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %s\r\nConnection: close\r\n\r\n%s' \
        "$1" "/inlet/v1$2" "${#3}" "$3" >&3
    cat <&3 > /dev/null
    exec 3>&-
}
for i in $(seq 1 50); do api GET /status "" 2>/dev/null && break; sleep 0.1; done
api PUT /receivers/1 '{"streamId":"own:1"}'

# The user's session: a D-Bus, PipeWire, WirePlumber, then Inlet in it. In a
# container there is no system bus or logind, which WirePlumber's Bluetooth
# monitor cannot do without: it is turned off.
export XDG_CONFIG_HOME="$work/config"
mkdir -p "$XDG_CONFIG_HOME/wireplumber/bluetooth.lua.d"
# (Before 90-enable-all.lua, which turns everything on.)
echo 'bluez_monitor.enabled = false' > "$XDG_CONFIG_HOME/wireplumber/bluetooth.lua.d/80-no-bluetooth.lua"
dbus-daemon --session --fork --address="unix:path=$XDG_RUNTIME_DIR/bus" --print-pid > "$work/dbus.pid"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
pids+=("$(cat "$work/dbus.pid")")
pipewire > "$work/pipewire.log" 2>&1 &
pids+=($!)
sleep 2
wireplumber > "$work/wireplumber.log" 2>&1 &
pids+=($!)
"$build/src/pipewire/inlet-pipewire" > "$work/inlet-pipewire.log" 2>&1 &
pids+=($!)
sleep 3
cat "$work/inlet-pipewire.log"
wpctl status 2>/dev/null | sed -n '/Sinks:/,/Streams:/p' | head -12

status=0
for rate in 48000 44100; do
    timeout $((seconds + 30)) "$build/tools/inlet-alsa-loopback" pipewire "$seconds" 256 "$rate" 2 || status=1
done
exit $status
