#include "GeckoRuntimeHost.h"

#include <windows.h>

#include <string>

#include "../client/Log.h"
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
// attempt count survives the crash; three failures in a row and Gecko is left
// alone until the file is removed, leaving a usable shell and a readable log.
constexpr int kMaxAttempts = 3;

std::wstring AttemptsPath(const std::wstring& localState) {
  return localState + L"\\gecko-attempts.txt";
}

int ReadAttempts(const std::wstring& localState) {
  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  HANDLE h = ::CreateFile2(AttemptsPath(localState).c_str(), GENERIC_READ,
                           FILE_SHARE_READ, OPEN_EXISTING, &params);
  if (h == INVALID_HANDLE_VALUE) return 0;

  char buf[16] = {};
  DWORD read = 0;
  ::ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
  ::CloseHandle(h);
  return std::atoi(buf);
}

void WriteAttempts(const std::wstring& localState, int value) {
  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  HANDLE h = ::CreateFile2(AttemptsPath(localState).c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ, CREATE_ALWAYS, &params);
  if (h == INVALID_HANDLE_VALUE) return;

  std::string text = std::to_string(value);
  DWORD written = 0;
  ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written,
              nullptr);
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
  WriteAttempts(args->localState, 0);

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

  int attempts = ReadAttempts(localStatePath);
  if (attempts >= kMaxAttempts) {
    Log::WriteNum(L"gecko: not starting, failed attempts", attempts);
    Log::Write(L"gecko: delete gecko-attempts.txt in LocalState to try again");
    return false;
  }
  WriteAttempts(localStatePath, attempts + 1);
  Log::WriteNum(L"gecko: starting runtime, attempt", attempts + 1);

  const std::wstring profileDir = localStatePath + L"\\profile";
  ::CreateDirectoryW(profileDir.c_str(), nullptr);

  gecko_w10m_gecko_set_logger(&BridgeLog);

  auto* args = new ThreadArgs{installDir, profileDir, localStatePath};
  HANDLE thread = ::CreateThread(nullptr, 0, &GeckoThread, args, 0, nullptr);
  if (!thread) {
    Log::WriteNum(L"gecko: CreateThread failed, err", ::GetLastError());
    delete args;
    return false;
  }
  ::CloseHandle(thread);
  return true;
}

}  // namespace gecko_w10m::engine
