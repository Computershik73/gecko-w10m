// gecko_bootstrap.cpp — start the real Gecko runtime.
//
// xul.dll exposes exactly one external interface, mozilla::Bootstrap, obtained
// from the exported XRE_GetBootstrap. Its only full-application entry point is
// XRE_main, which is what firefox.exe itself calls: it reads application.ini,
// sets up the profile, starts XPCOM and runs the event loop until shutdown.
//
// This runs headless. The Windows widget backend creates desktop HWNDs, which
// a packaged app on a phone has no way to present -- it has a CoreWindow, not
// a desktop window station. Headless keeps XPCOM, SpiderMonkey, networking and
// layout in play while leaving windowing out, which is the right first thing
// to prove. Putting pixels on screen comes after, and will mean giving Gecko a
// compositor target backed by a XAML SwapChainPanel rather than an HWND.
//
// Compiled by clang-cl with exceptions off; see gecko_bootstrap.h for why.

#include "gecko_bootstrap.h"

#include <windows.h>

#include <string>
#include <vector>

#include "mozilla/Bootstrap.h"

namespace {

gecko_w10m_gecko_log_fn gLog = nullptr;

void Log(const std::string& line) {
  if (gLog) gLog(line.c_str());
}

// The Gecko side speaks UTF-8 and the Windows side UTF-16; both conversions are
// needed often enough to be worth naming.
std::string Narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
  return out;
}

// Gecko reports early failures on stderr, and a packaged app has none. Point
// the standard handles at a file so those messages survive.
void RedirectStdErrTo(const std::wstring& path) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  params.lpSecurityAttributes = &sa;

  HANDLE h = ::CreateFile2(path.c_str(), FILE_APPEND_DATA | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS,
                           &params);
  if (h == INVALID_HANDLE_VALUE) {
    Log("stderr redirect failed, err " + std::to_string(::GetLastError()));
    return;
  }
  ::SetFilePointer(h, 0, nullptr, FILE_END);
  ::SetStdHandle(STD_ERROR_HANDLE, h);
  ::SetStdHandle(STD_OUTPUT_HANDLE, h);
}

// The engine reads its environment through the C runtime -- PR_GetEnv is
// getenv, and gfxPlatform::IsHeadless asks it directly -- and ucrtbase copies
// the Win32 environment block once at process start and never looks again.
// SetEnvironmentVariableW updates the block, not the copy, so nothing set that
// way is visible to the engine. GeckoW10m.exe cannot fix that with its own
// putenv either: it links the static runtime, so its environment is a third
// one that xul.dll never reads.
//
// Setting it where the engine looks means calling ucrtbase's own _wputenv_s.
// This is why MOZ_HEADLESS was ignored -- the Windows widget backend was being
// used on a device with no windows -- and why MOZ_LOG_FILE produced nothing.
void SetEngineEnvironment(const wchar_t* name, const wchar_t* value) {
  using PutEnvFn = int(__cdecl*)(const wchar_t*, const wchar_t*);
  static PutEnvFn crtPutEnv = [] {
    HMODULE ucrt = ::GetModuleHandleW(L"ucrtbase.dll");
    return ucrt ? reinterpret_cast<PutEnvFn>(::GetProcAddress(ucrt, "_wputenv_s"))
                : nullptr;
  }();

  ::SetEnvironmentVariableW(name, value);
  if (crtPutEnv) {
    crtPutEnv(name, value);
  }
}

using GetBootstrapFn = void(NS_FROZENCALL*)(mozilla::Bootstrap::UniquePtr&);

}  // namespace

extern "C" void gecko_w10m_gecko_set_logger(gecko_w10m_gecko_log_fn fn) { gLog = fn; }

extern "C" int gecko_w10m_gecko_run(const wchar_t* installDir,
                                 const wchar_t* profileDir) {
  const std::wstring install(installDir ? installDir : L"");
  const std::wstring profile(profileDir ? profileDir : L"");

  // Headless, and single process: an app container cannot spawn the content
  // child, and the build is configured for a single process anyway.
  SetEngineEnvironment(L"MOZ_HEADLESS", L"1");
  SetEngineEnvironment(L"MOZ_FORCE_DISABLE_E10S", L"1");

  // Where libxul writes the delay-load substitutions it had to make. See the
  // failure hook in toolkit/xre/Bootstrap.cpp.
  // Ad-hoc notes from inside the engine, for bringing this port up.
  ::SetEnvironmentVariableW(L"GECKO_W10M_NOTE_LOG",
                            (profile + L"\gecko-notes.log").c_str());

  ::SetEnvironmentVariableW(L"GECKO_W10M_DELAYLOAD_LOG",
                            (profile + L"\\delay-load-used.log").c_str());

  // Gecko's own logging, next to ours, so a failure inside the engine says
  // more than a return code.
  const std::wstring geckoLog = profile + L"\\gecko.log";
  SetEngineEnvironment(
      L"MOZ_LOG",
      L"timestamp,sync,nsAppRunner:5,XRE:5,nsComponentManager:5,"
      L"nsChromeRegistry:5,nsIOService:5,URILoader:5");
  SetEngineEnvironment(L"MOZ_LOG_FILE", geckoLog.c_str());
  RedirectStdErrTo(profile + L"\\gecko-stderr.log");

  Log("bootstrap: loading xul.dll");
  HMODULE xul = ::LoadPackagedLibrary(L"xul.dll", 0);
  if (!xul) {
    Log("bootstrap: xul.dll failed to load, err " +
        std::to_string(::GetLastError()));
    return -1;
  }

  auto getBootstrap =
      reinterpret_cast<GetBootstrapFn>(::GetProcAddress(xul, "XRE_GetBootstrap"));
  if (!getBootstrap) {
    Log("bootstrap: XRE_GetBootstrap missing, err " +
        std::to_string(::GetLastError()));
    return -2;
  }

  Log("bootstrap: calling XRE_GetBootstrap");
  mozilla::Bootstrap::UniquePtr bootstrap;
  getBootstrap(bootstrap);
  if (!bootstrap) {
    Log("bootstrap: XRE_GetBootstrap returned nothing");
    return -3;
  }
  Log("bootstrap: got the Bootstrap object");

  // This is the first call that runs Gecko's own code, so it is the first
  // that can fall over. NS_LogInit is cheap and touches the allocator, which
  // makes it a useful canary before XRE_main.
  bootstrap->NS_LogInit();
  Log("bootstrap: NS_LogInit returned");

  // argv[0] must be the executable; XRE_main derives the install directory
  // from it. -profile keeps the profile inside LocalState, the only place a
  // packaged app may write.
  const std::string exe = Narrow(install + L"\\GeckoW10m.exe");
  const std::string profileArg = Narrow(profile);
  std::vector<char*> argv;
  std::string a0 = exe;
  std::string a1 = "-profile";
  std::string a2 = profileArg;
  argv.push_back(a0.data());
  argv.push_back(a1.data());
  argv.push_back(a2.data());
  argv.push_back(nullptr);

  mozilla::BootstrapConfig config{};
  config.appData = nullptr;
  // With appData null this is the path of an application.ini to read, and the
  // directory that holds it becomes the application directory -- what
  // resource:/// and chrome://browser/ resolve against.
  //
  // That has to be browser/, not the root. The root application.ini says so
  // in its first line, and reading it instead left every resource:///module
  // unreachable: no command line handler could be created, so nothing opened
  // a window and nsAppStartup was handed eConsiderQuit. firefox.exe arranges
  // the same thing differently, by passing static app data with "browser" as
  // the relative directory; we have no such symbol, so we point at the copy
  // that tools/build-appx.sh stages there.
  std::wstring appIniPath = install + L"\\browser\\application.ini";
  if (::GetFileAttributesW(appIniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
    Log("bootstrap: no browser\\application.ini, falling back to the root one");
    appIniPath = install + L"\\application.ini";
  }
  const std::string appIni = Narrow(appIniPath);
  config.appDataPath = appIni.c_str();

  Log("bootstrap: XRE_main with " + appIni);
  int rc = bootstrap->XRE_main(static_cast<int>(argv.size()) - 1, argv.data(),
                               config);
  Log("bootstrap: XRE_main returned " + std::to_string(rc));

  bootstrap->NS_LogTerm();
  return rc;
}
