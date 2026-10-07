// CaptureConsent.h -- the phone's microphone consent, for the engine.
//
// Windows keeps the microphone behind a consent of its own (Settings >
// Privacy > Microphone). Until the app has it, the audio service refuses the
// capture endpoint, and the engine's WASAPI path never asks, so a call in
// Discord or WhatsApp Web started without a microphone. The engine asks here
// right after the user allowed the site (dom/system/
// nsOSPermissionRequestBase.cpp); the shell reads the consent, and asks the
// system for it -- by starting a MediaCapture for audio once, on the UI
// thread -- when it was never given.
#pragma once

#include <windows.h>

#include <winrt/Windows.UI.Core.h>

namespace gecko_w10m::client {

// Resolves the engine's entry points in xul and installs the shell's side.
// Safe to call more than once; does nothing after the first success.
void InstallCaptureConsent(HMODULE xul,
                           winrt::Windows::UI::Core::CoreDispatcher const& ui);

}  // namespace gecko_w10m::client
