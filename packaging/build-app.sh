#!/usr/bin/env bash
#
# build-app.sh — build a complete, self-contained MUSHclient package on macOS or Linux.
#
# Usage:
#   packaging/build-app.sh [OPTIONS]
#
# Examples:
#   # build standard Release package
#   packaging/build-app.sh
#
#   # build with Discworld MUD plugins and mapper data bundled
#   packaging/build-app.sh --with-discworld
#
# Platform packaging pipelines (internal helpers):
#   macOS -> packaging/.build-mac.sh   (builds MUSHclient.app, relocates dylibs, codesigns, notarizes, DMG)
#   Linux -> packaging/.build-linux.sh (builds MUSHclient, relocates shared libs, packages .tar.gz and .deb)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OS="$(uname -s)"

case "$OS" in
    Darwin)
        exec "$SCRIPT_DIR/.build-mac.sh" "$@"
        ;;
    Linux)
        exec "$SCRIPT_DIR/.build-linux.sh" "$@"
        ;;
    *)
        printf '\033[1;31merror:\033[0m unsupported operating system: %s (expected Darwin or Linux)\n' "$OS" >&2
        exit 1
        ;;
esac
