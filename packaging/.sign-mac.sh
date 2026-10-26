# .sign-mac.sh — signing, notarization and DMG creation for the macOS app.
#
# Sourced (not run) by .build-mac.sh and sign-release.sh, so a local build and a
# release signed from a CI build are signed exactly the same way.  Expects the
# caller to define note / warn / die.

# mc_sign_bundle APP IDENTITY ENTITLEMENTS
#   Sign every nested Mach-O inside-out, then the app with the hardened runtime.
#   IDENTITY "-" is ad-hoc (runs on this Mac only; cannot be notarized).
mc_sign_bundle() {
    local app="$1" id="$2" ent="$3" f
    local fw="$app/Contents/Frameworks"
    # A real Developer ID signature gets a secure timestamp (needs network);
    # ad-hoc cannot be timestamped.  Separate commands rather than an empty-array
    # flag keep this bash-3.2 clean under `set -u`.
    _mc_sign_lib() {
        if [ "$id" = "-" ]; then codesign --force --options runtime --sign - "$1"
        else codesign --force --options runtime --timestamp --sign "$id" "$1"; fi
    }
    # codesign refuses files carrying extended attributes (resource forks, Finder
    # info, com.apple.quarantine), which cp and downloads routinely leave behind.
    note "stripping extended attributes (xattr -cr)"
    xattr -cr "$app" 2>/dev/null || true   # tolerate dangling symlinks (framework PrivateHeaders)

    note "code signing with identity: $id"
    [ "$id" = "-" ] && warn "ad-hoc signing — fine for THIS Mac, but cannot be notarized or run elsewhere"
    if [ -d "$fw" ]; then
        while IFS= read -r -d '' f; do _mc_sign_lib "$f"; done \
            < <(find "$fw" -type f \( -name '*.dylib' -o -name '*.so' \) -print0)
        for f in "$fw"/*.framework; do [ -d "$f" ] && _mc_sign_lib "$f"; done
    fi
    # Nested Mach-O outside Frameworks — the relocatable Ruby's libruby and its
    # .bundle extensions under Contents/lib.  Unsigned nested code makes a
    # Developer ID / notarized build fail.
    local d
    for d in "$app/Contents/lib" "$app/Contents/Resources"; do
        [ -d "$d" ] || continue
        while IFS= read -r -d '' f; do _mc_sign_lib "$f"; done \
            < <(find "$d" -type f \( -name '*.dylib' -o -name '*.so' -o -name '*.bundle' \) -print0)
    done
    if [ "$id" = "-" ]; then
        codesign --force --options runtime --entitlements "$ent" --sign - "$app"
    else
        codesign --force --options runtime --timestamp --entitlements "$ent" --sign "$id" "$app"
    fi
    codesign --verify --deep --strict --verbose=2 "$app" 2>&1 | sed 's/^/    /' \
        || die "codesign --verify rejected $app"
}

# mc_notarize FILE PROFILE  — submit a .zip or .dmg to Apple and wait.
mc_notarize() {
    local file="$1" profile="$2"
    note "notarizing $(basename "$file") via keychain profile '$profile' (this can take a few minutes)"
    xcrun notarytool submit "$file" --keychain-profile "$profile" --wait --output-format plist \
        > "$file.notary.plist" || die "notarytool submit failed for $file"
    local status
    status="$(/usr/libexec/PlistBuddy -c 'Print :status' "$file.notary.plist" 2>/dev/null || true)"
    if [ "$status" != "Accepted" ]; then
        local id
        id="$(/usr/libexec/PlistBuddy -c 'Print :id' "$file.notary.plist" 2>/dev/null || true)"
        die "notarization of $(basename "$file") ended '$status'${id:+ — details: xcrun notarytool log $id --keychain-profile $profile}"
    fi
    rm -f "$file.notary.plist"
}

# mc_make_dmg APP DMG [IDENTITY]
#   A drag-to-Applications disk image; signed with IDENTITY unless it is "-"/empty.
mc_make_dmg() {
    local app="$1" dmg="$2" id="${3:-}" stage
    stage="$(mktemp -d)"
    cp -R "$app" "$stage/"
    ln -s /Applications "$stage/Applications"
    rm -f "$dmg"
    hdiutil create -volname "$(basename "$app" .app)" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$dmg" >/dev/null \
        || die "hdiutil could not create $dmg"
    rm -rf "$stage"
    if [ -n "$id" ] && [ "$id" != "-" ]; then
        codesign --force --timestamp --sign "$id" "$dmg" || die "could not sign $dmg"
    fi
}
