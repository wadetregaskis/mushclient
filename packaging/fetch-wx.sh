#!/usr/bin/env bash
#
# fetch-wx.sh — build the wxWidgets the macOS app bundle links, from source.
#
# Homebrew's wxWidgets is built for one architecture and for the macOS it was
# built on, and the bundle inherited both limits (arm64 only, macOS 15+).  This
# builds wxWidgets as a universal (arm64 + x86_64) STATIC library for macOS 11
# and later, with its own built-in copies of libpng/libjpeg/libtiff/zlib/expat/
# PCRE2 -- so nothing it needs comes from Homebrew, and no wx dylibs need
# relocating into the bundle.
#
# The version matches what the development build gets from Homebrew, so a
# release and a development build run the same wxWidgets.  The download is
# checked against the SHA-1 wxWidgets publishes in its release notes.
#
#   staging (git-ignored):
#     packaging/wx/install/bin/wx-config      — what CMake is pointed at
#     packaging/wx/install/lib/libwx_*.a      — the static libraries
#     packaging/wx/install/.stamp             — version, architectures, macOS minimum
#
# macOS only (Linux uses the distribution's wxWidgets).
#
# Usage:
#   packaging/fetch-wx.sh            # build if not already built with these settings
#   packaging/fetch-wx.sh --force    # rebuild
#   MACOS_MIN=11.0 MACOS_ARCHS="arm64 x86_64" packaging/fetch-wx.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$SCRIPT_DIR/wx"
INSTALL="$DEST/install"
VERSION="3.3.2"
SHA1="64ca276f82ccb2b1459bceb4b8fda33609461d69"
MACOS_MIN="${MACOS_MIN:-11.0}"
MACOS_ARCHS="${MACOS_ARCHS:-arm64 x86_64}"
FORCE=0

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --force)    FORCE=1; shift;;
        -h|--help)  sed -n '2,27p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

[ "$(uname -s)" = "Darwin" ] || die "this builds the macOS bundle's wxWidgets; Linux uses the system one"

STAMP="$VERSION | $MACOS_ARCHS | macOS $MACOS_MIN"
if [ "$FORCE" = 0 ] && [ "$(cat "$INSTALL/.stamp" 2>/dev/null || true)" = "$STAMP" ]; then
    note "wxWidgets $VERSION is up to date ($MACOS_ARCHS, macOS $MACOS_MIN+)"
    exit 0
fi

PKG="wxWidgets-$VERSION.tar.bz2"
URL="https://github.com/wxWidgets/wxWidgets/releases/download/v$VERSION/$PKG"
CACHE="$DEST/$PKG"
mkdir -p "$DEST"
if [ ! -f "$CACHE" ]; then
    note "downloading $URL"
    curl -fL --retry 3 -o "$CACHE.part" "$URL" || die "download failed: $URL"
    mv "$CACHE.part" "$CACHE"
fi
got="$(shasum -a 1 "$CACHE" | cut -d' ' -f1)"
[ "$got" = "$SHA1" ] || { rm -f "$CACHE"; die "checksum mismatch for $PKG (got $got); deleted it, re-run to download again"; }

note "building wxWidgets $VERSION (static, $MACOS_ARCHS, macOS $MACOS_MIN+) — this takes a while"
rm -rf "$INSTALL" "$DEST/src"
mkdir -p "$DEST/src"
tar -xjf "$CACHE" -C "$DEST/src" --strip-components=1
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
LOG="$DEST/build.log"
(
    mkdir -p "$DEST/src/build-mc"
    cd "$DEST/src/build-mc"
    # Keep Homebrew out of the build entirely: its libraries are single-arch and
    # built for a newer macOS, so anything configure picked up from it would
    # either fail to link universally or quietly raise the minimum macOS.
    export PATH="/usr/bin:/bin:/usr/sbin:/sbin"
    export PKG_CONFIG=/usr/bin/false
    unset PKG_CONFIG_PATH CPATH LIBRARY_PATH CPPFLAGS LDFLAGS CFLAGS CXXFLAGS
    ../configure \
        --prefix="$INSTALL" \
        --disable-shared \
        --with-osx_cocoa \
        --with-macosx-version-min="$MACOS_MIN" \
        --enable-universal_binary="${MACOS_ARCHS// /,}" \
        --disable-sys-libs \
        --without-liblzma \
        --without-opengl \
        --disable-webview --disable-mediactrl --disable-stc \
        --disable-richtext --disable-propgrid --disable-ribbon \
        --disable-tests
    make -j"$JOBS"
    make install
) > "$LOG" 2>&1 || { tail -40 "$LOG" >&2; die "wxWidgets build failed (full log: $LOG)"; }

[ -x "$INSTALL/bin/wx-config" ] || die "wx-config not produced"
core="$(ls "$INSTALL"/lib/libwx_osx_cocoau_core-*.a 2>/dev/null | head -1)"
[ -n "$core" ] || die "static wx core library not produced"
for a in $MACOS_ARCHS; do
    lipo -archs "$core" | tr ' ' '\n' | grep -qx "$a" || die "$core lacks $a"
done
printf '%s\n' "$STAMP" > "$INSTALL/.stamp"
rm -rf "$DEST/src"
note "wxWidgets $VERSION staged in $INSTALL ($(lipo -archs "$core"))"
