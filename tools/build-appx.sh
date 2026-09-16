#!/bin/bash
# Build and package the GeckoW10m UWP shell for Windows 10 Mobile (ARM32).
#
# The shell is a code-only C++/WinRT XAML app, compiled with MSVC cl.exe: clang
# implements no SEH on 32-bit ARM Windows and C++/WinRT needs C++ exceptions.
# The package carries the ported Gecko binaries from the browser objdir.
#
# MSBuild's appx signing step fails here with APPX0107, so the package is built
# with makeappx and signed separately with signtool.
#
# Every path handed to a Windows tool goes through cygpath: msys2 rewrites
# Unix-looking arguments, and MSYS2_ARG_CONV_EXCL only stops it mangling
# switches, not paths.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/app/GeckoW10m"
DIST="${GECKO_W10M_DIST:-C:/rw-obj/dist/bin}"
OUT="$ROOT/app/GeckoW10m/AppPackages"
STAGE="$OUT/stage"

VS="C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC/14.44.35207"
SDK="C:/Program Files (x86)/Windows Kits/10"
SDKV="10.0.22621.0"
CL="$VS/bin/Hostx64/arm/cl.exe"
LLVM="/c/Program Files/LLVM/bin"
BIN="$SDK/bin/$SDKV/x64"

export INCLUDE="$VS/include;$SDK/Include/$SDKV/ucrt;$SDK/Include/$SDKV/shared;$SDK/Include/$SDKV/um;$SDK/Include/$SDKV/winrt;$SDK/Include/$SDKV/cppwinrt"
export LIB="$VS/lib/arm/store;$VS/lib/arm;$SDK/Lib/$SDKV/ucrt/arm;$SDK/Lib/$SDKV/um/arm"
export PATH="$VS/bin/Hostx64/x64:$PATH"
export MSYS2_ARG_CONV_EXCL="*"

VERSION="$(grep -oE 'Version="[0-9.]+"' "$APP/Package.appxmanifest" | head -1 | grep -oE '[0-9.]+')"
PKG="$OUT/GeckoW10m_${VERSION}_ARM.appx"

rm -rf "$STAGE"; mkdir -p "$STAGE" "$OUT/obj"
cd "$APP"

STAGE_W="$(cygpath -w "$STAGE")"
OBJDIR_W="$(cygpath -w "$OUT/obj")"

echo "=== compile the shell (cl.exe, ARM, C++/WinRT) ==="
SRCS="pch.cpp App.cpp MainPage.cpp client/BrowserPreferences.cpp client/Log.cpp client/SearchEngines.cpp client/TabManager.cpp engine/CrashProbe.cpp engine/GeckoEngine.cpp engine/GeckoRuntimeHost.cpp engine/gecko_capi_stub.cpp"
OBJS=""
for s in $SRCS; do
  name="$(echo "$s" | tr '/' '_' | sed 's/\.cpp$/.obj/')"
  "$CL" /nologo /c "$(cygpath -w "$APP/$s")" "/Fo:$OBJDIR_W\\$name" \
        /std:c++20 /EHsc /GR- /O2 /utf-8 \
        /DGECKO_W10M_USE_ENGINE_STUB /D_ARM_ /DWIN32 /D_WIN32 /DNOMINMAX \
        /DUNICODE /D_UNICODE /DWINAPI_FAMILY=WINAPI_FAMILY_APP \
        "/I$(cygpath -w "$APP")" "/I$(cygpath -w "$SDK/Include/$SDKV/cppwinrt")"
  OBJS="$OBJS $OBJDIR_W\\$name"
done

echo "=== compile the Gecko bootstrap (clang-cl, ARM) ==="
# This one translation unit includes Gecko headers, and that decides its
# compiler. mfbt uses clang builtins cl.exe does not have (__builtin_unreachable
# among them), so it must be clang-cl -- and clang emits no C++ exception
# handling at all on 32-bit ARM Windows, so exceptions must be off. Gecko is
# built without them anyway. The rest of the shell is the mirror image: cl.exe
# with exceptions on, because C++/WinRT requires both. The two halves meet at a
# plain C boundary in gecko_bootstrap.h.
#
# gecko_w10m_arm_intrin.h fills in the MSVC ARM intrinsics the STL calls and clang
# does not provide; the engine build force-includes it for the same reason.
"$LLVM/clang-cl.exe" --target=thumbv7-windows-msvc /nologo /c \
  "$(cygpath -w "$APP/engine/gecko_bootstrap.cpp")" \
  "/Fo:$OBJDIR_W\\engine_gecko_bootstrap.obj" \
  /std:c++20 /EHs-c- /GR- /O2 /utf-8 -FIgecko_w10m_arm_intrin.h \
  /DWIN32 /D_WIN32 /DNOMINMAX /DUNICODE /D_UNICODE \
  /DWINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP /DXP_WIN /DGECKO_W10M=1 \
  "/I$(cygpath -w "$DIST/../include")"
OBJS="$OBJS $OBJDIR_W\\engine_gecko_bootstrap.obj"

echo "=== compat stubs ==="
# Windows 10 Mobile does not carry every desktop module xul.dll imports, and a
# single missing one stops the whole engine from loading. Each stub here stands
# in for one such module; see the source for what it replaces and why failing
# is the right answer. They are linked /NODEFAULTLIB against kernel32 alone so
# a stub never drags in a runtime of its own.
for stub in ktmw32; do
  "$CL" /nologo /c "$(cygpath -w "$APP/compat/$stub.c")" \
        "/Fo:$OBJDIR_W\\$stub.obj" /O2 /MT /GS- \
        /D_ARM_ /DWIN32 /D_WIN32 /DWINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP
  "$LLVM/lld-link.exe" "$OBJDIR_W\\$stub.obj" /DLL /APPCONTAINER \
    /MACHINE:ARM /NODEFAULTLIB /ENTRY:DllMain \
    "/DEF:$(cygpath -w "$APP/compat/$stub.def")" \
    "/OUT:$STAGE_W\\$stub.dll" \
    kernel32.lib
  echo "    $stub.dll ($(stat -c%s "$STAGE/$stub.dll") bytes)"
done

echo "=== link GeckoW10m.exe ==="
# mozglue.lib supplies moz_xmalloc and the rest of Gecko's allocator, which the
# bootstrap unit reaches through the mfbt headers. mozglue.dll already ships in
# the package, so this adds an import and no new payload.
"$LLVM/lld-link.exe" $OBJS "/OUT:$STAGE_W\\GeckoW10m.exe" /APPCONTAINER \
  /SUBSYSTEM:WINDOWS,10.0 /ENTRY:wWinMainCRTStartup /MACHINE:ARM \
  "/MAP:$OBJDIR_W\\GeckoW10m.map" \
  "/LIBPATH:$(cygpath -w "$DIST/../lib")" mozglue.lib \
  WindowsApp.lib
echo "    $(stat -c%s "$STAGE/GeckoW10m.exe") bytes"

echo "=== stage the package ==="
cp "$APP/Package.appxmanifest" "$STAGE/AppxManifest.xml"
mkdir -p "$STAGE/Assets"
cp "$APP/Assets/"*.png "$STAGE/Assets/"

# The C runtime. $VS/bin/Hostx64/arm holds the x64 binaries the cross-compiler
# itself runs on, NOT the ARM runtime -- staging from there shipped an x64
# vcruntime140.dll, and the device's loader answered mozglue.dll with
# ERROR_BAD_EXE_FORMAT (193) and then xul.dll with ERROR_MOD_NOT_FOUND (126).
#
# The right source is the UWP runtime out of the VCLibs framework package:
# ARM32 and built with /APPCONTAINER, unlike the desktop redist. Its DLLs carry
# an _app suffix and reference each other by that name, while mozglue and xul
# import the plain names, so both spellings go in the package.
VCLIBS="C:/Program Files (x86)/Microsoft SDKs/Windows Kits/10/ExtensionSDKs/Microsoft.VCLibs/14.0/Appx/Retail/ARM/Microsoft.VCLibs.arm.14.00.appx"
if [ -f "$VCLIBS" ]; then
  CRTTMP="$APP/AppPackages/crt"
  rm -rf "$CRTTMP" && mkdir -p "$CRTTMP"
  powershell.exe -NoProfile -Command "
    Add-Type -AssemblyName System.IO.Compression.FileSystem;
    \$zip = [System.IO.Compression.ZipFile]::OpenRead('$(cygpath -w "$VCLIBS")');
    \$zip.Entries | Where-Object { \$_.Name -like '*.dll' } | ForEach-Object {
      [System.IO.Compression.ZipFileExtensions]::ExtractToFile(\$_,
        (Join-Path '$(cygpath -w "$CRTTMP")' \$_.Name), \$true) };
    \$zip.Dispose()" >/dev/null
  for f in "$CRTTMP"/*.dll; do
    b=$(basename "$f")
    cp "$f" "$STAGE/$b"
    # Same bytes under the name the engine imports: vcruntime140_app.dll also
    # answers to vcruntime140.dll.
    plain=$(echo "$b" | sed 's/_app\.dll$/.dll/')
    [ "$plain" != "$b" ] && cp "$f" "$STAGE/$plain"
  done
  rm -rf "$CRTTMP"
  echo "    CRT from VCLibs 14.00 ARM (UWP, appcontainer)"
else
  echo "    WARNING: VCLibs ARM package not found, no CRT staged" >&2
fi

# The ported engine.
if [ -d "$DIST" ]; then
  cp -r "$DIST/." "$STAGE/"
  echo "    engine payload from $DIST"
  # Control Flow Guard, as lld-link emits it for 32-bit ARM, is wrong: the
  # guard dispatcher reaches its target with bx, and an entry recorded without
  # the Thumb bit lands there in ARM state, where the first honest Thumb
  # instruction is an illegal one. Every module we link needs the flag cleared,
  # not just xul.dll -- nss3.dll died this way at the entry of PR_CallOnce.
  # The CRT staged from VCLibs is Microsoft's own build and is left alone.
  # See tools/strip-cfg.py.
  cfg_targets=()
  while IFS= read -r rel; do
    cfg_targets+=("$(cygpath -w "$STAGE/$rel")")
  done < <(cd "$DIST" && find . \( -name '*.dll' -o -name '*.exe' \) -printf '%P\n')
  python "$(cygpath -w "$ROOT/tools/strip-cfg.py")" "${cfg_targets[@]}"     | sed 's/^/    /'
  # clang's 32-bit ARM virtual-call thunks tail-jump through r1, the first
  # argument register, so every pointer-to-member call on a virtual function
  # arrives with its first argument destroyed. The linker map is what finds
  # them: clang allocates registers differently from one thunk to the next, so
  # their bytes vary while the map names every one. See
  # tools/patch-vcall-thunks.py.
  python "$(cygpath -w "$ROOT/tools/patch-vcall-thunks.py")"     "$(cygpath -w "$STAGE/xul.dll")"     "$(cygpath -w "${DIST%/dist/bin}/toolkit/library/build/xul.map")"     | sed 's/^/    /'
else
  echo "    WARNING: $DIST not found, packaging the shell alone" >&2
fi
find "$STAGE" -name "*.pdb" -delete 2>/dev/null || true
echo "    staged $(find "$STAGE" -type f | wc -l) files, $(du -sm "$STAGE" | cut -f1) MB"

echo "=== pack ==="
rm -f "$PKG"
"$BIN/makeappx.exe" pack /d "$STAGE_W" /p "$(cygpath -w "$PKG")" /o /nv

echo "=== sign ==="
# Signing runs through PowerShell because msys2 leaves TEMP and TMP empty in
# the environment it hands to children, and signtool needs a temp directory to
# repack an appx. See tools/sign-appx.ps1.
CERT="$APP/GeckoW10m.pfx"
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File "$(cygpath -w "$ROOT/tools/sign-appx.ps1")" \
  -Package "$(cygpath -w "$PKG")" \
  -Certificate "$(cygpath -w "$CERT")" \
  -SdkBin "$(cygpath -w "$BIN")"

echo
echo "PACKAGE: $PKG"
ls -la "$PKG" | awk '{printf "  %.1f MB\n", $5/1048576}'
