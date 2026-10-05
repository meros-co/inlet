// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * Turns what a PTP follower reports into the mapping the drivers run on.
 *
 * The follower (GStreamer's GstPtpClock) gives a calibration: PTP time as a
 * linear function of its own internal clock, which is the local monotonic
 * clock under another origin. This class owns the policy around it, with no
 * GStreamer in it so the tests can drive it:
 *
 *  - no grandmaster yet: free-run on the local clock (PTP time = local time
 *    plus a constant near TAI), so the device runs and its senders say they
 *    are on their own clock;
 *  - locked: publish the follower's calibration, moved onto Inlet's monotonic
 *    timeline;
 *  - the grandmaster goes quiet: holdover on the last mapping;
 *  - PTP time jumps (a new grandmaster, a stepped clock, lock after free-run):
 *    a new epoch, so periods re-anchor instead of racing to catch up.
 *
 * Inlet only ever follows. It never offers to be a grandmaster.
 */

#include "inlet/Clock.h"

#include <cstdint>

namespace inlet
{

/** A follower's calibration: ptp = external + (internal - internalBase) * rate. */
struct PtpCalibration
{
    bool synced = false;
    int64_t internalNs = 0;
    int64_t externalNs = 0;   ///< PTP (TAI) nanoseconds
    double rate = 1.0;
    uint8_t gmIdentity[8] = {};
    int32_t domain = -1;
};

class ClockDiscipline
{
public:
    /** Seconds without a fresh calibration before holdover. */
    static constexpr int64_t kHoldoverAfterNs = 3'000'000'000LL;
    /** A mapping that moves PTP time by more than this is a step. */
    static constexpr int64_t kStepNs = 1'000'000LL;

    /** `freeRunPtpMinusMono`: in free-run, PTP time = mono + this. Inlet uses
        the system's real time plus TAI - UTC, so a free-running device is
        roughly on the TAI timescale. */
    explicit ClockDiscipline(int64_t freeRunPtpMinusMono) : freeRunOffset(freeRunPtpMinusMono) {}

    /** A fresh calibration arrived. `internalMinusMono`: the follower's
        internal clock less Inlet's monotonic clock, measured. */
    void update(const PtpCalibration& cal, int64_t internalMinusMono, int64_t nowMono);

    /** What the map should say now. Call regularly (inletd: 10 Hz); it moves
        to holdover when updates stop. */
    ClockSample current(int64_t nowMono);

private:
    void setSample(ClockSample next, int64_t nowMono);

    int64_t freeRunOffset;
    ClockSample sample;        // starts free-running, epoch 0
    bool havePublished = false;
    int64_t lastUpdateMono = 0;
};

} // namespace inlet
