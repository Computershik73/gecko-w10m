// ClipboardBridge.h -- the phone's clipboard, for the engine.
//
// The engine runs headless, and the headless clipboard kept what was copied
// in memory and nowhere else: text copied in the browser could not be pasted
// in another app, and text copied there could not be pasted here. The
// engine's paste asks synchronously on its own main thread, so neither side
// waits for the other (widget/headless/HeadlessClipboard.cpp):
//  - a copy in the browser comes here and is put on the phone's clipboard;
//  - the phone's clipboard is read when it changes and whenever the app is
//    activated, and the text left with the engine for its next paste.
// Every call into the phone's clipboard runs on a thread of its own, never
// on the UI thread: on the phone the first such call did not come back, and
// on the UI thread that kept the browser from starting at all. Text, and HTML
// with it when the browser copied some; the logs give sizes only, never what
// was copied.
#pragma once

#include <windows.h>

#include <winrt/Windows.UI.Core.h>

namespace gecko_w10m::client {

// Resolves the engine's entry points in xul and installs the shell's side.
// Safe to call more than once; does nothing after the first success. If the
// last launch never heard back from the phone's clipboard, the engine keeps
// its own clipboard for this one.
void InstallClipboardBridge(HMODULE xul,
                            winrt::Windows::UI::Core::CoreDispatcher const& ui);

// The app's window was activated: the phone's clipboard is read then (on
// the clipboard's own thread), and never before the first activation.
// Another app may have copied something meanwhile.
void ClipboardWindowActivated();

// The app's window went out of sight or came back. Only while it is out of
// sight can another app copy anything, so a browser copy the phone refused
// before that is not put back over what may be there now.
void ClipboardWindowVisible(bool visible);

}  // namespace gecko_w10m::client
