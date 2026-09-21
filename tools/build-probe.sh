#!/bin/bash
# Builds GeckoProbe_<n>_ARM.appx: the loader probe as its own package.
#
# Runs after tools/build-appx.sh, because it takes the DLLs it probes from the
# browser's staged tree -- the CRT copies, mozglue, nss3, ktmw32 and the
# api-set forwarder shims -- so the probe sees exactly the bytes the browser
# ships. Probe.exe itself is Win32 only, static CRT, linked against the 1607
# SDK's libraries like the shell.
set -euo pipefail
# Git-bash turns /nologo into C:/Program Files/Git/nologo unless told not to.
export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL="*"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/app/GeckoW10m"
BROWSER_STAGE="$APP/AppPackages/stage"
OUT="$APP/AppPackages"
STAGE="$OUT/probe-stage"
OBJ="$OUT/probe-obj"

VS="C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC/14.44.35207"
SDK="C:/Program Files (x86)/Windows Kits/10"
SDKV="10.0.22621.0"
SDKV_LIB="${GECKO_W10M_SDK_LIB:-10.0.14393.0}"
CL="$VS/bin/Hostx64/arm/cl.exe"
LLVM="/c/Program Files/LLVM/bin"
BIN="$SDK/bin/$SDKV/x64"
export INCLUDE="$VS/include;$SDK/Include/$SDKV/ucrt;$SDK/Include/$SDKV/shared;$SDK/Include/$SDKV/um;$SDK/Include/$SDKV/winrt"
export LIB="$VS/lib/arm;$SDK/Lib/$SDKV_LIB/ucrt/arm;$SDK/Lib/$SDKV_LIB/um/arm"

[ -f "$BROWSER_STAGE/mozglue.dll" ] || { echo "run tools/build-appx.sh first: no browser stage" >&2; exit 1; }

# One counter, shared with the browser's build number, so the version says
# which browser build the DLLs came from.
BUILD=$(cat "$APP/build-number.txt" 2>/dev/null || echo 0)
VERSION="1.0.0.$BUILD"
PKG="$OUT/GeckoProbe_${VERSION}_ARM.appx"

rm -rf "$STAGE" "$OBJ"; mkdir -p "$STAGE/Assets" "$OBJ"
STAGE_W="$(cygpath -w "$STAGE")"
OBJ_W="$(cygpath -w "$OBJ")"

echo "=== compile Probe.exe (cl.exe, ARM, static CRT, SDK $SDKV_LIB libraries) ==="
"$CL" /nologo /c "$(cygpath -w "$APP/probe/Probe.c")" "/Fo:$OBJ_W\\Probe.obj" \
      /TP /EHs-c- /GR- /MT /O1 /D_ARM_ /DWIN32 /D_WIN32 /DUNICODE /D_UNICODE \
      /DWINAPI_FAMILY=WINAPI_FAMILY_APP
"$LLVM/lld-link.exe" "$OBJ_W\\Probe.obj" "/OUT:$STAGE_W\\Probe.exe" /APPCONTAINER \
  /SUBSYSTEM:WINDOWS,10.0 /ENTRY:wWinMainCRTStartup /MACHINE:ARM   /NODEFAULTLIB:kernel32.lib WindowsApp.lib OneCoreUap.lib
# No kernel32.lib: the static CRT names it as a default library, and every
# symbol taken from it becomes an import of kernel32.dll -- a file Windows 10
# Mobile 1607 does not have (the loader ended Probe.exe with
# STATUS_DLL_NOT_FOUND after mapping only ntdll and KernelBase). Resolved
# from the umbrella libraries instead, the same functions are api-set
# imports, which that OS does have.
echo "    Probe.exe ($(stat -c%s "$STAGE/Probe.exe") bytes)"

echo "=== stage ==="
sed "s/Version=\"1.0.0.0\"/Version=\"$VERSION\"/" "$APP/probe/Package.appxmanifest" > "$STAGE/AppxManifest.xml"
cp "$APP/Assets/"*.png "$STAGE/Assets/"
for f in vcruntime140.dll msvcp140.dll mozglue.dll nss3.dll ktmw32.dll; do
  cp "$BROWSER_STAGE/$f" "$STAGE/$f"
done
cp "$BROWSER_STAGE"/api-ms-win-*.dll "$BROWSER_STAGE"/ext-ms-win-*.dll "$STAGE/" 2>/dev/null || true
# The forwarders for the legacy names the phone lacks, so the probe's load of
# mozglue, nss3 and xul is a dry run of the browser's fix.
for f in kernl32.dll advap32.dll ole3x.dll vers1on.dll dbghlp.dll; do
  [ -f "$BROWSER_STAGE/$f" ] && cp "$BROWSER_STAGE/$f" "$STAGE/$f"
done
echo "    $(ls "$STAGE" | grep -c '^api-ms\|^ext-ms') shims, $(du -sm "$STAGE" | cut -f1) MB"

PRITMP="$OUT/probe-pri"
rm -rf "$PRITMP" && mkdir -p "$PRITMP/Assets"
cp "$STAGE/AppxManifest.xml" "$PRITMP/" && cp "$STAGE/Assets/"* "$PRITMP/Assets/"
"$BIN/makepri.exe" createconfig /cf "$(cygpath -w "$PRITMP/priconfig.xml")" /dq en-US /pv 10.0.0 /o >/dev/null
"$BIN/makepri.exe" new /pr "$(cygpath -w "$PRITMP")" /cf "$(cygpath -w "$PRITMP/priconfig.xml")" \
    /mn "$(cygpath -w "$PRITMP/AppxManifest.xml")" /of "$(cygpath -w "$STAGE/resources.pri")" /o >/dev/null
rm -rf "$PRITMP"

echo "=== pack and sign ==="
rm -f "$PKG"
"$BIN/makeappx.exe" pack /d "$STAGE_W" /p "$(cygpath -w "$PKG")" /o /nv >/dev/null
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File "$(cygpath -w "$ROOT/tools/sign-appx.ps1")" \
  -Package "$(cygpath -w "$PKG")" \
  -Certificate "$(cygpath -w "$APP/Gecko.pfx")" \
  -SdkBin "$(cygpath -w "$BIN")" | grep -iE "signed|error" || true
echo "PROBE PACKAGE: $PKG ($(stat -c%s "$PKG") bytes)"
