// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "check.h"
#include "inlet/Clock.h"

#include <aes67/rtp.h>

#include <atomic>
#include <thread>

using namespace inlet;

static void mediaClockArithmetic()
{
    for (uint32_t rate : { 44100u, 48000u, 96000u })
    {
        // Round trip, and the boundary is exact: one nanosecond earlier is the
        // previous sample.
        for (uint64_t i : { 1ull, 47999ull, 48000ull, 123456789ull, 85'000'000'000'000ull, 300'000'000'000'007ull })
        {
            const int64_t t = ptpAtSample(i, rate);
            CHECK_EQ(sampleAt(t, rate), i);
            CHECK_EQ(sampleAt(t - 1, rate), i - 1);
        }
        // The RTP timestamp agrees with libaes67's media clock.
        for (int64_t ns : { 0LL, 999'999'999LL, 1'780'000'000'123'456'789LL })
            CHECK_EQ(rtpTimestamp(sampleAt(ns, rate)), aes67_media_clock_ts((uint64_t) ns, rate));
    }
}

static void mappingInverts()
{
    ClockSample c;
    c.monoNs = 5'000'000'000LL;
    c.ptpNs = 1'780'000'000'000'000'000LL;
    c.rate = 1.0 + 37e-6;
    for (int64_t d : { 0LL, 1LL, 1'000'000LL, 3'600'000'000'000LL })
    {
        const int64_t mono = c.monoNs + d;
        CHECK_NEAR(c.monoAt(c.ptpAt(mono)), mono, 1.0);
    }
}

static void seqlockNeverTears()
{
    ClockMap map;
    std::atomic<bool> stop { false };
    std::thread writer([&] {
        ClockSample s;
        for (int64_t i = 1; !stop.load(); ++i)
        {
            s.monoNs = i;
            s.ptpNs = 3 * i;
            s.epoch = (uint32_t) i;
            s.rate = (double) i;
            map.publish(s);
        }
    });
    // Not before the writer's first sample (a loaded machine may start it late).
    for (ClockSample first; !map.read(first);)
        std::this_thread::yield();
    int reads = 0;
    for (int i = 0; i < 2'000'000; ++i)
    {
        ClockSample s;
        if (!map.read(s))
            continue;
        ++reads;
        if (s.ptpNs != 3 * s.monoNs || (int64_t) s.epoch != s.monoNs || s.rate != (double) s.monoNs)
        {
            CHECK(!"torn read");
            break;
        }
    }
    stop = true;
    writer.join();
    CHECK(reads > 0);
}

/** A day of 128-frame periods at 48 kHz on a clock 50 ppm off the local one:
    the device counter stays exactly on the media clock and never re-anchors. */
static void periodsFollowTheMediaClock()
{
    ClockSample c;
    c.monoNs = 1'000'000'000LL;
    c.ptpNs = 1'780'000'000'000'000'000LL;
    c.rate = 1.0 + 50e-6;
    c.state = ClockState::Locked;

    PeriodClock pc(48000, 128);
    pc.start(c, c.monoNs);
    const uint64_t first = pc.periodStart();
    int64_t lastDeadline = pc.deadlineMono(c);
    const uint64_t periods = 24ull * 3600 * 48000 / 128;
    double worstSpacingError = 0;
    for (uint64_t k = 0; k < periods; ++k)
    {
        const int64_t deadline = pc.deadlineMono(c);
        if (k > 0)
        {
            // 128 samples of PTP time is 128/48000 s / rate of local time.
            const double expect = 128.0 / 48000.0 * 1e9 / c.rate;
            const double err = (double) (deadline - lastDeadline) - expect;
            if (err > worstSpacingError || -err > worstSpacingError)
                worstSpacingError = err < 0 ? -err : err;
        }
        lastDeadline = deadline;
        CHECK(!pc.advance(c, deadline));
        if (check::failures)
            return;
    }
    CHECK_EQ(pc.periodStart(), first + periods * 128);
    CHECK_EQ(pc.resyncs(), 0u);
    CHECK(worstSpacingError <= 2.0);   // nanoseconds: rounding only
}

static void periodsReanchor()
{
    ClockSample c;
    c.monoNs = 0;
    c.ptpNs = 1'000'000'000'000LL;
    PeriodClock pc(48000, 64);
    pc.start(c, 0);

    // PTP stepped: a new epoch re-anchors on the new timeline.
    c.epoch = 1;
    c.ptpNs += 5'000'000'000LL;
    CHECK(pc.advance(c, 1'000'000));
    CHECK_EQ(pc.periodEnd(), sampleAt(c.ptpAt(1'000'000), 48000) + 64);

    // Far behind (the thread was descheduled for 100 ms): re-anchor, counted.
    const int64_t late = pc.deadlineMono(c) + 100'000'000;
    CHECK(pc.advance(c, late));
    CHECK_EQ(pc.resyncs(), 2u);

    // Slightly behind is caught up, not re-anchored.
    const int64_t bitLate = pc.deadlineMono(c) + 500'000;
    CHECK(!pc.advance(c, bitLate));
}

int main()
{
    mediaClockArithmetic();
    mappingInverts();
    seqlockNeverTears();
    periodsFollowTheMediaClock();
    periodsReanchor();
    TEST_MAIN_END();
}
