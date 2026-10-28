# `locale\en.dll`: the Windows client's resources

Every menu, dialog, string table, accelerator, toolbar, bitmap and cursor the
Windows client shows comes from `locale\EN.dll`, which `MUSHclient.cpp`'s
`I18N_Setup` makes MFC's resource handle (the exe itself carries only its
icon and version).  This directory is that DLL's source, and
[`en.vcxproj`](en.vcxproj) builds it -- for Win32, x64 and ARM64 alike -- into
`locale\` beside each configuration's exe (`WinDebug-x64\locale\en.dll`, ...).
`MUSHclient.vcxproj` references the project, so building the client builds it.

It is the macOS and Linux client's source too.  Their build compiles the same
`en.rc` with `llvm-rc` ([`CMakeLists.txt`](CMakeLists.txt) here) and
`tools/port_resources.py` converts the result into the dialog and menu
templates, bitmaps and icons the app bundles; see
[`packaging/README.md`](../packaging/README.md#dialogs-menus-and-art-resourcesenrc).
So a dialog, menu, bitmap or icon is changed here, once, for every platform.

## Where it comes from

[github.com/nickgammon/mushclient_resources](https://github.com/nickgammon/mushclient_resources)
at `70f89f275be84b1c2e413ddcb7409461334c1df5`, its latest commit (2010-02-18),
imported verbatim in this repository's history and then changed as below.
Compiled as imported, 299 of its 307 resources are byte-identical to the
`en.dll` shipped in MUSHclient 5.06 (2016); the other eight are the items
below.  Where 5.06 was newer than the source, this copy has 5.06's version;
where this repository had changed a resource (in the dialogs the macOS/Linux
port used to keep a copy of), this copy has the change too.

## What this copy changes

- **`resource.h` is `src\resource.h`**, the one the client compiles against
  (the project puts `src\` on the include path).  It is a superset of the
  imported one: the additions are IDs the client uses, and the one other
  difference (`ID_MACRO_F1`) is the same 16-bit value spelled signed.
- **Display > Stop Trigger Sounds / Stop Script Sounds**, with status-bar
  prompts.  The client has handled both commands (`doc.cpp`) and the macOS
  and Linux menus have shown them, but this menu never did, so on Windows they
  were reachable only by keyboard shortcut.
- **Three texts are `src\platform\dialog_text\`'s** -- the licence agreement,
  the credits and the time-format help -- which are newer than both the
  imported copies and 5.06's (authorship, copyright years, a stray non-ASCII
  apostrophe).  The other two texts (`text\lua_init.txt`,
  `text\multiple_asterisks.txt`) say exactly what 5.06's do.
- **5.06's splash screen and activity-toolbar bitmap** (`res\Splsh16.bmp`,
  `res\activity_toolbar.bmp`), extracted from its `en.dll`: the 2016
  splash in place of the 2005 one, and the ten-button placeholder strip the
  toolbar loads before the chosen style's bitmap (`IDB_ACTIVITY_TOOLBAR_n`)
  replaces it.
- **The dialog changes this repository made** for the macOS/Linux port, back
  when it kept its own copy of the dialogs:
  - About: the authors (Nick Gammon and Wade Tregaskis) and the copyright
    years.
  - Welcome and upgrade notices: both authors, and "Release notes:".
  - World Properties > Output: the buffer-size range the client accepts
    (0 for unlimited, else 200 to 100,000,000).
  - World Properties > IP address: "Use SSL/TLS for this connection" is in
    the template.  The client used to create it at run time, and still does
    when a dialog lacks it (a translation made from an older `en.dll`).
  - The template of the long-gone `CRecallDlg` (`ID_RECALL_WINDOW`) is
    removed.
- **`AFX_IDB_CHECKLISTBOX_NT` is defined** (30995, VC6's value): today's
  `afxres.h` no longer defines it, which turned that bitmap into a resource
  named by the string instead of the number MFC looks it up by.
- **Portable to `llvm-rc`**, with no change to what `rc.exe` builds (the
  resulting `en.res` is byte-identical):
  - file names use forward slashes and the case of the files on disk
    (`res/drives.bmp`, `res/mushclient.rc2`), which a case-sensitive file
    system needs;
  - the three `TOOLBAR` resources, an MFC statement only `rc.exe` knows, are
    skipped when `MC_RC_NO_TOOLBARS` is defined, as
    [`llvm-rc/afxres.h`](llvm-rc/afxres.h) defines it (the port lays out its
    own toolbars);
  - `resource.h` gives `ID_FILE_CTRL_P` and `ID_EDIT_CTRL_Z` as 32964 and
    32966, not -32572 and -32570: the same 16-bit command ids, which `rc.exe`
    truncated to and `llvm-rc` rejects.
- **Dropped:** the VC6 project (`mc_resources.dsp`/`.dsw`), its 16-bit
  `LibMain`/`.def` scaffolding (a resource-only DLL needs no entry point:
  `/NOENTRY`), and an unreferenced `manifest.xml`.

Every other resource is byte-identical to 5.06's.

## `llvm-rc` and `llvm-rc\afxres.h`

`llvm-rc` has no Windows SDK or MFC headers to read, so
[`llvm-rc\afxres.h`](llvm-rc/afxres.h) stands in for `afxres.h` and what it
includes: the values of the window styles, virtual keys, language ids and MFC
ids `en.rc` uses, and nothing else.  An identifier `en.rc` gains later, which
`src\resource.h` does not define, fails the macOS/Linux build until it is
added there.

[`tests\check_port_resources.py`](../tests/check_port_resources.py) checks that
the two compilers agree.  It compiles `en.rc` with `llvm-rc` as the port does
(Visual Studio's *C++ Clang tools* component has one), converts that and an
`en.res` from `rc.exe`, and fails unless the two give the same files:

    python tests\check_port_resources.py WinRel-x64\en_dll\en.res

## `en_small.dll`

The 32-bit releases also shipped `en_small.dll`, a variant for screens below
1024x768, whose source was never published.  It is not built: when it is
absent the client uses `en.dll` on a small screen too.

## Translations

`locale_notes.txt` (in releases) describes making a translation by copying
and editing `en.dll`.  A translation made that way for the 32-bit client is a
32-bit DLL, which a 64-bit client cannot load;
`packaging\windows\fetch-deps.ps1 -Only locale -LocaleSource <dir>` rehosts
one for a 64-bit architecture (`docs/WINDOWS-64BIT.md`).
