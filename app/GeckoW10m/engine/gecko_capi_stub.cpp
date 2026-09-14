/* gecko_capi_stub.cpp
 *
 * Placeholder implementation of the Gecko C ABI so the UWP shell builds and
 * runs before the real arm-uwp engine is cross-compiled.
 *
 * It also performs a real JIT self-test on startup: allocate a page with
 * VirtualAllocFromApp, write a Thumb function into it, flip it to
 * PAGE_EXECUTE_READ with VirtualProtectFromApp, flush the I-cache, and call it.
 * If that returns 42, the `codeGeneration` capability is live on this device
 * and the full Gecko JIT will work. The result is surfaced to the shell via the
 * first title-change callback.
 *
 * Link this OR the real engine, never both. See GeckoW10m.vcxproj (GECKO_W10M_USE_ENGINE_STUB).
 */
#include "gecko_capi.h"

#include <windows.h>
#include <memoryapi.h>
#include <processthreadsapi.h>

#include <cstring>
#include <string>

namespace {

// Runs the JIT probe. Returns true if executable memory works end-to-end.
bool ProbeJit(std::string& detail) {
  // Thumb: movs r0, #42 ; bx lr   ->  2A 20 70 47
  const unsigned char code[] = {0x2A, 0x20, 0x70, 0x47};

  void* page = ::VirtualAllocFromApp(nullptr, sizeof(code),
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!page) {
    detail = "VirtualAllocFromApp(PAGE_READWRITE) failed (err " +
             std::to_string(::GetLastError()) + ")";
    return false;
  }

  std::memcpy(page, code, sizeof(code));

  ULONG oldProtect = 0;
  if (!::VirtualProtectFromApp(page, sizeof(code), PAGE_EXECUTE_READ,
                               &oldProtect)) {
    detail = "VirtualProtectFromApp(PAGE_EXECUTE_READ) failed (err " +
             std::to_string(::GetLastError()) +
             ") - codeGeneration capability missing?";
    ::VirtualFree(page, 0, MEM_RELEASE);
    return false;
  }

  ::FlushInstructionCache(::GetCurrentProcess(), page, sizeof(code));

  // Set the Thumb bit on the entry pointer.
  using Fn = int (*)();
  auto fn = reinterpret_cast<Fn>(reinterpret_cast<uintptr_t>(page) | 1u);
  int result = fn();

  ::VirtualFree(page, 0, MEM_RELEASE);

  if (result != 42) {
    detail = "JIT probe executed but returned " + std::to_string(result);
    return false;
  }
  detail = "JIT OK: codeGeneration executable memory verified";
  return true;
}

}  // namespace

struct gecko_runtime {
  std::string profile;
  bool jit = false;
  std::string jit_detail;
};

struct gecko_session {
  gecko_runtime* rt = nullptr;
  bool is_private = false;
  std::string url;
  gecko_session_delegate delegate{};
  bool announced = false;
};

extern "C" {

gecko_runtime* gecko_runtime_create(const gecko_runtime_config* config) {
  auto* rt = new gecko_runtime();
  if (config && config->profile_dir) rt->profile = config->profile_dir;
  rt->jit = config && config->jit_enabled;
  if (rt->jit) {
    ProbeJit(rt->jit_detail);
  } else {
    rt->jit_detail = "JIT disabled by config";
  }
  return rt;
}

void gecko_runtime_shutdown(gecko_runtime* rt) { delete rt; }

int32_t gecko_runtime_pump(gecko_runtime*) { return 0; }

gecko_session* gecko_session_create(gecko_runtime* rt, int32_t is_private) {
  auto* s = new gecko_session();
  s->rt = rt;
  s->is_private = is_private != 0;
  return s;
}

void gecko_session_close(gecko_session* s) { delete s; }

void gecko_session_load_uri(gecko_session* s, const char* uri) {
  if (!s) return;
  s->url = uri ? uri : "";
  if (s->delegate.on_location_change) {
    s->delegate.on_location_change(s->delegate.user_data, s->url.c_str());
  }
  if (s->delegate.on_title_change) {
    // Surface the JIT probe result the first time, so the shell can show it.
    std::string title = s->url;
    if (!s->announced && s->rt) {
      title = s->rt->jit_detail + "  |  " + s->url;
      s->announced = true;
    }
    s->delegate.on_title_change(s->delegate.user_data, title.c_str());
  }
  if (s->delegate.on_progress)
    s->delegate.on_progress(s->delegate.user_data, 1.0f);
}

void gecko_session_reload(gecko_session* s) {
  if (s) gecko_session_load_uri(s, s->url.c_str());
}
void gecko_session_stop(gecko_session*) {}
void gecko_session_go_back(gecko_session*) {}
void gecko_session_go_forward(gecko_session*) {}
int32_t gecko_session_can_go_back(gecko_session*) { return 0; }
int32_t gecko_session_can_go_forward(gecko_session*) { return 0; }

void gecko_session_set_surface(gecko_session*, void*, int32_t, int32_t, float) {}
void gecko_session_resize(gecko_session*, int32_t, int32_t, float) {}
void gecko_session_touch(gecko_session*, int32_t, int32_t, float, float) {}
void gecko_session_key(gecko_session*, int32_t, int32_t, uint32_t) {}
void gecko_session_scroll(gecko_session*, float, float) {}

void gecko_session_set_delegate(gecko_session* s,
                                const gecko_session_delegate* d) {
  if (s && d) s->delegate = *d;
}

}  // extern "C"
