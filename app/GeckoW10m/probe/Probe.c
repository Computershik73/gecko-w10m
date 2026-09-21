// Gecko Probe: its own tiny package, for phones with no debugger and no
// crash dumps. Win32 only, static CRT, no XAML, no WinRT. It asks the loader
// for each DLL the browser's start-up chain needs and reports every answer
// with its error code.
//
// A Lumia 650 on 1607 maps msvcp140.dll and then never maps the
// vcruntime140.dll it imports, and the kernel's image events say only that,
// not why. GetLastError after LoadPackagedLibrary says why: 126 is "not
// found", 193 "not a valid image", 577 "invalid image hash", 127 "a function
// it imports is missing", 1114 "its DllMain failed".
//
// The answers go out three ways at once, because on that phone no file has
// yet been seen to arrive: as ETW events under the provider below, which the
// Device Portal records like any other; into LocalState, built from
// %LOCALAPPDATA% and the package family name; and into the container's temp
// directory. The process then stays up five seconds, so a run is visible as
// a run and not mistaken for a crash.
#include <windows.h>
#include <appmodel.h>
#include <TraceLoggingProvider.h>
#include <stdio.h>
#include <wchar.h>

// {5f3c2e1a-7b8d-4c9e-9a1b-2c3d4e5f6a7b}: enter this GUID as a custom
// provider on the Device Portal's ETW page, level 5.
TRACELOGGING_DEFINE_PROVIDER(g_provider, "GeckoProbe",
    (0x5f3c2e1a, 0x7b8d, 0x4c9e, 0x9a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x6a, 0x7b));

static HANDLE g_files[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};

static void say(const wchar_t* text) {
  DWORD written = 0;
  int i;
  TraceLoggingWrite(g_provider, "Line", TraceLoggingLevel(4),
                    TraceLoggingWideString(text, "text"));
  for (i = 0; i < 2; ++i) {
    if (g_files[i] != INVALID_HANDLE_VALUE) {
      ::WriteFile(g_files[i], text, (DWORD)(wcslen(text) * sizeof(wchar_t)),
                  &written, NULL);
      ::WriteFile(g_files[i], L"\r\n", 2 * sizeof(wchar_t), &written, NULL);
      ::FlushFileBuffers(g_files[i]);
    }
  }
}

static HANDLE open_out(const wchar_t* path) {
  CREATEFILE2_EXTENDED_PARAMETERS params;
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  params.dwFileFlags = 0;
  params.dwSecurityQosFlags = 0;
  params.lpSecurityAttributes = NULL;
  params.hTemplateFile = NULL;
  return ::CreateFile2(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS,
                       &params);
}

static void probe(const wchar_t* name, int packaged) {
  wchar_t line[512];
  HMODULE m;
  DWORD err;
  ::SetLastError(0);
  m = packaged ? ::LoadPackagedLibrary(name, 0)
               : ::LoadLibraryExW(name, NULL, 0);
  err = ::GetLastError();
  if (m) {
    swprintf_s(line, 512, L"%s (%s) -> loaded at %p", name,
               packaged ? L"package" : L"system", (void*)m);
  } else {
    swprintf_s(line, 512, L"%s (%s) -> FAILED; error %lu (0x%lx)", name,
               packaged ? L"package" : L"system", err, err);
  }
  say(line);
}

int WINAPI wWinMain(HINSTANCE h, HINSTANCE p, PWSTR cmd, int show) {
  wchar_t local[MAX_PATH] = {0};
  wchar_t family[128] = {0};
  wchar_t path[MAX_PATH];
  wchar_t line[600];
  UINT32 familyLen = 128;
  DWORD localLen, familyRc;
  OSVERSIONINFOW ver;
  (void)h; (void)p; (void)cmd; (void)show;

  TraceLoggingRegister(g_provider);

  // LocalState, without WinRT: %LOCALAPPDATA%\Packages\<family>\LocalState.
  // Inside the container LOCALAPPDATA is already the package's own
  // ...\Packages\<family>\AC directory (the probe on the 650 said so);
  // LocalState is its sibling.
  localLen = ::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  familyRc = ::GetCurrentPackageFamilyName(&familyLen, family);
  if (localLen) {
    size_t n = wcslen(local);
    if (n > 3 && _wcsicmp(local + n - 3, L"\\AC") == 0) {
      local[n - 3] = 0;
      swprintf_s(path, MAX_PATH, L"%s\\LocalState\\gecko-probe.txt", local);
    } else {
      swprintf_s(path, MAX_PATH, L"%s\\Packages\\%s\\LocalState\\gecko-probe.txt",
                 local, family);
    }
    g_files[0] = open_out(path);
  }
  // And the container's own temp directory, whatever it is.
  if (::GetTempPathW(MAX_PATH, path)) {
    wcscat_s(path, MAX_PATH, L"gecko-probe.txt");
    g_files[1] = open_out(path);
  }

  say(L"Gecko loader probe");
  swprintf_s(line, 600, L"LOCALAPPDATA=\"%s\" (len %lu); family=\"%s\" (rc %lu); "
             L"LocalState file %s; temp file %s",
             local, localLen, family, familyRc,
             g_files[0] != INVALID_HANDLE_VALUE ? L"open" : L"NOT open",
             g_files[1] != INVALID_HANDLE_VALUE ? L"open" : L"NOT open");
  say(line);
  if (::GetTempPathW(MAX_PATH, path)) {
    swprintf_s(line, 600, L"temp path: %s", path);
    say(line);
  }

  // What the loader itself is: RtlGetVersion is not lied to by manifests.
  {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn fn = ntdll ? (RtlGetVersionFn)::GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    ver.dwOSVersionInfoSize = sizeof(ver);
    if (fn && fn(&ver) == 0) {
      swprintf_s(line, 600, L"OS build %lu.%lu.%lu", ver.dwMajorVersion,
                 ver.dwMinorVersion, ver.dwBuildNumber);
      say(line);
    }
  }

  // The chain the browser dies in, one link at a time.
  probe(L"vcruntime140.dll", 1);
  probe(L"msvcp140.dll", 1);
  probe(L"mozglue.dll", 1);
  probe(L"nss3.dll", 1);
  probe(L"xul.dll", 1);

  // The forwarder shims by name, and the api-set names through the system
  // path: whether the schema knows them, and whether the package directory
  // is searched when it does not.
  probe(L"api-ms-win-core-fibers-l1-1-0.dll", 1);
  probe(L"api-ms-win-core-fibers-l1-1-0.dll", 0);
  probe(L"api-ms-win-core-threadpool-l1-2-0.dll", 0);
  probe(L"api-ms-win-core-file-l2-1-0.dll", 0);
  probe(L"api-ms-win-core-sysinfo-l1-2-0.dll", 0);
  probe(L"api-ms-win-core-winrt-error-l1-1-1.dll", 0);
  probe(L"api-ms-win-core-com-l1-1-0.dll", 0);
  probe(L"api-ms-win-crt-locale-l1-1-0.dll", 0);
  probe(L"api-ms-win-crt-time-l1-1-0.dll", 0);

  // Every DLL the package names outside the api-sets. The trace from the
  // 650 shows kernel32legacy.dll where kernel32.dll would be; which of the
  // others exist there at all is what this list answers.
  probe(L"kernel32.dll", 0);
  probe(L"kernel32legacy.dll", 0);
  probe(L"advapi32.dll", 0);
  probe(L"ntdll.dll", 0);
  probe(L"rpcrt4.dll", 0);
  probe(L"bcrypt.dll", 0);
  probe(L"bcryptprimitives.dll", 0);
  probe(L"ole32.dll", 0);
  probe(L"combase.dll", 0);
  probe(L"shcore.dll", 0);
  probe(L"wsock32.dll", 0);
  probe(L"d3dcompiler_47.dll", 0);
  probe(L"oleaut32.dll", 0);
  probe(L"uiautomationcore.dll", 0);
  probe(L"dbghelp.dll", 0);
  probe(L"cfgmgr32.dll", 0);
  probe(L"advapi32.dll", 0);
  probe(L"ws2_32.dll", 0);
  probe(L"propsys.dll", 0);
  probe(L"windowscodecs.dll", 0);
  probe(L"version.dll", 0);
  probe(L"d3d11.dll", 0);
  probe(L"dxgi.dll", 0);

  // The functions the 1607 umbrella libraries cannot map to an api-set:
  // which DLL on this phone actually exports them.
  {
    const wchar_t* carriers[] = {L"kernel32legacy.dll", L"kernelbase.dll",
                                 L"combase.dll", L"ntdll.dll"};
    const char* names[] = {
        "CreateFileMappingA", "GetComputerNameW", "GetNamedPipeServerProcessId",
        "GetProcessAffinityMask", "GetSystemPowerStatus", "GlobalLock",
        "GlobalMemoryStatus", "GlobalSize", "GlobalUnlock", "MoveFileW",
        "OpenFileMappingA", "PowerClearRequest", "PowerCreateRequest",
        "PowerSetRequest", "RegisterApplicationRestart", "RegRenameKey",
        "RegisterEventSourceW", "ReportEventW", "LogonUserW",
        "LsaNtStatusToWinError", "RtlNtStatusToDosError", "SymFromAddr", NULL};
    int i, j;
    for (i = 0; names[i]; ++i) {
      wchar_t found[256] = L"";
      for (j = 0; j < 4; ++j) {
        HMODULE m = ::LoadLibraryExW(carriers[j], NULL, 0);
        if (m && ::GetProcAddress(m, names[i])) {
          wcscat_s(found, 256, carriers[j]);
          wcscat_s(found, 256, L" ");
        }
      }
      swprintf_s(line, 600, L"export %S: %s", names[i],
                 found[0] ? found : L"NOWHERE");
      say(line);
    }
  }

  say(L"done");
  if (g_files[0] != INVALID_HANDLE_VALUE) ::CloseHandle(g_files[0]);
  if (g_files[1] != INVALID_HANDLE_VALUE) ::CloseHandle(g_files[1]);
  TraceLoggingUnregister(g_provider);
  ::Sleep(5000);
  return 0;
}
