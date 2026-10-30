// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "inlet/PtpFollower.h"

#include <cstdio>

namespace inlet
{

std::string PtpStatus::identityText(const uint8_t id[8])
{
    char buf[24];
    std::snprintf(buf, sizeof buf, "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", id[0], id[1], id[2], id[3], id[4],
                  id[5], id[6], id[7]);
    return buf;
}

} // namespace inlet
