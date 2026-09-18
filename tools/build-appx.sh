#!/bin/bash
# Build and package the Gecko UWP shell for Windows 10 Mobile (ARM32).
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

# The build number is the fourth field of the version and belongs to the build,
# not to the author: it goes up on every build, is never reset and never
# reused. That is what makes "shell starting, version ..." in a device log name
# one package, and what makes installing over the previous one always work --
# Windows accepts only a strictly higher version.
COUNTER="$APP/build-number.txt"
[ -f "$COUNTER" ] || echo 0 > "$COUNTER"
BUILD="$(( $(tr -cd '0-9' < "$COUNTER") + 1 ))"
echo "$BUILD" > "$COUNTER"

BASE="$(grep -oE 'Version="[0-9]+\.[0-9]+\.[0-9]+' "$APP/Package.appxmanifest" | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')"
VERSION="$BASE.$BUILD"
# Write it back, so the tree records the package it produced.
python - "$(cygpath -w "$APP/Package.appxmanifest")" "$VERSION" <<'PY'
import re, sys
path, version = sys.argv[1], sys.argv[2]
text = open(path, encoding="utf-8").read()
text = re.sub(r'(<Identity[^>]*?Version=")[0-9.]+(")',
              lambda m: m.group(1) + version + m.group(2), text, count=1,
              flags=re.S)
open(path, "w", encoding="utf-8").write(text)
PY
echo "=== version $VERSION (build $BUILD) ==="
PKG="$OUT/Gecko_${VERSION}_ARM.appx"

rm -rf "$STAGE"; mkdir -p "$STAGE" "$OUT/obj"
cd "$APP"

STAGE_W="$(cygpath -w "$STAGE")"
OBJDIR_W="$(cygpath -w "$OUT/obj")"

echo "=== compile the shell (cl.exe, ARM, C++/WinRT) ==="
SRCS="pch.cpp App.cpp MainPage.cpp client/BrowserPreferences.cpp client/EngineView.cpp client/Log.cpp client/SearchEngines.cpp client/TabManager.cpp engine/CrashProbe.cpp engine/GeckoEngine.cpp engine/GeckoRuntimeHost.cpp engine/gecko_capi_stub.cpp"
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
  "/I$(cygpath -w "$DIST/../include")" \
  "/I$(cygpath -w "$ROOT/engine/firefox/toolkit/components/startup")"
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

echo "=== link Gecko.exe ==="
# mozglue.lib supplies moz_xmalloc and the rest of Gecko's allocator, which the
# bootstrap unit reaches through the mfbt headers. mozglue.dll already ships in
# the package, so this adds an import and no new payload.
"$LLVM/lld-link.exe" $OBJS "/OUT:$STAGE_W\\Gecko.exe" /APPCONTAINER \
  /SUBSYSTEM:WINDOWS,10.0 /ENTRY:wWinMainCRTStartup /MACHINE:ARM \
  "/MAP:$OBJDIR_W\\Gecko.map" \
  "/LIBPATH:$(cygpath -w "$DIST/../lib")" mozglue.lib \
  WindowsApp.lib
echo "    $(stat -c%s "$STAGE/Gecko.exe") bytes"

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
  # mobile-config-firefox: autoconfig at the root (the GRE directory, where
  # general.config.filename is looked for), modules and themes beside it,
  # policies in the app's distribution directory.
  MCF="$ROOT/vendor/mobile-config-firefox"
  if [ -d "$MCF" ]; then
    cp "$MCF/mobile-config-autoconfig.js" "$STAGE/mobile-config-autoconfig.js"
    mkdir -p "$STAGE/mobile-config-firefox" "$STAGE/browser/distribution"
    cp -r "$MCF/modules/." "$STAGE/mobile-config-firefox/"
    cp -r "$MCF/themes" "$STAGE/mobile-config-firefox/themes"
    cp "$MCF/policies.json" "$STAGE/browser/distribution/policies.json"
    echo "    mobile-config-firefox staged"
  fi
  echo "    engine payload from $DIST"
  # resource:/// and chrome://browser/ resolve against the directory holding
  # the application.ini that XRE_main is given, and for a browser build that
  # directory is browser/. The packaged tree has no application.ini there --
  # firefox.exe never needs one, it passes static app data instead -- so put
  # the root copy in place for the shell to point at.
  if [ -f "$STAGE/application.ini" ] && [ -d "$STAGE/browser" ]; then
    cp "$STAGE/application.ini" "$STAGE/browser/application.ini"
    echo "    application.ini staged into browser/ as the app directory"
  fi
  # Application default preferences for this port. The only one that matters
  # so far decides whether anything can be seen at all: the hardware
  # compositors present to a surface, and a headless widget has none, so the
  # software one -- which paints into a buffer the shell reads back -- is the
  # only one that reaches the screen here.
  if [ -d "$STAGE/browser/defaults/preferences" ]; then
    # THE NAME MATTERS. Gecko sorts this directory REVERSE-alphabetically and
    # applies the files in that order, so the alphabetically FIRST name is
    # applied LAST and wins (modules/libpref/Preferences.cpp,
    # pref_CompareFileNames). As "gecko_w10m.js" this file was applied before
    # firefox.js, which then quietly put its own defaults back: the home page,
    # the start page, first-run behaviour, resume-from-crash and
    # close-window-with-last-tab were all set here and none of them counted.
    # A leading "00-" puts this last in the queue and first in authority.
    cat > "$STAGE/browser/defaults/preferences/00-gecko.js" <<PREFS
// Gecko, Windows 10 Mobile. See tools/build-appx.sh.
// Back on. The control experiment answered: with the GPU path switched off
// entirely the window was still hidden, so nothing about the swap chain hides
// it -- but the software build died of its own separate fault on the way, in
// CompositorD3D11::Initialize, which is the line below.
// Software WebRender, hardware compositing left on. This is the one
// configuration that has never run to first paint: the earlier software
// control died in CompositorD3D11 before it got there, and that path is now
// closed by the pref below. The window is hidden at first paint with no
// panel, no swap chain and no GPU frame -- and the one thing every dying run
// has that every living run lacked is ANGLE and EGL coming up. With this true
// they never do: RenderCompositor::Create takes the software branch and
// falls through to RenderCompositorSWGL, while gfxConfig still says hardware
// compositing is on and the D3D11 device is still made. If the window lives,
// the hider is inside ANGLE's initialisation and the hardware path has one
// file to fix. If it is hidden anyway, it is the gfx configuration itself.
pref("gfx.webrender.software", false);

// Never the D3D11 software compositor, whichever way the pref above goes.
//
// RenderCompositorD3D11SWGL::Create builds a CompositorD3D11, and
// CompositorD3D11::Initialize reaches through the widget for an HWND. This
// widget is headless and has none, so it reads 0x1c out of a null pointer and
// the engine dies -- which is exactly what the software control did, on an
// NSPR thread, thirteen instructions into Initialize. It only became reachable
// when headless stopped force-disabling hardware compositing for the sake of
// the GPU path, so it is my regression, and this is the pref that closes it:
// CompositorOptions::AllowSoftwareWebRenderD3D11 gates that whole branch, and
// with it false the software path falls through to RenderCompositorSWGL, which
// wants no window at all.
pref("gfx.webrender.software.d3d11", false);
// Closing the last tab opens a fresh one instead of quitting the browser: on a
// phone a quit is a black screen and a relaunch, not something anyone asked for.
pref("browser.tabs.closeWindowWithLastTab", false);

// The start page. A default, so Settings > Home can change it and the change
// sticks in the profile.
pref("browser.startup.homepage", "https://google.com");

// The port's own version, for Settings > About. The engine's version is
// Firefox's and is displayed beside it.
pref("gecko.port.version", "$VERSION");
pref("browser.startup.page", 1);
// On a brand new profile Firefox skips the home page on purpose, because it
// normally shows its onboarding tour instead -- and that is off here, so the
// first launch after installing landed on about:blank. Do not skip it.
pref("browser.startup.firstrunSkipsHomepage", false);

// No gamepads on a phone, and looking for them is not free: the raw input
// calls the service uses are absent from this device's USER32, so it ended up
// allocating an array for an uninitialised device count and aborting the
// browser. Fixed in the stub as well; this switches off a service that has
// nothing to do here anyway.
pref("dom.gamepad.enabled", false);
pref("dom.gamepad.extensions.enabled", false);
pref("dom.gamepad.haptic_feedback.enabled", false);

// A phone suspends an app and then kills it, which Firefox cannot tell from a
// crash -- so it restored the previous session on every launch and the start
// page above was never reached. Start fresh instead. To keep tabs across a
// suspend instead of seeing the start page, set this back to true and
// browser.startup.page to 3.
pref("browser.sessionstore.resume_from_crash", false);

// Nothing else opens a tab of its own on a first run: no privacy-notice tab
// (that is the second tab that used to appear), no onboarding, no what's-new
// page after an update.
pref("datareporting.policy.firstRunURL", "");
pref("startup.homepage_welcome_url", "");
pref("startup.homepage_welcome_url.additional", "");
pref("startup.homepage_override_url", "");
pref("browser.aboutwelcome.enabled", false);
pref("browser.startup.upgradeDialog.enabled", false);
// No HTTP/3: QUIC over UDP on this phone's stack is an unknown, and a stuck
// QUIC attempt looks exactly like a page that never loads.
pref("network.http.http3.enable", false);

// mobile-config-firefox (postmarketOS): a phone-shaped chrome for desktop
// Firefox. Autoconfig loads its modules from mobile-config-firefox/ under the
// engine directory; see vendor/mobile-config-firefox/README-PORT.md.
pref("general.config.filename", "mobile-config-autoconfig.js");
pref("general.config.obscure_value", 0);
pref("general.config.sandbox_enabled", false);
pref("browser.uidensity", 2);
// A phone: touch events on, mobile viewport handling on, pinch zoom on.
pref("dom.w3c_touch_events.enabled", 1);
pref("dom.meta-viewport.enabled", true);
pref("apz.allow_zooming", true);
pref("apz.allow_double_tap_zooming", true);
pref("ui.touch.radius.enabled", true);

// The XAML compositor dies about two and a half seconds after these load, and
// the last thing in the log before it is always the same run: mozavcodec,
// mfplat, mf, dxva2, evr -- the media stack with hardware decoding, and the gfx
// sanity test trying to decode a video. The same libraries loaded in every
// software build and nothing happened; the difference now is that D3D11 is on,
// so DXVA takes a device.
//
// So this build does not ask. Hardware video decoding was already reported off
// as a feature, which did not stop the probe from happening.
// Hardware video decoding. Without it every frame above 360p is decoded by
// four phone cores and then converted from YV12 to RGB by the same cores,
// which is why 720p stuttered: the log said "wmf H264 codec software video
// decoder - no DXVA, yv12". With DXVA the Adreno decodes H.264, VP9 and AV1
// itself and hands the compositor an NV12 texture it can sample directly.
// force-enabled skips the blocklist, which has nothing to say about a phone
// GPU it has never heard of.
pref("media.hardware-video-decoding.enabled", true);
pref("media.hardware-video-decoding.force-enabled", true);
pref("media.wmf.dxva.enabled", true);
pref("media.wmf.dxva.d3d11.enabled", true);
pref("media.sanity-test.disabled", true);

// The Adreno in this phone decodes H.264 and nothing else: Windows 10 Mobile
// ships no VP9 or AV1 decoder, so those codecs land on the CPU, where four
// ARM32 cores manage about 360p. Sites that offer a choice -- YouTube offers
// VP9 first -- have to be told we cannot take it.
//
// Not with these prefs, though. Switching the codecs off browser-wide takes
// them from every site, including ones with nothing else to fall back to, and
// a site with no H.264 ladder has nothing left to play. h264ify has done this
// for years without that: it answers "no" to VP8, VP9 and AV1 inside the page,
// on YouTube only, and YouTube picks H.264 out of the ladder it was going to
// offer anyway. That is in the vendored config now; the hosts it does it on
// are gecko.h264ify.hosts, and everywhere else the browser keeps every codec
// Firefox ships with.
pref("gecko.h264ify.hosts", "youtube.com,youtube-nocookie.com");

// One hardware-decoded video at a time. The pre-roll ad on vkvideo.ru is a
// second <video> with a decoder of its own, and 16 ms after the second DXVA
// session was configured the Adreno's D3D11 driver went into an endless
// recursion and blew its stack -- 0xc00000fd, qcdx11um8996.dll+0x73503
// calling +0x71b73 calling +0x73503, all the way down. Upstream allows eight
// at once; this phone manages one. The ad is 360p and the CPU has never had
// trouble with 360p, so the one that gets the GPU is the one that needs it.
pref("media.wmf.dxva.max-videos", 1);

// And whatever decoders there are share a single D3D11 device instead of
// making one each. Firefox does this on Windows already; the blocklist is
// what kept it off here, and the blocklist has never heard of this GPU.
pref("gfx.direct3d11.reuse-decoder-device", true);
pref("gfx.direct3d11.reuse-decoder-device.force-enabled", true);

// The URL classifier's local database. The browser went silent for 79 seconds
// shortly after a cold start -- render thread included, taps queuing up and
// arriving all at once when it came back -- and the last thing in the engine
// log before the silence was "Classifier Update #1", writing and SHA-hashing
// several hundred files in a row. That is Safe Browsing keeping a multi-
// megabyte blocklist up to date, which a desktop does not notice and this
// phone's storage plainly does.
//
// It costs the warning page for known phishing and malware sites. Nothing
// else: certificates, mixed content, tracking protection and the permission
// prompts are all untouched.
pref("browser.safebrowsing.malware.enabled", false);
pref("browser.safebrowsing.phishing.enabled", false);
pref("browser.safebrowsing.downloads.enabled", false);
pref("browser.safebrowsing.provider.google4.updateURL", "");
pref("browser.safebrowsing.provider.google4.gethashURL", "");
pref("browser.safebrowsing.provider.mozilla.updateURL", "");
pref("browser.safebrowsing.provider.mozilla.gethashURL", "");
pref("urlclassifier.trackingTable", "");

// And the rest of what a fresh profile does over the network while the person
// is waiting for their first page: studies, experiments, telemetry pings.
pref("app.normandy.enabled", false);
pref("app.normandy.api_url", "");
pref("app.shield.optoutstudies.enabled", false);
pref("datareporting.healthreport.uploadEnabled", false);
pref("datareporting.policy.dataSubmissionEnabled", false);
pref("toolkit.telemetry.unified", false);
pref("toolkit.telemetry.archive.enabled", false);

// Canvas 2D was drawing on the CPU for a reason that does not apply here:
// upstream only accelerates it in the GPU process, and there is none, so the
// feature reported "Disabled by GPU Process disabled". allow-in-parent is the
// switch upstream added for exactly this case; force-enabled is again for the
// blocklist, which does not know this GPU.
// Partial present: WebRender redraws only what changed and trusts the swap
// chain to have kept the rest. On this ANGLE-on-D3D11 chain what comes back is
// not always what was left there -- the log shows frames rendered with one
// dirty rect, and the leftovers show up as specks of an older frame, white
// dots scattered over a dark photograph. Redrawing the whole frame costs fill
// rate this GPU can spare more easily than it can spare being wrong.
pref("gfx.webrender.max-partial-present-rects", 0);
pref("gfx.webrender.allow-partial-present-buffer-age", false);

// The sidebar's launcher strip is off. It is the vertical bar down the left
// edge with the tab list and the settings cog, and on a phone it is a column
// of chrome in front of the page.
pref("sidebar.revamp", false);
pref("sidebar.visibility", "hide-on-close");
pref("sidebar.verticalTabs", false);

pref("gfx.canvas.accelerated", true);
pref("gfx.canvas.accelerated.allow-in-parent", true);
pref("gfx.canvas.accelerated.force-enabled", true);
PREFS
    echo "    app default preferences staged"
  fi
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
  # lld-link synthesises the __imp_ pointer for a dllimport-declared symbol that
  # turns out to live in the same image, and on 32-bit ARM it synthesises it
  # without the Thumb bit -- so the indirect call through it lands in ARM state.
  # nss3.dll died at PR_CallOnce that way. See tools/fix-thumb-pointers.py.
  python "$(cygpath -w "$ROOT/tools/fix-thumb-pointers.py")" "${cfg_targets[@]}"     | sed 's/^/    /'
  # clang's 32-bit ARM virtual-call thunks tail-jump through r1, the first
  # argument register, so every pointer-to-member call on a virtual function
  # arrives with its first argument destroyed. The linker map is what finds
  # them: clang allocates registers differently from one thunk to the next, so
  # their bytes vary while the map names every one. See
  # tools/patch-vcall-thunks.py.
  python "$(cygpath -w "$ROOT/tools/patch-vcall-thunks.py")"     "$(cygpath -w "$STAGE/xul.dll")"     "$(cygpath -w "${DIST%/dist/bin}/toolkit/library/build/xul.map")"     | sed 's/^/    /'
  # ANGLE's two DLLs have the same thunks and, until 0.2.4.6, no map -- so they
  # went unpatched, and every glUniform* reached the D3D backend with the
  # location replaced by the thunk's address (a fast fail under the hardened
  # STL, and the reason hardware WebRender never survived its first frame).
  for angle in libGLESv2 libEGL; do
    map="${DIST%/dist/bin}/third_party/angle/${angle}_gn/${angle}.map"
    if [ -f "$map" ]; then
      python "$(cygpath -w "$ROOT/tools/patch-vcall-thunks.py")"         "$(cygpath -w "$STAGE/${angle}.dll")"         "$(cygpath -w "$map")"         | sed 's/^/    /'
    else
      echo "    WARNING: no linker map for ${angle}.dll -- its virtual-call thunks stay broken" >&2
    fi
  done
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
# Signed as CN=Computershik, which is what the manifest's Publisher says;
# the two must match exactly or the package will not install. Gecko.cer beside
# it is the certificate to trust on the phone.
CERT="$APP/Gecko.pfx"
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File "$(cygpath -w "$ROOT/tools/sign-appx.ps1")" \
  -Package "$(cygpath -w "$PKG")" \
  -Certificate "$(cygpath -w "$CERT")" \
  -SdkBin "$(cygpath -w "$BIN")"

# The certificate the package is signed with. A phone will not install a
# sideloaded package whose signer it does not trust, so this travels with it.
if [ -f "$APP/Gecko.cer" ]; then
  cp "$APP/Gecko.cer" "$OUT/Gecko.cer"
  cat > "$OUT/INSTALL.txt" <<'INSTALL'
Gecko for Windows 10 Mobile
===========================

The package needs nothing else installed: the C++ runtime it uses is inside
it, and there are no framework dependencies to fetch.

Two things the phone does need, and both are about the phone, not the package:

1. Trust the signing certificate. Copy Gecko.cer to the phone and open it,
   or install it through Interop Tools / WPinternals. Without this the
   installer refuses the package as untrusted, which is the usual reason a
   sideload fails on a device that has never seen this signer before.

2. Be interop-unlocked. The package asks for restricted capabilities --
   codeGeneration (the JIT, without which no modern web engine runs) and
   full file-system access for the profile. A stock, locked device will not
   grant them and the install fails.

Then install the .appx as usual (Device Portal, or an appx installer on the
phone). If an older build signed by a different publisher is installed, it is
a separate app: remove it if you do not want both.
INSTALL
  echo "    certificate and INSTALL.txt copied next to the package"
fi

echo
echo "PACKAGE: $PKG"
ls -la "$PKG" | awk '{printf "  %.1f MB\n", $5/1048576}'
