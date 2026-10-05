// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
//
// The ASIO driver, loaded the way a host loads it (but straight from the DLL,
// so nothing need be registered), against a shared region this test publishes
// in inletd's place. Audio in: a ramp in a receiver's ring reaches the host's
// input buffers at the right sample. Audio out: what the host writes reaches
// the output ring two periods on.

#include "check.h"
#include "inlet/Clock.h"
#include "inlet/SharedRegion.h"

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
#include <cstring>
#include <thread>
#include <vector>

#include <aes67/rtp.h>

using namespace inlet;

// {C228C42A-9291-4AEB-830B-16B7963F899E}
static const CLSID kClsid = { 0xc228c42a, 0x9291, 0x4aeb, { 0x83, 0x0b, 0x16, 0xb7, 0x96, 0x3f, 0x89, 0x9e } };

namespace
{
IASIO* gDriver = nullptr;
ASIOBufferInfo gBuffers[3];
std::atomic<int> gSwitches { 0 };
std::atomic<uint64_t> gFirstPos { 0 }, gLastPos { 0 };
std::atomic<bool> gPositionsContiguous { true };
std::atomic<int32_t> gInputSample { 0 };
std::atomic<uint64_t> gInputPos { 0 };
long gFrames = 0;

uint64_t join(const ASIOSamples& s) { return ((uint64_t) s.hi << 32) | s.lo; }

ASIOTime* onSwitchTimeInfo(ASIOTime* t, long index, ASIOBool)
{
    const uint64_t pos = join(t->timeInfo.samplePosition);
    const int n = gSwitches.fetch_add(1);
    if (n == 0)
        gFirstPos = pos;
    else if (pos != gLastPos.load() + (uint64_t) gFrames)
        gPositionsContiguous = false;
    gLastPos = pos;
    // Input 0, first frame of this period.
    gInputSample = static_cast<int32_t*>(gBuffers[0].buffers[index])[0];
    gInputPos = pos;
    // Output 0: the low bits of the period's position, every frame.
    auto* out = static_cast<int32_t*>(gBuffers[2].buffers[index]);
    for (long i = 0; i < gFrames; ++i)
        out[i] = (int32_t) (((pos + (uint64_t) i) & 0x7FFFF) << 8);
    gDriver->outputReady();
    return t;
}
void onSwitch(long, ASIOBool) {}
void onRate(ASIOSampleRate) {}
long onMessage(long selector, long value, void*, double*)
{
    if (selector == kAsioSelectorSupported)
        return value == kAsioSupportsTimeInfo ? 1 : 0;
    return selector == kAsioSupportsTimeInfo ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: test_asio_driver <path to inlet_asio.dll>\n");
        return 2;
    }

    // inletd's half: the region, a clock, a heartbeat, a ramp on receiver 1.
    std::string err;
    shm::Region::Layout layout;
    layout.inputs = 4;
    layout.outputs = 4;
    layout.receivers = 1;
    layout.receiverChannels = 2;
    auto region = shm::Region::create(layout, err);
    CHECK(region != nullptr);
    if (!region)
    {
        std::fprintf(stderr, "region: %s\n", err.c_str());
        TEST_MAIN_END();
    }
    std::strcpy(region->inputLabel(0), "Kick In");
    region->inputMap()[0].store(shm::mapCell(1, 0));
    region->header().linkOffsetFrames.store(96);
    ClockSample c;
    c.monoNs = monoNowNs();
    c.ptpNs = 1'780'000'000'000'000'000LL;
    region->clock().publish(c);
    std::atomic<bool> daemon { true };
    std::thread beat([&] {
        uint64_t written = sampleAt(c.ptpAt(monoNowNs()), 48000) - 4000;
        while (daemon)
        {
            region->header().daemonHeartbeatMono.store(monoNowNs());
            // The receive path: a ramp, written as the media clock passes.
            const uint64_t now = sampleAt(c.ptpAt(monoNowNs()), 48000);
            for (; written < now; ++written)
                aes67_ring_write(region->rxRings(1), (uint32_t) written, (int32_t) (written & 0x7FFFF));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // The host's half.
    HMODULE dll = LoadLibraryA(argv[1]);
    CHECK(dll != nullptr);
    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto getClass = dll ? reinterpret_cast<GetClassObject>(GetProcAddress(dll, "DllGetClassObject")) : nullptr;
    IClassFactory* factory = nullptr;
    CHECK(getClass && SUCCEEDED(getClass(kClsid, IID_IClassFactory, (void**) &factory)));
    if (factory)
        factory->CreateInstance(nullptr, kClsid, (void**) &gDriver);
    CHECK(gDriver != nullptr);
    if (gDriver)
    {
        CHECK(gDriver->init(nullptr) == ASIOTrue);
        char name[32];
        gDriver->getDriverName(name);
        CHECK(std::string(name) == "Inlet");
        long ins = 0, outs = 0;
        gDriver->getChannels(&ins, &outs);
        CHECK_EQ(ins, 4);
        CHECK_EQ(outs, 4);
        ASIOChannelInfo info {};
        info.channel = 0;
        info.isInput = ASIOTrue;
        gDriver->getChannelInfo(&info);
        CHECK(std::string(info.name) == "Kick In");
        CHECK_EQ(info.type, ASIOSTInt32LSB);
        CHECK(gDriver->canSampleRate(48000.0) == ASE_OK);
        CHECK(gDriver->canSampleRate(44100.0) != ASE_OK);

        long minB, maxB, pref, gran;
        gDriver->getBufferSize(&minB, &maxB, &pref, &gran);
        gFrames = pref;
        gBuffers[0] = { ASIOTrue, 0, { nullptr, nullptr } };
        gBuffers[1] = { ASIOTrue, 1, { nullptr, nullptr } };
        gBuffers[2] = { ASIOFalse, 0, { nullptr, nullptr } };
        ASIOCallbacks cb { onSwitch, onRate, onMessage, onSwitchTimeInfo };
        CHECK(gDriver->createBuffers(gBuffers, 3, gFrames, &cb) == ASE_OK);
        long inLat = 0, outLat = 0;
        gDriver->getLatencies(&inLat, &outLat);
        CHECK_EQ(inLat, gFrames + 96);
        CHECK_EQ(outLat, 2 * gFrames);

        CHECK(gDriver->start() == ASE_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(gDriver->stop() == ASE_OK);

        // ~187 switches of 128 frames in 500 ms, at consecutive positions on the media clock.
        std::printf("%d switches, first at %llu\n", gSwitches.load(), (unsigned long long) gFirstPos.load());
        CHECK(gSwitches.load() > 150);
        CHECK(gPositionsContiguous.load());
        // Input 0 at a period's first frame is the ramp at that position less the link offset.
        CHECK_EQ(gInputSample.load(), (int32_t) (((gInputPos.load() - 96) & 0x7FFFF) << 8));
        // The output ring holds what the host wrote, two periods on.
        const uint64_t lastOut = gLastPos.load() + 2 * (uint64_t) gFrames;
        int32_t v = 0;
        CHECK_EQ(aes67_ring_read(region->txRing(0), (uint32_t) lastOut, &v, 1), 1);
        CHECK_EQ(v, (int32_t) (gLastPos.load() & 0x7FFFF));
        // The client slot says who and how it went.
        CHECK(std::string(region->clients()[0].app).find("test_asio_driver") != std::string::npos);
        CHECK(region->clients()[0].periods.load() >= (uint64_t) gSwitches.load());

        gDriver->disposeBuffers();
        gDriver->Release();
    }
    if (factory)
        factory->Release();
    daemon = false;
    beat.join();
    TEST_MAIN_END();
}
