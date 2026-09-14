#include "GeckoRuntimeHost.h"

#include <windows.h>

#include <atomic>
#include <string>

#include "../client/Log.h"
#include "gecko_bootstrap.h"

// The app partition of errhandlingapi.h hides this one, though kernel32 exports
// it and an app container may call it. Declared here rather than compiling the
// whole file for the desktop partition, which would drag the A/W macros in
// ahead of everything else.
extern "C" {
using GeckoW10mVectoredHandler = LONG(CALLBACK*)(PEXCEPTION_POINTERS);
__declspec(dllimport) PVOID WINAPI
AddVectoredExceptionHandler(ULONG First, GeckoW10mVectoredHandler Handler);
}

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

std::wstring Hex(uintptr_t v) {
  wchar_t buf[19];
  ::swprintf_s(buf, L"0x%08llx", static_cast<unsigned long long>(v));
  return buf;
}

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

// An address on its own says nothing across runs -- ASLR moves every module.
// Named against its module and offset it stays meaningful, and an offset into
// xul.dll can be looked up against the build that produced it.
std::wstring DescribeAddress(const void* addr) {
  HMODULE mod = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &mod) ||
      !mod) {
    return Hex(reinterpret_cast<uintptr_t>(addr)) + L" (no module)";
  }

  wchar_t path[MAX_PATH] = {};
  DWORD n = ::GetModuleFileNameW(mod, path, MAX_PATH);
  std::wstring name(path, n);
  auto slash = name.find_last_of(L'\\');
  if (slash != std::wstring::npos) name = name.substr(slash + 1);

  uintptr_t rva = reinterpret_cast<uintptr_t>(addr) -
                  reinterpret_cast<uintptr_t>(mod);
  return name + L"+" + Hex(rva);
}

// MOZ_CRASH lands here as a breakpoint, which is why 0x80000003 counts as
// fatal: it is how Gecko says "this is unrecoverable" on Windows.
bool IsFatal(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_BREAKPOINT:
    case STATUS_HEAP_CORRUPTION:
      return true;
    default:
      return false;
  }
}

std::atomic<int> gReported{0};
constexpr int kMaxReports = 4;

LONG CALLBACK OnException(PEXCEPTION_POINTERS info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (!IsFatal(code)) return EXCEPTION_CONTINUE_SEARCH;
  if (gReported.fetch_add(1) >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;

  Log::Write(L"crash: code " + Hex(code) + L" at " +
             DescribeAddress(info->ExceptionRecord->ExceptionAddress));

  // The frames are the actually useful part: the faulting address alone rarely
  // names the caller that got there.
  void* frames[24] = {};
  USHORT count = ::RtlCaptureStackBackTrace(0, 24, frames, nullptr);
  for (USHORT i = 0; i < count; ++i) {
    Log::Write(L"crash:   " + DescribeAddress(frames[i]));
  }

  // Not ours to handle -- only to record.
  return EXCEPTION_CONTINUE_SEARCH;
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

  // First chance, before anyone else: a crash inside Gecko takes the process
  // with it, and this is the only record that survives.
  ::AddVectoredExceptionHandler(1, &OnException);

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
  ::CloseHandle(thread);
  return true;
}

}  // namespace gecko_w10m::engine
