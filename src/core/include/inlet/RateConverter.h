// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * A fixed-ratio sample-rate converter, for a host running at another rate than
 * the network (44.1 kHz in a DAW, 48 kHz on the wire).
 *
 * Both sides run on the same clock -- the host's periods are scheduled on the
 * PTP media clock at the host's rate -- so the ratio is exact and constant: no
 * servo, unlike RateBridge, which moves audio between two clocks. The driver
 * keeps the input and output positions itself, contiguous from an anchor, so
 * nothing drifts however long it runs.
 *
 * libsamplerate's band-limited sinc, interleaved floats in [-1, 1).
 * Real-time safe after construction: process() takes no locks and allocates
 * nothing.
 */

#include <cstdint>

struct SRC_STATE_tag;

namespace inlet
{

class RateConverter
{
public:
    RateConverter(uint32_t inRate, uint32_t outRate, int channels, int quality = 1);
    ~RateConverter();
    RateConverter(const RateConverter&) = delete;
    RateConverter& operator=(const RateConverter&) = delete;

    bool valid() const { return src != nullptr; }
    double ratio() const { return outPerIn; }
    int channels() const { return chans; }

    /** Output frame k is input frame k / ratio, but the converter reads its
        input up to this many frames further ahead: a driver anchors its input
        this far back (and reports it as latency). */
    uint32_t leadFrames() const { return lead; }

    /** Forgets everything: the next process() starts a fresh stream. */
    void reset();

    /** How many more input frames to offer for `outFrames` more output:
        what the ratio needs since the last reset plus the lead, and never
        fewer than a few. Offering more would let the converter buffer it,
        which reads the input further ahead than the output asks for. */
    long inputFor(long outFrames) const;

    /** Converts up to `inFrames` of `in` into at most `outCap` frames of
        `out`. Returns the frames written; `used` gets the input taken. */
    long process(const float* in, long inFrames, float* out, long outCap, long& used);

private:
    SRC_STATE_tag* src = nullptr;
    double outPerIn = 1.0;
    int chans = 0;
    uint32_t lead = 0;
    uint64_t inTotal = 0, outTotal = 0;   // since the last reset
    static constexpr long kMinOffer = 8;
};

/** The device sample at host sample `hostSample` (both counted from the PTP
    epoch, at their own rates), rounded down. Exact in 64 bits. */
uint64_t convertPosition(uint64_t sample, uint32_t fromRate, uint32_t toRate);

} // namespace inlet
