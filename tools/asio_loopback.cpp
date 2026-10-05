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
//   inlet-asio-loopback <inlet_asio.dll> [seconds] [buffer]

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
        return value == kAsioSupportsTimeInfo || value == kAsioResetRequest ? 1 : 0;
    if (selector == kAsioResetRequest)
    {
        std::fprintf(stderr, "the driver asked for a reset\n");
        return 1;
    }
    return selector == kAsioSupportsTimeInfo ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <inlet_asio.dll> [seconds] [buffer]\n", argv[0]);
        return 2;
    }
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 5;
    gFrames = argc > 3 ? std::atol(argv[3]) : 128;

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
    std::printf("buffer %ld, reported latency in %ld out %ld: expect a round trip of %ld samples\n", gFrames, inLat,
                outLat, outLat + (inLat - gFrames));
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
    for (auto& [lat, n] : gLatencies)
        std::printf("  round trip %u samples: %llu\n", lat, (unsigned long long) n);
    gDriver->Release();
    factory->Release();
    const bool ok = gLatencies.size() == 1 && gSilent.load() == 0 && gMatched.load() > 0;
    std::printf(ok ? "PASS: every sample came back, at one fixed latency\n" : "FAIL\n");
    return ok ? 0 : 1;
}
