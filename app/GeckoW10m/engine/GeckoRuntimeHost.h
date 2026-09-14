// GeckoRuntimeHost.h — starts the Gecko runtime and keeps it off the UI thread.
#pragma once

#include <string>

namespace gecko_w10m::engine {

// Starts Gecko on a thread of its own and returns immediately. XRE_main owns
// its thread until shutdown, so it can never share one with XAML.
//
// Returns false when the attempt was deliberately skipped -- the reason is
// logged either way.
bool StartGeckoRuntime(const std::wstring& localStatePath);

}  // namespace gecko_w10m::engine
