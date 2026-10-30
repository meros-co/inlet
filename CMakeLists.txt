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

if(WIN32 OR APPLE)
    option(INLET_BUILD_APP "Build the Inlet app (JUCE)" ON)
else()
    option(INLET_BUILD_APP "Build the Inlet app (JUCE)" OFF)
endif()

include(CTest)
include(cmake/Dependencies.cmake)

# The shared AES67 and mDNS libraries (meros-co/aes67), a git submodule:
# `git submodule update --init`. Targets meros::aes67 and meros::mdns.
add_subdirectory(third_party/meros-aes67)

add_subdirectory(src/core)

# PTP: linuxptp's ptp4l on Linux, GStreamer's GstPtpClock elsewhere (and on
# Linux too, with -DINLET_PTP_BACKEND=gstreamer).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(INLET_PTP_BACKEND "linuxptp" CACHE STRING "PTP follower: linuxptp or gstreamer")
else()
    set(INLET_PTP_BACKEND "gstreamer" CACHE STRING "PTP follower: gstreamer")
endif()
if(INLET_PTP_BACKEND STREQUAL "linuxptp")
    add_subdirectory(src/ptp)
else()
    include(cmake/GStreamerNet.cmake)
    if(GSTNET_FOUND)
        add_subdirectory(src/ptp)
    else()
        message(WARNING "GStreamer (gstreamer-net-1.0 >= 1.24) not found: inletd needs it for PTP")
    endif()
endif()

add_subdirectory(src/inletd)
if(WIN32)
    add_subdirectory(src/asio)
endif()
if(INLET_BUILD_APP)
    add_subdirectory(src/app)
endif()
add_subdirectory(tools)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
