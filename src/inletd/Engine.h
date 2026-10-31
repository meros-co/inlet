// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * Everything inletd runs, assembled from one Config: the shared region, PTP
 * (GStreamer's follower, or linuxptp on Linux), the receive and send paths, SAP,
 * the stream manager, the NMOS node and the HTTP server that carries NMOS and
 * the Inlet API.
 *
 * The Inlet API (`/inlet/v1`):
 *   GET  /inlet/v1/status                 host, OS, drivers, rate, PTP, health
 *   PUT  /inlet/v1/labels                 channel names, from a console
 *   POST /inlet/v1/profiles/{multitrack|playback}
 * and, for the Inlet app or another program on this computer only (loopback),
 * everything inletd needs to run without the app:
 *   GET/PUT /inlet/v1/settings            channel counts, rate, labelPrefix, nmos (false: no node), ...
 *   GET /inlet/v1/interfaces,
 *   GET/POST /inlet/v1/streams, DELETE /inlet/v1/streams/{id}   heard by SAP or NMOS, or an SDP
 *   GET /inlet/v1/receivers               with health: packets, lost, late, ptpOffsetUs, last packet
 *   PUT /inlet/v1/receivers/{k}           {streamId} or {sdp, senderId}; DELETE stops it
 *   GET /inlet/v1/senders                 with each open sender's SDP
 *   PUT /inlet/v1/senders/{k}             any of label, group, port, firstOutput, enabled
 *   GET /inlet/v1/nmos, POST/DELETE /inlet/v1/peers,
 *   POST /inlet/v1/stop (the app's Stop Inlet: the whole process quits)
 *
 * A settings change that alters the device's shape (channels, rate,
 * interface) needs the engine rebuilt: `onRestart` asks the owner to do it.
 */

#include "Config.h"
#include "NetInfo.h"

#include "inlet/SharedRegion.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace inlet
{

class Engine
{
public:
    /** `directory`: held by the process for its life; each engine publishes
        its own region there. */
    Engine(Config& config, const std::string& configPath, shm::Directory& directory);
    ~Engine();

    /** False, with the reason, when the engine cannot run at all (no
        interface, the API port taken, no shared memory). PTP missing is not
        fatal: the device free-runs and says why. */
    bool start(std::string& error);
    void stop();

    /** Called (on an API thread) with new settings, saved already, that need
        the engine rebuilt; the owner stops this engine and starts another. */
    std::function<void(const Config&)> onRestart;
    /** Called (on an API thread, after the reply) when the app asks Inlet to
        stop: the owner stops this engine and quits. */
    std::function<void()> onStop;

    nlohmann::json status() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};

} // namespace inlet
