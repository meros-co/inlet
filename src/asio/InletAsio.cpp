// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "InletAsio.h"

#include "inlet/DeviceClient.h"

#include <cstdio>
#include <cstring>
#include <shellapi.h>
#include <timeapi.h>

void inletAsioObjectGone();   // DllMain.cpp: the module's object count

namespace inlet
{

namespace
{

constexpr long kDriverVersion = 1;

std::string hostName()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    char out[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, base, -1, out, sizeof out - 1, nullptr, nullptr);
    return out;
}

void copyName(char* dst, size_t cap, const std::string& s)
{
    std::strncpy(dst, s.c_str(), cap - 1);
    dst[cap - 1] = '\0';
}

/** ASIO's system time: nanoseconds on timeGetTime()'s timeline. */
int64_t asioSystemNs()
{
    return (int64_t) timeGetTime() * 1'000'000LL;
}

void split(uint64_t v, unsigned long& hi, unsigned long& lo)
{
    hi = (unsigned long) (v >> 32);
    lo = (unsigned long) (v & 0xFFFFFFFFu);
}

} // namespace

InletAsio::InletAsio() = default;

InletAsio::~InletAsio()
{
    stop();
    disposeBuffers();
    client.reset();
    inletAsioObjectGone();
}

/* ---- IUnknown ----------------------------------------------------------------- */

HRESULT STDMETHODCALLTYPE InletAsio::QueryInterface(REFIID riid, void** ppv)
{
    if (ppv == nullptr)
        return E_POINTER;
    // ASIO hosts ask for the driver's own CLSID as the interface.
    if (riid == IID_IUnknown || riid == kInletAsioClsid)
    {
        *ppv = static_cast<IASIO*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE InletAsio::AddRef()
{
    return ++refs;
}

ULONG STDMETHODCALLTYPE InletAsio::Release()
{
    const ULONG n = --refs;
    if (n == 0)
        delete this;
    return n;
}

/* ---- IASIO ----------------------------------------------------------------------- */

ASIOBool InletAsio::init(void*)
{
    if (client)
        return ASIOTrue;
    DeviceClient::Options opt;
    opt.kind = shm::ClientKind::Asio;
    opt.app = hostName();
    client = DeviceClient::attach(opt, error);
    if (!client)
    {
        if (error.empty())
            error = "Inlet's service is not running";
        return ASIOFalse;
    }
    serial = client->serial();
    client->heartbeat(monoNowNs(), false, 0);
    return ASIOTrue;
}

void InletAsio::getDriverName(char* name)
{
    copyName(name, 32, "Inlet");
}

long InletAsio::getDriverVersion()
{
    return kDriverVersion;
}

void InletAsio::getErrorMessage(char* string)
{
    copyName(string, 124, error.empty() ? std::string("no error") : error);
}

ASIOError InletAsio::getChannels(long* numInputChannels, long* numOutputChannels)
{
    if (!client)
        return ASE_NotPresent;
    if (numInputChannels)
        *numInputChannels = (long) client->inputs();
    if (numOutputChannels)
        *numOutputChannels = (long) client->outputs();
    return ASE_OK;
}

ASIOError InletAsio::getLatencies(long* inputLatency, long* outputLatency)
{
    if (!client)
        return ASE_NotPresent;
    const long frames = bufferFrames ? bufferFrames : kPreferredBuffer;
    // Input: a period, plus the link offset the network's audio is played at.
    // Output: the double buffer's period, plus the one more Inlet allows a
    // host that processes outside the callback. Both are media-clock exact.
    if (inputLatency)
        *inputLatency = frames + client->linkOffsetFrames();
    if (outputLatency)
        *outputLatency = 2 * frames;
    return ASE_OK;
}

ASIOError InletAsio::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity)
{
    if (minSize)
        *minSize = kMinBuffer;
    if (maxSize)
        *maxSize = kMaxBuffer;
    if (preferredSize)
        *preferredSize = kPreferredBuffer;
    if (granularity)
        *granularity = -1;   // powers of two
    return ASE_OK;
}

ASIOError InletAsio::canSampleRate(ASIOSampleRate sampleRate)
{
    if (!client)
        return ASE_NotPresent;
    // The network's rate. (Other rates through the rate bridge come later.)
    return sampleRate == (ASIOSampleRate) client->sampleRate() ? ASE_OK : ASE_NoClock;
}

ASIOError InletAsio::getSampleRate(ASIOSampleRate* sampleRate)
{
    if (!client)
        return ASE_NotPresent;
    *sampleRate = (ASIOSampleRate) client->sampleRate();
    return ASE_OK;
}

ASIOError InletAsio::setSampleRate(ASIOSampleRate sampleRate)
{
    if (sampleRate == 0.0)
        return ASE_OK;   // "external sync": always so
    return canSampleRate(sampleRate);
}

ASIOError InletAsio::getClockSources(ASIOClockSource* clocks, long* numSources)
{
    if (clocks == nullptr || numSources == nullptr || *numSources < 1)
        return ASE_InvalidParameter;
    clocks[0].index = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    copyName(clocks[0].name, sizeof clocks[0].name, "PTP media clock");
    *numSources = 1;
    return ASE_OK;
}

ASIOError InletAsio::setClockSource(long reference)
{
    return reference == 0 ? ASE_OK : ASE_InvalidParameter;
}

ASIOError InletAsio::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp)
{
    if (!engine || !engine->running())
        return ASE_SPNotAdvancing;
    split(lastPeriodStart.load(), sPos->hi, sPos->lo);
    split((uint64_t) lastSwitchSystemNs.load(), tStamp->hi, tStamp->lo);
    return ASE_OK;
}

ASIOError InletAsio::getChannelInfo(ASIOChannelInfo* info)
{
    if (!client || info == nullptr)
        return ASE_NotPresent;
    const long count = info->isInput ? (long) client->inputs() : (long) client->outputs();
    if (info->channel < 0 || info->channel >= count)
        return ASE_InvalidParameter;
    info->isActive = ASIOFalse;
    for (auto& c : channels)
        if (c.isInput == (info->isInput != ASIOFalse) && c.index == info->channel)
            info->isActive = ASIOTrue;
    info->channelGroup = 0;
    info->type = ASIOSTInt32LSB;
    std::string label = info->isInput ? client->inputLabel(info->channel) : client->outputLabel(info->channel);
    if (label.empty())
        label = std::string(info->isInput ? "In " : "Out ") + std::to_string(info->channel + 1);
    copyName(info->name, sizeof info->name, label);
    return ASE_OK;
}

ASIOError InletAsio::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* cb)
{
    if (!client)
        return ASE_NotPresent;
    if (engine && engine->running())
        return ASE_InvalidMode;
    if (bufferSize < kMinBuffer || bufferSize > kMaxBuffer || cb == nullptr)
        return ASE_InvalidParameter;
    disposeBuffers();
    channels.clear();
    channels.reserve((size_t) numChannels);
    for (long i = 0; i < numChannels; ++i)
    {
        ASIOBufferInfo& b = bufferInfos[i];
        const long count = b.isInput ? (long) client->inputs() : (long) client->outputs();
        if (b.channelNum < 0 || b.channelNum >= count)
        {
            channels.clear();
            return ASE_InvalidParameter;
        }
        Channel c;
        c.isInput = b.isInput != ASIOFalse;
        c.index = b.channelNum;
        c.halves[0].assign((size_t) bufferSize, 0);
        c.halves[1].assign((size_t) bufferSize, 0);
        channels.push_back(std::move(c));
    }
    for (long i = 0; i < numChannels; ++i)
    {
        bufferInfos[i].buffers[0] = channels[(size_t) i].halves[0].data();
        bufferInfos[i].buffers[1] = channels[(size_t) i].halves[1].data();
    }
    bufferFrames = bufferSize;
    callbacks = cb;
    timeInfo = cb->asioMessage != nullptr && cb->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
    engine = std::make_unique<PeriodEngine>(*client, (uint32_t) bufferSize,
                                            [this](uint64_t start, bool late) { onPeriod(start, late); });
    return ASE_OK;
}

ASIOError InletAsio::disposeBuffers()
{
    if (engine)
        engine->stop();
    engine.reset();
    channels.clear();
    callbacks = nullptr;
    bufferFrames = 0;
    return ASE_OK;
}

ASIOError InletAsio::start()
{
    if (!client || !engine)
        return ASE_NotPresent;
    client->claimOutputs();   // the first running host writes the outputs
    half = 1;
    pendingOutput = false;
    hostUsesOutputReady = false;
    resetRequested = false;
    for (auto& c : channels)
        if (!c.isInput)
        {
            std::fill(c.halves[0].begin(), c.halves[0].end(), 0);
            std::fill(c.halves[1].begin(), c.halves[1].end(), 0);
        }
    return engine->start() ? ASE_OK : ASE_HWMalfunction;
}

ASIOError InletAsio::stop()
{
    if (engine)
        engine->stop();
    if (client)
    {
        client->releaseOutputs();
        client->heartbeat(monoNowNs(), false, (uint32_t) bufferFrames);
    }
    return ASE_OK;
}

ASIOError InletAsio::controlPanel()
{
    // The Inlet app, installed beside the driver.
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&kInletAsioClsid), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\'))
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"Inlet.exe");
    const auto r = (INT_PTR) ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
    return r > 32 ? ASE_OK : ASE_NotPresent;
}

ASIOError InletAsio::future(long selector, void*)
{
    switch (selector)
    {
    case kAsioSupportsTimeInfo:
        return ASE_SUCCESS;
    default:
        return ASE_InvalidParameter;
    }
}

ASIOError InletAsio::outputReady()
{
    // The host has filled the half it was handed: on the wire's rings now.
    hostUsesOutputReady = true;
    if (pendingOutput.exchange(false))
    {
        const int h = half.load();
        writeOutputHalf(h, halfFirst[h].load());
    }
    return ASE_OK;
}

/* ---- the period ----------------------------------------------------------------------- */

void InletAsio::writeOutputHalf(int h, uint64_t firstSample)
{
    // One ring per device output; outputs the host did not open stay silent.
    for (auto& c : channels)
        if (!c.isInput)
            client->writeOutputInt32(firstSample, (int) c.index, c.halves[h].data(), (int) bufferFrames);
}

void InletAsio::onPeriod(uint64_t periodStart, bool)
{
    if (callbacks == nullptr)
        return;

    // inletd went away or was restarted with another layout: the host must reset.
    if (!resetRequested && (client->serial() != serial || !client->daemonAlive(monoNowNs())))
    {
        resetRequested = true;
        if (callbacks->asioMessage)
            callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    }

    // The half handed out last time, if the host never said it was done:
    // written now, one period later than outputReady() would have.
    if (pendingOutput.exchange(false))
    {
        const int prev = half.load();
        writeOutputHalf(prev, halfFirst[prev].load());
    }

    const int h = half.load() ^ 1;
    for (auto& c : channels)
        if (c.isInput)
            client->readInputInt32(periodStart, (int) c.index, c.halves[h].data(), (int) bufferFrames);
    // This switch's output plays from the end of the next period.
    halfFirst[h].store(periodStart + 2 * (uint64_t) bufferFrames);
    half.store(h);
    pendingOutput.store(true);
    lastPeriodStart.store(periodStart);
    const int64_t sys = asioSystemNs();
    lastSwitchSystemNs.store(sys);

    if (timeInfo && callbacks->bufferSwitchTimeInfo)
    {
        ASIOTime t {};
        t.timeInfo.speed = 1.0;
        split((uint64_t) sys, t.timeInfo.systemTime.hi, t.timeInfo.systemTime.lo);
        split(periodStart, t.timeInfo.samplePosition.hi, t.timeInfo.samplePosition.lo);
        t.timeInfo.sampleRate = (ASIOSampleRate) client->sampleRate();
        t.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
        callbacks->bufferSwitchTimeInfo(&t, h, ASIOTrue);
    }
    else if (callbacks->bufferSwitch)
        callbacks->bufferSwitch(h, ASIOTrue);
}

} // namespace inlet
