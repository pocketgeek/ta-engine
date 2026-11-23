# Application icons

The application icon is our procedural crown from `src/util/appicon.cpp`, not a
retail asset. Regenerate the checked-in Windows and macOS icon containers with:

```sh
cmake --build build --target makeicons
python3 tools/make-platform-icons.py build/makeicons
```

The generator uses Python's standard library and the existing C++ renderer. It
adds no runtime dependency or normal-build requirement. Windows embeds the ICO
in `takclient.exe` through a compiled resource, and NSIS uses it for the installer
and uninstaller. macOS copies the ICNS into `Contents/Resources` and declares it
in `Info.plist`.

The macOS bundle retains its required `.app` suffix. Finder controls whether
extensions are displayed. Do not add FinderInfo attributes to the signed bundle:
strict code-signature verification rejects them. The ZIP is created with `ditto`
and distributed intact. Native CI validates the ICNS with `iconutil` and verifies
the bundle signature before packaging.

## macOS release signing

The `sign` job in `.github/workflows/macos.yml` signs, notarizes and
staples the macOS downloads for each architecture on a separate `macos-latest`
runner after the build, so the signing secrets never enter the build job. It
imports the Developer ID Application certificate into a temporary keychain,
signs every nested Mach-O and then each `.app` (game and Cartographer) with the
hardened runtime and a secure timestamp, notarizes the release ZIP and the debug
ZIP with an App Store Connect API key, staples the apps, rebuilds the ZIP and the
DMG from the stapled apps, and signs, notarizes and staples the DMG. Apple's
notarization log is always printed. The debug ZIP's bare command-line binaries
cannot hold a stapled ticket; Gatekeeper checks them online.

| Secret | Value |
| --- | --- |
| `MACOS_CERT_P12` | Base64 Developer ID Application certificate and key (`.p12`) |
| `MACOS_CERT_PASSWORD` | Password of that `.p12` |
| `MACOS_SIGN_IDENTITY` | Signing identity name |
| `NOTARY_KEY_P8` | Base64 App Store Connect API key (`.p8`) |
| `NOTARY_KEY_ID` | API key id |
| `NOTARY_ISSUER_ID` | API key issuer id |

On a `v*` tag the signed ZIP, debug ZIP and DMG replace the unsigned files the
build job attached, under the same names. On `main` they are uploaded as CI
artifacts. The job is skipped, not failed, on fork pull requests and wherever
the secrets are missing; decoded key material stays in `RUNNER_TEMP` and is
removed at the end.

## Windows release signing

Official `pocketgeek/tak-engine` `v*` tag builds use Azure Artifact Signing via
GitHub OIDC. Both native builds and their tests must pass before a separate
`windows-2022` x64 job downloads their same-run executable artifacts. The signing
action does not support ARM64 runners; ARM64 executables are signed on x64
without rebuilding or running them.

The job uses the **windows-signing** environment, whose deployment rule must
allow **tags matching `v*` only** (no branches). Its environment secrets are
`AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, and `AZURE_SUBSCRIPTION_ID`; no Azure client
secret or certificate private key is stored in GitHub. Its variables are:

| Variable | Value |
| --- | --- |
| `AZURE_SIGNING_ACCOUNT_NAME` | `Githubsign` |
| `AZURE_SIGNING_CERTIFICATE_PROFILE_NAME` | `tak-engine` |
| `AZURE_SIGNING_ENDPOINT` | `https://eus.codesigning.azure.net/` |

The Azure federated credential should trust issuer
`https://token.actions.githubusercontent.com`, audience `api://AzureADTokenExchange`,
and subject `repo:pocketgeek/tak-engine:environment:windows-signing`. The service
principal needs the **Artifact Signing Certificate Profile Signer** role on the
signing profile/account. Only the release signing job receives `id-token: write`.
The environment's tag restriction is required because an environment-based OIDC
subject identifies the environment, not a specific branch or tag.

All three executables (`takclient`, `takserver`, `cartographer`)
in both Release and Debug, for x64 and ARM64, are signed with SHA-256 and an
RFC 3161 SHA-256 timestamp. Authenticode and Windows SDK SignTool verify signatures,
certificate chains and timestamps before ZIP/NSIS packaging. Packaging checks
that ZIP entries and the NSIS staged payload retain the signed bytes. The
completed x64 installer is then signed and verified before anything is attached
to the official release. A missing signing setting, failed signing request,
missing timestamp or verification error fails the job; it cannot publish unsigned
Windows release assets.

Main builds and manual branch builds remain unsigned and require no Azure
configuration. Forks retain unsigned builds and tag packaging without entering
the signing environment. Native build artifacts are unsigned intermediate CI
outputs; official release downloads come only from the verified signing job.
The shared `windows/installer.cmake` keeps ordinary and signed NSIS packaging
consistent. This adds no application runtime dependency.

References: [Azure signing action and runner requirements](https://github.com/Azure/artifact-signing-action),
[OIDC setup](https://github.com/Azure/artifact-signing-action/blob/main/docs/OIDC.md),
and [SignTool verification](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool).

## Linux AppImage

`packaging/appimage/build.sh <build-dir> <out-dir>` turns a built Release tree into
`tak-engine-<version>-<arch>.AppImage`. The binaries link only libc/libm, so the AppDir
is the `cmake --install` tree plus `AppRun` and no libraries are bundled. `AppRun`
starts the game, or `cartographer` / `takserver` when the first argument names one.
appimagetool and the AppImage runtime are pinned by release and SHA-256 in the script;
to move to newer ones, change the release names and both checksums together. CI builds
it on the Ubuntu 22.04 legs (glibc 2.35, the oldest of the Ubuntu/Debian legs) for x64
and ARM64 and checks each program through `APPIMAGE_EXTRACT_AND_RUN`, since containers
have no FUSE.
