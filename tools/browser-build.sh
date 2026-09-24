#!/bin/bash
export MOZILLABUILD=/c/mozilla-build
export HOME=/c/Users/User
export USERPROFILE='C:\Users\User'
export MOZBUILD_STATE_PATH=/c/Users/User/.mozbuild
export ProgramFiles="C:/Program Files"
export PROGRAMW6432="C:/Program Files"
export ProgramW6432="C:/Program Files"
mkdir -p /c/Users/User/AppData/Local/Temp/geckow10mbuild
export TMP='C:\Users\User\AppData\Local\Temp\geckow10mbuild'; export TEMP="$TMP"; export TMPDIR=/c/Users/User/AppData/Local/Temp/geckow10mbuild
export MOZ_WINDOWS_RS_DIR='C:/Users/User/Documents/GitHub/gecko-w10m/engine/third_party/windows-0.62.2'
export VC_PATH='C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC/14.44.35207'
export PATH="/c/Users/User/.cargo/bin:/c/Program Files/nodejs:/c/Program Files/LLVM/bin:/c/mozilla-build/python3:/c/mozilla-build/python3/Scripts:/c/mozilla-build/bin:/c/mozilla-build/msys2/usr/bin:$PATH"
cd /c/Users/User/Documents/GitHub/gecko-w10m/engine/firefox
export MOZCONFIG=/c/Users/User/Documents/GitHub/gecko-w10m/mozconfig/mozconfig.arm-uwp-browser
# The build id is the clock at build time unless MOZ_BUILD_DATE says
# otherwise, and it goes into compatibility.ini in the profile: a new id
# purges every cache the previous version built. So a package that changed
# only the shell threw away the caches too, and a startup cache captured
# on one package could never ship in the next. The id is now the newest
# modification time among the engine's changed and untracked sources, so
# it moves when the engine moves and stays when it does not.
export MOZ_BUILD_DATE="$(python - <<'PY'
import os, subprocess, datetime
root = r"C:\Users\User\Documents\GitHub\gecko-w10m\engine\firefox"
# Against the upstream commit the port started from, not HEAD: the port's
# changes are committed on a local branch (w10m-port), so "modified" alone
# would be empty and the id would stop following the engine.
base = "fb95137a04eb8fe1196cb12f26b100c1e060295c"
out = subprocess.run(["git", "-C", root, "diff", "--name-only", "-z", base],
                     capture_output=True).stdout.decode("utf-8", "replace")
out += subprocess.run(["git", "-C", root, "ls-files", "-o", "--exclude-standard", "-z"],
                      capture_output=True).stdout.decode("utf-8", "replace")
latest = 0.0
for rel in out.split("\0"):
    if not rel:
        continue
    try:
        latest = max(latest, os.stat(os.path.join(root, rel)).st_mtime)
    except OSError:
        pass
if not latest:
    latest = os.stat(os.path.join(root, "configure.py")).st_mtime
print(datetime.datetime.fromtimestamp(latest).strftime("%Y%m%d%H%M%S"))
PY
)"
echo "=== browser BUILD start $(date), MOZ_BUILD_DATE=$MOZ_BUILD_DATE ==="
# 16 logical CPUs / 31 GB RAM: use most of the machine but leave headroom
# so it stays usable and the build does not page itself to death.
export CARGO_BUILD_JOBS=8
./mach build -j12 < /dev/null 2>&1
echo "=== build exit=$? at $(date) ==="
