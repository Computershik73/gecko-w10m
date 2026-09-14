// MainPage.cpp
#include "pch.h"
#include "MainPage.h"

#include "client/SearchEngines.h"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage;

namespace gecko_w10m {

MainPage::MainPage() {
  using client::BrowserPreferences;

  // Runtime: profile is the app's LocalState (reachable via broadFileSystemAccess).
  auto localState = ApplicationData::Current().LocalFolder().Path();
  bool jit = BrowserPreferences::Shared().IsJitEnabled();
  runtime_ = engine::Runtime::Create(std::wstring(localState), jit, 96);
  tabManager_ = std::make_unique<client::TabManager>(runtime_);

  BuildUi();
  WireEngine();

  // Open the first tab (homepage / blank).
  tabManager_->AddTab(/*isPrivate*/ false, L"about:home");
  RebuildTabStrip();
  RefreshChrome();
}

void MainPage::BuildUi() {
  using client::BrowserPreferences;
  using client::ChromePosition;

  root_ = Grid();

  // Rows: [tab strip][content][chrome] or [chrome][content][tab strip]
  auto rTop = RowDefinition();
  rTop.Height(GridLengthHelper::Auto());
  auto rMid = RowDefinition();
  rMid.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  auto rBot = RowDefinition();
  rBot.Height(GridLengthHelper::Auto());
  root_.RowDefinitions().Append(rTop);
  root_.RowDefinitions().Append(rMid);
  root_.RowDefinitions().Append(rBot);

  // --- Tab strip (horizontal, scrollable) ---
  auto tabScroller = ScrollViewer();
  tabScroller.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
  tabScroller.VerticalScrollBarVisibility(ScrollBarVisibility::Disabled);
  tabScroller.HorizontalScrollMode(ScrollMode::Auto);
  tabStrip_ = StackPanel();
  tabStrip_.Orientation(Orientation::Horizontal);
  tabScroller.Content(tabStrip_);

  // --- Content host (engine SwapChainPanel would attach here) ---
  contentHost_ = Border();
  contentHost_.Background(SolidColorBrush(winrt::Windows::UI::Colors::White()));
  statusText_ = TextBlock();
  statusText_.Text(L"GeckoW10m for Windows 10 Mobile");
  statusText_.HorizontalAlignment(HorizontalAlignment::Center);
  statusText_.VerticalAlignment(VerticalAlignment::Center);
  statusText_.TextWrapping(TextWrapping::Wrap);
  statusText_.Foreground(SolidColorBrush(winrt::Windows::UI::Colors::Gray()));
  contentHost_.Child(statusText_);

  // --- Chrome (address bar + nav buttons + progress) ---
  auto chrome = Grid();
  chrome.Padding(ThicknessHelper::FromUniformLength(6));
  auto chromeBg = SolidColorBrush();
  chromeBg.Color(winrt::Windows::UI::ColorHelper::FromArgb(255, 245, 245, 247));
  chrome.Background(chromeBg);

  progress_ = ProgressBar();
  progress_.Minimum(0);
  progress_.Maximum(1);
  progress_.Value(0);
  progress_.Height(2);
  progress_.VerticalAlignment(VerticalAlignment::Top);

  auto row = StackPanel();
  row.Orientation(Orientation::Horizontal);

  auto makeButton = [](std::wstring_view glyph) {
    Button b;
    FontIcon icon;
    icon.Glyph(winrt::hstring(glyph));
    icon.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
    b.Content(icon);
    b.Margin(ThicknessHelper::FromLengths(2, 0, 2, 0));
    b.MinWidth(40);
    return b;
  };

  backButton_ = makeButton(L"");     // Back
  forwardButton_ = makeButton(L"");  // Forward
  reloadButton_ = makeButton(L"");   // Refresh
  newTabButton_ = makeButton(L"");   // Add

  addressBar_ = TextBox();
  addressBar_.PlaceholderText(L"Search or enter address");
  addressBar_.InputScope([] {
    InputScope s;
    InputScopeName n;
    n.NameValue(InputScopeNameValue::Url);
    s.Names().Append(n);
    return s;
  }());
  addressBar_.Width(180);
  addressBar_.Margin(ThicknessHelper::FromLengths(4, 0, 4, 0));

  row.Children().Append(backButton_);
  row.Children().Append(forwardButton_);
  row.Children().Append(reloadButton_);
  row.Children().Append(addressBar_);
  row.Children().Append(newTabButton_);

  auto chromeStack = StackPanel();
  chromeStack.Children().Append(progress_);
  chromeStack.Children().Append(row);
  chrome.Children().Append(chromeStack);

  // --- Placement depends on the address-bar position preference ---
  bool barOnTop =
      BrowserPreferences::Shared().AddressBarPosition() == ChromePosition::Top;

  Grid::SetRow(tabScroller, barOnTop ? 0 : 2);
  Grid::SetRow(contentHost_, 1);
  Grid::SetRow(chrome, barOnTop ? 0 : 2);
  // If bar on top, tab strip goes bottom and vice versa; keep them distinct.
  Grid::SetRow(tabScroller, barOnTop ? 2 : 0);

  root_.Children().Append(tabScroller);
  root_.Children().Append(contentHost_);
  root_.Children().Append(chrome);

  // --- Events ---
  backButton_.Click([this](auto&&, auto&&) {
    if (auto* t = tabManager_->SelectedTab(); t && t->session)
      t->session->GoBack();
  });
  forwardButton_.Click([this](auto&&, auto&&) {
    if (auto* t = tabManager_->SelectedTab(); t && t->session)
      t->session->GoForward();
  });
  reloadButton_.Click([this](auto&&, auto&&) {
    if (auto* t = tabManager_->SelectedTab(); t && t->session)
      t->session->Reload();
  });
  newTabButton_.Click([this](auto&&, auto&&) {
    tabManager_->AddTab(false, L"about:home");
    RebuildTabStrip();
    RefreshChrome();
  });
  addressBar_.KeyDown([this](auto&&, KeyRoutedEventArgs const& e) {
    if (e.Key() == winrt::Windows::System::VirtualKey::Enter) {
      Navigate(std::wstring(addressBar_.Text()));
    }
  });
}

void MainPage::WireEngine() {
  tabManager_->OnChanged([this] { RefreshChrome(); });
}

void MainPage::Navigate(std::wstring_view entry) {
  auto* tab = tabManager_->SelectedTab();
  if (!tab) {
    tabManager_->AddTab(false, L"");
    tab = tabManager_->SelectedTab();
  }
  auto url = client::SearchEngines::ResolveEntry(entry);
  tab->url = url;

  if (tab->session) {
    engine::SessionObserver obs;
    auto dispatcher = root_.Dispatcher();
    obs.LocationChanged = [this, dispatcher](std::wstring u) {
      dispatcher.RunAsync(
          winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
          [this, u] { addressBar_.Text(winrt::hstring(u)); });
    };
    obs.TitleChanged = [this, dispatcher](std::wstring t) {
      dispatcher.RunAsync(
          winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
          [this, t] { statusText_.Text(winrt::hstring(t)); });
    };
    obs.ProgressChanged = [this, dispatcher](float p) {
      dispatcher.RunAsync(
          winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
          [this, p] { progress_.Value(p); });
    };
    obs.CanGoBackChanged = [](bool) {};
    obs.CanGoForwardChanged = [](bool) {};
    tab->session->Observe(std::move(obs));
    tab->session->LoadUri(url);
  }
  RefreshChrome();
}

void MainPage::RefreshChrome() {
  auto* tab = tabManager_->SelectedTab();
  if (!tab) {
    addressBar_.Text(L"");
    backButton_.IsEnabled(false);
    forwardButton_.IsEnabled(false);
    return;
  }
  addressBar_.Text(winrt::hstring(tab->url));
  bool back = tab->session && tab->session->CanGoBack();
  bool fwd = tab->session && tab->session->CanGoForward();
  backButton_.IsEnabled(back);
  forwardButton_.IsEnabled(fwd);
}

void MainPage::RebuildTabStrip() {
  tabStrip_.Children().Clear();
  for (int i = 0; i < tabManager_->Count(); ++i) {
    auto* tab = tabManager_->At(i);
    Button b;
    b.Content(winrt::box_value(
        winrt::hstring(tab->title.empty() ? L"New Tab" : tab->title)));
    b.Margin(ThicknessHelper::FromLengths(2, 4, 2, 4));
    int index = i;
    b.Click([this, index](auto&&, auto&&) {
      tabManager_->SelectTab(index);
      RefreshChrome();
    });
    tabStrip_.Children().Append(b);
  }
}

}  // namespace gecko_w10m
