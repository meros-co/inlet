// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "inlet/NetRx.h"
#include "inlet/Clock.h"

#include <chrono>
#include <cstring>
#include <mutex>
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
#  include <mswsock.h>
#  include <mstcpip.h>
#  include <windows.h>
#  include <avrt.h>
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <pthread.h>
#    include <sys/qos.h>
#  endif
#endif

// Last: libaes67's C atomics shim defines macros the standard headers trip on.
#include <aes67/rtp.h>
#include <aes67/rx.h>

namespace inlet
{

bool parseIpv4(const std::string& text, uint32_t& out)
{
    in_addr a {};
    if (inet_pton(AF_INET, text.c_str(), &a) != 1)
        return false;
    out = a.s_addr;
    return true;
}

namespace
{

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kBadSocket = INVALID_SOCKET;
inline void closeSocket(Socket s) { closesocket(s); }
inline int lastSocketError() { return WSAGetLastError(); }
#else
using Socket = int;
constexpr Socket kBadSocket = -1;
inline void closeSocket(Socket s) { close(s); }
inline int lastSocketError() { return errno; }
#endif

enum SlotState : int { Free = 0, Busy = 1, Active = 2 };

struct Slot
{
    std::atomic<int> state { Free };
    RxStreamConfig cfg;

    // Receive thread only.
    uint16_t lastSeq = 0;
    bool haveSeq = false;
    int64_t lastArrival = 0;

    // Written by the receive thread, read by anyone.
    std::atomic<uint64_t> packets { 0 }, bytes { 0 }, lost { 0 }, reordered { 0 }, wrongPayload { 0 },
        malformed { 0 }, jitterWorst { 0 };
    std::atomic<int64_t> lastArrivalMono { 0 };

    void resetStats()
    {
        haveSeq = false;
        lastArrival = 0;
        for (auto* a : { &packets, &bytes, &lost, &reordered, &wrongPayload, &malformed, &jitterWorst })
            a->store(0, std::memory_order_relaxed);
        lastArrivalMono.store(0, std::memory_order_relaxed);
    }
};

#if defined(_WIN32)
constexpr int kOpsPerSocket = 64;

struct RecvOp
{
    OVERLAPPED ov {};
    WSAMSG msg {};
    WSABUF buf {};
    sockaddr_in from {};
    char data[2048];
    char control[WSA_CMSG_SPACE(sizeof(IN_PKTINFO))];
    int port = 0;   // index into Impl::ports
};
#endif

struct PortSocket
{
    uint16_t port = 0;
    Socket fd = kBadSocket;
#if defined(_WIN32)
    std::unique_ptr<RecvOp[]> ops;
#endif
};

} // namespace

struct NetRx::Impl
{
    uint32_t iface = 0;   // network order; 0 = any
    Slot slots[kMaxStreams];
    PortSocket ports[kMaxPorts];
    std::atomic<int> portCount { 0 };

    std::mutex control;   // add/remove, one at a time
    std::atomic<bool> running { false };
    std::atomic<uint64_t> passes { 0 };   // receive-loop iterations: removal waits on these
    std::thread thread;

#if defined(_WIN32)
    bool wsaStarted = false;
    HANDLE iocp = nullptr;
    LPFN_WSARECVMSG recvMsg = nullptr;
    std::atomic<int64_t> outstanding { 0 };
    std::atomic<bool> closing { false };
#endif

    ~Impl();

    void process(uint32_t src, uint32_t dst, uint16_t port, const uint8_t* p, size_t len, int64_t arrival);
    int openPort(uint16_t port, std::string& error);
    bool join(const RxStreamConfig& cfg, int portIndex, bool add, std::string& error);
    void run();
#if defined(_WIN32)
    bool post(RecvOp& op);
#endif
};

/* ---- the packet path ------------------------------------------------------------ */

void NetRx::Impl::process(uint32_t src, uint32_t dst, uint16_t port, const uint8_t* p, size_t len,
                          int64_t arrival)
{
    for (auto& s : slots)
    {
        if (s.state.load(std::memory_order_acquire) != Active)
            continue;
        const RxStreamConfig& c = s.cfg;
        if (c.group != dst || c.port != port || (c.source != 0 && c.source != src))
            continue;

        aes67_rtp_header_t h;
        if (!aes67_rtp_parse_header(p, len, &h))
        {
            s.malformed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (h.payload_type != c.payloadType)
        {
            s.wrongPayload.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (s.haveSeq)
        {
            const uint16_t expect = (uint16_t) (s.lastSeq + 1);
            if (h.sequence != expect)
            {
                const int16_t d = (int16_t) (h.sequence - expect);
                if (d > 0)
                    s.lost.fetch_add((uint64_t) d, std::memory_order_relaxed);
                else
                    s.reordered.fetch_add(1, std::memory_order_relaxed);   // still played: rings are timestamp-indexed
            }
        }
        s.lastSeq = h.sequence;
        s.haveSeq = true;
        if (s.lastArrival != 0 && arrival > s.lastArrival)
        {
            const uint64_t gap = (uint64_t) (arrival - s.lastArrival);
            if (gap > s.jitterWorst.load(std::memory_order_relaxed))
                s.jitterWorst.store(gap, std::memory_order_relaxed);
        }
        s.lastArrival = arrival;
        s.lastArrivalMono.store(arrival, std::memory_order_relaxed);
        s.packets.fetch_add(1, std::memory_order_relaxed);
        s.bytes.fetch_add(len, std::memory_order_relaxed);

        const int cps = c.channels;
        if (cps <= 0 || c.rings == nullptr)
            return;
        const auto fmt = (aes67_sample_format_t) c.format;
        const int bps = aes67_format_bytes(fmt);
        const size_t payload = len - AES67_RTP_HEADER_BYTES;
        const int frames = (int) (payload / (size_t) (bps * cps));
        const uint32_t ts0 = h.timestamp - c.mediaClockOffset;   // the media clock, which the rings are indexed by
        const uint8_t* d = p + AES67_RTP_HEADER_BYTES;
        for (int f = 0; f < frames; ++f)
            for (int ch = 0; ch < cps; ++ch, d += bps)
            {
                int32_t v;
                if (fmt == AES67_FORMAT_L16)
                    v = (int32_t) (int16_t) (((uint16_t) d[0] << 8) | d[1]) * 256;
                else if (fmt == AES67_FORMAT_AM824)
                    v = aes67_l24_to_i32(d + 1);   // the AES3 flags octet first
                else
                    v = aes67_l24_to_i32(d);
                aes67_ring_write(&c.rings[ch], ts0 + (uint32_t) f, v);
            }
        return;   // one stream per (group, port, source)
    }
}

/* ---- sockets --------------------------------------------------------------------- */

int NetRx::Impl::openPort(uint16_t port, std::string& error)
{
    const int n = portCount.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        if (ports[i].port == port)
            return i;
    if (n >= kMaxPorts)
    {
        error = "too many receive ports";
        return -1;
    }

#if defined(_WIN32)
    Socket fd = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
#else
    Socket fd = socket(AF_INET, SOCK_DGRAM, 0);
#endif
    if (fd == kBadSocket)
    {
        error = "socket: error " + std::to_string(lastSocketError());
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*) &one, sizeof one);
#if defined(SO_REUSEPORT)
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char*) &one, sizeof one);
#endif
    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*) &rcvbuf, sizeof rcvbuf);
    // The destination address of each datagram: how streams on one port are told apart.
#if defined(_WIN32)
    setsockopt(fd, IPPROTO_IP, IP_PKTINFO, (const char*) &one, sizeof one);
#elif defined(IP_RECVPKTINFO)
    setsockopt(fd, IPPROTO_IP, IP_RECVPKTINFO, &one, sizeof one);
#else
    setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof one);
#endif
#if defined(IP_MULTICAST_ALL)
    int zero = 0;   // Linux: only the groups this socket joined
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof zero);
#endif
#if !defined(_WIN32)
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (sockaddr*) &addr, sizeof addr) != 0)
    {
        error = "bind port " + std::to_string(port) + ": error " + std::to_string(lastSocketError());
        closeSocket(fd);
        return -1;
    }

    PortSocket& ps = ports[n];
    ps.port = port;
    ps.fd = fd;
#if defined(_WIN32)
    if (CreateIoCompletionPort((HANDLE) fd, iocp, (ULONG_PTR) n, 0) == nullptr)
    {
        error = "completion port: error " + std::to_string(GetLastError());
        closeSocket(fd);
        ps.fd = kBadSocket;
        return -1;
    }
    ps.ops.reset(new RecvOp[kOpsPerSocket]);
    for (int i = 0; i < kOpsPerSocket; ++i)
    {
        ps.ops[i].port = n;
        post(ps.ops[i]);
    }
#endif
    portCount.store(n + 1, std::memory_order_release);
    return n;
}

bool NetRx::Impl::join(const RxStreamConfig& cfg, int portIndex, bool add, std::string& error)
{
    const Socket fd = ports[portIndex].fd;
    int rc;
    if (cfg.source != 0)
    {
        ip_mreq_source m {};
        m.imr_multiaddr.s_addr = cfg.group;
        m.imr_sourceaddr.s_addr = cfg.source;
        m.imr_interface.s_addr = iface;
        rc = setsockopt(fd, IPPROTO_IP, add ? IP_ADD_SOURCE_MEMBERSHIP : IP_DROP_SOURCE_MEMBERSHIP,
                        (const char*) &m, sizeof m);
    }
    else
    {
        ip_mreq m {};
        m.imr_multiaddr.s_addr = cfg.group;
        m.imr_interface.s_addr = iface;
        rc = setsockopt(fd, IPPROTO_IP, add ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, (const char*) &m, sizeof m);
    }
    if (rc != 0)
    {
        char g[INET_ADDRSTRLEN] = {};
        in_addr ga {};
        ga.s_addr = cfg.group;
        inet_ntop(AF_INET, &ga, g, sizeof g);
        error = std::string(add ? "join " : "leave ") + g + ": error " + std::to_string(lastSocketError());
        return false;
    }
    return true;
}

#if defined(_WIN32)

bool NetRx::Impl::post(RecvOp& op)
{
    std::memset(&op.ov, 0, sizeof op.ov);
    op.buf.buf = op.data;
    op.buf.len = sizeof op.data;
    op.msg.name = (sockaddr*) &op.from;
    op.msg.namelen = sizeof op.from;
    op.msg.lpBuffers = &op.buf;
    op.msg.dwBufferCount = 1;
    op.msg.Control.buf = op.control;
    op.msg.Control.len = sizeof op.control;
    op.msg.dwFlags = 0;
    outstanding.fetch_add(1, std::memory_order_relaxed);
    const int rc = recvMsg(ports[op.port].fd, &op.msg, nullptr, &op.ov, nullptr);
    if (rc == 0 || WSAGetLastError() == WSA_IO_PENDING)
        return true;   // completes through the port either way
    outstanding.fetch_sub(1, std::memory_order_relaxed);
    return false;
}

void NetRx::Impl::run()
{
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (mmcss == nullptr)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    OVERLAPPED_ENTRY entries[64];
    while (running.load(std::memory_order_relaxed))
    {
        ULONG n = 0;
        if (GetQueuedCompletionStatusEx(iocp, entries, 64, &n, 20, FALSE))
        {
            const int64_t arrival = monoNowNs();
            for (ULONG i = 0; i < n; ++i)
            {
                if (entries[i].lpOverlapped == nullptr)
                    continue;   // a wake-up
                auto* op = CONTAINING_RECORD(entries[i].lpOverlapped, RecvOp, ov);
                outstanding.fetch_sub(1, std::memory_order_relaxed);
                if (closing.load(std::memory_order_relaxed))
                    continue;
                if (entries[i].Internal == 0)
                {
                    uint32_t dst = 0;
                    for (WSACMSGHDR* c = WSA_CMSG_FIRSTHDR(&op->msg); c != nullptr; c = WSA_CMSG_NXTHDR(&op->msg, c))
                        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO)
                        {
                            IN_PKTINFO pi;
                            std::memcpy(&pi, WSA_CMSG_DATA(c), sizeof pi);
                            dst = pi.ipi_addr.s_addr;
                        }
                    process(op->from.sin_addr.s_addr, dst, ports[op->port].port, (const uint8_t*) op->data,
                            entries[i].dwNumberOfBytesTransferred, arrival);
                }
                post(*op);
            }
        }
        passes.fetch_add(1, std::memory_order_release);
    }
    if (mmcss != nullptr)
        AvRevertMmThreadCharacteristics(mmcss);
}

#else

void NetRx::Impl::run()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    std::vector<pollfd> fds;
    uint8_t buf[2048];
    alignas(8) uint8_t control[256];
    while (running.load(std::memory_order_relaxed))
    {
        const int n = portCount.load(std::memory_order_acquire);
        fds.resize((size_t) n);
        for (int i = 0; i < n; ++i)
            fds[(size_t) i] = pollfd { ports[i].fd, POLLIN, 0 };
        const int ready = n > 0 ? poll(fds.data(), (nfds_t) n, 20) : (std::this_thread::sleep_for(std::chrono::milliseconds(20)), 0);
        if (ready > 0)
            for (int i = 0; i < n; ++i)
            {
                if (!(fds[(size_t) i].revents & POLLIN))
                    continue;
                for (int k = 0; k < 64; ++k)
                {
                    sockaddr_in from {};
                    iovec iov { buf, sizeof buf };
                    msghdr mh {};
                    mh.msg_name = &from;
                    mh.msg_namelen = sizeof from;
                    mh.msg_iov = &iov;
                    mh.msg_iovlen = 1;
                    mh.msg_control = control;
                    mh.msg_controllen = sizeof control;
                    const ssize_t got = recvmsg(ports[i].fd, &mh, 0);
                    if (got <= 0)
                        break;
                    uint32_t dst = 0;
                    for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c != nullptr; c = CMSG_NXTHDR(&mh, c))
                        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO)
                        {
                            in_pktinfo pi;
                            std::memcpy(&pi, CMSG_DATA(c), sizeof pi);
                            dst = pi.ipi_addr.s_addr;
                        }
                    process(from.sin_addr.s_addr, dst, ports[i].port, buf, (size_t) got, monoNowNs());
                }
            }
        passes.fetch_add(1, std::memory_order_release);
    }
}

#endif

NetRx::Impl::~Impl()
{
    running.store(false);
    if (thread.joinable())
        thread.join();
#if defined(_WIN32)
    closing.store(true);
#endif
    const int n = portCount.load();
    for (int i = 0; i < n; ++i)
        if (ports[i].fd != kBadSocket)
            closeSocket(ports[i].fd);
#if defined(_WIN32)
    // Closing cancels every outstanding receive; their buffers stay ours until
    // each cancellation has come back through the port.
    if (iocp != nullptr)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        OVERLAPPED_ENTRY entries[64];
        while (outstanding.load() > 0 && std::chrono::steady_clock::now() < until)
        {
            ULONG got = 0;
            if (GetQueuedCompletionStatusEx(iocp, entries, 64, &got, 50, FALSE))
                for (ULONG i = 0; i < got; ++i)
                    if (entries[i].lpOverlapped != nullptr)
                        outstanding.fetch_sub(1);
        }
        CloseHandle(iocp);
    }
    if (wsaStarted)
        WSACleanup();
#endif
}

/* ---- public ---------------------------------------------------------------------- */

NetRx::NetRx(std::unique_ptr<Impl> i) : impl(std::move(i)) {}

NetRx::~NetRx() = default;

std::unique_ptr<NetRx> NetRx::open(const std::string& ifaceIp, std::string& error)
{
    auto impl = std::make_unique<Impl>();
    if (!ifaceIp.empty() && !parseIpv4(ifaceIp, impl->iface))
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
    impl->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (impl->iocp == nullptr)
    {
        error = "completion port: error " + std::to_string(GetLastError());
        return nullptr;
    }
    // WSARecvMsg is an extension function: fetched through any socket.
    Socket probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    GUID guid = WSAID_WSARECVMSG;
    DWORD bytes = 0;
    const int rc = WSAIoctl(probe, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof guid, &impl->recvMsg,
                            sizeof impl->recvMsg, &bytes, nullptr, nullptr);
    closesocket(probe);
    if (rc != 0 || impl->recvMsg == nullptr)
    {
        error = "WSARecvMsg unavailable";
        return nullptr;
    }
#endif
    return std::unique_ptr<NetRx>(new NetRx(std::move(impl)));
}

int NetRx::addStream(const RxStreamConfig& cfg, std::string& error)
{
    std::lock_guard<std::mutex> lock(impl->control);
    if (cfg.channels == 0 || cfg.rings == nullptr)
    {
        error = "a stream needs channels and rings";
        return -1;
    }
    int slot = -1;
    bool joined = false;
    for (int i = 0; i < kMaxStreams; ++i)
    {
        Slot& s = impl->slots[i];
        const int st = s.state.load(std::memory_order_relaxed);
        if (st == Free && slot < 0)
            slot = i;
        if (st == Active && s.cfg.group == cfg.group && s.cfg.port == cfg.port)
        {
            if (s.cfg.source == cfg.source)
            {
                error = "that stream is already being received";
                return -1;
            }
            joined = joined || (s.cfg.source == 0 && cfg.source == 0);
        }
    }
    if (slot < 0)
    {
        error = "too many receive streams";
        return -1;
    }
    const int port = impl->openPort(cfg.port, error);
    if (port < 0)
        return -1;
    if (!joined && !impl->join(cfg, port, true, error))
        return -1;

    Slot& s = impl->slots[slot];
    s.state.store(Busy, std::memory_order_relaxed);
    s.cfg = cfg;
    s.resetStats();
    s.state.store(Active, std::memory_order_release);
    return slot;
}

void NetRx::removeStream(int slot)
{
    if (slot < 0 || slot >= kMaxStreams)
        return;
    std::lock_guard<std::mutex> lock(impl->control);
    Slot& s = impl->slots[slot];
    if (s.state.load(std::memory_order_relaxed) != Active)
        return;
    s.state.store(Busy, std::memory_order_release);
    // Two passes of the receive loop: any packet that saw the slot active has
    // been written by then.
    if (impl->running.load())
    {
        const uint64_t p0 = impl->passes.load(std::memory_order_acquire);
        while (impl->passes.load(std::memory_order_acquire) < p0 + 2 && impl->running.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool shared = false;
    for (int i = 0; i < kMaxStreams; ++i)
    {
        const Slot& o = impl->slots[i];
        if (i != slot && o.state.load(std::memory_order_relaxed) == Active && o.cfg.group == s.cfg.group
            && o.cfg.port == s.cfg.port && o.cfg.source == s.cfg.source)
            shared = true;
    }
    if (!shared)
    {
        const int n = impl->portCount.load();
        for (int i = 0; i < n; ++i)
            if (impl->ports[i].port == s.cfg.port)
            {
                std::string ignored;
                impl->join(s.cfg, i, false, ignored);
            }
    }
    s.state.store(Free, std::memory_order_release);
}

bool NetRx::start()
{
    if (impl->running.exchange(true))
        return true;
    impl->thread = std::thread([this] { impl->run(); });
    return true;
}

void NetRx::stop()
{
    impl->running.store(false);
    if (impl->thread.joinable())
        impl->thread.join();
}

RxStreamStats NetRx::stats(int slot) const
{
    RxStreamStats out;
    if (slot < 0 || slot >= kMaxStreams)
        return out;
    const Slot& s = impl->slots[slot];
    out.packets = s.packets.load(std::memory_order_relaxed);
    out.bytes = s.bytes.load(std::memory_order_relaxed);
    out.lost = s.lost.load(std::memory_order_relaxed);
    out.reordered = s.reordered.load(std::memory_order_relaxed);
    out.wrongPayload = s.wrongPayload.load(std::memory_order_relaxed);
    out.malformed = s.malformed.load(std::memory_order_relaxed);
    out.jitterWorstNs = s.jitterWorst.load(std::memory_order_relaxed);
    out.lastArrivalMono = s.lastArrivalMono.load(std::memory_order_relaxed);
    return out;
}

void NetRx::inject(uint32_t source, uint32_t destination, uint16_t port, const uint8_t* packet, size_t len,
                   int64_t arrivalMono)
{
    impl->process(source, destination, port, packet, len, arrivalMono);
}

} // namespace inlet
