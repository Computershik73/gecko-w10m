#!/bin/bash
# Regenerate media/libvpx/config/win/arm/*_rtcd.h with HAVE_NEON_ASM off.
#
# libvpx's hand-written 32-bit ARM assembly is A32 (ARM mode) and Windows on
# ARM32 is Thumb-2 only, so those .asm fast paths cannot be built there. The
# run-time CPU dispatch headers are pre-generated per configuration and name the
# assembly entry points directly, so flipping HAVE_NEON_ASM in vpx_config.h is
# not enough -- the headers have to be regenerated too. This runs libvpx's own
# generator, the same way media/libvpx/generate_sources_mozbuild.sh does.
#
# Needs bash + perl (MozillaBuild's msys2 has both).
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
LIBVPX="$HERE/../engine/firefox/media/libvpx"
CFG="$LIBVPX/config/win/arm"
SRC="$LIBVPX/libvpx"

test -d "$CFG" || { echo "missing $CFG" >&2; exit 1; }

# 1. Turn the assembly off in the configuration itself.
sed -i 's/^#define HAVE_NEON_ASM 1$/#define HAVE_NEON_ASM 0/' "$CFG/vpx_config.h"
sed -i 's/^\.equ HAVE_NEON_ASM ,  1$/.equ HAVE_NEON_ASM ,  0/' "$CFG/vpx_config.asm"

# 2. Flatten the two config files into the CONFIG_FOO=yes form rtcd.pl wants.
TMPCFG="$(mktemp)"
trap 'rm -f "$TMPCFG"' EXIT
"$LIBVPX/lint_config.sh" -p \
  -h "$CFG/vpx_config.h" \
  -a "$CFG/vpx_config.asm" \
  -o "$TMPCFG"

# 3. Regenerate the dispatch headers.
gen() {  # $1 = --sym value, $2 = defs file, $3 = output
  perl "$SRC/build/make/rtcd.pl" \
    --arch=armv7 \
    --sym="$1" \
    --config="$TMPCFG" \
    "$SRC/$2" > "$CFG/$3"
  echo "  wrote $3"
}

gen vp8_rtcd       vp8/common/rtcd_defs.pl        vp8_rtcd.h
gen vp9_rtcd       vp9/common/vp9_rtcd_defs.pl    vp9_rtcd.h
gen vpx_scale_rtcd vpx_scale/vpx_scale_rtcd.pl    vpx_scale_rtcd.h
gen vpx_dsp_rtcd   vpx_dsp/vpx_dsp_rtcd_defs.pl   vpx_dsp_rtcd.h

echo "done: $CFG regenerated with HAVE_NEON_ASM=0"
