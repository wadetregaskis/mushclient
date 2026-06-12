#!/usr/bin/env bash
#
# fetch-python.sh — stage a clean, redistributable Python framework for bundling.
#
# Downloads the official python.org macOS installer and extracts its
# Python.framework WITHOUT installing it system-wide (pkgutil --expand-full
# unpacks the component payloads in place).  The result is a pristine,
# PSF-licensed, universal2 framework that build-app.sh bundles into the .app
# (replacing the Xcode framework, which isn't cleanly redistributable).
#
#   staging dir:  packaging/python/Python.framework   (git-ignored)
#   pkg cache:    packaging/python/<pkg>              (git-ignored)
#
# Usage:
#   packaging/fetch-python.sh                 # default version (below)
#   PYTHON_VERSION=3.14.6 packaging/fetch-python.sh
#   packaging/fetch-python.sh --pkg ~/Downloads/python-3.14.6-macos11.pkg
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$SCRIPT_DIR/python"
VERSION="${PYTHON_VERSION:-3.14.6}"
PKG_OVERRIDE=""

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --pkg)      PKG_OVERRIDE="$2"; shift 2;;
        --version)  VERSION="$2"; shift 2;;
        -h|--help)  sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

[ "$(uname -s)" = "Darwin" ] || die "the python.org framework is macOS-only"
SHORT="${VERSION%.*}"                       # 3.14.6 -> 3.14
PKG="python-$VERSION-macos11.pkg"           # universal2 installer
URL="https://www.python.org/ftp/python/$VERSION/$PKG"
mkdir -p "$DEST"
CACHE="${PKG_OVERRIDE:-$DEST/$PKG}"

if [ ! -f "$CACHE" ]; then
    note "downloading $URL  (~75 MB)"
    curl -fL --retry 3 -o "$CACHE" "$URL" || die "download failed"
fi

note "extracting Python.framework (pkgutil --expand-full, no system install)"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
pkgutil --expand-full "$CACHE" "$tmp/exp" >/dev/null

payload="$(echo "$tmp"/exp/Python_Framework*.pkg/Payload)"
[ -d "$payload" ] || die "framework payload not found in the pkg"
[ -d "$payload/Versions/$SHORT" ] || die "expected Versions/$SHORT inside the framework payload"

FW="$DEST/Python.framework"
rm -rf "$FW"
cp -R "$payload" "$FW"                      # the Payload's contents ARE the framework
chmod -R u+w "$FW"
# strip quarantine/provenance, or Gatekeeper assessment hangs on first run
xattr -cr "$FW" 2>/dev/null || true

# The framework, its tools, its bundled libs (libssl/libcrypto/libncurses…) and a
# handful of extension modules (_ssl, _hashlib, …) reference libpython + each
# other by the absolute /Library/Frameworks path they WOULD have if installed.
# Rewrite every such reference to @rpath and give each rewritten binary an
# @loader_path rpath back to the framework's container, so the framework is fully
# relocatable: it runs in place (CMake queries it), a build links it as @rpath,
# and the copy bundled into the .app resolves inside Contents/Frameworks.
ABS="/Library/Frameworks/Python.framework"
CONTAINER="$(cd "$DEST" && pwd)"            # the dir that holds Python.framework
note "making the framework relocatable (rewriting absolute install names)"
while IFS= read -r f; do
    [ -L "$f" ] && continue
    file "$f" 2>/dev/null | grep -q "Mach-O" || continue
    changed=0
    cid="$(otool -D "$f" 2>/dev/null | tail -1)"
    case "$cid" in "$ABS"/*)
        install_name_tool -id "@rpath/Python.framework/${cid#$ABS/}" "$f" 2>/dev/null && changed=1 ;;
    esac
    while IFS= read -r dep; do
        case "$dep" in "$ABS"/*)
            install_name_tool -change "$dep" "@rpath/Python.framework/${dep#$ABS/}" "$f" 2>/dev/null && changed=1 ;;
        esac
    done < <(otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}')
    if [ "$changed" = 1 ]; then
        rel="${f#$CONTAINER/}"; ups="$(printf '%s' "$rel" | tr -cd '/' | wc -c | tr -d ' ')"
        rp="@loader_path"; i=0; while [ "$i" -lt "$ups" ]; do rp="$rp/.."; i=$((i + 1)); done
        install_name_tool -add_rpath "$rp" "$f" 2>/dev/null || true
        codesign --force -s - "$f" 2>/dev/null || true   # ad-hoc; build-app re-signs with the real id
    fi
done < <(find "$FW" -type f \( -name '*.dylib' -o -name '*.so' -o -perm +111 \))

note "staged $FW  ($(lipo -archs "$FW/Versions/$SHORT/Python" 2>/dev/null))"
note "build with:  packaging/build-app.sh   (auto-detects this framework)"
