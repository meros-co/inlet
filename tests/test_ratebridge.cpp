// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "check.h"
#include "inlet/RateBridge.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <random>
#include <vector>

#include <aes67/rtp.h>

using namespace inlet;

namespace
{

constexpr double kPi = 3.14159265358979323846;

struct Result
{
    RateBridgeStats st;
    uint64_t underrunsAfterSettle = 0;
    double snrDb = 0;
};

/**
 * A sender on its own clock, `ppm` off the device's, sending 1 kHz in 48-frame
 * packets that arrive up to a millisecond late; the bridge ticked every
 * millisecond of device time. Returns the bridge's state and the output's SNR
 * against a perfect sine at the frequency the device should hear.
 */
Result run(uint32_t inRate, uint32_t outRate, double ppm, double seconds)
{
    std::unique_ptr<aes67_ring_t[]> in(new aes67_ring_t[1]), out(new aes67_ring_t[1]);
    aes67_ring_init(&in[0]);
    aes67_ring_init(&out[0]);

    RateBridgeConfig cfg;
    cfg.inRate = inRate;
    cfg.outRate = outRate;
    cfg.channels = 1;
    cfg.in = in.get();
    cfg.out = out.get();
    cfg.targetFillFrames = inRate * 3 / 1000;
    cfg.leadFrames = outRate * 2 / 1000;
    RateBridge bridge(cfg);
    CHECK(bridge.valid());

    const double tone = 1000.0, amp = 0.5 * 8388607.0;
    const double senderRate = inRate * (1.0 + ppm * 1e-6);   // true frames per second
    const uint32_t ts0 = 0xFFFF0000u;                        // wraps early in the run
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> jitter(0.0, 1e-3);

    struct Packet { double arrival; uint64_t first; };
    std::deque<Packet> inFlight;
    uint64_t nextPacket = 0;
    const int P = (int) (inRate / 1000);

    Result r;
    const uint64_t ticks = (uint64_t) (seconds * 1000);
    for (uint64_t k = 1; k <= ticks; ++k)
    {
        const double t = k / 1000.0;
        // Packets the sender has finished by now, each a little late.
        while ((double) ((nextPacket + 1) * P) / senderRate <= t)
        {
            inFlight.push_back({ (double) ((nextPacket + 1) * P) / senderRate + jitter(rng), nextPacket * P });
            ++nextPacket;
        }
        for (auto it = inFlight.begin(); it != inFlight.end();)
        {
            if (it->arrival > t)
            {
                ++it;
                continue;
            }
            for (int f = 0; f < P; ++f)
            {
                const uint64_t n = it->first + (uint64_t) f;
                const double v = amp * std::sin(2 * kPi * tone * (double) n / inRate);
                aes67_ring_write(&in[0], ts0 + (uint32_t) n, (int32_t) std::lround(v));
            }
            it = inFlight.erase(it);
        }
        const uint64_t outNow = (uint64_t) std::llround(t * outRate);
        bridge.tick(outNow);
        if (k == (uint64_t) (seconds * 1000 * 2 / 3))
            r.underrunsAfterSettle = bridge.stats().underruns;
    }
    r.st = bridge.stats();
    r.underrunsAfterSettle = r.st.underruns - r.underrunsAfterSettle;

    // The last 20 ms the drivers would read, fitted to a sine at the frequency
    // the device hears: the tone on the sender's clock, in device time.
    const uint64_t end = (uint64_t) std::llround(seconds * outRate) - outRate / 100;
    const int N = (int) (outRate / 50);
    std::vector<int32_t> y((size_t) N);
    CHECK_EQ(aes67_ring_read(&out[0], (uint32_t) (end - (uint64_t) N), y.data(), N), N);
    // Distortion and noise, not frequency: a few ppm of residual frequency
    // error is inaudible but would swamp a fixed-frequency fit, so the fit
    // searches +-100 ppm around the expected tone.
    const double expected = tone * (1.0 + ppm * 1e-6);
    double sig = 0, noise = 1e300;
    for (int k = -100; k <= 100; ++k)
    {
        const double w = 2 * kPi * expected * (1.0 + k * 1e-6) / outRate;
        double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
        for (int i = 0; i < N; ++i)
        {
            const double s = std::sin(w * i), c = std::cos(w * i);
            ss += s * s;
            sc += s * c;
            cc += c * c;
            ys += y[(size_t) i] * s;
            yc += y[(size_t) i] * c;
        }
        const double det = ss * cc - sc * sc;
        const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
        double sg = 0, ns = 0;
        for (int i = 0; i < N; ++i)
        {
            const double fit = a * std::sin(w * i) + b * std::cos(w * i);
            sg += fit * fit;
            ns += (y[(size_t) i] - fit) * (y[(size_t) i] - fit);
        }
        if (ns < noise)
        {
            noise = ns;
            sig = sg;
        }
    }
    r.snrDb = 10 * std::log10(sig / std::max(noise, 1e-9));
    return r;
}

void expectLocked(const char* name, const Result& r, double ppm)
{
    std::printf("%-22s drift %+8.2f ppm  fill error %+6.1f frames  underruns %llu (%llu late)  resets %llu  warmup %u  SNR %.1f dB\n",
                name, r.st.driftPpm, r.st.fillErrorFrames, (unsigned long long) r.st.underruns,
                (unsigned long long) r.underrunsAfterSettle, (unsigned long long) r.st.resets, r.st.warmupFrames, r.snrDb);
    // The integrator has learned the clocks' difference (the sender running
    // fast means the device must consume faster: a positive correction).
    CHECK(r.st.tracking);
    CHECK_NEAR(r.st.driftPpm, ppm, 5.0);
    CHECK(std::fabs(r.st.fillErrorFrames) < 48.0);
    CHECK_EQ(r.underrunsAfterSettle, 0u);
    CHECK_EQ(r.st.resets, 0u);
    CHECK(r.snrDb > 80.0);
}

} // namespace

int main()
{
    expectLocked("48k, same rate", run(48000, 48000, 0.0, 90), 0.0);
    expectLocked("48k, sender +300 ppm", run(48000, 48000, 300.0, 90), 300.0);
    expectLocked("48k, sender -300 ppm", run(48000, 48000, -300.0, 90), -300.0);
    expectLocked("44.1k into 48k", run(44100, 48000, 0.0, 90), 0.0);
    TEST_MAIN_END();
}
