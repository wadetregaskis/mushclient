#!/usr/bin/env bash
#
# fetch-ruby.sh — build a self-contained, relocatable Ruby from source, for
# bundling.  Replaces the Apple-deprecated system libruby (EOL 2.6) on macOS
# and provides a clean, portable runtime on Linux so the app does not depend
# on host system Ruby packages.
#
# Strategy:
#   * libyaml is built *statically* into a private deps prefix (psych bakes it in;
#     tiny + Ruby-only).
#   * OpenSSL is NOT built here: links the shared OpenSSL (Homebrew openssl@3 on
#     macOS, system libssl-dev on Linux).
#   * Build Ruby with --enable-shared --enable-load-relative: libruby is a single
#     shared library whose load path (the stdlib + the C extensions) is computed at
#     runtime *relative to the shared library's own location* (via dladdr).  Drop the whole
#     install/lib tree into Contents/lib (macOS) or lib/ (Linux) and Ruby finds its stdlib
#     with no environment variables — see CMakeLists.txt + ruby_engine.cpp.
#
#   staging (git-ignored):
#     packaging/ruby/install/lib/libruby.<ver>.dylib (or .so on Linux)
#     packaging/ruby/install/lib/ruby/<ver>/           — stdlib + arch extensions
#     packaging/ruby/install/include/ruby-<ver>/       — headers (+ arch config.h)
#
# Usage:
#   packaging/fetch-ruby.sh                 # default versions (below)
#   RUBY_VERSION=4.0.5 packaging/fetch-ruby.sh
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
DEST="$SCRIPT_DIR/ruby"
RUBY_VERSION="${RUBY_VERSION:-4.0.5}"
YAML_VERSION="${YAML_VERSION:-0.2.5}"

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --ruby-version)  RUBY_VERSION="$2"; shift 2;;
        --yaml-version)  YAML_VERSION="$2"; shift 2;;
        -h|--help)       sed -n '2,27p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

OS="$(uname -s)"
[ "$OS" = "Darwin" ] || [ "$OS" = "Linux" ] || die "this script builds Ruby for macOS and Linux"
ARCH="$(uname -m)"                                   # arm64 / aarch64 | x86_64

if [ "$OS" = "Darwin" ]; then
    OPENSSL_PREFIX="${OPENSSL_PREFIX:-$(brew --prefix openssl@3 2>/dev/null || true)}"
    [ -n "$OPENSSL_PREFIX" ] && [ -d "$OPENSSL_PREFIX/include/openssl" ] \
        || die "Homebrew openssl@3 not found — 'brew install openssl@3' (shared with the app)"
else
    OPENSSL_PREFIX="${OPENSSL_PREFIX:-$(pkg-config --variable=prefix openssl 2>/dev/null || true)}"
    if [ -z "$OPENSSL_PREFIX" ]; then
        for d in /usr /usr/local /tmp/sysroot/usr; do
            if [ -d "$d/include/openssl" ]; then
                OPENSSL_PREFIX="$d"
                break
            fi
        done
    fi
    [ -n "$OPENSSL_PREFIX" ] && [ -d "$OPENSSL_PREFIX/include/openssl" ] \
        || die "OpenSSL headers not found — please install libssl-dev"
fi

INSTALL="$DEST/install"                              # final staged Ruby
DEPS="$DEST/deps"                                    # static libyaml
SRC="$DEST/src"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
mkdir -p "$DEST" "$SRC"

# download $1=url $2=cache-file ; verify it is a gzip tarball
fetch() {
    local url="$1" out="$2"
    if [ ! -f "$out" ]; then
        note "downloading $url"
        curl -fL --retry 3 -o "$out" "$url" || die "download failed: $url"
    fi
    file "$out" | grep -q gzip || die "not a gzip tarball (got an HTML page?): $out"
}

extract() {  # $1=tarball $2=dest-dir (stripped of the top component)
    rm -rf "$2"; mkdir -p "$2"
    tar -xzf "$1" -C "$2" --strip-components=1
}

# ── 1. OpenSSL ────────────────────────────────────────────────────────────────
note "OpenSSL: $("$OPENSSL_PREFIX/bin/openssl" version 2>/dev/null || echo "$OPENSSL_PREFIX")"

# ── 2. libyaml (static) ───────────────────────────────────────────────────────
# Ruby's psych (YAML) extension links this.
if [ ! -f "$DEPS/lib/libyaml.a" ]; then
    PKG="yaml-$YAML_VERSION.tar.gz"
    fetch "https://github.com/yaml/libyaml/releases/download/$YAML_VERSION/$PKG" "$DEST/$PKG"
    extract "$DEST/$PKG" "$SRC/yaml"
    note "building libyaml $YAML_VERSION (static)"
    YAML_CFLAGS="-O2"
    [ "$OS" = "Linux" ] && YAML_CFLAGS="-fPIC -O2"
    (
        cd "$SRC/yaml"
        CFLAGS="$YAML_CFLAGS" ./configure --prefix="$DEPS" --disable-shared --enable-static >/dev/null
        make -j"$JOBS" >/dev/null
        make install >/dev/null
    ) || die "libyaml build failed"
fi
[ -f "$DEPS/lib/libyaml.a" ] || die "libyaml static lib missing"

# ── 3. Ruby (shared, load-relative) ───────────────────────────────────────────
SHORT="${RUBY_VERSION%.*}"                           # 4.0.5 -> 4.0
PKG="ruby-$RUBY_VERSION.tar.gz"
fetch "https://cache.ruby-lang.org/pub/ruby/$SHORT/$PKG" "$DEST/$PKG"
extract "$DEST/$PKG" "$SRC/ruby"
note "building Ruby $RUBY_VERSION (shared, load-relative, $ARCH)"
rm -rf "$INSTALL"

RB_CFLAGS="-I$DEPS/include -I$OPENSSL_PREFIX/include"
RB_LDFLAGS="-L$DEPS/lib -L$OPENSSL_PREFIX/lib"
EXTRA_PKG_CONFIG=""

if [ "$OS" = "Linux" ]; then
    RB_CFLAGS="$RB_CFLAGS -fPIC"
    if [ -d "$OPENSSL_PREFIX/lib/$ARCH-linux-gnu" ]; then
        RB_LDFLAGS="$RB_LDFLAGS -L$OPENSSL_PREFIX/lib/$ARCH-linux-gnu"
    fi
    if [ -d "$OPENSSL_PREFIX/lib/$ARCH-linux-gnu/pkgconfig" ]; then
        EXTRA_PKG_CONFIG=":$OPENSSL_PREFIX/lib/$ARCH-linux-gnu/pkgconfig"
    fi
fi

# Build self-contained from source tarball without probing host ruby
# (prevents configure from probing incomplete host rubies and spewing missing-baseruby errors).
BASERUBY_OPT="--without-baseruby"

(
    cd "$SRC/ruby"
    note "configuring Ruby $RUBY_VERSION..."
    ./configure \
        --prefix="$INSTALL" \
        --enable-shared --disable-static \
        --enable-load-relative \
        --disable-install-doc \
        --without-gmp \
        --with-openssl-dir="$OPENSSL_PREFIX" \
        --with-libyaml-dir="$DEPS" \
        $BASERUBY_OPT \
        PKG_CONFIG_PATH="$DEPS/lib/pkgconfig:$OPENSSL_PREFIX/lib/pkgconfig${EXTRA_PKG_CONFIG}" \
        CFLAGS="$RB_CFLAGS" \
        LDFLAGS="$RB_LDFLAGS" \
        optflags="-O2" >/dev/null

    note "compiling Ruby $RUBY_VERSION (using $JOBS parallel jobs)..."
    make -j"$JOBS" >/dev/null

    note "installing Ruby $RUBY_VERSION to staging..."
    make install >/dev/null
) || die "Ruby build failed"

# ── 4. make libruby relocatable + sign nested Mach-O ──────────────────────────
if [ "$OS" = "Darwin" ]; then
    DYLIB="$(find "$INSTALL/lib" -maxdepth 1 -type f -name 'libruby.*.dylib' | head -1)"
    [ -n "$DYLIB" ] || die "libruby dylib not produced (shared build failed?)"
    install_name_tool -id "@rpath/$(basename "$DYLIB")" "$DYLIB"

    # arm64 requires every Mach-O to carry at least an ad-hoc signature to load.
    xattr -cr "$INSTALL" 2>/dev/null || true
    codesign --force --sign - "$DYLIB" 2>/dev/null || true
    find "$INSTALL/lib/ruby" -name '*.bundle' -print0 \
        | xargs -0 -n1 codesign --force --sign - 2>/dev/null || true
else
    SO="$(find "$INSTALL/lib" -maxdepth 1 \( -type f -o -type l \) -name 'libruby.so*' | head -1)"
    [ -n "$SO" ] || die "libruby.so not produced (shared build failed?)"
    # Ensure unversioned symlink libruby.so exists for CMake
    if [ ! -e "$INSTALL/lib/libruby.so" ]; then
        ln -sf "$(basename "$SO")" "$INSTALL/lib/libruby.so"
    fi
fi

# ── 5. report ─────────────────────────────────────────────────────────────────
note "staged relocatable Ruby $RUBY_VERSION:"
if [ "$OS" = "Darwin" ]; then
    note "  libruby:  $DYLIB  ($(lipo -archs "$DYLIB" 2>/dev/null || echo '?'))"
    note "  id:       $(otool -D "$DYLIB" | tail -1)"
    note "  stdlib:   $INSTALL/lib/ruby/$SHORT.0/"
    note "  libruby non-system deps (should be none — psych/libyaml are static):"
    otool -L "$DYLIB" | tail -n +2 | awk '{print $1}' \
        | grep -vE '^(/usr/lib/|/System/|@)' | sed 's/^/    LEAK: /' \
        || note "    (none — only system libs + @rpath)"
else
    note "  libruby:  $(find "$INSTALL/lib" -maxdepth 1 -name 'libruby.so*' | head -1) ($ARCH)"
    note "  stdlib:   $INSTALL/lib/ruby/$SHORT.0/"
fi
note "build with:  packaging/build-app.sh   (or a normal CMake build; auto-detects this)"
