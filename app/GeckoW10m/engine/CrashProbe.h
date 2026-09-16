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

// A pulse from a thread of its own, twice a second, carrying the number of
// frames the UI thread has rendered since the last one.
//
// Every report this project has of the process dying ends at the same fault in
// the XAML compositor, and the log simply stops there -- which was read as the
// fault killing the process. It is not evidence of that. The log stops at the
// last thing written, and nothing writes unless something happens. A pulse
// makes silence mean something: if it goes on past the fault, the fault was
// survived and the death is elsewhere; if the frame count freezes while the
// pulse continues, the UI thread died and the system took the process after;
// if both stop together, the fault is the death.
void StartHeartbeat();

// Called from XAML's render callback. Costs one increment.
void NoteUiFrame();

// Makes a second D3D11 device -- the shell's own -- and then finds out how
// much this phone will actually give it.
//
// A second device beside XAML's turned out to be harmless: one was made at
// startup, used, and kept for a whole run, and the browser died exactly where
// it always does, fifteen seconds later. So it is not the device. What is left
// is what the engine asks of it, and the first thing WebRender does is
// allocate: render targets the size of the screen, texture cache pages, a
// staging pool. If this device has a ceiling an app container may not cross,
// the compositor would be the first thing to fail an allocation on the other
// side of it -- and failing an allocation and then writing through the null it
// returned is exactly the instruction the UI thread dies on.
//
// So this allocates screen-sized render targets, one at a time, saying how
// much it has taken each time. Either it stops at a number, which is the
// number we have to keep the engine under, or it does not, and allocation is
// innocent too.
void MakeSecondD3DDevice();

// How large a screen-sized render target is, for the ceiling probe. Set from
// the shell, which is the only thing that knows the screen.
void SetProbeSurfaceSize(int width, int height);

}  // namespace gecko_w10m::engine
