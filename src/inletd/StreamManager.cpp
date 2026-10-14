// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "StreamManager.h"

#include "Ids.h"
#include "Sap.h"

#include <aes67/rtp.h>
#include <aes67/rx.h>
#include <aes67/sdp.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace inlet
{

namespace
{

constexpr int64_t kSapExpireMs = 300'000;   // ten announcement intervals
constexpr uint8_t kPayloadType = 98;

int64_t wallMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/** The longest AES67 packet time a sender of this many channels fits in. */
uint32_t ptimeFor(int channels, uint32_t rate)
{
    for (uint32_t us : { 1000u, 333u, 250u, 125u })
    {
        const int frames = (int) (((uint64_t) rate * us + 500000u) / 1000000u);
        if (frames > 0 && aes67_max_channels(3, frames) >= channels)
            return us;
    }
    return 125;
}

std::string lastOctet(const std::string& ip)
{
    const auto dot = ip.rfind('.');
    return dot == std::string::npos ? "1" : ip.substr(dot + 1);
}

void copyLabel(char* dst, const std::string& s)
{
    std::memset(dst, 0, shm::kLabelBytes);
    std::strncpy(dst, s.c_str(), shm::kLabelBytes - 1);
}

bool allZero(const uint8_t* b, size_t n)
{
    return std::all_of(b, b + n, [](uint8_t v) { return v == 0; });
}

/** The MAC an SDP's a=ts-refclk:localmac= names, normalised to lower case
    with dashes; empty when it names none. */
std::string localmacOf(const std::string& sdp)
{
    const std::string key = "a=ts-refclk:localmac=";
    const auto at = sdp.find(key);
    if (at == std::string::npos)
        return {};
    std::string mac;
    for (size_t i = at + key.size(); i < sdp.size() && sdp[i] != '\r' && sdp[i] != '\n'; ++i)
        mac.push_back(sdp[i] == ':' ? '-' : (char) std::tolower((unsigned char) sdp[i]));
    return mac;
}

} // namespace

std::string describeSdp(const aes67_sdp_t& s)
{
    return std::to_string(s.channels) + " ch, " + std::to_string(s.sample_rate / 1000.0).substr(0, 4) + " kHz, " + s.encoding;
}

StreamManager::StreamManager(shm::Region& r, aes67_netrx_t* rxIn, NetTx& txIn, Sap* sapIn, const Settings& settings)
    : region(r), netRx(rxIn), netTx(txIn), sap(sapIn), cfg(settings)
{
    auto& h = region.header();
    h.linkOffsetFrames.store((int32_t) cfg.linkOffsetFrames);
    if (cfg.groupPrefix.empty())
        cfg.groupPrefix = "239.69." + lastOctet(cfg.ifaceIp);

    for (uint32_t k = 1; k <= h.receivers; ++k)
    {
        Receiver rcv;
        rcv.index = (int) k;
        rcv.label = cfg.labelPrefix + " In " + std::to_string(k);
        rx.push_back(std::move(rcv));
    }
    // Senders carry contiguous ranges of the outputs: as many as fit.
    const uint32_t per = std::max<uint32_t>(1, cfg.senderChannels);
    const uint32_t count = std::min<uint32_t>(cfg.senders, h.outputs / per);
    for (uint32_t k = 1; k <= count; ++k)
    {
        Sender s;
        s.index = (int) k;
        s.label = cfg.labelPrefix + " Out " + std::to_string(k);
        s.channels = (int) per;
        s.firstOutput = (int) ((k - 1) * per);
        s.group = cfg.groupPrefix + "." + std::to_string(k);
        s.port = cfg.port;
        s.ptimeUs = ptimeFor((int) per, cfg.sampleRate);
        tx.push_back(s);
    }
    // The default map: receiver k's channels onto consecutive inputs.
    for (uint32_t i = 0; i < h.inputs; ++i)
    {
        const uint32_t k = i / h.receiverChannels + 1, c = i % h.receiverChannels;
        region.inputMap()[i].store(k <= h.receivers ? shm::mapCell((int) k, (int) c) : 0);
    }
    inputLabels.assign(h.inputs, {});
    outputLabels.assign(h.outputs, {});
}

StreamManager::~StreamManager()
{
    stop();
}

void StreamManager::start()
{
    {
        std::lock_guard<std::mutex> g(lock);
        for (auto& s : tx)
            if (s.enabled && s.txSlot < 0)   // restoreState may have opened it already
            {
                std::string error;
                openSender(s, error);
            }
    }
    running = true;
    thread = std::thread([this] { run(); });
}

void StreamManager::stop()
{
    if (running.exchange(false) && thread.joinable())
        thread.join();
    std::lock_guard<std::mutex> g(lock);
    for (auto& r : rx)
        closeReceiver(r);
    for (auto& s : tx)
        closeSender(s);
}

void StreamManager::changed()
{
    if (onChanged)
        onChanged();
}

/* ---- receivers -------------------------------------------------------------------- */

bool StreamManager::sharesOurClock(const Receiver& r) const
{
    ClockSample clk;
    region.clock().read(clk);
    // Following a grandmaster: the stream must name it (or be traceable in
    // our domain). Free-running: only a stream on this computer's own clock
    // -- one of Inlet's own senders -- shares it.
    if (clk.state != ClockState::FreeRun && !allZero(clk.gmIdentity, 8))
        return aes67_sdp_shares_grandmaster(&r.sdp, clk.gmIdentity, clk.domain);
    return r.sdp.refclk_localmac && !cfg.ifaceMac.empty() && localmacOf(r.sdpText) == cfg.ifaceMac;
}

bool StreamManager::validateSdp(const std::string& text, aes67_sdp_t& s, std::string& error) const
{
    char err[160] = {};
    if (!aes67_sdp_parse(text.c_str(), text.size(), &s, err, sizeof err))
    {
        error = err;
        return false;
    }
    aes67_sample_format_t fmt;
    if (!aes67_format_from_encoding(s.encoding, &fmt))
    {
        error = std::string("the stream is ") + s.encoding + "; Inlet plays L24, L16 and AM824";
        return false;
    }
    const uint32_t maxCh = region.header().receiverChannels;
    if (s.channels == 0 || s.channels > maxCh)
    {
        error = "the stream has " + std::to_string(s.channels) + " channels; each receiver takes up to " + std::to_string(maxCh);
        return false;
    }
    const uint32_t rate = s.sample_rate ? s.sample_rate : cfg.sampleRate;
    if (std::find(std::begin(kReceiveRates), std::end(kReceiveRates), (int) rate) == std::end(kReceiveRates))
    {
        error = "the stream is " + std::to_string(rate) + " Hz; Inlet plays 44.1, 48, 88.2 and 96 kHz streams";
        return false;
    }
    uint32_t group = 0;
    if (!aes67_parse_ipv4(s.connection_ip, &group))
    {
        error = std::string("the stream's address is not IPv4: ") + s.connection_ip;
        return false;
    }
    return true;
}

bool StreamManager::openReceiver(Receiver& r, std::string& error)
{
    aes67_sdp_t& s = r.sdp;
    if (!validateSdp(r.sdpText, s, error))
        return false;
    aes67_sample_format_t fmt = AES67_FORMAT_L24;
    aes67_format_from_encoding(s.encoding, &fmt);
    const uint32_t maxCh = region.header().receiverChannels;
    const uint32_t rate = s.sample_rate ? s.sample_rate : cfg.sampleRate;
    uint32_t group = 0, source = 0;
    aes67_parse_ipv4(s.connection_ip, &group);
    if (s.source_ip[0] != '\0')
        aes67_parse_ipv4(s.source_ip, &source);

    // On our grandmaster at our rate: straight into the device's rings. Anything
    // else is bridged onto the device's clock.
    r.bridged = !sharesOurClock(r) || rate != cfg.sampleRate;

    aes67_ring* deviceRings = region.rxRings(r.index);
    for (uint32_t c = 0; c < maxCh; ++c)
        aes67_ring_init(deviceRings + c);

    aes67_netrx_stream_cfg_t rc {};
    rc.group = group;
    rc.source = source;
    rc.port = s.port;
    rc.payload_type = s.payload_type;
    rc.format = (uint8_t) fmt;
    rc.channels = (uint16_t) s.channels;
    if (r.bridged)
    {
        r.staging.reset(new aes67_ring_t[s.channels]);
        for (int c = 0; c < s.channels; ++c)
            aes67_ring_init(&r.staging[c]);
        RateBridgeConfig bc;
        bc.inRate = rate;
        bc.outRate = cfg.sampleRate;
        bc.channels = s.channels;
        bc.in = r.staging.get();
        bc.out = deviceRings;
        bc.targetFillFrames = rate * 3 / 1000;
        bc.leadFrames = cfg.sampleRate * 2 / 1000;
        r.bridge = std::make_unique<RateBridge>(bc);
        rc.rings = r.staging.get();
        rc.media_clock_offset = 0;   // the bridge does not care where the sender's clock starts
    }
    else
    {
        rc.rings = deviceRings;
        rc.media_clock_offset = s.mediaclk_direct ? (uint32_t) s.mediaclk_offset : 0;
    }
    char err[160] = { 0 };
    r.rxSlot = aes67_netrx_add_stream(netRx, &rc, err, sizeof err);
    if (r.rxSlot < 0)
    {
        error = err;
        r.bridge.reset();
        r.staging.reset();
        return false;
    }
    r.active = true;
    r.error.clear();
    return true;
}

void StreamManager::closeReceiver(Receiver& r)
{
    if (r.rxSlot >= 0)
        aes67_netrx_remove_stream(netRx, r.rxSlot);
    r.rxSlot = -1;
    r.bridge.reset();
    r.staging.reset();
    r.active = false;
}

bool StreamManager::subscribeReceiver(int index, const std::string& sdpText, const std::string& senderId, std::string& error)
{
    {
        std::lock_guard<std::mutex> g(lock);
        if (index < 1 || index > (int) rx.size())
        {
            error = "no such receiver";
            return false;
        }
        // A stream that cannot be played is refused before anything changes:
        // what the receiver plays now keeps playing (IS-05's failed activation).
        aes67_sdp_t probe;
        if (!validateSdp(sdpText, probe, error))
            return false;
        Receiver& r = rx[(size_t) index - 1];
        closeReceiver(r);
        r.sdpText = sdpText;
        r.senderId = senderId;
        r.streamId.clear();
        if (!openReceiver(r, error))
        {
            r.error = error;
            r.sdpText.clear();
            r.senderId.clear();
            return false;
        }
    }
    changed();
    return true;
}

void StreamManager::unsubscribeReceiver(int index)
{
    {
        std::lock_guard<std::mutex> g(lock);
        if (index < 1 || index > (int) rx.size())
            return;
        Receiver& r = rx[(size_t) index - 1];
        closeReceiver(r);
        r.sdpText.clear();
        r.senderId.clear();
        r.streamId.clear();
        aes67_ring* rings = region.rxRings(index);
        for (uint32_t c = 0; c < region.header().receiverChannels; ++c)
            aes67_ring_init(rings + c);
    }
    changed();
}

std::vector<ReceiverInfo> StreamManager::receivers() const
{
    std::lock_guard<std::mutex> g(lock);
    std::vector<ReceiverInfo> out;
    for (auto& r : rx)
    {
        ReceiverInfo i;
        i.index = r.index;
        i.label = r.label;
        i.maxChannels = (int) region.header().receiverChannels;
        i.active = r.active;
        i.senderId = r.senderId;
        i.sdpText = r.sdpText;
        i.sdp = r.sdp;
        i.channels = r.active ? r.sdp.channels : 0;
        out.push_back(i);
    }
    return out;
}

/* ---- senders ------------------------------------------------------------------------ */

std::string StreamManager::makeSdp(const Sender& s) const
{
    ClockSample clk;
    region.clock().read(clk);
    aes67_sdp_params_t p {};
    const std::string name = s.label;
    p.session_name = name.c_str();
    p.origin_ip = cfg.ifaceIp.c_str();
    p.group = s.group.c_str();
    p.port = s.port;
    p.payload_type = kPayloadType;
    p.channels = (uint8_t) s.channels;
    p.sample_rate = cfg.sampleRate;
    p.ptime_us = s.ptimeUs;
    p.ssrc = s.txSlot >= 0 ? netTx.ssrc(s.txSlot) : 0;
    p.ttl = 15;
    if (clk.state != ClockState::FreeRun)
    {
        std::memcpy(p.gm_identity, clk.gmIdentity, 8);
        p.ptp_domain = (uint16_t) std::max(0, clk.domain);
    }
    char buf[2048];
    const int n = aes67_sdp_write(buf, sizeof buf, &p);
    if (n <= 0 || n >= (int) sizeof buf)
        return {};
    std::string sdp(buf, (size_t) n);
    if (clk.state == ClockState::FreeRun && !cfg.ifaceMac.empty())
    {
        // Not on PTP: the stream is on this computer's own clock, and says so.
        std::string mac = cfg.ifaceMac;
        std::transform(mac.begin(), mac.end(), mac.begin(), [](char ch) { return (char) std::toupper((unsigned char) ch); });
        const auto at = sdp.find("a=ts-refclk:");
        if (at != std::string::npos)
        {
            const auto end = sdp.find("\r\n", at);
            sdp.replace(at, end - at, "a=ts-refclk:localmac=" + mac);
        }
    }
    return sdp;
}

bool StreamManager::openSender(Sender& s, std::string& error)
{
    uint32_t group = 0;
    if (!aes67_parse_ipv4(s.group.c_str(), &group))
    {
        error = "bad sender group " + s.group;
        return false;
    }
    TxStreamConfig tc;
    tc.group = group;
    tc.port = s.port;
    tc.payloadType = kPayloadType;
    tc.channels = (uint16_t) s.channels;
    tc.sampleRate = cfg.sampleRate;
    tc.framesPerPacket = (uint32_t) (((uint64_t) cfg.sampleRate * s.ptimeUs + 500000u) / 1000000u);
    // Every host's layer of these outputs, summed by the sender.
    tc.rings = region.txRing(0, s.firstOutput);
    tc.layers = shm::kOutputLayers;
    tc.layerStride = region.header().outputs;
    tc.layerInUse = region.header().outputLayers;
    s.txSlot = netTx.addStream(tc, error);
    if (s.txSlot < 0)
        return false;
    if (sap)
        sap->announce("tx" + std::to_string(s.index), makeSdp(s));
    return true;
}

void StreamManager::closeSender(Sender& s)
{
    if (s.txSlot >= 0)
    {
        netTx.removeStream(s.txSlot);
        if (sap)
            sap->withdraw("tx" + std::to_string(s.index));
    }
    s.txSlot = -1;
}

std::vector<SenderInfo> StreamManager::senders() const
{
    std::lock_guard<std::mutex> g(lock);
    std::vector<SenderInfo> out;
    for (auto& s : tx)
    {
        SenderInfo i;
        i.index = s.index;
        i.label = s.label;
        i.channels = s.channels;
        i.group = s.group;
        i.port = s.port;
        i.enabled = s.enabled;
        i.sending = s.txSlot >= 0;
        for (int c = 0; c < s.channels; ++c)
            i.channelLabels.push_back(outputLabels[(size_t) (s.firstOutput + c)]);
        out.push_back(i);
    }
    return out;
}

std::string StreamManager::senderSdp(int index) const
{
    std::lock_guard<std::mutex> g(lock);
    if (index < 1 || index > (int) tx.size() || tx[(size_t) index - 1].txSlot < 0)
        return {};
    return makeSdp(tx[(size_t) index - 1]);
}

bool StreamManager::setSenderEnabled(int index, bool enabled, std::string& error)
{
    {
        std::lock_guard<std::mutex> g(lock);
        if (index < 1 || index > (int) tx.size())
        {
            error = "no such sender";
            return false;
        }
        Sender& s = tx[(size_t) index - 1];
        s.enabled = enabled;
        if (enabled && s.txSlot < 0 && !openSender(s, error))
            return false;
        if (!enabled)
            closeSender(s);
    }
    changed();
    return true;
}

/* ---- the input map and labels -------------------------------------------------------------- */

std::vector<std::string> StreamManager::deviceInputLabels() const
{
    std::lock_guard<std::mutex> g(lock);
    std::vector<std::string> out;
    for (size_t i = 0; i < inputLabels.size(); ++i)
        out.push_back(inputLabels[i].empty() ? "Input " + std::to_string(i + 1) : inputLabels[i]);
    return out;
}

std::vector<InputMapCell> StreamManager::inputMap() const
{
    std::vector<InputMapCell> out;
    for (uint32_t i = 0; i < region.header().inputs; ++i)
    {
        const uint32_t cell = region.inputMap()[i].load();
        out.push_back({ (int) (cell >> 16), (int) (cell & 0xFFFF) });
    }
    return out;
}

bool StreamManager::setInputMap(const std::vector<std::pair<int, InputMapCell>>& changes, std::string& error)
{
    const auto& h = region.header();
    for (auto& [i, c] : changes)
        if (i < 0 || (uint32_t) i >= h.inputs || c.receiver < 0 || (uint32_t) c.receiver > h.receivers
            || (c.receiver > 0 && (c.channel < 0 || (uint32_t) c.channel >= h.receiverChannels)))
        {
            error = "input " + std::to_string(i) + " cannot take that";
            return false;
        }
    for (auto& [i, c] : changes)
        region.inputMap()[i].store(shm::mapCell(c.receiver, c.channel));
    changed();
    return true;
}

void StreamManager::setDeviceLabels(const std::vector<std::string>& inputs, const std::vector<std::string>& outputs)
{
    std::lock_guard<std::mutex> g(lock);
    auto& gen = region.header().labelsGeneration;
    gen.fetch_add(1);   // odd: being written
    for (size_t i = 0; i < inputLabels.size(); ++i)
    {
        inputLabels[i] = i < inputs.size() ? inputs[i] : std::string();
        copyLabel(region.inputLabel((int) i), inputLabels[i]);
    }
    for (size_t i = 0; i < outputLabels.size(); ++i)
    {
        outputLabels[i] = i < outputs.size() ? outputs[i] : std::string();
        copyLabel(region.outputLabel((int) i), outputLabels[i]);
    }
    gen.fetch_add(1);
}

bool StreamManager::applyLabels(const nlohmann::json& body, const std::function<int(const std::string&)>& receiverIndexOf,
                                const std::function<int(const std::string&)>& senderIndexOf, std::string& error)
{
    if (!body.is_object())
    {
        error = "the body must be an object of receivers and senders";
        return false;
    }
    // Each PUT replaces the whole set: what it does not name is unnamed.
    std::vector<std::string> ins, outs;
    {
        std::lock_guard<std::mutex> g(lock);
        ins.assign(inputLabels.size(), {});
        outs.assign(outputLabels.size(), {});
    }
    const auto map = inputMap();
    // A receiver's channel names land on every device input that plays it.
    for (auto& r : body.value("receivers", nlohmann::json::array()))
    {
        const int k = receiverIndexOf(r.value("id", std::string()));
        if (k <= 0)
            continue;
        const auto names = r.value("channels", nlohmann::json::array());
        for (size_t c = 0; c < names.size(); ++c)
            for (size_t i = 0; i < map.size(); ++i)
                if (map[i].receiver == k && map[i].channel == (int) c)
                    ins[i] = names[c].is_string() ? names[c].get<std::string>() : std::string();
    }
    // A sender's channel names are the device outputs that feed it.
    std::vector<Sender> senders;
    {
        std::lock_guard<std::mutex> g(lock);
        senders = tx;
    }
    for (auto& s : body.value("senders", nlohmann::json::array()))
    {
        const int k = senderIndexOf(s.value("id", std::string()));
        if (k <= 0 || k > (int) senders.size())
            continue;
        const auto& snd = senders[(size_t) k - 1];
        const auto names = s.value("channels", nlohmann::json::array());
        for (size_t c = 0; c < names.size() && (int) c < snd.channels; ++c)
            outs[(size_t) (snd.firstOutput + (int) c)] = names[c].is_string() ? names[c].get<std::string>() : std::string();
    }
    setDeviceLabels(ins, outs);
    changed();
    return true;
}

/* ---- streams on the wire ---------------------------------------------------------------------- */

void StreamManager::sapHeard(const std::string& key, const std::string& sdp)
{
    {
        std::lock_guard<std::mutex> g(lock);
        const std::string id = "sap:" + key;
        if (sdp.empty())
            streams.erase(id);
        else
        {
            aes67_sdp_t parsed;
            char err[128];
            if (!aes67_sdp_parse(sdp.c_str(), sdp.size(), &parsed, err, sizeof err))
                return;   // video, or nothing Inlet plays
            Stream& s = streams[id];
            s.id = id;
            s.name = parsed.session_name;
            s.sdp = sdp;
            s.via = "sap";
            s.lastHeardMs = wallMs();
        }
    }
    changed();
}

bool StreamManager::addManualStream(const std::string& sdp, std::string& id, std::string& error)
{
    aes67_sdp_t parsed;
    char err[160];
    if (!aes67_sdp_parse(sdp.c_str(), sdp.size(), &parsed, err, sizeof err))
    {
        error = err;
        return false;
    }
    {
        std::lock_guard<std::mutex> g(lock);
        id = "manual:" + md5Hex(sdp).substr(0, 12);
        Stream& s = streams[id];
        s.id = id;
        s.name = parsed.session_name;
        s.sdp = sdp;
        s.via = "manual";
        s.lastHeardMs = wallMs();
    }
    changed();
    return true;
}

bool StreamManager::removeStream(const std::string& id)
{
    bool found;
    {
        std::lock_guard<std::mutex> g(lock);
        found = streams.erase(id) > 0;
    }
    if (found)
        changed();
    return found;
}

void StreamManager::upsertNmosStream(const std::string& senderId, const std::string& name, const std::string& sdp,
                                     const std::vector<std::string>& channelLabels)
{
    std::lock_guard<std::mutex> g(lock);
    const std::string id = "nmos:" + senderId;
    Stream& s = streams[id];
    s.id = id;
    s.name = name;
    s.sdp = sdp;
    s.via = "nmos";
    s.channelLabels = channelLabels;
    s.lastHeardMs = wallMs();
}

void StreamManager::expireNmosStreamsExcept(const std::vector<std::string>& keep)
{
    std::lock_guard<std::mutex> g(lock);
    for (auto it = streams.begin(); it != streams.end();)
    {
        if (it->second.via == "nmos" && std::find(keep.begin(), keep.end(), it->first.substr(5)) == keep.end())
            it = streams.erase(it);
        else
            ++it;
    }
}

bool StreamManager::subscribeToStream(int receiver, const std::string& streamId, std::string& error)
{
    std::string sdp, senderId;
    {
        std::lock_guard<std::mutex> g(lock);
        if (streamId.rfind("own:", 0) == 0)
        {
            const int k = std::atoi(streamId.c_str() + 4);
            if (k >= 1 && k <= (int) tx.size() && tx[(size_t) k - 1].txSlot >= 0)
                sdp = makeSdp(tx[(size_t) k - 1]);
        }
        else if (auto it = streams.find(streamId); it != streams.end())
        {
            sdp = it->second.sdp;
            if (it->second.via == "nmos")
                senderId = streamId.substr(5);
        }
        if (sdp.empty())
        {
            error = "no such stream";
            return false;
        }
    }
    if (!subscribeReceiver(receiver, sdp, senderId, error))
        return false;
    std::lock_guard<std::mutex> g(lock);
    rx[(size_t) receiver - 1].streamId = streamId;
    return true;
}

/* ---- reports and persistence ---------------------------------------------------------------- */

nlohmann::json StreamManager::streamsJson() const
{
    std::lock_guard<std::mutex> g(lock);
    auto out = nlohmann::json::array();
    for (auto& [id, s] : streams)
    {
        aes67_sdp_t p;
        char err[64];
        const bool ok = aes67_sdp_parse(s.sdp.c_str(), s.sdp.size(), &p, err, sizeof err);
        nlohmann::json j = { { "id", id }, { "name", s.name }, { "via", s.via }, { "sdp", s.sdp },
                             { "channelLabels", s.channelLabels } };
        if (ok)
        {
            j["channels"] = p.channels;
            j["sampleRate"] = p.sample_rate;
            j["encoding"] = p.encoding;
            j["group"] = p.connection_ip;
            j["description"] = describeSdp(p);
        }
        out.push_back(j);
    }
    // This computer's own senders, so a stream can be looped back for testing.
    for (auto& snd : tx)
    {
        if (snd.txSlot < 0)
            continue;
        out.push_back({ { "id", "own:" + std::to_string(snd.index) }, { "name", snd.label }, { "via", "this computer" },
                        { "sdp", makeSdp(snd) }, { "channelLabels", nlohmann::json::array() }, { "channels", snd.channels },
                        { "sampleRate", cfg.sampleRate }, { "encoding", "L24" }, { "group", snd.group },
                        { "description", std::to_string(snd.channels) + " ch, " + std::to_string(cfg.sampleRate / 1000.0).substr(0, 4) + " kHz, L24" } });
    }
    return out;
}

nlohmann::json StreamManager::receiversJson() const
{
    std::lock_guard<std::mutex> g(lock);
    const int64_t now = monoNowNs();
    auto out = nlohmann::json::array();
    for (auto& r : rx)
    {
        nlohmann::json j = { { "index", r.index }, { "label", r.label }, { "active", r.active }, { "error", r.error } };
        // The health fields Manifold publishes for its own receivers (the
        // "Manifold <-> Inlet" spec), always present; zeros while idle.
        nlohmann::json health = { { "receiving", false }, { "packets", 0 }, { "lost", 0 }, { "reordered", 0 },
                                  { "wrongPayload", 0 }, { "jitterWorstNs", 0 }, { "resyncs", 0 }, { "underruns", 0 },
                                  { "latencyFrames", region.header().linkOffsetFrames.load() }, { "bridged", false } };
        if (r.active)
        {
            aes67_netrx_stats_t st {};
            aes67_netrx_get_stats(netRx, r.rxSlot, &st);
            j["stream"] = { { "name", r.sdp.session_name }, { "id", r.streamId }, { "senderId", r.senderId },
                            { "channels", r.sdp.channels }, { "sampleRate", r.sdp.sample_rate }, { "group", r.sdp.connection_ip } };
            // Receiving while the packet count moves: the library times
            // arrivals on its own monotonic clock, not on ours.
            if (st.packets != r.seenPackets)
            {
                r.seenPackets = st.packets;
                r.seenMovingMono = now;
            }
            health["receiving"] = st.packets != 0 && now - r.seenMovingMono < 1'000'000'000LL;
            health["packets"] = st.packets;
            health["lost"] = st.lost;
            health["reordered"] = st.reordered;
            health["wrongPayload"] = st.wrong_pt;
            health["jitterWorstNs"] = st.jitter_worst_ns;
            health["bridged"] = r.bridged;
            if (r.bridge)
            {
                // Through the rate bridge: its restarts and dry spells, and the
                // fill it holds on top of the link offset.
                const auto b = r.bridge->stats();
                health["resyncs"] = b.resets;
                health["underruns"] = b.underruns;
                health["latencyFrames"] = region.header().linkOffsetFrames.load() + (int64_t) cfg.sampleRate * 5 / 1000;
                health["bridge"] = { { "driftPpm", b.driftPpm }, { "tracking", b.tracking } };
            }
        }
        j["health"] = health;
        out.push_back(j);
    }
    return out;
}

nlohmann::json StreamManager::sendersJson() const
{
    std::lock_guard<std::mutex> g(lock);
    auto out = nlohmann::json::array();
    for (auto& s : tx)
    {
        nlohmann::json j = { { "index", s.index }, { "label", s.label }, { "channels", s.channels },
                             { "firstOutput", s.firstOutput }, { "group", s.group }, { "port", s.port },
                             { "ptimeUs", s.ptimeUs }, { "enabled", s.enabled } };
        // Sender health: on the wire or not, and packets sent (silent ones
        // included: no host was writing). Open but not sending (every send
        // failing, as on a saturated Wi-Fi link) is not "sending".
        nlohmann::json health = { { "sending", false }, { "packets", 0 }, { "silentPackets", 0 }, { "skipped", 0 },
                                  { "sendErrors", 0 } };
        if (s.txSlot >= 0)
        {
            const TxStreamStats st = netTx.stats(s.txSlot);
            const auto now = std::chrono::steady_clock::now();
            if (st.packets != s.seenPackets || s.seenMoving == std::chrono::steady_clock::time_point {})
            {
                s.seenPackets = st.packets;
                s.seenMoving = now;
            }
            health["sending"] = now - s.seenMoving < std::chrono::seconds(3);
            health["packets"] = st.packets;
            health["silentPackets"] = st.silentPackets;
            health["skipped"] = st.skipped;
            health["sendErrors"] = st.sendErrors;
        }
        j["health"] = health;
        out.push_back(j);
    }
    return out;
}

nlohmann::json StreamManager::persistentState() const
{
    std::lock_guard<std::mutex> g(lock);
    nlohmann::json j;
    j["receivers"] = nlohmann::json::array();
    for (auto& r : rx)
        if (r.active)
            j["receivers"].push_back({ { "index", r.index }, { "sdp", r.sdpText }, { "senderId", r.senderId }, { "streamId", r.streamId } });
    j["senders"] = nlohmann::json::array();
    for (auto& s : tx)
        j["senders"].push_back({ { "index", s.index }, { "enabled", s.enabled } });
    j["inputMap"] = nlohmann::json::array();
    for (uint32_t i = 0; i < region.header().inputs; ++i)
        j["inputMap"].push_back(region.inputMap()[i].load());
    j["labels"] = { { "inputs", inputLabels }, { "outputs", outputLabels } };
    j["manualStreams"] = nlohmann::json::array();
    for (auto& [id, s] : streams)
        if (s.via == "manual")
            j["manualStreams"].push_back(s.sdp);
    return j;
}

void StreamManager::restoreState(const nlohmann::json& j)
{
    if (!j.is_object())
        return;
    if (j.contains("labels"))
        setDeviceLabels(j["labels"].value("inputs", std::vector<std::string> {}), j["labels"].value("outputs", std::vector<std::string> {}));
    if (j.contains("inputMap") && j["inputMap"].is_array())
        for (uint32_t i = 0; i < region.header().inputs && i < j["inputMap"].size(); ++i)
            if (j["inputMap"][i].is_number_unsigned())
                region.inputMap()[i].store(j["inputMap"][i].get<uint32_t>());
    for (auto& s : j.value("senders", nlohmann::json::array()))
    {
        std::string error;
        setSenderEnabled(s.value("index", 0), s.value("enabled", true), error);
    }
    for (auto& sdp : j.value("manualStreams", nlohmann::json::array()))
        if (sdp.is_string())
        {
            std::string id, error;
            addManualStream(sdp.get<std::string>(), id, error);
        }
    for (auto& r : j.value("receivers", nlohmann::json::array()))
    {
        std::string error;
        const int k = r.value("index", 0);
        if (subscribeReceiver(k, r.value("sdp", std::string()), r.value("senderId", std::string()), error))
        {
            std::lock_guard<std::mutex> g(lock);
            rx[(size_t) k - 1].streamId = r.value("streamId", std::string());
        }
    }
}

/* ---- the manager's thread -------------------------------------------------------------------- */

void StreamManager::reevaluateClocks()
{
    // When Inlet's clock changes grandmaster (or locks, or loses lock), a
    // stream that was direct may now need bridging, or the reverse; and our
    // senders' SDPs name the new clock.
    ClockSample clk;
    if (!region.clock().read(clk))
        return;
    bool moved;
    {
        std::lock_guard<std::mutex> g(lock);
        moved = std::memcmp(clk.gmIdentity, lastGm, 8) != 0 || (clk.state == ClockState::FreeRun) != (lastState == ClockState::FreeRun);
        if (!moved)
            return;
        std::memcpy(lastGm, clk.gmIdentity, 8);
        lastState = clk.state;
        for (auto& r : rx)
        {
            if (!r.active)
                continue;
            const bool shouldBridge = !sharesOurClock(r) || (r.sdp.sample_rate && r.sdp.sample_rate != cfg.sampleRate);
            if (shouldBridge != r.bridged)
            {
                closeReceiver(r);
                std::string error;
                if (!openReceiver(r, error))
                    r.error = error;
            }
        }
        if (sap)
            for (auto& s : tx)
                if (s.txSlot >= 0)
                    sap->announce("tx" + std::to_string(s.index), makeSdp(s));
    }
    changed();
}

void StreamManager::run()
{
    int64_t lastSlowMs = 0;
    while (running.load())
    {
        bool anyBridge = false;
        {
            std::lock_guard<std::mutex> g(lock);
            ClockSample clk;
            if (region.clock().read(clk))
            {
                const uint64_t outNow = sampleAt(clk.ptpAt(monoNowNs()), cfg.sampleRate);
                for (auto& r : rx)
                    if (r.bridge)
                    {
                        r.bridge->tick(outNow);
                        anyBridge = true;
                    }
            }
        }
        const int64_t now = wallMs();
        if (now - lastSlowMs >= 1000)
        {
            lastSlowMs = now;
            reevaluateClocks();
            bool expired = false;
            {
                std::lock_guard<std::mutex> g(lock);
                for (auto it = streams.begin(); it != streams.end();)
                    if (it->second.via == "sap" && now - it->second.lastHeardMs > kSapExpireMs)
                    {
                        it = streams.erase(it);
                        expired = true;
                    }
                    else
                        ++it;
            }
            if (expired)
                changed();
        }
        // A millisecond, really (a plain sleep on Windows can be fifteen):
        // bridges write two milliseconds ahead of the drivers.
        sleepUntilMono(monoNowNs() + (anyBridge ? 1'000'000 : 50'000'000));
    }
}

} // namespace inlet
