// EngineView.h -- carries what Gecko painted to the screen.
//
// There is no window for web content on Windows 10 Mobile: an app container
// gets no HWND it can hand to a foreign engine, so Gecko runs headless and its
// compositor paints into a buffer instead of a surface. This is the other end
// of that buffer. It owns a XAML Image whose bitmap is refilled whenever the
// engine reports a newer frame.
//
// The two entry points come from the loaded xul.dll by name rather than by
// linking: the shell is built before the engine and has to keep working when
// the engine is not there at all.
#pragma once

#include <cstdint>

#include "winrt/Windows.UI.Xaml.Controls.h"
#include "winrt/Windows.UI.Xaml.Media.Imaging.h"

namespace gecko_w10m::client {

class EngineView {
 public:
  EngineView();

  winrt::Windows::UI::Xaml::Controls::Image Surface() const { return image_; }

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

  using CopyFn = int32_t (*)(void* dest, int32_t capacity, int32_t* width,
                             int32_t* height, uint64_t* serial);
  using MouseFn = void (*)(int32_t message, int32_t x, int32_t y);
  using WheelFn = void (*)(int32_t x, int32_t y, double dx, double dy);

  CopyFn copy_ = nullptr;
  MouseFn mouse_ = nullptr;
  WheelFn wheel_ = nullptr;

  bool pressed_ = false;
  double lastX_ = 0;
  double lastY_ = 0;
  double travelled_ = 0;

  winrt::Windows::UI::Xaml::Controls::Image image_{nullptr};
  winrt::Windows::UI::Xaml::Media::Imaging::WriteableBitmap bitmap_{nullptr};
  winrt::event_token tick_{};

  uint64_t seen_ = 0;
  int32_t width_ = 0;
  int32_t height_ = 0;
  bool reported_ = false;
};

}  // namespace gecko_w10m::client
