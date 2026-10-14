// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
//
// inlet-asio-loopback: an ASIO host for testing Inlet end to end, through a
// running inletd and the network.
//
// It writes a ramp on outputs 1..N whose value is the host's own sample
// position (channel c offset by c), and reads inputs 1..N. With inletd's
// receiver 1 subscribed to its own sender 1 (multicast loops back), every
// input sample should be an output sample from a fixed number of samples
// before: two periods of output latency plus the link offset. It counts the
// samples that are, the ones that are not, and the silent ones.
//
// At another rate than the network's (the fourth argument), the audio goes
// through the driver's rate converters both ways, so a sample comes back as
// the ramp's value give or take a step, except near each of the ramp's wraps,
// which no converter passes cleanly and which are left out: there, it passes
// when every sample judged is within a sample of one round trip.
//
//   inlet-asio-loopback <inlet_asio.dll> [seconds] [buffer] [rate]
//   inlet-asio-loopback <inlet_asio.dll> --control-panel     (opens the Inlet app, as a host's ASIO button does)

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <map>
#include <thread>
#include <vector>

static const CLSID kClsid = { 0xc228c42a, 0x9291, 0x4aeb, { 0x83, 0x0b, 0x16, 0xb7, 0x96, 0x3f, 0x89, 0x9e } };

namespace
{
constexpr int kChannels = 8;
constexpr uint32_t kMask = 0x3FFFF;   // the ramp wraps at 2^18; plus one, it is never zero

IASIO* gDriver = nullptr;
std::vector<ASIOBufferInfo> gBuffers;
long gFrames = 128;
std::atomic<long> gSwitches { 0 };
std::atomic<uint64_t> gMatched { 0 }, gSilent { 0 }, gWrong { 0 };
std::map<uint32_t, uint64_t> gLatencies;   // switch thread only, read at the end
std::atomic<bool> gCounting { false };
bool gConverting = false;     // set before start
uint32_t gExpected = 0;       // the reported round trip, set before start
constexpr uint32_t kNearWrap = 2048;   // converting: samples this close to a ramp wrap are not judged
uint64_t gCountStart = 0, gFirstSilent = 0, gLastSilent = 0;   // switch thread only

uint64_t join(const ASIOSamples& s) { return ((uint64_t) s.hi << 32) | s.lo; }

ASIOTime* onSwitchTimeInfo(ASIOTime* t, long index, ASIOBool)
{
    const uint64_t pos = join(t->timeInfo.samplePosition);
    ++gSwitches;
    for (int c = 0; c < kChannels; ++c)
    {
        auto* in = static_cast<int32_t*>(gBuffers[(size_t) c].buffers[index]);
        auto* out = static_cast<int32_t*>(gBuffers[(size_t) (kChannels + c)].buffers[index]);
        for (long i = 0; i < gFrames; ++i)
        {
            const uint32_t here = (uint32_t) (pos + (uint64_t) i);
            out[i] = (int32_t) ((((here + (uint32_t) c * 1000) & kMask) + 1) << 8);
            if (!gCounting)
                continue;
            const uint32_t v = ((uint32_t) (in[i] >> 8) - 1) & kMask;
            if (gCountStart == 0)
                gCountStart = pos;
            if (in[i] == 0)
            {
                ++gSilent;
                if (gFirstSilent == 0)
                    gFirstSilent = pos + (uint64_t) i;
                gLastSilent = pos + (uint64_t) i;
            }
            else if (gConverting
                     && (((here + (uint32_t) c * 1000 - gExpected) & kMask) < kNearWrap
                         || ((here + (uint32_t) c * 1000 - gExpected) & kMask) > kMask - kNearWrap))
                continue;   // sent near the ramp's wrap, where the converters ring: not judged
            else
            {
                const uint32_t latency = (here + (uint32_t) c * 1000 - v) & kMask;
                ++gLatencies[latency];
                ++gMatched;
            }
        }
    }
    gDriver->outputReady();
    return t;
}
void onSwitch(long, ASIOBool) {}
void onRate(ASIOSampleRate) {}
long onMessage(long selector, long value, void*, double*)
{
    if (selector == kAsioSelectorSupported)
        return value == kAsioSupportsTimeInfo || value == kAsioResetRequest || value == kAsioResyncRequest ? 1 : 0;
    if (selector == kAsioResetRequest)
    {
        std::fprintf(stderr, "the driver asked for a reset\n");
        return 1;
    }
    if (selector == kAsioResyncRequest)
    {
        std::fprintf(stderr, "the driver resynced\n");
        return 1;
    }
    return selector == kAsioSupportsTimeInfo ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <inlet_asio.dll> [seconds] [buffer] [rate]\n", argv[0]);
        return 2;
    }
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 5;
    gFrames = argc > 3 ? std::atol(argv[3]) : 128;
    const double rate = argc > 4 ? std::atof(argv[4]) : 0.0;

    HMODULE dll = LoadLibraryA(argv[1]);
    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto getClass = dll ? reinterpret_cast<GetClassObject>(GetProcAddress(dll, "DllGetClassObject")) : nullptr;
    IClassFactory* factory = nullptr;
    if (!getClass || FAILED(getClass(kClsid, IID_IClassFactory, (void**) &factory)) || !factory
        || FAILED(factory->CreateInstance(nullptr, kClsid, (void**) &gDriver)))
    {
        std::fprintf(stderr, "could not load the driver from %s\n", argv[1]);
        return 1;
    }
    if (gDriver->init(nullptr) != ASIOTrue)
    {
        char msg[128];
        gDriver->getErrorMessage(msg);
        std::fprintf(stderr, "init failed: %s\n", msg);
        return 1;
    }
    // What a host's "ASIO settings" button does: open the Inlet app.
    if (argc > 2 && std::string(argv[2]) == "--control-panel")
    {
        const ASIOError e = gDriver->controlPanel();
        std::printf(e == ASE_OK ? "control panel opened\n" : "control panel failed (%ld)\n", (long) e);
        gDriver->Release();
        factory->Release();
        return e == ASE_OK ? 0 : 1;
    }
    ASIOSampleRate deviceRate = 0;
    gDriver->getSampleRate(&deviceRate);
    const bool converting = rate != 0.0 && rate != deviceRate;
    gConverting = converting;
    if (rate != 0.0 && gDriver->setSampleRate(rate) != ASE_OK)
    {
        std::fprintf(stderr, "the driver does not play %.0f Hz\n", rate);
        return 1;
    }
    for (int c = 0; c < kChannels; ++c)
        gBuffers.push_back({ ASIOTrue, c, { nullptr, nullptr } });
    for (int c = 0; c < kChannels; ++c)
        gBuffers.push_back({ ASIOFalse, c, { nullptr, nullptr } });
    ASIOCallbacks cb { onSwitch, onRate, onMessage, onSwitchTimeInfo };
    if (gDriver->createBuffers(gBuffers.data(), (long) gBuffers.size(), gFrames, &cb) != ASE_OK)
    {
        std::fprintf(stderr, "createBuffers failed\n");
        return 1;
    }
    long inLat = 0, outLat = 0;
    gDriver->getLatencies(&inLat, &outLat);
    ASIOSampleRate hostRate = 0;
    gDriver->getSampleRate(&hostRate);
    std::printf("%.0f Hz on a %.0f Hz network, buffer %ld, reported latency in %ld out %ld: expect a round trip of %ld samples\n",
                hostRate, deviceRate, gFrames, inLat, outLat, outLat + (inLat - gFrames));
    gExpected = (uint32_t) (outLat + (inLat - gFrames));
    gDriver->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));   // the loop fills
    gCounting = true;
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    gCounting = false;
    gDriver->stop();
    gDriver->disposeBuffers();

    std::printf("%ld switches; samples: %llu arrived, %llu silent\n", gSwitches.load(), (unsigned long long) gMatched.load(),
                (unsigned long long) gSilent.load());
    if (gSilent.load())
        std::printf("silent from sample %llu to %llu after counting began\n", (unsigned long long) (gFirstSilent - gCountStart),
                    (unsigned long long) (gLastSilent - gCountStart));
    gDriver->Release();
    factory->Release();
    if (!converting)
    {
        for (auto& [lat, n] : gLatencies)
            std::printf("  round trip %u samples: %llu\n", lat, (unsigned long long) n);
        const bool ok = gLatencies.size() == 1 && gSilent.load() == 0 && gMatched.load() > 0;
        std::printf(ok ? "PASS: every sample came back, at one fixed latency\n" : "FAIL\n");
        return ok ? 0 : 1;
    }
    // Converted: the round trip most samples show, and how many are within a
    // sample of it.
    uint32_t mode = 0;
    uint64_t most = 0, within = 0;
    for (auto& [lat, n] : gLatencies)
        if (n > most)
        {
            most = n;
            mode = lat;
        }
    for (auto& [lat, n] : gLatencies)
        if (lat + 1 >= mode && lat <= mode + 1)
            within += n;
    const double share = gMatched.load() ? (double) within / (double) gMatched.load() : 0.0;
    std::printf("  round trip %u samples (within one: %.4f%% of %llu)\n", mode, share * 100.0,
                (unsigned long long) gMatched.load());
    if (std::getenv("INLET_LOOPBACK_DETAIL") != nullptr)
        for (auto& [lat, n] : gLatencies)
            if (lat + 1 < mode || lat > mode + 1)
                std::printf("    off: round trip %u samples: %llu\n", lat, (unsigned long long) n);
    const bool ok = gSilent.load() == 0 && share > 0.9999;
    std::printf(ok ? "PASS: the samples came back through the converters, at one round trip\n" : "FAIL\n");
    return ok ? 0 : 1;
}
