# Engine build status — ARM32 Windows 10 Mobile / UWP

**STATUS: the full browser Gecko builds and links.** `mach build` finishes with
`Your build was successful!` for `--enable-project=browser` on
**arm-pc-windows-msvc**, producing in `C:/rw-obj/dist/bin`:

| binary | size | machine | APPCONTAINER |
|---|---|---|---|
| `xul.dll` | 132.6 MB | `0x1C4` ARMNT | yes |
| `firefox.exe` | 0.8 MB | `0x1C4` | yes |
| `mozglue.dll` | 0.5 MB | `0x1C4` | yes |
| `gkcodecs.dll` | 8.0 MB | `0x1C4` | yes |
| `nss3.dll`, `mozavcodec.dll`, `libGLESv2.dll`, … | | `0x1C4` | yes |

`0x1C4` is `IMAGE_FILE_MACHINE_ARMNT` (32-bit ARM Thumb-2), the Windows 10
Mobile architecture. Every module carries
`IMAGE_DLL_CHARACTERISTICS_APPCONTAINER`.

This has **not been run on a device yet.** Building and linking is not the same
as working; see "What is untested" below.

---

## The decision that unblocked everything: the desktop Win32 partition

The earlier attempt built against `WINAPI_FAMILY_APP`, and that is what made
`widget/windows` look like a wall: no HWND, no GDI, so no widget backend.

That framing was wrong. The ARM32 Windows SDK ships import libraries for the
**entire desktop Win32 surface** — `kernel32`, `user32`, `gdi32`, `shlwapi`,
`version`, `wintrust`, `dbghelp` — and they really export `CreateFileW`,
`CreateWindowExW`, `CreateDCW`, `ReadProcessMemory`. `WINAPI_PARTITION_APP` is a
certification policy expressed in headers, not a kernel boundary.

So the build now uses `WINAPI_FAMILY_DESKTOP_APP` while still linking
`-APPCONTAINER`. That removed an entire class of work and, more importantly,
`widget/windows` (76 HWND/GDI files), `gfx`, `layout`, `dom` and `accessible`
all compile. Roughly a dozen stubs written for the app partition were reverted,
since they would have removed functionality the widget layer needs.

What remains are genuine **32-bit ARM** gaps, not UWP ones.

## Toolchain

VS2022 Enterprise (toolset 14.44), clang-cl 22 targeting
`thumbv7-windows-msvc`, lld-link, `armasm.exe`, Windows SDK **22621** (26100
dropped the 32-bit ARM libraries). Rust `thumbv7a-uwp-windows-msvc` is tier 3,
so std is built from source with `-Z build-std` and installed into the sysroot
(`tools/uwp-install-std.sh`).

Configure and build:

```bash
C:/mozilla-build/msys2/usr/bin/bash.exe -c tools/browser-configure.sh
C:/mozilla-build/msys2/usr/bin/bash.exe -c tools/browser-build.sh
```

### The objdir has to be short

`MOZ_OBJDIR=C:/rw-obj`, deliberately. The deepest libwebrtc object path,
resolved relative to `toolkit/library/build` as GNU make does it, comes to 262
characters — over `MAX_PATH`. GNU make 4.1 (what MozillaBuild ships here) has no
long-path manifest, so it reports `No rule to make target …` while linking
`xul.dll`, with the file sitting right there on disk. The old objdir location is
a junction to the new one.

## What had to be written, not just configured

### xptcall for Windows on ARM32

XPCOM's ABI glue had no Windows/ARM32 implementation at all — `md/win32`
covered x86, x86_64 and aarch64 only, so all 247 `nsXPTCStubBase::StubN` entry
points were undefined at link time. Three new files:

| file | what it is |
|---|---|
| `md/win32/xptcinvoke_arm.cpp` | the VFP half of the shared ARM implementation, which `md/unix/xptcinvoke_arm.cpp` keeps behind a Linux/iOS-only `#error`. clang reports `__ARM_PCS_VFP` for this target, so the hard-float path applies unchanged. |
| `md/win32/xptcstubs_arm.cpp` | `PrepareAndDispatch`, written for AAPCS-VFP: core registers r1–r3 with even-pair alignment for 64-bit values, stack overflow with 8-byte alignment, and VFP slots with back-filling. The Linux stubs do none of this because Android/Linux ARM builds are softfp. |
| `md/win32/xptcstubs_asm_arm.asm` | `SharedStub` plus the 247 thunks, in ARMASM syntax. `kxarm.h`'s `NESTED_ENTRY`/`PROLOG_*` macros are not in this SDK, so it uses plain `PROC`/`ENDP`. Symbols are MSVC-mangled: `?StubN@nsXPTCStubBase@@UAA?AW4nsresult@@XZ`. |

### 32-bit ARM support in the build system

Captured under `patches/w10m/` (65 files) plus four scripted bulk patches.

| area | fix |
|---|---|
| `build/moz.configure/uwp.configure` (new) | `--enable-uwp`: `MOZ_UWP`, `WINAPI_FAMILY_DESKTOP_APP`, `GECKO_W10M`, single-process |
| `windows.configure` | `_ARM_` cpu-arch macro; ARM-capable SDK selection; ATL skipped (this VS has no 32-bit ARM ATL); `HAVE_SEH_EXCEPTIONS` off for arm |
| `windows-toolchain.configure`, `toolchain.configure`, `rust.configure` | ARM32 compiler paths, `armasm.exe`, the `-uwp` Rust triple |
| `create_res.py` | `_ARM_` in the resource compiler's CPU map |
| `nscore.h`, `mfbt/ResultExtensions.h` | ARM32 has a single calling convention, so `__stdcall` specializations collide with the plain ones — same treatment Win64 already gets |
| interceptor (`PatcherDetour.h`, `TargetFunction.h`) | ARM32 branches: a Thumb-2 `LDR.W pc,[pc,#0]` veneer, and trampoline generation that refuses cleanly (relocating overwritten instructions needs a Thumb-2 decoder) |
| `mozglue/misc/StackWalk.cpp` | ARM32 table-driven unwinding via `RtlLookupFunctionEntry`/`RtlVirtualUnwind` (`ULONG_PTR`, not `ULONG64`) |
| `breakpad` | `RegisterValueType` for ARM32 |

### No SEH on 32-bit ARM

clang implements no structured exception handling for this target. Handled
centrally where possible — `HAVE_SEH_EXCEPTIONS` is off, so `MOZ_SEH_TRY`
degrades to plain blocks — and per library where the code uses raw `__try`:
`SQLITE_OMIT_SEH`, NSPR's thread namer, libwebrtc's thread namer, zucchini, and
the NEON probes in libaom/libvpx/pixman (every Windows-on-ARM32 device is ARMv7
with mandatory NEON, so the probe has one possible answer). `<comdef.h>` pulls
in `_com_ptr_t`, which uses `__try`; nothing in the tree uses those smart
pointers, so `-D_COM_SMARTPTR -D_COM_NO_STANDARD_GUIDS_` suppresses that half of
the header globally.

### A32 assembly is unusable: Windows on ARM32 is Thumb-2 only

Every hand-written 32-bit ARM assembly source in the tree is A32 (ARM mode).
The assembler rejects it outright (`target does not support ARM mode`,
`predicated instructions must be in IT block`). Each library falls back to its C
or NEON-intrinsics path instead:

- **libvpx** — `config/win/arm` generated with `HAVE_NEON_ASM=0`
  (`tools/gen-libvpx-win-arm-rtcd.sh` reruns libvpx's own `rtcd.pl`), and the
  routines that exist only in assembly for arm32 are taken from the
  NEON-intrinsics sources libvpx uses on arm64.
- **ffvpx** — no configuration existed for ARM32 Windows at all, and the
  `XP_WIN` branch fell through to `config_win32.h`, an *x86* config.
  `tools/gen-ffvpx-win32-arm-config.py` derives `config_win32_arm.h` from it
  with every x86 (and ARM) assembly switch off.
- **libwebrtc** — the GN translation defines `WEBRTC_HAS_NEON` in its per-CPU
  block while the NEON sources come from per-OS blocks that never fire for
  WINNT. `tools/patch-libwebrtc-win-arm32-neon.py` guards the define in 537
  generated `moz.build` files.
- **pixman, libpng, dav1d, libopus, gfx/ycbcr** — ARM asm paths disabled.
- **libwebp** — the opposite problem: `src/dsp/cpu.h` turns `WEBP_USE_NEON` on
  regardless of `BUILD_ARM_NEON`, so the NEON *intrinsics* sources now build.

### windows-rs has no Windows-on-ARM32 bindings

`tools/patch-windows-rs-arm32.py` (idempotent, covers both vendored crates):
adds `arm` to 508 architecture gates, leaves the `*LongPtr` aliases to the
32-bit path so the names do not collide, and writes out by hand the types that
have no ARM32 definition anywhere — `CONTEXT`, `SLIST_HEADER`,
`UNWIND_HISTORY_TABLE_ENTRY`, the minidump thread callbacks.

### One root cause worth calling out

`mfbt/Assertions.h` had been patched to include `<processthreadsapi.h>` for
`GetCurrentProcessId`. That header is where the Win32 A/W macros come from, and
`Assertions.h` is included by nearly everything, well before Gecko's
`<windows.h>` wrapper can undo them. It produced errors far away — an `override`
mismatch in `nsAppStartup` where the interface had been rewritten to
`GetStartupInfoW` — and an earlier `LPCTSTR` mismatch in the shell service.
Under the desktop partition `_getpid` is available again, so the patch was
reverted rather than worked around.

## What is disabled, and why

| disabled | reason |
|---|---|
| launcher process, DLL blocklist injection | desktop-only machinery; a UWP app is launched by the platform and cannot inject into other processes |
| Gecko's sandbox | the app container *is* the sandbox; Gecko's is Chromium NT-API code |
| updater | a UWP app is updated by replacing its package |
| crash reporter | a separate GUI process the app container cannot spawn; its Rust client cannot build (the manifest-embedding crate does not know this target) |
| js-ctypes | libffi has no Windows-on-ARM32 port: its ARM assembly implements the ELF/AAPCS variant. `js/src/ctypes` carries ARM32 ABI fixes in `patches/w10m` for whenever such a port exists |
| ML/llama.cpp backend | not meaningful on this device |
| IA2/MSAA marshaller DLLs | MIDL's 32-bit Windows proxy output guards its whole body with `!defined(_ARM_)`, so there is no marshaller to put in a DLL. The generated headers and IIDs, which the rest of `accessible/` consumes, are still built |
| ETW tracing | clang crashes on the TraceLogging macros for this target; the header's existing no-op path is used |
| debug symbols | clang cannot emit CodeView for ARM32 NEON register pairs (`D25_D26`) |

## Build cost, measured

`-j12` of 16 logical CPUs, `CARGO_BUILD_JOBS=8`, on 31 GB RAM. Sampled every 15
seconds across a full build (143 samples):

- **minimum free RAM: 4.5 GB**
- **peak pagefile: 1.8 GB**
- the single largest consumer is one `rustc` at **9.8 GB** compiling `gkrust`;
  11 concurrent `clang-cl` come to 6–8.6 GB together, 1.1–1.5 GB each.

There is no leak. An earlier session that exhausted memory and 60 GB of pagefile
was two browser-engine builds running at once — the other was a WebKit WinCairo
build from a separate Claude Code session, 17 × `cl.exe` at 9 GB.

## What is untested

The binaries have not been run. In particular:

- **The xptcall port has never executed.** It is new ABI code; a mistake there
  corrupts arguments rather than failing cleanly. It wants a targeted test
  (an XPCOM interface exercised with 64-bit and floating-point parameters,
  enough to cross the register/stack boundary) before anything else is trusted.
- The app-container runtime surface. Desktop Win32 links, but some calls may be
  refused at runtime inside the container; those show up as failing calls, not
  missing symbols.
- Whether Gecko's Windows widget layer actually drives a window on Windows 10
  Mobile, which has no classic desktop shell.
- Packaging: `firefox.exe` is a console/GUI executable, not an appx. The shell
  that hosts it (`test/JsSmoke` proved the engine embeds and packages) still has
  to be built around it.
