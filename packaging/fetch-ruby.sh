#!/usr/bin/env bash
#
# fetch-ruby.sh — build a self-contained, relocatable Ruby from source, for
# bundling.  Replaces the Apple-deprecated system libruby (EOL 2.6) so the app
# depends on neither Homebrew nor the macOS system Ruby.framework.
#
# Strategy:
#   * libyaml is built *statically* into a private deps prefix (psych bakes it in;
#     macOS ships no linkable copy, and it is tiny + Ruby-only).
#   * OpenSSL is NOT built here.  The openssl/digest extensions link Homebrew's
#     openssl@3 — the SAME OpenSSL the app itself links — so build-app.sh can
#     bundle ONE copy and point the app, Python and Ruby all at it (no duplicate
#     OpenSSL archives in the .app).  Homebrew openssl@3 is already a build
#     prerequisite for the app, so this adds nothing; the shipped .app stays
#     Homebrew-free at runtime (the copy is relocated into Contents/Frameworks).
#   * Build Ruby with --enable-shared --enable-load-relative: libruby is a single
#     dylib whose load path (the stdlib + the .bundle extensions) is computed at
#     runtime *relative to the dylib's own location* (via dladdr).  Drop the whole
#     install/lib tree into Contents/Resources/ruby/lib and Ruby finds its stdlib
#     with no environment variables — see CMakeLists.txt + ruby_engine.cpp.
#
#   staging (git-ignored):
#     packaging/ruby/install/lib/libruby.<ver>.dylib   — linked in (@rpath id)
#     packaging/ruby/install/lib/ruby/<ver>/           — stdlib + arch .bundles
#     packaging/ruby/install/include/ruby-<ver>/       — headers (+ arch config.h)
#
# Usage:
#   packaging/fetch-ruby.sh                 # default versions (below)
#   RUBY_VERSION=4.0.6 packaging/fetch-ruby.sh   # override the default below
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$SCRIPT_DIR/ruby"
RUBY_VERSION="${RUBY_VERSION:-4.0.5}"
YAML_VERSION="${YAML_VERSION:-0.2.5}"

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || die "this script builds Ruby for macOS"
ARCH="$(uname -m)"                                   # arm64 | x86_64

# OpenSSL comes from Homebrew openssl@3 — the same copy the app links — so the
# bundle ends up with a single shared OpenSSL (build-app.sh relocates it and
# repoints the app, Python and Ruby at it).  Override with OPENSSL_PREFIX=...
OPENSSL_PREFIX="${OPENSSL_PREFIX:-$(brew --prefix openssl@3 2>/dev/null || true)}"
[ -n "$OPENSSL_PREFIX" ] && [ -d "$OPENSSL_PREFIX/include/openssl" ] \
    || die "Homebrew openssl@3 not found — 'brew install openssl@3' (shared with the app)"

INSTALL="$DEST/install"                              # final staged Ruby
DEPS="$DEST/deps"                                    # static libyaml (OpenSSL = Homebrew)
SRC="$DEST/src"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || true)"
JOBS="${JOBS:-4}"
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

# ── 1. OpenSSL — Homebrew openssl@3 (shared with the app; not built here) ─────
note "OpenSSL: $("$OPENSSL_PREFIX/bin/openssl" version 2>/dev/null || echo "$OPENSSL_PREFIX") — Homebrew, shared with the app"

# ── 2. libyaml (static) ───────────────────────────────────────────────────────
# Ruby's psych (YAML) extension links this.
if [ ! -f "$DEPS/lib/libyaml.a" ]; then
    PKG="yaml-$YAML_VERSION.tar.gz"
    fetch "https://github.com/yaml/libyaml/releases/download/$YAML_VERSION/$PKG" "$DEST/$PKG"
    extract "$DEST/$PKG" "$SRC/yaml"
    note "building libyaml $YAML_VERSION (static)"
    (
        cd "$SRC/yaml"
        ./configure --prefix="$DEPS" --disable-shared --enable-static >/dev/null
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
(
    cd "$SRC/ruby"
    ./configure \
        --prefix="$INSTALL" \
        --enable-shared --disable-static \
        --enable-load-relative \
        --disable-install-doc \
        --without-gmp \
        --with-openssl-dir="$OPENSSL_PREFIX" \
        --with-libyaml-dir="$DEPS" \
        PKG_CONFIG_PATH="$DEPS/lib/pkgconfig:$OPENSSL_PREFIX/lib/pkgconfig" \
        CFLAGS="-I$DEPS/include -I$OPENSSL_PREFIX/include" \
        LDFLAGS="-L$DEPS/lib -L$OPENSSL_PREFIX/lib" \
        optflags="-O2" >/dev/null
    make -j"$JOBS" >/dev/null
    make install >/dev/null
) || die "Ruby build failed"

# ── 4. make libruby relocatable + sign nested Mach-O ──────────────────────────
DYLIB="$(find "$INSTALL/lib" -maxdepth 1 -type f -name 'libruby.*.dylib' | head -1)"
[ -n "$DYLIB" ] || die "libruby dylib not produced (shared build failed?)"
install_name_tool -id "@rpath/$(basename "$DYLIB")" "$DYLIB"

# arm64 requires every Mach-O to carry at least an ad-hoc signature to load.
xattr -cr "$INSTALL" 2>/dev/null || true
codesign --force --sign - "$DYLIB" 2>/dev/null || true
find "$INSTALL/lib/ruby" -name '*.bundle' -print0 \
    | xargs -0 -n1 codesign --force --sign - 2>/dev/null || true

# ── 5. report ─────────────────────────────────────────────────────────────────
note "staged relocatable Ruby $RUBY_VERSION:"
note "  libruby:  $DYLIB  ($(lipo -archs "$DYLIB" 2>/dev/null || echo '?'))"
note "  id:       $(otool -D "$DYLIB" | tail -1)"
note "  stdlib:   $INSTALL/lib/ruby/$SHORT.0/"
note "  libruby non-system deps (should be none — psych/libyaml are static):"
otool -L "$DYLIB" | tail -n +2 | awk '{print $1}' \
    | grep -vE '^(/usr/lib/|/System/|@)' | sed 's/^/    LEAK: /' \
    || note "    (none — only system libs + @rpath)"
note "  openssl/digest extensions link Homebrew openssl@3 (absolute path); this is"
note "  expected — build-app.sh relocates it into the .app as the shared OpenSSL."
note "build with:  packaging/build-app.sh   (or a normal CMake build; auto-detects this)"
