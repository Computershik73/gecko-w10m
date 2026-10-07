// CaptureConsent.cpp -- see CaptureConsent.h.
#include "pch.h"

#include "client/CaptureConsent.h"

#include <string>

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

DeviceClass ClassFor(int32_t kind) {
  return kind == kCamera ? DeviceClass::VideoCapture : DeviceClass::AudioCapture;
}

const wchar_t* NameFor(int32_t kind) {
  return kind == kCamera ? L"camera" : L"microphone";
}

void Answer(uint32_t id, int32_t kind, bool granted, std::wstring const& why) {
  Log::Write(std::wstring(L"capture: ") + NameFor(kind) +
             (granted ? L" allowed" : L" refused") +
             (why.empty() ? L"" : L", " + why));
  if (gResult) {
    gResult(id, granted ? 1 : 0);
  }
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
          MediaCapture capture;
          capture.InitializeAsync(settings).Completed(
              [id, kind, capture](IAsyncAction const& op, AsyncStatus done) {
                try {
                  capture.Close();
                } catch (...) {
                }
                if (done == AsyncStatus::Completed) {
                  Answer(id, kind, true, L"");
                  return;
                }
                const HRESULT code = op.ErrorCode();
                wchar_t text[64];
                swprintf_s(text, L"MediaCapture said 0x%08x",
                           static_cast<unsigned>(code));
                // Only a refusal is final. Anything else -- the capture
                // pipeline itself unhappy -- is left to the engine's own
                // WASAPI capture to try.
                Answer(id, kind, code != E_ACCESSDENIED, text);
              });
        } catch (winrt::hresult_error const& error) {
          Answer(id, kind, error.code() != E_ACCESSDENIED,
                 std::wstring(error.message()));
        }
      });
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
