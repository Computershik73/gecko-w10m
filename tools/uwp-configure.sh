#!/bin/bash
export MOZILLABUILD=/c/mozilla-build
export HOME=/c/Users/User
export USERPROFILE='C:\Users\User'
export MOZBUILD_STATE_PATH=/c/Users/User/.mozbuild
# Force VS2022 Enterprise's ARM-capable toolset (vswhere would otherwise pick
# the newer VS "18" BuildTools, whose toolset lacks the ARM32 cross compiler).
export VC_PATH='C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC/14.44.35207'
export PATH="/c/Users/User/.cargo/bin:/c/Program Files/nodejs:/c/Program Files/LLVM/bin:/c/mozilla-build/python3:/c/mozilla-build/python3/Scripts:/c/mozilla-build/bin:/c/mozilla-build/msys2/usr/bin:$PATH"
cd /c/Users/User/Documents/GitHub/gecko-w10m/engine/firefox
export MOZCONFIG=/c/Users/User/Documents/GitHub/gecko-w10m/mozconfig/mozconfig.arm-uwp
echo "=== starting mach configure at $(date) ==="
./mach configure < /dev/null 2>&1
echo "=== mach configure exit=$? at $(date) ==="
