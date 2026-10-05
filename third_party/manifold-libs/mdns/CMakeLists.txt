# libmdns -- DNS-SD over multicast DNS, the device's half.
#
# A manifold platform library: portable C11, no JUCE, one socket. It exists
# because the NMOS contract (IS-04) is DNS-SD -- a node advertises
# `_nmos-node._tcp` and finds a registry by `_nmos-register._tcp` -- and a
# manifold device is an appliance whose image should not need Avahi and a
# D-Bus for its outputs to be found, while the developer's desk is Windows,
# whose own responder answers no queries on our behalf. Consumers:
#
#   - the engine's NmosNode (workstream W9.5/W9.6): advertising the node,
#     finding a registry to register with.
#   - eddy, once it publishes its outputs as NMOS senders.
#
# The packet arithmetic (mdns_build_*, mdns_parse_message, the answer and
# browse-assembly logic) has no socket in it and is what the tests pin.

add_library(manifold_mdns STATIC
    src/mdns.c
)
add_library(manifold::mdns ALIAS manifold_mdns)

target_include_directories(manifold_mdns PUBLIC include)
set_target_properties(manifold_mdns PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
    target_compile_definitions(manifold_mdns PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(WIN32)
    target_link_libraries(manifold_mdns PUBLIC ws2_32)
else()
    target_compile_options(manifold_mdns PRIVATE -Wall -Wextra)
endif()

if(BUILD_TESTING)
    add_executable(test_mdns tests/test_mdns.c)
    target_link_libraries(test_mdns PRIVATE manifold_mdns)
    if(MSVC)
        target_compile_definitions(test_mdns PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME mdns COMMAND test_mdns)
endif()
