// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "check.h"

#include "Ids.h"
#include "NmosNode.h"

#include <httplib.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

using namespace inlet;
using nlohmann::json;

namespace
{

const char* kSdp =
    "v=0\r\n"
    "o=- 1 0 IN IP4 192.168.1.10\r\n"
    "s=Desk\r\n"
    "c=IN IP4 239.69.1.1/32\r\n"
    "t=0 0\r\n"
    "m=audio 5004 RTP/AVP 96\r\n"
    "a=rtpmap:96 L24/48000/8\r\n"
    "a=ptime:1\r\n"
    "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:0\r\n"
    "a=mediaclk:direct=0\r\n";

/** Two receivers, one sender, 16 device inputs. */
class FakeModel : public NodeModel
{
public:
    mutable std::mutex m;
    std::vector<ReceiverInfo> rx;
    std::vector<SenderInfo> tx;
    std::vector<InputMapCell> map = std::vector<InputMapCell>(16);
    int subscribes = 0;

    FakeModel()
    {
        for (int i = 1; i <= 2; ++i)
        {
            ReceiverInfo r;
            r.index = i;
            r.label = "Net In " + std::to_string(i);
            rx.push_back(r);
        }
        SenderInfo s;
        s.index = 1;
        s.label = "Net Out 1";
        s.group = "239.69.10.1";
        s.sending = true;
        s.channelLabels = { "Kick", "Snare" };
        tx.push_back(s);
    }
    std::vector<ReceiverInfo> receivers() const override { std::lock_guard<std::mutex> g(m); return rx; }
    std::vector<SenderInfo> senders() const override { std::lock_guard<std::mutex> g(m); return tx; }
    std::string senderSdp(int index) const override { return index == 1 ? std::string(kSdp) : std::string(); }
    bool subscribeReceiver(int index, const std::string& sdp, const std::string& senderId, std::string& error) override
    {
        std::lock_guard<std::mutex> g(m);
        aes67_sdp_t parsed;
        char err[128];
        if (!aes67_sdp_parse(sdp.c_str(), sdp.size(), &parsed, err, sizeof err))
        {
            error = err;
            return false;
        }
        auto& r = rx[(size_t) index - 1];
        r.active = true;
        r.sdpText = sdp;
        r.sdp = parsed;
        r.senderId = senderId;
        r.channels = parsed.channels;
        ++subscribes;
        return true;
    }
    void unsubscribeReceiver(int index) override
    {
        std::lock_guard<std::mutex> g(m);
        rx[(size_t) index - 1].active = false;
    }
    bool setSenderEnabled(int index, bool enabled, std::string&) override
    {
        std::lock_guard<std::mutex> g(m);
        tx[(size_t) index - 1].enabled = enabled;
        return true;
    }
    std::vector<std::string> deviceInputLabels() const override
    {
        std::vector<std::string> l;
        for (int i = 0; i < 16; ++i)
            l.push_back("Input " + std::to_string(i + 1));
        return l;
    }
    std::vector<InputMapCell> inputMap() const override { std::lock_guard<std::mutex> g(m); return map; }
    bool setInputMap(const std::vector<std::pair<int, InputMapCell>>& changes, std::string&) override
    {
        std::lock_guard<std::mutex> g(m);
        for (auto& [i, c] : changes)
            map[(size_t) i] = c;
        return true;
    }
    void upsertNmosStream(const std::string&, const std::string&, const std::string&, const std::vector<std::string>&) override {}
    void expireNmosStreamsExcept(const std::vector<std::string>&) override {}
};

NmosNode::Config config()
{
    NmosNode::Config c;
    c.label = "Inlet (test)";
    c.hostname = "test";
    c.address = "127.0.0.1";
    c.port = 8081;
    c.nodeId = "11111111-2222-4333-8444-555555555555";
    c.deviceId = "66666666-7777-4888-9999-aaaaaaaaaaaa";
    c.advertise = false;
    c.services = json::array({ { { "href", "http://127.0.0.1:8081/inlet/v1" }, { "type", "urn:x-meros:service:inlet/v1" } } });
    c.tags = { { "urn:x-meros:product", json::array({ "inlet" }) } };
    return c;
}

json get(NmosNode& node, const std::string& path, int expect = 200)
{
    NmosNode::HttpResponse r;
    CHECK(node.handleHttp("GET", path, "", r));
    CHECK_EQ(r.status, expect);
    try
    {
        return json::parse(r.body);
    }
    catch (...)
    {
        return json(r.body);
    }
}

NmosNode::HttpResponse send(NmosNode& node, const std::string& method, const std::string& path, const json& body)
{
    NmosNode::HttpResponse r;
    CHECK(node.handleHttp(method, path, body.dump(), r));
    return r;
}

} // namespace

static void identity()
{
    CHECK(md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    const auto id = derivedUuid("x");
    CHECK(id == derivedUuid("x"));
    CHECK(id[14] == '3');
    CHECK(randomUuid() != randomUuid());
}

static void nodeApi()
{
    FakeModel model;
    NmosNode node(model, config());
    const auto self = get(node, "/x-nmos/node/v1.3/self");
    CHECK(self["services"][0]["type"] == "urn:x-meros:service:inlet/v1");
    CHECK(self["tags"]["urn:x-meros:product"][0] == "inlet");
    const auto rxs = get(node, "/x-nmos/node/v1.3/receivers");
    CHECK_EQ(rxs.size(), 2u);
    CHECK(rxs[0]["caps"]["media_types"].size() == 3);
    CHECK(rxs[0]["caps"]["media_types"][2] == "audio/AM824");
    const auto senders = get(node, "/x-nmos/node/v1.3/senders");
    CHECK_EQ(senders.size(), 1u);
    CHECK(senders[0]["id"] == NmosNode::senderId(config().deviceId, 1));
    const auto sources = get(node, "/x-nmos/node/v1.3/sources");
    CHECK(sources[0]["channels"][1]["label"] == "Snare");
    // Versions are stable while nothing changes.
    CHECK(get(node, "/x-nmos/node/v1.3/self")["version"] == self["version"]);

    NmosNode::HttpResponse m;
    CHECK(node.handleHttp("GET", "/manifests/" + NmosNode::senderId(config().deviceId, 1) + ".sdp", "", m));
    CHECK_EQ(m.status, 200);
    CHECK(m.contentType == "application/sdp");
    CHECK(!node.handleHttp("GET", "/inlet/v1/status", "", m));   // not the node's
}

static void connection()
{
    FakeModel model;
    NmosNode node(model, config());
    const auto rid = NmosNode::receiverId(config().deviceId, 1);
    const std::string base = "/x-nmos/connection/v1.1/single/receivers/" + rid;

    // Immediate activation with a transport file, the group moved by params.
    json patch = { { "sender_id", "abc" }, { "master_enable", true },
                   { "transport_file", { { "data", kSdp }, { "type", "application/sdp" } } },
                   { "transport_params", json::array({ { { "multicast_ip", "239.69.2.2" } } }) },
                   { "activation", { { "mode", "activate_immediate" } } } };
    auto r = send(node, "PATCH", base + "/staged", patch);
    CHECK_EQ(r.status, 200);
    CHECK_EQ(model.subscribes, 1);
    CHECK(std::string(model.rx[0].sdp.connection_ip) == "239.69.2.2");
    auto active = get(node, base + "/active");
    CHECK(active["master_enable"] == true);
    CHECK(active["sender_id"] == "abc");
    // The staged activation clears after an immediate one.
    CHECK(get(node, base + "/staged")["activation"]["mode"].is_null());

    // A bad body and a bad mode are refused.
    NmosNode::HttpResponse bad;
    node.handleHttp("PATCH", base + "/staged", "{not json", bad);
    CHECK_EQ(bad.status, 400);
    r = send(node, "PATCH", base + "/staged", json { { "activation", { { "mode", "whenever" } } } });
    CHECK_EQ(r.status, 400);

    // Disable.
    r = send(node, "PATCH", base + "/staged", json { { "master_enable", false }, { "activation", { { "mode", "activate_immediate" } } } });
    CHECK_EQ(r.status, 200);
    CHECK(!model.rx[0].active);

    // Scheduled, 100 ms out: accepted (202) now, done by the node's thread.
    node.start();
    r = send(node, "PATCH", base + "/staged",
             json { { "master_enable", true }, { "transport_file", { { "data", kSdp }, { "type", "application/sdp" } } },
                    { "activation", { { "mode", "activate_scheduled_relative" }, { "requested_time", "0:100000000" } } } });
    CHECK_EQ(r.status, 202);
    CHECK(!model.rx[0].active);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(model.rx[0].active);
    node.stop();

    // Bulk: one good, one unknown.
    const auto rid2 = NmosNode::receiverId(config().deviceId, 2);
    json list = json::array({ { { "id", rid2 }, { "params", { { "master_enable", true },
                                                              { "transport_file", { { "data", kSdp }, { "type", "application/sdp" } } },
                                                              { "activation", { { "mode", "activate_immediate" } } } } } },
                              { { "id", "nope" }, { "params", json::object() } } });
    r = send(node, "POST", "/x-nmos/connection/v1.1/bulk/receivers", list);
    CHECK_EQ(r.status, 200);
    const auto results = json::parse(r.body);
    CHECK(results[0]["code"] == 200);
    CHECK(results[1]["code"] == 404);
    CHECK(model.rx[1].active);
}

static void channelMapping()
{
    FakeModel model;
    NmosNode node(model, config());
    const auto io = get(node, "/x-nmos/channelmapping/v1.0/io");
    CHECK(io["inputs"].contains("rx1"));
    CHECK_EQ(io["outputs"]["inputs"]["channels"].size(), 16u);

    // Receiver 1 channels 0-1 onto device inputs 4-5 (an idle receiver maps
    // ahead of its stream, within its capacity).
    json req = { { "activation", { { "mode", "activate_immediate" } } },
                 { "actions", { { "inputs", { { "4", { { "input", "rx1" }, { "channel_index", 0 } } },
                                              { "5", { { "input", "rx1" }, { "channel_index", 1 } } } } } } } };
    auto r = send(node, "POST", "/x-nmos/channelmapping/v1.0/map/activations", req);
    CHECK_EQ(r.status, 200);
    CHECK_EQ(model.map[4].receiver, 1);
    CHECK_EQ(model.map[5].channel, 1);
    const auto active = get(node, "/x-nmos/channelmapping/v1.0/map/active");
    CHECK(active["map"]["inputs"]["4"]["input"] == "rx1");
    CHECK(active["map"]["inputs"]["0"]["input"].is_null());

    // All or nothing: one bad cell refuses the lot.
    json badReq = { { "activation", { { "mode", "activate_immediate" } } },
                    { "actions", { { "inputs", { { "6", { { "input", "rx1" }, { "channel_index", 0 } } },
                                                 { "99", { { "input", "rx1" }, { "channel_index", 0 } } } } } } } };
    r = send(node, "POST", "/x-nmos/channelmapping/v1.0/map/activations", badReq);
    CHECK_EQ(r.status, 400);
    CHECK_EQ(model.map[6].receiver, 0);
}

static void transportParams()
{
    const auto out = NmosNode::withTransportParams(
        "v=0\r\nc=IN IP4 239.1.1.1/32\r\nm=audio 5004 RTP/AVP 96\r\na=source-filter: incl IN IP4 239.1.1.1 10.0.0.1\r\n",
        json { { "multicast_ip", "239.2.2.2" }, { "destination_port", 5010 }, { "source_ip", "10.0.0.9" } });
    CHECK(out.find("c=IN IP4 239.2.2.2/32") != std::string::npos);
    CHECK(out.find("m=audio 5010 RTP/AVP 96") != std::string::npos);
    CHECK(out.find("a=source-filter: incl IN IP4 239.2.2.2 10.0.0.9") != std::string::npos);
    int64_t ms = 0;
    CHECK(NmosNode::parseTaiTime(json("12:500000000"), ms));
    CHECK_EQ(ms, 12500);
    CHECK(!NmosNode::parseTaiTime(json("12"), ms));
}

/** A registry that records what it is told: on stop, the node deletes what it
    registered, children first and the node last. */
static void unregisterOnStop()
{
    httplib::Server registry;
    std::mutex m;
    std::vector<std::string> posted, deleted;
    registry.Post("/x-nmos/registration/v1.3/resource", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(m);
        posted.push_back(json::parse(req.body)["type"]);
        res.status = 201;
        res.set_content("{}", "application/json");
    });
    registry.Post(R"(/x-nmos/registration/v1.3/health/nodes/(.+))", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content("{}", "application/json");
    });
    registry.Delete(R"(/x-nmos/registration/v1.3/resource/(\w+)/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(m);
        deleted.push_back(req.matches[1].str() + "/" + req.matches[2].str());
        res.status = 204;
    });
    const int port = registry.bind_to_any_port("127.0.0.1");
    std::thread serve([&] { registry.listen_after_bind(); });

    FakeModel model;
    auto c = config();
    c.registryUrl = "http://127.0.0.1:" + std::to_string(port);
    NmosNode node(model, c);
    node.start();
    for (int i = 0; i < 100; ++i)
    {
        {
            std::lock_guard<std::mutex> g(m);
            if (posted.size() >= 7)
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    node.stop();
    registry.stop();
    serve.join();

    CHECK_EQ(posted.size(), 7u);   // node, device, source, flow, sender, 2 receivers
    CHECK_EQ(deleted.size(), 7u);
    if (deleted.size() == 7)
    {
        CHECK(deleted[0].rfind("receivers/", 0) == 0);
        CHECK(deleted[2].rfind("senders/", 0) == 0);
        CHECK(deleted[5].rfind("devices/", 0) == 0);
        CHECK(deleted[6] == "nodes/" + c.nodeId);
    }
}

int main()
{
    identity();
    nodeApi();
    connection();
    channelMapping();
    transportParams();
    unregisterOnStop();
    TEST_MAIN_END();
}
