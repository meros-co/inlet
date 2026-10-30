// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

/*
 * Inlet's PTP follower on Linux: linuxptp's ptp4l, read, never driven.
 *
 * On Linux, PTP is linuxptp's job: an AES67 machine usually runs ptp4l
 * already, and a second PTP stack on ports 319/320 would fight it. So inletd
 * follows the ptp4l on this computer when there is one (on its default socket,
 * on Inlet's domain), and otherwise runs one of its own: a slave-only,
 * free-running ptp4l on Inlet's interface and domain, which measures the
 * grandmaster's time and changes no clock.
 *
 * What ptp4l reports at each Sync (TIME_STATUS_NP) is the local clock's time
 * the Sync arrived and how far that clock is from the grandmaster then. The
 * local clock is the NIC's PTP hardware clock when ptp4l stamps in hardware,
 * the system's real-time clock when it stamps in software (on the UTC
 * timescale, so the UTC offset is added back). Whether ptp4l steers that
 * clock or leaves it free-running, PTP time = local time - offset; a line
 * fitted through the last few Syncs gives the rate, and the line is the
 * calibration ClockDiscipline takes, as with GStreamer's clock elsewhere.
 */

#include "inlet/PtpFollower.h"
#include "inlet/Clock.h"

#include "Pmc.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

namespace inlet
{

namespace
{

/** Where a ptp4l already running listens (its default uds_address). */
constexpr const char* kSystemSocket = "/var/run/ptp4l";
/** Inlet's own ptp4l, when there is none. */
constexpr const char* kOwnDir = "/run/inlet";
constexpr const char* kOwnSocket = "/run/inlet/ptp4l";
constexpr const char* kOwnConfig = "/etc/inlet/ptp4l.conf";

constexpr int kWindow = 16;                 // Syncs the line is fitted through
constexpr int64_t kStepNs = 1'000'000;      // a point this far off the line starts a new one
constexpr int kPollMs = 250;

clockid_t phcClock(int fd) { return (clockid_t) ((~(unsigned) fd << 3) | 3); }

/** The NIC's PTP hardware clock index, when it stamps in hardware; -1 if not. */
int phcIndex(const std::string& iface)
{
    const int s = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return -1;
    ethtool_ts_info info {};
    info.cmd = ETHTOOL_GET_TS_INFO;
    ifreq ifr {};
    std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    ifr.ifr_data = (char*) &info;
    const bool ok = ::ioctl(s, SIOCETHTOOL, &ifr) == 0;
    ::close(s);
    const unsigned need = SOF_TIMESTAMPING_RAW_HARDWARE | SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE;
    return ok && (info.so_timestamping & need) == need ? info.phc_index : -1;
}

std::string findPtp4l()
{
    for (const char* p : { "/usr/sbin/ptp4l", "/sbin/ptp4l", "/usr/local/sbin/ptp4l", "/usr/bin/ptp4l" })
        if (::access(p, X_OK) == 0)
            return p;
    return {};
}

int64_t readClock(clockid_t id)
{
    timespec t {};
    clock_gettime(id, &t);
    return (int64_t) t.tv_sec * 1'000'000'000LL + t.tv_nsec;
}

} // namespace

struct PtpFollower::Impl
{
    int domain = 0;
    std::string iface;
    std::unique_ptr<pmc::Client> client;
    pid_t child = -1;              // Inlet's own ptp4l, when it runs one
    std::string description;

    // The clock ptp4l stamps with.
    clockid_t localClock = CLOCK_REALTIME;
    int phcFd = -1;
    bool hardware = false;
    int utcOffset = 37;

    std::atomic<bool> running { false };
    std::thread thread;

    mutable std::mutex lock;
    PtpStatus status;
    std::deque<std::pair<int64_t, int64_t>> points;   // (ingress, PTP time), local-clock ns
    int64_t lastIngress = 0;
    double fitA = 0, fitB = 0;   // PTP - ingress = fitA + fitB * (ingress - x0)
    int64_t x0 = 0;
    bool haveFit = false;
    int64_t internalMinusMono = 0;
    bool haveOffset = false;

    ~Impl()
    {
        running = false;
        if (thread.joinable())
            thread.join();
        client.reset();
        if (child > 0)
        {
            ::kill(child, SIGTERM);
            int st = 0;
            for (int i = 0; i < 50 && ::waitpid(child, &st, WNOHANG) == 0; ++i)
                ::usleep(20'000);
            if (::waitpid(child, &st, WNOHANG) == 0)
            {
                ::kill(child, SIGKILL);
                ::waitpid(child, &st, 0);
            }
        }
        if (phcFd >= 0)
            ::close(phcFd);
    }

    bool startOwn(std::string& error)
    {
        const std::string exe = findPtp4l();
        if (exe.empty())
        {
            error = "linuxptp is not installed (no ptp4l)";
            return false;
        }
        ::mkdir(kOwnDir, 0755);
        ::unlink(kOwnSocket);
        // Slave-only and free-running: it measures, and changes no clock.
        std::vector<std::string> args = { exe, "-i", iface, "--slaveOnly", "1", "--free_running", "1",
                                          "--domainNumber", std::to_string(domain), "--uds_address", kOwnSocket,
                                          phcIndex(iface) >= 0 ? "-H" : "-S", "-m", "-q", "-l", "5" };
        if (::access(kOwnConfig, R_OK) == 0)
        {
            args.push_back("-f");
            args.push_back(kOwnConfig);
        }
        std::vector<char*> argv;
        for (auto& a : args)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        if (posix_spawn(&child, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
        {
            child = -1;
            error = "could not start ptp4l: " + std::string(std::strerror(errno));
            return false;
        }
        client = std::make_unique<pmc::Client>(kOwnSocket, (uint8_t) domain);
        // Up to five seconds for it to come up (until its socket exists, a
        // request fails at once).
        for (int i = 0; i < 50; ++i, std::this_thread::sleep_for(std::chrono::milliseconds(100)))
        {
            if (client->get(pmc::kDefaultDataSet, 100))
            {
                description = "linuxptp (Inlet's ptp4l on " + iface + ", " + (phcIndex(iface) >= 0 ? "hardware" : "software")
                            + " timestamps)";
                return true;
            }
            int st = 0;
            if (::waitpid(child, &st, WNOHANG) == child)
            {
                child = -1;
                error = "ptp4l stopped at once (is another PTP program using ports 319 and 320?)";
                return false;
            }
        }
        error = "ptp4l does not answer";
        return false;
    }

    /** Which clock ptp4l stamps with, from its port properties. */
    void learnClock()
    {
        auto r = client->get(pmc::kPortPropertiesNp);
        if (!r || r->error)
            return;
        auto pp = pmc::parsePortProperties(r->data);
        if (!pp)
            return;
        const int index = pp->hardwareTimestamps ? phcIndex(pp->interfaceName) : -1;
        if (index >= 0 && phcFd < 0)
        {
            const std::string dev = "/dev/ptp" + std::to_string(index);
            phcFd = ::open(dev.c_str(), O_RDONLY | O_CLOEXEC);
        }
        hardware = index >= 0 && phcFd >= 0;
        localClock = hardware ? phcClock(phcFd) : CLOCK_REALTIME;
        if (description.empty())
            description = "linuxptp (ptp4l on " + pp->interfaceName + ", " + (hardware ? "hardware" : "software")
                        + " timestamps)";
    }

    /** The local clock less Inlet's monotonic clock: the tightest of a few
        bracketed readings. */
    int64_t measureOffset() const
    {
        int64_t best = 0, width = INT64_MAX;
        for (int i = 0; i < 5; ++i)
        {
            const int64_t a = monoNowNs();
            const int64_t l = readClock(localClock);
            const int64_t b = monoNowNs();
            if (b - a < width)
            {
                width = b - a;
                best = l - (a + (b - a) / 2);
            }
        }
        return best;
    }

    void refit()
    {
        // Least squares of (PTP - ingress) on ingress, relative to the first
        // point: small numbers, so doubles keep nanoseconds.
        x0 = points.front().first;
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        const double n = (double) points.size();
        for (auto& [x, y] : points)
        {
            const double dx = (double) (x - x0), dy = (double) (y - x);
            sx += dx;
            sy += dy;
            sxx += dx * dx;
            sxy += dx * dy;
        }
        const double den = n * sxx - sx * sx;
        fitB = points.size() > 1 && den != 0 ? (n * sxy - sx * sy) / den : 0.0;
        fitA = (sy - fitB * sx) / n;
        haveFit = true;
    }

    double predict(int64_t x) const { return (double) x + fitA + fitB * (double) (x - x0); }

    void poll()
    {
        int tick = 0;
        while (running.load())
        {
            if (tick++ % 8 == 0)
            {
                // Slow-moving things, every two seconds.
                learnClock();
                if (auto r = client->get(pmc::kTimePropertiesDataSet); r && !r->error)
                    if (auto tp = pmc::parseTimeProperties(r->data); tp && tp->utcOffsetValid)
                        utcOffset = tp->currentUtcOffset;
                std::lock_guard<std::mutex> g(lock);
                status.reason.clear();
            }
            auto port = client->get(pmc::kPortDataSet);
            auto time = client->get(pmc::kTimeStatusNp);
            auto cur = client->get(pmc::kCurrentDataSet);
            auto parent = client->get(pmc::kParentDataSet);
            if (!port || !time)
            {
                std::lock_guard<std::mutex> g(lock);
                status.synced = false;
                status.reason = "ptp4l does not answer";
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
                continue;
            }
            const auto pd = pmc::parsePortData(port->data);
            const auto ts = pmc::parseTimeStatus(time->data);
            const auto cd = cur ? pmc::parseCurrentData(cur->data) : std::nullopt;
            const auto pa = parent ? pmc::parseParentData(parent->data) : std::nullopt;
            const int64_t offset = measureOffset();

            std::lock_guard<std::mutex> g(lock);
            internalMinusMono = offset;
            haveOffset = true;
            // Following a grandmaster: SLAVE, or UNCALIBRATED, where a
            // free-running ptp4l stays (it never locks a servo) and a steering
            // one converges. Its offsets are measured either way.
            if (pd)
                status.synced = pd->state == (uint8_t) pmc::PortState::Slave
                             || pd->state == (uint8_t) pmc::PortState::Uncalibrated;
            if (cd)
                status.meanPathDelayNs = cd->meanPathDelayNs;
            if (pa)
            {
                std::memcpy(status.masterIdentity, pa->parentIdentity, 8);
                status.masterPort = pa->parentPort;
            }
            if (ts)
            {
                status.gmPresent = ts->gmPresent;
                std::memcpy(status.gmIdentity, ts->gmIdentity, 8);
                if (status.synced && ts->gmPresent && ts->ingressNs != 0 && ts->ingressNs != lastIngress)
                {
                    // A new Sync. PTP time when it arrived; ptp4l compares a
                    // real-time clock on the UTC timescale.
                    lastIngress = ts->ingressNs;
                    int64_t ptp = ts->ingressNs - ts->masterOffsetNs;
                    if (!hardware)
                        ptp += (int64_t) utcOffset * 1'000'000'000LL;
                    if (haveFit && std::fabs(predict(ts->ingressNs) - (double) ptp) > (double) kStepNs)
                    {
                        // A new grandmaster, or the clock was stepped: a new line.
                        status.lastDiscontinuityNs = (int64_t) ((double) ptp - predict(ts->ingressNs));
                        points.clear();
                        haveFit = false;
                    }
                    points.emplace_back(ts->ingressNs, ptp);
                    while ((int) points.size() > kWindow)
                        points.pop_front();
                    refit();
                    status.updates++;
                }
                if (!status.synced || !ts->gmPresent)
                {
                    points.clear();
                    haveFit = false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        }
    }
};

std::unique_ptr<PtpFollower> PtpFollower::open(int domain, const std::vector<std::string>& interfaces, std::string& error)
{
    auto impl = std::make_unique<Impl>();
    impl->domain = domain;
    impl->iface = interfaces.empty() ? std::string() : interfaces.front();

    // The ptp4l already on this computer, if it follows Inlet's domain.
    impl->client = std::make_unique<pmc::Client>(kSystemSocket, (uint8_t) domain);
    bool system = false;
    if (auto r = impl->client->get(pmc::kDefaultDataSet, 300); r && !r->error)
        if (auto dd = pmc::parseDefaultData(r->data); dd && dd->domain == domain)
            system = true;
    if (!system)
    {
        impl->client.reset();
        if (impl->iface.empty())
        {
            error = "no network interface for PTP";
            return nullptr;
        }
        if (!impl->startOwn(error))
            return nullptr;
    }
    impl->learnClock();
    impl->status.available = true;
    impl->status.domain = domain;
    impl->status.source = "linuxptp";
    impl->status.detail = impl->description;
    impl->running = true;
    Impl* raw = impl.get();
    impl->thread = std::thread([raw] { raw->poll(); });
    return std::unique_ptr<PtpFollower>(new PtpFollower(std::move(impl)));
}

PtpFollower::PtpFollower(std::unique_ptr<Impl> i) : impl(std::move(i))
{
}

PtpFollower::~PtpFollower() = default;

bool PtpFollower::latest(PtpCalibration& cal, int64_t& internalMinusMono)
{
    std::lock_guard<std::mutex> g(impl->lock);
    if (!impl->haveFit || !impl->haveOffset || impl->points.size() < 4)
        return false;
    const int64_t x = impl->points.back().first;
    cal.synced = impl->status.synced && impl->status.gmPresent;
    cal.internalNs = x;
    cal.externalNs = (int64_t) std::llround(impl->predict(x));
    cal.rate = 1.0 + impl->fitB;
    std::memcpy(cal.gmIdentity, impl->status.gmIdentity, 8);
    cal.domain = impl->domain;
    internalMinusMono = impl->internalMinusMono;
    return true;
}

PtpStatus PtpFollower::status() const
{
    std::lock_guard<std::mutex> g(impl->lock);
    return impl->status;
}

int64_t PtpFollower::ptpNowNs() const
{
    std::lock_guard<std::mutex> g(impl->lock);
    if (!impl->haveFit)
        return -1;
    return (int64_t) std::llround(impl->predict(readClock(impl->localClock)));
}

} // namespace inlet
