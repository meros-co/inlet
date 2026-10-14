// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * SAP (RFC 2974): how AES67 devices without NMOS find each other's streams.
 * Inlet listens on 239.255.255.255:9875 for announcements (a list of streams
 * to offer in the app) and announces its own senders there every 30 s, with a
 * deletion when a sender stops. When another program holds the port without
 * sharing it, Inlet announces only, and tries the port again every 5 s.
 *
 * The packet code is socket-free (encode/decode) and tested on its own.
 */

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

namespace inlet
{

struct SapPacket
{
    bool deletion = false;
    uint16_t hash = 0;
    uint32_t origin = 0;            ///< network order
    std::string payloadType;        ///< "application/sdp", or empty (implicit SDP)
    std::string payload;
};

bool sapDecode(const uint8_t* data, size_t len, SapPacket& out);
std::vector<uint8_t> sapEncode(const SapPacket& p);

class Sap
{
public:
    struct Heard
    {
        std::string key;            ///< origin + hash: the session, as SAP identifies it
        std::string sdp;
        int64_t lastHeardMs = 0;
    };

    /** `ifaceIp`: the interface to listen and announce on. */
    explicit Sap(const std::string& ifaceIp);
    ~Sap();

    /** False when SAP cannot run at all. True with `warning` set when it
        runs announce-only (another program holds the port, for now). */
    bool start(std::string& warning);
    bool listening() const { return rxSock.load() != -1; }
    /** Why it is announce-only; empty once it listens. */
    std::string warning() const;
    void stop();

    /** Announce (or re-announce, when the SDP changed) a session; withdraw it. */
    void announce(const std::string& id, const std::string& sdp);
    void withdraw(const std::string& id);

    /** One of this Inlet's own announcements (heard back), as opposed to
        another program's, even from the same address. */
    bool announcedHere(const SapPacket& p) const;
    /** The message hash a session of ours is announced with; 0 if none. */
    uint16_t hashOf(const std::string& id) const;

    /** Called on the SAP thread: a session was announced or deleted (sdp empty). */
    std::function<void(const std::string& key, const std::string& sdp)> onHeard;

private:
    void run();
    /** Binds and joins the listening socket; false with the reason. */
    bool openListener(std::string& error);
    void sendNow(const std::string& id, const std::string& sdp, bool deletion);

    std::string iface;
    uint32_t ifaceAddr = 0;
    std::atomic<intptr_t> rxSock { -1 };
    intptr_t txSock = -1;
    std::string listenError;   // guarded by `lock`
    std::atomic<bool> running { false };
    std::thread thread;
    mutable std::mutex lock;
    struct Ours { std::string sdp; uint16_t hash = 0; int64_t lastSentMs = 0; };
    std::map<std::string, Ours> ours;
    uint16_t nextHash = 1;
};

} // namespace inlet
