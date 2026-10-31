// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "Engine.h"

#include "Ids.h"
#include "NmosNode.h"
#include "Sap.h"
#include "StreamManager.h"

#include "inlet/ClockDiscipline.h"
#include "inlet/NetTx.h"
#include "inlet/SharedRegion.h"
#if defined(INLET_HAVE_PTP)
#  include "inlet/PtpFollower.h"
#endif

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace inlet
{

namespace
{

constexpr const char* kVersion = "0.1.0";
constexpr const char* kServiceType = "urn:x-meros:service:inlet/v1";

int64_t wallNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/** linuxptp's way of writing a clock identity ("001b19.fffe.000001"), which
    is what a console's ptp_update carries and compares. */
std::string pmcIdentity(const uint8_t b[8])
{
    char s[24];
    std::snprintf(s, sizeof s, "%02x%02x%02x.%02x%02x.%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    return s;
}

const char* stateName(ClockState s)
{
    switch (s)
    {
    case ClockState::Locked: return "locked";
    case ClockState::Holdover: return "holdover";
    default: return "free-run";
    }
}

bool isLoopback(const std::string& addr)
{
    return addr == "127.0.0.1" || addr == "::1" || addr.rfind("127.", 0) == 0 || addr == "::ffff:127.0.0.1";
}

void sendJson(httplib::Response& res, int status, const nlohmann::json& j)
{
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

void sendError(httplib::Response& res, int status, const std::string& error)
{
    sendJson(res, status, { { "code", status }, { "error", error } });
}

} // namespace

struct Engine::Impl
{
    Config& config;
    std::string configPath;
    shm::Directory& directory;
    Engine* owner = nullptr;

    NetInterface iface;
    std::unique_ptr<shm::Region> region;
    struct NetRxClose
    {
        void operator()(aes67_netrx_t* r) const { aes67_netrx_close(r); }
    };
    std::unique_ptr<aes67_netrx_t, NetRxClose> netRx;
    std::unique_ptr<NetTx> netTx;
    std::unique_ptr<Sap> sap;
    std::unique_ptr<StreamManager> streams;
    std::unique_ptr<NmosNode> nmos;
    httplib::Server http;
    std::thread httpThread;

#if defined(INLET_HAVE_PTP)
    std::unique_ptr<PtpFollower> ptp;
#endif
    std::string ptpReason;
    std::string sapWarning;
    std::unique_ptr<ClockDiscipline> discipline;

    std::atomic<bool> running { false };
    std::thread clockThread;
    std::atomic<bool> dirty { false };   // state to persist
    mutable std::mutex configLock;

    Impl(Config& c, const std::string& p, shm::Directory& dir) : config(c), configPath(p), directory(dir) {}

    void clockLoop();
    void watchClients(int64_t nowMono);
    void persist();
    nlohmann::json ptpJson() const;
    nlohmann::json clientsJson() const;
    nlohmann::json status() const;
    void routes();
    int receiverIndexOf(const std::string& id) const;
    int senderIndexOf(const std::string& id) const;
};

Engine::Engine(Config& config, const std::string& configPath, shm::Directory& directory)
    : impl(std::make_unique<Impl>(config, configPath, directory))
{
    impl->owner = this;
}

Engine::~Engine()
{
    stop();
}

bool Engine::start(std::string& error)
{
    auto& d = *impl;
    Config& c = d.config;
    if (!chooseInterface(c.interfaceIp, d.iface))
    {
        error = c.interfaceIp.empty() ? "no network interface is up" : "the interface " + c.interfaceIp + " is not up";
        return false;
    }
    if (d.iface.wireless)
        std::fprintf(stderr, "inletd: warning: %s is wireless; AES67 and PTP need a wired network\n", d.iface.name.c_str());
    if (c.nodeId.empty())
        c.nodeId = randomUuid();
    if (c.deviceId.empty())
        c.deviceId = randomUuid();

    // The API port first: if it is taken, nothing else has started.
    if (!d.http.bind_to_port("0.0.0.0", c.apiPort))
    {
        error = "the API port " + std::to_string(c.apiPort) + " is in use";
        return false;
    }

    shm::Region::Layout layout;
    layout.sampleRate = c.sampleRate;
    layout.inputs = c.inputs;
    layout.outputs = c.outputs;
    layout.receivers = c.receivers;
    layout.receiverChannels = c.receiverChannels;
    const std::string regionName = shm::uniqueRegionName();
    d.region = shm::Region::create(layout, error, regionName.c_str());
    if (!d.region)
        return false;

    // The clock: free-running on the local clock (near TAI) until PTP locks.
    d.discipline = std::make_unique<ClockDiscipline>(wallNs() + 37'000'000'000LL - monoNowNs());
    d.region->clock().publish(d.discipline->current(monoNowNs()));
#if defined(INLET_HAVE_PTP)
    std::string ptpError;
    d.ptp = PtpFollower::open(c.ptpDomain, { d.iface.id }, ptpError);
    if (!d.ptp)
        d.ptpReason = ptpError;
#else
    d.ptpReason = "this build of Inlet has no PTP follower";
#endif

    {
        char err[160] = { 0 };
        d.netRx.reset(aes67_netrx_open(d.iface.ipv4.c_str(), err, sizeof err));
        if (!d.netRx)
        {
            error = err;
            return false;
        }
    }
    d.netTx = NetTx::open(d.iface.ipv4, d.region->clock(), error);
    if (!d.netTx)
        return false;
    aes67_netrx_start(d.netRx.get());
    d.netTx->start();

    d.sap = std::make_unique<Sap>(d.iface.ipv4);
    if (!d.sap->start(d.sapWarning))
    {
        std::fprintf(stderr, "inletd: %s; streams will not be announced or heard by SAP\n", d.sapWarning.c_str());
        d.sap.reset();
    }
    else if (!d.sapWarning.empty())
        std::fprintf(stderr, "inletd: %s\n", d.sapWarning.c_str());

    StreamManager::Settings ss;
    ss.ifaceIp = d.iface.ipv4;
    ss.ifaceMac = d.iface.mac;
    ss.sampleRate = c.sampleRate;
    ss.senders = c.senders;
    ss.senderChannels = c.senderChannels;
    ss.linkOffsetFrames = c.linkOffsetFrames();
    ss.labelPrefix = c.labelPrefix;
    d.streams = std::make_unique<StreamManager>(*d.region, d.netRx.get(), *d.netTx, d.sap.get(), ss);
    if (d.sap)
        d.sap->onHeard = [&d](const std::string& key, const std::string& sdp) { d.streams->sapHeard(key, sdp); };

    NmosNode::Config nc;
    const std::string host = hostName();
    nc.label = c.label.empty() ? "Inlet (" + host + ")" : c.label;
    nc.description = "Inlet AES67 virtual sound card";
    nc.hostname = host;
    nc.address = d.iface.ipv4;
    nc.port = c.apiPort;
    nc.nodeId = c.nodeId;
    nc.deviceId = c.deviceId;
    nc.registryUrl = c.registryUrl;
    nc.interfaceName = d.iface.name;
    nc.chassisId = d.iface.mac;
    nc.sampleRate = (int) c.sampleRate;
    nc.receiveRates.assign(std::begin(StreamManager::kReceiveRates), std::end(StreamManager::kReceiveRates));
    nc.services = nlohmann::json::array({ { { "href", "http://" + d.iface.ipv4 + ":" + std::to_string(c.apiPort) + "/inlet/v1" },
                                            { "type", kServiceType } } });
    nc.tags = { { "urn:x-meros:product", nlohmann::json::array({ "inlet" }) },
                { "urn:x-meros:version", nlohmann::json::array({ kVersion }) },
                { "urn:x-meros:os", nlohmann::json::array({ osDescription() }) } };
    // With NMOS off, nothing is advertised or registered and /x-nmos is not
    // served: a program that runs inletd presents its streams in its own node.
    if (c.nmos)
        d.nmos = std::make_unique<NmosNode>(*d.streams, nc);
    d.streams->onChanged = [&d] {
        d.dirty = true;
        if (d.nmos)
            d.nmos->resourcesChanged();
    };

    d.streams->restoreState(c.state);
    d.streams->start();

    d.routes();
    d.httpThread = std::thread([&d] { d.http.listen_after_bind(); });
    if (d.nmos)
        d.nmos->start();

    d.running = true;
    d.clockThread = std::thread([&d] { d.clockLoop(); });
    d.directory.publish(regionName);   // drivers attach from here on
    std::fprintf(stderr, "inletd: running on %s (%s), API on port %d, %u in / %u out at %u Hz%s%s\n", d.iface.name.c_str(),
                 d.iface.ipv4.c_str(), c.apiPort, c.inputs, c.outputs, c.sampleRate, d.ptpReason.empty() ? "" : "; PTP: ",
                 d.ptpReason.c_str());
    return true;
}

void Engine::stop()
{
    auto& d = *impl;
    if (d.running.exchange(false) && d.clockThread.joinable())
        d.clockThread.join();
    if (d.nmos)
        d.nmos->stop();
    d.http.stop();
    if (d.httpThread.joinable())
        d.httpThread.join();
    if (d.streams)
    {
        d.persist();
        d.streams->stop();
    }
    if (d.sap)
        d.sap->stop();
    d.nmos.reset();
    d.streams.reset();
    d.sap.reset();
    d.netTx.reset();
    d.netRx.reset();
#if defined(INLET_HAVE_PTP)
    d.ptp.reset();
#endif
    d.region.reset();
}

nlohmann::json Engine::status() const
{
    return impl->status();
}

/* ---- the clock, the heartbeat, the clients ------------------------------------------------ */

void Engine::Impl::clockLoop()
{
    int64_t lastPersistNs = 0;
    uint8_t lastGm[8] = {};
    bool lastLocked = false;
    while (running.load())
    {
        const int64_t now = monoNowNs();
#if defined(INLET_HAVE_PTP)
        if (ptp)
        {
            PtpCalibration cal;
            int64_t offset = 0;
            if (ptp->latest(cal, offset))
                discipline->update(cal, offset, now);
        }
#endif
        const ClockSample s = discipline->current(now);
        region->clock().publish(s);
        region->header().daemonHeartbeatMono.store(now);
        watchClients(now);

        // The NMOS node describes the clock; tell it when that changes.
        const bool locked = s.state == ClockState::Locked;
        if (nmos && (locked != lastLocked || std::memcmp(lastGm, s.gmIdentity, 8) != 0))
        {
            NmosNode::Clock nc;
            nc.ptp = s.state != ClockState::FreeRun;
            nc.locked = locked;
            std::memcpy(nc.gmIdentity, s.gmIdentity, 8);
            nmos->setClock(nc);
            std::memcpy(lastGm, s.gmIdentity, 8);
            lastLocked = locked;
        }
        // Persist at most every two seconds.
        if (dirty.load() && now - lastPersistNs > 2'000'000'000LL)
        {
            dirty = false;
            lastPersistNs = now;
            persist();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Engine::Impl::watchClients(int64_t now)
{
    // A host that crashed leaves its slot attached and, maybe, the outputs
    // claimed: freed after two seconds without a heartbeat.
    shm::ClientSlot* slots = region->clients();
    for (int i = 0; i < shm::kMaxClients; ++i)
    {
        if (slots[i].state.load() != 2)
            continue;
        if (now - slots[i].heartbeatMono.load() > 2'000'000'000LL && slots[i].running.load())
        {
            for (auto& layer : region->header().outputLayers)
            {
                uint32_t me = (uint32_t) i + 1;
                layer.compare_exchange_strong(me, 0);
            }
            slots[i].running.store(0);
        }
        // Attached but silent for a minute (a host that loaded the driver and
        // died): the slot is given back.
        if (now - slots[i].heartbeatMono.load() > 60'000'000'000LL)
            slots[i].state.store(0);
    }
}

void Engine::Impl::persist()
{
    std::lock_guard<std::mutex> g(configLock);
    config.state = streams->persistentState();
    std::string error;
    if (!config.save(configPath, error))
        std::fprintf(stderr, "inletd: %s\n", error.c_str());
}

/* ---- reports ---------------------------------------------------------------------------- */

nlohmann::json Engine::Impl::ptpJson() const
{
    // The shape of a console's ptp_update without its `type` key, so a console
    // reads it as its own.
    ClockSample s;
    region->clock().read(s);
#if defined(__linux__)
    const char* source = "linuxptp";
#else
    const char* source = "gstreamer";
#endif
    nlohmann::json j = { { "source", source }, { "domain", config.ptpDomain },
                         { "expectedDomain", config.ptpDomain }, { "expectedProfile", "aes67" }, { "utcOffsetS", 37 },
                         { "locked", s.state == ClockState::Locked }, { "state", stateName(s.state) },
                         { "epoch", s.epoch } };
#if defined(INLET_HAVE_PTP)
    if (ptp)
    {
        const PtpStatus st = ptp->status();
        j["available"] = true;
        j["reason"] = st.reason;
        j["source"] = st.source;
        j["detail"] = st.detail;
        j["gmPresent"] = st.gmPresent;
        j["gmIdentity"] = st.gmPresent ? pmcIdentity(st.gmIdentity) : "";
        j["portState"] = st.synced ? "SLAVE" : (st.gmPresent ? "UNCALIBRATED" : "LISTENING");
        j["offsetNs"] = st.lastDiscontinuityNs;
        j["meanPathDelayNs"] = st.meanPathDelayNs;
        j["mismatch"] = "";
        return j;
    }
#endif
    j["available"] = false;
    j["reason"] = ptpReason;
    j["gmPresent"] = false;
    j["gmIdentity"] = "";
    j["portState"] = "";
    j["mismatch"] = "";
    return j;
}

nlohmann::json Engine::Impl::clientsJson() const
{
    auto out = nlohmann::json::array();
    shm::ClientSlot* slots = region->clients();
    for (int i = 0; i < shm::kMaxClients; ++i)
    {
        if (slots[i].state.load() != 2)
            continue;
        bool writesOutputs = false;
        for (auto& layer : region->header().outputLayers)
            writesOutputs = writesOutputs || layer.load() == (uint32_t) i + 1;
        const auto kind = (shm::ClientKind) slots[i].kind.load();
        const char* kindName = kind == shm::ClientKind::Asio        ? "asio"
                             : kind == shm::ClientKind::CoreAudio ? "coreaudio"
                             : kind == shm::ClientKind::Alsa      ? "alsa"
                             : kind == shm::ClientKind::PipeWire  ? "pipewire"
                                                                  : "other";
        out.push_back({ { "kind", kindName },
                        { "app", std::string(slots[i].app, strnlen(slots[i].app, shm::kLabelBytes)) },
                        { "pid", slots[i].pid.load() }, { "running", slots[i].running.load() != 0 },
                        { "bufferFrames", slots[i].bufferFrames.load() }, { "periods", slots[i].periods.load() },
                        { "lateWakeups", slots[i].lateWakeups.load() }, { "resyncs", slots[i].resyncs.load() },
                        { "ownsOutputs", writesOutputs }, { "sampleRate", slots[i].sampleRate.load() } });
    }
    return out;
}

nlohmann::json Engine::Impl::status() const
{
    const auto clients = clientsJson();
    uint32_t buffer = 0;
    // The spec's string lists: the programs using Inlet, and the drivers
    // they came through. The detail is under `clients`.
    auto hosts = nlohmann::json::array(), drivers = nlohmann::json::array();
    for (auto& c : clients)
    {
        if (c.value("running", false) && buffer == 0)
            buffer = c.value("bufferFrames", 0u);
        hosts.push_back(c.value("app", std::string()));
        const std::string kind = c.value("kind", std::string());
        const std::string name = kind == "asio" ? "ASIO" : kind == "coreaudio" ? "Core Audio" : kind;
        if (std::find(drivers.begin(), drivers.end(), name) == drivers.end())
            drivers.push_back(name);
    }
    auto receivers = streams->receiversJson();
    for (auto& r : receivers)
        r["id"] = NmosNode::receiverId(config.deviceId, r.value("index", 0));
    auto senders = streams->sendersJson();
    for (auto& s : senders)
        s["id"] = NmosNode::senderId(config.deviceId, s.value("index", 0));
    return { { "product", "inlet" }, { "version", kVersion }, { "host", hostName() }, { "os", osDescription() },
             { "interface", { { "name", iface.name }, { "ip", iface.ipv4 }, { "mac", iface.mac } } },
             { "sampleRate", config.sampleRate }, { "bufferSize", buffer }, { "inputs", config.inputs },
             { "outputs", config.outputs }, { "linkOffsetFrames", region->header().linkOffsetFrames.load() },
             { "drivers", drivers }, { "hosts", hosts }, { "clients", clients }, { "ptp", ptpJson() },
             { "sap", { { "announcing", sap != nullptr }, { "listening", sap != nullptr && sap->listening() }, { "warning", sap != nullptr ? sap->warning() : sapWarning } } },
             { "receivers", receivers }, { "senders", senders },
             { "nmos", { { "enabled", nmos != nullptr }, { "nodeId", config.nodeId }, { "deviceId", config.deviceId } } } };
}

int Engine::Impl::receiverIndexOf(const std::string& id) const
{
    for (uint32_t k = 1; k <= config.receivers; ++k)
        if (NmosNode::receiverId(config.deviceId, (int) k) == id)
            return (int) k;
    return 0;
}

int Engine::Impl::senderIndexOf(const std::string& id) const
{
    for (uint32_t k = 1; k <= config.senders; ++k)
        if (NmosNode::senderId(config.deviceId, (int) k) == id)
            return (int) k;
    return 0;
}

/* ---- the API -------------------------------------------------------------------------------- */

void Engine::Impl::routes()
{
    // NMOS: everything under /x-nmos and /manifests goes to the node.
    auto nmosHandler = [this](const httplib::Request& req, httplib::Response& res) {
        NmosNode::HttpResponse out;
        if (!nmos || !nmos->handleHttp(req.method, req.path, req.body, out))
            out = { 404, "application/json", R"({"code":404,"error":"not found"})" };
        res.status = out.status;
        res.set_content(out.body, out.contentType);
    };
    for (const char* pattern : { R"(/x-nmos(/.*)?)", R"(/manifests/.*)" })
    {
        http.Get(pattern, nmosHandler);
        http.Post(pattern, nmosHandler);
        http.Patch(pattern, nmosHandler);
        http.Put(pattern, nmosHandler);
        http.Delete(pattern, nmosHandler);
    }

    // For the app on this computer only.
    auto local = [](const httplib::Request& req, httplib::Response& res) {
        if (isLoopback(req.remote_addr))
            return true;
        sendError(res, 403, "only the Inlet app on this computer may do that");
        return false;
    };
    auto parse = [](const httplib::Request& req, httplib::Response& res, nlohmann::json& out) {
        try
        {
            out = req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
            return true;
        }
        catch (const std::exception& e)
        {
            sendError(res, 400, std::string("the body is not JSON: ") + e.what());
            return false;
        }
    };

    http.Get("/inlet/v1/?", [](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, nlohmann::json::array({ "status/", "labels/", "profiles/", "settings/", "interfaces/", "streams/",
                                                   "receivers/", "senders/", "nmos/", "peers/" }));
    });
    http.Get("/inlet/v1/status", [this](const httplib::Request&, httplib::Response& res) { sendJson(res, 200, status()); });

    http.Put("/inlet/v1/labels", [this, parse](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse(req, res, body))
            return;
        std::string error;
        if (!streams->applyLabels(
                body, [this](const std::string& id) { return receiverIndexOf(id); },
                [this](const std::string& id) { return senderIndexOf(id); }, error))
            return sendError(res, 400, error);
        sendJson(res, 200, { { "inputs", streams->deviceInputLabels() } });
    });

    http.Post(R"(/inlet/v1/profiles/(multitrack|playback))", [this, parse](const httplib::Request& req, httplib::Response& res) {
        // What a console needs to route N channels to (multitrack) or from
        // (playback) this computer: the slots that will carry them. The
        // device's shape is set in the app; this never changes it.
        nlohmann::json body;
        if (!parse(req, res, body))
            return;
        const bool multitrack = req.matches[1] == "multitrack";
        const int n = body.value("channels", 0);
        const int per = (int) (multitrack ? config.receiverChannels : config.senderChannels);
        const int slots = (int) (multitrack ? config.receivers : std::min(config.senders, config.outputs / std::max(1u, config.senderChannels)));
        const int need = n <= 0 ? 0 : (n + per - 1) / per;
        if (n <= 0)
            return sendError(res, 400, "channels must be a positive number");
        if (need > slots)
            return sendJson(res, 409, { { "code", 409 },
                                        { "error", "Inlet has " + std::to_string(slots) + (multitrack ? " receivers" : " senders") + " of "
                                                       + std::to_string(per) + " channels; " + std::to_string(n) + " channels need "
                                                       + std::to_string(need) + ". Change this in the Inlet app." } });
        auto ids = nlohmann::json::array();
        for (int k = 1; k <= need; ++k)
            ids.push_back({ { "index", k },
                            { "id", multitrack ? NmosNode::receiverId(config.deviceId, k) : NmosNode::senderId(config.deviceId, k) },
                            { "channels", std::min(per, n - (k - 1) * per) } });
        sendJson(res, 200, { { multitrack ? "receivers" : "senders", ids } });
    });

    http.Get("/inlet/v1/settings", [this, local](const httplib::Request& req, httplib::Response& res) {
        if (!local(req, res))
            return;
        std::lock_guard<std::mutex> g(configLock);
        sendJson(res, 200, config.settingsJson());
    });
    http.Put("/inlet/v1/settings", [this, local, parse](const httplib::Request& req, httplib::Response& res) {
        if (!local(req, res))
            return;
        nlohmann::json body;
        if (!parse(req, res, body))
            return;
        std::string error;
        Config next;
        {
            std::lock_guard<std::mutex> g(configLock);
            next = config;
        }
        if (!next.applySettings(body, error))
            return sendError(res, 400, error);
        next.state = streams->persistentState();
        if (!next.save(configPath, error))
            return sendError(res, 500, error);
        sendJson(res, 202, { { "restarting", true } });
        if (owner->onRestart)
            owner->onRestart(next);
    });
    http.Post("/inlet/v1/stop", [this, local](const httplib::Request& req, httplib::Response& res) {
        if (!local(req, res))
            return;
        sendJson(res, 202, { { "stopping", true } });
        if (owner->onStop)
            owner->onStop();
    });
    http.Get("/inlet/v1/interfaces", [local](const httplib::Request& req, httplib::Response& res) {
        if (!local(req, res))
            return;
        auto out = nlohmann::json::array();
        for (auto& n : listInterfaces())
            out.push_back({ { "name", n.name }, { "ip", n.ipv4 }, { "mac", n.mac }, { "up", n.up }, { "wireless", n.wireless },
                            { "loopback", n.loopback } });
        sendJson(res, 200, out);
    });

    http.Get("/inlet/v1/streams", [this](const httplib::Request&, httplib::Response& res) { sendJson(res, 200, streams->streamsJson()); });
    http.Post("/inlet/v1/streams", [this, local, parse](const httplib::Request& req, httplib::Response& res) {
        if (!local(req, res))
            return;
        nlohmann::json body;
        if (!parse(req, res, body))
            return;
        std::string id, error;
        if (!streams->addManualStream(body.value("sdp", std::string()), id, error))
            return sendError(res, 400, error);
        sendJson(res, 201, { { "id", id } });
    });
    http.Delete(R"(/inlet/v1/streams/(.+))", [this, local](const httplib::Request& req, httplib::Response& res) {
        if (!local(req, res))
            return;
        if (!streams->removeStream(req.matches[1]))
            return sendError(res, 404, "no such stream");
        res.status = 204;
    });

    http.Get("/inlet/v1/receivers", [this](const httplib::Request&, httplib::Response& res) { sendJson(res, 200, status()["receivers"]); });
    http.Put(R"(/inlet/v1/receivers/(\d+))", [this, local, parse](const httplib::Request& req, httplib::Response& res) {
        if (!local(req, res))
            return;
        nlohmann::json body;
        if (!parse(req, res, body))
            return;
        const int k = std::stoi(req.matches[1]);
        std::string error;
        bool ok;
        if (body.contains("streamId"))
            ok = streams->subscribeToStream(k, body.value("streamId", std::string()), error);
        else
            ok = streams->subscribeReceiver(k, body.value("sdp", std::string()), body.value("senderId", std::string()), error);
        if (!ok)
            return sendError(res, 400, error);
        sendJson(res, 200, { { "index", k } });
    });
    http.Delete(R"(/inlet/v1/receivers/(\d+))", [this, local](const httplib::Request& req, httplib::Response& res) {
        if (!local(req, res))
            return;
        streams->unsubscribeReceiver(std::stoi(req.matches[1]));
        res.status = 204;
    });

    http.Get("/inlet/v1/senders", [this](const httplib::Request&, httplib::Response& res) { sendJson(res, 200, status()["senders"]); });
    http.Put(R"(/inlet/v1/senders/(\d+))", [this, local, parse](const httplib::Request& req, httplib::Response& res) {
        if (!local(req, res))
            return;
        nlohmann::json body;
        if (!parse(req, res, body))
            return;
        std::string error;
        if (!streams->configureSender(std::stoi(req.matches[1]), body, error))
            return sendError(res, 400, error);
        sendJson(res, 200, { { "index", std::stoi(req.matches[1]) } });
    });

    http.Get("/inlet/v1/nmos", [this](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, nmos ? nmos->toJson() : nlohmann::json { { "enabled", false } });
    });
    http.Post("/inlet/v1/peers", [this, local, parse](const httplib::Request& req, httplib::Response& res) {
        if (!local(req, res))
            return;
        if (!nmos)
            return sendError(res, 409, "NMOS is off in the settings");
        nlohmann::json body;
        if (!parse(req, res, body))
            return;
        nmos->addPeer(body.value("href", std::string()));
        res.status = 204;
    });
    http.Delete("/inlet/v1/peers", [this, local, parse](const httplib::Request& req, httplib::Response& res) {
        if (!local(req, res))
            return;
        if (!nmos)
            return sendError(res, 409, "NMOS is off in the settings");
        nlohmann::json body;
        if (!parse(req, res, body))
            return;
        nmos->removePeer(body.value("href", std::string()));
        res.status = 204;
    });
}

} // namespace inlet
