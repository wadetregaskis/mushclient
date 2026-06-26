#!/usr/bin/env bash
#
# fetch-discworld-plugins.sh — stage Quow's pre-configured Discworld world, his
# plugins, AND the mapper's data so `build-app.sh --with-discworld` can bundle a
# ready-to-play Discworld edition of the app.
#
# Quow's material is third-party and NOT part of this repository.  This script
# stages it into a git-ignored directory.  By DEFAULT it downloads Quow's
# canonical "Pre-Configured Portable Discworld MUSHclient" package — a single
# .zip that contains everything we need:
#
#     <pkg>/worlds/Discworld.MCL                 <- pre-configured world,
#                                                   plugins pre-installed via
#                                                   <include ... plugin="y"/>
#     <pkg>/quow_plugins/Quow*.xml               <- the plugins themselves
#     <pkg>/quow_plugins/maps/_quowmap_database.db
#     <pkg>/quow_plugins/maps/*.png  *.wav       <- the mapper's room DB + art
#
# It is restaged into the layout MUSHclient resolves on this port:
#
#     discworld-plugins/worlds/Discworld.mcl              (auto-opened world)
#     discworld-plugins/worlds/plugins/Quow*.xml          (m_strPluginsDirectory
#                                                          default = worlds/plugins)
#     discworld-plugins/quow_plugins/maps/*               (mapper data; the Cow
#                                                          Bar reads quow_plugins/maps)
#
# The per-user database (_quowmap_userdata.db — combat stats, hotspots) is NOT
# staged: the plugin creates a fresh one per user.
#
# Sources, in order of how you ask for them:
#
#   1. DEFAULT — download Quow's portable package and extract everything:
#        packaging/fetch-discworld-plugins.sh
#      (override the URL with --url or QUOW_PACKAGE_URL=...)
#
#   2. From a local copy you already downloaded/unzipped (or your own install):
#        packaging/fetch-discworld-plugins.sh --from ~/Downloads/MUSHclient
#      finds Discworld.mcl, the Quow*.xml plugins, and the maps/ folder anywhere
#      underneath it.
#
#   3. From your existing MUSHclient install:
#        packaging/fetch-discworld-plugins.sh --local
#      stages from ~/Library/Application Support/MUSHclient/
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAGE="${DISCWORLD_PLUGINS_DIR:-$SCRIPT_DIR/discworld-plugins}"
# Quow's canonical pre-configured portable package (world + plugins + map DB).
PACKAGE_URL="${QUOW_PACKAGE_URL:-https://quow.co.uk/MUSHclient.zip}"
URL=""
FROM=""
LOCAL=0
INSTALL_ROOT="$HOME/Library/Application Support/MUSHclient"

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --url)   URL="$2"; shift 2;;
        --from)  FROM="$2"; shift 2;;
        --local) LOCAL=1; shift;;
        -h|--help) sed -n '2,52p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

WORLD_DST="$STAGE/worlds"
PLUGINS_DST="$STAGE/worlds/plugins"
MAPS_DST="$STAGE/quow_plugins/maps"

# Start clean so a re-run never leaves stale files behind.
rm -rf "$STAGE"
mkdir -p "$WORLD_DST" "$PLUGINS_DST" "$MAPS_DST"

# stage_world SRC : copy the pre-configured Discworld world (the one carrying the
# Quow <include ... plugin="y"/> lines) into worlds/Discworld.mcl.
stage_world() {
    local src="$1" f=""
    # Prefer a Discworld*.mcl that actually pre-installs the Quow plugins.
    while IFS= read -r cand; do
        [ -n "$cand" ] || continue
        if grep -qiE '<include[^>]*Quow[^>]*plugin' "$cand" 2>/dev/null; then f="$cand"; break; fi
        [ -n "$f" ] || f="$cand"   # fall back to the first Discworld*.mcl seen
    done <<EOF
$(find "$src" -maxdepth 5 -type f -iname 'Discworld*.mcl' 2>/dev/null || true)
EOF
    if [ -z "$f" ]; then
        warn "no Discworld*.mcl found under: $src — the edition will have no world"
        return 0
    fi
    cp "$f" "$WORLD_DST/Discworld.mcl"
    note "staged world: $(basename "$f") -> worlds/Discworld.mcl"
}

# stage_plugins SRC : copy Quow*.xml found under SRC into worlds/plugins/.
stage_plugins() {
    local src="$1" found="" n=0 f
    found="$(find "$src" -maxdepth 5 -type f -name 'Quow*.xml' 2>/dev/null || true)"
    [ -n "$found" ] || die "no Quow*.xml plugins found under: $src"
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        cp "$f" "$PLUGINS_DST/" && n=$((n + 1))
    done <<EOF
$found
EOF
    note "staged $n Quow plugin(s) into worlds/plugins/"
}

# stage_maps SRC : find a maps/ folder (the one holding _quowmap_database.db)
# under SRC and stage its contents, minus the per-user database.
stage_maps() {
    local src="$1" mapsdir="" d
    while IFS= read -r d; do
        [ -n "$d" ] || continue
        if [ -f "$d/_quowmap_database.db" ]; then mapsdir="$d"; break; fi
    done <<EOF
$(find "$src" -maxdepth 6 -type d -name maps 2>/dev/null || true)
EOF
    if [ -z "$mapsdir" ]; then
        warn "no maps/ folder containing _quowmap_database.db found under: $src"
        warn "the Cow Bar mapper will not run without it."
        return 0
    fi
    # copy everything except the per-user DB (the plugin creates that itself)
    find "$mapsdir" -type f \
        ! -name '_quowmap_userdata.db' ! -name '.DS_Store' \
        -exec cp {} "$MAPS_DST/" \;
    local nm
    nm="$(find "$MAPS_DST" -type f | wc -l | tr -d ' ')"
    note "staged $nm map file(s) into quow_plugins/maps/ ($(du -sh "$MAPS_DST" | cut -f1))"
}

# stage_all SRC : pull the world, plugins and maps from one source tree.
stage_all() {
    local src="$1"
    stage_world   "$src"
    stage_plugins "$src"
    stage_maps    "$src"
}

# download_and_stage URL : fetch a .zip/.tar.gz, extract, and stage from it.
download_and_stage() {
    local url="$1" tmp out ext
    note "downloading $url"
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' RETURN
    out="$tmp/download"
    curl -fL --retry 3 -o "$out" "$url" || die "download failed: $url"
    note "downloaded $(du -h "$out" | cut -f1)"
    ext="$tmp/extract"; mkdir -p "$ext"
    case "$url" in
        *.zip)          unzip -qo "$out" -d "$ext" || die "unzip failed";;
        *.tar.gz|*.tgz) tar -xzf "$out" -C "$ext"  || die "untar failed";;
        *)  if unzip -l "$out" >/dev/null 2>&1; then unzip -qo "$out" -d "$ext"
            else die "don't know how to extract $url (expected .zip / .tar.gz)"; fi;;
    esac
    stage_all "$ext"
}

if [ -n "$URL" ]; then
    download_and_stage "$URL"
elif [ -n "$FROM" ]; then
    [ -d "$FROM" ] || die "not a directory: $FROM"
    stage_all "$FROM"
elif [ "$LOCAL" = 1 ]; then
    [ -d "$INSTALL_ROOT" ] || die "no local install at: $INSTALL_ROOT"
    stage_all "$INSTALL_ROOT"
else
    # DEFAULT: download Quow's canonical portable package.
    download_and_stage "$PACKAGE_URL"
fi

# Sanity checks — warn loudly if a required piece is missing, so the bundle is
# never silently incomplete.
[ -f "$WORLD_DST/Discworld.mcl" ] \
    || warn "no Discworld.mcl staged — the edition will not auto-open a world."
[ -n "$(find "$PLUGINS_DST" -name 'Quow*.xml' 2>/dev/null)" ] \
    || warn "no Quow*.xml staged — the world's plugins will be missing."
[ -f "$MAPS_DST/_quowmap_database.db" ] \
    || warn "no _quowmap_database.db staged — the Cow Bar mapper needs it to run."

note "ready — build with:  packaging/build-app.sh --with-discworld"
