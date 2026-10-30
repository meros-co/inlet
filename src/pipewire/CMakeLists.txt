# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# inlet-pipewire: Inlet as a PipeWire sink and source, driven by the PTP media
# clock, run for each user by a systemd user service that starts with PipeWire.

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

add_executable(inlet-pipewire main.cpp)
target_link_libraries(inlet-pipewire PRIVATE inlet_core PkgConfig::PIPEWIRE)
target_compile_options(inlet-pipewire PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

install(TARGETS inlet-pipewire RUNTIME DESTINATION bin COMPONENT inlet)
