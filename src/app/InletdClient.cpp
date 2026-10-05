// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "InletdClient.h"

namespace inlet::app
{

InletdClient::InletdClient() : juce::Thread("inletd client")
{
    startThread();
}

InletdClient::~InletdClient()
{
    alive->store(false);   // callbacks already queued see this and do nothing
    stopThread(4000);
}

void InletdClient::request(const juce::String& method, const juce::String& path, const juce::var& body, Done done)
{
    {
        const juce::ScopedLock sl(lock);
        pending.add({ method, path, body, std::move(done) });
    }
    notify();
}

bool InletdClient::call(const juce::String& method, const juce::String& path, const juce::String& body, int& status,
                        juce::String& response)
{
    juce::URL url("http://127.0.0.1:" + juce::String(port) + "/inlet/v1" + path);
    if (body.isNotEmpty())
        url = url.withPOSTData(body);
    status = 0;
    auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                       .withConnectionTimeoutMs(1500)
                       .withHttpRequestCmd(method)
                       .withExtraHeaders("Content-Type: application/json\r\n")
                       .withStatusCode(&status);
    auto stream = url.createInputStream(options);
    if (stream == nullptr)
        return false;
    response = stream->readEntireStreamAsString();
    return true;
}

void InletdClient::run()
{
    while (!threadShouldExit())
    {
        // Actions first, so the poll that follows shows their effect.
        juce::Array<Pending> todo;
        {
            const juce::ScopedLock sl(lock);
            todo.swapWith(pending);
        }
        for (auto& p : todo)
        {
            int status = 0;
            juce::String response;
            const auto body = p.body.isVoid() ? juce::String() : juce::JSON::toString(p.body, true);
            const bool reached = call(p.method, p.path, body, status, response);
            const bool ok = reached && status >= 200 && status < 300;
            juce::String message;
            if (!reached)
                message = "Inlet's service did not answer.";
            else if (!ok)
            {
                const auto parsed = juce::JSON::parse(response);
                message = parsed.getProperty("error", response).toString();
            }
            juce::MessageManager::callAsync([this, a = alive, ok, message, done = p.done] {
                if (!a->load())
                    return;
                if (done)
                    done(ok, message);
                else if (onActionDone)
                    onActionDone(ok, message);
            });
        }

        Snapshot s;
        int status = 0;
        juce::String body;
        if (call("GET", "/status", {}, status, body) && status == 200)
        {
            s.reachable = true;
            s.status = juce::JSON::parse(body);
            if (call("GET", "/streams", {}, status, body) && status == 200)
                s.streams = juce::JSON::parse(body);
            if (call("GET", "/settings", {}, status, body) && status == 200)
                s.settings = juce::JSON::parse(body);
            if (call("GET", "/interfaces", {}, status, body) && status == 200)
                s.interfaces = juce::JSON::parse(body);
        }
        else
            s.error = "Inlet's service (inletd) is not running on this computer.";
        juce::MessageManager::callAsync([this, a = alive, s] {
            if (a->load() && onSnapshot)
                onSnapshot(s);
        });
        wait(1000);
    }
}

} // namespace inlet::app
