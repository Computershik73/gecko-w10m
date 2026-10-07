// FileBridge.cpp -- see FileBridge.h.
#include "pch.h"

#include "client/FileBridge.h"

#include <algorithm>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.Pickers.h>

#include "client/Log.h"

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Foundation::Collections::IVectorView;
using winrt::Windows::Storage::ApplicationData;
using winrt::Windows::Storage::CreationCollisionOption;
using winrt::Windows::Storage::DownloadsFolder;
using winrt::Windows::Storage::IStorageItem;
using winrt::Windows::Storage::NameCollisionOption;
using winrt::Windows::Storage::StorageDeleteOption;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::Storage::StorageFolder;
using winrt::Windows::Storage::AccessCache::StorageApplicationPermissions;
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

// A staged download is kept this long after it was copied out, so the
// browser can still show and open it; then only the copy is left.
constexpr unsigned long long kKeepStagedFor = 2ull * 24 * 60 * 60;  // seconds
// Copies of picked files the engine could not read, for uploads.
constexpr unsigned long long kKeepPickedFor = 24ull * 60 * 60;
// The future access list holds a thousand; old export entries go first.
constexpr uint32_t kAccessListRoom = 800;

winrt::Windows::UI::Core::CoreDispatcher gUi{nullptr};
PickerResultFn gPickerResult = nullptr;
bool gInstalled = false;

std::wstring Copy(const wchar_t* text) { return text ? text : L""; }

std::wstring Lower(std::wstring text) {
  std::transform(text.begin(), text.end(), text.begin(), ::towlower);
  return text;
}

// Where the engine downloads when it may not write the phone's Downloads;
// empty when it may (engine/gecko_bootstrap.cpp).
std::wstring StagingDir() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD n = ::GetEnvironmentVariableW(L"GECKO_W10M_DOWNLOAD_STAGING",
                                            buffer, MAX_PATH);
  return n && n < MAX_PATH ? std::wstring(buffer, n) : std::wstring();
}

bool UnderStaging(std::wstring const& path) {
  const std::wstring staging = StagingDir();
  return !staging.empty() && path.size() > staging.size() + 1 &&
         Lower(path.substr(0, staging.size() + 1)) == Lower(staging + L"\\");
}

// Future access list tokens are named after the path they are for: "x" a
// download's copy, by the staged path; "s" the file picked in a Save As, by
// the staged path handed to the engine instead; "d" a folder picked in the
// settings, by its path.
std::wstring Token(wchar_t kind, std::wstring const& path) {
  unsigned long long hash = 14695981039346656037ull;
  for (wchar_t c : Lower(path)) {
    hash ^= static_cast<unsigned long long>(c);
    hash *= 1099511628211ull;
  }
  wchar_t text[24];
  swprintf_s(text, L"%c%016llx", kind, hash);
  return text;
}

std::wstring NowStamp() {
  return std::to_wstring(static_cast<unsigned long long>(_time64(nullptr)));
}

void Remember(std::wstring const& token, IStorageItem const& item) {
  auto list = StorageApplicationPermissions::FutureAccessList();
  try {
    if (list.Entries().Size() >= kAccessListRoom) {
      // Oldest first by the time written as each entry's metadata.
      std::vector<std::pair<unsigned long long, winrt::hstring>> exports;
      for (auto const& entry : list.Entries()) {
        if (!entry.Token.empty() && entry.Token[0] == L'x') {
          exports.emplace_back(wcstoull(entry.Metadata.c_str(), nullptr, 10),
                               entry.Token);
        }
      }
      std::sort(exports.begin(), exports.end());
      for (size_t i = 0; i < exports.size() && i < 100; ++i) {
        list.Remove(exports[i].second);
      }
    }
    list.AddOrReplace(winrt::hstring(token), item, winrt::hstring(NowStamp()));
  } catch (winrt::hresult_error const& error) {
    Log::Write(L"files: could not remember " + token,
               std::wstring(error.message()));
  }
}

bool Remembered(std::wstring const& token) {
  try {
    return StorageApplicationPermissions::FutureAccessList().ContainsItem(
        winrt::hstring(token));
  } catch (...) {
    return false;
  }
}

void Forget(std::wstring const& token) {
  try {
    StorageApplicationPermissions::FutureAccessList().Remove(
        winrt::hstring(token));
  } catch (...) {
  }
}

// What the engine (Win32, by path) may do with a path. Interop unlock alone
// lets the storage API reach places Win32 may not. CreateFile2 is the app
// container's CreateFile.
bool Win32CanRead(std::wstring const& path) {
  HANDLE h = ::CreateFile2(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           OPEN_EXISTING, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  ::CloseHandle(h);
  return true;
}

bool Win32CanWrite(std::wstring const& path) {
  HANDLE h = ::CreateFile2(path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           OPEN_ALWAYS, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  ::CloseHandle(h);
  return true;
}

// Answers the engine: the paths, each NUL-ended, the list ended by another
// NUL; an empty list is a cancel. Always called, so the engine's pending
// picker is never left waiting.
void Answer(uint32_t id, std::vector<std::wstring> const& paths) {
  std::wstring packed;
  size_t count = 0;
  for (auto const& path : paths) {
    if (!path.empty()) {
      packed += path;
      packed.push_back(L'\0');
      ++count;
    }
  }
  packed.push_back(L'\0');
  Log::Write(L"picker: " + std::to_wstring(id) + L" answered, " +
             std::to_wstring(count) + L" path(s)" +
             (paths.empty() ? L"" : L", first " + paths.front()));
  if (gPickerResult) {
    gPickerResult(id, packed.c_str());
  }
}

// Picked files the engine can open by path. Any it cannot -- no path at all
// (a virtual location), or one Win32 may not read, which with interop unlock
// alone is most of the phone -- is copied into the app's temporary folder
// first, in a folder of its own so the name the site sees is the file's.
void AnswerWithFiles(uint32_t id, IVectorView<StorageFile> const& files) {
  if (!files || files.Size() == 0) {
    Answer(id, {});
    return;
  }
  std::vector<StorageFile> list(files.begin(), files.end());
  std::thread([id, list]() {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    std::vector<std::wstring> paths;
    for (auto const& file : list) {
      std::wstring path(file.Path());
      if (!path.empty() && Win32CanRead(path)) {
        paths.push_back(path);
        continue;
      }
      try {
        StorageFolder folder =
            ApplicationData::Current()
                .TemporaryFolder()
                .CreateFolderAsync(L"picked",
                                   CreationCollisionOption::GenerateUniqueName)
                .get();
        StorageFile copy =
            file.CopyAsync(folder, file.Name(),
                           NameCollisionOption::ReplaceExisting)
                .get();
        paths.push_back(std::wstring(copy.Path()));
        Log::Write(L"picker: the engine may not read " +
                   (path.empty() ? std::wstring(file.Name()) : path) +
                   L", handing it a copy");
      } catch (winrt::hresult_error const& error) {
        Log::Write(L"picker: could not copy " + std::wstring(file.Name()),
                   std::wstring(error.message()));
      }
    }
    Answer(id, paths);
  }).detach();
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
        // Kept, so a download staged in LocalState can be copied into it
        // later, when the engine itself may not write there.
        Remember(Token(L'd', std::wstring(folder.Path())), folder);
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
        const std::wstring path(file.Path());
        const std::wstring staging = StagingDir();
        if (staging.empty() || Win32CanWrite(path)) {
          Answer(id, {path});
          return;
        }
        // The engine may not write there: it gets a file in LocalState
        // instead, in a folder of its own so the name stays the one chosen,
        // and the finished download is copied into the picked file
        // (OnFileMessage).
        const std::wstring folder =
            staging + L"\\saved-" + std::to_wstring(::GetTickCount64());
        ::CreateDirectoryW(folder.c_str(), nullptr);
        const std::wstring staged = folder + L"\\" + std::wstring(file.Name());
        Remember(Token(L's', staged), file);
        Log::Write(L"picker: the engine may not write " + path +
                   L", it saves to " + staged + L" and the shell copies it");
        Answer(id, {staged});
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

// The launcher wants the UI thread, and the storage calls before it finish
// on a pool thread.
void LaunchFile(StorageFile const& file, std::wstring const& what) {
  gUi.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
               [file, what]() {
                 try {
                   winrt::Windows::System::Launcher::LaunchFileAsync(file);
                   Log::Write(L"open: asked the system to open " + what);
                 } catch (winrt::hresult_error const& error) {
                   Log::Write(L"open: the system refused " + what,
                              std::wstring(error.message()));
                 }
               });
}

void LaunchFolder(StorageFolder const& folder, std::wstring const& what) {
  gUi.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
               [folder, what]() {
                 try {
                   winrt::Windows::System::Launcher::LaunchFolderAsync(folder);
                   Log::Write(L"open: showing the folder " + what);
                 } catch (winrt::hresult_error const& error) {
                   Log::Write(L"open: the system would not show " + what,
                              std::wstring(error.message()));
                 }
               });
}

// A download copied out of LocalState: the copy is the one the user knows,
// in Downloads or wherever they chose, so that is the one opened or shown.
bool LaunchCopy(std::wstring const& target, int32_t reveal) {
  const std::wstring token = Token(L'x', target);
  if (!Remembered(token)) {
    return false;
  }
  StorageApplicationPermissions::FutureAccessList()
      .GetFileAsync(winrt::hstring(token))
      .Completed([target, reveal](IAsyncOperation<StorageFile> const& op,
                                  AsyncStatus status) {
        if (status != AsyncStatus::Completed) {
          Log::Write(L"open: the copy of " + target + L" is gone");
          return;
        }
        StorageFile copy = op.GetResults();
        const std::wstring where(copy.Path());
        if (!reveal) {
          LaunchFile(copy, where);
          return;
        }
        // Its folder, when the app may see it; the file itself otherwise.
        copy.GetParentAsync().Completed(
            [copy, where](IAsyncOperation<StorageFolder> const& parentOp,
                          AsyncStatus parentStatus) {
              StorageFolder parent = parentStatus == AsyncStatus::Completed
                                         ? parentOp.GetResults()
                                         : nullptr;
              if (parent) {
                LaunchFolder(parent, std::wstring(parent.Path()));
              } else {
                LaunchFile(copy, where);
              }
            });
      });
  return true;
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
          if (LaunchCopy(target, reveal)) {
            return;
          }
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
                  LaunchFolder(op.GetResults(), folder);
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
                LaunchFile(op.GetResults(), target);
              });
        } catch (winrt::hresult_error const& error) {
          Log::Write(L"open: could not open " + target,
                     std::wstring(error.message()));
        }
      });
}

// A finished download, staged in LocalState, copied to where it belongs.
// On a worker thread in the multithreaded apartment: it waits.
JsonObject Export(std::wstring const& path, std::wstring const& dir) {
  JsonObject reply;
  reply.SetNamedValue(L"path", JsonValue::CreateStringValue(path));
  if (!UnderStaging(path)) {
    reply.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(false));
    reply.SetNamedValue(L"error",
                        JsonValue::CreateStringValue(L"not a staged download"));
    return reply;
  }
  std::wstring step = L"opening the staged file";
  try {
    StorageFile staged =
        StorageFile::GetFileFromPathAsync(winrt::hstring(path)).get();
    StorageFile copy{nullptr};
    auto list = StorageApplicationPermissions::FutureAccessList();
    const std::wstring saveAs = Token(L's', path);
    const std::wstring folderToken = dir.empty() ? L"" : Token(L'd', dir);
    if (Remembered(saveAs)) {
      // The file picked in a Save As.
      step = L"opening the file picked to save into";
      copy = list.GetFileAsync(winrt::hstring(saveAs)).get();
      step = L"copying into the file picked";
      staged.CopyAndReplaceAsync(copy).get();
      Forget(saveAs);
    } else if (!folderToken.empty() && Remembered(folderToken)) {
      // The folder picked in the settings.
      step = L"opening the folder chosen in the settings";
      StorageFolder folder =
          list.GetFolderAsync(winrt::hstring(folderToken)).get();
      step = L"copying into the folder chosen";
      copy = staged
                 .CopyAsync(folder, staged.Name(),
                            NameCollisionOption::GenerateUniqueName)
                 .get();
    } else {
      // The phone's Downloads; the system puts an app's files in a folder
      // named after it, Downloads\Gecko.
      if (!dir.empty()) {
        Log::Write(L"files: no access kept for " + dir +
                   L" (chosen before this version?), using Downloads");
      }
      step = L"creating the file in Downloads";
      copy = DownloadsFolder::CreateFileAsync(
                 staged.Name(), CreationCollisionOption::GenerateUniqueName)
                 .get();
      step = L"copying into Downloads";
      staged.CopyAndReplaceAsync(copy).get();
    }
    Remember(Token(L'x', path), copy);
    const std::wstring where(copy.Path());
    Log::Write(L"files: " + path + L" copied to " +
               (where.empty() ? std::wstring(copy.Name()) : where));
    reply.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(true));
    reply.SetNamedValue(
        L"where",
        JsonValue::CreateStringValue(where.empty() ? copy.Name()
                                                   : winrt::hstring(where)));
  } catch (winrt::hresult_error const& error) {
    wchar_t text[512];
    swprintf_s(text, L"%s: %s (0x%08x)", step.c_str(), error.message().c_str(),
               static_cast<unsigned>(error.code()));
    Log::Write(L"files: could not copy " + path + L" out", text);
    reply.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(false));
    reply.SetNamedValue(L"error", JsonValue::CreateStringValue(text));
  }
  return reply;
}

unsigned long long AgeSeconds(FILETIME const& written) {
  FILETIME nowFt;
  ::GetSystemTimeAsFileTime(&nowFt);
  ULARGE_INTEGER now, then;
  now.LowPart = nowFt.dwLowDateTime;
  now.HighPart = nowFt.dwHighDateTime;
  then.LowPart = written.dwLowDateTime;
  then.HighPart = written.dwHighDateTime;
  return now.QuadPart > then.QuadPart ? (now.QuadPart - then.QuadPart) / 10000000ull
                                      : 0;
}

// Staged downloads that were copied out a while ago, the folders Save As
// left behind, and old copies of picked files. LocalState and TempState are
// the app's own, so plain Win32 does.
void CleanUp(std::wstring const& dir, bool staged, unsigned long long keep,
             bool nested = false) {
  WIN32_FIND_DATAW found;
  HANDLE find = ::FindFirstFileW((dir + L"\\*").c_str(), &found);
  if (find == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    const std::wstring name(found.cFileName);
    if (name == L"." || name == L"..") {
      continue;
    }
    const std::wstring path = dir + L"\\" + name;
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (staged ? name.rfind(L"saved-", 0) == 0
                 : name.rfind(L"picked", 0) == 0) {
        CleanUp(path, staged, keep, true);
        ::RemoveDirectoryW(path.c_str());  // only once it is empty
      }
      continue;
    }
    if (!staged && !nested) {
      continue;  // TempState's own files are not ours to clear
    }
    if (AgeSeconds(found.ftLastWriteTime) < keep) {
      continue;
    }
    if (staged && !Remembered(Token(L'x', path))) {
      continue;  // never copied out: the only one there is
    }
    if (::DeleteFileW(path.c_str())) {
      Log::Write(L"files: cleared " + path);
    }
  } while (::FindNextFileW(find, &found));
  ::FindClose(find);
}

}  // namespace

void OnFileMessage(double id, std::wstring const& op, JsonObject const& msg,
                   void (*send)(JsonObject const&)) {
  JsonObject reply;
  if (op == L"file.export") {
    reply = Export(std::wstring(msg.GetNamedString(L"path", L"")),
                   std::wstring(msg.GetNamedString(L"dir", L"")));
  } else {
    reply.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(false));
    reply.SetNamedValue(L"error",
                        JsonValue::CreateStringValue(L"unknown op " + op));
  }
  reply.SetNamedValue(L"id", JsonValue::CreateNumberValue(id));
  reply.SetNamedValue(L"op", JsonValue::CreateStringValue(op));
  send(reply);
}

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
  std::thread([]() {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    try {
      const std::wstring staging = StagingDir();
      if (!staging.empty()) {
        CleanUp(staging, true, kKeepStagedFor);
      }
      CleanUp(std::wstring(ApplicationData::Current().TemporaryFolder().Path()),
              false, kKeepPickedFor);
    } catch (...) {
    }
  }).detach();
}

}  // namespace gecko_w10m::client
