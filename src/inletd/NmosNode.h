// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * Inlet as an NMOS node: IS-04 (node API, registration), IS-05 (connection
 * management) and IS-08 (channel mapping).
 *
 * Ported from a console's NmosNode (proven end to end against an NMOS
 * controller), off JUCE and onto the NodeModel interface. What a controller needs from a device is
 * this surface and no more: a DNS-SD advert (`_nmos-node._tcp`), the IS-04
 * node API with an SDP manifest for every sender, the IS-05 connection API,
 * and registration with an IS-04 registry when one is on the network. With no
 * registry the node still answers peer-to-peer, which is how controllers and
 * consoles without one find it.
 *
 * Identity: every resource's id is stable across restarts, derived from the
 * stored node and device ids, because a controller correlates by id.
 *
 * Inlet's specifics:
 *  - its IS-04 node carries the first-party service
 *    `urn:x-meros:service:inlet/v1`, whose href is the Inlet local API, and
 *    product tags;
 *  - IS-08 inputs are the receivers; its one output is the device's inputs
 *    (what a DAW records), so a controller can say "Net In 3, channels 1-8,
 *    land on inputs 17-24".
 *
 * Threads: the HTTP server's (handleHttp, which applies IS-05 and IS-08
 * changes through the model) and this object's own (mDNS, registration,
 * heartbeats, peer crawling, scheduled activations).
 */

#include "NodeModel.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct mdns;

namespace inlet
{

class NmosNode
{
public:
    struct Config
    {
        std::string label;              ///< "Inlet (studio-pc)"
        std::string description;
        std::string hostname;           ///< bare host name, for mDNS and the node
        std::string address;            ///< the IPv4 the APIs are reachable at
        int port = 0;                   ///< the HTTP server's port
        std::string nodeId, deviceId;   ///< stored UUIDs; generated when empty
        std::string registryUrl;        ///< "http://host:port" to skip discovery
        std::string interfaceName = "eth0";
        std::string chassisId;          ///< MAC as xx-xx-xx-xx-xx-xx
        bool advertise = true;          ///< mDNS; off for tests
        int sampleRate = 48000;
        std::vector<int> receiveRates;  ///< rates receivers play (empty: sampleRate only)
        /** IS-04 services ({ href, type }) and tags, as the node publishes them. */
        nlohmann::json services = nlohmann::json::array();
        nlohmann::json tags = nlohmann::json::object();
    };

    /** The clock, as the node and its receivers describe it. */
    struct Clock
    {
        bool ptp = false;               ///< following PTP (else "internal")
        bool locked = false;
        uint8_t gmIdentity[8] = {};
        int utcOffsetS = 37;
    };

    struct HttpResponse
    {
        int status = 404;
        std::string contentType = "application/json";
        std::string body;
    };

    NmosNode(NodeModel& model, const Config& cfg);
    ~NmosNode();
    NmosNode(const NmosNode&) = delete;
    NmosNode& operator=(const NmosNode&) = delete;

    void start();
    void stop();

    const Config& config() const { return cfg; }

    void setClock(const Clock& clock);
    /** The model's slots or streams changed: versions bump, the registry hears. */
    void resourcesChanged();

    /** One request under /x-nmos/ or /manifests/. False: not the node's. */
    bool handleHttp(const std::string& method, const std::string& path, const std::string& body, HttpResponse& out);

    /** Registration and peers, for the Inlet API and the app. */
    nlohmann::json toJson() const;

    // ---- peers: other nodes, whose senders Inlet can play ---------------------
    void addPeer(const std::string& href);
    void removePeer(const std::string& href);
    std::vector<std::string> manualPeers() const;

    // ---- the resources, as IS-04 describes them --------------------------------
    nlohmann::json self() const;
    nlohmann::json device() const;
    nlohmann::json sources() const;
    nlohmann::json flows() const;
    nlohmann::json senders() const;
    nlohmann::json receivers() const;

    static std::string receiverId(const std::string& deviceId, int index);
    static std::string senderId(const std::string& deviceId, int index);
    static std::string flowId(const std::string& deviceId, int index);
    static std::string sourceId(const std::string& deviceId, int index);
    /** An IS-04 version or activation stamp for now: "<TAI seconds>:<nanoseconds>". */
    static std::string versionNow();
    /** Rewrites an SDP with IS-05 transport parameters laid over it. */
    static std::string withTransportParams(const std::string& sdp, const nlohmann::json& params);
    /** "seconds:nanoseconds" -> milliseconds; false when malformed. */
    static bool parseTaiTime(const nlohmann::json& t, int64_t& ms);

private:
    struct ReceiverStage
    {
        nlohmann::json senderId = nullptr;
        bool masterEnable = false;
        nlohmann::json transportFile = { { "data", nullptr }, { "type", nullptr } };
        nlohmann::json transportParams = { { "source_ip", nullptr }, { "multicast_ip", nullptr },
                                           { "destination_port", nullptr }, { "rtp_enabled", true } };
        nlohmann::json activation = { { "mode", nullptr }, { "requested_time", nullptr }, { "activation_time", nullptr } };
    };
    struct SenderStage
    {
        nlohmann::json masterEnable = nullptr;
        nlohmann::json activation = { { "mode", nullptr }, { "requested_time", nullptr }, { "activation_time", nullptr } };
    };
    struct RegistryCandidate { std::string url; int priority = 100; int64_t seenMs = 0; };
    struct Peer
    {
        std::string href, nodeId, label, connectionHref;
        bool manual = false, online = false;
        int64_t seenMs = 0, crawledMs = 0;
        std::string error;
        nlohmann::json senders = nlohmann::json::array();
        nlohmann::json receivers = nlohmann::json::array();
        nlohmann::json services = nlohmann::json::array();
    };

    void run();
    void wake();
    nlohmann::json resource(const std::string& type, int index) const;
    std::string versionFor(const std::string& id, const nlohmann::json& content) const;
    std::string baseHref() const;

    // IS-05
    nlohmann::json receiverStagedJson(int index, bool active) const;
    nlohmann::json senderStagedJson(int index) const;
    bool stageActivation(const nlohmann::json& body, nlohmann::json& activation, int64_t& dueMs, std::string& mode,
                         HttpResponse& out) const;
    bool receiverSdp(int index, const ReceiverStage& st, std::string& sdp, std::string& error) const;
    int activateReceiver(int index, ReceiverStage& st, std::string& error);
    int activateSender(int index, SenderStage& st, std::string& error);
    bool patchReceiver(int index, const nlohmann::json& body, HttpResponse& out);
    bool patchSender(int index, const nlohmann::json& body, HttpResponse& out);
    bool bulk(bool receivers, const std::string& body, HttpResponse& out);
    void runDueActivations();

    // IS-08
    bool handleChannelMapping(const std::string& method, const std::vector<std::string>& t, const std::string& body,
                              HttpResponse& out);
    nlohmann::json is08Input(const std::string& id) const;
    nlohmann::json is08Output(const std::string& id) const;
    nlohmann::json is08ActiveMap() const;

    // Registration and peers
    bool registerAll(std::string& error);
    bool registerChanged(std::string& error);
    bool postResource(const std::string& type, const nlohmann::json& data, std::string& error);
    bool heartbeat(std::string& error);
    /** On stop: deletes what was registered, children first, so the registry
        drops Inlet now rather than at its heartbeat timeout. */
    void unregisterAll();
    void chooseRegistry();
    void crawlPeers();
    bool crawlPeer(Peer& peer);
    bool getJson(const std::string& url, nlohmann::json& out, std::string& error);
    bool findSenderSdp(const std::string& senderId, std::string& sdpOut) const;

    NodeModel& model;
    Config cfg;
    Clock clock;
    mutable std::mutex lock;
    std::map<int, ReceiverStage> staged;
    std::map<int, SenderStage> stagedSenders;
    std::map<int, int64_t> receiverDue, senderDue;   // index -> UTC ms
    mutable std::map<std::string, std::pair<std::string, std::string>> versions;   // id -> (hash, version)
    std::string startVersion;
    std::map<std::string, std::string> registeredVersions;

    struct mdns* dns = nullptr;
    std::vector<RegistryCandidate> candidates;
    std::string registryUrl;
    bool registered = false;
    int heartbeatFailures = 0, registerFailures = 0;
    int64_t lastHeartbeatMs = 0, lastBrowseMs = 0;
    std::string lastError;
    std::vector<Peer> peers;

    std::atomic<bool> dirty { false };
    std::atomic<bool> running { false };
    std::thread thread;
    std::mutex wakeLock;
    std::condition_variable wakeCv;
    bool wakeFlag = false;
};

} // namespace inlet
