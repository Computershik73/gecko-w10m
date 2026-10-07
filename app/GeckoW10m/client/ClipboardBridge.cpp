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

namespace gecko_w10m::client {

namespace {

// The engine's side (widget/headless/HeadlessClipboard.cpp). Copies are
// numbered there; a reading carries the number of the last copy put on the
// phone when the read began, so the engine can drop one taken before the
// browser's latest copy got there.
using CopySink = void (*)(uint32_t copy, const wchar_t* text,
                          const wchar_t* html);
using SetCopySinkFn = void (*)(CopySink);
using PhoneTextFn = void (*)(uint32_t afterCopy, const wchar_t* text);

PhoneTextFn gPhoneText = nullptr;
bool gInstalled = false;

// Everything that touches the phone's clipboard runs on this one thread,
// never on the UI thread. On the phone the first call into the clipboard --
// wiring ContentChanged, or reading it -- did not come back, and on the UI
// thread that was the whole app: no splash going down, and ANGLE waiting on
// that thread for the panel's swap chain, so builds 153 and 155 never
// started (gecko (31).log, gecko (32).log). Here a call that hangs holds
// only this thread; the browser keeps its own clipboard and goes on.
class Worker {
 public:
  void Post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mLock);
      mTasks.push_back(std::move(task));
      if (!mStarted) {
        mStarted = true;
        std::thread([this]() { Run(); }).detach();
      }
    }
    mWake.notify_one();
  }

 private:
  void Run() {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mLock);
        mWake.wait(lock, [this]() { return !mTasks.empty(); });
        task = std::move(mTasks.front());
        mTasks.pop_front();
      }
      try {
        task();
      } catch (winrt::hresult_error const& error) {
        Log::Write(L"clipboard: a call failed", std::wstring(error.message()));
      } catch (...) {
        Log::Write(L"clipboard: a call failed");
      }
    }
  }

  std::mutex mLock;
  std::condition_variable mWake;
  std::deque<std::function<void()>> mTasks;
  bool mStarted = false;
};
Worker& TheWorker() {
  static Worker* worker = new Worker();  // never destroyed: its thread lives on
  return *worker;
}

// The newest copy the engine handed over; written on the engine's thread.
std::atomic<uint32_t> gLatest{0};
// The worker's own from here on. The newest copy actually on the phone.
uint32_t gApplied = 0;
// A copy the phone refused, tried again before the next read -- unless the
// phone's clipboard may have been given something newer since.
DataPackage gRefused{nullptr};
uint32_t gRefusedCopy = 0;
// Whether the window is out of sight, when another app may copy, and
// whether it has been activated at all: only the app in front may read.
bool gHidden = false;
bool gActivated = false;
bool gWired = false;

// A file in LocalState that stands while the first call into the phone's
// clipboard has not come back. Found at the next start, it means that call
// never did -- the app had to be closed -- and the phone's clipboard is left
// alone for that launch.
std::wstring Marker() {
  try {
    return std::wstring(winrt::Windows::Storage::ApplicationData::Current()
                            .LocalFolder()
                            .Path()) +
           L"\\clipboard-asking.txt";
  } catch (...) {
    return L"";
  }
}
bool gContacted = false;

// Runs the first call into the phone's clipboard with the marker standing,
// and says in the log that it went in and that it came back.
template <typename F>
void Contact(const wchar_t* what, F&& call) {
  if (gContacted) {
    call();
    return;
  }
  gContacted = true;
  const std::wstring marker = Marker();
  if (!marker.empty()) {
    HANDLE h = ::CreateFile2(marker.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS,
                             nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      ::CloseHandle(h);
    }
  }
  Log::Write(std::wstring(L"clipboard: first call into the phone's clipboard: ") +
             what);
  struct Clear {
    std::wstring path;
    ~Clear() {
      if (!path.empty()) {
        ::DeleteFileW(path.c_str());
      }
    }
  } clear{marker};
  call();
  Log::Write(L"clipboard: the phone's clipboard answered");
}

// The refused copy is not put back: what is on the phone now may be newer,
// and wins. Reads go ahead again, numbered as after that copy, so the engine
// takes what the phone has.
void Supersede(const wchar_t* why) {
  if (!gRefused) {
    return;
  }
  gRefused = nullptr;
  if (gApplied < gRefusedCopy) {
    gApplied = gRefusedCopy;
  }
  Log::Write(std::wstring(L"clipboard: the refused copy is let go (") + why +
             L")");
}

// Puts a package on the phone's clipboard.
bool Put(DataPackage const& package, uint32_t copy) {
  try {
    Contact(L"putting a copy on it", [&]() { Clipboard::SetContent(package); });
  } catch (winrt::hresult_error const& error) {
    gRefused = package;
    gRefusedCopy = copy;
    Log::Write(L"clipboard: the phone's refused copy " + std::to_wstring(copy),
               std::wstring(error.message()));
    if (gHidden) {
      // Out of sight already: another app may copy before we are back.
      Supersede(L"refused while out of sight");
    }
    return false;
  }
  gApplied = copy;
  gRefused = nullptr;
  // Kept on the phone's clipboard after the browser closes. A failure here
  // is not a lost copy: the content is there while the app runs.
  try {
    Clipboard::Flush();
  } catch (winrt::hresult_error const& error) {
    Log::Write(L"clipboard: could not make the copy outlive the app",
               std::wstring(error.message()));
  }
  return true;
}

// Reads the phone's clipboard and leaves the text with the engine.
void Read(const wchar_t* why) {
  if (!gPhoneText || !gActivated || gHidden) {
    return;  // only the app in front may read it
  }
  if (gRefused && !Put(gRefused, gRefusedCopy)) {
    return;  // the phone holds something older than the browser's copy
  }
  const uint32_t copy = gApplied;
  if (copy != gLatest.load()) {
    return;  // a copy from the browser is still on its way; it comes first
  }
  try {
    DataPackageView view{nullptr};
    Contact(L"reading it", [&]() { view = Clipboard::GetContent(); });
    if (!view.Contains(StandardDataFormats::Text())) {
      gPhoneText(copy, nullptr);
      return;
    }
    view.GetTextAsync().Completed(
        [copy](IAsyncOperation<winrt::hstring> const& op, AsyncStatus status) {
          try {
            if (status != AsyncStatus::Completed) {
              Log::Write(L"clipboard: the phone's text could not be read");
              return;
            }
            const winrt::hstring text = op.GetResults();
            gPhoneText(copy, text.c_str());
          } catch (winrt::hresult_error const& error) {
            Log::Write(L"clipboard: the phone's text could not be read",
                       std::wstring(error.message()));
          }
        });
  } catch (winrt::hresult_error const& error) {
    Log::Write(std::wstring(L"clipboard: the phone's could not be read (") +
                   why + L")",
               std::wstring(error.message()));
  }
}

// From the engine's main thread: copies the strings and returns at once.
// Nothing may be thrown from here -- the caller is engine code built
// without exceptions -- so anything that does is caught and logged.
void OnCopy(uint32_t copy, const wchar_t* text, const wchar_t* html) {
  gLatest.store(copy);
  const bool hasText = text != nullptr;
  const bool hasHtml = html != nullptr;
  try {
    std::wstring plain = hasText ? text : L"";
    std::wstring markup = hasHtml ? html : L"";
    TheWorker().Post([=]() {
      if (copy != gLatest.load()) {
        return;  // a newer copy follows
      }
      try {
        DataPackage package;
        package.RequestedOperation(DataPackageOperation::Copy);
        if (hasText) {
          package.SetText(winrt::hstring(plain));
        }
        if (hasHtml) {
          package.SetHtmlFormat(
              HtmlFormatHelper::CreateHtmlFormat(winrt::hstring(markup)));
        }
        if (Put(package, copy)) {
          Log::Write(L"clipboard: copy " + std::to_wstring(copy) +
                     L" is on the phone's, " + std::to_wstring(plain.size()) +
                     L" characters" + (hasHtml ? L" with HTML" : L""));
        }
      } catch (winrt::hresult_error const& error) {
        // Never on the phone, so nothing to retry; reads go ahead again
        // rather than wait for a copy that will not come.
        if (gApplied < copy) {
          gApplied = copy;
        }
        Log::Write(L"clipboard: the copy could not be packaged",
                   std::wstring(error.message()));
      }
    });
  } catch (...) {
    Log::Write(L"clipboard: the copy could not be handed over");
  }
}

}  // namespace

void InstallClipboardBridge(HMODULE xul,
                            winrt::Windows::UI::Core::CoreDispatcher const&) {
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
  const std::wstring marker = Marker();
  if (!marker.empty() &&
      ::GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
    // The engine keeps its own clipboard, as before the bridge.
    ::DeleteFileW(marker.c_str());
    gPhoneText = nullptr;
    Log::Write(L"clipboard: the last launch never heard back from the "
               L"phone's clipboard -- copy and paste stay inside the "
               L"browser this time");
    return;
  }
  setSink(&OnCopy);
  // Nothing is asked of the phone's clipboard here; the first activation of
  // the window does it, on the worker.
  Log::Write(L"view: copy and paste go through the phone's clipboard");
}

void ClipboardWindowActivated() {
  if (!gInstalled || !gPhoneText) {
    return;
  }
  TheWorker().Post([]() {
    gActivated = true;
    if (!gWired) {
      gWired = true;
      try {
        // A refused copy never raises this, so a change seen while one is
        // held came from another app, and is newer.
        Contact(L"asking to hear of changes", []() {
          Clipboard::ContentChanged([](auto&&, auto&&) {
            TheWorker().Post([]() {
              Supersede(L"the phone's changed");
              Read(L"it changed");
            });
          });
        });
      } catch (winrt::hresult_error const& error) {
        Log::Write(L"clipboard: the phone will not say when it changes",
                   std::wstring(error.message()));
      }
    }
    Read(L"activated");
  });
}

void ClipboardWindowVisible(bool visible) {
  if (!gInstalled || !gPhoneText) {
    return;
  }
  TheWorker().Post([visible]() {
    gHidden = !visible;
    if (!visible) {
      Supersede(L"the window went out of sight");
    }
  });
}

}  // namespace gecko_w10m::client
