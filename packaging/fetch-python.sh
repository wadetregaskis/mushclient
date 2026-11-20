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
#   PYTHON_VERSION=3.15.2 packaging/fetch-python.sh
#   packaging/fetch-python.sh --system        # stage from host system/sysroot (Linux)
#
# Downloads are checked against pinned SHA-256s for the default version; for
# another, give PYTHON_SHA256=... (macOS: python.org's release page lists one per
# file; Linux: python-build-standalone's SHA256SUMS for the release).  On Linux a
# failed download is an error: staging the host's own Python instead (which ties
# the package to that distribution's libpython) happens only with --system, or
# with MC_ALLOW_SYSTEM_PYTHON=1 to fall back to it.
#
set -euo pipefail

# A local environment script (a sysroot in MC_SYSROOT, a non-root toolchain), if this
# checkout has one: packaging/local-env.sh, which git ignores.  Never one from /tmp,
# where anyone on the machine could have put it.
if [ -f "$(dirname "${BASH_SOURCE[0]}")/local-env.sh" ]; then
    set +u
    # shellcheck disable=SC1091
    . "$(dirname "${BASH_SOURCE[0]}")/local-env.sh"
    set -u
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$SCRIPT_DIR/python"
DEFAULT_VERSION="3.15.2"
VERSION="${PYTHON_VERSION:-$DEFAULT_VERSION}"
# The pinned SHA-256s of DEFAULT_VERSION's downloads.
#   macOS: python-3.15.2-macos11.pkg, from https://www.python.org/downloads/release/python-3152/
#   Linux: python-build-standalone 20261026's SHA256SUMS
#          (https://github.com/astral-sh/python-build-standalone/releases/tag/20261026)
DEFAULT_SHA256_MACOS="2c6104475535b720b45774c68b0dfc9fcca2235353cf9ff81e05488f89845396"
DEFAULT_SHA256_LINUX_X86_64="08d4ae9024a7cfbd3b70e559241afa2571cd3208d7bd5f96f88e0080dd4092aa"
DEFAULT_SHA256_LINUX_AARCH64="6fc73e8a4c80a65d2a0ed21535c65383f7b9f96bb58a5f40ed035bd7ee318616"
PKG_OVERRIDE=""
FORCE_SYSTEM=0

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --pkg)      PKG_OVERRIDE="$2"; shift 2;;
        --version)  VERSION="$2"; shift 2;;
        --system)   FORCE_SYSTEM=1; shift;;
        -h|--help)  sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

OS="$(uname -s)"
[ "$OS" = "Darwin" ] || [ "$OS" = "Linux" ] || die "this script is for macOS and Linux"
mkdir -p "$DEST"

# sha256_check FILE WANT -- delete FILE and stop if it isn't what was pinned.
sha256_check() {
    local got
    got="$( (sha256sum "$1" 2>/dev/null || shasum -a 256 "$1") | cut -d' ' -f1)"
    [ "$got" = "$2" ] || { rm -f "$1"; die "checksum mismatch for $1 (got $got, want $2); deleted it, re-run to download again"; }
}

# pinned_sha DEFAULT -- the SHA-256 to check against: PYTHON_SHA256 if given, else
# DEFAULT, which is only right for DEFAULT_VERSION.  (Called as a=$(pinned_sha x), so
# that set -e stops the script when it dies.)
pinned_sha() {
    if [ -n "${PYTHON_SHA256:-}" ]; then echo "$PYTHON_SHA256"; return; fi
    [ "$VERSION" = "$DEFAULT_VERSION" ] || die "no SHA-256 known for Python $VERSION's download: give it as PYTHON_SHA256=..."
    echo "$1"
}

# ════════════════════════════════════════════════════════════════════════════
# macOS: python.org universal2 framework
# ════════════════════════════════════════════════════════════════════════════
if [ "$OS" = "Darwin" ]; then
    MACOS_VERSION="$VERSION"
    SHORT="${MACOS_VERSION%.*}"                       # 3.15.2 -> 3.15
    PKG="python-$MACOS_VERSION-macos11.pkg"           # universal2 installer
    URL="https://www.python.org/ftp/python/$MACOS_VERSION/$PKG"
    CACHE="${PKG_OVERRIDE:-$DEST/$PKG}"

    WANT_SHA="$(pinned_sha "$DEFAULT_SHA256_MACOS")"
    if [ ! -f "$CACHE" ]; then
        note "downloading $URL  (~80 MB)"
        curl -fL --retry 3 -o "$CACHE.part" "$URL" || die "download failed"
        mv "$CACHE.part" "$CACHE"
    fi
    # (a --pkg of their own is theirs to vouch for)
    [ -n "$PKG_OVERRIDE" ] || sha256_check "$CACHE" "$WANT_SHA"

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
    x86_64)  STANDALONE_ARCH="x86_64-unknown-linux-gnu";  STANDALONE_SHA="$DEFAULT_SHA256_LINUX_X86_64" ;;
    aarch64) STANDALONE_ARCH="aarch64-unknown-linux-gnu"; STANDALONE_SHA="$DEFAULT_SHA256_LINUX_AARCH64" ;;
    *)       STANDALONE_ARCH="";                          STANDALONE_SHA="" ;;
esac

staged=0

if [ "$FORCE_SYSTEM" = 0 ] && [ -n "$STANDALONE_ARCH" ]; then
    RELEASE_TAG="20261026"
    PKG="cpython-${VERSION}+${RELEASE_TAG}-${STANDALONE_ARCH}-install_only.tar.gz"
    URL="https://github.com/astral-sh/python-build-standalone/releases/download/${RELEASE_TAG}/${PKG}"
    CACHE="${PKG_OVERRIDE:-$DEST/$PKG}"

    WANT_SHA="$(pinned_sha "$STANDALONE_SHA")"
    note "fetching standalone Python $VERSION ($ARCH)"
    if [ ! -f "$CACHE" ]; then
        if curl -fL --retry 3 -o "$CACHE.part" "$URL"; then
            mv "$CACHE.part" "$CACHE"
            note "downloaded $PKG"
        else
            rm -f "$CACHE.part"
            [ "${MC_ALLOW_SYSTEM_PYTHON:-0}" = 1 ] \
                || die "could not download $URL (MC_ALLOW_SYSTEM_PYTHON=1, or --system, stages this machine's own Python instead)"
            warn "DOWNLOAD FAILED: staging this machine's own Python instead (MC_ALLOW_SYSTEM_PYTHON=1); the package will need its libpython"
        fi
    fi
    [ ! -f "$CACHE" ] || [ -n "$PKG_OVERRIDE" ] || sha256_check "$CACHE" "$WANT_SHA"

    if [ -f "$CACHE" ]; then
        note "extracting standalone Python into $INSTALL"
        rm -rf "$INSTALL"
        mkdir -p "$INSTALL"
        tar -xzf "$CACHE" -C "$INSTALL" --strip-components=1
        staged=1
    fi
fi

if [ "$staged" = 0 ]; then
    if [ "$FORCE_SYSTEM" = 0 ] && [ -z "$STANDALONE_ARCH" ]; then
        [ "${MC_ALLOW_SYSTEM_PYTHON:-0}" = 1 ] \
            || die "no standalone Python for $ARCH: use --system (or MC_ALLOW_SYSTEM_PYTHON=1) to stage this machine's own"
        warn "no standalone Python for $ARCH: staging this machine's own (MC_ALLOW_SYSTEM_PYTHON=1)"
    fi
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
    for ldir in "$PY_LIBDIR" "/usr/lib/$ARCH-linux-gnu" ${MC_SYSROOT:+"$MC_SYSROOT/usr/lib/$ARCH-linux-gnu"}; do
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
