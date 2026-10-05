// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "inlet/DeviceClient.h"
#include "inlet/NetTx.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <avrt.h>
#else
#  include <pthread.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <sys/qos.h>
#  endif
#endif

// Last: libaes67's C atomics shim defines macros the standard headers trip on.
#include <aes67/rtp.h>

namespace inlet
{

namespace
{
constexpr float kToFloat = 1.0f / 8388608.0f;
constexpr int kChunk = 512;   // frames converted per ring read
constexpr int64_t kDaemonStaleNs = 1'000'000'000LL;

uint32_t currentPid()
{
#if defined(_WIN32)
    return (uint32_t) GetCurrentProcessId();
#else
    return (uint32_t) getpid();
#endif
}
} // namespace

std::unique_ptr<DeviceClient> DeviceClient::attach(const Options& options, std::string& error)
{
    auto region = shm::Region::open(error, options.regionName);
    if (!region)
        return nullptr;
    std::unique_ptr<DeviceClient> c(new DeviceClient());
    c->region = std::move(region);
    shm::ClientSlot* slots = c->region->clients();
    for (int i = 0; i < shm::kMaxClients; ++i)
    {
        uint32_t expect = 0;
        if (!slots[i].state.compare_exchange_strong(expect, 1))
            continue;
        slots[i].pid.store(currentPid());
        slots[i].kind.store((uint32_t) options.kind);
        slots[i].running.store(0);
        slots[i].periods.store(0);
        slots[i].lateWakeups.store(0);
        slots[i].resyncs.store(0);
        slots[i].heartbeatMono.store(monoNowNs());
        std::memset(slots[i].app, 0, sizeof slots[i].app);
        std::strncpy(slots[i].app, options.app.c_str(), sizeof slots[i].app - 1);
        slots[i].state.store(2, std::memory_order_release);
        c->slot = i;
        return c;
    }
    error = "too many applications are using Inlet at once";
    return nullptr;
}

DeviceClient::~DeviceClient()
{
    if (region && slot >= 0)
    {
        releaseOutputs();
        auto& s = region->clients()[slot];
        s.running.store(0);
        s.state.store(0, std::memory_order_release);
    }
}

bool DeviceClient::daemonAlive(int64_t nowMono) const
{
    const int64_t hb = region->header().daemonHeartbeatMono.load(std::memory_order_relaxed);
    return hb != 0 && nowMono - hb < kDaemonStaleNs;
}

bool DeviceClient::claimOutputs()
{
    const uint32_t me = (uint32_t) slot + 1;
    uint32_t expect = 0;
    auto& owner = region->header().outputOwner;
    return owner.compare_exchange_strong(expect, me) || expect == me;
}

void DeviceClient::releaseOutputs()
{
    uint32_t me = (uint32_t) slot + 1;
    region->header().outputOwner.compare_exchange_strong(me, 0);
}

bool DeviceClient::ownsOutputs() const
{
    return region->header().outputOwner.load(std::memory_order_relaxed) == (uint32_t) slot + 1;
}

void DeviceClient::readInputs(uint64_t periodStart, float* const* channels, int count, int frames)
{
    const uint32_t from = (uint32_t) (periodStart - (uint64_t) linkOffsetFrames());
    int32_t tmp[kChunk];
    for (int c = 0; c < count; ++c)
    {
        float* dst = channels[c];
        if (dst == nullptr)
            continue;
        const aes67_ring* ring = region->inputRing(c);
        for (int done = 0; done < frames; done += kChunk)
        {
            const int n = std::min(kChunk, frames - done);
            if (ring == nullptr)
                std::fill(dst + done, dst + done + n, 0.0f);
            else
            {
                aes67_ring_read(ring, from + (uint32_t) done, tmp, n);   // missing frames read as zero
                for (int i = 0; i < n; ++i)
                    dst[done + i] = (float) tmp[i] * kToFloat;
            }
        }
    }
}

void DeviceClient::readInputsInt32(uint64_t periodStart, int32_t* const* channels, int count, int frames)
{
    const uint32_t from = (uint32_t) (periodStart - (uint64_t) linkOffsetFrames());
    for (int c = 0; c < count; ++c)
    {
        int32_t* dst = channels[c];
        if (dst == nullptr)
            continue;
        const aes67_ring* ring = region->inputRing(c);
        if (ring == nullptr)
        {
            std::fill(dst, dst + frames, 0);
            continue;
        }
        aes67_ring_read(ring, from, dst, frames);
        for (int i = 0; i < frames; ++i)
            dst[i] = (int32_t) ((uint32_t) dst[i] << 8);   // 24 bits, left-justified
    }
}

void DeviceClient::readInputInt32(uint64_t periodStart, int channel, int32_t* dst, int frames)
{
    const aes67_ring* ring = region->inputRing(channel);
    if (ring == nullptr)
    {
        std::fill(dst, dst + frames, 0);
        return;
    }
    aes67_ring_read(ring, (uint32_t) (periodStart - (uint64_t) linkOffsetFrames()), dst, frames);
    for (int i = 0; i < frames; ++i)
        dst[i] = (int32_t) ((uint32_t) dst[i] << 8);   // 24 bits, left-justified
}

void DeviceClient::writeOutputInt32(uint64_t firstSample, int channel, const int32_t* src, int frames)
{
    if (!ownsOutputs() || channel < 0 || (uint32_t) channel >= outputs())
        return;
    aes67_ring* ring = region->txRing(channel);
    for (int i = 0; i < frames; ++i)
        aes67_ring_write(ring, (uint32_t) (firstSample + (uint64_t) i), src[i] >> 8);
}

void DeviceClient::writeOutputs(uint64_t firstSample, const float* const* channels, int count, int frames)
{
    if (!ownsOutputs())
        return;
    for (int c = 0; c < count; ++c)
    {
        const float* src = channels[c];
        aes67_ring* ring = region->txRing(c);
        for (int i = 0; i < frames; ++i)
        {
            const float v = src != nullptr ? std::clamp(src[i], -1.0f, 1.0f - kToFloat) : 0.0f;
            aes67_ring_write(ring, (uint32_t) (firstSample + (uint64_t) i), (int32_t) (v * 8388608.0f));
        }
    }
}

void DeviceClient::writeOutputsInt32(uint64_t firstSample, const int32_t* const* channels, int count, int frames)
{
    if (!ownsOutputs())
        return;
    for (int c = 0; c < count; ++c)
    {
        const int32_t* src = channels[c];
        aes67_ring* ring = region->txRing(c);
        for (int i = 0; i < frames; ++i)
            aes67_ring_write(ring, (uint32_t) (firstSample + (uint64_t) i), src != nullptr ? (src[i] >> 8) : 0);
    }
}

void DeviceClient::heartbeat(int64_t nowMono, bool isRunning, uint32_t bufferFrames)
{
    auto& s = region->clients()[slot];
    s.heartbeatMono.store(nowMono, std::memory_order_relaxed);
    s.running.store(isRunning ? 1u : 0u, std::memory_order_relaxed);
    s.bufferFrames.store(bufferFrames, std::memory_order_relaxed);
    s.sampleRate.store(sampleRate(), std::memory_order_relaxed);
}

void DeviceClient::countPeriod(bool late, bool resynced)
{
    auto& s = region->clients()[slot];
    s.periods.fetch_add(1, std::memory_order_relaxed);
    if (late)
        s.lateWakeups.fetch_add(1, std::memory_order_relaxed);
    if (resynced)
        s.resyncs.fetch_add(1, std::memory_order_relaxed);
}

std::string DeviceClient::inputLabel(int channel) const
{
    if (channel < 0 || (uint32_t) channel >= region->header().inputs)
        return {};
    const char* p = region->inputLabel(channel);
    return std::string(p, strnlen(p, shm::kLabelBytes));
}

std::string DeviceClient::outputLabel(int channel) const
{
    if (channel < 0 || (uint32_t) channel >= region->header().outputs)
        return {};
    const char* p = region->outputLabel(channel);
    return std::string(p, strnlen(p, shm::kLabelBytes));
}

/* ---- the period engine ---------------------------------------------------------- */

PeriodEngine::PeriodEngine(DeviceClient& c, uint32_t framesPerPeriod, Callback onPeriod)
    : client(c), frames(framesPerPeriod), callback(std::move(onPeriod))
{
}

PeriodEngine::~PeriodEngine()
{
    stop();
}

bool PeriodEngine::start()
{
    if (active.exchange(true))
        return true;
    thread = std::thread([this] { run(); });
    return true;
}

void PeriodEngine::stop()
{
    if (!active.exchange(false))
        return;
    if (thread.joinable())
        thread.join();
}

void PeriodEngine::run()
{
#if defined(_WIN32)
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (mmcss == nullptr)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    ClockSample clk;
    while (active.load() && !client.clock(clk))
        sleepUntilMono(monoNowNs() + 1'000'000);
    PeriodClock pc(client.sampleRate(), frames);
    pc.start(clk, monoNowNs());
    // A wake-up more than this past its deadline is counted as late.
    const int64_t lateNs = (int64_t) frames * 1'000'000'000LL / client.sampleRate() / 2;

    while (active.load())
    {
        client.clock(clk);
        const int64_t deadline = pc.deadlineMono(clk);
        sleepUntilMono(deadline);
        if (!active.load())
            break;
        const int64_t now = monoNowNs();
        const bool late = now - deadline > lateNs;
        callback(pc.periodStart(), late);
        client.clock(clk);
        const bool resynced = pc.advance(clk, monoNowNs());
        if (resynced)
            resyncCount.fetch_add(1);
        client.countPeriod(late, resynced);
        client.heartbeat(now, true, frames);
    }
    client.heartbeat(monoNowNs(), false, frames);
#if defined(_WIN32)
    if (mmcss != nullptr)
        AvRevertMmThreadCharacteristics(mmcss);
#endif
}

} // namespace inlet
