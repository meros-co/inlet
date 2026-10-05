// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "inlet/SharedRegion.h"

#include <chrono>
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
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

// Last: libaes67's C atomics shim defines macros the standard headers trip on.
#include <aes67/rtp.h>

namespace inlet::shm
{

namespace
{

constexpr size_t align64(size_t n) { return (n + 63) & ~(size_t) 63; }

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
    at = align64(at + sizeof(aes67_ring_t) * l.outputs);
    o.total = at;
    return o;
}

#if defined(_WIN32)
const wchar_t* const kNames[] = { L"Global\\Inlet.Shared", L"Local\\Inlet.Shared" };
#else
const char* const kPosixName = "/inlet.shared";
#endif

} // namespace

size_t Region::bytesFor(const Layout& layout)
{
    return offsetsFor(layout).total;
}

std::unique_ptr<Region> Region::create(const Layout& l, std::string& error, const char* name)
{
    const Offsets o = offsetsFor(l);
    std::unique_ptr<Region> r(new Region());
    r->size = o.total;
    void* base = nullptr;

#if defined(_WIN32)
    // The service runs as SYSTEM; DAWs run as the user. Everyone signed in may
    // read and write the region (it holds audio and labels, nothing secret).
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof sa;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)", SDDL_REVISION_1, &sd,
                                                             nullptr))
        sa.lpSecurityDescriptor = sd;
    HANDLE h = nullptr;
    std::wstring wide;
    if (name != nullptr)
        wide.assign(name, name + std::strlen(name));
    for (const wchar_t* n : kNames)
    {
        const wchar_t* use = name != nullptr ? wide.c_str() : n;
        h = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, (DWORD) ((uint64_t) o.total >> 32),
                               (DWORD) (o.total & 0xFFFFFFFFu), use);
        if (h != nullptr || name != nullptr)
            break;   // Global\ needs SeCreateGlobalPrivilege; a user session falls back to Local\ .
    }
    if (sd != nullptr)
        LocalFree(sd);
    if (h == nullptr)
    {
        error = "CreateFileMapping: error " + std::to_string(GetLastError());
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
    r->posixName = name != nullptr ? name : kPosixName;
    shm_unlink(r->posixName.c_str());
    r->fd = shm_open(r->posixName.c_str(), O_CREAT | O_RDWR, 0666);
    if (r->fd < 0 || ftruncate(r->fd, (off_t) o.total) != 0)
    {
        error = "shm_open/ftruncate failed";
        return nullptr;
    }
    fchmod(r->fd, 0666);
    base = mmap(nullptr, o.total, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, 0);
    if (base == MAP_FAILED)
    {
        error = "mmap failed";
        return nullptr;
    }
    r->owner = true;
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
    for (uint32_t i = 0; i < l.outputs; ++i)
        aes67_ring_init(reinterpret_cast<aes67_ring_t*>(bytes + o.txRings) + i);

    // Published last: a driver that sees the magic sees everything above.
    std::atomic_thread_fence(std::memory_order_release);
    reinterpret_cast<std::atomic<uint32_t>*>(&head->magic)->store(kMagic, std::memory_order_release);
    return r;
}

std::unique_ptr<Region> Region::open(std::string& error, const char* name)
{
    std::unique_ptr<Region> r(new Region());
    void* base = nullptr;
#if defined(_WIN32)
    HANDLE h = nullptr;
    std::wstring wide;
    if (name != nullptr)
        wide.assign(name, name + std::strlen(name));
    for (const wchar_t* n : kNames)
    {
        h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name != nullptr ? wide.c_str() : n);
        if (h != nullptr || name != nullptr)
            break;
    }
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
    r->posixName = name != nullptr ? name : kPosixName;
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
        shm_unlink(posixName.c_str());
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

aes67_ring* Region::txRing(int output)
{
    return reinterpret_cast<aes67_ring_t*>(reinterpret_cast<uint8_t*>(hdr) + hdr->txRingsOffset) + output;
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

} // namespace inlet::shm
