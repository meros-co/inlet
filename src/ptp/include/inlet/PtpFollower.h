// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * Inlet's PTP follower: GStreamer's GstPtpClock (libgstnet), used directly.
 *
 * GstPtpClock is a slave-only PTPv2 ordinary clock: multicast, end-to-end,
 * any domain, best-master selection, no change to the system clock. Its helper
 * process (gst-ptp-helper, Rust since 1.24) owns ports 319/320 and stamps
 * packets itself; it runs on Windows, macOS and Linux. No pipeline is built:
 * Inlet takes the clock's calibration (PTP time as a linear function of the
 * local monotonic clock) and its statistics, and ClockDiscipline turns those
 * into the mapping the drivers run on.
 *
 * One follower per process: GStreamer's PTP subsystem is process-wide.
 */

#include "inlet/ClockDiscipline.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace inlet
{

struct PtpStatus
{
    bool available = false;        ///< the PTP subsystem started
    std::string reason;            ///< why not, when not
    bool synced = false;
    int domain = 0;
    bool gmPresent = false;
    uint8_t gmIdentity[8] = {};
    uint8_t masterIdentity[8] = {};
    unsigned masterPort = 0;
    int64_t meanPathDelayNs = 0;
    int64_t lastDiscontinuityNs = 0;   ///< latest measured-minus-estimated PTP time
    double rSquared = 0.0;             ///< fit quality of the clock estimate
    uint64_t updates = 0;              ///< time updates received

    /** "00-1D-C1-FF-FE-12-34-56", as AES67 SDPs write it. */
    static std::string identityText(const uint8_t id[8]);
};

class PtpFollower
{
public:
    /** Starts following `domain` on `interfaces` (names or addresses; empty =
        all). Null with the reason when GStreamer's PTP cannot start (helper
        missing, ports taken). */
    static std::unique_ptr<PtpFollower> open(int domain, const std::vector<std::string>& interfaces,
                                             std::string& error);
    ~PtpFollower();

    /** The latest calibration, and how far the follower's internal clock is
        from Inlet's monotonic clock: what ClockDiscipline::update takes.
        False until the first time update. */
    bool latest(PtpCalibration& cal, int64_t& internalMinusMono);

    PtpStatus status() const;

    /** PTP time now, read from GStreamer's clock directly: a cross-check of
        the published mapping, for diagnostics. Negative before sync. */
    int64_t ptpNowNs() const;

    struct Impl;

private:
    explicit PtpFollower(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl;
};

} // namespace inlet
