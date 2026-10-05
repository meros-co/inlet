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
