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
const GECKO_HANDLER_PROBE = `
(function () {
  var say = function (m) {
    try {
      window.dispatchEvent(new CustomEvent("gecko-probe", { detail: m }));
    } catch (e) {}
    // And on the document, where nothing can be lost in translation.
    try {
      var root = document.documentElement;
      var kept = (root.getAttribute("data-gecko-probe") || "").split(" || ");
      kept.push(m);
      while (kept.length > 6) { kept.shift(); }
      root.setAttribute("data-gecko-probe", kept.join(" || "));
    } catch (e2) {}
  };
  var WATCHED = ["player-settings-icon", "player-control-play-pause-icon"];
  var nameOf = function (t) {
    try {
      if (!t || !t.classList) { return null; }
      for (var i = 0; i < WATCHED.length; i++) {
        if (t.classList.contains(WATCHED[i])) { return WATCHED[i]; }
      }
    } catch (e) {}
    return null;
  };
  // The map keeps removeEventListener working: it is handed the original
  // function and has to find the wrapper that was actually registered.
  var wrappers = new WeakMap();
  var add = EventTarget.prototype.addEventListener;
  var remove = EventTarget.prototype.removeEventListener;
  say("installed, addEventListener is " +
      (typeof add === "function" ? "wrappable" : "missing"));

  EventTarget.prototype.addEventListener = function (type, fn, opts) {
    if (typeof fn !== "function" || (type !== "click" && type !== "touchend")) {
      return add.call(this, type, fn, opts);
    }
    var wrapped = wrappers.get(fn);
    if (!wrapped) {
      wrapped = function (ev) {
        var which = nameOf(this);
        if (!which) { return fn.apply(this, arguments); }
        say("handler " + ev.type + " on " + which + " entered");
        try {
          var r = fn.apply(this, arguments);
          say("handler " + ev.type + " on " + which + " returned normally");
          return r;
        } catch (err) {
          say("handler " + ev.type + " on " + which + " THREW " + err);
          throw err;
        }
      };
      wrappers.set(fn, wrapped);
    }
    return add.call(this, type, wrapped, opts);
  };

  EventTarget.prototype.removeEventListener = function (type, fn, opts) {
    var wrapped = typeof fn === "function" ? wrappers.get(fn) : null;
    return remove.call(this, type, wrapped || fn, opts);
  };
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
                Cu.evalInSandbox(GECKO_HANDLER_PROBE, sandbox);
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
                const page = Cu.waiveXrays(doc.defaultView);
                const fn = page.EventTarget.prototype.addEventListener;
                Services.console.logStringMessage(
                    "gecko: the page's addEventListener is " +
                    (fn && fn.name ? fn.name : "unnamed") +
                    (String(fn).includes("[native code]")
                        ? " (native, our patch is not there)"
                        : " (patched)"));
            } catch (e9) {
                Services.console.logStringMessage(
                    "gecko: could not look at addEventListener: " + e9);
            }

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
