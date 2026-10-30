// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

/*
 * inlet-pipewire: Inlet as a PipeWire device, for the programs of the user
 * it runs for (a systemd user service, started with PipeWire).
 *
 * Two nodes, "Inlet" the sink (programs play into the device's outputs) and
 * "Inlet" the source (they record its inputs), in one node group, so one
 * driver schedules both: the sink, which drives. Its cycles are triggered at
 * the period boundaries of the PTP media clock, read from inletd's shared
 * region as the ASIO driver does, so every program on Inlet runs on the
 * network's clock, without resampling. Other devices linked with Inlet follow
 * it (PipeWire matches their rates); Inlet's driver priority is high so it is
 * the one chosen.
 *
 * The graph's rate and quantum are PipeWire's: the nodes ask for the device's
 * rate, and if the graph runs at another the audio is converted (HostAudio),
 * still on the same clock. Positions are counted at the graph's rate on the
 * media clock. Inputs are the period as received, the link offset behind;
 * outputs play two quanta after the cycle they were written in.
 *
 * When inletd stops, the nodes stay and carry silence; when it comes back with
 * the same channels they carry on, and with other channels they are made
 * again with the new count.
 */

#include "inlet/Clock.h"
#include "inlet/DeviceClient.h"
#include "inlet/HostAudio.h"
#include "inlet/NetTx.h"   // sleepUntilMono

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <unistd.h>

using namespace inlet;

namespace
{

constexpr int64_t kReattachEveryNs = 500'000'000;
constexpr uint32_t kDefaultQuantum = 256;
constexpr uint32_t kMaxQuantum = 8192;

pw_main_loop* gLoop = nullptr;

void onSignal(int)
{
    if (gLoop != nullptr)
        pw_main_loop_quit(gLoop);
}

/** "FL,FR,AUX2,AUX3,...": stereo programs land on the first two channels. */
std::string positions(uint32_t channels)
{
    std::string s;
    for (uint32_t c = 0; c < channels; ++c)
    {
        if (c > 0)
            s += ",";
        s += c == 0 ? "FL" : c == 1 ? "FR" : "AUX" + std::to_string(c);
    }
    return s;
}

/** The quantum and rate the graph wants for the next cycle. PipeWire 0.3.82
    and later keep them apart from the current ones (target_*); before that
    the driver read them from the current fields. */
template <typename C> auto targetDuration(const C& c, int) -> decltype(c.target_duration) { return c.target_duration; }
template <typename C> uint64_t targetDuration(const C& c, long) { return c.duration; }
template <typename C> auto targetRate(const C& c, int) -> decltype(c.target_rate) { return c.target_rate; }
template <typename C> spa_fraction targetRate(const C& c, long) { return c.rate; }

struct Device;

/** One node: the sink (programs' audio to the network) or the source. */
struct Node
{
    Device* device = nullptr;
    bool sink = false;
    pw_stream* stream = nullptr;
    spa_hook listener {};
    std::atomic<bool> streaming { false };   // connected and running: its cycles can be triggered
    uint32_t channels = 0;
    std::unique_ptr<HostAudio> audio;
    uint32_t audioRate = 0;
    uint64_t resyncsSeen = 0;
    uint64_t lastCycle = 0;   // the cycle last produced
    bool produced = false;
    std::atomic<spa_io_position*> position { nullptr };   // this node's view of its driver
    std::atomic<uint32_t> lastFrames { 0 };
    std::atomic<float> lastPeak { 0.0f };
    std::vector<std::vector<float>> buffers;
    std::vector<float*> pointers;
};

struct Device
{
    pw_main_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;

    std::vector<std::unique_ptr<DeviceClient>> clients;   // the last is current
    std::atomic<DeviceClient*> client { nullptr };
    Node sinkNode, sourceNode;
    std::mutex nodesLock;   // the clock triggers the sink while the main loop does not remake it

    // The driver's view of the graph, from the sink's position.
    std::atomic<spa_io_position*> position { nullptr };
    std::atomic<uint32_t> quantum { kDefaultQuantum };
    std::atomic<uint32_t> graphRate { 0 };
    std::atomic<uint64_t> cycleStart { 0 };     // the period being processed, at the graph rate
    std::atomic<uint64_t> resyncs { 0 };   // the schedule re-anchored this many times
    // Cycles triggered (the source makes one period in each), and processed
    // (for INLET_PIPEWIRE_DEBUG).
    std::atomic<uint64_t> triggers { 0 }, sinkCycles { 0 }, sourceCycles { 0 };

    std::atomic<bool> running { true };
    std::thread timer;
    int64_t lastAttachTry = 0;
    bool rebuild = false;   // the main loop makes the nodes again (another channel count)

    DeviceClient* cur() const { return client.load(); }
};

/* ---- the nodes' processing, on PipeWire's data thread --------------------------------------- */

/** Whether the PipeWire library running is older than this version. */
bool libraryBefore(int major, int minor, int micro)
{
    int a = 0, b = 0, c = 0;
    std::sscanf(pw_get_library_version(), "%d.%d.%d", &a, &b, &c);
    return std::make_tuple(a, b, c) < std::make_tuple(major, minor, micro);
}

void process(void* data)
{
    Node* n = static_cast<Node*>(data);
    Device* d = n->device;
    DeviceClient* c = d->cur();
    (n->sink ? d->sinkCycles : d->sourceCycles)++;
    // One period per cycle on PipeWire 0.3, which asks an output stream again
    // in a cycle and would repeat the period. (1.x asks again only when its
    // converter needs more, and stalls the source if it gets nothing. A cycle,
    // not its start: after a resync, the next cycle can start where the last did.)
    const uint64_t start = d->cycleStart.load();
    const uint64_t cycle = d->triggers.load();
    static const bool oncePerCycle = libraryBefore(1, 0, 0);
    if (!n->sink && oncePerCycle && n->produced && cycle == n->lastCycle)
        return;
    n->lastCycle = cycle;
    n->produced = true;
    pw_buffer* b = pw_stream_dequeue_buffer(n->stream);
    if (b == nullptr)
        return;
    spa_buffer* buf = b->buffer;
    spa_data& sd = buf->datas[0];
    const uint32_t stride = n->channels * sizeof(float);
    if (sd.data == nullptr || stride == 0 || c == nullptr)
    {
        pw_stream_queue_buffer(n->stream, b);
        return;
    }

    // The graph's rate, which the conversion (if any) follows.
    const uint32_t rate = d->graphRate.load() ? d->graphRate.load() : c->sampleRate();
    if (!n->audio || n->audioRate != rate)
    {
        std::vector<int> chans;
        for (uint32_t k = 0; k < n->channels; ++k)
            chans.push_back((int) k);
        const int quality = HostAudio::qualityFor(2 * n->channels, rate, c->sampleRate());
        n->audio = std::make_unique<HostAudio>(!n->sink, chans, rate, c->sampleRate(), kMaxQuantum, quality);
        n->audioRate = rate;
    }
    if (const uint64_t r = d->resyncs.load(); r != n->resyncsSeen)
    {
        n->resyncsSeen = r;
        n->audio->reset();
    }

    auto* samples = static_cast<float*>(sd.data);
    if (n->sink)
    {
        const uint32_t frames = std::min(sd.chunk->size / stride, kMaxQuantum);
        for (uint32_t f = 0; f < frames; ++f)
            for (uint32_t k = 0; k < n->channels; ++k)
                n->buffers[k][f] = samples[f * n->channels + k];
        // Played two quanta after this cycle, as ASIO's double buffer and one more.
        n->audio->write(*c, start + 2 * (uint64_t) frames, (int) frames, n->pointers.data());
    }
    else
    {
        uint32_t frames = std::min(d->quantum.load(), sd.maxsize / stride);
        // pw_buffer.requested is newer than PipeWire 0.3.48 (Ubuntu 22.04):
        // read only where the library running has it.
        static const bool hasRequested = !libraryBefore(0, 3, 49);
        if (hasRequested && b->requested != 0)
            frames = std::min<uint32_t>(frames, (uint32_t) b->requested);
        frames = std::min(frames, kMaxQuantum);
        n->audio->read(*c, start, (int) frames, n->pointers.data());
        for (uint32_t f = 0; f < frames; ++f)
            for (uint32_t k = 0; k < n->channels; ++k)
                samples[f * n->channels + k] = n->buffers[k][f];
        sd.chunk->offset = 0;
        sd.chunk->stride = (int32_t) stride;
        sd.chunk->size = frames * stride;
        float peak = 0.0f;
        for (uint32_t i = 0; i < frames * n->channels; ++i)
            peak = std::max(peak, std::fabs(samples[i]));
        n->lastFrames = frames;
        n->lastPeak = peak;
    }
    pw_stream_queue_buffer(n->stream, b);
}

void ioChanged(void* data, uint32_t id, void* area, uint32_t size)
{
    Node* n = static_cast<Node*>(data);
    if (id != SPA_IO_Position)
        return;
    spa_io_position* pos = size >= sizeof(spa_io_position) ? static_cast<spa_io_position*>(area) : nullptr;
    n->position.store(pos);
    if (n->sink)
        n->device->position.store(pos);
}

void stateChanged(void* data, pw_stream_state, pw_stream_state state, const char* error)
{
    Node* n = static_cast<Node*>(data);
    n->streaming = state == PW_STREAM_STATE_STREAMING;
    if (state == PW_STREAM_STATE_ERROR)
        std::fprintf(stderr, "inlet-pipewire: %s: %s\n", n->sink ? "sink" : "source", error ? error : "error");
}

const pw_stream_events kEvents = [] {
    pw_stream_events e {};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = process;
    e.io_changed = ioChanged;
    e.state_changed = stateChanged;
    return e;
}();

bool makeNode(Device& d, Node& n, bool sink, uint32_t channels, uint32_t deviceRate)
{
    n.device = &d;
    n.sink = sink;
    n.channels = channels;
    n.audio.reset();
    n.buffers.assign(channels, std::vector<float>(kMaxQuantum, 0.0f));
    n.pointers.clear();
    for (auto& b : n.buffers)
        n.pointers.push_back(b.data());

    const std::string rate = "1/" + std::to_string(deviceRate);
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, sink ? "Playback" : "Capture", PW_KEY_MEDIA_CLASS,
        sink ? "Audio/Sink" : "Audio/Source", PW_KEY_NODE_NAME, sink ? "inlet.sink" : "inlet.source",
        PW_KEY_NODE_DESCRIPTION, "Inlet AES67 virtual sound card", PW_KEY_NODE_NICK, "Inlet",
        "node.group", "inlet", PW_KEY_NODE_RATE, rate.c_str(), "node.force-rate", std::to_string(deviceRate).c_str(),
        "priority.driver", "30000", "priority.session", "1000", "node.always-process", "true",
        SPA_KEY_AUDIO_POSITION, positions(channels).c_str(), PW_KEY_DEVICE_API, "inlet", nullptr);
    n.stream = pw_stream_new(d.core, sink ? "Inlet sink" : "Inlet source", props);
    if (n.stream == nullptr)
        return false;
    pw_stream_add_listener(n.stream, &n.listener, &kEvents, &n);

    // 32-bit float, interleaved, the device's channels, at whatever rate the
    // graph runs (preferably the device's).
    uint8_t pod[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(pod, sizeof pod);
    const spa_pod* params[2];
    spa_pod_frame f;
    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio), SPA_FORMAT_mediaSubtype,
                        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_F32),
                        SPA_FORMAT_AUDIO_rate, SPA_POD_CHOICE_RANGE_Int((int) deviceRate, 8000, 384000),
                        SPA_FORMAT_AUDIO_channels, SPA_POD_Int((int) channels), 0);
    params[0] = (const spa_pod*) spa_pod_builder_pop(&b, &f);
    // Buffers enough that one can be filled while the other side holds the
    // last. (PipeWire 1.x otherwise gives a stream only one.)
    const int stride = (int) (channels * sizeof(float));
    params[1] = (const spa_pod*) spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 2, 8),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size, SPA_POD_Int(stride * (int) kMaxQuantum),
        SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride));

    const auto flags = (pw_stream_flags) (PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS
                                          | (sink ? PW_STREAM_FLAG_DRIVER : 0));
    return pw_stream_connect(n.stream, sink ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 2) == 0;
}

void destroyNode(Node& n)
{
    n.streaming = false;
    if (n.stream != nullptr)
    {
        spa_hook_remove(&n.listener);
        pw_stream_destroy(n.stream);
        n.stream = nullptr;
    }
}

/* ---- the clock: a thread that triggers the sink's cycles on the media clock -------------------- */

/** inletd stopped or restarted: find the one running now. */
void reattach(Device& d, int64_t now)
{
    if (now - d.lastAttachTry < kReattachEveryNs)
        return;
    d.lastAttachTry = now;
    DeviceClient* old = d.cur();
    DeviceClient::Options opt;
    opt.kind = shm::ClientKind::PipeWire;
    opt.app = "PipeWire";
    std::string err;
    auto next = DeviceClient::attach(opt, err);
    if (!next || (old != nullptr && next->serial() == old->serial()) || !next->daemonAlive(now))
        return;
    if (old != nullptr && (next->inputs() != old->inputs() || next->outputs() != old->outputs()))
        d.rebuild = true;   // the main loop makes the nodes again
    next->claimOutputs();
    if (old != nullptr)
        old->releaseOutputs();
    d.clients.push_back(std::move(next));
    d.client.store(d.clients.back().get());
}

void runClock(Device& d)
{
    uint32_t quantum = 0, rate = 0;
    std::unique_ptr<PeriodClock> pc;
    DeviceClient* clockOf = nullptr;
    ClockSample clk;
    while (d.running.load())
    {
        DeviceClient* c = d.cur();
        const int64_t now = monoNowNs();
        if (c == nullptr || !c->daemonAlive(now))
            reattach(d, now);
        c = d.cur();
        if (c == nullptr || !c->clock(clk))
        {
            sleepUntilMono(now + 100'000'000);
            continue;
        }
        // The graph's quantum and rate for the next cycle, as PipeWire sets them.
        uint32_t q = d.quantum.load(), r = c->sampleRate();
        if (spa_io_position* pos = d.position.load())
        {
            if (const uint64_t td = targetDuration(pos->clock, 0); td > 0)
                q = (uint32_t) std::min<uint64_t>(td, kMaxQuantum);
            if (const spa_fraction tr = targetRate(pos->clock, 0); tr.denom > 0)
                r = tr.denom;
        }
        if (!pc || q != quantum || r != rate || clockOf != c)
        {
            const bool first = !pc;
            quantum = q;
            rate = r;
            pc = std::make_unique<PeriodClock>(rate, quantum);
            pc->start(clk, now);
            clockOf = c;
            if (!first)
                d.resyncs++;
            d.quantum = quantum;
            d.graphRate = rate;
        }
        const int64_t deadline = pc->deadlineMono(clk);
        sleepUntilMono(deadline);
        if (!d.running.load())
            break;

        // One cycle: the clock the graph runs on is the period's, on the
        // media clock; the nodes process the period that just ended.
        d.cycleStart = pc->periodStart();
        const int64_t woke = monoNowNs();
        if (spa_io_position* pos = d.position.load())
        {
            pos->clock.nsec = (uint64_t) woke;
            pos->clock.rate = SPA_FRACTION(1, rate);
            pos->clock.position = pc->periodStart();
            pos->clock.duration = quantum;
            pos->clock.delay = 0;
            pos->clock.rate_diff = 1.0;
            pos->clock.next_nsec = (uint64_t) clk.monoAt(ptpAtSample(pc->periodEnd() + quantum, rate));
        }
        // Only once the sink is streaming: PipeWire 1.x crashes on a cycle
        // triggered while it is being set up.
        {
            std::lock_guard<std::mutex> lock(d.nodesLock);
            if (d.sinkNode.stream != nullptr && d.sinkNode.streaming.load())
            {
                d.triggers++;
                pw_stream_trigger_process(d.sinkNode.stream);
            }
        }
        c->clock(clk);
        if (pc->advance(clk, monoNowNs()))
            d.resyncs++;
        c->heartbeat(woke, true, quantum, rate != c->sampleRate() ? rate : 0);
        c->countPeriod(woke - deadline > (int64_t) quantum * 1'000'000'000LL / rate / 2, false);
    }
}

/** On the main loop, now and then: make the nodes again when the device changed shape. */
void checkShape(void* data, uint64_t)
{
    Device* d = static_cast<Device*>(data);
    static const bool debug = std::getenv("INLET_PIPEWIRE_DEBUG") != nullptr;
    static int ticks = 0;
    if (debug && ++ticks % 8 == 0 && d->sinkNode.stream != nullptr)
        std::fprintf(stderr, "inlet-pipewire: driving %d, %llu cycles triggered, %llu sink and %llu source processed, quantum %u at %u Hz\n",
                     pw_stream_is_driving(d->sinkNode.stream) ? 1 : 0, (unsigned long long) d->triggers.load(),
                     (unsigned long long) d->sinkCycles.load(), (unsigned long long) d->sourceCycles.load(),
                     d->quantum.load(), d->graphRate.load());
    if (debug && ticks % 8 == 0 && d->sourceNode.stream != nullptr)
    {
        spa_io_position* sp = d->sourceNode.position.load();
        spa_io_position* kp = d->sinkNode.position.load();
        std::fprintf(stderr, "inlet-pipewire: sink node %u driver %u, source node %u driver %u; source last %u frames, peak %.3f\n",
                     pw_stream_get_node_id(d->sinkNode.stream), kp ? kp->clock.id : 0,
                     pw_stream_get_node_id(d->sourceNode.stream), sp ? sp->clock.id : 0,
                     d->sourceNode.lastFrames.load(), d->sourceNode.lastPeak.load());
    }
    DeviceClient* c = d->cur();
    if (c == nullptr)
        return;
    if (d->sinkNode.stream != nullptr && !d->rebuild)
        return;
    d->rebuild = false;
    std::lock_guard<std::mutex> lock(d->nodesLock);
    destroyNode(d->sinkNode);
    destroyNode(d->sourceNode);
    d->position = nullptr;
    if (!makeNode(*d, d->sinkNode, true, c->outputs(), c->sampleRate())
        || !makeNode(*d, d->sourceNode, false, c->inputs(), c->sampleRate()))
        std::fprintf(stderr, "inlet-pipewire: could not make the nodes\n");
    else
        std::fprintf(stderr, "inlet-pipewire: Inlet is in PipeWire: %u outputs, %u inputs at %u Hz\n", c->outputs(),
                     c->inputs(), c->sampleRate());
}

} // namespace

int main(int argc, char** argv)
{
    pw_init(&argc, &argv);
    Device d;
    d.loop = pw_main_loop_new(nullptr);
    gLoop = d.loop;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    d.context = pw_context_new(pw_main_loop_get_loop(d.loop), nullptr, 0);
    d.core = d.context ? pw_context_connect(d.context, nullptr, 0) : nullptr;
    if (d.core == nullptr)
    {
        std::fprintf(stderr, "inlet-pipewire: PipeWire is not running for this user\n");
        return 1;
    }

    // The nodes appear once inletd is found, and follow it from then on.
    d.timer = std::thread([&d] { runClock(d); });
    spa_source* tick = pw_loop_add_timer(pw_main_loop_get_loop(d.loop), checkShape, &d);
    timespec every { 0, 250'000'000 };
    pw_loop_update_timer(pw_main_loop_get_loop(d.loop), tick, &every, &every, false);

    pw_main_loop_run(d.loop);

    d.running = false;
    d.timer.join();
    destroyNode(d.sinkNode);
    destroyNode(d.sourceNode);
    pw_core_disconnect(d.core);
    pw_context_destroy(d.context);
    pw_main_loop_destroy(d.loop);
    pw_deinit();
    return 0;
}
