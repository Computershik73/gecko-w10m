#!/bin/bash
# From a checkout with the engine in place (see README) to a signed appx:
#
#   1. windows-rs fetched and patched for ARM32     (tools/prepare-windows-rs.sh)
#   2. Rust std for the arm-uwp target, if missing  (tools/uwp-install-std.sh)
#   3. the engine                                   (tools/browser-build.sh)
#   4. the engine packaged into dist/firefox        (tools/package.sh)
#   5. the shell compiled, the appx packed, signed  (tools/build-appx.sh)
#
# Stops at the first step that fails. Locations: tools/env.sh.
set -e
TOOLS="$(cd "$(dirname "$0")" && pwd)"
# Each step runs in this same bash. env.sh puts MozillaBuild's msys first on
# PATH, and a plain "bash" would then be MozillaBuild's: started from another
# msys runtime (Git Bash's, say) it inherits none of the exported variables --
# GECKO_W10M_OBJ included, so the step would build into the default objdir.
SH="$BASH"
source "$TOOLS/env.sh"

if [ ! -f "$GECKO_W10M_ROOT/engine/firefox/mach" ]; then
  echo "No engine in engine/firefox: see README, 'Get the engine'." >&2
  exit 1
fi

echo "##### 1/5 windows-rs"
"$SH" "$TOOLS/prepare-windows-rs.sh"

echo "##### 2/5 Rust std for thumbv7a-uwp-windows-msvchf"
TARGET_DIR="$(cygpath -u "$(rustc --print sysroot)")/lib/rustlib/thumbv7a-uwp-windows-msvchf"
if ls "$TARGET_DIR"/lib/libstd-*.rlib > /dev/null 2>&1 && [ -f "$TARGET_DIR/target.json" ]; then
  echo "already installed in $TARGET_DIR"
else
  "$SH" "$TOOLS/uwp-install-std.sh"
fi

echo "##### 3/5 engine"
"$SH" "$TOOLS/browser-build.sh" | tee "$TMPDIR/build.log"
grep -q "=== build exit=0" "$TMPDIR/build.log" || { echo "engine build failed; log: $TMPDIR/build.log" >&2; exit 1; }

echo "##### 4/5 package"
"$SH" "$TOOLS/package.sh" > "$TMPDIR/package.log" 2>&1 || { tail -20 "$TMPDIR/package.log"; echo "package failed; log: $TMPDIR/package.log" >&2; exit 1; }
tail -1 "$TMPDIR/package.log"

echo "##### 5/5 appx"
"$SH" "$TOOLS/build-appx.sh"
