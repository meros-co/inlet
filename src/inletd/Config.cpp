// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "Config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace inlet
{

std::string Config::defaultPath()
{
#if defined(_WIN32)
    const char* pd = std::getenv("ProgramData");
    return std::string(pd ? pd : "C:\\ProgramData") + "\\Inlet\\inletd.json";
#elif defined(__APPLE__)
    return "/Library/Application Support/Inlet/inletd.json";
#else
    return "/var/lib/inlet/inletd.json";
#endif
}

nlohmann::json Config::settingsJson() const
{
    return { { "interfaceIp", interfaceIp }, { "ptpDomain", ptpDomain }, { "sampleRate", sampleRate },
             { "inputs", inputs }, { "outputs", outputs }, { "receivers", receivers },
             { "receiverChannels", receiverChannels }, { "senders", senders }, { "senderChannels", senderChannels },
             { "linkOffsetUs", linkOffsetUs }, { "apiPort", apiPort }, { "label", label },
             { "registryUrl", registryUrl } };
}

bool Config::applySettings(const nlohmann::json& j, std::string& error)
{
    if (!j.is_object())
    {
        error = "settings must be an object";
        return false;
    }
    Config next = *this;
    try
    {
        next.interfaceIp = j.value("interfaceIp", next.interfaceIp);
        next.ptpDomain = j.value("ptpDomain", next.ptpDomain);
        next.sampleRate = j.value("sampleRate", next.sampleRate);
        next.inputs = j.value("inputs", next.inputs);
        next.outputs = j.value("outputs", next.outputs);
        next.receivers = j.value("receivers", next.receivers);
        next.receiverChannels = j.value("receiverChannels", next.receiverChannels);
        next.senders = j.value("senders", next.senders);
        next.senderChannels = j.value("senderChannels", next.senderChannels);
        next.linkOffsetUs = j.value("linkOffsetUs", next.linkOffsetUs);
        next.apiPort = j.value("apiPort", next.apiPort);
        next.label = j.value("label", next.label);
        next.registryUrl = j.value("registryUrl", next.registryUrl);
    }
    catch (const std::exception& e)
    {
        error = std::string("a setting has the wrong type: ") + e.what();
        return false;
    }
    if (next.ptpDomain < 0 || next.ptpDomain > 127)
        error = "the PTP domain is 0 to 127";
    else if (next.sampleRate != 44100 && next.sampleRate != 48000 && next.sampleRate != 88200 && next.sampleRate != 96000)
        error = "the sample rate is 44100, 48000, 88200 or 96000";
    else if (next.inputs == 0 && next.outputs == 0)
        error = "the device needs inputs or outputs";
    else if (next.receiverChannels == 0 || next.receiverChannels > 80 || next.senderChannels == 0 || next.senderChannels > 80)
        error = "a stream carries 1 to 80 channels";
    else if (next.apiPort <= 0 || next.apiPort > 65535)
        error = "the API port is 1 to 65535";
    if (!error.empty())
        return false;
    *this = next;
    return true;
}

uint32_t Config::linkOffsetFrames() const
{
    // A 1 ms packet's last sample leaves a millisecond after its first, then
    // the send offset and the network: 1 ms is always late, 2 ms left too
    // little margin in end-to-end tests on Windows and on Linux, 3 ms ran
    // clean on both.
    const uint32_t us = linkOffsetUs ? linkOffsetUs : 3000;
    return (uint32_t) ((uint64_t) us * sampleRate / 1000000u);
}

bool Config::load(const std::string& path, std::string& error)
{
    std::ifstream in(path);
    if (!in)
        return true;   // first run: the defaults
    std::stringstream ss;
    ss << in.rdbuf();
    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(ss.str());
    }
    catch (const std::exception& e)
    {
        error = path + " is not JSON: " + e.what();
        return false;
    }
    if (!applySettings(j.value("settings", nlohmann::json::object()), error))
        return false;
    nodeId = j.value("nodeId", std::string());
    deviceId = j.value("deviceId", std::string());
    state = j.value("state", nlohmann::json::object());
    return true;
}

bool Config::save(const std::string& path, std::string& error) const
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const nlohmann::json j = { { "settings", settingsJson() }, { "nodeId", nodeId }, { "deviceId", deviceId }, { "state", state } };
    // Written aside and moved over: a crash mid-write never leaves half a file.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
        {
            error = "cannot write " + tmp;
            return false;
        }
        out << j.dump(2);
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        error = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace inlet
