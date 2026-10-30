# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# The ALSA plugin behind "pcm.inlet": libasound_module_pcm_inlet.so, which
# alsa-lib loads for `type inlet`, and the configuration that names it.

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_get_variable(ALSA_LIBDIR alsa libdir)
set(INLET_ALSA_PLUGIN_DIR "${ALSA_LIBDIR}/alsa-lib" CACHE PATH "Where alsa-lib looks for its plugins")

add_library(inlet_alsa MODULE InletPcm.cpp)
set_target_properties(inlet_alsa PROPERTIES OUTPUT_NAME asound_module_pcm_inlet PREFIX lib)
target_link_libraries(inlet_alsa PRIVATE inlet_core PkgConfig::ALSA)
target_compile_options(inlet_alsa PRIVATE -Wall -Wextra)
# alsa-lib's plugin macros take PIC to mean "built as a shared object".
target_compile_definitions(inlet_alsa PRIVATE PIC)
# Programs of every kind load it: only alsa-lib's entry point is visible, so
# Inlet's own code (and the resampler it links) never meets theirs.
target_link_options(inlet_alsa PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map" -Wl,--no-undefined)
set_target_properties(inlet_alsa PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/exports.map")

install(TARGETS inlet_alsa LIBRARY DESTINATION "${INLET_ALSA_PLUGIN_DIR}" COMPONENT inlet)
# alsa-lib reads /etc/alsa/conf.d on every distribution (its own alsa.conf).
include(GNUInstallDirs)
install(FILES 50-inlet.conf DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/alsa/conf.d" COMPONENT inlet)
