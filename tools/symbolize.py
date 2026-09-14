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


def image_name(path):
    """The image a map belongs to, taken from its first line.

    Maps name the image without an extension ("xul", "GeckoW10m"), and nothing in
    the file says whether it linked a .dll or an .exe -- so everything is keyed
    on the stem and addresses are matched the same way.
    """
    with open(path, "r", errors="replace") as f:
        first = f.readline().strip()
    return (first or path.stem).lower()


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

    map_paths = [Path(a) for a in argv[2:]] or [DEFAULT_MAP]
    missing = [p for p in map_paths if not p.exists()]
    if missing:
        print("no map at " + ", ".join(str(p) for p in missing))
        return 1

    target = argv[1]
    text = Path(target).read_text(errors="replace") if Path(target).exists() else target

    wanted = ADDRESS.findall(text)
    if not wanted:
        print("no module+offset addresses found")
        return 1

    # A map names its own image on the first line, so several can be passed at
    # once and each address goes to the one it belongs to.
    maps = {}
    for path in map_paths:
        stem = image_name(path)
        maps[stem] = load_map(path)
        rvas, _, _, stamp = maps[stem]
        print(f"map: {stem} <- {path}  ({len(rvas)} symbols, linked {stamp})")
    print()

    for module, offset in wanted:
        rva = int(offset, 16)
        key = module.lower().rsplit(".", 1)[0]
        if key not in maps:
            print(f"{module}+0x{rva:08x}  (no map for this module)")
            continue
        rvas, names, objects, _ = maps[key]
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
