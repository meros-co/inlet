// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "Sap.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using SockT = SOCKET;
#  define BAD_SOCK INVALID_SOCKET
#  define closesock closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
using SockT = int;
#  define BAD_SOCK (-1)
#  define closesock close
#endif

namespace inlet
{

namespace
{
constexpr const char* kGroup = "239.255.255.255";
constexpr uint16_t kPort = 9875;
constexpr int64_t kIntervalMs = 30000;
constexpr int64_t kRetryListenMs = 5000;

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

bool sapDecode(const uint8_t* b, size_t len, SapPacket& out)
{
    if (len < 8)
        return false;
    if ((b[0] >> 5) != 1)
        return false;   // version 1 only
    const bool ipv6 = (b[0] & 0x10) != 0;
    out.deletion = (b[0] & 0x04) != 0;
    if ((b[0] & 0x02) || (b[0] & 0x01))
        return false;   // encrypted or compressed: not supported
    const size_t authLen = (size_t) b[1] * 4;
    out.hash = (uint16_t) ((b[2] << 8) | b[3]);
    const size_t originLen = ipv6 ? 16 : 4;
    if (!ipv6)
        std::memcpy(&out.origin, b + 4, 4);
    size_t at = 4 + originLen + authLen;
    if (at > len)
        return false;
    // An optional MIME type, zero-terminated; absent means application/sdp.
    // SDP itself starts "v=0", which tells the two apart.
    out.payloadType.clear();
    if (len - at >= 3 && std::memcmp(b + at, "v=0", 3) != 0)
    {
        const void* nul = std::memchr(b + at, 0, len - at);
        if (nul == nullptr)
            return false;
        const size_t n = (size_t) (static_cast<const uint8_t*>(nul) - (b + at));
        out.payloadType.assign(reinterpret_cast<const char*>(b + at), n);
        at += n + 1;
    }
    out.payload.assign(reinterpret_cast<const char*>(b + at), len - at);
    return out.payloadType.empty() || out.payloadType == "application/sdp";
}

std::vector<uint8_t> sapEncode(const SapPacket& p)
{
    std::vector<uint8_t> out;
    out.push_back((uint8_t) (0x20 | (p.deletion ? 0x04 : 0)));   // v=1, IPv4
    out.push_back(0);
    out.push_back((uint8_t) (p.hash >> 8));
    out.push_back((uint8_t) p.hash);
    const auto* o = reinterpret_cast<const uint8_t*>(&p.origin);
    out.insert(out.end(), o, o + 4);
    const std::string type = p.payloadType.empty() ? "application/sdp" : p.payloadType;
    out.insert(out.end(), type.begin(), type.end());
    out.push_back(0);
    out.insert(out.end(), p.payload.begin(), p.payload.end());
    return out;
}

Sap::Sap(const std::string& ifaceIp) : iface(ifaceIp)
{
    in_addr a {};
    if (!ifaceIp.empty() && inet_pton(AF_INET, ifaceIp.c_str(), &a) == 1)
        ifaceAddr = a.s_addr;
    nextHash = (uint16_t) (std::random_device {}() | 1);
}

Sap::~Sap()
{
    stop();
}

bool Sap::start(std::string& error)
{
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SockT tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx == BAD_SOCK)
    {
        error = "SAP: socket failed";
        return false;
    }
    if (!openListener(error))
    {
        std::lock_guard<std::mutex> g(lock);
        listenError = error;
    }
    if (ifaceAddr != 0)
    {
        in_addr a {};
        a.s_addr = ifaceAddr;
        setsockopt(tx, IPPROTO_IP, IP_MULTICAST_IF, (const char*) &a, sizeof a);
    }
    int ttl = 15;
    setsockopt(tx, IPPROTO_IP, IP_MULTICAST_TTL, (const char*) &ttl, sizeof ttl);
    txSock = (intptr_t) tx;
    running = true;
    thread = std::thread([this] { run(); });
    return true;   // error, if set, says what does not work
}

std::string Sap::warning() const
{
    std::lock_guard<std::mutex> g(lock);
    return listenError;
}

bool Sap::openListener(std::string& error)
{
    SockT rx = socket(AF_INET, SOCK_DGRAM, 0);
    if (rx == BAD_SOCK)
    {
        error = "SAP: socket failed";
        return false;
    }
    int one = 1;
    setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, (const char*) &one, sizeof one);
#if defined(SO_REUSEPORT)
    setsockopt(rx, SOL_SOCKET, SO_REUSEPORT, (const char*) &one, sizeof one);
#endif
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(rx, (sockaddr*) &addr, sizeof addr) != 0)
    {
        // Another program holds 9875 without sharing it. Announcing still
        // works; hearing other devices' announcements does not.
        error = "SAP: port 9875 is held by another program; Inlet announces its senders but cannot hear others'";
        closesock(rx);
        return false;
    }
    ip_mreq m {};
    inet_pton(AF_INET, kGroup, &m.imr_multiaddr);
    m.imr_interface.s_addr = ifaceAddr;
    setsockopt(rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*) &m, sizeof m);
    rxSock = (intptr_t) rx;
    return true;
}

void Sap::stop()
{
    if (!running.exchange(false))
        return;
    if (thread.joinable())
        thread.join();
    // Deletions for everything still announced: receivers drop it now, not
    // in an hour.
    std::map<std::string, Ours> all;
    {
        std::lock_guard<std::mutex> g(lock);
        all = ours;
    }
    for (auto& [id, o] : all)
        sendNow(id, o.sdp, true);
    if (rxSock.load() != -1)
        closesock((SockT) rxSock.exchange(-1));
    closesock((SockT) txSock);
}

void Sap::announce(const std::string& id, const std::string& sdp)
{
    bool send = false;
    {
        std::lock_guard<std::mutex> g(lock);
        auto& o = ours[id];
        if (o.hash == 0)
            o.hash = nextHash++;
        if (o.sdp != sdp)
        {
            o.sdp = sdp;
            o.hash = nextHash++;   // a changed session is a new message (RFC 2974)
            send = true;
        }
    }
    if (send && running)
        sendNow(id, sdp, false);
}

void Sap::withdraw(const std::string& id)
{
    Ours o;
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = ours.find(id);
        if (it == ours.end())
            return;
        o = it->second;
        ours.erase(it);
    }
    if (running)
        sendNow(id, o.sdp, true);
}

bool Sap::announcedHere(const SapPacket& p) const
{
    // Our own announcements come back to us. Another AES67 program on this
    // computer announces from the same address, so it is our sessions that
    // are recognised, not the address.
    if (p.origin != ifaceAddr)
        return false;
    std::lock_guard<std::mutex> g(lock);
    for (auto& [id, session] : ours)
        if (session.hash == p.hash && (p.deletion || session.sdp == p.payload))
            return true;
    return false;
}

uint16_t Sap::hashOf(const std::string& id) const
{
    std::lock_guard<std::mutex> g(lock);
    auto it = ours.find(id);
    return it != ours.end() ? it->second.hash : 0;
}

void Sap::sendNow(const std::string& id, const std::string& sdp, bool deletion)
{
    SapPacket p;
    p.deletion = deletion;
    p.origin = ifaceAddr;
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = ours.find(id);
        p.hash = it != ours.end() ? it->second.hash : 0;
        if (it != ours.end())
            it->second.lastSentMs = nowMs();
    }
    p.payload = sdp;
    const auto bytes = sapEncode(p);
    sockaddr_in to {};
    to.sin_family = AF_INET;
    to.sin_port = htons(kPort);
    inet_pton(AF_INET, kGroup, &to.sin_addr);
    sendto((SockT) txSock, (const char*) bytes.data(), (int) bytes.size(), 0, (sockaddr*) &to, sizeof to);
}

void Sap::run()
{
    uint8_t buf[4096];
    int64_t lastTry = nowMs();
    while (running)
    {
        // Announce-only: the port may have come free (or a sharing program
        // taken it), so try it again every few seconds.
        if (rxSock.load() == -1 && nowMs() - lastTry >= kRetryListenMs)
        {
            lastTry = nowMs();
            std::string error;
            if (openListener(error))
            {
                {
                    std::lock_guard<std::mutex> g(lock);
                    listenError.clear();
                }
                std::fprintf(stderr, "inletd: SAP: port 9875 is free now; listening\n");
            }
        }
        int ready = 0;
#if defined(_WIN32)
        WSAPOLLFD pfd { (SockT) rxSock, POLLIN, 0 };
        if (rxSock != -1)
            ready = WSAPoll(&pfd, 1, 200);
#else
        pollfd pfd { (SockT) rxSock, POLLIN, 0 };
        if (rxSock != -1)
            ready = poll(&pfd, 1, 200);
#endif
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (ready > 0 && (pfd.revents & POLLIN))
        {
            sockaddr_in from {};
            socklen_t fl = sizeof from;
            const int n = (int) recvfrom((SockT) rxSock, (char*) buf, sizeof buf, 0, (sockaddr*) &from, &fl);
            SapPacket p;
            if (n > 0 && sapDecode(buf, (size_t) n, p) && onHeard)
            {
                char o[INET_ADDRSTRLEN] = {};
                in_addr oa {};
                oa.s_addr = p.origin;
                inet_ntop(AF_INET, &oa, o, sizeof o);
                const std::string key = std::string(o) + "/" + std::to_string(p.hash);
                if (!announcedHere(p))
                    onHeard(key, p.deletion ? std::string() : p.payload);
            }
        }
        // Our sessions, every 30 s each.
        std::vector<std::pair<std::string, std::string>> due;
        {
            std::lock_guard<std::mutex> g(lock);
            const int64_t now = nowMs();
            for (auto& [id, o] : ours)
                if (now - o.lastSentMs >= kIntervalMs)
                    due.push_back({ id, o.sdp });
        }
        for (auto& [id, sdp] : due)
            sendNow(id, sdp, false);
    }
}

} // namespace inlet
