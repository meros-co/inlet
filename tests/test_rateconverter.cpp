// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "check.h"
#include "inlet/RateConverter.h"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <tuple>
#include <vector>

using namespace inlet;

namespace
{

constexpr double kPi = 3.14159265358979323846;

/** 200 Hz rising 100 Hz a second: never periodic within a run. */
double chirp(double t, double amp)
{
    return amp * std::sin(2 * kPi * (200.0 * t + 50.0 * t * t));
}

void positions()
{
    CHECK_EQ(convertPosition(48000, 48000, 44100), 44100u);
    CHECK_EQ(convertPosition(441, 44100, 48000), 480u);
    CHECK_EQ(convertPosition(1, 44100, 48000), 1u);   // 1.088..., rounded down
    // Today's sample counts (PTP seconds x rate) without overflow.
    const uint64_t t = 1'780'000'000ull * 96000ull + 12345;
    CHECK_EQ(convertPosition(t, 96000, 48000), 1'780'000'000ull * 48000ull + 6172);
    CHECK_EQ(convertPosition(convertPosition(t, 96000, 44100), 44100, 96000) <= t, true);
}

/**
 * The driver's way of using it: one host period at a time, `frames` of host
 * audio out per period, pulling exactly what the converter takes. A slow
 * chirp at the device rate goes in; what comes out at the host rate should be
 * the same chirp at the same time (the converter keeps the timeline), and the
 * input taken should track the output at the exact ratio for the whole run.
 */
double pulled(uint32_t deviceRate, uint32_t hostRate, int frames, double seconds)
{
    RateConverter conv(deviceRate, hostRate, 1);
    CHECK(conv.valid());
    const double amp = 0.5;
    uint64_t inPos = 0;
    std::vector<float> in(4096), out((size_t) frames);
    std::vector<float> all;
    const uint64_t periods = (uint64_t) (seconds * hostRate / frames);
    for (uint64_t p = 0; p < periods; ++p)
    {
        long produced = 0;
        int guard = 0;
        while (produced < frames && guard++ < 1000)
        {
            const long need = std::min<long>(conv.inputFor(frames - produced), (long) in.size());
            for (long i = 0; i < need; ++i)
                in[(size_t) i] = (float) chirp((double) (inPos + (uint64_t) i) / deviceRate, amp);
            long used = 0;
            produced += conv.process(in.data(), need, out.data() + produced, frames - produced, used);
            inPos += (uint64_t) used;
        }
        CHECK_EQ(produced, (long) frames);
        all.insert(all.end(), out.begin(), out.end());
    }
    // Input taken versus output made, at the exact ratio: within the filter's
    // own delay and a few frames, after any length of run.
    const double expectIn = (double) all.size() / conv.ratio();
    CHECK(std::fabs((double) inPos - expectIn) <= conv.leadFrames() + 8.0);

    // SNR against the chirp the host should hear, at the delay that fits best:
    // none, to within a fraction of a frame.
    double best = -1e9, bestDelay = 0;
    for (double delayOut = -4; delayOut <= 4; delayOut += 0.125)
    {
        double sig = 0, err = 0;
        for (size_t i = (size_t) hostRate; i < all.size(); i += 7)   // after the first second
        {
            const double want = chirp(((double) i - delayOut) / hostRate, amp);
            sig += want * want;
            err += (all[i] - want) * (all[i] - want);
        }
        const double snr = 10.0 * std::log10(sig / std::max(err, 1e-30));
        if (snr > best)
        {
            best = snr;
            bestDelay = delayOut;
        }
    }
    std::printf("  delay %.3f frames, lead %u input frames\n", bestDelay, conv.leadFrames());
    CHECK(std::fabs(bestDelay) <= 0.25);
    return best;
}

} // namespace

int main()
{
    positions();
    for (auto [device, host, frames] : { std::tuple { 48000u, 44100u, 128 }, std::tuple { 44100u, 48000u, 256 },
                                         std::tuple { 96000u, 48000u, 64 }, std::tuple { 48000u, 96000u, 512 } })
    {
        const double snr = pulled(device, host, frames, 5.0);
        std::printf("%u -> %u Hz, %d frames: %.1f dB\n", device, host, frames, snr);
        CHECK(snr > 60.0);
    }
    TEST_MAIN_END();
}
