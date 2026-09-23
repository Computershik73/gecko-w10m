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
    // Between final-ui-startup and browser-delayed-startup-finished the
    // window is built and two hundred modules load, three and a half
    // seconds on the fastest phone. Their names, once, say which features
    // those are -- and which a phone could do without.
    let atUiStartup = null;
    const mark = topic => {
        Services.obs.addObserver({
            observe() {
                Services.obs.removeObserver(this, topic);
                Services.console.logStringMessage(
                    "gecko: phase " + topic + " -- " + since() + ", " + mods());
                if (topic === "final-ui-startup") {
                    atUiStartup = new Set(Cu.loadedESModules);
                } else if (topic === "browser-delayed-startup-finished" &&
                           atUiStartup) {
                    const fresh = Cu.loadedESModules
                        .filter(m => !atUiStartup.has(m))
                        .map(m => m.replace(/^resource:\/\/gre\/modules\//, "gre:")
                                   .replace(/^resource:\/\/\/modules\//, "app:")
                                   .replace(/^moz-src:\/\/\//, "src:")
                                   .replace(/\.sys\.mjs$/, ""));
                    for (let i = 0; i < fresh.length; i += 8) {
                        Services.console.logStringMessage(
                            "gecko: loaded while building the window: " +
                            fresh.slice(i, i + 8).join(" "));
                    }
                }
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


// PlayReady for one site, through the phone.
//
// Stage one: license acquisition only. The page sees a working
// com.microsoft.playready key system -- requestMediaKeySystemAccess,
// MediaKeys, MediaKeySession with generateRequest/update -- whose challenge
// comes from the phone's own PlayReady and whose license response goes back
// into it. Nothing here decrypts or plays; that is stage two, and it is only
// worth building if this stage shows the site issuing a license to this
// phone at all. Everything the page does with the key system is written to
// the device log.
const SPOTIFY_EME_SOURCE = `
(function () {
  var KS = "com.microsoft.playready";
  var UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36 Edg/126.0.0.0";
  var nextId = 1, pending = {};
  var say = function (m) {
    try { window.dispatchEvent(new CustomEvent("gecko-drm", { detail: JSON.stringify({ log: String(m).slice(0, 600) }) })); } catch (e) {}
  };
  var send = function (msg) {
    return new Promise(function (resolve, reject) {
      var id = nextId++;
      msg.id = id;
      pending[id] = { resolve: resolve, reject: reject };
      window.dispatchEvent(new CustomEvent("gecko-drm", { detail: JSON.stringify(msg) }));
    });
  };
  window.addEventListener("gecko-drm-reply", function (ev) {
    var r;
    try { r = JSON.parse(ev.detail); } catch (e) { return; }
    var p = pending[r.id];
    if (!p) { return; }
    delete pending[r.id];
    if (r.ok) { p.resolve(r); } else { p.reject(new DOMException(r.error || "PlayReady failure", "InvalidStateError")); }
  });
  var toB64 = function (data) {
    var u8 = data instanceof ArrayBuffer ? new Uint8Array(data)
           : ArrayBuffer.isView(data) ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
           : new Uint8Array(0);
    var s = "";
    for (var i = 0; i < u8.length; i += 8192) { s += String.fromCharCode.apply(null, u8.subarray(i, i + 8192)); }
    return btoa(s);
  };
  var fromB64 = function (b64) {
    var s = atob(b64), u8 = new Uint8Array(s.length);
    for (var i = 0; i < s.length; i++) { u8[i] = s.charCodeAt(i); }
    return u8.buffer;
  };
  var fire = function (target, type, props) {
    var ev = new Event(type);
    for (var k in props) { try { Object.defineProperty(ev, k, { value: props[k], enumerable: true }); } catch (e) {} }
    var handler = target["on" + type];
    if (typeof handler === "function") { try { handler.call(target, ev); } catch (e) { say("on" + type + " threw: " + e); } }
    target.dispatchEvent(ev);
  };
  var makeStatusMap = function () {
    var m = new Map();
    return {
      _map: m,
      get size() { return m.size; },
      get: function (k) { return m.get(String(k)); },
      has: function (k) { return m.has(String(k)); },
      keys: function () { return m.keys(); },
      values: function () { return m.values(); },
      entries: function () { return m.entries(); },
      forEach: function (fn, thisArg) { m.forEach(function (v, k) { fn.call(thisArg, v, k, this); }, this); },
      [Symbol.iterator]: function () { return m.entries(); }
    };
  };

  class Session extends EventTarget {
    constructor(type) {
      super();
      this.sessionId = "pr-" + Math.random().toString(36).slice(2);
      this.expiration = NaN;
      this.keyStatuses = makeStatusMap();
      this.onmessage = null;
      this.onkeystatuseschange = null;
      var self = this;
      this.closed = new Promise(function (res) { self._resolveClosed = res; });
      say("createSession(" + type + ") -> " + this.sessionId);
    }
    generateRequest(initDataType, initData) {
      var self = this;
      var bytes = initData instanceof ArrayBuffer ? initData.byteLength : (initData && initData.byteLength) || 0;
      say("generateRequest(" + initDataType + ", " + bytes + " bytes) on " + self.sessionId);
      return send({ op: "drm.challenge", session: self.sessionId, initDataType: initDataType, initData: toB64(initData) })
        .then(function (r) {
          var buf = fromB64(r.challenge);
          say("the phone made a " + buf.byteLength + "-byte challenge; its license URI " + (r.uri || "(none)"));
          fire(self, "message", { messageType: "license-request", message: buf });
        }, function (e) { say("challenge FAILED: " + e.message); throw e; });
    }
    update(response) {
      var self = this;
      var bytes = response instanceof ArrayBuffer ? response.byteLength : (response && response.byteLength) || 0;
      say("update(" + bytes + " bytes) on " + self.sessionId);
      return send({ op: "drm.response", session: self.sessionId, license: toB64(response) })
        .then(function () {
          say("the phone ACCEPTED the license for " + self.sessionId);
          self.keyStatuses._map.set("gecko-key", "usable");
          fire(self, "keystatuseschange", {});
        }, function (e) { say("the phone REJECTED the license: " + e.message); throw e; });
    }
    load() { say("load on " + this.sessionId); return Promise.resolve(false); }
    close() { say("close " + this.sessionId); this._resolveClosed(); return Promise.resolve(); }
    remove() { say("remove " + this.sessionId); return Promise.resolve(); }
  }

  class Keys {
    createSession(type) { return new Session(type || "temporary"); }
    setServerCertificate(cert) { say("setServerCertificate(" + ((cert && cert.byteLength) || 0) + " bytes)"); return Promise.resolve(true); }
    getStatusForPolicy() { return Promise.resolve("usable"); }
  }

  var makeAccess = function (keySystem, config) {
    return {
      keySystem: keySystem,
      getConfiguration: function () { return config; },
      createMediaKeys: function () { say("createMediaKeys for " + keySystem); return Promise.resolve(new Keys()); }
    };
  };
  var pickConfig = function (configs) {
    var c = (configs && configs[0]) ? JSON.parse(JSON.stringify(configs[0])) : {};
    c.label = c.label || "";
    c.initDataTypes = c.initDataTypes && c.initDataTypes.length ? c.initDataTypes : ["cenc", "keyids"];
    c.audioCapabilities = c.audioCapabilities || [];
    c.videoCapabilities = c.videoCapabilities || [];
    c.distinctiveIdentifier = "not-allowed";
    c.persistentState = c.persistentState === "required" ? "required" : "not-allowed";
    c.sessionTypes = c.sessionTypes && c.sessionTypes.length ? c.sessionTypes : ["temporary"];
    return c;
  };

  navigator.requestMediaKeySystemAccess = function (keySystem, configs) {
    say("requestMediaKeySystemAccess(" + keySystem + ", " + JSON.stringify(configs).slice(0, 400) + ")");
    if (/playready/i.test(keySystem)) {
      return Promise.resolve(makeAccess(keySystem, pickConfig(configs)));
    }
    return Promise.reject(new DOMException("Unsupported keySystem or supportedConfigurations.", "NotSupportedError"));
  };
  if (navigator.mediaCapabilities && navigator.mediaCapabilities.decodingInfo) {
    var realDecodingInfo = navigator.mediaCapabilities.decodingInfo.bind(navigator.mediaCapabilities);
    navigator.mediaCapabilities.decodingInfo = function (config) {
      var ksc = config && config.keySystemConfiguration;
      if (ksc) {
        say("decodingInfo with keySystem " + ksc.keySystem);
        if (/playready/i.test(ksc.keySystem)) {
          return Promise.resolve({ supported: true, smooth: true, powerEfficient: true,
                                   keySystemAccess: makeAccess(ksc.keySystem, pickConfig([{}])) });
        }
        return Promise.resolve({ supported: false, smooth: false, powerEfficient: false });
      }
      return realDecodingInfo(config);
    };
  }
  if (window.MediaSource && MediaSource.isTypeSupported) {
    var realIsTypeSupported = MediaSource.isTypeSupported.bind(MediaSource);
    MediaSource.isTypeSupported = function (type) {
      var r = realIsTypeSupported(type);
      say("MediaSource.isTypeSupported(" + type + ") -> " + r);
      return r;
    };
  }
  var mediaProto = HTMLMediaElement.prototype;
  mediaProto.setMediaKeys = function (keys) {
    say("setMediaKeys(" + (keys ? "keys" : "null") + ") on <" + this.tagName.toLowerCase() + ">");
    this._geckoMediaKeys = keys || null;
    return Promise.resolve();
  };
  Object.defineProperty(mediaProto, "mediaKeys", { get: function () { return this._geckoMediaKeys || null; }, configurable: true });
  Object.defineProperty(navigator, "userAgent", { get: function () { return UA; }, configurable: true });
  Object.defineProperty(navigator, "appVersion", { get: function () { return UA.slice(8); }, configurable: true });
  Object.defineProperty(navigator, "vendor", { get: function () { return "Google Inc."; }, configurable: true });
  say("PlayReady key system offered to the page");
})();
`;

function gecko_spotify() {
    const setting = Services.prefs.getStringPref("gecko.spotify.hosts", "open.spotify.com");
    const hosts = setting.split(",").map(h => h.trim().toLowerCase()).filter(h => h.length);
    if (!hosts.length) {
        return;
    }
    const matches = host => hosts.some(h => host === h || host.endsWith("." + h));
    // The player does its EME from a frame on another host: the first run
    // logged seven real requestMediaKeySystemAccess calls, all refused, from a
    // window the hook never saw, and then "No such device". So every frame
    // whose top document is on a Spotify host is hooked, whatever its own.
    const underSpotify = win => {
        try {
            if (matches(win.location.hostname.toLowerCase())) return true;
            const top = win.top;
            return top && top !== win && matches(top.location.hostname.toLowerCase());
        } catch (e) {
            return false;
        }
    };
    // The header the site sees, for every request to it and its CDN: the
    // page-side navigator.userAgent says the same, so the two agree.
    const UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " +
               "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36 Edg/126.0.0.0";
    const uaHosts = host => host.endsWith("spotify.com") || host.endsWith("scdn.co") ||
                            host.endsWith("spotifycdn.com");
    const log = m => Services.console.logStringMessage("gecko: drm: " + m);

    Services.obs.addObserver({
        observe(subject) {
            try {
                const channel = subject.QueryInterface(Ci.nsIHttpChannel);
                if (uaHosts(channel.URI.host.toLowerCase())) {
                    channel.setRequestHeader("User-Agent", UA, false);
                }
            } catch (e) {}
        }
    }, "http-on-modify-request");
    // The decisive line of stage one: what the site's license server answers.
    Services.obs.addObserver({
        observe(subject) {
            try {
                const channel = subject.QueryInterface(Ci.nsIHttpChannel);
                const spec = channel.URI.spec;
                if (uaHosts(channel.URI.host.toLowerCase()) &&
                    /licen[cs]e|playready|widevine|drm/i.test(spec)) {
                    log("license server " + channel.requestMethod + " " + spec.slice(0, 200) +
                        " -> HTTP " + channel.responseStatus);
                }
            } catch (e) {}
        }
    }, "http-on-examine-response");

    // The bridge: page -> chrome -> shell, and back, with chrome-side ids so
    // several windows cannot collide.
    let nextId = 1;
    const inflight = new Map();
    Services.obs.addObserver({
        observe(subject, topic, data) {
            let reply;
            try { reply = JSON.parse(data); } catch (e) { return; }
            const entry = inflight.get(reply.id);
            if (!entry) {
                if (reply.securityVersion !== undefined) {
                    log("the phone's PlayReady: security version " + reply.securityVersion +
                        ", hardware DRM " + reply.hardwareSupported);
                } else if (reply.error) {
                    log("shell: " + reply.error);
                }
                return;
            }
            inflight.delete(reply.id);
            reply.id = entry.pageId;
            try {
                const win = entry.win;
                win.dispatchEvent(new win.CustomEvent("gecko-drm-reply", { detail: JSON.stringify(reply) }));
            } catch (e) {
                log("could not deliver a reply to the page: " + e);
            }
        }
    }, "gecko-w10m-bridge-reply");

    // Keyed by document, not by window: a tab that navigates keeps its
    // WindowProxy, so a window-keyed set skipped every page after the first
    // one in that tab -- Spotify after the sign-in redirect, and a YouTube
    // page reached by a full navigation, never got their hooks.
    const INJECTED = new WeakSet();
    const observer = {
        observe(subject, topic) {
            let win = topic === "document-element-inserted" ? (subject && subject.defaultView) : subject;
            if (!win || !win.document || INJECTED.has(win.document)) {
                return;
            }
            let host;
            try {
                host = win.location.hostname.toLowerCase();
            } catch (e) {
                return;
            }
            if (!underSpotify(win) || !win.document || !win.document.documentElement) {
                return;
            }
            INJECTED.add(win.document);
            try {
                const sandbox = Cu.Sandbox(win, { sandboxPrototype: win, wantXrays: false });
                Cu.evalInSandbox(SPOTIFY_EME_SOURCE, sandbox);
                log("frame " + host + (win.top === win ? " (top)" : " (in " + win.top.location.hostname + ")"));
                // The fourth argument: the page's events are untrusted, and
                // chrome does not hear those unless it asks.
                win.addEventListener("gecko-drm", ev => {
                    let msg;
                    try { msg = JSON.parse(ev.detail); } catch (e) { return; }
                    if (msg.log !== undefined) {
                        log("page: " + msg.log);
                        return;
                    }
                    const id = nextId++;
                    inflight.set(id, { win, pageId: msg.id });
                    msg.id = id;
                    Services.obs.notifyObservers(null, "gecko-w10m-bridge", JSON.stringify(msg));
                }, true, true);
                Services.obs.notifyObservers(null, "gecko-w10m-bridge", JSON.stringify({ id: 0, op: "drm.info" }));
                log("PlayReady offered on " + host);
            } catch (e) {
                log("hook failed on " + host + ": " + e);
            }
        }
    };
    Services.obs.addObserver(observer, "content-document-global-created");
    Services.obs.addObserver(observer, "document-element-inserted");
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

    // Keyed by document, not by window: a tab that navigates keeps its
    // WindowProxy, so a window-keyed set skipped every page after the first
    // one in that tab -- Spotify after the sign-in redirect, and a YouTube
    // page reached by a full navigation, never got their hooks.
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
            if (!win || !win.document || INJECTED.has(win.document)) {
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
            INJECTED.add(win.document);
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

// DOM fullscreen without content processes: the chrome half that the actor
// chain never reaches is run here, and the page is nudged to re-measure once
// the window has settled. One line per transition.
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
                Services.console.logStringMessage(
                    "gecko: fullscreen " + name + ", inDOMFullscreen " +
                    doc.documentElement.hasAttribute("inDOMFullscreen"));
                // The page laid itself out while the window was changing
                // size and will not measure again on its own. Two nudges,
                // once the transition has settled.
                try {
                    const cw = win.gBrowser.selectedBrowser.contentWindow;
                    for (const delay of [300, 1200]) {
                        win.setTimeout(() => {
                            try {
                                cw.dispatchEvent(new cw.Event("resize"));
                            } catch (e2) {}
                        }, delay);
                    }
                } catch (e) {}
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
    gecko_spotify();
    delete_old_mcf_files();

    // Upstream clears the startup cache on every launch, so that edits to
    // boot.sys.mjs take effect on a development machine. Nothing is cleared
    // here at all: Gecko compares the platform and app directories recorded
    // in compatibility.ini with the current ones, and a new package has a
    // new directory, so it purges on its own the first time a version runs.
    // Asking for a purge "on restart" as well threw away the cache the
    // first launch had just built -- the second launch of every version
    // reported "version same, caches PURGED" and compiled everything again.

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
