// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
//
// The Core Audio driver, loaded the way coreaudiod loads it (through the
// bundle's CFPlugIn factory, so its Info.plist is checked too), with this test
// as Core Audio's host and running IO cycles, against shared regions this test
// publishes in inletd's place.
//
//  1. The objects and properties Core Audio reads: one device, its streams,
//     the format, the rate, channel names from inletd's labels.
//  2. Timestamps on the media clock: a whole number of periods apart, at the
//     network's rate in host time.
//  3. A program doing IO: named in the region's client slot, writing outputs.
//     A ramp in a receiver's ring reaches the input buffer at the media-clock
//     sample (less the link offset); what the output buffer holds reaches the
//     output ring at its sample.
//  4. inletd restarts with the same channels: the driver attaches to the new
//     region by itself, and the timestamps' seed changes.
//  5. inletd restarts with other channels: the driver asks for a
//     configuration change, and afterwards has the new shape.

#include "check.h"
#include "inlet/Clock.h"
#include "inlet/SharedRegion.h"

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <unistd.h>

#include <aes67/rtp.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace inlet;

namespace
{

constexpr AudioObjectID kDevice = 2, kInStream = 3, kOutStream = 4;
constexpr uint32_t kRate = 48000;
constexpr int kLinkOffset = 96;

AudioServerPlugInDriverRef gDriver = nullptr;
std::atomic<int> gConfigRequests { 0 }, gRunningChanges { 0 };

/* ---- Core Audio's side ---- */

OSStatus onPropertiesChanged(AudioServerPlugInHostRef, AudioObjectID, UInt32 n, const AudioObjectPropertyAddress* a)
{
    for (UInt32 i = 0; i < n; ++i)
        if (a[i].mSelector == kAudioDevicePropertyDeviceIsRunning)
            gRunningChanges++;
    return 0;
}
OSStatus onCopy(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef* out)
{
    *out = nullptr;
    return kAudioHardwareUnspecifiedError;
}
OSStatus onWrite(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef) { return 0; }
OSStatus onDelete(AudioServerPlugInHostRef, CFStringRef) { return 0; }
OSStatus onConfigRequest(AudioServerPlugInHostRef, AudioObjectID, UInt64, void*)
{
    gConfigRequests++;
    return 0;
}
AudioServerPlugInHostInterface gHost { onPropertiesChanged, onCopy, onWrite, onDelete, onConfigRequest };

template <typename T> T get(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
                            UInt32 element = kAudioObjectPropertyElementMain, const void* q = nullptr, UInt32 qSize = 0)
{
    const AudioObjectPropertyAddress a { sel, scope, element };
    T v {};
    UInt32 size = 0;
    CHECK((*gDriver)->GetPropertyDataSize(gDriver, obj, 0, &a, qSize, q, &size) == 0);
    CHECK_EQ(size, (UInt32) sizeof(T));
    CHECK((*gDriver)->GetPropertyData(gDriver, obj, 0, &a, qSize, q, sizeof(T), &size, &v) == 0);
    return v;
}

std::vector<AudioObjectID> list(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    const AudioObjectPropertyAddress a { sel, scope, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    CHECK((*gDriver)->GetPropertyDataSize(gDriver, obj, 0, &a, 0, nullptr, &size) == 0);
    std::vector<AudioObjectID> out(size / sizeof(AudioObjectID));
    CHECK((*gDriver)->GetPropertyData(gDriver, obj, 0, &a, 0, nullptr, size, &size, out.data()) == 0);
    return out;
}

std::string str(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
                UInt32 element = kAudioObjectPropertyElementMain)
{
    CFStringRef s = get<CFStringRef>(obj, sel, scope, element);
    char buf[256] = {};
    if (s != nullptr)
        CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    return buf;
}

int64_t ticksToNs(uint64_t ticks)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (int64_t) ((__uint128_t) ticks * tb.numer / tb.denom);
}

/* ---- inletd's side ---- */

/** A region, its clock and heartbeat, and a ramp of the media-clock sample
    (from `base`) on receiver 1's first channel as the clock passes. */
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
        region->header().linkOffsetFrames.store(kLinkOffset);
        region->clock().publish(clock);
        region->header().daemonHeartbeatMono.store(monoNowNs());
        directory.publish(name);
        beat = std::thread([this] {
            uint64_t written = sampleAt(clock.ptpAt(monoNowNs()), kRate) - 4000;
            while (alive)
            {
                region->header().daemonHeartbeatMono.store(monoNowNs());
                // Written 100 ms ahead: the rings are timestamp-indexed, and a
                // sleep can overrun far past the link offset on a busy machine.
                const uint64_t now = sampleAt(clock.ptpAt(monoNowNs()), kRate) + kRate / 10;
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

    const shm::ClientSlot* coreAudioClient()
    {
        for (int i = 0; i < shm::kMaxClients; ++i)
        {
            const auto& s = region->clients()[i];
            if (s.state.load() == 2 && s.kind.load() == (uint32_t) shm::ClientKind::CoreAudio)
                return &s;
        }
        return nullptr;
    }
};

struct Stamp
{
    Float64 sample = 0;
    UInt64 host = 0, seed = 0;
};

Stamp zero()
{
    Stamp s;
    CHECK((*gDriver)->GetZeroTimeStamp(gDriver, kDevice, 0, &s.sample, &s.host, &s.seed) == 0);
    return s;
}

/** The media-clock sample at Core Audio sample time `t`, from the zero stamp:
    the stamp's host time is when its sample falls. */
uint64_t mediaSampleAt(const ClockSample& clk, const Stamp& z, Float64 t)
{
    const uint64_t zeroSample = sampleAt(clk.ptpAt(ticksToNs(z.host)) + 1, kRate);   // +1 ns: the stamp rounds up
    return zeroSample + (uint64_t) std::llround(t - z.sample);
}

/** One IO cycle as Core Audio runs it, `frames` long, at the current time. */
void cycle(uint32_t inputs, uint32_t outputs, UInt32 frames, std::vector<float>& in, const std::vector<float>& out,
           AudioServerPlugInIOCycleInfo& info)
{
    const Stamp z = zero();
    const double perFrameNs = 1e9 / kRate;
    const double now = z.sample + (double) (monoNowNs() - ticksToNs(z.host)) / perFrameNs;
    info = {};
    info.mNominalIOBufferFrameSize = frames;
    info.mInputTime.mSampleTime = std::floor(now) - frames - 16;
    info.mOutputTime.mSampleTime = std::floor(now) + 16;
    in.assign((size_t) frames * inputs, -2.0f);
    CHECK((*gDriver)->BeginIOOperation(gDriver, kDevice, 7, kAudioServerPlugInIOOperationCycle, frames, &info) == 0);
    if (inputs > 0)
        CHECK((*gDriver)->DoIOOperation(gDriver, kDevice, kInStream, 7, kAudioServerPlugInIOOperationReadInput, frames, &info, in.data(),
                                        nullptr) == 0);
    if (outputs > 0)
        CHECK((*gDriver)->DoIOOperation(gDriver, kDevice, kOutStream, 7, kAudioServerPlugInIOOperationWriteMix, frames, &info,
                                        const_cast<float*>(out.data()), nullptr) == 0);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: test_coreaudio_driver <path to Inlet.driver>\n");
        return 2;
    }

    // A directory of the test's own, which the driver loaded below inherits,
    // so an Inlet installed on this computer is never the one found.
    const std::string directoryName = "Inlet.Dir.T" + std::to_string(getpid());
    setenv("INLET_DIRECTORY", directoryName.c_str(), 1);
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
    ClockSample clk;
    clk.monoNs = monoNowNs();
    clk.ptpNs = 1'780'000'000'000'000'000LL;
    const uint64_t rampBase = sampleAt(clk.ptpAt(monoNowNs()), kRate) - 8000;
    auto daemon = std::make_unique<FakeDaemon>(*directory, layout, clk, rampBase);

    // Loaded as coreaudiod loads it: the bundle, its factory for the
    // AudioServerPlugIn type, the driver interface.
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*) argv[1], (CFIndex) std::strlen(argv[1]), true);
    CFPlugInRef plugIn = CFPlugInCreate(nullptr, url);
    CHECK(plugIn != nullptr);
    CFArrayRef factories = plugIn ? CFPlugInFindFactoriesForPlugInTypeInPlugIn(kAudioServerPlugInTypeUUID, plugIn) : nullptr;
    CHECK(factories != nullptr && CFArrayGetCount(factories) == 1);
    if (factories == nullptr || CFArrayGetCount(factories) != 1)
        TEST_MAIN_END();
    auto factory = (CFUUIDRef) CFArrayGetValueAtIndex(factories, 0);
    void* unknown = CFPlugInInstanceCreate(nullptr, factory, kAudioServerPlugInTypeUUID);
    CHECK(unknown != nullptr);
    if (unknown == nullptr)
        TEST_MAIN_END();
    auto* iunknown = static_cast<IUnknownVTbl**>(unknown);
    (*iunknown)->QueryInterface(unknown, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), (LPVOID*) &gDriver);
    CHECK(gDriver != nullptr);
    if (gDriver == nullptr)
        TEST_MAIN_END();
    CHECK((*gDriver)->Initialize(gDriver, &gHost) == 0);

    // 1. Objects and properties.
    {
        const auto devices = list(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList);
        CHECK(devices.size() == 1 && devices[0] == kDevice);
        CFStringRef uid = CFSTR("co.meros.inlet.device");
        CHECK_EQ(get<AudioObjectID>(kAudioObjectPlugInObject, kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain, &uid, sizeof uid),
                 kDevice);
        CHECK(str(kDevice, kAudioObjectPropertyName) == "Inlet");
        CHECK(str(kDevice, kAudioDevicePropertyDeviceUID) == "co.meros.inlet.device");
        CHECK(str(kDevice, kAudioDevicePropertyConfigurationApplication) == "co.meros.inlet");
        CHECK_EQ(get<Float64>(kDevice, kAudioDevicePropertyNominalSampleRate), (Float64) kRate);
        CHECK_EQ(get<UInt32>(kDevice, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput), (UInt32) kLinkOffset);
        const auto ins = list(kDevice, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput);
        const auto outs = list(kDevice, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
        CHECK(ins.size() == 1 && ins[0] == kInStream);
        CHECK(outs.size() == 1 && outs[0] == kOutStream);
        const auto f = get<AudioStreamBasicDescription>(kInStream, kAudioStreamPropertyVirtualFormat);
        CHECK_EQ(f.mChannelsPerFrame, 4u);
        CHECK_EQ(f.mSampleRate, (Float64) kRate);
        CHECK_EQ(get<UInt32>(kInStream, kAudioStreamPropertyDirection), 1u);
        CHECK_EQ(get<UInt32>(kOutStream, kAudioStreamPropertyDirection), 0u);
        CHECK(str(kDevice, kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, 1) == "Kick In");
        CHECK(str(kDevice, kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, 2) == "In 2");
        CHECK(str(kDevice, kAudioObjectPropertyElementName, kAudioObjectPropertyScopeOutput, 4) == "Out 4");
        const AudioObjectPropertyAddress rate { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                                kAudioObjectPropertyElementMain };
        const Float64 other = 44100;
        CHECK((*gDriver)->SetPropertyData(gDriver, kDevice, 0, &rate, 0, nullptr, sizeof other, &other) != 0);
    }

    // 3 (first half). A program starts IO.
    CFStringRef bundle = CFSTR("com.example.Player");
    AudioServerPlugInClientInfo client { 7, getpid(), true, bundle };
    CHECK((*gDriver)->AddDeviceClient(gDriver, kDevice, &client) == 0);
    CHECK((*gDriver)->StartIO(gDriver, kDevice, 7) == 0);
    CHECK_EQ(get<UInt32>(kDevice, kAudioDevicePropertyDeviceIsRunning), 1u);
    CHECK(gRunningChanges.load() >= 1);
    {
        const shm::ClientSlot* slot = daemon->coreAudioClient();
        CHECK(slot != nullptr);
        if (slot != nullptr)
        {
            CHECK(std::string(slot->app).find("test_coreaudio") == 0);
            CHECK_EQ(slot->running.load(), 1u);
            CHECK(daemon->region->header().outputLayers[0].load() != 0);
        }
    }

    // 2. Timestamps.
    {
        const Stamp a = zero();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const Stamp b = zero();
        CHECK_EQ(a.seed, b.seed);
        CHECK_EQ(std::fmod(a.sample, 2048.0), 0.0);
        CHECK(b.sample > a.sample);
        const double seconds = (double) (ticksToNs(b.host) - ticksToNs(a.host)) / 1e9;
        CHECK_NEAR(seconds, (b.sample - a.sample) / kRate, 1e-6);
    }

    // 3. Audio both ways.
    {
        const UInt32 frames = 256;
        std::vector<float> in, out((size_t) frames * 4, 0.0f);
        for (UInt32 i = 0; i < frames; ++i)
            out[(size_t) i * 4] = (float) (i + 1) / 8388608.0f;   // output 1: 1, 2, 3, ... in 24-bit steps
        AudioServerPlugInIOCycleInfo info;
        for (int n = 0; n < 5; ++n)
        {
            cycle(4, 4, frames, in, out, info);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const Stamp z = zero();
        // Input 1 is the ramp: the media-clock sample, less the link offset.
        const uint64_t inAt = mediaSampleAt(clk, z, info.mInputTime.mSampleTime);
        const int64_t got = std::llround(in[0] * 8388608.0);
        const int64_t want = (int64_t) ((inAt - kLinkOffset - rampBase) & 0x7FFFF);
        CHECK_NEAR((double) got, (double) want, 1.0);
        bool ramp = true, silent = true;
        for (UInt32 i = 1; i < frames; ++i)
        {
            ramp = ramp && std::llround(in[(size_t) i * 4] * 8388608.0) == got + i;
            silent = silent && in[(size_t) i * 4 + 1] == 0.0f;   // input 2: unmapped
        }
        CHECK(ramp);
        CHECK(silent);
        // Output 1 is in its ring from the output time's media-clock sample.
        const uint64_t outAt = mediaSampleAt(clk, z, info.mOutputTime.mSampleTime);
        int layer = -1;
        for (int l = 0; l < shm::kOutputLayers; ++l)
            if (daemon->region->header().outputLayers[l].load() != 0)
                layer = l;
        CHECK(layer >= 0);
        if (layer >= 0)
        {
            int32_t ring[frames + 2] = {};
            aes67_ring_read(daemon->region->txRing(layer, 0), (uint32_t) (outAt - 1), ring, (int) frames + 2);
            // Within a sample of the expected place (the stamp's rounding).
            const bool exact = ring[1] == 1 && ring[frames] == (int32_t) frames;
            const bool early = ring[0] == 1 && ring[frames - 1] == (int32_t) frames;
            const bool late = ring[2] == 1 && ring[frames + 1] == (int32_t) frames;
            CHECK(exact || early || late);
        }
        CHECK(daemon->coreAudioClient() != nullptr && daemon->coreAudioClient()->periods.load() >= 5);
    }

    // 4. inletd restarts with the same channels.
    {
        const Stamp before = zero();
        daemon.reset();
        daemon = std::make_unique<FakeDaemon>(*directory, layout, clk, rampBase);
        // The old region's heartbeat goes stale after a second; the driver
        // looks four times a second.
        std::this_thread::sleep_for(std::chrono::milliseconds(1800));
        const Stamp after = zero();
        CHECK(after.seed != before.seed);
        CHECK(after.sample >= before.sample);
        const shm::ClientSlot* slot = daemon->coreAudioClient();
        CHECK(slot != nullptr && slot->running.load() == 1);
        CHECK(daemon->region->header().outputLayers[0].load() != 0);
        CHECK_EQ(gConfigRequests.load(), 0);
    }

    // 5. inletd restarts with other channels.
    {
        shm::Region::Layout wide = layout;
        wide.inputs = 8;
        wide.outputs = 2;
        daemon.reset();
        daemon = std::make_unique<FakeDaemon>(*directory, wide, clk, rampBase);
        std::this_thread::sleep_for(std::chrono::milliseconds(1800));
        CHECK_EQ(gConfigRequests.load(), 1);
        // Core Audio stops IO, then has the device change.
        CHECK((*gDriver)->StopIO(gDriver, kDevice, 7) == 0);
        CHECK((*gDriver)->PerformDeviceConfigurationChange(gDriver, kDevice, 0, nullptr) == 0);
        CHECK_EQ(get<AudioStreamBasicDescription>(kInStream, kAudioStreamPropertyVirtualFormat).mChannelsPerFrame, 8u);
        CHECK_EQ(get<AudioStreamBasicDescription>(kOutStream, kAudioStreamPropertyVirtualFormat).mChannelsPerFrame, 2u);
        CHECK((*gDriver)->StartIO(gDriver, kDevice, 7) == 0);
        std::vector<float> in, out(256 * 2, 0.0f);
        AudioServerPlugInIOCycleInfo info;
        cycle(8, 2, 256, in, out, info);
        CHECK(in[0] != -2.0f);
        CHECK((*gDriver)->StopIO(gDriver, kDevice, 7) == 0);
        CHECK_EQ(get<UInt32>(kDevice, kAudioDevicePropertyDeviceIsRunning), 0u);
        const shm::ClientSlot* slot = daemon->coreAudioClient();
        CHECK(slot != nullptr && slot->running.load() == 0);
    }

    CHECK((*gDriver)->RemoveDeviceClient(gDriver, kDevice, &client) == 0);
    daemon.reset();
    TEST_MAIN_END();
}
