# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# inlet_asio: the ASIO driver, an in-process COM DLL. Built with the Steinberg
# ASIO SDK's interface headers under the SDK's GPLv3 option (third_party/asiosdk).

add_library(inlet_asio SHARED
    DllMain.cpp
    InletAsio.cpp
    inlet_asio.def
)
target_include_directories(inlet_asio PRIVATE ${PROJECT_SOURCE_DIR}/third_party/asiosdk/common)
target_link_libraries(inlet_asio PRIVATE inlet_core winmm shell32 ole32 advapi32)
target_compile_definitions(inlet_asio PRIVATE _CRT_SECURE_NO_WARNINGS)
