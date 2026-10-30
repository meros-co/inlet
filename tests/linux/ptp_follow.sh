#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Inlet following linuxptp, end to end, on one machine: a virtual cable (a
# veth pair) into a network namespace of its own, a software-stamping ptp4l
# grandmaster in there, and Inlet's PTP probe on this end, which starts
# Inlet's own ptp4l. The grandmaster's PTP time is this machine's real time on
# the TAI scale (the namespace shares the clock), so the probe's mapping should
# agree with real time + 37 s to within the jitter of software timestamps.
#
# Needs root, CAP_NET_ADMIN and CAP_SYS_ADMIN, and linuxptp (in Docker:
# --privileged, or --cap-add NET_ADMIN --cap-add SYS_ADMIN).
#
#   tests/linux/ptp_follow.sh <path to inlet-ptp-probe> [seconds] [extra probe arguments]

set -eu
probe=$1
seconds=${2:-30}
shift 2 || shift $#

ip netns add inletgm
ip link add inlet-gm type veth peer name inlet-fl
ip link set inlet-gm netns inletgm
ip -n inletgm addr add 10.99.0.1/24 dev inlet-gm
ip -n inletgm link set inlet-gm up
ip -n inletgm link set lo up
ip addr add 10.99.0.2/24 dev inlet-fl
ip link set inlet-fl up
cleanup() {
    kill "$gm" 2>/dev/null || true
    ip link del inlet-fl 2>/dev/null || true
    ip netns del inletgm 2>/dev/null || true
}
trap cleanup EXIT

# The grandmaster: software stamps, its own socket, domain 0, best priority.
ip netns exec inletgm ptp4l -i inlet-gm -S --priority1 1 --domainNumber 0 --uds_address /run/inlet-test-gm -q -l 4 &
gm=$!
# INLET_TEST_SYSTEM_PTP4L=1: a ptp4l already running on this end, on its
# default socket (free-running, so the test changes no clock), which Inlet
# should follow rather than start its own.
if [ "${INLET_TEST_SYSTEM_PTP4L:-0}" = 1 ]; then
    ptp4l -i inlet-fl -S --slaveOnly 1 --free_running 1 --domainNumber 0 -q -l 4 &
    sys=$!
    trap 'cleanup; kill "$sys" 2>/dev/null || true' EXIT
fi
sleep 1

"$probe" --interface inlet-fl --domain 0 --seconds "$seconds" --against-real-time "$@"
