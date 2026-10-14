// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "inlet/SharedRegion.h"

#include <aes67/rtp.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <sddl.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace inlet::shm
{

namespace
{

constexpr size_t align64(size_t n) { return (n + 63) & ~(size_t) 63; }

constexpr const char* kDefaultRegion = "Inlet.Shared";
constexpr const char* kDirectoryName = "Inlet.Directory";

/** The directory's name: Inlet's own, or INLET_DIRECTORY's, which lets a test
    run a private inletd and driver beside an installed Inlet. */
std::string directoryName(const char* given)
{
    if (given != nullptr)
        return given;
#if defined(_WIN32)
    char env[128] = {};
    if (GetEnvironmentVariableA("INLET_DIRECTORY", env, sizeof env) > 0)
        return env;
#else
    if (const char* env = std::getenv("INLET_DIRECTORY"); env != nullptr && *env != '\0')
        return env;
#endif
    return kDirectoryName;
}
constexpr uint32_t kDirectoryMagic = 0x52444C49;   // "ILDR"

struct Offsets
{
    size_t inputMap, clients, inputLabels, outputLabels, rxRings, txRings, total;
};

Offsets offsetsFor(const Region::Layout& l)
{
    Offsets o {};
    size_t at = align64(sizeof(Header));
    o.inputMap = at;
    at = align64(at + sizeof(std::atomic<uint32_t>) * l.inputs);
    o.clients = at;
    at = align64(at + sizeof(ClientSlot) * kMaxClients);
    o.inputLabels = at;
    at = align64(at + (size_t) kLabelBytes * l.inputs);
    o.outputLabels = at;
    at = align64(at + (size_t) kLabelBytes * l.outputs);
    o.rxRings = at;
    at = align64(at + sizeof(aes67_ring_t) * l.receivers * l.receiverChannels);
    o.txRings = at;
    at = align64(at + sizeof(aes67_ring_t) * l.outputs * kOutputLayers);
    o.total = at;
    return o;
}

#if defined(_WIN32)

std::wstring widen(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

/** A new named mapping, under Global\ (the service may) or Local\ (a user
    session may). `existed`: the name was taken, which callers treat as an
    error. */
HANDLE createMapping(const std::string& base, size_t size, bool& existed, std::string& error)
{
    // The service runs as SYSTEM; DAWs run as the user. Everyone signed in may
    // read and write (it holds audio and labels, nothing secret).
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof sa;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)", SDDL_REVISION_1, &sd, nullptr))
        sa.lpSecurityDescriptor = sd;
    HANDLE h = nullptr;
    existed = false;
    for (const char* prefix : { "Global\\", "Local\\" })
    {
        h = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, (DWORD) ((uint64_t) size >> 32),
                               (DWORD) (size & 0xFFFFFFFFu), widen(prefix + base).c_str());
        if (h != nullptr)
        {
            existed = GetLastError() == ERROR_ALREADY_EXISTS;
            break;
        }
    }
    if (sd != nullptr)
        LocalFree(sd);
    if (h == nullptr)
        error = "CreateFileMapping: error " + std::to_string(GetLastError());
    else if (existed)
    {
        CloseHandle(h);
        h = nullptr;
    }
    return h;
}

HANDLE openMapping(const std::string& base)
{
    for (const char* prefix : { "Global\\", "Local\\" })
        if (HANDLE h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, widen(prefix + base).c_str()))
            return h;
    return nullptr;
}

#endif

} // namespace

std::string uniqueRegionName()
{
    static std::atomic<uint32_t> n { 0 };
#if defined(_WIN32)
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = (unsigned long) getpid();
#endif
    return "Inlet.Shared." + std::to_string(pid) + "." + std::to_string(++n);
}

size_t Region::bytesFor(const Layout& layout)
{
    return offsetsFor(layout).total;
}

std::unique_ptr<Region> Region::create(const Layout& l, std::string& error, const char* nameIn)
{
    const std::string name = nameIn != nullptr ? nameIn : kDefaultRegion;
    const Offsets o = offsetsFor(l);
    std::unique_ptr<Region> r(new Region());
    r->size = o.total;
    void* base = nullptr;

#if defined(_WIN32)
    bool existed = false;
    HANDLE h = createMapping(name, o.total, existed, error);
    if (h == nullptr)
    {
        if (existed)
            error = "the shared region " + name + " exists already (is another inletd running?)";
        return nullptr;
    }
    base = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, o.total);
    if (base == nullptr)
    {
        error = "MapViewOfFile: error " + std::to_string(GetLastError());
        CloseHandle(h);
        return nullptr;
    }
    r->mapping = h;
#else
    r->posixName = "/" + name;
    r->fd = shm_open(r->posixName.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
    if (r->fd < 0)
    {
        error = "the shared region " + name + " exists already (is another inletd running?)";
        return nullptr;
    }
    r->owner = true;
    if (ftruncate(r->fd, (off_t) o.total) != 0)
    {
        error = "ftruncate failed";
        return nullptr;
    }
    fchmod(r->fd, 0666);
    base = mmap(nullptr, o.total, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, 0);
    if (base == MAP_FAILED)
    {
        error = "mmap failed";
        return nullptr;
    }
#endif

    std::memset(base, 0, o.total);
    auto* head = new (base) Header();
    head->layoutVersion = kLayoutVersion;
    head->serial = ((uint64_t) std::random_device {}() << 32) ^ (uint64_t) std::chrono::steady_clock::now().time_since_epoch().count();
    head->totalBytes = o.total;
    head->sampleRate = l.sampleRate;
    head->inputs = l.inputs;
    head->outputs = l.outputs;
    head->receivers = l.receivers;
    head->receiverChannels = l.receiverChannels;
    head->inputMapOffset = o.inputMap;
    head->clientsOffset = o.clients;
    head->inputLabelsOffset = o.inputLabels;
    head->outputLabelsOffset = o.outputLabels;
    head->rxRingsOffset = o.rxRings;
    head->txRingsOffset = o.txRings;
    r->hdr = head;

    auto* bytes = static_cast<uint8_t*>(base);
    for (uint32_t i = 0; i < l.inputs; ++i)
        new (bytes + o.inputMap + i * sizeof(std::atomic<uint32_t>)) std::atomic<uint32_t>(0);
    for (int i = 0; i < kMaxClients; ++i)
        new (bytes + o.clients + i * sizeof(ClientSlot)) ClientSlot();
    for (uint32_t i = 0; i < l.receivers * l.receiverChannels; ++i)
        aes67_ring_init(reinterpret_cast<aes67_ring_t*>(bytes + o.rxRings) + i);
    for (uint32_t i = 0; i < l.outputs * kOutputLayers; ++i)
        aes67_ring_init(reinterpret_cast<aes67_ring_t*>(bytes + o.txRings) + i);

    // Published last: a driver that sees the magic sees everything above.
    std::atomic_thread_fence(std::memory_order_release);
    reinterpret_cast<std::atomic<uint32_t>*>(&head->magic)->store(kMagic, std::memory_order_release);
    return r;
}

std::unique_ptr<Region> Region::open(std::string& error, const char* nameIn)
{
    std::string name;
    if (nameIn != nullptr)
        name = nameIn;
    else if (!Directory::current(name, error))
        return nullptr;

    std::unique_ptr<Region> r(new Region());
    void* base = nullptr;
#if defined(_WIN32)
    HANDLE h = openMapping(name);
    if (h == nullptr)
    {
        error = "Inlet's service is not running";
        return nullptr;
    }
    // The header first, for the size; then the whole region.
    void* head = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Header));
    if (head == nullptr)
    {
        error = "MapViewOfFile: error " + std::to_string(GetLastError());
        CloseHandle(h);
        return nullptr;
    }
    const uint64_t total = static_cast<Header*>(head)->totalBytes;
    UnmapViewOfFile(head);
    base = total ? MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, (SIZE_T) total) : nullptr;
    if (base == nullptr)
    {
        error = "the shared region is not ready";
        CloseHandle(h);
        return nullptr;
    }
    r->mapping = h;
    r->size = (size_t) total;
#else
    r->posixName = "/" + name;
    r->fd = shm_open(r->posixName.c_str(), O_RDWR, 0);
    if (r->fd < 0)
    {
        error = "Inlet's service is not running";
        return nullptr;
    }
    struct stat st {};
    fstat(r->fd, &st);
    base = mmap(nullptr, (size_t) st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, 0);
    if (base == MAP_FAILED || st.st_size < (off_t) sizeof(Header))
    {
        error = "the shared region is not ready";
        return nullptr;
    }
    r->size = (size_t) st.st_size;
#endif
    r->hdr = static_cast<Header*>(base);
    const uint32_t magic = reinterpret_cast<std::atomic<uint32_t>*>(&r->hdr->magic)->load(std::memory_order_acquire);
    if (magic != kMagic || r->hdr->layoutVersion != kLayoutVersion || r->hdr->totalBytes > r->size)
    {
        error = "the shared region is from another version of Inlet";
        return nullptr;
    }
    return r;
}

Region::~Region()
{
#if defined(_WIN32)
    if (hdr != nullptr)
        UnmapViewOfFile(hdr);
    if (mapping != nullptr)
        CloseHandle(mapping);
#else
    if (hdr != nullptr)
        munmap(hdr, size);
    if (fd >= 0)
        close(fd);
    if (owner)
        shm_unlink(posixName.c_str());   // a driver that has it mapped keeps its view
#endif
}

std::atomic<uint32_t>* Region::inputMap()
{
    return reinterpret_cast<std::atomic<uint32_t>*>(reinterpret_cast<uint8_t*>(hdr) + hdr->inputMapOffset);
}

ClientSlot* Region::clients()
{
    return reinterpret_cast<ClientSlot*>(reinterpret_cast<uint8_t*>(hdr) + hdr->clientsOffset);
}

char* Region::inputLabel(int channel)
{
    return reinterpret_cast<char*>(hdr) + hdr->inputLabelsOffset + (size_t) channel * kLabelBytes;
}

char* Region::outputLabel(int channel)
{
    return reinterpret_cast<char*>(hdr) + hdr->outputLabelsOffset + (size_t) channel * kLabelBytes;
}

aes67_ring* Region::rxRings(int receiver)
{
    return reinterpret_cast<aes67_ring_t*>(reinterpret_cast<uint8_t*>(hdr) + hdr->rxRingsOffset)
         + (size_t) (receiver - 1) * hdr->receiverChannels;
}

aes67_ring* Region::txRing(int layer, int output)
{
    return reinterpret_cast<aes67_ring_t*>(reinterpret_cast<uint8_t*>(hdr) + hdr->txRingsOffset)
         + (size_t) layer * hdr->outputs + output;
}

const aes67_ring* Region::inputRing(int input)
{
    if (input < 0 || (uint32_t) input >= hdr->inputs)
        return nullptr;
    const uint32_t cell = inputMap()[input].load(std::memory_order_relaxed);
    const int receiver = (int) (cell >> 16), channel = (int) (cell & 0xFFFF);
    if (receiver < 1 || (uint32_t) receiver > hdr->receivers || (uint32_t) channel >= hdr->receiverChannels)
        return nullptr;
    return rxRings(receiver) + channel;
}

/* ---- the directory -------------------------------------------------------------------- */

struct Directory::Data
{
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> seq;   // odd while the name is being written
    char region[128];
};

std::unique_ptr<Directory> Directory::create(std::string& error, const char* nameIn)
{
    const std::string name = directoryName(nameIn);
    std::unique_ptr<Directory> d(new Directory());
    void* base = nullptr;
#if defined(_WIN32)
    bool existed = false;
    HANDLE h = createMapping(name, sizeof(Data), existed, error);
    if (h == nullptr)
    {
        // Taken -- or not ours to open with full access, which also means
        // another inletd made it.
        if (HANDLE other = openMapping(name); existed || other != nullptr)
        {
            if (other != nullptr)
                CloseHandle(other);
            error = "another inletd is running";
        }
        return nullptr;
    }
    base = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Data));
    if (base == nullptr)
    {
        error = "MapViewOfFile: error " + std::to_string(GetLastError());
        CloseHandle(h);
        return nullptr;
    }
    d->mapping = h;
#else
    // A crashed inletd leaves its shm name behind; the lock file is what says
    // whether one is running.
    const std::string lockPath = "/tmp/" + name + ".lock";
    d->lockFd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0666);
    if (d->lockFd < 0 || flock(d->lockFd, LOCK_EX | LOCK_NB) != 0)
    {
        error = "another inletd is running";
        return nullptr;
    }
    d->posixName = "/" + name;
    shm_unlink(d->posixName.c_str());
    d->fd = shm_open(d->posixName.c_str(), O_CREAT | O_RDWR, 0666);
    if (d->fd < 0 || ftruncate(d->fd, sizeof(Data)) != 0)
    {
        error = "shm_open failed for the directory";
        return nullptr;
    }
    fchmod(d->fd, 0666);
    base = mmap(nullptr, sizeof(Data), PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (base == MAP_FAILED)
    {
        error = "mmap failed for the directory";
        return nullptr;
    }
#endif
    std::memset(base, 0, sizeof(Data));
    d->data = new (base) Data();
    return d;
}

Directory::~Directory()
{
    if (data != nullptr)
        data->magic.store(0);   // drivers looking now find nothing, not a stale name
#if defined(_WIN32)
    if (data != nullptr)
        UnmapViewOfFile(data);
    if (mapping != nullptr)
        CloseHandle(mapping);
#else
    if (data != nullptr)
        munmap(data, sizeof(Data));
    if (fd >= 0)
    {
        close(fd);
        shm_unlink(posixName.c_str());
    }
    if (lockFd >= 0)
        close(lockFd);   // releases the lock
#endif
}

void Directory::publish(const std::string& regionName)
{
    data->seq.fetch_add(1, std::memory_order_acq_rel);
    std::memset(data->region, 0, sizeof data->region);
    std::strncpy(data->region, regionName.c_str(), sizeof data->region - 1);
    data->seq.fetch_add(1, std::memory_order_acq_rel);
    data->magic.store(kDirectoryMagic, std::memory_order_release);
}

bool Directory::current(std::string& regionName, std::string& error, const char* nameIn)
{
    const std::string name = directoryName(nameIn);
    void* base = nullptr;
#if defined(_WIN32)
    HANDLE h = openMapping(name);
    if (h == nullptr)
    {
        error = "Inlet's service is not running";
        return false;
    }
    base = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(Data));
#else
    const int fd = shm_open(("/" + name).c_str(), O_RDONLY, 0);
    if (fd < 0)
    {
        error = "Inlet's service is not running";
        return false;
    }
    base = mmap(nullptr, sizeof(Data), PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
        base = nullptr;
#endif
    bool ok = false;
    if (base != nullptr)
    {
        auto* data = static_cast<Data*>(base);
        for (int tries = 0; tries < 1000 && !ok; ++tries)
        {
            if (data->magic.load(std::memory_order_acquire) != kDirectoryMagic)
                break;
            const uint32_t q = data->seq.load(std::memory_order_acquire);
            if (q & 1u)
                continue;
            char copy[sizeof data->region];
            std::memcpy(copy, data->region, sizeof copy);
            if (data->seq.load(std::memory_order_acquire) == q)
            {
                regionName.assign(copy, strnlen(copy, sizeof copy));
                ok = !regionName.empty();
            }
        }
    }
#if defined(_WIN32)
    if (base != nullptr)
        UnmapViewOfFile(base);
    CloseHandle(h);
#else
    if (base != nullptr)
        munmap(base, sizeof(Data));
    close(fd);
#endif
    if (!ok)
        error = "Inlet's service is starting";
    return ok;
}

} // namespace inlet::shm
