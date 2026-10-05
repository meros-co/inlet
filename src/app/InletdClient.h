// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * The app's line to inletd: its local API on this computer
 * (http://127.0.0.1:8091/inlet/v1). A thread polls the status, the stream
 * list, the settings and the interfaces once a second and hands them to the
 * message thread; actions (subscribe, enable, apply settings) run on the same
 * thread and are followed by a fresh poll. The UI never blocks on the network.
 */

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>

namespace inlet::app
{

class InletdClient : private juce::Thread
{
public:
    struct Snapshot
    {
        bool reachable = false;
        juce::String error;          ///< why not, when not
        juce::var status, streams, settings, interfaces;
    };

    InletdClient();
    ~InletdClient() override;

    /** Message thread, after every poll. */
    std::function<void(const Snapshot&)> onSnapshot;
    /** Message thread, when an action finishes: ok, and the server's reason when not. */
    std::function<void(bool ok, const juce::String& message)> onActionDone;

    using Done = std::function<void(bool ok, const juce::String& message)>;
    /** Queues a request ("PUT", "/receivers/1", body) to run after the current
        poll. `done`, when given, is called instead of onActionDone. */
    void request(const juce::String& method, const juce::String& path, const juce::var& body = {}, Done done = {});

    void pollNow() { notify(); }

    static constexpr int kPort = 8091;
    /** The port asked of inletd: kPort, or another for a debug build's tests. */
    static inline int port = kPort;

private:
    void run() override;
    bool call(const juce::String& method, const juce::String& path, const juce::String& body, int& status, juce::String& response);

    struct Pending { juce::String method, path; juce::var body; Done done; };
    juce::CriticalSection lock;
    juce::Array<Pending> pending;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace inlet::app
