# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# inlet_version_resource(<target> <description>): on Windows, the target's
# file properties (Details tab, the installer, SmartScreen) say what it is and
# which Inlet it came from; elsewhere nothing.

set(INLET_VERSION_RC_IN "${CMAKE_CURRENT_LIST_DIR}/version.rc.in")

function(inlet_version_resource target description)
    if(NOT WIN32)
        return()
    endif()
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "SHARED_LIBRARY")
        set(INLET_RC_FILETYPE "VFT_DLL")
    else()
        set(INLET_RC_FILETYPE "VFT_APP")
    endif()
    set(INLET_RC_DESCRIPTION "${description}")
    set(INLET_RC_FILENAME "$<TARGET_FILE_NAME:${target}>")
    set(INLET_RC_VERSION_COMMAS "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file("${INLET_VERSION_RC_IN}" "${rc}.in" @ONLY)
    file(GENERATE OUTPUT "${rc}" INPUT "${rc}.in")
    target_sources(${target} PRIVATE "${rc}")
endfunction()
