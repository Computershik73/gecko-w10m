// EngineView.h -- carries what Gecko painted to the screen, and what the
// phone did back to Gecko.
//
// There is no window for web content on Windows 10 Mobile: an app container
// gets no HWND it can hand to a foreign engine, so Gecko runs headless and its
// compositor paints into a buffer instead of a surface. This is the other end
// of that buffer, and the near end of touch and typing.
//
// The entry points come from the loaded xul.dll by name rather than by
// linking: the shell is built before the engine and has to keep working when
// the engine is not there at all.
#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "winrt/Windows.UI.Xaml.Controls.h"
#include "winrt/Windows.UI.Xaml.Media.Imaging.h"
#include "winrt/Windows.UI.ViewManagement.h"

namespace gecko_w10m::client {

class EngineView {
 public:
  // The window size in physical pixels and the number of them Windows puts in
  // a view pixel -- the same two numbers the engine was started with, because
  // the keyboard is measured in one and the window in the other.
  // withPanel false makes no SwapChainPanel at all: the one structural thing
  // this shell has that the builds which lived did not.
  EngineView(int32_t pixelWidth, int32_t pixelHeight, double rawPerView,
             bool withPanel);

  winrt::Windows::UI::Xaml::Controls::Image Surface() const { return image_; }
  // What the engine presents to when it is drawing on the GPU. It sits under
  // the picture and stays empty while the software path is in use, so whichever
  // of the two produces a frame is the one that shows.
  winrt::Windows::UI::Xaml::Controls::SwapChainPanel Panel() const {
    return panel_;
  }
  // Hands the panel to the engine. Must happen before the engine starts, since
  // EGL asks for it as soon as it makes a surface.
  void GivePanelToEngine();
  // The display in view pixels, as the window sees it; sent on to the engine
  // in device pixels so the headless screen turns with the phone.
  void SetScreen(double viewWidth, double viewHeight);
  // The display's density. APZ measures how far a finger may wander and still
  // count as a tap against this; left to itself it assumes a 96 dpi monitor
  // and lets a tap move nine pixels, which on this screen is a fifth of a
  // millimetre -- so ordinary taps became tiny pans and never arrived.
  void SetDpi(double dpi);
  // Makes the room match the keyboard as it actually is. The pane's Hiding
  // event is not delivered when the keyboard goes away with the app -- a
  // suspend with it open, a focus change the shell never sees -- and the
  // room then stayed short, with the browser in the top half of the screen
  // and the placeholder showing under it.
  void SyncKeyboardMargin();
  // Hands a URL to the engine as soon as it can take one. Safe to call before
  // the engine has started.
  void OpenUrl(std::wstring_view url);
  // Handed to the engine so it can open mailto:, tel: and ms-settings: URIs
  // through the system. Static because the engine keeps a plain pointer.
  static void LaunchSystemUri(const char* utf8);
  // Handed to the engine the same way: it says when a page goes fullscreen,
  // and the shell takes the status bar and the navigation bar off the screen.
  // The engine never resizes itself -- the room this frees up comes back to it
  // through the ordinary resize, so the window and the swap chain change
  // shape together.
  static void FullscreenChanged(int32_t on);
  // Which half of the swap-chain hand-over this launch leaves out, if any:
  // 0 nothing, 1 the panel never gets the chain, 2 the chain is never
  // presented. Passed on to ANGLE as soon as it can be reached.
  void SetExperimentMode(int mode) { experimentMode_ = mode; }
  // Whether the engine is given the panel at all. Without it ANGLE has no
  // window, no EGL surface is made, and WebRender cannot draw on the GPU --
  // while every other thing the engine does at first paint still happens.
  void SetPanelWithheld(bool withheld) { panelWithheld_ = withheld; }
  // Invisible, and focused only when Gecko says something takes text. It is
  // what the on-screen keyboard types into and the only way its keys can be
  // caught at all.
  winrt::Windows::UI::Xaml::Controls::TextBox TextSink() const {
    return sink_;
  }

  // Begins watching for frames. Cheap when the engine is idle: one lock and a
  // comparison per display refresh, no copy.
  // The element that holds the picture. Its size is the room the window has,
  // and it is watched for as long as the view lives.
  void WatchRoom(winrt::Windows::UI::Xaml::FrameworkElement const& host);

  void Start();
  void Stop();

  // Called once, when the engine has drawn something. It is the only
  // evidence that a start succeeded, so it is what takes the splash down
  // and what clears the failed-attempt count.
  void OnFirstFrame(std::function<void()> handler) {
    firstFrame_ = std::move(handler);
  }

 private:
  void Tick();
  bool Resolve();
  void EnsureBitmap(int32_t width, int32_t height);

  // A drag scrolls and a tap clicks, which is what a phone means by touch. The
  // engine is told in the pixels of the frame it drew, so every point has to
  // come back through the letterbox the image is shown in.
  void OnPressed(winrt::Windows::Foundation::Point const& point);
  void OnMoved(winrt::Windows::Foundation::Point const& point);
  void OnReleased(winrt::Windows::Foundation::Point const& point);
  bool ToFrame(winrt::Windows::Foundation::Point const& point, int32_t* x,
               int32_t* y) const;

  void FollowTextInput();
  // Tells the engine how much room the picture has, whenever that changes.
  void PushSize();
  // Runs work on a later turn of the UI loop, never inside the handler that
  // asked for it.
  static void PostToUi(std::function<void()> work);
  void WireKeyboard();

  using CopyFn = int32_t (*)(void* dest, int32_t capacity, int32_t* width,
                             int32_t* height, uint64_t* serial);
  using MouseFn = void (*)(int32_t message, int32_t x, int32_t y);
  using WheelFn = void (*)(int32_t x, int32_t y, double dx, double dy);
  using WantedFn = int32_t (*)();
  using OverlayFn = int32_t (*)();
  using TextFn = void (*)(const uint16_t* text, int32_t length);
  using KeyFn = void (*)(int32_t keyCode);
  using ResizeFn = void (*)(int32_t width, int32_t height);
  using TouchFn = void (*)(int32_t pointerId, int32_t state, int32_t x, int32_t y);
  using ScreenFn = void (*)(int32_t width, int32_t height);
  using DpiFn = void (*)(int32_t dpi);
  using OpenUrlFn = int32_t (*)(const char* url);
  using SetLauncherFn = void (*)(void (*)(const char*));
  using SetFullscreenSinkFn = void (*)(void (*)(int32_t));
  using SetBridgeSinkFn = int32_t (*)(void (*)(const char*));
  using BridgeReplyFn = void (*)(const char*);
  using PanelFn = void (*)(void* panel);
  using PanelPresentingFn = int32_t (*)();
  using PanelSizeFn = void (*)(int32_t width, int32_t height);
  using PanelScaleFn = void (*)(float x, float y);
  using AngleLogFn = void (*)(void (*)(const char*));
  using AngleModeFn = void (*)(int32_t);

  CopyFn copy_ = nullptr;
  MouseFn mouse_ = nullptr;
  WheelFn wheel_ = nullptr;
  WantedFn wanted_ = nullptr;
  OverlayFn overlay_ = nullptr;
  TextFn text_ = nullptr;
  KeyFn key_ = nullptr;
  ResizeFn resize_ = nullptr;
  TouchFn touch_ = nullptr;
  ScreenFn screen_fn_ = nullptr;
  DpiFn dpi_fn_ = nullptr;
  int32_t dpi_ = 0;
  OpenUrlFn open_url_ = nullptr;
  SetLauncherFn set_launcher_ = nullptr;
  SetFullscreenSinkFn set_fullscreen_ = nullptr;
  // The chrome-to-shell message bridge (client/DrmBridge). Armed once the
  // engine's main thread is up, which is later than the exports resolve.
  SetBridgeSinkFn set_bridge_ = nullptr;
  BridgeReplyFn bridge_reply_ = nullptr;
  bool bridgeArmed_ = false;
  unsigned long long lastBridgeAttempt_ = 0;
  void ArmBridge();
  // A URL the phone handed us -- from a tap on a link in another app, or from
  // this being the browser it opens links with. Kept until the engine has a
  // window to put it in.
  std::string pendingUrl_;
  unsigned long long lastOpenAttempt_ = 0;
  // Where the last touch move we actually sent was, so a finger resting on the
  // glass stops producing them.
  int32_t lastSentTouchX_ = 0;
  int32_t lastSentTouchY_ = 0;
  int32_t screenWidth_ = 0;
  int32_t screenHeight_ = 0;
  PanelFn panel_fn_ = nullptr;
  PanelPresentingFn panel_presenting_fn_ = nullptr;
  PanelSizeFn panel_size_fn_ = nullptr;
  PanelScaleFn panel_scale_fn_ = nullptr;

  winrt::Windows::UI::Xaml::Controls::Image image_{nullptr};
  winrt::Windows::UI::Xaml::Controls::SwapChainPanel panel_{nullptr};
  winrt::Windows::UI::Xaml::FrameworkElement host_{nullptr};
  winrt::Windows::UI::Xaml::Controls::TextBox sink_{nullptr};
  winrt::Windows::UI::Xaml::Media::Imaging::WriteableBitmap bitmap_{nullptr};
  winrt::event_token tick_{};

  uint64_t seen_ = 0;
  int32_t width_ = 0;
  int32_t height_ = 0;
  bool reported_ = false;
  bool panelGiven_ = false;
  bool textInputPending_ = false;
  bool touched_ = false;
  bool declinedUntilTouch_ = false;
  int experimentMode_ = 0;
  bool panelWithheld_ = false;
  bool saidWaiting_ = false;
  unsigned long long lastResolveAttempt_ = 0;
  std::function<void()> firstFrame_;

  bool pressed_ = false;
  double lastX_ = 0;
  double lastY_ = 0;
  double travelled_ = 0;

  bool typing_ = false;    // text input is wanted right now
  bool clearing_ = false;  // emptying the sink, so ignore its own change
  int32_t fullWidth_ = 0;
  int32_t fullHeight_ = 0;
  double rawPerView_ = 1.0;
};

}  // namespace gecko_w10m::client
