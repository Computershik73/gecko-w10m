// BrowserPreferences.h — W10M port of the iOS BrowserPreferences.
// Backed by Windows.Storage.ApplicationData local settings (the UWP analogue of
// NSUserDefaults). Keys mirror the iOS "<profile>.<Section>.<name>" scheme so
// behaviour and defaults stay recognizable.
#pragma once

#include <string>
#include <winrt/Windows.Storage.h>

namespace gecko_w10m::client {

enum class SearchEngineId {
  Google, Yahoo, Bing, Brave, DuckDuckGo, Ecosia, Startpage, Custom
};

enum class NewTabDisplay { Homepage, BlankPage, CustomUrl };

enum class ChromePosition { Top, Bottom };

class BrowserPreferences {
 public:
  static BrowserPreferences& Shared();

  // Search
  SearchEngineId SearchEngine() const;
  void SetSearchEngine(SearchEngineId id);
  std::wstring CustomSearchTemplate() const;
  void SetCustomSearchTemplate(std::wstring_view t);
  bool ShowSearchSuggestions() const;

  // Browsing
  bool RequestDesktopWebsite() const;
  void SetRequestDesktopWebsite(bool v);

  // Appearance
  ChromePosition AddressBarPosition() const;
  void SetAddressBarPosition(ChromePosition p);

  // New tab
  NewTabDisplay NewTabDisplayOption() const;
  std::wstring CustomNewTabUrl() const;

  // JIT (drives whether the runtime is created with codeGeneration JIT on)
  bool IsJitEnabled() const;
  void SetJitEnabled(bool v);

 private:
  BrowserPreferences();
  std::wstring Key(std::wstring_view section, std::wstring_view name) const;

  bool GetBool(std::wstring_view section, std::wstring_view name,
               bool fallback) const;
  void SetBool(std::wstring_view section, std::wstring_view name, bool v);
  std::wstring GetString(std::wstring_view section, std::wstring_view name,
                         std::wstring_view fallback) const;
  void SetString(std::wstring_view section, std::wstring_view name,
                 std::wstring_view v);
  int GetInt(std::wstring_view section, std::wstring_view name,
             int fallback) const;
  void SetInt(std::wstring_view section, std::wstring_view name, int v);

  std::wstring profile_ = L"default";
  winrt::Windows::Storage::ApplicationDataContainer settings_{nullptr};
};

}  // namespace gecko_w10m::client
