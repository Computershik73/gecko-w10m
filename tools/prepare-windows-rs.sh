#!/bin/bash
# Fetch the windows crate (windows-rs 0.62.2) that the engine build takes from
# MOZ_WINDOWS_RS_DIR, check it against the crates.io index, and patch it for
# 32-bit ARM Windows with tools/patch-windows-rs-arm32.py. The result lands in
# engine/third_party/windows-0.62.2 (not tracked: it is generated).
#
# Run once after cloning. It does nothing if the crate is already there.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=0.62.2
SHA256=527fadee13e0c05939a6a05d5bd6eec6cd2e3dbd648b9f8e447c6518133d8580
DEST="$ROOT/engine/third_party"
CRATE="$DEST/windows-$VERSION.crate"

if [ -d "$DEST/windows-$VERSION/src" ]; then
  echo "windows-$VERSION is already in engine/third_party"
  exit 0
fi

mkdir -p "$DEST"
if [ ! -f "$CRATE" ]; then
  curl -sSfL -o "$CRATE" "https://static.crates.io/crates/windows/windows-$VERSION.crate"
fi
echo "$SHA256  $CRATE" | sha256sum -c --quiet
tar -xzf "$CRATE" -C "$DEST"
python "$ROOT/tools/patch-windows-rs-arm32.py" "$DEST/windows-$VERSION/src"
echo "windows-$VERSION fetched and patched into engine/third_party"
