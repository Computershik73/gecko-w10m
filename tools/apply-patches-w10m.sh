#!/bin/bash
# Put the Windows 10 Mobile port on a plain Firefox tree.
#
# patches/w10m holds the port as a series of commits (git format-patch) on top
# of the upstream release tag named in engine/release.txt. This checks out that
# tag on a new branch and replays the series onto it with `git am`, keeping
# authors, dates and messages -- the result is the same tree, byte for byte,
# as the branch the patches were exported from.
#
# Usage:
#   tools/apply-patches-w10m.sh [path-to-firefox-clone]
#
# The clone defaults to engine/firefox. It only needs the release tag:
#   git clone --depth 1 --branch FIREFOX_155_0_1_RELEASE \
#       https://github.com/mozilla-firefox/firefox engine/firefox
#
# Line endings: the patches carry CRLF and LF lines as they are. Firefox's
# .gitattributes turns conversion off (* -text), and `--keep-cr` stops
# git am from stripping the carriage returns on the way in.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GECKO="${1:-$ROOT/engine/firefox}"
TAG="$(awk 'NR == 1 { print $1 }' "$ROOT/engine/release.txt")"
BRANCH="${GECKO_W10M_ENGINE_BRANCH:-w10m-port}"

if ! git -C "$GECKO" rev-parse -q --verify "$TAG^{commit}" > /dev/null; then
  echo "$GECKO has no $TAG. Fetch it first:" >&2
  echo "  git -C \"$GECKO\" fetch --depth 1 origin tag $TAG" >&2
  exit 1
fi
if git -C "$GECKO" rev-parse -q --verify "refs/heads/$BRANCH" > /dev/null; then
  echo "$GECKO already has a branch $BRANCH; delete it or set GECKO_W10M_ENGINE_BRANCH." >&2
  exit 1
fi

shopt -s nullglob
PATCHES=("$ROOT"/patches/w10m/*.patch)
if [ ${#PATCHES[@]} -eq 0 ]; then
  echo "No patches in $ROOT/patches/w10m." >&2
  exit 1
fi

git -C "$GECKO" checkout -q -b "$BRANCH" "$TAG"
git -C "$GECKO" am --keep-cr --3way --whitespace=nowarn "${PATCHES[@]}"
echo "Applied ${#PATCHES[@]} patches: $GECKO is on $BRANCH, $(git -C "$GECKO" rev-parse --short HEAD)."
