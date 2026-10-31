// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "inlet/DeviceClient.h"
#include "inlet/NetTx.h"

#include <aes67/rtp.h>

#include <algorithm>
#include <cstring>
#include <utility>

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
    c->countedTo.assign(c->region->header().receivers, 0);
    c->counting.assign(c->region->header().receivers, 0);
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
    if (ownsOutputs())
        return true;
    const uint32_t me = (uint32_t) slot + 1;
    auto* layers = region->header().outputLayers;
    for (int l = 0; l < shm::kOutputLayers; ++l)
    {
        uint32_t expect = 0;
        if (layers[l].compare_exchange_strong(expect, me))
        {
            layer = l;
            return true;
        }
    }
    layer = -1;
    return false;
}

void DeviceClient::releaseOutputs()
{
    if (layer < 0)
        return;
    uint32_t me = (uint32_t) slot + 1;
    region->header().outputLayers[layer].compare_exchange_strong(me, 0);
    layer = -1;
}

bool DeviceClient::ownsOutputs() const
{
    // inletd frees the layer of a host that stopped answering.
    return layer >= 0 && region->header().outputLayers[layer].load(std::memory_order_relaxed) == (uint32_t) slot + 1;
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
                readRing(c, ring, from + (uint32_t) done, tmp, n);   // missing frames read as zero
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
        readRing(c, ring, from, dst, frames);
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
    readRing(channel, ring, (uint32_t) (periodStart - (uint64_t) linkOffsetFrames()), dst, frames);
    for (int i = 0; i < frames; ++i)
        dst[i] = (int32_t) ((uint32_t) dst[i] << 8);   // 24 bits, left-justified
}

void DeviceClient::writeOutputInt32(uint64_t firstSample, int channel, const int32_t* src, int frames)
{
    if (!ownsOutputs() || channel < 0 || (uint32_t) channel >= outputs())
        return;
    aes67_ring* ring = region->txRing(layer, channel);
    for (int i = 0; i < frames; ++i)
        aes67_ring_write(ring, (uint32_t) (firstSample + (uint64_t) i), src[i] >> 8);
}

void DeviceClient::readInputRaw(uint64_t position, int channel, int32_t* dst, int frames)
{
    const aes67_ring* ring = region->inputRing(channel);
    if (ring == nullptr)
        std::fill(dst, dst + frames, 0);
    else
        readRing(channel, ring, (uint32_t) position, dst, frames);
}

void DeviceClient::readRing(int input, const aes67_ring* ring, uint32_t from, int32_t* dst, int frames)
{
    const int r = region->inputReceiver(input);
    // A ring nothing was written to since the receiver started (no stream,
    // or a channel past the stream's) is silence, not loss.
    if (r == 0 || (size_t) r > countedTo.size() || aes67_ring_head(ring) == 0)
    {
        aes67_ring_read(ring, from, dst, frames);
        return;
    }
    const size_t k = (size_t) r - 1;
    // Frames already counted: this period's earlier channels of the receiver.
    // A position further back than a ring is a new start.
    int counted = 0;
    if (counting[k])
    {
        const int32_t ahead = (int32_t) (countedTo[k] - from);
        if (ahead > 0 && ahead <= AES67_RING_FRAMES)
            counted = std::min(frames, (int) ahead);
    }
    if (counted > 0)
        aes67_ring_read(ring, from, dst, counted);
    if (counted < frames)
    {
        const int have = aes67_ring_read(ring, from + (uint32_t) counted, dst + counted, frames - counted);
        region->receiverStats(r).missingFrames.fetch_add((uint64_t) (frames - counted - have), std::memory_order_relaxed);
        countedTo[k] = from + (uint32_t) frames;
        counting[k] = 1;
    }
}

void DeviceClient::writeOutputRaw(uint64_t position, int channel, const int32_t* src, int frames)
{
    if (!ownsOutputs() || channel < 0 || (uint32_t) channel >= outputs())
        return;
    aes67_ring* ring = region->txRing(layer, channel);
    for (int i = 0; i < frames; ++i)
        aes67_ring_write(ring, (uint32_t) (position + (uint64_t) i), src[i]);
}

void DeviceClient::writeOutputs(uint64_t firstSample, const float* const* channels, int count, int frames)
{
    if (!ownsOutputs())
        return;
    for (int c = 0; c < count; ++c)
    {
        const float* src = channels[c];
        aes67_ring* ring = region->txRing(layer, c);
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
        aes67_ring* ring = region->txRing(layer, c);
        for (int i = 0; i < frames; ++i)
            aes67_ring_write(ring, (uint32_t) (firstSample + (uint64_t) i), src != nullptr ? (src[i] >> 8) : 0);
    }
}

void DeviceClient::heartbeat(int64_t nowMono, bool isRunning, uint32_t bufferFrames, uint32_t hostRate)
{
    auto& s = region->clients()[slot];
    s.heartbeatMono.store(nowMono, std::memory_order_relaxed);
    s.running.store(isRunning ? 1u : 0u, std::memory_order_relaxed);
    s.bufferFrames.store(bufferFrames, std::memory_order_relaxed);
    s.sampleRate.store(hostRate != 0 ? hostRate : sampleRate(), std::memory_order_relaxed);
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

void DeviceClient::setApp(const std::string& app)
{
    // Read by inletd's status without a lock: at worst one report shows a
    // name half-written.
    char* dst = region->clients()[slot].app;
    const size_t n = std::min(app.size(), (size_t) shm::kLabelBytes - 1);
    std::memcpy(dst, app.data(), n);
    std::memset(dst + n, 0, (size_t) shm::kLabelBytes - n);
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

PeriodEngine::PeriodEngine(DeviceClient& c, uint32_t framesPerPeriod, Callback onPeriod, uint32_t periodRate)
    : client(&c), frames(framesPerPeriod), rate(periodRate != 0 ? periodRate : c.sampleRate()), callback(std::move(onPeriod))
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
    while (active.load() && !client->clock(clk))
        sleepUntilMono(monoNowNs() + 1'000'000);
    PeriodClock pc(rate, frames);
    pc.start(clk, monoNowNs());
    // A wake-up more than this past its deadline is counted as late.
    const int64_t lateNs = (int64_t) frames * 1'000'000'000LL / rate / 2;
    // The host reports its own rate when it is not the device's.
    const auto hostRate = [this] { return rate != client->sampleRate() ? rate : 0u; };
    bool resyncNext = false;

    while (active.load())
    {
        client->clock(clk);
        const int64_t deadline = pc.deadlineMono(clk);
        sleepUntilMono(deadline);
        if (!active.load())
            break;
        const int64_t now = monoNowNs();
        Period p;
        p.start = pc.periodStart();
        p.late = now - deadline > lateNs;
        p.resynced = resyncNext;
        callback(p);
        if (switchTo != nullptr)
        {
            // A new inletd: its clock from now on, the schedule anchored afresh.
            client = std::exchange(switchTo, nullptr);
            while (active.load() && !client->clock(clk))
                sleepUntilMono(monoNowNs() + 1'000'000);
            pc.start(clk, monoNowNs());
            resyncNext = true;
            resyncCount.fetch_add(1);
            client->heartbeat(now, true, frames, hostRate());
            continue;
        }
        client->clock(clk);
        resyncNext = pc.advance(clk, monoNowNs());
        if (resyncNext)
            resyncCount.fetch_add(1);
        client->countPeriod(p.late, resyncNext);
        client->heartbeat(now, true, frames, hostRate());
    }
    client->heartbeat(monoNowNs(), false, frames, hostRate());
#if defined(_WIN32)
    if (mmcss != nullptr)
        AvRevertMmThreadCharacteristics(mmcss);
#endif
}

} // namespace inlet
