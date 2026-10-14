// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "check.h"
#include "inlet/Clock.h"
#include "inlet/NetTx.h"

#include <aes67/netrx.h>
#include <aes67/rtp.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace inlet;

namespace
{

struct Sent
{
    aes67_rtp_header_t h;
    std::vector<int32_t> samples;   // interleaved
};

std::unique_ptr<aes67_ring_t[]> makeRings(int n)
{
    std::unique_ptr<aes67_ring_t[]> r(new aes67_ring_t[(size_t) n]);
    for (int i = 0; i < n; ++i)
        aes67_ring_init(&r[i]);
    return r;
}

/** A free-running clock: PTP time is the local clock plus a constant, so a
    test can say exactly which sample "now" is. */
ClockSample freeRun()
{
    ClockSample c;
    c.monoNs = 0;
    c.ptpNs = 1'780'000'000'000'000'000LL;   // a whole number of 48 kHz samples
    c.rate = 1.0;
    return c;
}

int64_t monoForSample(const ClockSample& c, uint64_t index)
{
    return c.monoAt(ptpAtSample(index, 48000));
}

} // namespace

/** Packets leave once their last sample is past (plus the send offset), carry
    the ring's samples, and are stamped with the media clock. */
static void pacesOnTheMediaClock()
{
    ClockMap map;
    const ClockSample c = freeRun();
    map.publish(c);
    std::vector<Sent> sent;
    auto tx = NetTx::openDetached(map, [&](int, uint32_t, uint16_t, const uint8_t* p, size_t len) {
        Sent s;
        aes67_rtp_parse_header(p, len, &s.h);
        for (size_t o = AES67_RTP_HEADER_BYTES; o + 3 <= len; o += 3)
            s.samples.push_back(aes67_l24_to_i32(p + o));
        sent.push_back(s);
    });
    tx->setSendOffsetUs(0);

    auto rings = makeRings(2);
    TxStreamConfig cfg;
    cfg.group = 0x010145EF;   // 239.69.1.1 in network order
    cfg.channels = 2;
    cfg.framesPerPacket = 48;
    cfg.rings = rings.get();
    std::string err;
    const int slot = tx->addStream(cfg, err);
    CHECK(slot >= 0);

    const uint64_t base = sampleAt(c.ptpNs, 48000);   // "now" at mono 0
    // The host has written 10 ms ahead: channel 0 counts up, channel 1 down.
    for (uint64_t i = base; i < base + 480; ++i)
    {
        aes67_ring_write(&rings[0], (uint32_t) i, (int32_t) (i - base));
        aes67_ring_write(&rings[1], (uint32_t) i, -(int32_t) (i - base));
    }

    // At sample base+10 nothing is complete yet: the first packet is the one
    // containing now, [base, base+48), due at base+48.
    tx->pump(monoForSample(c, base + 10));
    CHECK(sent.empty());
    const int64_t due = tx->pump(monoForSample(c, base + 47));
    CHECK(sent.empty());
    CHECK_EQ(due, monoForSample(c, base + 48));
    tx->pump(monoForSample(c, base + 48));
    CHECK_EQ(sent.size(), 1u);
    // Three more packet-times pass at once: three packets, in order.
    tx->pump(monoForSample(c, base + 48 * 4 + 5));
    CHECK_EQ(sent.size(), 4u);
    for (size_t k = 0; k < sent.size(); ++k)
    {
        CHECK_EQ(sent[k].h.timestamp, (uint32_t) (base + 48 * k));
        CHECK_EQ((uint16_t) (sent[k].h.sequence - sent[0].h.sequence), (uint16_t) k);
        CHECK_EQ(sent[k].samples.size(), 96u);
        CHECK_EQ(sent[k].samples[0], (int32_t) (48 * k));
        CHECK_EQ(sent[k].samples[1], -(int32_t) (48 * k));
        CHECK_EQ(sent[k].samples[94], (int32_t) (48 * k + 47));
    }
    CHECK_EQ(tx->stats(slot).silentPackets, 0u);

    // Past what the host wrote: silence, still on time.
    tx->pump(monoForSample(c, base + 48 * 12));
    CHECK_EQ(sent.size(), 12u);
    CHECK_EQ(sent.back().samples[0], 0);
    CHECK_EQ(tx->stats(slot).silentPackets, 2u);

    // A stall of a second: no burst. One packet, the rest counted as skipped.
    const size_t before = sent.size();
    tx->pump(monoForSample(c, base + 48 * 12 + 48000));
    CHECK_EQ(sent.size(), before + 1);
    CHECK_EQ(tx->stats(slot).skipped, 999u);
    CHECK_EQ(sent.back().h.timestamp, (uint32_t) (base + 48 * 12 + 48000 - 48));

    // The send offset delays the packet, not its timestamp.
    tx->setSendOffsetUs(500);   // 24 samples
    const uint64_t next = base + 48 * 12 + 48000;
    tx->pump(monoForSample(c, next + 48 + 23));
    CHECK_EQ(sent.size(), before + 1);
    tx->pump(monoForSample(c, next + 48 + 24));
    CHECK_EQ(sent.size(), before + 2);
    CHECK_EQ(sent.back().h.timestamp, (uint32_t) next);
}

/** Two hosts writing outputs: the packet carries their sum, clipped; a layer
    not in use, or missing samples, adds nothing. */
static void mixesLayers()
{
    ClockMap map;
    const ClockSample c = freeRun();
    map.publish(c);
    std::vector<Sent> sent;
    auto tx = NetTx::openDetached(map, [&](int, uint32_t, uint16_t, const uint8_t* p, size_t len) {
        Sent s;
        aes67_rtp_parse_header(p, len, &s.h);
        for (size_t o = AES67_RTP_HEADER_BYTES; o + 3 <= len; o += 3)
            s.samples.push_back(aes67_l24_to_i32(p + o));
        sent.push_back(s);
    });
    tx->setSendOffsetUs(0);

    // Three layers of one channel; layer 2 is written but not in use.
    auto rings = makeRings(3);
    std::atomic<uint32_t> inUse[3] { { 1 }, { 2 }, { 0 } };
    TxStreamConfig cfg;
    cfg.group = 0x010145EF;
    cfg.channels = 1;
    cfg.rings = rings.get();
    cfg.layers = 3;
    cfg.layerStride = 1;
    cfg.layerInUse = inUse;
    std::string err;
    const int slot = tx->addStream(cfg, err);
    CHECK(slot >= 0);

    const uint64_t base = sampleAt(c.ptpNs, 48000);
    for (uint64_t i = base; i < base + 96; ++i)
    {
        aes67_ring_write(&rings[0], (uint32_t) i, 1000);
        aes67_ring_write(&rings[1], (uint32_t) i, i < base + 48 ? 234 : 8388000);
        aes67_ring_write(&rings[2], (uint32_t) i, 5);
    }
    // Packet 3: only layer 0 has it.
    for (uint64_t i = base + 96; i < base + 144; ++i)
        aes67_ring_write(&rings[0], (uint32_t) i, -7);

    tx->pump(monoForSample(c, base + 10));   // starts with the packet containing now
    tx->pump(monoForSample(c, base + 48 * 4));
    CHECK_EQ(sent.size(), 4u);
    if (sent.size() == 4)
    {
        CHECK_EQ(sent[0].samples[0], 1234);
        CHECK_EQ(sent[1].samples[0], 8388607);   // 8 389 000, clipped to 24 bits
        CHECK_EQ(sent[2].samples[0], -7);
        CHECK_EQ(sent[3].samples[0], 0);
    }
    CHECK_EQ(tx->stats(slot).silentPackets, 1u);   // only the last
}

static void refusesWhatDoesNotFit()
{
    ClockMap map;
    auto tx = NetTx::openDetached(map, [](int, uint32_t, uint16_t, const uint8_t*, size_t) {});
    auto rings = makeRings(16);
    TxStreamConfig cfg;
    cfg.channels = 16;   // 16 x 48 frames x 3 bytes is more than a packet holds
    cfg.framesPerPacket = 48;
    cfg.rings = rings.get();
    std::string err;
    CHECK_EQ(tx->addStream(cfg, err), -1);
    CHECK(!err.empty());
    cfg.framesPerPacket = 6;   // 125 us
    CHECK(tx->addStream(cfg, err) >= 0);
}

/** Through the sockets: what the pacer sends, the receive path files under the
    same media-clock index. */
static void loopsBackThroughTheWire()
{
    ClockMap map;
    ClockSample c;
    c.monoNs = monoNowNs();
    c.ptpNs = 1'780'000'000'000'000'000LL;
    map.publish(c);

    std::string err;
    char rxErr[160] = { 0 };
    std::unique_ptr<aes67_netrx_t, decltype(&aes67_netrx_close)> rx(aes67_netrx_open("127.0.0.1", rxErr, sizeof rxErr),
                                                                     aes67_netrx_close);
    auto tx = NetTx::open("127.0.0.1", map, err);
    CHECK(rx && tx);
    if (!rx || !tx)
        return;
    uint32_t group = 0;
    aes67_parse_ipv4("239.69.201.1", &group);

    auto txRings = makeRings(2), rxRings = makeRings(2);
    aes67_netrx_stream_cfg_t rc {};
    rc.payload_type = 96;
    rc.group = group;
    rc.port = 15006;
    rc.channels = 2;
    rc.rings = rxRings.get();
    if (aes67_netrx_add_stream(rx.get(), &rc, rxErr, sizeof rxErr) < 0)
    {
        std::fprintf(stderr, "SKIPPED loopsBackThroughTheWire: %s\n", rxErr);
        return;
    }
    TxStreamConfig tc;
    tc.group = group;
    tc.port = 15006;
    tc.channels = 2;
    tc.rings = txRings.get();
    const int slot = tx->addStream(tc, err);
    CHECK(slot >= 0);

    // The host has written 80 ms ahead (the ring holds 85 ms).
    const uint64_t now = sampleAt(c.ptpAt(monoNowNs()), 48000);
    for (uint64_t i = now; i < now + 3840; ++i)
    {
        aes67_ring_write(&txRings[0], (uint32_t) i, (int32_t) (i & 0x7FFFF));
        aes67_ring_write(&txRings[1], (uint32_t) i, -(int32_t) (i & 0x7FFFF));
    }
    aes67_netrx_start(rx.get());
    tx->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    tx->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    aes67_netrx_stop(rx.get());

    // A sample well inside what was sent arrived at its own index.
    const uint64_t probe = now + 3000;
    int32_t v[2] = {};
    CHECK_EQ(aes67_ring_read(&rxRings[0], (uint32_t) probe, v, 2), 2);
    CHECK_EQ(v[0], (int32_t) (probe & 0x7FFFF));
    CHECK_EQ(aes67_ring_read(&rxRings[1], (uint32_t) probe, v, 1), 1);
    CHECK_EQ(v[0], -(int32_t) (probe & 0x7FFFF));
    CHECK(tx->stats(slot).packets >= 80);
    CHECK_EQ(tx->stats(slot).sendErrors, 0u);
}

int main()
{
    pacesOnTheMediaClock();
    mixesLayers();
    refusesWhatDoesNotFit();
    loopsBackThroughTheWire();
    TEST_MAIN_END();
}
