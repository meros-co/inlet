// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "inlet/RateBridge.h"

#include <algorithm>
#include <cmath>

#include <samplerate.h>

// Last: libaes67's C atomics shim defines macros the standard headers trip on.
#include <aes67/rtp.h>

namespace inlet
{

namespace
{
constexpr int kMaxChunk = 2048;   // input frames per tick, at most
constexpr float kScale = 8388608.0f;
} // namespace

RateBridge::RateBridge(const RateBridgeConfig& c) : cfg(c)
{
    if (cfg.channels == 0 || cfg.in == nullptr || cfg.out == nullptr || cfg.inRate == 0 || cfg.outRate == 0)
        return;
    int err = 0;
    src = src_new(std::clamp(cfg.quality, 0, 2), cfg.channels, &err);
    nominal = (double) cfg.outRate / (double) cfg.inRate;
    scratch.resize(kMaxChunk);
    inBuf.resize((size_t) kMaxChunk * cfg.channels);
    outBuf.resize((size_t) kMaxChunk * 2 * cfg.channels);

    // How much input the resampler holds before its first output: its
    // filter's look-ahead. A fresh start gives it that on top of the target
    // fill, or it would drain the fill to prime itself and starve.
    if (src != nullptr)
    {
        std::fill(inBuf.begin(), inBuf.end(), 0.0f);
        SRC_DATA d {};
        d.data_in = inBuf.data();
        d.input_frames = kMaxChunk;
        d.data_out = outBuf.data();
        d.output_frames = kMaxChunk;
        d.src_ratio = nominal;
        if (src_process(src, &d) == 0)
            warmup = (uint32_t) std::max(0.0, (double) d.input_frames_used - (double) d.output_frames_gen / nominal);
        src_reset(src);
        st.warmupFrames = warmup;
    }
}

RateBridge::~RateBridge()
{
    if (src != nullptr)
        src_delete(src);
}

void RateBridge::reset(uint32_t head, uint64_t outNow)
{
    src_reset(src);
    inPos = head - cfg.targetFillFrames - warmup;
    outNext = outNow;
    windowSum = 0;
    windowCount = 0;
    windowStart = outNow;
    primed = false;
    inBandS = 0;
    st.tracking = false;
    if (started)
        st.resets++;
    started = true;
    st.running = true;
}

void RateBridge::tick(uint64_t outNow)
{
    if (src == nullptr)
        return;
    const uint32_t head = aes67_ring_head(&cfg.in[0]);
    if (head == 0)
        return;   // nothing has arrived
    if (!started)
        reset(head, outNow);

    // The source jumped (a sender restarted), or the output fell out of the
    // window it can still write: start again rather than play nonsense.
    const int32_t fill = (int32_t) (head - inPos);
    if (fill < -(int32_t) AES67_RING_FRAMES / 2 || fill > (int32_t) AES67_RING_FRAMES - kMaxChunk
        || outNext + AES67_RING_FRAMES / 2 < outNow)
        reset(head, outNow);

    const uint64_t target = outNow + cfg.leadFrames;
    while (outNext < target)
    {
        const int want = (int) std::min<uint64_t>(target - outNext, (uint64_t) kMaxChunk);
        // Only what this output needs (and a little over): libsamplerate takes
        // all it is given into its own buffer, which would hide the fill the
        // servo steers by.
        const double ratio = nominal * (1.0 - correction);
        const int32_t avail = (int32_t) (head - inPos);
        const int need = (int) std::ceil(want / ratio) + 4;
        const int take = std::clamp(std::min(avail, need), 0, kMaxChunk);
        for (int ch = 0; ch < cfg.channels; ++ch)
        {
            // Frames missing inside the window (a lost packet) read as zero:
            // the concealment the rest of Inlet uses.
            if (take > 0)
                aes67_ring_read(&cfg.in[ch], inPos, scratch.data(), take);
            for (int f = 0; f < take; ++f)
                inBuf[(size_t) (f * cfg.channels + ch)] = (float) scratch[(size_t) f] / kScale;
        }

        SRC_DATA d {};
        d.data_in = inBuf.data();
        d.input_frames = take;
        d.data_out = outBuf.data();
        d.output_frames = want;
        d.src_ratio = ratio;
        d.end_of_input = 0;
        if (src_process(src, &d) != 0)
        {
            reset(head, outNow);
            return;
        }
        inPos += (uint32_t) d.input_frames_used;
        for (long f = 0; f < d.output_frames_gen; ++f)
            for (int ch = 0; ch < cfg.channels; ++ch)
            {
                const float v = std::clamp(outBuf[(size_t) (f * cfg.channels + ch)] * kScale, -kScale, kScale - 1.0f);
                aes67_ring_write(&cfg.out[ch], (uint32_t) (outNext + (uint64_t) f), (int32_t) std::lrintf(v));
            }
        outNext += (uint64_t) d.output_frames_gen;
        if (d.output_frames_gen < want)
        {
            // The input ran dry; the drivers read silence past here. (Not
            // counted while the resampler's filter is still filling.)
            if (primed)
                st.underruns++;
            break;
        }
        primed = true;
    }

    // The servo, on the window's mean fill.
    windowSum += (double) (int32_t) (head - inPos);
    windowCount++;
    const double windowFrames = kWindowS * cfg.outRate;
    if ((double) (outNow - windowStart) >= windowFrames)
    {
        const double T = (double) (outNow - windowStart) / cfg.outRate;
        const double fillErr = windowSum / (double) windowCount - cfg.targetFillFrames;
        const double errS = fillErr / cfg.inRate;
        inBandS = std::fabs(errS) <= kBandS ? inBandS + T : 0.0;
        if (inBandS >= kTrackAfterS)
            st.tracking = true;
        const double kp = st.tracking ? kKp / kTrackDivide : kKp;
        const double ki = st.tracking ? kKi / (kTrackDivide * kTrackDivide) : kKi;
        integrator = std::clamp(integrator + ki * errS * T, -kMaxCorrection, kMaxCorrection);
        correction = std::clamp(kp * errS + integrator, -kMaxCorrection, kMaxCorrection);
        st.fillErrorFrames = fillErr;
        st.correctionPpm = correction * 1e6;
        st.driftPpm = integrator * 1e6;
        windowSum = 0;
        windowCount = 0;
        windowStart = outNow;
    }
}

} // namespace inlet
