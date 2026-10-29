// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * The driver's side of Inlet: what an ASIO driver or the Core Audio plug-in
 * runs on. It attaches to inletd's shared region, claims a client slot, and
 * moves one period of audio at a time between the host's buffers and the
 * rings, on the media clock.
 *
 * Timing, for a period ending at device sample b (PeriodClock):
 *  - inputs: the host gets samples [b - frames, b) as received, which is the
 *    ring read at b - frames - linkOffset (the network's audio arrives within
 *    the link offset of its timestamp);
 *  - outputs: what the host writes in that callback plays from b + frames
 *    (ASIO's double buffer: the half filled now is the one played next), and
 *    is written to the rings at that position, before the pacer needs it.
 *
 * Each client writing outputs has a layer of rings of its own, and the sender
 * sums the layers: several hosts play out at once, mixed. Past
 * shm::kOutputLayers of them, a host plays inputs only, and the status says so.
 *
 * Real-time safety: readInputs/writeOutputs/heartbeat take no locks and
 * allocate nothing.
 */

#include "inlet/Clock.h"
#include "inlet/SharedRegion.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace inlet
{

class DeviceClient
{
public:
    struct Options
    {
        shm::ClientKind kind = shm::ClientKind::Asio;
        std::string app;            ///< the host application, for the status
        const char* regionName = nullptr;   ///< tests: a private region
    };

    static std::unique_ptr<DeviceClient> attach(const Options& options, std::string& error);
    ~DeviceClient();

    uint32_t inputs() const { return region->header().inputs; }
    uint32_t outputs() const { return region->header().outputs; }
    uint32_t sampleRate() const { return region->header().sampleRate; }
    int32_t linkOffsetFrames() const { return region->header().linkOffsetFrames.load(std::memory_order_relaxed); }
    uint64_t serial() const { return region->header().serial; }

    bool clock(ClockSample& out) const { return region->clock().read(out); }

    /** inletd is alive: its heartbeat is recent. */
    bool daemonAlive(int64_t nowMono) const;

    /** Takes a free output layer. True when this client writes outputs (now
        or already); false when every layer is taken. */
    bool claimOutputs();
    void releaseOutputs();
    bool ownsOutputs() const;

    /** Host input buffers for the period starting at `periodStart` (device
        samples). `channels[c]` may be null (an input the host did not ask
        for). 24-bit integers become floats in [-1, 1). */
    void readInputs(uint64_t periodStart, float* const* channels, int count, int frames);
    /** The same, as ASIO's Int32LSB: the 24-bit word left-justified. */
    void readInputsInt32(uint64_t periodStart, int32_t* const* channels, int count, int frames);

    /** One channel at a time (what ASIO's per-channel buffers want). */
    void readInputInt32(uint64_t periodStart, int channel, int32_t* dst, int frames);
    void writeOutputInt32(uint64_t firstSample, int channel, const int32_t* src, int frames);

    /** The rings as they are: 24-bit samples at device positions, no link
        offset (for a driver converting rates, which keeps its own positions).
        A missing frame reads as zero. */
    void readInputRaw(uint64_t position, int channel, int32_t* dst, int frames);
    void writeOutputRaw(uint64_t position, int channel, const int32_t* src, int frames);

    /** Host output buffers to play from `firstSample`. Ignored unless this
        client owns the outputs. */
    void writeOutputs(uint64_t firstSample, const float* const* channels, int count, int frames);
    void writeOutputsInt32(uint64_t firstSample, const int32_t* const* channels, int count, int frames);

    /** The client slot's liveness and counters; call once a period.
        `hostRate`: the rate the host runs at (0: the device's). */
    void heartbeat(int64_t nowMono, bool running, uint32_t bufferFrames, uint32_t hostRate = 0);
    void countPeriod(bool late, bool resynced);

    /** Renames the client in the status: the Core Audio plug-in is one client
        for every program playing through it, and names those playing now. */
    void setApp(const std::string& app);

    /** A device input's or output's name, as labelled (empty = unnamed). */
    std::string inputLabel(int channel) const;
    std::string outputLabel(int channel) const;

    shm::Region& shared() { return *region; }

private:
    DeviceClient() = default;
    std::unique_ptr<shm::Region> region;
    int slot = -1;
    int layer = -1;   // the output layer this client writes; -1 = none
};

/**
 * A thread that wakes at each period boundary of the media clock and calls
 * back with the period's first sample: the ASIO driver's clock (Core Audio
 * has its own IO thread and takes zero timestamps instead).
 *
 * The periods are counted at `rate`, the host's rate, which need not be the
 * device's: a 44.1 kHz host on a 48 kHz network gets 44.1 kHz periods, still
 * on the PTP media clock.
 */
class PeriodEngine
{
public:
    struct Period
    {
        uint64_t start = 0;     ///< the period's first sample, at the engine's rate
        bool late = false;      ///< woke well past its deadline
        bool resynced = false;  ///< the schedule re-anchored: this start does not follow the last
    };
    /** Called on the engine's thread, at real-time priority. */
    using Callback = std::function<void(const Period&)>;

    /** `rate`: the periods' sample rate; 0 is the device's. */
    PeriodEngine(DeviceClient& client, uint32_t framesPerPeriod, Callback onPeriod, uint32_t rate = 0);
    ~PeriodEngine();

    bool start();
    void stop();
    bool running() const { return active.load(); }

    /** From the callback only: carry on with `next` (inletd restarted and the
        driver attached to its new region). The schedule re-anchors on the new
        clock, and the next period says so. */
    void switchClient(DeviceClient& next) { switchTo = &next; }

    uint64_t resyncs() const { return resyncCount.load(); }

private:
    void run();

    DeviceClient* client;
    DeviceClient* switchTo = nullptr;   // engine thread only
    uint32_t frames;
    uint32_t rate;
    Callback callback;
    std::atomic<bool> active { false };
    std::atomic<uint64_t> resyncCount { 0 };
    std::thread thread;
};

} // namespace inlet
