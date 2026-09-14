// GeckoEngine.cpp
#include "pch.h"
#include "GeckoEngine.h"

#include <string>
#include <vector>

namespace gecko_w10m::engine {

namespace {

std::string ToUtf8(std::wstring_view w) {
  if (w.empty()) return {};
  int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr,
                                  0, nullptr, nullptr);
  std::string out(len, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), len,
                        nullptr, nullptr);
  return out;
}

std::wstring FromUtf8(const char* s) {
  if (!s || !*s) return {};
  int len = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring out(len ? len - 1 : 0, L'\0');
  if (len) ::MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), len);
  return out;
}

}  // namespace

// ---- Session ---------------------------------------------------------------

Session::Session(gecko_session* raw) : raw_(raw) {}

Session::~Session() {
  if (raw_) gecko_session_close(raw_);
}

void Session::LoadUri(std::wstring_view uri) {
  auto u = ToUtf8(uri);
  gecko_session_load_uri(raw_, u.c_str());
}
void Session::Reload() { gecko_session_reload(raw_); }
void Session::Stop() { gecko_session_stop(raw_); }
void Session::GoBack() { gecko_session_go_back(raw_); }
void Session::GoForward() { gecko_session_go_forward(raw_); }
bool Session::CanGoBack() const { return gecko_session_can_go_back(raw_) != 0; }
bool Session::CanGoForward() const {
  return gecko_session_can_go_forward(raw_) != 0;
}

void Session::SetSurface(void* swapChain, int width, int height, float scale) {
  gecko_session_set_surface(raw_, swapChain, width, height, scale);
}
void Session::Resize(int width, int height, float scale) {
  gecko_session_resize(raw_, width, height, scale);
}

void Session::Observe(SessionObserver observer) {
  observer_ = std::make_unique<SessionObserver>(std::move(observer));
  InstallDelegate();
}

void Session::InstallDelegate() {
  delegate_ = {};
  delegate_.user_data = observer_.get();
  delegate_.on_location_change = [](void* u, const char* uri) {
    auto* o = static_cast<SessionObserver*>(u);
    if (o && o->LocationChanged) o->LocationChanged(FromUtf8(uri));
  };
  delegate_.on_title_change = [](void* u, const char* title) {
    auto* o = static_cast<SessionObserver*>(u);
    if (o && o->TitleChanged) o->TitleChanged(FromUtf8(title));
  };
  delegate_.on_progress = [](void* u, float p) {
    auto* o = static_cast<SessionObserver*>(u);
    if (o && o->ProgressChanged) o->ProgressChanged(p);
  };
  delegate_.on_can_go_back = [](void* u, int32_t c) {
    auto* o = static_cast<SessionObserver*>(u);
    if (o && o->CanGoBackChanged) o->CanGoBackChanged(c != 0);
  };
  delegate_.on_can_go_forward = [](void* u, int32_t c) {
    auto* o = static_cast<SessionObserver*>(u);
    if (o && o->CanGoForwardChanged) o->CanGoForwardChanged(c != 0);
  };
  delegate_.on_new_session = nullptr;
  gecko_session_set_delegate(raw_, &delegate_);
}

// ---- Runtime ---------------------------------------------------------------

Runtime::Runtime(gecko_runtime* raw) : raw_(raw) {}

Runtime::~Runtime() {
  if (raw_) gecko_runtime_shutdown(raw_);
}

std::shared_ptr<Runtime> Runtime::Create(std::wstring_view profileDir,
                                         bool jitEnabled, int dpi) {
  auto profile = ToUtf8(profileDir);
  gecko_runtime_config cfg{};
  cfg.profile_dir = profile.c_str();
  cfg.jit_enabled = jitEnabled ? 1 : 0;
  cfg.device_dpi = dpi;
  gecko_runtime* raw = gecko_runtime_create(&cfg);
  if (!raw) return nullptr;
  return std::shared_ptr<Runtime>(new Runtime(raw));
}

std::shared_ptr<Session> Runtime::CreateSession(bool isPrivate) {
  gecko_session* s = gecko_session_create(raw_, isPrivate ? 1 : 0);
  if (!s) return nullptr;
  return std::make_shared<Session>(s);
}

int Runtime::Pump() { return gecko_runtime_pump(raw_); }

}  // namespace gecko_w10m::engine
