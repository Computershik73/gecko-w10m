// CrashProbe.h — find out how and where Gecko is killing the process.
#pragma once

namespace gecko_w10m::engine {

// Installs everything needed to get a last word out of a dying process:
//
//   * a vectored exception handler, for faults and MOZ_CRASH breakpoints;
//   * hooks on xul.dll's imports of abort, _exit and TerminateProcess, which
//     end the process with no exception at all and so leave no other trace.
//
// Nothing here changes behaviour. Each hook records a backtrace and calls
// straight through to the original.
void InstallCrashProbes();

// Records where a thread is, over and over, so that when the process dies the
// last line written says where it was standing.
//
// This exists because a __fastfail -- how a stack cookie check, a Control Flow
// Guard violation or a range check reports failure -- terminates the process
// without raising anything. No handler runs, no hook is reached, and the only
// way left to locate it is to have been watching.
//
// Takes ownership of the handle.
void StartLastLocationSampler(void* thread);

}  // namespace gecko_w10m::engine
