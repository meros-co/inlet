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
inlet_test(test_streams inletd_lib)
inlet_test(test_device)

if(WIN32)
    add_executable(test_asio_driver test_asio_driver.cpp)
    target_include_directories(test_asio_driver PRIVATE ${PROJECT_SOURCE_DIR}/third_party/asiosdk/common)
    target_link_libraries(test_asio_driver PRIVATE inlet_core ole32)
    add_dependencies(test_asio_driver inlet_asio)
    add_test(NAME test_asio_driver COMMAND test_asio_driver $<TARGET_FILE:inlet_asio>)
endif()
