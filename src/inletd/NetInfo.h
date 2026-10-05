// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

#include <string>
#include <vector>

namespace inlet
{

struct NetInterface
{
    std::string name;          ///< "Ethernet", "en0"
    std::string id;            ///< what GStreamer's PTP helper is given: the adapter GUID on Windows, the name elsewhere
    std::string ipv4;          ///< dotted quad
    std::string mac;           ///< xx-xx-xx-xx-xx-xx, lower case; empty when none
    bool up = false;
    bool wireless = false;
    bool loopback = false;
    bool gateway = false;      ///< has a default gateway: the network the computer actually uses
    bool virtualSwitch = false;   ///< Hyper-V, WSL, Docker, VMware, VirtualBox: not a real network
};

std::vector<NetInterface> listInterfaces();
std::string hostName();
/** "Windows 11 (build 26200)", "macOS 15.1". */
std::string osDescription();

/** The interface to use: the configured address if present and up; else, of
    the real (not virtual-switch) interfaces with a routable address, a wired
    one with a gateway, any with a gateway, any wired one; else anything up. */
bool chooseInterface(const std::string& configuredIp, NetInterface& out);

} // namespace inlet
