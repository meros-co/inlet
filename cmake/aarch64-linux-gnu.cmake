# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Cross-compiling Inlet for 64-bit ARM Linux on an x86-64 Debian or Ubuntu
# machine with crossbuild-essential-arm64 and the :arm64 development packages
# (packaging/linux/docker/Dockerfile.debian12-arm64):
#
#   cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake ...

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
# The arm64 packages' pkg-config files, not this machine's, through the cross
# wrapper. (Not PKG_CONFIG_LIBDIR: JUCE clears it to build its own tool.)
set(PKG_CONFIG_EXECUTABLE aarch64-linux-gnu-pkg-config CACHE FILEPATH "")
