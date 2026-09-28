#!/bin/bash
# Refresh patches/w10m from the engine branch.
#
# Every commit on the engine's port branch since the upstream release tag in
# engine/release.txt becomes one file (git format-patch), and the old files go.
# Run it after committing to the engine, then commit patches/w10m together
# with the submodule update, so the two never disagree.
#
# --no-numbered keeps "[PATCH n/m]" out of the subjects, so adding a commit
# changes one file here, not all of them.
#
# Before replacing anything, the new series is replayed onto the release tag
# in a scratch index -- nothing is checked out -- and the resulting tree must
# be the branch's tree exactly.
#
# Usage:
#   tools/export-patches-w10m.sh [branch]      # default: w10m-port
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GECKO="$ROOT/engine/firefox"
TAG="$(awk 'NR == 1 { print $1 }' "$ROOT/engine/release.txt")"
BRANCH="${1:-w10m-port}"
OUT="$ROOT/patches/w10m"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

git -C "$GECKO" format-patch --binary --no-signature --zero-commit --no-numbered -q \
  -o "$WORK/series" "$TAG..$BRANCH"

# Check the series against the branch.
INDEX="$WORK/index"
command -v cygpath > /dev/null && INDEX="$(cygpath -w "$INDEX")"
GIT_INDEX_FILE="$INDEX" git -C "$GECKO" read-tree "$TAG"
for patch in "$WORK"/series/*.patch; do
  GIT_INDEX_FILE="$INDEX" git -C "$GECKO" apply --cached --binary \
    --whitespace=nowarn "$patch"
done
GOT="$(GIT_INDEX_FILE="$INDEX" git -C "$GECKO" write-tree)"
WANT="$(git -C "$GECKO" rev-parse "$BRANCH^{tree}")"
if [ "$GOT" != "$WANT" ]; then
  echo "The exported series does not rebuild $BRANCH ($GOT, want $WANT)." >&2
  exit 1
fi

rm -f "$OUT"/*.patch
mkdir -p "$OUT"
cp "$WORK"/series/*.patch "$OUT"/
echo "patches/w10m: $(ls "$OUT"/*.patch | wc -l) patches, $TAG..$BRANCH ($(git -C "$GECKO" rev-parse --short "$BRANCH")), tree verified."
