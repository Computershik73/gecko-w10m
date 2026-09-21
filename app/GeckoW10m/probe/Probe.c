// A second application in the package, for phones with no debugger and no
// crash dumps: Win32 only, static CRT, no XAML, no WinRT. It asks the loader
// for each DLL the browser needs, one at a time, and writes the answer with
// the error code into TempState\gecko-probe.txt -- which the Device Portal's
// App File Explorer shows -- and then exits.
//
// A Lumia 650 on 1607 maps msvcp140.dll and then never maps the
// vcruntime140.dll it imports, and the kernel's image events say only that,
// not why. GetLastError after LoadPackagedLibrary says why: 126 is "not
// found", 193 "not a valid image", 577 "invalid image hash", 127 "a function
// it imports is missing", 1114 "its DllMain failed".
#include <windows.h>
#include <appmodel.h>
#include <stdio.h>
#include <wchar.h>

static HANDLE g_out = INVALID_HANDLE_VALUE;

static void say(const wchar_t* text) {
  DWORD written = 0;
  if (g_out != INVALID_HANDLE_VALUE) {
    ::WriteFile(g_out, text, (DWORD)(wcslen(text) * sizeof(wchar_t)),
                &written, NULL);
    ::WriteFile(g_out, L"\r\n", 2 * sizeof(wchar_t), &written, NULL);
    ::FlushFileBuffers(g_out);
  }
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
    swprintf_s(line, 512, L"%s  (%s)  -> loaded at %p", name,
              packaged ? L"package" : L"system", (void*)m);
  } else {
    swprintf_s(line, 512, L"%s  (%s)  -> FAILED, error %lu (0x%lx)", name,
              packaged ? L"package" : L"system", err, err);
  }
  say(line);
}

int WINAPI wWinMain(HINSTANCE h, HINSTANCE p, PWSTR cmd, int show) {
  wchar_t path[MAX_PATH];
  CREATEFILE2_EXTENDED_PARAMETERS params;
  OSVERSIONINFOW ver;
  wchar_t line[256];
  (void)h; (void)p; (void)cmd; (void)show;

  // Not GetTempPath: inside the container that is AC\Temp, which the
  // Device Portal never shows. LocalState is
  // %LOCALAPPDATA%\Packages\<family>\LocalState, and both halves come
  // from Win32 alone.
  {
    wchar_t local[MAX_PATH] = {0};
    wchar_t family[128] = {0};
    UINT32 familyLen = 128;
    ::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    ::GetCurrentPackageFamilyName(&familyLen, family);
    swprintf_s(path, MAX_PATH,
               L"%s\\Packages\\%s\\LocalState\\gecko-probe.txt", local,
               family);
  }
  {
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    params.dwFileFlags = 0;
    params.dwSecurityQosFlags = 0;
    params.lpSecurityAttributes = NULL;
    params.hTemplateFile = NULL;
    g_out = ::CreateFile2(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS,
                          &params);
  }
  say(L"Gecko loader probe");

  // What the loader itself is: RtlGetVersion is not lied to by manifests.
  {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn fn = ntdll ? (RtlGetVersionFn)::GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    ver.dwOSVersionInfoSize = sizeof(ver);
    if (fn && fn(&ver) == 0) {
      swprintf_s(line, 256, L"OS build %lu.%lu.%lu", ver.dwMajorVersion,
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

  // System DLLs the engine names directly.
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

  say(L"done");
  if (g_out != INVALID_HANDLE_VALUE) {
    ::CloseHandle(g_out);
  }
  return 0;
}
