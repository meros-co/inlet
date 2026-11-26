# Releasing Inlet

A release is a version tag. Pushing `vMAJOR.MINOR.PATCH` runs the
[Release workflow](.github/workflows/release.yml), which:

1. checks that the tag and the source say the same version;
2. builds and tests Inlet on Windows, macOS and Linux;
3. signs the Windows binaries and installer (Azure Artifact Signing), and signs and notarizes the
   macOS package (Developer ID);
4. installs and uninstalls each package on its runner;
5. creates a **draft** GitHub Release with the packages, `SHA256SUMS` and notes.

Nothing is public until a maintainer publishes the draft. Versions `0.x` are marked as
pre-releases.

| Platform | Package |
| --- | --- |
| Windows x64 | `Inlet-<version>-windows-x64-setup.exe` (Inno Setup, `packaging/windows/inlet.iss`) |
| macOS 13+, universal | `Inlet-<version>-macos.pkg` (`packaging/macos/build-package.sh`) |
| Debian 12+, Ubuntu 22.04+ | `inlet_<version>_amd64.deb` (CPack, built on Debian 12) |
| RHEL, Rocky, Alma 9+, Fedora | `inlet-<version>-1.x86_64.rpm` (CPack, built on Rocky Linux 9) |
| Arch Linux | `inlet-<version>-1-x86_64.pkg.tar.zst` (`packaging/linux/arch/PKGBUILD`) |

## Making a release

1. Check that CI is green on `main`.
2. Set the version. `tools/set-version.py` writes it everywhere it appears (the CMake project, the
   PKGBUILD and the Linux guide):

   ```
   python3 tools/set-version.py 0.2.0
   git commit -am "Set the version to 0.2.0."
   ```

3. Tag that commit and push both:

   ```
   git tag -a v0.2.0 -m "Inlet 0.2.0"
   git push origin main v0.2.0
   ```

4. When the workflow finishes, open the draft under **Releases**. The notes start with the
   download table (`.github/release-notes.md`), followed by the changes GitHub lists since the
   last release; edit them as needed. Install a package or two, then press **Publish release**.

Versions are three numbers. A tag that does not match the source fails the first job, before
anything is built.

### When the release build fails

Fix the problem on `main`, then move the tag to the fixed commit and push it again:

```
git tag -d v0.2.0 && git push origin :refs/tags/v0.2.0
git tag -a v0.2.0 -m "Inlet 0.2.0" && git push origin v0.2.0
```

Delete any draft the failed run left first (**Releases**, the draft, **Delete**).

### Rehearsing

**Actions → Release → Run workflow** on a branch does everything except the release: the signed
and notarized packages are kept as the run's artifacts. Run it after changing the packaging or
the signing setup, before tagging.

## One-time setup

The workflow reads these from the repository's **Settings → Secrets and variables → Actions**.
CI on pull requests never sees them and builds unsigned packages.

### Windows: Azure Artifact Signing

An Artifact Signing account with a certificate profile, and an app registration (service
principal) holding the **Artifact Signing Certificate Profile Signer** role on that profile.

| Name | Kind | Value |
| --- | --- | --- |
| `ARTIFACT_SIGNING_ENDPOINT` | variable | the account's endpoint, such as `https://eus.codesigning.azure.net/` |
| `ARTIFACT_SIGNING_ACCOUNT` | variable | the account's name |
| `ARTIFACT_SIGNING_PROFILE` | variable | the certificate profile's name |
| `AZURE_TENANT_ID` | secret | the app registration's tenant ID |
| `AZURE_CLIENT_ID` | secret | the app registration's client (application) ID |
| `AZURE_CLIENT_SECRET` | secret | a client secret of the app registration |

`packaging/windows/sign.ps1` signs with these through signtool and Microsoft's signing client. On a
Windows machine with the same environment variables set, `packaging/windows/build-package.ps1`
makes a signed installer locally. Saved as user environment variables, they apply to every build,
including from a shell opened before they were saved.

### macOS: Developer ID and notarization

From an Apple Developer account (Team Agent or Admin):

- a **Developer ID Application** certificate and a **Developer ID Installer** certificate, each
  exported from Keychain Access with its private key as a `.p12`, both with the same password;
- an **App Store Connect API key** with the Developer role (Users and Access → Integrations →
  App Store Connect API), downloaded as a `.p8`.

| Name | Kind | Value |
| --- | --- | --- |
| `MACOS_APPLICATION_P12` | secret | the Developer ID Application `.p12`, base64 |
| `MACOS_INSTALLER_P12` | secret | the Developer ID Installer `.p12`, base64 |
| `MACOS_P12_PASSWORD` | secret | the `.p12` files' password |
| `NOTARY_KEY_P8` | secret | the API key's `.p8`, base64 |
| `NOTARY_KEY_ID` | secret | the API key's ID |
| `NOTARY_ISSUER` | secret | the issuer ID shown above the keys |

For example, with the GitHub CLI on a Mac:

```
base64 -i DeveloperIDApplication.p12 | gh secret set MACOS_APPLICATION_P12
base64 -i DeveloperIDInstaller.p12 | gh secret set MACOS_INSTALLER_P12
gh secret set MACOS_P12_PASSWORD
base64 -i AuthKey_XXXXXXXXXX.p8 | gh secret set NOTARY_KEY_P8
gh secret set NOTARY_KEY_ID --body XXXXXXXXXX
gh secret set NOTARY_ISSUER --body 00000000-0000-0000-0000-000000000000
```

On a Mac with the certificates in the login keychain, `build-package.sh` does the same locally:

```
ARCHS="arm64;x86_64" \
CODESIGN_IDENTITY="Developer ID Application: …" INSTALLER_IDENTITY="Developer ID Installer: …" \
NOTARY_KEY_PATH=AuthKey_XXXXXXXXXX.p8 NOTARY_KEY_ID=XXXXXXXXXX NOTARY_ISSUER=… \
packaging/macos/build-package.sh
```

A universal build needs GStreamer's official framework (the runtime and development packages from
gstreamer.freedesktop.org) with `PKG_CONFIG_PATH=/Library/Frameworks/GStreamer.framework/Versions/1.0/lib/pkgconfig`;
Homebrew's GStreamer builds for the Mac it is on only.
