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

#include "../client/Log.h"

namespace {
// The probe details are plain ASCII; widen them for the log.
std::wstring Widen(const std::string& s) {
  return std::wstring(s.begin(), s.end());
}
}  // namespace

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

// Walks xul.dll's import table on the device and reports which of its
// dependencies will not load. xul.dll is built against the desktop Win32
// surface, and Windows 10 Mobile does not carry all of it -- urlmon, msi,
// credui and friends. ERROR_MOD_NOT_FOUND from the loader names nothing, so
// this asks each dependency individually and lets the device tell us what is
// actually missing instead of guessing from a desktop SDK.
//
// The image is mapped with LOAD_LIBRARY_AS_DATAFILE, which maps the raw file
// rather than a laid-out image, so every RVA has to be walked back to a file
// offset through the section table.
void ProbeDependencies(const std::wstring& xulPath, std::string& detail) {
  HMODULE data = ::LoadLibraryExW(xulPath.c_str(), nullptr,
                                  LOAD_LIBRARY_AS_DATAFILE);
  if (!data) {
    detail = "could not map xul.dll to read its imports (err " +
             std::to_string(::GetLastError()) + ")";
    return;
  }

  // LOAD_LIBRARY_AS_DATAFILE tags the low bits of the handle.
  auto* base = reinterpret_cast<const unsigned char*>(
      reinterpret_cast<ULONG_PTR>(data) & ~static_cast<ULONG_PTR>(0xF));

  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  auto* sections = IMAGE_FIRST_SECTION(nt);
  const WORD sectionCount = nt->FileHeader.NumberOfSections;

  auto rvaToOffset = [&](DWORD rva) -> DWORD {
    for (WORD i = 0; i < sectionCount; ++i) {
      DWORD start = sections[i].VirtualAddress;
      DWORD size = sections[i].Misc.VirtualSize;
      if (rva >= start && rva < start + size) {
        return sections[i].PointerToRawData + (rva - start);
      }
    }
    return 0;
  };

  DWORD importRva =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
          .VirtualAddress;
  DWORD importOff = rvaToOffset(importRva);
  if (!importOff) {
    detail = "xul.dll has no readable import directory";
    ::FreeLibrary(data);
    return;
  }

  auto* desc =
      reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + importOff);

  int total = 0;
  int missing = 0;
  std::string missingList;
  for (; desc->Name; ++desc) {
    DWORD nameOff = rvaToOffset(desc->Name);
    if (!nameOff) continue;
    const char* name = reinterpret_cast<const char*>(base + nameOff);
    ++total;

    // The package's own payload is next to the exe; everything else has to
    // come from the system. Ask for both, the way the real load will.
    HMODULE m = ::LoadLibraryExW(Widen(name).c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                     LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (m) {
      ::FreeLibrary(m);
      continue;
    }
    ++missing;
    if (!missingList.empty()) missingList += ", ";
    missingList += name;
    missingList += "(" + std::to_string(::GetLastError()) + ")";
  }

  ::FreeLibrary(data);

  detail = std::to_string(total) + " imports, " + std::to_string(missing) +
           " unavailable";
  if (missing) detail += ": " + missingList;
}

// Tries to load the ported engine and resolve its entry point, in stages, so a
// failure says which stage failed. The engine links against the desktop Win32
// surface, which the ARM32 SDK provides and Windows 10 Mobile exports; whether
// the app container lets those calls through at run time is the open question
// this answers on the device.
void ProbeXul(std::string& detail, std::wstring& xulPath) {
  // Stage 1: is the file even in the package? A missing payload and a blocked
  // load look identical from LoadPackagedLibrary's error code alone.
  std::wstring& path = xulPath;
  wchar_t dir[MAX_PATH] = {};
  DWORD n = ::GetModuleFileNameW(nullptr, dir, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    path.assign(dir, n);
    auto slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) path.resize(slash + 1);
    path += L"xul.dll";

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
      unsigned long long size =
          (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
          fad.nFileSizeLow;
      detail = "xul.dll present (" + std::to_string(size / (1024 * 1024)) +
               " MB); ";
    } else {
      detail = "xul.dll NOT found next to the exe (err " +
               std::to_string(::GetLastError()) + "); ";
    }
  }

  // Stage 2: mozglue must be in memory before xul on Windows -- Gecko's
  // allocator lives there and xul imports from it.
  HMODULE glue = ::LoadPackagedLibrary(L"mozglue.dll", 0);
  if (!glue) {
    detail += "mozglue.dll did not load (err " +
              std::to_string(::GetLastError()) + "); ";
  }

  // Stage 3: the real load, which resolves every import.
  HMODULE xul = ::LoadPackagedLibrary(L"xul.dll", 0);
  if (!xul) {
    DWORD packagedErr = ::GetLastError();

    // As a datafile the loader maps the image without resolving imports. If
    // this succeeds where the real load failed, the file is reachable and the
    // problem is an API the app container refuses.
    HMODULE asData = ::LoadLibraryExW(path.empty() ? L"xul.dll" : path.c_str(),
                                      nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (asData) {
      ::FreeLibrary(asData);
      detail += "load failed with an unresolved import or blocked API "
                "(LoadPackagedLibrary err " +
                std::to_string(packagedErr) +
                "); the file itself maps fine as a datafile";
    } else {
      detail += "xul.dll did not load (LoadPackagedLibrary err " +
                std::to_string(packagedErr) + ", datafile err " +
                std::to_string(::GetLastError()) + ")";
    }
    return;
  }

  // XRE_GetBootstrap is how a Gecko host normally gets hold of the runtime.
  auto bootstrap = ::GetProcAddress(xul, "XRE_GetBootstrap");
  if (!bootstrap) {
    detail += "xul.dll loaded but XRE_GetBootstrap is missing (err " +
              std::to_string(::GetLastError()) + ")";
    return;
  }
  detail += "xul.dll loaded, XRE_GetBootstrap resolved";
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
  gecko_w10m::client::Log::Write(L"probe jit", Widen(rt->jit_detail));

  // Whether the engine loads has nothing to do with the JIT preference, so
  // this runs either way -- gating it behind the JIT setting meant one flag
  // hid the answer to the only question that matters right now.
  std::string xul_detail;
  std::wstring xul_path;
  ProbeXul(xul_detail, xul_path);
  gecko_w10m::client::Log::Write(L"probe xul", Widen(xul_detail));

  // When the engine does not load, the interesting question is which of its
  // dependencies the device lacks -- so ask only then, and only if the file
  // was found in the first place.
  if (xul_detail.find("loaded, XRE_GetBootstrap resolved") == std::string::npos &&
      !xul_path.empty()) {
    std::string deps;
    ProbeDependencies(xul_path, deps);
    gecko_w10m::client::Log::Write(L"probe deps", Widen(deps));
  }

  rt->jit_detail += "  |  " + xul_detail;
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
