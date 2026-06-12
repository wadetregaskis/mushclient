# Packaging MUSHclient for macOS

Three scripts live here:

| Script | What it does |
|--------|--------------|
| [`build-app.sh`](build-app.sh) | Build a complete, self-contained, signed `MUSHclient.app` (+ `.dmg`). |
| [`fetch-python.sh`](fetch-python.sh) | Stage a clean, redistributable python.org framework to embed. |
| [`fetch-discworld-plugins.sh`](fetch-discworld-plugins.sh) | Stage Quow's Discworld plugins so they can be pre-bundled. |

Everything the scripts produce lands in `dist/` (git-ignored).

---

## Quick start

```bash
# self-contained, ad-hoc-signed app + DMG in dist/  (runs on THIS Mac)
packaging/build-app.sh
```

That builds Release, copies every non-system dylib/framework the app links into
`MUSHclient.app/Contents/Frameworks`, rewrites the load commands so the app no
longer needs Homebrew/Xcode installed, ad-hoc signs it, and rolls a DMG.

To make a build you can give to *other* people you must sign + notarize with
your Apple Developer ID (see below).

### Prerequisites

- Xcode command-line tools (`xcode-select --install`)
- Homebrew deps the app links: `brew install wxwidgets libpng lua@5.1 openssl@3`
- For notarization: an Apple Developer account + a *Developer ID Application*
  certificate in your login keychain.

---

## Signed + notarized release build

This is the "give it to other people" build. Do it on a Mac that has your
Developer ID certificate (you mentioned your credentials aren't on this
machine — this is the part that needs them).

**1. One-time: store a notarytool credential profile in your keychain.**

```bash
xcrun notarytool store-credentials mushclient-notary \
    --apple-id "you@example.com" \
    --team-id  "YOURTEAMID" \
    --password "app-specific-password"      # from appleid.apple.com
```

**2. Find your signing identity.**

```bash
security find-identity -v -p codesigning
# → "Developer ID Application: Your Name (YOURTEAMID)"
```

**3. Build, sign, notarize, staple, DMG — one command.**

```bash
packaging/build-app.sh \
    --sign-identity   "Developer ID Application: Your Name (YOURTEAMID)" \
    --notarize-profile mushclient-notary
```

You can also export `CODESIGN_IDENTITY` and `NOTARYTOOL_PROFILE` instead of
passing the flags.

**4. Verify (after it finishes).**

```bash
codesign --verify --deep --strict --verbose=2 dist/MUSHclient.app
spctl  -a -t exec -vvv               dist/MUSHclient.app   # → "accepted, source=Notarized Developer ID"
xcrun  stapler validate              dist/MUSHclient.dmg
```

---

## Discworld edition (Quow's plugins pre-bundled)

Quow's plugins are **not** in this repo (they're third-party; the staging dir
`packaging/discworld-plugins/` is git-ignored). Stage them, then build with
`--with-discworld`:

```bash
# stage the plugins (pick whichever applies):
packaging/fetch-discworld-plugins.sh                       # auto-copy from your install
packaging/fetch-discworld-plugins.sh --from ~/Downloads/quow
packaging/fetch-discworld-plugins.sh --url https://quow.site/plugins.zip

# then build the edition:
packaging/build-app.sh --with-discworld \
    --sign-identity "Developer ID Application: ..." --notarize-profile mushclient-notary
```

`--with-discworld` copies the staged `*.xml` into
`MUSHclient.app/Contents/Resources/plugins/`. **On first launch** the app copies
any of those the user doesn't already have into
`~/Library/Application Support/MUSHclient/worlds/plugins/` (non-destructive — it
never overwrites an existing file), so they show up in the plugin manager and
can be added to a Discworld world. A normal build ships no bundled plugins and
the seeding step is a no-op.

---

## Embedded Python (clean python.org framework)

The app embeds CPython. To avoid shipping **Xcode's** `Python3.framework` (Apple's,
not cleanly redistributable), stage a clean **python.org** framework once:

```bash
packaging/fetch-python.sh                 # downloads + extracts python.org 3.14.6
PYTHON_VERSION=3.14.6 packaging/fetch-python.sh
packaging/fetch-python.sh --pkg ~/Downloads/python-3.14.6-macos11.pkg   # offline
```

`fetch-python.sh` downloads the official installer, extracts its `Python.framework`
**without installing it** (`pkgutil --expand-full`), and makes it relocatable, into
the git-ignored `packaging/python/`. After that, **`build-app.sh` uses it
automatically** — CMake links it, and the app bundles it under
`Contents/Frameworks/`, thinned to the app's architecture and pruned (no
tkinter/tests/idle). The embedded interpreter finds its stdlib inside the `.app`
at runtime (via `dladdr`), so it works on any Mac. PSF-licensed → free to ship.

If `packaging/python/` is absent, `build-app.sh` falls back to whatever
`find_package(Python3)` locates (e.g. Xcode's framework) — fine for local testing,
**not** for a public release.

(Perl uses the system `libperl`, and JavaScript uses the system
`JavaScriptCore.framework` — both are always present on macOS, so neither needs
bundling.)

---

## What `build-app.sh` does, step by step

1. **Build** the Release `mushgui` target into `build-release/`.
2. **Bundle plugins** (`--with-discworld` only) into `Contents/Resources/plugins/`.
3. **Relocate dependencies**: BFS over the app's Mach-O dependencies; copy every
   non-system `.dylib` into `Contents/Frameworks/` (id → `@rpath/...`), copy
   linked `.framework`s in whole, rewrite all load commands to `@rpath`, add an
   `@executable_path/../Frameworks` rpath, and strip leftover Homebrew/Xcode
   rpaths. Result: `otool -L` shows only `/usr/lib`, `/System`, and `@rpath`.
4. **Info.plist**: set `LSMinimumSystemVersion`, `NSHighResolutionCapable`,
   `CFBundleVersion`.
5. **Code sign** inside-out (nested dylibs/frameworks first, then the app) with
   the hardened runtime and [`entitlements.plist`](entitlements.plist). Without
   `--sign-identity` it ad-hoc signs (`-`).
6. **Notarize + staple** (`--notarize-profile` only).
7. **Package**: copy the `.app` to `dist/`, make a `.zip` (for notarization /
   upload) and a `.dmg` (with an `/Applications` symlink for drag-install).

### Entitlements

The hardened runtime is relaxed just enough for the three embedded engines —
JIT (JavaScriptCore), unsigned executable memory (CPython/libperl), and library
validation disabled (so the interpreters can load extension modules and the app
can load the bundled dylibs). See the comments in
[`entitlements.plist`](entitlements.plist).

### Useful flags

`--no-relocate` (fast local build against Homebrew), `--no-dmg`, `--clean`,
`-j N`, `-o DIR`, `-t Debug`. Run `packaging/build-app.sh --help` for the full list.
