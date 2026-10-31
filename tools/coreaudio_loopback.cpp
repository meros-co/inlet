// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
//
// inlet-coreaudio-loopback: a Core Audio client for testing Inlet end to end
// on macOS, through the installed driver, a running inletd and the network.
//
// It writes a ramp on outputs 1..8 of the "Inlet" device whose value is the
// device's sample time (channel c offset by c * 1000), and reads inputs 1..8.
// With inletd's receiver 1 subscribed to its own sender 1 (multicast loops
// back), every input sample should be an output sample from a fixed number of
// samples before. It counts the samples that are, the ones that are not, and
// the silent ones.
//
//   inlet-coreaudio-loopback [seconds] [buffer]

#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
constexpr UInt32 kChannels = 8;
constexpr uint32_t kMask = 0x3FFFF;   // the ramp wraps at 2^18; plus one, it is never zero
constexpr float kStep = 1.0f / 8388608.0f;   // one 24-bit step

UInt32 gInputs = 0, gOutputs = 0;
std::atomic<bool> gCounting { false };
std::atomic<uint64_t> gCycles { 0 }, gMatched { 0 }, gSilent { 0 }, gWrong { 0 };
std::mutex gLock;
std::map<uint32_t, uint64_t> gLatencies;
// Input 1's silences: runs of silent frames, by where they start (sample time).
std::vector<std::pair<uint64_t, uint64_t>> gSilentRuns;
uint64_t gCountStart = 0;

template <typename T> OSStatus get(AudioObjectID obj, AudioObjectPropertySelector sel, T& out,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    const AudioObjectPropertyAddress a { sel, scope, kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, &out);
}

UInt32 channels(AudioObjectID device, AudioObjectPropertyScope scope)
{
    const AudioObjectPropertyAddress a { kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &a, 0, nullptr, &size) != noErr)
        return 0;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
    if (AudioObjectGetPropertyData(device, &a, 0, nullptr, &size, list) != noErr)
        return 0;
    UInt32 n = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        n += list->mBuffers[i].mNumberChannels;
    return n;
}

OSStatus onIO(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp* inTime, AudioBufferList* out,
              const AudioTimeStamp* outTime, void*)
{
    gCycles++;
    // Outputs: the ramp of the output's own sample time.
    if (out != nullptr && out->mNumberBuffers > 0)
    {
        auto* buf = static_cast<float*>(out->mBuffers[0].mData);
        const UInt32 n = out->mBuffers[0].mNumberChannels;
        const UInt32 frames = out->mBuffers[0].mDataByteSize / (n * sizeof(float));
        const auto first = (uint64_t) std::llround(outTime->mSampleTime);
        for (UInt32 f = 0; f < frames; ++f)
            for (UInt32 c = 0; c < n; ++c)
                buf[f * n + c] = c < kChannels ? (float) (((first + f + c * 1000) & kMask) + 1) * kStep : 0.0f;
    }
    // Inputs: how long ago each sample was written.
    if (in != nullptr && in->mNumberBuffers > 0 && gCounting.load())
    {
        const auto* buf = static_cast<const float*>(in->mBuffers[0].mData);
        const UInt32 n = in->mBuffers[0].mNumberChannels;
        const UInt32 frames = in->mBuffers[0].mDataByteSize / (n * sizeof(float));
        const auto first = (uint64_t) std::llround(inTime->mSampleTime);
        std::lock_guard<std::mutex> g(gLock);   // the counts are read only at the end
        if (gCountStart == 0)
            gCountStart = first;
        for (UInt32 f = 0; f < frames; ++f)
        {
            if (buf[f * n] == 0.0f)
            {
                if (!gSilentRuns.empty() && gSilentRuns.back().first + gSilentRuns.back().second == first + f)
                    gSilentRuns.back().second++;
                else
                    gSilentRuns.push_back({ first + f, 1 });
            }
        }
        for (UInt32 f = 0; f < frames; ++f)
            for (UInt32 c = 0; c < std::min(n, kChannels); ++c)
            {
                const long v = std::lround(buf[f * n + c] / kStep);
                if (v == 0)
                {
                    gSilent++;
                    continue;
                }
                const uint32_t here = (uint32_t) ((first + f + c * 1000) & kMask) + 1;
                ++gLatencies[(here - (uint32_t) v) & kMask];
            }
    }
    return noErr;
}

} // namespace

int main(int argc, char** argv)
{
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
    const UInt32 buffer = argc > 2 ? (UInt32) std::atoi(argv[2]) : 256;

    AudioObjectID device = kAudioObjectUnknown;
    CFStringRef uid = CFSTR("co.meros.inlet.device");
    const AudioObjectPropertyAddress translate { kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain };
    UInt32 size = sizeof device;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &translate, sizeof uid, &uid, &size, &device) != noErr
        || device == kAudioObjectUnknown)
    {
        std::fprintf(stderr, "No \"Inlet\" device: is Inlet.driver in /Library/Audio/Plug-Ins/HAL (and coreaudiod restarted)?\n");
        return 1;
    }
    gInputs = channels(device, kAudioObjectPropertyScopeInput);
    gOutputs = channels(device, kAudioObjectPropertyScopeOutput);
    if (gInputs < kChannels || gOutputs < kChannels)
    {
        std::fprintf(stderr, "Inlet has %u inputs and %u outputs; the test needs %u of each\n", gInputs, gOutputs, kChannels);
        return 1;
    }
    Float64 rate = 0;
    get(device, kAudioDevicePropertyNominalSampleRate, rate);
    UInt32 inLatency = 0, outLatency = 0, inSafety = 0, outSafety = 0;
    get(device, kAudioDevicePropertyLatency, inLatency, kAudioObjectPropertyScopeInput);
    get(device, kAudioDevicePropertyLatency, outLatency, kAudioObjectPropertyScopeOutput);
    get(device, kAudioDevicePropertySafetyOffset, inSafety, kAudioObjectPropertyScopeInput);
    get(device, kAudioDevicePropertySafetyOffset, outSafety, kAudioObjectPropertyScopeOutput);
    const AudioObjectPropertyAddress bufAddr { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMain };
    AudioObjectSetPropertyData(device, &bufAddr, 0, nullptr, sizeof buffer, &buffer);
    UInt32 actual = 0;
    get(device, kAudioDevicePropertyBufferFrameSize, actual);
    std::printf("Inlet: %u in, %u out at %.0f Hz, buffer %u; latency in %u out %u, safety in %u out %u\n", gInputs, gOutputs, rate,
                actual, inLatency, outLatency, inSafety, outSafety);

    AudioDeviceIOProcID proc = nullptr;
    if (AudioDeviceCreateIOProcID(device, onIO, nullptr, &proc) != noErr || AudioDeviceStart(device, proc) != noErr)
    {
        std::fprintf(stderr, "Could not start IO on Inlet\n");
        return 1;
    }
    // A second for the first samples to come round, then the count.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    gCounting = true;
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    gCounting = false;
    AudioDeviceStop(device, proc);
    AudioDeviceDestroyIOProcID(device, proc);

    std::lock_guard<std::mutex> g(gLock);
    uint64_t total = 0, best = 0;
    uint32_t latency = 0;
    for (const auto& [l, n] : gLatencies)
    {
        total += n;
        if (n > best)
            best = n, latency = l;
    }
    const uint64_t wrong = total - best;
    std::printf("%llu IO cycles; %llu samples came back %u samples later, %llu at other delays, %llu silent\n",
                (unsigned long long) gCycles.load(), (unsigned long long) best, latency, (unsigned long long) wrong,
                (unsigned long long) gSilent.load());
    for (const auto& [l, n] : gLatencies)
        if (l != latency && n > 0)
            std::printf("  %llu at %u\n", (unsigned long long) n, l);
    if (!gSilentRuns.empty())
    {
        std::printf("input 1 was silent %zu times:", gSilentRuns.size());
        for (size_t i = 0; i < gSilentRuns.size() && i < 12; ++i)
            std::printf(" %llu frames at +%.3f s;", (unsigned long long) gSilentRuns[i].second,
                        (double) (gSilentRuns[i].first - gCountStart) / rate);
        std::printf("\n");
    }
    const bool ok = best > 0 && wrong == 0 && gSilent.load() == 0;
    if (best == 0)
        std::printf("Nothing came back: subscribe Inlet's receiver 1 to its sender 1 (Receivers page, \"Net Out 1\").\n");
    std::printf(ok ? "PASS: every sample came back, at one fixed latency\n" : "FAIL\n");
    return ok ? 0 : 1;
}
