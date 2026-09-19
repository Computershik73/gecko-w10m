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
// Port: the files ship inside the app package, next to the engine.
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

// Port: older builds pinned the home page to about:home through an
// enterprise policy. The policy is gone, but a profile made under it can still
// carry that value, and then the default (google.com) never shows. Clear it
// once, and only when it is the page the policy set -- a page the user chose
// is left alone.
function gecko_fix_homepage() {
    const MARKER = "gecko.homepage.migrated";
    if (Services.prefs.getBoolPref(MARKER, false)) {
        return;
    }
    Services.prefs.setBoolPref(MARKER, true);
    if (!Services.prefs.prefHasUserValue("browser.startup.homepage")) {
        return;
    }
    const current = Services.prefs.getStringPref("browser.startup.homepage", "");
    if (current === "about:home" || current === "about:blank" || current === "") {
        Services.prefs.clearUserPref("browser.startup.homepage");
        log("cleared a home page left over from the old policy: " + current);
    }
}

// And a line in the log saying what the browser will actually open, so a
// start page that is not what was asked for can be read off a device log.
function gecko_note_startup() {
    const homepage = Services.prefs.getStringPref("browser.startup.homepage", "?");
    const page = Services.prefs.getIntPref("browser.startup.page", -1);
    const resume = Services.prefs.getBoolPref("browser.sessionstore.resume_from_crash", true);
    Services.console.logStringMessage(
        "gecko: start page " + homepage + ", browser.startup.page " + page +
        ", resume_from_crash " + resume);
}

// h264ify, without the extension.
//
// The extension does not touch the browser's codec support at all -- it hooks
// MediaSource.isTypeSupported, HTMLMediaElement.canPlayType and
// mediaCapabilities.decodingInfo inside the page and answers "no" for VP8, VP9
// and AV1. YouTube then picks H.264 from a ladder it was going to offer
// anyway, which is why h264ify users get ordinary videos and not an error.
//
// Turning the codecs off with prefs looks like the same thing from the page's
// side, but it is browser-wide: every other site loses them too, with nothing
// to fall back to. So the prefs are back to what Firefox ships and this does
// the refusing, on the hosts named by gecko.h264ify.hosts and nowhere else.
const H264IFY_SOURCE = `
(function () {
  var BAD = ["vp8", "vp9", "vp08", "vp09", "av01", "av1"];
  function blocked(type) {
    if (typeof type !== "string") { return false; }
    var t = type.toLowerCase();
    for (var i = 0; i < BAD.length; i++) {
      if (t.indexOf(BAD[i]) !== -1) { return true; }
    }
    return false;
  }
  if (window.MediaSource && MediaSource.isTypeSupported) {
    var wasTypeSupported = MediaSource.isTypeSupported.bind(MediaSource);
    MediaSource.isTypeSupported = function (type) {
      return blocked(type) ? false : wasTypeSupported(type);
    };
  }
  var proto = window.HTMLMediaElement && HTMLMediaElement.prototype;
  if (proto && proto.canPlayType) {
    var wasCanPlayType = proto.canPlayType;
    proto.canPlayType = function (type) {
      return blocked(type) ? "" : wasCanPlayType.call(this, type);
    };
  }
  var caps = navigator.mediaCapabilities;
  if (caps && caps.decodingInfo) {
    var wasDecodingInfo = caps.decodingInfo.bind(caps);
    caps.decodingInfo = function (config) {
      var type = config && config.video && config.video.contentType;
      if (blocked(type)) {
        return Promise.resolve({
          supported: false, smooth: false, powerEfficient: false,
          configuration: config,
        });
      }
      return wasDecodingInfo(config);
    };
  }
})();
`;

// What the tabs actually are, a few seconds after a window opens. The first
// tab after a crash loads forever and comes right the moment it is closed, and
// which of these fields is wrong says why: a tab left pending by session
// restore, a browser with no docShell, a load that was started and never
// finished, or a remoteness the frame loader could not honour.
function gecko_watch_tabs() {
    function dump(win) {
        const gb = win.gBrowser;
        if (!gb) {
            return;
        }
        const say = m => Services.console.logStringMessage("gecko: " + m);
        say("tabs: " + gb.tabs.length + " open, selected index " +
            gb.tabs.indexOf(gb.selectedTab));
        for (let i = 0; i < gb.tabs.length; i++) {
            const tab = gb.tabs[i];
            const b = tab.linkedBrowser;
            let uri = "?";
            try {
                uri = b.currentURI ? b.currentURI.spec : "none";
            } catch (e) {
                uri = "threw: " + e;
            }
            let loading = "?";
            try {
                loading = String(b.webProgress &&
                                 b.webProgress.isLoadingDocument);
            } catch (e) {
                loading = "threw";
            }
            say("tab " + i + ": " + uri.slice(0, 90) +
                " | pending " + !!tab.getAttribute("pending") +
                " | remote attr " + b.hasAttribute("remote") +
                " | isRemoteBrowser " + b.isRemoteBrowser +
                " | docShell " + !!b.docShell +
                " | contentWindow " + !!b.contentWindow +
                " | currentWindowGlobal " +
                !!(b.browsingContext && b.browsingContext.currentWindowGlobal) +
                " | loading " + loading +
                " | userTypedValue " + (b.userTypedValue || "none"));
        }
    }

    Services.obs.addObserver({
        observe(subject, topic) {
            if (topic !== "domwindowopened") {
                return;
            }
            const win = subject;
            win.addEventListener("load", () => {
                if (win.document.location.href !==
                    "chrome://browser/content/browser.xhtml") {
                    return;
                }
                // Twice: once when the window has settled, once after the load
                // it started has had time to finish or to hang.
                for (const delay of [4000, 15000]) {
                    win.setTimeout(() => {
                        try {
                            dump(win);
                        } catch (e) {
                            Services.console.logStringMessage(
                                "gecko: tabs: " + e);
                        }
                    }, delay);
                }
            }, { once: true });
        }
    }, "domwindowopened");
}

function gecko_h264ify() {
    const setting = Services.prefs.getStringPref(
        "gecko.h264ify.hosts", "youtube.com,youtube-nocookie.com");
    const hosts = setting.split(",").map(h => h.trim().toLowerCase())
                         .filter(h => h.length);
    if (!hosts.length) {
        return;
    }
    const matches = host =>
        hosts.some(h => host === h || host.endsWith("." + h));

    Services.obs.addObserver({
        observe(subject) {
            let win = subject;
            let host;
            try {
                host = win.location.hostname.toLowerCase();
            } catch (e) {
                return;
            }
            if (!matches(host)) {
                return;
            }
            try {
                // A sandbox whose prototype is the window, with no Xrays, is
                // the page's own scope: what it assigns lands on the objects
                // the page's scripts will look at. "content-document-global-
                // created" is early enough that they have not run yet.
                const sandbox = Cu.Sandbox(win, {
                    sandboxPrototype: win,
                    wantXrays: false,
                });
                Cu.evalInSandbox(H264IFY_SOURCE, sandbox);
            } catch (e) {
                Services.console.logStringMessage(
                    "gecko: h264ify failed on " + host + ": " + e);
            }
        }
    }, "content-document-global-created");
}

// Every fullscreen transition, written down. Which of these arrive says where
// the chain breaks: MozDOMFullscreen:Entered is the chrome event the actors
// listen for, inDOMFullscreen is the attribute whose absence leaves the
// toolbar on screen, and the toolbox height says whether it actually went.
function gecko_watch_fullscreen() {
    const EVENTS = ["MozDOMFullscreen:Entered", "MozDOMFullscreen:Exited",
                    "fullscreenchange", "fullscreenerror"];
    function watch(win) {
        const doc = win.document;
        if (doc.location.href !== "chrome://browser/content/browser.xhtml") {
            return;
        }
        for (const name of EVENTS) {
            win.addEventListener(name, () => {
                const toolbox = doc.getElementById("navigator-toolbox");
                Services.console.logStringMessage(
                    "gecko: fullscreen " + name +
                    " -- chrome element " +
                    (doc.fullscreenElement ? doc.fullscreenElement.localName
                                           : "none") +
                    ", inDOMFullscreen " +
                    doc.documentElement.hasAttribute("inDOMFullscreen") +
                    ", window.fullScreen " + win.fullScreen +
                    ", toolbox " +
                    (toolbox ? Math.round(
                         toolbox.getBoundingClientRect().height) : "?") +
                    "px, window " + win.innerWidth + "x" + win.innerHeight +
                    // enterDomFullscreen gives up without a word if either of
                    // these is wrong, and inDOMFullscreen stays unset -- which
                    // is what the last log showed.
                    ", focus.activeWindow is us " +
                    (Services.focus.activeWindow === win) +
                    ", selectedBrowser matches " +
                    (win.gBrowser &&
                     win.gBrowser.selectedBrowser === doc.fullscreenElement));
            }, true);
        }
    }
    Services.obs.addObserver({
        observe(subject, topic) {
            if (topic !== "domwindowopened") {
                return;
            }
            const win = subject;
            win.addEventListener("load", () => {
                try {
                    watch(win);
                } catch (e) {
                    Services.console.logStringMessage(
                        "gecko: fullscreen watch failed: " + e);
                }
            }, { once: true });
        }
    }, "domwindowopened");
}

try {
    gecko_fix_homepage();
    gecko_note_startup();
    gecko_watch_fullscreen();
    gecko_h264ify();
    gecko_watch_tabs();
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
