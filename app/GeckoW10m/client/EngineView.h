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
  // Invisible, and focused only when Gecko says something takes text. It is
  // what the on-screen keyboard types into and the only way its keys can be
  // caught at all.
  winrt::Windows::UI::Xaml::Controls::TextBox TextSink() const {
    return sink_;
  }

  // Begins watching for frames. Cheap when the engine is idle: one lock and a
  // comparison per display refresh, no copy.
  void Start();
  void Stop();

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
  void WireKeyboard();

  using CopyFn = int32_t (*)(void* dest, int32_t capacity, int32_t* width,
                             int32_t* height, uint64_t* serial);
  using MouseFn = void (*)(int32_t message, int32_t x, int32_t y);
  using WheelFn = void (*)(int32_t x, int32_t y, double dx, double dy);
  using WantedFn = int32_t (*)();
  using TextFn = void (*)(const uint16_t* text, int32_t length);
  using KeyFn = void (*)(int32_t keyCode);
  using ResizeFn = void (*)(int32_t width, int32_t height);

  CopyFn copy_ = nullptr;
  MouseFn mouse_ = nullptr;
  WheelFn wheel_ = nullptr;
  WantedFn wanted_ = nullptr;
  TextFn text_ = nullptr;
  KeyFn key_ = nullptr;
  ResizeFn resize_ = nullptr;

  winrt::Windows::UI::Xaml::Controls::Image image_{nullptr};
  winrt::Windows::UI::Xaml::Controls::TextBox sink_{nullptr};
  winrt::Windows::UI::Xaml::Media::Imaging::WriteableBitmap bitmap_{nullptr};
  winrt::event_token tick_{};

  uint64_t seen_ = 0;
  int32_t width_ = 0;
  int32_t height_ = 0;
  bool reported_ = false;

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
