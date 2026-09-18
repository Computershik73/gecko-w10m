// SearchEngines.cpp
#include "pch.h"
#include "SearchEngines.h"

#include <array>
#include <cwctype>

namespace gecko_w10m::client {

std::wstring SearchEngines::DisplayName(SearchEngineId id) {
  switch (id) {
    case SearchEngineId::Google: return L"Google";
    case SearchEngineId::Yahoo: return L"Yahoo";
    case SearchEngineId::Bing: return L"Bing";
    case SearchEngineId::Brave: return L"Brave";
    case SearchEngineId::DuckDuckGo: return L"DuckDuckGo";
    case SearchEngineId::Ecosia: return L"Ecosia";
    case SearchEngineId::Startpage: return L"Startpage";
    case SearchEngineId::Custom: return L"Custom";
  }
  return L"Google";
}

std::wstring SearchEngines::QueryTemplate(SearchEngineId id) {
  switch (id) {
    case SearchEngineId::Google:
      return L"https://www.google.com/search?q=%s";
    case SearchEngineId::Yahoo:
      return L"https://search.yahoo.com/search?p=%s";
    case SearchEngineId::Bing:
      return L"https://www.bing.com/search?q=%s";
    case SearchEngineId::Brave:
      return L"https://search.brave.com/search?q=%s";
    case SearchEngineId::DuckDuckGo:
      return L"https://duckduckgo.com/?q=%s";
    case SearchEngineId::Ecosia:
      return L"https://www.ecosia.org/search?q=%s";
    case SearchEngineId::Startpage:
      return L"https://www.startpage.com/sp/search?query=%s";
    case SearchEngineId::Custom:
      return L"";
  }
  return L"https://www.google.com/search?q=%s";
}

std::wstring SearchEngines::PercentEncodeQuery(std::wstring_view s) {
  static const wchar_t* hex = L"0123456789ABCDEF";
  std::wstring out;
  // Encode to UTF-8 bytes, then percent-encode non-unreserved bytes.
  int len = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr,
                                  0, nullptr, nullptr);
  std::string utf8(len, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), utf8.data(), len,
                        nullptr, nullptr);
  for (unsigned char c : utf8) {
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '~';
    if (unreserved) {
      out.push_back((wchar_t)c);
    } else if (c == ' ') {
      out.push_back(L'+');
    } else {
      out.push_back(L'%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

std::wstring SearchEngines::Destination(std::wstring_view query) {
  auto id = BrowserPreferences::Shared().SearchEngine();
  std::wstring tmpl = QueryTemplate(id);
  if (id == SearchEngineId::Custom) {
    tmpl = BrowserPreferences::Shared().CustomSearchTemplate();
    if (tmpl.find(L"%s") == std::wstring::npos) {
      tmpl = QueryTemplate(SearchEngineId::Google);
    }
  }
  auto encoded = PercentEncodeQuery(query);
  auto pos = tmpl.find(L"%s");
  if (pos != std::wstring::npos) tmpl.replace(pos, 2, encoded);
  return tmpl;
}

bool SearchEngines::LooksLikeUrl(std::wstring_view s) {
  // Has a scheme, or has a dot with no spaces (e.g. "example.com/path").
  if (s.find(L"://") != std::wstring::npos) return true;
  bool hasSpace = s.find(L' ') != std::wstring::npos;
  bool hasDot = s.find(L'.') != std::wstring::npos;
  if (s.rfind(L"about:", 0) == 0 || s.rfind(L"gecko:", 0) == 0) return true;
  return hasDot && !hasSpace;
}

std::wstring SearchEngines::ResolveEntry(std::wstring_view entry) {
  std::wstring trimmed(entry);
  // trim
  size_t b = trimmed.find_first_not_of(L" \t\r\n");
  size_t e = trimmed.find_last_not_of(L" \t\r\n");
  if (b == std::wstring::npos) return Destination(L"");
  trimmed = trimmed.substr(b, e - b + 1);

  if (!LooksLikeUrl(trimmed)) return Destination(trimmed);
  if (trimmed.find(L"://") == std::wstring::npos &&
      trimmed.rfind(L"about:", 0) != 0 && trimmed.rfind(L"gecko:", 0) != 0) {
    return L"https://" + trimmed;
  }
  return trimmed;
}

}  // namespace gecko_w10m::client
