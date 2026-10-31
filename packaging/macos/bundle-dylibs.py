#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Meros Inc.
#
# Copies the libraries a program links (GStreamer, GLib and theirs), outside
# macOS's own, into a folder beside it, and points the program and the copies
# at each other there, so the program runs on a Mac without them installed.
#
#   bundle-dylibs.py <libdir> <program>...
#
# Each program then finds them at @executable_path/<libdir's name>; the copies
# find each other at @loader_path. Everything changed is signed again (ad hoc,
# or with $CODESIGN_IDENTITY).

import os
import shutil
import subprocess
import sys


def linked(path):
    """The libraries `path` links, as written in it."""
    out = subprocess.run(["otool", "-L", path], check=True, capture_output=True, text=True).stdout
    return [line.strip().split(" (")[0] for line in out.splitlines()[1:] if line.strip()]


def rpaths(path):
    out = subprocess.run(["otool", "-l", path], check=True, capture_output=True, text=True).stdout.splitlines()
    found = []
    for i, line in enumerate(out):
        if line.strip() == "cmd LC_RPATH":
            for follow in out[i + 1:i + 4]:
                follow = follow.strip()
                if follow.startswith("path "):
                    found.append(follow[5:].split(" (offset")[0])
    return found


def resolve(name, user):
    """Where a linked name is on this Mac, from the program or library `user`."""
    here = os.path.dirname(os.path.realpath(user))
    if name.startswith("@rpath/"):
        for r in rpaths(user):
            r = r.replace("@loader_path", here).replace("@executable_path", here)
            candidate = os.path.join(r, name[len("@rpath/"):])
            if os.path.exists(candidate):
                return os.path.realpath(candidate)
        return None
    if name.startswith("@loader_path/") or name.startswith("@executable_path/"):
        return os.path.realpath(os.path.join(here, name.split("/", 1)[1]))
    return os.path.realpath(name) if os.path.exists(name) else None


def system(name):
    return name.startswith("/usr/lib/") or name.startswith("/System/")


def sign(path):
    identity = os.environ.get("CODESIGN_IDENTITY", "-")
    args = ["codesign", "--force", "--sign", identity]
    if identity != "-":
        args += ["--options", "runtime", "--timestamp"]
    subprocess.run(args + [path], check=True, capture_output=True)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    libdir = os.path.abspath(sys.argv[1])
    programs = [os.path.abspath(p) for p in sys.argv[2:]]
    os.makedirs(libdir, exist_ok=True)
    prefix = os.path.basename(libdir)

    copied = {}   # real source path -> copy
    queue = list(programs)
    while queue:
        user = queue.pop()
        source = user if user in programs else next(s for s, c in copied.items() if c == user)
        for name in linked(user):
            if system(name) or os.path.basename(name) == os.path.basename(user) and user not in programs:
                continue
            real = resolve(name, source)
            if real is None:
                if not name.startswith("@"):
                    sys.exit(f"{user} links {name}, which is not on this Mac")
                continue
            if real not in copied:
                copy = os.path.join(libdir, os.path.basename(name))
                shutil.copy2(real, copy)
                os.chmod(copy, 0o755)
                copied[real] = copy
                queue.append(copy)

    by_name = {os.path.basename(c): c for c in copied.values()}
    for path in programs + list(copied.values()):
        is_program = path in programs
        args = ["install_name_tool"]
        if not is_program:
            args += ["-id", "@rpath/" + os.path.basename(path)]
        for name in linked(path):
            base = os.path.basename(name)
            if system(name) or base not in by_name:
                continue
            target = (f"@executable_path/{prefix}/" if is_program else "@loader_path/") + base
            args += ["-change", name, target]
        if len(args) > 1:
            subprocess.run(args + [path], check=True, capture_output=True)
    for path in list(copied.values()) + programs:
        sign(path)
    print(f"bundled {len(copied)} libraries into {libdir}")


if __name__ == "__main__":
    main()
