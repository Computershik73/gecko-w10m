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
SRCS="pch.cpp App.cpp MainPage.cpp client/BrowserPreferences.cpp client/Log.cpp client/SearchEngines.cpp client/TabManager.cpp engine/GeckoEngine.cpp engine/gecko_capi_stub.cpp"
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

echo "=== link GeckoW10m.exe ==="
"$LLVM/lld-link.exe" $OBJS "/OUT:$STAGE_W\\GeckoW10m.exe" /APPCONTAINER \
  /SUBSYSTEM:WINDOWS,10.0 /ENTRY:wWinMainCRTStartup /MACHINE:ARM \
  WindowsApp.lib
echo "    $(stat -c%s "$STAGE/GeckoW10m.exe") bytes"

echo "=== stage the package ==="
cp "$APP/Package.appxmanifest" "$STAGE/AppxManifest.xml"
mkdir -p "$STAGE/Assets"
cp "$APP/Assets/"*.png "$STAGE/Assets/"

# The C runtime matching the toolset that built both the shell and the engine.
for c in msvcp140.dll msvcp140_1.dll msvcp140_2.dll vcruntime140.dll concrt140.dll; do
  [ -f "$VS/bin/Hostx64/arm/$c" ] && cp "$VS/bin/Hostx64/arm/$c" "$STAGE/"
done

# The ported engine.
if [ -d "$DIST" ]; then
  cp -r "$DIST/." "$STAGE/"
  echo "    engine payload from $DIST"
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
