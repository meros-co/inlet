#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# The ALSA plugin end to end on one machine: inletd on this machine's network,
# its receiver 1 playing its own sender 1 (multicast loops back), and
# inlet-alsa-loopback playing and recording "pcm.inlet" through it, at the
# network's rate and at others. The plugin and its configuration must be
# installed (cmake --install <build> --component inlet).
#
# Needs root (inletd's ports and its own ptp4l).
#
#   tests/linux/alsa_loopback.sh <build directory | installed> [seconds]
#
# "installed": the programs of an installed inlet package, on the PATH.

set -eu
build=$1
seconds=${2:-10}
if [ "$build" = installed ]; then
    inletd=inletd; loopback=inlet-alsa-loopback; inlet_pipewire=inlet-pipewire
else
    inletd="$build/src/inletd/inletd"; loopback="$build/tools/inlet-alsa-loopback"
    inlet_pipewire="$build/src/pipewire/inlet-pipewire"
fi
work=$(mktemp -d)
printf '{"settings":{"apiPort":8099,"receivers":2,"senders":2,"inputs":16,"outputs":16}}' > "$work/inletd.json"
"$inletd" --config "$work/inletd.json" > "$work/inletd.log" 2>&1 &
daemon=$!
cleanup() {
    kill "$daemon" 2>/dev/null || true
    wait "$daemon" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT
# A request to inletd's API, with bash alone (no HTTP client needed).
api() {
    exec 3<>/dev/tcp/127.0.0.1/8099 || return 1
    printf '%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %s\r\nConnection: close\r\n\r\n%s' \
        "$1" "/inlet/v1$2" "${#3}" "$3" >&3
    if [ "${4:-}" = show ]; then sed -n '/^\r$/,$p' <&3 | tail -n +2; else cat <&3 > /dev/null; fi
    exec 3>&-
}
for i in $(seq 1 50); do api GET /status "" 2>/dev/null && break; sleep 0.1; done
api PUT /receivers/1 '{"streamId":"own:1"}'
sleep 1

status=0
for rate in 48000 44100 96000; do
    "$loopback" inlet "$seconds" 256 "$rate" || status=1
done
# What the receiver and the sender saw.
api GET /receivers "" show | head -c 600; echo
api GET /senders "" show | head -c 400; echo
exit $status
