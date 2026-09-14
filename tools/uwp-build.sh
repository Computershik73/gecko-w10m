#!/bin/bash
export MOZILLABUILD=/c/mozilla-build
export HOME=/c/Users/User
export USERPROFILE='C:\Users\User'
export MOZBUILD_STATE_PATH=/c/Users/User/.mozbuild
mkdir -p /c/Users/User/AppData/Local/Temp/geckow10mbuild
export TMP='C:\Users\User\AppData\Local\Temp\geckow10mbuild'; export TEMP="$TMP"; export TMPDIR=/c/Users/User/AppData/Local/Temp/geckow10mbuild
export VC_PATH='C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC/14.44.35207'
export PATH="/c/Users/User/.cargo/bin:/c/Program Files/nodejs:/c/Program Files/LLVM/bin:/c/mozilla-build/python3:/c/mozilla-build/python3/Scripts:/c/mozilla-build/bin:/c/mozilla-build/msys2/usr/bin:$PATH"
export CC_thumbv7a_uwp_windows_msvc="clang-cl"
export CFLAGS_thumbv7a_uwp_windows_msvc="--target=thumbv7-windows-msvc"
cd /c/Users/User/Documents/GitHub/gecko-w10m/engine/firefox
export MOZCONFIG=/c/Users/User/Documents/GitHub/gecko-w10m/mozconfig/mozconfig.arm-uwp
echo "=== mach build -k start $(date) ==="
./mach build < /dev/null 2>&1
echo "=== mach build exit=$? at $(date) ==="
