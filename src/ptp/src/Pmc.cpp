// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "Pmc.h"

#include <atomic>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace inlet::pmc
{

namespace
{

constexpr uint8_t kManagement = 0x0D;
constexpr uint16_t kTlvManagement = 0x0001;
constexpr uint16_t kTlvManagementErrorStatus = 0x0002;
constexpr size_t kHeaderBytes = 34;
constexpr size_t kTlvAt = 48;
constexpr const char* kRunDir = "/run/inlet";

uint16_t be16(const uint8_t* p) { return (uint16_t) ((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return ((uint64_t) be32(p) << 32) | be32(p + 4); }
void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t) (v >> 8);
    p[1] = (uint8_t) v;
}

/** IEEE 1588 TimeInterval: nanoseconds scaled by 2^16. */
int64_t timeIntervalNs(const uint8_t* p) { return (int64_t) be64(p) / 65536; }

int64_t nowMs()
{
    timespec t {};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t) t.tv_sec * 1000 + t.tv_nsec / 1'000'000;
}

} // namespace

const char* portStateName(uint8_t state)
{
    static const char* const names[] = { "", "INITIALIZING", "FAULTY", "DISABLED", "LISTENING", "PRE_MASTER",
                                         "MASTER", "PASSIVE", "UNCALIBRATED", "SLAVE" };
    return state < sizeof names / sizeof names[0] ? names[state] : "";
}

/** The data a GET carries: zeros the size of the answer, as linuxptp's own
    pmc sends (ptp4l does not answer a GET without them). */
static size_t getDataBytes(uint16_t id)
{
    switch (id)
    {
    case kDefaultDataSet: return 20;
    case kCurrentDataSet: return 18;
    case kParentDataSet: return 32;
    case kTimePropertiesDataSet: return 4;
    case kPortDataSet: return 26;
    case kTimeStatusNp: return 50;
    default: return 0;
    }
}

std::vector<uint8_t> encodeGet(uint16_t id, uint8_t domain, uint16_t sequence, uint16_t sourcePort)
{
    const size_t data = getDataBytes(id);
    std::vector<uint8_t> m(54 + data, 0);
    m[0] = kManagement;
    m[1] = 0x02;                     // PTP version 2
    put16(&m[2], (uint16_t) m.size());
    m[4] = domain;
    // sourcePortIdentity: no clock identity of our own, a port number to tell
    // our requests from other clients'.
    put16(&m[28], sourcePort);
    put16(&m[30], sequence);
    m[32] = 0x04;                    // controlField: management
    m[33] = 0x7F;                    // logMessageInterval: none
    std::memset(&m[34], 0xFF, 10);   // targetPortIdentity: every port
    m[44] = 0;                       // startingBoundaryHops: this clock only,
    m[45] = 0;                       // boundaryHops          as pmc asks locally
    m[46] = 0;                       // action: GET
    put16(&m[kTlvAt], kTlvManagement);
    put16(&m[kTlvAt + 2], (uint16_t) (2 + data));   // the management ID and the empty data
    put16(&m[kTlvAt + 4], id);
    return m;
}

std::optional<Response> decode(const uint8_t* b, size_t len)
{
    if (len < kTlvAt + 6 || (b[0] & 0x0F) != kManagement)
        return std::nullopt;
    Response r;
    r.domain = b[4];
    std::memcpy(r.sourceIdentity, b + 20, 8);
    r.sourcePort = be16(b + 28);
    r.sequence = be16(b + 30);
    const uint16_t type = be16(b + kTlvAt);
    const uint16_t length = be16(b + kTlvAt + 2);
    if (kTlvAt + 4 + (size_t) length > len || length < 2)
        return std::nullopt;
    if (type == kTlvManagementErrorStatus)
    {
        if (length < 4)
            return std::nullopt;
        r.error = true;
        r.id = be16(b + kTlvAt + 6);
        return r;
    }
    if (type != kTlvManagement)
        return std::nullopt;
    r.id = be16(b + kTlvAt + 4);
    r.data.assign(b + kTlvAt + 6, b + kTlvAt + 4 + length);
    return r;
}

std::optional<TimeStatus> parseTimeStatus(const std::vector<uint8_t>& d)
{
    if (d.size() < 50)
        return std::nullopt;
    TimeStatus t;
    t.masterOffsetNs = (int64_t) be64(&d[0]);
    t.ingressNs = (int64_t) be64(&d[8]);
    // cumulativeScaledRateOffset, scaledLastGmPhaseChange, gmTimeBaseIndicator
    // and lastGmPhaseChange (4 + 4 + 2 + 12 bytes) are not used.
    t.gmPresent = be32(&d[38]) != 0;
    std::memcpy(t.gmIdentity, &d[42], 8);
    return t;
}

std::optional<PortData> parsePortData(const std::vector<uint8_t>& d)
{
    if (d.size() < 20)
        return std::nullopt;
    PortData p;
    std::memcpy(p.identity, &d[0], 8);
    p.port = be16(&d[8]);
    p.state = d[10];
    p.peerMeanPathDelayNs = timeIntervalNs(&d[12]);
    return p;
}

std::optional<CurrentData> parseCurrentData(const std::vector<uint8_t>& d)
{
    if (d.size() < 18)
        return std::nullopt;
    CurrentData c;
    c.stepsRemoved = be16(&d[0]);
    c.offsetFromMasterNs = timeIntervalNs(&d[2]);
    c.meanPathDelayNs = timeIntervalNs(&d[10]);
    return c;
}

std::optional<TimeProperties> parseTimeProperties(const std::vector<uint8_t>& d)
{
    if (d.size() < 4)
        return std::nullopt;
    TimeProperties t;
    t.currentUtcOffset = (int16_t) be16(&d[0]);
    t.utcOffsetValid = (d[2] & 0x04) != 0;
    t.ptpTimescale = (d[2] & 0x08) != 0;
    return t;
}

std::optional<DefaultData> parseDefaultData(const std::vector<uint8_t>& d)
{
    if (d.size() < 20)
        return std::nullopt;
    DefaultData x;
    x.slaveOnly = (d[0] & 0x02) != 0;
    std::memcpy(x.identity, &d[10], 8);
    x.domain = d[18];
    return x;
}

std::optional<PortProperties> parsePortProperties(const std::vector<uint8_t>& d)
{
    if (d.size() < 13)
        return std::nullopt;
    PortProperties p;
    p.state = d[10];
    p.hardwareTimestamps = d[11] != 0;   // anything but TS_SOFTWARE stamps in the NIC
    const size_t n = d[12];
    if (13 + n <= d.size())
        p.interfaceName.assign((const char*) &d[13], n);
    return p;
}

std::optional<ParentData> parseParentData(const std::vector<uint8_t>& d)
{
    if (d.size() < 32)
        return std::nullopt;
    ParentData p;
    std::memcpy(p.parentIdentity, &d[0], 8);
    p.parentPort = be16(&d[8]);
    std::memcpy(p.gmIdentity, &d[24], 8);
    return p;
}

/* ---- the client ---------------------------------------------------------------------- */

Client::Client(std::string path, uint8_t dom) : socketPath(std::move(path)), domain(dom)
{
}

Client::~Client()
{
    if (fd >= 0)
        ::close(fd);
    if (!selfPath.empty())
        ::unlink(selfPath.c_str());
}

bool Client::open()
{
    if (fd >= 0)
        return true;
    fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    // An address of our own for the answers, beside Inlet's other run-time
    // files (ptp4l does not answer abstract addresses), removed on close.
    static std::atomic<uint32_t> n { 0 };
    ::mkdir(kRunDir, 0755);
    selfPath = std::string(kRunDir) + "/pmc-" + std::to_string(getpid()) + "-" + std::to_string(++n);
    ::unlink(selfPath.c_str());
    sockaddr_un self {};
    self.sun_family = AF_UNIX;
    std::strncpy(self.sun_path, selfPath.c_str(), sizeof self.sun_path - 1);
    if (::bind(fd, (sockaddr*) &self, sizeof self) != 0)
    {
        selfPath.clear();
        ::close(fd);
        fd = -1;
        return false;
    }
    return true;
}

std::optional<Response> Client::get(uint16_t id, int timeoutMs)
{
    if (!open())
        return std::nullopt;
    const uint16_t seq = ++sequence;
    const auto msg = encodeGet(id, domain, seq, (uint16_t) getpid());
    sockaddr_un to {};
    to.sun_family = AF_UNIX;
    std::strncpy(to.sun_path, socketPath.c_str(), sizeof to.sun_path - 1);
    if (::sendto(fd, msg.data(), msg.size(), 0, (sockaddr*) &to, sizeof to) != (ssize_t) msg.size())
        return std::nullopt;
    const int64_t until = nowMs() + timeoutMs;
    uint8_t buf[1500];
    for (int64_t left = timeoutMs; left > 0; left = until - nowMs())
    {
        pollfd p { fd, POLLIN, 0 };
        if (::poll(&p, 1, (int) left) <= 0)
            break;
        const ssize_t got = ::recv(fd, buf, sizeof buf, 0);
        if (got <= 0)
            continue;
        auto r = decode(buf, (size_t) got);
        if (r && r->sequence == seq && r->id == id)
            return r;   // late answers to earlier requests are dropped
    }
    return std::nullopt;
}

} // namespace inlet::pmc
