// App.cpp — UWP application entry point (C++/WinRT, no XAML markup compiler).
#include "pch.h"

#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.Storage.h>

#include "MainPage.h"
#include "client/Log.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Xaml;

namespace gecko_w10m {

// A code-only C++/WinRT XAML app must still implement IXamlMetadataProvider; the
// framework QIs the Application for it during initialization. We author no
// custom XAML types, so the implementations return empties.
struct App : ApplicationT<App, winrt::Windows::UI::Xaml::Markup::IXamlMetadataProvider> {
  std::shared_ptr<MainPage> page_;

  App() {
    // A XAML app does not die of a Win32 exception; it dies when an error
    // reaches the framework with nobody to answer for it. Those two events are
    // the only place that is visible, and neither leaves a trace in the Win32
    // handlers -- which is why an entire crash could look like a fault in
    // CoreUIComponents that the process then happily survived.
    UnhandledException([](auto const&, UnhandledExceptionEventArgs const& e) {
      client::Log::Write(L"FATAL: XAML unhandled exception",
                         std::wstring(e.Message()));
    });

    CoreApplication::UnhandledErrorDetected(
        [](auto const&, UnhandledErrorDetectedEventArgs const& e) {
          // Propagating it is what turns the error back into an exception this
          // process can report on; without it the framework reports nothing.
          try {
            e.UnhandledError().Propagate();
          } catch (winrt::hresult_error const& error) {
            client::Log::Write(L"FATAL: unhandled WinRT error",
                               std::wstring(error.message()));
          } catch (...) {
            client::Log::Write(L"FATAL: unhandled WinRT error, no detail");
          }
        });

    Suspending([](auto const&, auto const&) {
      client::Log::Write(L"app: suspending");
      client::Log::FlushFromFault();
    });

    // Gecko runs a dozen threads of its own and shuts itself down by calling
    // TerminateProcess; there is no orderly way to park that and pick it up
    // again. When the system says the app is going, the process goes with it
    // rather than lingering with the engine still painting into a window
    // nobody will ever see.
    CoreApplication::Exiting([](auto const&, auto const&) {
      client::Log::Write(L"app: exiting");
      ::TerminateProcess(::GetCurrentProcess(), 0);
    });
    Resuming([](auto const&, auto const&) {
      client::Log::Write(L"app: resuming");
    });

    // The module that hides the window turned out to be the phone's navigation
    // client -- the thing the shell talks to when it switches what is in
    // front. So the shell is switching away from us, and these are the ways an
    // app is told that. None has been on the record.
    CoreApplication::EnteredBackground([](auto const&, auto const&) {
      client::Log::Write(L"app: ENTERED the background -- the shell put "
                         L"something else in front");
      client::Log::FlushFromFault();
    });
    CoreApplication::LeavingBackground([](auto const&, auto const&) {
      client::Log::Write(L"app: leaving the background");
    });
  }

  void OnLaunched(LaunchActivatedEventArgs const& args) {
    // The system splash: the shell's own full-screen surface with the package
    // image, held up until the app is ready. If it is still up thirty seconds
    // in, the window that reports itself visible has never actually been on
    // screen -- and its dismissal is a navigation, done by the very module
    // whose assertion this has been.
    try {
      client::Log::Write(L"splash: previous execution state",
                         std::to_wstring(static_cast<int>(args.PreviousExecutionState())));
      auto splash = args.SplashScreen();
      if (splash) {
        auto where = splash.ImageLocation();
        client::Log::Write(L"splash: the system splash is up, image at " +
                           std::to_wstring(static_cast<int>(where.X)) + L"," +
                           std::to_wstring(static_cast<int>(where.Y)) + L" " +
                           std::to_wstring(static_cast<int>(where.Width)) + L"x" +
                           std::to_wstring(static_cast<int>(where.Height)));
        splash.Dismissed([](auto const&, auto const&) {
          client::Log::Write(L"splash: the system splash was DISMISSED");
          client::Log::FlushFromFault();
        });
      } else {
        client::Log::Write(L"splash: no system splash object");
      }
    } catch (winrt::hresult_error const& error) {
      client::Log::Write(L"splash: could not ask about it",
                         std::wstring(error.message()));
    }
    EnsureContent();
    Window::Current().Activate();
  }

  void OnActivated(IActivatedEventArgs const& args) {
    // This is how the phone hands a browser a link: it activates the app that
    // is registered for http and https with the URL. Without this the app
    // merely opened, which is what "default browser" looked like before.
    std::wstring url;
    try {
      const auto kind = args.Kind();
      client::Log::Write(L"activation: kind " +
                         std::to_wstring(static_cast<int>(kind)));
      if (kind == ActivationKind::Protocol) {
        auto protocolArgs = args.as<ProtocolActivatedEventArgs>();
        if (auto uri = protocolArgs.Uri()) {
          url = uri.AbsoluteUri();
        }
      } else if (kind == ActivationKind::File) {
        auto fileArgs = args.as<FileActivatedEventArgs>();
        auto files = fileArgs.Files();
        if (files && files.Size() > 0) {
          if (auto file =
                  files.GetAt(0)
                      .try_as<winrt::Windows::Storage::IStorageItem>()) {
            // A local file is a URL like any other once it has a scheme.
            std::wstring path(file.Path());
            for (auto& ch : path) {
              if (ch == L'\\') ch = L'/';
            }
            url = L"file:///" + path;
          }
        }
      }
    } catch (winrt::hresult_error const& error) {
      client::Log::Write(L"activation: could not read it",
                         std::wstring(error.message()));
    }

    EnsureContent();
    if (!url.empty() && page_) {
      page_->OpenExternalUrl(url);
    }
    Window::Current().Activate();
  }

  void EnsureContent() {
    if (!page_) {
      page_ = std::make_shared<MainPage>();
    }
    if (!Window::Current().Content()) {
      Window::Current().Content(page_->Root());
    }
  }

  // IXamlMetadataProvider — empty (no custom XAML types).
  winrt::Windows::UI::Xaml::Markup::IXamlType GetXamlType(
      winrt::Windows::UI::Xaml::Interop::TypeName const&) {
    return nullptr;
  }
  winrt::Windows::UI::Xaml::Markup::IXamlType GetXamlType(
      winrt::hstring const&) {
    return nullptr;
  }
  winrt::com_array<winrt::Windows::UI::Xaml::Markup::XmlnsDefinition>
  GetXmlnsDefinitions() {
    return {};
  }
};

}  // namespace gecko_w10m

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // Entry thread must be MTA (cf. C++/CX's [Platform::MTAThread] main).
  // Application::Start creates the ASTA UI thread itself; an uninitialized or
  // STA/ASTA entry thread makes CoreApplication throw RPC_E_WRONG_THREAD.
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  Application::Start([](auto&&) { winrt::make<gecko_w10m::App>(); });
  return 0;
}
