#!/usr/bin/env bash
#
# sign-release.sh — sign and notarize a macOS build made by the GitHub release
# workflow, on your own Mac, so your Developer ID key never leaves it.
#
# The "Release builds" workflow uploads the macOS app UNSIGNED (ad-hoc signed
# only) as MUSHclient-<version>-macOS-universal-UNSIGNED.zip, attached to a draft
# GitHub release.  Download that zip, then run:
#
#   packaging/sign-release.sh MUSHclient-6.00-macOS-universal-UNSIGNED.zip \
#       --sign-identity "Developer ID Application: Your Name (TEAMID)" \
#       --notarize-profile mushclient-notary
#
# It signs the app (inside-out, hardened runtime), notarizes and staples it,
# builds a signed drag-to-Applications DMG, notarizes and staples that too, and
# writes MUSHclient-<version>-macOS-universal.dmg + .zip next to the input (or
# into --output).  With the GitHub CLI installed and logged in (`gh auth login`),
# --upload TAG also attaches both to that draft release; otherwise upload them
# from the release page yourself.
#
# One-time setup for notarization: store an App Store Connect API key (or an
# app-specific password) in your keychain under a profile name:
#   xcrun notarytool store-credentials mushclient-notary \
#       --key AuthKey_XXXX.p8 --key-id XXXX --issuer <issuer-uuid>
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENTITLEMENTS="$SCRIPT_DIR/entitlements.plist"
SIGN_ID="${CODESIGN_IDENTITY:-}"
NOTARIZE_PROFILE="${NOTARYTOOL_PROFILE:-}"
OUTPUT_DIR=""
UPLOAD_TAG=""
INPUT=""

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        -s|--sign-identity)    SIGN_ID="$2"; shift 2;;
        -n|--notarize-profile) NOTARIZE_PROFILE="$2"; shift 2;;
        -o|--output)           OUTPUT_DIR="$2"; shift 2;;
        --upload)              UPLOAD_TAG="$2"; shift 2;;
        -h|--help)             sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
        -*) die "unknown option: $1 (try --help)";;
        *)  [ -z "$INPUT" ] || die "only one input zip"; INPUT="$1"; shift;;
    esac
done

[ "$(uname -s)" = "Darwin" ] || die "this signs a macOS app; run it on a Mac"
[ -n "$INPUT" ] && [ -f "$INPUT" ] || die "give the downloaded ...-UNSIGNED.zip (try --help)"
[ -n "$SIGN_ID" ] && [ "$SIGN_ID" != "-" ] || die "pass --sign-identity \"Developer ID Application: …\" (or set CODESIGN_IDENTITY)"
[ -n "$NOTARIZE_PROFILE" ] || die "pass --notarize-profile NAME (see --help for the one-time setup)"
security find-identity -v -p codesigning | grep -qF "$SIGN_ID" \
    || die "no valid signing identity matching \"$SIGN_ID\" in your keychain"

# shellcheck source=.sign-mac.sh
. "$SCRIPT_DIR/.sign-mac.sh"

base="$(basename "$INPUT" .zip)"
base="${base%-UNSIGNED}"
[ -n "$OUTPUT_DIR" ] || OUTPUT_DIR="$(cd "$(dirname "$INPUT")" && pwd)"
mkdir -p "$OUTPUT_DIR"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

note "unpacking $(basename "$INPUT")"
ditto -x -k "$INPUT" "$work/in"
APP="$(find "$work/in" -maxdepth 2 -name '*.app' -type d | head -1)"
[ -n "$APP" ] || die "no .app inside $INPUT"
note "  $(basename "$APP"): $(lipo -archs "$APP/Contents/MacOS/"* 2>/dev/null | head -1), needs macOS $(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$APP/Contents/Info.plist" 2>/dev/null || echo '?')+"

mc_sign_bundle "$APP" "$SIGN_ID" "$ENTITLEMENTS"

ZIP="$OUTPUT_DIR/$base.zip"
ditto -c -k --keepParent "$APP" "$ZIP"
mc_notarize "$ZIP" "$NOTARIZE_PROFILE"
note "stapling the app"
xcrun stapler staple "$APP" || die "could not staple the app"
ditto -c -k --keepParent "$APP" "$ZIP"          # re-zip with the ticket stapled

DMG="$OUTPUT_DIR/$base.dmg"
note "building $(basename "$DMG")"
mc_make_dmg "$APP" "$DMG" "$SIGN_ID"
mc_notarize "$DMG" "$NOTARIZE_PROFILE"
xcrun stapler staple "$DMG" || die "could not staple the DMG"
spctl --assess --type open --context context:primary-signature -v "$DMG" 2>&1 | sed 's/^/    /' || true

note "signed and notarized:"
echo "  $DMG"
echo "  $ZIP"

if [ -n "$UPLOAD_TAG" ]; then
    command -v gh >/dev/null || die "--upload needs the GitHub CLI (brew install gh; gh auth login)"
    note "attaching both to release $UPLOAD_TAG"
    gh release upload "$UPLOAD_TAG" "$DMG" "$ZIP" --clobber
    note "uploaded; delete the -UNSIGNED.zip asset from the draft before publishing"
else
    echo
    echo "Next: attach both files to the draft release on GitHub, delete the"
    echo "-UNSIGNED.zip asset, and publish."
fi
