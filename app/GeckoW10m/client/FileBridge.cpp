// FileBridge.cpp -- see FileBridge.h.
#include "pch.h"

#include "client/FileBridge.h"

#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Storage.Pickers.h>

#include "client/Log.h"

using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Foundation::Collections::IVectorView;
using winrt::Windows::Storage::ApplicationData;
using winrt::Windows::Storage::NameCollisionOption;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::Storage::StorageFolder;
using namespace winrt::Windows::Storage::Pickers;

namespace gecko_w10m::client {

namespace {

// The engine's side (nsFilePicker.cpp, nsLocalFileWin.cpp).
using PickerSink = void (*)(uint32_t id, int32_t mode, const wchar_t* title,
                            const wchar_t* initialDir,
                            const wchar_t* defaultName,
                            const wchar_t* defaultExtension,
                            const wchar_t* filters);
using SetPickerSinkFn = void (*)(PickerSink);
using PickerResultFn = void (*)(uint32_t id, const wchar_t* paths);
using FileLauncher = void (*)(const wchar_t* path, int32_t reveal);
using SetFileLauncherFn = void (*)(FileLauncher);

// nsIFilePicker::Mode
constexpr int32_t kModeOpen = 0;
constexpr int32_t kModeSave = 1;
constexpr int32_t kModeGetFolder = 2;
constexpr int32_t kModeOpenMultiple = 3;

winrt::Windows::UI::Core::CoreDispatcher gUi{nullptr};
PickerResultFn gPickerResult = nullptr;
bool gInstalled = false;

std::wstring Copy(const wchar_t* text) { return text ? text : L""; }

// Answers the engine: the paths, each NUL-ended, the list ended by another
// NUL; an empty list is a cancel. Always called, so the engine's pending
// picker is never left waiting.
void Answer(uint32_t id, std::vector<std::wstring> const& paths) {
  std::wstring packed;
  for (auto const& path : paths) {
    if (!path.empty()) {
      packed += path;
      packed.push_back(L'\0');
    }
  }
  packed.push_back(L'\0');
  Log::Write(L"picker: " + std::to_wstring(id) + L" answered, " +
             std::to_wstring(paths.size()) + L" path(s)" +
             (paths.empty() ? L"" : L", first " + paths.front()));
  if (gPickerResult) {
    gPickerResult(id, packed.c_str());
  }
}

// A picked file the engine can open by path. Items with no path (in a
// virtual location) are copied into the app's temporary folder first; the
// answer waits for all of them.
void AnswerWithFiles(uint32_t id, IVectorView<StorageFile> const& files) {
  struct Pending {
    uint32_t id;
    std::vector<std::wstring> paths;
    size_t left = 0;
  };
  auto pending = std::make_shared<Pending>();
  pending->id = id;
  if (!files || files.Size() == 0) {
    Answer(id, {});
    return;
  }
  pending->paths.resize(files.Size());
  pending->left = files.Size();
  auto finishOne = [pending]() {
    if (--pending->left == 0) {
      Answer(pending->id, pending->paths);
    }
  };
  for (uint32_t i = 0; i < files.Size(); ++i) {
    StorageFile file = files.GetAt(i);
    std::wstring path(file.Path());
    if (!path.empty()) {
      pending->paths[i] = path;
      finishOne();
      continue;
    }
    try {
      file.CopyAsync(ApplicationData::Current().TemporaryFolder(), file.Name(),
                     NameCollisionOption::GenerateUniqueName)
          .Completed([pending, i, finishOne](
                         IAsyncOperation<StorageFile> const& op,
                         AsyncStatus status) {
            if (status == AsyncStatus::Completed) {
              pending->paths[i] = std::wstring(op.GetResults().Path());
            }
            finishOne();
          });
    } catch (...) {
      finishOne();
    }
  }
}

// "*.png; *.jpg" lines from the engine's filters, as ".png", ".jpg"; empty
// when any filter takes everything.
std::vector<std::wstring> Extensions(std::wstring const& filters) {
  std::vector<std::wstring> out;
  size_t start = 0;
  while (start < filters.size()) {
    size_t end = filters.find(L'\n', start);
    if (end == std::wstring::npos) end = filters.size();
    std::wstring line = filters.substr(start, end - start);
    start = end + 1;
    const size_t bar = line.find(L'|');
    std::wstring spec = bar == std::wstring::npos ? line : line.substr(bar + 1);
    size_t s = 0;
    while (s < spec.size()) {
      size_t e = spec.find(L';', s);
      if (e == std::wstring::npos) e = spec.size();
      std::wstring pattern = spec.substr(s, e - s);
      s = e + 1;
      while (!pattern.empty() && pattern.front() == L' ') pattern.erase(0, 1);
      while (!pattern.empty() && pattern.back() == L' ') pattern.pop_back();
      if (pattern == L"*" || pattern == L"*.*") return {};
      if (pattern.size() > 2 && pattern[0] == L'*' && pattern[1] == L'.' &&
          pattern.find_first_of(L"*?", 1) == std::wstring::npos) {
        out.push_back(pattern.substr(1));
      }
    }
  }
  return out;
}

void ShowOpenPicker(uint32_t id, bool multiple, std::wstring const& filters) {
  FileOpenPicker picker;
  picker.ViewMode(PickerViewMode::List);
  picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
  const auto extensions = Extensions(filters);
  if (extensions.empty()) {
    picker.FileTypeFilter().Append(L"*");
  } else {
    for (auto const& extension : extensions) {
      picker.FileTypeFilter().Append(winrt::hstring(extension));
    }
  }
  if (multiple) {
    picker.PickMultipleFilesAsync().Completed(
        [id](IAsyncOperation<IVectorView<StorageFile>> const& op,
             AsyncStatus status) {
          if (status != AsyncStatus::Completed) {
            Answer(id, {});
            return;
          }
          AnswerWithFiles(id, op.GetResults());
        });
    return;
  }
  picker.PickSingleFileAsync().Completed(
      [id](IAsyncOperation<StorageFile> const& op, AsyncStatus status) {
        StorageFile file = status == AsyncStatus::Completed ? op.GetResults()
                                                            : nullptr;
        if (!file) {
          Answer(id, {});
          return;
        }
        auto one = winrt::single_threaded_vector<StorageFile>({file});
        AnswerWithFiles(id, one.GetView());
      });
}

void ShowFolderPicker(uint32_t id) {
  FolderPicker picker;
  picker.SuggestedStartLocation(PickerLocationId::Downloads);
  picker.FileTypeFilter().Append(L"*");
  picker.PickSingleFolderAsync().Completed(
      [id](IAsyncOperation<StorageFolder> const& op, AsyncStatus status) {
        StorageFolder folder =
            status == AsyncStatus::Completed ? op.GetResults() : nullptr;
        if (!folder || folder.Path().empty()) {
          Answer(id, {});
          return;
        }
        Answer(id, {std::wstring(folder.Path())});
      });
}

void ShowSavePicker(uint32_t id, std::wstring name, std::wstring extension) {
  FileSavePicker picker;
  picker.SuggestedStartLocation(PickerLocationId::Downloads);
  // The type list may not be empty: the extension asked for, else the one
  // the suggested name has, else none in particular.
  if (extension.empty()) {
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot + 1 < name.size()) {
      extension = name.substr(dot + 1);
    }
  }
  if (!extension.empty() && extension.front() == L'.') {
    extension.erase(0, 1);
  }
  const std::wstring dotted = L"." + (extension.empty() ? L"bin" : extension);
  auto types = winrt::single_threaded_vector<winrt::hstring>(
      {winrt::hstring(dotted)});
  picker.FileTypeChoices().Insert(
      winrt::hstring(extension.empty() ? L"File" : extension), types);
  picker.DefaultFileExtension(winrt::hstring(dotted));
  // The picker adds the extension itself.
  if (name.size() > dotted.size() &&
      _wcsicmp(name.c_str() + name.size() - dotted.size(), dotted.c_str()) ==
          0) {
    name.resize(name.size() - dotted.size());
  }
  if (!name.empty()) {
    picker.SuggestedFileName(winrt::hstring(name));
  }
  picker.PickSaveFileAsync().Completed(
      [id](IAsyncOperation<StorageFile> const& op, AsyncStatus status) {
        StorageFile file =
            status == AsyncStatus::Completed ? op.GetResults() : nullptr;
        if (!file || file.Path().empty()) {
          Answer(id, {});
          return;
        }
        Answer(id, {std::wstring(file.Path())});
      });
}

// From the engine's main thread. Never blocks it: the arguments are copied
// and the picker is shown on the UI thread, which pickers belong to.
void OnPicker(uint32_t id, int32_t mode, const wchar_t* title,
              const wchar_t* initialDir, const wchar_t* defaultName,
              const wchar_t* defaultExtension, const wchar_t* filters) {
  std::wstring name = Copy(defaultName);
  std::wstring extension = Copy(defaultExtension);
  std::wstring filterText = Copy(filters);
  Log::Write(L"picker: " + std::to_wstring(id) + L" asked, mode " +
             std::to_wstring(mode) + L", title " + Copy(title) +
             L", from " + Copy(initialDir));
  if (!gUi) {
    Answer(id, {});
    return;
  }
  gUi.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
               [id, mode, name, extension, filterText]() {
                 try {
                   switch (mode) {
                     case kModeGetFolder:
                       ShowFolderPicker(id);
                       break;
                     case kModeSave:
                       ShowSavePicker(id, name, extension);
                       break;
                     case kModeOpenMultiple:
                       ShowOpenPicker(id, true, filterText);
                       break;
                     case kModeOpen:
                     default:
                       ShowOpenPicker(id, false, filterText);
                       break;
                   }
                 } catch (winrt::hresult_error const& error) {
                   Log::Write(L"picker: the system refused",
                              std::wstring(error.message()));
                   Answer(id, {});
                 } catch (...) {
                   Answer(id, {});
                 }
               });
}

// From the engine's main thread: open a file with the app the system has
// for it, or show the folder it is in.
void OnLaunchFile(const wchar_t* path, int32_t reveal) {
  std::wstring target = Copy(path);
  if (target.empty() || !gUi) {
    return;
  }
  gUi.RunAsync(
      winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
      [target, reveal]() {
        try {
          if (reveal) {
            std::wstring folder = target;
            const size_t slash = folder.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
              folder.resize(slash);
            }
            StorageFolder::GetFolderFromPathAsync(winrt::hstring(folder))
                .Completed([folder](IAsyncOperation<StorageFolder> const& op,
                                    AsyncStatus status) {
                  if (status != AsyncStatus::Completed) {
                    Log::Write(L"open: no folder at " + folder);
                    return;
                  }
                  try {
                    winrt::Windows::System::Launcher::LaunchFolderAsync(
                        op.GetResults());
                    Log::Write(L"open: showing the folder " + folder);
                  } catch (winrt::hresult_error const& error) {
                    Log::Write(L"open: the system would not show " + folder,
                               std::wstring(error.message()));
                  }
                });
            return;
          }
          StorageFile::GetFileFromPathAsync(winrt::hstring(target))
              .Completed([target](IAsyncOperation<StorageFile> const& op,
                                  AsyncStatus status) {
                if (status != AsyncStatus::Completed) {
                  Log::Write(L"open: no file at " + target);
                  return;
                }
                try {
                  winrt::Windows::System::Launcher::LaunchFileAsync(
                      op.GetResults());
                  Log::Write(L"open: asked the system to open " + target);
                } catch (winrt::hresult_error const& error) {
                  Log::Write(L"open: the system refused " + target,
                             std::wstring(error.message()));
                }
              });
        } catch (winrt::hresult_error const& error) {
          Log::Write(L"open: could not open " + target,
                     std::wstring(error.message()));
        }
      });
}

}  // namespace

void InstallFileBridge(HMODULE xul,
                       winrt::Windows::UI::Core::CoreDispatcher const& ui) {
  if (gInstalled || !xul) {
    return;
  }
  gUi = ui;
  auto setPicker = reinterpret_cast<SetPickerSinkFn>(
      ::GetProcAddress(xul, "gecko_w10m_set_picker_sink"));
  gPickerResult = reinterpret_cast<PickerResultFn>(
      ::GetProcAddress(xul, "gecko_w10m_picker_result"));
  auto setLauncher = reinterpret_cast<SetFileLauncherFn>(
      ::GetProcAddress(xul, "gecko_w10m_set_file_launcher"));
  if (!setPicker || !gPickerResult) {
    return;
  }
  setPicker(&OnPicker);
  if (setLauncher) {
    setLauncher(&OnLaunchFile);
  }
  gInstalled = true;
  Log::Write(L"view: file dialogs and opening files go through the shell");
}

}  // namespace gecko_w10m::client
