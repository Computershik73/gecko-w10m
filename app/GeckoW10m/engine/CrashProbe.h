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

// Makes a second D3D11 device -- the shell's own -- and then pushes work
// through it, alongside the compositor, for fifteen seconds.
//
// Two things about this device are now known and neither is the answer: a
// second device beside XAML's is harmless, and so is taking memory from it --
// forty-eight screen-sized render targets, six hundred and seventy-five
// megabytes, while XAML went on drawing sixty frames a second. The adapter
// says ninety-six megabytes dedicated and one and a half gigabytes shared, and
// it never refused any of it.
//
// What the probe has never done is submit. On a phone the user-mode driver is
// one library shared by every device in the process, and the compositor is
// pushing through it from the UI thread all the while. That is the last thing
// the engine does that this has not, it starts the moment hardware compositing
// comes on, and that is the line every dying run since 0.1.9.5 is on the wrong
// side of.
void MakeSecondD3DDevice();

// How large a frame the traffic probe pushes, which is the size of the ones
// WebRender pushes. Set from the shell, which is the only thing that knows
// the screen.
void SetProbeSurfaceSize(int width, int height);

}  // namespace gecko_w10m::engine
