// ClipboardBridge.cpp -- see ClipboardBridge.h.
#include "pch.h"

#include "client/ClipboardBridge.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <winrt/Windows.ApplicationModel.DataTransfer.h>

#include "client/Log.h"

using namespace winrt::Windows::ApplicationModel::DataTransfer;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::UI::Core::CoreDispatcherPriority;

namespace gecko_w10m::client {

namespace {

// What happened on the phone, build by build:
//  - 153 and 155: on the UI thread, Clipboard::ContentChanged never came back.
//    Registering for it creates a COM object (the clipboard's notification
//    client) registered with no threading model, which COM puts in the
//    process's main single-threaded apartment -- Gecko's own thread, which is
//    such an apartment and never pumps messages. The UI thread waited on it
//    for good, and the app never started.
//  - 156: from a multithreaded worker, every call failed at once: the
//    Clipboard class is registered single-threaded only (0x8000001D).
// GetContent and SetContent go straight to the system's clipboard broker,
// which is what XAML's own paste does -- and that works on the phone.
//
// So: ContentChanged is never used. Every other call is made on a thread of
// our own that is a classic single-threaded apartment and pumps while it
// waits; if the phone refuses calls from there, the UI thread itself is used,
// where Microsoft's documentation puts clipboard access. Before each call the
// path is written down as in flight; a launch that never came back from one
// leaves it so, and that path is not used again in this build.

// The engine's side (widget/headless/HeadlessClipboard.cpp). Copies are
// numbered there; a reading carries the number of the last copy put on the
// phone when the read began, so the engine can drop one taken before the
// browser's latest copy got there.
using CopySink = void (*)(uint32_t copy, const wchar_t* text,
                          const wchar_t* html);
using SetCopySinkFn = void (*)(CopySink);
using PhoneTextFn = void (*)(uint32_t afterCopy, const wchar_t* text);

winrt::Windows::UI::Core::CoreDispatcher gUi{nullptr};
PhoneTextFn gPhoneText = nullptr;
bool gInstalled = false;

// ---------------------------------------------------------------- the ledger
// LocalState\clipboard-ledger.txt: "<build>|<sta state>|<ui state>", each
// state "ok", "inflight" or "hung". A different build starts clean.
enum class Path { Sta, Ui };
std::mutex gLedgerLock;
std::wstring gBuild;
std::wstring gStaState = L"ok";
std::wstring gUiState = L"ok";

std::wstring LedgerPath() {
  try {
    return std::wstring(winrt::Windows::Storage::ApplicationData::Current()
                            .LocalFolder()
                            .Path()) +
           L"\\clipboard-ledger.txt";
  } catch (...) {
    return L"";
  }
}

void WriteLedgerLocked() {
  const std::wstring path = LedgerPath();
  if (path.empty()) {
    return;
  }
  const std::wstring text = gBuild + L"|" + gStaState + L"|" + gUiState;
  HANDLE h = ::CreateFile2(path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS,
                           nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD written = 0;
  ::WriteFile(h, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)),
              &written, nullptr);
  ::FlushFileBuffers(h);
  ::CloseHandle(h);
}

void ReadLedger() {
  std::lock_guard<std::mutex> lock(gLedgerLock);
  try {
    auto v = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
    gBuild = std::to_wstring(v.Major) + L"." + std::to_wstring(v.Minor) + L"." +
             std::to_wstring(v.Build) + L"." + std::to_wstring(v.Revision);
  } catch (...) {
    gBuild = L"?";
  }
  const std::wstring path = LedgerPath();
  std::wstring text;
  if (!path.empty()) {
    HANDLE h = ::CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             OPEN_EXISTING, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      wchar_t buffer[128] = {};
      DWORD read = 0;
      ::ReadFile(h, buffer, sizeof(buffer) - sizeof(wchar_t), &read, nullptr);
      ::CloseHandle(h);
      text.assign(buffer, read / sizeof(wchar_t));
    }
  }
  const size_t a = text.find(L'|');
  const size_t b = a == std::wstring::npos ? a : text.find(L'|', a + 1);
  if (b != std::wstring::npos && text.substr(0, a) == gBuild) {
    gStaState = text.substr(a + 1, b - a - 1);
    gUiState = text.substr(b + 1);
  }
  // A call left in flight never came back: that launch was closed or ended
  // waiting on it.
  for (std::wstring* state : {&gStaState, &gUiState}) {
    if (*state == L"inflight") {
      *state = L"hung";
    }
  }
  WriteLedgerLocked();
  Log::Write(L"clipboard: build " + gBuild + L", our own thread " + gStaState +
             L", the UI thread " + gUiState);
}

bool Usable(Path path) {
  std::lock_guard<std::mutex> lock(gLedgerLock);
  return (path == Path::Sta ? gStaState : gUiState) != L"hung";
}

void Mark(Path path, const wchar_t* state) {
  std::lock_guard<std::mutex> lock(gLedgerLock);
  std::wstring& slot = path == Path::Sta ? gStaState : gUiState;
  if (slot == L"hung" || slot == state) {
    return;
  }
  slot = state;
  WriteLedgerLocked();
}

// Runs one call into the phone's clipboard with the ledger saying so; the
// first on each path is logged going in and coming back.
template <typename F>
void Guarded(Path path, const wchar_t* what, F&& call) {
  static std::atomic<bool> loggedSta{false}, loggedUi{false};
  std::atomic<bool>& logged = path == Path::Sta ? loggedSta : loggedUi;
  const bool first = !logged.exchange(true);
  const wchar_t* where = path == Path::Sta ? L"our own thread" : L"the UI thread";
  if (first) {
    Log::Write(std::wstring(L"clipboard: first call on ") + where + L": " + what);
  }
  Mark(path, L"inflight");
  struct Done {
    Path path;
    ~Done() { Mark(path, L"ok"); }
  } done{path};
  call();
  if (first) {
    Log::Write(std::wstring(L"clipboard: ") + where + L" came back");
  }
}

// -------------------------------------------------------- the clipboard state
// Shared by whichever thread makes the calls; under gStateLock.
std::mutex gStateLock;
std::atomic<uint32_t> gLatest{0};   // newest copy the engine handed over
uint32_t gApplied = 0;              // newest copy actually on the phone
struct PendingCopy {
  uint32_t copy = 0;
  bool hasText = false, hasHtml = false;
  std::wstring text, html;
};
PendingCopy gPending;               // a copy not yet on the phone
bool gHidden = false;               // the window is out of sight
bool gActivated = false;            // the window is in front
bool gDrawing = false;              // the engine has drawn its first frame
// Whether calls from our own thread were refused this launch.
std::atomic<bool> gStaRefused{false};

// ------------------------------------------------- our own single-threaded apartment
using CoWaitFn = HRESULT(WINAPI*)(DWORD, DWORD, ULONG, LPHANDLE, LPDWORD);

class StaThread {
 public:
  void Post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mLock);
      mTasks.push_back(std::move(task));
      if (!mStarted) {
        mStarted = true;
        mWake = ::CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        std::thread([this]() { Run(); }).detach();
      }
    }
    if (mWake) {
      ::SetEvent(mWake);
    }
  }

 private:
  void Run() {
    try {
      winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (winrt::hresult_error const& error) {
      Log::Write(L"clipboard: our thread is not a single-threaded apartment",
                 std::wstring(error.message()));
    }
    APTTYPE type = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    ::CoGetApartmentType(&type, &qualifier);
    // CoWaitForMultipleHandles is declared for desktop apps only, and the
    // phone's combase has it: a single-threaded apartment that waits must
    // keep dispatching what other apartments send it.
    CoWaitFn coWait = nullptr;
    if (HMODULE combase = ::GetModuleHandleW(L"combase.dll")) {
      coWait = reinterpret_cast<CoWaitFn>(
          ::GetProcAddress(combase, "CoWaitForMultipleHandles"));
    }
    Log::Write(L"clipboard: our thread is apartment type " +
               std::to_wstring(static_cast<int>(type)) + L" (qualifier " +
               std::to_wstring(static_cast<int>(qualifier)) + L"), " +
               (coWait ? L"pumping while it waits"
                       : L"NOT pumping while it waits"));
    for (;;) {
      bool waited = false;
      if (coWait) {
        DWORD index = 0;
        // COWAIT_DISPATCH_WINDOW_MESSAGES: in a single-threaded apartment
        // this is COM's modal loop, which dispatches what other apartments
        // send while it waits.
        const HRESULT hr = coWait(0x10, INFINITE, 1, &mWake, &index);
        if (SUCCEEDED(hr)) {
          waited = true;
        } else {
          // Refused outright: going round again would spin.
          wchar_t code[16];
          swprintf_s(code, L"0x%08x", static_cast<unsigned>(hr));
          Log::Write(std::wstring(L"clipboard: our thread cannot pump (") +
                     code + L"), it waits plainly");
          coWait = nullptr;
        }
      }
      if (!waited) {
        ::WaitForSingleObjectEx(mWake, INFINITE, TRUE);
      }
      for (;;) {
        std::function<void()> task;
        {
          std::lock_guard<std::mutex> lock(mLock);
          if (mTasks.empty()) {
            break;
          }
          task = std::move(mTasks.front());
          mTasks.pop_front();
        }
        try {
          task();
        } catch (...) {
          Log::Write(L"clipboard: a task on our thread failed");
        }
      }
    }
  }

  std::mutex mLock;
  std::deque<std::function<void()>> mTasks;
  HANDLE mWake = nullptr;
  bool mStarted = false;
};
StaThread& Sta() {
  static StaThread* thread = new StaThread();  // never destroyed: it lives on
  return *thread;
}

// ------------------------------------------------------------- the calls
// The path for the next call, or false when none is left.
bool ChoosePath(Path* path) {
  if (!gStaRefused.load() && Usable(Path::Sta)) {
    *path = Path::Sta;
    return true;
  }
  if (gUi && Usable(Path::Ui)) {
    *path = Path::Ui;
    return true;
  }
  return false;
}

void RunOn(Path path, std::function<void()> work) {
  if (path == Path::Sta) {
    Sta().Post(std::move(work));
    return;
  }
  try {
    gUi.RunAsync(CoreDispatcherPriority::Low, [work]() { work(); });
  } catch (...) {
  }
}

// The phone refused a call made from our own thread: from here on this
// launch, calls are made on the UI thread.
void NoteRefused(Path path, winrt::hresult_error const& error,
                 const wchar_t* what) {
  wchar_t code[16];
  swprintf_s(code, L"0x%08x", static_cast<unsigned>(error.code()));
  Log::Write(std::wstring(L"clipboard: ") +
                 (path == Path::Sta ? L"our own thread" : L"the UI thread") +
                 L" was refused " + what + L" (" + code + L")",
             std::wstring(error.message()));
  if (path == Path::Sta) {
    gStaRefused.store(true);
  }
}

void Read(const wchar_t* why);

// Puts the pending copy on the phone's clipboard.
void Write() {
  PendingCopy pending;
  {
    std::lock_guard<std::mutex> lock(gStateLock);
    if (!gActivated || gHidden || !gPending.copy) {
      return;  // only the app in front may set it; tried again on activation
    }
    pending = gPending;
  }
  Path path;
  if (!ChoosePath(&path)) {
    return;
  }
  RunOn(path, [path, pending]() {
    try {
      DataPackage package;
      package.RequestedOperation(DataPackageOperation::Copy);
      if (pending.hasText) {
        package.SetText(winrt::hstring(pending.text));
      }
      if (pending.hasHtml) {
        package.SetHtmlFormat(
            HtmlFormatHelper::CreateHtmlFormat(winrt::hstring(pending.html)));
      }
      Guarded(path, L"putting a copy on it",
              [&]() { Clipboard::SetContent(package); });
      try {
        Clipboard::Flush();  // still there after the browser closes
      } catch (winrt::hresult_error const&) {
      }
      {
        std::lock_guard<std::mutex> lock(gStateLock);
        if (gApplied < pending.copy) {
          gApplied = pending.copy;
        }
        if (gPending.copy == pending.copy) {
          gPending = PendingCopy{};
        }
      }
      Log::Write(L"clipboard: copy " + std::to_wstring(pending.copy) +
                 L" is on the phone's, " + std::to_wstring(pending.text.size()) +
                 L" characters" + (pending.hasHtml ? L" with HTML" : L""));
    } catch (winrt::hresult_error const& error) {
      NoteRefused(path, error, L"a copy");
      if (path == Path::Sta) {
        Write();  // once more, on the UI thread
      }
    }
  });
}

// Reads the phone's clipboard and leaves the text with the engine.
void Read(const wchar_t* why) {
  uint32_t copy = 0;
  {
    std::lock_guard<std::mutex> lock(gStateLock);
    if (!gDrawing || !gActivated || gHidden || !gPhoneText) {
      return;  // not before the first frame, and only while in front
    }
    if (gPending.copy) {
      return;  // a copy from the browser is still on its way; it comes first
    }
    copy = gApplied;
    if (copy != gLatest.load()) {
      return;
    }
  }
  Path path;
  if (!ChoosePath(&path)) {
    return;
  }
  const std::wstring reason = why;
  RunOn(path, [path, copy, reason]() {
    try {
      DataPackageView view{nullptr};
      Guarded(path, L"reading it", [&]() { view = Clipboard::GetContent(); });
      if (!view.Contains(StandardDataFormats::Text())) {
        gPhoneText(copy, nullptr);
        return;
      }
      // Never waited for here: the answer comes on whatever thread finishes
      // it, and the engine takes it from any thread.
      view.GetTextAsync().Completed(
          [copy](IAsyncOperation<winrt::hstring> const& op, AsyncStatus status) {
            try {
              if (status == AsyncStatus::Completed) {
                const winrt::hstring text = op.GetResults();
                gPhoneText(copy, text.c_str());
              }
            } catch (winrt::hresult_error const& error) {
              Log::Write(L"clipboard: the phone's text could not be read",
                         std::wstring(error.message()));
            }
          });
    } catch (winrt::hresult_error const& error) {
      NoteRefused(path, error, (L"a read (" + reason + L")").c_str());
      if (path == Path::Sta) {
        Read(L"again, on the UI thread");
      }
    }
  });
}

// From the engine's main thread: copies the strings and returns at once.
// Nothing may be thrown from here -- the caller is engine code built
// without exceptions -- so anything that does is caught and logged.
void OnCopy(uint32_t copy, const wchar_t* text, const wchar_t* html) {
  try {
    gLatest.store(copy);
    {
      std::lock_guard<std::mutex> lock(gStateLock);
      gPending.copy = copy;
      gPending.hasText = text != nullptr;
      gPending.hasHtml = html != nullptr;
      gPending.text = text ? text : L"";
      gPending.html = html ? html : L"";
    }
    Write();
  } catch (...) {
    Log::Write(L"clipboard: the copy could not be handed over");
  }
}

}  // namespace

void InstallClipboardBridge(HMODULE xul,
                            winrt::Windows::UI::Core::CoreDispatcher const& ui) {
  if (gInstalled || !xul) {
    return;
  }
  auto setSink = reinterpret_cast<SetCopySinkFn>(
      ::GetProcAddress(xul, "gecko_w10m_set_clipboard_sink"));
  gPhoneText = reinterpret_cast<PhoneTextFn>(
      ::GetProcAddress(xul, "gecko_w10m_clipboard_phone_text"));
  if (!setSink || !gPhoneText) {
    return;
  }
  gInstalled = true;
  gUi = ui;
  ::DeleteFileW((LedgerPath().substr(0, LedgerPath().rfind(L'\\')) +
                 L"\\clipboard-asking.txt")
                    .c_str());  // the marker of builds 155 and 156
  ReadLedger();
  if (!Usable(Path::Sta) && !Usable(Path::Ui)) {
    gPhoneText = nullptr;
    Log::Write(L"clipboard: both ways hung in this build -- copy and paste "
               L"stay inside the browser");
    return;
  }
  setSink(&OnCopy);
  Log::Write(L"view: copy and paste go through the phone's clipboard");
}

void ClipboardEngineDrawing() {
  if (!gInstalled || !gPhoneText) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(gStateLock);
    gDrawing = true;
  }
  Read(L"first frame");
}

void ClipboardWindowActivated(bool active) {
  if (!gInstalled || !gPhoneText) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(gStateLock);
    gActivated = active;
  }
  if (active) {
    Write();  // a copy the phone refused while we were not in front
    Read(L"activated");
  }
}

void ClipboardWindowVisible(bool visible) {
  if (!gInstalled || !gPhoneText) {
    return;
  }
  std::lock_guard<std::mutex> lock(gStateLock);
  gHidden = !visible;
  if (!visible && gPending.copy) {
    // Out of sight, another app may copy; ours is not put back over it.
    Log::Write(L"clipboard: copy " + std::to_wstring(gPending.copy) +
               L" never reached the phone's, let go");
    if (gApplied < gPending.copy) {
      gApplied = gPending.copy;
    }
    gPending = PendingCopy{};
  }
}

void ClipboardRefresh(const wchar_t* why) {
  if (!gInstalled || !gPhoneText) {
    return;
  }
  Read(why);
}

}  // namespace gecko_w10m::client
