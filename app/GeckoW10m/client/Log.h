// Log.h — a diagnostic log that survives a device with no debugger attached.
//
// Every line goes to three places:
//   * gecko_w10m.log in the app's LocalState folder, flushed immediately, so it
//     can be pulled over USB / Device Portal after a crash;
//   * OutputDebugString, for when a debugger *is* attached;
//   * an in-memory ring buffer the shell shows on screen, because on a phone
//     that is often the only way to read anything at all.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gecko_w10m::client {

class Log {
 public:
  // Opens the log file. Safe to call more than once; safe to skip entirely,
  // in which case lines still reach the ring buffer and the debugger.
  static void Init(std::wstring_view localStatePath);

  static void Write(std::wstring_view line);

  // printf-free formatting helpers for the common cases.
  static void Write(std::wstring_view label, std::wstring_view value);
  static void WriteNum(std::wstring_view label, long long value);

  // The last lines, oldest first.
  static std::vector<std::wstring> Recent();

  // Called on whatever thread logged; marshal to the UI thread yourself.
  static void OnLine(std::function<void(std::wstring)> handler);

  // Full path of the log file, or empty if Init has not run.
  static std::wstring Path();
};

}  // namespace gecko_w10m::client
