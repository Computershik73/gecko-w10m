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
  ├─ engine/          GeckoEngine wrapper + gecko_capi C ABI + JIT self-test stub
  └─ Package.appxmanifest   declares codeGeneration + restricted capabilities
mozconfig/            arm-uwp, single-process, JIT-enabled build config
patches/w10m/         the engine port as git format-patch files over the release tag
tools/                apply-patches + build scripts (VS2017 + Win10 SDK)
engine/firefox/       Gecko submodule: the port's branch w10m-port, on Firefox 155.0.1
```

## Build

```powershell
# 1. Get the ported engine, either way -- both give the same tree:
#  a) the submodule, from the fork that carries the port's branch (w10m-port)
git submodule update --init engine/firefox
#  b) or plain Firefox at the tag in engine/release.txt, plus patches/w10m
git clone --depth 1 --branch FIREFOX_155_0_1_RELEASE https://github.com/mozilla-firefox/firefox engine/firefox
tools\apply-patches-w10m.ps1          # or: bash tools/apply-patches-w10m.sh

# 2. After committing to the engine, refresh the patches (checks they rebuild the branch)
bash tools/export-patches-w10m.sh

# 3. Cross-compile the engine (VS2017 + Windows 10 SDK, arm-uwp)
tools\build-gecko-uwp.ps1

# 4. Open app\GeckoW10m.sln in VS2017, build Release|ARM, sideload the appx
```

## Current status

- **Shell**: builds and runs standalone against the bundled engine **stub**. The stub
  performs a live **JIT self-test** on launch (allocates a page, writes a Thumb function,
  flips it executable via `VirtualProtectFromApp`, calls it) so you can confirm the
  `codeGeneration` capability works on the device before committing to the full build.
- **Engine**: mozconfig, patch set and JIT wrappers are in place. The remaining work is
  the `arm-uwp` cross-compile — chiefly redirecting the residual Win32-only calls to their
  OneCore / `*FromApp` equivalents. This iterates.

Flip the project from the stub to the real engine by setting
`GeckoW10mUseEngineStub=false` and linking the import library produced in step 3.

## License

Shell code: GPL-3.0. `patches/` and the engine branch (Gecko modifications): MPL-2.0.
