#!/usr/bin/env bash
#
# fetch-python.sh — stage a clean, redistributable Python runtime for bundling.
#
# macOS:
#   Downloads the official python.org installer and extracts its
#   Python.framework WITHOUT installing it system-wide (pkgutil --expand-full
#   unpacks the component payloads in place).  The result is a pristine,
#   PSF-licensed, universal2 framework that build-app.sh bundles into the .app.
#
# Linux:
#   Stages a portable, standalone Python 3 embed environment into
#   packaging/python/install, containing include/ headers, lib/libpython3.so,
#   and the standard library.
#
#   staging dir:
#     macOS: packaging/python/Python.framework   (git-ignored)
#     Linux: packaging/python/install            (git-ignored)
#
# Usage:
#   packaging/fetch-python.sh                 # default version
#   PYTHON_VERSION=3.13.1 packaging/fetch-python.sh
#   packaging/fetch-python.sh --system        # stage from host system/sysroot (Linux)
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
DEST="$SCRIPT_DIR/python"
VERSION="${PYTHON_VERSION:-3.13.1}"
PKG_OVERRIDE=""
FORCE_SYSTEM=0

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --pkg)      PKG_OVERRIDE="$2"; shift 2;;
        --version)  VERSION="$2"; shift 2;;
        --system)   FORCE_SYSTEM=1; shift;;
        -h|--help)  sed -n '2,24p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

OS="$(uname -s)"
[ "$OS" = "Darwin" ] || [ "$OS" = "Linux" ] || die "this script is for macOS and Linux"
mkdir -p "$DEST"

# ════════════════════════════════════════════════════════════════════════════
# macOS: python.org universal2 framework
# ════════════════════════════════════════════════════════════════════════════
if [ "$OS" = "Darwin" ]; then
    MACOS_VERSION="${VERSION:-${PYTHON_VERSION:-3.14.6}}"
    SHORT="${MACOS_VERSION%.*}"                       # 3.14.6 -> 3.14
    PKG="python-$MACOS_VERSION-macos11.pkg"           # universal2 installer
    URL="https://www.python.org/ftp/python/$MACOS_VERSION/$PKG"
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
    cp -R "$payload" "$FW"
    chmod -R u+w "$FW"
    xattr -cr "$FW" 2>/dev/null || true

    ABS="/Library/Frameworks/Python.framework"
    CONTAINER="$(cd "$DEST" && pwd)"
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
            codesign --force -s - "$f" 2>/dev/null || true
        fi
    done < <(find "$FW" -type f \( -name '*.dylib' -o -name '*.so' -o -perm +111 \))

    note "staged $FW  ($(lipo -archs "$FW/Versions/$SHORT/Python" 2>/dev/null))"
    note "build with:  packaging/build-app.sh   (auto-detects this framework)"
    exit 0
fi

# ════════════════════════════════════════════════════════════════════════════
# Linux: portable standalone CPython or system staging
# ════════════════════════════════════════════════════════════════════════════
INSTALL="$DEST/install"
ARCH="$(uname -m)"
case "$ARCH" in
    x86_64)  STANDALONE_ARCH="x86_64-unknown-linux-gnu" ;;
    aarch64) STANDALONE_ARCH="aarch64-unknown-linux-gnu" ;;
    *)       STANDALONE_ARCH="" ;;
esac

staged=0

if [ "$FORCE_SYSTEM" = 0 ] && [ -n "$STANDALONE_ARCH" ]; then
    RELEASE_TAG="20250115"
    PKG="cpython-3.13.1+${RELEASE_TAG}-${STANDALONE_ARCH}-install_only.tar.gz"
    URL="https://github.com/astral-sh/python-build-standalone/releases/download/${RELEASE_TAG}/${PKG}"
    CACHE="${PKG_OVERRIDE:-$DEST/$PKG}"

    note "fetching standalone Python 3.13 ($ARCH)"
    if [ ! -f "$CACHE" ]; then
        if curl -fL --retry 3 -o "$CACHE" "$URL" 2>/dev/null; then
            note "downloaded $PKG"
        else
            warn "download failed; falling back to local Python installation"
            rm -f "$CACHE"
        fi
    fi

    if [ -f "$CACHE" ] && file "$CACHE" | grep -q gzip; then
        note "extracting standalone Python into $INSTALL"
        rm -rf "$INSTALL"
        mkdir -p "$INSTALL"
        tar -xzf "$CACHE" -C "$INSTALL" --strip-components=1
        staged=1
    fi
fi

if [ "$staged" = 0 ]; then
    note "staging Python from local environment"
    command -v python3 >/dev/null 2>&1 || die "python3 not found"
    
    PY_VER="$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
    PY_INC="$(python3 -c 'import sysconfig; print(sysconfig.get_path("include"))' 2>/dev/null || true)"
    PY_LIBDIR="$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("LIBDIR"))' 2>/dev/null || true)"
    PY_SO="$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("LDLIBRARY"))' 2>/dev/null || true)"
    PY_STDLIB="$(python3 -c 'import sysconfig; print(sysconfig.get_path("stdlib"))' 2>/dev/null || true)"

    [ -d "$PY_INC" ] || die "Python include directory not found: $PY_INC (install python3-dev)"

    rm -rf "$INSTALL"
    mkdir -p "$INSTALL/include" "$INSTALL/lib" "$INSTALL/bin"

    cp -R "$PY_INC" "$INSTALL/include/"
    if [ -d "$PY_STDLIB" ]; then
        cp -R "$PY_STDLIB" "$INSTALL/lib/"
    fi
    for ldir in "$PY_LIBDIR" "/usr/lib/$ARCH-linux-gnu" "/tmp/sysroot/usr/lib/$ARCH-linux-gnu"; do
        if [ -n "$PY_SO" ] && [ -f "$ldir/$PY_SO" ]; then
            cp "$ldir/$PY_SO"* "$INSTALL/lib/" 2>/dev/null || true
            break
        fi
    done
    cp "$(command -v python3)" "$INSTALL/bin/python3"
    if command -v "python3-config" >/dev/null 2>&1; then
        cp "$(command -v python3-config)" "$INSTALL/bin/"
    fi
fi

[ -d "$INSTALL/include" ] || die "staging failed: $INSTALL/include missing"
note "staged Python into $INSTALL ($ARCH)"
note "build with:  packaging/build-app.sh   (auto-detects this)"
