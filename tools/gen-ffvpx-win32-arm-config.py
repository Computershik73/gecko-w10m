"""Derive media/ffvpx/config_win32_arm.h from config_win32.h.

ffvpx ships no configuration for 32-bit ARM Windows. config_win32.h is the right
starting point -- it carries all the Windows settings (W32THREADS, the Win32
API probes, the enabled codecs) -- but it is an *x86* configuration, so every
x86 ISA and assembly switch has to be turned off. ARM stays off too: ffvpx's
32-bit ARM assembly is A32 (ARM mode) and Windows on ARM32 is Thumb-2 only.
"""
import re

SRC = ("C:/Users/User/Documents/GitHub/gecko-w10m/engine/firefox/media/ffvpx/"
       "config_win32.h")
DST = ("C:/Users/User/Documents/GitHub/gecko-w10m/engine/firefox/media/ffvpx/"
       "config_win32_arm.h")

ZERO = {
    "ARCH_X86", "ARCH_X86_32", "ARCH_X86_64",
    "HAVE_X86ASM", "HAVE_INLINE_ASM", "HAVE_I686", "HAVE_RDTSC", "HAVE_MM_EMPTY",
    "HAVE_AESNI", "HAVE_AESNI_EXTERNAL",
    "HAVE_AMD3DNOW", "HAVE_AMD3DNOW_EXTERNAL",
    "HAVE_AMD3DNOWEXT", "HAVE_AMD3DNOWEXT_EXTERNAL",
    "HAVE_AVX", "HAVE_AVX_EXTERNAL",
    "HAVE_AVX2", "HAVE_AVX2_EXTERNAL",
    "HAVE_AVX512", "HAVE_AVX512_EXTERNAL", "HAVE_AVX512ICL",
    "HAVE_FMA3", "HAVE_FMA3_EXTERNAL", "HAVE_FMA4", "HAVE_FMA4_EXTERNAL",
    "HAVE_MMX", "HAVE_MMX_EXTERNAL", "HAVE_MMXEXT", "HAVE_MMXEXT_EXTERNAL",
    "HAVE_SSE", "HAVE_SSE_EXTERNAL", "HAVE_SSE2", "HAVE_SSE2_EXTERNAL",
    "HAVE_SSE3", "HAVE_SSE3_EXTERNAL", "HAVE_SSSE3", "HAVE_SSSE3_EXTERNAL",
    "HAVE_SSE4", "HAVE_SSE4_EXTERNAL", "HAVE_SSE42", "HAVE_SSE42_EXTERNAL",
    "HAVE_XOP", "HAVE_XOP_EXTERNAL",
    "HAVE_SIMD_ALIGN_16", "HAVE_SIMD_ALIGN_32", "HAVE_SIMD_ALIGN_64",
    "HAVE_LOCAL_ALIGNED",
    # ARM assembly stays off as well; see the module docstring.
    "ARCH_ARM", "HAVE_NEON", "HAVE_NEON_EXTERNAL", "HAVE_NEON_INLINE",
    "HAVE_VFP", "HAVE_VFP_EXTERNAL", "HAVE_VFP_INLINE",
    "HAVE_ARMV5TE", "HAVE_ARMV5TE_EXTERNAL", "HAVE_ARMV5TE_INLINE",
    "HAVE_ARMV6", "HAVE_ARMV6_EXTERNAL", "HAVE_ARMV6_INLINE",
    "HAVE_ARMV6T2", "HAVE_ARMV6T2_EXTERNAL", "HAVE_ARMV6T2_INLINE",
    "HAVE_ARMV8", "HAVE_ARMV8_EXTERNAL", "HAVE_ARMV8_INLINE",
}

RENAME = {
    "FFMPEG_CONFIGURATION": '"--disable-all --enable-avcodec '
                            "--enable-decoder='vp8,vp9,mp3,flac' "
                            "--enable-parser='vp8,vp9' --disable-static "
                            '--enable-shared --disable-autodetect --disable-asm"',
}

HEADER = """/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Generated for GeckoW10m from config_win32.h by
 * tools/gen-ffvpx-win32-arm-config.py.
 *
 * 32-bit ARM Windows: the Windows settings of config_win32.h with every x86
 * ISA and assembly switch turned off. ARM assembly is off too -- ffvpx's 32-bit
 * ARM assembly is A32 (ARM mode) and this platform is Thumb-2 only -- so this
 * is a portable-C build that still uses Win32 threads and the Win32 API probes.
 */
"""

out = []
changed = 0
seen_guard = False
for line in open(SRC, encoding="utf-8", errors="replace"):
    m = re.match(r'(#define\s+)(\w+)(\s+)(.*?)(\s*)$', line)
    if m:
        name = m.group(2)
        if name in ZERO and m.group(4).strip() != "0":
            out.append("%s%s%s0\n" % (m.group(1), name, m.group(3)))
            changed += 1
            continue
        if name in RENAME:
            out.append("%s%s%s%s\n" % (m.group(1), name, m.group(3),
                                       RENAME[name]))
            continue
    out.append(line)

text = "".join(out)
# Keep the include guard distinct from config_win32.h's.
text = text.replace("MOZ_FFVPX_CONFIG_WIN32_H", "MOZ_FFVPX_CONFIG_WIN32_ARM_H")

open(DST, "w", encoding="utf-8", newline="\n").write(HEADER + "\n" + text)
print("wrote", DST, "- zeroed", changed, "defines")
