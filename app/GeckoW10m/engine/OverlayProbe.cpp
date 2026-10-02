// OverlayProbe.cpp — can this phone show a video without the GPU drawing it?
//
// Fullscreen 1080p60 costs the compositor 22-29 ms a frame, and almost all of
// it is one thing: the video, sampled from NV12 and scaled to 2560x1440 by the
// 3D engine, with the system compositor then drawing the result a second time.
// The way out is the one every phone's own player takes: hand the video to
// the display as a layer of its own -- a hardware overlay plane, scanned out
// and scaled by the display controller, not drawn by anyone.
//
// Whether that is possible here is a set of questions only the phone can
// answer, and this asks them once, at launch, without changing anything:
//
//   * does the display output exist for this app, and does it support
//     overlays at all, and for which formats (NV12, YUY2, BGRA);
//   * is there a video processor for NV12 to BGRA with scaling -- the engine
//     that would do the conversion if the display cannot take NV12;
//   * can a swap chain for a composition surface handle be made, in NV12 and
//     in BGRA (IDXGIFactoryMedia, which is what the system's own media
//     element uses);
//   * will a XAML SwapChainPanel take such a handle (ISwapChainPanelNative2).
//
// If the display has no overlays, a second layer would only give the system
// compositor more to draw, and the plan is dropped.
#include "pch.h"

#include "OverlayProbe.h"

#include <d3d11.h>
#include <dxgi1_3.h>
#include <windows.ui.xaml.media.dxinterop.h>

#include <string>
#include <thread>

#include "../client/Log.h"

namespace gecko_w10m::engine {
namespace {

using gecko_w10m::client::Log;

// IDXGIFactoryMedia: the SDK declares it for desktop apps only, but the
// interface is the system's and an app container may ask for it.
MIDL_INTERFACE("41e7d1f2-a591-4f7b-a2e5-fa9c843e1c12")
IGeckoW10mFactoryMedia : public IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE CreateSwapChainForCompositionSurfaceHandle(
      IUnknown* device, HANDLE surface, const DXGI_SWAP_CHAIN_DESC1* desc,
      IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  CreateDecodeSwapChainForCompositionSurfaceHandle(
      IUnknown* device, HANDLE surface, void* desc, IUnknown* yuvDecodeBuffers,
      IDXGIOutput* restrictToOutput, IUnknown** swapChain) = 0;
};

constexpr DWORD kCompositionObjectAllAccess = 0x0003;  // COMPOSITIONOBJECT_ALL_ACCESS
constexpr UINT kSwapChainFlagYuvVideo = 64;              // DXGI_SWAP_CHAIN_FLAG_YUV_VIDEO

std::wstring Hex(uint32_t value) {
  wchar_t text[16];
  swprintf_s(text, L"0x%08x", value);
  return text;
}

std::wstring OverlayFlags(UINT flags) {
  if (!flags) {
    return L"none";
  }
  std::wstring out;
  if (flags & 1) out += L"direct ";     // DXGI_OVERLAY_SUPPORT_FLAG_DIRECT
  if (flags & 2) out += L"scaling ";    // DXGI_OVERLAY_SUPPORT_FLAG_SCALING
  if (flags & ~3u) out += Hex(flags);
  return out;
}

void TrySwapChain(IGeckoW10mFactoryMedia* media, ID3D11Device* device,
                  HRESULT(WINAPI* createHandle)(DWORD, SECURITY_ATTRIBUTES*,
                                                 HANDLE*),
                  DXGI_FORMAT format, UINT flags, const wchar_t* name) {
  HANDLE surface = nullptr;
  HRESULT hr = createHandle(kCompositionObjectAllAccess, nullptr, &surface);
  if (FAILED(hr) || !surface) {
    Log::Write(std::wstring(L"overlay: no composition surface handle for ") +
               name + L", " + Hex(hr));
    return;
  }
  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = 1920;
  desc.Height = 1080;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = flags;
  IDXGISwapChain1* chain = nullptr;
  hr = media->CreateSwapChainForCompositionSurfaceHandle(device, surface, &desc,
                                                         nullptr, &chain);
  Log::Write(std::wstring(L"overlay: a ") + name +
             L" swap chain for a composition surface: " +
             (SUCCEEDED(hr) ? std::wstring(L"made") : L"refused, " + Hex(hr)));
  if (chain) {
    chain->Release();
  }
  ::CloseHandle(surface);
}

void ProbeDevice() {
  HMODULE d3d11 = ::LoadLibraryExW(L"d3d11.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
  using CreateFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE,
                                    UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                    ID3D11Device**, D3D_FEATURE_LEVEL*,
                                    ID3D11DeviceContext**);
  auto create = d3d11 ? reinterpret_cast<CreateFn>(
                            ::GetProcAddress(d3d11, "D3D11CreateDevice"))
                      : nullptr;
  if (!create) {
    Log::Write(L"overlay: no D3D11CreateDevice");
    return;
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0,
                                      D3D_FEATURE_LEVEL_9_3};
  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> context;
  HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                      D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                          D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                      levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                      device.put(), nullptr, context.put());
  if (FAILED(hr)) {
    Log::Write(L"overlay: no video-capable device, " + Hex(hr));
    return;
  }

  // The display, and what it can lay over the picture by itself.
  winrt::com_ptr<IDXGIDevice> dxgi = device.as<IDXGIDevice>();
  winrt::com_ptr<IDXGIAdapter> adapter;
  dxgi->GetAdapter(adapter.put());
  UINT outputs = 0;
  for (;; ++outputs) {
    winrt::com_ptr<IDXGIOutput> output;
    if (adapter->EnumOutputs(outputs, output.put()) != S_OK) {
      break;
    }
    if (outputs > 0) {
      continue;
    }
    auto output2 = output.try_as<IDXGIOutput2>();
    Log::Write(std::wstring(L"overlay: output 0 says overlays are ") +
               (output2 ? (output2->SupportsOverlays() ? L"SUPPORTED"
                                                       : L"not supported")
                        : L"unknown (no IDXGIOutput2)"));
    if (auto output3 = output.try_as<IDXGIOutput3>()) {
      const struct {
        DXGI_FORMAT format;
        const wchar_t* name;
      } formats[] = {{DXGI_FORMAT_NV12, L"NV12"},
                     {DXGI_FORMAT_YUY2, L"YUY2"},
                     {DXGI_FORMAT_B8G8R8A8_UNORM, L"BGRA"}};
      for (const auto& f : formats) {
        UINT flags = 0;
        hr = output3->CheckOverlaySupport(f.format, device.get(), &flags);
        Log::Write(std::wstring(L"overlay: ") + f.name + L" -- " +
                   (SUCCEEDED(hr) ? OverlayFlags(flags) : L"error " + Hex(hr)));
      }
    }
  }
  Log::WriteNum(L"overlay: display outputs visible to the app", outputs);

  // The video engine: NV12 in, BGRA out, 1920x1080 scaled to 2560x1440.
  if (auto video = device.try_as<ID3D11VideoDevice>()) {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = 1920;
    desc.InputHeight = 1080;
    desc.OutputWidth = 2560;
    desc.OutputHeight = 1440;
    desc.InputFrameRate = {60, 1};
    desc.OutputFrameRate = {60, 1};
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> processors;
    hr = video->CreateVideoProcessorEnumerator(&desc, processors.put());
    if (SUCCEEDED(hr)) {
      UINT in = 0;
      UINT out = 0;
      processors->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &in);
      processors->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &out);
      Log::Write(std::wstring(L"overlay: video processor -- NV12 ") +
                 ((in & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ? L"in"
                                                                    : L"NOT in") +
                 L", BGRA " +
                 ((out & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) ? L"out"
                                                                      : L"NOT out"));
    } else {
      Log::Write(L"overlay: no video processor for 1080p to 1440p, " + Hex(hr));
    }
  } else {
    Log::Write(L"overlay: the device has no video interface");
  }

  // A swap chain the system compositor can take as a layer of its own.
  winrt::com_ptr<IDXGIFactory2> factory;
  adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());
  winrt::com_ptr<IGeckoW10mFactoryMedia> media;
  if (factory) {
    factory->QueryInterface(__uuidof(IGeckoW10mFactoryMedia), media.put_void());
  }
  HMODULE dcomp = ::LoadLibraryExW(L"dcomp.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
  using HandleFn = HRESULT(WINAPI*)(DWORD, SECURITY_ATTRIBUTES*, HANDLE*);
  auto createHandle =
      dcomp ? reinterpret_cast<HandleFn>(
                  ::GetProcAddress(dcomp, "DCompositionCreateSurfaceHandle"))
            : nullptr;
  Log::Write(std::wstring(L"overlay: media swap chains ") +
             (media ? L"available" : L"NOT available") +
             L", composition surface handles " +
             (createHandle ? L"available"
                           : (dcomp ? L"NOT available (no export)"
                                    : L"NOT available (no dcomp.dll)")));
  if (media && createHandle) {
    TrySwapChain(media.get(), device.get(), createHandle, DXGI_FORMAT_NV12,
                 kSwapChainFlagYuvVideo, L"NV12");
    TrySwapChain(media.get(), device.get(), createHandle,
                 DXGI_FORMAT_B8G8R8A8_UNORM, 0, L"BGRA");
  }
}

}  // namespace

void ProbeVideoOverlay(
    winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel) {
  // The panel answers on the UI thread, where this is called.
  if (panel) {
    winrt::com_ptr<ISwapChainPanelNative2> native2;
    HRESULT hr = winrt::get_unknown(panel)->QueryInterface(
        __uuidof(ISwapChainPanelNative2), native2.put_void());
    Log::Write(std::wstring(L"overlay: the panel ") +
               (SUCCEEDED(hr) ? L"takes composition surface handles"
                              : L"takes swap chains only"));
  }
  // The rest makes a device of its own; off the UI thread.
  std::thread([] {
    try {
      ProbeDevice();
    } catch (...) {
      Log::Write(L"overlay: the probe threw");
    }
  }).detach();
}

}  // namespace gecko_w10m::engine
