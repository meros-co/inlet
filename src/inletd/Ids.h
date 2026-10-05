// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

// Identifiers: MD5 (RFC 1321), name-derived version-3 UUIDs, random version-4
// UUIDs. NMOS correlates everything by id, so a resource's id must be the
// same on every boot and every build: Inlet derives them from stored ids.

#include <array>
#include <cstdint>
#include <string>

namespace inlet
{

std::array<uint8_t, 16> md5(const std::string& data);
std::string md5Hex(const std::string& data);

/** MD5 of `name` laid out as a version-3 UUID (lower case, dashed). */
std::string derivedUuid(const std::string& name);

/** A random version-4 UUID. */
std::string randomUuid();

} // namespace inlet
