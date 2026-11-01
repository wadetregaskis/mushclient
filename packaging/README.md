# Packaging MUSHclient

The packaging scripts build self-contained, distributable MUSHclient releases for macOS, Linux and 64-bit Windows.

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
- `dist/mushclient-6.00-linux-<arch>.tar.xz`: self-contained relocatable tarball archive
- `dist/mushclient_6.00_<arch>.deb`: standard Debian/Ubuntu package for system installation

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
- llvm-rc and clang, which compile `resources/en.rc` (`apt install llvm clang`):
  see [Dialogs, menus and art](#dialogs-menus-and-art-resourcesenrc)
- Optional scripting runtimes: Python 3, Perl (`libperl-dev`), JavaScriptCore (`libjavascriptcoregtk-4.1-dev`), Tcl (`tcl-dev`), Ruby (`ruby-dev`)
- For `.deb` creation: `dpkg-deb` (standard on Debian/Ubuntu)

---

## Packaging for macOS

The scripts here build a complete, self-contained, signed `MUSHclient.app`:

| Script | What it does |
|--------|--------------|
| [`build-app.sh`](build-app.sh) | Build the whole thing — relocate deps, sign, notarize, DMG. |
| [`sign-release.sh`](sign-release.sh) | Sign + notarize a macOS build made by the GitHub release workflow, on your own Mac (see *Release pipeline*). |
| [`fetch-wx.sh`](fetch-wx.sh) | Build **wxWidgets** from source: universal, static, macOS 11+. `build-app.sh` runs it. |
| [`fetch-python.sh`](fetch-python.sh) | Vendor a clean, redistributable **python.org** framework (Python engine; already universal, macOS 11+). |
| [`fetch-tcl.sh`](fetch-tcl.sh) | Build a static **Tcl 8.6** from source (Tcl engine). |
| [`fetch-openssl.sh`](fetch-openssl.sh) | Build the bundle's one **OpenSSL** (newest 3.5.x LTS release) from source. `build-app.sh` runs it. |
| [`fetch-ruby.sh`](fetch-ruby.sh) | Build a relocatable **Ruby 4.0** from source, + static libyaml, against that OpenSSL (Ruby engine). |
| [`fetch-discworld-plugins.sh`](fetch-discworld-plugins.sh) | Stage Quow's Discworld plugins so they can be pre-bundled. |

Everything the scripts produce lands in `dist/` (the final app) or git-ignored
staging dirs (`packaging/{openssl,wx,python,tcl,ruby,discworld-plugins}/`).

> **Universal, macOS 11+.**  The app runs natively on Apple Silicon and Intel
> Macs, on macOS 11 Big Sur and later.  Nothing in the bundle comes from
> Homebrew, whose libraries are single-architecture and built for the build
> machine's own macOS: wxWidgets, OpenSSL, Tcl and Ruby are built from source by
> the `fetch-*.sh` scripts, libpng (`png/`) and Lua 5.1 are compiled into the
> app, and Python is python.org's universal2 framework.  `build-app.sh` then
> refuses to finish if any binary in the bundle lacks an architecture or needs a
> newer macOS than 11.  `--archs` / `--macos-min` change either (e.g.
> `--archs arm64` for a quicker local build).
>
> Perl and JavaScript are Apple's own.  JavaScriptCore is present on every macOS;
> Perl is weak-linked, because Apple's `libperl` path names its version
> (`/System/Library/Perl/5.34/...`): on a Mac without that version the app still
> runs and Perl scripting reports itself unavailable.

---

## Quick start

```bash
# self-contained, ad-hoc-signed app + DMG in dist/  (runs on THIS Mac)
packaging/build-app.sh
```

The first run builds every dependency from source (wxWidgets, OpenSSL, Tcl,
Ruby twice — once per architecture), which takes a while; after that each
`fetch-*.sh` is a quick up-to-date check.

That builds Release, copies every non-system dylib/framework the app links into
`MUSHclient.app/Contents/Frameworks` (the bundled Ruby goes in `Contents/lib`),
rewrites the load commands so the app no longer needs Homebrew/Xcode installed,
ad-hoc signs it, and rolls a DMG.

A plain CMake build (not `build-app.sh`) still uses Homebrew's wxWidgets and
whatever interpreters it finds — fine for development, **not** for a release.

To make a build you can give to *other* people you must sign + notarize with
your Apple Developer ID (see below).

### Prerequisites

- Xcode command-line tools (`xcode-select --install`)
- Rosetta 2 on an Apple Silicon Mac (`softwareupdate --install-rosetta`): Ruby's
  Intel half is built natively under it, and it runs the Intel half's tests
- Homebrew's LLVM, for `llvm-rc`, which compiles `resources/en.rc`
  (`brew install llvm`): see [Dialogs, menus and art](#dialogs-menus-and-art-resourcesenrc).
  Nothing of it ends up in the package.  Otherwise nothing from Homebrew for a
  package.  A *development* build (plain CMake) uses `brew install wxwidgets
  lua@5.1` and any OpenSSL CMake finds, 3.x or 4.x.
- For notarization: an Apple Developer account + a *Developer ID Application*
  certificate in your login keychain

---

## Packaging for Windows (x64, ARM64)

| Script | What it does |
|--------|--------------|
| [`windows/fetch-deps.ps1`](windows/fetch-deps.ps1) | Build or fetch every third-party piece for one architecture (OpenSSL, Lua, LuaSocket, Python, Tcl, QuickJS, Ruby) and the Dina font.  `locale\en.dll` is built from [`../resources/`](../resources/README.md) with the client. |
| [`windows/build-app.ps1`](windows/build-app.ps1) | Release build, NSIS installer, portable zip, self-containment check, every language's selftest from the package. |

```powershell
# from the repo root, in any PowerShell (5.1 or 7); no Developer prompt needed
powershell -ExecutionPolicy Bypass -File packaging\windows\build-app.ps1 -Arch x64
powershell -ExecutionPolicy Bypass -File packaging\windows\build-app.ps1 -Arch arm64

# the Discworld edition (stage Quow's plugins first, as for the other platforms)
bash packaging/fetch-discworld-plugins.sh
powershell -ExecutionPolicy Bypass -File packaging\windows\build-app.ps1 -Arch x64 -WithDiscworld
```

Output in `dist\`: `MUSHclient-<version>-windows-<arch>[-discworld]-setup.exe`,
the matching `.zip` (unzip anywhere, run `MUSHclient.exe`), and
`SHA256SUMS-windows-<arch>[-discworld].txt`.  The installer is the one list of
what a Windows package holds (`install/mushclient.nsi`); the zip is that
installer's payload, extracted, so the two cannot differ.

Prerequisites: Visual Studio 2022 (or its Build Tools) with *Desktop development
with C++*, the **MFC** component for each target architecture and, for ARM64,
the **ARM64 build tools** (plus *C++ Clang Compiler for Windows*, OpenSSL's ARM64
assembler); **Strawberry Perl** and, for x64, **NASM** for OpenSSL.  NSIS and
7-Zip are used if installed and otherwise fetched in their official portable
forms (pinned); `docs/WINDOWS-64BIT.md` has the details, including a
no-administrator route for Perl and NASM.

The scripting runtimes, as on the other platforms, are the package's own:

| Engine | Source | Where it ends up |
|--------|--------|------------------|
| **Lua 5.1** | lua.org 5.1.5, built as `lua5.1.dll` | beside the exe |
| **Python 3.14** | python.org's NuGet package | `python3XX.dll` beside the exe, stdlib in `python\` |
| **Tcl 8.6** | built static from source | linked into the exe; script library in `tcl8.6\` |
| **Ruby 4.0** | built from source (MSVC) | `<arch>-vcruntime140-ruby400.dll` beside the exe, stdlib in `lib\ruby\` |
| **JavaScript** | QuickJS-ng, built static | linked into the exe (macOS/Linux use JavaScriptCore, which Windows lacks) |
| **VBScript, PerlScript, …** | Windows' ActiveScript, as before | not bundled |

Python and Ruby need the VC++ runtime, so `vcruntime140.dll` (and, on x64,
`vcruntime140_1.dll`) ship beside the exe, from Visual Studio's redistributable.

**Discworld edition.**  Quow's plugins go into `worlds\plugins\` and their map
data into `quow_plugins\maps\` under the install directory (the minimap looks
for `.\quow_plugins\maps\`); `Discworld.mcl` goes into `worlds\` and is never
overwritten by an upgrade, and the per-user `_quowmap_userdata.db` is neither
shipped nor touched by the uninstaller.

---

## Release pipeline (GitHub Actions)

[`.github/workflows/release.yml`](../.github/workflows/release.yml) builds and
tests every platform.  It never runs on a push: start it from the repository's
**Actions** tab → *Release builds* → **Run workflow**, optionally giving a tag
(default `v` + `VERSION_STRING`), whether to also build the Discworld edition,
and whether to create a draft release.  It produces:

| Platform | Package | Tested on |
|----------|---------|-----------|
| macOS | one universal app, macOS 11+ — **unsigned** | Apple Silicon runner (natively, and the Intel half under Rosetta) + an Intel runner |
| Linux x86-64, AArch64 | `.deb` + self-contained `.tar.xz` each | a runner of each architecture, under a virtual X display |
| Windows x64, ARM64 | NSIS installer + portable `.zip` each (`windows/build-app.ps1`) | a runner of each architecture: the unit tests, then every scripting language's selftest run from the package |

Release file names are part of the client's behaviour: `plugins/Version_Check.xml`
finds the file for a copy of the client by its exact name, so renaming one here
means changing it there too.  A release's tag is `v<version>`, as in `v6.00`.

| Platform | File | `<arch>` |
|----------|------|----------|
| Windows, installed | `MUSHclient-<version>-windows-<arch>-setup.exe` | `x64`, `arm64` |
| Windows, portable | `MUSHclient-<version>-windows-<arch>.zip` | `x64`, `arm64` |
| macOS | `MUSHclient-<version>-macOS-universal.dmg` and `.zip` | |
| Linux, tarball | `mushclient-<version>-linux-<arch>.tar.xz` | `x86_64`, `arm64` |
| Linux, Debian | `mushclient_<version>_<arch>.deb` | `amd64`, `arm64` |

The Discworld edition adds `-discworld` after the architecture (Windows, macOS) or
after `mushclient` (Linux).  The version check never offers it: a Discworld copy is
sent to the plain edition's file, or to the release page where there is none.

With *create draft release* on, the packages are attached to a **draft** GitHub
release.  The macOS app is attached unsigned so that your Developer ID key never
goes to GitHub.  To finish it, download `MUSHclient-<version>-macOS-universal-UNSIGNED.zip`
on your Mac and run:

```bash
packaging/sign-release.sh MUSHclient-<version>-macOS-universal-UNSIGNED.zip \
    --sign-identity "Developer ID Application: Your Name (YOURTEAMID)" \
    --notarize-profile mushclient-notary \
    --upload v<version>          # needs the GitHub CLI (gh auth login); else upload by hand
```

It signs the app inside-out, notarizes and staples it, builds a signed DMG,
notarizes and staples that too, and attaches both.  Delete the `-UNSIGNED`
asset from the draft, then publish.  (Notarization setup: step 1 below.)

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

The script also patches the staged `QuowMinimap.xml` so that it can't silently drop
commands, and stops if the plugin isn't the text that was reviewed: see
[`docs/QUOW-HIDDEN-COMMAND-DROP.md`](../docs/QUOW-HIDDEN-COMMAND-DROP.md). A copy staged
before that existed needs `packaging/fetch-discworld-plugins.sh --patch-only` once.

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
overridable, e.g. `RUBY_VERSION=4.0.9 packaging/fetch-ruby.sh`,
`TCL_VERSION=8.6.18 packaging/fetch-tcl.sh`,
`PYTHON_VERSION=3.15.2 packaging/fetch-python.sh` (the last also accepts
`--pkg ~/Downloads/python-3.15.2-macos11.pkg` for a fully offline run).

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
  linked statically into psych. Its `openssl` extension links the OpenSSL
  `fetch-openssl.sh` builds — the *same* one the app and Python use — so the
  bundle ships a **single** shared OpenSSL (next note), not a private Ruby copy. `libruby` lives
  in `Contents/lib` (not `Frameworks`): Ruby's extensions link it as
  `@executable_path/../lib/...` and load-relative derives the stdlib prefix from
  there, so the host and every extension are self-locating.
- **OpenSSL (exactly one copy, built here)** — `fetch-openssl.sh` builds the
  newest final release of the **3.5 LTS** series (supported to April 2030),
  looked up on GitHub each run, so security releases arrive by themselves;
  pre-releases are never used, and the download is checked against the release's
  published SHA-256.  Taking OpenSSL from Homebrew made the package depend on
  each Mac's state (`openssl@3` on one, `openssl@4` on another, plus a cached Ruby
  linked against whichever was current when it was built).  `build-app.sh` links
  the app against it, rebuilds the vendored Ruby when Ruby was built against a
  different OpenSSL *major* (a patch release needs no rebuild: 3.x shares one
  ABI), relocates a single `libssl.3.dylib`+`libcrypto.3.dylib` into
  `Contents/Frameworks`, and repoints the app, Ruby's `openssl` extension and
  Python's `_ssl`/`_hashlib` at it (deleting Python's framework-internal pair).
  It then **fails the build** if the bundle holds any other OpenSSL file, if
  anything references an OpenSSL library other than that pair, or if the pair
  is not the version just built.  `OPENSSLDIR` is `/etc/ssl`, so the default
  certificate store is macOS's own `/etc/ssl/cert.pem` — Homebrew's path, which
  the bundle used to inherit, does not exist on a Mac without Homebrew.

---

## Dialogs, menus and art (`resources/en.rc`)

Every dialog layout, menu, bitmap and icon comes from one source,
[`resources/en.rc`](../resources/README.md): Nick Gammon's
[mushclient_resources](https://github.com/nickgammon/mushclient_resources),
imported into this repository and brought up to date.  Windows compiles it with
`rc.exe` into `locale/en.dll`.  The macOS and Linux builds compile the same file
with **`llvm-rc`**, LLVM's work-alike (`resources/CMakeLists.txt`), and
`tools/port_resources.py` converts the result into what the app bundles: the
dialog and menu templates as `mushclient_dialogs.rc`, which the wx dialog
bridge parses, and every bitmap and icon as `art/bitmap/<id>.bmp` and
`art/icon/<id>.ico`.  A change to a dialog or an icon is made in `en.rc` (or
`resources/res/`), and every platform picks it up on its next build.

`llvm-rc` is a build-time tool only: `brew install llvm` on macOS (CMake finds
Homebrew's keg-only copy itself), `apt install llvm clang` on Debian or Ubuntu
(`llvm-rc` uses clang to preprocess).  Without it, CMake stops and says so;
`-DMC_LLVM_RC=/path/to/llvm-rc` names one elsewhere.
`resources/llvm-rc/afxres.h` stands in for the Windows SDK headers `en.rc`
includes, which the port does not have.

The two compilers are held to the same answer on Windows, where both exist:
`tests/check_port_resources.py` compiles `en.rc` with `llvm-rc` exactly as the
port does and fails unless converting it gives the same files, byte for byte, as
converting `rc.exe`'s own `en.res`.  The release workflow's Windows jobs run it.

The one image the port has that `en.rc` does not is `src/platform/MUSHclient.png`,
a high-resolution stand-in for the app icon (`IDR_MAINFRAME`), from which the
macOS `.icns` is also made.

---

## What `build-app.sh` does, step by step

1. **Build** the Release `mushgui` target into `build-release/`. The CMake build
   already drops the vendored Ruby (libruby + stdlib) into `Contents/lib` and the
   Tcl script library into `Contents/Resources/tcl8.6`.
2. **Bundle plugins**: stage into `Contents/Resources/userdata-seed/` (standard plugins + optional `--with-discworld` maps and triggers) for first-launch user data seeding.
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
   sourced from `src/version.h`'s `VERSION_STRING` — the app's version is
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
