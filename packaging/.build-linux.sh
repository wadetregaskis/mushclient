#!/usr/bin/env bash
#
# build-linux.sh — build a complete, self-contained MUSHclient package on Linux.
#
# One command performs the entire pipeline:
#   1. configure + build the Release `mushgui` target
#   2. stage the portable application layout (executable, launcher, dialog resources,
#      dialog/toolbar art, fonts, spell checker, and Lua scripting modules)
#   3. (optional) bundle Quow's Discworld plugins inside the application
#   4. bundle non-system dynamic shared libraries into lib/ with relocatable RPATH
#   5. produce a standalone relocatable .tar.xz archive (compressed with xz -9 -e)
#   6. produce a standard Debian .deb package
#
# Examples:
#   # local self-contained build, tarball + deb in dist/
#   packaging/build-app.sh
#
#   # Discworld edition with Quow's plugins pre-bundled
#   packaging/fetch-discworld-plugins.sh
#   packaging/build-app.sh --with-discworld
#
#   # Clean build with custom output directory
#   packaging/build-app.sh --clean --output my-dist/
#
set -euo pipefail

# ── locate the repo ─────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── defaults (overridable by flags / env) ───────────────────────────────────
APP_NAME="MUSHclient"
DEFAULT_VERSION="$(sed -n 's/^[[:space:]]*#define[[:space:]]*VERSION_STRING[[:space:]]*"\([^"]*\)".*/\1/p' "$REPO/src/version.h" 2>/dev/null || echo "6.0.0")"
VERSION="${VERSION:-${DEFAULT_VERSION:-6.0.0}}"
BUILD_TYPE="Release"
BUILD_DIR="$REPO/build-linux-release"
OUTPUT_DIR="$REPO/dist"
DISCWORLD_DIR="${DISCWORLD_PLUGINS_DIR:-$SCRIPT_DIR/discworld-plugins}"
WITH_DISCWORLD=0
MAKE_TAR=1
MAKE_DEB=1
DO_RELOCATE=1
DO_CLEAN=0
JOBS="$(nproc 2>/dev/null || echo 4)"

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: packaging/build-app.sh [OPTIONS]

Options:
  -t, --build-type TYPE     CMake build type (default: Release)
  -o, --output DIR          where to put the packages (default: dist/)
  -j, --jobs N              parallel build jobs (default: nproc)
  -d, --with-discworld      bundle Quow's Discworld plugins from staging dir
      --discworld-dir DIR   staging dir (default: packaging/discworld-plugins)
      --no-tar              skip .tar.xz creation
      --no-deb              skip .deb creation
      --no-relocate         skip library bundling into lib/
      --clean               wipe the build dir first
  -h, --help                show this help
EOF
}

# ── parse args ──────────────────────────────────────────────────────────────
while [ $# -gt 0 ]; do
    case "$1" in
        -t|--build-type)      BUILD_TYPE="$2"; shift 2;;
        -o|--output)          OUTPUT_DIR="$2"; shift 2;;
        -j|--jobs)            JOBS="$2"; shift 2;;
        -d|--with-discworld)  WITH_DISCWORLD=1; shift;;
        --discworld-dir)      DISCWORLD_DIR="$2"; shift 2;;
        --no-tar)             MAKE_TAR=0; shift;;
        --no-deb)             MAKE_DEB=0; shift;;
        --no-relocate)        DO_RELOCATE=0; shift;;
        --clean)              DO_CLEAN=1; shift;;
        -h|--help)            usage; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

[ "$(uname -s)" = "Linux" ] || die "this script builds Linux packages; run it on Linux"

# Source environment script if present (e.g. non-root sysroot or development env)
if [ -f "/tmp/env.sh" ]; then
    set +u
    # shellcheck disable=SC1091
    . /tmp/env.sh
    set -u
fi

command -v cmake >/dev/null 2>&1 || die "cmake not found; please install cmake"
command -v tar >/dev/null 2>&1 || die "tar not found"

UNAME_M="$(uname -m)"
case "$UNAME_M" in
    x86_64)   DEB_ARCH="amd64"; ARCH_TAG="x86_64" ;;
    aarch64)  DEB_ARCH="arm64"; ARCH_TAG="arm64" ;;
    armv7l)   DEB_ARCH="armhf"; ARCH_TAG="armhf" ;;
    i686)     DEB_ARCH="i386";  ARCH_TAG="x86" ;;
    *)        DEB_ARCH="$UNAME_M"; ARCH_TAG="$UNAME_M" ;;
esac

# ════════════════════════════════════════════════════════════════════════════
# 1. configure + build
# ════════════════════════════════════════════════════════════════════════════
[ "$DO_CLEAN" = 1 ] && { note "cleaning $BUILD_DIR"; rm -rf "$BUILD_DIR"; }

note "configuring ($BUILD_TYPE) -> $BUILD_DIR"
cmake -S "$REPO" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_RPATH='$ORIGIN/lib:$ORIGIN' \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=TRUE \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,--disable-new-dtags" \
    ${EXTRA_CMAKE_ARGS:-}

note "building mushgui target (-j$JOBS)"
cmake --build "$BUILD_DIR" --target mushgui -j"$JOBS"

EXE="$BUILD_DIR/MUSHclient"
[ -f "$EXE" ] || die "build did not produce $EXE"

# ════════════════════════════════════════════════════════════════════════════
# 2. stage standalone application tree
# ════════════════════════════════════════════════════════════════════════════
STAGE_APP="$OUTPUT_DIR/mushclient"
note "staging standalone bundle -> $STAGE_APP"
rm -rf "$STAGE_APP"
mkdir -p "$STAGE_APP" "$STAGE_APP/lib"

cp "$EXE" "$STAGE_APP/MUSHclient"
if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "$STAGE_APP/MUSHclient" 2>/dev/null || true
fi

# Copy runtime resource files and directories alongside the binary
cp "$REPO/src/platform/mushclient_dialogs.rc" "$STAGE_APP/"
cp "$REPO/src/resource.h"                     "$STAGE_APP/"
cp "$REPO/res/tips.txt"                       "$STAGE_APP/"
cp "$REPO/res/names.txt"                      "$STAGE_APP/"
cp "$REPO/src/scripting/spellchecker.lua"     "$STAGE_APP/"

for dir in lua fonts spell; do
    if [ -d "$REPO/$dir" ]; then
        cp -R "$REPO/$dir" "$STAGE_APP/"
    fi
done

for dir in dialog_art toolbar_art; do
    if [ -d "$REPO/src/platform/$dir" ]; then
        cp -R "$REPO/src/platform/$dir" "$STAGE_APP/"
    elif [ -d "$REPO/$dir" ]; then
        cp -R "$REPO/$dir" "$STAGE_APP/"
    fi
done

if [ -d "$REPO/src/platform/dialog_text" ]; then
    cp -R "$REPO/src/platform/dialog_text" "$STAGE_APP/text"
fi

if [ -d "$REPO/worlds" ]; then
    cp -R "$REPO/worlds" "$STAGE_APP/"
fi

# Application icon
if [ -f "$REPO/src/platform/dialog_art/MUSHclient.png" ]; then
    cp "$REPO/src/platform/dialog_art/MUSHclient.png" "$STAGE_APP/mushclient.png"
fi

# Bundle scripting runtime libraries and stdlibs if present
# 1. Tcl script library
for tcldir in "$REPO/packaging/tcl/install/lib/tcl8.6" "$BUILD_DIR/tcl8.6" "/tmp/sysroot/usr/share/tcltk/tcl8.6" "/usr/share/tcltk/tcl8.6"; do
    if [ -d "$tcldir" ] && [ -f "$tcldir/init.tcl" ]; then
        cp -R "$tcldir" "$STAGE_APP/"
        note "  bundled Tcl 8.6 script library"
        break
    fi
done

# 2. Ruby standard library and shared library
for rbdir in "$REPO/packaging/ruby/install/lib/ruby" "$BUILD_DIR/lib/ruby"; do
    if [ -d "$rbdir" ]; then
        mkdir -p "$STAGE_APP/lib"
        cp -R "$rbdir" "$STAGE_APP/lib/"
        note "  bundled Ruby standard library"
        break
    fi
done
for rbso in "$REPO/packaging/ruby/install/lib"/libruby*.so*; do
    if [ -e "$rbso" ]; then
        cp -P "$rbso" "$STAGE_APP/lib/" 2>/dev/null || true
    fi
done

# 3. Python standard library and shared library
for pydir in "$REPO/packaging/python/install/lib"/python3*; do
    if [ -d "$pydir" ]; then
        mkdir -p "$STAGE_APP/lib"
        cp -R "$pydir" "$STAGE_APP/lib/"
        note "  bundled Python $(basename "$pydir") library"
        break
    fi
done
for pyso in "$REPO/packaging/python/install/lib"/libpython3*.so*; do
    if [ -e "$pyso" ]; then
        cp -P "$pyso" "$STAGE_APP/lib/" 2>/dev/null || true
    fi
done

# ════════════════════════════════════════════════════════════════════════════
# 3. bundle Discworld plugins (optional)
# ════════════════════════════════════════════════════════════════════════════
if [ "$WITH_DISCWORLD" = 1 ]; then
    [ -d "$DISCWORLD_DIR" ] || die "no Discworld staging dir: $DISCWORLD_DIR
  Stage Quow's plugins + maps first:  packaging/fetch-discworld-plugins.sh"

    SEED="$STAGE_APP/userdata-seed"
    rm -rf "$SEED"; mkdir -p "$SEED"

    if [ -d "$DISCWORLD_DIR/worlds" ] || [ -d "$DISCWORLD_DIR/quow_plugins" ]; then
        for sub in worlds quow_plugins; do
            [ -d "$DISCWORLD_DIR/$sub" ] && cp -R "$DISCWORLD_DIR/$sub" "$SEED/"
        done
    else
        mkdir -p "$SEED/worlds/plugins"
        cp "$DISCWORLD_DIR"/*.xml "$SEED/worlds/plugins/" 2>/dev/null || true
        [ -d "$DISCWORLD_DIR/maps" ] && { mkdir -p "$SEED/quow_plugins"; cp -R "$DISCWORLD_DIR/maps" "$SEED/quow_plugins/"; }
    fi

    find "$SEED" \( -name '_quowmap_userdata.db' -o -name '.DS_Store' \) -delete 2>/dev/null || true

    nseed=$(find "$SEED" -type f | wc -l | tr -d ' ')
    [ "$nseed" -gt 0 ] || die "nothing staged in $DISCWORLD_DIR"
    note "bundled Discworld edition: $nseed file(s)"

    if [ -f "$SEED/worlds/Discworld.mcl" ]; then
        printf 'Discworld.mcl' > "$STAGE_APP/default-world"
        note "  default world: Discworld.mcl"
    fi
fi

# ════════════════════════════════════════════════════════════════════════════
# 4. relocate shared libraries (relocatable bundle)
# ════════════════════════════════════════════════════════════════════════════
if [ "$DO_RELOCATE" = 1 ]; then
    note "relocating shared library dependencies into $STAGE_APP/lib"

    # Identify libraries to bundle: non-core-glibc dependencies from sysroot
    # or extra library directories so the bundle runs on other Linux installations.
    ldd_libs=$(ldd "$EXE" 2>/dev/null | awk '/=>/{print $3}' | grep -E '^/' || true)
    count=0
    for lib in $ldd_libs; do
        [ -f "$lib" ] || continue
        base="$(basename "$lib")"
        case "$base" in
            # Skip core glibc/system loader libraries that must match the host kernel/C runtime
            libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|ld-linux*.so.*)
                continue
                ;;
        esac

        real="$(readlink -f "$lib")"
        realbase="$(basename "$real")"

        if [ ! -f "$STAGE_APP/lib/$realbase" ]; then
            cp "$real" "$STAGE_APP/lib/$realbase"
            chmod u+w "$STAGE_APP/lib/$realbase" 2>/dev/null || true
            if command -v strip >/dev/null 2>&1; then
                strip --strip-unneeded "$STAGE_APP/lib/$realbase" 2>/dev/null || true
            fi
            count=$((count + 1))
        fi

        if [ "$base" != "$realbase" ] && [ ! -e "$STAGE_APP/lib/$base" ]; then
            ln -sf "$realbase" "$STAGE_APP/lib/$base"
        fi
    done
    note "  bundled $count shared libraries into lib/"
fi

# Ensure DT_RPATH tag is set (not DT_RUNPATH) so transitive library lookups in lib/ succeed
python3 -c "
import struct, os
def convert_runpath_to_rpath(filepath):
    try:
        with open(filepath, 'rb') as f:
            elf = bytearray(f.read())
        if len(elf) < 64 or elf[:4] != b'\x7fELF' or elf[4] != 2:
            return
        e_shoff = struct.unpack('<Q', elf[40:48])[0]
        e_shentsize = struct.unpack('<H', elf[58:60])[0]
        e_shnum = struct.unpack('<H', elf[60:62])[0]
        modified = False
        for i in range(e_shnum):
            offset = e_shoff + i * e_shentsize
            sh_type = struct.unpack('<I', elf[offset+4:offset+8])[0]
            if sh_type == 6:
                sh_offset = struct.unpack('<Q', elf[offset+24:offset+32])[0]
                sh_size = struct.unpack('<Q', elf[offset+32:offset+40])[0]
                sh_entsize = struct.unpack('<Q', elf[offset+56:offset+64])[0]
                for j in range(0, sh_size, sh_entsize):
                    tag = struct.unpack('<Q', elf[sh_offset+j:sh_offset+j+8])[0]
                    if tag == 29:
                        struct.pack_into('<Q', elf, sh_offset+j, 15)
                        modified = True
        if modified:
            with open(filepath, 'wb') as f:
                f.write(elf)
    except Exception:
        pass

for root, _, files in os.walk('$STAGE_APP'):
    for name in files:
        path = os.path.join(root, name)
        if not os.path.islink(path):
            convert_runpath_to_rpath(path)
" 2>/dev/null || true

# ════════════════════════════════════════════════════════════════════════════
# 5. create portable launcher and desktop file
# ════════════════════════════════════════════════════════════════════════════
cat <<'EOF' > "$STAGE_APP/mushclient.sh"
#!/bin/sh
HERE="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GDK_BACKEND="${GDK_BACKEND:-x11,wayland}"
exec "$HERE/MUSHclient" "$@"
EOF
chmod +x "$STAGE_APP/mushclient.sh"
ln -sf mushclient.sh "$STAGE_APP/mushclient"

cat <<'EOF' > "$STAGE_APP/mushclient.desktop"
[Desktop Entry]
Name=MUSHclient
Comment=MUD/MUSH/MOO client with scripting
Exec=sh -c '"$(dirname "%k")"/mushclient.sh' %F
Icon=mushclient
Terminal=false
Type=Application
Categories=Game;RolePlaying;Network;
Keywords=MUD;MUSH;MOO;Telnet;Client;
StartupNotify=true
EOF
chmod +x "$STAGE_APP/mushclient.desktop"
gio set "$STAGE_APP/mushclient.desktop" metadata::trusted true 2>/dev/null || true

# Also register in user applications menu if directory exists
if [ -d "$HOME/.local/share/applications" ] || mkdir -p "$HOME/.local/share/applications" 2>/dev/null; then
    cat <<EOF > "$HOME/.local/share/applications/mushclient.desktop"
[Desktop Entry]
Name=MUSHclient
Comment=MUD/MUSH/MOO client with scripting
Exec=$STAGE_APP/mushclient.sh %F
Icon=$STAGE_APP/mushclient.png
Terminal=false
Type=Application
Categories=Game;RolePlaying;Network;
StartupNotify=true
EOF
    chmod +x "$HOME/.local/share/applications/mushclient.desktop"
    update-desktop-database "$HOME/.local/share/applications/" 2>/dev/null || true
fi

# ════════════════════════════════════════════════════════════════════════════
# 6. package .tar.xz archive
# ════════════════════════════════════════════════════════════════════════════
mkdir -p "$OUTPUT_DIR"

if [ "$MAKE_TAR" = 1 ]; then
    command -v xz >/dev/null 2>&1 || die "xz command not found (install xz-utils)"
    TAR_NAME="mushclient-${VERSION}-linux-${ARCH_TAG}.tar.xz"
    TAR_PATH="$OUTPUT_DIR/$TAR_NAME"
    note "packaging tarball (xz -9 -e) -> $TAR_PATH"
    tar -C "$OUTPUT_DIR" -cf - mushclient | xz -9 -e > "$TAR_PATH"
    note "  wrote $TAR_PATH ($(du -sh "$TAR_PATH" | cut -f1))"
fi

# ════════════════════════════════════════════════════════════════════════════
# 7. package Debian .deb
# ════════════════════════════════════════════════════════════════════════════
if [ "$MAKE_DEB" = 1 ]; then
    if command -v dpkg-deb >/dev/null 2>&1; then
        DEB_NAME="mushclient_${VERSION}_${DEB_ARCH}.deb"
        DEB_PATH="$OUTPUT_DIR/$DEB_NAME"
        DEB_STAGE="$(mktemp -d)"

        note "packaging Debian package -> $DEB_PATH"

        # Directory layout for Debian package
        INSTALL_BIN="$DEB_STAGE/usr/bin"
        INSTALL_LIB="$DEB_STAGE/usr/lib/mushclient"
        INSTALL_SHARE="$DEB_STAGE/usr/share/mushclient"
        INSTALL_DESKTOP="$DEB_STAGE/usr/share/applications"
        INSTALL_ICON="$DEB_STAGE/usr/share/icons/hicolor/256x256/apps"
        INSTALL_PIXMAP="$DEB_STAGE/usr/share/pixmaps"
        DEBIAN_DIR="$DEB_STAGE/DEBIAN"

        mkdir -p "$INSTALL_BIN" "$INSTALL_LIB" "$INSTALL_SHARE" \
                 "$INSTALL_DESKTOP" "$INSTALL_ICON" "$INSTALL_PIXMAP" "$DEBIAN_DIR"

        # Binary and bundled libs
        cp -R "$STAGE_APP/MUSHclient" "$INSTALL_LIB/"
        if [ -d "$STAGE_APP/lib" ] && [ "$(ls -A "$STAGE_APP/lib")" ]; then
            cp -R "$STAGE_APP/lib" "$INSTALL_LIB/"
        fi

        # Wrapper script in /usr/bin/mushclient
        cat <<'EOF' > "$INSTALL_BIN/mushclient"
#!/bin/sh
if [ -d /usr/lib/mushclient/lib ]; then
    export LD_LIBRARY_PATH="/usr/lib/mushclient/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
exec /usr/lib/mushclient/MUSHclient "$@"
EOF
        chmod 755 "$INSTALL_BIN/mushclient"

        # Resources in /usr/share/mushclient
        for item in mushclient_dialogs.rc resource.h tips.txt names.txt spellchecker.lua; do
            [ -f "$STAGE_APP/$item" ] && cp "$STAGE_APP/$item" "$INSTALL_SHARE/"
        done
        for dir in lua dialog_art toolbar_art text fonts spell worlds userdata-seed tcl8.6; do
            [ -d "$STAGE_APP/$dir" ] && cp -R "$STAGE_APP/$dir" "$INSTALL_SHARE/"
        done
        [ -f "$STAGE_APP/default-world" ] && cp "$STAGE_APP/default-world" "$INSTALL_SHARE/"

        # Symlink resources alongside binary so both lookup paths succeed
        for item in "$INSTALL_SHARE"/*; do
            base="$(basename "$item")"
            [ -e "$INSTALL_LIB/$base" ] || ln -sf "/usr/share/mushclient/$base" "$INSTALL_LIB/$base"
        done

        # Desktop entry and icon
        cp "$STAGE_APP/mushclient.desktop" "$INSTALL_DESKTOP/"
        if [ -f "$STAGE_APP/mushclient.png" ]; then
            cp "$STAGE_APP/mushclient.png" "$INSTALL_ICON/mushclient.png"
            cp "$STAGE_APP/mushclient.png" "$INSTALL_PIXMAP/mushclient.png"
        fi

        # Control file
        cat <<EOF > "$DEBIAN_DIR/control"
Package: mushclient
Version: ${VERSION}
Section: games
Priority: optional
Architecture: ${DEB_ARCH}
Maintainer: MUSHclient Community <mushclient@example.com>
Description: MUSHclient MUD client with rich scripting
 MUSHclient is a fast, powerful, and extensible MUD/MUSH/MOO client.
 It features full ANSI colour, triggers, aliases, timers, speedwalking,
 miniwindows, sound playback, and scripting in Lua, Python, Perl, Tcl, and Ruby.
EOF

        dpkg-deb --build --root-owner-group "$DEB_STAGE" "$DEB_PATH" >/dev/null
        rm -rf "$DEB_STAGE"
        note "  wrote $DEB_PATH ($(du -sh "$DEB_PATH" | cut -f1))"
    else
        warn "dpkg-deb not found; skipping .deb creation"
    fi
fi

note "Linux packaging complete."
echo
echo "  bundle : $STAGE_APP"
[ "$MAKE_TAR" = 1 ] && echo "  tar.xz : $OUTPUT_DIR/mushclient-${VERSION}-linux-${ARCH_TAG}.tar.xz"
[ "$MAKE_DEB" = 1 ] && [ -f "$OUTPUT_DIR/mushclient_${VERSION}_${DEB_ARCH}.deb" ] && \
    echo "  deb    : $OUTPUT_DIR/mushclient_${VERSION}_${DEB_ARCH}.deb"
