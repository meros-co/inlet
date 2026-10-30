// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * A PTP management client for linuxptp's ptp4l, as its own `pmc` tool is:
 * IEEE 1588 management messages (GET only) over ptp4l's Unix datagram socket.
 *
 * Inlet reads what ptp4l knows (the offset from the grandmaster at each Sync,
 * the port state, the path delay, the UTC offset, which clock it stamps with)
 * and never changes anything in it. Encoding and decoding are socket-free and
 * tested on their own.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace inlet::pmc
{

// Management IDs (IEEE 1588-2008 table 40, and linuxptp's own).
constexpr uint16_t kDefaultDataSet = 0x2000;
constexpr uint16_t kCurrentDataSet = 0x2001;
constexpr uint16_t kParentDataSet = 0x2002;
constexpr uint16_t kTimePropertiesDataSet = 0x2003;
constexpr uint16_t kPortDataSet = 0x2004;
constexpr uint16_t kTimeStatusNp = 0xC000;
constexpr uint16_t kPortPropertiesNp = 0xC004;

/** IEEE 1588 port states. */
enum class PortState : uint8_t
{
    Initializing = 1, Faulty, Disabled, Listening, PreMaster, Master, Passive, Uncalibrated, Slave
};

const char* portStateName(uint8_t state);

/** A GET request for one management ID. */
std::vector<uint8_t> encodeGet(uint16_t id, uint8_t domain, uint16_t sequence, uint16_t sourcePort);

/** A response: which ID, the sending port, and the data after the ID. */
struct Response
{
    uint16_t id = 0;
    uint16_t sequence = 0;
    uint8_t domain = 0;
    uint8_t sourceIdentity[8] = {};
    uint16_t sourcePort = 0;
    bool error = false;          ///< a MANAGEMENT_ERROR_STATUS
    std::vector<uint8_t> data;
};
std::optional<Response> decode(const uint8_t* bytes, size_t len);

struct TimeStatus
{
    int64_t masterOffsetNs = 0;   ///< the local clock less the grandmaster's time, at `ingressNs`
    int64_t ingressNs = 0;        ///< local-clock time the last Sync arrived
    bool gmPresent = false;
    uint8_t gmIdentity[8] = {};
};
std::optional<TimeStatus> parseTimeStatus(const std::vector<uint8_t>& d);

struct PortData
{
    uint8_t identity[8] = {};
    uint16_t port = 0;
    uint8_t state = 0;
    int64_t peerMeanPathDelayNs = 0;
};
std::optional<PortData> parsePortData(const std::vector<uint8_t>& d);

struct CurrentData
{
    uint16_t stepsRemoved = 0;
    int64_t offsetFromMasterNs = 0;
    int64_t meanPathDelayNs = 0;
};
std::optional<CurrentData> parseCurrentData(const std::vector<uint8_t>& d);

struct TimeProperties
{
    int16_t currentUtcOffset = 0;
    bool utcOffsetValid = false;
    bool ptpTimescale = false;
};
std::optional<TimeProperties> parseTimeProperties(const std::vector<uint8_t>& d);

struct DefaultData
{
    uint8_t identity[8] = {};
    uint8_t domain = 0;
    bool slaveOnly = false;
};
std::optional<DefaultData> parseDefaultData(const std::vector<uint8_t>& d);

struct PortProperties
{
    uint8_t state = 0;
    bool hardwareTimestamps = false;
    std::string interfaceName;
};
std::optional<PortProperties> parsePortProperties(const std::vector<uint8_t>& d);

struct ParentData
{
    uint8_t parentIdentity[8] = {};
    uint16_t parentPort = 0;
    uint8_t gmIdentity[8] = {};
};
std::optional<ParentData> parseParentData(const std::vector<uint8_t>& d);

/** A management session with one ptp4l, over its Unix socket. */
class Client
{
public:
    /** `socketPath`: ptp4l's uds_address (its default is /var/run/ptp4l). */
    Client(std::string socketPath, uint8_t domain);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /** One GET, waiting up to `timeoutMs` for its answer (the first port's,
        when the clock has several). Empty when ptp4l does not answer. */
    std::optional<Response> get(uint16_t id, int timeoutMs = 300);

    const std::string& path() const { return socketPath; }

private:
    bool open();
    std::string socketPath;
    std::string selfPath;   // our own address, for the answers
    uint8_t domain;
    int fd = -1;
    uint16_t sequence = 0;
};

} // namespace inlet::pmc
