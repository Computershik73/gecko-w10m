#!/bin/bash
# Package the engine (mach package) into $GECKO_W10M_OBJ/dist/firefox: two
# omni.ja archives instead of eight thousand loose files, and a preload list
# ordered by startup use. tools/build-appx.sh takes the engine from there.
#
# mach package goes on, after the zip, to make the desktop Windows installer,
# which fails here ("windows: No such file or directory"). That step is not
# ours; success is dist/firefox being complete, which is checked instead of the
# exit status.
source "$(dirname "$0")/env.sh" || exit 1
cd "$GECKO_W10M_ROOT/engine/firefox"
MOZ_BUILD_DATE="$(gecko_w10m_build_date)"
export MOZ_BUILD_DATE
echo "=== browser PACKAGE start $(date), MOZ_BUILD_DATE=$MOZ_BUILD_DATE ==="
export CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-$(( GECKO_W10M_JOBS * 2 / 3 ))}"
export MOZ_PKG_FATAL_WARNINGS=0
# omni.ja deflated, as Firefox for Android has it, not stored as on the
# desktop (toolkit/mozapps/installer/upload-files.mk: JAR_COMPRESSION ?= none
# outside Android). Both archives are mapped whole into the process: stored,
# that was 94 MB of a 32-bit process's 2 GB address space; deflated, 29. On
# the HP Elite x3 Discord after login ran out of address space with
# hundreds of MB free in pieces. Entries are inflated when first read; the
# startup cache and the script preloader keep most of that off later starts.
export JAR_COMPRESSION=deflate
rm -f "$GECKO_W10M_OBJ/dist/firefox/omni.ja"
./mach package < /dev/null 2>&1
echo "=== mach package exit=$? at $(date) ==="
for f in omni.ja browser/omni.ja xul.dll; do
  if [ ! -f "$GECKO_W10M_OBJ/dist/firefox/$f" ]; then
    echo "=== package FAILED: dist/firefox/$f is missing ===" >&2
    exit 1
  fi
done
echo "=== package ok: $GECKO_W10M_OBJ/dist/firefox ==="
