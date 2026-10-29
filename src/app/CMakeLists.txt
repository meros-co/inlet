# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# The Inlet app: inletd's settings UI, with a tray icon. JUCE 8.

juce_add_gui_app(InletApp
    PRODUCT_NAME "Inlet"
    COMPANY_NAME "Meros Inc."
    VERSION ${PROJECT_VERSION}
    BUNDLE_ID co.meros.inlet
    ICON_BIG ${CMAKE_CURRENT_SOURCE_DIR}/assets/app-icon-256.png
    ICON_SMALL ${CMAKE_CURRENT_SOURCE_DIR}/assets/app-icon-32.png)

juce_generate_juce_header(InletApp)

target_sources(InletApp PRIVATE
    Main.cpp Assets.cpp InletdClient.cpp InletLookAndFeel.cpp Pages.cpp Widgets.cpp)

# Inter (SIL OFL 1.1), the Phosphor icons (MIT), the app and tray icons, and
# on Windows the ASIO Compatible logo for About (Steinberg's usage guidelines).
file(GLOB INLET_APP_ASSETS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/*.png
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.ttf
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/*.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/tray/*.png)
if(WIN32)
    list(APPEND INLET_APP_ASSETS ${PROJECT_SOURCE_DIR}/third_party/asiosdk/logo/ASIO-compatible-logo-Steinberg-TM-BW.png)
endif()
juce_add_binary_data(InletAppData SOURCES ${INLET_APP_ASSETS})

target_compile_definitions(InletApp PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:InletApp,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:InletApp,JUCE_VERSION>"
    JUCE_DISPLAY_SPLASH_SCREEN=0)

if(MSVC)
    target_compile_options(InletApp PRIVATE /utf-8)
endif()

target_link_libraries(InletApp PRIVATE
    InletAppData
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
