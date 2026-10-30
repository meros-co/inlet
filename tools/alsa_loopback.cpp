// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
//
// inlet-alsa-loopback: an ALSA program for testing Inlet end to end, through
// a running inletd and the network, as inlet-asio-loopback does on Windows.
//
// It plays a ramp on channels 1..8 whose value is the frame's own position
// (channel c offset by c), and records channels 1..8. With inletd's receiver 1
// playing its own sender 1, every recorded frame should be a played one from a
// fixed number of frames before. At the network's rate that holds for every
// frame; at another rate, through the converters, every frame judged must be
// within a frame of it (those sent near the ramp's wraps, where converters
// ring, are left out).
//
//   inlet-alsa-loopback [device] [seconds] [period] [rate]     (default: inlet 10 256 48000)

#include <alsa/asoundlib.h>

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr int kChannels = 8;
constexpr uint32_t kMask = 0x3FFFF;     // the ramp wraps at 2^18; plus one, it is never zero
constexpr uint32_t kNearWrap = 2048;

snd_pcm_t* open(const char* device, snd_pcm_stream_t stream, unsigned rate, snd_pcm_uframes_t period)
{
    snd_pcm_t* pcm = nullptr;
    int e = snd_pcm_open(&pcm, device, stream, 0);
    if (e < 0)
    {
        std::fprintf(stderr, "%s: %s\n", device, snd_strerror(e));
        return nullptr;
    }
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_uframes_t buffer = period * 4;
    if ((e = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0
        || (e = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE)) < 0
        || (e = snd_pcm_hw_params_set_channels(pcm, hw, kChannels)) < 0
        || (e = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0)) < 0
        || (e = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr)) < 0
        || (e = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0 || (e = snd_pcm_hw_params(pcm, hw)) < 0)
    {
        std::fprintf(stderr, "%s: hw params: %s\n", device, snd_strerror(e));
        snd_pcm_close(pcm);
        return nullptr;
    }
    return pcm;
}
} // namespace

int main(int argc, char** argv)
{
    const char* device = argc > 1 ? argv[1] : "inlet";
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 10;
    const snd_pcm_uframes_t period = argc > 3 ? (snd_pcm_uframes_t) std::atol(argv[3]) : 256;
    const unsigned rate = argc > 4 ? (unsigned) std::atoi(argv[4]) : 48000;

    snd_pcm_t* play = open(device, SND_PCM_STREAM_PLAYBACK, rate, period);
    snd_pcm_t* rec = open(device, SND_PCM_STREAM_CAPTURE, rate, period);
    if (!play || !rec)
        return 1;
    snd_pcm_sframes_t playDelay = 0, recDelay = 0;

    std::atomic<bool> running { true };
    std::atomic<uint64_t> playErrors { 0 };
    std::thread writer([&] {
        std::vector<int32_t> buf(period * kChannels);
        uint64_t pos = 0;
        while (running)
        {
            for (snd_pcm_uframes_t f = 0; f < period; ++f, ++pos)
                for (int c = 0; c < kChannels; ++c)
                    buf[f * kChannels + c] = (int32_t) ((((uint32_t) pos + (uint32_t) c * 1000) & kMask) + 1) << 8;
            // All of the period, in as many writes as it takes.
            for (snd_pcm_uframes_t done = 0; done < period && running;)
            {
                snd_pcm_sframes_t n = snd_pcm_writei(play, buf.data() + done * kChannels, period - done);
                if (n < 0)
                {
                    ++playErrors;
                    snd_pcm_recover(play, (int) n, 1);
                    continue;
                }
                done += (snd_pcm_uframes_t) n;
            }
        }
    });

    std::vector<int32_t> buf(period * kChannels);
    uint64_t pos = 0, silent = 0, judged = 0, skipped = 0, firstSilent = 0, lastSilent = 0, recErrors = 0;
    std::map<uint32_t, uint64_t> trips;   // (recorded position - played position) mod the ramp
    std::map<uint32_t, uint64_t> learning;
    uint32_t learned = UINT32_MAX;
    const uint64_t warmup = rate;          // the first second fills the loop
    const uint64_t total = (uint64_t) seconds * rate + warmup;
    snd_pcm_start(rec);
    while (pos < total)
    {
        snd_pcm_sframes_t n = snd_pcm_readi(rec, buf.data(), period);
        if (n < 0)
        {
            ++recErrors;
            snd_pcm_recover(rec, (int) n, 1);
            continue;
        }
        for (snd_pcm_sframes_t f = 0; f < n; ++f, ++pos)
        {
            if (pos < warmup)
                continue;
            for (int c = 0; c < kChannels; ++c)
            {
                const int32_t v = buf[(size_t) f * kChannels + c];
                if (v == 0)
                {
                    if (silent++ == 0)
                        firstSilent = pos - warmup;
                    lastSilent = pos - warmup;
                    continue;
                }
                const uint32_t played = ((uint32_t) (v >> 8) - 1) & kMask;
                const uint32_t trip = ((uint32_t) pos + (uint32_t) c * 1000 - played) & kMask;
                // The offset, learned from the first half second's samples
                // away from the wraps. After that a sample is judged by where
                // it should have come from: one sent near a wrap rings in the
                // converters into any value at all, so it is left out.
                if (pos < warmup + rate / 2)
                {
                    if (played >= kNearWrap && played <= kMask - kNearWrap)
                        ++learning[trip];
                    continue;
                }
                if (learned == UINT32_MAX)
                {
                    uint64_t most = 0;
                    for (auto& [t, k] : learning)
                        if (k > most)
                        {
                            most = k;
                            learned = t;
                        }
                }
                const uint32_t source = ((uint32_t) pos + (uint32_t) c * 1000 - learned) & kMask;
                if (source < kNearWrap || source > kMask - kNearWrap)
                {
                    ++skipped;
                    continue;
                }
                ++trips[trip];
                ++judged;
            }
        }
    }
    snd_pcm_delay(play, &playDelay);
    snd_pcm_delay(rec, &recDelay);
    running = false;
    writer.join();
    snd_pcm_close(play);
    snd_pcm_close(rec);

    uint32_t mode = 0;
    uint64_t most = 0, within = 0, exact = 0;
    for (auto& [t, k] : trips)
        if (k > most)
        {
            most = k;
            mode = t;
        }
    for (auto& [t, k] : trips)
    {
        if (t + 1 >= mode && t <= mode + 1)
            within += k;
        if (t == mode)
            exact += k;
    }
    std::printf("%s at %u Hz, period %lu: %llu samples judged, %llu silent, %llu near wraps\n", device, rate,
                (unsigned long) period, (unsigned long long) judged, (unsigned long long) silent,
                (unsigned long long) skipped);
    if (silent)
        std::printf("  silent from frame %llu to %llu\n", (unsigned long long) firstSilent, (unsigned long long) lastSilent);
    if (playErrors || recErrors)
        std::printf("  %llu playback and %llu capture errors (underruns, overruns)\n", (unsigned long long) playErrors.load(),
                    (unsigned long long) recErrors);
    if (std::getenv("INLET_LOOPBACK_DETAIL") != nullptr)
    {
        int shown = 0;
        for (auto& [t, k] : trips)
            if ((t + 1 < mode || t > mode + 1) && shown++ < 20)
                std::printf("    off: %u frames: %llu\n", t, (unsigned long long) k);
    }
    std::printf("  offset %u frames: %llu exactly, %llu within one (%.4f%%)\n", mode, (unsigned long long) exact,
                (unsigned long long) within, judged ? 100.0 * (double) within / (double) judged : 0.0);
    const bool ok = judged > 0 && silent == 0 && (double) within >= 0.9999 * (double) judged;
    std::printf(ok ? "PASS: the samples came back at one offset\n" : "FAIL\n");
    return ok ? 0 : 1;
}
