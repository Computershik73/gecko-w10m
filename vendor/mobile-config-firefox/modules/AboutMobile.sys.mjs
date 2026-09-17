// Copyright 2025 Danny Colin
// SPDX-License-Identifier: MPL-2.0

/**
 * About Mobile
 * Adapted from toolkit/components/normandy/content/AboutPages.sys.mjs
 *
 * TODO:
 * - Example of a parent/child about page implementation
 *   https://searchfox.org/mozilla-central/source/browser/components/newtab/AboutNewTabRedirector.sys.mjs#444
 */
export class AboutMobile {
  constructor({ chromeUrl, aboutHost, classID, description, uriFlags }) {
    this.chromeUrl = chromeUrl;
    this.aboutHost = aboutHost;
    this.classID = Components.ID(classID);
    this.description = description;
    this.uriFlags = uriFlags;
  }

  getURIFlags() {
    return this.uriFlags;
  }

  newChannel(uri, loadInfo) {
    const newURI = Services.io.newURI(this.chromeUrl);
    const channel = Services.io.newChannelFromURIWithLoadInfo(newURI, loadInfo);
    channel.originalURI = uri;

    if (this.uriFlags & Ci.nsIAboutModule.URI_SAFE_FOR_UNTRUSTED_CONTENT) {
      const principal = Services.scriptSecurityManager.createContentPrincipal(
        uri,
        {}
      );
      channel.owner = principal;
    }
    return channel;
  }
}
AboutMobile.prototype.QueryInterface = ChromeUtils.generateQI(["nsIAboutModule"]);
