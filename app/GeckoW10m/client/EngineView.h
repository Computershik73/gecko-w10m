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
  EngineView(int32_t pixelWidth, int32_t pixelHeight, double rawPerView);

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
  using PanelFn = void (*)(void* panel);
  using PanelSizeFn = void (*)(int32_t width, int32_t height);

  CopyFn copy_ = nullptr;
  MouseFn mouse_ = nullptr;
  WheelFn wheel_ = nullptr;
  WantedFn wanted_ = nullptr;
  OverlayFn overlay_ = nullptr;
  TextFn text_ = nullptr;
  KeyFn key_ = nullptr;
  ResizeFn resize_ = nullptr;
  PanelFn panel_fn_ = nullptr;
  PanelSizeFn panel_size_fn_ = nullptr;

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
