// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * One direction of a host's audio, between its buffers and Inlet's rings: what
 * every driver does once a period, whatever its API (ASIO, ALSA, PipeWire).
 *
 * The host runs at its own rate, on the PTP media clock (its periods are
 * counted at its rate by a PeriodEngine, or by its sound server). At the
 * device's rate the audio goes straight to and from the rings; at another, a
 * RateConverter each way moves it, at the exact ratio of the two rates on the
 * same clock, so nothing drifts. Positions are kept from an anchor and carried
 * on period after period; a period that does not follow the last (a resync,
 * a restart) re-anchors.
 *
 * Inputs: the host gets the period [hostStart, hostStart + frames) as
 * received, which is the network's audio from the link offset before (and,
 * converting, the converter's look-ahead). Outputs: the host's period plays
 * from `hostFirst` (the driver chooses how far ahead; ASIO plays two periods
 * on), plus the converter's look-ahead when converting.
 *
 * Samples are floats in [-1, 1). Real-time safe after construction.
 */

#include "inlet/DeviceClient.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace inlet
{

class RateConverter;

class HostAudio
{
public:
    /** `deviceChannels`: the device inputs (or outputs) the host's channels
        are, in order. `maxFrames`: the longest period. `quality`: the
        converter's (0 best, 1 medium, 2 fastest). */
    HostAudio(bool input, std::vector<int> deviceChannels, uint32_t hostRate, uint32_t deviceRate, uint32_t maxFrames,
              int quality = 1);
    ~HostAudio();
    HostAudio(const HostAudio&) = delete;
    HostAudio& operator=(const HostAudio&) = delete;

    bool converting() const { return hostRate != deviceRate; }
    uint32_t rate() const { return hostRate; }
    int channels() const { return (int) devChannels.size(); }

    /** Inputs: `frames` host frames of each channel, out[k] for the k-th
        device channel (null: not wanted). */
    void read(DeviceClient& client, uint64_t hostStart, int frames, float* const* out);
    /** Outputs: `frames` host frames to play from host sample `hostFirst`
        (null channel: silence). Ignored unless the client writes outputs. */
    void write(DeviceClient& client, uint64_t hostFirst, int frames, const float* const* in);

    /** How far behind the host's own time its inputs are, in host frames:
        the link offset, and converting, the converter's look-ahead. */
    uint32_t inputLatency(int32_t linkOffsetFrames) const;
    /** What converting adds to the output's latency, in host frames. */
    uint32_t outputExtraLatency() const;

    /** The next period starts a fresh stream (after a resync or a restart). */
    void reset() { anchored = false; }

    /** The conversion quality worth spending on this many channels at these
        rates: medium, or the fastest sinc when medium would take a good part
        of a core (about 64 channels each way at 48 kHz, or 16 at 192). */
    static int qualityFor(size_t totalChannels, uint32_t hostRate, uint32_t deviceRate);

private:
    bool input;
    std::vector<int> devChannels;
    uint32_t hostRate, deviceRate;
    std::unique_ptr<RateConverter> conv;
    std::vector<float> interleavedIn, interleavedOut;
    std::vector<int32_t> scratch;
    uint64_t devicePos = 0;   // next device frame read or written
    uint64_t nextHost = 0;    // the host sample the stream continues from
    bool anchored = false;
};

} // namespace inlet
