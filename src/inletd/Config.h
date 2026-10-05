// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * inletd's settings and the state it keeps across restarts, as one JSON file:
 * %ProgramData%\Inlet\inletd.json on Windows, /Library/Application Support/
 * Inlet/inletd.json on macOS. The app edits the settings through the local
 * API; the state (subscriptions, the input map, labels) is inletd's own.
 */

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace inlet
{

struct Config
{
    // The network
    std::string interfaceIp;         ///< empty: the first wired interface that is up
    int ptpDomain = 0;
    uint32_t sampleRate = 48000;

    // The device the drivers present
    uint32_t inputs = 64, outputs = 64;
    uint32_t receivers = 8, receiverChannels = 8;
    uint32_t senders = 8, senderChannels = 8;
    uint32_t linkOffsetUs = 0;       ///< 0: 2 ms on Windows, 1 ms elsewhere

    // NMOS and the API
    int apiPort = 8091;
    std::string label;               ///< empty: "Inlet (<host name>)"
    std::string registryUrl;
    std::string nodeId, deviceId;    ///< generated once, then kept

    nlohmann::json state = nlohmann::json::object();   ///< StreamManager::persistentState

    static std::string defaultPath();
    bool load(const std::string& path, std::string& error);
    bool save(const std::string& path, std::string& error) const;

    nlohmann::json settingsJson() const;
    /** Settings from the app; unknown keys ignored, bad values refused. */
    bool applySettings(const nlohmann::json& j, std::string& error);

    uint32_t linkOffsetFrames() const;
};

} // namespace inlet
