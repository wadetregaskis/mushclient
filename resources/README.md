# `locale\en.dll`: the Windows client's resources

Every menu, dialog, string table, accelerator, toolbar, bitmap and cursor the
Windows client shows comes from `locale\EN.dll`, which `MUSHclient.cpp`'s
`I18N_Setup` makes MFC's resource handle (the exe itself carries only its
icon and version).  This directory is that DLL's source, and
[`en.vcxproj`](en.vcxproj) builds it -- for Win32, x64 and ARM64 alike -- into
`locale\` beside each configuration's exe (`WinDebug-x64\locale\en.dll`, ...).
`MUSHclient.vcxproj` references the project, so building the client builds it.

## Where it comes from

[github.com/nickgammon/mushclient_resources](https://github.com/nickgammon/mushclient_resources)
at `70f89f275be84b1c2e413ddcb7409461334c1df5`, its latest commit (2010-02-18),
imported verbatim in this repository's history and then changed as below.
Compiled as imported, 299 of its 307 resources are byte-identical to the
`en.dll` shipped in MUSHclient 5.06 (2016); the other eight are the items
below.

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
- **`AFX_IDB_CHECKLISTBOX_NT` is defined** (30995, VC6's value): today's
  `afxres.h` no longer defines it, which turned that bitmap into a resource
  named by the string instead of the number MFC looks it up by.
- **Dropped:** the VC6 project (`mc_resources.dsp`/`.dsw`), its 16-bit
  `LibMain`/`.def` scaffolding (a resource-only DLL needs no entry point:
  `/NOENTRY`), and an unreferenced `manifest.xml`.

Two of 5.06's bitmaps are newer than these and were *not* taken from it: the
splash screen (5.06's is a high-colour image) and the activity-window
toolbar.  Both here are the originals from the imported source.

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
