#!/bin/bash
# Build the Rust std library from source for the tier-3 arm-uwp target and
# install the produced rlibs into the nightly toolchain sysroot so rustc finds
# std directly (needed because thumbv7a-uwp-windows-msvc ships no prebuilt std).
set -e
export HOME=/c/Users/User
mkdir -p /c/Users/User/AppData/Local/Temp/geckow10mbuild
export TMP='C:\Users\User\AppData\Local\Temp\geckow10mbuild'; export TEMP="$TMP"
export PATH="/c/Program Files/LLVM/bin:$PATH"
export CC_thumbv7a_uwp_windows_msvc="clang-cl"
export CFLAGS_thumbv7a_uwp_windows_msvc="--target=thumbv7-windows-msvc"
export RUSTFLAGS="-Cembed-bitcode=yes -Cpanic=abort"
WORK="$(mktemp -d)"; cd "$WORK"
cargo new --bin stdbuild >/dev/null 2>&1; cd stdbuild
printf '[profile.dev]\npanic = "abort"\n' >> Cargo.toml
cargo +nightly build --release -Z build-std=std,panic_abort --target thumbv7a-uwp-windows-msvc || true
SYSROOT="$HOME/.rustup/toolchains/nightly-x86_64-pc-windows-msvc/lib/rustlib/thumbv7a-uwp-windows-msvc/lib"
mkdir -p "$SYSROOT"
cp target/thumbv7a-uwp-windows-msvc/release/deps/*.rlib "$SYSROOT"/
echo "Installed $(ls "$SYSROOT"/*.rlib | wc -l) std rlibs into $SYSROOT"
