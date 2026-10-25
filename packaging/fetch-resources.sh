#!/usr/bin/env bash
#
# fetch-resources.sh — the upstream Windows resource project, for its art.
#
# MUSHclient's dialog layouts, menus, bitmaps, icons and cursors live in a
# separate project that Visual Studio compiles into locale/en.dll:
#
#   https://github.com/nickgammon/mushclient_resources
#
# (This repo's README.md points at it under "Resources".)
#
# Windows loads en.dll at runtime.  The macOS port has no DLL resource loader,
# so it needs the underlying files.  Historically they were extracted by hand —
# src/platform/dialog_art/ is 13 bitmaps and icons copied out once, with the
# provenance recorded only in a commit message, and getting one more meant
# repeating the archaeology.  This script replaces that with a pinned checkout.
#
#   staging (git-ignored):
#     packaging/resources/            — the checkout
#     packaging/resources/res/        — the bitmaps/icons/cursors
#     packaging/resources/MUSHclient.rc  — the CANONICAL resource script
#
# NOT REQUIRED TO BUILD.  Unlike the language runtimes, dialog art is not
# optional at runtime — the About box and the Tray/Taskbar page need it — so
# the files this project actually uses stay committed under src/platform/dialog_art/
# and CMake falls back to them.  Running this script is how you obtain art the
# port does not bundle yet; it is not a precondition for `cmake && make`.
# A plain clone with no network must keep building.
#
# LONGER TERM, and deliberately not done here: src/platform/mushclient_dialogs.rc is
# decompiled from a shipped en.dll by tools/decompile_dialogs.py, with numeric
# ids mapped back to resource.h symbols by inference.  The MUSHclient.rc in THIS
# checkout is the real source of those same templates — true symbol names, real
# DLGINIT, no decompiler heuristics — and sourcing the port from it directly
# would be strictly better provenance.  That is a migration across all 116
# dialogs and wants its own diff-by-diff pass with GUI verification, so it is a
# goal rather than a side effect of this script.  Nothing here changes which
# .rc the build uses.
#
# Usage:
#   packaging/fetch-resources.sh                    # pinned revision (below)
#   MC_RESOURCES_REV=master packaging/fetch-resources.sh
#
# Licence: this is Nick Gammon's artwork, redistributed inside the .app exactly
# as the Windows build redistributes it inside en.dll.  Pinning the revision
# here makes that provenance explicit instead of implicit in git history.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$SCRIPT_DIR/resources"
REPO="https://github.com/nickgammon/mushclient_resources"

# Pin by commit, not by branch: an unpinned clone makes the build
# unreproducible, the same reason fetch-tcl.sh pins TCL_VERSION.
REV="${MC_RESOURCES_REV:-70f89f275be84b1c2e413ddcb7409461334c1df5}"

if [ -d "$DEST/.git" ]; then
    have="$(git -C "$DEST" rev-parse HEAD 2>/dev/null || echo none)"
    if [ "$have" = "$REV" ]; then
        echo "resources: already at $REV — nothing to do"
        exit 0
    fi
    echo "resources: updating $have -> $REV"
    git -C "$DEST" fetch --quiet origin
    git -C "$DEST" checkout --quiet "$REV"
else
    echo "resources: cloning $REPO"
    rm -rf "$DEST"
    git clone --quiet "$REPO" "$DEST"
    git -C "$DEST" checkout --quiet "$REV"
fi

# Sanity-check the two things the port cares about, so a moved or renamed
# upstream fails here with a clear message rather than silently later.
missing=0
for f in "MUSHclient.rc" "resource.h" "res"; do
    if [ ! -e "$DEST/$f" ]; then
        echo "resources: MISSING $f — upstream layout changed?" >&2
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1

echo "resources: ready at $DEST ($(git -C "$DEST" rev-parse --short HEAD))"
echo
echo "  res/ bitmaps + icons:  $(ls "$DEST/res" | wc -l | tr -d ' ') files"
echo "  canonical .rc:         $(wc -l < "$DEST/MUSHclient.rc" | tr -d ' ') lines"
echo
echo "The build does NOT require this; src/platform/dialog_art/ is the committed"
echo "fallback.  See packaging/README.md."
