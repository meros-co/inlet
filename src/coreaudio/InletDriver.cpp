// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
//
// Inlet's Core Audio driver: an AudioServerPlugIn with one device, "Inlet",
// that every application on the Mac can use. Like the ASIO driver it is a
// client of inletd: it attaches to the shared region and moves audio between
// Core Audio's buffers and the rings, on the media clock.
//
// The objects:
//   1  the plug-in
//   2  the device: inputs and outputs as inletd is configured (64 x 64 at
//      48 kHz unless set otherwise), at the network's rate only -- Core Audio
//      converts for applications that run at another
//   3  its input stream, 4  its output stream (32-bit float, interleaved)
//
// Timing. The device's sample counter is the media clock, as for ASIO: Core
// Audio sample time t is media-clock sample `anchor + t`. GetZeroTimeStamp
// gives the media clock's latest period boundary in host time, through
// inletd's published PTP mapping, so Core Audio's IO runs on the network's
// clock and needs no resampling against it. Inputs are read at the input time
// less the link offset (the network's audio arrives within it); outputs are
// written to the rings at the output time, ahead of inletd's sender.
//
// One shared-region client stands for the plug-in. Every application doing IO
// through it is named in that client's status ("Music, Logic Pro"); the
// outputs Core Audio mixes from them go out on one output layer.
//
// When inletd is not running, the device stays: inputs are silent, outputs go
// nowhere, and a background thread attaches as soon as inletd is back. If
// inletd comes back with another channel count or rate, the device changes
// shape through Core Audio's configuration change.

#include "inlet/DeviceClient.h"

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <os/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace inlet
{

namespace
{

constexpr AudioObjectID kPlugIn = kAudioObjectPlugInObject;
constexpr AudioObjectID kDevice = 2;
constexpr AudioObjectID kInStream = 3;
constexpr AudioObjectID kOutStream = 4;

const CFStringRef kDeviceUID = CFSTR("co.meros.inlet.device");
const CFStringRef kModelUID = CFSTR("co.meros.inlet.model");
const CFStringRef kAppBundle = CFSTR("co.meros.inlet");

/** Core Audio's timestamps come at this spacing of media-clock samples; it is
    also the largest IO buffer Core Audio will use. Well inside the rings. */
constexpr UInt32 kZeroPeriod = 2048;
/** Frames converted at a time, on the IO thread's stack-free scratch. */
constexpr int kChunk = 256;
/** How far ahead of an IO cycle Core Audio may read inputs or write outputs. */
constexpr UInt32 kSafetyFrames = 16;
constexpr int64_t kKeeperEveryNs = 250'000'000;

os_log_t logger()
{
    static os_log_t l = os_log_create("co.meros.inlet", "driver");
    return l;
}

struct Shape
{
    uint32_t inputs = 64, outputs = 64, rate = 48000;
    bool operator==(const Shape&) const = default;
};

Shape shapeOf(const DeviceClient& c)
{
    return { c.inputs(), c.outputs(), c.sampleRate() };
}

int64_t hostTicksFromNs(int64_t ns)
{
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t i;
        mach_timebase_info(&i);
        return i;
    }();
    if (tb.numer == tb.denom)
        return ns;
    return (int64_t) ((__int128) ns * tb.denom / tb.numer);
}

std::string cfToString(CFStringRef s)
{
    if (s == nullptr)
        return {};
    char buf[256] = {};
    if (!CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8))
        return {};
    return buf;
}

/** What the status calls a program: its process name, else its bundle's. */
std::string programName(pid_t pid, CFStringRef bundle)
{
    char name[2 * MAXCOMLEN + 1] = {};
    if (pid > 0 && proc_name(pid, name, sizeof name) > 0 && name[0] != '\0')
        return name;
    std::string id = cfToString(bundle);
    const auto dot = id.find_last_of('.');
    return dot == std::string::npos ? id : id.substr(dot + 1);
}

AudioStreamBasicDescription formatFor(uint32_t channels, Float64 rate)
{
    AudioStreamBasicDescription f {};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = (UInt32) kAudioFormatFlagIsFloat | (UInt32) kAudioFormatFlagsNativeEndian | (UInt32) kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * channels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4 * channels;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = 32;
    return f;
}

class Driver
{
public:
    static Driver& get()
    {
        static Driver* d = new Driver();   // lives as long as the server
        return *d;
    }

    AudioServerPlugInDriverInterface* interface = &vtable;
    AudioServerPlugInDriverInterface** ref = &interface;

    /* ---- lifecycle ---- */

    OSStatus initialize(AudioServerPlugInHostRef h)
    {
        host = h;
        // The shape inletd had last, so the device keeps its channels while
        // inletd is not running (at boot, say).
        recallShape();
        // Before Core Audio has seen the device, it takes the running inletd's
        // shape without a configuration change.
        initializing = true;
        attach(monoNowNs());
        initializing = false;
        keeper = std::thread([this] { keep(); });
        return kAudioHardwareNoError;
    }

    OSStatus addClient(const AudioServerPlugInClientInfo* info)
    {
        std::lock_guard<std::mutex> g(lock);
        programs[info->mClientID] = programName(info->mProcessID, info->mBundleID);
        return kAudioHardwareNoError;
    }

    OSStatus removeClient(const AudioServerPlugInClientInfo* info)
    {
        std::lock_guard<std::mutex> g(lock);
        programs.erase(info->mClientID);
        playing.erase(info->mClientID);
        return kAudioHardwareNoError;
    }

    OSStatus performConfigurationChange()
    {
        // Core Audio has stopped IO: the device takes the shape of the inletd
        // that is running now.
        std::lock_guard<std::mutex> g(lock);
        if (pending)
        {
            adopt(std::move(pending));
            shape = shapeOf(*current.load());
            storeShape();
        }
        changeRequested = false;
        return kAudioHardwareNoError;
    }

    OSStatus abortConfigurationChange()
    {
        std::lock_guard<std::mutex> g(lock);
        pending.reset();
        changeRequested = false;
        return kAudioHardwareNoError;
    }

    /* ---- IO ---- */

    OSStatus startIO(UInt32 clientID)
    {
        bool first = false;
        {
            std::lock_guard<std::mutex> g(lock);
            first = playing.empty();
            playing.insert(clientID);
            if (DeviceClient* c = current.load())
            {
                c->claimOutputs();
                c->setApp(namesPlaying());
                c->heartbeat(monoNowNs(), true, bufferFrames.load());
            }
        }
        if (first)
        {
            reanchor(true);
            changed(kDevice, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal);
        }
        return kAudioHardwareNoError;
    }

    OSStatus stopIO(UInt32 clientID)
    {
        bool last = false;
        {
            std::lock_guard<std::mutex> g(lock);
            playing.erase(clientID);
            last = playing.empty();
            if (DeviceClient* c = current.load())
            {
                c->setApp(namesPlaying());
                if (last)
                {
                    c->releaseOutputs();
                    c->heartbeat(monoNowNs(), false, bufferFrames.load());
                }
            }
            if (last)
                retired.clear();   // no IO thread can be holding them now
        }
        if (last)
            changed(kDevice, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal);
        return kAudioHardwareNoError;
    }

    OSStatus zeroTimeStamp(Float64* sampleTime, UInt64* hostTime, UInt64* seedOut)
    {
        ClockSample clk;
        const DeviceClient* c = current.load();
        if (c == nullptr || !c->clock(clk))
            clk = ClockSample();   // free-running on the local clock
        const uint32_t rate = c != nullptr ? c->sampleRate() : shape.rate;
        const uint64_t serial = c != nullptr ? c->serial() : 0;
        if (clk.epoch != lastEpoch || serial != lastSerial)
        {
            // PTP stepped, or another inletd: positions jump, and Core Audio
            // is told so by the seed.
            lastEpoch = clk.epoch;
            lastSerial = serial;
            reanchor(false, &clk, rate);
        }
        const int64_t now = monoNowNs();
        const uint64_t at = sampleAt(clk.ptpAt(now), rate);
        const uint64_t base = anchor.load(std::memory_order_relaxed);
        int64_t rel = (int64_t) (at - base);
        if (rel < 0)
        {
            reanchor(false, &clk, rate);
            rel = (int64_t) (at - anchor.load(std::memory_order_relaxed));
        }
        const int64_t zero = rel - rel % kZeroPeriod;
        lastZero = zero;
        *sampleTime = (Float64) zero;
        *hostTime = (UInt64) hostTicksFromNs(clk.monoAt(ptpAtSample(anchor.load(std::memory_order_relaxed) + (uint64_t) zero, rate)));
        *seedOut = seed.load(std::memory_order_relaxed);
        return kAudioHardwareNoError;
    }

    OSStatus willDo(UInt32 op, Boolean* will, Boolean* inPlace)
    {
        const bool yes = op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix
                      || op == kAudioServerPlugInIOOperationCycle;
        if (will)
            *will = yes;
        if (inPlace)
            *inPlace = true;
        return kAudioHardwareNoError;
    }

    OSStatus beginIO(UInt32 op, UInt32 frames)
    {
        if (op == kAudioServerPlugInIOOperationCycle)
        {
            bufferFrames.store(frames, std::memory_order_relaxed);
            if (DeviceClient* c = current.load())
                c->countPeriod(false, false);
        }
        return kAudioHardwareNoError;
    }

    OSStatus doIO(AudioObjectID stream, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo* cycle, void* buffer)
    {
        if (buffer == nullptr)
            return kAudioHardwareNoError;
        DeviceClient* c = current.load();
        const uint64_t base = anchor.load(std::memory_order_relaxed);
        if (op == kAudioServerPlugInIOOperationReadInput && stream == kInStream)
        {
            const uint32_t n = shape.inputs;
            auto* out = static_cast<float*>(buffer);
            if (c == nullptr || c->inputs() != n || n > kMaxChannels)
            {
                std::memset(out, 0, (size_t) frames * n * sizeof(float));
                return kAudioHardwareNoError;
            }
            const uint64_t first = base + (uint64_t) std::llround(cycle->mInputTime.mSampleTime);
            for (UInt32 done = 0; done < frames; done += kChunk)
            {
                const int len = (int) std::min<UInt32>(kChunk, frames - done);
                c->readInputs(first + done, inPtrs, (int) n, len);
                for (int f = 0; f < len; ++f)
                    for (uint32_t ch = 0; ch < n; ++ch)
                        out[(size_t) (done + f) * n + ch] = inPtrs[ch][f];
            }
        }
        else if (op == kAudioServerPlugInIOOperationWriteMix && stream == kOutStream)
        {
            const uint32_t n = shape.outputs;
            if (c == nullptr || c->outputs() != n || n > kMaxChannels)
                return kAudioHardwareNoError;
            const auto* in = static_cast<const float*>(buffer);
            const uint64_t first = base + (uint64_t) std::llround(cycle->mOutputTime.mSampleTime);
            for (UInt32 done = 0; done < frames; done += kChunk)
            {
                const int len = (int) std::min<UInt32>(kChunk, frames - done);
                for (int f = 0; f < len; ++f)
                    for (uint32_t ch = 0; ch < n; ++ch)
                        outPtrs[ch][f] = in[(size_t) (done + f) * n + ch];
                c->writeOutputs(first + done, outConst, (int) n, len);
            }
        }
        return kAudioHardwareNoError;
    }

    /* ---- properties ---- */

    Boolean has(AudioObjectID obj, const AudioObjectPropertyAddress* a)
    {
        UInt32 size = 0;
        return sizeOf(obj, a, 0, nullptr, size) == kAudioHardwareNoError;
    }

    OSStatus settable(AudioObjectID obj, const AudioObjectPropertyAddress* a, Boolean* out)
    {
        UInt32 size = 0;
        if (const OSStatus e = sizeOf(obj, a, 0, nullptr, size); e != kAudioHardwareNoError)
            return e;
        *out = (obj == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate)
            || ((obj == kInStream || obj == kOutStream)
                && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat));
        return kAudioHardwareNoError;
    }

    OSStatus sizeOf(AudioObjectID obj, const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q, UInt32& size)
    {
        return property(obj, a, qSize, q, 0, &size, nullptr);
    }

    OSStatus setProperty(AudioObjectID obj, const AudioObjectPropertyAddress* a, UInt32 size, const void* data)
    {
        // Only the one rate and format exist: setting them to themselves is all
        // that is allowed.
        const Shape s = shape;
        if (obj == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate)
        {
            if (size < sizeof(Float64))
                return kAudioHardwareBadPropertySizeError;
            return *static_cast<const Float64*>(data) == (Float64) s.rate ? kAudioHardwareNoError : kAudioDeviceUnsupportedFormatError;
        }
        if ((obj == kInStream || obj == kOutStream)
            && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat))
        {
            if (size < sizeof(AudioStreamBasicDescription))
                return kAudioHardwareBadPropertySizeError;
            const auto* f = static_cast<const AudioStreamBasicDescription*>(data);
            const auto want = formatFor(obj == kInStream ? s.inputs : s.outputs, s.rate);
            return f->mSampleRate == want.mSampleRate && f->mChannelsPerFrame == want.mChannelsPerFrame
                        && f->mFormatID == want.mFormatID
                     ? kAudioHardwareNoError
                     : kAudioDeviceUnsupportedFormatError;
        }
        return kAudioHardwareUnknownPropertyError;
    }

    /** Every property: its size into `size`, and, with `data`, its value
        (no more than `capacity` bytes). */
    OSStatus property(AudioObjectID obj, const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q, UInt32 capacity,
                      UInt32* size, void* data)
    {
        Out o { capacity, size, data };
        const Shape s = shape;
        const AudioObjectPropertySelector sel = a->mSelector;
        switch (obj)
        {
        case kPlugIn:
            switch (sel)
            {
            case kAudioObjectPropertyBaseClass: return o.value<AudioClassID>(kAudioObjectClassID);
            case kAudioObjectPropertyClass: return o.value<AudioClassID>(kAudioPlugInClassID);
            case kAudioObjectPropertyOwner: return o.value<AudioObjectID>(kAudioObjectUnknown);
            case kAudioObjectPropertyManufacturer: return o.value<CFStringRef>(CFSTR("Meros Inc."));
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList: return o.list<AudioObjectID>({ kDevice });
            case kAudioPlugInPropertyTranslateUIDToDevice:
            {
                if (data == nullptr)
                    return o.value<AudioObjectID>(kAudioObjectUnknown);   // the size alone
                if (qSize < sizeof(CFStringRef) || q == nullptr)
                    return kAudioHardwareBadPropertySizeError;
                const CFStringRef uid = *static_cast<const CFStringRef*>(q);
                return o.value<AudioObjectID>(uid != nullptr && CFStringCompare(uid, kDeviceUID, 0) == kCFCompareEqualTo ? kDevice
                                                                                                                       : kAudioObjectUnknown);
            }
            case kAudioPlugInPropertyResourceBundle: return o.value<CFStringRef>(CFSTR(""));
            default: return kAudioHardwareUnknownPropertyError;
            }

        case kDevice:
        {
            const bool in = a->mScope == kAudioObjectPropertyScopeInput, out = a->mScope == kAudioObjectPropertyScopeOutput;
            switch (sel)
            {
            case kAudioObjectPropertyBaseClass: return o.value<AudioClassID>(kAudioObjectClassID);
            case kAudioObjectPropertyClass: return o.value<AudioClassID>(kAudioDeviceClassID);
            case kAudioObjectPropertyOwner: return o.value<AudioObjectID>(kPlugIn);
            case kAudioObjectPropertyName: return o.value<CFStringRef>(CFSTR("Inlet"));
            case kAudioObjectPropertyManufacturer: return o.value<CFStringRef>(CFSTR("Meros Inc."));
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams:
            {
                // A direction with no channels has no stream.
                const bool hasIn = s.inputs > 0 && !out, hasOut = s.outputs > 0 && !in;
                if (hasIn && hasOut)
                    return o.list<AudioObjectID>({ kInStream, kOutStream });
                if (hasIn)
                    return o.list<AudioObjectID>({ kInStream });
                if (hasOut)
                    return o.list<AudioObjectID>({ kOutStream });
                return o.list<AudioObjectID>({});
            }
            case kAudioObjectPropertyControlList: return o.list<AudioObjectID>({});
            case kAudioDevicePropertyDeviceUID: return o.value<CFStringRef>(kDeviceUID);
            case kAudioDevicePropertyModelUID: return o.value<CFStringRef>(kModelUID);
            case kAudioDevicePropertyConfigurationApplication: return o.value<CFStringRef>(kAppBundle);
            case kAudioDevicePropertyTransportType: return o.value<UInt32>(kAudioDeviceTransportTypeVirtual);
            case kAudioDevicePropertyRelatedDevices: return o.list<AudioObjectID>({ kDevice });
            case kAudioDevicePropertyClockDomain: return o.value<UInt32>(0);
            case kAudioDevicePropertyDeviceIsAlive: return o.value<UInt32>(1);
            case kAudioDevicePropertyDeviceIsRunning:
            {
                std::lock_guard<std::mutex> g(lock);
                return o.value<UInt32>(playing.empty() ? 0 : 1);
            }
            case kAudioDevicePropertyDeviceCanBeDefaultDevice: return o.value<UInt32>(1);
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: return o.value<UInt32>(1);
            case kAudioDevicePropertyIsHidden: return o.value<UInt32>(0);
            case kAudioDevicePropertyLatency:
                // Inputs: the link offset the network's audio plays at.
                // Outputs: one packet, which the sender waits to fill.
                if (in)
                    return o.value<UInt32>((UInt32) std::max<int32_t>(0, linkOffset()));
                return o.value<UInt32>(s.rate / 1000);
            case kAudioDevicePropertySafetyOffset: return o.value<UInt32>(kSafetyFrames);
            case kAudioDevicePropertyNominalSampleRate: return o.value<Float64>((Float64) s.rate);
            case kAudioDevicePropertyAvailableNominalSampleRates:
                return o.list<AudioValueRange>({ AudioValueRange { (Float64) s.rate, (Float64) s.rate } });
            case kAudioDevicePropertyZeroTimeStampPeriod: return o.value<UInt32>(kZeroPeriod);
            case kAudioDevicePropertyPreferredChannelsForStereo: return o.list<UInt32>({ 1, 2 });
            case kAudioObjectPropertyElementName:
            {
                if (!in && !out)
                    return kAudioHardwareUnknownPropertyError;
                const uint32_t count = in ? s.inputs : s.outputs;
                if (a->mElement < 1 || a->mElement > count)
                    return kAudioHardwareUnknownPropertyError;
                return o.created(channelName(in, (int) a->mElement - 1));
            }
            default: return kAudioHardwareUnknownPropertyError;
            }
        }

        case kInStream:
        case kOutStream:
        {
            const bool in = obj == kInStream;
            const uint32_t channels = in ? s.inputs : s.outputs;
            switch (sel)
            {
            case kAudioObjectPropertyBaseClass: return o.value<AudioClassID>(kAudioObjectClassID);
            case kAudioObjectPropertyClass: return o.value<AudioClassID>(kAudioStreamClassID);
            case kAudioObjectPropertyOwner: return o.value<AudioObjectID>(kDevice);
            case kAudioObjectPropertyName: return o.value<CFStringRef>(in ? CFSTR("Inlet In") : CFSTR("Inlet Out"));
            case kAudioObjectPropertyOwnedObjects: return o.list<AudioObjectID>({});
            case kAudioStreamPropertyIsActive: return o.value<UInt32>(1);
            case kAudioStreamPropertyDirection: return o.value<UInt32>(in ? 1 : 0);
            case kAudioStreamPropertyTerminalType: return o.value<UInt32>(kAudioStreamTerminalTypeLine);
            case kAudioStreamPropertyStartingChannel: return o.value<UInt32>(1);
            case kAudioStreamPropertyLatency: return o.value<UInt32>(0);
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: return o.value<AudioStreamBasicDescription>(formatFor(channels, s.rate));
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
            {
                AudioStreamRangedDescription r {};
                r.mFormat = formatFor(channels, s.rate);
                r.mSampleRateRange = { (Float64) s.rate, (Float64) s.rate };
                return o.list<AudioStreamRangedDescription>({ r });
            }
            default: return kAudioHardwareUnknownPropertyError;
            }
        }

        default: return kAudioHardwareBadObjectError;
        }
    }

private:
    Driver()
    {
        for (int c = 0; c < kMaxChannels; ++c)
        {
            inPtrs[c] = inScratch[c];
            outPtrs[c] = outScratch[c];
            outConst[c] = outScratch[c];
        }
        vtable = {
            nullptr,
            [](void* d, REFIID iid, LPVOID* out) -> HRESULT {
                const CFUUIDRef asked = CFUUIDCreateFromUUIDBytes(nullptr, iid);
                const bool ok = CFEqual(asked, IUnknownUUID) || CFEqual(asked, kAudioServerPlugInDriverInterfaceUUID);
                CFRelease(asked);
                if (!ok)
                {
                    *out = nullptr;
                    return E_NOINTERFACE;
                }
                *out = d;
                return S_OK;
            },
            [](void*) -> ULONG { return 1; },   // one driver for the server's life
            [](void*) -> ULONG { return 1; },
            [](AudioServerPlugInDriverRef, AudioServerPlugInHostRef h) { return get().initialize(h); },
            [](AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*) -> OSStatus {
                return kAudioHardwareUnsupportedOperationError;
            },
            [](AudioServerPlugInDriverRef, AudioObjectID) -> OSStatus { return kAudioHardwareUnsupportedOperationError; },
            [](AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo* i) { return get().addClient(i); },
            [](AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo* i) { return get().removeClient(i); },
            [](AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) { return get().performConfigurationChange(); },
            [](AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) { return get().abortConfigurationChange(); },
            [](AudioServerPlugInDriverRef, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* a) -> Boolean {
                return get().has(obj, a);
            },
            [](AudioServerPlugInDriverRef, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* a, Boolean* out) {
                return get().settable(obj, a, out);
            },
            [](AudioServerPlugInDriverRef, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* a, UInt32 qs,
               const void* q, UInt32* size) { return get().sizeOf(obj, a, qs, q, *size); },
            [](AudioServerPlugInDriverRef, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* a, UInt32 qs,
               const void* q, UInt32 cap, UInt32* size, void* data) { return get().property(obj, a, qs, q, cap, size, data); },
            [](AudioServerPlugInDriverRef, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* a, UInt32, const void*,
               UInt32 size, const void* data) { return get().setProperty(obj, a, size, data); },
            [](AudioServerPlugInDriverRef, AudioObjectID, UInt32 client) { return get().startIO(client); },
            [](AudioServerPlugInDriverRef, AudioObjectID, UInt32 client) { return get().stopIO(client); },
            [](AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64* st, UInt64* ht, UInt64* seed) {
                return get().zeroTimeStamp(st, ht, seed);
            },
            [](AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32 op, Boolean* will, Boolean* inPlace) {
                return get().willDo(op, will, inPlace);
            },
            [](AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo*) {
                return get().beginIO(op, frames);
            },
            [](AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID stream, UInt32, UInt32 op, UInt32 frames,
               const AudioServerPlugInIOCycleInfo* cycle, void* main, void*) { return get().doIO(stream, op, frames, cycle, main); },
            [](AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) -> OSStatus {
                return kAudioHardwareNoError;
            },
        };
    }

    /** Writes a property's value, Core Audio's way: the size always, the
        value when there is room. */
    struct Out
    {
        UInt32 capacity;
        UInt32* size;
        void* data;

        template <typename T> OSStatus value(const T& v)
        {
            *size = sizeof(T);
            if (data == nullptr)
                return kAudioHardwareNoError;
            if (capacity < sizeof(T))
                return kAudioHardwareBadPropertySizeError;
            std::memcpy(data, &v, sizeof(T));
            return kAudioHardwareNoError;
        }
        template <typename T> OSStatus list(std::initializer_list<T> items)
        {
            if (data == nullptr)
            {
                *size = (UInt32) (sizeof(T) * items.size());
                return kAudioHardwareNoError;
            }
            // As many as fit: Core Audio asks for lists with smaller buffers.
            const size_t n = std::min(items.size(), (size_t) capacity / sizeof(T));
            std::memcpy(data, items.begin(), n * sizeof(T));
            *size = (UInt32) (n * sizeof(T));
            return kAudioHardwareNoError;
        }
        /** A string made for the caller, who releases it. */
        OSStatus created(CFStringRef s)
        {
            *size = sizeof(CFStringRef);
            if (data == nullptr)
            {
                CFRelease(s);
                return kAudioHardwareNoError;
            }
            if (capacity < sizeof(CFStringRef))
            {
                CFRelease(s);
                return kAudioHardwareBadPropertySizeError;
            }
            std::memcpy(data, &s, sizeof s);
            return kAudioHardwareNoError;
        }
    };

    static constexpr int kMaxChannels = 256;

    CFStringRef channelName(bool input, int channel)
    {
        std::string label;
        {
            std::lock_guard<std::mutex> g(lock);
            if (DeviceClient* c = current.load())
                label = input ? c->inputLabel(channel) : c->outputLabel(channel);
        }
        if (label.empty())
            label = std::string(input ? "In " : "Out ") + std::to_string(channel + 1);
        return CFStringCreateWithCString(nullptr, label.c_str(), kCFStringEncodingUTF8);
    }

    int32_t linkOffset()
    {
        std::lock_guard<std::mutex> g(lock);
        DeviceClient* c = current.load();
        return c != nullptr ? c->linkOffsetFrames() : 96;
    }

    /** The programs playing now, for the status (lock held). */
    std::string namesPlaying() const
    {
        std::string out;
        for (UInt32 id : playing)
        {
            const auto it = programs.find(id);
            const std::string name = it != programs.end() && !it->second.empty() ? it->second : "Core Audio";
            if (out.find(name) != std::string::npos)
                continue;
            if (!out.empty())
                out += ", ";
            out += name;
        }
        return out.empty() ? "Core Audio" : out;
    }

    /** A new media-clock anchor. `fresh`: IO is starting, sample time starts
        near zero; otherwise sample time carries on from the last timestamp, so
        it never runs backwards. */
    void reanchor(bool fresh, const ClockSample* given = nullptr, uint32_t rateIn = 0)
    {
        ClockSample clk;
        uint32_t rate = rateIn;
        if (given != nullptr)
            clk = *given;
        else
        {
            const DeviceClient* c = current.load();
            if (c == nullptr || !c->clock(clk))
                clk = ClockSample();
            rate = c != nullptr ? c->sampleRate() : shape.rate;
            lastEpoch = clk.epoch;
            lastSerial = c != nullptr ? c->serial() : 0;
        }
        const uint64_t at = sampleAt(clk.ptpAt(monoNowNs()), rate);
        const uint64_t boundary = at - at % kZeroPeriod;
        const uint64_t carry = fresh ? 0 : (uint64_t) std::max<int64_t>(0, lastZero) + kZeroPeriod;
        anchor.store(boundary - carry, std::memory_order_relaxed);
        if (fresh)
            lastZero = 0;
        seed.fetch_add(1, std::memory_order_relaxed);
    }

    /** Makes `next` the client IO uses (lock held). The one it replaces stays
        mapped until IO stops: the IO thread may still be reading it. */
    void adopt(std::unique_ptr<DeviceClient> next)
    {
        DeviceClient* old = current.load();
        const bool running = !playing.empty();
        if (old != nullptr)
            old->releaseOutputs();
        next->setApp(namesPlaying());
        if (running)
            next->claimOutputs();
        next->heartbeat(monoNowNs(), running, bufferFrames.load());
        clients.push_back(std::move(next));
        current.store(clients.back().get());
        if (old != nullptr)
        {
            for (auto it = clients.begin(); it != clients.end(); ++it)
                if (it->get() == old)
                {
                    retired.push_back(std::move(*it));
                    clients.erase(it);
                    break;
                }
        }
        if (!running)
            retired.clear();
    }

    /** Attaches to the inletd running now, if it is not the one attached. */
    void attach(int64_t now)
    {
        DeviceClient::Options opt;
        opt.kind = shm::ClientKind::CoreAudio;
        opt.app = "Core Audio";
        std::string error;
        auto next = DeviceClient::attach(opt, error);
        std::unique_lock<std::mutex> g(lock);
        DeviceClient* c = current.load();
        if (!next)
        {
            if (error != lastError)
                os_log(logger(), "Inlet: not attached: %{public}s", error.c_str());
            lastError = error;
            return;
        }
        lastError.clear();
        if (c != nullptr && next->serial() == c->serial())
            return;
        if (!next->daemonAlive(now))
            return;
        os_log(logger(), "Inlet: attached to inletd (%u in, %u out, %u Hz)", next->inputs(), next->outputs(), next->sampleRate());
        if (initializing || shapeOf(*next) == shape)
        {
            const bool reshaped = !(shapeOf(*next) == shape);
            shape = shapeOf(*next);
            adopt(std::move(next));
            if (reshaped)
                storeShape();
            return;
        }
        // Another shape: Core Audio stops IO, then asks the device to change.
        if (!changeRequested)
        {
            pending = std::move(next);
            changeRequested = true;
            g.unlock();
            host->RequestDeviceConfigurationChange(host, kDevice, 0, nullptr);
        }
    }

    /** The background thread: keeps the client's heartbeat while no IO runs,
        and attaches whenever inletd is new or back. */
    void keep()
    {
        for (;;)
        {
            const int64_t now = monoNowNs();
            DeviceClient* c = current.load();
            if (c == nullptr || !c->daemonAlive(now))
                attach(now);
            else
            {
                std::lock_guard<std::mutex> g(lock);
                if ((c = current.load()) != nullptr)
                    c->heartbeat(now, !playing.empty(), bufferFrames.load());
            }
            std::this_thread::sleep_for(std::chrono::nanoseconds(kKeeperEveryNs));
        }
    }

    /** Core Audio keeps a little storage for each plug-in, across restarts. */
    void recallShape()
    {
        CFPropertyListRef v = nullptr;
        if (host->CopyFromStorage(host, CFSTR("shape"), &v) != kAudioHardwareNoError || v == nullptr)
            return;
        if (CFGetTypeID(v) == CFArrayGetTypeID() && CFArrayGetCount((CFArrayRef) v) == 3)
        {
            uint32_t n[3] = {};
            for (CFIndex i = 0; i < 3; ++i)
            {
                const void* e = CFArrayGetValueAtIndex((CFArrayRef) v, i);
                if (CFGetTypeID(e) == CFNumberGetTypeID())
                    CFNumberGetValue((CFNumberRef) e, kCFNumberSInt32Type, &n[i]);
            }
            if (n[0] + n[1] > 0 && n[0] <= kMaxChannels && n[1] <= kMaxChannels && n[2] >= 44100 && n[2] <= 192000)
                shape = { n[0], n[1], n[2] };
        }
        CFRelease(v);
    }

    void storeShape()
    {
        const int32_t n[3] = { (int32_t) shape.inputs, (int32_t) shape.outputs, (int32_t) shape.rate };
        CFNumberRef nums[3];
        for (int i = 0; i < 3; ++i)
            nums[i] = CFNumberCreate(nullptr, kCFNumberSInt32Type, &n[i]);
        CFArrayRef a = CFArrayCreate(nullptr, (const void**) nums, 3, &kCFTypeArrayCallBacks);
        host->WriteToStorage(host, CFSTR("shape"), a);
        CFRelease(a);
        for (auto* x : nums)
            CFRelease(x);
    }

    void changed(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
    {
        const AudioObjectPropertyAddress a { sel, scope, kAudioObjectPropertyElementMain };
        if (host != nullptr)
            host->PropertiesChanged(host, obj, 1, &a);
    }

    AudioServerPlugInDriverInterface vtable {};
    AudioServerPlugInHostRef host = nullptr;
    std::thread keeper;

    std::mutex lock;
    std::map<UInt32, std::string> programs;   // Core Audio client -> program name
    std::set<UInt32> playing;                 // clients doing IO now
    std::vector<std::unique_ptr<DeviceClient>> clients, retired;
    std::unique_ptr<DeviceClient> pending;    // a differently shaped inletd, waiting for the change
    bool changeRequested = false;
    bool initializing = false;
    std::string lastError;

    std::atomic<DeviceClient*> current { nullptr };
    Shape shape;   // what the device is now; changed only during a configuration change
    std::atomic<uint32_t> bufferFrames { 0 };

    // The media clock, IO thread (and StartIO, before IO runs).
    std::atomic<uint64_t> anchor { 0 };
    std::atomic<uint64_t> seed { 1 };
    uint32_t lastEpoch = 0;
    uint64_t lastSerial = 0;
    int64_t lastZero = 0;

    float inScratch[kMaxChannels][kChunk] {};
    float outScratch[kMaxChannels][kChunk] {};
    float* inPtrs[kMaxChannels] {};
    float* outPtrs[kMaxChannels] {};
    const float* outConst[kMaxChannels] {};
};

} // namespace
} // namespace inlet

/** The CFPlugIn factory the bundle's Info.plist names. */
extern "C" __attribute__((visibility("default"))) void* InletDriverFactory(CFAllocatorRef, CFUUIDRef type)
{
    if (!CFEqual(type, kAudioServerPlugInTypeUUID))
        return nullptr;
    return inlet::Driver::get().ref;
}
