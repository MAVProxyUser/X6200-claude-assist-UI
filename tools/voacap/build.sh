#!/bin/bash
# GoldCorpHF - voacapl for the radios (tools/voacap): the program Pat runs for the VOACAP column of its Winlink
# gateway list, on hfcast-engine (https://github.com/jonathanmcsweet/hfcast-engine, Apache-2.0, VOACAP in Rust with
# its coefficients compiled in). One ARMv7 binary for the X6100 and the X6200, built with the official Rust toolchain
# at the version tools/xara/build.sh pins (the same download, kept in the same place), from hfcast at the pinned
# commit (cloned, the commit checked) and this folder's crate. See UPSTREAM.md, which says what the binary carries.
#
#   CC=<the radios' gcc> tools/voacap/build.sh    prints the binary's path on stdout, and nothing else
#
# buildroot/local-build.sh runs it with CC set to the buildroot tree's own cross gcc and gives the path to the GUI's
# CMake (-DVOACAPL_BINARY), which installs it as /usr/bin/voacapl and its notices in /usr/share/x6100/licenses/
# voacapl. Kept in $HOME/.cache/openx6x00: the binary under a name of hfcast's commit, the Rust version, the target and
# the hash of this crate, this script and the linker; a second run with nothing changed prints that path.
set -eo pipefail
exec 3>&1 1>&2          # stdout is the binary's path only: everything else goes to stderr

HERE=$(cd "$(dirname "$0")" && pwd)
HFCAST_URL=https://github.com/jonathanmcsweet/hfcast-engine.git
HFCAST_COMMIT=bf34e8f9f375e190ed43fcac19f25d800992e0e6      # 2.14.0, 2026-10-27
# Rust as tools/xara/build.sh pins it, the same parts and hashes (static.rust-lang.org's <file>.sha256)
RUST_VER=1.99.1
RUST_URL=https://static.rust-lang.org/dist
RUST_PARTS=(
    "rustc-$RUST_VER-x86_64-unknown-linux-gnu 2ff05ff9c08a90861e7f6e4065b40a8b87463b4576186fd613871c3e37f2091f"
    "cargo-$RUST_VER-x86_64-unknown-linux-gnu 53f3521949404d595efb06a55c12c5658d4ed1c75566f33d3d514eaebe70bd8a"
    "rust-std-$RUST_VER-x86_64-unknown-linux-gnu 109b074225bf5b9ea3820890876bfdf4ed161adbfaf94b2b4b5984c85ee5d958"
    "rust-std-$RUST_VER-armv7-unknown-linux-gnueabihf 53fdf6581a24f510def5173813a0d23518a8cde547b9fbf4876499f19feda18c"
)
TARGET=armv7-unknown-linux-gnueabihf

[ "$(uname -m)" = x86_64 ] || { echo "tools/voacap/build.sh: the pinned Rust toolchain is for x86_64 hosts"; exit 1; }
LINKER=${CC:-}
[ -n "$LINKER" ] || { echo "tools/voacap/build.sh: CC must name the radios' gcc (arm-buildroot-linux-gnueabihf-gcc)"; exit 1; }
case "$("$LINKER" -dumpmachine)" in
    arm*-linux-gnueabihf) ;;
    *) echo "tools/voacap/build.sh: $LINKER is not a gcc for arm-linux-gnueabihf"; exit 1 ;;
esac

CACHE=$HOME/.cache/openx6x00
RUST=$CACHE/rust-$RUST_VER
DL=$CACHE/xara/dl                                       # where tools/xara keeps the toolchain's parts
KEY=$( (cat "$HERE/Cargo.toml" "$HERE/Cargo.lock" "$HERE"/src/*.rs "$HERE/build.sh"; "$LINKER" --version | head -1
        "$LINKER" -dumpmachine; cat "$("$LINKER" -print-file-name=libc.so.6)" 2> /dev/null || true) |
       sha256sum | cut -c1-12)
OUT=$CACHE/voacap/voacapl-${HFCAST_COMMIT:0:12}-rust$RUST_VER-$TARGET-$KEY/voacapl

if [ -x "$OUT" ] && [ -f "$(dirname "$OUT")/doc/LICENSE" ]; then
    echo "== voacapl (hfcast ${HFCAST_COMMIT:0:12}, Rust $RUST_VER, $TARGET, $KEY): built already"
    echo "$OUT" >&3
    exit 0
fi
mkdir -p "$DL" "$CACHE/voacap"

fetch() {   # URL FILE SHA256: downloaded once, kept only with the pinned hash
    [ -f "$2" ] && echo "$3  $2" | sha256sum -c --quiet - 2> /dev/null && return 0
    echo "== $1"
    # every error retried: one HTTP/2 stream error from static.rust-lang.org failed the X6100's image build of
    # 4f7f874f (curl 92), which --retry alone does not retry
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 5 -o "$2.tmp.$$" "$1"
    echo "$3  $2.tmp.$$" | sha256sum -c --quiet - \
        || { echo "tools/voacap/build.sh: $1 does not have the pinned SHA-256"; rm -f "$2.tmp.$$"; exit 1; }
    mv -f "$2.tmp.$$" "$2"
}

# The toolchain, as tools/xara/build.sh unpacks it (one of the two builds made it already, as a rule)
if [ ! -x "$RUST/bin/cargo" ]; then
    T=$(mktemp -d "$CACHE/rust-$RUST_VER.tmp.XXXXXX")
    trap 'rm -rf "$T"' EXIT
    mkdir "$T/root"
    for p in "${RUST_PARTS[@]}"; do
        name=${p% *}
        fetch "$RUST_URL/$name.tar.xz" "$DL/$name.tar.xz" "${p#* }"
        tar xJf "$DL/$name.tar.xz" -C "$T"
        for c in $(cat "$T/$name/components"); do
            (cd "$T/$name/$c" && tar cf - --exclude=manifest.in .) | tar xf - -C "$T/root"
        done
        rm -rf "${T:?}/$name"
    done
    [ -x "$RUST/bin/cargo" ] || { rm -rf "$RUST"; mv "$T/root" "$RUST"; }
    rm -rf "$T"
    trap - EXIT
fi
"$RUST/bin/rustc" --version | grep -q "^rustc $RUST_VER " && [ -d "$RUST/lib/rustlib/$TARGET/lib" ] \
    || { echo "tools/voacap/build.sh: $RUST is not Rust $RUST_VER with $TARGET"; exit 1; }

# hfcast at the pinned commit, kept; a clone whose HEAD is anything else is made again
SRC=$CACHE/voacap/hfcast-$HFCAST_COMMIT
if [ "$(git -C "$SRC" rev-parse HEAD 2> /dev/null)" != "$HFCAST_COMMIT" ]; then
    rm -rf "$SRC" "$SRC.tmp"
    echo "== $HFCAST_URL at $HFCAST_COMMIT"
    git clone --quiet "$HFCAST_URL" "$SRC.tmp"
    git -C "$SRC.tmp" -c advice.detachedHead=false checkout --quiet "$HFCAST_COMMIT"
    [ "$(git -C "$SRC.tmp" rev-parse HEAD)" = "$HFCAST_COMMIT" ] \
        || { echo "tools/voacap/build.sh: hfcast is not at $HFCAST_COMMIT"; exit 1; }
    mv "$SRC.tmp" "$SRC"
fi

# This crate in a work folder of its own, its path to hfcast that clone's
WORK=$(mktemp -d "$CACHE/voacap/work.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp -R "$HERE/Cargo.toml" "$HERE/Cargo.lock" "$HERE/src" "$WORK/"
sed -i "s|path = \"../../build/voacap/hfcast-engine\"|path = \"$SRC\"|" "$WORK/Cargo.toml"
grep -q "path = \"$SRC\"" "$WORK/Cargo.toml"

# Rust as pinned and nothing else: no rustup, no flags, wrappers or directories from the environment
for v in $(compgen -e); do
    case $v in CARGO_*|RUSTC*|RUSTDOC*|RUSTFLAGS|RUSTUP_*) unset "$v" ;; esac
done
export PATH=$RUST/bin:$PATH RUSTC=$RUST/bin/rustc
export CARGO_HOME=$CACHE/voacap/cargo CARGO_TARGET_DIR=$WORK/target
export "CARGO_TARGET_$(echo $TARGET | tr a-z- A-Z_)_LINKER=$LINKER"
export RUSTFLAGS="--remap-path-prefix=$WORK=/build --remap-path-prefix=$SRC=/hfcast --remap-path-prefix=$CARGO_HOME=/cargo"
export HFCAST_VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' "$SRC/Cargo.toml" | head -1)

echo "== voacapl for $TARGET (offline: hfcast's crate has no dependencies, Cargo.lock as it is)"
(cd "$WORK" && cargo build --frozen --offline --release --target $TARGET)
BIN=$WORK/target/$TARGET/release/voacapl

# The notices that go with it: hfcast's licence and NOTICE (VOACAP's US Government status, J.A. Watson's CC0 port,
# the data files' origins), its licence notes, and the Rust standard library's licence
DOC=$WORK/doc
mkdir "$DOC"
cp "$SRC/LICENSE" "$DOC/LICENSE"
cp "$SRC/NOTICE" "$DOC/NOTICE"
cp "$SRC/docs/licence.md" "$DOC/hfcast-licence.md"
cp "$HERE/UPSTREAM.md" "$DOC/UPSTREAM.md"
gzip -9 -n -c "$RUST/share/doc/rust/COPYRIGHT-library.html" > "$DOC/rust-std-COPYRIGHT.html.gz"   # as tools/xara

D=$(dirname "$OUT")
rm -rf "$D.tmp"
mkdir -p "$D.tmp"
cp "$BIN" "$D.tmp/voacapl"
cp -R "$DOC" "$D.tmp/doc"
rm -rf "$D"
mv "$D.tmp" "$D"
echo "== $OUT"
echo "$OUT" >&3
