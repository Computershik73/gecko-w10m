#include "pch.h"

#include "client/EngineView.h"

#include <robuffer.h>
#include <windows.ui.xaml.media.dxinterop.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "client/CaptureConsent.h"
#include "client/ClipboardBridge.h"
#include "client/DrmBridge.h"
#include "client/FileBridge.h"
#include "client/Log.h"
#include "engine/CrashProbe.h"
#include "engine/OverlayProbe.h"
#include <winrt/Windows.Foundation.Metadata.h>

#include "winrt/Windows.Graphics.Display.h"
#include "winrt/Windows.UI.Core.h"
#include "winrt/Windows.UI.Input.h"
#include "winrt/Windows.UI.Xaml.Input.h"
#include "winrt/Windows.UI.Xaml.Media.h"

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
namespace Input = winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Media::Imaging;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::UI::ViewManagement::InputPane;
using winrt::Windows::UI::ViewManagement::StatusBar;

namespace gecko_w10m::client {

namespace {
// The UI thread's dispatcher, kept for the launcher callback, which is called
// from Gecko's threads and has no way to ask for one.
winrt::Windows::UI::Core::CoreDispatcher gUiDispatcher{nullptr};
// Whether the engine has a window fullscreen, as it last told the shell
// (FullscreenChanged); the back button must answer at once and reads it.
std::atomic<bool> gFullscreen{false};
// When the on-screen keyboard last went away. A Back that closes the keyboard
// seems to reach the app as well, in the same moment, and that press was the
// keyboard's, not the page's.
std::atomic<unsigned long long> gKeyboardHiddenAt{0};
}  // namespace

namespace {
// Set by ANGLE's logger, which is a plain function pointer and can carry no
// state of its own.
std::atomic<bool> g_panelPresenting{false};

// The video layer's panel and the engine's acknowledgement entry point, for
// the static callbacks the engine's compositor thread calls.
winrt::Windows::UI::Xaml::Controls::SwapChainPanel gVideoPanel{nullptr};
std::atomic<void (*)(uint32_t)> gVideoAck{nullptr};
double gRawPerView = 1.0;

// The browser's panel, which the video panel lies under or above.
winrt::Windows::UI::Xaml::Controls::SwapChainPanel gBrowserPanel{nullptr};

// The video's last placement, in the browser surface's pixels, and the rows
// and columns cut off its visible part while the panel is above the
// browser's (for a band the page draws over the video's edge).
struct VideoPlacement {
  int32_t x = 0, y = 0, width = 0, height = 0;
  int32_t clipX = 0, clipY = 0, clipWidth = 0, clipHeight = 0;
  int32_t chainWidth = 0, chainHeight = 0;
  // How far the engine turned the frames in the chain, clockwise; the panel
  // turns them back.
  int32_t rotation = 0;
  int32_t trimTop = 0, trimBottom = 0, trimLeft = 0, trimRight = 0;
  bool above = false;
  // The content host's background, taken away while the video is above.
  winrt::Windows::UI::Xaml::Controls::Border host{nullptr};
  winrt::Windows::UI::Xaml::Media::Brush hostBackground{nullptr};
};
VideoPlacement gVideoPlacement;

// The panel's transform: from the chain's pixels to the video's place in
// view pixels. Everything arrives in the browser surface's pixels, which are
// the display's. A swap chain given to a SwapChainPanel is shown one of its
// pixels per view pixel, whatever the display's density -- the browser's own
// panel has ANGLE undo that with a matrix transform -- so the panel is
// chain-sized in view pixels, and this takes it to the video's place. Only
// the display's density goes into it: the panel's CompositionScale already
// includes the transform set here, and computing from it fed each placement
// the last one's scale -- the video showed magnified.
//
// Frames the engine turned (in landscape, so that the display has nothing
// left to turn: see Rotator in GeckoW10mVideoLayer.cpp) are turned back
// here. XAML applies scale, then rotation (clockwise, about the origin),
// then translation; the translation puts the turned box back on the video.
CompositeTransform VideoTransform() {
  const VideoPlacement& p = gVideoPlacement;
  const double raw = gRawPerView > 0 ? gRawPerView : 1.0;
  const double left = p.x / raw, top = p.y / raw;
  const double width = p.width / raw, height = p.height / raw;
  const int32_t back = (360 - p.rotation) % 360;
  const bool quarter = back == 90 || back == 270;
  CompositeTransform t;
  // The chain is the frame's size, swapped for a quarter turn.
  t.ScaleX((quarter ? height : width) / p.chainWidth);
  t.ScaleY((quarter ? width : height) / p.chainHeight);
  t.Rotation(back);
  switch (back) {
    case 90:  // (u, v) -> (-v, u)
      t.TranslateX(left + width);
      t.TranslateY(top);
      break;
    case 180:  // (u, v) -> (-u, -v)
      t.TranslateX(left + width);
      t.TranslateY(top + height);
      break;
    case 270:  // (u, v) -> (v, -u)
      t.TranslateX(left);
      t.TranslateY(top + height);
      break;
    default:
      t.TranslateX(left);
      t.TranslateY(top);
      break;
  }
  return t;
}

// The part of the video the page shows, less the trimmed rows and columns,
// as the panel's clip in its own units (chain pixels, before the transform)
// -- and no clip at all when all of it shows: a clipped layer may be one the
// display cannot take as an overlay. True when the panel is clipped.
bool ApplyVideoClip(CompositeTransform const& aTransform) {
  const VideoPlacement& p = gVideoPlacement;
  if (!gVideoPanel || p.width <= 0 || p.height <= 0) {
    return false;
  }
  const int32_t clipX = p.clipX + p.trimLeft;
  const int32_t clipY = p.clipY + p.trimTop;
  const int32_t clipWidth = std::max(0, p.clipWidth - p.trimLeft - p.trimRight);
  const int32_t clipHeight =
      std::max(0, p.clipHeight - p.trimTop - p.trimBottom);
  const bool whole = clipX == p.x && clipY == p.y && clipWidth == p.width &&
                     clipHeight == p.height;
  if (whole) {
    gVideoPanel.Clip(nullptr);
    return false;
  }
  const double raw = gRawPerView > 0 ? gRawPerView : 1.0;
  const winrt::Windows::Foundation::Rect shown{
      static_cast<float>(clipX / raw), static_cast<float>(clipY / raw),
      static_cast<float>(clipWidth / raw), static_cast<float>(clipHeight / raw)};
  RectangleGeometry clip;
  clip.Rect(aTransform.Inverse().TransformBounds(shown));
  gVideoPanel.Clip(clip);
  return true;
}

bool ApplyVideoClip() { return ApplyVideoClip(VideoTransform()); }

// How far the display turns what the app draws, clockwise, for the engine
// to turn video frames ahead of it (the DirectX samples' display rotation).
int32_t DisplayRotation() {
  using winrt::Windows::Graphics::Display::DisplayInformation;
  using winrt::Windows::Graphics::Display::DisplayOrientations;
  try {
    auto info = DisplayInformation::GetForCurrentView();
    const auto native = info.NativeOrientation();
    const auto current = info.CurrentOrientation();
    if (native == DisplayOrientations::Portrait) {
      switch (current) {
        case DisplayOrientations::Landscape:
          return 270;
        case DisplayOrientations::PortraitFlipped:
          return 180;
        case DisplayOrientations::LandscapeFlipped:
          return 90;
        default:
          return 0;
      }
    }
    switch (current) {
      case DisplayOrientations::Portrait:
        return 90;
      case DisplayOrientations::LandscapeFlipped:
        return 180;
      case DisplayOrientations::PortraitFlipped:
        return 270;
      default:
        return 0;
    }
  } catch (...) {
    return 0;
  }
}

std::atomic<void (*)(int32_t)> gVideoRotation{nullptr};

void TellVideoRotation() {
  if (auto tell = gVideoRotation.load()) {
    const int32_t degrees = DisplayRotation();
    tell(degrees);
    Log::Write(L"video layer: the display turns the picture " +
               std::to_wstring(degrees) + L" degrees");
  }
}

// The video panel above the browser's or under it, made exactly as in build
// 132's test, the one arrangement the display took as a hardware overlay:
// moved in the tree to just after the browser's panel, laid out at no size
// of its own (a swap chain shows from the panel's origin whatever its size),
// and nothing under it -- the content host's background taken away. Under
// the browser everything is put back.
void StackVideoPanel(bool above) {
  VideoPlacement& p = gVideoPlacement;
  if (!gVideoPanel || p.above == above) {
    return;
  }
  p.above = above;
  auto grid = gVideoPanel.Parent()
                  .try_as<winrt::Windows::UI::Xaml::Controls::Panel>();
  if (grid && gBrowserPanel) {
    uint32_t index = 0;
    if (grid.Children().IndexOf(gVideoPanel, index)) {
      grid.Children().RemoveAt(index);
    }
    uint32_t browser = 0;
    if (grid.Children().IndexOf(gBrowserPanel, browser)) {
      grid.Children().InsertAt(above ? browser + 1 : browser, gVideoPanel);
    }
  }
  if (above) {
    gVideoPanel.Width(std::numeric_limits<double>::quiet_NaN());
    gVideoPanel.Height(std::numeric_limits<double>::quiet_NaN());
    if (!p.host && grid) {
      p.host = grid.Parent()
                   .try_as<winrt::Windows::UI::Xaml::Controls::Border>();
    }
    if (p.host) {
      p.hostBackground = p.host.Background();
      p.host.Background(nullptr);
    }
  } else {
    if (p.chainWidth > 0 && p.chainHeight > 0) {
      gVideoPanel.Width(p.chainWidth);
      gVideoPanel.Height(p.chainHeight);
    }
    if (p.host && p.hostBackground) {
      p.host.Background(p.hostBackground);
    }
  }
}

// Where a finger is, in the coordinates ToFrame reads: the panel's while the
// GPU presents through it, whichever element the finger landed on. The image
// above the panel holds the layer of menus and dialogs, and it was taking the
// fingers over its rectangle once a menu had painted -- transparent or not --
// sized to the window as it was then and shown centred in the room. After the
// phone turned it was a band down the middle of the screen, and a point
// measured from its corner reached the page about 190 view pixels to the left
// of the finger: landscape taps landed on the wrong thing.
winrt::Windows::Foundation::Point FingerPoint(
    Input::PointerRoutedEventArgs const& args, SwapChainPanel const& panel,
    Image const& image) {
  if (g_panelPresenting.load() && panel) {
    return args.GetCurrentPoint(panel).Position();
  }
  return args.GetCurrentPoint(image).Position();
}
}  // namespace


EngineView::EngineView(int32_t pixelWidth, int32_t pixelHeight,
                       double rawPerView, bool withPanel)
    : fullWidth_(pixelWidth),
      fullHeight_(pixelHeight),
      rawPerView_(rawPerView > 0 ? rawPerView : 1.0) {
  // What the engine presents to when it draws on the GPU. Normally created
  // whether or not that succeeds: it stays empty and invisible under the
  // picture if the engine falls back to drawing in software, so there is no
  // mode to switch and no way for the two to disagree.
  //
  // Except on the launches that leave it out entirely. The window is hidden
  // by the shell at the engine's first paint with no GPU frame, no swap
  // chain and no EGL surface anywhere -- and this object is the one thing
  // this shell has that the builds which lived did not. It has been taken
  // out of the tree before and the window was hidden anyway; it has never
  // been not made.
  if (withPanel) {
    panel_ = SwapChainPanel();
    panel_.HorizontalAlignment(HorizontalAlignment::Stretch);
    panel_.VerticalAlignment(VerticalAlignment::Stretch);
    // Whether a video could have a display layer of its own; asks, changes
    // nothing ("overlay:" lines in the log). Only with verbose logs: it makes
    // a device and two swap chains of its own at launch.
    if (Log::Verbose()) {
      engine::ProbeVideoOverlay(panel_);
    }
    // Under the browser's panel: where a playing video goes, as a hardware
    // overlay, seen through a transparent hole the browser leaves for it.
    // Placed and sized by the engine; top-left, since its transform is
    // computed from the panel's origin, which is the browser panel's origin.
    video_panel_ = SwapChainPanel();
    video_panel_.HorizontalAlignment(HorizontalAlignment::Left);
    video_panel_.VerticalAlignment(VerticalAlignment::Top);
    video_panel_.IsHitTestVisible(false);
    video_panel_.Visibility(Visibility::Collapsed);
    gVideoPanel = video_panel_;
    gBrowserPanel = panel_;
    gRawPerView = rawPerView_;
  } else {
    panelWithheld_ = true;
  }

  try {
    gUiDispatcher =
        winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().Dispatcher();
  } catch (winrt::hresult_error const&) {
  }

  image_ = Image();
  // The engine paints a whole window; show all of it, keeping its shape. The
  // headless screen it was given has the window's aspect, so this costs at
  // most a small uniform scale.
  image_.Stretch(Stretch::Uniform);
  image_.HorizontalAlignment(HorizontalAlignment::Stretch);
  image_.VerticalAlignment(VerticalAlignment::Stretch);

  image_.PointerPressed([this](winrt::Windows::Foundation::IInspectable const&,
                               Input::PointerRoutedEventArgs const& args) {
    touched_ = true;
    OnPressed(args.Pointer().PointerId(), FingerPoint(args, panel_, image_));
    image_.CapturePointer(args.Pointer());
  });
  image_.PointerMoved([this](winrt::Windows::Foundation::IInspectable const&,
                             Input::PointerRoutedEventArgs const& args) {
    OnMoved(args.Pointer().PointerId(), FingerPoint(args, panel_, image_));
  });
  image_.PointerReleased([this](winrt::Windows::Foundation::IInspectable const&,
                                Input::PointerRoutedEventArgs const& args) {
    OnReleased(args.Pointer().PointerId(), FingerPoint(args, panel_, image_));
    image_.ReleasePointerCapture(args.Pointer());
  });
  image_.PointerCaptureLost(
      [this](winrt::Windows::Foundation::IInspectable const&,
             Input::PointerRoutedEventArgs const& args) {
        OnCaptureLost(args.Pointer().PointerId());
      });

  // On the hardware path the picture is the panel; the image above it shows
  // only the layer of menus and dialogs, and once the panel presents it takes
  // no fingers at all (PlaceLayer). The panel takes the same gestures; ToFrame
  // knows which one is showing.
  if (panel_) {
    panel_.PointerPressed([this](winrt::Windows::Foundation::IInspectable const&,
                                 Input::PointerRoutedEventArgs const& args) {
      touched_ = true;
      OnPressed(args.Pointer().PointerId(), args.GetCurrentPoint(panel_).Position());
      panel_.CapturePointer(args.Pointer());
    });
    panel_.PointerMoved([this](winrt::Windows::Foundation::IInspectable const&,
                               Input::PointerRoutedEventArgs const& args) {
      OnMoved(args.Pointer().PointerId(), args.GetCurrentPoint(panel_).Position());
    });
    panel_.PointerReleased([this](winrt::Windows::Foundation::IInspectable const&,
                                  Input::PointerRoutedEventArgs const& args) {
      OnReleased(args.Pointer().PointerId(), args.GetCurrentPoint(panel_).Position());
      panel_.ReleasePointerCapture(args.Pointer());
    });
    panel_.PointerCaptureLost(
        [this](winrt::Windows::Foundation::IInspectable const&,
               Input::PointerRoutedEventArgs const& args) {
          OnCaptureLost(args.Pointer().PointerId());
        });
  }


  WireKeyboard();
}

void EngineView::WireKeyboard() {
  // The keyboard types into this and nothing else. It is a real text control
  // because that is the only thing Windows will raise a keyboard for, and it
  // is invisible because the field the user is actually looking at is drawn by
  // Gecko, inside the picture.
  // Windows raises its keyboard for a focused text control and nothing else,
  // so there is one. It does no other work: it is the size of the content it
  // sits behind, because a control one pixel across is not something the
  // system treats as a place to type, and it is invisible because the field
  // the user is looking at is drawn by Gecko inside the picture.
  sink_ = TextBox();
  sink_.Opacity(0);
  sink_.HorizontalAlignment(HorizontalAlignment::Stretch);
  sink_.VerticalAlignment(VerticalAlignment::Stretch);
  // Nothing else is done to it. It was made non-hit-testable to stop taps in
  // the letterbox bands reaching it, but there are no bands any more -- the
  // window is the size of the room -- and the picture above it takes every
  // tap. That left an edit control the system had been told not to accept
  // input on, which is a candidate for why nothing typed arrives, so it is an
  // ordinary text box again.
  sink_.AcceptsReturn(false);
  sink_.IsSpellCheckEnabled(false);
  sink_.IsTextPredictionEnabled(false);

  // This is how the typing actually arrives. The on-screen keyboard of a phone
  // does not send characters to the window the way a physical one does: it
  // edits the focused text control directly, through the text services, and
  // the only sign of it is that the control's text changed. Listening for
  // characters on the window was the mistake -- the keyboard appeared, the
  // sink held focus, and nothing was ever delivered to listen for.
  //
  // So whatever lands here is passed on and the sink is emptied again, which
  // keeps it from growing and keeps the next change a pure delta.
  sink_.TextChanged([this](winrt::Windows::Foundation::IInspectable const&,
                           Controls::TextChangedEventArgs const&) {
    if (clearing_) {
      return;
    }
    auto typed = sink_.Text();
    Log::Write(L"view: the sink changed, " + std::to_wstring(typed.size()) +
               L" characters, engine " +
               std::wstring(text_ && typing_ ? L"told" : L"not told"));
    if (typed.empty()) {
      return;
    }
    if (text_ && typing_) {
      text_(reinterpret_cast<const uint16_t*>(typed.c_str()),
            static_cast<int32_t>(typed.size()));
    }
    clearing_ = true;
    sink_.Text(L"");
    clearing_ = false;
  });

  // There is no second way in. Characters were also being taken from the
  // window, kept on the grounds that a hardware keyboard would need it and
  // that it cost nothing -- both wrong. It cost every letter twice, because
  // the on-screen keyboard raises CharacterReceived as well as editing the
  // control, and a hardware keyboard needs nothing special either: it types
  // into whatever holds focus, which is this same sink, and arrives as the
  // same change.
  auto window = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread();

  window.KeyDown([this](winrt::Windows::UI::Core::CoreWindow const&,
                        winrt::Windows::UI::Core::KeyEventArgs const& args) {
    if (!key_ || !typing_) {
      return;
    }
    // Only the keys that produce no text, so there is nothing here that the
    // sink's own change also reports. Backspace on an empty sink changes
    // nothing, and Enter cannot, so neither is delivered twice.
    const int32_t code = static_cast<int32_t>(args.VirtualKey());
    switch (code) {
      case 8:   // Back
      case 9:   // Tab
      case 13:  // Enter
      case 27:  // Escape
      case 33:  // PageUp
      case 34:  // PageDown
      case 35:  // End
      case 36:  // Home
      case 37:  // Left
      case 38:  // Up
      case 39:  // Right
      case 40:  // Down
      case 46:  // Delete
        Log::Write(L"key: " + std::to_wstring(code) + L" sent to the engine");
        key_(code);
        args.Handled(true);
        break;
      default:
        break;
    }
  });

  // A dialog or a menu here has no title bar -- a headless window has no frame
  // for the system to draw one in -- so there is nothing on it to press to
  // close it. The About window could be opened and then never dismissed. The
  // phone's back button does it instead, as Escape, which is what closes a
  // dialog or a menu in Firefox.
  //
  // The rest of the time Back is the browser's while it has something to undo
  // -- a prompt over the page, fullscreen, the address bar being edited,
  // history in the tab, a tab a page opened -- and the phone's once it has
  // not: an unhandled Back puts the app in the background, which is what it
  // should do then. Handled must be decided before this returns, so nothing
  // here waits for the engine: the overlay the engine says itself, fullscreen
  // it has already told us, and the rest chrome sends whenever it changes
  // ("nav.state", client/DrmBridge.cpp). The press goes to chrome as
  // {"op":"nav.back"} (gecko_back_button in mobile-config-autoconfig.js).
  auto navigation =
      winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
  navigation.BackRequested(
      [this](winrt::Windows::Foundation::IInspectable const&,
             winrt::Windows::UI::Core::BackRequestedEventArgs const& args) {
        const bool overlay = key_ && overlay_ && overlay_() == 1;
        const bool fullscreen = gFullscreen.load();
        const bool armed = bridgeArmed_ && bridge_reply_ != nullptr;
        const bool chrome = armed && DrmBridge::ChromeTakesBack();
        bool keyboard = false;
        try {
          keyboard = InputPane::GetForCurrentView().Visible();
        } catch (winrt::hresult_error const&) {
        }
        const unsigned long long sinceKeyboard =
            ::GetTickCount64() - gKeyboardHiddenAt.load();
        const bool forKeyboard = keyboard || sinceKeyboard < 300;
        const wchar_t* branch =
            forKeyboard                ? L"the keyboard's"
            : overlay                  ? L"Escape to the menu or dialog"
            : chrome                   ? L"to chrome"
            : (fullscreen && key_)     ? L"Escape out of fullscreen"
                                       : L"the phone's";
        Log::Write(std::wstring(L"back: ") + branch + L" -- overlay " +
                   (overlay ? L"open" : L"none") + L", fullscreen " +
                   (fullscreen ? L"yes" : L"no") + L", chrome " +
                   (armed ? (chrome ? L"takes it" : L"has nothing")
                          : L"not listening") +
                   L", keyboard " +
                   (keyboard ? std::wstring(L"up")
                             : L"gone " + std::to_wstring(sinceKeyboard) +
                                   L" ms"));
        if (forKeyboard) {
          // Closing the keyboard is all this press was for. Not from inside
          // this system handler (see the Showing handler above).
          if (keyboard) {
            PostToUi([]() {
              try {
                InputPane::GetForCurrentView().TryHide();
              } catch (winrt::hresult_error const&) {
              }
            });
          }
          args.Handled(true);
          return;
        }
        if (overlay) {
          key_(27);  // Escape: the engine's rollup or the dialog's cancel
          args.Handled(true);
          return;
        }
        if (chrome) {
          bridge_reply_("{\"op\":\"nav.back\"}");
          args.Handled(true);
          return;
        }
        if (fullscreen && key_) {
          // Chrome not listening yet: Escape still leaves a page's
          // fullscreen.
          key_(27);
          args.Handled(true);
          return;
        }
      });

  // The keyboard takes the bottom of the screen away. Telling the engine makes
  // it lay the window out in what is left, so the field being typed into is
  // not underneath it.
  // The keyboard takes the bottom of the screen, so the room loses its bottom:
  // the container is made shorter and everything else follows from that. The
  // window was being resized directly before, which left the container at full
  // height with a shorter frame centred inside it -- a band of empty across
  // the top about a fifth of the screen deep, which is the other half of the
  // 878 pixels the keyboard took.
  //
  // A margin is in view pixels and so is the occluded rectangle, so no scale
  // comes into this at all.
  auto pane = InputPane::GetForCurrentView();
  pane.Showing([this](InputPane const&,
                      winrt::Windows::UI::ViewManagement::
                          InputPaneVisibilityEventArgs const& args) {
    if (!host_) {
      return;
    }
    const double covered = args.OccludedRect().Height;
    // The room is made smaller below, so the focused field is ours to keep in
    // view. Without this XAML also slid the whole app up to keep the sink --
    // which covers the page -- visible: the picture moved under the finger,
    // and on the way back the slide ran after the resize.
    args.EnsuredFocusedElementInView(true);
    Log::Write(L"view: keyboard covers " +
               std::to_wstring(static_cast<int>(covered)) +
               L" view px, taking it off the bottom of the room");
    // Not from in here. This is the input pane's own Showing handler, and
    // changing a margin inside it makes XAML lay the tree out again while the
    // pane is still animating itself into place. It is the last thing we do
    // from inside a system event handler, and it is the last thing in the log
    // before every one of these crashes. The work is posted instead.
    PostToUi([this, covered]() { host_.Margin(Thickness{0, 0, 0, covered}); });
  });
  pane.Hiding([this](InputPane const&,
                     winrt::Windows::UI::ViewManagement::
                         InputPaneVisibilityEventArgs const& args) {
    gKeyboardHiddenAt.store(::GetTickCount64());
    args.EnsuredFocusedElementInView(true);
    if (!host_) {
      return;
    }
    Log::Write(L"view: keyboard gone, the room is whole again");
    PostToUi([this]() { host_.Margin(Thickness{0, 0, 0, 0}); });
  });
}

void EngineView::ArmBridge() {
  if (bridgeArmed_) {
    return;
  }
  const ULONGLONG now = ::GetTickCount64();
  if (lastBridgeAttempt_ && now - lastBridgeAttempt_ < 1000) {
    return;
  }
  lastBridgeAttempt_ = now;
  HMODULE xul = ::GetModuleHandleW(L"xul.dll");
  if (!xul) {
    return;
  }
  if (!set_bridge_) {
    set_bridge_ = reinterpret_cast<SetBridgeSinkFn>(
        ::GetProcAddress(xul, "gecko_w10m_set_bridge_sink"));
    bridge_reply_ = reinterpret_cast<BridgeReplyFn>(
        ::GetProcAddress(xul, "gecko_w10m_bridge_reply"));
  }
  if (!set_bridge_ || !bridge_reply_) {
    return;
  }
  DrmBridge::SetReply(bridge_reply_);
  if (set_bridge_(&DrmBridge::OnMessage) == 1) {
    bridgeArmed_ = true;
    Log::Write(L"bridge: the shell is listening for chrome");
    // Chrome may have said what the back button would do before anyone was
    // listening; it is asked to say it again.
    bridge_reply_("{\"op\":\"nav.ask\"}");
  }
}

bool EngineView::Resolve() {
  ArmBridge();
  if (copy_) {
    return true;
  }
  // The engine starts on its own thread and may not be up yet, so this is
  // retried rather than done once. LoadPackagedLibrary on an already-loaded
  // module just returns it; it is also the only load an app container allows.
  // Once a second at most. This is called from the render callback, so at
  // display rate it was asking the loader for a module sixty times a second
  // for the whole of startup -- while the engine thread was inside the loader
  // lock bringing xul.dll up.
  const ULONGLONG now = ::GetTickCount64();
  if (lastResolveAttempt_ && now - lastResolveAttempt_ < 1000) {
    return false;
  }
  lastResolveAttempt_ = now;

  HMODULE xul = ::LoadPackagedLibrary(L"xul.dll", 0);
  if (!xul) {
    return false;
  }
  copy_ = reinterpret_cast<CopyFn>(::GetProcAddress(xul, "gecko_w10m_frame_copy"));
  mouse_ =
      reinterpret_cast<MouseFn>(::GetProcAddress(xul, "gecko_w10m_input_mouse"));
  wheel_ =
      reinterpret_cast<WheelFn>(::GetProcAddress(xul, "gecko_w10m_input_wheel"));
  wanted_ = reinterpret_cast<WantedFn>(
      ::GetProcAddress(xul, "gecko_w10m_text_input_wanted"));
  text_state_ = reinterpret_cast<TextStateFn>(
      ::GetProcAddress(xul, "gecko_w10m_text_input_state"));
  overlay_ = reinterpret_cast<OverlayFn>(
      ::GetProcAddress(xul, "gecko_w10m_overlay_open"));
  text_ = reinterpret_cast<TextFn>(::GetProcAddress(xul, "gecko_w10m_input_text"));
  key_ = reinterpret_cast<KeyFn>(::GetProcAddress(xul, "gecko_w10m_input_key"));
  resize_ =
      reinterpret_cast<ResizeFn>(::GetProcAddress(xul, "gecko_w10m_resize"));
  touch_ = reinterpret_cast<TouchFn>(::GetProcAddress(xul, "gecko_w10m_input_touch"));
  screen_fn_ =
      reinterpret_cast<ScreenFn>(::GetProcAddress(xul, "gecko_w10m_set_screen"));
  dpi_fn_ = reinterpret_cast<DpiFn>(::GetProcAddress(xul, "gecko_w10m_set_dpi"));
  open_url_ =
      reinterpret_cast<OpenUrlFn>(::GetProcAddress(xul, "gecko_w10m_open_url"));
  if (!set_launcher_) {
    set_launcher_ = reinterpret_cast<SetLauncherFn>(
        ::GetProcAddress(xul, "gecko_w10m_set_uri_launcher"));
    if (set_launcher_) {
      set_launcher_(&LaunchSystemUri);
      Log::Write(L"open: the engine can now hand system URIs to the shell");
    }
  }
  if (!set_fullscreen_) {
    set_fullscreen_ = reinterpret_cast<SetFullscreenSinkFn>(
        ::GetProcAddress(xul, "gecko_w10m_set_fullscreen_sink"));
    if (set_fullscreen_) {
      set_fullscreen_(&FullscreenChanged);
      Log::Write(L"view: the engine can now ask for the whole screen");
    }
  }
  // File dialogs and opening downloaded files (client/FileBridge.cpp).
  if (gUiDispatcher) {
    InstallFileBridge(xul, gUiDispatcher);
    // The phone's microphone consent (client/CaptureConsent.cpp).
    InstallCaptureConsent(xul, gUiDispatcher);
    // The phone's clipboard (client/ClipboardBridge.cpp).
    InstallClipboardBridge(xul, gUiDispatcher);
  }
  if (!set_video_layer_ && video_panel_) {
    set_video_layer_ = reinterpret_cast<SetVideoLayerSinkFn>(
        ::GetProcAddress(xul, "gecko_w10m_set_video_layer_sink"));
    if (set_video_layer_) {
      gVideoAck.store(reinterpret_cast<void (*)(uint32_t)>(
          ::GetProcAddress(xul, "gecko_w10m_video_layer_ack")));
      gVideoRotation.store(reinterpret_cast<void (*)(int32_t)>(
          ::GetProcAddress(xul, "gecko_w10m_video_layer_rotation")));
      // The display's turn now and whenever the phone is turned; asked on
      // the UI thread, which DisplayInformation belongs to.
      if (gUiDispatcher) {
        gUiDispatcher.RunAsync(
            winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [] {
              TellVideoRotation();
              try {
                winrt::Windows::Graphics::Display::DisplayInformation::
                    GetForCurrentView()
                        .OrientationChanged([](auto const&, auto const&) {
                          TellVideoRotation();
                        });
              } catch (...) {
              }
            });
      }
      static const VideoLayerSink sink{&VideoLayerAttach, &VideoLayerPlace,
                                       &VideoLayerShow, &VideoLayerStack};
      set_video_layer_(&sink);
      Log::Write(L"view: a playing video can go to the display as an overlay");
    }
  }
  if (screen_fn_ && screenWidth_ > 0) {
    screen_fn_(screenWidth_, screenHeight_);
  }
  if (dpi_fn_ && dpi_ > 0) {
    dpi_fn_(dpi_);
  }
  Log::Write(std::wstring(L"view: touch entry point ") +
             (touch_ ? L"found -- fingers go to APZ" : L"MISSING -- taps and wheel"));
  panel_fn_ =
      reinterpret_cast<PanelFn>(::GetProcAddress(xul, "gecko_w10m_set_panel"));
  // The other road to the panel: phones whose GPU stops at feature level 9_3
  // run software WebRender and composite it with D3D11, which presents to the
  // panel from xul rather than from ANGLE -- so ANGLE's note never comes.
  panel_presenting_fn_ = reinterpret_cast<PanelPresentingFn>(
      ::GetProcAddress(xul, "gecko_w10m_panel_presenting"));
  // The size goes to ANGLE rather than to the engine: ANGLE needs it on its
  // render thread, where XAML cannot be asked for anything, and xul cannot
  // pass it on because xul does not link against ANGLE -- EGL loads it at run
  // time. It may not be loaded yet, so this is retried like the rest.
  if (!panel_size_fn_) {
    if (HMODULE gles = ::LoadPackagedLibrary(L"libGLESv2.dll", 0)) {
      panel_size_fn_ = reinterpret_cast<PanelSizeFn>(
          ::GetProcAddress(gles, "angle_uwp_set_panel_size"));
      panel_scale_fn_ = reinterpret_cast<PanelScaleFn>(
          ::GetProcAddress(gles, "angle_uwp_set_panel_scale"));
      // And somewhere for it to say what it did. Its notes cannot go where
      // Gecko's do -- those live in xul, which ANGLE is not linked against --
      // so they come here, which is also where the crash trace lands, so the
      // two can be read against each other.
      auto install = reinterpret_cast<AngleLogFn>(
          ::GetProcAddress(gles, "angle_uwp_set_logger"));
      Log::Write(std::wstring(L"view: ANGLE size entry point ") +
                 (panel_size_fn_ ? L"found" : L"MISSING") +
                 L", logging entry point " + (install ? L"found" : L"MISSING"));
      // The mode goes in before the logger, so that ANGLE's own note about it
      // is the first thing it says once it can be heard.
      if (auto setMode = reinterpret_cast<AngleModeFn>(
              ::GetProcAddress(gles, "angle_uwp_set_mode"))) {
        setMode(experimentMode_);
      } else {
        Log::Write(L"view: ANGLE has no mode entry point -- an old engine");
      }
      if (install) {
        install([](const char* text) {
          if (!text) {
            return;
          }
          // ANGLE announcing that the panel has the swap chain is the only
          // notice the shell gets that frames have stopped coming through
          // memory. Nothing else changes: gecko_w10m_frame_copy simply stops
          // answering, which is indistinguishable from an engine that has not
          // drawn yet -- and the splash waits for a copied frame, so it would
          // have covered a working GPU path for ever.
          if (strstr(text, "took the swap chain")) {
            g_panelPresenting.store(true);
          } else if (!Log::Verbose() && !strstr(text, "fail") &&
                     !strstr(text, "FAIL") && !strstr(text, "error") &&
                     !strstr(text, "lost") && !strstr(text, "reset") &&
                     !strstr(text, "resized")) {
            // Quiet logs: ANGLE reports every texture and program it makes,
            // dozens a second while a page draws. Only trouble gets through.
            return;
          }
          std::wstring wide;
          wide.reserve(strlen(text));
          for (const char* p = text; *p; ++p) {
            wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
          }
          Log::Write(wide);
        });
      }
    }
  }
  if (copy_ && !reported_) {
    reported_ = true;
    Log::Write(L"view: engine frame buffer found");
  }
  return copy_ != nullptr;
}

void EngineView::EnsureBitmap(int32_t width, int32_t height) {
  if (bitmap_ && width_ == width && height_ == height) {
    return;
  }
  width_ = width;
  height_ = height;
  bitmap_ = WriteableBitmap(width, height);
  image_.Source(bitmap_);
  Log::Write(L"view: frame size " + std::to_wstring(width) + L"x" +
             std::to_wstring(height));
  PlaceLayer();
}

void EngineView::PlaceLayer() {
  if (!g_panelPresenting.load() || !panel_) {
    return;  // the software path: the image is the whole picture
  }
  if (!layerPlaced_) {
    layerPlaced_ = true;
    // Shown the way the panel shows the GPU's frame -- from its corner, one
    // device pixel a pixel -- not fitted into the room: the layer keeps the
    // size the window had when a menu last painted, and fitted it was
    // squeezed into the middle of the screen after the phone turned or the
    // keyboard came and went, with the menus drawn somewhere else than the
    // engine had them.
    image_.IsHitTestVisible(false);
    image_.Stretch(Stretch::Fill);
    image_.HorizontalAlignment(HorizontalAlignment::Left);
    image_.VerticalAlignment(VerticalAlignment::Top);
    Log::Write(L"view: the panel takes the fingers; the menu layer only "
               L"shows, from the panel's corner");
  }
  if (width_ > 0 && height_ > 0) {
    image_.Width(width_ / rawPerView_);
    image_.Height(height_ / rawPerView_);
  }
}

void EngineView::WatchRoom(
    winrt::Windows::UI::Xaml::FrameworkElement const& host) {
  // The room has to be measured on whatever holds the picture, never on the
  // picture. A stretched Image reports the size of what it drew, not of the
  // space it was given, so measuring it fed the frame's own size back to the
  // engine: the engine resized, the picture resized to match, and the two
  // chased each other down -- 1440, 1393, 1298, 810 -- with the log saying
  // "room is now" each time. The container's size is a fact about the screen
  // and does not move when the frame does.
  host_ = host;
  host_.SizeChanged([this](winrt::Windows::Foundation::IInspectable const&,
                           SizeChangedEventArgs const&) { PushSize(); });
  PushSize();
}

void EngineView::PostToUi(std::function<void()> work) {
  auto dispatcher =
      winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().Dispatcher();
  dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                      [work = std::move(work)]() { work(); });
}

void EngineView::GivePanelToEngine() {
  Resolve();
  if (!panel_ || fullWidth_ <= 0) {
    return;
  }
  if (panelWithheld_) {
    if (!panelGiven_) {
      panelGiven_ = true;
      Log::Write(L"view: the panel is WITHHELD from the engine this launch -- "
                 L"no EGL surface, no GPU frames, everything else as usual");
    }
    return;
  }
  if (panel_size_fn_) {
    panel_size_fn_(fullWidth_, fullHeight_);
  }
  if (panel_scale_fn_) {
    // The chain is in physical pixels; XAML would show them as logical ones,
    // scaled up by this, unless ANGLE undoes it on the chain.
    panel_scale_fn_(panel_.CompositionScaleX(), panel_.CompositionScaleY());
  }
  if (!panel_fn_) {
    return;
  }
  // The engine takes the panel as a bare COM pointer -- it is the shell that
  // knows it is XAML, and the engine only passes it on to ANGLE.
  auto inspectable = panel_.as<winrt::Windows::Foundation::IInspectable>();
  panel_fn_(winrt::get_abi(inspectable));
  if (!panelGiven_) {
    panelGiven_ = true;
    Log::Write(L"view: the engine has the panel, " +
               std::to_wstring(fullWidth_) + L"x" +
               std::to_wstring(fullHeight_) + L", size entry point " +
               std::wstring(panel_size_fn_ ? L"found" : L"missing"));
  }
}

void EngineView::PushSize() {
  if (!host_) {
    return;
  }
  const int32_t width =
      static_cast<int32_t>(host_.ActualWidth() * rawPerView_ + 0.5);
  const int32_t height =
      static_cast<int32_t>(host_.ActualHeight() * rawPerView_ + 0.5);
  if (width <= 0 || height <= 0) {
    return;
  }
  if (width == fullWidth_ && height == fullHeight_) {
    return;
  }
  fullWidth_ = width;
  fullHeight_ = height;

  // The panel goes first and goes every time. ANGLE needs its size on the
  // render thread, where XAML cannot be asked for anything, and it needs the
  // panel itself before EGL makes a surface -- which is early, so this cannot
  // wait for a resize that may never come.
  GivePanelToEngine();

  if (!resize_) {
    return;
  }
  Log::Write(L"view: room is now " + std::to_wstring(width) + L"x" +
             std::to_wstring(height) + L", telling the engine");
  resize_(width, height);
}

void EngineView::ApplyInputKind(int32_t kind) {
  if (kind == sinkKind_) {
    return;
  }
  sinkKind_ = kind;
  using winrt::Windows::UI::Xaml::Input::InputScope;
  using winrt::Windows::UI::Xaml::Input::InputScopeName;
  using winrt::Windows::UI::Xaml::Input::InputScopeNameValue;
  InputScopeNameValue value = InputScopeNameValue::Default;
  const wchar_t* name = L"text";
  switch (kind) {
    case 1: value = InputScopeNameValue::Password; name = L"password"; break;
    case 2: value = InputScopeNameValue::EmailSmtpAddress; name = L"e-mail"; break;
    case 3: value = InputScopeNameValue::Url; name = L"URL"; break;
    case 4: value = InputScopeNameValue::TelephoneNumber; name = L"telephone"; break;
    case 5: value = InputScopeNameValue::Number; name = L"number"; break;
    case 6: value = InputScopeNameValue::Search; name = L"search"; break;
    default: break;
  }
  InputScope scope;
  InputScopeName scopeName;
  scopeName.NameValue(value);
  scope.Names().Append(scopeName);
  sink_.InputScope(scope);
  Log::Write(std::wstring(L"view: the field is ") + name +
             L", the keyboard follows");
}

void EngineView::FollowTextInput() {
  if (!wanted_) {
    return;
  }
  const bool wants = wanted_() == 1;
  const uint32_t state = text_state_ ? text_state_() : 0;
  const uint32_t serial = state >> 8;
  // A field newly focused, or tapped while it has the focus, is someone about
  // to type -- even when the engine wanted text all along and the keyboard was
  // put away. Before this the keyboard only came up when "wanted" changed, so
  // a field tapped a second time after the keyboard was dismissed got none.
  const bool again = wants && (serial != lastTextSerial_ || forceKeyboard_);
  forceKeyboard_ = false;
  if (wants == typing_ && !again) {
    return;
  }
  lastTextSerial_ = serial;
  typing_ = wants;
  if (wants) {
    // Someone may be about to paste: what the phone's clipboard holds now.
    ClipboardRefresh(L"a field took the focus");
  }

  auto pane = InputPane::GetForCurrentView();
  if (wants) {
    // Not before the screen has been touched. Firefox puts the caret in the
    // address bar as it starts, so the keyboard came up on its own every
    // launch -- which no phone browser should do, and which is also the one
    // event present in every crash there has been. Waiting for a real touch
    // makes the launch behave and makes the experiment: if the browser lives
    // when the keyboard never appears, the keyboard is the trigger, and we
    // will at last see whether the GPU path draws anything.
    if (!touched_) {
      typing_ = false;  // ask again once there has been a touch
      declinedUntilTouch_ = true;
      if (!saidWaiting_) {
        saidWaiting_ = true;
        Log::Write(L"view: the engine wants text, but nothing has been touched "
                   L"yet -- not raising the keyboard");
      }
      return;
    }
    ApplyInputKind(static_cast<int32_t>(state & 0xff));
    const bool focused = sink_.Focus(FocusState::Programmatic);
    // Focus alone raises the keyboard only when the focus came from a touch,
    // and this one came from Gecko, so ask outright as well.
    const bool shown = pane.TryShow();
    Log::Write(L"view: engine asked for text input, sink focus " +
               std::wstring(focused ? L"taken" : L"refused") + L", pane " +
               std::wstring(shown ? L"shown" : L"refused"));
  } else {
    Log::Write(L"view: engine no longer wants text input");
    pane.TryHide();
    // TryHide does not always come back as a Hiding event; check for real a
    // moment later.
    PostToUi([this]() { SyncKeyboardMargin(); });
  }
}

// Gecko calls this from its main thread; Launcher wants the UI thread, so
// the work is posted to the dispatcher captured when the view was built.
void EngineView::LaunchSystemUri(const char* utf8) {
  if (!utf8 || !*utf8 || !gUiDispatcher) {
    return;
  }
  const int size =
      ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  if (size <= 1) {
    return;
  }
  std::wstring wide(static_cast<size_t>(size) - 1, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), size);

  gUiDispatcher.RunAsync(
      winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
      [wide]() {
        try {
          winrt::Windows::Foundation::Uri uri(winrt::hstring{wide});
          winrt::Windows::System::Launcher::LaunchUriAsync(uri);
          Log::Write(L"open: asked the system to open " + wide);
        } catch (winrt::hresult_error const& error) {
          Log::Write(L"open: the system refused " + wide,
                     std::wstring(error.message()));
        }
      });
}

void EngineView::VideoLayerAttach(void* surface) {
  if (!gUiDispatcher) {
    return;
  }
  gUiDispatcher.RunAsync(
      winrt::Windows::UI::Core::CoreDispatcherPriority::High, [surface]() {
        if (!gVideoPanel) {
          return;
        }
        winrt::com_ptr<ISwapChainPanelNative2> native2;
        HRESULT hr = winrt::get_unknown(gVideoPanel)
                         ->QueryInterface(__uuidof(ISwapChainPanelNative2),
                                          native2.put_void());
        if (SUCCEEDED(hr)) {
          hr = native2->SetSwapChainHandle(static_cast<HANDLE>(surface));
        }
        Log::Write(std::wstring(L"video layer: the panel ") +
                   (SUCCEEDED(hr) ? (surface ? L"took the video's surface"
                                             : L"let the video's surface go")
                                  : L"REFUSED the video's surface"));
      });
}

void EngineView::VideoLayerPlace(uint32_t generation, int32_t x, int32_t y,
                                 int32_t width, int32_t height, int32_t clipX,
                                 int32_t clipY, int32_t clipWidth,
                                 int32_t clipHeight, int32_t chainWidth,
                                 int32_t chainHeight, int32_t rotation) {
  if (!gUiDispatcher || width <= 0 || height <= 0 || chainWidth <= 0 ||
      chainHeight <= 0) {
    return;
  }
  gUiDispatcher.RunAsync(
      winrt::Windows::UI::Core::CoreDispatcherPriority::High,
      [=]() {
        if (!gVideoPanel) {
          return;
        }
        const double raw = gRawPerView > 0 ? gRawPerView : 1.0;
        if (!gVideoPlacement.above) {
          gVideoPanel.Width(chainWidth);
          gVideoPanel.Height(chainHeight);
        }
        VideoPlacement& placement = gVideoPlacement;
        placement.x = x;
        placement.y = y;
        placement.width = width;
        placement.height = height;
        placement.clipX = clipX;
        placement.clipY = clipY;
        placement.clipWidth = clipWidth;
        placement.clipHeight = clipHeight;
        placement.chainWidth = chainWidth;
        placement.chainHeight = chainHeight;
        placement.rotation = rotation;
        CompositeTransform transform = VideoTransform();
        gVideoPanel.RenderTransform(transform);
        const double scaleX = transform.ScaleX();
        const double scaleY = transform.ScaleY();
        const bool whole = !ApplyVideoClip(transform);
        static int sPlacementNotes = 0;
        if (sPlacementNotes < 40) {
          ++sPlacementNotes;
          Log::Write(L"video layer: placed " + std::to_wstring(generation) +
                     L" -- chain " + std::to_wstring(chainWidth) + L"x" +
                     std::to_wstring(chainHeight) + L" scaled " +
                     std::to_wstring(scaleX) + L"x" + std::to_wstring(scaleY) +
                     L" at " + std::to_wstring(x / raw) + L"," +
                     std::to_wstring(y / raw) + L" view px, turned back " +
                     std::to_wstring((360 - rotation) % 360) +
                     (whole ? L", no clip" : L", clipped"));
        }
        if (auto ack = gVideoAck.load()) {
          ack(generation);
        }
      });
}

void EngineView::VideoLayerStack(int32_t above, int32_t trimTop,
                                 int32_t trimBottom, int32_t trimLeft,
                                 int32_t trimRight) {
  if (!gUiDispatcher) {
    return;
  }
  gUiDispatcher.RunAsync(
      winrt::Windows::UI::Core::CoreDispatcherPriority::High,
      [above, trimTop, trimBottom, trimLeft, trimRight]() {
        // The display scans the video out as a hardware overlay only while
        // its panel lies above the browser's (build 132's test); the engine
        // asks for that while nothing of the page is in front of the video
        // but a band along an edge (a player's progress bar), which is left
        // out of the panel so it shows, and for under the browser again as
        // soon as more is.
        if (gVideoPanel) {
          VideoPlacement& p = gVideoPlacement;
          p.trimTop = above ? trimTop : 0;
          p.trimBottom = above ? trimBottom : 0;
          p.trimLeft = above ? trimLeft : 0;
          p.trimRight = above ? trimRight : 0;
          ApplyVideoClip();
          StackVideoPanel(above != 0);
        }
      });
}

void EngineView::VideoLayerShow(int32_t visible) {
  if (!gUiDispatcher) {
    return;
  }
  gUiDispatcher.RunAsync(
      winrt::Windows::UI::Core::CoreDispatcherPriority::High, [visible]() {
        if (gVideoPanel) {
          gVideoPanel.Visibility(visible ? Visibility::Visible
                                         : Visibility::Collapsed);
          Log::Write(visible ? L"video layer: shown" : L"video layer: hidden");
        }
      });
}

void EngineView::FullscreenChanged(int32_t on) {
  gFullscreen.store(on != 0);
  if (!gUiDispatcher) {
    return;
  }
  const bool wanted = on != 0;
  gUiDispatcher.RunAsync(
      winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [wanted]() {
        // Both bars go. The engine has already resized its window to the whole
        // display by the time this runs -- that is the point of doing it in
        // this order -- so what happens here only catches the screen up with a
        // window that is already the right size, and the page is never handed
        // a resize it has to answer.
        try {
          if (ApiInformation::IsTypePresent(
                  L"Windows.UI.ViewManagement.StatusBar")) {
            auto status = StatusBar::GetForCurrentView();
            if (wanted) {
              status.HideAsync();
            } else {
              status.ShowAsync();
            }
          }
        } catch (winrt::hresult_error const& error) {
          Log::Write(L"fullscreen: the status bar would not move",
                     std::wstring(error.message()));
        }
        try {
          auto view = winrt::Windows::UI::ViewManagement::ApplicationView::
              GetForCurrentView();
          if (wanted) {
            // Standard overlay mode is what makes the navigation bar hide
            // itself and come back on a swipe -- as an overlay, over the
            // picture, without the window changing size underneath it. That is
            // what keeps a fullscreen player fullscreen while someone reaches
            // for the back button.
            try {
              view.FullScreenSystemOverlayMode(
                  winrt::Windows::UI::ViewManagement::
                      FullScreenSystemOverlayMode::Standard);
            } catch (winrt::hresult_error const&) {
            }
            const bool took = view.TryEnterFullScreenMode();
            Log::Write(took ? L"fullscreen: on, the whole screen is ours"
                            : L"fullscreen: on, but the view kept its bars");
          } else {
            view.ExitFullScreenMode();
            Log::Write(L"fullscreen: off");
          }
        } catch (winrt::hresult_error const& error) {
          Log::Write(L"fullscreen: the view refused",
                     std::wstring(error.message()));
        }
      });
}

void EngineView::OpenUrl(std::wstring_view url) {
  if (url.empty()) {
    return;
  }
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, url.data(),
                                         static_cast<int>(url.size()), nullptr,
                                         0, nullptr, nullptr);
  std::string utf8(size > 0 ? size : 0, '\0');
  if (size > 0) {
    ::WideCharToMultiByte(CP_UTF8, 0, url.data(), static_cast<int>(url.size()),
                          utf8.data(), size, nullptr, nullptr);
  }
  pendingUrl_ = std::move(utf8);
  lastOpenAttempt_ = 0;
  Log::Write(L"open: the phone handed us " + std::wstring(url));
}

void EngineView::SyncKeyboardMargin() {
  if (!host_) {
    return;
  }
  double covered = 0;
  try {
    auto pane = InputPane::GetForCurrentView();
    if (pane.Visible()) {
      covered = pane.OccludedRect().Height;
    }
  } catch (winrt::hresult_error const&) {
    covered = 0;
  }
  const auto current = host_.Margin();
  if (std::abs(current.Bottom - covered) < 0.5) {
    return;
  }
  Log::Write(L"view: keyboard margin was " +
             std::to_wstring(static_cast<int>(current.Bottom)) +
             L" view px, the keyboard actually covers " +
             std::to_wstring(static_cast<int>(covered)) + L" -- fixing the room");
  host_.Margin(Thickness{0, 0, 0, covered});
}

void EngineView::Tick() {
  // Counted before anything can return early: this is the proof that the UI
  // thread is still drawing, and the heartbeat reports it from a thread of its
  // own. A count that stops while the pulse goes on says the UI thread died
  // without the process.
  engine::NoteUiFrame();

  if (!g_panelPresenting.load() && panel_presenting_fn_ &&
      panel_presenting_fn_()) {
    Log::Write(L"view: the D3D11 compositor has the panel");
    g_panelPresenting.store(true);
  }

  // The hardware path presents on its own and never fills the buffer the
  // splash is waiting for, so the splash has to be told separately.
  if (g_panelPresenting.load() && firstFrame_) {
    auto handler = std::move(firstFrame_);
    firstFrame_ = nullptr;
    Log::Write(L"view: the GPU is presenting through the panel, splash down");
    handler();
  }
  if (g_panelPresenting.load() && !layerPlaced_) {
    PlaceLayer();
  }

  if (!Resolve()) {
    return;
  }

  // A URL from outside waits here for a browser window to exist. The engine
  // answers 0 while it has none, so this simply keeps asking.
  if (!pendingUrl_.empty() && open_url_) {
    const unsigned long long now = ::GetTickCount64();
    if (!lastOpenAttempt_ || now - lastOpenAttempt_ >= 1000) {
      lastOpenAttempt_ = now;
      if (open_url_(pendingUrl_.c_str()) == 1) {
        Log::Write(L"open: the engine took the URL");
        pendingUrl_.clear();
      }
    }
  }
  // Taking focus and raising the keyboard are not things to do from in here.
  // This runs inside CompositionTarget::Rendering -- XAML's own render pass --
  // and Focus() and InputPane::TryShow() re-enter focus, layout and the input
  // host while the frame is still in flight. The crash lands about a second
  // after the keyboard appears, which is the wrong place to be clever.
  //
  // So the render pass only notices; the work is posted and happens on a
  // later turn of the message loop, when XAML is between frames.
  // declinedUntilTouch_ closes a hole this check had: when the keyboard is
  // held back, typing_ stays false while the engine goes on wanting text, so
  // the condition below stayed true and posted to the dispatcher on every
  // single frame -- sixty queued turns a second, for ever.
  if (tapKeyboardCheckAt_ && ::GetTickCount64() >= tapKeyboardCheckAt_) {
    // A tap a moment ago; if it left a field focused and no keyboard up,
    // the keyboard comes up now. The pause lets a tap elsewhere blur the field
    // first, so it does not flash up for a tap that moved the focus away.
    tapKeyboardCheckAt_ = 0;
    if (wanted_ && wanted_() == 1) {
      bool visible = false;
      try {
        visible = InputPane::GetForCurrentView().Visible();
      } catch (winrt::hresult_error const&) {
      }
      if (!visible) {
        forceKeyboard_ = true;
      }
    }
  }
  const bool newField = text_state_ && wanted_ && wanted_() == 1 &&
                        (text_state_() >> 8) != lastTextSerial_;
  if (wanted_ &&
      ((wanted_() == 1) != typing_ || newField || forceKeyboard_) &&
      !textInputPending_ && !(declinedUntilTouch_ && !touched_)) {
    textInputPending_ = true;
    PostToUi([this]() {
      textInputPending_ = false;
      FollowTextInput();
    });
  }

  // Ask with the serial we last drew. An unchanged engine answers zero without
  // copying anything, so an idle page costs a lock and a comparison.
  // The engine is told the shape of the bitmap and answers with the shape of
  // the frame; it copies only when the two agree.
  int32_t width = width_;
  int32_t height = height_;
  uint64_t serial = seen_;

  if (!bitmap_) {
    // Nothing to copy into yet; the call still reports the size to build one.
    width = 0;
    height = 0;
    copy_(nullptr, 0, &width, &height, nullptr);
    if (width > 0 && height > 0) {
      EnsureBitmap(width, height);
    }
    return;
  }

  uint8_t* pixels = nullptr;
  auto access = bitmap_.PixelBuffer()
                    .as<::Windows::Storage::Streams::IBufferByteAccess>();
  if (FAILED(access->Buffer(&pixels)) || !pixels) {
    return;
  }

  const int32_t capacity = width_ * height_ * 4;
  if (copy_(pixels, capacity, &width, &height, &serial) == 1) {
    seen_ = serial;
    bitmap_.Invalidate();
    if (firstFrame_) {
      auto handler = std::move(firstFrame_);
      firstFrame_ = nullptr;
      handler();

      // The engine could not be told a size before it existed, so the first
      // frame is when the room it actually has is handed over.
      PushSize();
    }
    return;
  }

  // Zero with a size that is not ours means the engine resized its window.
  if (width > 0 && height > 0 && (width != width_ || height != height_)) {
    EnsureBitmap(width, height);
  }
}

bool EngineView::ToFrame(winrt::Windows::Foundation::Point const& point,
                         int32_t* x, int32_t* y) const {
  if (g_panelPresenting.load() && panel_) {
    // The panel shows the frame at one physical pixel per frame pixel, and
    // its points arrive in view pixels.
    const double fx = point.X * rawPerView_;
    const double fy = point.Y * rawPerView_;
    // A finger that slides off the edge is still a finger; APZ wants to hear
    // where it went, so the point is clamped rather than refused.
    *x = static_cast<int32_t>(std::clamp(fx, 0.0, double(fullWidth_ - 1)));
    *y = static_cast<int32_t>(std::clamp(fy, 0.0, double(fullHeight_ - 1)));
    return fullWidth_ > 0 && fullHeight_ > 0;
  }
  if (width_ <= 0 || height_ <= 0) {
    return false;
  }
  const double actualW = image_.ActualWidth();
  const double actualH = image_.ActualHeight();
  if (actualW <= 0 || actualH <= 0) {
    return false;
  }
  // Uniform means the frame is centred in whatever space it was given, so the
  // bands on either side of it are not part of the window and a point in them
  // belongs to nothing.
  const double scale = (std::min)(actualW / width_, actualH / height_);
  if (scale <= 0) {
    return false;
  }
  const double offsetX = (actualW - width_ * scale) / 2;
  const double offsetY = (actualH - height_ * scale) / 2;
  const double fx = (point.X - offsetX) / scale;
  const double fy = (point.Y - offsetY) / scale;
  if (fx < 0 || fy < 0 || fx >= width_ || fy >= height_) {
    return false;
  }
  *x = static_cast<int32_t>(fx);
  *y = static_cast<int32_t>(fy);
  return true;
}

EngineView::Finger* EngineView::FindFinger(uint32_t id) {
  for (auto& f : fingers_) {
    if (f.id == id) {
      return &f;
    }
  }
  return nullptr;
}

void EngineView::OnPressed(uint32_t id,
                           winrt::Windows::Foundation::Point const& point) {
  int32_t x = 0;
  int32_t y = 0;
  if (!ToFrame(point, &x, &y)) {
    return;
  }
  if (touch_) {
    // A second finger is a second touch point, not the first one jumping:
    // the engine collects them by id into one touch event, and APZ makes a
    // pinch of two.
    if (Finger* f = FindFinger(id)) {
      f->x = x;
      f->y = y;
    } else {
      if (!fingers_.empty()) {
        std::wstring held;
        for (const Finger& other : fingers_) {
          held += L" " + std::to_wstring(other.id);
        }
        Log::Write(L"touch: finger " + std::to_wstring(id) +
                   L" down while the shell holds" + held);
      }
      fingers_.push_back({id, x, y, x, y, ::GetTickCount64()});
    }
    touch_(static_cast<int32_t>(id), 0, x, y);
    return;
  }
  if (pressed_) {
    return;  // the wheel-and-mouse fallback follows one finger only
  }
  pressed_ = true;
  pressedId_ = id;
  travelled_ = 0;
  lastX_ = point.X;
  lastY_ = point.Y;
}

void EngineView::OnMoved(uint32_t id,
                         winrt::Windows::Foundation::Point const& point) {
  int32_t x = 0;
  int32_t y = 0;
  if (!ToFrame(point, &x, &y)) {
    return;
  }

  if (touch_) {
    Finger* f = FindFinger(id);
    if (!f) {
      return;
    }
    // The finger itself; APZ turns its path into a pan, a fling or a pinch.
    //
    // Only when it has actually gone somewhere. A digitizer reports a finger
    // resting on the glass over and over, and every one of those reports was
    // becoming a touchmove: the log shows six to nine of them for a tap that
    // moved nothing at all. A real touchscreen sends none, and a player that
    // reads the touch stream itself -- YouTube's does, in fullscreen, which is
    // why swipes work there and taps do not -- takes any touchmove as "this is
    // a drag, not a tap". Three device pixels is a little over one CSS pixel
    // here, far below anything a person means as a movement.
    if (std::abs(x - f->x) + std::abs(y - f->y) < 3) {
      return;
    }
    f->x = x;
    f->y = y;
    touch_(static_cast<int32_t>(id), 1, x, y);
    return;
  }

  if (!pressed_ || id != pressedId_ || !wheel_) {
    return;
  }
  const double dx = point.X - lastX_;
  const double dy = point.Y - lastY_;
  lastX_ = point.X;
  lastY_ = point.Y;
  travelled_ += std::abs(dx) + std::abs(dy);
  // HeadlessWidget negates what it is given, so a finger moving up -- a
  // negative delta -- becomes a positive wheel delta, which is scrolling down.
  wheel_(x, y, dx, dy);
}

void EngineView::OnReleased(uint32_t id,
                            winrt::Windows::Foundation::Point const& point) {
  if (touch_) {
    Finger* f = FindFinger(id);
    if (!f) {
      return;
    }
    const Finger last = *f;
    fingers_.erase(fingers_.begin() + (f - fingers_.data()));
    int32_t x = 0;
    int32_t y = 0;
    if (!ToFrame(point, &x, &y)) {
      touch_(static_cast<int32_t>(id), 3, last.x, last.y);  // off the picture
      return;
    }
    touch_(static_cast<int32_t>(id), 2, x, y);
    if (std::abs(x - last.startX) + std::abs(y - last.startY) < 20) {
      // A tap. Where the finger was, in the frame's pixels -- the engine's
      // "tap:" line says where the page took it.
      Log::Write(L"touch: tap at " + std::to_wstring(x) + L"," +
                 std::to_wstring(y) + L" frame px (" +
                 std::to_wstring(static_cast<int>(point.X)) + L"," +
                 std::to_wstring(static_cast<int>(point.Y)) + L" view px), id " +
                 std::to_wstring(id) + L", " +
                 std::to_wstring(::GetTickCount64() - last.downAt) +
                 L" ms on the glass, " + std::to_wstring(fingers_.size()) +
                 L" other finger(s) held");
      tapKeyboardCheckAt_ = ::GetTickCount64() + 300;
    }
    return;
  }

  if (!pressed_ || id != pressedId_) {
    return;
  }
  pressed_ = false;
  if (!mouse_) {
    return;
  }
  int32_t x = 0;
  int32_t y = 0;
  if (!ToFrame(point, &x, &y)) {
    return;
  }
  // A finger never holds still; anything under a few pixels was meant as a tap.
  if (travelled_ > 12) {
    return;
  }
  mouse_(0, x, y);
  mouse_(1, x, y);
  mouse_(2, x, y);
}

void EngineView::OnCaptureLost(uint32_t id) {
  // After a release this finger is already gone. Otherwise something took the
  // pointer away mid-gesture, and the engine must hear that the touch ended,
  // or it keeps a finger on the glass that no longer exists.
  if (Finger* f = FindFinger(id)) {
    const Finger last = *f;
    fingers_.erase(fingers_.begin() + (f - fingers_.data()));
    if (touch_) {
      touch_(static_cast<int32_t>(id), 3, last.x, last.y);
    }
  }
  if (pressed_ && id == pressedId_) {
    pressed_ = false;
  }
}

void EngineView::SetScreen(double viewWidth, double viewHeight) {
  const int32_t width = static_cast<int32_t>(viewWidth * rawPerView_ + 0.5);
  const int32_t height = static_cast<int32_t>(viewHeight * rawPerView_ + 0.5);
  if (width <= 0 || height <= 0 ||
      (width == screenWidth_ && height == screenHeight_)) {
    return;
  }
  screenWidth_ = width;
  screenHeight_ = height;
  if (screen_fn_) {
    screen_fn_(width, height);
  }
}

void EngineView::SetDpi(double dpi) {
  const int32_t value = static_cast<int32_t>(dpi + 0.5);
  if (value <= 0 || value == dpi_) {
    return;
  }
  dpi_ = value;
  Log::Write(L"view: the display is " + std::to_wstring(value) + L" dpi" +
             (dpi_fn_ ? L", telling the engine now"
                      : L", the engine is not up yet"));
  if (dpi_fn_) {
    dpi_fn_(dpi_);
  }
}

void EngineView::Start() {
  if (tick_.value) {
    return;
  }
  tick_ = CompositionTarget::Rendering(
      [this](winrt::Windows::Foundation::IInspectable const&,
             winrt::Windows::Foundation::IInspectable const&) { Tick(); });
}

void EngineView::Stop() {
  if (tick_.value) {
    CompositionTarget::Rendering(tick_);
    tick_ = {};
  }
}

}  // namespace gecko_w10m::client
