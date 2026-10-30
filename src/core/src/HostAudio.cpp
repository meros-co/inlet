// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "inlet/HostAudio.h"
#include "inlet/RateConverter.h"

#include <algorithm>
#include <cmath>

namespace inlet
{

namespace
{
constexpr float kFrom24 = 1.0f / 8388608.0f;
/** Channels (both ways) x rate up to which the converters use medium quality. */
constexpr uint64_t kMediumQualityWork = 128ull * 48000;

int32_t toInt24(float v)
{
    return (int32_t) std::lrint(std::clamp((double) v, -1.0, 8388607.0 / 8388608.0) * 8388608.0);
}
} // namespace

int HostAudio::qualityFor(size_t totalChannels, uint32_t hostRate, uint32_t deviceRate)
{
    return (uint64_t) totalChannels * std::max(hostRate, deviceRate) <= kMediumQualityWork ? 1 : 2;
}

HostAudio::HostAudio(bool in, std::vector<int> channels, uint32_t host, uint32_t device, uint32_t maxFrames, int quality)
    : input(in), devChannels(std::move(channels)), hostRate(host), deviceRate(device)
{
    const int n = (int) devChannels.size();
    if (n == 0)
        return;
    if (!converting())
    {
        scratch.assign(maxFrames, 0);
        return;
    }
    if (input)
    {
        conv = std::make_unique<RateConverter>(deviceRate, hostRate, n, quality);
        // Device frames per offer: a period's worth at the ratio, and slack.
        const size_t cap = (size_t) std::ceil(maxFrames / conv->ratio()) + conv->leadFrames() + 64;
        interleavedIn.assign(cap * n, 0.0f);
        interleavedOut.assign((size_t) maxFrames * n, 0.0f);
        scratch.assign(cap, 0);
    }
    else
    {
        conv = std::make_unique<RateConverter>(hostRate, deviceRate, n, quality);
        const size_t cap = (size_t) std::ceil(maxFrames * conv->ratio()) + 64;
        interleavedIn.assign((size_t) maxFrames * n, 0.0f);
        interleavedOut.assign(cap * n, 0.0f);
        scratch.assign(std::max(cap, (size_t) maxFrames), 0);
    }
}

HostAudio::~HostAudio() = default;

uint32_t HostAudio::inputLatency(int32_t linkOffsetFrames) const
{
    if (!converting())
        return (uint32_t) linkOffsetFrames;
    const uint32_t lead = conv ? conv->leadFrames() : 0;
    return (uint32_t) std::ceil((double) ((uint32_t) linkOffsetFrames + lead) * hostRate / deviceRate);
}

uint32_t HostAudio::outputExtraLatency() const
{
    return converting() && conv ? conv->leadFrames() : 0;
}

void HostAudio::read(DeviceClient& c, uint64_t hostStart, int frames, float* const* out)
{
    const int n = (int) devChannels.size();
    if (!converting())
    {
        const uint64_t at = hostStart - (uint64_t) c.linkOffsetFrames();
        for (int k = 0; k < n; ++k)
        {
            if (out[k] == nullptr)
                continue;
            c.readInputRaw(at, devChannels[(size_t) k], scratch.data(), frames);
            for (int f = 0; f < frames; ++f)
                out[k][f] = (float) scratch[(size_t) f] * kFrom24;
        }
        return;
    }
    if (!conv)
        return;
    if (!anchored || hostStart != nextHost)
    {
        // A fresh stream: the device position of this period's first frame,
        // less the link offset, and less what the converter reads ahead.
        conv->reset();
        devicePos = convertPosition(hostStart, hostRate, deviceRate) - (uint64_t) c.linkOffsetFrames() - conv->leadFrames();
        anchored = true;
    }
    nextHost = hostStart + (uint64_t) frames;
    const long cap = (long) scratch.size();
    long produced = 0;
    for (int guard = 0; produced < frames && guard < 64; ++guard)
    {
        const long offer = std::min(conv->inputFor(frames - produced), cap);
        for (int k = 0; k < n; ++k)
        {
            c.readInputRaw(devicePos, devChannels[(size_t) k], scratch.data(), (int) offer);
            for (long f = 0; f < offer; ++f)
                interleavedIn[(size_t) (f * n + k)] = (float) scratch[(size_t) f] * kFrom24;
        }
        long used = 0;
        produced += conv->process(interleavedIn.data(), offer, interleavedOut.data() + produced * n, frames - produced, used);
        devicePos += (uint64_t) used;
    }
    for (int k = 0; k < n; ++k)
    {
        if (out[k] == nullptr)
            continue;
        for (long f = 0; f < produced; ++f)
            out[k][f] = interleavedOut[(size_t) (f * n + k)];
        std::fill(out[k] + produced, out[k] + frames, 0.0f);
    }
}

void HostAudio::write(DeviceClient& c, uint64_t hostFirst, int frames, const float* const* in)
{
    const int n = (int) devChannels.size();
    if (!converting())
    {
        for (int k = 0; k < n; ++k)
        {
            for (int f = 0; f < frames; ++f)
                scratch[(size_t) f] = in[k] != nullptr ? toInt24(in[k][f]) : 0;
            c.writeOutputRaw(hostFirst, devChannels[(size_t) k], scratch.data(), frames);
        }
        return;
    }
    if (!conv)
        return;
    if (!anchored || hostFirst != nextHost)
    {
        // A fresh stream, played from this period's first frame plus what the
        // converter holds back for its look-ahead.
        conv->reset();
        devicePos = convertPosition(hostFirst, hostRate, deviceRate) + (uint64_t) std::ceil(conv->leadFrames() * conv->ratio());
        anchored = true;
    }
    nextHost = hostFirst + (uint64_t) frames;
    for (int k = 0; k < n; ++k)
        for (int f = 0; f < frames; ++f)
            interleavedIn[(size_t) (f * n + k)] = in[k] != nullptr ? in[k][f] : 0.0f;
    const long cap = (long) (interleavedOut.size() / (size_t) n);
    long consumed = 0;
    for (int guard = 0; consumed < frames && guard < 64; ++guard)
    {
        long used = 0;
        const long made = conv->process(interleavedIn.data() + consumed * n, frames - consumed, interleavedOut.data(), cap, used);
        consumed += used;
        for (int k = 0; k < n; ++k)
        {
            for (long f = 0; f < made; ++f)
                scratch[(size_t) f] = toInt24(interleavedOut[(size_t) (f * n + k)]);
            c.writeOutputRaw(devicePos, devChannels[(size_t) k], scratch.data(), (int) made);
        }
        devicePos += (uint64_t) made;
        if (used == 0 && made == 0)
            break;
    }
}

} // namespace inlet
