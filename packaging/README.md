# Packaging MUSHclient

The packaging scripts build self-contained, distributable MUSHclient releases for macOS and Linux.

---

## Packaging for Linux

| Script | What it does |
|--------|--------------|
| [`build-app.sh`](build-app.sh) | Build Release, relocate shared libraries, package standalone `.tar.xz` and Debian `.deb`. |
| [`fetch-python.sh`](fetch-python.sh) | Vendor a standalone CPython 3 embed environment (Python engine). |
| [`fetch-tcl.sh`](fetch-tcl.sh) | Build a self-contained static **Tcl 8.6** from source (Tcl engine). |
| [`fetch-ruby.sh`](fetch-ruby.sh) | Build a relocatable **Ruby 4.0** from source, with load-relative stdlib (Ruby engine). |
| [`fetch-discworld-plugins.sh`](fetch-discworld-plugins.sh) | Stage Quow's Discworld plugins so they can be pre-bundled. |

`packaging/build-app.sh` is the single build script across both macOS and Linux, automatically delegating to `.build-mac.sh` on macOS and `.build-linux.sh` on Linux.

### Quick start (Linux)

```bash
packaging/build-app.sh
```

This builds Release mode with CMake, vendors/relocates dynamic libraries, stages runtime resources (dialog templates, dialog art, toolbar strips, FIGlet fonts, spell checker dictionaries, and Lua modules), and produces in `dist/`:
- `dist/mushclient/`: standalone relocatable application bundle with launcher script `mushclient.sh`
- `dist/mushclient-6.0.0-linux-<arch>.tar.xz`: self-contained relocatable tarball archive
- `dist/mushclient_6.0.0_<arch>.deb`: standard Debian/Ubuntu package for system installation

To include Quow's Discworld plugins and maps:
```bash
packaging/fetch-discworld-plugins.sh
packaging/build-app.sh --with-discworld
```

### Linux Prerequisites

- CMake 3.14+ (`apt install cmake ninja-build`)
- C++14 compiler (`g++` or `clang++`)
- wxWidgets 3.2+ GTK3 development headers (`apt install libwxgtk3.2-dev` or `libwxgtk3.0-gtk3-dev`)
- Lua 5.1 development library (`apt install liblua5.1-0-dev`)
- Optional scripting runtimes: Python 3, Perl (`libperl-dev`), JavaScriptCore (`libjavascriptcoregtk-4.1-dev`), Tcl (`tcl-dev`), Ruby (`ruby-dev`)
- For `.deb` creation: `dpkg-deb` (standard on Debian/Ubuntu)

---

## Packaging for macOS

The scripts here build a complete, self-contained, signed `MUSHclient.app`:

| Script | What it does |
|--------|--------------|
| [`build-app.sh`](build-app.sh) | Build the whole thing — relocate deps, sign, notarize, DMG. |
| [`fetch-python.sh`](fetch-python.sh) | Vendor a clean, redistributable **python.org** framework (Python engine). |
| [`fetch-tcl.sh`](fetch-tcl.sh) | Build a static **Tcl 8.6** from source (Tcl engine). |
| [`fetch-ruby.sh`](fetch-ruby.sh) | Build a relocatable **Ruby 4.0** from source, + static OpenSSL/libyaml (Ruby engine). |
| [`fetch-discworld-plugins.sh`](fetch-discworld-plugins.sh) | Stage Quow's Discworld plugins so they can be pre-bundled. |

Everything the scripts produce lands in `dist/` (the final app) or git-ignored
staging dirs (`packaging/{python,tcl,ruby,discworld-plugins}/`).

> **Currently arm64-only** — wxWidgets here is arm64-only, so the app is too;
> `build-app.sh` thins the bundled runtimes to match. A universal build needs
> universal wx + deps first.

---

## Quick start

```bash
# one-time: vendor the brew-free scripting runtimes (cached; see below)
packaging/fetch-python.sh
packaging/fetch-tcl.sh
packaging/fetch-ruby.sh

# self-contained, ad-hoc-signed app + DMG in dist/  (runs on THIS Mac)
packaging/build-app.sh
```

That builds Release, copies every non-system dylib/framework the app links into
`MUSHclient.app/Contents/Frameworks` (the bundled Ruby goes in `Contents/lib`),
rewrites the load commands so the app no longer needs Homebrew/Xcode installed,
ad-hoc signs it, and rolls a DMG.

If you skip the `fetch-*` scripts the build still works but falls back to the
**system** interpreters (Apple's Xcode Python, system Tcl 8.5, system Ruby 2.6 —
all deprecated) — fine for a quick local build, **not** for a release.

To make a build you can give to *other* people you must sign + notarize with
your Apple Developer ID (see below).

### Prerequisites

- Xcode command-line tools (`xcode-select --install`)
- Homebrew deps the app itself links: `brew install wxwidgets libpng lua@5.1 openssl@3`
  (wxWidgets = GUI; libpng = images; lua@5.1 = the native scripting engine, else
  CMake builds it via FetchContent; openssl@3 = the app's own TLS, bundled into
  `Contents/Frameworks` at package time)
- The vendored scripting runtimes (`fetch-python.sh` / `fetch-tcl.sh` /
  `fetch-ruby.sh`) — these build from source and need no Homebrew
- For notarization: an Apple Developer account + a *Developer ID Application*
  certificate in your login keychain

---

## Signed + notarized release build

This is the "give it to other people" build. Do it on a Mac that has your
Developer ID certificate.

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

### What gets bundled — plugins *and* their map data

The Cow Bar plugin (`QuowMinimap.xml`) is a mapper: at runtime it loads a room
database (`_quowmap_database.db`, ~18 MB), map images (`*.png`) and sound effects
(`*.wav`) from a `quow_plugins/maps/` folder. Bundling only the `*.xml` is not
enough — the plugin aborts on launch ("Unable to find/load the room database
file") without that data. So `fetch-discworld-plugins.sh` stages **both**, into a
tree that mirrors the per-user data directory:

```
packaging/discworld-plugins/
  worlds/plugins/Quow*.xml                       # the plugins
  quow_plugins/maps/_quowmap_database.db         # the room database
  quow_plugins/maps/*.png  *.wav                 # map images + sounds
```

(The per-user database `_quowmap_userdata.db` — combat stats, hotspots — is
**not** staged; the plugin creates a fresh one per user.) `fetch` finds the maps
folder automatically from your install (`~/Library/Application Support/
MUSHclient/quow_plugins/maps/`) or anywhere under a `--from` / `--url` source.

`--with-discworld` copies that tree into
`MUSHclient.app/Contents/Resources/userdata-seed/`. **On first launch** the app
recursively seeds anything the user doesn't already have into their data dir —
plugins into `worlds/plugins/`, map data into `quow_plugins/maps/` —
non-destructive (an existing file is never overwritten, so the per-user database
and any user edits survive a reinstall). A normal build ships no `userdata-seed`
and the seeding step is a no-op.

> The mapper finds its data via a path relative to the MUSHclient data dir, so
> the app sets its working directory there at startup (the macOS analogue of the
> Windows "MUSHclient folder"); without that the relative path would not resolve.

---

## Embedded scripting runtimes

MUSHclient runs six languages (worlds **and** plugins): **Lua, Python, Perl,
JavaScript, Tcl, Ruby**. To keep the app self-contained and free of Homebrew, the
three runtimes that aren't already on every Mac are **vendored from source** by
the `fetch-*` scripts (cached in git-ignored dirs; `build-app.sh` then picks them
up automatically):

| Engine | Source | Where it ends up in the `.app` |
|--------|--------|--------------------------------|
| **Lua 5.1** | Homebrew `lua@5.1`, else FetchContent | statically linked into the executable |
| **Python 3.14** | `fetch-python.sh` (python.org framework) | `Contents/Frameworks/Python.framework` (relocated) |
| **Tcl 8.6** | `fetch-tcl.sh` (built static) | statically linked; script library at `Contents/Resources/tcl8.6` |
| **Ruby 4.0** | `fetch-ruby.sh` (built shared) | `Contents/lib/libruby.<ver>.dylib` + stdlib |
| **Perl 5.34** | system `libperl` | not bundled (always present) |
| **JavaScript** | system `JavaScriptCore.framework` | not bundled (always present) |

**Not supported:** VBScript (COM-only, no embeddable form) and PHP (postponed —
macOS has no redistributable embeddable PHP). The GUI shows a clear note when a
world/plugin selects an unsupported language.

Each `fetch-*` script is **idempotent and offline-friendly** — it caches the
downloaded source and only rebuilds if the staged output is missing. Versions are
overridable, e.g. `RUBY_VERSION=4.0.6 packaging/fetch-ruby.sh`,
`TCL_VERSION=8.6.16 packaging/fetch-tcl.sh`,
`PYTHON_VERSION=3.14.6 packaging/fetch-python.sh` (the last also accepts
`--pkg ~/Downloads/python-3.14.6-macos11.pkg` for a fully offline run).

Notes:
- **Python** — `fetch-python.sh` downloads the official installer, extracts its
  `Python.framework` *without installing it* (`pkgutil --expand-full`) and makes
  it relocatable. `build-app.sh` thins it to the app's arch and prunes
  tkinter/tests/idle. The interpreter finds its stdlib inside the `.app` via
  `dladdr`. PSF-licensed → free to ship.
- **Tcl** — built static (8.6 keeps the engine's C API; 9.0 changed signatures),
  so it bundles its own libtommath and needs only system zlib/CoreFoundation —
  nothing extra to relocate. `tcl_engine.cpp` points `TCL_LIBRARY` at the bundled
  script library.
- **Ruby** — built `--enable-shared --enable-load-relative`, with **libyaml**
  linked statically into psych. Its `openssl`/`digest` extensions link Homebrew
  `openssl@3` — the *same* OpenSSL the app and Python use — so the bundle ships a
  **single** shared OpenSSL (next note), not a private Ruby copy. `libruby` lives
  in `Contents/lib` (not `Frameworks`): Ruby's extensions link it as
  `@executable_path/../lib/...` and load-relative derives the stdlib prefix from
  there, so the host and every extension are self-locating.
- **OpenSSL (one shared copy)** — the app links Homebrew `openssl@3` for its own
  TLS; `build-app.sh` relocates a single `libssl.3.dylib`+`libcrypto.3.dylib`
  into `Contents/Frameworks` and repoints **all three** consumers at it: the app,
  Ruby's `openssl`/`digest` extensions, and Python's `_ssl`/`_hashlib` (whose
  framework-internal pair is then deleted). OpenSSL 3.x is ABI-stable within the
  series, so Python's extensions — built against an older 3.x — load the shared
  (newer) copy fine. Net: no duplicate OpenSSL archives in the `.app`, and one
  version to track for CVEs.

---

## Upstream Windows resources (`fetch-resources.sh`)

MUSHclient's dialog layouts, menus, bitmaps, icons and cursors are not in the
main source tree. They live in a separate project that Visual Studio compiles
into `locale/en.dll` — <https://github.com/nickgammon/mushclient_resources>,
which this repo's `README.md` points at under "Resources". Windows loads that
DLL at runtime; the macOS port has no DLL resource loader, so it needs the
underlying files.

```bash
packaging/fetch-resources.sh          # pinned checkout into packaging/resources/
MC_RESOURCES_REV=master packaging/fetch-resources.sh
```

**This is not required to build.** Unlike the language runtimes, dialog art is
not optional at runtime — the About box and the Tray/Taskbar page need it — so
the files the port actually uses stay committed under `src/platform/dialog_art/`,
and that is what CMake bundles. A plain `git clone && cmake && make` with no
network must keep working. Run this script when you need art the port does not
bundle yet.

Pinned by commit, not by branch: an unpinned clone would make builds
unreproducible, the same reason `fetch-tcl.sh` pins `TCL_VERSION`.

**Licence.** This is Nick Gammon's artwork, redistributed inside the `.app`
exactly as the Windows build redistributes it inside `en.dll`. Pinning the
revision here makes that provenance explicit rather than implicit in git
history.

### Direction of travel

Two things in that checkout are better than what the port uses today, in
increasing order of effort:

1. **Art the port has never had.** e.g. the activity button bar
   (`IDB_ACTIVITY_TOOLBAR_0..5`), whose six 480×15 4-bit strips — 30 tiles of
   16×15 each — are `res/bitmap1.bmp` and `res/mc activity 0..4.bmp`. Note the
   mapping is not in step: `_0` is `bitmap1.bmp`, `_1`…`_5` are
   `mc activity 0`…`4`.

   **THE ART NEEDS REDOING BEFORE THIS BAR CAN BE BUILT — there is no plan for
   that yet, and that is what blocks the feature, not the code.** These are
   1990s Windows toolbar assets: 16×15 tiles at 4 bits per pixel (16 colours),
   authored for a 96 dpi 1× display. On a Retina screen every button would be
   point-doubled — soft edges, visible stair-stepping — and a 16-colour palette
   next to the rest of the macOS UI looks like what it is. Upscaling will not
   rescue them; 16×15 has no detail to recover, and the magenta-key transparency
   convention of the era does not survive resampling cleanly.

   What "redone" has to mean, whenever someone takes it on: vector or @2x/@3x
   raster sources at the sizes macOS actually asks for, with the palette
   reconsidered rather than sampled. Until then, the sensible options are to
   leave the bar out (where it is today) or to draw it from SF Symbols and
   accept that it will not look like the Windows original.

   Tracked as settings-audit item 54 — "Activity button bar style (Style 1..6)"
   — which cannot be honoured while there is no bar to style.

2. **The canonical `.rc` itself.** `src/platform/mushclient_dialogs.rc` is
   *decompiled* from a shipped `en.dll` by `tools/decompile_dialogs.py`, with
   numeric ids mapped back to `src/resource.h` symbols by inference. The
   `MUSHclient.rc` in this checkout is the real source of those same templates —
   true symbol names, real `DLGINIT`, no decompiler heuristics. Sourcing the
   port from it directly is the long-term goal and would be strictly better
   provenance. It is also a migration across all 116 dialogs, so it wants its
   own diff-by-diff pass with GUI verification rather than being done as a side
   effect. Nothing in the build reads this checkout yet.

The port does **not** need to build `en.dll` or load resources from a library to
get there — it parses `.rc` text directly (`wx_dialog_resource.cpp`), so
"consume the upstream resource project" means "read its `.rc` and its `res/`",
not "reproduce the Windows DLL".

---

## What `build-app.sh` does, step by step

1. **Build** the Release `mushgui` target into `build-release/`. The CMake build
   already drops the vendored Ruby (libruby + stdlib) into `Contents/lib` and the
   Tcl script library into `Contents/Resources/tcl8.6`.
2. **Bundle plugins** (`--with-discworld` only) into `Contents/Resources/plugins/`.
3. **Relocate dependencies**: BFS over the app's Mach-O dependencies; copy every
   non-system `.dylib` (e.g. Homebrew's `openssl@3`) into `Contents/Frameworks/`
   (id → `@rpath/...`), copy linked `.framework`s in whole (e.g. the vendored
   Python framework), rewrite all load commands to `@rpath`, add an
   `@executable_path/../Frameworks` rpath, and strip leftover Homebrew/Xcode
   rpaths. (The Ruby in `Contents/lib` is already `@rpath`/`@executable_path`-
   relative, so the relocator leaves it alone.) Result: `otool -L` shows only
   `/usr/lib`, `/System`, and `@`-relative paths.
4. **Info.plist**: set `LSMinimumSystemVersion`, `NSHighResolutionCapable`.
   (`CFBundleShortVersionString`/`CFBundleVersion` are already set by CMake,
   sourced from `../version.h`'s `VERSION_STRING` — the app's version is
   edited in exactly one place.)
5. **Code sign** inside-out — nested Mach-O first (everything in
   `Contents/Frameworks` *and* `Contents/lib`, i.e. libruby + its `.bundle`
   extensions), then the app — with the hardened runtime and
   [`entitlements.plist`](entitlements.plist). Without `--sign-identity` it ad-hoc
   signs (`-`).
6. **Notarize + staple** (`--notarize-profile` only).
7. **Package**: copy the `.app` to `dist/`, make a `.zip` (for notarization /
   upload) and a `.dmg` (with an `/Applications` symlink for drag-install).

### Entitlements

The hardened runtime is relaxed just enough for the embedded interpreters — JIT
(JavaScriptCore), unsigned executable memory (CPython / libperl / Ruby), and
library validation disabled (so the interpreters can load extension modules and
the app can load the bundled dylibs / framework). See the comments in
[`entitlements.plist`](entitlements.plist).

### Useful flags

`--no-relocate` (fast local build against Homebrew), `--no-dmg`, `--clean`,
`-j N`, `-o DIR`, `-t Debug`, `--discworld-dir DIR`, `--no-prune-python`
(keep tkinter/tests/idle in the bundled Python framework). Run
`packaging/build-app.sh --help` for the full list.
