// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * Inlet's clock: the PTP media clock, as seen from the local monotonic clock.
 *
 * A virtual sound card has no crystal of its own. Inlet decides when the host's
 * buffers are due, so it makes them due on the network's clock: the device's
 * sample counter *is* the AES67 media clock (RFC 7273 `mediaclk:direct=0`, PTP
 * time in samples), and a stream timed against the same grandmaster plays and
 * sends with no resampling and no drift.
 *
 * The network owner (inletd) follows PTP and publishes a linear mapping from
 * the local monotonic clock to PTP time. The drivers, in other processes, read
 * it wait-free and compute when each period falls. The mapping is a seqlock in
 * shared memory: plain atomics, no pointers, the same layout in every process.
 */

#include <atomic>
#include <cstdint>

namespace inlet
{

/** The local monotonic clock in nanoseconds: QueryPerformanceCounter on
    Windows, mach_absolute_time on macOS, CLOCK_MONOTONIC elsewhere. Every
    process on the machine reads the same timeline. */
int64_t monoNowNs();

/** Where the mapping comes from. */
enum class ClockState : uint32_t
{
    FreeRun = 0,   ///< no grandmaster: PTP time is the local clock, offset to TAI
    Locked = 1,    ///< following a grandmaster
    Holdover = 2,  ///< the grandmaster went quiet; running on the last rate
};

/** One reading of the mapping: ptp = ptpNs + (mono - monoNs) * rate. */
struct ClockSample
{
    int64_t monoNs = 0;
    int64_t ptpNs = 0;
    double rate = 1.0;          ///< PTP nanoseconds per local nanosecond
    uint32_t epoch = 0;         ///< bumped whenever PTP time steps; periods re-anchor
    ClockState state = ClockState::FreeRun;
    uint8_t gmIdentity[8] = {}; ///< zero when free-running
    int32_t domain = -1;

    int64_t ptpAt(int64_t mono) const;
    int64_t monoAt(int64_t ptp) const;
};

/** The published mapping. Lives in shared memory; one writer (inletd), any
    number of readers. Lock-free and address-free on every target Inlet
    builds for (64-bit atomics on x64 and arm64). */
class ClockMap
{
public:
    /** Writer only. */
    void publish(const ClockSample& s);
    /** Readers, any thread or process. False only before the first publish. */
    bool read(ClockSample& out) const;

private:
    std::atomic<uint32_t> seq { 0 };
    std::atomic<int64_t> monoNs { 0 };
    std::atomic<int64_t> ptpNs { 0 };
    std::atomic<uint64_t> rateBits { 0 };
    std::atomic<uint32_t> epoch { 0 };
    std::atomic<uint32_t> state { 0 };
    std::atomic<uint64_t> gm { 0 };
    std::atomic<int32_t> domain { -1 };
};

static_assert(std::atomic<uint64_t>::is_always_lock_free, "the clock map lives in shared memory");

/* ---- the media clock --------------------------------------------------------
 *
 * Sample indices are 64-bit counts since the PTP epoch; the RTP timestamp of a
 * sample is the low 32 bits. Exact integer arithmetic throughout: PTP
 * nanoseconds times a sample rate overflows 64 bits, so seconds and the
 * remainder are scaled apart. */

/** The index of the sample taken at `ptpNs` (the last sample boundary at or
    before it). */
uint64_t sampleAt(int64_t ptpNs, uint32_t rate);
/** The PTP time at which sample `index` is taken (rounded up to a nanosecond). */
int64_t ptpAtSample(uint64_t index, uint32_t rate);
inline uint32_t rtpTimestamp(uint64_t index) { return (uint32_t) index; }

/**
 * When each of a device's periods falls, on the published clock.
 *
 * A period ends at sample `boundary`; the host is called then, with the
 * period's first sample (`boundary - frames`) as its sample position. The
 * schedule advances exactly `frames` per period, so the device counter never
 * drifts from the media clock; it re-anchors only when the clock's epoch
 * changes (PTP stepped) or the thread fell so far behind that catching up
 * would be wrong (both counted).
 */
class PeriodClock
{
public:
    PeriodClock(uint32_t sampleRate, uint32_t framesPerPeriod);

    /** Starts the schedule: the first period ends one period after now. */
    void start(const ClockSample& clock, int64_t nowMono);

    /** The local time the current period ends: when to wake. */
    int64_t deadlineMono(const ClockSample& clock) const;
    /** First sample of the current period (ASIO's samplePosition, Core
        Audio's sample time). */
    uint64_t periodStart() const { return boundary - frames; }
    uint64_t periodEnd() const { return boundary; }

    /** The period was delivered: on to the next. Re-anchors on an epoch
        change, or when more than `maxLatePeriods` periods behind. Returns
        true when it re-anchored. */
    bool advance(const ClockSample& clock, int64_t nowMono, uint32_t maxLatePeriods = 4);

    uint32_t sampleRate() const { return rate; }
    uint32_t framesPerPeriod() const { return frames; }
    uint64_t resyncs() const { return resyncCount; }

private:
    void anchor(const ClockSample& clock, int64_t nowMono);

    uint32_t rate;
    uint32_t frames;
    uint64_t boundary = 0;
    uint32_t epoch = 0;
    uint64_t resyncCount = 0;
};

} // namespace inlet
