// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * Moves audio from one clock to another: an adaptive resampler steered by how
 * full its input is.
 *
 * Inlet's device clock is the PTP media clock, so a stream on the same
 * grandmaster needs none of this. A RateBridge sits between two sets of rings
 * only where the clocks differ:
 *
 *  - a received stream timed against another grandmaster, a sender's own
 *    clock (`ts-refclk:localmac`), or nothing at all;
 *  - a host running at another rate than the network (44.1 kHz on 48 kHz);
 *  - Inlet free-running with no grandmaster while a sender follows one.
 *
 * The input is a set of timestamp-indexed rings filled at the source's pace
 * (by the receive path); the output is a set of rings indexed by the device's
 * media clock, written a little ahead of where the drivers read. Each tick,
 * the bridge produces output up to `outNow + lead` and consumes whatever input
 * that takes at the current ratio.
 *
 * The servo: how far the input's newest frame is ahead of the next frame to
 * be consumed (the fill) is sampled every tick, and each half second the
 * window's mean is compared with the target. (The fill is a sawtooth -- whole
 * packets arrive, frames leave one by one -- so a single reading, or the
 * window's maximum, moves in packet-sized steps; the mean of five hundred is
 * smooth.) A critically damped PI loop turns the error into a ratio
 * correction; its integrator settles on the clocks' true difference. It
 * acquires with a 5 s time constant, then, once the fill has held within a
 * quarter of a millisecond for ten seconds, tracks with a 20 s one: the
 * reading still carries some packet-against-tick aliasing, and the slower loop
 * averages it away. The resampler is libsamplerate's band-limited sinc, which
 * changes ratio smoothly within a block.
 */

#include <cstdint>
#include <memory>
#include <vector>

struct aes67_ring;
struct SRC_STATE_tag;

namespace inlet
{

struct RateBridgeConfig
{
    uint32_t inRate = 48000;
    uint32_t outRate = 48000;
    uint16_t channels = 0;
    aes67_ring* in = nullptr;        ///< `channels` rings, source timeline
    aes67_ring* out = nullptr;       ///< `channels` rings, device media clock
    uint32_t targetFillFrames = 144; ///< input frames held behind the newest (3 ms at 48 kHz)
    uint32_t leadFrames = 96;        ///< output written this far ahead of now (2 ms at 48 kHz)
    int quality = 1;                 ///< libsamplerate converter: 0 best, 1 medium, 2 fastest
};

struct RateBridgeStats
{
    bool running = false;            ///< input seen, servo engaged
    bool tracking = false;           ///< past acquisition: the slow loop
    double correctionPpm = 0.0;      ///< ratio correction applied now
    double driftPpm = 0.0;           ///< the integrator: the clocks' learned difference
    double fillErrorFrames = 0.0;    ///< last window's maximum fill less the target
    uint64_t underruns = 0;          ///< ticks that ran out of input
    uint64_t resets = 0;             ///< restarts: the source jumped, or the output fell out of the window
    uint32_t warmupFrames = 0;       ///< the resampler's look-ahead, in input frames
};

class RateBridge
{
public:
    explicit RateBridge(const RateBridgeConfig& cfg);
    ~RateBridge();
    RateBridge(const RateBridge&) = delete;
    RateBridge& operator=(const RateBridge&) = delete;

    bool valid() const { return src != nullptr; }

    /** Call about once a millisecond. `outNow`: the device's current sample
        index at the output rate (the media clock). */
    void tick(uint64_t outNow);

    RateBridgeStats stats() const { return st; }

    static constexpr double kKp = 0.4;     ///< acquiring: per second of fill error
    static constexpr double kKi = 0.04;    ///< acquiring: per second squared (critically damped, ~5 s)
    static constexpr double kTrackDivide = 4.0;   ///< tracking: time constant x4 (Kp / 4, Ki / 16)
    static constexpr double kTrackAfterS = 10.0;  ///< in band this long before tracking
    static constexpr double kBandS = 0.25e-3;     ///< "in band": fill within this of the target
    static constexpr double kWindowS = 0.5;
    static constexpr double kMaxCorrection = 2e-3;

private:
    void reset(uint32_t head, uint64_t outNow);

    RateBridgeConfig cfg;
    SRC_STATE_tag* src = nullptr;
    double nominal = 1.0;   // out/in
    bool started = false;
    uint32_t inPos = 0;     // next input frame to hand the resampler
    uint64_t outNext = 0;   // next output frame to write
    double integrator = 0.0, correction = 0.0;
    uint32_t warmup = 0;    // input the resampler holds before it outputs
    bool primed = false;    // output produced since the last reset: underruns count from here
    double inBandS = 0.0;
    double windowSum = 0.0;
    uint64_t windowCount = 0;
    uint64_t windowStart = 0;
    std::vector<int32_t> scratch;
    std::vector<float> inBuf, outBuf;
    RateBridgeStats st;
};

} // namespace inlet
