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
#
# macOS only: the Linux packages use the distribution's OpenSSL.
#
# Usage:
#   packaging/fetch-openssl.sh                    # newest 3.5.x, built if not already
#   packaging/fetch-openssl.sh --version 3.5.9    # exactly this release
#   packaging/fetch-openssl.sh --force            # rebuild even if up to date
#   OPENSSL_SERIES=3.5 packaging/fetch-openssl.sh # the series to follow (default 3.5)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$SCRIPT_DIR/openssl"
INSTALL="$DEST/install"
SERIES="${OPENSSL_SERIES:-3.5}"
VERSION="${OPENSSL_VERSION:-}"
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

case "$(uname -m)" in
    arm64)  TARGET="darwin64-arm64-cc" ;;
    x86_64) TARGET="darwin64-x86_64-cc" ;;
    *) die "unsupported architecture: $(uname -m)" ;;
esac

HAVE="$(cat "$INSTALL/.version" 2>/dev/null || true)"

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

note "building OpenSSL $VERSION ($TARGET, shared, OPENSSLDIR=/etc/ssl)${HAVE:+ — replacing $HAVE}"
rm -rf "$INSTALL" "$DEST/src"
mkdir -p "$DEST/src"
tar -xzf "$CACHE" -C "$DEST/src" --strip-components=1
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
LOG="$DEST/build.log"
(
    cd "$DEST/src"
    ./Configure "$TARGET" shared no-tests no-docs \
        --prefix="$INSTALL" --libdir=lib --openssldir=/etc/ssl
    make -j"$JOBS"
    make install_sw      # libraries + headers only: no docs, no openssldir files
) > "$LOG" 2>&1 || { tail -40 "$LOG" >&2; die "OpenSSL build failed (full log: $LOG)"; }

MAJOR="${VERSION%%.*}"
for lib in "libssl.$MAJOR.dylib" "libcrypto.$MAJOR.dylib"; do
    [ -f "$INSTALL/lib/$lib" ] || die "$lib not produced"
done
# Shared only: a static copy linked into Ruby or the app would be a second
# OpenSSL in the bundle, which is exactly what this build exists to prevent.
rm -f "$INSTALL/lib/libssl.a" "$INSTALL/lib/libcrypto.a"
printf '%s\n' "$VERSION" > "$INSTALL/.version"
rm -rf "$DEST/src"
find "$DEST" -maxdepth 1 -name 'openssl-*.tar.gz' ! -name "$PKG" -delete   # older tarballs
note "OpenSSL $VERSION staged in $INSTALL"
