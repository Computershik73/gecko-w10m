// FileBridge.h -- the system's file dialogs, file launching and getting
// downloads out of the app, for the engine.
//
// The engine runs headless in an app container: it has no IFileOpenDialog and
// may not ShellExecute. Its file picker (widget/windows/nsFilePicker.cpp) and
// nsLocalFile::Launch / Reveal hand the work to the functions installed here,
// which show FileOpenPicker / FolderPicker / FileSavePicker and call
// Windows.System.Launcher on the UI thread.
//
// With interop unlock alone the app may write no folder outside its own by
// path, so the engine downloads into LocalState\Downloads (the shell decides,
// engine/gecko_bootstrap.cpp) and chrome asks, for each finished download,
//   {"id":N,"op":"file.export","path":"<staged file>","dir":"<chosen folder>"}
// and the file is copied out through the storage API: into the file picked
// in a Save As, the folder picked in the settings, or the phone's
// Downloads\Gecko. The answer is
//   {"id":N,"op":"file.export","ok":true|false,"path":..,"where":..,"error":..}
#pragma once

#include <windows.h>

#include <string>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.UI.Core.h>

namespace gecko_w10m::client {

// Resolves the engine's entry points in xul and installs the shell's side.
// Safe to call more than once; does nothing after the first success.
void InstallFileBridge(HMODULE xul,
                       winrt::Windows::UI::Core::CoreDispatcher const& ui);

// A "file.*" message from chrome (client/DrmBridge.cpp routes it), on a
// worker thread in the multithreaded apartment, so it may wait. Always
// answers through send.
void OnFileMessage(double id, std::wstring const& op,
                   winrt::Windows::Data::Json::JsonObject const& msg,
                   void (*send)(winrt::Windows::Data::Json::JsonObject const&));

}  // namespace gecko_w10m::client
