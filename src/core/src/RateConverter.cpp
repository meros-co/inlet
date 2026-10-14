// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "inlet/RateConverter.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <samplerate.h>

namespace inlet
{

RateConverter::RateConverter(uint32_t inRate, uint32_t outRate, int channels, int quality) : chans(channels)
{
    if (inRate == 0 || outRate == 0 || channels <= 0)
        return;
    int err = 0;
    src = src_new(std::clamp(quality, 0, 2), channels, &err);
    outPerIn = (double) outRate / (double) inRate;
    if (src == nullptr)
        return;
    constexpr long kProbe = 8192;
    std::vector<float> in((size_t) kProbe * channels, 0.0f), out((size_t) kProbe * 4 * channels, 0.0f);

    // libsamplerate keeps the timeline -- output frame k is input frame
    // k / ratio -- and holds output back until it has the input its filter
    // looks ahead to. How far ahead that is (the look-ahead plus what it
    // buffers) is measured by running it as a driver does: small periods,
    // offered a little at a time.
    long ahead = 0;
    for (int period = 0; period < 400; ++period)
    {
        long produced = 0;
        for (int guard = 0; produced < 64 && guard < 1000; ++guard)
        {
            long used = 0;
            produced += process(in.data(), kMinOffer, out.data(), 64 - produced, used);
            ahead = std::max(ahead, (long) std::ceil((double) inTotal - (double) outTotal / outPerIn));
        }
    }
    lead = (uint32_t) ahead;
    reset();
}

RateConverter::~RateConverter()
{
    if (src != nullptr)
        src_delete(src);
}

void RateConverter::reset()
{
    if (src != nullptr)
        src_reset(src);
    inTotal = outTotal = 0;
}

long RateConverter::inputFor(long outFrames) const
{
    const double wanted = std::ceil((double) (outTotal + (uint64_t) outFrames) / outPerIn) + lead;
    return std::max(kMinOffer, (long) (wanted - (double) inTotal));
}

long RateConverter::process(const float* in, long inFrames, float* out, long outCap, long& used)
{
    used = 0;
    if (src == nullptr || outCap <= 0)
        return 0;
    SRC_DATA d {};
    d.data_in = in;
    d.input_frames = inFrames;
    d.data_out = out;
    d.output_frames = outCap;
    d.src_ratio = outPerIn;
    d.end_of_input = 0;
    if (src_process(src, &d) != 0)
        return 0;
    used = d.input_frames_used;
    inTotal += (uint64_t) used;
    outTotal += (uint64_t) d.output_frames_gen;
    return d.output_frames_gen;
}

uint64_t convertPosition(uint64_t sample, uint32_t fromRate, uint32_t toRate)
{
    if (fromRate == toRate || fromRate == 0)
        return sample;
    // Whole seconds and the remainder apart: sample x rate overflows 64 bits.
    return (sample / fromRate) * toRate + (sample % fromRate) * toRate / fromRate;
}

} // namespace inlet
