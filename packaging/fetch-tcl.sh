#!/usr/bin/env bash
#
# fetch-tcl.sh — build a self-contained static Tcl from source, for bundling.
#
# MUSHclient's Tcl engine links libtcl *statically* and ships Tcl's script
# library inside the application bundle, so the app depends on neither Homebrew
# nor system Tcl packages.  Tcl 8.6 is used deliberately: it keeps the C API
# the engine targets (Tcl 9.0 changed several signatures).  A static Tcl 8.6
# is self-contained — it bundles its own libtommath and needs only zlib +
# m/dl/pthread (or CoreFoundation on macOS) — so there is no dylib to relocate.
#
#   staging (git-ignored):
#     packaging/tcl/install/lib/libtcl8.6.a    — the static library (linked in)
#     packaging/tcl/install/include/*.h        — headers
#     packaging/tcl/install/lib/tcl8.6/        — the script library (bundled to Resources / bundle)
#
# On macOS it is built universal (arm64 + x86_64) for macOS 11 and later by
# default, and skipped when already built with the same version and settings.
#
# Usage:
#   packaging/fetch-tcl.sh                 # default version (below)
#   TCL_VERSION=8.6.16 packaging/fetch-tcl.sh
#   packaging/fetch-tcl.sh --force         # rebuild even if up to date
#   MACOS_MIN=11.0 MACOS_ARCHS="arm64 x86_64" packaging/fetch-tcl.sh
#
set -euo pipefail

# Source environment script if present (e.g. non-root sysroot or development env)
if [ -f "/tmp/env.sh" ]; then
    set +u
    # shellcheck disable=SC1091
    . /tmp/env.sh
    set -u
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$SCRIPT_DIR/tcl"
VERSION="${TCL_VERSION:-8.6.16}"
MACOS_MIN="${MACOS_MIN:-11.0}"
MACOS_ARCHS="${MACOS_ARCHS:-arm64 x86_64}"
FORCE=0

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --version)  VERSION="$2"; shift 2;;
        --force)    FORCE=1; shift;;
        -h|--help)  sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

OS="$(uname -s)"
[ "$OS" = "Darwin" ] || [ "$OS" = "Linux" ] || die "this script builds Tcl for macOS and Linux"
SHORT="${VERSION%.*}"                       # 8.6.16 -> 8.6
PKG="tcl$VERSION-src.tar.gz"
URL="https://prdownloads.sourceforge.net/tcl/$PKG"
mkdir -p "$DEST"
CACHE="$DEST/$PKG"
INSTALL="$DEST/install"

STAMP="$VERSION"
[ "$OS" = "Darwin" ] && STAMP="$VERSION | $MACOS_ARCHS | macOS $MACOS_MIN"
if [ "$FORCE" = 0 ] && [ "$(cat "$INSTALL/.stamp" 2>/dev/null || true)" = "$STAMP" ]; then
    note "Tcl $VERSION is up to date"
    exit 0
fi

if [ ! -f "$CACHE" ]; then
    note "downloading $URL"
    curl -fL --retry 3 -o "$CACHE" "$URL" || die "download failed"
fi
# Guard against SourceForge handing back an HTML mirror page instead of the tarball.
file "$CACHE" | grep -q gzip || die "download is not a gzip tarball (got an HTML page?); re-download $CACHE"

note "extracting Tcl $VERSION source"
rm -rf "$INSTALL" "$DEST/src"
mkdir -p "$DEST/src"
tar -xzf "$CACHE" -C "$DEST/src" --strip-components=1

note "configuring (static, threaded, no Tk) and building"
CFLAGS_EXTRA=""
if [ "$OS" = "Linux" ]; then
    CFLAGS_EXTRA="-fPIC -O2"
else
    CFLAGS_EXTRA="-O2 -mmacosx-version-min=$MACOS_MIN"
    for a in $MACOS_ARCHS; do CFLAGS_EXTRA="$CFLAGS_EXTRA -arch $a"; done
fi
(
    cd "$DEST/src/unix"
    CFLAGS="${CFLAGS_EXTRA}" ./configure --prefix="$INSTALL" --disable-shared --enable-threads >/dev/null
    JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    make -j"${JOBS}" >/dev/null
    make install >/dev/null
) || die "Tcl build failed"

[ -f "$INSTALL/lib/libtcl$SHORT.a" ] || die "static libtcl$SHORT.a not produced"
[ -f "$INSTALL/lib/tcl$SHORT/init.tcl" ] || die "Tcl script library not installed"
if [ "$OS" = "Darwin" ]; then
    for a in $MACOS_ARCHS; do
        lipo -archs "$INSTALL/lib/libtcl$SHORT.a" | tr ' ' '\n' | grep -qx "$a" || die "libtcl$SHORT.a lacks $a"
    done
fi
printf '%s\n' "$STAMP" > "$INSTALL/.stamp"

ARCH_TAG="$(lipo -archs "$INSTALL/lib/libtcl$SHORT.a" 2>/dev/null || uname -m)"
note "staged static Tcl $VERSION:"
note "  lib:            $INSTALL/lib/libtcl$SHORT.a  ($ARCH_TAG)"
note "  script library: $INSTALL/lib/tcl$SHORT/"
note "build with:  packaging/build-app.sh   (or a normal CMake build; auto-detects this)"
