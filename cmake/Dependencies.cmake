# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Third-party sources fetched at configure time, pinned by commit.

include(FetchContent)

# libsamplerate 0.2.2 (BSD-2-Clause): the variable-ratio resampler behind
# RateBridge. Its own tests, examples and install rules stay out of Inlet's build.
FetchContent_Declare(libsamplerate
    GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
    GIT_TAG c96f5e3de9c4488f4e6c97f59f5245f22fda22f7)
set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
set(_inlet_build_testing ${BUILD_TESTING})
set(BUILD_TESTING OFF)
# It declares cmake_minimum_required(3.1), which CMake 4 no longer accepts.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(libsamplerate)
unset(CMAKE_POLICY_VERSION_MINIMUM)
set(BUILD_TESTING ${_inlet_build_testing})
# Linked into shared modules (the ALSA plugin) too, where compilers that do not
# make position-independent code by default (Rocky Linux 9's) need it.
set_target_properties(samplerate PROPERTIES POSITION_INDEPENDENT_CODE ON)

# nlohmann/json 3.11.3 (MIT): the control plane's JSON, as in Manifold.
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# cpp-httplib 0.18.7 (MIT): the HTTP server for NMOS and the Inlet API, and the
# client for registration and peers. Header-only.
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG 5814e121dfb5049f72a5c3956c3c8961b40da78b)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

# JUCE 8.0.4 (AGPLv3 option): the Inlet app's UI. Only the app needs it.
if(INLET_BUILD_APP)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()
