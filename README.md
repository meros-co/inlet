# Inlet

Inlet is an open-source AES67 virtual sound card for Windows and macOS. A computer sends and
receives AES67 audio as if the network were a local audio interface.

- **Windows:** an ASIO® driver, for ASIO hosts (DAWs and similar audio software).
- **macOS:** a Core Audio driver (AudioServerPlugIn), available to every application.
- **Clocking:** Inlet follows the network's PTP grandmaster. Its audio clock *is* the AES67
  media clock, so streams on the same grandmaster play and send without resampling or drift.
- **Discovery and routing:** SAP/SDP and NMOS (IS-04, IS-05, IS-08), so any NMOS controller,
  and a [Manifold](https://github.com/meros-co) console as a first-party device, can route it.

<img src="third_party/asiosdk/logo/ASIO-compatible-logo-Steinberg-TM-BW.png" alt="ASIO Compatible" width="120"/>

Inlet is in early development and is not yet usable.

## Platforms

- Windows 10 22H2 or later, and Windows 11, on x64 and ARM64. (No 32-bit ASIO hosts.)
- macOS 13 or later, universal binaries.

## Building

CMake 3.24 or later and a C11/C++20 compiler.

```
git submodule update --init
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## Licence

Inlet is licensed under the GNU Affero General Public License, version 3 only
(`AGPL-3.0-only`). See [LICENSE](LICENSE). Copyright © 2026 Meros Inc.

The Windows driver is built with the Steinberg ASIO SDK, used under the GNU General Public
License, version 3; see [third_party/asiosdk](third_party/asiosdk/VENDORED.md).

ASIO is a registered trademark of Steinberg Media Technologies GmbH.
