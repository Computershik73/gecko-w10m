// Copyright 2025 Arnaud Ferraris, Danny Colin, Oliver Smith
// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const { classes: Cc, interfaces: Ci, utils: Cu } = Components;
const Services = globalThis.Services;
const { AppConstants } =
    ChromeUtils.importESModule("resource://gre/modules/AppConstants.sys.mjs");
const IS_ESM_READY = parseInt(AppConstants.MOZ_APP_VERSION, 10) >= 128;
const { FileUtils } =
    IS_ESM_READY
      ? ChromeUtils.importESModule("resource://gre/modules/FileUtils.sys.mjs")
      : Cu.import("resource://gre/modules/FileUtils.jsm");

var g_chromeDir; // nsIFile object for the "chrome" dir in user's profile
var g_logFileStream;


function write_line(ostream, line) {
    line = line + "\n"
    ostream.write(line, line.length);
}

// Create <profile>/chrome/ directory if not already present
function chrome_dir_init() {
    g_chromeDir = Services.dirsvc.get("ProfD", Ci.nsIFile);
    g_chromeDir.append("chrome");
    if (!g_chromeDir.exists()) {
        g_chromeDir.create(Ci.nsIFile.DIRECTORY_TYPE, FileUtils.PERMS_DIRECTORY);
    }
}

function log_init() {
    var mode = FileUtils.MODE_WRONLY | FileUtils.MODE_CREATE | FileUtils.MODE_APPEND;
    var logFile = g_chromeDir.clone();
    logFile.append("mobile-config-firefox.log");
    g_logFileStream = FileUtils.openFileOutputStream(logFile, mode);
}

function log(line) {
    var date = new Date().toISOString().replace("T", " ").slice(0, 19);
    line = "[" + date + "] " + line;
    write_line(g_logFileStream, line);
}

function is_css_file_from_old_mcf(css_file) {
        var istream = Cc["@mozilla.org/network/file-input-stream;1"].
                      createInstance(Components.interfaces.nsIFileInputStream);
        istream.init(css_file, 0x01, 0444, 0);
        istream.QueryInterface(Components.interfaces.nsILineInputStream);

        var has_more;
        var is_from_mcf = false;
        do {
            var line = {};
            has_more = istream.readLine(line);
            if (line.value.includes("WARNING: DO NOT EDIT THE COPY OF THIS FILE LOCATED IN YOUR USER PROFILE!")) {
                is_from_mcf = true;
                break;
            }
        } while (has_more);
        istream.close();

        return is_from_mcf;
}

function delete_old_mcf_files() {
    /* mobile-config-firefox prior to 5.0.0 wrote userChrome.css and
     * userContent.css files into ~/.mozilla/firefox/<profile>/chrome/. This is
     * not necessary anymore and still having these old files with previous
     * customizations from MCF causes problems. Remove them. */
    var names = ["userChrome", "userContent"];

    for (var i in names) {
        var name = names[i];
        var css_file = g_chromeDir.clone()
        css_file.append(name + ".css");

        if (css_file.exists() && is_css_file_from_old_mcf(css_file)) {
            log("Cleaning up CSS file from old mobile-config-firefox: " + name + ".css");
            css_file.remove(false);
        }
    }
}

// Distributions install it below /usr/lib, but that path is not visible
// inside the sandbox of a flatpak'd Firefox, where the files are shipped as
// an extension mounted into /app/etc/firefox instead.
// GeckoW10m: the files ship inside the app package, next to the engine.
function find_chrome_manifest() {
    const f = Services.dirsvc.get("GreD", Ci.nsIFile);
    f.append("mobile-config-firefox");
    f.append("chrome.manifest");
    if (f.exists())
        return f;
    log("chrome.manifest not found at: " + f.path);
    return null;
}

chrome_dir_init();
log_init();

try {
    delete_old_mcf_files();

    // Firefox is caching some files to make the startup time faster. We need to
    // clear the startup cache for the changes in boot.sys.mjs to take effect.
    //
    // TODO:
    // - Find a solution to only trigger a cache clearing when the source files
    //   have changed.
    Services.appinfo.invalidateCachesOnRestart();

    // nsIFile of chrome.manifest, so it can be consumed by autoRegister below.
    const chromeManifest = find_chrome_manifest();

    if(chromeManifest){
        log("Loading mobile-config-firefox from: " + chromeManifest.parent.path);
        Components.manager.QueryInterface(Ci.nsIComponentRegistrar)
            .autoRegister(chromeManifest);
        ChromeUtils.importESModule(
            'chrome://mobileconfigfirefox/content/boot.sys.mjs'
        );
    }
} catch(e) {
    log("mobile-config-autoconfig failed: " + e);
    // console.* isn't defined in autoconfig. We can use
    // Components.utils.reportError() for error messages or
    // Service.console.logStringMessage() for regular log messages.
    Cu.reportError(`Mobile Config Firefox: ${e}`);
};
g_logFileStream.close();
