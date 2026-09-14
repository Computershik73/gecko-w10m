#!/usr/bin/env python3
"""Rewrite clang's 32-bit ARM virtual-call thunks to stop destroying r1.

A pointer-to-member call on a virtual function goes through a compiler-made
thunk (`??_9Class@@$B...`). From identical source, cl.exe and clang-cl disagree
about which register it may use:

    MSVC                       clang
    ldr r12, [r0]              ldr r1, [r0]
    ldr r12, [r12, #4]         ldr r1, [r1, #4]
    bx  r12                    bx  r1

r1 is the first argument register. r12 is the intra-procedure scratch register,
which is what it exists for. clang picks r1, so every pointer-to-member call on
a virtual function reaches its target with the first argument replaced by the
target's own address. Gecko dispatches manifest directives exactly this way,
and that is where it surfaced: nsChromeRegistryChrome::ManifestLocale received
its own address instead of a ManifestProcessingContext, read a member 24 bytes
into its own code, and NS_NewURI dereferenced the result.

The clang bug is in the thunk, not in Control Flow Guard -- but CFG decides
whether there is room to repair it. With -guard:cf the thunk grows to 34 bytes
inside a 48-byte slot, because it has to save the target across the guard call:

    push {r4, r5, r11, lr}
    mov  r4, r0              ; this
    ldr  r0, [r0]            ; vtable
    movw r1, #...            ; &__guard_check_icall_fptr   <- r1 dies here
    movt r1, #...
    ldr  r1, [r1]
    ldr  r0, [r0, #N]        ; target
    mov  r5, r0
    blx  r1                  ; the guard check
    mov  r0, r4
    mov  r1, r5
    pop  {r4, r5, r11, lr}
    bx   r1

Fourteen spare bytes, and ten are needed. Without the guard the thunk is six
bytes with nothing after it, and there is nowhere to put a correct one. So the
engine is built with -guard:cf for the space and the flag is cleared in the
image afterwards so the guard is never enforced (its tables are wrong on this
target too -- see tools/strip-cfg.py), and each thunk is replaced here with
what MSVC would have emitted:

    ldr.w r12, [r0]
    ldr.w r12, [r12, #N]
    bx    r12

N is read out of the thunk being replaced, so nothing needs to be known about
the class. The rest of the slot is filled with nops; it is unreachable.

Usage: python tools/patch-vcall-thunks.py <image.dll> [...]
"""

import struct
import sys
from pathlib import Path

# push {r4,r5,r11,lr} / mov r4,r0 / ldr r0,[r0]
HEAD = bytes.fromhex("2de93048") + bytes.fromhex("0446") + bytes.fromhex("0068")
# mov r5,r0 / blx r1 / mov r0,r4 / mov r1,r5 / pop {r4,r5,r11,lr} / bx r1
TAIL = (bytes.fromhex("0546") + bytes.fromhex("8847") + bytes.fromhex("2046") +
        bytes.fromhex("2946") + bytes.fromhex("bde83048") + bytes.fromhex("0847"))

NOP = bytes.fromhex("00bf")


def vtable_offset(code: bytes):
    """The offset in `ldr r0, [r0, #N]`, narrow or wide, or None."""
    if len(code) == 2:
        (hw,) = struct.unpack_from("<H", code)
        # T1: 0110 1 imm5 Rn(3) Rt(3), with Rn = Rt = r0
        if hw & 0xF83F == 0x6800:
            return ((hw >> 6) & 0x1F) * 4
    elif len(code) == 4:
        first, second = struct.unpack_from("<HH", code)
        # T3: F8D0 0imm12
        if first == 0xF8D0 and second & 0xF000 == 0x0000:
            return second & 0x0FFF
    return None


def replacement(offset: int) -> bytes:
    if offset > 0xFFF:
        return b""
    return (struct.pack("<HH", 0xF8D0, 0xC000) +          # ldr.w r12, [r0]
            struct.pack("<HH", 0xF8DC, 0xC000 | offset) +  # ldr.w r12, [r12, #N]
            struct.pack("<H", 0x4760))                     # bx r12


def patch(path: Path) -> int:
    data = bytearray(path.read_bytes())

    fixed = 0
    skipped = 0
    start = 0
    while True:
        head = data.find(HEAD, start)
        if head < 0:
            break
        start = head + len(HEAD)

        # The ldr that reads the vtable slot sits immediately before the tail,
        # and is two bytes or four depending on how far into the table it is.
        body = head + len(HEAD)
        for width in (2, 4):
            tail = body + 6 + width  # the movw/movt/ldr pair is 10 bytes
            if data[tail:tail + len(TAIL)] == TAIL:
                offset = vtable_offset(bytes(data[tail - width:tail]))
                break
        else:
            continue

        if offset is None:
            skipped += 1
            continue
        new = replacement(offset)
        if not new:
            skipped += 1
            continue

        end = tail + len(TAIL)
        data[head:end] = new + NOP * ((end - head - len(new)) // 2)
        fixed += 1

    if not fixed and not skipped:
        print(f"{path.name}: no thunks of this shape")
        return 0

    path.write_bytes(bytes(data))
    note = f", {skipped} left alone" if skipped else ""
    print(f"{path.name}: {fixed} vcall thunks rewritten to use r12{note}")
    return fixed


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for name in argv[1:]:
        patch(Path(name))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
