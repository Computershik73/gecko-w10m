#!/usr/bin/env python3
"""Stop libwebrtc from claiming NEON on 32-bit ARM Windows.

The GN-to-moz.build translation assumes TARGET_CPU == "arm" implies Android or
Linux, because that is the only place upstream builds 32-bit ARM. Its per-CPU
blocks therefore define WEBRTC_HAS_NEON, while the NEON sources themselves are
added by the per-OS blocks, which do not fire for WINNT. The result is a build
that compiles the run-time dispatchers but none of the *Neon implementations,
and libxul fails to link on WebRtcSpl_MaxAbsValueW16Neon and friends.

libwebrtc's own 32-bit ARM assembly is A32 (ARM mode) anyway, which Windows on
ARM32 -- Thumb-2 only -- cannot use, so the portable C paths are the right
answer here. This script guards the define accordingly, touching only the
`TARGET_CPU == "arm"` block of each generated moz.build.

Idempotent: re-running it makes no further changes.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.join(HERE, "..", "engine", "firefox", "third_party",
                            "libwebrtc")

ARM_BLOCK = re.compile(r'^if CONFIG\["TARGET_CPU"\] == "arm":\n(.*?)(?=^if |\Z)',
                       re.S | re.M)

OLD = '    DEFINES["WEBRTC_HAS_NEON"] = True\n'
NEW = ('    # Windows on ARM32 has no libwebrtc NEON sources to go with this;\n'
       '    # see tools/patch-libwebrtc-win-arm32-neon.py.\n'
       '    if CONFIG["OS_TARGET"] != "WINNT":\n'
       '        DEFINES["WEBRTC_HAS_NEON"] = True\n')


def main():
    root = os.path.normpath(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_ROOT)
    changed = 0
    skipped = 0
    for dirpath, _dirs, names in os.walk(root):
        if "moz.build" not in names:
            continue
        path = os.path.join(dirpath, "moz.build")
        text = open(path, encoding="utf-8").read()
        m = ARM_BLOCK.search(text)
        if not m or OLD not in m.group(1):
            continue
        if 'OS_TARGET"] != "WINNT"' in m.group(1):
            skipped += 1
            continue
        block = m.group(1).replace(OLD, NEW, 1)
        text = text[:m.start(1)] + block + text[m.end(1):]
        open(path, "w", encoding="utf-8", newline="\n").write(text)
        changed += 1
    print("patched: %d, already patched: %d" % (changed, skipped))


if __name__ == "__main__":
    main()
