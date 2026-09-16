#!/usr/bin/env python3
"""Clear IMAGE_DLLCHARACTERISTICS_GUARD_CF in a PE image.

Control Flow Guard validates the target of every indirect call -- and a virtual
method call is an indirect call, which in Gecko means nearly all of them. When
the check fails it reports through __fastfail: the process is gone at once,
with no exception raised, no handler run and nothing written down. That is
exactly the death this port has been seeing inside XRE_main.

Whether the check is enforced is the loader's decision, taken from this flag in
the image header. With it clear, the loader leaves __guard_check_icall_fptr
pointing at a no-op instead of the validator, and the guarded call sites become
ordinary indirect calls. Nothing else about the image changes, so this is two
bytes rather than a rebuild.

CFG is not wrong in principle; its metadata is wrong here. Firefox ships with
it on x86, x64 and ARM64, but 32-bit ARM with lld-link is a combination almost
nobody builds, and the guard function table has to account for the Thumb bit on
every entry. It does not: the dispatcher reaches its target with bx, and an
entry recorded without that bit arrives in ARM state, where the first honest
Thumb instruction of the callee decodes as an illegal one. That is how
nss3.dll died -- 0xc000001d on the push at the entry of PR_CallOnce, which is
as legal an instruction as exists.

So every module this port links needs the flag cleared, not just the one that
showed the symptom first. The CRT staged from VCLibs is Microsoft's own build
and keeps its own, correct, metadata.

Usage: python tools/strip-cfg.py <image.dll> [...]
"""

import struct
import sys
from pathlib import Path

GUARD_CF = 0x4000


def strip(path: Path) -> bool:
    data = bytearray(path.read_bytes())

    if data[:2] != b"MZ":
        print(f"{path.name}: not a PE image")
        return False

    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        print(f"{path.name}: no PE signature")
        return False

    # DllCharacteristics sits at a fixed offset in the optional header, and at
    # the same one for PE32 and PE32+ -- the fields that differ in width come
    # after it.
    offset = pe + 0x5E
    flags = struct.unpack_from("<H", data, offset)[0]

    if not flags & GUARD_CF:
        print(f"{path.name}: CFG already off (0x{flags:04X})")
        return False

    struct.pack_into("<H", data, offset, flags & ~GUARD_CF)
    path.write_bytes(data)
    print(f"{path.name}: 0x{flags:04X} -> 0x{flags & ~GUARD_CF:04X}, CFG off")
    return True


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for name in argv[1:]:
        strip(Path(name))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
