#!/bin/bash
export HOME=/c/Users/User
mkdir -p /c/Users/User/AppData/Local/Temp/geckow10mbuild
export TMP='C:\Users\User\AppData\Local\Temp\geckow10mbuild'
export TEMP="$TMP"
export TMPDIR=/c/Users/User/AppData/Local/Temp/geckow10mbuild
export PATH="/c/Program Files/LLVM/bin:$PATH"
export CC_thumbv7a_uwp_windows_msvc="clang-cl"
export CFLAGS_thumbv7a_uwp_windows_msvc="--target=thumbv7-windows-msvc"
cd "$(dirname "$0")"
echo "=== build-std std ==="
cargo +nightly build -Z build-std=std,panic_abort --target thumbv7a-uwp-windows-msvc 2>&1 | tail -25
echo "=== EXIT=${PIPESTATUS[0]} ==="
