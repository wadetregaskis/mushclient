# 64-bit Windows: x64 and ARM64

The 64-bit Windows builds: how to build, test and package them, where every
binary comes from, and what changed to get there.  The groundwork (project
configurations, `fetch-deps.ps1`, the pointer-truncation pass, the installer's
`ARCH` switch) was prepared on macOS by reading; the first real builds, the
embedded scripting engines and the packaging were done on Windows 11 ARM64
(build 26200), building and testing **both** architectures there -- x64 runs
under emulation (see *Verification* at the end).

Upstream MUSHclient has only ever been built 32-bit.  The bar is unchanged:
existing plugins must behave exactly as they do in the 32-bit Windows client,
except where this document says otherwise.

The Win32 build is still built and tested (`tests\win\run_tests.ps1` with no
`-Platform`) and behaves as before, but it is not packaged by the new scripts.

## 1. What to do, in order

### 1.1 Prerequisites

- **Visual Studio 2022** (v143) or its *Build Tools*, with *Desktop
  development with C++*, plus these individual components (the `--add` ids
  are for `setup.exe modify` / the installer's command line):
  - MSVC v143 x64/x86 build tools (in the workload);
  - **MSVC v143 ARM64 build tools** -- `Microsoft.VisualStudio.Component.VC.Tools.ARM64`;
  - **C++ MFC** for x86/x64 *and* for ARM64 --
    `...VC.ATLMFC`, `...VC.MFC.ARM64` (+ `...VC.ATL.ARM64`).  MFC is
    per-architecture and `UseOfMfc=Static` needs `nafxcw*.lib` for the target;
    a missing one is `LNK1104: cannot open file 'nafxcwd.lib'`;
  - **C++ Clang Compiler for Windows** -- `...VC.Llvm.Clang` and
    `...VC.Llvm.ClangToolset`: OpenSSL's ARM64 assembler.  `fetch-deps.ps1`
    finds VS's own `clang-cl` itself.  Without it ARM64 OpenSSL has no assembler.
- **Strawberry Perl** (OpenSSL's `Configure`) -- the `perl` first on PATH;
  Git-for-Windows' MSYS perl is rejected.
- **NASM** for x64 OpenSSL, or pass `-NoAsm`.

  Neither needs installing: the *portable* zips work on PATH
  (`strawberry-perl-5.42.x-64bit-portable.zip`, `nasm-2.16.03-win64.zip`), which
  is how the first build was done, without administrator rights.
- **NSIS 3.04+** and **7-Zip**: used if installed; otherwise `build-app.ps1` /
  `fetch-deps.ps1` fetch NSIS's portable zip and 7-Zip's MSI (administrative
  extract -- nothing is installed), both SHA-256 pinned, into
  `packaging\windows\deps\_tools\`.  GitHub's Windows runners have 7-Zip.

Nothing else: the 32-bit locale DLLs, `help.db` and the other files git does
not carry come from the 5.06 release's installer (§2).

### 1.2 Everything at once: a package

```powershell
powershell -ExecutionPolicy Bypass -File packaging\windows\build-app.ps1 -Arch x64
powershell -ExecutionPolicy Bypass -File packaging\windows\build-app.ps1 -Arch arm64 -WithDiscworld
#   -Version 6.00     the version in the file names (default: src\version.h)
#   -NoBuild          package the existing WinRel-<plat>\MUSHclient.exe
#   -SkipDeps         do not run fetch-deps.ps1 first
#   -NoSelftest       skip the last step
```

It runs `fetch-deps.ps1`, builds `Release|<Platform>`, builds the installer,
extracts the installer's payload as the portable zip, checks that tree is
self-contained, and runs every scripting language's selftest from it -- see
`packaging/README.md`.  Output in `dist\`.

### 1.3 Dependencies alone

From any PowerShell (5.1 or 7) at the repo root.  The script finds Visual
Studio with `vswhere`, imports `vcvarsall.bat` itself and picks native or
cross tools from the *machine's* architecture (read from the system
environment: an x64 PowerShell under emulation is told "X64" by .NET):

```powershell
powershell -ExecutionPolicy Bypass -File packaging\windows\fetch-deps.ps1 -Arch x64
powershell -ExecutionPolicy Bypass -File packaging\windows\fetch-deps.ps1 -Arch arm64
#   -Only openssl,lua,luasocket,upstream,locale,python,tcl,quickjs,ruby
#   -OpenSslVersion 3.5.9   pin instead of "newest 3.5.x on GitHub"
#   -NoAsm                  no NASM / clang-cl needed (slower crypto)
#   -LocaleSource <dir>     rehost these en.dll / en_small.dll instead of 5.06's
#   -StageTo WinDebug-x64   also copy the runtime files next to a dev build
#   -Force                  rebuild what is already built
```

(`-Only a,b` needs the script run in-process -- `& .\packaging\windows\
fetch-deps.ps1 ...` -- because `powershell -File` passes the list as one string.)

What is built is skipped next time.  Ruby dominates a clean run (its MSVC
build is a serial `nmake`, ~20 minutes per architecture), then OpenSSL.
Output is the git-ignored `packaging\windows\deps\<x64|ARM64>\` plus the
architecture-neutral `deps\upstream\`.

### 1.4 Build and run a development copy

```bat
msbuild MUSHclient.vcxproj /m /p:Configuration=Debug   /p:Platform=x64 /p:SolutionDir=%CD%\
msbuild MUSHclient.vcxproj /m /p:Configuration=Release /p:Platform=ARM64 /p:SolutionDir=%CD%\
msbuild MUSHclient.vcxproj /m /p:Configuration=Debug   /p:Platform=Win32 /p:SolutionDir=%CD%\
```

Outputs: `WinRel-<plat>\`, `WinDebug-<plat>\` (Win32: `WinRel\`, `WinDebug\`).
To run one, put the runtime files beside it -- `fetch-deps.ps1 -Arch x64
-StageTo WinDebug-x64` copies the DLLs, the locale DLLs, `python\`,
`lib\ruby\`, `tcl8.6\` and the VC++ runtime -- plus the architecture-neutral
data files (`lua\`, `spellchecker.lua`, ...), or simply run a packaged copy.

### 1.5 Tests

```powershell
powershell -File tests\win\run_tests.ps1                      # Win32 Debug, as before
powershell -File tests\win\run_tests.ps1 -Platform x64
powershell -File tests\win\run_tests.ps1 -Platform ARM64      # builds anywhere; runs on an ARM64 machine
```

And the scripting-language conformance suite (`src/platform/engines_selftest.cpp`,
the one macOS and Linux run from ctest), from any 64-bit build with its
runtime beside it:

```powershell
MUSHclient.exe --engines-selftest            # every language; or =lua, =python, =js, =tcl, =ruby
```

It runs in `InitInstance` before any window exists, prints to stdout (so
redirect it), closes any dialog it raises, and exits 0 / 1, or 77 for a
language the build does not embed.

## 2. Where every binary comes from

| Item | 64-bit source | Notes |
|---|---|---|
| OpenSSL `libcrypto.lib`, `libssl.lib` | `fetch-deps.ps1`: newest 3.5.x, `VC-WIN64A` / `VC-WIN64-CLANGASM-ARM`, `no-shared`, checked against the release's `.sha256` | static, `/MT /Zl` -- the same recipe as the committed x86 libs (§2.1) |
| `lua5.1.dll` + `.lib` | Lua **5.1.5** from lua.org (SHA-256 pinned), `LUA_BUILD_AS_DLL`, `/MT`; all 123 names the x86 DLL exports are checked | `luaconf.h` patched to round like x86 (§2.2) |
| `socket\core.dll`, `mime\core.dll` | LuaSocket **v2.0.2** (pinned) | the version the committed `lua\socket*.lua` are written for |
| `locale\en.dll`, `en_small.dll` | the 5.06 release's x86 DLLs, **rehosted**: every resource copied byte for byte into a fresh resource-only DLL for the target machine, then compared | §2.3 |
| `help.db`, `MUSHCLIENT.HLP`, `Dina.fon`, `locale\Localize_template.lua` | extracted from the 5.06 installer (`mushclient506.exe`, SHA-256 pinned) by the `upstream` component | none is in git; `help.db`'s source (`documentation.sql`) is not either |
| `python3XX.dll`, `python\Lib`, `python\DLLs` | python.org's NuGet package (`python` / `pythonarm64`, 3.14.x, SHA-256 pinned) | IDLE, Tk, the test suite and pip removed (no `python.exe` ships to run pip) |
| `tcl86.lib` (linked in), `tcl8.6\` | Tcl **8.6.16** from source, `makefile.vc OPTS=static` | |
| `<arch>-vcruntime140-ruby400.dll`, `lib\ruby\` | Ruby **4.0.5** from source, native MSVC build, with libyaml 0.2.5 (static), the repo's zlib, and the OpenSSL above | §2.4 |
| `quickjs.lib` (linked in) | QuickJS-ng **0.17.0** (pinned) | §5 |
| `vcruntime140.dll` (+ `vcruntime140_1.dll` on x64) | Visual Studio's own redistributable (`VC\Redist\MSVC\<ver>\<arch>\Microsoft.VC143.CRT`) | Python and Ruby are `/MD`; the exe itself links the CRT statically |
| zlib, PCRE, libpng, SQLite, lpeg, lsqlite3, LuaCOM, bc, lrexlib | compiled into the exe from the repo | |

### 2.1 Why not vcpkg for OpenSSL

vcpkg's `openssl` port follows OpenSSL's newest release, not the 3.5 LTS, and
pinning a 3.5.x works only for the versions its registry happened to record.
OpenSSL's own `Configure` reproduces the committed x86 libs exactly (their
build string is `platform: VC-WIN32`, `/MT /Zl`), so `VC-WIN64A no-shared` is
the like-for-like 64-bit build, and it follows `packaging/fetch-openssl.sh`.

`fetch-deps.ps1` passes **no `--prefix` / `--openssldir`**, on purpose: both
are compiled into the library, and a build-tree OPENSSLDIR is a directory any
user of the installed client could create (CVE-2019-1552's shape).  MUSHclient
takes its trust roots from the Windows `ROOT` store (`pal/win_transport.cpp`).

`/Zl` is also what lets the same static OpenSSL link into the `/MT` exe and
Ruby's `/MD` `openssl` extension alike.

### 2.2 The 64-bit `lua5.1.dll` is patched in one place

Lua 5.1's `luaconf.h` converts a number to an integer by **rounding** on x86
and by a **truncating** C cast everywhere else.  The shipped x86 DLL rounds, so
a stock 64-bit build would turn `2.7` into `2` instead of `3` -- a miniwindow
coordinate a pixel off.  `fetch-deps.ps1` swaps the generic definitions for
`lrint` / `llrint`, and stops if the text it patches is missing.  Unfixable:
`lua_Integer` is 64-bit in a 64-bit DLL, so values past 2^31 survive where x86
wrapped them.

### 2.3 The locale DLLs

`AfxSetResourceHandle` makes `locale\EN.dll` the source of every menu, dialog,
string table and accelerator -- the exe itself carries only its icon and
version (15 resources against `en.dll`'s 307).  The full resource script is
not in this repository (`src/platform/mushclient_dialogs.rc` is a decompile of
the dialogs and menus only, for the wx port), and `en_small.dll` has no source
anywhere, so both are rehosted from the last release's x86 DLLs: resources are
architecture-neutral data, only the PE wrapper is not, and a byte-for-byte copy
is the one method that guarantees the 64-bit client shows exactly what the
32-bit one does.  The rehosted DLLs from the 5.06 installer and from an
installed 5.06 are identical (same SHA-256 before rehosting).

User-made translations (a copied and edited `en.dll`, as `locale_notes.txt`
suggests) are 32-bit and do not load in a 64-bit client; the rehost tool in
`fetch-deps.ps1` converts one in seconds (`-LocaleSource <dir> -Only locale`).

### 2.4 Ruby on MSVC: the details that mattered

- **No host Ruby.**  The tarball carries every generated file except
  `ext/ripper/ripper.c`; ripper (irb's and rdoc's parser library) is left out
  rather than paying for a two-stage bootstrap.
- `--with-opt-dir` must be given with forward slashes: mkmf passes it through a
  Ruby string, and `C:\Users\...` arrived as `-IC:Users...`, so zlib and psych
  were silently skipped.  `fetch-deps.ps1` now also fails if any of
  `zlib.so`, `psych.so`, `openssl.so` is missing.
- Ruby's `openssl` extension links only crypt32, ws2_32 and user32; a static
  OpenSSL 3 also needs advapi32, which is folded into Ruby's copy of
  `libcrypto.lib` (`lib.exe` merges an import library like any other).
- **The import library MUSHclient links is not Ruby's own.**  The DLL also
  exports ~150 POSIX, CRT and Winsock names as aliases of its `rb_w32_*`
  wrappers (`accept`, `connect`, `close`, `fclose`, `getenv`, `Sleep`, ...);
  linked as-is, the client's own calls to those could bind to Ruby's versions
  (the link noticed only `getenv` and `strerror`, as duplicates).
  `fetch-deps.ps1` cuts Ruby's generated `.def` to Ruby's own namespaces
  (`rb_`, `ruby_`, `st_`, `onig`, ...) and makes the import library from that.
- `NoDefaultCurrentDirectoryInExePath` (set by some shells and agents) breaks
  both Ruby's and Tcl's builds, which run generated `.bat` files and freshly
  built helpers by bare name; the script clears it for its own process.

## 3. Project configuration

`MUSHclient.vcxproj` and `MUSHclient_2022.sln` have `Debug|x64`,
`Release|x64`, `Debug|ARM64`, `Release|ARM64` beside Win32.  What differs from
the Win32 twins:

- `OutDir` / `IntDir`: `WinDebug-<plat>` / `WinRel-<plat>`.
- MIDL `TargetEnvironment` and link `TargetMachine` per platform.
- OpenSSL and Lua from `packaging\windows\deps\<plat>\` *instead of*
  `openssl\` and `src\` (so the 32-bit `lua5.1.lib` can never be found).
- Dropped as x86-only: `/Oy`, `/SAFESEH:NO`; ARM64 Debug uses `/Zi`.
- **C4311 is an error**: a pointer truncated to 32 bits is a crash on 64-bit.
- ARM64: `PNG_ARM_NEON_OPT=0` (libpng's NEON filters are in files the project
  does not compile).
- **`MUSHclient.engines.props`** (64-bit only): the embedded-engine defines
  (`MUSHCLIENT_EMBEDDED_ENGINES` and one per language), the engine libraries,
  and the interpreter include directories -- given only to the engine sources,
  because Python's include directory alone holds a hundred-odd generic header
  names.  The test project imports the same file, since the defines change
  `CScriptEngine`'s layout.

**C++17 everywhere.**  The app is now `stdcpp17` in all six configurations
(it was MSVC's C++14 default), like the test target and the CMake build.  The
only casualty was `array_less` in `stdafx.h`, which derived from
`std::binary_function`; nothing used the typedefs it supplied, so the base is
gone (its `__cplusplus` guard never fired: MSVC reports 199711 without
`/Zc:__cplusplus`).  No `std::byte` clash appeared.

`MUSHclient_tests.vcxproj` is generated (`tests/win/gen_test_vcxproj.ps1`,
pinned by `tests/check_win_test_sources.py`); regenerate it after adding a
source file to the app project.

## 4. Code changes for 64-bit

The macOS-side reading pass (casts through `long`, `OnTimer(UINT_PTR)`, the
manifest's `processorArchitecture`, the crash handler's registers, ...) is in
git history (`3b16c4bc`).  The first real compile found the rest:

| Where | What |
|---|---|
| `src/scripting/lua_compress.c` `getdirectory` | **`utils.readdir` crashed**: `_findfirsti64`'s `intptr_t` handle was kept in an `int`.  A C file, so C4311 never saw it; the test suite's `LuaSurface` crashed on it. |
| `ActivityView.cpp`, `mainfrm.cpp`, `mushview.cpp`, `sendvw.cpp` | `AppendMenu (MF_POPUP, (UINT) hMenu)` -> `UINT_PTR` |
| `dialogs/world_prefs/prefspropertypages.cpp`, `genpropertypage.cpp` | `POSITION` <-> `long` index casts made explicit through `INT_PTR` |
| `world_debug.cpp` | a loop variable whose declaration had gone (the sound-buffer count) |
| `lsqlite/lsqlite3.c` | `#include "..\lua.h"` (a path from the old tree layout) -> `"lua.h"` |

What remains at `/W3` is C4267 (`size_t` -> `int`, ~530, the expected
`strlen`/`size()` into `int` kind) and nothing else.

Known, unchanged: a `long` is 32 bits on all Windows builds, so world-API
values are 32-bit as they always were.

## 5. Scripting on 64-bit Windows

The 32-bit client runs every language but Lua through ActiveScript, and the
ActiveScript engines for Python, Tcl and Ruby were only ever 32-bit -- a
64-bit client cannot load them.  So the 64-bit builds embed those languages
the way the macOS and Linux builds do: the same `src/platform/{py,tcl,ruby}_engine.cpp`,
over the same portable dispatch table (`src/platform/mc_dispatch.cpp`), so a
script sees the same world API, coercions and callbacks on all three
platforms.  JavaScript is QuickJS-ng (`src/platform/qjs_engine.cpp`), a new
adapter with `js_engine.cpp`'s contract point for point, because
JavaScriptCore does not exist on Windows.

| Language name | 64-bit Windows | Win32 |
|---|---|---|
| Lua | native (unchanged) | native |
| Python | embedded CPython 3.14 | ActiveScript (third-party engine) |
| JScript / JavaScript / js | embedded QuickJS-ng (ES2023) | ActiveScript JScript (ES3) |
| TclScript / Tcl | embedded Tcl 8.6 | ActiveScript (third-party engine) |
| RubyScript / Ruby | embedded Ruby 4.0 | ActiveScript (third-party engine) |
| VBScript, PerlScript, anything else | ActiveScript (64-bit engines only) | ActiveScript |

How it is wired: `scripting.h` defines `MC_EMBEDDED_ENGINES` everywhere but
Win32; `CreateScriptEngine` sends the four language names to the embedded
engines before trying ActiveScript; every trigger/alias/timer/plugin call site
already gated on `UsesEmbeddedCallback()`, so none changed.

Behaviour a plugin author may notice, against the 32-bit client:

- **JScript is now modern JavaScript.**  ES3 code runs as before.  JScript's
  two COM adapters exist as thin shims -- `new VBArray(world.GetTriggerList()).toArray()`
  and `Enumerator` work -- over the plain arrays this engine returns.
  `ActiveXObject` does not exist.  Bare method references coerce as in COM
  (`"x" + world.GetPluginName` calls it).
- **World methods are not global names** in any embedded language (ActiveScript
  added them with `SCRIPTITEM_GLOBALMEMBERS`, so `Note "x"` worked in
  VBScript); write `world.Note`.  VBScript itself is unchanged.
- **Text is the world's bytes**, decoded as UTF-8 with undecodable bytes
  carried as U+DC80..U+DCFF ("surrogateescape") so they round-trip -- the
  macOS/Linux rule.  In a non-UTF-8 (e.g. Latin-1) world, a Python `'é'`
  written by a script is sent as UTF-8.
- An integer beyond 32 bits passed to a string parameter arrives as its exact
  digits (Python, Ruby), not as a rounded double.
- Python finds a per-user `site-packages` (install the matching python.org
  Python and `pip install --user`); the package has no `python.exe`.

Windows-specific engine fixes, each found by `--engines-selftest`:

- BSTRs are UTF-16 on Windows, `char*` elsewhere: the engines read and make
  them only through `MC_BstrBytes` / `MC_BstrFromBytes` (`mc_dispatch.h`),
  which convert with the thread code page exactly as MFC does, so the
  document's bytes survive.  `mc_dispatch.cpp` has explicit `BSTR` and
  `IDispatch*` overloads (both would otherwise convert silently to the
  `void*` one), and drops the COM reference `GetWorld` takes, so the engines
  hold the bare document pointer they hold elsewhere, checked on every use.
- Ruby: `ruby_sysinit` on Windows re-parses the **process** command line, so
  `ruby_options` was bootstrapping from MUSHclient's own arguments (a world
  file to run as a script, or `--engines-selftest`); the engine puts its own
  `-e0` back.  The GC stack base is the last word inside the thread's stack
  (`StackBase - 1`), not `StackBase`, which belongs to the next region.
- Python: a Debug build includes `Python.h` without `_DEBUG` (else
  `Py_DEBUG` and `python3XX_d.lib`); `PyConfig.home` is `python\` beside the DLL.
- Tcl: `STATIC_BUILD`; `TCL_LIBRARY` is set in the process environment block
  (Tcl reads it with `GetEnvironmentVariableW`) before `Tcl_FindExecutable`.

## 6. The installer

`install/mushclient.nsi` with `/DARCH=x64|arm64` (plain `makensis
mushclient.nsi` is the 32-bit installer exactly as before).  For 64-bit:

- the exe from `..\WinRel-<plat>\`; Lua, LuaSocket, locale, Python, Ruby and
  Tcl from `..\packaging\windows\deps\<plat>\`; `help.db`, the WinHelp file,
  `Dina.fon` and `Localize_template.lua` from `deps\upstream\`; the type
  library from the build (`MUSHclient.tlb`, MIDL'd from `src\mushclient.odl`);
  the VC++ runtime from `/DCRTDIR` (build-app passes VS's redist directory);
- the runtime trees (`python\`, `lib\ruby\`, `tcl8.6\`) are replaced, not
  merged, on an upgrade, and removed by the uninstaller;
- no `tools\localize.bat` (it regenerates `help.db` from `documentation.sql`,
  which is not in the repository, and needs `lua5.1.exe`, `sqlite3`, `xgettext`);
- `/DVERSION=` names the output `MUSHclient-<version>-windows-<arch>[-discworld]-setup.exe`;
- `/DDISCWORLD=<dir>`: the Discworld edition (`packaging/README.md`);
- default directory `$PROGRAMFILES64\MUSHclient`; `.onInit` refuses 32-bit
  Windows (x64) or non-ARM64 Windows (ARM64); `SetRegView 64`.

Deliberately unchanged: `InstallDirRegKey` reads the per-user install
directory, which 32-bit and 64-bit programs share, so on a machine with a
32-bit install the 64-bit installer proposes *that* directory -- which keeps
every world, plugin and log path in the user's preferences valid.

## 7. What a 64-bit client cannot do that the 32-bit one can

- **32-bit in-process COM servers and ActiveScript engines.**  VBScript exists
  in 64-bit; PerlScript, PHP's and most third-party engines do not.  A world in
  such a language fails to start its engine.  (Python, JavaScript, Tcl and Ruby
  are embedded, §5.)  The same applies to LuaCOM's `CreateObject` on a
  32-bit-only in-process server.
- **32-bit Lua C modules** a user added (`require` of a DLL built against the
  32-bit `lua5.1.dll`).  The bundled `socket` and `mime` cores are rebuilt.
- **32-bit translations** (§2.3).
- WinHelp (`MUSHclient.hlp`) is unavailable on modern Windows regardless.

## 8. The release workflow's Windows job

`.github/workflows/release.yml`, `windows` job, on `windows-2025` (x64) and
`windows-11-arm` (ARM64): Strawberry Perl and NASM if the runner lacks them;
`fetch-deps.ps1` (every component, cached on the script's hash); the unit
tests; `build-app.ps1 -SkipDeps`, which builds Release, the installer and the
zip and runs every language's selftest from the package; with *discworld*, the
Discworld edition from the same build.  It uploads both installers, both zips
and the checksum files.  It has not run on GitHub yet; the same steps run
locally (*Verification*).

## Verification (Windows 11 ARM64, 2026-10-27)

All on one ARM64 machine with VS 2022 Build Tools 14.44, x64 under emulation:

- `fetch-deps.ps1`, every component, both architectures, from clean.
- Debug and Release builds of x64 and ARM64, and Debug Win32, as C++17.
- `tests\win\run_tests.ps1`: **x64 1694/1694, ARM64 1694/1694** (Win32's
  baseline on the same commit: 1689 passed, 5 failed -- tests written where
  `long` is 64-bit, fixed in the tests).
- `--engines-selftest`, x64 and ARM64 Debug: Lua 94, Python 104, JavaScript 96,
  Tcl 80, Ruby 96 passed, 0 failed (skips are per-language, as on macOS).
- `build-app.ps1 -Arch x64`: the self-containment check passed and the selftest
  run from the extracted package passed (470/470).
