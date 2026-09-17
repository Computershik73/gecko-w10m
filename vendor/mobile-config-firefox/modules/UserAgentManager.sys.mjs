// Copyright 2025 Danny Colin
// SPDX-License-Identifier: MPL-2.0

import { PrefManager } from 'chrome://mobileconfigfirefox/content/PrefManager.sys.mjs'
import { AppConstants } from 'resource://gre/modules/AppConstants.sys.mjs'

export class UserAgentManager {
  constructor() {
    const APP_VERSION = `${AppConstants.MOZ_APP_VERSION.split(".")[0]}.0`;
    // We added `Mobile;` to the default Firefox Desktop user agent
    const UA_FIREFOX_DESKTOP =
          `Mozilla/5.0 (X11; Linux x86_64; Mobile; rv:${APP_VERSION}) Gecko/20100101 Firefox/${APP_VERSION}`;
    const UA_FIREFOX_ANDROID =
          `Mozilla/5.0 (Android 15; Mobile; rv:${APP_VERSION}) Gecko/${APP_VERSION} Firefox/${APP_VERSION}`;
    // Google version API: https://developer.chrome.com/docs/web-platform/versionhistory/reference
    const UA_CHROME_ANDROID =
          "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.7827.115 Mobile Safari/537.36";
    const UA_CHROME_CHROMEOS =
          "Mozilla/5.0 (X11; CrOS aarch64 16640.57.0) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.7778.250 Safari/537.36";
    // Set default user agent
    PrefManager.defaultPref('general.useragent.override', UA_FIREFOX_DESKTOP);

    const userAgentRules = {
        // Google
        // FIX: Google login not trusting the browser
        "^https?://accounts.google.com($|/)": UA_FIREFOX_DESKTOP,
        // FIX: Google Search showing up as the old layout
        "^https?://(www.)?google.com($|/)": UA_FIREFOX_ANDROID,
        // FIX: ???
        "^https?://drive.google.com($|/)": UA_CHROME_ANDROID,
        // FIX: Google Maps search bar not being interactive
        "^https?://(www.)?google.com/maps($|/)": UA_FIREFOX_DESKTOP,
        "^https?://maps.google.com($|/)": UA_FIREFOX_DESKTOP,

        // Firefox
        // FIX: Sync login not completing
        "^https?://accounts.firefox.com($|/)": UA_FIREFOX_DESKTOP,

        // Mozilla
        // FIX: AMO thinks we want the android extensions
        "^https?://addons.mozilla.org($|/)": UA_FIREFOX_DESKTOP,

        // Youtube
        // FIX: YouTube fullscreen acting weird
        "^https?://(m.)?youtube.com($|/)": UA_FIREFOX_ANDROID,

        // Netflix
        // FIX: Netflix refusing to playback even if EME is working
        "^https?://(www.)?netflix.com($|/)": UA_CHROME_CHROMEOS,
    };


    const requestObserver = {
        observe: function(subject, topic, data) {
            if (topic == "http-on-modify-request") {
                const httpChannel = subject.QueryInterface(Ci.nsIHttpChannel);
                const uri = httpChannel.URI.spec;

                for (const [urlPattern, userAgentRule] of Object.entries(userAgentRules)) {
                    if (new RegExp(urlPattern).test(uri)) {
                        httpChannel.setRequestHeader("User-Agent", userAgentRule, false);
                        break;
                    }
                }
            }
        }
    };

    const observerService = Cc["@mozilla.org/observer-service;1"].getService(Ci.nsIObserverService);
    observerService.addObserver(requestObserver, "http-on-modify-request", false);
  }
}
