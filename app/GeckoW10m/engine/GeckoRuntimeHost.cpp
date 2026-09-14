#include "GeckoRuntimeHost.h"

#include <windows.h>

#include <string>

#include "../client/Log.h"
#include "CrashProbe.h"
#include "gecko_bootstrap.h"

namespace gecko_w10m::engine {
namespace {

using gecko_w10m::client::Log;

std::wstring Widen(const char* s) {
  if (!s) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  if (n <= 1) return std::wstring();
  std::wstring out(static_cast<size_t>(n - 1), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
  return out;
}

void BridgeLog(const char* line) { Log::Write(L"gecko", Widen(line)); }

std::wstring InstallDirectory() {
  wchar_t buf[MAX_PATH] = {};
  DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return std::wstring();
  std::wstring path(buf, n);
  auto slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) return std::wstring();
  path.resize(slash);
  return path;
}

// Bringing an engine up on a phone means the process may well die where it
// stands, and an app that dies on every launch cannot even be inspected. The
// count survives the crash; three failures in a row and Gecko is left alone
// until the file is removed, leaving a usable shell and a readable log.
//
// The install directory is stored with it because it carries the package
// version, so a new build starts from a clean slate instead of inheriting the
// previous one's failures.
constexpr int kMaxAttempts = 3;

std::wstring AttemptsPath(const std::wstring& localState) {
  return localState + L"\\gecko-attempts.txt";
}

// Every other launch leaves the engine alone.
//
// The device has been faulting inside XAML's own dispatcher shortly after
// Gecko starts, at the same address every run. Whether Gecko has anything to
// do with that cannot be told from a log in which Gecko always starts -- and
// until now it always did, so there was no control to compare against.
// Alternating gives both cases in one log file, from one device, minutes
// apart.
bool IsControlLaunch(const std::wstring& localState) {
  const std::wstring path = localState + L"\\gecko-control.txt";

  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;

  char previous = '0';
  HANDLE h = ::CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           OPEN_EXISTING, &params);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD read = 0;
    ::ReadFile(h, &previous, 1, &read, nullptr);
    ::CloseHandle(h);
    if (!read) previous = '0';
  }

  const bool control = previous == '1';
  const char next = control ? '0' : '1';

  h = ::CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS,
                    &params);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    ::WriteFile(h, &next, 1, &written, nullptr);
    ::FlushFileBuffers(h);
    ::CloseHandle(h);
  }
  return control;
}

int ReadAttempts(const std::wstring& localState, const std::wstring& installDir) {
  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  HANDLE h = ::CreateFile2(AttemptsPath(localState).c_str(), GENERIC_READ,
                           FILE_SHARE_READ, OPEN_EXISTING, &params);
  if (h == INVALID_HANDLE_VALUE) return 0;

  char buf[1024] = {};
  DWORD read = 0;
  ::ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
  ::CloseHandle(h);

  // "<count>|<install directory>"
  std::string text(buf, read);
  auto bar = text.find('|');
  if (bar == std::string::npos) return 0;

  std::wstring recorded = Widen(text.substr(bar + 1).c_str());
  if (recorded != installDir) {
    Log::Write(L"gecko: new build, attempt count reset");
    return 0;
  }
  return std::atoi(text.substr(0, bar).c_str());
}

void WriteAttempts(const std::wstring& localState, const std::wstring& installDir,
                   int value) {
  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  HANDLE h = ::CreateFile2(AttemptsPath(localState).c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ, CREATE_ALWAYS, &params);
  if (h == INVALID_HANDLE_VALUE) return;

  int n = ::WideCharToMultiByte(CP_UTF8, 0, installDir.c_str(), -1, nullptr, 0,
                                nullptr, nullptr);
  std::string dir(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) {
    ::WideCharToMultiByte(CP_UTF8, 0, installDir.c_str(), -1, dir.data(), n,
                          nullptr, nullptr);
  }

  std::string text = std::to_string(value) + "|" + dir;
  DWORD written = 0;
  ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
  ::FlushFileBuffers(h);
  ::CloseHandle(h);
}

struct ThreadArgs {
  std::wstring installDir;
  std::wstring profileDir;
  std::wstring localState;
};

DWORD WINAPI GeckoThread(LPVOID param) {
  auto* args = static_cast<ThreadArgs*>(param);

  int rc = gecko_w10m_gecko_run(args->installDir.c_str(), args->profileDir.c_str());
  Log::WriteNum(L"gecko: runtime exited with", rc);

  // Reaching this line at all means the process stayed alive, so the next
  // launch starts from a clean slate.
  WriteAttempts(args->localState, args->installDir, 0);

  delete args;
  return 0;
}

}  // namespace

bool StartGeckoRuntime(const std::wstring& localStatePath) {
  const std::wstring installDir = InstallDirectory();
  if (installDir.empty()) {
    Log::Write(L"gecko: could not determine the install directory");
    return false;
  }

  if (IsControlLaunch(localStatePath)) {
    Log::Write(L"control run: leaving the engine alone this launch");
    Log::Write(L"control run: anything that faults now is not Gecko's doing");
    return false;
  }

  int attempts = ReadAttempts(localStatePath, installDir);
  if (attempts >= kMaxAttempts) {
    Log::WriteNum(L"gecko: not starting, failed attempts", attempts);
    Log::Write(L"gecko: delete gecko-attempts.txt in LocalState to try again");
    return false;
  }
  WriteAttempts(localStatePath, installDir, attempts + 1);
  Log::WriteNum(L"gecko: starting runtime, attempt", attempts + 1);

  const std::wstring profileDir = localStatePath + L"\\profile";
  ::CreateDirectoryW(profileDir.c_str(), nullptr);

  // The rest of the probes went in when the shell started; these need the
  // engine loaded, so they wait until now.
  InstallEngineProbes();

  gecko_w10m_gecko_set_logger(&BridgeLog);

  auto* args = new ThreadArgs{installDir, profileDir, localStatePath};
  // 8 MB, reserved rather than committed. Gecko's main thread does deep work
  // and the executable's default of 1 MB is not what it expects.
  HANDLE thread = ::CreateThread(nullptr, 8 * 1024 * 1024, &GeckoThread, args,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (!thread) {
    Log::WriteNum(L"gecko: CreateThread failed, err", ::GetLastError());
    delete args;
    return false;
  }
  // The sampler takes the handle: it is the only thing that will know where
  // the thread was if the process goes without a word.
  StartLastLocationSampler(thread);
  return true;
}

}  // namespace gecko_w10m::engine
