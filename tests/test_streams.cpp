// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "check.h"

#include "NmosNode.h"
#include "Sap.h"
#include "StreamManager.h"

#include "inlet/SharedRegion.h"

#include <cstring>
#include <string>

#if defined(_WIN32)
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

using namespace inlet;

static const char* kForeignSdp =
    "v=0\r\n"
    "o=- 7 0 IN IP4 192.168.1.50\r\n"
    "s=Stagebox\r\n"
    "c=IN IP4 239.69.77.1/32\r\n"
    "t=0 0\r\n"
    "m=audio 5004 RTP/AVP 97\r\n"
    "a=rtpmap:97 L24/48000/4\r\n"
    "a=ptime:1\r\n"
    "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-00-00-01:0\r\n"
    "a=mediaclk:direct=0\r\n";

static void sap()
{
    SapPacket p;
    p.hash = 0x1234;
    p.origin = 0x0100A8C0;   // 192.168.0.1
    p.payload = "v=0\r\ns=x\r\n";
    const auto bytes = sapEncode(p);
    SapPacket q;
    CHECK(sapDecode(bytes.data(), bytes.size(), q));
    CHECK_EQ(q.hash, 0x1234);
    CHECK(q.payload == p.payload);
    CHECK(!q.deletion);
    p.deletion = true;
    const auto del = sapEncode(p);
    CHECK(sapDecode(del.data(), del.size(), q));
    CHECK(q.deletion);
    // Implicit SDP (no MIME type) is understood too.
    std::vector<uint8_t> bare = { 0x20, 0, 0, 1, 10, 0, 0, 1 };
    const std::string body = "v=0\r\n";
    bare.insert(bare.end(), body.begin(), body.end());
    CHECK(sapDecode(bare.data(), bare.size(), q));
    CHECK(q.payload == body);

    // Our own announcements are recognised by session, not by address:
    // another AES67 program on this computer announces from it too.
    Sap here("192.168.0.1");   // not started: announcing only records
    here.announce("own:1", p.payload);
    SapPacket echo;
    echo.origin = 0x0100A8C0;
    echo.hash = here.hashOf("own:1");
    echo.payload = p.payload;
    CHECK(echo.hash != 0);
    CHECK(here.announcedHere(echo));
    SapPacket neighbour = echo;
    neighbour.hash = (uint16_t) (echo.hash + 1);
    neighbour.payload = "v=0\r\ns=another program\r\n";
    CHECK(!here.announcedHere(neighbour));
    neighbour.hash = echo.hash;   // the same hash by chance, another session
    CHECK(!here.announcedHere(neighbour));
    SapPacket goodbye = echo;
    goodbye.deletion = true;
    goodbye.payload.clear();
    CHECK(here.announcedHere(goodbye));
    SapPacket elsewhere = echo;
    elsewhere.origin = 0x0200A8C0;   // 192.168.0.2
    CHECK(!here.announcedHere(elsewhere));
}

static void streams()
{
    const std::string name = "InletStreamsTest." + std::to_string(getpid());
    std::string err;
    shm::Region::Layout layout;
    layout.inputs = 16;
    layout.outputs = 16;
    layout.receivers = 2;
    layout.receiverChannels = 8;
    auto region = shm::Region::create(layout, err, name.c_str());
    CHECK(region != nullptr);
    if (!region)
        return;
    ClockSample free;
    free.monoNs = monoNowNs();
    free.ptpNs = 1'780'000'000'000'000'000LL;
    region->clock().publish(free);

    char rxErr[160] = { 0 };
    std::unique_ptr<aes67_netrx_t, decltype(&aes67_netrx_close)> rx(aes67_netrx_open("", rxErr, sizeof rxErr), aes67_netrx_close);
    auto tx = NetTx::openDetached(region->clock(), [](int, uint32_t, uint16_t, const uint8_t*, size_t) {});
    CHECK(rx && tx);
    if (!rx || !tx)
        return;
    StreamManager::Settings ss;
    ss.ifaceIp = "192.168.1.216";
    ss.ifaceMac = "e0-d4-e8-78-37-fa";
    ss.senders = 4;
    ss.senderChannels = 8;
    StreamManager m(*region, rx.get(), *tx, nullptr, ss);
    m.start();

    // Senders: as many as the outputs hold, contiguous, named, grouped.
    const auto senders = m.senders();
    CHECK_EQ(senders.size(), 2u);
    CHECK(senders[1].group == "239.69.216.2");
    const std::string sdp = m.senderSdp(1);
    // Free-running: the SDP says the stream is on this computer's own clock.
    CHECK(sdp.find("a=ts-refclk:localmac=E0-D4-E8-78-37-FA") != std::string::npos);

    // The default map: receiver 1's channels on inputs 1-8, receiver 2's on 9-16.
    auto map = m.inputMap();
    CHECK_EQ(map[0].receiver, 1);
    CHECK_EQ(map[9].receiver, 2);
    CHECK_EQ(map[9].channel, 1);

    // Our own stream plays direct; a stream on another clock is bridged.
    CHECK(m.subscribeReceiver(1, sdp, "", err));
    CHECK(m.subscribeReceiver(2, kForeignSdp, "abc", err));
    auto rjson = m.receiversJson();
    CHECK(rjson[0]["health"]["bridged"] == false);
    CHECK(rjson[1]["health"]["bridged"] == true);
    CHECK_EQ(m.receivers()[1].channels, 4);

    // Another rate the bridge plays is accepted (and bridged); one it does not is refused.
    std::string at96 = kForeignSdp;
    at96.replace(at96.find("L24/48000/4"), 11, "L24/96000/4");
    at96.replace(at96.find("239.69.77.1"), 11, "239.69.77.9");   // not receiver 2's stream
    CHECK(m.subscribeReceiver(1, at96, "", err));
    CHECK(m.receiversJson()[0]["health"]["bridged"] == true);
    std::string at32 = kForeignSdp;
    at32.replace(at32.find("L24/48000/4"), 11, "L24/32000/4");
    CHECK(!m.subscribeReceiver(1, at32, "", err));
    CHECK(err.find("32000") != std::string::npos);
    CHECK(m.subscribeReceiver(1, sdp, "", err));

    // Too many channels for a slot: refused, with the reason.
    std::string big = kForeignSdp;
    big.replace(big.find("L24/48000/4"), 11, "L24/48000/16");
    CHECK(!m.subscribeReceiver(1, big, "", err));
    CHECK(err.find("8") != std::string::npos);

    // Labels from a console: receiver 2's channel names land on inputs 9-12.
    nlohmann::json labels = { { "receivers", { { { "id", "r2" }, { "channels", { "Kick", "Snare", "Hat", "Tom" } } } } },
                              { "senders", { { { "id", "s1" }, { "channels", { "Mix L", "Mix R" } } } } } };
    CHECK(m.applyLabels(
        labels, [](const std::string& id) { return id == "r2" ? 2 : 0; }, [](const std::string& id) { return id == "s1" ? 1 : 0; },
        err));
    CHECK(m.deviceInputLabels()[8] == "Kick");
    CHECK(m.deviceInputLabels()[11] == "Tom");
    CHECK(std::string(region->inputLabel(9)) == "Snare");
    CHECK(std::string(region->outputLabel(1)) == "Mix R");
    CHECK(m.senders()[0].channelLabels[0] == "Mix L");

    // Each PUT replaces the whole set: a receiver left out loses its names.
    nlohmann::json fewer = { { "receivers", nlohmann::json::array() },
                             { "senders", { { { "id", "s1" }, { "channels", { "", "Mix R" } } } } } };
    CHECK(m.applyLabels(
        fewer, [](const std::string& id) { return id == "r2" ? 2 : 0; }, [](const std::string& id) { return id == "s1" ? 1 : 0; },
        err));
    CHECK(m.deviceInputLabels()[8] == "Input 9");
    CHECK(std::string(region->outputLabel(0)).empty());
    CHECK(std::string(region->outputLabel(1)) == "Mix R");
    // Re-apply the first set for what follows.
    m.applyLabels(
        labels, [](const std::string& id) { return id == "r2" ? 2 : 0; }, [](const std::string& id) { return id == "s1" ? 1 : 0; },
        err);

    // Health in the spec's shape, present for idle slots too.
    for (const char* key : { "receiving", "packets", "lost", "reordered", "wrongPayload", "jitterWorstNs", "resyncs", "underruns",
                             "latencyFrames" })
        CHECK(m.receiversJson()[0]["health"].contains(key));
    CHECK(m.sendersJson()[0]["health"]["sending"] == true);
    CHECK(m.sendersJson()[0]["health"].contains("packets"));

    // The map refuses what does not exist, all or nothing.
    CHECK(!m.setInputMap({ { 0, { 3, 0 } } }, err));
    CHECK(m.setInputMap({ { 0, { 2, 3 } }, { 1, { 0, 0 } } }, err));
    map = m.inputMap();
    CHECK_EQ(map[0].receiver, 2);
    CHECK_EQ(map[1].receiver, 0);

    // What persists comes back.
    const auto state = m.persistentState();
    CHECK_EQ(state["receivers"].size(), 2u);
    m.stop();
    StreamManager again(*region, rx.get(), *tx, nullptr, ss);
    again.restoreState(state);
    CHECK(again.receivers()[1].active);
    CHECK(again.receivers()[1].senderId == "abc");
    CHECK(again.deviceInputLabels()[8] == "Kick");
    CHECK_EQ(again.inputMap()[0].receiver, 2);
    again.stop();
}

int main()
{
    sap();
    streams();
    TEST_MAIN_END();
}
