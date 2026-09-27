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
