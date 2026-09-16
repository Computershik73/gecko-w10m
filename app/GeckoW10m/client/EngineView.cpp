#include "pch.h"

#include "client/EngineView.h"

#include <robuffer.h>

#include "client/Log.h"
#include "winrt/Windows.UI.Xaml.Media.h"

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Media::Imaging;

namespace gecko_w10m::client {

EngineView::EngineView() {
  image_ = Image();
  // The engine paints a whole window; show all of it, keeping its shape. The
  // headless screen it was given has the window's aspect, so this costs at
  // most a small uniform scale.
  image_.Stretch(Stretch::Uniform);
  image_.HorizontalAlignment(HorizontalAlignment::Stretch);
  image_.VerticalAlignment(VerticalAlignment::Stretch);
}

bool EngineView::Resolve() {
  if (copy_) {
    return true;
  }
  // The engine starts on its own thread and may not be up yet, so this is
  // retried rather than done once. LoadPackagedLibrary on an already-loaded
  // module just returns it; it is also the only load an app container allows.
  HMODULE xul = ::LoadPackagedLibrary(L"xul.dll", 0);
  if (!xul) {
    return false;
  }
  copy_ = reinterpret_cast<CopyFn>(::GetProcAddress(xul, "gecko_w10m_frame_copy"));
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

void EngineView::Tick() {
  if (!Resolve()) {
    return;
  }

  // Ask with the serial we last drew. An unchanged engine answers zero without
  // copying anything, so an idle page costs a lock and a comparison.
  int32_t width = 0;
  int32_t height = 0;
  uint64_t serial = seen_;

  if (!bitmap_) {
    // Nothing to copy into yet; the call still reports the size to build one.
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
    return;
  }

  // Zero with a size that is not ours means the engine resized its window.
  if (width > 0 && height > 0 && (width != width_ || height != height_)) {
    EnsureBitmap(width, height);
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
