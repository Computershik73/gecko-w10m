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

// The decisive experiment, for a test build: lays a panel with an NV12 swap
// chain over the top of the window for eight seconds, changes its colour
// sixty times a second, and asks the system every second how each frame was
// shown -- as a hardware overlay or drawn by the GPU into the composition
// ("overlay test:" lines). Call on the UI thread once the browser draws.
void RunOverlayTest(winrt::Windows::UI::Xaml::Controls::Grid const& host,
                    double rawPerView);

}  // namespace gecko_w10m::engine
