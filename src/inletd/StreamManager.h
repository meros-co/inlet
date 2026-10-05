// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * inletd's model of the network: receiver and sender slots, the streams on the
 * wire, and how they meet the device the drivers present.
 *
 *  - Receivers ("Net In k") each play one stream into their rings in the
 *    shared region. A stream on Inlet's own grandmaster at the device's rate
 *    is written straight there, indexed by the media clock; any other (another
 *    grandmaster, a sender's own clock, another rate) goes through a
 *    RateBridge first. The IS-08 input map says which received channel each
 *    device input plays.
 *  - Senders ("Net Out k") each carry a contiguous range of the device's
 *    outputs, paced by NetTx, announced by SAP and published by NMOS.
 *  - Streams: what could be listened to -- heard by SAP, found on NMOS peers,
 *    or pasted -- offered in the app.
 *
 * Thread-safe: the NMOS node, the API server and the manager's own thread
 * (bridges, SAP expiry) all call in.
 */

#include "NodeModel.h"

#include "inlet/Clock.h"
#include "inlet/NetRx.h"
#include "inlet/NetTx.h"
#include "inlet/RateBridge.h"
#include "inlet/SharedRegion.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct aes67_ring;

namespace inlet
{

class Sap;

class StreamManager : public NodeModel
{
public:
    struct Settings
    {
        std::string ifaceIp;
        std::string ifaceMac;            ///< for ts-refclk:localmac when free-running
        uint32_t sampleRate = 48000;
        uint32_t senders = 8;
        uint32_t senderChannels = 8;
        uint16_t port = 5004;
        std::string groupPrefix;         ///< "239.69.<last octet>" by default; sender k is <prefix>.<k>
        uint32_t linkOffsetFrames = 96;
        std::string labelPrefix = "Net";
    };

    StreamManager(shm::Region& region, NetRx& rx, NetTx& tx, Sap* sap, const Settings& settings);

    /** The stream rates a receiver plays: the device's own directly, the rest
        through the rate bridge (tested converting into 48 kHz). */
    static constexpr int kReceiveRates[] = { 44100, 48000, 88200, 96000 };
    ~StreamManager() override;

    void start();
    void stop();

    // ---- NodeModel ------------------------------------------------------------------
    std::vector<ReceiverInfo> receivers() const override;
    std::vector<SenderInfo> senders() const override;
    std::string senderSdp(int index) const override;
    bool subscribeReceiver(int index, const std::string& sdp, const std::string& senderId, std::string& error) override;
    void unsubscribeReceiver(int index) override;
    bool setSenderEnabled(int index, bool enabled, std::string& error) override;
    std::vector<std::string> deviceInputLabels() const override;
    std::vector<InputMapCell> inputMap() const override;
    bool setInputMap(const std::vector<std::pair<int, InputMapCell>>& changes, std::string& error) override;
    void upsertNmosStream(const std::string& senderId, const std::string& name, const std::string& sdp,
                          const std::vector<std::string>& channelLabels) override;
    void expireNmosStreamsExcept(const std::vector<std::string>& keepSenderIds) override;

    // ---- the app and the Inlet API -----------------------------------------------------
    /** SAP heard (sdp empty = deleted). */
    void sapHeard(const std::string& key, const std::string& sdp);
    /** An SDP pasted by the operator; returns its stream id. */
    bool addManualStream(const std::string& sdp, std::string& id, std::string& error);
    bool removeStream(const std::string& id);
    /** Plays a listed stream on a receiver. */
    bool subscribeToStream(int receiver, const std::string& streamId, std::string& error);

    /** The Inlet API's PUT /labels: { receivers: [{id, channels}], senders: [{id, channels}] },
        ids being NMOS receiver and sender ids (resolved by `receiverIdOf`/`senderIdOf`). */
    bool applyLabels(const nlohmann::json& body, const std::function<int(const std::string&)>& receiverIndexOf,
                     const std::function<int(const std::string&)>& senderIndexOf, std::string& error);
    void setDeviceLabels(const std::vector<std::string>& inputs, const std::vector<std::string>& outputs);

    nlohmann::json streamsJson() const;
    nlohmann::json receiversJson() const;   ///< with health
    nlohmann::json sendersJson() const;
    /** What is worth keeping across restarts: subscriptions, map, labels, sender states. */
    nlohmann::json persistentState() const;
    void restoreState(const nlohmann::json& state);

    /** Something the NMOS node or the config should hear about changed. */
    std::function<void()> onChanged;

private:
    struct Receiver
    {
        int index = 0;
        std::string label;
        bool active = false;
        std::string sdpText, senderId, streamId;
        aes67_sdp_t sdp {};
        int rxSlot = -1;
        bool bridged = false;
        std::unique_ptr<aes67_ring[]> staging;     // the bridge's input, when bridged
        std::unique_ptr<RateBridge> bridge;
        std::string error;
    };
    struct Sender
    {
        int index = 0;
        std::string label;
        int channels = 8;
        int firstOutput = 0;
        std::string group;
        uint16_t port = 5004;
        bool enabled = true;
        int txSlot = -1;
        uint32_t ptimeUs = 1000;
    };
    struct Stream
    {
        std::string id, name, sdp, via;   // via: "sap", "nmos", "manual"
        std::vector<std::string> channelLabels;
        int64_t lastHeardMs = 0;
    };

    void run();
    bool sharesOurClock(const Receiver& r) const;
    bool validateSdp(const std::string& text, aes67_sdp_t& out, std::string& error) const;
    bool openReceiver(Receiver& r, std::string& error);   // with lock held
    void closeReceiver(Receiver& r);
    bool openSender(Sender& s, std::string& error);
    void closeSender(Sender& s);
    std::string makeSdp(const Sender& s) const;
    void reevaluateClocks();
    void changed();

    shm::Region& region;
    NetRx& netRx;
    NetTx& netTx;
    Sap* sap;
    Settings cfg;

    mutable std::mutex lock;
    std::vector<Receiver> rx;
    std::vector<Sender> tx;
    std::map<std::string, Stream> streams;
    std::vector<std::string> inputLabels, outputLabels;
    uint8_t lastGm[8] = {};
    ClockState lastState = ClockState::FreeRun;

    std::atomic<bool> running { false };
    std::thread thread;
};

/** "audio, 8 channels, 48 kHz, L24" from a parsed SDP, for lists. */
std::string describeSdp(const aes67_sdp_t& sdp);

} // namespace inlet
