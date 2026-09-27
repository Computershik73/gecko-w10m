#!/bin/bash
# Build the Rust std library from source for the arm-uwp target and install the
# produced rlibs into the toolchain's sysroot so rustc finds std directly (the
# target ships no prebuilt std).
#
# The toolchain is the one `rustc` resolves to -- the one the engine build will
# use -- unless GECKO_W10M_RUST_TOOLCHAIN names another. It must be a nightly
# with the rust-src component (rustup component add rust-src). Run this again
# whenever that nightly changes: std built by one rustc cannot be linked by
# another.
#
# The target is our own thumbv7a-uwp-windows-msvchf, not the built-in
# thumbv7a-uwp-windows-msvc. The two differ in one field, llvm-target, and that
# field decides whether rustc passes homogeneous float aggregates in the VFP
# registers. rustc gates that on the llvm-target string ending in "hf", a test
# meant for gnueabihf triples that no MSVC triple can pass -- so on stock
# thumbv7a-uwp-windows-msvc a struct of four floats goes in r1-r3 and the stack
# while clang-cl puts it in s0-s3, and every argument after it lands somewhere
# the callee does not look. See mozconfig/rust-targets/.
set -e
source "$(dirname "$0")/env.sh"
export CC_thumbv7a_uwp_windows_msvchf="clang-cl"
export CFLAGS_thumbv7a_uwp_windows_msvchf="--target=thumbv7-windows-msvc"
export RUSTFLAGS="-Cembed-bitcode=yes -Cpanic=abort -Zunstable-options"
TARGET=thumbv7a-uwp-windows-msvchf
TC=()
[ -n "$GECKO_W10M_RUST_TOOLCHAIN" ] && TC=("+$GECKO_W10M_RUST_TOOLCHAIN")
SYSROOT_DIR="$(cygpath -u "$(rustc "${TC[@]}" --print sysroot)")/lib/rustlib/$TARGET"
echo "toolchain: $(rustc "${TC[@]}" -V)"

# Install the target description first. rustc looks for a custom target in
# RUST_TARGET_PATH and in <sysroot>/lib/rustlib/<target>/target.json, and only
# the second one survives the trip into the build: make exports reach cargo, but
# msys2 rewrites any variable whose name ends in PATH, and the build never sees
# the value that was set. The sysroot needs no environment at all.
mkdir -p "$SYSROOT_DIR"
cp "$GECKO_W10M_ROOT/mozconfig/rust-targets/$TARGET.json" "$SYSROOT_DIR/target.json"

WORK="$(mktemp -d)"; cd "$WORK"
cargo "${TC[@]}" new --bin stdbuild > /dev/null 2>&1; cd stdbuild
printf '[profile.dev]\npanic = "abort"\n' >> Cargo.toml
cargo "${TC[@]}" build --release -Z build-std=std,panic_abort --target "$TARGET" || true
SYSROOT="$SYSROOT_DIR/lib"
mkdir -p "$SYSROOT"
cp target/"$TARGET"/release/deps/*.rlib "$SYSROOT"/
echo "Installed $(ls "$SYSROOT"/*.rlib | wc -l) std rlibs into $SYSROOT"
