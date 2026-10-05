// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "check.h"
#include "inlet/Clock.h"
#include "inlet/NetRx.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <aes67/rtp.h>
#include <aes67/rx.h>

using namespace inlet;

static uint32_t ip(const char* s)
{
    uint32_t v = 0;
    parseIpv4(s, v);
    return v;
}

static std::vector<uint8_t> l24Packet(uint8_t pt, uint16_t seq, uint32_t ts, const std::vector<int32_t>& interleaved,
                                      int channels)
{
    std::vector<uint8_t> p(1500);
    aes67_rtp_header_t h {};
    h.payload_type = pt;
    h.sequence = seq;
    h.timestamp = ts;
    h.ssrc = 1;
    const int frames = (int) interleaved.size() / channels;
    p.resize(aes67_rtp_pack_l24(p.data(), p.size(), &h, interleaved.data(), frames, channels));
    return p;
}

struct Rings
{
    explicit Rings(int n) : rings(new aes67_ring_t[(size_t) n])
    {
        for (int i = 0; i < n; ++i)
            aes67_ring_init(&rings[i]);
    }
    std::unique_ptr<aes67_ring_t[]> rings;
};

/** Two streams on one port, told apart by destination; a third by source. */
static void demultiplexesOnePort()
{
    std::string err;
    auto rx = NetRx::open("", err);
    CHECK(rx != nullptr);
    if (!rx)
        return;

    Rings a(2), b(1), c(1);
    RxStreamConfig ca;
    ca.group = ip("239.69.1.1");
    ca.channels = 2;
    ca.rings = a.rings.get();
    ca.mediaClockOffset = 1000;
    RxStreamConfig cb = ca;
    cb.group = ip("239.69.1.2");
    cb.channels = 1;
    cb.rings = b.rings.get();
    cb.mediaClockOffset = 0;
    RxStreamConfig cc = cb;
    cc.group = ip("232.1.1.1");
    cc.source = ip("192.168.1.20");
    cc.rings = c.rings.get();

    // Socket-free: the slots are configured without joining anything.
    const int sa = rx->addStream(ca, err), sb = rx->addStream(cb, err), sc = rx->addStream(cc, err);
    if (sa < 0 || sb < 0 || sc < 0)
    {
        std::fprintf(stderr, "addStream: %s (no multicast on this machine?)\n", err.c_str());
        CHECK(!"addStream");
        return;
    }

    const uint32_t src = ip("192.168.1.10");
    auto pa = l24Packet(96, 1, 5000, { 100, -100, 200, -200 }, 2);
    rx->inject(src, ip("239.69.1.1"), 5004, pa.data(), pa.size(), 1);
    auto pb = l24Packet(96, 7, 9000, { 42, 43 }, 1);
    rx->inject(src, ip("239.69.1.2"), 5004, pb.data(), pb.size(), 2);
    // Same group, other port: nobody's.
    rx->inject(src, ip("239.69.1.2"), 5006, pb.data(), pb.size(), 3);
    // SSM: only from its source.
    auto pc = l24Packet(96, 1, 77, { 9 }, 1);
    rx->inject(src, ip("232.1.1.1"), 5004, pc.data(), pc.size(), 4);
    rx->inject(ip("192.168.1.20"), ip("232.1.1.1"), 5004, pc.data(), pc.size(), 5);

    int32_t out[2] = {};
    // Indexed by the media clock: RTP 5000 less the stream's offset of 1000.
    CHECK_EQ(aes67_ring_read(&a.rings[0], 4000, out, 2), 2);
    CHECK_EQ(out[0], 100);
    CHECK_EQ(out[1], 200);
    CHECK_EQ(aes67_ring_read(&a.rings[1], 4000, out, 2), 2);
    CHECK_EQ(out[0], -100);
    CHECK_EQ(out[1], -200);
    CHECK_EQ(aes67_ring_read(&b.rings[0], 9000, out, 2), 2);
    CHECK_EQ(out[1], 43);
    CHECK_EQ(rx->stats(sb).packets, 1u);
    CHECK_EQ(rx->stats(sc).packets, 1u);
    CHECK_EQ(aes67_ring_read(&c.rings[0], 77, out, 1), 1);
    CHECK_EQ(out[0], 9);

    // Loss, reorder and a wrong payload type are counted.
    auto p2 = l24Packet(96, 4, 5012, { 1, 1, 1, 1 }, 2);   // 2, 3 missing
    rx->inject(src, ip("239.69.1.1"), 5004, p2.data(), p2.size(), 10);
    auto p3 = l24Packet(96, 2, 5004, { 2, 2, 2, 2 }, 2);   // late
    rx->inject(src, ip("239.69.1.1"), 5004, p3.data(), p3.size(), 11);
    auto bad = l24Packet(97, 5, 5016, { 3, 3 }, 2);
    rx->inject(src, ip("239.69.1.1"), 5004, bad.data(), bad.size(), 12);
    const RxStreamStats st = rx->stats(sa);
    CHECK_EQ(st.packets, 3u);
    CHECK_EQ(st.lost, 2u);
    CHECK_EQ(st.reordered, 1u);
    CHECK_EQ(st.wrongPayload, 1u);

    rx->removeStream(sb);
    auto pb2 = l24Packet(96, 8, 9002, { 50 }, 1);
    rx->inject(src, ip("239.69.1.2"), 5004, pb2.data(), pb2.size(), 13);
    CHECK_EQ(aes67_ring_read(&b.rings[0], 9002, out, 1), 0);
}

/** Real datagrams through the socket path, on the loopback interface. */
static void receivesFromTheWire()
{
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    std::string err;
    auto rx = NetRx::open("127.0.0.1", err);
    CHECK(rx != nullptr);
    if (!rx)
        return;
    Rings r1(1), r2(1);
    RxStreamConfig c1;
    c1.group = ip("239.69.200.1");
    c1.port = 15004;
    c1.channels = 1;
    c1.rings = r1.rings.get();
    RxStreamConfig c2 = c1;
    c2.group = ip("239.69.200.2");
    c2.rings = r2.rings.get();
    const int s1 = rx->addStream(c1, err);
    const int s2 = s1 >= 0 ? rx->addStream(c2, err) : -1;
    if (s1 < 0 || s2 < 0)
    {
        std::fprintf(stderr, "SKIPPED receivesFromTheWire: %s\n", err.c_str());
        return;
    }
    rx->start();

    auto tx = socket(AF_INET, SOCK_DGRAM, 0);
    in_addr lo {};
    lo.s_addr = ip("127.0.0.1");
    setsockopt(tx, IPPROTO_IP, IP_MULTICAST_IF, (const char*) &lo, sizeof lo);
    int one = 1;
    setsockopt(tx, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*) &one, sizeof one);
    auto send = [&](const char* group, const std::vector<uint8_t>& p) {
        sockaddr_in to {};
        to.sin_family = AF_INET;
        to.sin_port = htons(15004);
        to.sin_addr.s_addr = ip(group);
        sendto(tx, (const char*) p.data(), (int) p.size(), 0, (sockaddr*) &to, sizeof to);
    };
    for (uint16_t i = 0; i < 20; ++i)
    {
        send("239.69.200.1", l24Packet(96, i, 48u * i, std::vector<int32_t>(48, 1000 + i), 1));
        send("239.69.200.2", l24Packet(96, i, 48u * i, std::vector<int32_t>(48, -1000 - i), 1));
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((rx->stats(s1).packets < 20 || rx->stats(s2).packets < 20) && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_EQ(rx->stats(s1).packets, 20u);
    CHECK_EQ(rx->stats(s2).packets, 20u);
    CHECK_EQ(rx->stats(s1).lost, 0u);
    int32_t v = 0;
    CHECK_EQ(aes67_ring_read(&r1.rings[0], 48 * 19, &v, 1), 1);
    CHECK_EQ(v, 1019);
    CHECK_EQ(aes67_ring_read(&r2.rings[0], 48 * 19, &v, 1), 1);
    CHECK_EQ(v, -1019);
#if defined(_WIN32)
    closesocket(tx);
#else
    close(tx);
#endif
    rx->removeStream(s1);
    rx.reset();
}

int main()
{
    demultiplexesOnePort();
    receivesFromTheWire();
    TEST_MAIN_END();
}
