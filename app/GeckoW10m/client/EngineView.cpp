#include "pch.h"

#include "client/EngineView.h"

#include <robuffer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "client/Log.h"
#include "winrt/Windows.UI.Core.h"
#include "winrt/Windows.UI.Input.h"
#include "winrt/Windows.UI.Xaml.Input.h"
#include "winrt/Windows.UI.Xaml.Media.h"

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
namespace Input = winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Media::Imaging;
using winrt::Windows::UI::ViewManagement::InputPane;

namespace gecko_w10m::client {

EngineView::EngineView(int32_t pixelWidth, int32_t pixelHeight,
                       double rawPerView)
    : fullWidth_(pixelWidth),
      fullHeight_(pixelHeight),
      rawPerView_(rawPerView > 0 ? rawPerView : 1.0) {
  // What the engine presents to when it draws on the GPU. It is created
  // whether or not that succeeds: it stays empty and invisible under the
  // picture if the engine falls back to drawing in software, so there is no
  // mode to switch and no way for the two to disagree.
  panel_ = SwapChainPanel();
  panel_.HorizontalAlignment(HorizontalAlignment::Stretch);
  panel_.VerticalAlignment(VerticalAlignment::Stretch);

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
    OnPressed(args.GetCurrentPoint(image_).Position());
    image_.CapturePointer(args.Pointer());
  });
  image_.PointerMoved([this](winrt::Windows::Foundation::IInspectable const&,
                             Input::PointerRoutedEventArgs const& args) {
    OnMoved(args.GetCurrentPoint(image_).Position());
  });
  image_.PointerReleased([this](winrt::Windows::Foundation::IInspectable const&,
                                Input::PointerRoutedEventArgs const& args) {
    OnReleased(args.GetCurrentPoint(image_).Position());
    image_.ReleasePointerCapture(args.Pointer());
  });
  image_.PointerCaptureLost(
      [this](winrt::Windows::Foundation::IInspectable const&,
             Input::PointerRoutedEventArgs const&) { pressed_ = false; });


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
  // Only while something is actually open. Swallowing back the rest of the
  // time would take away the way out of the app.
  auto navigation =
      winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
  navigation.BackRequested(
      [this](winrt::Windows::Foundation::IInspectable const&,
             winrt::Windows::UI::Core::BackRequestedEventArgs const& args) {
        if (!key_ || !overlay_ || overlay_() != 1) {
          return;
        }
        key_(27);  // Escape
        args.Handled(true);
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
                         InputPaneVisibilityEventArgs const&) {
    if (!host_) {
      return;
    }
    Log::Write(L"view: keyboard gone, the room is whole again");
    PostToUi([this]() { host_.Margin(Thickness{0, 0, 0, 0}); });
  });
}

bool EngineView::Resolve() {
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
  overlay_ = reinterpret_cast<OverlayFn>(
      ::GetProcAddress(xul, "gecko_w10m_overlay_open"));
  text_ = reinterpret_cast<TextFn>(::GetProcAddress(xul, "gecko_w10m_input_text"));
  key_ = reinterpret_cast<KeyFn>(::GetProcAddress(xul, "gecko_w10m_input_key"));
  resize_ =
      reinterpret_cast<ResizeFn>(::GetProcAddress(xul, "gecko_w10m_resize"));
  panel_fn_ =
      reinterpret_cast<PanelFn>(::GetProcAddress(xul, "gecko_w10m_set_panel"));
  // The size goes to ANGLE rather than to the engine: ANGLE needs it on its
  // render thread, where XAML cannot be asked for anything, and xul cannot
  // pass it on because xul does not link against ANGLE -- EGL loads it at run
  // time. It may not be loaded yet, so this is retried like the rest.
  if (!panel_size_fn_) {
    if (HMODULE gles = ::LoadPackagedLibrary(L"libGLESv2.dll", 0)) {
      panel_size_fn_ = reinterpret_cast<PanelSizeFn>(
          ::GetProcAddress(gles, "angle_uwp_set_panel_size"));
      // And somewhere for it to say what it did. Its notes cannot go where
      // Gecko's do -- those live in xul, which ANGLE is not linked against --
      // so they come here, which is also where the crash trace lands, so the
      // two can be read against each other.
      auto install = reinterpret_cast<AngleLogFn>(
          ::GetProcAddress(gles, "angle_uwp_set_logger"));
      Log::Write(std::wstring(L"view: ANGLE size entry point ") +
                 (panel_size_fn_ ? L"found" : L"MISSING") +
                 L", logging entry point " + (install ? L"found" : L"MISSING"));
      if (install) {
        install([](const char* text) {
          if (!text) {
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
  if (panel_size_fn_) {
    panel_size_fn_(fullWidth_, fullHeight_);
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

void EngineView::FollowTextInput() {
  if (!wanted_) {
    return;
  }
  const bool wants = wanted_() == 1;
  if (wants == typing_) {
    return;
  }
  typing_ = wants;

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
      if (!saidWaiting_) {
        saidWaiting_ = true;
        Log::Write(L"view: the engine wants text, but nothing has been touched "
                   L"yet -- not raising the keyboard");
      }
      return;
    }
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
  }
}

void EngineView::Tick() {
  if (!Resolve()) {
    return;
  }
  // Taking focus and raising the keyboard are not things to do from in here.
  // This runs inside CompositionTarget::Rendering -- XAML's own render pass --
  // and Focus() and InputPane::TryShow() re-enter focus, layout and the input
  // host while the frame is still in flight. The crash lands about a second
  // after the keyboard appears, which is the wrong place to be clever.
  //
  // So the render pass only notices; the work is posted and happens on a
  // later turn of the message loop, when XAML is between frames.
  if (wanted_ && (wanted_() == 1) != typing_ && !textInputPending_) {
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

void EngineView::OnPressed(winrt::Windows::Foundation::Point const& point) {
  int32_t x = 0;
  int32_t y = 0;
  if (!ToFrame(point, &x, &y)) {
    return;
  }
  pressed_ = true;
  travelled_ = 0;
  lastX_ = point.X;
  lastY_ = point.Y;
}

void EngineView::OnMoved(winrt::Windows::Foundation::Point const& point) {
  if (!pressed_ || !wheel_) {
    return;
  }
  int32_t x = 0;
  int32_t y = 0;
  if (!ToFrame(point, &x, &y)) {
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

void EngineView::OnReleased(winrt::Windows::Foundation::Point const& point) {
  const bool wasPressed = pressed_;
  pressed_ = false;
  if (!wasPressed || !mouse_) {
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
