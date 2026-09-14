// jit_probe.h — self-contained test of UWP executable memory (codeGeneration).
// No dependency on Gecko; use it to confirm JIT works on the device before the
// full engine build.
#pragma once

#include <string>

namespace gecko_w10m::test {

struct JitProbeResult {
  bool passed = false;         // the mandatory W^X path worked end-to-end
  bool rwCommit = false;       // VirtualAllocFromApp(PAGE_READWRITE)
  bool madeExecutable = false; // VirtualProtectFromApp(PAGE_EXECUTE_READ)
  bool executed = false;       // called JIT code, got expected value
  bool wxToggle = false;       // flipped back to RW, rewrote, re-executed
  bool rwxRejected = false;    // direct RWX alloc correctly refused (expected)
  int  returnedFirst = 0;
  int  returnedSecond = 0;
  std::wstring report;         // human-readable multi-line summary
};

// Runs the full probe and fills a result + report.
JitProbeResult RunJitProbe();

}  // namespace gecko_w10m::test
