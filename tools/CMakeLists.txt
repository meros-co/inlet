# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.

if(TARGET inlet_ptp)
    add_executable(inlet-ptp-probe ptp_probe.cpp)
    target_link_libraries(inlet-ptp-probe PRIVATE inlet_ptp)
endif()
