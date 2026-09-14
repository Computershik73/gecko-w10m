# JitProbe — verify UWP JIT before building the engine

A tiny standalone UWP app that confirms the `codeGeneration` restricted
capability actually grants executable memory on your device. Run this first; if
it fails, the full Gecko build cannot JIT and there is no point compiling it yet.

## What it does

On launch it:

1. Commits a page with `VirtualAllocFromApp(PAGE_READWRITE)`.
2. Writes a tiny `return 42` function (Thumb-2 on ARM, native code on x86/x64).
3. Flips it to `PAGE_EXECUTE_READ` with `VirtualProtectFromApp` and flushes the
   instruction cache.
4. **Calls it** and checks it returns 42.
5. Toggles the page back to writable (W^X), rewrites `return 7`, re-executes.
6. Confirms a direct `PAGE_EXECUTE_READWRITE` allocation is refused (expected —
   SpiderMonkey never needs RWX).

A green **JIT: PASS** banner means the engine's JIT will work. Red **JIT: FAIL**
shows exactly which step failed (usually step 3, meaning `codeGeneration` is not
active because the package was not sideloaded with the capability honoured).

## Build & sign (verified commands)

Built with VS2017 Enterprise, compiled against the Windows 10 SDK 10.0.17763.0
cppwinrt headers (runtime floor stays 10586 via the manifest). The MSBuild
signing step is finicky about self-signed certs (APPX0107), so sign the produced
`.appx` directly with `signtool` — that works.

```powershell
$msbuild  = "C:\Program Files (x86)\Microsoft Visual Studio\2017\Enterprise\MSBuild\15.0\Bin\MSBuild.exe"
$signtool = "C:\Program Files (x86)\Windows Kits\10\bin\x86\signtool.exe"

# One-time: a code-signing cert whose subject matches the manifest Publisher.
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=GeckoW10m" `
  -CertStoreLocation "Cert:\CurrentUser\My" -NotAfter (Get-Date).AddYears(5)
$pwd = ConvertTo-SecureString "gecko_w10m" -Force -AsPlainText
Export-PfxCertificate -Cert "Cert:\CurrentUser\My\$($cert.Thumbprint)" -FilePath JitProbe_TemporaryKey.pfx -Password $pwd
Export-Certificate  -Cert "Cert:\CurrentUser\My\$($cert.Thumbprint)" -FilePath JitProbe.cer   # import on the phone

# ARM (the phone). Build unsigned, then sign the package.
& $msbuild JitProbe.vcxproj /p:Configuration=Release /p:Platform=ARM /p:AppxPackageSigningEnabled=false
& $signtool sign /fd SHA256 /f JitProbe_TemporaryKey.pfx /p gecko_w10m `
  AppPackages\JitProbe\JitProbe_0.1.0.0_ARM_Test\JitProbe_0.1.0.0_ARM.appx
```

Output: `AppPackages\JitProbe\JitProbe_0.1.0.0_ARM_Test\` containing
`JitProbe_0.1.0.0_ARM.appx` plus `Dependencies\ARM\Microsoft.VCLibs.ARM.14.00.appx`.

## Deploy to the phone

1. Import `JitProbe.cer` into the device's Trusted Root (or rely on interop
   unlock, which trusts sideloaded packages).
2. Install `Dependencies\ARM\Microsoft.VCLibs.ARM.14.00.appx`.
3. Install `JitProbe_0.1.0.0_ARM.appx`.
4. Launch. Green **JIT: PASS** = `codeGeneration` executable memory works.

## Logic already validated on this PC

Compiled `jit_probe.cpp` as a native x86 console program and ran it:

```
[ ok ] VirtualAllocFromApp(PAGE_READWRITE)
[ ok ] VirtualProtectFromApp(PAGE_EXECUTE_READ) + FlushInstructionCache
[ ok ] executed JIT code, returned 42 (expected 42)
[ ok ] W^X toggle re-executed, returned 7 (expected 7)
[ ok ] direct RWX alloc refused as expected (W^X enforced).
RESULT: PASS
```

So the probe mechanism is correct; the phone run confirms the container grants
the capability. (A desktop PASS is necessary but not sufficient — desktop UWP is
more permissive, so the ARM-on-device run is the real test.)

Tile assets in `Assets\` are simple colored placeholders generated at build
time; they are not required for the probe to run.

## Gotcha: the entry-thread apartment

A hand-authored C++/WinRT UWP `wWinMain` starts with **no COM apartment**
(`CoGetApartmentType` returns `CO_E_NOTINITIALIZED`). It must initialize the
entry thread as **MTA** before `Application::Start`:

```cpp
winrt::init_apartment(winrt::apartment_type::multi_threaded);
Application::Start([](auto&&){ winrt::make<App>(); });
```

`Application::Start` then creates the ASTA UI thread itself (the same model as
C++/CX's `[Platform::MTAThread] int main`). Leaving the thread uninitialized, or
making it STA/ASTA, makes CoreApplication fail its main-thread check and throw
`RPC_E_WRONG_THREAD` (0x8001010E) before `OnLaunched` — the app dies instantly
at launch. This was diagnosed with `cdb` (`sxd eh; g` to catch the second-chance
exception) against the packaged app.
