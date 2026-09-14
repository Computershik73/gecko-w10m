// App.cpp — minimal UWP app that runs JavaScript through the ported
// SpiderMonkey engine (mozjs-155) and shows the result. C++/WinRT (exceptions);
// the JSAPI embedding lives in smoke.cpp, compiled exception-free.
#include "pch.h"

#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <cstdint>
#include <string>

using namespace winrt;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml::Interop;
using namespace winrt::Windows::UI::Xaml::Markup;
using namespace winrt::Windows::ApplicationModel::Activation;

// Defined in smoke.cpp (exception-free TU): runs a hot JS loop via the JSAPI.
bool RunJsSmoke(int32_t* out, std::string* detail);

struct App : ApplicationT<App, IXamlMetadataProvider> {
  void OnLaunched(LaunchActivatedEventArgs const&) {
    int32_t result = 0;
    std::string detail;
    bool ok = RunJsSmoke(&result, &detail);

    std::wstring report =
        ok ? L"JS: RUNS\n\n" : L"JS: FAIL\n\n";
    report += L"SpiderMonkey (mozjs-155) on Windows 10 Mobile\n\n";
    report += ok ? L"The ported engine ran JavaScript with JIT.\n"
                 : L"The engine FAILED to run.\n";
    report += L"\nScript: a 3,000,000-iteration integer loop, mod 1e6\n";
    report += L"Result: " + std::to_wstring(result) + L"\n";
    report += L"Status: " + std::wstring(detail.begin(), detail.end());

    // A single TextBlock as the window content — avoids XAML collection Append,
    // which the SDK's prebuilt cppwinrt headers don't define for a manual build.
    Border root;
    root.Padding(ThicknessHelper::FromUniformLength(20));
    root.Background(SolidColorBrush(
        ok ? ColorHelper::FromArgb(255, 24, 96, 48)
           : ColorHelper::FromArgb(255, 128, 24, 24)));
    TextBlock text;
    text.Text(winrt::hstring(report));
    text.Foreground(SolidColorBrush(Colors::White()));
    text.FontSize(18);
    text.TextWrapping(TextWrapping::Wrap);
    root.Child(text);

    Window::Current().Content(root);
    Window::Current().Activate();
  }

  IXamlType GetXamlType(TypeName const&) { return nullptr; }
  IXamlType GetXamlType(winrt::hstring const&) { return nullptr; }
  winrt::com_array<XmlnsDefinition> GetXmlnsDefinitions() { return {}; }
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  Application::Start([](auto&&) { winrt::make<App>(); });
  return 0;
}
