// Log.cpp
#include "pch.h"
#include "Log.h"

#include <mutex>

namespace gecko_w10m::client {
namespace {

constexpr size_t kRingCapacity = 400;

std::mutex g_mutex;
HANDLE g_file = INVALID_HANDLE_VALUE;
std::wstring g_path;
std::vector<std::wstring> g_ring;
std::function<void(std::wstring)> g_handler;

std::wstring Timestamp() {
  SYSTEMTIME st{};
  ::GetLocalTime(&st);
  wchar_t buf[32];
  // 00:00:00.000
  swprintf_s(buf, L"%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);
  return buf;
}

// The log file is UTF-8 so it is readable off-device without ceremony.
std::string ToUtf8(std::wstring_view s) {
  if (s.empty()) return {};
  int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0,
                                nullptr, nullptr);
  std::string out(n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n,
                        nullptr, nullptr);
  return out;
}

}  // namespace

void Log::Init(std::wstring_view localStatePath) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file != INVALID_HANDLE_VALUE) return;

  g_path.assign(localStatePath);
  if (!g_path.empty() && g_path.back() != L'\\') g_path += L'\\';
  g_path += L"gecko_w10m.log";

  // CreateFile2 is the app-container form of CreateFile; the app's own
  // LocalState is always writable, no capability required.
  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  g_file = ::CreateFile2(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                         OPEN_ALWAYS, &params);
  if (g_file == INVALID_HANDLE_VALUE) {
    g_path.clear();
    return;
  }
  ::SetFilePointer(g_file, 0, nullptr, FILE_END);

  const char* banner = "\r\n==== GeckoW10m session start ====\r\n";
  DWORD written = 0;
  ::WriteFile(g_file, banner, (DWORD)strlen(banner), &written, nullptr);
}

void Log::Write(std::wstring_view line) {
  std::wstring stamped = Timestamp() + L"  " + std::wstring(line);

  std::function<void(std::wstring)> handler;
  {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_file != INVALID_HANDLE_VALUE) {
      std::string utf8 = ToUtf8(stamped) + "\r\n";
      DWORD written = 0;
      ::WriteFile(g_file, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
      ::FlushFileBuffers(g_file);  // a phone can die at any moment
    }

    g_ring.push_back(stamped);
    if (g_ring.size() > kRingCapacity) {
      g_ring.erase(g_ring.begin(), g_ring.begin() + (g_ring.size() - kRingCapacity));
    }
    handler = g_handler;
  }

  ::OutputDebugStringW((stamped + L"\r\n").c_str());
  if (handler) handler(stamped);
}

void Log::Write(std::wstring_view label, std::wstring_view value) {
  Write(std::wstring(label) + L": " + std::wstring(value));
}

void Log::WriteNum(std::wstring_view label, long long value) {
  Write(std::wstring(label) + L": " + std::to_wstring(value));
}

std::vector<std::wstring> Log::Recent() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_ring;
}

void Log::OnLine(std::function<void(std::wstring)> handler) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_handler = std::move(handler);
}

std::wstring Log::Path() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_path;
}

}  // namespace gecko_w10m::client
