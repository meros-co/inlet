#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Sets Inlet's version everywhere it is written: the CMake project (which every
# binary, bundle and package takes it from), the Arch PKGBUILD and the Linux
# testing guide's file names. With --check, changes nothing and fails unless
# they all say VERSION (the release workflow's check of a tag).
#
#   tools/set-version.py 0.2.0
#   tools/set-version.py --check 0.2.0
#   tools/set-version.py                 prints the current version

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
V = r"[0-9]+\.[0-9]+\.[0-9]+"

# (file, pattern with the version as group 2, how many times it must match)
PLACES = [
    ("CMakeLists.txt", rf"(project\(Inlet VERSION )({V})", 1),
    ("packaging/linux/arch/PKGBUILD", rf"(^pkgver=)({V})", 1),
    ("packaging/linux/TESTING.txt", rf"(inlet[_-])({V})(?=[_-])", 2),
]


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def current():
    m = re.search(PLACES[0][1], read(PLACES[0][0]), re.M)
    return m.group(2)


def main(argv):
    check = "--check" in argv
    args = [a for a in argv if a != "--check"]
    if not args:
        print(current())
        return 0
    version = args[0].removeprefix("v")
    if not re.fullmatch(V, version):
        sys.exit(f"{args[0]}: a version is MAJOR.MINOR.PATCH, numbers only")
    wrong = []
    for rel, pattern, count in PLACES:
        text = read(rel)
        found = re.findall(pattern, text, re.M)
        if len(found) != count:
            sys.exit(f"{rel}: expected {count} version(s), found {len(found)}")
        if check:
            wrong += [f"{rel} says {f[1]}" for f in found if f[1] != version]
            continue
        text = re.sub(pattern, lambda m: m.group(1) + version, text, flags=re.M)
        if rel.endswith("PKGBUILD"):
            text = re.sub(r"^pkgrel=.*$", "pkgrel=1", text, flags=re.M)
        (ROOT / rel).write_text(text, encoding="utf-8", newline="\n")
    if wrong:
        sys.exit(f"not version {version}: " + "; ".join(wrong))
    print(f"{'all at' if check else 'set to'} {version}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
