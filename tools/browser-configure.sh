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
# The mozconfig force-includes this header (-FI) and finds it through the
# object directory's dist/include, so it goes there before every build.
mkdir -p C:/rw-obj/dist/include
cp -f /c/Users/User/Documents/GitHub/gecko-w10m/mozconfig/gecko_w10m_arm_intrin.h C:/rw-obj/dist/include/
echo "=== browser configure start $(date) ==="
./mach configure < /dev/null 2>&1
echo "=== configure exit=$? at $(date) ==="
