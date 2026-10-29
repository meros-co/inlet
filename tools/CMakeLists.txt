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

if(APPLE)
    add_executable(inlet-coreaudio-loopback coreaudio_loopback.cpp)
    target_link_libraries(inlet-coreaudio-loopback PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
endif()
