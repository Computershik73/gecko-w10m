// OverlayProbe.h — whether a video can be shown as a display layer of its own.
#pragma once

#include <winrt/Windows.UI.Xaml.Controls.h>

namespace gecko_w10m::engine {

// Asks the phone, once, whether its display supports hardware overlays and
// for which formats, and whether the pieces needed to give a video its own
// layer exist (video processor, media swap chains, panel handles). Writes the
// answers to the log ("overlay:" lines) and changes nothing. Call on the UI
// thread; the device work runs on a thread of its own.
void ProbeVideoOverlay(
    winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel);

}  // namespace gecko_w10m::engine
