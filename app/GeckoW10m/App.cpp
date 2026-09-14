// App.cpp — UWP application entry point (C++/WinRT, no XAML markup compiler).
#include "pch.h"

#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

#include "MainPage.h"

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

  void OnLaunched(LaunchActivatedEventArgs const&) {
    EnsureContent();
    Window::Current().Activate();
  }

  void OnActivated(IActivatedEventArgs const&) {
    // Protocol activation (gecko_w10m:// , http:// , https://) lands here.
    EnsureContent();
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
