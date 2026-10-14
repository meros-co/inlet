// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "inlet/NetTx.h"

#include <aes67/netrx.h>
#include <aes67/rtp.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <avrt.h>
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <time.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <mach/mach_time.h>
#    include <pthread.h>
#    include <sys/qos.h>
#  endif
#endif

namespace inlet
{

/* ---- waiting ------------------------------------------------------------------- */

void sleepUntilMono(int64_t mono)
{
#if defined(_WIN32)
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                       TIMER_ALL_ACCESS);
    const int64_t wait = mono - monoNowNs();
    // The high-resolution timer is good to a few hundred microseconds; the
    // rest is a short spin, which is what keeps 125 us packets evenly spaced.
    if (wait > 300'000 && timer != nullptr)
    {
        LARGE_INTEGER due;
        due.QuadPart = -(wait - 200'000) / 100;
        if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(timer, INFINITE);
    }
    while (monoNowNs() < mono)
        YieldProcessor();
#elif defined(__APPLE__)
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t i;
        mach_timebase_info(&i);
        return i;
    }();
    if (mono > 0)
        mach_wait_until((uint64_t) ((__uint128_t) mono * tb.denom / tb.numer));
#else
    timespec ts;
    ts.tv_sec = mono / 1'000'000'000LL;
    ts.tv_nsec = mono % 1'000'000'000LL;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR)
    {
    }
#endif
}

/* ---- the pacer ------------------------------------------------------------------- */

namespace
{

enum SlotState : int { Free = 0, Busy = 1, Active = 2 };

/** The pacer never sends more than this many milliseconds of packets in one
    go; further behind than that, it skips. */
constexpr int64_t kMaxBurstMs = 20;

struct TxSlot
{
    std::atomic<int> state { Free };
    TxStreamConfig cfg;

    // Pacer thread only.
    bool started = false;
    uint32_t epoch = 0;
    uint64_t next = 0;   // first sample of the next packet
    uint16_t seq = 0;
    std::vector<int32_t> channel, interleaved;
    std::vector<int64_t> mix;

    std::atomic<uint32_t> ssrc { 0 };
    std::atomic<uint64_t> packets { 0 }, silent { 0 }, skipped { 0 }, sendErrors { 0 };
};

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kBadSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket kBadSocket = -1;
#endif

} // namespace

struct NetTx::Impl
{
    const ClockMap& clock;
    NetTx::SendFn send;   // detached: instead of the socket
    Socket fd = kBadSocket;
#if defined(_WIN32)
    bool wsaStarted = false;
#endif
    TxSlot slots[kMaxStreams];
    std::mutex control;
    std::atomic<bool> running { false };
    std::atomic<uint64_t> passes { 0 };
    std::atomic<uint32_t> sendOffsetUs { 250 };
    std::thread thread;
    std::mt19937 rng { std::random_device {}() };

    explicit Impl(const ClockMap& c) : clock(c) {}
    ~Impl();

    int64_t pump(int64_t nowMono);
    void emit(int slot, TxSlot& s, const ClockSample& c);
    void run();
};

NetTx::Impl::~Impl()
{
    running.store(false);
    if (thread.joinable())
        thread.join();
#if defined(_WIN32)
    if (fd != kBadSocket)
        closesocket(fd);
    if (wsaStarted)
        WSACleanup();
#else
    if (fd != kBadSocket)
        close(fd);
#endif
}

void NetTx::Impl::emit(int index, TxSlot& s, const ClockSample&)
{
    const TxStreamConfig& c = s.cfg;
    const int P = (int) c.framesPerPacket;
    const int ch = c.channels;
    // Each host writing outputs has a layer; the packet is their sum. A layer
    // missing any of the packet's samples (its host stopped, or fell behind)
    // adds nothing.
    std::fill(s.mix.begin(), s.mix.end(), 0);
    int heard = 0;
    for (int l = 0; l < c.layers; ++l)
    {
        if (c.layerInUse != nullptr && c.layerInUse[l].load(std::memory_order_relaxed) == 0)
            continue;
        const aes67_ring* rings = c.rings + (size_t) l * c.layerStride;
        bool complete = true;
        for (int k = 0; k < ch && complete; ++k)
        {
            if (aes67_ring_read(&rings[k], (uint32_t) s.next, s.channel.data(), P) < P)
                complete = false;
            else
                for (int f = 0; f < P; ++f)
                    s.interleaved[(size_t) (f * ch + k)] = s.channel[(size_t) f];
        }
        if (!complete)
            continue;
        for (size_t i = 0; i < s.mix.size(); ++i)
            s.mix[i] += s.interleaved[i];
        ++heard;
    }
    for (size_t i = 0; i < s.mix.size(); ++i)
        s.interleaved[i] = (int32_t) std::clamp<int64_t>(s.mix[i], -8388608, 8388607);
    if (heard == 0)
    {
        // Silence, not a gap: receivers keep the route and the clock.
        s.silent.fetch_add(1, std::memory_order_relaxed);
    }

    aes67_rtp_header_t h {};
    h.payload_type = (uint8_t) (c.payloadType & 0x7F);
    h.sequence = s.seq++;
    h.timestamp = rtpTimestamp(s.next);
    h.ssrc = s.ssrc.load(std::memory_order_relaxed);
    uint8_t packet[1500];
    const size_t len = aes67_rtp_pack_l24(packet, sizeof packet, &h, s.interleaved.data(), P, ch);
    if (len == 0)
    {
        s.sendErrors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (send)
        send(index, c.group, c.port, packet, len);
    else
    {
        sockaddr_in to {};
        to.sin_family = AF_INET;
        to.sin_port = htons(c.port);
        to.sin_addr.s_addr = c.group;
        const auto sent = sendto(fd, (const char*) packet, (int) len, 0, (const sockaddr*) &to, sizeof to);
        if (sent < 0 || (size_t) sent != len)
        {
            s.sendErrors.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    s.packets.fetch_add(1, std::memory_order_relaxed);
}

int64_t NetTx::Impl::pump(int64_t nowMono)
{
    int64_t nextDue = nowMono + 1'000'000;
    ClockSample c;
    if (!clock.read(c))
        return nextDue;
    const int64_t ptpNow = c.ptpAt(nowMono);
    const uint32_t offsetUs = sendOffsetUs.load(std::memory_order_relaxed);

    for (int i = 0; i < kMaxStreams; ++i)
    {
        TxSlot& s = slots[i];
        if (s.state.load(std::memory_order_acquire) != Active)
            continue;
        const uint32_t rate = s.cfg.sampleRate;
        const uint64_t P = s.cfg.framesPerPacket;
        const uint64_t offset = (uint64_t) offsetUs * rate / 1'000'000u;
        const uint64_t nowS = sampleAt(ptpNow, rate);
        const uint64_t sendable = nowS > offset ? nowS - offset : 0;   // packets ending at or before this may go

        if (!s.started || s.epoch != c.epoch)
        {
            // The packet containing "now": the first one sent is complete
            // the moment it is due, never one already half in the past.
            s.next = sendable / P * P;
            s.started = true;
            s.epoch = c.epoch;
        }
        if (sendable > s.next + P)
        {
            const uint64_t behind = (sendable - s.next) / P;   // packets due
            const uint64_t limit = (uint64_t) kMaxBurstMs * rate / 1000u / P + 1;
            if (behind > limit)
            {
                s.skipped.fetch_add(behind - 1, std::memory_order_relaxed);
                s.next = sendable / P * P - P;
            }
        }
        while (s.next + P <= sendable)
        {
            emit(i, s, c);
            s.next += P;
        }
        const int64_t due = c.monoAt(ptpAtSample(s.next + P + offset, rate));
        nextDue = std::min(nextDue, due);
    }
    passes.fetch_add(1, std::memory_order_release);
    return nextDue;
}

void NetTx::Impl::run()
{
#if defined(_WIN32)
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (mmcss == nullptr)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    while (running.load(std::memory_order_relaxed))
    {
        const int64_t now = monoNowNs();
        const int64_t due = pump(now);
        // Never sleep long: a stream may be added, the clock may step.
        sleepUntilMono(std::min(due, monoNowNs() + 2'000'000));
    }
#if defined(_WIN32)
    if (mmcss != nullptr)
        AvRevertMmThreadCharacteristics(mmcss);
#endif
}

/* ---- public ------------------------------------------------------------------------- */

NetTx::NetTx(std::unique_ptr<Impl> i) : impl(std::move(i)) {}
NetTx::~NetTx() = default;

std::unique_ptr<NetTx> NetTx::openDetached(const ClockMap& clock, SendFn send)
{
    auto impl = std::make_unique<Impl>(clock);
    impl->send = std::move(send);
    return std::unique_ptr<NetTx>(new NetTx(std::move(impl)));
}

std::unique_ptr<NetTx> NetTx::open(const std::string& ifaceIp, const ClockMap& clock, std::string& error)
{
    auto impl = std::make_unique<Impl>(clock);
    uint32_t iface = 0;
    if (!ifaceIp.empty() && !aes67_parse_ipv4(ifaceIp.c_str(), &iface))
    {
        error = "bad interface address " + ifaceIp;
        return nullptr;
    }
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        error = "WSAStartup failed";
        return nullptr;
    }
    impl->wsaStarted = true;
#endif
    impl->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (impl->fd == kBadSocket)
    {
        error = "socket failed";
        return nullptr;
    }
    if (iface != 0)
    {
        in_addr a {};
        a.s_addr = iface;
        if (setsockopt(impl->fd, IPPROTO_IP, IP_MULTICAST_IF, (const char*) &a, sizeof a) != 0)
        {
            error = "IP_MULTICAST_IF " + ifaceIp + " failed";
            return nullptr;
        }
    }
    int ttl = 15;
    setsockopt(impl->fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char*) &ttl, sizeof ttl);
    int loop = 1;   // receivers on this machine (and the tests) hear it too
    setsockopt(impl->fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*) &loop, sizeof loop);
#if !defined(_WIN32)
    // DSCP EF (46), AES67 section 7. Windows takes this from a QoS policy,
    // which the installer creates; a socket option there is ignored.
    int tos = 46 << 2;
    setsockopt(impl->fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
#endif
    return std::unique_ptr<NetTx>(new NetTx(std::move(impl)));
}

int NetTx::addStream(const TxStreamConfig& cfg, std::string& error)
{
    std::lock_guard<std::mutex> lock(impl->control);
    if (cfg.channels == 0 || cfg.rings == nullptr || cfg.framesPerPacket == 0 || cfg.sampleRate == 0)
    {
        error = "a sender needs channels, rings, a rate and a packet time";
        return -1;
    }
    if (aes67_max_channels(3, (int) cfg.framesPerPacket) < cfg.channels)
    {
        error = std::to_string(cfg.channels) + " channels do not fit a " + std::to_string(cfg.framesPerPacket)
              + "-frame packet";
        return -1;
    }
    for (int i = 0; i < kMaxStreams; ++i)
    {
        TxSlot& s = impl->slots[i];
        if (s.state.load(std::memory_order_relaxed) != Free)
            continue;
        s.state.store(Busy, std::memory_order_relaxed);
        s.cfg = cfg;
        s.started = false;
        s.seq = (uint16_t) impl->rng();
        s.ssrc.store(cfg.ssrc ? cfg.ssrc : (uint32_t) impl->rng(), std::memory_order_relaxed);
        s.channel.assign(cfg.framesPerPacket, 0);
        s.interleaved.assign((size_t) cfg.framesPerPacket * cfg.channels, 0);
        s.mix.assign((size_t) cfg.framesPerPacket * cfg.channels, 0);
        for (auto* a : { &s.packets, &s.silent, &s.skipped, &s.sendErrors })
            a->store(0, std::memory_order_relaxed);
        s.state.store(Active, std::memory_order_release);
        return i;
    }
    error = "too many senders";
    return -1;
}

void NetTx::removeStream(int slot)
{
    if (slot < 0 || slot >= kMaxStreams)
        return;
    std::lock_guard<std::mutex> lock(impl->control);
    TxSlot& s = impl->slots[slot];
    if (s.state.load() != Active)
        return;
    s.state.store(Busy, std::memory_order_release);
    if (impl->running.load())
    {
        const uint64_t p0 = impl->passes.load(std::memory_order_acquire);
        while (impl->passes.load(std::memory_order_acquire) < p0 + 2 && impl->running.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    s.state.store(Free, std::memory_order_release);
}

void NetTx::setSendOffsetUs(uint32_t us)
{
    impl->sendOffsetUs.store(us);
}

bool NetTx::start()
{
    if (impl->running.exchange(true))
        return true;
    impl->thread = std::thread([this] { impl->run(); });
    return true;
}

void NetTx::stop()
{
    impl->running.store(false);
    if (impl->thread.joinable())
        impl->thread.join();
}

int64_t NetTx::pump(int64_t nowMono)
{
    return impl->pump(nowMono);
}

TxStreamStats NetTx::stats(int slot) const
{
    TxStreamStats out;
    if (slot < 0 || slot >= kMaxStreams)
        return out;
    const TxSlot& s = impl->slots[slot];
    out.packets = s.packets.load(std::memory_order_relaxed);
    out.silentPackets = s.silent.load(std::memory_order_relaxed);
    out.skipped = s.skipped.load(std::memory_order_relaxed);
    out.sendErrors = s.sendErrors.load(std::memory_order_relaxed);
    return out;
}

uint32_t NetTx::ssrc(int slot) const
{
    return slot >= 0 && slot < kMaxStreams ? impl->slots[slot].ssrc.load() : 0;
}

} // namespace inlet
