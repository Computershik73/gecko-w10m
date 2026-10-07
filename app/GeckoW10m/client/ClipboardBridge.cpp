// ClipboardBridge.cpp -- see ClipboardBridge.h.
#include "pch.h"

#include "client/ClipboardBridge.h"

#include <atomic>
#include <string>

#include <winrt/Windows.ApplicationModel.DataTransfer.h>

#include "client/Log.h"

using namespace winrt::Windows::ApplicationModel::DataTransfer;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::UI::Core::CoreDispatcherPriority;

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

winrt::Windows::UI::Core::CoreDispatcher gUi{nullptr};
PhoneTextFn gPhoneText = nullptr;
bool gInstalled = false;

// The newest copy the engine handed over; written on the engine's thread.
std::atomic<uint32_t> gLatest{0};
// UI thread only from here on. The newest copy actually on the phone.
uint32_t gApplied = 0;
// A copy the phone refused, tried again before the next read -- unless the
// phone's clipboard may have been given something newer since.
DataPackage gRefused{nullptr};
uint32_t gRefusedCopy = 0;
// Whether the window is out of sight, when another app may copy.
bool gHidden = false;

// The refused copy is not put back: what is on the phone now may be newer,
// and wins. Reads go ahead again, numbered as after that copy, so the engine
// takes what the phone has. UI thread.
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

// Puts a package on the phone's clipboard. UI thread.
bool Put(DataPackage const& package, uint32_t copy) {
  try {
    Clipboard::SetContent(package);
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

// Reads the phone's clipboard and leaves the text with the engine. UI thread:
// the phone lets an app read its clipboard only there, and only while it is
// in front, so this runs again whenever the app comes back.
void Read(const wchar_t* why) {
  if (!gPhoneText) {
    return;
  }
  if (gRefused && !Put(gRefused, gRefusedCopy)) {
    return;  // the phone holds something older than the browser's copy
  }
  const uint32_t copy = gApplied;
  if (copy != gLatest.load()) {
    return;  // a copy from the browser is still on its way; it comes first
  }
  try {
    auto view = Clipboard::GetContent();
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

void ReadOnUi(const wchar_t* why) {
  gUi.RunAsync(CoreDispatcherPriority::Normal, [why]() { Read(why); });
}

// From the engine's main thread: copies the strings and returns at once.
// Nothing may be thrown from here -- the caller is engine code built
// without exceptions -- so anything that does is caught and logged.
void OnCopy(uint32_t copy, const wchar_t* text, const wchar_t* html) {
  if (!gUi) {
    return;
  }
  gLatest.store(copy);
  const bool hasText = text != nullptr;
  const bool hasHtml = html != nullptr;
  try {
    std::wstring plain = hasText ? text : L"";
    std::wstring markup = hasHtml ? html : L"";
    gUi.RunAsync(CoreDispatcherPriority::Normal, [=]() {
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
    Log::Write(L"clipboard: the copy could not be handed to the UI thread");
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
  gUi = ui;
  setSink(&OnCopy);
  gInstalled = true;
  Log::Write(L"view: copy and paste go through the phone's clipboard");
  gUi.RunAsync(CoreDispatcherPriority::Normal, []() {
    try {
      // A refused copy never raises this, so a change seen while one is
      // held came from another app, and is newer.
      Clipboard::ContentChanged([](auto&&, auto&&) {
        gUi.RunAsync(CoreDispatcherPriority::Normal, []() {
          Supersede(L"the phone's changed");
          Read(L"it changed");
        });
      });
    } catch (winrt::hresult_error const& error) {
      Log::Write(L"clipboard: the phone will not say when it changes",
                 std::wstring(error.message()));
    }
    Read(L"start");
  });
}

void RefreshClipboard(const wchar_t* why) {
  if (gInstalled && gUi) {
    ReadOnUi(why);
  }
}

void ClipboardWindowVisible(bool visible) {
  gHidden = !visible;
  if (!visible) {
    Supersede(L"the window went out of sight");
  }
}

}  // namespace gecko_w10m::client
