# Campaign C endgame — flip inventory (historical record)

> **Historical record:** the pre-flip inventory that drove the Campaign C endgame; the flip has since landed (`b5dbd00` compiles the real `scripting/lua_methods.cpp` into mushcore and deletes the twin). Kept for reference, not maintained.

Goal: compile the real `scripting/lua_methods.cpp` into **mushcore** and delete
`scripting/lua_world_api_portable.cpp` (248 KB) plus the twin registration /
constants blocks in `scripting/lua_utils_portable.cpp`.

Prepared 2026-07-18. Method: a **compile probe** — `lua_methods.cpp` compiled
`-fsyntax-only -ferror-limit=0` against `platform/doc_stub.h` with empty shims
for its GUI headers — plus `nm` symbol analysis of `build/libmushcore.a` and a
registration-table diff. 147 probe errors total; every one is classified below.
The three prep blockers (ErrorDesc table, GetGlobalOption pair, rich CallPlugin)
are **already cleared** in the working tree (see "Prep already landed" at the end).

---

## 1. CMUSHclientDoc methods lua_methods.cpp needs that doc_stub.h lacks

**116 methods**, falling into exactly **five mushapp-only method TUs** (none of
these TUs is compiled into mushcore; no definition of any of these methods
exists in any mushcore TU today — all need either inert stub definitions or TU
adoption). Per family:

### (a) miniwindow family — 49 methods — `scripting/methods/methods_miniwindows.cpp` (1469 lines)
BlendPixel, FilterPixel, WindowAddHotspot, WindowArc, WindowBezier,
WindowBlendImage, WindowCircleOp, WindowCreate, WindowCreateImage,
WindowDelete, WindowDeleteAllHotspots, WindowDeleteHotspot, WindowDragHandler,
WindowDrawImage, WindowDrawImageAlpha, WindowFilter, WindowFont,
WindowFontInfo, WindowFontList, WindowGetImageAlpha, WindowGetPixel,
WindowGradient, WindowHotspotInfo, WindowHotspotList, WindowHotspotTooltip,
WindowImageFromWindow, WindowImageInfo, WindowImageList, WindowImageOp,
WindowInfo, WindowLine, WindowList, WindowLoadImage, WindowLoadImageMemory,
WindowMenu, WindowMergeImageAlpha, WindowMoveHotspot, WindowPolygon,
WindowPosition, WindowRectOp, WindowResize, WindowScrollwheelHandler,
WindowSetPixel, WindowSetZOrder, WindowShow, WindowText, WindowTextWidth,
WindowTransformImage, WindowWrite.

- Definitions in mushcore: **none** (the TU drives the CMiniWindow engine,
  miniwindow.cpp — wx/GDI, mushapp-only).
- Target headless semantics (= current portable twins):
  - value-returning ops → `eNoSuchWindow` (portable `L_WindowStub`)
  - list queries (WindowList, WindowFontList, WindowHotspotList,
    WindowImageList) → **empty table**; as doc methods that is an **empty
    safearray VARIANT** (`COleSafeArray sa; sa.CreateOneDim(VT_VARIANT, 0);
    sa.Detach()`), because the real `L_WindowList` etc. push the VARIANT
    through `pushVariant` (portable `L_WindowListStub`)
  - info queries (WindowInfo, WindowFontInfo, WindowHotspotInfo,
    WindowImageInfo) → **VT_NULL / empty VARIANT** → nil (portable
    `L_GuiNilStub`)

### (b) notepad family — 14 methods — `scripting/methods/methods_notepad.cpp` (594 lines, already 8 `_WIN32` guards)
AppendToNotepad, CloseNotepad, FindNotepad, GetNotepadLength, GetNotepadList,
GetNotepadText, MoveNotepadWindow, NotepadColour, NotepadFont, NotepadReadOnly,
NotepadSaveMethod, ReplaceNotepad, SaveNotepad, SendToNotepad.

- Definitions in mushcore: **none**.
- Target headless semantics: the portable twins are **not** pure stubs — they
  route through the `g_wxhook_notepad_*` / `g_wxhook_save_notepad` hook table
  (`platform/wx_mfc_hooks_storage.cpp`, which mushcore already compiles), and
  return `eNoSuchWindow` when the hook is null (always, headless).  The stub
  doc-method bodies can therefore be hook-based (identical behaviour headless
  AND future-proof), or plain `eNoSuchWindow` / FALSE / empty stubs.
  `FindNotepad` → NULL (that alone short-circuits the CTextDocument path in
  `L_GetNotepadWindowPosition`). `NotepadSaveMethod` → eOK (portable
  `L_GuiOkStub`). `GetNotepadList` → empty safearray; `GetNotepadText` → empty
  BSTR; `GetNotepadLength` → 0.

### (c) chat family — 25 methods — `scripting/methods/methods_chat.cpp` (1257 lines, zero port guards)
ChatAcceptCalls, ChatCall, ChatCallzChat, ChatDisconnect, ChatDisconnectAll,
ChatEverybody, ChatGetID, ChatGroup, ChatID, ChatMessage, ChatNameChange,
ChatNote, ChatPasteEverybody, ChatPasteText, ChatPeekConnections, ChatPersonal,
ChatPing, ChatRequestConnections, ChatSendFile, ChatStopAcceptingCalls,
ChatStopFileTransfer, GetChatInfo, GetChatList, GetChatOption, SetChatOption.

- Definitions in mushcore: **none** (CChatSocket lives in chatsock.cpp,
  mushapp-only; windows_stubs.cpp only carries the class statics).
- Target headless semantics (= portable twins): error-code ops →
  `eNoChatConnections` (`L_ChatStub`); data queries GetChatInfo / GetChatList /
  GetChatOption → **no value** portable-side (`L_ChatDataStub` returns 0
  results); as doc methods that is an empty/VT_NULL VARIANT → nil.
  SetChatOption → `eNoChatConnections`.

### (d) output/view family — 25 methods — `scripting/methods/methods_output.cpp` (868 lines, 6 `_WIN32` guards + 4 port-seam refs already)
ActivateClient, AddFont, Bookmark, DeleteLines, DeleteOutput, FlashIcon,
GetFrame, GetRecentLines, MoveMainWindow, MoveWorldWindowX, Pause, Repaint,
ResetStatusTime, SetBackgroundColour, SetBackgroundImage, SetCursor,
SetForegroundImage, SetMainTitle, SetOutputFont, SetScroll, SetToolBarPosition,
SetUnseenLines, SetWorldWindowStatus, TextRectangle, Transparency.

- Definitions in mushcore: **none** — BUT this TU is the most port-adapted of
  the five (doc_portable.cpp already carries a marked COPY of its non-Windows
  `GetWorldWindowPositionX` arm), so it is the best **adoption** candidate
  post-flip.
- Target headless semantics (= portable twins, mostly `L_GuiOkStub` /
  `L_GuiNilStub`): "do GUI thing" ops → **eOK silently** (Bookmark,
  MoveMainWindow, Pause, Repaint, SetCursor, SetScroll, TextRectangle,
  Transparency, SetBackground/ForegroundImage, SetMainTitle, SetOutputFont,
  FlashIcon, ResetStatusTime, ActivateClient, SetToolBarPosition,
  SetUnseenLines, SetWorldWindowStatus, MoveWorldWindowX, DeleteOutput,
  DeleteLines...); GetRecentLines → empty BSTR; GetFrame → a HWND-ish null;
  AddFont → `eCannotAddFont` or eOK (check the portable table entry when
  writing the stubs — the portable file registers most of these through
  `L_GuiOkStub`).

### (e) accelerator family — 3 methods — `scripting/methods/methods_accelerators.cpp` (223 lines)
Accelerator, AcceleratorTo, AcceleratorList.

- Definitions in mushcore: **only the tables** (`accelerators.cpp`
  CommandIDs[]/VirtualKeys is already compiled into mushcore) — the three doc
  methods themselves are not. This small TU is likely **directly adoptable**
  (it mostly manipulates `m_AcceleratorToCommandMap` doc state + the tables);
  check its view-touching tail. Otherwise: Accelerator/AcceleratorTo → eOK,
  AcceleratorList → empty safearray.

### (f) anything else (non-method gaps found by the probe)

| Gap | Fix at flip |
|---|---|
| `CMUSHclientDoc::m_bRemoveMapReverses` (doc.h:2802, BOOL) | add member to doc_stub.h (property-style direct access from L_RemoveMapReverses) |
| `CDocument::SetPathName(LPCTSTR, BOOL)` — stub takes 1 arg | widen the stub signature (`BOOL bAddToMRU = TRUE`, ignored) |
| `CMUSHclientApp::ActivateNotepad(LPCTSTR)` (MUSHclient.h:285) | stub → FALSE |
| `CMUSHclientApp::AppendToTheNotepad(CString, CString, bool, int)` (MUSHclient.h:279) | stub → false (used by our_lua_debug print path) |
| `CMUSHclientApp::m_strLastDebugCommand` (MUSHclient.h:212, CString) | add member |
| `CTextDocument` type (TextDocument.h) | minimal file-local stand-in in the headless arm (needs only `std::function`-style `m_fn_get_rect` used by the port fork; FindNotepad → NULL makes it unreachable) — same pattern as methods_info.cpp's CWindowPlacement stand-in |
| `CDebugLuaDlg` (dialogs/DebugLuaDlg.h) at `our_lua_debug_function` (lua_methods.cpp:7895) | headless arm: return an error ("no debugger") or inert dialog stand-in; on mushapp the real dialog bridges — keep that path |
| `flags_pair` typedef (doc.h:474) | add to doc_stub.h next to the existing `int_flags_pair` |
| `MINIWINDOW_DRAW_UNDERNEATH/ABSOLUTE_LOCATION/TRANSPARENT/IGNORE_MOUSE/KEEP_HOTSPOTS` (miniwindow.h:79-83) | 5 `#define`s in doc_stub.h (miniwin constants table) |
| `init_by_array` decl (stdafx.h:875; body in scripting/mt19937ar.c — already IN mushcore) | add the extern "C" decl to doc_stub.h |
| `LUA_DBLIBNAME/IOLIBNAME/OSLIBNAME/LOADLIBNAME` (lualib.h; stdafx.h:149-157 includes it) | the headless include head must `#include "lualib.h"` |

## 2. Free functions / globals / macros

### lua_methods.cpp brings its OWN definitions (nothing to do)
Non-static: `isudata`, `my_argerror`, `my_typerror`, `my_checklstring`
(→`my_checkstring` macro), `my_optlstring`, `my_checknumber`, `my_optnumber`,
`get_option_value`, `concatArgs`, `optboolean`, `pushBstr`, `pushVariant`,
`world2string`, `GetFromStack`, `DisableDLLs`, `RegisterLuaRoutines` — plus all
the static L_* and the static `MakeFlagsTable`/`MakeIntFlagsTable`, `l_print`,
`luaWindowPositionHelper`, flag tables. None of the `my_*`/push*/`concatArgs`
have any GUI dependency — all headless-viable as-is.

### Link collisions with TUs that STAY in mushcore (must carve out at flip)
`nm build/libmushcore.a` today:

| Symbol | Currently defined in | Action |
|---|---|---|
| `RegisterLuaRoutines` | `scripting/lua_utils_portable.cpp:860` | delete the portable body **and its whole constants-tables block** (trigger/alias/timer/custom_colour/error_code/error_desc/sendto/miniwin/colour_names/extended_colours mirrors, ~lines 814-1150) — lua_methods.cpp provides all of them |
| `DisableDLLs` | `scripting/lua_utils_portable.cpp` | delete the portable copy; **diff the two bodies first** (the portable one honours `App.m_bEnablePackageLibrary` the same way? verify before deleting) — called from lua_scripting.cpp |

No other collisions: `FindLuaFunction`, `MakeTableItem*`, `MakeLuaState`,
`GetNestedFunction` live only in lua_utils_portable.cpp and lua_methods.cpp
does NOT define them (on Windows they come from lua_utils.cpp) — they resolve
cleanly. `optboolean`/`pushVariant`/`GetFromStack` exist nowhere in mushcore
today — lua_methods.cpp brings them.

### pushVariant vs push_option_variant
No collision at any point: the portable `push_option_variant`
(lua_world_api_portable.cpp:2315) is `static`, and its file is deleted at the
flip. `pushVariant` (lua_methods.cpp:102, non-static) becomes the one
implementation. They are behaviourally equivalent (the portable one was written
to mirror pushVariant, including the VT_ARRAY|VT_VARIANT walk and the port
VT_DATE raw-Unix-seconds convention). Nothing outside these two files
references either name (the engines use mc_dispatch's own conversion). No
rename needed.

### Referenced-but-missing headless (must add)
| Symbol | Windows home | Fix |
|---|---|---|
| `MakeTableItem(lua_State*, const char*, COleDateTime)` | scripting/lua_utils.cpp | **add the overload to lua_utils_portable.cpp** (declared in lua_helpers.h; used at lua_methods.cpp:2990 GetLineInfo "timestr") — else link error |
| `ptr_xmllib` (`const struct luaL_Reg*`) | scripting/lua_utils.cpp:2247 (= its "utils" grab-bag list: xmlread, directorypicker, filepicker, ...) | export `ptr_xmllib` from lua_utils_portable.cpp pointing at its own full `utils_lib` table. Elegant side-effect: `RegisterLuaRoutines`'s final `luaL_register(L, "utils", ptr_xmllib)` then registers the ENTIRE portable utils library — replacing the deleted `RegisterUtilsLib` call with zero lua_methods.cpp edits |
| `xterm_256_colours[256]` | utilities_shared.cpp | already in mushcore ✓ |
| `CallLuaWithTraceBack`, `LuaError` | lua_scripting.cpp | already in mushcore ✓ |
| `GetNestedFunction` | lua_utils_portable.cpp | already in mushcore ✓ |
| `TFormat`/`CFormat`/`Translate` | utilities/doc_portable/format | already in mushcore ✓ |

## 3. Registration structure

### How each side registers
- **Real** (`lua_methods.cpp::RegisterLuaRoutines`, ~line 7758): registers
  `worldlib` (434 entries, one flat luaL_reg list) as the `world` library;
  builds the `_G` metatable (`__index = world`, so bare `Note()` works); sets
  `print` → `l_print`; creates the `mushclient.world` named metatable
  (worldlib_meta `__tostring` + `__index = world` for `w:Note()`); then
  MakeFlagsTable: `trigger_flag, alias_flag, timer_flag, custom_colour,
  error_code, sendto, miniwin`, MakeIntFlagsTable `error_desc`,
  `colour_names` (App.m_ColoursMap walk), `extended_colours`
  (xterm_256_colours), and finally `luaL_register(L, "utils", ptr_xmllib)`.
  It does **not** call `luaL_openlibs` (lua_scripting.cpp::OpenLuaDelayed does,
  before calling RegisterLuaRoutines — and the tests' `make_lua()` fixtures do
  the same ✓).
- **Portable**: `RegisterLuaRoutines` lives in **lua_utils_portable.cpp:860**
  (calls `luaL_openlibs` again — harmless duplicate), then `RegisterWorldLib`
  (lua_world_api_portable.cpp:6423 — the 455-entry table + the same _G/named
  metatable dance), `RegisterUtilsLib` (static, lua_utils_portable.cpp:808),
  `print` override, then hand-rolled copies of every constants table.
  The portable table double-registers `GetVariableList` (twice; benign).

### Portable-only EXTRAS (20) — recommendation per extra
Registration diff: real 434 unique names, portable 454; **real-only missing:
0** (the portable surface is a superset). The 20 extras:

| Extra | Windows world API? | Used by tests? | Recommendation |
|---|---|---|---|
| `MD5`, `SHA256` | no (utils.md5 / utils.sha256 exist — from lua_compress.c, in mushcore; the test fixture must add a `luaopen_compress` call, it only does openlibs+RegisterLuaRoutines today) | **test_lua_scripting.cpp** (MD5/SHA256 TESTs) | keep via portable-extras appendix OR migrate tests to `utils.md5/utils.sha256` — **recommend migrating the two tests** (cleaner: no world-API superset) |
| `ExpandVariables` | no world function (only a trigger/alias flag name) | **test_lua_scripting.cpp:1127** | port-only convenience; **keep in a small portable-extras appendix** (it exercises real doc logic) or rewrite the test around a trigger; keeping is cheaper |
| `AddToCommandHistory` | real doc method, not registered on Windows | no | drop (dispatch surface still reachable) — or keep; **drop** |
| `AtTime` | real doc method (AddTimer-style), not in Windows Lua reg | no | **drop** (Windows parity: not a Lua name) |
| `GetVariableNameList` | not on Windows (GetVariableList exists both sides) | no | **drop** |
| `GetWorldName` | not a Windows Lua name (WorldName property analog) | no | **drop** |
| `GetWorldWindowPositionX`, `MoveWorldWindowX` | real doc methods; Windows registers the un-X names | no | **drop** (un-X names are registered by lua_methods) |
| `IsPlugin`, `SetPluginInfo`, `Trace`, `NormalColour`, `NoteColourFore`, `NoteColourBack` | Windows exposes these as **properties** via dispatch, not Lua functions | no | **drop** for Lua parity (Lua worlds/plugins on Windows never had them as functions) |
| `ClearPane`, `ClosePane`, `CreatePane`, `PaneLine`, `PaneText` | pane legacy API — **not registered** in lua_methods.cpp (methods_panes.cpp is dispatch-only) | no | **drop** (Windows Lua parity: absent) |

If any extras are kept: a tiny `RegisterPortableLuaExtras(lua_State*)` in a
port-owned TU, called after `RegisterLuaRoutines` from lua_scripting.cpp under
`#ifndef _WIN32` — do NOT edit the lua_methods.cpp tables.
**Net recommendation: keep nothing except (optionally) ExpandVariables;
migrate the MD5/SHA256 tests to utils.*; delete the rest** — they are
undocumented port inventions and nothing uses them.

## 4. lua_methods.cpp file facts

- **CRLF line endings** (the whole file; python nl-aware edits only, per
  campaign rules). 272 KB / ~7,940 lines. The already-landed error_descriptions
  relocation demonstrates the working edit pattern.
- Include head (lines 1-10): `stdafx.h`, `../mainfrm.h`, `../MUSHclient.h`,
  `../doc.h`, `../scripting/errors.h`, `../winplace.h`, `../TextDocument.h`,
  `../TextView.h`, `../sendvw.h`, `../dialogs/DebugLuaDlg.h`.
  Flip pattern = the standard adopted-TU head-switch (methods_info.cpp
  precedent): `#if defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD)` → real
  headers; `#else` → `platform/doc_stub.h` + `lualib.h` + local stand-ins
  (CTextDocument, CDebugLuaDlg or a guard) `#endif`.
- Internal `#if` structure — essentially none: three `#ifndef _WIN32` forks
  already present (all port-added, all compile against doc_stub-visible seams):
  - 3064-3092 `L_GetMainWindowPosition` → `pDoc->GetMainRect` (IWorldView ✓)
  - 3109-3141 `L_GetNotepadWindowPosition` → `CTextDocument::m_fn_get_rect`
  - 3158-3206 `L_GetWorldWindowPosition` → `pDoc->GetOutputRect` (IWorldView ✓)
  plus `#ifdef LUA_52` at 7764. That's all.
- `mushclient_typename` ("mushclient.world") is defined at line 12 —
  lua_world_api_portable.cpp defines its own copy; both die with the file ✓.

## 5. Estimated stub-side additions

- **doc_stub.h `CMUSHclientDoc`: 116 method declarations** (signatures copied
  from doc.h — mostly `long f(...)`, `VARIANT Get*List()`, `BSTR Get*Text()`).
- **doc_stub.h data members: 1** (`BOOL m_bRemoveMapReverses`).
- **doc_stub.h App: 2 methods + 1 member** (ActivateNotepad,
  AppendToTheNotepad, m_strLastDebugCommand).
- **doc_stub.h misc: ~9** (flags_pair typedef, 5 MINIWINDOW_* defines,
  init_by_array decl, SetPathName widening; CTextDocument/CDebugLuaDlg
  stand-ins live in lua_methods.cpp's headless arm, not the stub header).
- **New stub-bodies TU** (e.g. `platform/doc_gui_method_stubs.cpp`,
  mushcore-only): 116 tiny bodies with the family semantics of §1 (the
  portable twins' behaviours are the spec). ~600-800 lines, entirely
  mechanical. Alternative: adopt methods_accelerators.cpp (3) and later
  methods_output.cpp (25) for real behaviour — Phase-4 material, NOT needed for
  the flip.
- **lua_utils_portable.cpp**: add `MakeTableItem(COleDateTime)` overload +
  `ptr_xmllib` export; delete `RegisterLuaRoutines` + constants block +
  `RegisterUtilsLib` (folded into ptr_xmllib) + `DisableDLLs` (after diff).
- **CMake**: add `scripting/lua_methods.cpp` + the stub TU to mushcore; remove
  `scripting/lua_world_api_portable.cpp`.

## 6. Recommended staging plan

Each step leaves the tree green (build + ctest 21/21); commit per step.

1. **Stub surface first, no behaviour change**: add the 116 decls + members +
   misc to doc_stub.h; add `doc_gui_method_stubs.cpp` with the §1 semantics;
   compile it into mushcore *while lua_world_api_portable.cpp still reigns*
   (nothing calls the stubs yet — pure additive). Risk: doc_stub.h layout is
   NOT affected (methods only, 1 BOOL member + App CString — but remember the
   tests-vs-lib layout rule: **full rebuild**, stale-`.o` hazard).
   Rollback: revert one commit.
2. **lua_utils_portable.cpp carve-out prep**: add `MakeTableItem(COleDateTime)`
   + `ptr_xmllib` export (additive, unused yet). Optionally rewire the twin
   `L_GetLineInfo` to prove the overload. Rollback: trivial.
3. **Migrate/decide the extras** (tests first): switch test_lua_scripting.cpp
   MD5/SHA256 to `utils.md5/utils.sha256`; decide ExpandVariables (keep-extra
   or rewrite test). Tests still green against the OLD portable lib (it has
   utils.md5 too — verify; if not, this step rides with step 5).
4. **The flip commit** (the only big one — do it atomically):
   - lua_methods.cpp: head-switch + headless stand-ins (CTextDocument,
     CDebugLuaDlg arm) — the ONLY in-file edits;
   - CMake: mushcore swaps `lua_world_api_portable.cpp` → `lua_methods.cpp`;
   - lua_utils_portable.cpp: delete RegisterLuaRoutines + constants block +
     RegisterUtilsLib + DisableDLLs;
   - delete `scripting/lua_world_api_portable.cpp`;
   - (if keeping any extra) add RegisterPortableLuaExtras + the `#ifndef
     _WIN32` call in lua_scripting.cpp.
   **Full clean rebuild** (layout rule), ctest.
   Rollback point: single revert restores the old world.
5. **Post-flip verification sweep**: the known portable-subset test
   expectations flip from "subset" to "full API" — REMOVE the
   KNOWN-PORTABLE-SUBSET-FAILURES carve-outs in test_lua_scripting.cpp
   (AddAndGetTrigger, MD5... now pass wholesale) and re-enable them; run the
   in-app PAL selftests; live-check mushgui (its lua_methods.cpp path is
   textually touched by the head-switch — must stay byte-identical semantics).
6. **Cleanup**: doc_portable.cpp locator comments; memory notes
   (portable-twin-drift becomes historical); CLAUDE.md status.

### What can go wrong (watch list)
- **Layout/stale-.o SIGSEGV**: doc_stub.h changes → full rebuild both lib and
  tests (documented failure mode).
- **Duplicate symbols** at step 4 if the lua_utils_portable carve-out misses a
  twin (RegisterLuaRoutines, DisableDLLs are the two known; re-`nm` after).
- **`luaL_openlibs` ordering**: portable RegisterLuaRoutines opened libs
  itself; the real one does not. All current callers (OpenLuaDelayed, test
  fixtures, the 5 engine embeds if they make Lua states — check
  `mc_dispatch`/engines for RegisterLuaRoutines calls) must open libs first.
- **`ptr_xmllib` utils semantics**: after the flip utils.* comes from
  luaopen_compress + ptr_xmllib(= portable utils_lib). Verify no utils name
  regression (compare `utils` table keys before/after in a scratch harness).
- **GetLineInfo timestr**: needs the new MakeTableItem(COleDateTime) — the
  port COleDateTime stores raw Unix seconds; mirror lua_utils.cpp's format
  exactly (it formats via COleDateTime::Format on Windows — decide the
  portable rendering deliberately, it is user-visible).
- **world.Save path**: `SetPathName(strOldName, FALSE)` — widening the stub
  signature must not change CDocument behaviour for other callers (grep for
  SetPathName uses in mushcore TUs first).
- The `--pal-*-selftest` in-app tests run the REAL miniwindow engine in
  mushgui — untouched by the flip, but run them anyway (mushgui links mushapp,
  and lua_methods.cpp is shared by both after the flip).

---

## Prep already landed (this working tree, uncommitted)

1. **error_descriptions[]** relocated verbatim (CRLF→LF) from lua_methods.cpp
   to the new shared TU `scripting/errors.cpp` (mushcore + mushapp + vcxproj);
   windows_stubs.cpp sentinel removed; portable `L_ErrorDesc` and the
   `error_desc` Lua table now walk the real table (including the Windows
   duplicate-30037 behaviour). `Translate_NoOp` added to doc_stub.h.
2. **globalregistryoptions.cpp adopted into mushcore** (head-switch; stub App
   gained MUSHclient.h-exact types for all 77 table members + inert
   GetProfile*/db_* accessors); doc_portable's VT_NULL GetGlobalOption bodies
   deleted; portable L_GetGlobalOption(List) are now thin real-method wrappers.
   Headless GetGlobalOptionList returns all 77 Windows names.
3. **Rich CallPlugin marshalling** ported verbatim into the portable
   L_CallPlugin (multi-arg/multi-return cross-lua_State, 4.55 semantics,
   error/traceback paths) — behaviour is Windows-complete pre-flip.

All three verified by an ad-hoc harness against libmushcore.a (18/18 checks:
descriptions, dup-key, code-as-string fallback, 77-name list, option values,
nil for unknown, LoadPlugin + CallPlugin multi-arg/multi-return/nil-hole,
eBadParameter table rejection, eNoSuchRoutine, runtime-error triple,
eNoSuchPlugin). Full build green; ctest 21/21.
