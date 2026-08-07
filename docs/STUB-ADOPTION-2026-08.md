# The headless GUI stubs: what is actually left (2026-08-06)

A five-way survey of `platform/doc_gui_method_stubs.cpp` and
`platform/doc_flip_gui_stubs.cpp`, prompted by the CLAUDE.md bullet sizing the
"one-library endgame" tidy-up at *"138 stubbed methods … architectural work, not
a tidy-up that fits in a session"*.

**That sizing was wrong in both directions at once**, which is why it is worth a
document rather than a line. The count is too low and the difficulty is far too
high.

## 1. The count

| File | Definitions |
|---|---|
| `platform/doc_gui_method_stubs.cpp` | **108** `CMUSHclientDoc` methods |
| `platform/doc_flip_gui_stubs.cpp` | **102** member/free definitions (+2 globals, 14 message maps) |
| **Total** | **~210** |

The in-file family headers are stale too — they say 49/14/25/25 where the truth
is 47/14/25/22, because `BlendPixel`, `FilterPixel` and the whole accelerator
family have already been un-stubbed.

## 2. The shape, which is the actual finding

**The two files are opposite kinds of thing and should never have been counted
together.**

`doc_flip_gui_stubs.cpp` is **~86% correct-by-design**. Of its 102 definitions
roughly **88 are irreducible**, and the largest block is the clearest case: 44
dialog ctor / `DoDataExchange` / message-map definitions across 16 classes.
`CDialog::DoModal` is `return g_run_mfc_dialog ? … : IDCANCEL`
(`platform/mfc_ui.h:3384`) and that hook is null in the test link, so adopting
the real dialog TUs would execute **zero** additional lines. Likewise the 33
menu/update handlers (`:86-122`), whose addresses `doc.cpp`'s message map takes
and which no headless menu can ever fire, and the five dead-view methods, whose
call sites all sit inside a `GetFirstView` walk that yields nothing (the
recorded *dead view family strategy*). Only **~18** of that file are real work.

`doc_gui_method_stubs.cpp` is **~95% real todo — but almost none of it is
implementation.** The survey said **five of 108** were irreducible because
`FlashIcon`, `MoveMainWindow`, `GetFrame`, `Transparency` and
`SetToolBarPosition` all dereference `Frame` = `*(CMainFrame*)App.m_pMainWnd`
(`stdafx.h:271`), nullptr headless.

**That is wrong on this platform, and the correction matters.** `stdafx.h:271` is
inside `#ifdef _WIN32`. Off Windows `Frame` is a **real extern global object**
(`CMainFrame Frame;`, `platform/windows_stubs.cpp:27`), so `FlashIcon`,
`MoveMainWindow` and `SetToolBarPosition` would call inert methods on a real
object — no crash. Only **two** genuinely deref a null pointer, and they do it
through `App.m_pMainWnd->m_hWnd`, not through `Frame`:

| Method | Why |
|---|---|
| `GetFrame` | `return (long) App.m_pMainWnd->m_hWnd;` (`methods_output.cpp:81`) |
| `Transparency` | `MakeWindowTransparent (App.m_pMainWnd->m_hWnd, …)` (`:249`) |

So the permanent-contract list is **two**, not five. Anyone doing the output
family must still decide what those two answer headless before moving the TU.

The other 103 are **TUs to be moved, not bodies to be written**. Measured with
`nm` against the real objects, the entire implementation cost across the four
families is:

| Family | Undefined symbols if adopted |
|---|---|
| Miniwindow (47) | **5** — `LoadPng`, `LoadPngMemory`, `CBitmap::CreateCanvasBitmap`, `CBitmap::Attach`, `CBitmap::DeleteObject` |
| Chat (25) | **1** — `GetHostNameAndAddresses` (`Utilities.cpp:413`, pure POSIX) |
| Notepad (14) | **0** |
| Output/view (22) | **2** — `ValidateBrushStyle`, `CMUSHView::AutoWrapWindowWidth` (both for `TextRectangle` alone) |

### Why miniwindow is the cheap family, not the dear one

This is the *blocker stated at the wrong layer* pattern again, and it is the
third time it has appeared in this port.

The intuition is "miniwindows need the GUI". They do not — they need a
**surface**. `miniwindow.cpp`, `miniwindow_compose.cpp` and
`methods_miniwindows.cpp` include **no wx headers at all**; ~190 of the ~205
drawing sites already go through `pal::ISurface` via `pal/surface_bridge.h`, and
every one is guarded `if (!srf) return …`. A conforming headless rasteriser
already exists and is already conformance-tested (`pal/cpu_raster.cpp`, covered
by `tests/test_pal_surface.cpp`).

## 3. The sizing note that was wrong, and the check that finds it

CLAUDE.md also recorded that one hazard had been checked and cleared:

> *only 11 of the 138 return anything but 0, and each of those returns the real
> method's own documented failure sentinel … Nothing to unpick first.*

**This is wrong, and three of the five analysts refuted it independently.** There
are roughly **30 test assertions pinning stub contracts**:

- `tests/test_lua_scripting.cpp:515-517` asserts `WindowCreate(…) == 30073`, and
  `:521-524` that the follow-on `WindowList()` is nil. The real bodies return
  `eOK` and `{'w'}`.
- `tests/test_lua_surface.cpp:454, 455, 463, 513, 522` assert `== 0` for the two
  image setters, `SetToolBarPosition` and `AddFont`.
- `tests/test_lua_surface.cpp:598-628` asserts `== eNoChatConnections` for **23**
  chat calls.

The old check asked *"is the return value a plausible sentinel?"* — and
`eNoChatConnections` passes that. The check that actually finds the problem is:

> **grep the test suite for the stub's return value.**

Run that before sizing any adoption. A test pinning a stub does not merely miss
coverage; it *blocks* the adoption, which is the recorded
*stubs-defend-wrong-contracts* shape.

## 4. Product bugs the stubs were hiding

Two are confirmed by reading both sides at HEAD. Neither is a stub defect — they
are genuine unimplemented features on macOS, and the stubs' cheerful `eOK` is
why they went unnoticed.

**`utils.appendtonotepad('newtitle', …)` did nothing on macOS** — fixed in
`9feb902`. It routed through `g_wxhook_append_to_notepad`, which looks a notepad
up by title alone and returns false when there is none; the canonical body calls
`App.AppendToTheNotepad`, which creates one. Two smaller drifts in the same twin
went with it: the `\n` → `ENDLINE` conversion and the `eNotepadPacketDebug` type.

**`world.AddFont()` silently registers nothing on macOS.** `AddFont` →
`AddSpecialFont` (`doc.cpp:3851`), whose first act is `LoadLibrary("gdi32")`
(`doc.cpp:3855`); the shim returns nullptr (`platform/mfc_ui.h:5921`), so
`pAddFontResourceEx` stays NULL and the real body returns `eNoSuchCommand` at
`doc.cpp:3874`. The headless stub returns `eOK` and
`tests/test_lua_surface.cpp:513` **asserts** `eOK`. The macOS equivalent is
`CTFontManagerRegisterFontsForURL`.

**`world.ChatCall('hostname', port)` has never worked in the shipping mac app;
only dotted-IP calls do.** `WSAAsyncGetHostByName` is an inline stub returning
nullptr (`platform/mfc_ui.h:7451`), so `ChatCallGeneral` takes
`if (!pSocket->m_hNameLookup) { delete pSocket; return eCannotLookupDomainName; }`
(`scripting/methods/methods_chat.cpp:145-149`). The completion side is dead too:
`CChatSocket::HostNameResolved` is dispatched only from
`CMainFrame::PreTranslateMessage` off a `WM_USER_HOST_NAME_RESOLVED` message no
port code ever posts. There are exactly three call sites, and
`platform/posix_transport.cpp` already has a headless-drivable `getaddrinfo`
resolver (`:103-130`) draining through `pal::PumpTransportCompletions()`.

**Probable, needs a product run:** `world.SetToolBarPosition` likely returns
`eBadParameter` for every `Which` on the product, because the real body gates on
`!pToolBar->m_hWnd` (`scripting/methods/methods_output.cpp:275-276`) and the
toolbars are created in `CMainFrame::OnCreate` — one of the port's recorded dead
readers. Headless says `eOK`.

## 5. Order of work

0. ~~**Drift fixes**~~ — **done**, `4f41ad5`. Four verified drifts
   (`TextRectangle`'s dropped NAWS renegotiation, `GetRecentLines`' separator
   guard, `Redraw`/`Repaint` as no-ops, `QuoteForumCodes` hoisted out of the GUI
   TU), each with a regression test mutated in both directions.
1. ~~**Notepad (14)**~~ — **done**, `3d9d241` + `9feb902`. `TextDocument.cpp`
   and `methods_notepad.cpp` are in `mushcore`; the App-level trio
   (`AppendToTheNotepad` / `FindNotepad` / `ActivateNotepad`) is hoisted to
   `notepad_app_shared.cpp` and added to `MUSHclient.vcxproj`. All eleven
   `g_wxhook_*_notepad` hooks are gone. Net **-349 lines**, `ctest` 56 → 57.

   Worth carrying forward from it:
   - The link confirmed the analyst's one caveat did not materialise: **zero**
     new undefined symbols, no `CTextView` to stub. Adoption really was a CMake
     move.
   - It found **six** twins for one family, not one: the stub file, the App-level
     pair in `headless_app.cpp`, `AppendToTheNotepad` in `doc_port_glue.cpp`
     (which passed `App.m_strLuaScript` — the Lua sandbox script — as a **font
     name**, and docnum 0 so its notepad could never be found again),
     `ActivateNotepad` in `doc_headless_stubs.cpp`, `SetTheFont` in
     `doc_flip_gui_stubs.cpp`, and the `utils.*` pair in
     `lua_utils_portable.cpp`. Count twins per *family*, not per file.
   - One real macOS product bug fell out: `utils.appendtonotepad` to a title
     that did not exist yet did **nothing**, because the hook it used looks up by
     title alone and never creates, where the canonical creates.
   - **Four existing tests were using a twin's hook as their capture seam** —
     that is a second way a stub blocks adoption, distinct from pinning a return
     value, and the grep in §3 does not find it. They now capture at
     `g_wxhook_create_text_window`, which is what the product actually uses;
     `test_connection`'s SendTo test got materially stronger as a result,
     because it can now assert that replace *discards* what append kept.
   - One behaviour changed headless, deliberately and towards Windows:
     `GetNotepadList` with nothing open returns nil, not an empty table.
2. ~~**Chat (25)**~~ — **done**, `44383aa` + `065cfab`. The three TUs are in
   `mushcore`, `GetHostNameAndAddresses` is hoisted to `utilities_shared.cpp`,
   and `tests/test_chat_sockets.cpp` drives the whole accept chain over loopback,
   closing `docs/UNREACHABLE-PATHS-2026-08.md` finding 23 outright.

   Worth carrying forward:
   - The recorded "chat is half-ported, `Accept`/`AsyncSelect`/`m_ChatList`/
     file-transfer is accepted debt" note was **stale**. `mushcore` already
     linked half the engine.
   - `eNoChatConnections` was the right answer for **2** of the 25. The real
     bodies distinguish five failure modes, and two of the calls are not failures
     at all — `ChatDisconnectAll` returns a **count** (so the stub put an error
     code in a count slot and `> 0` read backwards) and `ChatNameChange`
     **succeeds** with nobody connected, which is exactly when a script sets it.
   - Finding 23's cost estimate was for *untangling the stub*, not for the
     harness. Once unrelated work removed its stated blocker the "multi-day"
     item was an afternoon. **Re-derive a cost estimate when its blocker has been
     removed by something else.**
   - `test_display`'s chat-snoop test stopped being coverage-only: its comment
     recorded the bytes as unobservable, which was true only because
     `SendChatMessage` was an empty stub.
3. **Miniwindow stage 1 (47).** Build-graph move + three one-line `CBitmap`
   bodies + the `LoadPng` hoist (do it once — the output family's image setters
   need the same symbol).
4. **Headless surface bridge + DIB store.** The one genuine infrastructure item.
5. **`scripting/scripting.cpp`** — carries a real behavioural question (see
   below), so keep it separate.
6. **`dialogs/ProgDlg.cpp`** — opportunistic; retires 14 definitions.

## 5a. A hazard found while sizing the output family

Moving `scripting/methods/methods_output.cpp` into `mushcore` **without deleting
the stubs in the same step builds cleanly and passes the entire suite** — and is
wrong. Both archives then define the family, and which body is live depends on
static-archive link order (`mushcore_testsupport` is listed first, so its stub
wins the symbols it defines, while `mushcore`'s object may still be pulled in for
others). There is no duplicate-symbol error to warn you.

**Delete the stubs in the same commit as the TU move**, and prove the real body
is live with an assertion the stub would fail — not with a green suite. A green
suite is exactly what this hazard produces, not what rules it out.


## 6. Explicitly not doing

- **Retiring the five `Frame`-dereferencing stubs.** Permanent contract; see §2.
- **Adopting dialog TUs for their own sake.** Zero executed lines.
  `PluginWizardSheet.cpp` was adopted for *cost* (8 aggregated page members made
  hand-stubbing impractical), not capability. Adopt a dialog TU when the stub is
  expensive to maintain or its `DoDataExchange`/`OnInitDialog` carries engine
  logic worth testing. `CProgressDlg` qualifies; the other fifteen do not.
- **Closing `CpuRaster`'s documented S1 gaps** (`pal/cpu_raster.h:15-19`). The wx
  adapter is the shipping rasteriser and the right parity bearer; those probes
  belong in `platform/miniwindow_selftest.cpp` by design. Because those ops are
  silent no-ops in `CpuRaster`, **every assertion in an adopted miniwindow test
  must be built so "drew nothing" FAILS.**
- **Widening `pal::g_adoptSurface`'s signature.** It is shared with Windows
  (`pal/gdi_surface.cpp`). Key the headless side map on the `CBitmap` pointer.
- **"Fixing" stubs to match real bodies as a general strategy.** That is the
  drift-maintenance treadmill and buys no coverage. Step 0 above is the
  exception, and it is written to be *deleted* by the adoptions.

## 7. Unresolved, needs a decision

`CreateScriptEngine` exists in **two** hand-written copies
(`platform/doc_flip_gui_stubs.cpp:154` and `platform/doc_port_glue.cpp:468`) that
have drifted from each other and from `scripting/scripting.cpp:23-101`. Three
differences, none recorded in `docs/UPSTREAM-DIVERGENCES.md`:

1. The re-entrancy guard (`scripting.cpp:28-32`) is absent, so a second call
   overwrites `m_ScriptEngine` and **leaks** the previous one.
2. The real body creates the engine *before* the `m_strScriptFilename.IsEmpty()`
   bail, so on Windows a world with scripting enabled and no script file still
   has a live engine — immediate mode, `world.Execute` — and the port does not.
3. On script-open failure the real body `ColourNote`s and calls
   `DisableScripting()`; the twin prints to stderr, keeps the empty engine, and
   returns **false meaning success**.

(2) is a deliberate headless deviation dating from when Lua was the only engine.
Six languages work in the product now, so it should be re-decided rather than
inherited.

## 8. A documentation correction

CLAUDE.md credits `ccd094b` (the `StepIt`/`OffsetPos`/`SetWindowText` fix) with
making "the plugin-loading bar" move. **It cannot have.** The plugin-loading
progress block at `xml/xml_load_world.cpp:734` sits inside
`#if IT_GET_GOATS_BLOOD_OUT`, and that macro occurs exactly once in the entire
tree — at that `#if`. It is defined nowhere, so the block is dead code on every
platform, Windows included.

The fix was still real and still worth having; it just benefited the *other*
`CProgressDlg` users (Recall, Send-to-world, world-prefs page 9067, the Lua
`progress` library) rather than plugin loading. Worth correcting because the
claim reads as evidence that the progress bar is exercised during world load,
which would make it a natural place to hang an adoption test — and it is not.
