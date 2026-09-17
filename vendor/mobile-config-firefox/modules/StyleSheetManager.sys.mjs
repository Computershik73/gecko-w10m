// Copyright 2025 Danny Colin
// SPDX-License-Identifier: MPL-2.0
const { AppConstants } =
    ChromeUtils.importESModule("resource://gre/modules/AppConstants.sys.mjs");

export class StyleSheetManager {
    constructor() {
        try {
            const sss =
                Cc["@mozilla.org/content/style-sheet-service;1"].getService(
                    Ci.nsIStyleSheetService
                );

            const sharedStyleURI = Services.io.newURI(
                "chrome://mobileconfigfirefox/content/themes/shared/main.css"
            );
            sss.loadAndRegisterSheet(
                sharedStyleURI,
                Ci.nsIStyleSheetService.USER_SHEET
            );

            let channelStyleURI;
            if (AppConstants.IS_ESR) {
                channelStyleURI = Services.io.newURI(
                    `chrome://mobileconfigfirefox/content/themes/esr/main.css`
                );
            } else {
                channelStyleURI = Services.io.newURI(
                    `chrome://mobileconfigfirefox/content/themes/release/main.css`
                );
            }
            sss.loadAndRegisterSheet(
                channelStyleURI,
                Ci.nsIStyleSheetService.USER_SHEET
            );
        } catch (e) {
            console.error("Error adding dynamic stylesheet:", e);
        }
    }
}
