// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "check.h"
#include "inlet/ClockDiscipline.h"

using namespace inlet;

static PtpCalibration cal(int64_t internal, int64_t external, double rate, uint8_t gm = 1)
{
    PtpCalibration c;
    c.synced = true;
    c.internalNs = internal;
    c.externalNs = external;
    c.rate = rate;
    c.gmIdentity[7] = gm;
    c.domain = 0;
    return c;
}

int main()
{
    const int64_t taiMinusMono = 1'780'000'000'000'000'000LL;
    ClockDiscipline d(taiMinusMono);

    // Free-run until a grandmaster: PTP time is the local clock plus the offset.
    ClockSample s = d.current(1'000);
    CHECK(s.state == ClockState::FreeRun);
    CHECK_EQ(s.ptpAt(5'000), 5'000 + taiMinusMono);
    CHECK_EQ(s.epoch, 0u);

    // Locked: the follower's internal clock is 7 s ahead of ours; PTP time 2 s
    // off free-run. A new epoch, and the mapping is the follower's.
    const int64_t internalMinusMono = 7'000'000'000LL;
    d.update(cal(10'000'000'000LL, taiMinusMono + 2'000'000'000LL, 1.0 + 20e-6), internalMinusMono, 3'000'000'000LL);
    s = d.current(3'000'000'000LL);
    CHECK(s.state == ClockState::Locked);
    CHECK_EQ(s.epoch, 1u);
    CHECK_EQ(s.gmIdentity[7], 1);
    // At our mono 3 s the follower's internal clock reads 10 s: PTP time is its external.
    CHECK_EQ(s.ptpAt(3'000'000'000LL), taiMinusMono + 2'000'000'000LL);

    // A refined calibration a microsecond off: same epoch.
    d.update(cal(11'000'000'000LL, taiMinusMono + 3'000'020'001LL, 1.0 + 20e-6), internalMinusMono, 4'000'000'000LL);
    s = d.current(4'000'000'000LL);
    CHECK_EQ(s.epoch, 1u);

    // A grandmaster that agrees: same epoch. One 5 ms off: a step.
    d.update(cal(12'000'000'000LL, taiMinusMono + 4'000'040'001LL, 1.0 + 20e-6, 2), internalMinusMono,
             5'000'000'000LL);
    CHECK_EQ(d.current(5'000'000'000LL).epoch, 1u);
    d.update(cal(13'000'000'000LL, taiMinusMono + 5'005'060'001LL, 1.0 + 20e-6, 3), internalMinusMono,
             6'000'000'000LL);
    CHECK_EQ(d.current(6'000'000'000LL).epoch, 2u);

    // Unsynced reports change nothing; silence becomes holdover on the same mapping.
    PtpCalibration lost = cal(0, 0, 1.0);
    lost.synced = false;
    d.update(lost, internalMinusMono, 6'500'000'000LL);
    s = d.current(6'500'000'000LL);
    CHECK(s.state == ClockState::Locked);
    const int64_t before = s.ptpAt(20'000'000'000LL);
    s = d.current(20'000'000'000LL);
    CHECK(s.state == ClockState::Holdover);
    CHECK_EQ(s.epoch, 2u);
    CHECK_EQ(s.ptpAt(20'000'000'000LL), before);

    TEST_MAIN_END();
}
