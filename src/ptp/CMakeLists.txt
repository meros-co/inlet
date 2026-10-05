# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# inlet_ptp: the PTP follower, GStreamer's GstPtpClock.

add_library(inlet_ptp STATIC src/PtpFollower.cpp)
target_include_directories(inlet_ptp PUBLIC include)
target_link_libraries(inlet_ptp PUBLIC inlet_core PRIVATE Inlet::GStreamerNet)
