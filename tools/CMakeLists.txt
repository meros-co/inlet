# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.

if(TARGET inlet_ptp)
    add_executable(inlet-ptp-probe ptp_probe.cpp)
    target_link_libraries(inlet-ptp-probe PRIVATE inlet_ptp)
endif()

if(WIN32)
    add_executable(inlet-asio-loopback asio_loopback.cpp)
    target_include_directories(inlet-asio-loopback PRIVATE ${PROJECT_SOURCE_DIR}/third_party/asiosdk/common)
    target_link_libraries(inlet-asio-loopback PRIVATE ole32)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(ALSA REQUIRED)
    find_package(Threads REQUIRED)
    add_executable(inlet-alsa-loopback alsa_loopback.cpp)
    target_link_libraries(inlet-alsa-loopback PRIVATE ALSA::ALSA Threads::Threads)
endif()
