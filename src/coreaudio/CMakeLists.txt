# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Inlet.driver: the Core Audio driver (an AudioServerPlugIn), installed in
# /Library/Audio/Plug-Ins/HAL.

add_library(inlet_coreaudio MODULE InletDriver.cpp)
target_link_libraries(inlet_coreaudio PRIVATE inlet_core "-framework CoreFoundation" "-framework CoreAudio")
target_compile_options(inlet_coreaudio PRIVATE -Wall -Wextra -fvisibility=hidden)
configure_file(Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
set_target_properties(inlet_coreaudio PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION driver
    OUTPUT_NAME Inlet
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
# Core Audio needs the factory alone.
target_link_options(inlet_coreaudio PRIVATE "LINKER:-exported_symbol,_InletDriverFactory")
