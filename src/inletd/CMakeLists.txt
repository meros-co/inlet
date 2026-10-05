# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# inletd: the per-machine process that owns the network -- PTP, the AES67
# streams, SAP, NMOS -- for Inlet's drivers and app.

add_library(inletd_lib STATIC
    Config.cpp
    Engine.cpp
    HttpClient.cpp
    Ids.cpp
    NetInfo.cpp
    NmosNode.cpp
    Sap.cpp
    StreamManager.cpp
)
target_include_directories(inletd_lib PUBLIC .)
target_link_libraries(inletd_lib PUBLIC inlet_core manifold::mdns nlohmann_json::nlohmann_json httplib::httplib)
if(TARGET inlet_ptp)
    target_link_libraries(inletd_lib PUBLIC inlet_ptp)
    target_compile_definitions(inletd_lib PRIVATE INLET_HAVE_PTP=1)
endif()
if(WIN32)
    target_link_libraries(inletd_lib PUBLIC iphlpapi)
    target_compile_definitions(inletd_lib PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(inletd main.cpp)
target_link_libraries(inletd PRIVATE inletd_lib)
if(WIN32)
    target_link_libraries(inletd PRIVATE advapi32)
endif()
