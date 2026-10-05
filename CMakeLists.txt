# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Inlet: an AES67 virtual sound card for Windows and macOS.

cmake_minimum_required(VERSION 3.24)
project(Inlet VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)

# Shared AES67 and mDNS libraries, vendored until meros-co/aes67 is published.
add_subdirectory(third_party/manifold-libs/aes67)
add_subdirectory(third_party/manifold-libs/mdns)
