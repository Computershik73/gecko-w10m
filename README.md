# GeckoW10m for Windows 10 Mobile

A port of the [GeckoW10m](https://github.com/Computershik73/gecko-w10m) **Gecko**-based
browser to **Windows 10 Mobile**, running the real Firefox engine with **JIT enabled**
inside a UWP app.

Like the iOS original, this targets an unsupported platform whose stock browser (here the
EdgeHTML-based Edge) can no longer render the modern web. Gecko is kept current
independently, so it brings those sites back.

## How Gecko runs under UWP

Windows 10 Mobile apps run in the UWP app container, which by default blocks the three
things a browser engine needs. Each is handled:

| Need | Solution |
|---|---|
| **JIT / executable memory** | The `codeGeneration` restricted capability + `VirtualAllocFromApp` / `VirtualProtectFromApp`. A force-included compat header remaps the allocators tree-wide; ARM I-cache flushing is added by a small wrapper. |
| **No child processes** | Gecko is built **single-process** (`MOZ_FORCE_DISABLE_E10S=1`). |
| **Sandbox / filesystem** | On an **interop-unlocked** device the appx may declare `broadFileSystemAccess` / `unrestrictedFileSystemAccess`. Interop unlock is the W10M equivalent of the iOS TrollStore / jailbreak path GeckoW10m already uses. |

Full detail: [`docs/PORTING-W10M.md`](docs/PORTING-W10M.md).

## Layout

```
app/GeckoW10m/          C++/WinRT UWP shell (the browser UI)
  ├─ App.cpp          application entry
  ├─ MainPage.*       address bar, nav, tabs, engine host
  ├─ client/          ported core models (preferences, search, tabs)
  ├─ engine/          GeckoEngine wrapper + gecko_capi C ABI + JIT self-test stub
  └─ Package.appxmanifest   declares codeGeneration + restricted capabilities
mozconfig/            arm-uwp, single-process, JIT-enabled build config
patches/w10m/         engine source redirections (JIT memory, single-process)
tools/                apply-patches + build scripts (VS2017 + Win10 SDK)
engine/firefox/       Gecko submodule (Firefox 155.0.1, matches release.txt)
```

## Build

```powershell
# 1. Get Gecko (pinned to engine/release.txt)
git submodule update --init --recursive engine/firefox

# 2. Apply the W10M patch set + drop in the JIT wrappers
tools\apply-patches-w10m.ps1

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

Shell code follows the upstream project: GPL-3.0, except `patches/` (Gecko modifications)
under MPL-2.0.
