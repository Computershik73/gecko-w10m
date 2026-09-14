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

}  // namespace gecko_w10m::engine
