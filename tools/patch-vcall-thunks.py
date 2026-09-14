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
whether there is room to repair it. With -guard:cf the thunk has to keep the
target across the guard call, which grows it to around 34 bytes in a 48-byte
slot; ten are needed for a correct one. Without the guard it is six bytes with
nothing after it, and a correct thunk does not fit. So the engine is built with
-guard:cf for the space, the flag is cleared in the image afterwards so the
guard is never enforced (its tables are wrong on this target too -- see
tools/strip-cfg.py), and each thunk is replaced here with what MSVC would have
emitted:

    ldr.w r12, [r0]
    ldr.w r12, [r12, #N]
    bx    r12

Thunks are found through the linker map rather than by matching their bytes:
clang allocates registers differently from one to the next, so the prologue
varies while only the last few instructions are constant. Those constant ones
are the tail below, and the load right before them carries the vtable offset,
which is the only thing that has to be recovered.

Usage: python tools/patch-vcall-thunks.py <image.dll> <image.map>
"""

import re
import struct
import sys
from pathlib import Path

# mov rX,r0 / blx r1 / mov r0,r4 / mov r1,r5 / pop {r4,r5,r11,lr} / bx r1
TAIL = bytes.fromhex("8847204629 46bde830480847".replace(" ", ""))
NOP = bytes.fromhex("00bf")

MAP_SYMBOL = re.compile(r"^\s+[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})")
MAP_BASE = re.compile(r"Preferred load address is\s+([0-9a-fA-F]+)")


def thunk_rvas(map_path):
    """Every `??_9` vcall thunk in the map, as image-relative addresses."""
    base = None
    out = []
    with open(map_path, "r", errors="replace") as f:
        for line in f:
            if base is None:
                found = MAP_BASE.search(line)
                if found:
                    base = int(found.group(1), 16)
                continue
            found = MAP_SYMBOL.match(line)
            if found and found.group(1).startswith("??_9"):
                out.append(int(found.group(2), 16) - base)
    return sorted(set(out))


def section_table(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    first = pe + 24 + opt_size
    sections = []
    for i in range(count):
        entry = first + i * 40
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", data, entry + 8
        )
        sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer))
    return sections


def to_offset(sections, rva):
    for virtual_address, size, raw_pointer in sections:
        if virtual_address <= rva < virtual_address + size:
            return raw_pointer + (rva - virtual_address)
    return None


def vtable_offset(code):
    """The offset in the `ldr rT, [rB, #N]` that reads the vtable slot."""
    if len(code) == 2:
        (hw,) = struct.unpack_from("<H", code)
        # T1: 0110 1 imm5 Rn Rt, low registers only.
        if hw & 0xF800 == 0x6800:
            return ((hw >> 6) & 0x1F) * 4
    else:
        first, second = struct.unpack_from("<HH", code)
        # T3: 1111 1000 1101 Rn / Rt imm12
        if first & 0xFFF0 == 0xF8D0:
            return second & 0x0FFF
    return None


def patch(image_path, map_path):
    data = bytearray(image_path.read_bytes())
    sections = section_table(data)

    fixed = skipped = 0
    for rva in thunk_rvas(map_path):
        start = to_offset(sections, rva)
        if start is None:
            skipped += 1
            continue

        # The tail is the only constant part. Everything before it is prologue
        # whose shape depends on how clang happened to allocate registers.
        window = bytes(data[start:start + 48])
        at = window.find(TAIL)
        if at < 0:
            skipped += 1
            continue

        # Immediately before the tail: mov rX,r0 (2 bytes), and before that the
        # load of the vtable slot, narrow or wide.
        offset = None
        for width in (2, 4):
            offset = vtable_offset(window[at - 2 - width:at - 2])
            if offset is not None:
                break
        if offset is None or offset > 0xFFF:
            skipped += 1
            continue

        end = start + at + len(TAIL)
        new = (struct.pack("<HH", 0xF8D0, 0xC000) +           # ldr.w r12, [r0]
               struct.pack("<HH", 0xF8DC, 0xC000 | offset) +  # ldr.w r12, [r12, #N]
               struct.pack("<H", 0x4760))                     # bx r12
        data[start:end] = new + NOP * ((end - start - len(new)) // 2)
        fixed += 1

    image_path.write_bytes(bytes(data))
    note = f", {skipped} left alone" if skipped else ""
    print(f"{image_path.name}: {fixed} vcall thunks rewritten to use r12{note}")
    return fixed


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    patch(Path(argv[1]), Path(argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
