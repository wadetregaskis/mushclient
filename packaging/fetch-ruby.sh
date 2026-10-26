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
#   * OpenSSL is NOT built here.  On macOS it links the one OpenSSL the app
#     bundle ships (packaging/openssl, built by fetch-openssl.sh, which this runs
#     if needed); on Linux, the system libssl-dev.  The OpenSSL used is recorded
#     in install/.openssl so .build-mac.sh can tell when Ruby needs rebuilding.
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
OPENSSL_PREFIX="${OPENSSL_PREFIX:-}"
MACOS_MIN="${MACOS_MIN:-11.0}"
MACOS_ARCHS="${MACOS_ARCHS:-arm64 x86_64}"

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --ruby-version)    RUBY_VERSION="$2"; shift 2;;
        --yaml-version)    YAML_VERSION="$2"; shift 2;;
        --openssl-prefix)  OPENSSL_PREFIX="$2"; shift 2;;
        -h|--help)         sed -n '2,27p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

OS="$(uname -s)"
[ "$OS" = "Darwin" ] || [ "$OS" = "Linux" ] || die "this script builds Ruby for macOS and Linux"
ARCH="$(uname -m)"                                   # arm64 / aarch64 | x86_64

if [ "$OS" = "Darwin" ]; then
    # The bundle's own OpenSSL -- never "whatever Homebrew has", which differs
    # from Mac to Mac (openssl@3 here, openssl@4 there).
    if [ -z "$OPENSSL_PREFIX" ]; then
        OPENSSL_PREFIX="$SCRIPT_DIR/openssl/install"
        [ -f "$OPENSSL_PREFIX/.version" ] || "$SCRIPT_DIR/fetch-openssl.sh" \
            || die "could not build the bundle's OpenSSL (packaging/fetch-openssl.sh)"
    fi
    [ -d "$OPENSSL_PREFIX/include/openssl" ] || die "no OpenSSL headers under $OPENSSL_PREFIX"
else
    candidates=(
        "$OPENSSL_PREFIX"
        "$(pkg-config --variable=prefix openssl 2>/dev/null || true)"
        "$(PKG_CONFIG_SYSROOT_DIR="" pkg-config --variable=prefix openssl 2>/dev/null || true)"
        "${PKG_CONFIG_SYSROOT_DIR:-}/usr"
        "/usr"
        "/usr/local"
        "/tmp/sysroot/usr"
    )
    OPENSSL_PREFIX=""
    for d in "${candidates[@]}"; do
        if [ -n "$d" ] && [ -d "$d/include/openssl" ]; then
            OPENSSL_PREFIX="$d"
            break
        fi
    done
    [ -n "$OPENSSL_PREFIX" ] \
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
YAML_STAMP="$YAML_VERSION"
[ "$OS" = "Darwin" ] && YAML_STAMP="$YAML_VERSION | $MACOS_ARCHS | macOS $MACOS_MIN"
if [ ! -f "$DEPS/lib/libyaml.a" ] || [ "$(cat "$DEPS/.stamp" 2>/dev/null || true)" != "$YAML_STAMP" ]; then
    rm -rf "$DEPS"
    PKG="yaml-$YAML_VERSION.tar.gz"
    fetch "https://github.com/yaml/libyaml/releases/download/$YAML_VERSION/$PKG" "$DEST/$PKG"
    extract "$DEST/$PKG" "$SRC/yaml"
    note "building libyaml $YAML_VERSION (static)"
    YAML_CFLAGS="-O2"
    [ "$OS" = "Linux" ] && YAML_CFLAGS="-fPIC -O2"
    if [ "$OS" = "Darwin" ]; then
        YAML_CFLAGS="$YAML_CFLAGS -mmacosx-version-min=$MACOS_MIN"
        for a in $MACOS_ARCHS; do YAML_CFLAGS="$YAML_CFLAGS -arch $a"; done
    fi
    (
        cd "$SRC/yaml"
        CFLAGS="$YAML_CFLAGS" ./configure --prefix="$DEPS" --disable-shared --enable-static >/dev/null
        make -j"$JOBS" >/dev/null
        make install >/dev/null
    ) || die "libyaml build failed"
    printf '%s\n' "$YAML_STAMP" > "$DEPS/.stamp"
fi
[ -f "$DEPS/lib/libyaml.a" ] || die "libyaml static lib missing"

# ── 3. Ruby (shared, load-relative) ───────────────────────────────────────────
SHORT="${RUBY_VERSION%.*}"                           # 4.0.5 -> 4.0
PKG="ruby-$RUBY_VERSION.tar.gz"
fetch "https://cache.ruby-lang.org/pub/ruby/$SHORT/$PKG" "$DEST/$PKG"
note "building Ruby $RUBY_VERSION (shared, load-relative)"
rm -rf "$INSTALL"
RB_CFLAGS="-I$DEPS/include -I$OPENSSL_PREFIX/include"
RB_LDFLAGS="-L$DEPS/lib -L$OPENSSL_PREFIX/lib"
EXTRA_PKG_CONFIG=""

if [ "$OS" = "Linux" ]; then
    RB_CFLAGS="$RB_CFLAGS -fPIC"
    for mdir in "$OPENSSL_PREFIX/lib/$ARCH-linux-gnu" \
                "$OPENSSL_PREFIX/lib/aarch64-linux-gnu" \
                "$OPENSSL_PREFIX/lib/x86_64-linux-gnu" \
                "$OPENSSL_PREFIX/lib64" \
                "$OPENSSL_PREFIX/lib"; do
        if [ -d "$mdir" ]; then
            RB_LDFLAGS="$RB_LDFLAGS -L$mdir"
            if [ -d "$mdir/pkgconfig" ]; then
                EXTRA_PKG_CONFIG=":$mdir/pkgconfig"
            fi
            break
        fi
    done
fi

# Build self-contained from source tarball without probing host ruby
# (prevents configure from probing incomplete host rubies and spewing missing-baseruby errors).
BASERUBY_OPT="--without-baseruby"

# build_ruby SRCDIR "EXTRA_FLAGS" "RUNNER" DESTDIR -- one Ruby tree.  RUNNER is
# e.g. "arch -x86_64" to build the Intel half natively under Rosetta (Ruby's
# cross-compilation needs a separate host Ruby; a native build does not).
build_ruby() {
    local srcdir="$1" xflags="$2" runner="$3" destdir="$4" cc="${5:-}" cxx="${6:-}"
(
    cd "$srcdir"
    note "configuring Ruby $RUBY_VERSION${runner:+ ($runner)}..."

    # If OPENSSL_PREFIX is outside PKG_CONFIG_SYSROOT_DIR, do not let pkg-config
    # prepend PKG_CONFIG_SYSROOT_DIR to OpenSSL paths.
    CLEAN_SYSROOT="${PKG_CONFIG_SYSROOT_DIR:-}"
    if [ -n "$CLEAN_SYSROOT" ]; then
        case "$OPENSSL_PREFIX" in
            "$CLEAN_SYSROOT"*) ;;
            *) CLEAN_SYSROOT="" ;;
        esac
    fi

    PKG_CONFIG_SYSROOT_DIR="$CLEAN_SYSROOT" \
    PKG_CONFIG_PATH="$DEPS/lib/pkgconfig:$OPENSSL_PREFIX/lib/pkgconfig${EXTRA_PKG_CONFIG}${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
    CFLAGS="$RB_CFLAGS $xflags" \
    LDFLAGS="$RB_LDFLAGS $xflags" \
    optflags="-O2" \
    env ${cc:+"CC=$cc"} ${cxx:+"CXX=$cxx"} \
    $runner ./configure \
        --prefix="$INSTALL" \
        --enable-shared --disable-static \
        --enable-load-relative \
        --disable-install-doc \
        --without-gmp \
        --with-openssl-dir="$OPENSSL_PREFIX" \
        --with-libyaml-dir="$DEPS" \
        $BASERUBY_OPT >/dev/null || exit 1

    note "compiling Ruby $RUBY_VERSION (using $JOBS parallel jobs)..."
    # (set -e does not apply inside a "( ... ) || die" subshell: each step
    #  must stop the build itself, or a failed configure runs on into make)
    $runner make -j"$JOBS" >/dev/null || exit 1

    note "installing Ruby $RUBY_VERSION to staging..."
    $runner make install ${destdir:+DESTDIR="$destdir"} >/dev/null || exit 1
) || die "Ruby build failed${runner:+ ($runner)}"
}

# merge_trees OUT TREE... -- union of the trees: a file in only one is copied,
# identical files are kept once, and Mach-O files present in several are lipo'd.
# Ruby keeps its architecture-specific files in per-arch directories
# (lib/ruby/<ver>/<arch>-darwin*, include/ruby-<ver>/<arch>-darwin*), so those
# simply sit side by side; what is left to lipo is mainly libruby itself.  Any
# other file that differs between architectures is listed, never silently
# picked.
merge_trees() {
    local out="$1"; shift
    local first="$1" t rel
    rm -rf "$out"; mkdir -p "$(dirname "$out")"; cp -R "$first" "$out"
    for t in "$@"; do
        [ "$t" = "$first" ] && continue
        while IFS= read -r rel; do
            if [ ! -e "$out/$rel" ] && [ ! -L "$out/$rel" ]; then
                mkdir -p "$(dirname "$out/$rel")"; cp -a "$t/$rel" "$out/$rel"
            elif [ -L "$t/$rel" ] || cmp -s "$t/$rel" "$out/$rel"; then
                :
            elif file -b "$t/$rel" | grep -q Mach-O; then
                lipo -create "$out/$rel" "$t/$rel" -output "$out/$rel.lipo" && mv "$out/$rel.lipo" "$out/$rel"
            else
                note "  differs between architectures, kept $(basename "$first")'s: $rel"
            fi
        done < <(cd "$t" && find . \( -type f -o -type l \) | sed 's|^\./||')
    done
}

if [ "$OS" = "Darwin" ]; then
    stages=()
    for a in $MACOS_ARCHS; do
        extract "$DEST/$PKG" "$SRC/ruby-$a"
        stage="$DEST/stage-$a"; rm -rf "$stage"
        # The architecture goes into CC/CXX themselves, not just CFLAGS: parts of
        # Ruby's build (the encoding modules, for one) compile with the bare
        # compiler, and "arch -x86_64" does not reach every process it spawns --
        # a first attempt produced arm64 enc/*.o inside the Intel build.
        MACOSX_DEPLOYMENT_TARGET="$MACOS_MIN" \
            build_ruby "$SRC/ruby-$a" "-arch $a -mmacosx-version-min=$MACOS_MIN" "arch -$a" "$stage" \
                       "clang -arch $a" "clang++ -arch $a"
        stages+=("$stage")   # load-relative: DESTDIR holds the tree itself, no prefix below it
    done
    note "merging the architectures into one Ruby"
    merge_trees "$INSTALL" "${stages[@]}"
    find "$INSTALL" -name '*.dSYM' -type d -prune -exec rm -rf {} +   # debug symbols, not runtime
    # Every architecture in libruby, and every binary in an architecture's own
    # directory really built for it (a mixed build once put arm64 objects into
    # the Intel half).
    rb_lib="$(find "$INSTALL/lib" -maxdepth 1 -type f -name 'libruby.*.dylib' | head -1)"
    for a in $MACOS_ARCHS; do
        lipo -archs "$rb_lib" | tr ' ' '\n' | grep -qx "$a" || die "merged libruby lacks $a"
        while IFS= read -r -d '' f; do
            file -b "$f" | grep -q Mach-O || continue
            lipo -archs "$f" | tr ' ' '\n' | grep -qx "$a" || die "${f#"$INSTALL"/} is in the $a tree but is $(lipo -archs "$f")"
        done < <(find "$INSTALL" -type f -path "*/$a-darwin*/*" \( -name '*.bundle' -o -name '*.dylib' \) -print0)
    done
    rm -rf "$DEST"/stage-* "$SRC"/ruby-*
else
    extract "$DEST/$PKG" "$SRC/ruby"
    build_ruby "$SRC/ruby" "" "" ""
fi

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

# Record the OpenSSL this Ruby links -- where, and which major version (the ABI)
# -- for .build-mac.sh's staleness check.
ossl_ver="$(cat "$OPENSSL_PREFIX/.version" 2>/dev/null || echo unknown)"
printf '%s %s\n' "$OPENSSL_PREFIX" "${ossl_ver%%.*}" > "$INSTALL/.openssl"
# ...and, on macOS, the architectures and minimum macOS it was built for.
[ "$OS" = "Darwin" ] && printf '%s\n' "$MACOS_ARCHS | macOS $MACOS_MIN" > "$INSTALL/.build"

# ── 5. report ─────────────────────────────────────────────────────────────────
note "staged relocatable Ruby $RUBY_VERSION:"
if [ "$OS" = "Darwin" ]; then
    note "  libruby:  $DYLIB  ($(lipo -archs "$DYLIB" 2>/dev/null || echo '?'))"
    note "  id:       $(otool -D "$DYLIB" | tail -1)"
    note "  stdlib:   $INSTALL/lib/ruby/$SHORT.0/"
    note "  libruby non-system deps (should be none — psych/libyaml are static):"
    otool -L "$DYLIB" | awk '/^\t/ {print $1}' \
        | grep -vE '^(/usr/lib/|/System/|@)' | sed 's/^/    LEAK: /' \
        || note "    (none — only system libs + @rpath)"
else
    note "  libruby:  $(find "$INSTALL/lib" -maxdepth 1 -name 'libruby.so*' | head -1) ($ARCH)"
    note "  stdlib:   $INSTALL/lib/ruby/$SHORT.0/"
fi
note "build with:  packaging/build-app.sh   (or a normal CMake build; auto-detects this)"
