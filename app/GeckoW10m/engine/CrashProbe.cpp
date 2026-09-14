#include "CrashProbe.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

#include "../client/Log.h"

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

std::wstring Hex(uintptr_t v) {
  wchar_t buf[19];
  ::swprintf_s(buf, L"0x%08llx", static_cast<unsigned long long>(v));
  return buf;
}

// An address on its own says nothing across runs -- ASLR moves every module.
// Named against its module and offset it stays meaningful, and an offset into
// xul.dll resolves through tools/symbolize.py against that build's map.
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

  uintptr_t rva =
      reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
  return name + L"+" + Hex(rva);
}

void LogBacktrace(const wchar_t* tag) {
  void* frames[28] = {};
  USHORT count = ::RtlCaptureStackBackTrace(0, 28, frames, nullptr);
  for (USHORT i = 0; i < count; ++i) {
    Log::Write(std::wstring(tag) + L"   " + DescribeAddress(frames[i]));
  }
}

// ---------------------------------------------------------------- exceptions

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
  LogBacktrace(L"crash:");

  // Not ours to handle -- only to record.
  return EXCEPTION_CONTINUE_SEARCH;
}

// -------------------------------------------------------------- import hooks
//
// A process that ends through abort, _exit or TerminateProcess raises no
// exception, so no handler of any kind sees it go. Gecko reaches all three:
// a failed MOZ_RELEASE_ASSERT or an OOM abort is a deliberate, silent kill.
// Redirecting xul.dll's own import slots catches the call while its stack is
// still standing, which is the only moment the caller can still be named.

using AbortFn = void(__cdecl*)();
using ExitFn = void(__cdecl*)(int);
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);

AbortFn gRealAbort = nullptr;
ExitFn gRealExit = nullptr;
TerminateProcessFn gRealTerminateProcess = nullptr;

void __cdecl HookAbort() {
  Log::Write(L"kill: xul.dll called abort()");
  LogBacktrace(L"kill:");
  if (gRealAbort) gRealAbort();
  ::TerminateProcess(::GetCurrentProcess(), 3);
}

void __cdecl HookExit(int code) {
  Log::WriteNum(L"kill: xul.dll called _exit", code);
  LogBacktrace(L"kill:");
  if (gRealExit) gRealExit(code);
}

BOOL WINAPI HookTerminateProcess(HANDLE process, UINT code) {
  Log::WriteNum(L"kill: xul.dll called TerminateProcess, code", code);
  LogBacktrace(L"kill:");
  return gRealTerminateProcess ? gRealTerminateProcess(process, code) : FALSE;
}

// Writes one import slot, saving what was there. The import table sits in
// read-only memory, and VirtualProtectFromApp is the only way an app container
// may change that.
bool ReplaceSlot(void** slot, void* replacement, void** original) {
  ULONG previous = 0;
  if (!::VirtualProtectFromApp(slot, sizeof(void*), PAGE_READWRITE, &previous)) {
    return false;
  }
  *original = *slot;
  *slot = replacement;
  ULONG ignored = 0;
  ::VirtualProtectFromApp(slot, sizeof(void*), previous, &ignored);
  return true;
}

int HookImports(HMODULE module) {
  auto* base = reinterpret_cast<unsigned char*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);

  DWORD importRva =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
          .VirtualAddress;
  if (!importRva) return 0;

  // A loaded image is laid out, so an RVA is simply an offset from the base --
  // no section walk, unlike reading the same table out of the file.
  auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importRva);

  int hooked = 0;
  for (; desc->Name; ++desc) {
    if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;

    auto* names =
        reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->OriginalFirstThunk);
    auto* addresses =
        reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->FirstThunk);

    for (; names->u1.AddressOfData; ++names, ++addresses) {
      if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;

      auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
          base + names->u1.AddressOfData);
      const char* name = byName->Name;

      void** slot = reinterpret_cast<void**>(&addresses->u1.Function);
      if (std::strcmp(name, "abort") == 0 && !gRealAbort) {
        if (ReplaceSlot(slot, reinterpret_cast<void*>(&HookAbort),
                        reinterpret_cast<void**>(&gRealAbort))) {
          ++hooked;
        }
      } else if (std::strcmp(name, "_exit") == 0 && !gRealExit) {
        if (ReplaceSlot(slot, reinterpret_cast<void*>(&HookExit),
                        reinterpret_cast<void**>(&gRealExit))) {
          ++hooked;
        }
      } else if (std::strcmp(name, "TerminateProcess") == 0 &&
                 !gRealTerminateProcess) {
        if (ReplaceSlot(slot, reinterpret_cast<void*>(&HookTerminateProcess),
                        reinterpret_cast<void**>(&gRealTerminateProcess))) {
          ++hooked;
        }
      }
    }
  }
  return hooked;
}

// ------------------------------------------------------------- pc sampling

// SuspendThread and GetThreadContext are outside the app partition of the
// headers, and unlike AddVectoredExceptionHandler they may genuinely be
// refused, so they are resolved at run time and their absence is reported
// rather than failing the link.
using SuspendThreadFn = DWORD(WINAPI*)(HANDLE);
using ResumeThreadFn = DWORD(WINAPI*)(HANDLE);
using GetThreadContextFn = BOOL(WINAPI*)(HANDLE, PCONTEXT);

SuspendThreadFn gSuspendThread = nullptr;
ResumeThreadFn gResumeThread = nullptr;
GetThreadContextFn gGetThreadContext = nullptr;

bool ResolveSamplingApis() {
  HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
  if (!k32) return false;
  gSuspendThread =
      reinterpret_cast<SuspendThreadFn>(::GetProcAddress(k32, "SuspendThread"));
  gResumeThread =
      reinterpret_cast<ResumeThreadFn>(::GetProcAddress(k32, "ResumeThread"));
  gGetThreadContext = reinterpret_cast<GetThreadContextFn>(
      ::GetProcAddress(k32, "GetThreadContext"));
  return gSuspendThread && gResumeThread && gGetThreadContext;
}

// Long enough to cover a startup that has been dying in tens of milliseconds,
// short enough that a runtime which survives stops paying for it.
constexpr int kMaxSamples = 1200;

DWORD WINAPI SamplerThread(LPVOID param) {
  HANDLE target = static_cast<HANDLE>(param);

  if (!ResolveSamplingApis()) {
    Log::Write(L"sampler: thread inspection unavailable in this container");
    ::CloseHandle(target);
    return 0;
  }

  uintptr_t last = 0;
  int written = 0;
  for (int i = 0; i < kMaxSamples; ++i) {
    if (::WaitForSingleObject(target, 0) == WAIT_OBJECT_0) break;

    if (gSuspendThread(target) == static_cast<DWORD>(-1)) break;

    // CONTEXT wants 8-byte alignment on ARM and GetThreadContext will refuse
    // it otherwise.
    __declspec(align(8)) CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    BOOL ok = gGetThreadContext(target, &context);
    gResumeThread(target);

    if (!ok) {
      Log::WriteNum(L"sampler: GetThreadContext failed, err", ::GetLastError());
      break;
    }

    uintptr_t pc = static_cast<uintptr_t>(context.Pc);
    // Consecutive samples inside one tight loop say nothing new; only movement
    // is worth a line.
    if (pc != last) {
      last = pc;
      Log::Write(L"pc: " + DescribeAddress(reinterpret_cast<void*>(pc)));
      ++written;
    }
    ::Sleep(1);
  }

  Log::WriteNum(L"sampler: finished, samples written", written);
  ::CloseHandle(target);
  return 0;
}

}  // namespace

void StartLastLocationSampler(void* thread) {
  HANDLE sampler =
      ::CreateThread(nullptr, 0, &SamplerThread, thread, 0, nullptr);
  if (!sampler) {
    Log::WriteNum(L"sampler: CreateThread failed, err", ::GetLastError());
    ::CloseHandle(static_cast<HANDLE>(thread));
    return;
  }
  ::CloseHandle(sampler);
}

void InstallCrashProbes() {
  PVOID handler = ::AddVectoredExceptionHandler(1, &OnException);
  Log::Write(L"probe crash: vectored handler",
             handler ? L"installed" : L"REFUSED");

  HMODULE xul = ::LoadPackagedLibrary(L"xul.dll", 0);
  if (!xul) {
    Log::WriteNum(L"probe crash: xul.dll not loaded, err", ::GetLastError());
    return;
  }
  Log::WriteNum(L"probe crash: exit paths hooked", HookImports(xul));
}

}  // namespace gecko_w10m::engine
