// jit_probe.cpp
#include "pch.h"
#include "jit_probe.h"

#include <windows.h>
#include <memoryapi.h>
#include <processthreadsapi.h>

#include <sstream>

namespace gecko_w10m::test {

namespace {

// Machine code for `int f() { return N; }`, per target ISA.
// Returns the byte length written and whether the entry needs the Thumb bit.
struct CodeBlob {
  unsigned char bytes[16];
  size_t len;
  bool thumb;
};

CodeBlob MakeReturnConst(unsigned char n) {
  CodeBlob b{};
#if defined(_M_ARM)
  // Thumb-2:  movs r0, #n   (0x20nn)   ;  bx lr (0x4770)
  b.bytes[0] = n;      // low byte of movs imm
  b.bytes[1] = 0x20;   // movs r0
  b.bytes[2] = 0x70;   // bx lr
  b.bytes[3] = 0x47;
  b.len = 4;
  b.thumb = true;
#elif defined(_M_IX86) || defined(_M_X64)
  // mov eax, imm32 ; ret   ->  B8 nn 00 00 00  C3
  b.bytes[0] = 0xB8;
  b.bytes[1] = n;
  b.bytes[2] = 0x00;
  b.bytes[3] = 0x00;
  b.bytes[4] = 0x00;
  b.bytes[5] = 0xC3;
  b.len = 6;
  b.thumb = false;
#elif defined(_M_ARM64)
  // mov w0, #n ; ret  ->  0x52800000|(n<<5) , 0xD65F03C0
  uint32_t mov = 0x52800000u | (uint32_t(n) << 5);
  b.bytes[0] = (mov) & 0xFF;
  b.bytes[1] = (mov >> 8) & 0xFF;
  b.bytes[2] = (mov >> 16) & 0xFF;
  b.bytes[3] = (mov >> 24) & 0xFF;
  b.bytes[4] = 0xC0; b.bytes[5] = 0x03; b.bytes[6] = 0x5F; b.bytes[7] = 0xD6;
  b.len = 8;
  b.thumb = false;
#endif
  return b;
}

using Fn = int (*)();

Fn AsCallable(void* page, bool thumb) {
  uintptr_t addr = reinterpret_cast<uintptr_t>(page);
  if (thumb) addr |= 1u;
  return reinterpret_cast<Fn>(addr);
}

const wchar_t* Arch() {
#if defined(_M_ARM)
  return L"ARM (Thumb-2)";
#elif defined(_M_ARM64)
  return L"ARM64";
#elif defined(_M_X64)
  return L"x64";
#elif defined(_M_IX86)
  return L"x86";
#else
  return L"unknown";
#endif
}

}  // namespace

JitProbeResult RunJitProbe() {
  JitProbeResult r{};
  std::wstringstream log;
  log << L"GeckoW10m JIT probe\n";
  log << L"Architecture: " << Arch() << L"\n\n";

  const size_t pageBytes = 4096;

  // 1) Commit a page as PAGE_READWRITE via the app-container allocator.
  void* page = ::VirtualAllocFromApp(nullptr, pageBytes,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!page) {
    log << L"[FAIL] VirtualAllocFromApp(PAGE_READWRITE) err="
        << ::GetLastError() << L"\n";
    r.report = log.str();
    return r;
  }
  r.rwCommit = true;
  log << L"[ ok ] VirtualAllocFromApp(PAGE_READWRITE)\n";

  // 2) Write `return 42` and make it executable.
  CodeBlob blob = MakeReturnConst(42);
  memcpy(page, blob.bytes, blob.len);

  ULONG old = 0;
  if (!::VirtualProtectFromApp(page, pageBytes, PAGE_EXECUTE_READ, &old)) {
    log << L"[FAIL] VirtualProtectFromApp(PAGE_EXECUTE_READ) err="
        << ::GetLastError()
        << L"  -> codeGeneration capability is NOT active.\n";
    ::VirtualFree(page, 0, MEM_RELEASE);
    r.report = log.str();
    return r;
  }
  r.madeExecutable = true;
  ::FlushInstructionCache(::GetCurrentProcess(), page, blob.len);
  log << L"[ ok ] VirtualProtectFromApp(PAGE_EXECUTE_READ) + FlushInstructionCache\n";

  // 3) Execute it.
  r.returnedFirst = AsCallable(page, blob.thumb)();
  r.executed = (r.returnedFirst == 42);
  log << (r.executed ? L"[ ok ] " : L"[FAIL] ")
      << L"executed JIT code, returned " << r.returnedFirst
      << L" (expected 42)\n";

  // 4) W^X toggle: back to writable, rewrite `return 7`, re-execute.
  if (::VirtualProtectFromApp(page, pageBytes, PAGE_READWRITE, &old)) {
    CodeBlob blob2 = MakeReturnConst(7);
    memcpy(page, blob2.bytes, blob2.len);
    if (::VirtualProtectFromApp(page, pageBytes, PAGE_EXECUTE_READ, &old)) {
      ::FlushInstructionCache(::GetCurrentProcess(), page, blob2.len);
      r.returnedSecond = AsCallable(page, blob2.thumb)();
      r.wxToggle = (r.returnedSecond == 7);
      log << (r.wxToggle ? L"[ ok ] " : L"[FAIL] ")
          << L"W^X toggle re-executed, returned " << r.returnedSecond
          << L" (expected 7)\n";
    }
  }

  ::VirtualFree(page, 0, MEM_RELEASE);

  // 5) Direct RWX allocation should be refused under codeGeneration; that is
  //    the expected, correct behaviour (SpiderMonkey never needs it).
  void* rwx = ::VirtualAllocFromApp(nullptr, pageBytes, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
  if (rwx) {
    log << L"[note] direct PAGE_EXECUTE_READWRITE alloc succeeded "
           L"(unusual; not required).\n";
    ::VirtualFree(rwx, 0, MEM_RELEASE);
  } else {
    r.rwxRejected = true;
    log << L"[ ok ] direct RWX alloc refused as expected (W^X enforced).\n";
  }

  r.passed = r.rwCommit && r.madeExecutable && r.executed && r.wxToggle;

  log << L"\n" << (r.passed ? L"RESULT: PASS — JIT works on this device."
                            : L"RESULT: FAIL — see lines above.");
  r.report = log.str();
  return r;
}

}  // namespace gecko_w10m::test
