// ClipboardBridge.h -- the phone's clipboard, for the engine.
//
// The engine runs headless, and the headless clipboard kept what was copied
// in memory and nowhere else: text copied in the browser could not be pasted
// in another app, and text copied there could not be pasted here. The
// engine's paste asks synchronously on its own main thread, so neither side
// waits for the other (widget/headless/HeadlessClipboard.cpp):
//  - a copy in the browser comes here and is put on the phone's clipboard;
//  - the phone's clipboard is read when the app is activated, when a field
//    is about to be typed into, and once the engine has drawn, and the text
//    is left with the engine for its next paste.
// ContentChanged is never used: on the phone it waited on Gecko's thread for
// good, and on the UI thread that kept the browser from starting. Reads and
// writes run on a single-threaded apartment of our own, or on the UI thread
// if the phone refuses them there; a way that ever hangs is not used again in
// that build. Text, and HTML with it when the browser copied some; the logs
// give sizes only, never what was copied.
#pragma once

#include <windows.h>

#include <winrt/Windows.UI.Core.h>

namespace gecko_w10m::client {

// Resolves the engine's entry points in xul and installs the shell's side.
// Safe to call more than once; does nothing after the first success.
void InstallClipboardBridge(HMODULE xul,
                            winrt::Windows::UI::Core::CoreDispatcher const& ui);

// The engine drew its first frame: nothing touches the phone's clipboard
// before, so it can never keep the browser from starting.
void ClipboardEngineDrawing();

// The app's window was activated (true) or deactivated (false). The phone's
// clipboard is read on activation -- another app may have copied something
// meanwhile -- and a copy it refused while we were not in front is put on it.
void ClipboardWindowActivated(bool active);

// The app's window went out of sight or came back. Out of sight another app
// may copy, so a browser copy not yet on the phone is let go.
void ClipboardWindowVisible(bool visible);

// Reads the phone's clipboard again, for a field about to be typed into.
void ClipboardRefresh(const wchar_t* why);

}  // namespace gecko_w10m::client
