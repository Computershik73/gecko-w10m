// Log.cpp
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
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
bool g_verbose = false;
// The swap chain capacity mode (Log.h). Its default is the default of
// gecko.swapchain.capacity in 00-gecko.js (tools/build-appx.sh), and the two
// have to stay the same.
constexpr int kDefaultCapacityMode = 1;
int g_capacityMode = kDefaultCapacityMode;

// Gecko writes a pref the user changed as
//   user_pref("gecko.debug.verbose_logs", true);
// and drops the line again when it goes back to its default, false.
bool ReadVerbosePref(const std::wstring& localState) {
  const std::wstring prefs = localState + L"profile\\prefs.js";
  FILE* f = _wfopen(prefs.c_str(), L"rb");
  if (!f) return false;
  bool verbose = false;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    if (strstr(line, "\"gecko.debug.verbose_logs\"")) {
      verbose = strstr(line, "true") != nullptr;
    }
  }
  fclose(f);
  return verbose;
}

// What the swap chain's capacity is used as (Log.h), and in *source where
// that was said.
int ReadSwapChainCapacity(const std::wstring& localState,
                          const wchar_t** source) {
  // The file first. It is made on a PC, and what PowerShell's echo writes
  // there begins with a byte order mark and has a zero after every
  // character, so the digit is the first byte that is none of those.
  const std::wstring file = localState + L"swap-chain-capacity.txt";
  if (FILE* f = _wfopen(file.c_str(), L"rb")) {
    unsigned char head[8];
    const size_t got = fread(head, 1, sizeof(head), f);
    fclose(f);
    for (size_t i = 0; i < got; ++i) {
      const unsigned char c = head[i];
      if (c == 0xEF || c == 0xBB || c == 0xBF || c == 0xFF || c == 0xFE ||
          c == 0 || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        continue;
      }
      if (c >= '0' && c <= '2') {
        *source = L"swap-chain-capacity.txt in LocalState";
        return c - '0';
      }
      break;  // not one of the three: as if the file were not there
    }
  }

  // Gecko writes the pref into prefs.js only while it differs from its
  // default, so no line means the default.
  int mode = kDefaultCapacityMode;
  *source = L"the default";
  const std::wstring prefs = localState + L"profile\\prefs.js";
  FILE* f = _wfopen(prefs.c_str(), L"rb");
  if (!f) return mode;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    // user_pref("gecko.swapchain.capacity", 2);
    const char* name = strstr(line, "\"gecko.swapchain.capacity\"");
    const char* value = name ? strchr(name, ',') : nullptr;
    if (!value) continue;
    do {
      ++value;
    } while (*value == ' ' || *value == '\t');
    if (value[0] >= '0' && value[0] <= '2' &&
        (value[1] < '0' || value[1] > '9')) {
      mode = value[0] - '0';
      *source = L"gecko.swapchain.capacity in prefs.js";
    }
  }
  fclose(f);
  return mode;
}

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

void Log::Mirror() {
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
  } catch (...) {
  }
  try {
    using namespace winrt::Windows::Storage;
    FlushFromFault();
    auto folder = KnownFolders::PicturesLibrary()
                      .CreateFolderAsync(L"Gecko logs",
                                         CreationCollisionOption::OpenIfExists)
                      .get();
    const std::wstring dir = g_path.substr(0, g_path.rfind(L'\\') + 1);
    const wchar_t* names[] = {L"gecko.log",
                              L"profile\\gecko-notes.log",
                              L"profile\\delay-load-used.log",
                              L"profile\\gecko-stderr.log",
                              L"profile\\gecko-moz.log"};
    int copied = 0;
    for (const wchar_t* name : names) {
      const std::wstring full = dir + name;
      if (::GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) {
        continue;
      }
      std::wstring leaf(name);
      leaf = leaf.substr(leaf.rfind(L'\\') + 1);
      auto file = StorageFile::GetFileFromPathAsync(full).get();
      file.CopyAsync(folder, leaf, NameCollisionOption::ReplaceExisting).get();
      ++copied;
    }
    Write(L"mirror: " + std::to_wstring(copied) +
          L" logs copied to Pictures\\Gecko logs");
  } catch (winrt::hresult_error const& e) {
    Write(L"mirror: failed", std::wstring(e.message()));
  } catch (...) {
    Write(L"mirror: failed");
  }
}

bool Log::Verbose() { return g_verbose; }

int Log::SwapChainCapacityMode() { return g_capacityMode; }

void Log::Init(std::wstring_view localStatePath) {
  std::unique_lock<std::mutex> lock(g_mutex);
  if (g_file != INVALID_HANDLE_VALUE) return;

  g_path.assign(localStatePath);
  if (!g_path.empty() && g_path.back() != L'\\') g_path += L'\\';
  g_verbose = ReadVerbosePref(g_path);
  const wchar_t* capacitySource = L"";
  g_capacityMode = ReadSwapChainCapacity(g_path, &capacitySource);
  // For the engine bootstrap, which does not link this file: it decides from
  // this whether Gecko's own logging goes on.
  ::SetEnvironmentVariableW(L"GECKO_W10M_VERBOSE_LOGS",
                            g_verbose ? L"1" : nullptr);
  g_path += L"gecko.log";

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

  const char* banner = "\r\n==== Gecko session start ====\r\n";
  DWORD written = 0;
  ::WriteFile(g_file, banner, (DWORD)strlen(banner), &written, nullptr);

  // Which of the three decided is the first thing to know about a picture
  // that looks wrong. Write takes the mutex itself.
  const std::wstring capacity =
      L"swap chain capacity: mode " + std::to_wstring(g_capacityMode) +
      L", from " + capacitySource;
  lock.unlock();
  Write(capacity);
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

void Log::WriteFromFault(std::wstring_view line) {
  std::wstring stamped = Timestamp() + L"  " + std::wstring(line);

  // try_lock, not lock: if the fault happened while this thread already held
  // the mutex, taking it again is undefined and waiting on it is a deadlock.
  // A missing line is better than a hung phone.
  if (g_mutex.try_lock()) {
    if (g_file != INVALID_HANDLE_VALUE) {
      std::string utf8 = ToUtf8(stamped) + "\r\n";
      DWORD written = 0;
      ::WriteFile(g_file, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
      // No FlushFileBuffers here. The report is flushed once at its end.
    }
    g_mutex.unlock();
  }
  ::OutputDebugStringW((stamped + L"\r\n").c_str());
  // And no handler. That is the whole point of this function.
}

void Log::FlushFromFault() {
  if (g_mutex.try_lock()) {
    if (g_file != INVALID_HANDLE_VALUE) {
      ::FlushFileBuffers(g_file);
    }
    g_mutex.unlock();
  }
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
