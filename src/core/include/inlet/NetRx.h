// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * Inlet's AES67 receive path: multicast RTP off the wire, into timestamp-indexed
 * rings that live in shared memory.
 *
 * AES67 puts every stream on one port (5004) and tells them apart by multicast
 * group. Windows cannot bind a socket to a group address, so a socket per
 * stream on a shared port would receive every group joined on it. Inlet opens
 * one socket per (port, interface), joins every group on it, and demultiplexes
 * each datagram by its destination address (IP_PKTINFO) and, for
 * source-specific streams, its source.
 *
 *  - Windows: overlapped WSARecvMsg with an I/O completion port, many receives
 *    outstanding per socket, the thread in the MMCSS "Pro Audio" class.
 *  - macOS and other POSIX: poll + recvmsg, IP_PKTINFO/IP_RECVPKTINFO.
 *
 * The rings are the caller's (they are in the shared memory the drivers read);
 * this writes them by RTP timestamp minus the stream's media-clock offset, so a
 * ring's index *is* the media clock and a driver reads it at its own sample
 * position less the link offset.
 *
 * Packet parsing and sample conversion are libaes67's (rtp.h). The socket
 * handling is here, not in libaes67's aes67_rx, because aes67_rx owns its rings
 * (heap memory) and its sockets (one per stream); Inlet needs neither.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct aes67_ring;

namespace inlet
{

struct RxStreamConfig
{
    uint32_t group = 0;          ///< destination multicast group, network byte order
    uint32_t source = 0;         ///< source-specific (SSM) sender, network order; 0 = any
    uint16_t port = 5004;
    uint8_t payloadType = 96;
    uint8_t format = 0;          ///< aes67_sample_format_t: L24, L16, AM824
    uint16_t channels = 0;
    uint32_t mediaClockOffset = 0;   ///< SDP a=mediaclk:direct=<offset>
    aes67_ring* rings = nullptr;     ///< `channels` rings, one per channel
};

struct RxStreamStats
{
    uint64_t packets = 0, bytes = 0;
    uint64_t lost = 0, reordered = 0, wrongPayload = 0, malformed = 0;
    uint64_t jitterWorstNs = 0;      ///< worst inter-arrival gap
    int64_t lastArrivalMono = 0;
};

class NetRx
{
public:
    static constexpr int kMaxStreams = 128;
    static constexpr int kMaxPorts = 16;

    /** `ifaceIp`: the local interface (dotted quad) for joins; empty = the
        system's choice. Null with a reason when sockets are unavailable. */
    static std::unique_ptr<NetRx> open(const std::string& ifaceIp, std::string& error);
    ~NetRx();

    /** Starts receiving: a socket for the stream's port (if it has none yet),
        a join of its group. Any thread, while running or not. Returns the
        stream's slot, or -1 with the reason. */
    int addStream(const RxStreamConfig& cfg, std::string& error);
    /** Leaves the group (unless another stream shares it) and stops writing
        the stream's rings. On return the receive thread no longer touches
        them. */
    void removeStream(int slot);

    bool start();
    void stop();

    RxStreamStats stats(int slot) const;

    /** Feeds one datagram as if it had arrived: the whole demux, parse and
        ring path without a socket. Addresses in network byte order. */
    void inject(uint32_t source, uint32_t destination, uint16_t port, const uint8_t* packet, size_t len,
                int64_t arrivalMono);

    struct Impl;

private:
    explicit NetRx(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl;
};

/** Dotted quad to network-order IPv4; false when malformed. */
bool parseIpv4(const std::string& text, uint32_t& out);

} // namespace inlet
