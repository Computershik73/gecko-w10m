"""Talk to the phone's Windows Device Portal (USB: http://127.0.0.1:10080).

Usage:
    python tools/phone.py version                # installed package of the browser
    python tools/phone.py install [APPX]         # update the app (newest appx by default)
    python tools/phone.py logs [--dir DIR]       # gecko.log, gecko-notes.log (readable while running)
    python tools/phone.py mem [--watch SEC] [--top N]
                                                 # memory of the browser, the phone and the GPU

Over Wi-Fi, name the phone's portal (Settings > Update & security > For
developers > Device Portal shows it), before the command:
    python tools/phone.py --portal http://192.168.31.226 install
or set GECKO_W10M_PORTAL. With two phones on USB only one gets the USB port.
"""
import argparse
import glob
import http.cookiejar
import json
import os
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import uuid

PORTAL = os.environ.get("GECKO_W10M_PORTAL", "http://127.0.0.1:10080")
PACKAGE_PREFIX = "Gecko_"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPX_DIR = os.path.join(ROOT, "app", "GeckoW10m", "AppPackages")
# Files in the package's LocalAppData, by their folder under it.
LOGS = [
    ("\\LocalState", "gecko.log"),
    ("\\LocalState\\profile", "gecko-notes.log"),
    ("\\LocalState", "gecko-boot.txt"),
]


# Over USB the portal is sometimes only on its HTTPS port, with the phone's
# own self-signed certificate.
PORTAL_HTTPS = os.environ.get("GECKO_W10M_PORTAL_HTTPS", "https://127.0.0.1:10443")


# Never through a proxy: the phone is on this desk or this network, and a
# proxy set for the PC (a VPN client's, say) cannot reach it. The cookies are
# for the portal's CSRF token, which it wants back on every POST.
def _opener():
    import ssl
    return urllib.request.build_opener(
        urllib.request.ProxyHandler({}),
        urllib.request.HTTPSHandler(context=ssl._create_unverified_context()),
        urllib.request.HTTPCookieProcessor(_COOKIES))


_COOKIES = http.cookiejar.CookieJar()


def _usb():
    return urllib.parse.urlsplit(PORTAL).hostname in ("127.0.0.1", "localhost")


def request(endpoint, data=None, method=None, headers=None, timeout=30, **params):
    global PORTAL
    query = endpoint + ("?" + urllib.parse.urlencode(params) if params else "")

    def send():
        req = urllib.request.Request(PORTAL + query, data=data, method=method,
                                     headers=dict(headers or {}))
        for c in _COOKIES:
            if c.name == "CSRF-Token":
                req.add_header("X-CSRF-Token", c.value)
        return _opener().open(req, timeout=timeout)

    try:
        return send()
    except urllib.error.URLError as e:
        if (not _usb() or PORTAL == PORTAL_HTTPS or
                not isinstance(getattr(e, "reason", None), ConnectionRefusedError)):
            raise
    PORTAL = PORTAL_HTTPS
    return send()


def get(endpoint, **params):
    with request(endpoint, **params) as r:
        return r.read()


def package():
    pkgs = json.loads(get("/api/app/packagemanager/packages"))["InstalledPackages"]
    for p in pkgs:
        if p["PackageFullName"].startswith(PACKAGE_PREFIX):
            return p
    sys.exit("the browser is not installed on the phone")


def cmd_version(_):
    print(package()["PackageFullName"])


def winappdeploycmd():
    found = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\10.*\x86\WinAppDeployCmd.exe"))
    if not found:
        sys.exit("WinAppDeployCmd.exe not found in the Windows Kits")
    # 10.0.16299's is the one known to talk to Windows 10 Mobile.
    for path in found:
        if "10.0.16299.0" in path:
            return path
    return found[-1]


def install_through_portal(appx):
    """The portal's own install: an update when the package is newer, so the
    app's data stays. Used over Wi-Fi, where WinAppDeployCmd wants pairing."""
    name = os.path.basename(appx)
    get("/api/os/machinename")  # for the CSRF cookie
    boundary = uuid.uuid4().hex
    with open(appx, "rb") as f:
        payload = f.read()
    body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"; "
            f"filename=\"{name}\"\r\nContent-Type: application/octet-stream\r\n\r\n"
            ).encode() + payload + f"\r\n--{boundary}--\r\n".encode()
    started = time.time()
    request("/api/app/packagemanager/package", data=body, method="POST", timeout=1800,
            headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
            package=name).close()
    print(f"uploaded {len(payload) / 2**20:.0f} MB in {time.time() - started:.0f} s")
    # The install goes on after the upload; the state call says 204 meanwhile.
    while True:
        time.sleep(3)
        try:
            with request("/api/app/packagemanager/state") as r:
                if r.status == 204:
                    continue
                text = r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            print("install failed:", e.code, e.read().decode("utf-8", "replace").strip())
            return 1
        try:
            state = json.loads(text)
        except ValueError:
            state = {}
        print("install:", state.get("Reason") or text.strip())
        return 0 if state.get("Success", True) else 1


def cmd_install(a):
    appx = a.appx
    if not appx:
        candidates = glob.glob(os.path.join(APPX_DIR, "Gecko_*_ARM.appx"))
        if not candidates:
            sys.exit(f"no Gecko_*_ARM.appx in {APPX_DIR}")
        appx = max(candidates, key=os.path.getmtime)
    print(f"installing {os.path.basename(appx)} on {PORTAL} (app data kept)")
    if _usb():
        # update keeps the app's data; the phone already trusts the certificate.
        r = subprocess.run([winappdeploycmd(), "update", "-file", appx, "-ip", "127.0.0.1"],
                           capture_output=True, text=True)
        print((r.stdout + r.stderr).strip().splitlines()[-1] if (r.stdout + r.stderr).strip() else "")
        code = r.returncode
    else:
        code = install_through_portal(appx)
    print("now on the phone:", package()["PackageFullName"])
    return code


def cmd_logs(a):
    pfn = package()["PackageFullName"]
    out_dir = a.dir or os.path.join(ROOT, "work", "phone")
    os.makedirs(out_dir, exist_ok=True)
    for folder, name in LOGS:
        try:
            data = get("/api/filesystem/apps/file", knownfolderid="LocalAppData",
                       packagefullname=pfn, filename=name, path=folder)
        except Exception as e:
            print(f"{name}: {e}")
            continue
        out = os.path.join(out_dir, name)
        with open(out, "wb") as f:
            f.write(data)
        print(f"{name}: {len(data)} bytes -> {out}")


def mb(v):
    return v / 2**20


def cmd_mem(a):
    while True:
        stamp = time.strftime("%H:%M:%S")
        procs = json.loads(get("/api/resourcemanager/processes"))["Processes"]
        perf = json.loads(get("/api/resourcemanager/systemperf"))
        page = perf.get("PageSize", 4096)
        total = perf.get("TotalPages", 0) * page
        avail = perf.get("AvailablePages", 0) * page
        commit = perf.get("CommittedPages", 0) * page
        limit = perf.get("CommitLimit", 0) * page
        browser = [p for p in procs if (p.get("PackageFullName") or "").startswith(PACKAGE_PREFIX)]
        if not browser:
            print(f"{stamp} the browser is not running")
        for p in browser:
            print(f"{stamp} browser: private {mb(p.get('PrivateWorkingSet', 0)):.0f} MB, "
                  f"working set {mb(p.get('WorkingSetSize', 0)):.0f} MB, "
                  f"commit {mb(p.get('TotalCommit', 0)):.0f} MB, "
                  f"virtual {mb(p.get('VirtualSize', 0)):.0f} MB, cpu {p.get('CPUUsage', 0):.0f}%")
        print(f"{stamp} phone: {mb(avail):.0f} MB free of {mb(total):.0f} MB, "
              f"committed {mb(commit):.0f} of {mb(limit):.0f} MB, cpu {perf.get('CpuLoad', 0)}%")
        for gpu in (perf.get("GPUData") or {}).get("AvailableAdapters", []):
            engines = ", ".join(f"{e:.0f}" for e in gpu.get("EnginesUtilization", [])[:4])
            print(f"{stamp} gpu {gpu.get('Description')}: shared memory used "
                  f"{mb(gpu.get('SystemMemoryUsed', 0)):.0f} of {mb(gpu.get('SystemMemory', 0)):.0f} MB, "
                  f"dedicated {mb(gpu.get('DedicatedMemoryUsed', 0)):.0f} of "
                  f"{mb(gpu.get('DedicatedMemory', 0)):.0f} MB, engines % {engines}")
        if a.top:
            print(f"{stamp} top {a.top} by private working set:")
            for p in sorted(procs, key=lambda p: p.get("PrivateWorkingSet", 0), reverse=True)[:a.top]:
                print(f"    {mb(p.get('PrivateWorkingSet', 0)):6.0f} MB  {p.get('ImageName')}")
        if not a.watch:
            break
        time.sleep(a.watch)


def main():
    global PORTAL
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--portal", help="the phone's Device Portal, e.g. http://192.168.31.226")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("version")
    ip = sub.add_parser("install")
    ip.add_argument("appx", nargs="?")
    lp = sub.add_parser("logs")
    lp.add_argument("--dir")
    mp = sub.add_parser("mem")
    mp.add_argument("--watch", type=float, default=0)
    mp.add_argument("--top", type=int, default=10)
    a = ap.parse_args()
    if a.portal:
        PORTAL = a.portal.rstrip("/")
    sys.exit({"version": cmd_version, "install": cmd_install, "logs": cmd_logs,
              "mem": cmd_mem}[a.cmd](a))


if __name__ == "__main__":
    main()
