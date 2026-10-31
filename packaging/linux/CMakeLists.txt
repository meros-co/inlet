# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Inlet for Linux, installed and packaged: inletd and its systemd service,
# Inlet in PipeWire (a systemd user service started with PipeWire), the ALSA
# plugin, the app (in the menu, and in the tray at sign-in), Inlet's ptp4l
# configuration, the polkit rule for Start and Stop, and the licenses. One
# package, "inlet", as .deb and .rpm (cpack -G DEB, cpack -G RPM); configure
# with -DCMAKE_INSTALL_PREFIX=/usr.

include(GNUInstallDirs)

install(FILES systemd/inletd.service DESTINATION lib/systemd/system COMPONENT inlet)
install(FILES systemd/inlet-pipewire.service DESTINATION lib/systemd/user COMPONENT inlet)
install(FILES polkit/50-inlet.rules DESTINATION share/polkit-1/rules.d COMPONENT inlet)
install(FILES ptp4l.conf DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/inlet" COMPONENT inlet)
install(FILES desktop/inlet.desktop DESTINATION share/applications COMPONENT inlet)
install(FILES desktop/inlet-tray.desktop DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart" COMPONENT inlet)
foreach(size 32 64 128 256)
    install(FILES ${PROJECT_SOURCE_DIR}/src/app/assets/app-icon-${size}.png
            DESTINATION share/icons/hicolor/${size}x${size}/apps RENAME inlet.png COMPONENT inlet)
endforeach()
install(FILES THIRD-PARTY.txt DESTINATION share/doc/inlet COMPONENT inlet)
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION share/doc/inlet RENAME LICENSE.txt COMPONENT inlet)
install(FILES ${PROJECT_SOURCE_DIR}/src/app/assets/fonts/Inter-OFL.txt
              ${PROJECT_SOURCE_DIR}/src/app/assets/icons/PHOSPHOR-LICENSE.txt
        DESTINATION share/doc/inlet/licenses COMPONENT inlet)

# ---- the packages ----------------------------------------------------------------------------

set(CPACK_PACKAGE_NAME inlet)
set(CPACK_PACKAGE_VENDOR "Meros Inc.")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "AES67 virtual sound card")
set(CPACK_PACKAGE_DESCRIPTION
    "Inlet makes the computer an AES67 device: programs play into it and record from it as a sound card \
(through PipeWire, or ALSA directly), and it sends and receives AES67 streams on the network, on the PTP \
media clock (through linuxptp), announced by SAP and NMOS.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/meros-co/inlet")
set(CPACK_PACKAGE_CONTACT "Meros Inc.")
set(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/LICENSE)
set(CPACK_GENERATOR "DEB;RPM" CACHE STRING "Package formats")
set(CPACK_STRIP_FILES ON)

# Only Inlet's own files: the libraries it fetches have install rules of their own.
set(CPACK_COMPONENTS_ALL inlet)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION sound)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# The app opens its X11 libraries at run time, so they are named here.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "linuxptp, libx11-6, libxext6, libxrandr2, libxinerama1, libxcursor1")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pipewire, wireplumber")
configure_file(scripts/conffiles.in ${CMAKE_CURRENT_BINARY_DIR}/conffiles @ONLY)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/deb/postinst
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/deb/prerm
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/deb/postrm
    ${CMAKE_CURRENT_BINARY_DIR}/conffiles)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "AGPL-3.0-only")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
set(CPACK_RPM_PACKAGE_REQUIRES "linuxptp, libX11, libXext, libXrandr, libXinerama, libXcursor")
set(CPACK_RPM_PACKAGE_SUGGESTS "pipewire")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/scripts/rpm/post.sh)
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/scripts/rpm/preun.sh)
set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/scripts/rpm/postun.sh)
set(CPACK_RPM_USER_FILELIST
    "%config(noreplace) ${CMAKE_INSTALL_FULL_SYSCONFDIR}/inlet/ptp4l.conf"
    "%config(noreplace) ${CMAKE_INSTALL_FULL_SYSCONFDIR}/alsa/conf.d/50-inlet.conf"
    "%config(noreplace) ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart/inlet-tray.desktop")
# Directories other packages own.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    ${CMAKE_INSTALL_FULL_SYSCONFDIR}/alsa ${CMAKE_INSTALL_FULL_SYSCONFDIR}/alsa/conf.d
    ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart
    /usr/lib/systemd /usr/lib/systemd/system /usr/lib/systemd/user
    /usr/share/polkit-1 /usr/share/polkit-1/rules.d /usr/share/applications
    /usr/share/icons /usr/share/icons/hicolor /usr/share/icons/hicolor/32x32 /usr/share/icons/hicolor/32x32/apps
    /usr/share/icons/hicolor/64x64 /usr/share/icons/hicolor/64x64/apps /usr/share/icons/hicolor/128x128
    /usr/share/icons/hicolor/128x128/apps /usr/share/icons/hicolor/256x256 /usr/share/icons/hicolor/256x256/apps
    /usr/lib64/alsa-lib /usr/lib/alsa-lib)

include(CPack)
