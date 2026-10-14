// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
//
// The ASIO driver, loaded the way a host loads it (but straight from the DLL,
// so nothing need be registered), against shared regions this test publishes
// in inletd's place.
//
//  1. At the network's rate: a ramp in a receiver's ring reaches the host's
//     input buffers at the right sample, and what the host writes reaches the
//     output ring two periods on.
//  2. A 44.1 kHz host on the 48 kHz network: the same, through the rate
//     converters, at the latency the driver reports.
//  3. inletd restarts with the same channels: the driver finds the new region
//     and carries on (a resync, no reset).
//  4. inletd restarts with other channels: the host is asked to reset.

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

#include <aes67/rtp.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace inlet;

// {C228C42A-9291-4AEB-830B-16B7963F899E}
static const CLSID kClsid = { 0xc228c42a, 0x9291, 0x4aeb, { 0x83, 0x0b, 0x16, 0xb7, 0x96, 0x3f, 0x89, 0x9e } };

namespace
{
IASIO* gDriver = nullptr;
ASIOBufferInfo gBuffers[3];
std::atomic<int> gSwitches { 0 }, gResyncs { 0 }, gResets { 0 };
std::atomic<uint64_t> gFirstPos { 0 }, gLastPos { 0 };
std::atomic<bool> gPositionsContiguous { true };
std::atomic<int32_t> gInputSample { 0 };
std::atomic<uint64_t> gInputPos { 0 };
std::atomic<double> gRate { 48000 };
long gFrames = 0;

uint64_t join(const ASIOSamples& s) { return ((uint64_t) s.hi << 32) | s.lo; }

/** The 48 kHz device sample at a 44.1 kHz host sample. */
uint64_t convertDevice(uint64_t host) { return host / 44100 * 48000 + host % 44100 * 48000 / 44100; }

void resetCounts()
{
    gSwitches = 0;
    gResyncs = 0;
    gResets = 0;
    gPositionsContiguous = true;
}

ASIOTime* onSwitchTimeInfo(ASIOTime* t, long index, ASIOBool)
{
    const uint64_t pos = join(t->timeInfo.samplePosition);
    gRate = t->timeInfo.sampleRate;
    const int n = gSwitches.fetch_add(1);
    if (n == 0)
        gFirstPos = pos;
    else if (pos != gLastPos.load() + (uint64_t) gFrames && gResyncs.load() == 0)
        gPositionsContiguous = false;
    gLastPos = pos;
    // Input 0, first frame of this period.
    gInputSample = static_cast<int32_t*>(gBuffers[0].buffers[index])[0];
    gInputPos = pos;
    // Output 0: a ramp of the host's position since the first switch.
    auto* out = static_cast<int32_t*>(gBuffers[2].buffers[index]);
    for (long i = 0; i < gFrames; ++i)
        out[i] = (int32_t) (((pos - gFirstPos.load() + (uint64_t) i) & 0x7FFFF) << 8);
    gDriver->outputReady();
    return t;
}
void onSwitch(long, ASIOBool) {}
void onRate(ASIOSampleRate) {}
long onMessage(long selector, long value, void*, double*)
{
    switch (selector)
    {
    case kAsioSelectorSupported:
        return value == kAsioSupportsTimeInfo || value == kAsioResyncRequest || value == kAsioResetRequest ? 1 : 0;
    case kAsioSupportsTimeInfo:
        return 1;
    case kAsioResyncRequest:
        gResyncs++;
        return 1;
    case kAsioResetRequest:
        gResets++;
        return 1;
    default:
        return 0;
    }
}

/** inletd's part: a region, its clock, its heartbeat, and a ramp of the
    device position (from `base`) on receiver 1 as the media clock passes. */
struct FakeDaemon
{
    std::unique_ptr<shm::Region> region;
    ClockSample clock;
    uint64_t base = 0;
    std::atomic<bool> alive { true };
    std::thread beat;

    FakeDaemon(shm::Directory& directory, const shm::Region::Layout& layout, const ClockSample& c, uint64_t rampBase)
        : clock(c), base(rampBase)
    {
        std::string err;
        const std::string name = shm::uniqueRegionName();
        region = shm::Region::create(layout, err, name.c_str());
        CHECK(region != nullptr);
        if (!region)
            return;
        std::strcpy(region->inputLabel(0), "Kick In");
        region->inputMap()[0].store(shm::mapCell(1, 0));
        region->header().linkOffsetFrames.store(96);
        region->clock().publish(clock);
        region->header().daemonHeartbeatMono.store(monoNowNs());
        directory.publish(name);
        beat = std::thread([this] {
            uint64_t written = sampleAt(clock.ptpAt(monoNowNs()), 48000) - 4000;
            while (alive)
            {
                region->header().daemonHeartbeatMono.store(monoNowNs());
                const uint64_t now = sampleAt(clock.ptpAt(monoNowNs()), 48000);
                for (; written < now; ++written)
                    aes67_ring_write(region->rxRings(1), (uint32_t) written, (int32_t) ((written - base) & 0x7FFFF));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    void stop()
    {
        alive = false;
        if (beat.joinable())
            beat.join();
    }
    ~FakeDaemon() { stop(); }
};

void createAndRun(int milliseconds)
{
    ASIOCallbacks cb { onSwitch, onRate, onMessage, onSwitchTimeInfo };
    gBuffers[0] = { ASIOTrue, 0, { nullptr, nullptr } };
    gBuffers[1] = { ASIOTrue, 1, { nullptr, nullptr } };
    gBuffers[2] = { ASIOFalse, 0, { nullptr, nullptr } };
    CHECK(gDriver->createBuffers(gBuffers, 3, gFrames, &cb) == ASE_OK);
    resetCounts();
    CHECK(gDriver->start() == ASE_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    CHECK(gDriver->stop() == ASE_OK);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: test_asio_driver <path to inlet_asio.dll>\n");
        return 2;
    }

    // As inletd does: the directory, then a region of its own name, published.
    // A directory of the test's own (the driver, loaded below, inherits the
    // name), so an Inlet installed on this computer is never the one found.
    const std::string directoryName = "Inlet.Directory.Test." + std::to_string(GetCurrentProcessId());
    SetEnvironmentVariableA("INLET_DIRECTORY", directoryName.c_str());
    std::string err;
    auto directory = shm::Directory::create(err);
    CHECK(directory != nullptr);
    if (!directory)
        TEST_MAIN_END();
    shm::Region::Layout layout;
    layout.inputs = 4;
    layout.outputs = 4;
    layout.receivers = 1;
    layout.receiverChannels = 2;
    ClockSample c;
    c.monoNs = monoNowNs();
    c.ptpNs = 1'780'000'000'000'000'000LL;
    const uint64_t rampBase = sampleAt(c.ptpAt(monoNowNs()), 48000) - 8000;
    auto daemon = std::make_unique<FakeDaemon>(*directory, layout, c, rampBase);
    shm::Region* region = daemon->region.get();
    if (!region)
        TEST_MAIN_END();

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
        CHECK(gDriver->canSampleRate(44100.0) == ASE_OK);
        CHECK(gDriver->canSampleRate(32000.0) != ASE_OK);

        long minB, maxB, pref, gran;
        gDriver->getBufferSize(&minB, &maxB, &pref, &gran);
        gFrames = pref;

        // 1. The network's rate.
        {
            CHECK(gDriver->setSampleRate(48000.0) == ASE_OK);
            ASIOCallbacks cb { onSwitch, onRate, onMessage, onSwitchTimeInfo };
            gBuffers[0] = { ASIOTrue, 0, { nullptr, nullptr } };
            gBuffers[1] = { ASIOTrue, 1, { nullptr, nullptr } };
            gBuffers[2] = { ASIOFalse, 0, { nullptr, nullptr } };
            CHECK(gDriver->createBuffers(gBuffers, 3, gFrames, &cb) == ASE_OK);
            long inLat = 0, outLat = 0;
            gDriver->getLatencies(&inLat, &outLat);
            CHECK_EQ(inLat, gFrames + 96);
            CHECK_EQ(outLat, 2 * gFrames);
            gDriver->disposeBuffers();
            createAndRun(500);

            // ~187 switches of 128 frames in 500 ms, at consecutive positions on the media clock.
            std::printf("48 kHz: %d switches, first at %llu\n", gSwitches.load(), (unsigned long long) gFirstPos.load());
            CHECK(gSwitches.load() > 150);
            CHECK(gPositionsContiguous.load());
            CHECK_EQ(gResyncs.load(), 0);
            // Input 0 at a period's first frame is the ramp at that position less the link offset.
            CHECK_EQ(gInputSample.load(), (int32_t) (((gInputPos.load() - 96 - rampBase) & 0x7FFFF) << 8));
            // The output ring holds what the host wrote, two periods on.
            const uint64_t lastOut = gLastPos.load() + 2 * (uint64_t) gFrames;
            int32_t v = 0;
            CHECK_EQ(aes67_ring_read(region->txRing(0, 0), (uint32_t) lastOut, &v, 1), 1);
            CHECK_EQ(v, (int32_t) ((gLastPos.load() - gFirstPos.load()) & 0x7FFFF));
            // The client slot says who and how it went.
            CHECK(std::string(region->clients()[0].app).find("test_asio_driver") != std::string::npos);
            CHECK(region->clients()[0].periods.load() >= (uint64_t) gSwitches.load());
            gDriver->disposeBuffers();
        }

        // 2. A 44.1 kHz host.
        {
            CHECK(gDriver->setSampleRate(44100.0) == ASE_OK);
            ASIOSampleRate r = 0;
            gDriver->getSampleRate(&r);
            CHECK_EQ(r, 44100.0);
            createAndRun(1000);
            long inLat = 0, outLat = 0;
            gDriver->getLatencies(&inLat, &outLat);
            std::printf("44.1 kHz: %d switches, latencies %ld in, %ld out\n", gSwitches.load(), inLat, outLat);
            CHECK(gSwitches.load() > 300);   // ~344 in a second
            CHECK(gPositionsContiguous.load());
            CHECK_EQ(gRate.load(), 44100.0);
            // Positions are the media clock at 44.1 kHz.
            const uint64_t expect = sampleAt(c.ptpAt(monoNowNs()), 44100);
            CHECK(gLastPos.load() < expect && expect - gLastPos.load() < 44100 / 10);
            // Input: the device ramp, converted. The ramp's value says which
            // device sample a host frame holds; how far that trails the host
            // frame's own time is the input latency, less a period.
            const double hostPos = (double) gInputPos.load();
            const double ramp = (double) (gInputSample.load() / 256) + (double) rampBase;
            const double behindHostFrames = (hostPos * 48000.0 / 44100.0 - ramp) * 44100.0 / 48000.0;
            std::printf("  input %.2f host frames behind (reported %ld)\n", behindHostFrames, inLat - gFrames);
            CHECK(std::fabs(behindHostFrames - (double) (inLat - gFrames)) <= 2.0);
            // Output: the host's ramp, converted to 48 kHz. A host frame plays
            // the reported output latency after its own time (two periods,
            // and the converter's look-ahead).
            const uint64_t at = convertDevice(gLastPos.load() - 4 * (uint64_t) gFrames);
            int32_t v[2] = {};
            CHECK_EQ(aes67_ring_read(region->txRing(0, 0), (uint32_t) at, v, 1), 1);
            CHECK_EQ(aes67_ring_read(region->txRing(0, 0), (uint32_t) (at + 480), v + 1, 1), 1);
            CHECK(std::abs((v[1] - v[0]) - 441) <= 1);   // 480 device frames are 441 host frames
            const double hostOfDevice = (double) at * 44100.0 / 48000.0;
            const double written = (double) v[0] + (double) gFirstPos.load();
            std::printf("  output %.2f host frames late (reported %ld)\n", hostOfDevice - written, outLat);
            CHECK(std::fabs(hostOfDevice - written - (double) outLat) <= 2.0);
            gDriver->disposeBuffers();
            CHECK(gDriver->setSampleRate(48000.0) == ASE_OK);
        }

        // 3. inletd restarts, same channels: the driver follows it.
        {
            ASIOCallbacks cb { onSwitch, onRate, onMessage, onSwitchTimeInfo };
            gBuffers[0] = { ASIOTrue, 0, { nullptr, nullptr } };
            gBuffers[1] = { ASIOTrue, 1, { nullptr, nullptr } };
            gBuffers[2] = { ASIOFalse, 0, { nullptr, nullptr } };
            CHECK(gDriver->createBuffers(gBuffers, 3, gFrames, &cb) == ASE_OK);
            resetCounts();
            CHECK(gDriver->start() == ASE_OK);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            daemon->stop();   // gone: the heartbeat stops
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            const int switchesWhileAway = gSwitches.load();
            auto restarted = std::make_unique<FakeDaemon>(*directory, layout, c, rampBase);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            CHECK(gDriver->stop() == ASE_OK);
            std::printf("restart: %d switches (%d before the new inletd), %d resyncs, %d resets\n", gSwitches.load(),
                        switchesWhileAway, gResyncs.load(), gResets.load());
            CHECK(switchesWhileAway > 400);   // kept running on silence
            CHECK_EQ(gResets.load(), 0);
            CHECK_EQ(gResyncs.load(), 1);
            shm::Region* now = restarted->region.get();
            CHECK(now != nullptr);
            if (now)
            {
                CHECK(std::string(now->clients()[0].app).find("test_asio_driver") != std::string::npos);
                CHECK(now->clients()[0].periods.load() > 100);
                // The host's input is the new inletd's ramp again.
                CHECK_EQ(gInputSample.load(), (int32_t) (((gInputPos.load() - 96 - rampBase) & 0x7FFFF) << 8));
            }
            gDriver->disposeBuffers();
            daemon = std::move(restarted);
            region = daemon->region.get();
        }

        // 4. inletd restarts with eight inputs: the host must reset.
        {
            ASIOCallbacks cb { onSwitch, onRate, onMessage, onSwitchTimeInfo };
            gBuffers[0] = { ASIOTrue, 0, { nullptr, nullptr } };
            gBuffers[1] = { ASIOTrue, 1, { nullptr, nullptr } };
            gBuffers[2] = { ASIOFalse, 0, { nullptr, nullptr } };
            CHECK(gDriver->createBuffers(gBuffers, 3, gFrames, &cb) == ASE_OK);
            resetCounts();
            CHECK(gDriver->start() == ASE_OK);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            daemon->stop();
            shm::Region::Layout wider = layout;
            wider.inputs = 8;
            auto other = std::make_unique<FakeDaemon>(*directory, wider, c, rampBase);
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            CHECK(gDriver->stop() == ASE_OK);
            std::printf("other layout: %d resets\n", gResets.load());
            CHECK_EQ(gResets.load(), 1);
            gDriver->disposeBuffers();
            daemon = std::move(other);
        }

        gDriver->Release();
    }
    if (factory)
        factory->Release();
    daemon.reset();
    TEST_MAIN_END();
}
