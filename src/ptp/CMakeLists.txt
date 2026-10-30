# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# inlet_ptp: the PTP follower. linuxptp's ptp4l (read through its management
# socket) on Linux; GStreamer's GstPtpClock elsewhere.

if(INLET_PTP_BACKEND STREQUAL "linuxptp")
    add_library(inlet_ptp STATIC src/PtpFollowerLinuxptp.cpp src/Pmc.cpp src/PtpStatus.cpp)
    target_include_directories(inlet_ptp PUBLIC include PRIVATE src)
    target_link_libraries(inlet_ptp PUBLIC inlet_core)
else()
    add_library(inlet_ptp STATIC src/PtpFollower.cpp src/PtpStatus.cpp)
    target_include_directories(inlet_ptp PUBLIC include)
    target_link_libraries(inlet_ptp PUBLIC inlet_core PRIVATE Inlet::GStreamerNet)
endif()
