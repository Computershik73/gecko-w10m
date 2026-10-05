// FileBridge.h -- the system's file dialogs and file launching, for the engine.
//
// The engine runs headless in an app container: it has no IFileOpenDialog and
// may not ShellExecute. Its file picker (widget/windows/nsFilePicker.cpp) and
// nsLocalFile::Launch / Reveal hand the work to the functions installed here,
// which show FileOpenPicker / FolderPicker / FileSavePicker and call
// Windows.System.Launcher on the UI thread.
#pragma once

#include <windows.h>

#include <winrt/Windows.UI.Core.h>

namespace gecko_w10m::client {

// Resolves the engine's entry points in xul and installs the shell's side.
// Safe to call more than once; does nothing after the first success.
void InstallFileBridge(HMODULE xul,
                       winrt::Windows::UI::Core::CoreDispatcher const& ui);

}  // namespace gecko_w10m::client
