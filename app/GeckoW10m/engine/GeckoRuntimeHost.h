// GeckoRuntimeHost.h — starts the Gecko runtime and keeps it off the UI thread.
#pragma once

#include <string>

namespace gecko_w10m::engine {

// Starts Gecko on a thread of its own and returns immediately. XRE_main owns
// its thread until shutdown, so it can never share one with XAML.
//
// Returns false when the attempt was deliberately skipped -- the reason is
// logged either way.
// width/height are the size in physical pixels of the area the shell can show.
// Gecko is headless here, so this is the only thing that decides how big the
// window it opens will be. Zero leaves Gecko's own default alone.
// Called once the engine has drawn something, which is what clears the
// failed-attempt count. Nothing else is evidence that a start succeeded.
void MarkGeckoHealthy(const std::wstring& localStatePath);

// The app is being suspended. One suspended before its first frame -- and
// then ended by the system, as a phone does -- did not fail to start, and is
// not counted as if it had. UI thread.
void NoteAppSuspended();

enum class StartResult {
  Started,
  // This launch leaves the engine alone after failed starts; the next one
  // tries again.
  SkippedAfterFailures,
  Failed,
};

StartResult StartGeckoRuntime(const std::wstring& localStatePath, int width,
                              int height, double scale);

}  // namespace gecko_w10m::engine
