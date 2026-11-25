## Download

| Platform | File |
| --- | --- |
| Windows 10 22H2 or later, Windows 11 (x64) | `Inlet-@VERSION@-windows-x64-setup.exe` |
| macOS 13 or later (Apple silicon and Intel) | `Inlet-@VERSION@-macos.pkg` |
| Debian 12+, Ubuntu 22.04+ (x86-64) | `inlet_@VERSION@_amd64.deb` |
| RHEL, Rocky, Alma 9+, Fedora (x86-64) | `inlet-@VERSION@-1.x86_64.rpm` |
| Arch Linux (x86-64) | `inlet-@VERSION@-1-x86_64.pkg.tar.zst` |

The Windows installer and every program in it are signed; the macOS package is signed and
notarized. `SHA256SUMS` lists every file's SHA-256.

- **Windows:** run the installer. Unattended: `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART`.
- **macOS:** open the package.
- **Debian, Ubuntu:** `sudo apt install ./inlet_@VERSION@_amd64.deb`
- **RHEL, Rocky, Alma, Fedora:** `sudo dnf install ./inlet-@VERSION@-1.x86_64.rpm`
- **Arch Linux:** `sudo pacman -U inlet-@VERSION@-1-x86_64.pkg.tar.zst`

Installing over an earlier Inlet keeps its settings. Each platform's guide is installed with
Inlet (`TESTING.txt`) and is in the source under `packaging/`.

## Changes
