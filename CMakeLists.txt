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

# Drivers load into other people's processes: no C runtime to install.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)
include(cmake/Dependencies.cmake)

# Shared AES67 and mDNS libraries, vendored until meros-co/aes67 is published.
add_subdirectory(third_party/manifold-libs/aes67)
add_subdirectory(third_party/manifold-libs/mdns)

add_subdirectory(src/core)

include(cmake/GStreamerNet.cmake)
if(GSTNET_FOUND)
    add_subdirectory(src/ptp)
else()
    message(WARNING "GStreamer (gstreamer-net-1.0 >= 1.24) not found: inletd needs it for PTP")
endif()

add_subdirectory(src/inletd)
if(WIN32)
    add_subdirectory(src/asio)
endif()
add_subdirectory(tools)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
