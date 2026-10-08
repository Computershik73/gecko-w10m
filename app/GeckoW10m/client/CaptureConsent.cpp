// CaptureConsent.cpp -- see CaptureConsent.h.
#include "pch.h"

#include "client/CaptureConsent.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Media.Capture.h>

#include "client/Log.h"

using winrt::Windows::Devices::Enumeration::DeviceAccessInformation;
using winrt::Windows::Devices::Enumeration::DeviceAccessStatus;
using winrt::Windows::Devices::Enumeration::DeviceClass;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncAction;
using winrt::Windows::Media::Capture::MediaCapture;
using winrt::Windows::Media::Capture::MediaCaptureInitializationSettings;
using winrt::Windows::Media::Capture::StreamingCaptureMode;

namespace gecko_w10m::client {

namespace {

// The engine's side (dom/system/nsOSPermissionRequestBase.cpp).
using ConsentState = int32_t (*)(int32_t kind);
using ConsentRequest = void (*)(uint32_t id, int32_t kind);
using SetConsentSinkFn = void (*)(ConsentState, ConsentRequest);
using ConsentResultFn = void (*)(uint32_t id, int32_t granted);

// nsIOSPermissionRequest::PERMISSION_STATE_*
constexpr int32_t kNotDetermined = 0;
constexpr int32_t kRestricted = 1;
constexpr int32_t kAuthorized = 3;

constexpr int32_t kCamera = 1;

winrt::Windows::UI::Core::CoreDispatcher gUi{nullptr};
ConsentResultFn gResult = nullptr;
bool gInstalled = false;

// One consent request: the MediaCapture whose start shows the system's
// prompt, and that start -- and through it, its completion handler. Left to
// Windows.Media they were let go while its prompt was up on the HP Elite x3,
// and the freed handler was called all the same: the browser fell over the
// moment the user answered. So they are kept here for as long as the browser
// runs: there is no telling when Windows.Media is done with them, and there
// is one of these per system prompt, which stops coming once it is answered.
struct Ask {
  uint32_t id = 0;
  int32_t kind = 0;
  MediaCapture capture{nullptr};
  IAsyncAction start{nullptr};
  std::atomic<bool> answered{false};
};
std::mutex gAsksLock;
std::vector<std::shared_ptr<Ask>> gAsks;

DeviceClass ClassFor(int32_t kind) {
  return kind == kCamera ? DeviceClass::VideoCapture : DeviceClass::AudioCapture;
}

const wchar_t* NameFor(int32_t kind) {
  return kind == kCamera ? L"camera" : L"microphone";
}

// Never throws, and tells the engine before anything else: the log line
// goes through the UI dispatcher, which is the very thing that may have
// refused work when this is the fallback.
void Answer(uint32_t id, int32_t kind, bool granted,
            std::wstring const& why) noexcept {
  if (gResult) {
    gResult(id, granted ? 1 : 0);
  }
  try {
    Log::Write(std::wstring(L"capture: ") + NameFor(kind) +
               (granted ? L" allowed" : L" refused") +
               (why.empty() ? L"" : L", " + why));
  } catch (...) {
  }
}

// On the UI thread, where the capture was made: closed first, so the engine
// asks for the device only once it is free again, then the answer.
void Finish(std::shared_ptr<Ask> const& ask, bool granted,
            std::wstring const& why) noexcept {
  try {
    ask->capture.Close();
  } catch (...) {
  }
  Answer(ask->id, ask->kind, granted, why);
}

// From the engine's main thread; must answer at once and never ask.
// Refused by the user counts as not yet decided, so the request below runs
// and can take them to the setting.
int32_t OnState(int32_t kind) {
  try {
    switch (DeviceAccessInformation::CreateFromDeviceClass(ClassFor(kind))
                .CurrentStatus()) {
      case DeviceAccessStatus::Allowed:
        return kAuthorized;
      case DeviceAccessStatus::DeniedBySystem:
        return kRestricted;
      default:
        return kNotDetermined;
    }
  } catch (...) {
    return kNotDetermined;
  }
}

// From the engine's main thread. The system shows its consent prompt the
// first time a MediaCapture starts, on the UI thread; it is closed again at
// once, so the engine's own capture gets the device.
void OnRequest(uint32_t id, int32_t kind) {
  if (!gUi) {
    Answer(id, kind, true, L"no UI thread to ask on, left to the device");
    return;
  }
  // From engine code built without exceptions: nothing may be thrown back.
  try {
    gUi.RunAsync(
        winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [id, kind]() {
          try {
            const auto status =
                DeviceAccessInformation::CreateFromDeviceClass(ClassFor(kind))
                    .CurrentStatus();
            if (status == DeviceAccessStatus::DeniedByUser) {
              // Turned off for this app in Settings: the only place it can be
              // turned back on, so that is where the user is taken.
              winrt::Windows::System::Launcher::LaunchUriAsync(
                  winrt::Windows::Foundation::Uri(
                      kind == kCamera ? L"ms-settings:privacy-webcam"
                                      : L"ms-settings:privacy-microphone"));
              Answer(id, kind, false, L"off in Settings, opened them");
              return;
            }
            MediaCaptureInitializationSettings settings;
            settings.StreamingCaptureMode(kind == kCamera
                                              ? StreamingCaptureMode::Video
                                              : StreamingCaptureMode::Audio);
            auto ask = std::make_shared<Ask>();
            ask->id = id;
            ask->kind = kind;
            ask->capture = MediaCapture();
            {
              std::lock_guard<std::mutex> lock(gAsksLock);
              gAsks.push_back(ask);
            }
            Log::Write(std::wstring(L"capture: asking the phone for the ") +
                       NameFor(kind));
            ask->start = ask->capture.InitializeAsync(settings);
            ask->start.Completed([ask](IAsyncAction const& op,
                                       AsyncStatus done) {
              // A copy of our own: nothing of the handler's is touched after
              // the work is handed over below.
              const std::shared_ptr<Ask> mine = ask;
              // Once: whatever comes after the first answer is not one.
              if (mine->answered.exchange(true)) {
                return;
              }
              HRESULT code = S_OK;
              if (done != AsyncStatus::Completed) {
                try {
                  code = op.ErrorCode();
                } catch (...) {
                  code = E_FAIL;
                }
              }
              // Only a refusal is final. Anything else -- the capture
              // pipeline itself unhappy -- is left to the engine's own
              // WASAPI capture to try.
              const bool granted =
                  done == AsyncStatus::Completed || code != E_ACCESSDENIED;
              std::wstring why;
              if (done != AsyncStatus::Completed) {
                wchar_t text[64];
                swprintf_s(text, L"MediaCapture said 0x%08x",
                           static_cast<unsigned>(code));
                why = text;
              }
              // Not from inside its own completion, on a media work-queue
              // thread: closed and answered on the UI thread.
              try {
                gUi.RunAsync(
                    winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                    [mine, granted, why]() { Finish(mine, granted, why); });
              } catch (...) {
                // The answer at least; the capture stays open, once.
                Answer(mine->id, mine->kind, granted,
                       why.empty() ? std::wstring(L"not closed") : why);
              }
            });
          } catch (winrt::hresult_error const& error) {
            Answer(id, kind, error.code() != E_ACCESSDENIED,
                   std::wstring(error.message()));
          }
        });
  } catch (...) {
    Answer(id, kind, true, L"could not reach the UI thread, left to the device");
  }
}

}  // namespace

void InstallCaptureConsent(HMODULE xul,
                           winrt::Windows::UI::Core::CoreDispatcher const& ui) {
  if (gInstalled || !xul) {
    return;
  }
  auto setSink = reinterpret_cast<SetConsentSinkFn>(
      ::GetProcAddress(xul, "gecko_w10m_set_capture_consent_sink"));
  gResult = reinterpret_cast<ConsentResultFn>(
      ::GetProcAddress(xul, "gecko_w10m_capture_consent_result"));
  if (!setSink || !gResult) {
    return;
  }
  gUi = ui;
  setSink(&OnState, &OnRequest);
  gInstalled = true;
  Log::Write(std::wstring(L"view: microphone consent goes through the shell (") +
             (OnState(0) == kAuthorized ? L"already given" : L"not given yet") +
             L")");
}

}  // namespace gecko_w10m::client
