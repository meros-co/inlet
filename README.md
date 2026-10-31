# Inlet

[![CI](https://github.com/meros-co/inlet/actions/workflows/ci.yml/badge.svg)](https://github.com/meros-co/inlet/actions/workflows/ci.yml)

Inlet is an open-source AES67 virtual sound card for Windows, macOS and Linux. A computer sends
and receives AES67 audio as if the network were a local audio interface.

- **Windows:** an ASIO® driver, for ASIO hosts (DAWs and similar audio software).
- **macOS:** a Core Audio driver (AudioServerPlugIn), available to every application.
- **Linux:** a PipeWire device, and the ALSA device `inlet` for programs that use ALSA directly
  (headless machines, systems without PipeWire).
- **Clocking:** Inlet follows the network's PTP grandmaster (GStreamer's PTP clock on Windows and
  macOS, linuxptp on Linux). Its audio clock *is* the AES67 media clock, so streams on the same
  grandmaster play and send without resampling or drift. Programs may run at another sample rate;
  Inlet converts and reports the latency.
- **Discovery and routing:** SAP/SDP and NMOS (IS-04, IS-05, IS-08), so any NMOS controller or
  console can route it.

<img src="third_party/asiosdk/logo/ASIO-compatible-logo-Steinberg-TM-BW.png" alt="ASIO Compatible" width="120"/>

Inlet is in early development. Test builds are available; they are unsigned and not yet ready
for production use.

## How it fits together

- **inletd**, the service (a Windows service, a launchd daemon, a systemd unit), owns the network:
  PTP, the AES67 receivers and senders, SAP and the NMOS node. The drivers are thin clients that
  share audio with it through shared memory.
- **The Inlet app** shows the status and edits the settings, receivers and senders. It talks to
  inletd through its local HTTP API at `http://127.0.0.1:8091/inlet/v1/`, which another program
  can use the same way to run inletd without the app (see `src/inletd/Engine.h`). Turning the
  `nmos` setting off leaves NMOS to that program.

## Platforms

- Windows 10 22H2 or later, and Windows 11, on x64. (No 32-bit ASIO hosts.)
- macOS 13 or later, universal binaries (Apple silicon and Intel).
- Linux on x86-64: Ubuntu 22.04 or later, Debian 12 or later, Fedora, RHEL, Rocky or Alma Linux 9
  or later, and Arch Linux, with systemd and PipeWire or ALSA.

A wired network interface on the AES67 network is recommended: Inlet warns on Wi-Fi, which carries
the streams poorly.

## Building

CMake 3.24 or later and a C11/C++20 compiler. The build fetches JUCE, libsamplerate, nlohmann/json
and cpp-httplib.

```
git submodule update --init
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

- **Windows:** Visual Studio 2022 or later with C++, and GStreamer 1.24 or later (the MSVC x86_64
  runtime and development files). `packaging/windows/build-package.ps1` makes the test package
  (a zip with install scripts); see `packaging/windows/TESTING.txt`.
- **macOS:** Xcode's command-line tools and GStreamer 1.24 or later (`brew install gstreamer`).
  `packaging/macos/build-package.sh` makes the installer package (the app, the Core Audio driver,
  inletd as a launchd daemon); see `packaging/macos/TESTING.txt`.
- **Linux:** the ALSA, PipeWire, X11, FreeType and Fontconfig development packages; the
  Dockerfiles in `packaging/linux/docker` list them for Debian, Rocky Linux and Arch. In the build
  directory, `cpack -G DEB` and `cpack -G RPM` make the packages, and `packaging/linux/arch` has a
  PKGBUILD; see `packaging/linux/TESTING.txt`. `tests/linux` has end-to-end checks of the ALSA
  and PipeWire devices and of PTP.

Continuous integration (`.github/workflows/ci.yml`) builds and tests every push and pull request
on Windows, macOS, Debian, Rocky Linux and Arch, and keeps the Windows zip, the macOS package and
the .deb and .rpm as artifacts of each run.

## License

Inlet is licensed under the GNU Affero General Public License, version 3 only
(`AGPL-3.0-only`). See [LICENSE](LICENSE). Copyright © 2026 Meros Inc.

The Windows driver is built with the Steinberg ASIO SDK, used under the GNU General Public
License, version 3; see [third_party/asiosdk](third_party/asiosdk/VENDORED.md).

ASIO is a registered trademark of Steinberg Media Technologies GmbH.
