# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# inlet_core: what every Inlet process shares -- the clock, the shared-memory
# layout, and the real-time network paths. C++20, no framework.

add_library(inlet_core STATIC
    src/Clock.cpp
    src/ClockDiscipline.cpp
    src/NetTx.cpp
    src/RateBridge.cpp
    src/RateConverter.cpp
    src/SharedRegion.cpp
    src/DeviceClient.cpp
)
target_include_directories(inlet_core PUBLIC include)
target_link_libraries(inlet_core PUBLIC meros::aes67 PRIVATE SampleRate::samplerate)
set_target_properties(inlet_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    # C4324: the shared region's alignas(64) padding is the point of it.
    target_compile_options(inlet_core PRIVATE /W4 /wd4324)
    target_compile_definitions(inlet_core PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(inlet_core PRIVATE -Wall -Wextra)
endif()
if(WIN32)
    target_link_libraries(inlet_core PUBLIC ws2_32 avrt advapi32)
    target_compile_definitions(inlet_core PUBLIC _WIN32_WINNT=0x0A00)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(inlet_core PUBLIC Threads::Threads)
    if(NOT APPLE)
        target_link_libraries(inlet_core PUBLIC rt)
    endif()
endif()
