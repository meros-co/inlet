// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
//
// inlet-ptp-probe: follows PTP for a while and prints what Inlet's clock
// would do. Diagnostic only.
//
//   inlet-ptp-probe [--domain N] [--interface NAME|ADDR] [--seconds S]

#include "inlet/Clock.h"
#include "inlet/ClockDiscipline.h"
#include "inlet/PtpFollower.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace inlet;

static const char* stateName(ClockState s)
{
    switch (s)
    {
    case ClockState::Locked: return "locked";
    case ClockState::Holdover: return "holdover";
    default: return "free-run";
    }
}

int main(int argc, char** argv)
{
    int domain = 0, seconds = 30;
    std::vector<std::string> interfaces;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--domain") && i + 1 < argc)
            domain = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--interface") && i + 1 < argc)
            interfaces.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = std::atoi(argv[++i]);
        else
        {
            std::fprintf(stderr, "usage: %s [--domain N] [--interface NAME|ADDR] [--seconds S]\n", argv[0]);
            return 2;
        }
    }

    std::string err;
    auto ptp = PtpFollower::open(domain, interfaces, err);
    if (!ptp)
    {
        std::fprintf(stderr, "PTP: %s\n", err.c_str());
        return 1;
    }
    const auto realNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
    ClockDiscipline discipline(realNs + 37'000'000'000LL - monoNowNs());

    std::printf("following PTP domain %d for %d s\n", domain, seconds);
    for (int t = 0; t < seconds * 10; ++t)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const int64_t now = monoNowNs();
        PtpCalibration cal;
        int64_t off = 0;
        if (ptp->latest(cal, off))
            discipline.update(cal, off, now);
        const ClockSample s = discipline.current(now);
        if (t % 10 != 9)
            continue;
        const PtpStatus st = ptp->status();
        // The published mapping against GStreamer's own reading of PTP time.
        const int64_t gstPtp = ptp->ptpNowNs();
        const int64_t mapPtp = s.ptpAt(monoNowNs());
        if (gstPtp >= 0)
            std::printf("      mapping minus GStreamer: %lld ns\n", (long long) (mapPtp - gstPtp));
        std::printf("%3ds  %-8s epoch %u  gm %s  synced %d  path %lld ns  disc %lld ns  r2 %.4f  rate %+.3f ppm  updates %llu\n",
                    (t + 1) / 10, stateName(s.state), s.epoch,
                    st.gmPresent ? PtpStatus::identityText(st.gmIdentity).c_str() : "none", st.synced ? 1 : 0,
                    (long long) st.meanPathDelayNs, (long long) st.lastDiscontinuityNs, st.rSquared,
                    (s.rate - 1.0) * 1e6, (unsigned long long) st.updates);
        std::fflush(stdout);
    }
    return 0;
}
