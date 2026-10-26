#!/usr/bin/env bash
#
# build-app.sh — build a complete, self-contained, signed MUSHclient.app on macOS.
#
# One command does the whole pipeline:
#   1. configure + build the Release `mushgui` target
#   2. relocate every non-system dylib / framework it links into the bundle
#      (Contents/Frameworks) and rewrite the load commands → the .app no longer
#      depends on Homebrew / Xcode being installed
#   3. (optional) bundle Quow's Discworld plugins inside the app
#   4. code-sign inside-out with the hardened runtime + entitlements
#   5. (optional) notarize + staple
#   6. produce a DMG (and a .zip)
#
# You need an Apple "Developer ID Application" certificate in your keychain to
# produce a distributable build; without one the script ad-hoc signs so the app
# still runs locally (it just can't be notarized or opened on other Macs).
#
# Examples
#   # local self-contained build, ad-hoc signed, DMG in dist/
#   packaging/build-app.sh
#
#   # signed + notarized release (identity + notary profile from your keychain)
#   packaging/build-app.sh \
#       --sign-identity "Developer ID Application: Jane Dev (TEAMID)" \
#       --notarize-profile mushclient-notary
#
#   # Discworld edition with Quow's plugins pre-bundled
#   packaging/fetch-discworld-plugins.sh          # stage them first (see script)
#   packaging/build-app.sh --with-discworld --sign-identity "Developer ID Application: ..."
#
set -euo pipefail

# ── locate the repo ─────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── defaults (overridable by flags / env) ───────────────────────────────────
APP_NAME="MUSHclient"
BUILD_TYPE="Release"
BUILD_DIR="$REPO/build-release"
OUTPUT_DIR="$REPO/dist"
ENTITLEMENTS="$SCRIPT_DIR/entitlements.plist"
SIGN_ID="${CODESIGN_IDENTITY:--}"                  # "-" == ad-hoc
NOTARIZE_PROFILE="${NOTARYTOOL_PROFILE:-}"         # keychain profile name
DISCWORLD_DIR="${DISCWORLD_PLUGINS_DIR:-$SCRIPT_DIR/discworld-plugins}"
WITH_DISCWORLD=0
MAKE_DMG=1
DO_RELOCATE=1
PRUNE_PYTHON=1
DO_CLEAN=0
MIN_MACOS="11.0"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    cat <<EOF

Options
  -t, --build-type TYPE     CMake build type (default: Release)
  -s, --sign-identity ID    codesign identity, or "-" for ad-hoc
                            (default: \$CODESIGN_IDENTITY, else ad-hoc)
  -n, --notarize-profile P  notarytool keychain profile; if set, notarize+staple
                            (default: \$NOTARYTOOL_PROFILE)
  -d, --with-discworld      bundle Quow's plugins from the staging dir
      --discworld-dir DIR   staging dir (default: packaging/discworld-plugins)
  -o, --output DIR          where to put the .app / .dmg (default: dist/)
  -j, --jobs N              parallel build jobs (default: ncpu)
      --no-dmg              skip DMG creation (leave the .app in dist/)
      --no-relocate         skip dylib bundling (faster; not self-contained)
      --no-prune-python     keep tkinter/tests/idle in the bundled Python framework
      --clean               wipe the build dir first
  -h, --help                this help
EOF
}

# ── parse args ──────────────────────────────────────────────────────────────
while [ $# -gt 0 ]; do
    case "$1" in
        -t|--build-type)      BUILD_TYPE="$2"; shift 2;;
        -s|--sign-identity)   SIGN_ID="$2"; shift 2;;
        -n|--notarize-profile) NOTARIZE_PROFILE="$2"; shift 2;;
        -d|--with-discworld)  WITH_DISCWORLD=1; shift;;
        --discworld-dir)      DISCWORLD_DIR="$2"; shift 2;;
        -o|--output)          OUTPUT_DIR="$2"; shift 2;;
        -j|--jobs)            JOBS="$2"; shift 2;;
        --no-dmg)             MAKE_DMG=0; shift;;
        --no-relocate)        DO_RELOCATE=0; shift;;
        --no-prune-python)    PRUNE_PYTHON=0; shift;;
        --clean)              DO_CLEAN=1; shift;;
        -h|--help)            usage; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

[ "$(uname -s)" = "Darwin" ] || die "this script builds a macOS .app; run it on macOS"
command -v cmake >/dev/null || die "cmake not found"

APP="$BUILD_DIR/$APP_NAME.app"
EXE="$APP/Contents/MacOS/$APP_NAME"
FRAMEWORKS="$APP/Contents/Frameworks"

# ════════════════════════════════════════════════════════════════════════════
# 1. build
# ════════════════════════════════════════════════════════════════════════════
[ "$DO_CLEAN" = 1 ] && { note "cleaning $BUILD_DIR"; rm -rf "$BUILD_DIR"; }
note "configuring ($BUILD_TYPE) → $BUILD_DIR"
# EXTRA_CMAKE_ARGS lets you steer the configure, e.g. point at a redistributable
# python.org framework:  EXTRA_CMAKE_ARGS="-DPython3_ROOT_DIR=/Library/Frameworks/Python.framework/Versions/3.12 -DPython3_FIND_FRAMEWORK=ONLY"
#
# Pin OpenSSL to Homebrew's openssl@3 unless EXTRA_CMAKE_ARGS names one.  The
# bundled Python (python.org) and Ruby (fetch-ruby.sh, which also prefers
# openssl@3) are built against OpenSSL 3, and step 3c makes all three share ONE
# libssl/libcrypto, so the app must link 3.x too.  Left to itself, CMake takes
# whichever OpenSSL Homebrew has linked -- openssl@4 on a Mac that has it.  -U
# drops a previous run's cached choice, which would otherwise survive the pin.
OPENSSL_PIN=()
if [[ "${EXTRA_CMAKE_ARGS:-}" != *OPENSSL* ]]; then
    ossl3="$(brew --prefix openssl@3 2>/dev/null || true)"
    if [ -n "$ossl3" ] && [ -d "$ossl3/include/openssl" ]; then
        OPENSSL_PIN=(-U 'OPENSSL_*' -DOPENSSL_ROOT_DIR="$ossl3")
        note "OpenSSL: pinned to $ossl3"
    fi
fi
# shellcheck disable=SC2086
cmake -S "$REPO" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" ${OPENSSL_PIN[@]+"${OPENSSL_PIN[@]}"} ${EXTRA_CMAKE_ARGS:-} >/dev/null
note "building mushgui (-j$JOBS)"
# Force a pristine relink so the executable carries its ORIGINAL (Homebrew/Xcode)
# rpaths + install names every run.  Without this, re-running on an already-
# relocated binary (incremental build doesn't relink) leaves dyld unable to find
# the still-un-bundled originals, and relocation resolves nothing.
rm -f "$EXE"
cmake --build "$BUILD_DIR" --target mushgui -j"$JOBS"
[ -d "$APP" ] || die "build did not produce $APP"

# ════════════════════════════════════════════════════════════════════════════
# 2. bundle Discworld plugins (optional) — done before signing so they're sealed
# ════════════════════════════════════════════════════════════════════════════
SEED="$APP/Contents/Resources/userdata-seed"
# Start from nothing.  The build dir (and so $APP) is reused between runs and
# only the executable is relinked, so without this a plain build that follows a
# --with-discworld one would still ship the Discworld world, Quow's plugins, the
# maps AND the auto-open marker -- i.e. silently be the Discworld edition.
rm -rf "$SEED" "$APP/Contents/Resources/default-world"
mkdir -p "$SEED/worlds/plugins"
if [ -f "$REPO/packaging/bundled-plugins.txt" ]; then
    while IFS= read -r p || [ -n "$p" ]; do
        p="${p%%#*}"
        p="$(echo "$p" | tr -d " 	")"
        [ -n "$p" ] && [ -f "$REPO/plugins/$p" ] && cp "$REPO/plugins/$p" "$SEED/worlds/plugins/"
    done < "$REPO/packaging/bundled-plugins.txt"
fi

if [ "$WITH_DISCWORLD" = 1 ]; then
    [ -d "$DISCWORLD_DIR" ] || die "no Discworld staging dir: $DISCWORLD_DIR
  Stage Quow's plugins + maps first:  packaging/fetch-discworld-plugins.sh"

    # The app seeds Contents/Resources/userdata-seed/** into the user's data dir
    # on first launch (see SeedBundledUserData).  The staging dir mirrors that
    # tree: worlds/plugins/Quow*.xml (the plugins) + quow_plugins/maps/ (the room
    # database, map images and sounds the Cow Bar mapper loads at runtime).

    if [ -d "$DISCWORLD_DIR/worlds" ] || [ -d "$DISCWORLD_DIR/quow_plugins" ]; then
        # current mirror layout (what fetch-discworld-plugins.sh produces)
        for sub in worlds quow_plugins; do
            [ -d "$DISCWORLD_DIR/$sub" ] && cp -R "$DISCWORLD_DIR/$sub" "$SEED/"
        done
    else
        # legacy flat layout: Quow*.xml at the top level (+ an optional maps/)
        mkdir -p "$SEED/worlds/plugins"
        cp "$DISCWORLD_DIR"/*.xml "$SEED/worlds/plugins/" 2>/dev/null || true
        [ -d "$DISCWORLD_DIR/maps" ] && { mkdir -p "$SEED/quow_plugins"; cp -R "$DISCWORLD_DIR/maps" "$SEED/quow_plugins/"; }
    fi

    # never ship the per-user database (the plugin creates it) or macOS junk
    find "$SEED" \( -name '_quowmap_userdata.db' -o -name '.DS_Store' \) -delete 2>/dev/null || true

    nseed=$(find "$SEED" -type f | wc -l | tr -d ' ')
    [ "$nseed" -gt 0 ] || die "nothing staged in $DISCWORLD_DIR
  Stage Quow's plugins + maps first:  packaging/fetch-discworld-plugins.sh"
    nmaps=$(find "$SEED/quow_plugins/maps" -type f 2>/dev/null | wc -l | tr -d ' ')
    note "bundling Discworld edition: $nseed file(s), $(du -sh "$SEED" | cut -f1) (maps: $nmaps file(s))"
    [ "$nmaps" -gt 0 ] || note "  NOTE: no map data staged — the Cow Bar mapper will not run (see fetch-discworld-plugins.sh)"

    # Mark the bundled world to auto-open on launch.  The app opens it when no
    # other world is already open (hook_open_default_world, called from wx_app).
    # The marker lives OUTSIDE userdata-seed/ so it is never copied into the
    # user's writable data dir; a normal (non-Discworld) build has no marker and
    # the hook is a no-op.
    if [ -f "$SEED/worlds/Discworld.mcl" ]; then
        printf 'Discworld.mcl' > "$APP/Contents/Resources/default-world"
        note "  default world: Discworld.mcl (opens on launch)"
    else
        note "  NOTE: no Discworld.mcl staged — the edition will not auto-open a world"
    fi
fi

# ════════════════════════════════════════════════════════════════════════════
# 3. relocate dependencies → self-contained bundle
# ════════════════════════════════════════════════════════════════════════════
is_system_lib() { case "$1" in /usr/lib/*|/System/*) return 0;; *) return 1;; esac; }

# rpaths of a Mach-O binary (one per line).  In `otool -l`, each LC_RPATH is
# three lines (cmd / cmdsize / path …); capture the path that follows the cmd.
rpaths_of() { otool -l "$1" | awk '/^ *cmd LC_RPATH$/{f=1} f&&/^ *path /{print $2; f=0}'; }

add_rpath_once() {
    local bin="$1" rp="$2"
    rpaths_of "$bin" | grep -qxF "$rp" || install_name_tool -add_rpath "$rp" "$bin" 2>/dev/null || true
}

# follow a symlink chain to the real file (bash 3.2; no `realpath` dependency).
# Lets aliases of one library (e.g. libwx_…core-3.3.dylib -> …-3.3.2.dylib ->
# …-3.3.2.0.0.dylib) collapse to a single bundled copy under the real name,
# instead of copying each alias separately (two copies => two runtimes load =>
# duplicate ObjC classes / "mysterious crashes").
resolve_symlink() {
    local p="$1" t
    while [ -L "$p" ]; do
        t="$(readlink "$p")"
        case "$t" in /*) p="$t";; *) p="$(dirname "$p")/$t";; esac
    done
    echo "$p"
}

# resolve a dependency string (absolute / @rpath / @loader_path / @executable_path)
# to an on-disk path, using the loading binary's rpaths for @rpath
resolve_dep() {
    local dep="$1" bin="$2" bindir; bindir="$(dirname "$bin")"
    case "$dep" in
        /*) [ -f "$dep" ] && echo "$dep"; return;;
        @loader_path/*)     echo "$bindir/${dep#@loader_path/}"; return;;
        @executable_path/*) echo "$(dirname "$EXE")/${dep#@executable_path/}"; return;;
        @rpath/*)
            local rest="${dep#@rpath/}" rp exp
            while IFS= read -r rp; do
                exp="${rp/@loader_path/$bindir}"; exp="${exp/@executable_path/$(dirname "$EXE")}"
                [ -e "$exp/$rest" ] && { echo "$exp/$rest"; return; }
            done < <(rpaths_of "$bin")
            # last resort: the standard Homebrew/local link dirs.  All our deps
            # are Homebrew-provided, so this resolves even if $bin's own rpaths
            # were already rewritten (e.g. on a re-run) or never pointed here.
            for exp in /opt/homebrew/lib /usr/local/lib; do
                [ -e "$exp/$rest" ] && { echo "$exp/$rest"; return; }
            done
            ;;
    esac
}

relocate_framework() {  # $1=loading bin, $2=dep string pointing inside a .framework
    local bin="$1" dep="$2"
    local fwname inner resolved fwroot
    fwname="$(basename "${dep%%.framework/*}").framework"   # e.g. Python3.framework
    inner="${dep#*.framework/}"                             # e.g. Versions/3.9/Python3
    if [ ! -d "$FRAMEWORKS/$fwname" ]; then
        resolved="$(resolve_dep "$dep" "$bin")"
        [ -n "$resolved" ] && [ -e "$resolved" ] || { warn "cannot resolve framework $dep"; return; }
        fwroot="${resolved%%.framework/*}.framework"
        case "$fwroot" in /Applications/Xcode*|*/CommandLineTools/*)
            warn "bundling $fwname from Xcode/CLT: $fwroot
         This is Apple's framework — fine for local/testing, but NOT cleanly
         redistributable. For public release, rebuild against a python.org
         framework (see packaging/README.md) and re-run this script."
        ;; esac
        note "  bundling framework $fwname"
        cp -R "$fwroot" "$FRAMEWORKS/$fwname"               # -R keeps internal symlinks
        chmod -R u+w "$FRAMEWORKS/$fwname" 2>/dev/null || true
    fi
    install_name_tool -change "$dep" "@rpath/$fwname/$inner" "$bin" 2>/dev/null || true
}

WORKLIST=()   # plain indexed array (bash 3.2 has no associative arrays);
              # "already bundled?" is answered by testing for the file on disk.

relocate_one() {  # scan one binary, bundle + rewrite its non-system deps
    local bin="$1" dep base resolved selfbase; selfbase="$(basename "$bin")"
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        is_system_lib "$dep" && continue
        [ "$(basename "$dep")" = "$selfbase" ] && continue   # skip a dylib's own id line
        case "$dep" in
            *.framework/*) relocate_framework "$bin" "$dep";;
            *)
                resolved="$(resolve_dep "$dep" "$bin")"
                if [ -n "$resolved" ] && [ -f "$resolved" ]; then
                    # Bundle under the REAL (symlink-resolved) name, so multiple
                    # aliases of the same library collapse to one copy and one
                    # loaded image.
                    real="$(resolve_symlink "$resolved")"
                    base="$(basename "$real")"
                    # A dependency that already resolves to a path inside the
                    # .app (e.g. libruby staged into Contents/lib by CMake) is
                    # already bundled -- copying it into Frameworks too would
                    # ship and sign a second copy of the same image.
                    case "$real" in
                        "$APP"/*) continue;;
                    esac
                    if [ ! -f "$FRAMEWORKS/$base" ]; then          # not yet bundled
                        cp "$real" "$FRAMEWORKS/$base"
                        chmod u+w "$FRAMEWORKS/$base"
                        install_name_tool -id "@rpath/$base" "$FRAMEWORKS/$base" 2>/dev/null || true
                        add_rpath_once "$FRAMEWORKS/$base" "@loader_path"
                        # Keep the source dir on the rpath *temporarily* so this lib's
                        # own @rpath siblings (e.g. libwebp → libsharpyuv, in the same
                        # Homebrew dir) resolve while we recurse.  Stripped afterwards.
                        add_rpath_once "$FRAMEWORKS/$base" "$(dirname "$real")"
                        WORKLIST+=("$FRAMEWORKS/$base")
                    fi
                    install_name_tool -change "$dep" "@rpath/$base" "$bin" 2>/dev/null || true
                else
                    warn "could not resolve $dep (needed by $selfbase)"
                fi
                ;;
        esac
    done < <(otool -L "$bin" | tail -n +2 | awk '{print $1}')
}

if [ "$DO_RELOCATE" = 1 ]; then
    note "relocating dependencies into $APP_NAME.app/Contents/Frameworks"
    rm -rf "$FRAMEWORKS"          # always relocate from scratch (no stale state on re-runs)
    mkdir -p "$FRAMEWORKS"
    WORKLIST=("$EXE")
    widx=0
    while [ $widx -lt ${#WORKLIST[@]} ]; do      # index walk: relocate_one appends (BFS)
        bin="${WORKLIST[$widx]}"; widx=$((widx + 1))
        relocate_one "$bin"
    done
    # the main executable finds the bundled libs/frameworks via this rpath
    add_rpath_once "$EXE" "@executable_path/../Frameworks"
    # drop every ABSOLUTE rpath (Homebrew/Xcode dirs, the dev-time vendored-Python
    # dir, …) from the exe AND every bundled dylib, so nothing leaks the build
    # machine; @loader_path / @executable_path rpaths keep everything resolvable
    # inside Contents/Frameworks.
    rpath_targets=("$EXE")
    if [ -d "$FRAMEWORKS" ]; then
        while IFS= read -r f; do [ -n "$f" ] && rpath_targets+=("$f"); done < <(find "$FRAMEWORKS" -type f \( -name "*.dylib" -o -name "*.so" -o -perm -0111 \) 2>/dev/null)
    fi
    if [ -d "$APP/Contents/lib" ]; then
        while IFS= read -r f; do [ -n "$f" ] && rpath_targets+=("$f"); done < <(find "$APP/Contents/lib" -type f \( -name "*.dylib" -o -name "*.so" -o -name "*.bundle" -o -perm -0111 \) 2>/dev/null)
    fi
    for b in "${rpath_targets[@]}"; do
        [ -f "$b" ] || continue
        while IFS= read -r rp; do
            case "$rp" in /*)
                install_name_tool -delete_rpath "$rp" "$b" 2>/dev/null || true;; esac
        done < <(rpaths_of "$b")
    done

    # ── arch consistency ───────────────────────────────────────────────────
    # Thin every bundled Mach-O to the architecture(s) the executable actually
    # has, so an arm64-only app never ships a fat (universal2) Python framework
    # or dylib.  If the exe is itself multi-arch, leave deps fat (a true
    # universal app — the deps are assumed to match).
    APP_ARCHS="$(lipo -archs "$EXE" 2>/dev/null)"
    if [ "$(printf '%s' "$APP_ARCHS" | wc -w | tr -d ' ')" = 1 ]; then
        note "thinning bundled Mach-O to '$APP_ARCHS' (matching the executable)"
        while IFS= read -r f; do
            [ -L "$f" ] && continue
            file "$f" 2>/dev/null | grep -q "Mach-O" || continue
            if file "$f" | grep -q "Mach-O universal"; then          # fat → thin to target
                lipo "$f" -thin "$APP_ARCHS" -output "$f" 2>/dev/null || true
            fi
            # anything still not the target arch is a stray (e.g. python's x86_64
            # -intel64 Rosetta launcher); drop unused launchers, flag anything else.
            if [ "$(lipo -archs "$f" 2>/dev/null)" != "$APP_ARCHS" ]; then
                case "$f" in
                    *-intel64|*/bin/*) rm -f "$f" ;;
                    *) warn "non-$APP_ARCHS Mach-O kept: ${f#$APP/} ($(lipo -archs "$f" 2>/dev/null))" ;;
                esac
            fi
        done < <(find "$FRAMEWORKS" -type f \( -name '*.dylib' -o -name '*.so' -o -perm +111 \))
    else
        note "executable is multi-arch ($APP_ARCHS) — leaving bundled Mach-O fat (universal app)"
    fi
    note "  bundled $(ls -1 "$FRAMEWORKS" | wc -l | tr -d ' ') item(s); residual non-system deps:"
    otool -L "$EXE" | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/|@)' | sed 's/^/    LEAK: /' || true
fi

# ════════════════════════════════════════════════════════════════════════════
# 3b. prune the bundled Python framework (size)
# ════════════════════════════════════════════════════════════════════════════
# Drop the parts an embedded MUD-client interpreter never uses: Tcl/Tk (the
# tkinter GUI — large, and it carries dangling symlinks that trip xattr/codesign),
# the test suite, IDLE, and cached bytecode.
pyfw="$FRAMEWORKS/Python.framework"
if [ "$PRUNE_PYTHON" = 1 ] && [ -d "$pyfw" ]; then
    note "pruning bundled Python (tkinter / tests / idle / bytecode)"
    before="$(du -sm "$pyfw" 2>/dev/null | awk '{print $1}')"
    pvd="$(echo "$pyfw"/Versions/3.*)"
    rm -rf "$pvd"/Frameworks/Tcl.framework "$pvd"/Frameworks/Tk.framework
    std="$(echo "$pvd"/lib/python3.*)"
    rm -rf "$std/test" "$std/idlelib" "$std/turtledemo" "$std/tkinter" \
           "$std/ensurepip" "$std/lib2to3" "$std/pydoc_data"
    rm -f  "$std"/lib-dynload/_tkinter*.so
    rm -rf "$pvd/bin"                  # command-line launchers — the embedded app never execs them
    rm -rf "$pvd/Resources/Python.app" # GUI launcher stub (a nested Mach-O bundle the embed never uses)
    rm -rf "$pvd/share"                # man pages / docs
    find "$pyfw" -type d -name __pycache__ -prune -exec rm -rf {} + 2>/dev/null || true
    note "  Python framework: ${before}MB -> $(du -sm "$pyfw" 2>/dev/null | awk '{print $1}')MB"
fi

# ════════════════════════════════════════════════════════════════════════════
# 3c. unify OpenSSL — one libssl/libcrypto for the app, Python AND Ruby
# ════════════════════════════════════════════════════════════════════════════
# The relocation BFS already bundled the app's (Homebrew) libssl/libcrypto into
# Frameworks.  But Python ships its OWN pair inside its framework, and Ruby's
# openssl/digest extensions are dlopen'd at runtime (so the BFS never saw them,
# and their OpenSSL dep — also Homebrew — was left as an absolute path).  Point
# every consumer at the single Frameworks copy and delete Python's, so the .app
# carries exactly one OpenSSL.  All OpenSSL 3.x share the soname (libssl.3.dylib,
# compat 3.0.0); the bundled copy is the newest any consumer was built against
# (Homebrew — the app + Ruby), so Python's _ssl (built against an older 3.x)
# loads against it fine: OpenSSL 3.x only adds symbols within the series.
if [ "$DO_RELOCATE" = 1 ] && ls "$FRAMEWORKS"/libssl.*.dylib >/dev/null 2>&1; then
    note "unifying OpenSSL — single libssl/libcrypto for app + Python + Ruby"

    # rewrite any libssl/libcrypto dep of $1 to the shared copy.  Written to be
    # set -e safe (no bare `&&`/`continue` that returns non-zero on the common
    # path); always returns 0.
    repoint_openssl() {
        local bin="$1" dep base changed=0
        while IFS= read -r dep; do
            base="$(basename "$dep")"
            case "$base" in
                libssl.*.dylib|libcrypto.*.dylib)
                    if [ "$dep" != "@rpath/$base" ]; then
                        install_name_tool -change "$dep" "@rpath/$base" "$bin" 2>/dev/null || true
                        changed=1
                    fi
                    ;;
            esac
        done < <(otool -L "$bin" | tail -n +2 | awk '{print $1}')
        # @executable_path is the main exe wherever this Mach-O is loaded from, so
        # @rpath/libssl.3.dylib resolves to Contents/Frameworks for a dlopen'd
        # extension at any nesting depth.
        if [ "$changed" = 1 ]; then
            add_rpath_once "$bin" "@executable_path/../Frameworks"
        fi
        return 0
    }

    # Ruby crypto extensions (dlopen'd — invisible to the BFS)
    if [ -d "$APP/Contents/lib/ruby" ]; then
        while IFS= read -r b; do repoint_openssl "$b"; done \
            < <(find "$APP/Contents/lib/ruby" -name '*.bundle')
    fi
    # Python extensions, then delete Python's now-redundant internal OpenSSL pair
    if [ -d "$FRAMEWORKS/Python.framework" ]; then
        while IFS= read -r b; do repoint_openssl "$b"; done \
            < <(find "$FRAMEWORKS/Python.framework" \( -name '*.so' -o -name '*.dylib' \) \
                  ! -name 'libssl.*' ! -name 'libcrypto.*')
        find "$FRAMEWORKS/Python.framework" \
            \( -name 'libssl*.dylib' -o -name 'libcrypto*.dylib' \) -delete 2>/dev/null || true
    fi

    # Every OpenSSL a bundled binary now asks for must be the copy in Frameworks.
    # The repoint keeps each dependency's basename, so a consumer built against a
    # different major (libssl.3 while the app linked libssl.4) would be pointed at
    # a file that does not exist -- and Python's own copy has just been deleted.
    # That loads fine until the first `import ssl`, so stop the build here.
    missing=$(find "$APP" -type f \( -perm -u+x -o -name '*.dylib' -o -name '*.so' -o -name '*.bundle' \) \
                -exec otool -L {} \; 2>/dev/null \
              | awk '$1 ~ /^@rpath\/lib(ssl|crypto)\..*dylib$/ {sub("@rpath/", "", $1); print $1}' \
              | sort -u | while IFS= read -r lib; do [ -f "$FRAMEWORKS/$lib" ] || echo "$lib"; done)
    [ -z "$missing" ] || die "bundled code needs OpenSSL dylib(s) not in Frameworks: $missing
  The app, Python and Ruby must share one OpenSSL major (see the OpenSSL pin above)."

    n=$(find "$APP" \( -name 'libssl*.dylib' -o -name 'libcrypto*.dylib' \) | wc -l | tr -d ' ')
    note "  OpenSSL dylibs in bundle now: $n (expect 2 — libssl.3.dylib + libcrypto.3.dylib)"
    [ "$n" = 2 ] || warn "expected exactly 2 OpenSSL dylibs after unify, found $n"
fi

# ════════════════════════════════════════════════════════════════════════════
# 4. Info.plist touch-ups
# ════════════════════════════════════════════════════════════════════════════
PB=/usr/libexec/PlistBuddy
plist="$APP/Contents/Info.plist"
$PB -c "Set :LSMinimumSystemVersion $MIN_MACOS" "$plist" 2>/dev/null || \
    $PB -c "Add :LSMinimumSystemVersion string $MIN_MACOS" "$plist"
$PB -c "Set :NSHighResolutionCapable true" "$plist" 2>/dev/null || \
    $PB -c "Add :NSHighResolutionCapable bool true" "$plist"
if ! $PB -c "Print :CFBundleDocumentTypes" "$plist" >/dev/null 2>&1; then
    $PB -c "Add :CFBundleDocumentTypes array" "$plist"
    $PB -c "Add :CFBundleDocumentTypes:0 dict" "$plist"
    $PB -c "Add :CFBundleDocumentTypes:0:CFBundleTypeName string \"MUSHclient World\"" "$plist"
    $PB -c "Add :CFBundleDocumentTypes:0:CFBundleTypeRole string Editor" "$plist"
    $PB -c "Add :CFBundleDocumentTypes:0:CFBundleTypeExtensions array" "$plist"
    $PB -c "Add :CFBundleDocumentTypes:0:CFBundleTypeExtensions:0 string mcl" "$plist"
    $PB -c "Add :CFBundleDocumentTypes:0:CFBundleTypeExtensions:1 string mck" "$plist"
    $PB -c "Add :CFBundleDocumentTypes:0:CFBundleTypeExtensions:2 string mcp" "$plist"
    $PB -c "Add :CFBundleDocumentTypes:0:CFBundleTypeIconFile string MUSHclient.icns" "$plist"
    $PB -c "Add :CFBundleDocumentTypes:0:LSHandlerRank string Owner" "$plist"
fi

# ════════════════════════════════════════════════════════════════════════════
# 5. code sign (inside-out)
# ════════════════════════════════════════════════════════════════════════════
# A real Developer ID signature gets a secure timestamp (needs network); ad-hoc
# ("-") cannot be timestamped/notarized.  Avoid an empty-array flag so the
# script stays bash-3.2 clean under `set -u`.
sign_lib() {
    if [ "$SIGN_ID" = "-" ]; then codesign --force --options runtime --sign - "$1"
    else codesign --force --options runtime --timestamp --sign "$SIGN_ID" "$1"; fi
}
sign_app() {
    if [ "$SIGN_ID" = "-" ]; then
        codesign --force --options runtime --entitlements "$ENTITLEMENTS" --sign - "$1"
    else
        codesign --force --options runtime --timestamp --entitlements "$ENTITLEMENTS" --sign "$SIGN_ID" "$1"
    fi
}

# codesign refuses a bundle whose files carry extended attributes (resource
# forks / Finder info / com.apple.quarantine), which cp and downloaded assets
# routinely leave behind.  Strip them before signing.
note "stripping extended attributes (xattr -cr)"
xattr -cr "$APP" 2>/dev/null || true   # tolerate dangling symlinks (e.g. framework PrivateHeaders)

note "code signing with identity: $SIGN_ID"
[ "$SIGN_ID" = "-" ] && warn "ad-hoc signing — fine for THIS Mac, but cannot be notarized or run elsewhere"
if [ -d "$FRAMEWORKS" ]; then
    # nested Mach-O first (deepest last is fine; each is independent)
    while IFS= read -r -d '' f; do sign_lib "$f"; done \
        < <(find "$FRAMEWORKS" -type f \( -name '*.dylib' -o -name '*.so' \) -print0)
    for f in "$FRAMEWORKS"/*.framework; do [ -d "$f" ] && sign_lib "$f"; done
fi
# Nested Mach-O bundled outside Frameworks — the relocatable Ruby's libruby dylib
# and its .bundle C extensions live under Contents/lib (load-relative finds the
# stdlib there).  They must be signed too, or a Developer-ID / notarized build is
# rejected for unsigned nested code.
for d in "$APP/Contents/lib" "$APP/Contents/Resources"; do
    [ -d "$d" ] || continue
    while IFS= read -r -d '' f; do sign_lib "$f"; done \
        < <(find "$d" -type f \( -name '*.dylib' -o -name '*.so' -o -name '*.bundle' \) -print0)
done
sign_app "$APP"
codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | sed 's/^/    /' || warn "codesign --verify reported issues"

# ════════════════════════════════════════════════════════════════════════════
# 6. notarize (optional) + package
# ════════════════════════════════════════════════════════════════════════════
mkdir -p "$OUTPUT_DIR"
ZIP="$OUTPUT_DIR/$APP_NAME.zip"

if [ -n "$NOTARIZE_PROFILE" ]; then
    [ "$SIGN_ID" = "-" ] && die "cannot notarize an ad-hoc-signed app; pass --sign-identity"
    note "notarizing via keychain profile '$NOTARIZE_PROFILE' (this can take a few minutes)"
    ditto -c -k --keepParent "$APP" "$ZIP"
    xcrun notarytool submit "$ZIP" --keychain-profile "$NOTARIZE_PROFILE" --wait
    note "stapling ticket"
    xcrun stapler staple "$APP"
fi

note "packaging → $OUTPUT_DIR"
rm -rf "$OUTPUT_DIR/$APP_NAME.app"
cp -R "$APP" "$OUTPUT_DIR/$APP_NAME.app"
ditto -c -k --keepParent "$OUTPUT_DIR/$APP_NAME.app" "$ZIP"
note "  wrote $ZIP"

if [ "$MAKE_DMG" = 1 ]; then
    DMG="$OUTPUT_DIR/$APP_NAME.dmg"
    stage="$(mktemp -d)"
    cp -R "$APP" "$stage/$APP_NAME.app"
    ln -s /Applications "$stage/Applications"
    rm -f "$DMG"
    hdiutil create -volname "$APP_NAME" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$DMG" >/dev/null
    rm -rf "$stage"
    [ -n "$NOTARIZE_PROFILE" ] && xcrun stapler staple "$DMG" 2>/dev/null || true
    note "  wrote $DMG"
fi

note "done."
echo
echo "  app : $OUTPUT_DIR/$APP_NAME.app"
echo "  zip : $ZIP"
[ "$MAKE_DMG" = 1 ] && echo "  dmg : $OUTPUT_DIR/$APP_NAME.dmg"
[ "$SIGN_ID" = "-" ] && echo "  (ad-hoc signed — for a distributable build pass --sign-identity / --notarize-profile)"
