#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
"""A minimal PTPv2 grandmaster for testing Inlet on a desk with no real one.

Two-step, multicast, end-to-end: Announce once a second, Sync and Follow_Up
eight times a second (the AES67 media profile's rates), and a Delay_Resp for
every Delay_Req. Its time is the system clock plus TAI - UTC, timestamped in
software. Good enough to exercise lock, holdover and stepping; useless as a
reference for accuracy.

    fake_ptp_master.py --interface 192.168.1.50 [--domain 0] [--step-after 20]
"""

import argparse
import select
import socket
import struct
import time

PTP_GROUP = "224.0.1.129"
EVENT_PORT, GENERAL_PORT = 319, 320
SYNC, DELAY_REQ, FOLLOW_UP, DELAY_RESP, ANNOUNCE = 0x0, 0x1, 0x8, 0x9, 0xB
CONTROL = {SYNC: 0, DELAY_REQ: 1, FOLLOW_UP: 2, DELAY_RESP: 3, ANNOUNCE: 5}
TAI_MINUS_UTC = 37


def timestamp(ns):
    s, n = divmod(ns, 1_000_000_000)
    return struct.pack(">HIL", (s >> 32) & 0xFFFF, s & 0xFFFFFFFF, n)


def header(msg, length, domain, clock_id, seq, log_interval, flags=0):
    return struct.pack(">BBHBBHq4s8sHHBb", msg, 2, length, domain, 0, flags, 0, b"\0" * 4,
                       clock_id, 1, seq & 0xFFFF, CONTROL[msg], log_interval)


def open_socket(port, iface):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", port))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 socket.inet_aton(PTP_GROUP) + socket.inet_aton(iface))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(iface))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--interface", required=True, help="local IPv4 address to run on")
    ap.add_argument("--domain", type=int, default=0)
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0 = never)")
    ap.add_argument("--step-after", type=float, default=0, help="jump time by 1 s after this long")
    args = ap.parse_args()

    clock_id = bytes([0x02, 0x00, 0x5E, 0xFF, 0xFE, 0x12, 0x34, 0x56])
    ev, gen = open_socket(EVENT_PORT, args.interface), open_socket(GENERAL_PORT, args.interface)
    start = time.monotonic()
    offset = TAI_MINUS_UTC * 1_000_000_000
    seq_sync = seq_ann = 0
    next_sync = next_ann = time.monotonic()
    stepped = False

    def now():
        return time.time_ns() + offset

    while True:
        t = time.monotonic()
        if args.seconds and t - start > args.seconds:
            break
        if args.step_after and not stepped and t - start > args.step_after:
            offset += 1_000_000_000
            stepped = True
            print("stepped by 1 s", flush=True)
        if t >= next_ann:
            body = timestamp(0) + struct.pack(">hBBBBHB8sHB", TAI_MINUS_UTC, 0, 128, 248, 0xFE, 0xFFFF, 128,
                                              clock_id, 0, 0xA0)
            gen.sendto(header(ANNOUNCE, 64, args.domain, clock_id, seq_ann, 0) + body,
                       (PTP_GROUP, GENERAL_PORT))
            seq_ann += 1
            next_ann += 1.0
        if t >= next_sync:
            t1 = now()
            ev.sendto(header(SYNC, 44, args.domain, clock_id, seq_sync, -3, flags=0x0200) + timestamp(0),
                      (PTP_GROUP, EVENT_PORT))
            gen.sendto(header(FOLLOW_UP, 44, args.domain, clock_id, seq_sync, -3) + timestamp(t1),
                       (PTP_GROUP, GENERAL_PORT))
            seq_sync += 1
            next_sync += 0.125
        ready, _, _ = select.select([ev], [], [], 0.005)
        if ready:
            data, _ = ev.recvfrom(256)
            t4 = now()
            if len(data) >= 44 and (data[0] & 0x0F) == DELAY_REQ and data[4] == args.domain:
                requester = data[20:30]
                if requester[:8] == clock_id:
                    continue
                seq = struct.unpack(">H", data[30:32])[0]
                resp = header(DELAY_RESP, 54, args.domain, clock_id, seq, -3) + timestamp(t4) + requester
                gen.sendto(resp, (PTP_GROUP, GENERAL_PORT))


if __name__ == "__main__":
    main()
