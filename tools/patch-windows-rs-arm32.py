#!/usr/bin/env python3
"""Teach the vendored windows-rs crates about 32-bit ARM Windows.

windows-rs ships no Windows-on-ARM32 bindings at all. Three things are missing,
and this script adds all of them -- idempotently -- to both crates Gecko uses:

  1. Architecture gates. Every layout-sensitive struct is gated on x86
     (packed(1)) versus the 64-bit arches (natural alignment). Windows on ARM32
     uses natural alignment, so it belongs with the 64-bit group.
  2. The *LongPtr* aliases. On any 32-bit target Get/SetWindowLongPtr are
     aliases for the non-Ptr entry points, so step 1 must not also declare the
     real 64-bit imports there or the names collide (E0255).
  3. Types with no ARM32 definition anywhere: CONTEXT, SLIST_HEADER,
     UNWIND_HISTORY_TABLE_ENTRY and the minidump thread callbacks. Those are
     genuinely architecture-specific and are written out by hand below,
     following winnt.h's ARM branch.

Usage: patch-windows-rs-arm32.py [<crate src dir> ...]
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CRATES = [
    os.path.join(HERE, "..", "engine", "firefox", "third_party", "rust",
                 "windows-sys", "src"),
    os.path.join(HERE, "..", "engine", "third_party", "windows-0.62.2", "src"),
]

GATE_OLD = 'target_arch = "aarch64", target_arch = "arm64ec", target_arch = "x86_64"'
GATE_NEW = ('target_arch = "aarch64", target_arch = "arm", '
            'target_arch = "arm64ec", target_arch = "x86_64"')

LONGPTR = re.compile(r"fn (Get|Set)Window(Long|ClassLong)Ptr[AW]")

MARKER = "// --- Windows on ARM32 (gecko_w10m) ---"

KERNEL_ARM = MARKER + """
// winnt.h's `_ARM_` branch: the same union as x86, except the trailing WORD is
// Reserved rather than CpuId.
#[repr(C)]
#[cfg(target_arch = "arm")]
#[derive(Clone, Copy)]
pub union SLIST_HEADER {
    pub Alignment: u64,
    pub Anonymous: SLIST_HEADER_0,
}
#[cfg(target_arch = "arm")]
impl Default for SLIST_HEADER {
    fn default() -> Self {
        unsafe { core::mem::zeroed() }
    }
}
#[repr(C)]
#[cfg(target_arch = "arm")]
#[derive(Clone, Copy, Default)]
pub struct SLIST_HEADER_0 {
    pub Next: SINGLE_LIST_ENTRY,
    pub Depth: u16,
    pub Reserved: u16,
}
"""

DEBUG_ARM = MARKER + """
// winnt.h's ARM CONTEXT: 13 GPRs, 4 control registers, the VFP/NEON save area
// as a union of 16 x 128-bit / 32 x 64-bit / 32 x 32-bit views, then the debug
// registers (ARM_MAX_BREAKPOINTS = 8, ARM_MAX_WATCHPOINTS = 1).
#[repr(C)]
#[cfg(target_arch = "arm")]
#[derive(Clone, Copy)]
pub union CONTEXT_0 {
    pub Q: [u64; 32],
    pub D: [u64; 32],
    pub S: [u32; 32],
}
#[cfg(target_arch = "arm")]
impl Default for CONTEXT_0 {
    fn default() -> Self {
        unsafe { core::mem::zeroed() }
    }
}
#[repr(C)]
#[cfg(target_arch = "arm")]
#[derive(Clone, Copy)]
pub struct CONTEXT {
    pub ContextFlags: CONTEXT_FLAGS,
    pub R0: u32,
    pub R1: u32,
    pub R2: u32,
    pub R3: u32,
    pub R4: u32,
    pub R5: u32,
    pub R6: u32,
    pub R7: u32,
    pub R8: u32,
    pub R9: u32,
    pub R10: u32,
    pub R11: u32,
    pub R12: u32,
    pub Sp: u32,
    pub Lr: u32,
    pub Pc: u32,
    pub Cpsr: u32,
    pub Fpscr: u32,
    pub Padding: u32,
    pub Anonymous: CONTEXT_0,
    pub Bvr: [u32; 8],
    pub Bcr: [u32; 8],
    pub Wvr: [u32; 1],
    pub Wcr: [u32; 1],
    pub Padding2: [u32; 2],
}
#[cfg(target_arch = "arm")]
impl Default for CONTEXT {
    fn default() -> Self {
        unsafe { core::mem::zeroed() }
    }
}
#[repr(C)]
#[cfg(target_arch = "arm")]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct UNWIND_HISTORY_TABLE_ENTRY {
    pub ImageBase: usize,
    // IMAGE_ARM_RUNTIME_FUNCTION_ENTRY sits behind a separate feature gate;
    // the field is only ever an opaque pointer here.
    pub FunctionEntry: *mut core::ffi::c_void,
}
#[cfg(target_arch = "arm")]
impl Default for UNWIND_HISTORY_TABLE_ENTRY {
    fn default() -> Self {
        unsafe { core::mem::zeroed() }
    }
}
#[repr(C, packed(4))]
#[cfg(target_arch = "arm")]
#[cfg(feature = "Win32_System_Kernel")]
#[derive(Clone, Copy)]
pub struct MINIDUMP_THREAD_CALLBACK {
    pub ThreadId: u32,
    pub ThreadHandle: super::super::super::Foundation::HANDLE,
    pub Context: CONTEXT,
    pub SizeOfContext: u32,
    pub StackBase: u64,
    pub StackEnd: u64,
}
#[cfg(target_arch = "arm")]
#[cfg(feature = "Win32_System_Kernel")]
impl Default for MINIDUMP_THREAD_CALLBACK {
    fn default() -> Self {
        unsafe { core::mem::zeroed() }
    }
}
#[repr(C, packed(4))]
#[cfg(target_arch = "arm")]
#[cfg(feature = "Win32_System_Kernel")]
#[derive(Clone, Copy)]
pub struct MINIDUMP_THREAD_EX_CALLBACK {
    pub ThreadId: u32,
    pub ThreadHandle: super::super::super::Foundation::HANDLE,
    pub Context: CONTEXT,
    pub SizeOfContext: u32,
    pub StackBase: u64,
    pub StackEnd: u64,
    pub BackingStoreBase: u64,
    pub BackingStoreEnd: u64,
}
#[cfg(target_arch = "arm")]
#[cfg(feature = "Win32_System_Kernel")]
impl Default for MINIDUMP_THREAD_EX_CALLBACK {
    fn default() -> Self {
        unsafe { core::mem::zeroed() }
    }
}
"""

APPEND = {
    os.path.join("Windows", "Win32", "System", "Kernel", "mod.rs"): KERNEL_ARM,
    os.path.join("Windows", "Win32", "System", "Diagnostics", "Debug",
                 "mod.rs"): DEBUG_ARM,
}


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def patch_crate(src):
    src = os.path.normpath(src)
    if not os.path.isdir(src):
        print("skip (not found): %s" % src)
        return
    print("== %s" % src)

    gates = 0
    aliases = 0
    for root, _dirs, names in os.walk(src):
        for name in names:
            if not name.endswith(".rs"):
                continue
            path = os.path.join(root, name)
            text = read(path)
            if GATE_OLD not in text:
                continue
            text = text.replace(GATE_OLD, GATE_NEW)
            gates += 1
            # Undo the gate wherever the 32-bit alias already provides the name.
            lines = text.split("\n")
            for i, line in enumerate(lines):
                if 'target_arch = "arm"' not in line:
                    continue
                if LONGPTR.search(" ".join(lines[i + 1:i + 4])):
                    lines[i] = line.replace('target_arch = "arm", ', "")
                    aliases += 1
            write(path, "\n".join(lines))
    print("   files gated for arm: %d, LongPtr aliases left to the 32-bit "
          "path: %d" % (gates, aliases))

    for rel, block in APPEND.items():
        path = os.path.join(src, rel)
        if not os.path.isfile(path):
            print("   skip (no %s)" % rel)
            continue
        text = read(path)
        if MARKER in text:
            print("   already has ARM32 types: %s" % rel)
            continue
        write(path, text + "\n" + block)
        print("   appended ARM32 types: %s" % rel)

    # Vendored crates carry per-file checksums that in-place edits invalidate.
    checksum = os.path.join(os.path.dirname(src), ".cargo-checksum.json")
    if os.path.isfile(checksum):
        data = json.load(open(checksum))
        if data.get("files"):
            data["files"] = {}
            json.dump(data, open(checksum, "w"), separators=(",", ":"))
            print("   cleared .cargo-checksum.json file hashes")


def main():
    for crate in (sys.argv[1:] or DEFAULT_CRATES):
        patch_crate(crate)


if __name__ == "__main__":
    main()
