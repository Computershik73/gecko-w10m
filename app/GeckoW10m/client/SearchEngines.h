// SearchEngines.h — W10M port of the iOS SearchEngines. Turns an address-bar
// entry into either a direct URL or a search query URL.
#pragma once

#include <string>
#include "BrowserPreferences.h"

namespace gecko_w10m::client {

class SearchEngines {
 public:
  // Display name for a given engine id.
  static std::wstring DisplayName(SearchEngineId id);

  // Query template ("...?q=%s") for a built-in engine, empty for Custom.
  static std::wstring QueryTemplate(SearchEngineId id);

  // Build the search-result URL for a query using the current engine setting.
  static std::wstring Destination(std::wstring_view query);

  // Resolve an address-bar string: if it looks like a URL, normalize it;
  // otherwise turn it into a search destination.
  static std::wstring ResolveEntry(std::wstring_view entry);

  static bool LooksLikeUrl(std::wstring_view s);

 private:
  static std::wstring PercentEncodeQuery(std::wstring_view s);
};

}  // namespace gecko_w10m::client
