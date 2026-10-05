# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.

function(inlet_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE inlet_core ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

inlet_test(test_clock)
inlet_test(test_netrx)
inlet_test(test_nettx)
inlet_test(test_discipline)
inlet_test(test_ratebridge)
inlet_test(test_nmos inletd_lib)
inlet_test(test_device)

