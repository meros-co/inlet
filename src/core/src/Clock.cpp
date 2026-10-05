// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "inlet/Clock.h"

#include <bit>
#include <cmath>
#include <cstring>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach/mach_time.h>
#else
#  include <time.h>
#endif

namespace inlet
{

int64_t monoNowNs()
{
#if defined(_WIN32)
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (int64_t) f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const int64_t t = c.QuadPart;
    return (t / freq) * 1'000'000'000LL + ((t % freq) * 1'000'000'000LL) / freq;
#elif defined(__APPLE__)
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t i;
        mach_timebase_info(&i);
        return i;
    }();
    const uint64_t t = mach_absolute_time();
    if (tb.numer == tb.denom)
        return (int64_t) t;
    return (int64_t) ((__uint128_t) t * tb.numer / tb.denom);
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
#endif
}

int64_t ClockSample::ptpAt(int64_t mono) const
{
    return ptpNs + (int64_t) std::llround((double) (mono - monoNs) * rate);
}

int64_t ClockSample::monoAt(int64_t ptp) const
{
    return monoNs + (int64_t) std::llround((double) (ptp - ptpNs) / rate);
}

/* ---- the seqlock ------------------------------------------------------------- */

void ClockMap::publish(const ClockSample& s)
{
    uint64_t g = 0;
    std::memcpy(&g, s.gmIdentity, sizeof g);
    const uint32_t q = seq.load(std::memory_order_relaxed);
    seq.store(q + 1, std::memory_order_relaxed);          // odd: being written
    std::atomic_thread_fence(std::memory_order_release);
    monoNs.store(s.monoNs, std::memory_order_relaxed);
    ptpNs.store(s.ptpNs, std::memory_order_relaxed);
    rateBits.store(std::bit_cast<uint64_t>(s.rate), std::memory_order_relaxed);
    epoch.store(s.epoch, std::memory_order_relaxed);
    state.store((uint32_t) s.state, std::memory_order_relaxed);
    gm.store(g, std::memory_order_relaxed);
    domain.store(s.domain, std::memory_order_relaxed);
    seq.store(q + 2, std::memory_order_release);           // even: complete
}

bool ClockMap::read(ClockSample& out) const
{
    for (;;)
    {
        const uint32_t q0 = seq.load(std::memory_order_acquire);
        if (q0 == 0)
            return false;
        if (q0 & 1u)
            continue;   // the writer is mid-update; it takes nanoseconds
        ClockSample s;
        s.monoNs = monoNs.load(std::memory_order_relaxed);
        s.ptpNs = ptpNs.load(std::memory_order_relaxed);
        s.rate = std::bit_cast<double>(rateBits.load(std::memory_order_relaxed));
        s.epoch = epoch.load(std::memory_order_relaxed);
        s.state = (ClockState) state.load(std::memory_order_relaxed);
        const uint64_t g = gm.load(std::memory_order_relaxed);
        std::memcpy(s.gmIdentity, &g, sizeof g);
        s.domain = domain.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) == q0)
        {
            out = s;
            return true;
        }
    }
}

/* ---- the media clock ----------------------------------------------------------- */

uint64_t sampleAt(int64_t ptpNs, uint32_t rate)
{
    if (ptpNs < 0)
        return 0;
    const uint64_t s = (uint64_t) ptpNs / 1'000'000'000ull;
    const uint64_t ns = (uint64_t) ptpNs % 1'000'000'000ull;
    return s * rate + ns * rate / 1'000'000'000ull;
}

int64_t ptpAtSample(uint64_t index, uint32_t rate)
{
    const uint64_t s = index / rate;
    const uint64_t r = index % rate;
    return (int64_t) (s * 1'000'000'000ull + (r * 1'000'000'000ull + rate - 1) / rate);
}

/* ---- the period schedule --------------------------------------------------------- */

PeriodClock::PeriodClock(uint32_t sampleRate, uint32_t framesPerPeriod)
    : rate(sampleRate ? sampleRate : 48000), frames(framesPerPeriod ? framesPerPeriod : 1)
{
}

void PeriodClock::anchor(const ClockSample& clock, int64_t nowMono)
{
    boundary = sampleAt(clock.ptpAt(nowMono), rate) + frames;
    epoch = clock.epoch;
}

void PeriodClock::start(const ClockSample& clock, int64_t nowMono)
{
    anchor(clock, nowMono);
    resyncCount = 0;
}

int64_t PeriodClock::deadlineMono(const ClockSample& clock) const
{
    return clock.monoAt(ptpAtSample(boundary, rate));
}

bool PeriodClock::advance(const ClockSample& clock, int64_t nowMono, uint32_t maxLatePeriods)
{
    boundary += frames;
    if (clock.epoch != epoch)
    {
        anchor(clock, nowMono);
        ++resyncCount;
        return true;
    }
    const uint64_t now = sampleAt(clock.ptpAt(nowMono), rate);
    if (now > boundary && now - boundary > (uint64_t) maxLatePeriods * frames)
    {
        anchor(clock, nowMono);
        ++resyncCount;
        return true;
    }
    return false;
}

} // namespace inlet
