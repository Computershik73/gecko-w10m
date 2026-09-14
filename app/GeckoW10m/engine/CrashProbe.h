// CrashProbe.h — find out how and where Gecko is killing the process.
#pragma once

#include <string>

namespace gecko_w10m::engine {

// Installs everything needed to get a last word out of a dying process:
//
//   * a vectored exception handler, which sees every fault first chance --
//     including ones the process goes on to handle perfectly well;
//   * an unhandled exception filter, which sees only the ones that are
//     actually about to end the process, and so tells noise from cause;
//   * hooks on xul.dll's imports of abort, _exit and TerminateProcess, which
//     end the process with no exception at all and so leave no other trace.
//
// Nothing here changes behaviour. Each hook records a backtrace and calls
// straight through to the original.
// Call this as early in the process as there is a log to write to, well
// before the engine starts. The exception handlers were previously installed
// alongside Gecko, which meant every fault they reported looked like Gecko's
// doing -- there was no way to tell an engine crash from something the host
// does on every launch regardless.
//
// localStatePath is where the PC trace lives, and where the previous run's
// trace is read back from before this one overwrites it.
void InstallProcessProbes(const std::wstring& localStatePath);

// Redirects xul.dll's own exit paths, so it needs the engine loaded. Separate
// from the above only because of that ordering.
void InstallEngineProbes();

// Records where a thread is, over and over, so that when the process dies
// there is a trace of where it had got to.
//
// This exists because a __fastfail -- how a stack cookie check, a Control Flow
// Guard violation or a range check reports failure -- terminates the process
// without raising anything. No handler runs, no hook is reached, and the only
// way left to locate it is to have been watching.
//
// Samples go to a memory-mapped ring rather than the log: writing and flushing
// a line costs most of a millisecond, which held the rate down to a handful of
// samples across an entire startup. Mapped pages cost nothing per sample and
// the memory manager still writes them back after the process dies, so the
// trace is waiting on disk at the next launch.
//
// Takes ownership of the handle.
void StartLastLocationSampler(void* thread);

}  // namespace gecko_w10m::engine
