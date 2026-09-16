#!/usr/bin/env python3
"""Set the Thumb bit on link-time function pointers that lost it.

nss3.dll dies at PR_CallOnce+8 with an illegal instruction, on a `push` that is
as legal as instructions get. It is legal in Thumb; the processor was in ARM
state. It got there because the call is indirect --

    movw r1, #0x1674
    movt r1, #0x101f        ; r1 = the slot
    ldr  r2, [r1]           ; r2 = the function pointer
    blx  r2

-- and the slot, at .rdata RVA 0x1f1674, holds PR_CallOnce's address with the
low bit clear. blx to an even address switches to ARM, and two instructions
later the decoder hits an encoding that does not exist.

The slot is not an import: it is outside the import table, the IAT and the
delay-import table. NSS declares the NSPR entry points __declspec(dllimport),
and NSPR is linked into nss3 itself, so the linker resolves __imp_PR_CallOnce
to a pointer it synthesises in .rdata aimed at the local definition. On 32-bit
ARM lld-link synthesises it without the Thumb bit. There are 69 of them in
nss3.dll and none anywhere else in the build.

Finding them needs no guesswork and no symbol names. A slot qualifies when all
three hold:

  * it has a base relocation of type HIGHLOW, so it is a pointer the loader
    rewrites rather than an integer that happens to look like one;
  * its value is even; and
  * that value, minus the image base, lands in a section marked executable.

An even pointer into code is a contradiction on this architecture: every
address that can be branched to carries the Thumb bit. So the rule is not a
heuristic about what the value probably is -- it is the invariant itself.

Measured across the whole build: 89 in nss3.dll, and zero in every other
module, xul.dll included, which alone has 456964 relocations. A filter that
finds what it is looking for whether or not it is there would not do that.

An earlier version of this required the target to be a function start listed
in .pdata. That missed 20 of the 89: the linker-generated import thunks have
no unwind data, and neither do leaf functions that never touch the stack. The
crash simply moved from PR_CallOnce to one of the thunks.

Usage: python tools/fix-thumb-pointers.py <image.dll> [...]
"""

import struct
import sys
from pathlib import Path

IMAGE_FILE_MACHINE_ARMNT = 0x01C4
REL_HIGHLOW = 3
IMAGE_SCN_MEM_EXECUTE = 0x20000000


def fix(path: Path) -> int:
    data = bytearray(path.read_bytes())
    if data[:2] != b"MZ":
        return 0

    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        return 0
    if struct.unpack_from("<H", data, pe + 4)[0] != IMAGE_FILE_MACHINE_ARMNT:
        return 0

    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    image_base = struct.unpack_from("<I", data, pe + 24 + 28)[0]

    secs = []
    for i in range(nsec):
        o = pe + 24 + optsize + 40 * i
        secs.append(
            (
                struct.unpack_from("<I", data, o + 12)[0],  # virtual address
                struct.unpack_from("<I", data, o + 8)[0],  # virtual size
                struct.unpack_from("<I", data, o + 20)[0],  # raw pointer
            )
        )

    def offset(rva):
        for va, vsize, raw in secs:
            if va <= rva < va + max(vsize, 1):
                return raw + (rva - va)
        return None

    reloc_rva, reloc_size = struct.unpack_from("<II", data, pe + 24 + 96 + 8 * 5)
    if not reloc_size:
        return 0

    executable = []
    for i in range(nsec):
        o = pe + 24 + optsize + 40 * i
        characteristics = struct.unpack_from("<I", data, o + 36)[0]
        if characteristics & IMAGE_SCN_MEM_EXECUTE:
            va = struct.unpack_from("<I", data, o + 12)[0]
            vsize = struct.unpack_from("<I", data, o + 8)[0]
            executable.append((va, va + max(vsize, 1)))

    fixed = 0
    p = offset(reloc_rva)
    end = p + reloc_size
    while p + 8 <= end:
        page, block = struct.unpack_from("<II", data, p)
        if block < 8:
            break
        for j in range((block - 8) // 2):
            entry = struct.unpack_from("<H", data, p + 8 + 2 * j)[0]
            if entry >> 12 != REL_HIGHLOW:
                continue
            slot = offset(page + (entry & 0xFFF))
            if slot is None or slot + 4 > len(data):
                continue
            value = struct.unpack_from("<I", data, slot)[0]
            if value & 1:
                continue
            target = value - image_base
            if not any(lo <= target < hi for lo, hi in executable):
                continue
            struct.pack_into("<I", data, slot, value | 1)
            fixed += 1
        p += block

    if fixed:
        path.write_bytes(data)
        print(f"{path.name}: {fixed} function pointers given their Thumb bit back")
    return fixed


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for name in argv[1:]:
        fix(Path(name))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
