#!/usr/bin/env python3
"""Repair clang's 32-bit ARM virtual-call thunks, which pass through r1.

A pointer-to-member call on a virtual function goes through a compiler-made
thunk. With Control Flow Guard on, clang emits this on 32-bit ARM:

    push {r4, r5, r11, lr}
    mov  r4, r0            ; save this
    ldr  r0, [r0]          ; vtable
    ldr  r1, =__guard_check_icall_fptr
    ldr  r1, [r1]
    ldr  r0, [r0, #N]      ; target
    mov  r5, r0
    blx  r1                ; the guard check
    mov  r0, r4            ; restore this
    mov  r1, r5            ; target -> r1
    pop  {r4, r5, r11, lr}
    bx   r1                ; tail-jump

r1 is the first argument register. The thunk restores `this` and then writes
the callee's own address over the callee's first parameter, so every
pointer-to-member call on a virtual function arrives with its first argument
destroyed. r12 is the intra-procedure scratch register and exists for exactly
this; using r1 is the bug.

Gecko dispatches manifest directives this way, which is where it surfaced:
nsChromeRegistryChrome::ManifestLocale received its own address in place of
the ManifestProcessingContext, read a member 24 bytes into its own code, and
NS_NewURI dereferenced the result.

This rewrites the last two instructions of each thunk to use r12:

    mov  r1, r5   4629  ->  mov  r12, r5   46ac
    bx   r1       4708  ->  bx   r12       4760

Both are two-byte Thumb encodings, so nothing moves and no fixups are needed.
The real fix is to build without -guard:cf, which removes the guard call and
with it the whole shape; this exists so a package can be tested without a
four-hour rebuild first.

Usage: python tools/patch-vcall-thunks.py <image.dll>
"""

import sys
from pathlib import Path

# mov r0,r4 / mov r1,r5 / pop {r4,r5,r11,lr} / bx r1
BROKEN = bytes.fromhex("2046") + bytes.fromhex("2946") + \
         bytes.fromhex("bde83048") + bytes.fromhex("0847")
# mov r0,r4 / mov r12,r5 / pop {r4,r5,r11,lr} / bx r12
FIXED = bytes.fromhex("2046") + bytes.fromhex("ac46") + \
        bytes.fromhex("bde83048") + bytes.fromhex("6047")


def patch(path: Path) -> int:
    data = bytearray(path.read_bytes())
    count = data.count(BROKEN)
    if not count:
        print(f"{path.name}: no thunks of this shape")
        return 0

    data = bytearray(bytes(data).replace(BROKEN, FIXED))
    path.write_bytes(data)
    print(f"{path.name}: {count} vcall thunks moved off r1")
    return count


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for name in argv[1:]:
        patch(Path(name))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
