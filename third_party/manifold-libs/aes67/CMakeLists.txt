# libaes67 — AES67 / ST 2110-30 RTP receive and transmit.
#
# A manifold platform library: portable C11, no JUCE. Migrated in from eddy
# (host/net librx/libtx, 2026-09-23) when the engine grew a network-audio
# path to consume it -- the migration rule in plans/PlatformArchitecture.md:
# shared code lives in manifold and flows outward to devices. Two consumers:
#
#   - the engine's Aes67IoProvider (workstream W9), which routes a received
#     stream as if it were a local input and sends the console's outputs.
#   - eddy, whose transmit path (built-in mic, mix, aux) is what makes its
#     outputs subscribable from a manifold system.
#
# The packet arithmetic (rtp.c) is identical on every platform and is what
# the tests exercise exhaustively. The socket paths differ: Linux keeps the
# recvmmsg/epoll/SO_TIMESTAMPNS path the eddy bench measured; everything else
# gets poll + recvfrom, which is right for a developer's desk and not the
# path a show runs on.

add_library(manifold_aes67 STATIC
    src/rtp.c
    src/sdp.c
    src/rx.c
    src/tx.c
)
add_library(manifold::aes67 ALIAS manifold_aes67)

target_include_directories(manifold_aes67 PUBLIC include)
set_target_properties(manifold_aes67 PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
    target_compile_definitions(manifold_aes67 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(WIN32)
    target_link_libraries(manifold_aes67 PUBLIC ws2_32)
else()
    target_compile_options(manifold_aes67 PRIVATE -Wall -Wextra)
    find_package(Threads REQUIRED)
    target_link_libraries(manifold_aes67 PUBLIC Threads::Threads)
endif()

if(BUILD_TESTING)
    add_executable(test_aes67 tests/test_aes67.c)
    target_link_libraries(test_aes67 PRIVATE manifold_aes67)
    if(MSVC)
        target_compile_definitions(test_aes67 PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME aes67 COMMAND test_aes67)
endif()
