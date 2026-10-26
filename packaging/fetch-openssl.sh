#!/usr/bin/env bash
#
# fetch-openssl.sh — build the one OpenSSL the macOS app bundle ships.
#
# The bundle carries exactly one libssl/libcrypto, shared by the app itself, the
# bundled Ruby (fetch-ruby.sh links against this build) and the bundled Python
# (whose _ssl/_hashlib .build-mac.sh repoints here).  Taking it from Homebrew
# made the result depend on what each Mac happened to have installed: openssl@3
# on one, openssl@4 on another, and a Ruby cached from whichever was current
# when it was built.  Building it here gives every Mac the same OpenSSL.
#
# Architectures and minimum macOS: universal (arm64 + x86_64) for macOS 11+ by
# default.  OpenSSL's build handles one architecture per run, so each is built
# separately and the libraries merged with lipo.
#
# Which version: the newest final release of OpenSSL's 3.5 LTS series (supported
# until April 2030), looked up on GitHub each run, so security releases are
# picked up as they appear.  Alphas, betas and anything GitHub marks as a
# pre-release are never used.  The download is checked against the .sha256
# published with that release.  When it is already built this is a quick check;
# offline, it keeps what it has.  All 3.x releases share one ABI, so a newer
# patch release needs no rebuild of Ruby or Python.
#
# OPENSSLDIR is /etc/ssl, so the default certificate store is macOS's own
# /etc/ssl/cert.pem rather than a Homebrew path a user's Mac may not have.
#
#   staging (git-ignored):
#     packaging/openssl/install/lib/libssl.3.dylib, libcrypto.3.dylib
#     packaging/openssl/install/include/openssl/*.h
#     packaging/openssl/install/.version            — what was built
#     packaging/openssl/install/.build              — architectures, macOS minimum
#
# macOS only: the Linux packages use the distribution's OpenSSL.
#
# Usage:
#   packaging/fetch-openssl.sh                    # newest 3.5.x, built if not already
#   packaging/fetch-openssl.sh --version 3.5.9    # exactly this release
#   packaging/fetch-openssl.sh --force            # rebuild even if up to date
#   OPENSSL_SERIES=3.5 packaging/fetch-openssl.sh # the series to follow (default 3.5)
#   MACOS_MIN=11.0 MACOS_ARCHS="arm64 x86_64" packaging/fetch-openssl.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$SCRIPT_DIR/openssl"
INSTALL="$DEST/install"
SERIES="${OPENSSL_SERIES:-3.5}"
VERSION="${OPENSSL_VERSION:-}"
MACOS_MIN="${MACOS_MIN:-11.0}"
MACOS_ARCHS="${MACOS_ARCHS:-arm64 x86_64}"
FORCE=0

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --version)  VERSION="$2"; shift 2;;
        --series)   SERIES="$2"; shift 2;;
        --force)    FORCE=1; shift;;
        -h|--help)  sed -n '2,36p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

[ "$(uname -s)" = "Darwin" ] || die "this builds the macOS bundle's OpenSSL; Linux uses the system one"

BUILD="$MACOS_ARCHS | macOS $MACOS_MIN"
HAVE="$(cat "$INSTALL/.version" 2>/dev/null || true)"
# Built for other architectures or another minimum: treat as not built.
[ "$(cat "$INSTALL/.build" 2>/dev/null || true)" = "$BUILD" ] || HAVE=""

# The newest final release in $SERIES, from GitHub's release list: tags of the
# exact form openssl-<series>.<n>, not drafts, not pre-releases.  Empty on failure.
latest_in_series() {
    curl -fsSL --retry 2 --max-time 20 \
        "https://api.github.com/repos/openssl/openssl/releases?per_page=100" 2>/dev/null \
    | /usr/bin/python3 -c '
import json, re, sys
series = sys.argv[1]
pat = re.compile(r"^openssl-" + re.escape(series) + r"\.(\d+)$")
best = None
for r in json.load(sys.stdin):
    m = pat.match(r.get("tag_name", ""))
    if m and not r.get("draft") and not r.get("prerelease"):
        n = int(m.group(1))
        if best is None or n > best: best = n
if best is not None: print("%s.%d" % (series, best))
' "$SERIES" 2>/dev/null || true
}

if [ -z "$VERSION" ]; then
    VERSION="$(latest_in_series)"
    if [ -z "$VERSION" ]; then
        [ -n "$HAVE" ] || die "could not look up the latest OpenSSL $SERIES release (offline?), and none is built yet"
        warn "could not look up the latest OpenSSL $SERIES release (offline?); keeping $HAVE"
        exit 0
    fi
fi

if [ "$FORCE" = 0 ] && [ "$HAVE" = "$VERSION" ]; then
    note "OpenSSL $VERSION is up to date"
    exit 0
fi

PKG="openssl-$VERSION.tar.gz"
BASE="https://github.com/openssl/openssl/releases/download/openssl-$VERSION"
CACHE="$DEST/$PKG"
mkdir -p "$DEST"

want_sha="$(curl -fsSL --retry 2 "$BASE/$PKG.sha256" | awk '{print $1}')" \
    || die "could not fetch the checksum for OpenSSL $VERSION"
[[ "$want_sha" =~ ^[0-9a-f]{64}$ ]] || die "unexpected checksum file for OpenSSL $VERSION: $want_sha"

if [ ! -f "$CACHE" ]; then
    note "downloading $BASE/$PKG"
    curl -fL --retry 3 -o "$CACHE.part" "$BASE/$PKG" || die "download failed: $BASE/$PKG"
    mv "$CACHE.part" "$CACHE"
fi
got="$(shasum -a 256 "$CACHE" | cut -d' ' -f1)"
[ "$got" = "$want_sha" ] || { rm -f "$CACHE"; die "checksum mismatch for $PKG (got $got, want $want_sha); deleted it, re-run to download again"; }

note "building OpenSSL $VERSION ($MACOS_ARCHS, macOS $MACOS_MIN+, shared, OPENSSLDIR=/etc/ssl)${HAVE:+ — replacing $HAVE}"
rm -rf "$INSTALL" "$DEST/src" "$DEST"/arch-*
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
LOG="$DEST/build.log"
: > "$LOG"
MAJOR="${VERSION%%.*}"
first=""
for arch in $MACOS_ARCHS; do
    case "$arch" in
        arm64)  target="darwin64-arm64-cc" ;;
        x86_64) target="darwin64-x86_64-cc" ;;
        *) die "unsupported architecture: $arch" ;;
    esac
    src="$DEST/src-$arch"; out="$DEST/arch-$arch"
    note "  $arch"
    rm -rf "$src"; mkdir -p "$src"
    tar -xzf "$CACHE" -C "$src" --strip-components=1
    (
        cd "$src"
        # The final path is fixed with --prefix so install names and the
        # compiled-in paths are right; DESTDIR stages each architecture apart.
        ./Configure "$target" shared no-tests no-docs \
            --prefix="$INSTALL" --libdir=lib --openssldir=/etc/ssl \
            -mmacosx-version-min="$MACOS_MIN"
        make -j"$JOBS"
        make install_sw DESTDIR="$out"   # libraries + headers only
    ) >> "$LOG" 2>&1 || { tail -40 "$LOG" >&2; die "OpenSSL build failed for $arch (full log: $LOG)"; }
    rm -rf "$src"
    [ -n "$first" ] || first="$out$INSTALL"
done

# Merge: the first architecture's tree, with every Mach-O replaced by a lipo of
# all of them.  The headers must agree, or one architecture would compile
# against the other's configuration.
mkdir -p "$(dirname "$INSTALL")"
cp -R "$first" "$INSTALL"
for arch in $MACOS_ARCHS; do
    diff -r "$first/include" "$DEST/arch-$arch$INSTALL/include" >/dev/null \
        || die "OpenSSL headers differ between architectures ($arch); a merged include dir would be wrong"
done
while IFS= read -r rel; do
    slices=()
    for arch in $MACOS_ARCHS; do slices+=("$DEST/arch-$arch$INSTALL/$rel"); done
    lipo -create "${slices[@]}" -output "$INSTALL/$rel"
done < <(cd "$first" && find . -type f \( -name '*.dylib' -o -path './bin/*' \) | sed 's|^\./||' \
           | while IFS= read -r f; do file -b "$first/$f" | grep -q Mach-O && echo "$f"; done)
rm -rf "$DEST"/arch-*

for lib in "libssl.$MAJOR.dylib" "libcrypto.$MAJOR.dylib"; do
    [ -f "$INSTALL/lib/$lib" ] || die "$lib not produced"
    for arch in $MACOS_ARCHS; do
        lipo -archs "$INSTALL/lib/$lib" | tr ' ' '\n' | grep -qx "$arch" || die "$lib lacks $arch"
    done
done
# Shared only: a static copy linked into Ruby or the app would be a second
# OpenSSL in the bundle, which is exactly what this build exists to prevent.
rm -f "$INSTALL/lib/libssl.a" "$INSTALL/lib/libcrypto.a"
printf '%s\n' "$VERSION" > "$INSTALL/.version"
printf '%s\n' "$BUILD" > "$INSTALL/.build"
find "$DEST" -maxdepth 1 -name 'openssl-*.tar.gz' ! -name "$PKG" -delete   # older tarballs
note "OpenSSL $VERSION staged in $INSTALL ($(lipo -archs "$INSTALL/lib/libcrypto.$MAJOR.dylib"))"
