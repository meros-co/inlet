// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * Inlet's AES67 send path: shared rings in, paced RTP/L24 multicast out.
 *
 * The drivers write each output channel into a ring at the device's sample
 * position, which is the media clock. The pacer sends a packet for samples
 * [k*P, (k+1)*P) once PTP time has passed its last sample (plus a small send
 * offset), stamped with RTP timestamp k*P: AES67's `mediaclk:direct=0` sender,
 * exactly, because the device clock is the media clock. A packet whose samples
 * are not all in the ring (no host running, or a host that missed a period)
 * goes out as silence: the stream, its NMOS sender and every receiver's route
 * stay up while no DAW is open.
 *
 * The pacer never bursts to catch up after a long stall; it skips ahead and
 * counts it. Packet building is socket-free (pump()), which is what the tests
 * drive; the thread around it only decides when to call it.
 */

#include "inlet/Clock.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct aes67_ring;

namespace inlet
{

struct TxStreamConfig
{
    uint32_t group = 0;          ///< destination, network byte order
    uint16_t port = 5004;
    uint8_t payloadType = 96;
    uint16_t channels = 0;
    uint32_t sampleRate = 48000;
    uint32_t framesPerPacket = 48;   ///< 1 ms at 48 kHz
    uint32_t ssrc = 0;               ///< 0 = random
    aes67_ring* rings = nullptr;     ///< `channels` rings written by the drivers
    /** Several hosts' outputs: `layers` sets of `channels` rings,
        `layerStride` rings apart, summed. A layer whose `layerInUse` entry
        is zero is skipped (null: all are used). */
    int layers = 1;
    size_t layerStride = 0;
    const std::atomic<uint32_t>* layerInUse = nullptr;
};

struct TxStreamStats
{
    uint64_t packets = 0;
    uint64_t silentPackets = 0;   ///< sent as silence: samples missing from the rings
    uint64_t skipped = 0;         ///< packets never sent: the pacer stalled past its limit
    uint64_t sendErrors = 0;
};

class NetTx
{
public:
    static constexpr int kMaxStreams = 64;

    using SendFn = std::function<void(int slot, uint32_t group, uint16_t port, const uint8_t* packet, size_t len)>;

    /** `ifaceIp`: the interface to send from; empty = the system's choice.
        `clock`: the published mapping the pacer schedules on. */
    static std::unique_ptr<NetTx> open(const std::string& ifaceIp, const ClockMap& clock, std::string& error);
    /** Socket-free: packets go to `send` instead (tests, captures). */
    static std::unique_ptr<NetTx> openDetached(const ClockMap& clock, SendFn send);
    ~NetTx();

    int addStream(const TxStreamConfig& cfg, std::string& error);
    void removeStream(int slot);

    /** Microseconds after a packet's last sample before it is sent. */
    void setSendOffsetUs(uint32_t us);

    bool start();
    void stop();

    /** Sends every packet due at `nowMono`. The pacer thread calls this; the
        tests call it directly. Returns the local time the next packet is due
        (or nowMono + 1 ms when nothing is active). */
    int64_t pump(int64_t nowMono);

    TxStreamStats stats(int slot) const;

    /** The SSRC a stream sends with: the SDP states it. */
    uint32_t ssrc(int slot) const;

    struct Impl;

private:
    explicit NetTx(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl;
};

/** Sleeps until the local monotonic clock reaches `mono` (high-resolution
    timer then a short spin on Windows; mach_wait_until on macOS). */
void sleepUntilMono(int64_t mono);

} // namespace inlet
