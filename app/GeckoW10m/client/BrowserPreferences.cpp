// BrowserPreferences.cpp
#include "pch.h"
#include "BrowserPreferences.h"

using namespace winrt::Windows::Storage;

namespace gecko_w10m::client {

BrowserPreferences& BrowserPreferences::Shared() {
  static BrowserPreferences instance;
  return instance;
}

BrowserPreferences::BrowserPreferences() {
  settings_ = ApplicationData::Current().LocalSettings();
}

std::wstring BrowserPreferences::Key(std::wstring_view section,
                                     std::wstring_view name) const {
  return profile_ + L"." + std::wstring(section) + L"." + std::wstring(name);
}

bool BrowserPreferences::GetBool(std::wstring_view section,
                                 std::wstring_view name, bool fallback) const {
  auto values = settings_.Values();
  auto k = winrt::hstring(Key(section, name));
  if (values.HasKey(k)) return winrt::unbox_value<bool>(values.Lookup(k));
  return fallback;
}
void BrowserPreferences::SetBool(std::wstring_view section,
                                 std::wstring_view name, bool v) {
  settings_.Values().Insert(winrt::hstring(Key(section, name)),
                            winrt::box_value(v));
}

std::wstring BrowserPreferences::GetString(std::wstring_view section,
                                           std::wstring_view name,
                                           std::wstring_view fallback) const {
  auto values = settings_.Values();
  auto k = winrt::hstring(Key(section, name));
  if (values.HasKey(k))
    return std::wstring(winrt::unbox_value<winrt::hstring>(values.Lookup(k)));
  return std::wstring(fallback);
}
void BrowserPreferences::SetString(std::wstring_view section,
                                   std::wstring_view name,
                                   std::wstring_view v) {
  settings_.Values().Insert(winrt::hstring(Key(section, name)),
                            winrt::box_value(winrt::hstring(v)));
}

int BrowserPreferences::GetInt(std::wstring_view section,
                               std::wstring_view name, int fallback) const {
  auto values = settings_.Values();
  auto k = winrt::hstring(Key(section, name));
  if (values.HasKey(k)) return winrt::unbox_value<int32_t>(values.Lookup(k));
  return fallback;
}
void BrowserPreferences::SetInt(std::wstring_view section,
                                std::wstring_view name, int v) {
  settings_.Values().Insert(winrt::hstring(Key(section, name)),
                            winrt::box_value<int32_t>(v));
}

// ---- Typed accessors -------------------------------------------------------

SearchEngineId BrowserPreferences::SearchEngine() const {
  auto raw = GetString(L"SearchSettings", L"searchEngine", L"google");
  if (raw == L"yahoo") return SearchEngineId::Yahoo;
  if (raw == L"bing") return SearchEngineId::Bing;
  if (raw == L"brave") return SearchEngineId::Brave;
  if (raw == L"duckDuckGo") return SearchEngineId::DuckDuckGo;
  if (raw == L"ecosia") return SearchEngineId::Ecosia;
  if (raw == L"startpage") return SearchEngineId::Startpage;
  if (raw == L"custom") return SearchEngineId::Custom;
  return SearchEngineId::Google;
}
void BrowserPreferences::SetSearchEngine(SearchEngineId id) {
  const wchar_t* v = L"google";
  switch (id) {
    case SearchEngineId::Yahoo: v = L"yahoo"; break;
    case SearchEngineId::Bing: v = L"bing"; break;
    case SearchEngineId::Brave: v = L"brave"; break;
    case SearchEngineId::DuckDuckGo: v = L"duckDuckGo"; break;
    case SearchEngineId::Ecosia: v = L"ecosia"; break;
    case SearchEngineId::Startpage: v = L"startpage"; break;
    case SearchEngineId::Custom: v = L"custom"; break;
    default: break;
  }
  SetString(L"SearchSettings", L"searchEngine", v);
}
std::wstring BrowserPreferences::CustomSearchTemplate() const {
  return GetString(L"SearchSettings", L"customSearchTemplate", L"");
}
void BrowserPreferences::SetCustomSearchTemplate(std::wstring_view t) {
  SetString(L"SearchSettings", L"customSearchTemplate", t);
}
bool BrowserPreferences::ShowSearchSuggestions() const {
  return GetBool(L"SearchSettings", L"showSearchSuggestions", true);
}

bool BrowserPreferences::RequestDesktopWebsite() const {
  return GetBool(L"BrowsingSettings", L"requestDesktopWebsite", false);
}
void BrowserPreferences::SetRequestDesktopWebsite(bool v) {
  SetBool(L"BrowsingSettings", L"requestDesktopWebsite", v);
}

ChromePosition BrowserPreferences::AddressBarPosition() const {
  return GetString(L"AppearanceSettings", L"addressBarPosition", L"bottom") ==
                 L"top"
             ? ChromePosition::Top
             : ChromePosition::Bottom;
}
void BrowserPreferences::SetAddressBarPosition(ChromePosition p) {
  SetString(L"AppearanceSettings", L"addressBarPosition",
            p == ChromePosition::Top ? L"top" : L"bottom");
}

NewTabDisplay BrowserPreferences::NewTabDisplayOption() const {
  auto raw = GetString(L"NewTabSettings", L"newTabDisplayOption", L"homepage");
  if (raw == L"blankPage") return NewTabDisplay::BlankPage;
  if (raw == L"customURL") return NewTabDisplay::CustomUrl;
  return NewTabDisplay::Homepage;
}
std::wstring BrowserPreferences::CustomNewTabUrl() const {
  return GetString(L"NewTabSettings", L"customNewTabURL", L"");
}

bool BrowserPreferences::IsJitEnabled() const {
  return GetBool(L"JITSettings", L"isJITEnabled", false);
}
void BrowserPreferences::SetJitEnabled(bool v) {
  SetBool(L"JITSettings", L"isJITEnabled", v);
}

}  // namespace gecko_w10m::client
