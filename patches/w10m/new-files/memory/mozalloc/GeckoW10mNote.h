/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_GeckoW10mNote_h
#define mozilla_GeckoW10mNote_h

// A packaged app has no console, and ucrtbase bound stderr to a handle that
// goes nowhere, so anything an allocation failure has to say is lost before it
// can be read off the device.  This writes it to a file instead.
//
// It is deliberately a copy of what toolkit/xre/Bootstrap.cpp does rather than
// a call to it: mozglue sits underneath xul and cannot reach up into it.  It
// also avoids the CRT entirely -- by the time it runs, the allocator has
// already said no.

#if defined(GECKO_W10M)

#  include <windows.h>
#  include <stddef.h>

namespace gecko_w10m {

inline void NoteAppend(char*& out, const char* end, const char* text) {
  while (*text && out < end) *out++ = *text++;
}

inline void NoteAppendHex(char*& out, const char* end, size_t value) {
  static const char kDigits[] = "0123456789abcdef";
  NoteAppend(out, end, "0x");
  bool leading = true;
  for (int shift = (sizeof(size_t) * 8) - 4; shift >= 0; shift -= 4) {
    unsigned digit = (value >> shift) & 0xf;
    if (digit == 0 && leading && shift > 0) continue;
    leading = false;
    if (out < end) *out++ = kDigits[digit];
  }
}

// Names the module an address belongs to, as "xul.dll+0x1234", so the line can
// be symbolized without having to match it against a particular launch's
// module bases.
inline void NoteAppendAddress(char*& out, const char* end, const void* address) {
  HMODULE module = nullptr;
  if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
      module) {
    wchar_t path[MAX_PATH] = {};
    DWORD length = ::GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = path;
    for (DWORD i = 0; i < length; ++i) {
      if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
    }
    while (*name && out < end) *out++ = static_cast<char>(*name++);
    NoteAppend(out, end, "+");
    NoteAppendHex(out, end,
                  reinterpret_cast<size_t>(address) -
                      reinterpret_cast<size_t>(module));
    return;
  }
  NoteAppendHex(out, end, reinterpret_cast<size_t>(address));
}

inline void Note(const char* text) {
  wchar_t path[MAX_PATH] = {};
  if (!::GetEnvironmentVariableW(L"GECKO_W10M_NOTE_LOG", path, MAX_PATH)) return;

  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  HANDLE file = ::CreateFile2(path, FILE_APPEND_DATA, FILE_SHARE_READ,
                              OPEN_ALWAYS, &params);
  if (file == INVALID_HANDLE_VALUE) return;

  ::SetFilePointer(file, 0, nullptr, FILE_END);
  DWORD written = 0;
  ::WriteFile(file, "mozalloc: ", 10, &written, nullptr);
  size_t length = 0;
  while (text[length]) ++length;
  ::WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
  ::WriteFile(file, "\r\n", 2, &written, nullptr);
  ::FlushFileBuffers(file);
  ::CloseHandle(file);
}

// The product of nmemb and size is all mozalloc_handle_oom ever sees, and a
// product tells you nothing about which of the two was wrong.
inline void NoteFailedAllocation(const char* what, size_t first, size_t second,
                                 const void* caller) {
  char line[256];
  char* out = line;
  const char* end = line + sizeof(line) - 1;
  NoteAppend(out, end, what);
  NoteAppend(out, end, " failed: ");
  NoteAppendHex(out, end, first);
  NoteAppend(out, end, " x ");
  NoteAppendHex(out, end, second);
  NoteAppend(out, end, " called from ");
  NoteAppendAddress(out, end, caller);
  *out = '\0';
  Note(line);
}

}  // namespace gecko_w10m

#endif  // defined(GECKO_W10M)
#endif  // mozilla_GeckoW10mNote_h
