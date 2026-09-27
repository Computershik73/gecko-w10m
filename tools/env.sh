# Build environment shared by the tools/*.sh build scripts. Source it, do not
# run it. Every location has a default and can be overridden by setting the
# variable before the script runs:
#
#   GECKO_W10M_OBJ          engine object directory      (C:/rw-obj)
#   MOZILLABUILD            MozillaBuild                 (C:/mozilla-build)
#   GECKO_W10M_VC           MSVC toolset with ARM32 tools (newest under VS 2022)
#   GECKO_W10M_LLVM         LLVM bin (clang-cl, lld-link) (C:/Program Files/LLVM/bin)
#   GECKO_W10M_SDK          Windows Kits 10              (C:/Program Files (x86)/Windows Kits/10)
#   GECKO_W10M_SDK_VERSION  SDK for headers and tools    (10.0.22621.0)
#   GECKO_W10M_SDK_LIB      SDK the shell links against  (same as above; 10.0.14393.0 for 1607 phones)
#   GECKO_W10M_JOBS         parallel compile jobs        (number of CPUs, less a quarter)
#
# The object directory's path is short on purpose: the deepest libwebrtc object
# path otherwise exceeds MAX_PATH and GNU make reports "No rule to make target".

GECKO_W10M_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GECKO_W10M_ROOT
export GECKO_W10M_OBJ="${GECKO_W10M_OBJ:-C:/rw-obj}"
export MOZILLABUILD="${MOZILLABUILD:-C:/mozilla-build}"
export GECKO_W10M_LLVM="${GECKO_W10M_LLVM:-C:/Program Files/LLVM/bin}"
export GECKO_W10M_SDK="${GECKO_W10M_SDK:-C:/Program Files (x86)/Windows Kits/10}"
export GECKO_W10M_SDK_VERSION="${GECKO_W10M_SDK_VERSION:-10.0.22621.0}"
export GECKO_W10M_SDK_LIB="${GECKO_W10M_SDK_LIB:-$GECKO_W10M_SDK_VERSION}"

# MSVC supplies headers and libraries to clang-cl and compiles the shell itself
# (cl.exe: clang has no SEH on 32-bit ARM Windows). It must have the ARM32
# tools, which VS 2022 17.14 is the last to ship. The newest toolset that has
# them is picked unless GECKO_W10M_VC names one.
if [ -z "$GECKO_W10M_VC" ]; then
  for cl in "C:/Program Files/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/*/bin/Hostx64/arm/cl.exe; do
    [ -f "$cl" ] && GECKO_W10M_VC="${cl%/bin/Hostx64/arm/cl.exe}"
  done
fi
if [ -z "$GECKO_W10M_VC" ] || [ ! -f "$GECKO_W10M_VC/bin/Hostx64/arm/cl.exe" ]; then
  echo "No MSVC toolset with ARM32 tools found; install the VS 2022 'MSVC ARM build tools' or set GECKO_W10M_VC." >&2
  return 1 2> /dev/null || exit 1
fi
export GECKO_W10M_VC
# mozbuild's own name for it; without it vswhere may pick a Visual Studio that
# has no ARM32 compiler.
export VC_PATH="$GECKO_W10M_VC"

if [ -z "$GECKO_W10M_JOBS" ]; then
  n="$(nproc 2> /dev/null || echo 4)"
  GECKO_W10M_JOBS=$(( n - n / 4 ))
fi
export GECKO_W10M_JOBS

# MozillaBuild's msys has a HOME of its own; rustup, cargo and mach's state
# live in the Windows profile.
if [ -n "$USERPROFILE" ]; then
  HOME="$(cygpath -u "$USERPROFILE")"
  export HOME
fi
export MOZBUILD_STATE_PATH="${MOZBUILD_STATE_PATH:-$HOME/.mozbuild}"
export ProgramFiles="C:/Program Files"
export PROGRAMW6432="C:/Program Files"
export ProgramW6432="C:/Program Files"

# A writable temp directory: msys2 can leave TMP pointing at C:\Windows.
_tmp="$(cygpath -u "${LOCALAPPDATA:-$USERPROFILE/AppData/Local}")/Temp/geckow10mbuild"
mkdir -p "$_tmp"
TMP="$(cygpath -w "$_tmp")"
export TMP TEMP="$TMP" TMPDIR="$_tmp"
unset _tmp

# The windows-rs crate, patched for ARM32 by tools/prepare-windows-rs.sh.
export MOZ_WINDOWS_RS_DIR="$(cygpath -m "$GECKO_W10M_ROOT")/engine/third_party/windows-0.62.2"

export MOZCONFIG="$GECKO_W10M_ROOT/mozconfig/mozconfig.arm-uwp-browser"

PATH="$HOME/.cargo/bin:$(cygpath -u "$GECKO_W10M_LLVM"):$(cygpath -u "$MOZILLABUILD")/python3:$(cygpath -u "$MOZILLABUILD")/python3/Scripts:$(cygpath -u "$MOZILLABUILD")/bin:$(cygpath -u "$MOZILLABUILD")/msys2/usr/bin:$PATH"
[ -d "/c/Program Files/nodejs" ] && PATH="/c/Program Files/nodejs:$PATH"
export PATH

# The build id: the newest modification time among the engine's changed and
# untracked sources, against the release tag the port starts from. It goes into
# compatibility.ini in the profile, and a new id purges every cache the previous
# version built -- so it moves when the engine moves and stays when only the
# shell changed, and a startup cache captured on one package can ship in the
# next.
gecko_w10m_build_date() {
  python - "$GECKO_W10M_ROOT/engine/firefox" "$(awk 'NR == 1 { print $1 }' "$GECKO_W10M_ROOT/engine/release.txt")" <<'PY'
import datetime, os, subprocess, sys
root, tag = sys.argv[1], sys.argv[2]
def git(*args):
    return subprocess.run(["git", "-C", root, *args], capture_output=True).stdout.decode("utf-8", "replace")
base = git("rev-parse", tag + "^{commit}").strip()
out = git("diff", "--name-only", "-z", base) if base else ""
out += git("ls-files", "-o", "--exclude-standard", "-z")
latest = 0.0
for rel in out.split("\0"):
    if rel:
        try:
            latest = max(latest, os.stat(os.path.join(root, rel)).st_mtime)
        except OSError:
            pass
if not latest:
    latest = os.stat(os.path.join(root, "configure.py")).st_mtime
print(datetime.datetime.fromtimestamp(latest).strftime("%Y%m%d%H%M%S"))
PY
}

# The mozconfig force-includes this header (-FI) and finds it through the
# object directory's dist/include, so it goes there before configure and build.
gecko_w10m_stage_intrin() {
  mkdir -p "$GECKO_W10M_OBJ/dist/include"
  cp -f "$GECKO_W10M_ROOT/mozconfig/gecko_w10m_arm_intrin.h" "$GECKO_W10M_OBJ/dist/include/"
}
