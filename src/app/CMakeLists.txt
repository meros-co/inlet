# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# The Inlet app: inletd's settings UI, with a tray icon. JUCE 8.

juce_add_gui_app(InletApp
    PRODUCT_NAME "Inlet"
    COMPANY_NAME "Meros Inc."
    VERSION ${PROJECT_VERSION}
    BUNDLE_ID co.meros.inlet)

juce_generate_juce_header(InletApp)

target_sources(InletApp PRIVATE Main.cpp InletdClient.cpp Pages.cpp)

# The ASIO Compatible logo, for the About page (Steinberg's usage guidelines).
juce_add_binary_data(InletAppData SOURCES
    ${PROJECT_SOURCE_DIR}/third_party/asiosdk/logo/ASIO-compatible-logo-Steinberg-TM-BW.png)

target_compile_definitions(InletApp PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:InletApp,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:InletApp,JUCE_VERSION>"
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(InletApp PRIVATE
    InletAppData
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
