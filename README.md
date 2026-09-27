# gecko-w10m

A **Gecko**-based browser for **Windows 10 Mobile**: the real Firefox engine, with **JIT
enabled**, inside a UWP app.

The phone's stock browser (the EdgeHTML-based Edge) can no longer render the modern web.
Gecko is kept current independently, so it brings those sites back.

## How Gecko runs under UWP

Windows 10 Mobile apps run in the UWP app container, which by default blocks the three
things a browser engine needs. Each is handled:

| Need | Solution |
|---|---|
| **JIT / executable memory** | The `codeGeneration` restricted capability + `VirtualAllocFromApp` / `VirtualProtectFromApp`. A force-included compat header remaps the allocators tree-wide; ARM I-cache flushing is added by a small wrapper. |
| **No child processes** | Gecko is built **single-process** (`MOZ_FORCE_DISABLE_E10S=1`). |
| **Sandbox / filesystem** | On an **interop-unlocked** device the appx may declare `broadFileSystemAccess` / `unrestrictedFileSystemAccess`. Interop unlock is what makes restricted capabilities available to a sideloaded app. |

Full detail: [`docs/PORTING-W10M.md`](docs/PORTING-W10M.md).

## Layout

```
app/GeckoW10m/        C++/WinRT UWP shell (the browser UI)
  ├─ App.cpp          application entry
  ├─ MainPage.*       address bar, nav, tabs, engine host
  ├─ client/          core models (preferences, search, tabs)
  ├─ engine/          GeckoEngine wrapper + gecko_capi C ABI
  └─ Package.appxmanifest   declares codeGeneration + restricted capabilities
mozconfig/            the engine's build configuration (arm-uwp, single-process, JIT)
patches/w10m/         the engine port as git format-patch files over the release tag
tools/                build scripts (build-all.sh runs the whole chain)
vendor/               mobile-config-firefox: the phone UI theme and default prefs
engine/firefox/       Gecko submodule: the port's branch w10m-port, on Firefox 155.0.1
```

## Build

The build runs on x64 Windows and cross-compiles for 32-bit ARM. The tools below are the
versions it is known to build with.

### Requirements

| Tool | Version | Notes |
|---|---|---|
| Visual Studio 2022 | 17.14 (MSVC 14.44) | Workload "Desktop development with C++" plus the individual component **MSVC v143 ARM build tools**. 17.14 is the last release with ARM32 tools. |
| Windows 10/11 SDK | 10.0.22621.0 | Headers, tools, cppwinrt. The newer 26100 dropped ARM32 libraries. |
| Windows 10 SDK | 10.0.14393.0 | Optional: the shell links against it so the package starts on 1607 phones (`GECKO_W10M_SDK_LIB=10.0.14393.0`). |
| VCLibs ARM | 14.00 | `Microsoft.VCLibs.arm.14.00.appx` under `C:\Program Files (x86)\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0`, installed with the UWP workload of Visual Studio. The package carries its CRT from it. |
| LLVM | 22.1 | clang-cl and lld-link, installed in `C:\Program Files\LLVM`. |
| MozillaBuild | 4.2 | In `C:\mozilla-build`. Run the scripts from its `start-shell.bat` or any bash that has it on PATH. |
| Rust | nightly (built with 1.96.0-nightly 2026-03-11) | The default toolchain, with the `rust-src` component: `rustup default nightly-2026-03-11`, `rustup component add rust-src`. The std for the ARM32 UWP target is compiled from source by `tools/uwp-install-std.sh`. |
| cbindgen | 0.29 | `cargo install cbindgen` |
| Node.js | 24 | On PATH. |

All locations can be changed with environment variables; see the top of
[`tools/env.sh`](tools/env.sh). The engine's object directory is `C:\rw-obj` by default
(`GECKO_W10M_OBJ`); keep its path short, or libwebrtc's deepest objects exceed MAX_PATH.
About 25 GB of free space are needed for it.

### Get the engine

Either way gives the same tree:

```bash
# a) the submodule, from the fork that carries the port's branch
git submodule update --init --depth 1 engine/firefox

# b) or plain Firefox at the tag in engine/release.txt, plus patches/w10m
git clone --depth 1 --branch FIREFOX_155_0_1_RELEASE https://github.com/mozilla-firefox/firefox engine/firefox
bash tools/apply-patches-w10m.sh      # or: tools\apply-patches-w10m.ps1
```

### Build the package

```bash
bash tools/build-all.sh
```

That fetches and patches the `windows` crate (`tools/prepare-windows-rs.sh`), builds the
Rust std for the target once (`tools/uwp-install-std.sh`), builds the engine
(`tools/browser-build.sh`), packages it (`tools/package.sh`) and builds, packs and signs
the appx (`tools/build-appx.sh`). The package lands in `app/GeckoW10m/AppPackages/`,
together with the certificate to trust on the phone and `INSTALL.txt`. A full engine build
takes about half an hour on 16 cores; after that the steps can be run one by one.

### Signing

The package is signed with `app/GeckoW10m/Gecko.pfx` (not in the repository; password
`gecko_w10m`, or `GECKO_W10M_PFX_PASSWORD`). When there is none, a self-signed certificate
is made for the manifest's `Publisher`. The signer must match that Publisher exactly, so
to publish your own build change `Publisher` in `app/GeckoW10m/Package.appxmanifest`
(and `Name`, if it should not replace this app on a phone that has it).

### Changing the engine

Commit to the `w10m-port` branch in `engine/firefox`, then refresh the patches, which also
checks that they rebuild the branch exactly, and commit them with the submodule:

```bash
bash tools/export-patches-w10m.sh
```

### Older scripts

`tools/build-gecko-uwp.ps1`, `uwp-configure.sh`, `uwp-build.sh`, `uwp-build-std.sh` and
`mozconfig/mozconfig.arm-uwp` are the SpiderMonkey-only bring-up from before the whole
browser built; they are kept for reference and are not part of the build above.

## License

Shell code: GPL-3.0. `patches/` and the engine branch (Gecko modifications): MPL-2.0.
