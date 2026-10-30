// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

/*
 * Inlet as an ALSA device: the libasound plugin behind "pcm.inlet", for
 * programs that talk to ALSA directly on systems without PipeWire (headless
 * machines, minimal installs). Playback is the device's outputs, capture its
 * inputs, the first channels of each unless the configuration says `first`.
 *
 *   pcm.inlet { type inlet }                 (installed in alsa.conf.d)
 *   pcm.inlet2 { type inlet first 8 }        (from device channel 9 on)
 *
 * Like the ASIO driver, it is a thin client of inletd's shared region. A
 * thread wakes at each period boundary of the PTP media clock (at the
 * program's own rate, converted when that is not the network's) and moves one
 * period between ALSA's buffer and the rings, so the program runs on the
 * network's clock. Capture hands the program the period as received (the link
 * offset behind); playback plays two periods after the period it was written
 * in. When inletd stops, the stream carries on silently and picks inletd up
 * again when it comes back with the same channels; with other channels the
 * stream is disconnected and the program reopens it.
 */

#include "inlet/Clock.h"
#include "inlet/DeviceClient.h"
#include "inlet/HostAudio.h"

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace
{

using namespace inlet;

constexpr unsigned kRates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
constexpr int64_t kReattachEveryNs = 500'000'000;

std::string programName()
{
    char name[64] = {};
    if (FILE* f = std::fopen("/proc/self/comm", "r"))
    {
        if (std::fgets(name, sizeof name, f) != nullptr)
            name[std::strcspn(name, "\n")] = '\0';
        std::fclose(f);
    }
    return name[0] != '\0' ? name : "ALSA program";
}

struct InletPcm
{
    snd_pcm_ioplug_t io {};
    bool playback = false;
    int first = 0;   // the first device channel

    std::vector<std::unique_ptr<DeviceClient>> clients;   // the last is current
    std::atomic<DeviceClient*> client { nullptr };
    std::unique_ptr<PeriodEngine> engine;
    std::unique_ptr<HostAudio> audio;
    std::vector<std::vector<float>> buffers;
    std::vector<float*> pointers;

    int fds[2] = { -1, -1 };   // fds[1] is the program's poll descriptor
    std::atomic<uint64_t> hwTotal { 0 };   // frames moved since prepare
    snd_pcm_uframes_t boundary = 0, availMin = 1;
    int64_t lastAttachTry = 0;

    ~InletPcm()
    {
        if (engine)
            engine->stop();
        for (int fd : fds)
            if (fd >= 0)
                ::close(fd);
    }

    DeviceClient* cur() const { return client.load(); }

    snd_pcm_uframes_t hwPointer() const { return (snd_pcm_uframes_t) (hwTotal.load() % (boundary ? boundary : 1)); }

    /** Frames the program may read (capture) or write (playback) now. */
    snd_pcm_uframes_t avail() const
    {
        const snd_pcm_uframes_t hw = hwPointer(), appl = io.appl_ptr;
        const snd_pcm_uframes_t b = boundary ? boundary : 1;
        if (playback)
        {
            const snd_pcm_uframes_t queued = (appl + b - hw) % b;
            return queued >= io.buffer_size ? 0 : io.buffer_size - queued;
        }
        return (hw + b - appl) % b;
    }

    void wake()
    {
        const char c = 0;
        if (avail() >= availMin)
            (void) !::write(fds[0], &c, 1);
    }

    /** inletd stopped or restarted: find the one running now. */
    void reattach(int64_t now)
    {
        if (now - lastAttachTry < kReattachEveryNs)
            return;
        lastAttachTry = now;
        DeviceClient* old = cur();
        DeviceClient::Options opt;
        opt.kind = shm::ClientKind::Alsa;
        opt.app = programName();
        std::string err;
        auto next = DeviceClient::attach(opt, err);
        if (!next || next->serial() == old->serial() || !next->daemonAlive(now))
            return;
        if (next->inputs() != old->inputs() || next->outputs() != old->outputs() || next->sampleRate() != old->sampleRate())
        {
            snd_pcm_ioplug_set_state(&io, SND_PCM_STATE_DISCONNECTED);
            return;
        }
        if (playback)
        {
            old->releaseOutputs();
            next->claimOutputs();
        }
        clients.push_back(std::move(next));
        client.store(clients.back().get());
        engine->switchClient(*clients.back());
    }

    void onPeriod(const PeriodEngine::Period& p)
    {
        DeviceClient* c = cur();
        const int64_t now = monoNowNs();
        if (!c->daemonAlive(now))
            reattach(now);
        if (p.resynced)
            audio->reset();
        const snd_pcm_uframes_t frames = io.period_size;
        const snd_pcm_channel_area_t* areas = snd_pcm_ioplug_mmap_areas(&io);
        const snd_pcm_uframes_t offset = hwPointer() % io.buffer_size;
        if (playback)
        {
            // What the program has written of this period; silence for the rest.
            const snd_pcm_uframes_t queued = io.buffer_size - std::min(avail(), (snd_pcm_uframes_t) io.buffer_size);
            fromArea(areas, offset, std::min(queued, frames), frames);
            audio->write(*c, p.start + 2 * (uint64_t) frames, (int) frames, pointers.data());
        }
        else
        {
            audio->read(*c, p.start, (int) frames, pointers.data());
            toArea(areas, offset, frames);
        }
        hwTotal.fetch_add(frames);
        wake();
    }

    void* at(const snd_pcm_channel_area_t& a, snd_pcm_uframes_t frame) const
    {
        return (char*) a.addr + (a.first + (size_t) a.step * frame) / 8;
    }

    void fromArea(const snd_pcm_channel_area_t* areas, snd_pcm_uframes_t offset, snd_pcm_uframes_t have, snd_pcm_uframes_t frames)
    {
        for (unsigned k = 0; k < io.channels; ++k)
        {
            float* dst = buffers[k].data();
            for (snd_pcm_uframes_t f = 0; f < frames; ++f)
            {
                if (f >= have)
                {
                    dst[f] = 0.0f;
                    continue;
                }
                const void* src = at(areas[k], (offset + f) % io.buffer_size);
                switch (io.format)
                {
                case SND_PCM_FORMAT_S16_LE: dst[f] = (float) *(const int16_t*) src / 32768.0f; break;
                case SND_PCM_FORMAT_S32_LE: dst[f] = (float) ((double) *(const int32_t*) src / 2147483648.0); break;
                default: dst[f] = *(const float*) src; break;
                }
            }
        }
    }

    void toArea(const snd_pcm_channel_area_t* areas, snd_pcm_uframes_t offset, snd_pcm_uframes_t frames)
    {
        for (unsigned k = 0; k < io.channels; ++k)
        {
            const float* src = buffers[k].data();
            for (snd_pcm_uframes_t f = 0; f < frames; ++f)
            {
                void* dst = at(areas[k], (offset + f) % io.buffer_size);
                const double v = std::clamp((double) src[f], -1.0, 1.0);
                switch (io.format)
                {
                case SND_PCM_FORMAT_S16_LE: *(int16_t*) dst = (int16_t) std::lrint(std::min(v * 32768.0, 32767.0)); break;
                case SND_PCM_FORMAT_S32_LE:
                    *(int32_t*) dst = (int32_t) std::llrint(std::min(v * 2147483648.0, 2147483647.0));
                    break;
                default: *(float*) dst = (float) v; break;
                }
            }
        }
    }
};

InletPcm* self(snd_pcm_ioplug_t* io) { return static_cast<InletPcm*>(io->private_data); }

int pcmStart(snd_pcm_ioplug_t* io)
{
    InletPcm* p = self(io);
    if (p->playback)
        p->cur()->claimOutputs();   // a layer of its own, mixed with other programs' by the sender
    p->audio->reset();
    return p->engine->start() ? 0 : -EIO;
}

int pcmStop(snd_pcm_ioplug_t* io)
{
    InletPcm* p = self(io);
    if (p->engine)
        p->engine->stop();
    if (p->playback)
        p->cur()->releaseOutputs();
    return 0;
}

snd_pcm_sframes_t pcmPointer(snd_pcm_ioplug_t* io)
{
    InletPcm* p = self(io);
    if (io->state == SND_PCM_STATE_DISCONNECTED)
        return -ENODEV;
    return (snd_pcm_sframes_t) (p->hwPointer() % io->buffer_size);
}

int pcmHwParams(snd_pcm_ioplug_t* io, snd_pcm_hw_params_t*)
{
    InletPcm* p = self(io);
    DeviceClient* c = p->cur();
    if (p->engine)
        p->engine->stop();
    std::vector<int> channels;
    for (unsigned k = 0; k < io->channels; ++k)
        channels.push_back(p->first + (int) k);
    const int quality = HostAudio::qualityFor(io->channels, io->rate, c->sampleRate());
    p->audio = std::make_unique<HostAudio>(!p->playback, channels, io->rate, c->sampleRate(), (uint32_t) io->period_size, quality);
    p->buffers.assign(io->channels, std::vector<float>(io->period_size, 0.0f));
    p->pointers.clear();
    for (auto& b : p->buffers)
        p->pointers.push_back(b.data());
    p->engine = std::make_unique<PeriodEngine>(*c, (uint32_t) io->period_size,
                                               [p](const PeriodEngine::Period& period) { p->onPeriod(period); }, io->rate);
    return 0;
}

int pcmSwParams(snd_pcm_ioplug_t* io, snd_pcm_sw_params_t* params)
{
    InletPcm* p = self(io);
    snd_pcm_uframes_t min = 1;
    snd_pcm_sw_params_get_avail_min(params, &min);
    p->availMin = std::max<snd_pcm_uframes_t>(min, 1);
    return 0;
}

int pcmPrepare(snd_pcm_ioplug_t* io)
{
    InletPcm* p = self(io);
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    if (snd_pcm_sw_params_current(io->pcm, sw) == 0)
    {
        snd_pcm_sw_params_get_boundary(sw, &p->boundary);
        snd_pcm_uframes_t min = 1;
        snd_pcm_sw_params_get_avail_min(sw, &min);
        p->availMin = std::max<snd_pcm_uframes_t>(min, 1);
    }
    p->hwTotal = 0;
    char buf[64];
    while (::recv(p->fds[1], buf, sizeof buf, MSG_DONTWAIT) > 0)
    {
    }
    if (p->playback)
        (void) !::write(p->fds[0], buf, 1);   // room to write at once
    return 0;
}

int pcmPollRevents(snd_pcm_ioplug_t* io, struct pollfd* pfds, unsigned nfds, unsigned short* revents)
{
    InletPcm* p = self(io);
    if (nfds != 1 || revents == nullptr)
        return -EINVAL;
    char buf[64];
    while (::recv(pfds[0].fd, buf, sizeof buf, MSG_DONTWAIT) > 0)
    {
    }
    *revents = pfds[0].revents & ~(POLLIN | POLLOUT);
    if (io->state == SND_PCM_STATE_DISCONNECTED)
        *revents |= POLLERR;
    else if ((pfds[0].revents & POLLIN) && p->avail() >= p->availMin)
        *revents |= p->playback ? POLLOUT : POLLIN;
    else if (io->state != SND_PCM_STATE_RUNNING && io->state != SND_PCM_STATE_DRAINING)
        *revents |= p->playback ? POLLOUT : POLLIN;
    return 0;
}

int pcmDelay(snd_pcm_ioplug_t* io, snd_pcm_sframes_t* delay)
{
    InletPcm* p = self(io);
    DeviceClient* c = p->cur();
    const snd_pcm_uframes_t queued = io->buffer_size - std::min(p->avail(), (snd_pcm_uframes_t) io->buffer_size);
    if (p->playback)
        *delay = (snd_pcm_sframes_t) (queued + 2 * io->period_size + p->audio->outputExtraLatency());
    else
        *delay = (snd_pcm_sframes_t) (p->avail() + io->period_size + p->audio->inputLatency(c->linkOffsetFrames()));
    return 0;
}

int pcmClose(snd_pcm_ioplug_t* io)
{
    delete self(io);
    return 0;
}

const snd_pcm_ioplug_callback_t kCallbacks = [] {
    snd_pcm_ioplug_callback_t cb {};
    cb.start = pcmStart;
    cb.stop = pcmStop;
    cb.pointer = pcmPointer;
    cb.close = pcmClose;
    cb.hw_params = pcmHwParams;
    cb.sw_params = pcmSwParams;
    cb.prepare = pcmPrepare;
    cb.poll_revents = pcmPollRevents;
    cb.delay = pcmDelay;
    return cb;
}();

} // namespace

extern "C" {

SND_PCM_PLUGIN_DEFINE_FUNC(inlet)
{
    (void) root;
    long first = 0;
    snd_config_iterator_t i, next;
    snd_config_for_each(i, next, conf)
    {
        snd_config_t* n = snd_config_iterator_entry(i);
        const char* id = nullptr;
        if (snd_config_get_id(n, &id) < 0)
            continue;
        if (!std::strcmp(id, "comment") || !std::strcmp(id, "type") || !std::strcmp(id, "hint"))
            continue;
        if (!std::strcmp(id, "first"))
        {
            if (snd_config_get_integer(n, &first) < 0 || first < 0)
            {
                SNDERR("Invalid value for %s", id);
                return -EINVAL;
            }
            continue;
        }
        SNDERR("Unknown field %s", id);
        return -EINVAL;
    }

    auto* p = new InletPcm();
    p->playback = stream == SND_PCM_STREAM_PLAYBACK;
    p->first = (int) first;
    DeviceClient::Options opt;
    opt.kind = shm::ClientKind::Alsa;
    opt.app = programName();
    std::string err;
    auto c = DeviceClient::attach(opt, err);
    if (!c)
    {
        SNDERR("Inlet: %s", err.c_str());
        delete p;
        return -ENODEV;
    }
    const int available = (int) (p->playback ? c->outputs() : c->inputs()) - p->first;
    if (available < 1)
    {
        SNDERR("Inlet: no device channels from %ld on", first + 1);
        delete p;
        return -EINVAL;
    }
    c->heartbeat(monoNowNs(), false, 0);
    p->clients.push_back(std::move(c));
    p->client.store(p->clients.back().get());

    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, p->fds) != 0)
    {
        delete p;
        return -errno;
    }
    p->io.version = SND_PCM_IOPLUG_VERSION;
    p->io.name = "Inlet AES67 virtual sound card";
    p->io.mmap_rw = 1;
    p->io.callback = &kCallbacks;
    p->io.private_data = p;
    p->io.poll_fd = p->fds[1];
    p->io.poll_events = POLLIN;
    int e = snd_pcm_ioplug_create(&p->io, name, stream, mode);
    if (e < 0)
    {
        delete p;
        return e;
    }

    const unsigned access[] = { SND_PCM_ACCESS_RW_INTERLEAVED, SND_PCM_ACCESS_MMAP_INTERLEAVED };
    const unsigned formats[] = { SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_FLOAT_LE, SND_PCM_FORMAT_S16_LE };
    std::vector<unsigned> rates(std::begin(kRates), std::end(kRates));
    const unsigned device = p->cur()->sampleRate();
    if (std::find(rates.begin(), rates.end(), device) == rates.end())
        rates.push_back(device);
    std::sort(rates.begin(), rates.end());
    if ((e = snd_pcm_ioplug_set_param_list(&p->io, SND_PCM_IOPLUG_HW_ACCESS, 2, access)) < 0
        || (e = snd_pcm_ioplug_set_param_list(&p->io, SND_PCM_IOPLUG_HW_FORMAT, 3, formats)) < 0
        || (e = snd_pcm_ioplug_set_param_minmax(&p->io, SND_PCM_IOPLUG_HW_CHANNELS, 1, (unsigned) available)) < 0
        || (e = snd_pcm_ioplug_set_param_list(&p->io, SND_PCM_IOPLUG_HW_RATE, (unsigned) rates.size(), rates.data())) < 0
        || (e = snd_pcm_ioplug_set_param_minmax(&p->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 32 * 2, 8192 * 4 * available)) < 0
        || (e = snd_pcm_ioplug_set_param_minmax(&p->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 64)) < 0)
    {
        snd_pcm_ioplug_delete(&p->io);
        return e;
    }
    *pcmp = p->io.pcm;
    return 0;
}

SND_PCM_PLUGIN_SYMBOL(inlet);

} // extern "C"
