#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Builds the Arch package with this directory's PKGBUILD from a commit of a
# local checkout, rather than from the release tag on GitHub: what CI and the
# release workflow run in the Arch image (packaging/linux/docker), as a user
# that may run makepkg.
#
#   packaging/linux/arch/makepkg-checkout.sh <checkout> <commit> <output directory>

set -eu
checkout=$1
commit=$2
out=$3
here=$(cd "$(dirname "$0")" && pwd)

git config --global --add safe.directory '*'
git config --global protocol.file.allow always
work=$(mktemp -d)
cp "$here/PKGBUILD" "$here/inlet.install" "$work/"
cd "$work"
sed -i "s|^source=.*|source=(\"inlet::git+file://$checkout#commit=$commit\")|" PKGBUILD
makepkg --syncdeps --noconfirm --nocheck
# The package, not its debug symbols.
find . -maxdepth 1 -name 'inlet-[0-9]*.pkg.tar.zst' -exec cp {} "$out/" \;
ls "$out"/inlet-[0-9]*.pkg.tar.zst
