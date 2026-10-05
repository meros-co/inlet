// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "NetInfo.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  include <windows.h>
#else
#  include <arpa/inet.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netinet/in.h>
#  include <sys/types.h>
#  include <sys/utsname.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <net/if_dl.h>
#    include <sys/sysctl.h>
#  else
#    include <netpacket/packet.h>
#  endif
#endif

namespace inlet
{

namespace
{
std::string macText(const unsigned char* b, size_t n)
{
    if (n < 6)
        return {};
    char s[24];
    std::snprintf(s, sizeof s, "%02x-%02x-%02x-%02x-%02x-%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    return s;
}
} // namespace

#if defined(_WIN32)

std::vector<NetInterface> listInterfaces()
{
    std::vector<NetInterface> out;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
                                    addrs, &size);
    if (rc == ERROR_BUFFER_OVERFLOW)
    {
        buf.resize(size);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
                                  addrs, &size);
    }
    if (rc != NO_ERROR)
        return out;
    for (auto* a = addrs; a != nullptr; a = a->Next)
    {
        for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next)
        {
            if (u->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            NetInterface n;
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof name - 1, nullptr, nullptr);
            n.name = name;
            n.id = a->AdapterName;   // "{GUID}"
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr, ip, sizeof ip);
            n.ipv4 = ip;
            n.mac = macText(a->PhysicalAddress, a->PhysicalAddressLength);
            n.up = a->OperStatus == IfOperStatusUp;
            n.wireless = a->IfType == IF_TYPE_IEEE80211;
            n.loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
            n.gateway = a->FirstGatewayAddress != nullptr;
            char desc[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, desc, sizeof desc - 1, nullptr, nullptr);
            for (const char* v : { "Hyper-V", "Virtual", "VMware", "VirtualBox", "Docker", "WSL", "TAP-" })
                if (std::strstr(desc, v) != nullptr || std::strstr(name, v) != nullptr)
                    n.virtualSwitch = true;
            out.push_back(n);
        }
    }
    return out;
}

std::string hostName()
{
    char buf[256] = {};
    DWORD n = sizeof buf;
    if (GetComputerNameExA(ComputerNameDnsHostname, buf, &n))
        return buf;
    return "inlet";
}

std::string osDescription()
{
    // RtlGetVersion tells the truth where GetVersionEx is shimmed.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v {};
    v.dwOSVersionInfoSize = sizeof v;
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        fn(&v);
    const char* name = v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
    return std::string(name) + " (build " + std::to_string(v.dwBuildNumber) + ")";
}

#else

std::vector<NetInterface> listInterfaces()
{
    std::vector<NetInterface> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
        return out;
    for (ifaddrs* a = list; a != nullptr; a = a->ifa_next)
    {
        if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET)
            continue;
        NetInterface n;
        n.name = a->ifa_name;
        n.id = a->ifa_name;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, ip, sizeof ip);
        n.ipv4 = ip;
        n.up = (a->ifa_flags & IFF_UP) && (a->ifa_flags & IFF_RUNNING);
        n.loopback = (a->ifa_flags & IFF_LOOPBACK) != 0;
        for (ifaddrs* l = list; l != nullptr; l = l->ifa_next)
        {
            if (l->ifa_addr == nullptr || std::strcmp(l->ifa_name, a->ifa_name) != 0)
                continue;
#if defined(__APPLE__)
            if (l->ifa_addr->sa_family == AF_LINK)
            {
                auto* sdl = reinterpret_cast<sockaddr_dl*>(l->ifa_addr);
                n.mac = macText(reinterpret_cast<unsigned char*>(LLADDR(sdl)), sdl->sdl_alen);
            }
#else
            if (l->ifa_addr->sa_family == AF_PACKET)
            {
                auto* sll = reinterpret_cast<sockaddr_ll*>(l->ifa_addr);
                n.mac = macText(sll->sll_addr, sll->sll_halen);
            }
#endif
        }
        out.push_back(n);
    }
    freeifaddrs(list);
    return out;
}

std::string hostName()
{
    char buf[256] = {};
    if (gethostname(buf, sizeof buf - 1) != 0)
        return "inlet";
    std::string s = buf;
    if (auto dot = s.find('.'); dot != std::string::npos)
        s.resize(dot);
    return s;
}

std::string osDescription()
{
#if defined(__APPLE__)
    char v[64] = {};
    size_t n = sizeof v;
    if (sysctlbyname("kern.osproductversion", v, &n, nullptr, 0) == 0)
        return std::string("macOS ") + v;
    return "macOS";
#else
    utsname u {};
    uname(&u);
    return std::string(u.sysname) + " " + u.release;
#endif
}

#endif

bool chooseInterface(const std::string& configuredIp, NetInterface& out)
{
    const auto all = listInterfaces();
    if (!configuredIp.empty())
        for (auto& n : all)
            if (n.ipv4 == configuredIp && n.up)
            {
                out = n;
                return true;
            }
    // Real interfaces with a routable address first: wired with a gateway,
    // anything with a gateway (a desk on Wi-Fi), anything wired. Then anything up.
    for (int pass = 0; pass < 4; ++pass)
        for (auto& n : all)
        {
            if (!n.up || n.loopback)
                continue;
            const bool real = !n.virtualSwitch && n.ipv4.rfind("169.254.", 0) != 0;
#if defined(_WIN32)
            const bool gw = n.gateway;
#else
            const bool gw = true;   // no gateway information here
#endif
            const bool ok = pass == 0 ? real && !n.wireless && gw
                          : pass == 1 ? real && gw
                          : pass == 2 ? real && !n.wireless
                                      : true;
            if (ok)
            {
                out = n;
                return true;
            }
        }
    return false;
}

} // namespace inlet
