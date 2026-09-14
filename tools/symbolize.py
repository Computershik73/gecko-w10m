#!/usr/bin/env python3
"""Turn crash addresses from the device log into function names.

The Windows 10 Mobile build carries no debug symbols -- compiling 132 MB of
engine with them would not fit the machine -- so the on-device crash logger
reports what it can: the module and the offset into it, as "xul.dll+0x3782930".
The linker map fills in the rest. toolkit/library/build/moz.build asks for one
on this port, so every xul.dll build produces a matching xul.map.

Usage:
    python tools/symbolize.py 'xul.dll+0x3782930'
    python tools/symbolize.py gecko_w10m.log          # every crash line in a log

The map must come from the build that produced the running binary. A map from a
different build resolves to plausible and wrong names, so the timestamp in its
header is printed with the results -- check it against the package.
"""

import bisect
import re
import sys
from pathlib import Path

DEFAULT_MAP = Path("C:/rw-obj/toolkit/library/build/xul.map")

# " 0001:0377d500       ?XRE_mainInit@XREMain@@... 000000001377e500   nsAppRunner.obj"
SYMBOL = re.compile(
    r"^\s+[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+(?:f\s+)?(\S*)"
)
PREFERRED_BASE = re.compile(r"Preferred load address is\s+([0-9a-fA-F]+)")
TIMESTAMP = re.compile(r"Timestamp is \w+ \((.+)\)")
ADDRESS = re.compile(r"([A-Za-z0-9_.\-]+\.(?:dll|exe))\+0x([0-9a-fA-F]+)")


def load_map(path):
    """Returns (sorted rvas, symbols, objects, timestamp)."""
    rvas, names, objects = [], [], []
    base = None
    stamp = "unknown"

    with open(path, "r", errors="replace") as f:
        for line in f:
            if base is None:
                m = PREFERRED_BASE.search(line)
                if m:
                    base = int(m.group(1), 16)
                    continue
                m = TIMESTAMP.search(line)
                if m:
                    stamp = m.group(1)
                continue

            m = SYMBOL.match(line)
            if not m:
                continue
            obj = m.group(3)
            # ARM32 branches reach 16 MB, so the linker sprinkles
            # range_extension_thunk entries between real functions. They sort
            # ahead of the function an address actually belongs to and would
            # answer every query with a thunk.
            if obj == "<linker-defined>":
                continue
            rvas.append(int(m.group(2), 16) - base)
            names.append(m.group(1))
            objects.append(obj)

    order = sorted(range(len(rvas)), key=lambda i: rvas[i])
    return (
        [rvas[i] for i in order],
        [names[i] for i in order],
        [objects[i] for i in order],
        stamp,
    )


def resolve(rvas, names, objects, rva):
    # The symbol covering an address is the last one that starts at or before it.
    i = bisect.bisect_right(rvas, rva) - 1
    if i < 0:
        return None
    return names[i], objects[i], rva - rvas[i]


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    map_path = Path(argv[2]) if len(argv) > 2 else DEFAULT_MAP
    if not map_path.exists():
        print(f"no map at {map_path}; build xul.dll to produce one")
        return 1

    target = argv[1]
    text = Path(target).read_text(errors="replace") if Path(target).exists() else target

    wanted = ADDRESS.findall(text)
    if not wanted:
        print("no module+offset addresses found")
        return 1

    rvas, names, objects, stamp = load_map(map_path)
    print(f"map: {map_path}  ({len(rvas)} symbols, linked {stamp})")
    print()

    for module, offset in wanted:
        rva = int(offset, 16)
        if module.lower() != "xul.dll":
            print(f"{module}+0x{rva:08x}  (no map for this module)")
            continue
        hit = resolve(rvas, names, objects, rva)
        if not hit:
            print(f"{module}+0x{rva:08x}  (below the first symbol)")
            continue
        name, obj, delta = hit
        where = f" [{obj}]" if obj else ""
        print(f"{module}+0x{rva:08x}  {name}+0x{delta:x}{where}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
