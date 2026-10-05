# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# inletd: the per-machine process that owns the network -- PTP, the AES67
# streams, SAP, NMOS -- for Inlet's drivers and app.

add_library(inletd_lib STATIC
    HttpClient.cpp
    Ids.cpp
    NmosNode.cpp
)
target_include_directories(inletd_lib PUBLIC .)
target_link_libraries(inletd_lib PUBLIC inlet_core manifold::mdns nlohmann_json::nlohmann_json httplib::httplib)
