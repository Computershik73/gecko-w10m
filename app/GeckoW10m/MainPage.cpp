// MainPage.cpp
#include "pch.h"
#include "MainPage.h"

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Display.h>

#include <thread>

#include "client/Log.h"
#include "engine/CrashProbe.h"
#include "engine/GeckoRuntimeHost.h"
#include "client/SearchEngines.h"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::ViewManagement;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Metadata;
using namespace winrt::Windows::Storage;

namespace gecko_w10m {
namespace {

using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;

// A deliberately high-contrast scheme. The first build used near-white chrome
// with grey text on a white page, which on a phone screen in daylight is
// simply unreadable.
Color ChromeBg() { return ColorHelper::FromArgb(255, 32, 32, 36); }
Color ChromeFg() { return ColorHelper::FromArgb(255, 245, 245, 245); }
Color ContentBg() { return ColorHelper::FromArgb(255, 255, 255, 255); }
Color ContentFg() { return ColorHelper::FromArgb(255, 24, 24, 28); }
Color Accent() { return ColorHelper::FromArgb(255, 224, 108, 31); }
Color LogBg() { return ColorHelper::FromArgb(242, 16, 16, 18); }
Color LogFg() { return ColorHelper::FromArgb(255, 190, 235, 190); }

SolidColorBrush Brush(Color c) {
  SolidColorBrush b;
  b.Color(c);
  return b;
}

}  // namespace

MainPage::MainPage() {
  using client::BrowserPreferences;
  using client::Log;

  auto localState = ApplicationData::Current().LocalFolder().Path();
  Log::Init(std::wstring(localState));
  Log::Write(L"shell starting");
  Log::Write(L"LocalState", std::wstring(localState));

  // Before the engine, before the UI: a fault reported only once Gecko is
  // running looks like Gecko's fault, and there was no way to tell that from
  // something this device does on every launch.
  engine::InstallProcessProbes(std::wstring(localState));
  engine::StartHeartbeat();
  engine::MakeSecondD3DDevice();

  // The compositor telling us it has lost its surfaces is the one warning a
  // GPU reset gives an application. If the phone's driver is being knocked
  // over by the engine using D3D11 alongside XAML, this is where it would say
  // so, and it has never been asked.
  Media::CompositionTarget::SurfaceContentsLost([](auto&&, auto&&) {
    client::Log::Write(L"FATAL: the compositor lost its surfaces");
  });

  bool jit = BrowserPreferences::Shared().IsJitEnabled();
  Log::Write(L"jit preference", jit ? L"enabled" : L"disabled");

  runtime_ = engine::Runtime::Create(std::wstring(localState), jit, 96);
  Log::Write(L"runtime", runtime_ ? L"created" : L"FAILED to create");

  tabManager_ = std::make_unique<client::TabManager>(runtime_);

  // Gecko is headless and has no idea how large the phone is, so it has to be
  // told: the size of the window in physical pixels, which is what the buffer
  // the shell reads back will be. XAML measures in view pixels, and on a phone
  // those are nothing like the same thing.
  double raw = 1.0;
  if (ApiInformation::IsPropertyPresent(
          L"Windows.Graphics.Display.DisplayInformation",
          L"RawPixelsPerViewPixel")) {
    raw = winrt::Windows::Graphics::Display::DisplayInformation::
        GetForCurrentView()
            .RawPixelsPerViewPixel();
  }
  auto bounds = Window::Current().Bounds();
  const int pixelWidth = static_cast<int>(bounds.Width * raw + 0.5);
  const int pixelHeight = static_cast<int>(bounds.Height * raw + 0.5);
  Log::Write(L"view: asking the engine for " + std::to_wstring(pixelWidth) +
             L"x" + std::to_wstring(pixelHeight) + L" physical pixels");

  // The desktop Firefox chrome has a narrowest width it will accept, and it
  // wins: ask for anything narrower and the window comes back at the minimum.
  // Measured at 504 CSS pixels on this build -- the engine was asked for 1440
  // device pixels and returned 1765, which is 504 at 3.5 device pixels per CSS
  // pixel. At the scale Windows reports, a 1440-pixel screen is 411 CSS pixels
  // wide, so the window could not be as narrow as the phone and every frame
  // carried a quarter more columns than the screen can show, rasterised in
  // software and then copied.
  //
  // So the scale is whatever Windows says, or whatever makes the screen wide
  // enough, whichever is smaller. 540 rather than 504 leaves room for rounding
  // and for a locale whose toolbar needs a little more.
  //
  // This is a second scale, not a correction to the first. `raw` says how many
  // physical pixels Windows puts in a view pixel and is what turns a XAML size
  // or a keyboard height into frame pixels; `cssScale` says how big Gecko
  // should draw. Writing the second over the first, which the last build did,
  // makes the shell measure the keyboard in the wrong unit: it reported 669
  // physical pixels covered where the keyboard really takes 878, and the
  // window moved up by too little.
  constexpr double kNarrowestChromeCss = 540.0;
  double cssScale = raw;
  if (pixelWidth > 0 && cssScale > pixelWidth / kNarrowestChromeCss) {
    cssScale = pixelWidth / kNarrowestChromeCss;
    Log::Write(L"view: scale " + std::to_wstring(raw) + L" would leave " +
               std::to_wstring(static_cast<int>(pixelWidth / raw)) +
               L" CSS pixels, too narrow for the chrome; drawing at " +
               std::to_wstring(cssScale));
  }
  Log::WriteNum(L"cpu: cores visible to the process",
                static_cast<int>(std::thread::hardware_concurrency()));

  engineView_ = std::make_unique<client::EngineView>(pixelWidth, pixelHeight,
                                                     raw);

  BuildUi();
  WireEngine();
  WireLog();

  tabManager_->AddTab(/*isPrivate*/ false, L"about:home");
  Log::WriteNum(L"tabs after first AddTab", tabManager_->Count());
  RebuildTabStrip();
  RefreshChrome();

  // The engine only reports anything in response to a load. Without this the
  // shell sat on a static placeholder and never told us whether the runtime,
  // the JIT probe or xul.dll had worked.
  Navigate(L"about:home");

  engineView_->OnFirstFrame([this, state = std::wstring(localState)]() {
    if (splash_) {
      splash_.Visibility(Visibility::Collapsed);
    }
    // Drawing is the only proof the engine started; anything short of it could
    // be a launch that is about to die.
    engine::MarkGeckoHealthy(state);
  });
  engineView_->Start();

  // Last, so the window is up and the log is readable before Gecko gets its
  // chance to take the process down with it.
  engine::StartGeckoRuntime(std::wstring(localState), pixelWidth, pixelHeight,
                            cssScale);
}

void MainPage::ApplyVisibleBounds() {
  auto view = ApplicationView::GetForCurrentView();
  auto visible = view.VisibleBounds();
  auto window = Window::Current().Bounds();

  double left = visible.X - window.X;
  double top = visible.Y - window.Y;
  double right = (window.X + window.Width) - (visible.X + visible.Width);
  double bottom = (window.Y + window.Height) - (visible.Y + visible.Height);

  // Negative or absurd values mean the two rectangles are not comparable;
  // padding nothing is better than padding wrongly.
  auto sane = [](double v) { return (v > 0 && v < 400) ? v : 0.0; };
  root_.Padding(
      ThicknessHelper::FromLengths(sane(left), sane(top), sane(right), sane(bottom)));
}

void MainPage::BuildUi() {
  using client::BrowserPreferences;
  using client::ChromePosition;
  using client::Log;

  root_ = Grid();
  root_.Background(Brush(ChromeBg()));

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
  tabScroller.Background(Brush(ChromeBg()));
  tabStrip_ = StackPanel();
  tabStrip_.Orientation(Orientation::Horizontal);
  tabScroller.Content(tabStrip_);

  // --- Content host (engine SwapChainPanel would attach here) ---
  contentHost_ = Border();
  contentHost_.Background(Brush(ContentBg()));
  statusText_ = TextBlock();
  statusText_.Text(L"GeckoW10m for Windows 10 Mobile");
  statusText_.HorizontalAlignment(HorizontalAlignment::Center);
  statusText_.VerticalAlignment(VerticalAlignment::Center);
  statusText_.TextWrapping(TextWrapping::Wrap);
  statusText_.TextAlignment(TextAlignment::Center);
  statusText_.Margin(ThicknessHelper::FromUniformLength(16));
  statusText_.Foreground(Brush(ContentFg()));
  // The engine's frames go on top of the placeholder, so the text below shows
  // until there is something better to show and never afterwards.
  auto contentStack = Grid();
  contentStack.Children().Append(statusText_);
  // Order matters: the text sink goes underneath the picture. Above it, every
  // tap landed on a text control and Windows raised the keyboard for each one,
  // and nothing reached the engine at all.
  contentStack.Children().Append(engineView_->TextSink());
  // Back in the tree. Out of it the browser died on time, twice, with the
  // swap chain made and handed over and never composited -- so what the
  // compositor does with our swap chain is not what kills this process, and
  // there is no reason left to keep the picture off the screen.
  contentStack.Children().Append(engineView_->Panel());
  contentStack.Children().Append(engineView_->Surface());

  // The browser's own logo, out of the browser's own package. It is the thing
  // the user is waiting for, so it is the right thing to wait in front of.
  splash_ = Grid();
  splash_.Background(Brush(ContentBg()));
  {
    Image logo;
    logo.Width(128);
    logo.Height(128);
    logo.HorizontalAlignment(HorizontalAlignment::Center);
    logo.VerticalAlignment(VerticalAlignment::Center);
    logo.Source(Media::Imaging::BitmapImage(Uri(
        L"ms-appx:///browser/chrome/browser/content/branding/about-logo@2x.png")));
    splash_.Children().Append(logo);
  }
  contentStack.Children().Append(splash_);
  contentHost_.Child(contentStack);

  // --- Diagnostics overlay, hidden until asked for ---
  logPanel_ = Border();
  logPanel_.Background(Brush(LogBg()));
  logPanel_.Visibility(Visibility::Collapsed);
  logScroller_ = ScrollViewer();
  logScroller_.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
  logText_ = TextBlock();
  logText_.Foreground(Brush(LogFg()));
  logText_.FontFamily(FontFamily(L"Consolas"));
  logText_.FontSize(11);
  logText_.TextWrapping(TextWrapping::Wrap);
  logText_.Margin(ThicknessHelper::FromUniformLength(8));
  logScroller_.Content(logText_);
  logPanel_.Child(logScroller_);

  // --- Chrome (address bar + nav buttons + progress) ---
  auto chrome = Grid();
  chrome.Padding(ThicknessHelper::FromLengths(6, 4, 6, 4));
  chrome.Background(Brush(ChromeBg()));

  progress_ = ProgressBar();
  progress_.Minimum(0);
  progress_.Maximum(1);
  progress_.Value(0);
  progress_.Height(3);
  progress_.Foreground(Brush(Accent()));
  progress_.VerticalAlignment(VerticalAlignment::Top);

  auto row = StackPanel();
  row.Orientation(Orientation::Horizontal);

  auto makeButton = [](std::wstring_view glyph) {
    Button b;
    FontIcon icon;
    icon.Glyph(winrt::hstring(glyph));
    icon.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
    icon.FontSize(16);
    icon.Foreground(Brush(ChromeFg()));
    b.Content(icon);
    b.Margin(ThicknessHelper::FromLengths(1, 0, 1, 0));
    b.Padding(ThicknessHelper::FromLengths(6, 6, 6, 6));
    b.MinWidth(38);
    b.Background(Brush(ColorHelper::FromArgb(255, 52, 52, 58)));
    b.Foreground(Brush(ChromeFg()));
    b.BorderThickness(ThicknessHelper::FromUniformLength(0));
    return b;
  };

  backButton_ = makeButton(L"\uE72B");     // Back
  forwardButton_ = makeButton(L"\uE72A");  // Forward
  reloadButton_ = makeButton(L"\uE72C");   // Refresh
  newTabButton_ = makeButton(L"\uE710");   // Add
  logButton_ = makeButton(L"\uE7BA");      // Warning/diagnostics

  addressBar_ = TextBox();
  addressBar_.PlaceholderText(L"Search or enter address");
  addressBar_.InputScope([] {
    InputScope s;
    InputScopeName n;
    n.NameValue(InputScopeNameValue::Url);
    s.Names().Append(n);
    return s;
  }());
  addressBar_.Margin(ThicknessHelper::FromLengths(4, 0, 4, 0));
  addressBar_.MinWidth(140);
  addressBar_.FontSize(14);

  row.Children().Append(backButton_);
  row.Children().Append(forwardButton_);
  row.Children().Append(reloadButton_);
  row.Children().Append(addressBar_);
  row.Children().Append(newTabButton_);
  row.Children().Append(logButton_);

  auto chromeStack = StackPanel();
  chromeStack.Children().Append(progress_);
  chromeStack.Children().Append(row);
  chrome.Children().Append(chromeStack);

  // --- Placement depends on the address-bar position preference ---
  bool barOnTop =
      BrowserPreferences::Shared().AddressBarPosition() == ChromePosition::Top;

  // Only the engine's own picture goes on screen. Firefox has a tab strip, an
  // address bar and a menu of its own, and now that they are visible and can be
  // touched, a second set underneath them is not a second opinion -- it is two
  // address bars, one of which does nothing. The controls above are still built
  // because the engine facade reports through them; they are simply not shown.
  //
  // The log panel goes with them. It was the only way to read anything off the
  // device before the engine could draw, and that has not been true for a while
  // -- the log is a file, and that is how it has actually been read all along.
  Grid::SetRow(contentHost_, 1);
  root_.Children().Append(contentHost_);

  // The engine's window is the size of this, measured whenever it changes.
  // Not the size of the picture inside it: a stretched Image reports what it
  // drew, which is the frame, which would make the frame decide its own size.
  engineView_->WatchRoom(contentHost_);

  // The status bar exists only on mobile; tint it to match so the chrome does
  // not look like it is floating under a foreign strip.
  if (ApiInformation::IsTypePresent(L"Windows.UI.ViewManagement.StatusBar")) {
    auto status = StatusBar::GetForCurrentView();
    status.BackgroundColor(ChromeBg());
    status.BackgroundOpacity(1.0);
    status.ForegroundColor(ChromeFg());
  }

  auto view = ApplicationView::GetForCurrentView();
  view.SetDesiredBoundsMode(ApplicationViewBoundsMode::UseVisible);
  view.VisibleBoundsChanged(
      [this](auto&&, auto&&) { ApplyVisibleBounds(); });
  ApplyVisibleBounds();

  // --- Events ---
  backButton_.Click([this](auto&&, auto&&) {
    client::Log::Write(L"ui: back");
    if (auto* t = tabManager_->SelectedTab(); t && t->session)
      t->session->GoBack();
  });
  forwardButton_.Click([this](auto&&, auto&&) {
    client::Log::Write(L"ui: forward");
    if (auto* t = tabManager_->SelectedTab(); t && t->session)
      t->session->GoForward();
  });
  reloadButton_.Click([this](auto&&, auto&&) {
    client::Log::Write(L"ui: reload");
    if (auto* t = tabManager_->SelectedTab(); t && t->session)
      t->session->Reload();
  });
  newTabButton_.Click([this](auto&&, auto&&) {
    client::Log::Write(L"ui: new tab");
    tabManager_->AddTab(false, L"about:home");
    RebuildTabStrip();
    RefreshChrome();
  });
  logButton_.Click([this](auto&&, auto&&) {
    bool showing = logPanel_.Visibility() == Visibility::Visible;
    logPanel_.Visibility(showing ? Visibility::Collapsed : Visibility::Visible);
    if (!showing) {
      std::wstring all;
      for (auto const& line : client::Log::Recent()) {
        all += line;
        all += L"\n";
      }
      all += L"\nlog file: " + client::Log::Path();
      logText_.Text(winrt::hstring(all));
      logScroller_.UpdateLayout();
      logScroller_.ChangeView(nullptr, logScroller_.ScrollableHeight(), nullptr);
    }
  });
  addressBar_.KeyDown([this](auto&&, KeyRoutedEventArgs const& e) {
    if (e.Key() == winrt::Windows::System::VirtualKey::Enter) {
      Navigate(std::wstring(addressBar_.Text()));
    }
  });

  Log::Write(L"ui built");
}

void MainPage::WireEngine() {
  tabManager_->OnChanged([this] { RefreshChrome(); });
}

void MainPage::WireLog() {
  auto dispatcher = root_.Dispatcher();
  client::Log::OnLine([this, dispatcher](std::wstring line) {
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low,
                        [this, line] { AppendLogLine(line); });
  });
}

void MainPage::AppendLogLine(std::wstring line) {
  if (logPanel_.Visibility() != Visibility::Visible) return;
  logText_.Text(logText_.Text() + winrt::hstring(line + L"\n"));
  logScroller_.ChangeView(nullptr, logScroller_.ScrollableHeight(), nullptr);
}

void MainPage::Navigate(std::wstring_view entry) {
  using client::Log;

  auto* tab = tabManager_->SelectedTab();
  if (!tab) {
    Log::Write(L"navigate: no selected tab, creating one");
    tabManager_->AddTab(false, L"");
    tab = tabManager_->SelectedTab();
  }
  if (!tab) {
    Log::Write(L"navigate: still no tab, giving up");
    return;
  }

  auto url = client::SearchEngines::ResolveEntry(entry);
  tab->url = url;
  Log::Write(L"navigate", url);

  if (!tab->session) {
    Log::Write(L"navigate: tab has no engine session (runtime create failed?)");
    statusText_.Text(L"No engine session.\nOpen the log for details.");
    RefreshChrome();
    return;
  }

  engine::SessionObserver obs;
  auto dispatcher = root_.Dispatcher();
  obs.LocationChanged = [this, dispatcher](std::wstring u) {
    client::Log::Write(L"engine: location", u);
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [this, u] { addressBar_.Text(winrt::hstring(u)); });
  };
  obs.TitleChanged = [this, dispatcher](std::wstring t) {
    client::Log::Write(L"engine: title", t);
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [this, t] { statusText_.Text(winrt::hstring(t)); });
  };
  obs.ProgressChanged = [this, dispatcher](float p) {
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [this, p] { progress_.Value(p); });
  };
  obs.CanGoBackChanged = [](bool) {};
  obs.CanGoForwardChanged = [](bool) {};
  tab->session->Observe(std::move(obs));
  tab->session->LoadUri(url);
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
    TextBlock label;
    label.Text(winrt::hstring(tab->title.empty() ? L"New Tab" : tab->title));
    label.Foreground(Brush(ChromeFg()));
    label.FontSize(13);
    b.Content(label);
    b.Background(Brush(ColorHelper::FromArgb(
        255, i == tabManager_->SelectedIndex() ? 70 : 44,
        i == tabManager_->SelectedIndex() ? 70 : 44,
        i == tabManager_->SelectedIndex() ? 78 : 50)));
    b.BorderThickness(ThicknessHelper::FromUniformLength(0));
    b.Margin(ThicknessHelper::FromLengths(2, 4, 2, 4));
    b.Padding(ThicknessHelper::FromLengths(10, 4, 10, 4));
    int index = i;
    b.Click([this, index](auto&&, auto&&) {
      tabManager_->SelectTab(index);
      RefreshChrome();
    });
    tabStrip_.Children().Append(b);
  }
}

}  // namespace gecko_w10m
