#include "CrashProbe.h"

#include <windows.h>
#include <winrt/Windows.System.h>

#include <delayimp.h>

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

std::wstring Widen(const char* s) {
  if (!s) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  if (n <= 1) return std::wstring();
  std::wstring out(static_cast<size_t>(n - 1), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
  return out;
}

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

// RtlCaptureStackBackTrace returns one frame here and stops: 32-bit ARM
// Windows unwinds from tables rather than a frame pointer chain, and without
// consulting them there is no second frame to find. Walking those tables gives
// a real trace -- and, unlike the capture, one that can start from the context
// of the thread that faulted rather than from the handler's own stack.
using LookupFunctionEntryFn = PVOID(WINAPI*)(DWORD, PDWORD, PVOID);
using VirtualUnwindFn = PVOID(WINAPI*)(DWORD, DWORD, DWORD, PVOID, PCONTEXT,
                                       PVOID*, PDWORD, PVOID);

LookupFunctionEntryFn gLookupFunctionEntry = nullptr;
VirtualUnwindFn gVirtualUnwind = nullptr;

void ResolveUnwindApis() {
  HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return;
  gLookupFunctionEntry = reinterpret_cast<LookupFunctionEntryFn>(
      ::GetProcAddress(ntdll, "RtlLookupFunctionEntry"));
  gVirtualUnwind = reinterpret_cast<VirtualUnwindFn>(
      ::GetProcAddress(ntdll, "RtlVirtualUnwind"));
}

// Takes the context by value: unwinding rewrites it, and the caller's copy
// belongs to the exception record.
void LogStack(const wchar_t* tag, CONTEXT context) {
  if (!gLookupFunctionEntry || !gVirtualUnwind) {
    Log::Write(std::wstring(tag) + L"   (no unwind support)");
    return;
  }

  for (int depth = 0; depth < 32; ++depth) {
    if (!context.Pc) return;
    Log::Write(std::wstring(tag) + L"   " +
               DescribeAddress(reinterpret_cast<void*>(context.Pc)));

    DWORD imageBase = 0;
    PVOID entry = gLookupFunctionEntry(context.Pc, &imageBase, nullptr);
    if (!entry) return;

    PVOID handlerData = nullptr;
    DWORD establisher = 0;
    DWORD previous = context.Pc;
    gVirtualUnwind(0 /*UNW_FLAG_NHANDLER*/, imageBase, context.Pc, entry,
                   &context, &handlerData, &establisher, nullptr);
    // A frame that does not move is a frame that will not move again.
    if (context.Pc == previous) return;
  }
}

// For the hooks, which have no exception context of their own.
// A phone gives an app a hard memory ceiling, and 132 MB of engine plus
// everything Gecko allocates on startup is a real candidate for reaching it.
// Whatever else a crash report says, it should say how close this was.
void LogMemory(const wchar_t* tag) {
  auto used = winrt::Windows::System::MemoryManager::AppMemoryUsage();
  auto limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
  Log::Write(std::wstring(tag) + L" memory " +
             std::to_wstring(used / (1024 * 1024)) + L" MB of " +
             std::to_wstring(limit / (1024 * 1024)) + L" MB");
}

void LogBacktrace(const wchar_t* tag) {
  CONTEXT context{};
  ::RtlCaptureContext(&context);
  LogStack(tag, context);
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

  // First chance means exactly that: the process may well have a handler for
  // this and carry on. Said plainly so it is not read as a cause of death.
  Log::Write(L"first-chance: code " + Hex(code) + L" at " +
             DescribeAddress(info->ExceptionRecord->ExceptionAddress));
  LogMemory(L"first-chance:");
  LogStack(L"first-chance:", *info->ContextRecord);

  // Not ours to handle -- only to record.
  return EXCEPTION_CONTINUE_SEARCH;
}

// -------------------------------------------------------------- import hooks
//
// Two jobs, one mechanism. A process ended through abort, _exit,
// TerminateProcess or ExitProcess raises nothing, so no handler of any kind
// sees it go -- those get hooked so the caller can be named while its stack is
// still standing. And with the control run showing that the XAML fault only
// happens when the engine runs, the rest are here to say what the engine was
// asking the system for on its way there: which libraries it pulled in, and
// what it was raising, since every trace so far has ended inside
// RaiseException.
//
// Every hook records and calls straight through. None of them change what
// happens.

using AbortFn = void(__cdecl*)();
using ExitFn = void(__cdecl*)(int);
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
using ExitProcessFn = void(WINAPI*)(UINT);
using RaiseExceptionFn = void(WINAPI*)(DWORD, DWORD, DWORD, const ULONG_PTR*);
using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using LoadPackagedLibraryFn = HMODULE(WINAPI*)(LPCWSTR, DWORD);

AbortFn gRealAbort = nullptr;
ExitFn gRealExit = nullptr;
TerminateProcessFn gRealTerminateProcess = nullptr;
ExitProcessFn gRealExitProcess = nullptr;
RaiseExceptionFn gRealRaiseException = nullptr;
LoadLibraryExWFn gRealLoadLibraryExW = nullptr;
LoadPackagedLibraryFn gRealLoadPackagedLibrary = nullptr;

void __cdecl HookAbort() {
  Log::Write(L"kill: the engine called abort()");
  LogBacktrace(L"kill:");
  if (gRealAbort) gRealAbort();
  ::TerminateProcess(::GetCurrentProcess(), 3);
}

void __cdecl HookExit(int code) {
  Log::WriteNum(L"kill: the engine called _exit", code);
  LogBacktrace(L"kill:");
  if (gRealExit) gRealExit(code);
}

BOOL WINAPI HookTerminateProcess(HANDLE process, UINT code) {
  Log::WriteNum(L"kill: the engine called TerminateProcess, code", code);
  LogBacktrace(L"kill:");
  return gRealTerminateProcess ? gRealTerminateProcess(process, code) : FALSE;
}

void WINAPI HookExitProcess(UINT code) {
  Log::WriteNum(L"kill: the engine called ExitProcess, code", code);
  LogBacktrace(L"kill:");
  if (gRealExitProcess) gRealExitProcess(code);
}

// Naming a thread is done by raising an exception a debugger is meant to
// swallow, and Gecko names a lot of threads. Reporting those would bury
// everything else.
constexpr DWORD kThreadNameException = 0x406D1388;

std::atomic<int> gRaiseReports{0};

// The delay-load helper reports a missing library or function by raising, and
// the argument it raises with names both. Without reading it, all a delay-load
// failure says is that something somewhere was not there.
constexpr DWORD kDelayLoadModuleMissing = 0xC06D007E;
constexpr DWORD kDelayLoadProcMissing = 0xC06D007F;

void LogDelayLoadFailure(DWORD code, DWORD count, const ULONG_PTR* args) {
  if (!count || !args) return;
  auto* info = reinterpret_cast<const DelayLoadInfo*>(args[0]);
  if (!info) return;

  std::wstring dll = info->szDll ? Widen(info->szDll) : L"(unnamed)";
  std::wstring what =
      code == kDelayLoadModuleMissing
          ? L" did not load"
          : (info->dlp.fImportByName
                 ? L"!" + Widen(info->dlp.szProcName) + L" not found"
                 : L" ordinal not found");
  Log::Write(L"delay-load: " + dll + what + L", err " +
             std::to_wstring(info->dwLastError));
}

void WINAPI HookRaiseException(DWORD code, DWORD flags, DWORD count,
                               const ULONG_PTR* args) {
  if (code == kDelayLoadModuleMissing || code == kDelayLoadProcMissing) {
    LogDelayLoadFailure(code, count, args);
  } else if (code != kThreadNameException && gRaiseReports.fetch_add(1) < 12) {
    Log::Write(L"raise: the engine raised " + Hex(code));
    LogBacktrace(L"raise:");
  }
  if (gRealRaiseException) gRealRaiseException(code, flags, count, args);
}

HMODULE WINAPI HookLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
  HMODULE module = gRealLoadLibraryExW(name, file, flags);
  Log::Write(L"load: " + std::wstring(name ? name : L"(null)") +
             (module ? L"" : L"  FAILED"));
  return module;
}

HMODULE WINAPI HookLoadPackagedLibrary(LPCWSTR name, DWORD reserved) {
  HMODULE module = gRealLoadPackagedLibrary(name, reserved);
  Log::Write(L"load: " + std::wstring(name ? name : L"(null)") +
             (module ? L" (packaged)" : L" (packaged)  FAILED"));
  return module;
}

struct Hook {
  const char* name;
  void* replacement;
  void** original;
};

const Hook kHooks[] = {
    {"abort", reinterpret_cast<void*>(&HookAbort),
     reinterpret_cast<void**>(&gRealAbort)},
    {"_exit", reinterpret_cast<void*>(&HookExit),
     reinterpret_cast<void**>(&gRealExit)},
    {"TerminateProcess", reinterpret_cast<void*>(&HookTerminateProcess),
     reinterpret_cast<void**>(&gRealTerminateProcess)},
    {"ExitProcess", reinterpret_cast<void*>(&HookExitProcess),
     reinterpret_cast<void**>(&gRealExitProcess)},
    {"RaiseException", reinterpret_cast<void*>(&HookRaiseException),
     reinterpret_cast<void**>(&gRealRaiseException)},
    {"LoadLibraryExW", reinterpret_cast<void*>(&HookLoadLibraryExW),
     reinterpret_cast<void**>(&gRealLoadLibraryExW)},
    {"LoadPackagedLibrary", reinterpret_cast<void*>(&HookLoadPackagedLibrary),
     reinterpret_cast<void**>(&gRealLoadPackagedLibrary)},
};

// Writes one import slot, saving what was there. The import table sits in
// read-only memory, and VirtualProtectFromApp is the only way an app container
// may change that.
bool ReplaceSlot(void** slot, void* replacement, void** original) {
  ULONG previous = 0;
  if (!::VirtualProtectFromApp(slot, sizeof(void*), PAGE_READWRITE, &previous)) {
    return false;
  }
  if (!*original) *original = *slot;
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

      for (const Hook& hook : kHooks) {
        if (std::strcmp(byName->Name, hook.name) != 0) continue;
        void** slot = reinterpret_cast<void**>(&addresses->u1.Function);
        // A slot already pointing at the replacement belongs to a module that
        // has been through here.
        if (*slot == hook.replacement) break;
        if (ReplaceSlot(slot, hook.replacement, hook.original)) ++hooked;
        break;
      }
    }
  }
  return hooked;
}

// --------------------------------------------------------- delay-load audit
//
// The engine delay-loads some forty desktop libraries, and this device ships
// reduced versions of most of them: the library is there, a given function is
// not. Each such miss ends the thread, because a delay-load thunk reports
// failure by raising and 32-bit ARM has no exception handling to catch it.
//
// Finding them one per build, one crash at a time, is no way to spend a day.
// This walks the delay-load directory and asks after every function in it at
// once, so the whole list arrives in a single run and the stubs can be written
// against it.

// delayimp's own descriptor, spelled out rather than including delayimp.h for
// it: the header's definition drags in the helper machinery too.
struct DelayDescriptor {
  DWORD attributes;
  DWORD nameRva;
  DWORD moduleHandleRva;
  DWORD iatRva;
  DWORD intRva;
  DWORD boundIatRva;
  DWORD unloadIatRva;
  DWORD timestamp;
};

void ProbeDelayLoads(HMODULE module) {
  auto* base = reinterpret_cast<unsigned char*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);

  DWORD rva = nt->OptionalHeader
                  .DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]
                  .VirtualAddress;
  if (!rva) {
    Log::Write(L"delay-probe: no delay-load directory");
    return;
  }

  auto* desc = reinterpret_cast<DelayDescriptor*>(base + rva);

  int modules = 0;
  int imports = 0;
  int missing = 0;
  for (; desc->nameRva; ++desc) {
    // The first attribute bit says the fields are RVAs. Anything older stores
    // absolute addresses, which no linker has produced in twenty years.
    if (!(desc->attributes & 1)) continue;

    const char* moduleName =
        reinterpret_cast<const char*>(base + desc->nameRva);
    ++modules;

    HMODULE target = ::LoadLibraryExW(Widen(moduleName).c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                          LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                          LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!target) {
      ++missing;
      Log::Write(L"delay-probe: " + Widen(moduleName) + L" absent entirely");
      continue;
    }

    int moduleImports = 0;
    int moduleMissing = 0;

    if (!desc->intRva) continue;
    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->intRva);
    for (; thunk->u1.AddressOfData; ++thunk) {
      ++imports;
      ++moduleImports;
      FARPROC proc = nullptr;
      std::wstring label;
      if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32) {
        WORD ordinal = static_cast<WORD>(IMAGE_ORDINAL32(thunk->u1.Ordinal));
        proc = ::GetProcAddress(
            target, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(ordinal)));
        label = L"#" + std::to_wstring(ordinal);
      } else {
        auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
            base + thunk->u1.AddressOfData);
        proc = ::GetProcAddress(target, byName->Name);
        label = Widen(byName->Name);
      }
      if (proc) continue;
      ++missing;
      ++moduleMissing;
    }

    // One line per library rather than one per function: 221 names bury the
    // shape of it, and the shape is what matters -- whole subsystems this
    // device does not have.
    if (moduleMissing) {
      Log::Write(L"delay-probe: " + Widen(moduleName) + L" missing " +
                 std::to_wstring(moduleMissing) + L" of " +
                 std::to_wstring(moduleImports));
    }
    ::FreeLibrary(target);
  }

  Log::Write(L"delay-probe: " + std::to_wstring(modules) + L" modules, " +
             std::to_wstring(imports) + L" delayed imports, " +
             std::to_wstring(missing) + L" unavailable");
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

// The trace file. A raw address is worthless in the next process -- ASLR moves
// every module -- so each sample is stored as a module index and an offset,
// with the names written alongside them.
constexpr unsigned kTraceMagic = 0x31525452;  // "RTR1"
constexpr unsigned kMaxModules = 16;
constexpr unsigned kCapacity = 8192;

struct TraceModule {
  char name[40];
  unsigned base;
};

struct TraceSample {
  unsigned module;  // index into modules, or kMaxModules when unknown
  unsigned offset;
};

struct TraceFile {
  unsigned magic;
  unsigned moduleCount;
  unsigned written;  // total samples ever written; the ring holds the last few
  unsigned capacity;
  TraceModule modules[kMaxModules];
  TraceSample samples[kCapacity];
};

TraceFile* gTrace = nullptr;
HANDLE gTraceMapping = nullptr;
HANDLE gTraceFile = INVALID_HANDLE_VALUE;

std::wstring TracePath(const std::wstring& localState) {
  return localState + L"\\pc-trace.bin";
}

TraceFile* MapTrace(const std::wstring& localState, bool createNew) {
  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;

  gTraceFile = ::CreateFile2(TracePath(localState).c_str(),
                             GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                             createNew ? OPEN_ALWAYS : OPEN_EXISTING, &params);
  if (gTraceFile == INVALID_HANDLE_VALUE) return nullptr;

  gTraceMapping = ::CreateFileMappingFromApp(gTraceFile, nullptr, PAGE_READWRITE,
                                             sizeof(TraceFile), nullptr);
  if (!gTraceMapping) {
    ::CloseHandle(gTraceFile);
    gTraceFile = INVALID_HANDLE_VALUE;
    return nullptr;
  }

  auto* view = static_cast<TraceFile*>(::MapViewOfFileFromApp(
      gTraceMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, sizeof(TraceFile)));
  if (!view) {
    ::CloseHandle(gTraceMapping);
    ::CloseHandle(gTraceFile);
    gTraceMapping = nullptr;
    gTraceFile = INVALID_HANDLE_VALUE;
  }
  return view;
}

// Reports the previous run's trace before this run overwrites it. Only the
// tail matters: the ring is long enough that the interesting part is always at
// the end.
void ReportPreviousTrace(const std::wstring& localState) {
  TraceFile* trace = MapTrace(localState, /*createNew*/ false);
  if (!trace) return;

  if (trace->magic != kTraceMagic || trace->written == 0) {
    ::UnmapViewOfFile(trace);
    return;
  }

  constexpr unsigned kReport = 40;
  unsigned total = trace->written;
  unsigned have = total < kCapacity ? total : kCapacity;
  unsigned show = have < kReport ? have : kReport;

  Log::WriteNum(L"trace: samples from the previous run", total);
  for (unsigned i = have - show; i < have; ++i) {
    unsigned slot = (total - have + i) % kCapacity;
    const TraceSample& sample = trace->samples[slot];

    std::wstring where;
    if (sample.module < trace->moduleCount) {
      const char* name = trace->modules[sample.module].name;
      int n = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
      std::wstring wide(static_cast<size_t>(n > 1 ? n - 1 : 0), L'\0');
      if (n > 1) {
        ::MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), n);
      }
      where = wide + L"+" + Hex(sample.offset);
    } else {
      where = Hex(sample.offset) + L" (no module)";
    }
    Log::Write(L"trace: " + where);
  }

  ::UnmapViewOfFile(trace);
  // Reopened for writing by the sampler.
  if (gTraceMapping) ::CloseHandle(gTraceMapping);
  if (gTraceFile != INVALID_HANDLE_VALUE) ::CloseHandle(gTraceFile);
  gTraceMapping = nullptr;
  gTraceFile = INVALID_HANDLE_VALUE;
}

// Resolving a module per sample would cost more than the sample; the handles
// repeat, so a short table answers nearly every lookup.
unsigned ModuleIndexFor(const void* addr) {
  HMODULE mod = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &mod) ||
      !mod) {
    return kMaxModules;
  }

  unsigned base = reinterpret_cast<unsigned>(mod);
  for (unsigned i = 0; i < gTrace->moduleCount; ++i) {
    if (gTrace->modules[i].base == base) return i;
  }
  if (gTrace->moduleCount >= kMaxModules) return kMaxModules;

  wchar_t path[MAX_PATH] = {};
  DWORD n = ::GetModuleFileNameW(mod, path, MAX_PATH);
  std::wstring name(path, n);
  auto slash = name.find_last_of(L'\\');
  if (slash != std::wstring::npos) name = name.substr(slash + 1);

  unsigned index = gTrace->moduleCount;
  TraceModule& entry = gTrace->modules[index];
  entry.base = base;
  ::WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, entry.name,
                        sizeof(entry.name) - 1, nullptr, nullptr);
  gTrace->moduleCount = index + 1;
  return index;
}

DWORD WINAPI SamplerThread(LPVOID param) {
  HANDLE target = static_cast<HANDLE>(param);

  if (!gTrace || !ResolveSamplingApis()) {
    Log::Write(L"sampler: thread inspection unavailable in this container");
    ::CloseHandle(target);
    return 0;
  }

  unsigned last = 0;
  DWORD nextReport = ::GetTickCount();
  while (::WaitForSingleObject(target, 0) != WAIT_OBJECT_0) {
    // Often enough to show a climb, rarely enough that the log stays readable.
    if (::GetTickCount() >= nextReport) {
      nextReport = ::GetTickCount() + 500;
      LogMemory(L"gecko:");
    }
    if (gSuspendThread(target) == static_cast<DWORD>(-1)) break;

    // CONTEXT wants 8-byte alignment on ARM and GetThreadContext refuses it
    // otherwise.
    __declspec(align(8)) CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    BOOL ok = gGetThreadContext(target, &context);
    gResumeThread(target);
    if (!ok) break;

    unsigned pc = static_cast<unsigned>(context.Pc);
    // Consecutive samples at one address say nothing new; only movement is
    // worth a slot in the ring.
    if (pc != last) {
      last = pc;
      unsigned index = ModuleIndexFor(reinterpret_cast<void*>(pc));
      TraceSample& sample = gTrace->samples[gTrace->written % kCapacity];
      sample.module = index;
      sample.offset = index < kMaxModules ? pc - gTrace->modules[index].base : pc;
      ++gTrace->written;
    }
  }

  Log::WriteNum(L"sampler: finished, samples", gTrace->written);
  ::CloseHandle(target);
  return 0;
}

// Reached only when nothing in the process handled the exception, which makes
// this the one report that names a cause rather than an event.
LONG WINAPI OnUnhandledException(PEXCEPTION_POINTERS info) {
  Log::Write(L"FATAL: unhandled " + Hex(info->ExceptionRecord->ExceptionCode) +
             L" at " + DescribeAddress(info->ExceptionRecord->ExceptionAddress));
  LogMemory(L"FATAL:");
  LogStack(L"FATAL:", *info->ContextRecord);
  return EXCEPTION_CONTINUE_SEARCH;
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

// The trace and the crash reports are full of offsets into ntdll and
// KERNELBASE, and there is no map for either. Asking the loader where a
// handful of interesting functions live turns those offsets into names -- and
// in particular says whether an address is a wait, which is a thread doing
// nothing wrong, or a process-ending call, which is not.
void LogKnownOffsets() {
  struct Wanted {
    const wchar_t* module;
    const char* names[10];
  };
  const Wanted wanted[] = {
      {L"ntdll.dll",
       {"RtlExitUserProcess", "NtTerminateProcess", "RtlRaiseStatus",
        "RtlRaiseException", "NtWaitForSingleObject",
        "RtlWaitOnAddress", "RtlUserThreadStart", nullptr}},
      {L"KERNELBASE.dll",
       {"TerminateProcess", "ExitProcess", "RaiseFailFastException",
        "RaiseException", "WaitForSingleObjectEx", "WaitForMultipleObjectsEx",
        "SleepEx", nullptr}},
  };

  for (const Wanted& entry : wanted) {
    HMODULE module = ::GetModuleHandleW(entry.module);
    if (!module) continue;
    for (const char* const* name = entry.names; *name; ++name) {
      FARPROC proc = ::GetProcAddress(module, *name);
      if (!proc) continue;
      uintptr_t offset = reinterpret_cast<uintptr_t>(proc) -
                         reinterpret_cast<uintptr_t>(module);
      std::wstring wide(*name, *name + std::strlen(*name));
      Log::Write(std::wstring(L"known: ") + entry.module + L"+" + Hex(offset) +
                 L" " + wide);
    }
  }
}

void InstallProcessProbes(const std::wstring& localStatePath) {
  ResolveUnwindApis();
  ReportPreviousTrace(localStatePath);

  gTrace = MapTrace(localStatePath, /*createNew*/ true);
  if (gTrace) {
    gTrace->magic = kTraceMagic;
    gTrace->capacity = kCapacity;
    gTrace->written = 0;
    gTrace->moduleCount = 0;
  } else {
    Log::WriteNum(L"trace: could not map the trace file, err", ::GetLastError());
  }

  PVOID handler = ::AddVectoredExceptionHandler(1, &OnException);
  Log::Write(L"probe crash: vectored handler",
             handler ? L"installed" : L"REFUSED");
  ::SetUnhandledExceptionFilter(&OnUnhandledException);
  LogKnownOffsets();
}

void InstallEngineProbes() {
  // mozglue and nss3 reach the same exits and load the same way, and a call
  // through either of them would otherwise pass unseen.
  int hooked = 0;
  for (const wchar_t* name : {L"xul.dll", L"mozglue.dll", L"nss3.dll"}) {
    HMODULE module = ::LoadPackagedLibrary(name, 0);
    if (!module) {
      Log::Write(std::wstring(L"probe crash: ") + name + L" not loaded");
      continue;
    }
    hooked += HookImports(module);
  }
  Log::WriteNum(L"probe crash: imports hooked", hooked);

  HMODULE xul = ::LoadPackagedLibrary(L"xul.dll", 0);
  if (!xul) return;

  ProbeDelayLoads(xul);

  // Whether the loader is enforcing Control Flow Guard on the engine decides
  // whether an indirect call can end the process outright, so it is worth
  // stating in the log next to the crash it might explain.
  auto* base = reinterpret_cast<unsigned char*>(xul);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  WORD characteristics = nt->OptionalHeader.DllCharacteristics;
  Log::Write(L"probe crash: xul.dll CFG",
             (characteristics & 0x4000) ? L"ENFORCED" : L"off");
}

}  // namespace gecko_w10m::engine
