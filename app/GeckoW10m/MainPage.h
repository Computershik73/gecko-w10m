// MainPage.h — the browser shell UI, built programmatically (no XAML markup).
// Maps the essential surface of the iOS BrowserViewController: address bar,
// back / forward / reload, progress, tab strip, and the engine content host.
#pragma once

#include <memory>
#include "client/BrowserPreferences.h"
#include "client/TabManager.h"
#include "engine/GeckoEngine.h"

namespace gecko_w10m {

class MainPage {
 public:
  MainPage();

  // The root visual to set as Window content.
  winrt::Windows::UI::Xaml::UIElement Root() const { return root_; }

 private:
  void BuildUi();
  void WireEngine();

  void Navigate(std::wstring_view entry);
  void RefreshChrome();
  void RebuildTabStrip();

  // Engine
  std::shared_ptr<engine::Runtime> runtime_;
  std::unique_ptr<client::TabManager> tabManager_;

  // Views
  winrt::Windows::UI::Xaml::Controls::Grid root_{nullptr};
  winrt::Windows::UI::Xaml::Controls::TextBox addressBar_{nullptr};
  winrt::Windows::UI::Xaml::Controls::Button backButton_{nullptr};
  winrt::Windows::UI::Xaml::Controls::Button forwardButton_{nullptr};
  winrt::Windows::UI::Xaml::Controls::Button reloadButton_{nullptr};
  winrt::Windows::UI::Xaml::Controls::Button newTabButton_{nullptr};
  winrt::Windows::UI::Xaml::Controls::ProgressBar progress_{nullptr};
  winrt::Windows::UI::Xaml::Controls::Border contentHost_{nullptr};
  winrt::Windows::UI::Xaml::Controls::TextBlock statusText_{nullptr};
  winrt::Windows::UI::Xaml::Controls::StackPanel tabStrip_{nullptr};
};

}  // namespace gecko_w10m
