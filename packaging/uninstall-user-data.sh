#!/usr/bin/env bash
#
# uninstall-user-data.sh — DEV TOOL.  Remove every file MUSHclient writes
# OUTSIDE its .app bundle, to get a pristine "first run" environment.  Does NOT
# touch the .app itself, the source tree, or anything else.
#
# This is for development/testing only (not for shipping): a stale per-user
# settings file (the wxConfig file or the global-preferences DB) can otherwise
# make a "fresh build" behave like an upgrade and pick up old state.
#
# What it removes (bundle id: com.wadetregaskis.MUSHclient):
#
#   macOS:
#     ~/Library/Application Support/MUSHclient/         worlds, plugins, maps,
#                                                        spell DB, plugin state…
#     ~/Library/Preferences/MUSHclient/                 global-preferences DB
#                                                        (mushclient_prefs.sqlite)
#     ~/Library/Preferences/mushclient Preferences      wxConfig (window state…)
#     ~/Library/Preferences/com.wadetregaskis.MUSHclient.plist   NSUserDefaults / cfprefs
#     ~/Library/Saved Application State/com.wadetregaskis.MUSHclient.savedState/
#     ~/Library/Caches/com.wadetregaskis.MUSHclient/
#     ~/Library/HTTPStorages/com.wadetregaskis.MUSHclient*
#     + `defaults delete com.wadetregaskis.MUSHclient` and a cfprefsd cache flush
#
#   Linux:
#     ${XDG_DATA_HOME:-~/.local/share}/MUSHclient/
#     ${XDG_CONFIG_HOME:-~/.config}/mushclient   and   ~/.mushclient
#
# Usage:
#   packaging/uninstall-user-data.sh             # list what exists, then prompt
#   packaging/uninstall-user-data.sh --yes       # remove without prompting
#   packaging/uninstall-user-data.sh --dry-run   # show what WOULD go; delete nothing
#
set -u

BUNDLE_ID="com.wadetregaskis.MUSHclient"
DRY=0
YES=0

while [ $# -gt 0 ]; do
    case "$1" in
        -n|--dry-run) DRY=1; shift;;
        -y|--yes)     YES=1; shift;;
        -h|--help)    sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
        *) printf 'unknown option: %s (try --help)\n' "$1" >&2; exit 2;;
    esac
done

# ── build the per-OS target list ─────────────────────────────────────────────
TARGETS=()
IS_MAC=0
case "$(uname -s)" in
    Darwin)
        IS_MAC=1
        L="$HOME/Library"
        TARGETS+=("$L/Application Support/MUSHclient")
        TARGETS+=("$L/Preferences/MUSHclient")
        TARGETS+=("$L/Preferences/mushclient Preferences")
        TARGETS+=("$L/Preferences/$BUNDLE_ID.plist")
        TARGETS+=("$L/Saved Application State/$BUNDLE_ID.savedState")
        TARGETS+=("$L/Caches/$BUNDLE_ID")
        TARGETS+=("$L/HTTPStorages/$BUNDLE_ID")
        TARGETS+=("$L/HTTPStorages/$BUNDLE_ID.binarycookies")
        ;;
    *)
        TARGETS+=("${XDG_DATA_HOME:-$HOME/.local/share}/MUSHclient")
        TARGETS+=("${XDG_CONFIG_HOME:-$HOME/.config}/mushclient")
        TARGETS+=("$HOME/.mushclient")
        ;;
esac

# ── warn if the app is running (it rewrites prefs on exit) ────────────────────
if pgrep -f 'MUSHclient.app/Contents/MacOS/MUSHclient' >/dev/null 2>&1 \
   || pgrep -x MUSHclient >/dev/null 2>&1; then
    printf 'warning: MUSHclient appears to be running — quit it first, or it may\n' >&2
    printf '         rewrite its preferences on exit and undo this cleanup.\n' >&2
fi

# ── show what exists ─────────────────────────────────────────────────────────
printf 'MUSHclient user-data uninstall  (bundle id: %s)\n' "$BUNDLE_ID"
found=0
for t in "${TARGETS[@]}"; do
    if [ -e "$t" ]; then
        found=1
        printf '  %-6s %s\n' "$(du -sh "$t" 2>/dev/null | cut -f1)" "$t"
    fi
done
have_defaults=0
if [ "$IS_MAC" = 1 ] && defaults read "$BUNDLE_ID" >/dev/null 2>&1; then
    have_defaults=1; found=1
    printf '  %-6s defaults domain: %s\n' "(cfprefs)" "$BUNDLE_ID"
fi

if [ "$found" = 0 ]; then
    printf '  nothing to remove — already clean.\n'
    exit 0
fi

if [ "$DRY" = 1 ]; then
    printf '(dry run — nothing was deleted)\n'
    exit 0
fi

# ── confirm ──────────────────────────────────────────────────────────────────
if [ "$YES" != 1 ]; then
    printf 'Delete ALL of the above? [y/N] '
    read -r ans
    case "$ans" in
        y|Y|yes|YES) ;;
        *) printf 'aborted.\n'; exit 1;;
    esac
fi

# ── remove ───────────────────────────────────────────────────────────────────
for t in "${TARGETS[@]}"; do
    if [ -e "$t" ]; then
        rm -rf "$t" && printf '  removed  %s\n' "$t"
    fi
done
if [ "$have_defaults" = 1 ]; then
    defaults delete "$BUNDLE_ID" 2>/dev/null || true
    # flush the cfprefs cache so it does not immediately re-write the plist
    killall -u "$USER" cfprefsd 2>/dev/null || true
    printf '  removed  defaults domain %s (+ cfprefsd flush)\n' "$BUNDLE_ID"
fi

printf 'done — the next launch will be a clean first run.\n'
