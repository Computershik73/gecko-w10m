// App.cpp — minimal UWP app that runs the JIT probe and shows PASS/FAIL.
#include "pch.h"
#include "jit_probe.h"

#include <fstream>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

using namespace winrt;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::ApplicationModel::Activation;

namespace {
void LogStartup(std::string const& msg) {
  try {
    auto dir = winrt::Windows::Storage::ApplicationData::Current()
                   .LocalFolder()
                   .Path();
    std::string path = winrt::to_string(dir) + "\\startup.log";
    std::ofstream f(path, std::ios::app);
    f << msg << "\n";
  } catch (...) {
  }
}
}  // namespace

// Minimal XAML metadata provider. A code-only C++/WinRT XAML app must still
// implement IXamlMetadataProvider; the framework QIs the Application for it
// during initialization. We author no custom XAML types, so return empties.
struct App : ApplicationT<App, winrt::Windows::UI::Xaml::Markup::IXamlMetadataProvider> {
  void OnLaunched(LaunchActivatedEventArgs const&) {
    LogStartup("OnLaunched enter");
    try {
      auto result = gecko_w10m::test::RunJitProbe();
      LogStartup("probe done");

      Grid root;
      auto rTop = RowDefinition{}; rTop.Height(GridLengthHelper::Auto());
      auto rMid = RowDefinition{};
      rMid.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
      root.RowDefinitions().Append(rTop);
      root.RowDefinitions().Append(rMid);

      Border banner;
      banner.Padding(ThicknessHelper::FromUniformLength(16));
      banner.Background(SolidColorBrush(
          result.passed ? ColorHelper::FromArgb(255, 30, 140, 60)
                        : ColorHelper::FromArgb(255, 176, 32, 32)));
      TextBlock bannerText;
      bannerText.Text(result.passed ? L"JIT: PASS" : L"JIT: FAIL");
      bannerText.Foreground(SolidColorBrush(Colors::White()));
      bannerText.FontSize(32);
      bannerText.FontWeight(FontWeights::Bold());
      banner.Child(bannerText);
      Grid::SetRow(banner, 0);

      ScrollViewer scroller;
      TextBlock detail;
      detail.Text(winrt::hstring(result.report));
      detail.FontFamily(FontFamily(L"Consolas"));
      detail.FontSize(14);
      detail.TextWrapping(TextWrapping::Wrap);
      detail.Padding(ThicknessHelper::FromUniformLength(16));
      scroller.Content(detail);
      Grid::SetRow(scroller, 1);

      root.Children().Append(banner);
      root.Children().Append(scroller);

      Window::Current().Content(root);
      Window::Current().Activate();
      LogStartup("activated");
    } catch (winrt::hresult_error const& e) {
      char buf[256];
      sprintf_s(buf, "hresult_error 0x%08X: %ls", (unsigned)e.code().value,
                e.message().c_str());
      LogStartup(buf);
      throw;
    } catch (std::exception const& e) {
      LogStartup(std::string("std::exception: ") + e.what());
      throw;
    } catch (...) {
      LogStartup("unknown exception");
      throw;
    }
  }

  // IXamlMetadataProvider — empty implementations (no custom XAML types).
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

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // The XAML app model (cf. C++/CX's [Platform::MTAThread] main): the entry
  // thread must be MTA. Application::Start then creates the ASTA UI thread
  // itself and runs the view there. Leaving the thread uninitialized, or making
  // it an STA/ASTA, makes CoreApplication fail its main-thread check with
  // RPC_E_WRONG_THREAD (0x8001010E) and the app dies before OnLaunched.
  winrt::init_apartment(winrt::apartment_type::multi_threaded);

  Application::Start([](auto&&) { winrt::make<App>(); });
  return 0;
}
