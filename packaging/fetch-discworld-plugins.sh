#!/usr/bin/env bash
#
# fetch-discworld-plugins.sh — stage Quow's Discworld plugins for bundling.
#
# Quow's plugins are NOT part of this repository (they're third-party and not
# ours to redistribute in git).  This script stages them into a git-ignored
# directory so `build-app.sh --with-discworld` can bundle them into the .app.
#
#   staging dir:  packaging/discworld-plugins/   (git-ignored; *.xml plugins)
#
# Three ways to populate it (in order of convenience):
#
#   1. From your existing MUSHclient install (auto-detected):
#        packaging/fetch-discworld-plugins.sh
#      copies ~/Library/Application Support/MUSHclient/worlds/plugins/Quow*.xml
#
#   2. From a local folder you already downloaded/unzipped:
#        packaging/fetch-discworld-plugins.sh --from ~/Downloads/quow-plugins
#
#   3. From a URL (Quow's website — a .zip, .tar.gz, or a single .xml):
#        packaging/fetch-discworld-plugins.sh --url https://quow.example/plugins.zip
#        # or:  QUOW_PLUGINS_URL=... packaging/fetch-discworld-plugins.sh
#
# Whatever the source, the result is *.xml files sitting in the staging dir.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAGE="${DISCWORLD_PLUGINS_DIR:-$SCRIPT_DIR/discworld-plugins}"
URL="${QUOW_PLUGINS_URL:-}"
FROM=""
DEFAULT_INSTALL="$HOME/Library/Application Support/MUSHclient/worlds/plugins"

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --url)   URL="$2"; shift 2;;
        --from)  FROM="$2"; shift 2;;
        -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

mkdir -p "$STAGE"

stage_from_dir() {  # copy *.xml from $1 into the staging dir
    local dir="$1"
    shopt -s nullglob
    local xmls=("$dir"/*.xml)
    shopt -u nullglob
    [ ${#xmls[@]} -gt 0 ] || die "no *.xml files found in: $dir"
    cp "${xmls[@]}" "$STAGE/"
    note "staged ${#xmls[@]} plugin(s) into $STAGE"
}

if [ -n "$URL" ]; then
    note "downloading $URL"
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    out="$tmp/download"
    curl -fL --retry 3 -o "$out" "$URL" || die "download failed"
    case "$URL" in
        *.zip)            unzip -joq "$out" '*.xml' -d "$STAGE" || die "unzip failed";;
        *.tar.gz|*.tgz)   tar -xzf "$out" -C "$tmp"; stage_from_dir "$tmp";;
        *.xml)            cp "$out" "$STAGE/$(basename "$URL")";;
        *) # sniff the content
           if unzip -l "$out" >/dev/null 2>&1; then unzip -joq "$out" '*.xml' -d "$STAGE"
           else die "don't know how to extract $URL (expected .zip/.tar.gz/.xml)"; fi;;
    esac
    note "staged $(ls -1 "$STAGE"/*.xml 2>/dev/null | wc -l | tr -d ' ') plugin(s) into $STAGE"

elif [ -n "$FROM" ]; then
    [ -d "$FROM" ] || die "not a directory: $FROM"
    stage_from_dir "$FROM"

else
    # auto-detect: prefer Quow* from a local install, else any plugins there
    shopt -s nullglob
    quow=("$DEFAULT_INSTALL"/Quow*.xml)
    shopt -u nullglob
    if [ ${#quow[@]} -gt 0 ]; then
        cp "${quow[@]}" "$STAGE/"
        note "staged ${#quow[@]} Quow* plugin(s) from your install into $STAGE"
    elif ls "$STAGE"/*.xml >/dev/null 2>&1; then
        note "already staged: $(ls -1 "$STAGE"/*.xml | wc -l | tr -d ' ') plugin(s) in $STAGE"
    else
        cat >&2 <<EOF
No plugins staged yet, and none auto-detected at:
  $DEFAULT_INSTALL

Do ONE of these, then re-run, or pass the source directly:
  • download Quow's plugin pack and:  $0 --from /path/to/unzipped
  • give a URL:                        $0 --url https://.../plugins.zip
  • or just drop the *.xml files into: $STAGE
EOF
        exit 1
    fi
fi

note "ready — build with:  packaging/build-app.sh --with-discworld"
