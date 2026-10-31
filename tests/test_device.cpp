// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "check.h"
#include "inlet/DeviceClient.h"
#include "inlet/SharedRegion.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

#include <aes67/rtp.h>

using namespace inlet;

static std::string regionName()
{
    return "InletTest." + std::to_string(getpid());
}

static void sharesAudio()
{
    const std::string name = regionName();
    std::string err;
    shm::Region::Layout layout;
    layout.inputs = 4;
    layout.outputs = 2;
    layout.receivers = 2;
    layout.receiverChannels = 2;
    auto daemon = shm::Region::create(layout, err, name.c_str());
    CHECK(daemon != nullptr);
    if (!daemon)
    {
        std::fprintf(stderr, "%s\n", err.c_str());
        return;
    }
    daemon->header().linkOffsetFrames.store(10);
    // Device input 1 plays receiver 2's channel 1; the rest are silent.
    daemon->inputMap()[1].store(shm::mapCell(2, 1));
    for (uint32_t t = 1000; t < 1100; ++t)
        aes67_ring_write(daemon->rxRings(2) + 1, t, (int32_t) (t * 100));
    std::strcpy(daemon->inputLabel(1), "Kick In");
    daemon->header().daemonHeartbeatMono.store(monoNowNs());

    DeviceClient::Options opt;
    opt.app = "test host";
    opt.regionName = name.c_str();
    auto a = DeviceClient::attach(opt, err);
    auto b = DeviceClient::attach(opt, err);
    CHECK(a && b);
    if (!a || !b)
        return;
    CHECK_EQ(a->inputs(), 4u);
    CHECK(a->daemonAlive(monoNowNs()));
    CHECK(a->inputLabel(1) == "Kick In");
    CHECK(std::string(daemon->clients()[0].app) == "test host");

    // Period [1010, 1020) reads the ring at 1000..1009 (10 frames of link offset).
    std::vector<float> in0(10), in1(10);
    float* chans[4] = { in0.data(), in1.data(), nullptr, nullptr };
    a->readInputs(1010, chans, 4, 10);
    CHECK_NEAR(in1[0], 1000 * 100 / 8388608.0, 1e-7);
    CHECK_NEAR(in1[9], 1009 * 100 / 8388608.0, 1e-7);
    CHECK_EQ(in0[0], 0.0f);
    std::vector<int32_t> i1(10);
    int32_t* ichans[4] = { nullptr, i1.data(), nullptr, nullptr };
    a->readInputsInt32(1010, ichans, 4, 10);
    CHECK_EQ(i1[3], 1003 * 100 * 256);

    // Each output writer has a layer of its own.
    std::vector<float> out(8, 0.25f);
    const float* ochans[2] = { out.data(), nullptr };
    a->writeOutputs(5000, ochans, 2, 8);   // not writing outputs yet: ignored
    int32_t v = 0;
    CHECK_EQ(aes67_ring_read(daemon->txRing(0, 0), 5000, &v, 1), 0);
    CHECK(a->claimOutputs());
    CHECK(b->claimOutputs());
    CHECK_EQ(daemon->header().outputLayers[0].load(), 1u);   // slot 0 + 1
    CHECK_EQ(daemon->header().outputLayers[1].load(), 2u);
    a->writeOutputs(5000, ochans, 2, 8);
    CHECK_EQ(aes67_ring_read(daemon->txRing(0, 0), 5000, &v, 1), 1);
    CHECK_EQ(v, 2097152);   // 0.25 x 2^23
    CHECK_EQ(aes67_ring_read(daemon->txRing(0, 1), 5007, &v, 1), 1);
    CHECK_EQ(v, 0);
    CHECK_EQ(aes67_ring_read(daemon->txRing(1, 0), 5000, &v, 1), 0);   // b's layer: nothing yet
    b->writeOutputs(5000, ochans, 2, 8);
    CHECK_EQ(aes67_ring_read(daemon->txRing(1, 0), 5000, &v, 1), 1);
    a.reset();   // detaching gives the layer up
    CHECK_EQ(daemon->header().outputLayers[0].load(), 0u);
    CHECK_EQ(daemon->clients()[0].state.load(), 0u);
    // Every layer taken: the next host plays inputs only.
    std::vector<std::unique_ptr<DeviceClient>> more;
    for (int i = 0; i < shm::kOutputLayers; ++i)
    {
        more.push_back(DeviceClient::attach(opt, err));
        CHECK(more.back() != nullptr);
    }
    int claimed = 0;
    for (auto& m : more)
        claimed += m && m->claimOutputs() ? 1 : 0;
    CHECK_EQ(claimed, shm::kOutputLayers - 1);   // b holds one
}

/** The engine wakes once a period, on the media clock, with consecutive positions. */
static void periodsOnTime()
{
    const std::string name = regionName() + "b";
    std::string err;
    auto daemon = shm::Region::create({}, err, name.c_str());
    CHECK(daemon != nullptr);
    if (!daemon)
        return;
    ClockSample c;
    c.monoNs = monoNowNs();
    c.ptpNs = 1'780'000'000'000'000'000LL;
    daemon->clock().publish(c);

    DeviceClient::Options opt;
    opt.regionName = name.c_str();
    auto client = DeviceClient::attach(opt, err);
    CHECK(client != nullptr);
    if (!client)
        return;

    std::mutex m;
    std::vector<uint64_t> starts;
    std::vector<int64_t> wakes;
    int late = 0;
    PeriodEngine engine(*client, 128, [&](const PeriodEngine::Period& p) {
        std::lock_guard<std::mutex> g(m);
        starts.push_back(p.start);
        wakes.push_back(monoNowNs());
        late += p.late ? 1 : 0;
    });
    engine.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    engine.stop();

    std::lock_guard<std::mutex> g(m);
    // 300 ms of 128-frame periods at 48 kHz is about 112.
    CHECK(starts.size() >= 100);
    for (size_t i = 1; i < starts.size(); ++i)
        CHECK_EQ(starts[i] - starts[i - 1], 128u);
    // Each wake-up is at its period's end on the media clock.
    for (size_t i = 0; i < starts.size(); ++i)
    {
        const int64_t deadline = c.monoAt(ptpAtSample(starts[i] + 128, 48000));
        CHECK(wakes[i] >= deadline);
    }
    std::printf("%zu periods, %d late, %llu resyncs\n", starts.size(), late, (unsigned long long) engine.resyncs());
    CHECK(daemon->clients()[0].periods.load() == starts.size());
}

/** A 44.1 kHz host on a 48 kHz device: periods counted at 44.1 kHz, on the
    media clock; a PTP step and a switch to a restarted inletd's region each
    re-anchor the schedule and say so on the next period. */
static void hostRateStepsAndSwitch()
{
    const std::string nameA = regionName() + "c", nameB = regionName() + "d";
    std::string err;
    auto daemonA = shm::Region::create({}, err, nameA.c_str());
    auto daemonB = shm::Region::create({}, err, nameB.c_str());
    CHECK(daemonA && daemonB);
    if (!daemonA || !daemonB)
        return;
    ClockSample c;
    c.monoNs = monoNowNs();
    c.ptpNs = 1'780'000'000'000'000'000LL;
    daemonA->clock().publish(c);
    ClockSample later = c;
    later.ptpNs += 5'000'000'000LL;   // the restarted inletd's clock: 5 s on
    daemonB->clock().publish(later);

    DeviceClient::Options opt;
    opt.regionName = nameA.c_str();
    auto clientA = DeviceClient::attach(opt, err);
    opt.regionName = nameB.c_str();
    auto clientB = DeviceClient::attach(opt, err);
    CHECK(clientA && clientB);
    if (!clientA || !clientB)
        return;

    std::mutex m;
    std::vector<PeriodEngine::Period> periods;
    std::vector<int64_t> wakes;
    PeriodEngine* self = nullptr;
    PeriodEngine engine(*clientA, 128, [&](const PeriodEngine::Period& p) {
        std::lock_guard<std::mutex> g(m);
        periods.push_back(p);
        wakes.push_back(monoNowNs());
        if (periods.size() == 40)
        {
            // PTP stepped by a second: a new epoch.
            ClockSample stepped = c;
            stepped.ptpNs += 1'000'000'000LL;
            stepped.epoch = 1;
            daemonA->clock().publish(stepped);
        }
        if (periods.size() == 80)
            self->switchClient(*clientB);
    }, 44100);
    self = &engine;
    engine.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    engine.stop();

    std::lock_guard<std::mutex> g(m);
    // 400 ms of 128-frame periods at 44.1 kHz is about 138.
    CHECK(periods.size() >= 110);
    int resyncs = 0, behind = 0;
    for (size_t i = 1; i < periods.size(); ++i)
    {
        if (periods[i].resynced)
        {
            // The step and the switch; any other is the thread having fallen
            // more than four periods behind (a loaded machine), which
            // re-anchors by design: rare, or something is wrong.
            if (i == 40 || i == 80)
                ++resyncs;
            else
                ++behind;
            continue;
        }
        CHECK_EQ(periods[i].start - periods[i - 1].start, 128u);
    }
    CHECK_EQ(resyncs, 2);
    CHECK(behind <= 2);
    CHECK(periods[40].resynced);   // the step, seen after period 40's callback
    CHECK(periods[80].resynced);   // the switch
    // After the switch, the periods are on the new clock, at 44.1 kHz.
    for (size_t i = 81; i < periods.size(); ++i)
        CHECK(wakes[i] >= later.monoAt(ptpAtSample(periods[i].start + 128, 44100)));
    // The client says it runs at 44.1 kHz on the 48 kHz device.
    CHECK_EQ(daemonB->clients()[0].sampleRate.load(), 44100u);
}

/** Each client counts the frames it asked of a receiver and did not get, once
    per frame whichever of the receiver's channels it read, into the region. */
static void countsMissingFrames()
{
    const std::string name = regionName() + "m";
    std::string err;
    shm::Region::Layout layout;
    layout.inputs = 4;
    layout.outputs = 2;
    layout.receivers = 2;
    layout.receiverChannels = 2;
    auto daemon = shm::Region::create(layout, err, name.c_str());
    CHECK(daemon != nullptr);
    if (!daemon)
        return;
    daemon->header().linkOffsetFrames.store(0);
    daemon->header().daemonHeartbeatMono.store(monoNowNs());
    // Inputs 1-2: receiver 1's two channels; input 3: receiver 2, never written.
    daemon->inputMap()[0].store(shm::mapCell(1, 0));
    daemon->inputMap()[1].store(shm::mapCell(1, 1));
    daemon->inputMap()[2].store(shm::mapCell(2, 0));
    // Frames 1000..1099 on both channels, but for one 8-frame packet that never came.
    for (int c = 0; c < 2; ++c)
        for (uint32_t t = 1000; t < 1100; ++t)
            if (t < 1040 || t >= 1048)
                aes67_ring_write(daemon->rxRings(1) + c, t, (int32_t) t);
    auto missing = [&](int receiver) { return daemon->receiverStats(receiver).missingFrames.load(); };

    DeviceClient::Options opt;
    opt.app = "recorder";
    opt.regionName = name.c_str();
    auto a = DeviceClient::attach(opt, err);
    CHECK(a != nullptr);
    if (!a)
        return;
    std::vector<int32_t> buf(64);
    a->readInputRaw(1000, 0, buf.data(), 50);
    CHECK_EQ(buf[39], 1039);
    CHECK_EQ(buf[40], 0);   // the lost packet: silence, not what the slot held before
    CHECK_EQ(missing(1), 8u);
    a->readInputRaw(1000, 1, buf.data(), 50);   // the receiver's other channel, same frames
    CHECK_EQ(missing(1), 8u);
    a->readInputRaw(1000, 2, buf.data(), 50);   // no stream on receiver 2: not loss
    CHECK_EQ(missing(2), 0u);
    // The float path, two channels a period: 1090..1109, of which 1100..1109 have not arrived.
    std::vector<float> f0(20), f1(20);
    float* chans[4] = { f0.data(), f1.data(), nullptr, nullptr };
    a->readInputs(1090, chans, 4, 20);
    CHECK_EQ(missing(1), 18u);
    // Partly read before: only the new frames count.
    a->readInputRaw(1105, 0, buf.data(), 10);
    CHECK_EQ(missing(1), 23u);

    // A client that reconnects carries on from the region's count.
    a.reset();
    auto b = DeviceClient::attach(opt, err);
    CHECK(b != nullptr);
    if (!b)
        return;
    std::vector<int32_t> i0(10);
    int32_t* ichans[4] = { i0.data(), nullptr, nullptr, nullptr };
    b->readInputsInt32(1095, ichans, 4, 10);   // 1095..1104: 5 missing
    CHECK_EQ(missing(1), 28u);
    CHECK_EQ(i0[0], 1095 * 256);
}

int main()
{
    sharesAudio();
    countsMissingFrames();
    periodsOnTime();
    hostRateStepsAndSwitch();
    TEST_MAIN_END();
}
