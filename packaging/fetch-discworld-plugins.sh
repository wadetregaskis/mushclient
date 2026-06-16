#!/usr/bin/env bash
#
# fetch-discworld-plugins.sh — stage Quow's Discworld plugins AND their map data
# for bundling.
#
# Quow's plugins are NOT part of this repository (they're third-party and not
# ours to redistribute in git).  This script stages them into a git-ignored
# directory so `build-app.sh --with-discworld` can bundle them into the .app.
#
# The Cow Bar plugin (QuowMinimap.xml) needs more than the .xml: it loads a room
# database (_quowmap_database.db) plus map images (*.png) and sound effects
# (*.wav) from a "maps" folder at runtime.  Those must be staged too, or the
# mapper aborts on launch ("Unable to find/load the room database file").
#
# The staging dir mirrors the per-user data directory, so seeding it on first
# launch lands everything where the plugins look for it:
#
#   discworld-plugins/worlds/plugins/Quow*.xml          <- the plugins
#   discworld-plugins/quow_plugins/maps/_quowmap_database.db
#   discworld-plugins/quow_plugins/maps/*.png  *.wav    <- the map data
#
# The per-user database (_quowmap_userdata.db — combat stats, hotspots) is NOT
# staged: the plugin creates a fresh one per user.
#
# Three ways to populate it (in order of convenience):
#
#   1. From your existing MUSHclient install (auto-detected):
#        packaging/fetch-discworld-plugins.sh
#      copies Quow*.xml + the quow_plugins/maps/ folder from
#      ~/Library/Application Support/MUSHclient/
#
#   2. From a local folder you already downloaded/unzipped (Minimap.zip etc.):
#        packaging/fetch-discworld-plugins.sh --from ~/Downloads/quow
#      finds the *.xml plugins and the maps/ folder anywhere underneath it.
#
#   3. From a URL (a .zip / .tar.gz containing the above):
#        packaging/fetch-discworld-plugins.sh --url https://quow.example/pack.zip
#        # or:  QUOW_PLUGINS_URL=... packaging/fetch-discworld-plugins.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAGE="${DISCWORLD_PLUGINS_DIR:-$SCRIPT_DIR/discworld-plugins}"
URL="${QUOW_PLUGINS_URL:-}"
FROM=""
INSTALL_ROOT="$HOME/Library/Application Support/MUSHclient"

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --url)   URL="$2"; shift 2;;
        --from)  FROM="$2"; shift 2;;
        -h|--help) sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

PLUGINS_DST="$STAGE/worlds/plugins"
MAPS_DST="$STAGE/quow_plugins/maps"

# Drop any legacy flat-layout staging (Quow*.xml at the top level) so we don't
# leave stale copies beside the new worlds/plugins/ tree.
rm -f "$STAGE"/*.xml 2>/dev/null || true
mkdir -p "$PLUGINS_DST"

# stage_plugins SRC : copy Quow*.xml (else *.xml) found under SRC into the stage
stage_plugins() {
    local src="$1" found="" n=0 f
    found="$(find "$src" -maxdepth 4 -type f -name 'Quow*.xml' 2>/dev/null || true)"
    [ -n "$found" ] || found="$(find "$src" -maxdepth 4 -type f -name '*.xml' 2>/dev/null || true)"
    [ -n "$found" ] || die "no plugin *.xml files found under: $src"
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        cp "$f" "$PLUGINS_DST/" && n=$((n + 1))
    done <<EOF
$found
EOF
    note "staged $n plugin(s) into worlds/plugins/"
}

# stage_maps SRC : find a maps/ folder (the one holding _quowmap_database.db)
# under SRC and stage its contents, minus the per-user database.
stage_maps() {
    local src="$1" mapsdir="" d
    while IFS= read -r d; do
        [ -n "$d" ] || continue
        if [ -f "$d/_quowmap_database.db" ]; then mapsdir="$d"; break; fi
    done <<EOF
$(find "$src" -maxdepth 5 -type d -name maps 2>/dev/null || true)
EOF
    if [ -z "$mapsdir" ]; then
        warn "no maps/ folder containing _quowmap_database.db found under: $src"
        warn "the Cow Bar mapper will not run without it — download Quow's Minimap"
        warn "pack and re-run with --from <unzipped-folder>."
        return 0
    fi
    mkdir -p "$MAPS_DST"
    # copy everything except the per-user DB (the plugin creates that itself)
    find "$mapsdir" -type f \
        ! -name '_quowmap_userdata.db' ! -name '.DS_Store' \
        -exec cp {} "$MAPS_DST/" \;
    local nm
    nm="$(find "$MAPS_DST" -type f | wc -l | tr -d ' ')"
    note "staged $nm map file(s) into quow_plugins/maps/ ($(du -sh "$MAPS_DST" | cut -f1))"
}

if [ -n "$URL" ]; then
    note "downloading $URL"
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    out="$tmp/download"
    curl -fL --retry 3 -o "$out" "$URL" || die "download failed"
    ext="$tmp/extract"; mkdir -p "$ext"
    case "$URL" in
        *.zip)          unzip -qo "$out" -d "$ext" || die "unzip failed";;
        *.tar.gz|*.tgz) tar -xzf "$out" -C "$ext"  || die "untar failed";;
        *)  if unzip -l "$out" >/dev/null 2>&1; then unzip -qo "$out" -d "$ext"
            else die "don't know how to extract $URL (expected .zip / .tar.gz)"; fi;;
    esac
    stage_plugins "$ext"
    stage_maps    "$ext"

elif [ -n "$FROM" ]; then
    [ -d "$FROM" ] || die "not a directory: $FROM"
    stage_plugins "$FROM"
    stage_maps    "$FROM"

else
    # auto-detect from a local install
    if [ -d "$INSTALL_ROOT/worlds/plugins" ]; then
        stage_plugins "$INSTALL_ROOT/worlds/plugins"
        stage_maps    "$INSTALL_ROOT/quow_plugins"
    elif find "$STAGE" -name '*.xml' >/dev/null 2>&1; then
        note "already staged: $(find "$STAGE" -name '*.xml' | wc -l | tr -d ' ') plugin(s)"
    else
        cat >&2 <<EOF
No plugins staged yet, and none auto-detected at:
  $INSTALL_ROOT/worlds/plugins

Do ONE of these, then re-run, or pass the source directly:
  • from your install:  just run this script (it looks in the path above)
  • download Quow's pack (plugins + Minimap maps) and:
        $0 --from /path/to/unzipped
  • give a URL:        $0 --url https://.../pack.zip
EOF
        exit 1
    fi
fi

# Sanity: warn loudly if the mapper data is missing — the bundle would build but
# the Cow Bar would abort on launch.
if [ ! -f "$MAPS_DST/_quowmap_database.db" ]; then
    warn "no _quowmap_database.db staged — the Cow Bar mapper needs it to run."
fi

note "ready — build with:  packaging/build-app.sh --with-discworld"
