// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * Inlet's ASIO driver: an in-process COM object an ASIO host loads.
 *
 * It owns no network and no clock of its own. It attaches to inletd's shared
 * region (DeviceClient) and runs a PeriodEngine, which wakes at each period
 * boundary of the PTP media clock, so the host's buffer switches *are* the
 * network's clock: samplePosition is the media clock, and nothing drifts.
 *
 * Per period ending at device sample b (the period [b - frames, b)):
 *  - the input half for this switch is filled from the rings at
 *    (b - frames) - linkOffset;
 *  - the output half the host fills is played from b + frames (one more
 *    period than the double buffer itself, so a host that processes outside
 *    the callback still makes it); it is written to the rings when the host
 *    calls outputReady(), or at the next switch if it never does.
 *
 * Several hosts may load Inlet at once (each attaches its own client); the
 * first one running writes the outputs.
 */

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

namespace inlet
{
class DeviceClient;
class PeriodEngine;

// {C228C42A-9291-4AEB-830B-16B7963F899E}
inline constexpr CLSID kInletAsioClsid = { 0xc228c42a, 0x9291, 0x4aeb, { 0x83, 0x0b, 0x16, 0xb7, 0x96, 0x3f, 0x89, 0x9e } };

class InletAsio : public IASIO
{
public:
    InletAsio();
    virtual ~InletAsio();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IASIO
    ASIOBool init(void* sysHandle) override;
    void getDriverName(char* name) override;
    long getDriverVersion() override;
    void getErrorMessage(char* string) override;
    ASIOError start() override;
    ASIOError stop() override;
    ASIOError getChannels(long* numInputChannels, long* numOutputChannels) override;
    ASIOError getLatencies(long* inputLatency, long* outputLatency) override;
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) override;
    ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getSampleRate(ASIOSampleRate* sampleRate) override;
    ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override;
    ASIOError setClockSource(long reference) override;
    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;
    ASIOError getChannelInfo(ASIOChannelInfo* info) override;
    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) override;
    ASIOError disposeBuffers() override;
    ASIOError controlPanel() override;
    ASIOError future(long selector, void* opt) override;
    ASIOError outputReady() override;

    static constexpr long kMinBuffer = 32, kMaxBuffer = 2048, kPreferredBuffer = 128;

private:
    struct Channel
    {
        bool isInput = false;
        long index = 0;
        std::vector<int32_t> halves[2];
    };

    void onPeriod(uint64_t periodStart, bool late);
    void writeOutputHalf(int half, uint64_t firstSample);

    std::atomic<ULONG> refs { 1 };
    std::unique_ptr<DeviceClient> client;
    std::unique_ptr<PeriodEngine> engine;
    std::string error;

    std::vector<Channel> channels;
    long bufferFrames = 0;
    ASIOCallbacks* callbacks = nullptr;
    bool timeInfo = false;

    // Engine thread and host thread.
    std::atomic<int> half { 1 };                    // the half handed to the host last
    std::atomic<uint64_t> lastPeriodStart { 0 };
    std::atomic<int64_t> lastSwitchSystemNs { 0 };
    std::atomic<bool> hostUsesOutputReady { false };
    std::atomic<bool> pendingOutput { false };      // the last half handed out awaits writing
    std::atomic<bool> resetRequested { false };
    std::atomic<uint64_t> halfFirst[2] {};        // the device sample each half plays from
    uint64_t serial = 0;
};

} // namespace inlet
