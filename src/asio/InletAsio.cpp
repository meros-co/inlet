// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "InletAsio.h"

#include "inlet/DeviceClient.h"
#include "inlet/RateConverter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <shellapi.h>
#include <timeapi.h>

void inletAsioObjectGone();   // DllMain.cpp: the module's object count

namespace inlet
{

namespace
{

constexpr long kDriverVersion = 2;
constexpr int64_t kReattachEveryNs = 500'000'000;
/** Channels (both ways) x rate up to which the converters use medium quality. */
constexpr uint64_t kMediumQualityWork = 128ull * 48000;
constexpr float kFrom24 = 1.0f / 8388608.0f;
constexpr float kFrom32 = 1.0f / 2147483648.0f;

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

/** A float in [-1, 1) as ASIO's Int32LSB, and as the rings' 24-bit word. */
int32_t toInt32(float v)
{
    return (int32_t) std::lrint(std::clamp((double) v, -1.0, 2147483647.0 / 2147483648.0) * 2147483648.0);
}
int32_t toInt24(float v)
{
    return (int32_t) std::lrint(std::clamp((double) v, -1.0, 8388607.0 / 8388608.0) * 8388608.0);
}

} // namespace

InletAsio::InletAsio() = default;

InletAsio::~InletAsio()
{
    stop();
    disposeBuffers();
    current.store(nullptr);
    clients.clear();
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
    if (client() != nullptr)
        return ASIOTrue;
    DeviceClient::Options opt;
    opt.kind = shm::ClientKind::Asio;
    app = hostName();
    opt.app = app;
    auto c = DeviceClient::attach(opt, error);
    if (!c)
    {
        if (error.empty())
            error = "Inlet's service is not running";
        return ASIOFalse;
    }
    c->heartbeat(monoNowNs(), false, 0);
    clients.push_back(std::move(c));
    current.store(clients.back().get());
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

uint32_t InletAsio::deviceRate() const
{
    const DeviceClient* c = client();
    return c != nullptr ? c->sampleRate() : 48000;
}

uint32_t InletAsio::rate() const
{
    return hostRate != 0 ? hostRate : deviceRate();
}

ASIOError InletAsio::getChannels(long* numInputChannels, long* numOutputChannels)
{
    DeviceClient* c = client();
    if (c == nullptr)
        return ASE_NotPresent;
    if (numInputChannels)
        *numInputChannels = (long) c->inputs();
    if (numOutputChannels)
        *numOutputChannels = (long) c->outputs();
    return ASE_OK;
}

ASIOError InletAsio::getLatencies(long* inputLatency, long* outputLatency)
{
    DeviceClient* c = client();
    if (c == nullptr)
        return ASE_NotPresent;
    const long frames = bufferFrames ? bufferFrames : kPreferredBuffer;
    // Input: a period, plus the link offset the network's audio is played at
    // (and, converting, the converter's look-ahead), in the host's frames.
    // Output: the double buffer's period, plus the one more Inlet allows a
    // host that processes outside the callback (and the converter's
    // look-ahead). All media-clock exact.
    long inExtra = c->linkOffsetFrames(), outExtra = 0;
    if (converting())
    {
        const uint32_t inLead = inputs.conv ? inputs.conv->leadFrames() : 0;
        inExtra = (long) std::ceil((double) (c->linkOffsetFrames() + inLead) * rate() / deviceRate());
        outExtra = outputs.conv ? (long) outputs.conv->leadFrames() : 0;
    }
    if (inputLatency)
        *inputLatency = frames + inExtra;
    if (outputLatency)
        *outputLatency = 2 * frames + outExtra;
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
    if (client() == nullptr)
        return ASE_NotPresent;
    // The network's rate directly; the others through a converter.
    for (uint32_t r : kHostRates)
        if (sampleRate == (ASIOSampleRate) r)
            return ASE_OK;
    return sampleRate == (ASIOSampleRate) deviceRate() ? ASE_OK : ASE_NoClock;
}

ASIOError InletAsio::getSampleRate(ASIOSampleRate* sampleRate)
{
    if (client() == nullptr)
        return ASE_NotPresent;
    *sampleRate = (ASIOSampleRate) rate();
    return ASE_OK;
}

ASIOError InletAsio::setSampleRate(ASIOSampleRate sampleRate)
{
    if (sampleRate == 0.0)
        return ASE_OK;   // "external sync": always so
    if (const ASIOError e = canSampleRate(sampleRate); e != ASE_OK)
        return e;
    const uint32_t r = (uint32_t) sampleRate;
    if (r == rate())
        return ASE_OK;
    hostRate = r == deviceRate() ? 0 : r;
    if (engine && engine->running())
    {
        // Running at the old rate: the host sets everything up again.
        message(kAsioResetRequest);
        return ASE_OK;
    }
    if (engine)
    {
        // Buffers exist: the engine and converters follow the new rate.
        engine = std::make_unique<PeriodEngine>(*client(), (uint32_t) bufferFrames,
                                                [this](const PeriodEngine::Period& p) { onPeriod(p); }, rate());
        buildConversions();
    }
    return ASE_OK;
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
    DeviceClient* c = client();
    if (c == nullptr || info == nullptr)
        return ASE_NotPresent;
    const long count = info->isInput ? (long) c->inputs() : (long) c->outputs();
    if (info->channel < 0 || info->channel >= count)
        return ASE_InvalidParameter;
    info->isActive = ASIOFalse;
    for (auto& ch : channels)
        if (ch.isInput == (info->isInput != ASIOFalse) && ch.index == info->channel)
            info->isActive = ASIOTrue;
    info->channelGroup = 0;
    info->type = ASIOSTInt32LSB;
    std::string label = info->isInput ? c->inputLabel(info->channel) : c->outputLabel(info->channel);
    if (label.empty())
        label = std::string(info->isInput ? "In " : "Out ") + std::to_string(info->channel + 1);
    copyName(info->name, sizeof info->name, label);
    return ASE_OK;
}

ASIOError InletAsio::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* cb)
{
    DeviceClient* c = client();
    if (c == nullptr)
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
        const long count = b.isInput ? (long) c->inputs() : (long) c->outputs();
        if (b.channelNum < 0 || b.channelNum >= count)
        {
            channels.clear();
            return ASE_InvalidParameter;
        }
        Channel ch;
        ch.isInput = b.isInput != ASIOFalse;
        ch.index = b.channelNum;
        ch.halves[0].assign((size_t) bufferSize, 0);
        ch.halves[1].assign((size_t) bufferSize, 0);
        channels.push_back(std::move(ch));
    }
    for (long i = 0; i < numChannels; ++i)
    {
        bufferInfos[i].buffers[0] = channels[(size_t) i].halves[0].data();
        bufferInfos[i].buffers[1] = channels[(size_t) i].halves[1].data();
    }
    bufferFrames = bufferSize;
    callbacks = cb;
    timeInfo = cb->asioMessage != nullptr && cb->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
    engine = std::make_unique<PeriodEngine>(*c, (uint32_t) bufferSize,
                                            [this](const PeriodEngine::Period& p) { onPeriod(p); }, rate());
    buildConversions();
    return ASE_OK;
}

void InletAsio::buildConversions()
{
    for (auto* conv : { &inputs, &outputs })
    {
        conv->conv.reset();
        conv->channels.clear();
        conv->anchored = false;
    }
    if (!converting())
        return;
    for (int i = 0; i < (int) channels.size(); ++i)
        (channels[(size_t) i].isInput ? inputs : outputs).channels.push_back(i);
    const long frames = bufferFrames;
    // libsamplerate's medium sinc where it costs little; its fastest (still a
    // band-limited sinc) where the channels and the rate would make the
    // medium one take a good part of a core: about 64 channels each way at
    // 48 kHz, or 16 at 192.
    const uint64_t work = (uint64_t) channels.size() * std::max(rate(), deviceRate());
    const int quality = work <= kMediumQualityWork ? 1 : 2;
    if (!inputs.channels.empty())
    {
        const int n = (int) inputs.channels.size();
        inputs.conv = std::make_unique<RateConverter>(deviceRate(), rate(), n, quality);
        // Device frames per offer: a period's worth at the ratio, and slack.
        const long cap = (long) std::ceil(frames / inputs.conv->ratio()) + (long) inputs.conv->leadFrames() + 64;
        inputs.in.assign((size_t) cap * n, 0.0f);
        inputs.out.assign((size_t) frames * n, 0.0f);
        inputs.scratch.assign((size_t) cap, 0);
    }
    if (!outputs.channels.empty())
    {
        const int n = (int) outputs.channels.size();
        outputs.conv = std::make_unique<RateConverter>(rate(), deviceRate(), n, quality);
        const long cap = (long) std::ceil(frames * outputs.conv->ratio()) + 64;
        outputs.in.assign((size_t) frames * n, 0.0f);
        outputs.out.assign((size_t) cap * n, 0.0f);
        outputs.scratch.assign((size_t) cap, 0);
    }
}

ASIOError InletAsio::disposeBuffers()
{
    if (engine)
        engine->stop();
    engine.reset();
    channels.clear();
    for (auto* conv : { &inputs, &outputs })
    {
        conv->conv.reset();
        conv->channels.clear();
    }
    callbacks = nullptr;
    bufferFrames = 0;
    // Regions left behind by restarts of inletd go now; the current one stays.
    if (clients.size() > 1)
        clients.erase(clients.begin(), clients.end() - 1);
    return ASE_OK;
}

ASIOError InletAsio::start()
{
    DeviceClient* c = client();
    if (c == nullptr || !engine)
        return ASE_NotPresent;
    c->claimOutputs();   // a layer of its own, mixed with other hosts' by the sender
    half = 1;
    pendingOutput = false;
    hostUsesOutputReady = false;
    resetRequested = false;
    inputs.anchored = outputs.anchored = false;
    for (auto& ch : channels)
        if (!ch.isInput)
        {
            std::fill(ch.halves[0].begin(), ch.halves[0].end(), 0);
            std::fill(ch.halves[1].begin(), ch.halves[1].end(), 0);
        }
    return engine->start() ? ASE_OK : ASE_HWMalfunction;
}

ASIOError InletAsio::stop()
{
    if (engine)
        engine->stop();
    if (DeviceClient* c = client())
    {
        c->releaseOutputs();
        c->heartbeat(monoNowNs(), false, (uint32_t) bufferFrames, converting() ? rate() : 0);
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

void InletAsio::message(long selector)
{
    if (callbacks == nullptr || callbacks->asioMessage == nullptr)
        return;
    if (callbacks->asioMessage(kAsioSelectorSupported, selector, nullptr, nullptr) == 1)
        callbacks->asioMessage(selector, 0, nullptr, nullptr);
}

void InletAsio::readInputHalf(int h, uint64_t hostStart)
{
    DeviceClient* c = client();
    const int frames = (int) bufferFrames;
    if (!converting())
    {
        for (auto& ch : channels)
            if (ch.isInput)
                c->readInputInt32(hostStart, (int) ch.index, ch.halves[h].data(), frames);
        return;
    }
    Conversion& cv = inputs;
    if (!cv.conv)
        return;
    const int n = (int) cv.channels.size();
    if (!cv.anchored || hostStart != cv.nextHost)
    {
        // A fresh stream: the device position of this period's first frame,
        // less the link offset, and less what the converter reads ahead.
        cv.conv->reset();
        cv.devicePos = convertPosition(hostStart, rate(), deviceRate()) - (uint64_t) c->linkOffsetFrames()
                     - cv.conv->leadFrames();
        cv.anchored = true;
    }
    cv.nextHost = hostStart + (uint64_t) frames;
    const long cap = (long) cv.scratch.size();
    long produced = 0;
    for (int guard = 0; produced < frames && guard < 64; ++guard)
    {
        const long offer = std::min(cv.conv->inputFor(frames - produced), cap);
        for (int k = 0; k < n; ++k)
        {
            c->readInputRaw(cv.devicePos, (int) channels[(size_t) cv.channels[(size_t) k]].index, cv.scratch.data(), (int) offer);
            for (long f = 0; f < offer; ++f)
                cv.in[(size_t) (f * n + k)] = (float) cv.scratch[(size_t) f] * kFrom24;
        }
        long used = 0;
        produced += cv.conv->process(cv.in.data(), offer, cv.out.data() + produced * n, frames - produced, used);
        cv.devicePos += (uint64_t) used;
    }
    for (int k = 0; k < n; ++k)
    {
        int32_t* dst = channels[(size_t) cv.channels[(size_t) k]].halves[h].data();
        for (long f = 0; f < produced; ++f)
            dst[f] = toInt32(cv.out[(size_t) (f * n + k)]);
        std::fill(dst + produced, dst + frames, 0);
    }
}

void InletAsio::writeOutputHalf(int h, uint64_t firstSample)
{
    DeviceClient* c = client();
    const int frames = (int) bufferFrames;
    if (!converting())
    {
        // One ring per device output; outputs the host did not open stay silent.
        for (auto& ch : channels)
            if (!ch.isInput)
                c->writeOutputInt32(firstSample, (int) ch.index, ch.halves[h].data(), frames);
        return;
    }
    Conversion& cv = outputs;
    if (!cv.conv)
        return;
    const int n = (int) cv.channels.size();
    if (!cv.anchored || firstSample != cv.nextHost)
    {
        // A fresh stream, played from this half's first frame plus what the
        // converter holds back for its look-ahead.
        cv.conv->reset();
        cv.devicePos = convertPosition(firstSample, rate(), deviceRate())
                     + (uint64_t) std::ceil(cv.conv->leadFrames() * cv.conv->ratio());
        cv.anchored = true;
    }
    cv.nextHost = firstSample + (uint64_t) frames;
    for (int k = 0; k < n; ++k)
    {
        const int32_t* src = channels[(size_t) cv.channels[(size_t) k]].halves[h].data();
        for (int f = 0; f < frames; ++f)
            cv.in[(size_t) (f * n + k)] = (float) src[f] * kFrom32;
    }
    const long cap = (long) cv.scratch.size();
    long consumed = 0;
    for (int guard = 0; consumed < frames && guard < 64; ++guard)
    {
        long used = 0;
        const long made = cv.conv->process(cv.in.data() + consumed * n, frames - consumed, cv.out.data(), cap, used);
        consumed += used;
        for (int k = 0; k < n; ++k)
        {
            for (long f = 0; f < made; ++f)
                cv.scratch[(size_t) f] = toInt24(cv.out[(size_t) (f * n + k)]);
            c->writeOutputRaw(cv.devicePos, (int) channels[(size_t) cv.channels[(size_t) k]].index, cv.scratch.data(), (int) made);
        }
        cv.devicePos += (uint64_t) made;
        if (used == 0 && made == 0)
            break;
    }
}

void InletAsio::reattach(int64_t nowMono)
{
    // inletd stopped, or restarted with new settings (a region of its own).
    // Look for the one running now, every half second, without bothering the
    // host: until it is found, inputs are silent and outputs go nowhere.
    if (nowMono - lastAttachTry < kReattachEveryNs)
        return;
    lastAttachTry = nowMono;
    DeviceClient* old = client();
    DeviceClient::Options opt;
    opt.kind = shm::ClientKind::Asio;
    opt.app = app;
    std::string err;
    auto next = DeviceClient::attach(opt, err);
    if (!next || next->serial() == old->serial() || !next->daemonAlive(nowMono))
        return;
    if (next->inputs() != old->inputs() || next->outputs() != old->outputs() || next->sampleRate() != old->sampleRate())
    {
        // Another shape of device: only the host can set itself up again.
        if (!resetRequested.exchange(true))
            message(kAsioResetRequest);
        return;
    }
    const bool owned = old->ownsOutputs();
    old->releaseOutputs();
    if (owned)
        next->claimOutputs();
    clients.push_back(std::move(next));
    current.store(clients.back().get());
    engine->switchClient(*clients.back());
}

void InletAsio::onPeriod(const PeriodEngine::Period& p)
{
    if (callbacks == nullptr)
        return;
    DeviceClient* c = client();
    const int64_t now = monoNowNs();
    if (!c->daemonAlive(now))
        reattach(now);

    // The schedule re-anchored (PTP stepped, or a new inletd): positions jump,
    // and the host is told so.
    if (p.resynced)
    {
        inputs.anchored = outputs.anchored = false;
        message(kAsioResyncRequest);
    }

    // The half handed out last time, if the host never said it was done:
    // written now, one period later than outputReady() would have.
    if (pendingOutput.exchange(false))
    {
        const int prev = half.load();
        writeOutputHalf(prev, halfFirst[prev].load());
    }

    const int h = half.load() ^ 1;
    readInputHalf(h, p.start);
    // This switch's output plays from the end of the next period.
    halfFirst[h].store(p.start + 2 * (uint64_t) bufferFrames);
    half.store(h);
    pendingOutput.store(true);
    lastPeriodStart.store(p.start);
    const int64_t sys = asioSystemNs();
    lastSwitchSystemNs.store(sys);

    if (timeInfo && callbacks->bufferSwitchTimeInfo)
    {
        ASIOTime t {};
        t.timeInfo.speed = 1.0;
        split((uint64_t) sys, t.timeInfo.systemTime.hi, t.timeInfo.systemTime.lo);
        split(p.start, t.timeInfo.samplePosition.hi, t.timeInfo.samplePosition.lo);
        t.timeInfo.sampleRate = (ASIOSampleRate) rate();
        t.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
        callbacks->bufferSwitchTimeInfo(&t, h, ASIOTrue);
    }
    else if (callbacks->bufferSwitch)
        callbacks->bufferSwitch(h, ASIOTrue);
}

} // namespace inlet
