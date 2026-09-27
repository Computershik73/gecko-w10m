# Porting Gecko to Windows 10 Mobile

This document describes how a Gecko browser is built for **Windows 10 Mobile (W10M)** as a
native UWP application, with a **JIT-enabled** SpiderMonkey/Gecko engine.

The target device is a **personal, interop-unlocked** phone. That matters: interop unlock
lets us sideload an appx that declares **restricted capabilities** and escape the default
app-container restrictions.

## The four hard problems and how each is solved

| Problem | Default UWP behaviour | Our approach |
|---|---|---|
| **JIT / executable memory** | App container blocks `PAGE_EXECUTE_*` via plain `VirtualAlloc`/`VirtualProtect`. | Declare the **`codeGeneration`** restricted capability and use `VirtualAllocFromApp` + `VirtualProtectFromApp`, which *are* permitted with that capability. This is exactly what Chakra/EdgeHTML did. See `patches/w10m/js/src/jit/`. |
| **Multiprocess (e10s)** | UWP forbids `CreateProcess` / spawning arbitrary child processes. | Build Gecko **single-process (non-e10s)**: no content/GPU/socket child processes. See `patches/w10m/ipc/`. |
| **Sandbox breadth** (files, no Win32) | App container limits filesystem and blocks many Win32 APIs. | On an **interop-unlocked** device, declare `broadFileSystemAccess` + `unrestrictedFileSystemAccess` and rely on the interop-unlock trust to load the appx. Redirect the remaining forbidden Win32 calls to their **OneCore / `*FromApp`** equivalents. |
| **Cross-compile to `arm-uwp`** | Not an upstream-supported target. | Custom `mozconfig` (`mozconfig/mozconfig.arm-uwp`) plus a set of `Win32 → OneCore/FromApp` source redirections. This is the largest remaining engineering task and iterates. |

## JIT: how executable memory works under `codeGeneration`

`VirtualAllocFromApp` **cannot** allocate `PAGE_EXECUTE_*` at commit time. The permitted
flow (and the one SpiderMonkey already follows) is a **W^X toggle**:

1. Reserve the whole code region once with `VirtualAllocFromApp(MEM_RESERVE, PAGE_NOACCESS)`.
2. To write code: commit / reprotect the page to `PAGE_READWRITE` with `VirtualProtectFromApp`.
3. To execute: reprotect to `PAGE_EXECUTE_READ` with `VirtualProtectFromApp`, then
   `FlushInstructionCache` (required on ARM for I-cache coherency).

This maps cleanly onto SpiderMonkey's existing `ProtectionSetting::{Writable,Executable}`
model, so the port reuses the upstream `XP_WIN` code path and only swaps the two allocator
calls for their `*FromApp` variants, gated on `GECKO_W10M`.

`codeGeneration` and Arbitrary Code Guard (ACG) are mutually exclusive: **ACG must be
off**, which is the default for a sideloaded appx that is not opted into that mitigation.

## Build

```powershell
# 1. Clone Gecko (matches engine/release.txt)
git submodule update --init --recursive engine/firefox

# 2. Apply the W10M patch set
tools\apply-patches-w10m.ps1

# 3. Build the engine (uses VS2017 + Win10 SDK)
tools\build-gecko-uwp.ps1
```

`build-gecko-uwp.ps1` points mozbuild at the VS2017 Enterprise toolchain at
`C:\Program Files (x86)\Microsoft Visual Studio\2017\Enterprise\` and the ARM UWP libraries.

## Deploy (interop-unlocked device)

1. Build the app package (`app/GeckoW10m`, `Release|ARM`).
2. Sign with your own / interop cert and sideload the `.appx`.
3. The `codeGeneration` and file-system restricted capabilities are honoured because the
   device is interop-unlocked.

## Layout

```
mozconfig/        build configuration for arm-uwp, single-process, JIT
patches/w10m/     engine source redirections (JIT memory, single-process, Win32→OneCore)
app/GeckoW10m/      the UWP shell app that hosts the engine (manifest, capabilities)
tools/            apply-patches and build scripts
```
