// ClipboardBridge.h -- the phone's clipboard, for the engine.
//
// The engine runs headless, and the headless clipboard kept what was copied
// in memory and nowhere else: text copied in the browser could not be pasted
// in another app, and text copied there could not be pasted here. The
// phone's clipboard may only be used on the UI thread, while the engine's
// paste asks synchronously on its own main thread, so neither waits for the
// other (widget/headless/HeadlessClipboard.cpp):
//  - a copy in the browser comes here and is put on the phone's clipboard on
//    the UI thread;
//  - the phone's clipboard is read when it changes and whenever the app
//    comes to the front, and the text left with the engine for its next
//    paste.
// Text, and HTML with it when the browser copied some; the logs give sizes
// only, never what was copied.
#pragma once

#include <windows.h>

#include <winrt/Windows.UI.Core.h>

namespace gecko_w10m::client {

// Resolves the engine's entry points in xul and installs the shell's side.
// Safe to call more than once; does nothing after the first success.
void InstallClipboardBridge(HMODULE xul,
                            winrt::Windows::UI::Core::CoreDispatcher const& ui);

// The app's window was activated: the phone's clipboard is read then, and
// never before the first activation -- asked for earlier, it held the UI
// thread for good. Another app may have copied something meanwhile. Does
// nothing before the bridge is installed. UI thread.
void ClipboardWindowActivated();

// The app's window went out of sight or came back. Only while it is out of
// sight can another app copy anything, so a browser copy the phone refused
// before that is not put back over what may be there now. UI thread.
void ClipboardWindowVisible(bool visible);

}  // namespace gecko_w10m::client
