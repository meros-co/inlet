// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "NmosNode.h"

#include "HttpClient.h"
#include "Ids.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <mdns/mdns.h>

namespace inlet
{

namespace
{
constexpr const char* kTransport = "urn:x-nmos:transport:rtp.mcast";
constexpr const char* kFormatAudio = "urn:x-nmos:format:audio";
constexpr const char* kConnVersions[] = { "v1.0", "v1.1" };
constexpr int64_t kHeartbeatMs = 5000;   // IS-04: every 5 s; a registry gives up at 12
constexpr int64_t kBrowseMs = 10000;
constexpr int64_t kRegisterRetryMs = 5000;
constexpr int64_t kCrawlEveryMs = 10000;

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

nlohmann::json errorJson(int code, const std::string& error, const std::string& debug = {})
{
    return { { "code", code }, { "error", error }, { "debug", debug.empty() ? nlohmann::json(nullptr) : nlohmann::json(debug) } };
}

nlohmann::json nullActivation()
{
    return { { "mode", nullptr }, { "requested_time", nullptr }, { "activation_time", nullptr } };
}

std::vector<std::string> tokens(const std::string& pathIn)
{
    std::string path = pathIn.substr(0, pathIn.find('?'));
    std::vector<std::string> out;
    std::string cur;
    for (char c : path)
    {
        if (c == '/')
        {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        }
        else
            cur.push_back(c);
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : text)
    {
        if (c == '\n')
        {
            if (!cur.empty() && cur.back() == '\r')
                cur.pop_back();
            out.push_back(cur);
            cur.clear();
        }
        else
            cur.push_back(c);
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::vector<std::string> words(const std::string& line)
{
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string w;
    while (in >> w)
        out.push_back(w);
    return out;
}

std::string join(const std::vector<std::string>& parts, const char* sep)
{
    std::string s;
    for (size_t i = 0; i < parts.size(); ++i)
        s += (i ? sep : "") + parts[i];
    return s;
}

bool isNodeVersion(const std::string& v) { return v == "v1.2" || v == "v1.3"; }
bool isConnVersion(const std::string& v) { return v == "v1.0" || v == "v1.1"; }

std::string gmidDashed(const uint8_t* b)
{
    char s[32];
    std::snprintf(s, sizeof s, "%02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    return s;
}

std::string trimSlash(std::string id)
{
    while (!id.empty() && id.back() == '/')
        id.pop_back();
    return id;
}

/** "rx3" -> 3; anything else -> 0. */
int receiverIndexFrom(const std::string& inputId)
{
    const auto id = trimSlash(inputId);
    return id.rfind("rx", 0) == 0 && id.size() > 2 ? std::atoi(id.c_str() + 2) : 0;
}

constexpr const char* kDeviceOutput = "inputs";   // IS-08: the device's inputs, what a DAW records

} // namespace

/* ---- identity --------------------------------------------------------------------- */

std::string NmosNode::receiverId(const std::string& deviceId, int index) { return derivedUuid(deviceId + ":receiver:" + std::to_string(index)); }
std::string NmosNode::senderId(const std::string& deviceId, int index) { return derivedUuid(deviceId + ":sender:" + std::to_string(index)); }
std::string NmosNode::flowId(const std::string& deviceId, int index) { return derivedUuid(deviceId + ":flow:" + std::to_string(index)); }
std::string NmosNode::sourceId(const std::string& deviceId, int index) { return derivedUuid(deviceId + ":source:" + std::to_string(index)); }

std::string NmosNode::versionNow()
{
    // TAI seconds:nanoseconds. A controller only compares stamps, but TAI is
    // what IS-04 says, so the 37 s go in.
    const int64_t ms = nowMs();
    return std::to_string(ms / 1000 + 37) + ":" + std::to_string((ms % 1000) * 1000000);
}

NmosNode::NmosNode(NodeModel& m, const Config& c) : model(m), cfg(c)
{
    if (cfg.nodeId.empty())
        cfg.nodeId = randomUuid();
    if (cfg.deviceId.empty())
        cfg.deviceId = randomUuid();
    startVersion = versionNow();
}

NmosNode::~NmosNode()
{
    stop();
}

void NmosNode::start()
{
    if (cfg.advertise && dns == nullptr)
    {
        char err[160] = { 0 };
        dns = mdns_open(cfg.hostname.c_str(), cfg.address.c_str(), err, sizeof err);
        if (dns == nullptr)
            std::fprintf(stderr, "inletd: NMOS: not advertising: %s\n", err);
        else
        {
            mdns_service_t svc;
            std::memset(&svc, 0, sizeof svc);
            // Unique on the link (the library does not probe), plain in NMOS.
            const auto instance = cfg.label + " (" + cfg.hostname + ")";
            std::strncpy(svc.instance, instance.c_str(), sizeof svc.instance - 1);
            std::strncpy(svc.type, "_nmos-node._tcp", sizeof svc.type - 1);
            svc.port = (uint16_t) cfg.port;
            svc.txt_count = 3;
            std::strncpy(svc.txt[0], "api_ver=v1.2,v1.3", MDNS_TXT_LEN - 1);
            std::strncpy(svc.txt[1], "api_proto=http", MDNS_TXT_LEN - 1);
            std::strncpy(svc.txt[2], "api_auth=false", MDNS_TXT_LEN - 1);
            mdns_add_service(dns, &svc);
        }
    }
    if (!running.exchange(true))
        thread = std::thread([this] { run(); });
}

void NmosNode::stop()
{
    if (running.exchange(false))
    {
        wake();
        if (thread.joinable())
            thread.join();
    }
    if (dns != nullptr)
    {
        mdns_close(dns);   // says goodbye
        dns = nullptr;
    }
}

void NmosNode::wake()
{
    {
        std::lock_guard<std::mutex> g(wakeLock);
        wakeFlag = true;
    }
    wakeCv.notify_all();
}

void NmosNode::setClock(const Clock& c)
{
    {
        std::lock_guard<std::mutex> g(lock);
        clock = c;
    }
    dirty = true;
}

void NmosNode::resourcesChanged()
{
    dirty = true;
}

std::string NmosNode::baseHref() const
{
    return "http://" + cfg.address + ":" + std::to_string(cfg.port);
}

/* ---- resources -------------------------------------------------------------------- */

std::string NmosNode::versionFor(const std::string& id, const nlohmann::json& content) const
{
    // A version changes when the content does, and only then: a registry
    // reads a new version as "something happened here".
    const auto hash = md5Hex(content.dump());
    std::lock_guard<std::mutex> g(lock);
    auto it = versions.find(id);
    if (it == versions.end())
    {
        versions[id] = { hash, startVersion };
        return startVersion;
    }
    if (it->second.first != hash)
        it->second = { hash, versionNow() };
    return it->second.second;
}

nlohmann::json NmosNode::self() const
{
    nlohmann::json j;
    j["id"] = cfg.nodeId;
    j["label"] = cfg.label;
    j["description"] = cfg.description;
    j["tags"] = cfg.tags;
    j["href"] = baseHref() + "/";
    j["hostname"] = cfg.hostname;
    j["caps"] = nlohmann::json::object();
    j["api"] = { { "versions", nlohmann::json::array({ "v1.2", "v1.3" }) },
                 { "endpoints", nlohmann::json::array({ { { "host", cfg.address }, { "port", cfg.port }, { "protocol", "http" } } }) } };
    j["services"] = cfg.services;
    {
        std::lock_guard<std::mutex> g(lock);
        if (clock.ptp)
            j["clocks"] = nlohmann::json::array({ { { "name", "clk0" }, { "ref_type", "ptp" }, { "traceable", clock.locked },
                                                    { "version", "IEEE1588-2008" }, { "gmid", gmidDashed(clock.gmIdentity) },
                                                    { "locked", clock.locked } } });
        else
            j["clocks"] = nlohmann::json::array({ { { "name", "clk0" }, { "ref_type", "internal" } } });
    }
    j["interfaces"] = nlohmann::json::array({ { { "chassis_id", cfg.chassisId.empty() ? nlohmann::json(nullptr) : nlohmann::json(cfg.chassisId) },
                                                { "port_id", cfg.chassisId.empty() ? nlohmann::json(nullptr) : nlohmann::json(cfg.chassisId) },
                                                { "name", cfg.interfaceName } } });
    j["version"] = versionFor(cfg.nodeId, j);
    return j;
}

nlohmann::json NmosNode::device() const
{
    nlohmann::json j;
    j["id"] = cfg.deviceId;
    j["label"] = cfg.label;
    j["description"] = cfg.description;
    j["tags"] = nlohmann::json::object();
    j["type"] = "urn:x-nmos:device:generic";
    j["node_id"] = cfg.nodeId;
    j["senders"] = nlohmann::json::array();
    for (auto& s : model.senders())
        j["senders"].push_back(senderId(cfg.deviceId, s.index));
    j["receivers"] = nlohmann::json::array();
    for (auto& r : model.receivers())
        j["receivers"].push_back(receiverId(cfg.deviceId, r.index));
    j["controls"] = nlohmann::json::array();
    for (auto* v : kConnVersions)
        j["controls"].push_back({ { "href", baseHref() + "/x-nmos/connection/" + v + "/" },
                                  { "type", std::string("urn:x-nmos:control:sr-ctrl/") + v } });
    j["controls"].push_back({ { "href", baseHref() + "/x-nmos/channelmapping/v1.0/" },
                              { "type", "urn:x-nmos:control:cm-ctrl/v1.0" } });
    j["version"] = versionFor(cfg.deviceId, j);
    return j;
}

nlohmann::json NmosNode::sources() const
{
    auto out = nlohmann::json::array();
    for (auto& s : model.senders())
        out.push_back(resource("source", s.index));
    return out;
}

nlohmann::json NmosNode::flows() const
{
    auto out = nlohmann::json::array();
    for (auto& s : model.senders())
        out.push_back(resource("flow", s.index));
    return out;
}

nlohmann::json NmosNode::senders() const
{
    auto out = nlohmann::json::array();
    for (auto& s : model.senders())
        out.push_back(resource("sender", s.index));
    return out;
}

nlohmann::json NmosNode::receivers() const
{
    auto out = nlohmann::json::array();
    for (auto& r : model.receivers())
        out.push_back(resource("receiver", r.index));
    return out;
}

nlohmann::json NmosNode::resource(const std::string& type, int index) const
{
    nlohmann::json j;
    j["tags"] = nlohmann::json::object();
    j["description"] = "";
    j["device_id"] = cfg.deviceId;

    if (type == "receiver")
    {
        const auto rs = model.receivers();
        auto it = std::find_if(rs.begin(), rs.end(), [&](const ReceiverInfo& r) { return r.index == index; });
        if (it == rs.end())
            return nullptr;
        const auto& r = *it;
        j["id"] = receiverId(cfg.deviceId, index);
        j["label"] = r.label;
        j["transport"] = kTransport;
        j["interface_bindings"] = nlohmann::json::array({ cfg.interfaceName });
        j["format"] = kFormatAudio;
        // What the receiver really plays: the three encodings, and every rate
        // the rate bridge converts (the device's own needs no conversion).
        auto rates = nlohmann::json::array();
        for (int rate : cfg.receiveRates.empty() ? std::vector<int> { cfg.sampleRate } : cfg.receiveRates)
            rates.push_back({ { "numerator", rate } });
        j["caps"] = { { "media_types", nlohmann::json::array({ "audio/L24", "audio/L16", "audio/AM824" }) },
                      { "constraint_sets", nlohmann::json::array({ {
                            { "urn:x-nmos:cap:format:sample_rate", { { "enum", rates } } },
                            { "urn:x-nmos:cap:format:channel_count", { { "maximum", r.maxChannels } } } } }) } };
        // Observed truth, which a controller trusts over its own record.
        j["subscription"] = { { "sender_id", r.active && !r.senderId.empty() ? nlohmann::json(r.senderId) : nlohmann::json(nullptr) },
                              { "active", r.active } };
        j["version"] = versionFor(j["id"].get<std::string>(), j);
        return j;
    }

    const auto ss = model.senders();
    auto it = std::find_if(ss.begin(), ss.end(), [&](const SenderInfo& s) { return s.index == index; });
    if (it == ss.end())
        return nullptr;
    const auto& s = *it;
    if (type == "sender")
    {
        j["id"] = senderId(cfg.deviceId, index);
        j["label"] = s.label;
        j["flow_id"] = flowId(cfg.deviceId, index);
        j["transport"] = kTransport;
        j["manifest_href"] = baseHref() + "/manifests/" + j["id"].get<std::string>() + ".sdp";
        j["interface_bindings"] = nlohmann::json::array({ cfg.interfaceName });
        j["subscription"] = { { "receiver_id", nullptr }, { "active", s.sending } };
        j["caps"] = nlohmann::json::object();
    }
    else if (type == "flow")
    {
        j["id"] = flowId(cfg.deviceId, index);
        j["label"] = s.label;
        j["source_id"] = sourceId(cfg.deviceId, index);
        j["parents"] = nlohmann::json::array();
        j["format"] = kFormatAudio;
        j["media_type"] = "audio/L24";
        j["sample_rate"] = { { "numerator", cfg.sampleRate } };
        j["bit_depth"] = 24;
    }
    else if (type == "source")
    {
        j["id"] = sourceId(cfg.deviceId, index);
        j["label"] = s.label;
        j["caps"] = nlohmann::json::object();
        j["parents"] = nlohmann::json::array();
        j["clock_name"] = "clk0";
        j["format"] = kFormatAudio;
        // IS-04's per-channel labels: what feeds each channel, as the DAW or
        // a controller named it.
        j["channels"] = nlohmann::json::array();
        for (int c = 0; c < s.channels; ++c)
        {
            const bool named = c < (int) s.channelLabels.size() && !s.channelLabels[(size_t) c].empty();
            j["channels"].push_back({ { "label", named ? s.channelLabels[(size_t) c] : s.label + " " + std::to_string(c + 1) } });
        }
    }
    else
        return nullptr;
    j["version"] = versionFor(j["id"].get<std::string>(), j);
    return j;
}

/* ---- IS-05 state -------------------------------------------------------------------- */

nlohmann::json NmosNode::receiverStagedJson(int index, bool active) const
{
    const auto rs = model.receivers();
    auto it = std::find_if(rs.begin(), rs.end(), [&](const ReceiverInfo& r) { return r.index == index; });
    if (it == rs.end())
        return nullptr;
    const auto& r = *it;
    nlohmann::json j;
    if (active)
    {
        // What is: the slot's own state, never the controller's memory of it.
        j["sender_id"] = r.active && !r.senderId.empty() ? nlohmann::json(r.senderId) : nlohmann::json(nullptr);
        j["master_enable"] = r.active;
        j["activation"] = nullActivation();
        j["transport_file"] = { { "data", r.active ? nlohmann::json(r.sdpText) : nlohmann::json(nullptr) },
                                { "type", r.active ? nlohmann::json("application/sdp") : nlohmann::json(nullptr) } };
        j["transport_params"] = nlohmann::json::array({ { { "source_ip", r.active ? nlohmann::json(r.sdp.origin_ip) : nlohmann::json(nullptr) },
                                                          { "multicast_ip", r.active ? nlohmann::json(r.sdp.connection_ip) : nlohmann::json(nullptr) },
                                                          { "interface_ip", cfg.address },
                                                          { "destination_port", r.active ? nlohmann::json(r.sdp.port) : nlohmann::json(nullptr) },
                                                          { "rtp_enabled", r.active } } });
    }
    else
    {
        std::lock_guard<std::mutex> g(lock);
        auto st = staged.find(index);
        const ReceiverStage s = st == staged.end() ? ReceiverStage {} : st->second;
        j["sender_id"] = s.senderId;
        j["master_enable"] = s.masterEnable;
        j["activation"] = s.activation;
        j["transport_file"] = s.transportFile;
        auto leg = s.transportParams;
        leg["interface_ip"] = cfg.address;
        j["transport_params"] = nlohmann::json::array({ leg });
    }
    return j;
}

nlohmann::json NmosNode::senderStagedJson(int index) const
{
    const auto ss = model.senders();
    auto it = std::find_if(ss.begin(), ss.end(), [&](const SenderInfo& s) { return s.index == index; });
    if (it == ss.end())
        return nullptr;
    const auto& s = *it;
    nlohmann::json j;
    j["receiver_id"] = nullptr;
    j["master_enable"] = s.enabled;
    j["activation"] = nullActivation();
    {
        std::lock_guard<std::mutex> g(lock);
        auto st = stagedSenders.find(index);
        if (st != stagedSenders.end())
        {
            if (st->second.masterEnable.is_boolean())
                j["master_enable"] = st->second.masterEnable;
            j["activation"] = st->second.activation;
        }
    }
    j["transport_params"] = nlohmann::json::array({ { { "source_ip", cfg.address },
                                                      { "destination_ip", s.group.empty() ? nlohmann::json(nullptr) : nlohmann::json(s.group) },
                                                      { "source_port", s.port }, { "destination_port", s.port },
                                                      { "rtp_enabled", s.sending } } });
    return j;
}

bool NmosNode::parseTaiTime(const nlohmann::json& t, int64_t& ms)
{
    if (!t.is_string())
        return false;
    const auto text = t.get<std::string>();
    const auto colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size())
        return false;
    const auto sec = text.substr(0, colon), ns = text.substr(colon + 1);
    auto digits = [](const std::string& s) { return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); };
    if (!digits(sec) || !digits(ns))
        return false;
    ms = std::stoll(sec) * 1000 + std::stoll(ns) / 1000000;
    return true;
}

bool NmosNode::stageActivation(const nlohmann::json& body, nlohmann::json& activation, int64_t& dueMs,
                               std::string& mode, HttpResponse& out) const
{
    // IS-05's three modes, and null to cancel what is scheduled. TAI to UTC
    // through the offset the grandmaster announces.
    mode.clear();
    dueMs = 0;
    if (!body.contains("activation") || !body["activation"].is_object())
        return true;
    const auto& a = body["activation"];
    if (!a.contains("mode"))
        return true;
    if (a["mode"].is_null())
    {
        mode = "cancel";
        return true;
    }
    if (!a["mode"].is_string())
    {
        out.status = 400;
        out.body = errorJson(400, "activation mode must be a string or null").dump();
        return false;
    }
    mode = a["mode"].get<std::string>();
    if (mode == "activate_immediate")
        return true;
    if (mode != "activate_scheduled_absolute" && mode != "activate_scheduled_relative")
    {
        out.status = 400;
        out.body = errorJson(400, "unknown activation mode " + mode).dump();
        return false;
    }
    int64_t t = 0;
    if (!parseTaiTime(a.value("requested_time", nlohmann::json(nullptr)), t))
    {
        out.status = 400;
        out.body = errorJson(400, "requested_time must be \"<seconds>:<nanoseconds>\"").dump();
        return false;
    }
    int utcOffset = 37;
    {
        std::lock_guard<std::mutex> g(lock);
        utcOffset = clock.utcOffsetS;
    }
    const int64_t now = nowMs();
    dueMs = mode == "activate_scheduled_absolute" ? t - (int64_t) utcOffset * 1000 : now + t;
    const int64_t taiDue = dueMs + (int64_t) utcOffset * 1000;
    activation = { { "mode", mode }, { "requested_time", a["requested_time"] },
                   { "activation_time", std::to_string(taiDue / 1000) + ":" + std::to_string((taiDue % 1000) * 1000000) } };
    return true;
}

std::string NmosNode::withTransportParams(const std::string& sdp, const nlohmann::json& params)
{
    // The staged leg over a session description: the group (c=), the port
    // (m=) and the source a receiver filters on (a=source-filter).
    const auto group = params.value("multicast_ip", nlohmann::json(nullptr));
    const auto port = params.value("destination_port", nlohmann::json(nullptr));
    const auto source = params.value("source_ip", nlohmann::json(nullptr));
    auto useful = [](const nlohmann::json& v) { return v.is_string() && !v.get<std::string>().empty() && v.get<std::string>() != "auto"; };
    if (!useful(group) && !port.is_number_integer() && !useful(source))
        return sdp;   // nothing staged over it: the file exactly as given
    auto ls = lines(sdp);
    for (auto& l : ls)
    {
        if (l.rfind("c=IN IP4 ", 0) == 0 && useful(group))
        {
            const auto rest = l.substr(9);
            const auto slash = rest.find('/');
            l = "c=IN IP4 " + group.get<std::string>() + (slash == std::string::npos ? std::string() : rest.substr(slash));
        }
        else if (l.rfind("m=audio ", 0) == 0 && port.is_number_integer())
        {
            auto f = words(l);
            if (f.size() > 2)
            {
                f[1] = std::to_string(port.get<int>());
                l = join(f, " ");
            }
        }
        else if (l.rfind("a=source-filter:", 0) == 0 && useful(source))
        {
            auto f = words(l);
            if (f.size() >= 6)
            {
                if (useful(group))
                    f[4] = group.get<std::string>();
                f[5] = source.get<std::string>();
                l = join(f, " ");
            }
        }
    }
    return join(ls, "\r\n") + "\r\n";
}

bool NmosNode::receiverSdp(int index, const ReceiverStage& st, std::string& sdp, std::string& error) const
{
    sdp.clear();
    if (st.transportFile["data"].is_string() && !st.transportFile["data"].get<std::string>().empty())
        sdp = st.transportFile["data"].get<std::string>();
    else if (st.senderId.is_string() && findSenderSdp(st.senderId.get<std::string>(), sdp))
        ;   // a sender this node or a peer publishes
    else
    {
        // Only new transport parameters: the stream it plays, moved.
        for (auto& r : model.receivers())
            if (r.index == index && r.active)
                sdp = r.sdpText;
    }
    if (sdp.empty())
    {
        error = "a transport_file (SDP), or a sender this node has seen, is needed to enable this receiver";
        return false;
    }
    sdp = withTransportParams(sdp, st.transportParams);
    return true;
}

int NmosNode::activateReceiver(int index, ReceiverStage& st, std::string& error)
{
    const bool enable = st.masterEnable && st.transportParams.value("rtp_enabled", true);
    if (enable)
    {
        std::string sdp;
        if (!receiverSdp(index, st, sdp, error))
            return 400;
        const std::string sid = st.senderId.is_string() ? st.senderId.get<std::string>() : std::string();
        if (!model.subscribeReceiver(index, sdp, sid, error))
            return 500;
    }
    else
        model.unsubscribeReceiver(index);
    dirty = true;
    return 200;
}

int NmosNode::activateSender(int index, SenderStage& st, std::string& error)
{
    if (st.masterEnable.is_boolean() && !model.setSenderEnabled(index, st.masterEnable.get<bool>(), error))
        return 500;
    dirty = true;
    return 200;
}

bool NmosNode::patchReceiver(int index, const nlohmann::json& body, HttpResponse& out)
{
    if (!body.is_object())
    {
        out.status = 400;
        out.body = errorJson(400, "the body must be a JSON object").dump();
        return true;
    }
    ReceiverStage st;
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = staged.find(index);
        if (it != staged.end())
            st = it->second;
    }
    if (body.contains("sender_id"))
        st.senderId = body["sender_id"].is_string() ? body["sender_id"] : nlohmann::json(nullptr);
    if (body.contains("master_enable") && body["master_enable"].is_boolean())
        st.masterEnable = body["master_enable"].get<bool>();
    if (body.contains("transport_file") && body["transport_file"].is_object())
    {
        const auto& tf = body["transport_file"];
        const auto data = tf.value("data", nlohmann::json(nullptr));
        const auto type = tf.value("type", nlohmann::json(nullptr));
        if (data.is_null() != type.is_null())
        {
            out.status = 400;
            out.body = errorJson(400, "transport_file data and type must both be set or both be null").dump();
            return true;
        }
        st.transportFile = { { "data", data }, { "type", type } };
    }
    if (body.contains("transport_params"))
    {
        // One leg: Inlet's receivers are not redundant (no ST 2022-7).
        const auto& tp = body["transport_params"];
        if (!tp.is_array() || tp.size() > 1 || (tp.size() == 1 && !tp[0].is_object()))
        {
            out.status = 400;
            out.body = errorJson(400, "transport_params must be an array of one object").dump();
            return true;
        }
        if (tp.size() == 1)
            for (auto& [k, v] : tp[0].items())
                if (k == "source_ip" || k == "multicast_ip" || k == "destination_port" || k == "rtp_enabled")
                    st.transportParams[k] = v;
    }

    nlohmann::json activation;
    int64_t due = 0;
    std::string mode;
    if (!stageActivation(body, activation, due, mode, out))
        return true;

    if (mode == "activate_immediate")
    {
        std::string error;
        const int code = activateReceiver(index, st, error);
        if (code != 200)
        {
            out.status = code;
            out.body = errorJson(code, code == 400 ? error : "the receiver could not play that stream", error).dump();
            return true;
        }
        st.activation = { { "mode", "activate_immediate" }, { "requested_time", nullptr }, { "activation_time", versionNow() } };
        {
            std::lock_guard<std::mutex> g(lock);
            staged[index] = st;
            receiverDue.erase(index);
        }
        auto j = receiverStagedJson(index, false);
        // After an immediate activation the staged activation clears (IS-05
        // 3.1): the response carries the stamp, the next GET does not.
        {
            std::lock_guard<std::mutex> g(lock);
            staged[index].activation = nullActivation();
        }
        out.status = 200;
        out.body = j.dump();
        return true;
    }
    if (mode == "activate_scheduled_absolute" || mode == "activate_scheduled_relative")
    {
        st.activation = activation;
        {
            std::lock_guard<std::mutex> g(lock);
            staged[index] = st;
            receiverDue[index] = due;
        }
        wake();
        out.status = 202;
        out.body = receiverStagedJson(index, false).dump();
        return true;
    }
    if (mode == "cancel")
        st.activation = nullActivation();
    {
        std::lock_guard<std::mutex> g(lock);
        staged[index] = st;
        if (mode == "cancel")
            receiverDue.erase(index);
    }
    out.status = 200;
    out.body = receiverStagedJson(index, false).dump();
    return true;
}

bool NmosNode::patchSender(int index, const nlohmann::json& body, HttpResponse& out)
{
    if (!body.is_object())
    {
        out.status = 400;
        out.body = errorJson(400, "the body must be a JSON object").dump();
        return true;
    }
    SenderStage st;
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = stagedSenders.find(index);
        if (it != stagedSenders.end())
            st = it->second;
    }
    if (body.contains("master_enable") && body["master_enable"].is_boolean())
        st.masterEnable = body["master_enable"];

    nlohmann::json activation;
    int64_t due = 0;
    std::string mode;
    if (!stageActivation(body, activation, due, mode, out))
        return true;

    if (mode == "activate_immediate")
    {
        std::string error;
        if (activateSender(index, st, error) != 200)
        {
            out.status = 500;
            out.body = errorJson(500, "the sender could not be switched", error).dump();
            return true;
        }
        {
            std::lock_guard<std::mutex> g(lock);
            stagedSenders.erase(index);   // what is staged is now what is active
            senderDue.erase(index);
        }
        auto j = senderStagedJson(index);
        j["activation"] = { { "mode", "activate_immediate" }, { "requested_time", nullptr }, { "activation_time", versionNow() } };
        out.status = 200;
        out.body = j.dump();
        return true;
    }
    if (mode == "activate_scheduled_absolute" || mode == "activate_scheduled_relative")
    {
        st.activation = activation;
        {
            std::lock_guard<std::mutex> g(lock);
            stagedSenders[index] = st;
            senderDue[index] = due;
        }
        wake();
        out.status = 202;
        out.body = senderStagedJson(index).dump();
        return true;
    }
    if (mode == "cancel")
        st.activation = nullActivation();
    {
        std::lock_guard<std::mutex> g(lock);
        stagedSenders[index] = st;
        if (mode == "cancel")
            senderDue.erase(index);
    }
    out.status = 200;
    out.body = senderStagedJson(index).dump();
    return true;
}

void NmosNode::runDueActivations()
{
    const int64_t now = nowMs();
    std::vector<int> receiversDue, sendersDue;
    {
        std::lock_guard<std::mutex> g(lock);
        for (auto& [i, t] : receiverDue)
            if (t <= now)
                receiversDue.push_back(i);
        for (auto& [i, t] : senderDue)
            if (t <= now)
                sendersDue.push_back(i);
        for (int i : receiversDue)
            receiverDue.erase(i);
        for (int i : sendersDue)
            senderDue.erase(i);
    }
    for (int i : receiversDue)
    {
        ReceiverStage st;
        {
            std::lock_guard<std::mutex> g(lock);
            st = staged[i];
        }
        std::string error;
        if (activateReceiver(i, st, error) != 200)
            std::fprintf(stderr, "inletd: NMOS: scheduled activation of receiver %d failed: %s\n", i, error.c_str());
        std::lock_guard<std::mutex> g(lock);
        staged[i].activation = nullActivation();
    }
    for (int i : sendersDue)
    {
        SenderStage st;
        {
            std::lock_guard<std::mutex> g(lock);
            st = stagedSenders[i];
        }
        std::string error;
        if (activateSender(i, st, error) != 200)
            std::fprintf(stderr, "inletd: NMOS: scheduled activation of sender %d failed: %s\n", i, error.c_str());
        std::lock_guard<std::mutex> g(lock);
        stagedSenders.erase(i);
    }
}

bool NmosNode::bulk(bool isReceivers, const std::string& body, HttpResponse& out)
{
    // IS-05 bulk: each entry as its own staged PATCH, each answered.
    nlohmann::json list;
    try
    {
        list = nlohmann::json::parse(body);
    }
    catch (const std::exception& e)
    {
        out.status = 400;
        out.body = errorJson(400, "the body is not JSON", e.what()).dump();
        return true;
    }
    if (!list.is_array())
    {
        out.status = 400;
        out.body = errorJson(400, "a bulk request is an array of { id, params }").dump();
        return true;
    }
    auto results = nlohmann::json::array();
    for (auto& entry : list)
    {
        const auto id = entry.is_object() ? entry.value("id", std::string()) : std::string();
        int index = 0;
        if (isReceivers)
        {
            for (auto& r : model.receivers())
                if (receiverId(cfg.deviceId, r.index) == id)
                    index = r.index;
        }
        else
        {
            for (auto& s : model.senders())
                if (senderId(cfg.deviceId, s.index) == id)
                    index = s.index;
        }
        nlohmann::json result = { { "id", id } };
        if (index == 0)
        {
            result["code"] = 404;
            result["error"] = isReceivers ? "no such receiver" : "no such sender";
        }
        else
        {
            HttpResponse one;
            const auto params = entry.value("params", nlohmann::json::object());
            if (isReceivers)
                patchReceiver(index, params, one);
            else
                patchSender(index, params, one);
            result["code"] = one.status;
            if (one.status >= 400)
            {
                try
                {
                    result["error"] = nlohmann::json::parse(one.body).value("error", std::string("failed"));
                }
                catch (const std::exception&)
                {
                    result["error"] = "failed";
                }
            }
        }
        results.push_back(result);
    }
    out.status = 200;
    out.body = results.dump();
    return true;
}

/* ---- HTTP ------------------------------------------------------------------------------ */

bool NmosNode::handleHttp(const std::string& method, const std::string& path, const std::string& body, HttpResponse& out)
{
    const auto t = tokens(path);
    if (t.empty())
        return false;

    auto listing = [&](std::initializer_list<const char*> items) {
        auto j = nlohmann::json::array();
        for (auto* i : items)
            j.push_back(i);
        out.status = 200;
        out.body = j.dump();
        return true;
    };
    auto notFound = [&](const std::string& what) {
        out.status = 404;
        out.body = errorJson(404, what + " not found").dump();
        return true;
    };
    auto ok = [&](const nlohmann::json& j) {
        out.status = 200;
        out.body = j.dump();
        return true;
    };

    // ---- manifests
    if (t[0] == "manifests")
    {
        if (t.size() != 2 || method != "GET")
            return notFound("manifest");
        auto name = t[1];
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".sdp") == 0)
            name.resize(name.size() - 4);
        for (auto& s : model.senders())
            if (senderId(cfg.deviceId, s.index) == name)
            {
                const auto sdp = model.senderSdp(s.index);
                if (sdp.empty())
                    return notFound("manifest (the sender is not sending)");
                out.status = 200;
                out.contentType = "application/sdp";
                out.body = sdp;
                return true;
            }
        return notFound("manifest");
    }

    if (t[0] != "x-nmos")
        return false;
    if (t.size() == 1)
        return listing({ "node/", "connection/", "channelmapping/" });
    if (t[1] == "channelmapping")
        return handleChannelMapping(method, t, body, out);

    // ---- IS-04 node API
    if (t[1] == "node")
    {
        if (method != "GET")
        {
            out.status = 405;
            out.body = errorJson(405, "the node API is read-only").dump();
            return true;
        }
        if (t.size() == 2)
            return listing({ "v1.2/", "v1.3/" });
        if (!isNodeVersion(t[2]))
            return notFound("API version");
        if (t.size() == 3)
            return listing({ "self/", "sources/", "flows/", "devices/", "senders/", "receivers/" });
        const auto& what = t[3];
        nlohmann::json list;
        if (what == "self")
            return t.size() == 4 ? ok(self()) : notFound("resource");
        else if (what == "devices")
            list = nlohmann::json::array({ device() });
        else if (what == "sources")
            list = sources();
        else if (what == "flows")
            list = flows();
        else if (what == "senders")
            list = senders();
        else if (what == "receivers")
            list = receivers();
        else
            return notFound("resource");
        if (t.size() == 4)
            return ok(list);
        for (auto& r : list)
            if (r.value("id", "") == t[4])
                return ok(r);
        return notFound(what.substr(0, what.size() - 1));
    }

    // ---- IS-05 connection API
    if (t[1] == "connection")
    {
        if (t.size() == 2)
            return listing({ "v1.0/", "v1.1/" });
        if (!isConnVersion(t[2]))
            return notFound("API version");
        if (t.size() == 3)
            return listing({ "bulk/", "single/" });
        if (t[3] == "bulk")
        {
            if (t.size() == 4)
                return listing({ "senders/", "receivers/" });
            if (t.size() != 5 || (t[4] != "senders" && t[4] != "receivers"))
                return notFound("resource");
            if (method != "POST")
            {
                out.status = 405;
                out.body = errorJson(405, "bulk takes POST").dump();
                return true;
            }
            return bulk(t[4] == "receivers", body, out);
        }
        if (t[3] != "single")
            return notFound("resource");
        if (t.size() == 4)
            return listing({ "senders/", "receivers/" });
        const bool isReceiver = t[4] == "receivers";
        if (!isReceiver && t[4] != "senders")
            return notFound("resource");

        if (t.size() == 5)
        {
            auto j = nlohmann::json::array();
            if (isReceiver)
                for (auto& r : model.receivers())
                    j.push_back(receiverId(cfg.deviceId, r.index) + "/");
            else
                for (auto& s : model.senders())
                    j.push_back(senderId(cfg.deviceId, s.index) + "/");
            return ok(j);
        }
        int index = 0;
        if (isReceiver)
        {
            for (auto& r : model.receivers())
                if (receiverId(cfg.deviceId, r.index) == t[5])
                    index = r.index;
        }
        else
        {
            for (auto& s : model.senders())
                if (senderId(cfg.deviceId, s.index) == t[5])
                    index = s.index;
        }
        if (index == 0)
            return notFound(isReceiver ? "receiver" : "sender");
        if (t.size() == 6)
            return isReceiver ? listing({ "constraints/", "staged/", "active/", "transporttype/" })
                              : listing({ "constraints/", "staged/", "active/", "transporttype/", "transportfile/" });
        const auto& leaf = t[6];
        if (leaf == "constraints" && method == "GET")
            return ok(nlohmann::json::array({ nlohmann::json::object() }));
        if (leaf == "transporttype" && method == "GET")
            return ok(kTransport);
        if (leaf == "transportfile" && !isReceiver && method == "GET")
        {
            const auto sdp = model.senderSdp(index);
            if (sdp.empty())
                return notFound("transport file (the sender is not sending)");
            out.status = 200;
            out.contentType = "application/sdp";
            out.body = sdp;
            return true;
        }
        if (leaf == "active" && method == "GET")
            return ok(isReceiver ? receiverStagedJson(index, true) : senderStagedJson(index));
        if (leaf == "staged")
        {
            if (method == "GET")
                return ok(isReceiver ? receiverStagedJson(index, false) : senderStagedJson(index));
            if (method == "PATCH")
            {
                nlohmann::json parsed;
                try
                {
                    parsed = nlohmann::json::parse(body);
                }
                catch (const std::exception& e)
                {
                    out.status = 400;
                    out.body = errorJson(400, "the body is not JSON", e.what()).dump();
                    return true;
                }
                return isReceiver ? patchReceiver(index, parsed, out) : patchSender(index, parsed, out);
            }
            out.status = 405;
            out.body = errorJson(405, "staged takes GET and PATCH").dump();
            return true;
        }
        return notFound("resource");
    }
    return notFound("API");
}

/* ---- IS-08 ------------------------------------------------------------------------------ */

nlohmann::json NmosNode::is08Input(const std::string& idIn) const
{
    // Inputs are the receivers, parented to their IS-04 receiver, with the
    // channels of what they play (none while idle: the slot still exists).
    const int index = receiverIndexFrom(idIn);
    if (index <= 0)
        return nullptr;
    for (auto& r : model.receivers())
        if (r.index == index)
        {
            nlohmann::json j;
            j["id"] = trimSlash(idIn);
            j["properties"] = { { "name", r.label }, { "description", "" } };
            j["parent"] = { { "id", receiverId(cfg.deviceId, index) }, { "type", "receiver" } };
            j["channels"] = nlohmann::json::array();
            for (int c = 0; r.active && c < r.channels; ++c)
                j["channels"].push_back({ { "label", r.label + " " + std::to_string(c + 1) } });
            j["caps"] = { { "reordering", true }, { "block_size", 1 } };
            return j;
        }
    return nullptr;
}

nlohmann::json NmosNode::is08Output(const std::string& id) const
{
    // One output: the device's inputs, what a DAW records.
    if (trimSlash(id) != kDeviceOutput)
        return nullptr;
    nlohmann::json j;
    j["id"] = kDeviceOutput;
    j["properties"] = { { "name", "Inlet inputs" }, { "description", "What the computer's audio software records" } };
    j["source_id"] = nullptr;
    j["channels"] = nlohmann::json::array();
    for (auto& label : model.deviceInputLabels())
        j["channels"].push_back({ { "label", label } });
    j["caps"] = { { "routable_inputs", nullptr } };   // any input
    return j;
}

nlohmann::json NmosNode::is08ActiveMap() const
{
    nlohmann::json row = nlohmann::json::object();
    const auto map = model.inputMap();
    for (size_t i = 0; i < map.size(); ++i)
    {
        const auto& c = map[i];
        row[std::to_string(i)] = c.receiver > 0
                                     ? nlohmann::json { { "input", "rx" + std::to_string(c.receiver) }, { "channel_index", c.channel } }
                                     : nlohmann::json { { "input", nullptr }, { "channel_index", nullptr } };
    }
    return { { "activation", nullActivation() }, { "map", { { kDeviceOutput, row } } } };
}

bool NmosNode::handleChannelMapping(const std::string& method, const std::vector<std::string>& t,
                                    const std::string& body, HttpResponse& out)
{
    auto listing = [&](std::initializer_list<const char*> items) {
        auto j = nlohmann::json::array();
        for (auto* i : items)
            j.push_back(i);
        out.status = 200;
        out.body = j.dump();
        return true;
    };
    auto ok = [&](const nlohmann::json& j) {
        out.status = 200;
        out.body = j.dump();
        return true;
    };
    auto notFound = [&](const std::string& what) {
        out.status = 404;
        out.body = errorJson(404, what + " not found").dump();
        return true;
    };
    auto bad = [&](const std::string& why) {
        out.status = 400;
        out.body = errorJson(400, why).dump();
        return true;
    };

    if (t.size() == 2)
        return listing({ "v1.0/" });
    if (t[2] != "v1.0")
        return notFound("API version");
    if (t.size() == 3)
        return listing({ "io/", "map/", "inputs/", "outputs/" });

    std::vector<std::string> inputIds, outputIds { kDeviceOutput };
    for (auto& r : model.receivers())
        inputIds.push_back("rx" + std::to_string(r.index));

    const auto& what = t[3];
    if (what == "io" && method == "GET")
    {
        nlohmann::json j = { { "inputs", nlohmann::json::object() }, { "outputs", nlohmann::json::object() } };
        for (auto& id : inputIds)
            if (auto in = is08Input(id); !in.is_null())
            {
                in.erase("id");
                j["inputs"][id] = in;
            }
        for (auto& id : outputIds)
            if (auto o = is08Output(id); !o.is_null())
            {
                o.erase("id");
                j["outputs"][id] = o;
            }
        return ok(j);
    }
    if (what == "inputs" || what == "outputs")
    {
        if (method != "GET")
        {
            out.status = 405;
            out.body = errorJson(405, "read-only").dump();
            return true;
        }
        const bool isInput = what == "inputs";
        if (t.size() == 4)
        {
            auto j = nlohmann::json::array();
            for (auto& id : isInput ? inputIds : outputIds)
                j.push_back(id);
            return ok(j);
        }
        const auto r = isInput ? is08Input(t[4]) : is08Output(t[4]);
        if (r.is_null())
            return notFound(isInput ? "input" : "output");
        if (t.size() == 5)
            return isInput ? listing({ "properties/", "parent/", "channels/", "caps/" })
                           : listing({ "properties/", "source_id/", "channels/", "caps/" });
        if (r.contains(t[5]))
            return ok(r[t[5]]);
        return notFound("resource");
    }
    if (what == "map")
    {
        if (t.size() == 4)
            return listing({ "active/", "activations/" });
        if (t[4] == "active" && method == "GET")
            return t.size() == 5 ? ok(is08ActiveMap())
                                 : (t.size() == 6 && t[5] == "map" ? ok(is08ActiveMap()["map"]) : notFound("resource"));
        if (t[4] == "activations")
        {
            if (method == "GET")
                return ok(nlohmann::json::array());   // nothing scheduled, ever
            if (method != "POST")
            {
                out.status = 405;
                out.body = errorJson(405, "activations take GET and POST").dump();
                return true;
            }
            nlohmann::json req;
            try
            {
                req = nlohmann::json::parse(body);
            }
            catch (const std::exception& e)
            {
                return bad(std::string("the body is not JSON: ") + e.what());
            }
            const auto mode = req.value("activation", nlohmann::json::object()).value("mode", std::string());
            if (mode != "activate_immediate")
                return bad("only activate_immediate is supported");
            if (!req.contains("actions") || !req["actions"].is_object())
                return bad("actions must be an object of outputs");

            // Check everything before changing anything: all or nothing.
            const auto outputLabels = model.deviceInputLabels();
            std::vector<std::pair<int, InputMapCell>> changes;
            for (auto it = req["actions"].begin(); it != req["actions"].end(); ++it)
            {
                if (trimSlash(it.key()) != kDeviceOutput)
                    return bad("no such output: " + it.key());
                if (!it.value().is_object())
                    return bad("actions for " + it.key() + " must be an object of channels");
                for (auto c = it.value().begin(); c != it.value().end(); ++c)
                {
                    const auto& key = c.key();
                    const bool numeric = !key.empty() && std::all_of(key.begin(), key.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
                    const int ch = numeric ? std::atoi(key.c_str()) : -1;
                    if (ch < 0 || ch >= (int) outputLabels.size())
                        return bad(std::string(kDeviceOutput) + " has no channel " + key);
                    const auto& cell = c.value();
                    InputMapCell m;
                    if (cell.is_object() && cell.value("input", nlohmann::json(nullptr)).is_string())
                    {
                        const auto in = cell["input"].get<std::string>();
                        const auto def = is08Input(in);
                        if (def.is_null())
                            return bad("no such input: " + in);
                        const int idx = cell.value("channel_index", nlohmann::json(nullptr)).is_number_integer()
                                            ? cell["channel_index"].get<int>() : -1;
                        if (idx < 0)
                            return bad("channel_index is needed for " + in);
                        // An idle receiver has no channels yet; the slot's
                        // capacity bounds what may be mapped ahead of a stream.
                        int capacity = 0;
                        for (auto& r : model.receivers())
                            if (r.index == receiverIndexFrom(in))
                                capacity = r.maxChannels;
                        if (idx >= capacity)
                            return bad(in + " has no channel " + std::to_string(idx));
                        m.receiver = receiverIndexFrom(in);
                        m.channel = idx;
                    }
                    changes.push_back({ ch, m });
                }
            }
            std::string error;
            if (!model.setInputMap(changes, error))
                return bad(error);
            dirty = true;
            nlohmann::json resp = { { "activation", { { "mode", "activate_immediate" }, { "requested_time", nullptr }, { "activation_time", versionNow() } } },
                                    { "actions", req["actions"] } };
            out.status = 200;
            out.body = resp.dump();
            return true;
        }
        return notFound("resource");
    }
    return notFound("resource");
}

/* ---- registry ------------------------------------------------------------------------ */

bool NmosNode::postResource(const std::string& type, const nlohmann::json& data, std::string& error)
{
    const nlohmann::json body = { { "type", type }, { "data", data } };
    const auto r = httpRequest("POST", registryUrl + "/x-nmos/registration/v1.3/resource", body.dump());
    if (!r.connected)
    {
        error = "no answer from " + registryUrl;
        return false;
    }
    if (r.status != 200 && r.status != 201)
    {
        error = type + " refused with HTTP " + std::to_string(r.status) + ": " + r.body.substr(0, 200);
        return false;
    }
    return true;
}

bool NmosNode::registerAll(std::string& error)
{
    // Dependency order: a registry refuses a device whose node it has not
    // seen, a sender whose flow it has not seen.
    std::map<std::string, std::string> posted;
    auto post = [&](const char* type, const nlohmann::json& r) {
        if (!postResource(type, r, error))
            return false;
        posted[r.value("id", "")] = r.value("version", "");
        return true;
    };
    if (!post("node", self()))
        return false;
    if (!post("device", device()))
        return false;
    for (auto& s : sources())
        if (!post("source", s))
            return false;
    for (auto& f : flows())
        if (!post("flow", f))
            return false;
    for (auto& s : senders())
        if (!post("sender", s))
            return false;
    for (auto& r : receivers())
        if (!post("receiver", r))
            return false;
    std::lock_guard<std::mutex> g(lock);
    registeredVersions = posted;
    return true;
}

bool NmosNode::registerChanged(std::string& error)
{
    // Only resources whose version the registry has not seen go up, and none
    // on a quiet pass, so re-registration never starves the heartbeat.
    struct Item
    {
        const char* type;
        nlohmann::json r;
    };
    std::vector<Item> all;
    all.push_back({ "node", self() });
    all.push_back({ "device", device() });
    for (auto& s : sources())
        all.push_back({ "source", s });
    for (auto& f : flows())
        all.push_back({ "flow", f });
    for (auto& s : senders())
        all.push_back({ "sender", s });
    for (auto& r : receivers())
        all.push_back({ "receiver", r });
    for (auto& item : all)
    {
        const auto id = item.r.value("id", "");
        const auto version = item.r.value("version", "");
        bool known;
        {
            std::lock_guard<std::mutex> g(lock);
            auto it = registeredVersions.find(id);
            known = it != registeredVersions.end() && it->second == version;
        }
        if (known)
            continue;
        if (!postResource(item.type, item.r, error))
            return false;
        std::lock_guard<std::mutex> g(lock);
        registeredVersions[id] = version;
    }
    return true;
}

bool NmosNode::heartbeat(std::string& error)
{
    const auto r = httpRequest("POST", registryUrl + "/x-nmos/registration/v1.3/health/nodes/" + cfg.nodeId, "{}");
    if (!r.connected)
    {
        error = "no answer from " + registryUrl;
        return false;
    }
    if (r.status == 404)
    {
        error = "the registry has forgotten us";
        return false;
    }
    if (r.status != 200)
    {
        error = "heartbeat refused with HTTP " + std::to_string(r.status);
        return false;
    }
    return true;
}

void NmosNode::chooseRegistry()
{
    // Configured wins; otherwise the best-priority one seen (IS-04: lower
    // `pri` is better).
    if (!cfg.registryUrl.empty())
    {
        registryUrl = cfg.registryUrl;
        return;
    }
    std::lock_guard<std::mutex> g(lock);
    const RegistryCandidate* best = nullptr;
    for (auto& c : candidates)
        if (best == nullptr || c.priority < best->priority)
            best = &c;
    const std::string chosen = best != nullptr ? best->url : std::string();
    if (chosen != registryUrl)
    {
        registryUrl = chosen;
        registered = false;
        heartbeatFailures = 0;
    }
}

void NmosNode::run()
{
    int64_t lastRegisterAttemptMs = 0;
    while (running.load())
    {
        const int64_t now = nowMs();

        // How long the loop may wait: until the next scheduled activation, at most 100 ms.
        int waitMs = 100;
        {
            std::lock_guard<std::mutex> g(lock);
            for (auto& [i, t] : receiverDue)
                waitMs = (int) std::clamp<int64_t>(t - now, 0, waitMs);
            for (auto& [i, t] : senderDue)
                waitMs = (int) std::clamp<int64_t>(t - now, 0, waitMs);
        }

        if (dns != nullptr)
        {
            if (now - lastBrowseMs > kBrowseMs)
            {
                if (cfg.registryUrl.empty())
                    mdns_browse(dns, "_nmos-register._tcp");
                mdns_browse(dns, "_nmos-node._tcp");
                lastBrowseMs = now;
            }
            auto handle = [](const mdns_result_t* r, void* user) {
                auto* self = static_cast<NmosNode*>(user);
                if (std::string(r->type) == "_nmos-node._tcp")
                {
                    const std::string href = "http://" + std::string(r->ipv4) + ":" + std::to_string(r->port);
                    if (href == self->baseHref())
                        return;
                    std::lock_guard<std::mutex> g(self->lock);
                    for (auto it = self->peers.begin(); it != self->peers.end(); ++it)
                        if (it->href == href)
                        {
                            if (r->ttl == 0 && !it->manual)
                                self->peers.erase(it);
                            else
                                it->seenMs = nowMs();
                            return;
                        }
                    if (r->ttl != 0)
                    {
                        Peer p;
                        p.href = href;
                        p.seenMs = nowMs();
                        self->peers.push_back(p);
                    }
                    return;
                }
                if (std::string(r->type) != "_nmos-register._tcp")
                    return;
                std::string proto = "http";
                int pri = 100;
                for (int i = 0; i < r->txt_count; ++i)
                {
                    const std::string kv = r->txt[i];
                    if (kv.rfind("api_proto=", 0) == 0)
                        proto = kv.substr(10);
                    if (kv.rfind("pri=", 0) == 0)
                        pri = std::atoi(kv.c_str() + 4);
                }
                const std::string url = proto + "://" + r->ipv4 + ":" + std::to_string(r->port);
                std::lock_guard<std::mutex> g(self->lock);
                auto& cs = self->candidates;
                for (auto it = cs.begin(); it != cs.end(); ++it)
                    if (it->url == url)
                    {
                        if (r->ttl == 0)
                            cs.erase(it);
                        else
                        {
                            it->priority = pri;
                            it->seenMs = nowMs();
                        }
                        return;
                    }
                if (r->ttl != 0 && proto == "http")
                    cs.push_back({ url, pri, nowMs() });
            };
            mdns_poll(dns, waitMs, handle, this);
        }
        else
        {
            std::unique_lock<std::mutex> g(wakeLock);
            wakeCv.wait_for(g, std::chrono::milliseconds(waitMs), [this] { return wakeFlag; });
            wakeFlag = false;
        }
        if (!running.load())
            break;

        runDueActivations();
        crawlPeers();
        chooseRegistry();
        if (registryUrl.empty())
            continue;

        std::string error;
        if (!registered)
        {
            if (now - lastRegisterAttemptMs < kRegisterRetryMs)
                continue;
            lastRegisterAttemptMs = now;
            if (registerAll(error))
            {
                registered = true;
                heartbeatFailures = 0;
                registerFailures = 0;
                lastHeartbeatMs = now;
                lastError.clear();
                dirty = false;
                std::fprintf(stderr, "inletd: NMOS: registered with %s\n", registryUrl.c_str());
            }
            else
            {
                if (lastError != error)
                    std::fprintf(stderr, "inletd: NMOS: registration with %s failed: %s\n", registryUrl.c_str(), error.c_str());
                lastError = error;
                // A discovered registry that never answers is set aside until
                // it advertises again, so a real one can be chosen instead.
                if (++registerFailures >= 3 && cfg.registryUrl.empty())
                {
                    std::lock_guard<std::mutex> g(lock);
                    for (auto it = candidates.begin(); it != candidates.end(); ++it)
                        if (it->url == registryUrl)
                        {
                            candidates.erase(it);
                            break;
                        }
                    registerFailures = 0;
                }
            }
            continue;
        }

        if (dirty.exchange(false) && !registerChanged(error))
        {
            lastError = error;
            registered = false;
            continue;
        }

        if (now - lastHeartbeatMs >= kHeartbeatMs)
        {
            lastHeartbeatMs = now;
            if (heartbeat(error))
            {
                heartbeatFailures = 0;
                lastError.clear();
            }
            else
            {
                lastError = error;
                if (error.find("forgotten") != std::string::npos || ++heartbeatFailures >= 3)
                {
                    registered = false;
                    if (heartbeatFailures >= 3 && cfg.registryUrl.empty())
                    {
                        std::lock_guard<std::mutex> g(lock);
                        for (auto it = candidates.begin(); it != candidates.end(); ++it)
                            if (it->url == registryUrl)
                            {
                                candidates.erase(it);
                                break;
                            }
                    }
                }
            }
        }
    }
}

/* ---- peers ----------------------------------------------------------------------------- */

void NmosNode::addPeer(const std::string& hrefIn)
{
    std::string href = trimSlash(hrefIn);
    if (href.rfind("http", 0) != 0)
        href = "http://" + href;
    {
        std::lock_guard<std::mutex> g(lock);
        for (auto& p : peers)
            if (p.href == href)
            {
                p.manual = true;
                p.crawledMs = 0;   // look again now
                return;
            }
        Peer p;
        p.href = href;
        p.manual = true;
        p.seenMs = nowMs();
        peers.push_back(p);
    }
    wake();
}

void NmosNode::removePeer(const std::string& href)
{
    {
        std::lock_guard<std::mutex> g(lock);
        for (auto it = peers.begin(); it != peers.end(); ++it)
            if (it->href == href || it->href == href + "/")
            {
                peers.erase(it);
                break;
            }
    }
    dirty = true;
}

std::vector<std::string> NmosNode::manualPeers() const
{
    std::lock_guard<std::mutex> g(lock);
    std::vector<std::string> out;
    for (auto& p : peers)
        if (p.manual)
            out.push_back(p.href);
    return out;
}

bool NmosNode::getJson(const std::string& url, nlohmann::json& out, std::string& error)
{
    const auto r = httpRequest("GET", url);
    if (!r.connected)
    {
        error = "no answer";
        return false;
    }
    if (r.status != 200)
    {
        error = "HTTP " + std::to_string(r.status);
        return false;
    }
    try
    {
        out = nlohmann::json::parse(r.body);
    }
    catch (const std::exception&)
    {
        error = "not JSON";
        return false;
    }
    return true;
}

bool NmosNode::crawlPeer(Peer& peer)
{
    // A controller's walk: self, devices (for the IS-05 control), senders with
    // their manifests, receivers.
    std::string error;
    nlohmann::json selfJ, devices, sendersJ, receiversJ;
    const std::string base = peer.href + "/x-nmos/node/v1.3";
    if (!getJson(base + "/self", selfJ, error) || !getJson(base + "/devices", devices, error)
        || !getJson(base + "/senders", sendersJ, error) || !getJson(base + "/receivers", receiversJ, error))
    {
        peer.error = error;
        return false;
    }
    peer.nodeId = selfJ.value("id", "");
    peer.label = selfJ.value("label", "");
    peer.services = nlohmann::json::array();
    if (selfJ.contains("services") && selfJ["services"].is_array())
        for (auto& sv : selfJ["services"])
            if (sv.is_object() && sv.contains("href") && sv.contains("type"))
                peer.services.push_back({ { "href", sv.value("href", "") }, { "type", sv.value("type", "") } });
    peer.connectionHref.clear();
    if (devices.is_array())
        for (auto& d : devices)
            if (d.contains("controls"))
                for (auto& c : d["controls"])
                    if (c.value("type", "") == "urn:x-nmos:control:sr-ctrl/v1.1"
                        || (peer.connectionHref.empty() && c.value("type", "").rfind("urn:x-nmos:control:sr-ctrl/", 0) == 0))
                        peer.connectionHref = c.value("href", "");
    peer.connectionHref = trimSlash(peer.connectionHref);

    // Flows and sources, for what each sender's channels are called. Not
    // fatal: a node that lists senders but will not list these still routes.
    std::map<std::string, std::string> sourceOfFlow;
    std::map<std::string, std::vector<std::string>> channelsOfSource;
    {
        nlohmann::json flowsJ, sourcesJ;
        std::string ignored;
        if (getJson(base + "/flows", flowsJ, ignored) && flowsJ.is_array())
            for (auto& f : flowsJ)
                if (f.is_object() && f.value("id", nlohmann::json(nullptr)).is_string())
                    sourceOfFlow[f.value("id", "")] = f.value("source_id", nlohmann::json(nullptr)).is_string() ? f["source_id"].get<std::string>() : "";
        if (getJson(base + "/sources", sourcesJ, ignored) && sourcesJ.is_array())
            for (auto& src : sourcesJ)
                if (src.is_object() && src.contains("channels") && src["channels"].is_array())
                {
                    std::vector<std::string> labels;
                    for (auto& ch : src["channels"])
                        labels.push_back(ch.is_object() ? ch.value("label", "") : std::string());
                    channelsOfSource[src.value("id", "")] = labels;
                }
    }

    auto sendersOut = nlohmann::json::array();
    if (sendersJ.is_array())
        for (auto& sn : sendersJ)
        {
            nlohmann::json e = { { "id", sn.value("id", "") }, { "label", sn.value("label", "") }, { "sdp", "" } };
            e["channelLabels"] = nlohmann::json::array();
            // flow_id may be null (IS-04 v1.3: a sender with no flow yet).
            const auto flow = sn.contains("flow_id") && sn["flow_id"].is_string() ? sn["flow_id"].get<std::string>() : std::string();
            if (auto f = sourceOfFlow.find(flow); f != sourceOfFlow.end())
                if (auto c = channelsOfSource.find(f->second); c != channelsOfSource.end())
                    e["channelLabels"] = c->second;
            const auto manifest = sn.contains("manifest_href") && sn["manifest_href"].is_string() ? sn["manifest_href"].get<std::string>() : std::string();
            if (!manifest.empty())
            {
                const auto r = httpRequest("GET", manifest);
                if (r.connected && r.status == 200 && r.body.rfind("v=0", 0) == 0)
                    e["sdp"] = r.body;
            }
            sendersOut.push_back(e);
        }
    auto receiversOut = nlohmann::json::array();
    if (receiversJ.is_array())
        for (auto& r : receiversJ)
            receiversOut.push_back({ { "id", r.value("id", "") }, { "label", r.value("label", "") },
                                     { "subscription", r.value("subscription", nlohmann::json { { "sender_id", nullptr }, { "active", false } }) } });
    peer.senders = sendersOut;
    peer.receivers = receiversOut;
    peer.error.clear();
    return true;
}

void NmosNode::crawlPeers()
{
    // One peer per pass, the stalest first, so a slow box never holds the
    // loop (and the heartbeat) for long.
    const int64_t now = nowMs();
    Peer work;
    bool any = false;
    {
        std::lock_guard<std::mutex> g(lock);
        Peer* pick = nullptr;
        for (auto& p : peers)
            if (now - p.crawledMs >= kCrawlEveryMs && (pick == nullptr || p.crawledMs < pick->crawledMs))
                pick = &p;
        if (pick != nullptr)
        {
            work = *pick;
            any = true;
        }
    }
    if (!any)
        return;
    // A peer whose JSON has the wrong type where a string belongs is a peer
    // that failed, not a reason to stop.
    bool ok = false;
    try
    {
        ok = crawlPeer(work);
    }
    catch (const std::exception& e)
    {
        work.error = std::string("unreadable answer: ") + e.what();
    }
    if (ok && work.nodeId == cfg.nodeId)
    {
        // Ourselves, advertised on another of this computer's addresses: not a peer.
        std::lock_guard<std::mutex> g(lock);
        peers.erase(std::remove_if(peers.begin(), peers.end(), [&](const Peer& p) { return p.href == work.href; }), peers.end());
        return;
    }
    std::vector<std::string> keep;
    {
        std::lock_guard<std::mutex> g(lock);
        for (auto& p : peers)
            if (p.href == work.href)
            {
                p.crawledMs = now;
                p.online = ok;
                p.error = work.error;
                if (ok)
                {
                    p.nodeId = work.nodeId;
                    p.label = work.label;
                    p.connectionHref = work.connectionHref;
                    p.senders = work.senders;
                    p.receivers = work.receivers;
                    p.services = work.services;
                }
            }
        // One node, one entry: reached by two addresses, the manual entry stays.
        if (ok && !work.nodeId.empty())
            for (auto it = peers.begin(); it != peers.end();)
            {
                if (it->href != work.href && it->nodeId == work.nodeId)
                {
                    bool thisIsManual = false;
                    for (auto& p : peers)
                        if (p.href == work.href)
                            thisIsManual = p.manual;
                    if (!it->manual || thisIsManual)
                    {
                        it = peers.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        for (auto& p : peers)
            if (p.online)
                for (auto& sn : p.senders)
                    keep.push_back(sn.value("id", ""));
    }
    if (ok)
        for (auto& sn : work.senders)
            if (!sn.value("sdp", "").empty())
                model.upsertNmosStream(sn.value("id", ""),
                                       work.label.empty() ? sn.value("label", "") : work.label + " · " + sn.value("label", ""),
                                       sn.value("sdp", ""), sn.value("channelLabels", std::vector<std::string> {}));
    model.expireNmosStreamsExcept(keep);
}

bool NmosNode::findSenderSdp(const std::string& id, std::string& sdpOut) const
{
    for (auto& s : model.senders())
        if (senderId(cfg.deviceId, s.index) == id)
        {
            sdpOut = model.senderSdp(s.index);
            return !sdpOut.empty();
        }
    std::lock_guard<std::mutex> g(lock);
    for (auto& p : peers)
        for (auto& sn : p.senders)
            if (sn.value("id", "") == id && !sn.value("sdp", "").empty())
            {
                sdpOut = sn.value("sdp", "");
                return true;
            }
    return false;
}

nlohmann::json NmosNode::toJson() const
{
    std::lock_guard<std::mutex> g(lock);
    nlohmann::json j;
    j["nodeId"] = cfg.nodeId;
    j["deviceId"] = cfg.deviceId;
    j["label"] = cfg.label;
    j["href"] = baseHref() + "/x-nmos/node/v1.3/";
    j["advertising"] = dns != nullptr;
    j["registry"] = { { "url", registryUrl }, { "configured", !cfg.registryUrl.empty() }, { "registered", registered },
                      { "lastHeartbeatMs", lastHeartbeatMs }, { "error", lastError } };
    j["registries"] = nlohmann::json::array();
    for (auto& c : candidates)
        j["registries"].push_back({ { "url", c.url }, { "priority", c.priority } });
    j["peers"] = nlohmann::json::array();
    for (auto& p : peers)
        j["peers"].push_back({ { "href", p.href }, { "nodeId", p.nodeId }, { "label", p.label }, { "manual", p.manual },
                               { "online", p.online }, { "error", p.error }, { "senders", p.senders },
                               { "receivers", p.receivers }, { "services", p.services } });
    return j;
}

} // namespace inlet
