# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# GStreamer's network library (libgstnet), for its PTP clock. Defines the
# imported target Inlet::GStreamerNet when found.
#
# pkg-config where there is one (macOS, Linux); on Windows the official MSVC
# runtime+development install, found through GSTREAMER_1_0_ROOT_MSVC_X86_64
# (set by its installer) or its default location.

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GSTNET QUIET IMPORTED_TARGET gstreamer-net-1.0>=1.24)
    if(GSTNET_FOUND)
        add_library(Inlet::GStreamerNet ALIAS PkgConfig::GSTNET)
        return()
    endif()
endif()

if(WIN32)
    set(GSTREAMER_ROOT "$ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64}" CACHE PATH "GStreamer MSVC install root")
    if(NOT GSTREAMER_ROOT)
        set(GSTREAMER_ROOT "C:/Program Files/gstreamer/1.0/msvc_x86_64" CACHE PATH "GStreamer MSVC install root" FORCE)
    endif()
    file(TO_CMAKE_PATH "${GSTREAMER_ROOT}" GSTREAMER_ROOT)
    if(EXISTS "${GSTREAMER_ROOT}/include/gstreamer-1.0/gst/net/gstptpclock.h")
        add_library(Inlet::GStreamerNet INTERFACE IMPORTED)
        target_include_directories(Inlet::GStreamerNet INTERFACE
            "${GSTREAMER_ROOT}/include/gstreamer-1.0"
            "${GSTREAMER_ROOT}/include/glib-2.0"
            "${GSTREAMER_ROOT}/lib/glib-2.0/include")
        target_link_libraries(Inlet::GStreamerNet INTERFACE
            "${GSTREAMER_ROOT}/lib/gstnet-1.0.lib"
            "${GSTREAMER_ROOT}/lib/gstreamer-1.0.lib"
            "${GSTREAMER_ROOT}/lib/gobject-2.0.lib"
            "${GSTREAMER_ROOT}/lib/glib-2.0.lib")
        set(GSTNET_FOUND TRUE)
    endif()
endif()
