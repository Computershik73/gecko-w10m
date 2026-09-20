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


// The autoconfig scope has no setTimeout; this is the same thing on an
// nsITimer, and it keeps the timer alive for as long as it is needed.
const GECKO_TIMERS = new Set();
function gecko_after(ms, fn, repeat) {
    const timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
    GECKO_TIMERS.add(timer);
    timer.initWithCallback({
        notify() {
            if (!repeat) {
                GECKO_TIMERS.delete(timer);
            }
            try {
                fn();
            } catch (e) {
                Services.console.logStringMessage("gecko: timer: " + e);
            }
        }
    }, ms, repeat ? Ci.nsITimer.TYPE_REPEATING_SLACK : Ci.nsITimer.TYPE_ONE_SHOT);
    return timer;
}

// What Firefox saves at shutdown, saved when the phone says the app is
// going to the background -- which here is also the only warning it gets
// before being ended. The shell holds the suspend for a couple of seconds
// after telling the engine; this is what those seconds are for.
function gecko_watch_app_state() {
    Services.obs.addObserver({
        observe() {
            const t0 = Date.now();
            const done = [];
            try {
                Services.prefs.savePrefFile(null);
                done.push("prefs");
            } catch (e) {
                done.push("prefs failed: " + e);
            }
            try {
                const { SessionSaver } = ChromeUtils.importESModule(
                    "resource:///modules/sessionstore/SessionSaver.sys.mjs");
                SessionSaver.run();
                done.push("session");
            } catch (e) {
                done.push("session failed: " + e);
            }
            try {
                Services.obs.notifyObservers(null, "memory-pressure", "heap-minimize");
                done.push("heap minimised");
            } catch (e) {
                done.push("heap-minimize failed: " + e);
            }
            Services.console.logStringMessage(
                "gecko: background -- flushed " + done.join(", ") + " in " +
                (Date.now() - t0) + " ms");
            gecko_note_memory("going to the background");
            if (!gecko_watch_app_state.dumped) {
                gecko_watch_app_state.dumped = true;
                gecko_note_memory_top("first time in the background");
            }
        }
    }, "application-background");
    Services.obs.addObserver({
        observe() {
            Services.console.logStringMessage("gecko: foreground again");
        }
    }, "application-foreground");
}

// Where the startup time goes. The device logs give one minute from
// XRE_main to the start page on the fastest phone and nothing in between;
// these are the marks Firefox itself keeps, written out as each is reached.
function gecko_note_phases() {
    const t0 = Date.now();
    const since = () => (Date.now() - t0) + " ms after autoconfig";
    const mods = () => Cu.loadedESModules.length + " modules";
    Services.console.logStringMessage(
        "gecko: phase autoconfig -- " + mods());
    const mark = topic => {
        Services.obs.addObserver({
            observe() {
                Services.obs.removeObserver(this, topic);
                Services.console.logStringMessage(
                    "gecko: phase " + topic + " -- " + since() + ", " + mods());
            }
        }, topic);
    };
    for (const topic of ["final-ui-startup", "sessionstore-windows-restored",
                         "browser-delayed-startup-finished",
                         "browser-idle-startup-tasks-finished"]) {
        mark(topic);
    }
    Services.obs.addObserver({
        observe() {
            Services.obs.removeObserver(this, "browser-idle-startup-tasks-finished");
            try {
                const info = Services.startup.getStartupInfo();
                const base = info.process;
                const rel = k => info[k] ? (info[k] - base) + " ms" : "never";
                Services.console.logStringMessage(
                    "gecko: startup timeline from process start: main " +
                    rel("main") + ", profile " + rel("selectProfile") +
                    ", profile locked " + rel("afterProfileLocked") +
                    ", first paint " + rel("firstPaint") +
                    ", session restored " + rel("sessionRestored"));
            } catch (e) {
                Services.console.logStringMessage("gecko: startup timeline: " + e);
            }
        }
    }, "browser-idle-startup-tasks-finished");
}

// The startup cache holds every chrome script already compiled. Whether it
// was found, whether it is being used and whether it gets written back is
// exactly the difference between a two-second launch and a forty-five second
// one, and no log so far has said which of the two this is.
function gecko_note_startup_cache(when) {
    try {
        const info = Cc["@mozilla.org/startupcacheinfo;1"]
            .getService(Ci.nsIStartupCacheInfo);
        let onDisk = "no file";
        try {
            const f = Cc["@mozilla.org/file/local;1"].createInstance(Ci.nsIFile);
            f.initWithPath(info.DiskCachePath);
            if (f.exists()) {
                onDisk = Math.round(f.fileSize / 1024) + " KB, written " +
                    new Date(f.lastModifiedTime).toISOString();
            }
        } catch (e2) {
            onDisk = "unreadable: " + e2;
        }
        Services.console.logStringMessage(
            "gecko: startup cache (" + when + "): found on init " +
            info.FoundDiskCacheOnInit + ", ignored " + info.IgnoreDiskCache +
            ", written this run " + info.WroteToDiskCache + ", at " +
            info.DiskCachePath + " -- " + onDisk);
    } catch (e) {
        Services.console.logStringMessage("gecko: startup cache: " + e);
    }
}

// What the engine is holding, in its own accounting: resident and virtual
// size, the heap, the JS heap and decoded images. The shell logs the ceiling
// the phone imposes; this is what is filling it.
function gecko_note_memory(when) {
    try {
        const mgr = Cc["@mozilla.org/memory-reporter-manager;1"]
            .getService(Ci.nsIMemoryReporterManager);
        const mb = v => Math.round(v / 1048576) + " MB";
        const read = (name) => {
            try { return mb(mgr[name]); } catch (e) { return "n/a"; }
        };
        Services.console.logStringMessage(
            "gecko: memory (" + when + "): resident " + read("resident") +
            ", virtual " + read("vsize") + ", heap " + read("heapAllocated") +
            ", js gc heap " + read("JSMainRuntimeGCHeap") +
            ", images " + read("imagesContentUsedUncompressed") +
            ", ghost windows " + (mgr.ghostWindows ?? "?"));
    } catch (e) {
        Services.console.logStringMessage("gecko: memory: " + e);
    }
}

// The largest things in memory, by the engine's own reports -- the same
// tree about:memory shows, reduced to the thirty entries that matter. The
// totals said 350 MB with a blank tab and 850 MB on YouTube; this says of
// what.
function gecko_note_memory_top(when) {
    try {
        const mgr = Cc["@mozilla.org/memory-reporter-manager;1"]
            .getService(Ci.nsIMemoryReporterManager);
        const rows = [];
        const handle = {
            callback(process, path, kind, units, amount) {
                // The explicit tree is what is allocated on purpose; the
                // address-space and vsize reports beside it only said how
                // much, never of what.
                if (units === Ci.nsIMemoryReporter.UNITS_BYTES &&
                    amount >= 2 * 1048576 && path.startsWith("explicit/")) {
                    rows.push([path, amount]);
                }
            }
        };
        const finish = {
            callback() {
                rows.sort((a, b) => b[1] - a[1]);
                // One line each: the device log keeps a line to about a
                // kilobyte, and a single message with thirty of them was cut
                // off after ten.
                Services.console.logStringMessage(
                    "gecko: memory top (" + when + "), " + rows.length +
                    " explicit entries over 2 MB:");
                for (const r of rows.slice(0, 30)) {
                    Services.console.logStringMessage(
                        "gecko: memory top: " + Math.round(r[1] / 1048576) +
                        " MB  " + r[0]);
                }
            }
        };
        mgr.getReports(handle, null, finish, null, false);
    } catch (e) {
        Services.console.logStringMessage("gecko: memory top: " + e);
    }
}

function gecko_watch_memory() {
    Services.obs.addObserver({
        observe() {
            Services.obs.removeObserver(this, "sessionstore-windows-restored");
            gecko_note_startup_cache("session restored");
            gecko_note_memory("session restored");
            // The cache is written a few seconds after the last script lands
            // in it; one more look says whether it got there.
            gecko_after(30000, () => gecko_note_startup_cache("30 s later"));
            gecko_after(60000, () => gecko_note_memory("periodic"), true);
            gecko_after(45000, () => gecko_note_memory_top("45 s after start"));
        }
    }, "sessionstore-windows-restored");
    Services.obs.addObserver({
        observe(subject, topic, data) {
            Services.console.logStringMessage(
                "gecko: memory-pressure (" + data + ")");
            gecko_note_memory("under pressure");
        }
    }, "memory-pressure");
}

// The guard that turned one phone's hardware compositing off for good after
// its user killed a slow launch. The shell now switches the guards off
// through the environment; the verdicts they left behind are cleared here so
// that phone gets its hardware back, and both facts are written down.
function gecko_reset_crash_guards() {
    try {
        const env = Services.env.get("MOZ_DISABLE_CRASH_GUARD");
        const branch = "gfx.crash-guard.";
        const left = Services.prefs.getChildList(branch)
            .filter(p => Services.prefs.prefHasUserValue(p))
            .map(p => p + "=" + Services.prefs.getIntPref(p, -1));
        Services.console.logStringMessage(
            "gecko: crash guards: MOZ_DISABLE_CRASH_GUARD=" +
            JSON.stringify(env) + ", verdicts on record: " +
            (left.length ? left.join(" ") : "none"));
        for (const p of Services.prefs.getChildList(branch)) {
            Services.prefs.clearUserPref(p);
        }
    } catch (e) {
        Services.console.logStringMessage("gecko: crash guards: " + e);
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
// The settings gear in the fullscreen player.
//
// Measured, not guessed: the tap reaches
// button.icon-button.player-settings-icon with its own click and touch
// listeners, the page hook is confirmed installed at the moment of the click,
// and the page answers with more mutations than an ordinary control tap
// makes. The handler runs. It just builds no sheet -- not in the document,
// not in any shadow root.
//
// Which is reasonable of it. The fullscreen element is
// div#player-container-id, and YouTube's settings sheet belongs to the app
// shell outside it, where fullscreen painting could never show it. Declining
// to open a sheet nobody could see is the right call for the site to make.
//
// So give it somewhere to be seen. The gear steps out of fullscreen first --
// the state where the sheet already opens -- and the player is one tap from
// going back in. Narrow on purpose: only the gear, only while fullscreen.
const GECKO_FULLSCREEN_MENU = `
(function () {
  var GEAR = ".player-settings-icon";
  document.addEventListener("click", function (ev) {
    if (!document.fullscreenElement) {
      return;
    }
    var gear;
    try {
      gear = ev.target && ev.target.closest && ev.target.closest(GEAR);
    } catch (e) {
      return;
    }
    if (!gear) {
      return;
    }
    // Replaying the tap after leaving fullscreen sent the player into a
    // storm -- thirty-eight thousand DOM changes and a script the browser
    // wanted to stop -- while the user's own next tap opened the sheet at
    // once. So: leave fullscreen, and let the next tap be theirs.
    ev.preventDefault();
    ev.stopImmediatePropagation();
    try {
      document.exitFullscreen();
    } catch (e3) {}
  }, true);
})();
`;

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

    const INJECTED = new WeakSet();

    const observer = {
        observe(subject, topic) {
            // document-element-inserted hands over the document, the other
            // hands over the window. Both land before any script in the page
            // runs; the difference is that by the first one there is a root
            // element, so anything the injected code wants to say about
            // itself has somewhere to go.
            let win = topic === "document-element-inserted"
                ? (subject && subject.defaultView) : subject;
            if (!win || INJECTED.has(win)) {
                return;
            }
            let host;
            try {
                host = win.location.hostname.toLowerCase();
            } catch (e) {
                return;
            }
            if (!matches(host)) {
                return;
            }
            if (!win.document || !win.document.documentElement) {
                // The other notification will come with a root element.
                return;
            }
            INJECTED.add(win);
            try {
                // A sandbox whose prototype is the window, with no Xrays, is
                // the page's own scope: what it assigns lands on the objects
                // the page's scripts will look at, and both notifications
                // are early enough that none of them have run.
                const sandbox = Cu.Sandbox(win, {
                    sandboxPrototype: win,
                    wantXrays: false,
                });
                Cu.evalInSandbox(GECKO_FULLSCREEN_MENU, sandbox);
                Cu.evalInSandbox(H264IFY_SOURCE, sandbox);
                Services.console.logStringMessage(
                    "gecko: page hooks injected into " + host);
            } catch (e) {
                Services.console.logStringMessage(
                    "gecko: h264ify failed on " + host + ": " + e);
            }
        }
    };
    Services.obs.addObserver(observer, "content-document-global-created");
    Services.obs.addObserver(observer, "document-element-inserted");
}

// What a tap actually lands on. Every control in the fullscreen player answers
// now except the settings gear, and the only way to tell "the tap missed it"
// from "the page did nothing with it" is to name the element it reached.
const GECKO_CLICK_WATCHED = new WeakSet();
const GECKO_PROBE_WATCHED = new WeakSet();

function gecko_watch_clicks(cw) {
    if (GECKO_CLICK_WATCHED.has(cw)) {
        return;
    }
    GECKO_CLICK_WATCHED.add(cw);
    const describe = el => {
        if (!el) {
            return "nothing";
        }
        let out = el.localName || "?";
        if (el.id) {
            out += "#" + el.id;
        }
        const cls = typeof el.className === "string" ? el.className : "";
        if (cls) {
            out += "." + cls.trim().split(/\s+/).slice(0, 3).join(".");
        }
        const label = el.getAttribute && (el.getAttribute("aria-label") ||
                                          el.getAttribute("title"));
        if (label) {
            out += " [" + label.slice(0, 40) + "]";
        }
        return out;
    };
    // Anything menu-, sheet-, scrim- or dialog-shaped, listed whether or not
    // it can be seen -- an element under an ancestor with display:none has no
    // box at all, and the first version of this dropped exactly those.
    const MENUISH =
        "[role=menu],[role=dialog],dialog,ytw-scrim,ytm-bottom-sheet-renderer," +
        "[class*=menu i],[class*=sheet i],[class*=scrim i],[class*=popup i]," +
        "[class*=dialog i]";

    const hiddenBy = el => {
        const win = el.ownerDocument.defaultView;
        for (let n = el; n && n.nodeType === 1; n = n.parentElement) {
            const st = win.getComputedStyle(n);
            if (st.display === "none") {
                return describe(n) + " has display:none";
            }
            if (st.visibility === "hidden" || st.visibility === "collapse") {
                return describe(n) + " has visibility:" + st.visibility;
            }
            if (st.opacity === "0") {
                return describe(n) + " has opacity:0";
            }
        }
        return "no ancestor hides it";
    };

    // querySelectorAll stops at a shadow boundary, and these components are
    // made of them, so walk the roots as well.
    const collect = root => {
        const out = [];
        const visit = r => {
            let all;
            try {
                all = r.querySelectorAll("*");
            } catch (e) {
                return;
            }
            for (const el of all) {
                if (el.matches && el.matches(MENUISH)) {
                    out.push(el);
                }
                if (el.shadowRoot) {
                    visit(el.shadowRoot);
                }
            }
        };
        visit(root);
        return out;
    };

    const snapshot = doc => new Set(collect(doc));

    const surveyMenus = (doc, before) => {
        const fs = doc.fullscreenElement;
        let listed = 0;
        for (const el of collect(doc)) {
            const r = el.getBoundingClientRect();
            const isNew = !before.has(el);
            // Everything that is new, and anything old that now has a box
            // bigger than a control -- the sheet we are looking for is one or
            // the other.
            if (!isNew && !(r.width > 120 && r.height > 120)) {
                continue;
            }
            if (++listed > 8) {
                break;
            }
            Services.console.logStringMessage(
                "gecko: menu " + (isNew ? "APPEARED " : "present ") +
                describe(el) + " box " + Math.round(r.left) + "," +
                Math.round(r.top) + " " + Math.round(r.width) + "x" +
                Math.round(r.height) +
                ", inside fullscreen element " +
                (fs ? fs.contains(el) : "no fullscreen") +
                (r.width && r.height ? "" : ", " + hiddenBy(el)));
        }
        if (!listed) {
            Services.console.logStringMessage(
                "gecko: menu -- the page added nothing and nothing grew " +
                "(shadow roots included)");
        }
    };

    cw.addEventListener("click", ev => {
        try {
            const doc = ev.target && ev.target.ownerDocument;
            const inFullscreen = !!(doc && doc.fullscreenElement);
            Services.console.logStringMessage(
                "gecko: click at " + Math.round(ev.clientX) + "," +
                Math.round(ev.clientY) + " on " + describe(ev.target) +
                ", fullscreen " + (inFullscreen ? "yes" : "no") +
                (inFullscreen
                     ? ", fullscreen element " + describe(doc.fullscreenElement)
                     : ""));
            try {
                const said =
                    doc.documentElement.getAttribute("data-gecko-probe");
                if (said) {
                    Services.console.logStringMessage(
                        "gecko: the page reported " + said.slice(0, 300));
                    doc.documentElement.removeAttribute("data-gecko-probe");
                }
            } catch (e8) {
                Services.console.logStringMessage("gecko: report read: " + e8);
            }

            if (inFullscreen) {
                // Everything at that point, topmost first. If a control does
                // nothing, what sits over it is the first thing to know.
                try {
                    const stack = doc.elementsFromPoint(ev.clientX, ev.clientY);
                    const win = doc.defaultView;
                    const parts = [];
                    for (const el of stack.slice(0, 6)) {
                        const st = win.getComputedStyle(el);
                        parts.push(describe(el) + " (" + st.position + ", z=" +
                                   st.zIndex + ", pointer-events=" +
                                   st.pointerEvents + ")");
                    }
                    Services.console.logStringMessage(
                        "gecko: stack at " + Math.round(ev.clientX) + "," +
                        Math.round(ev.clientY) + ", topmost first: " +
                        parts.join(" | "));
                } catch (e6) {
                    Services.console.logStringMessage(
                        "gecko: stack: " + e6);
                }

                // Which listeners the engine sees on the thing under the
                // finger, and on the few elements above it -- a click reaching
                // an element nobody is listening on goes nowhere, and that
                // cannot be seen from inside the page.
                try {
                    const els = Cc["@mozilla.org/eventlistenerservice;1"]
                        .getService(Ci.nsIEventListenerService);
                    const parts = [];
                    let el = ev.target;
                    for (let depth = 0; el && depth < 4; depth++) {
                        const types = [];
                        for (const info of els.getListenerInfoFor(el)) {
                            if (!info.inSystemEventGroup) {
                                types.push(info.type);
                            }
                        }
                        parts.push(describe(el) + " <" +
                                   (types.length ? types.join(",") : "none") +
                                   ">");
                        el = el.parentElement;
                    }
                    Services.console.logStringMessage(
                        "gecko: listeners " + parts.join(" << "));
                } catch (e7) {
                    Services.console.logStringMessage(
                        "gecko: listeners: " + e7);
                }

                const before = snapshot(doc);

                // Does the handler run at all? The button carries no aria
                // state to read, so watch the document instead: if anything
                // the press set in motion touches the page, it shows up here.
                let changes = 0;
                const samples = [];
                let observer = null;
                try {
                    observer = new cw.MutationObserver(records => {
                        for (const rec of records) {
                            changes++;
                            if (samples.length < 6) {
                                samples.push(
                                    rec.type === "attributes"
                                        ? describe(rec.target) + " @" +
                                          rec.attributeName
                                        : describe(rec.target) + " +" +
                                          rec.addedNodes.length + "/-" +
                                          rec.removedNodes.length);
                            }
                        }
                    });
                    observer.observe(doc, {
                        childList: true,
                        subtree: true,
                        attributes: true,
                        characterData: false,
                    });
                } catch (e4) {
                    Services.console.logStringMessage(
                        "gecko: mutation watch: " + e4);
                }
                cw.setTimeout(() => {
                    try {
                        if (observer) {
                            observer.disconnect();
                        }
                        Services.console.logStringMessage(
                            "gecko: page made " + changes +
                            " changes after the click" +
                            (samples.length ? ": " + samples.join("; ") : ""));
                        const said =
                            doc.documentElement.getAttribute("data-gecko-probe");
                        if (said) {
                            Services.console.logStringMessage(
                                "gecko: the page reported " + said.slice(0, 300));
                            doc.documentElement.removeAttribute(
                                "data-gecko-probe");
                        }
                    } catch (e5) {
                        Services.console.logStringMessage(
                            "gecko: mutation report: " + e5);
                    }
                }, 1000);
                cw.setTimeout(() => {
                    try {
                        surveyMenus(doc, before);
                    } catch (e2) {
                        Services.console.logStringMessage(
                            "gecko: menu survey: " + e2);
                    }
                }, 600);
            }
        } catch (e) {
            Services.console.logStringMessage("gecko: click watch: " + e);
        }
    }, true);
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
        // The browser's own half of DOM fullscreen, which the actor chain does
        // not reach without content processes. Harmless when it already ran:
        // both calls are the ones Firefox makes itself.
        const runChromeHalf = entering => {
            try {
                const root = doc.documentElement;
                if (entering === root.hasAttribute("inDOMFullscreen")) {
                    return;
                }
                const browser = win.gBrowser && win.gBrowser.selectedBrowser;
                const global = browser && browser.browsingContext &&
                               browser.browsingContext.currentWindowGlobal;
                const actor = global && global.getActor("DOMFullscreen");
                if (!browser || !actor) {
                    return;
                }
                if (entering) {
                    win.FullScreen.enterDomFullscreen(browser, actor);
                } else {
                    win.FullScreen.cleanupDomFullscreen(actor);
                }
                Services.console.logStringMessage(
                    "gecko: ran the chrome half of fullscreen (" +
                    (entering ? "enter" : "exit") + "), inDOMFullscreen now " +
                    root.hasAttribute("inDOMFullscreen"));
            } catch (e) {
                Services.console.logStringMessage(
                    "gecko: chrome half of fullscreen failed: " + e);
            }
        };

        for (const name of EVENTS) {
            win.addEventListener(name, () => {
                if (name === "MozDOMFullscreen:Entered") {
                    runChromeHalf(true);
                } else if (name === "MozDOMFullscreen:Exited") {
                    runChromeHalf(false);
                }
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
                // And what the page itself believes, which is the only thing
                // that decides where the video sits and where a tap lands.
                try {
                    const cw = win.gBrowser.selectedBrowser.contentWindow;
                    const cd = cw.document;
                    const fs = cd.fullscreenElement;
                    const r = fs ? fs.getBoundingClientRect() : null;
                    const vv = cw.visualViewport;
                    Services.console.logStringMessage(
                        "gecko: page viewport " + cw.innerWidth + "x" +
                        cw.innerHeight + " at dpr " + cw.devicePixelRatio +
                        ", visual " + (vv ? Math.round(vv.width) + "x" +
                                            Math.round(vv.height) + " offset " +
                                            Math.round(vv.offsetLeft) + "," +
                                            Math.round(vv.offsetTop) +
                                            " scale " + vv.scale
                                          : "none") +
                        ", document " + cd.documentElement.clientWidth + "x" +
                        cd.documentElement.clientHeight +
                        ", fullscreen element " +
                        (fs ? fs.localName + " " + Math.round(r.left) + "," +
                              Math.round(r.top) + " " + Math.round(r.width) +
                              "x" + Math.round(r.height)
                            : "none"));
                    // The page laid itself out while the window was
                    // changing size and will not measure again on its own.
                    // Two nudges, once the transition has settled.
                    for (const delay of [300, 1200]) {
                        win.setTimeout(() => {
                            try {
                                cw.dispatchEvent(new cw.Event("resize"));
                            } catch (e2) {
                                Services.console.logStringMessage(
                                    "gecko: resize nudge failed: " + e2);
                            }
                        }, delay);
                    }
                    gecko_watch_clicks(cw);
                    if (!GECKO_PROBE_WATCHED.has(cw)) {
                        GECKO_PROBE_WATCHED.add(cw);
                        // The fourth argument is the point: an event a page
                        // dispatches is untrusted, and chrome does not hear
                        // those unless it says so.
                        cw.addEventListener("gecko-probe", pe => {
                            Services.console.logStringMessage(
                                "gecko: " + String(pe.detail).slice(0, 200));
                        }, true, true);
                    }
                } catch (e) {
                    Services.console.logStringMessage(
                        "gecko: page viewport unavailable: " + e);
                }
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
    gecko_note_phases();
    gecko_fix_homepage();
    gecko_note_startup();
    gecko_reset_crash_guards();
    gecko_watch_memory();
    gecko_watch_app_state();
    gecko_watch_fullscreen();
    gecko_h264ify();
    gecko_watch_tabs();
    delete_old_mcf_files();

    // Upstream clears the startup cache on every launch, so that edits to
    // boot.sys.mjs take effect on a development machine. Here nothing under
    // the package can change between launches -- only a new package can
    // change it -- so the cache is cleared once per package version and kept
    // otherwise. Cleared every time, every launch recompiled every chrome
    // script from source.
    const built = Services.prefs.getStringPref("gecko.port.version", "?");
    const cached = Services.prefs.getStringPref("gecko.cache.builtFor", "");
    if (cached !== built) {
        Services.appinfo.invalidateCachesOnRestart();
        Services.prefs.setStringPref("gecko.cache.builtFor", built);
        Services.console.logStringMessage(
            "gecko: startup cache will be rebuilt: package " + built +
            ", cache was for " + (cached || "nothing"));
    } else {
        Services.console.logStringMessage(
            "gecko: startup cache kept for package " + built);
    }

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
