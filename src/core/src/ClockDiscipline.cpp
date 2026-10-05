// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "inlet/ClockDiscipline.h"

#include <cstdlib>
#include <cstring>

namespace inlet
{

void ClockDiscipline::setSample(ClockSample next, int64_t nowMono)
{
    if (havePublished)
    {
        // Continuous unless PTP time at "now" moved by more than a step. A new
        // grandmaster that agrees with the old one (both on GPS, say) is not a
        // discontinuity; one that disagrees is.
        const int64_t jump = next.ptpAt(nowMono) - sample.ptpAt(nowMono);
        next.epoch = sample.epoch + (std::llabs(jump) > kStepNs ? 1u : 0u);
    }
    sample = next;
    havePublished = true;
}

void ClockDiscipline::update(const PtpCalibration& cal, int64_t internalMinusMono, int64_t nowMono)
{
    if (!cal.synced || cal.rate <= 0.0)
        return;
    ClockSample s;
    s.monoNs = cal.internalNs - internalMinusMono;
    s.ptpNs = cal.externalNs;
    s.rate = cal.rate;
    s.state = ClockState::Locked;
    std::memcpy(s.gmIdentity, cal.gmIdentity, sizeof s.gmIdentity);
    s.domain = cal.domain;
    setSample(s, nowMono);
    lastUpdateMono = nowMono;
}

ClockSample ClockDiscipline::current(int64_t nowMono)
{
    if (!havePublished)
    {
        ClockSample s;
        s.monoNs = nowMono;
        s.ptpNs = nowMono + freeRunOffset;
        s.rate = 1.0;
        s.state = ClockState::FreeRun;
        setSample(s, nowMono);
    }
    else if (sample.state == ClockState::Locked && nowMono - lastUpdateMono > kHoldoverAfterNs)
    {
        sample.state = ClockState::Holdover;   // same mapping, same epoch: the device keeps time
    }
    return sample;
}

} // namespace inlet
