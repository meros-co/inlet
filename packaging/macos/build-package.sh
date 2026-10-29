#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Builds Inlet for macOS (Release) and makes an installer package,
# dist/Inlet-<version>-macos.pkg, which installs:
#   /Applications/Inlet.app                       the app
#   /Library/Audio/Plug-Ins/HAL/Inlet.driver      the Core Audio driver
#   /Library/Application Support/Inlet/bin        inletd, GStreamer's PTP helper and
#                                                 the libraries they need, the tools
#   /Library/LaunchDaemons/co.meros.inletd.plist  inletd as a launchd daemon
#   /Library/LaunchAgents/co.meros.inlet.plist    the app in the menu bar at sign-in
#
#   packaging/macos/build-package.sh
#
# Needs Xcode's command-line tools, CMake 3.24+, Ninja, pkg-config and
# GStreamer 1.24 or later (Homebrew's `gstreamer`, or the official macOS
# framework with PKG_CONFIG_PATH pointing into it). Homebrew's GStreamer is for
# this Mac's architecture and macOS version only; for a universal build that
# runs on macOS 13, use the official framework and ARCHS="arm64;x86_64".
#
# Environment:
#   ARCHS                 CMAKE_OSX_ARCHITECTURES (default: this Mac's)
#   CODESIGN_IDENTITY     a "Developer ID Application" identity (default: ad hoc)
#   INSTALLER_IDENTITY    a "Developer ID Installer" identity to sign the package
#   BUILD_DIR             default: build-release

set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build=${BUILD_DIR:-"$root/build-release"}
identity=${CODESIGN_IDENTITY:--}

git -C "$root" submodule update --init
set -- -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
if [ -n "${ARCHS:-}" ]; then
    set -- "$@" "-DCMAKE_OSX_ARCHITECTURES=$ARCHS"
fi
cmake "$@"
cmake --build "$build" --target inletd inlet_coreaudio InletApp inlet-ptp-probe inlet-coreaudio-loopback

version=$(sed -n 's/^project(Inlet VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
stage="$root/dist/macos-stage"
payload="$stage/root"
rm -rf "$stage"
support="$payload/Library/Application Support/Inlet"
mkdir -p "$payload/Applications" "$payload/Library/Audio/Plug-Ins/HAL" "$support/bin" "$support/licences" \
         "$payload/Library/LaunchDaemons" "$payload/Library/LaunchAgents"

# Inlet itself.
ditto "$build/src/app/InletApp_artefacts/Release/Inlet.app" "$payload/Applications/Inlet.app"
ditto "$build/src/coreaudio/Inlet.driver" "$payload/Library/Audio/Plug-Ins/HAL/Inlet.driver"
cp "$build/src/inletd/inletd" "$build/tools/inlet-ptp-probe" "$build/tools/inlet-coreaudio-loopback" "$support/bin/"
cp "$root/tools/fake_ptp_master.py" "$support/bin/"

# GStreamer's PTP helper beside inletd, and the libraries inletd and the probe
# link (LGPL; see THIRD-PARTY.txt), with them.
gst=$(pkg-config --variable=prefix gstreamer-1.0)
cp "$gst/libexec/gstreamer-1.0/gst-ptp-helper" "$support/bin/"
python3 "$here/bundle-dylibs.py" "$support/bin/lib" "$support/bin/inletd" "$support/bin/inlet-ptp-probe" "$support/bin/gst-ptp-helper"

# launchd, the uninstaller, the guide and the licences.
cp "$here/co.meros.inletd.plist" "$payload/Library/LaunchDaemons/"
cp "$here/co.meros.inlet.plist" "$payload/Library/LaunchAgents/"
cp "$here/uninstall.sh" "$here/TESTING.txt" "$here/THIRD-PARTY.txt" "$support/"
cp "$root/LICENSE" "$support/LICENSE.txt"
cp "$root/src/app/assets/fonts/Inter-OFL.txt" "$support/licences/Inter-OFL.txt"
cp "$root/src/app/assets/icons/PHOSPHOR-LICENSE.txt" "$support/licences/Phosphor-LICENSE.txt"

# No extended attributes (quarantine, provenance): the package would carry them
# as "._" files.
xattr -cr "$payload"

# Signed: ad hoc unless an identity is given (then with the hardened runtime,
# for notarisation).
sign() {
    if [ "$identity" = "-" ]; then
        codesign --force --sign - "$1"
    else
        codesign --force --sign "$identity" --options runtime --timestamp "$1"
    fi
}
for f in inletd inlet-ptp-probe inlet-coreaudio-loopback gst-ptp-helper; do
    sign "$support/bin/$f"
done
sign "$payload/Library/Audio/Plug-Ins/HAL/Inlet.driver"
sign "$payload/Applications/Inlet.app"

# The package: installed where it says, never "relocated" to another copy of
# the app the installer happens to find.
pkgbuild --analyze --root "$payload" "$stage/components.plist" >/dev/null
n=0
while plutil -extract "$n" xml1 -o /dev/null "$stage/components.plist" >/dev/null 2>&1; do
    plutil -replace "$n.BundleIsRelocatable" -bool NO "$stage/components.plist"
    n=$((n + 1))
done
pkg="$root/dist/Inlet-$version-macos.pkg"
set -- --root "$payload" --component-plist "$stage/components.plist" --scripts "$here/scripts" \
       --identifier co.meros.inlet --version "$version" --install-location /
if [ -n "${INSTALLER_IDENTITY:-}" ]; then
    set -- "$@" --sign "$INSTALLER_IDENTITY"
fi
COPYFILE_DISABLE=1 pkgbuild "$@" "$pkg"
echo "Package: $pkg"
