// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * The memory inletd shares with the drivers.
 *
 * inletd creates it; every driver instance (an ASIO DLL in a DAW, the Core
 * Audio plug-in in coreaudiod) maps it. Everything in it is plain data and
 * atomics -- no pointers, the same layout in every process:
 *
 *   header      magic, layout, counts, the daemon's heartbeat
 *   clock       the PTP media-clock mapping (ClockMap)
 *   input map   per device input: which receiver channel feeds it
 *   clients     one slot per attached driver: who, what, alive
 *   labels      device input and output channel names
 *   rx rings    receivers x channels, written by inletd's receive path
 *   tx rings    one per device output, written by the output-owning client
 *
 * The counts are fixed for the region's life: inletd sizes it from its
 * configuration and makes a new one (with a new name and serial) when they
 * change, and drivers that see a stale heartbeat re-attach. A driver still
 * holding the old region keeps it alive, which is why each region has a name
 * of its own: a fixed name could not be created again while it was held.
 *
 * The directory is how drivers find the current region: a small mapping with a
 * fixed name that holds the region's name. inletd holds it for as long as it
 * runs, which also makes it the guard against a second inletd. Tests rename it
 * with the INLET_DIRECTORY environment variable, so they never meet an
 * installed Inlet.
 *
 * Windows: named file mappings, under "Global\" (the service has the
 * privilege) or "Local\" when inletd runs in a user session for development.
 * POSIX (development; macOS's sandboxed plug-in gets the region over a Mach
 * service instead): shm_open, with a lock file for the single-instance guard.
 */

#include "inlet/Clock.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct aes67_ring;

namespace inlet
{

namespace shm
{

constexpr uint32_t kMagic = 0x544C4E49;   // "INLT"
constexpr uint32_t kLayoutVersion = 1;
constexpr int kMaxClients = 16;
constexpr int kLabelBytes = 64;

enum class ClientKind : uint32_t { None = 0, Asio = 1, CoreAudio = 2 };

struct alignas(64) ClientSlot
{
    std::atomic<uint32_t> state { 0 };     ///< 0 free, 1 claiming, 2 attached
    std::atomic<uint32_t> pid { 0 };
    std::atomic<uint32_t> kind { 0 };      ///< ClientKind
    std::atomic<uint32_t> running { 0 };   ///< streaming audio now
    std::atomic<uint32_t> bufferFrames { 0 };
    std::atomic<uint32_t> sampleRate { 0 };
    std::atomic<int64_t> heartbeatMono { 0 };
    std::atomic<uint64_t> periods { 0 }, lateWakeups { 0 }, resyncs { 0 };
    char app[kLabelBytes] = {};            ///< written before state goes to 2
};

struct alignas(64) Header
{
    uint32_t magic = 0;
    uint32_t layoutVersion = 0;
    uint64_t serial = 0;                   ///< new region, new serial
    uint64_t totalBytes = 0;

    uint32_t sampleRate = 48000;
    uint32_t inputs = 0, outputs = 0;      ///< device channels
    uint32_t receivers = 0, receiverChannels = 0;   ///< rx rings = receivers x receiverChannels

    // Offsets from the start of the region.
    uint64_t inputMapOffset = 0, clientsOffset = 0, inputLabelsOffset = 0, outputLabelsOffset = 0;
    uint64_t rxRingsOffset = 0, txRingsOffset = 0;

    std::atomic<int64_t> daemonHeartbeatMono { 0 };
    std::atomic<int32_t> linkOffsetFrames { 96 };   ///< inputs play this far behind the media clock
    std::atomic<uint32_t> labelsGeneration { 0 };   ///< odd while labels are being written
    std::atomic<uint32_t> outputOwner { 0 };        ///< client slot + 1 that writes the tx rings; 0 = none

    ClockMap clock;
};

/** Input map cell: receiver (1-based) << 16 | channel; 0 = silence. */
inline uint32_t mapCell(int receiver, int channel) { return receiver > 0 ? ((uint32_t) receiver << 16) | (uint32_t) channel : 0; }

/** A mapped region, whichever side opened it. */
class Region
{
public:
    struct Layout
    {
        uint32_t sampleRate = 48000;
        uint32_t inputs = 64, outputs = 64;
        uint32_t receivers = 8, receiverChannels = 8;
    };

    /** inletd: creates a region called `name` (no prefix: "Inlet.Shared.42").
        Null with the reason, also when one of that name exists already. */
    static std::unique_ptr<Region> create(const Layout& layout, std::string& error, const char* name = nullptr);
    /** Drivers: opens the region the directory names (or `name`, for tests).
        Null when inletd is not running. */
    static std::unique_ptr<Region> open(std::string& error, const char* name = nullptr);
    ~Region();

    Header& header() { return *hdr; }
    const Header& header() const { return *hdr; }
    ClockMap& clock() { return hdr->clock; }

    std::atomic<uint32_t>* inputMap();
    ClientSlot* clients();
    char* inputLabel(int channel);
    char* outputLabel(int channel);
    aes67_ring* rxRings(int receiver);   ///< 1-based: `receiverChannels` rings
    aes67_ring* txRing(int output);      ///< 0-based device output

    /** The ring a device input reads, per the map; null = silence. */
    const aes67_ring* inputRing(int input);

    static size_t bytesFor(const Layout& layout);

private:
    Region() = default;
    Header* hdr = nullptr;
    size_t size = 0;
#if defined(_WIN32)
    void* mapping = nullptr;
#else
    int fd = -1;
    std::string posixName;
    bool owner = false;
#endif
};

/** Where drivers find the current region; held by inletd while it runs. */
class Directory
{
public:
    /** inletd, once at start: null, with the reason, when another inletd holds it. */
    static std::unique_ptr<Directory> create(std::string& error, const char* name = nullptr);
    ~Directory();

    /** The region drivers should attach to from now on. */
    void publish(const std::string& regionName);

    /** Drivers: the current region's name. False when inletd is not running. */
    static bool current(std::string& regionName, std::string& error, const char* name = nullptr);

private:
    Directory() = default;
    struct Data;
    Data* data = nullptr;
#if defined(_WIN32)
    void* mapping = nullptr;
#else
    int fd = -1, lockFd = -1;
    std::string posixName;
#endif
};

} // namespace shm
} // namespace inlet

namespace inlet
{
namespace shm
{
/** A region name unique to this process and call: "Inlet.Shared.<pid>.<n>". */
std::string uniqueRegionName();

} // namespace shm
} // namespace inlet
