# Phase 2 Staged Flip Plan — mushcore onto the real `doc.h`, one library at the end

> **⚠ CORRECTION (2026-07-19, Stage 4 attempt): the central premise below is FALSE.**
> doc_stub.h's `CMUSHclientDoc` is NOT layout-compatible with doc.h's — verified
> independently: doc.h is `: public CDocument` (vtable + base subobject),
> doc_stub.h:1944 is `class CMUSHclientDoc {` (no base); member ORDER is entirely
> different (doc.h: m_pTransport…; stub: m_normalcolour…). Flipping xml/ (batch 1)
> alone built clean but SIGSEGV'd 4 test binaries — a doc constructed at stub layout,
> written at doc.h offset (m_strCurrentIncludeFileList 1424B past the allocation).
> The 'serialize.cpp tolerated mixed-layout precedent' is illusory: serialize.cpp
> compiles against doc.h but no test invokes CMUSHclientDoc::Serialize, so its
> cross-layout access never runs. There is NO offsetof/static_assert guardrail.
> The incremental Stages 4–8 are therefore UNSAFE as written. Two recovery paths
> under discussion (see docs/memory): (A) genuine layout convergence first —
> reorder doc_stub.h onto doc.h's base+order, add an offset-parity guardrail, THEN
> the incremental flip is safe; (B) big-bang — flip every doc TU to doc.h atomically
> so no mixed-layout window ever exists. Stages 1–3 (hygiene, decls, support TUs)
> remain valid and are committed.


**Goal.** Retire `platform/doc_stub.h` (4,046 lines) and `doc_portable.cpp` (1,274 lines):
every TU in the tree compiles against the real Windows headers
(`stdafx.h` → `MUSHclient.h` → `doc.h`), `mushcore` adopts the real `doc.cpp` /
`doc_construct.cpp`, and the mushcore/mushapp twin-library split collapses into
**one core library + GUI adapters + test fakes**. Windows behaviour stays sacred;
`MUSHclient.vcxproj` sees only deleted `#else` arms (a no-op — Windows already takes
the real-header arm of every include switch).

**Evidence base.** Four audit lenses (2026-07): a compile probe of flipped TUs under
mushcore's exact flags, a member-by-member surface diff of the two class trios, an
`nm`-level link census of `libmushcore.a`, and a consumer/restructure census. Probe
artifacts: `/private/tmp/claude-501/-Users-robot-Documents-mushclient/67a6b272-ea6f-4497-b5d9-699936b93cf4/scratchpad/p2clean/`.
(Do **not** reuse `…/scratchpad/probe/` — it contains stale `// probe shim` decoy
headers that shadow the real ones and produce false-clean compiles.)

## Why this is cheaper than it looks (the de-risking facts)

1. **The header flip itself is free.** The real `doc.h` compiles with **zero errors**
   under mushcore's exact flag set (`-DHAVE_CONFIG_H -DMUSHCLIENT_LUA_SCRIPTING
   -DOPENSSL_FOUND -DWIN32 -D_NOSOUND`, C++17, no `MUSHCLIENT_FULL_BUILD`), and the
   `sizeof(CMUSHclientDoc)` static_assert passes. So does `mainfrm.h`. The
   `#ifndef MUSHCLIENT_FULL_BUILD` no-op arms in `platform/mfc_ui.h` (:2023-2482 etc.)
   fully satisfy `doc.h`. No edits to `doc.h`'s Windows content are needed.
2. **The tree already lives with two layouts.** `serialize.cpp` (in
   `MUSHCORE_SOURCES`, CMakeLists.txt:489) and `platform/windows_stubs.cpp`
   (:16-22) include the real `doc.h`/`mainfrm.h`/`MUSHclient.h` **inside mushcore
   today**, so `libmushcore.a` already carries both `CMUSHclientDoc` definitions,
   two `CMainFrame` types and two `Frame` objects — tolerated because the stub is
   layout-converged. The flip *removes* this standing ODR hazard rather than adding one.
3. **The seam is already in `doc.h`.** m_pTransport/m_pTransportEvents (doc.h:660-661),
   m_pWorldView + the ~45 Emit*/Have*Sink helpers (doc.h:1516-1697),
   m_gui_lines_emitted (:1507), m_gui_rows/cols/font/paused (:1690-1697),
   DisconnectWorld (:1499), port-arm UpdateAllViews/InitZlib (:1885-1888). The feared
   drift classes (m_gui_*, IWorldView fields, option-cache members, using-decls)
   produced **zero** probe errors.
4. **Compile viability of the adoption targets is proven daily**: mushapp compiles
   `doc.cpp`, `doc_construct.cpp`, `world_debug.cpp`, `mushview.cpp` off-Windows now
   (CMakeLists.txt:731-806). The open cost is mushcore's **link closure**, not compilation.
5. **Total distinct blocking issues across every probed TU: ~17**, overwhelmingly
   mechanical (5 port-section decls, 1 doc-registry API rename, App/Frame global
   rehoming, 2 duplicate deletions, 2 one-token fixes, per-TU includes/externs,
   3 mechanical test refactors).

## Invariants for every commit (the option/display-flip discipline)

- **Always green:** `cmake --build build -j6` builds `mushcore`, `mushapp`,
  `mushgui`, all 18 test executables; `ctest` 21/21 (incl. the 3 in-app PAL
  selftests). Bounded `-jN` only (memory: build-parallelism-limit).
- **Layout lockstep:** while any TU still includes `doc_stub.h`, every member
  added/changed in `doc.h` is mirrored into the stub in the *same* commit; the
  `mc_test` defines in tests/CMakeLists.txt:21-25 change in the *same* commit as
  mushcore's define set. After any `doc_stub.h`/`doc.h` member change: **full
  rebuild** (stale `.o` layout in the archive → destructor SIGSEGV; see CLAUDE.md
  *Tests*).
- **Per-symbol duplicate audit** before every adoption commit: grep
  `windows_stubs.cpp`, `mushapp_stubs.cpp`, `doc_portable.cpp`,
  `doc_shared.cpp`/`doc_display_shared.cpp` for each symbol the incoming TU
  defines (the duplicate-definition trap fired 3× in Campaign C).
- **GUI smoke** after behaviour-adjacent stages: `pkill -9 -f
  MUSHclient.app/Contents/MacOS/MUSHclient`, launch, open a world, connect,
  confirm output + a miniwindow plugin (memory: macos-gui-test-safety).
- Commit style per CLAUDE.md: small, focused, no assistant attribution.

---

## Stage 1 — Pre-flip hygiene: kill dead weight and drift in the stub

*No behaviour change intended except one deliberate parity fix.*

Commits (each independently green):

1. **Delete dead stub-only members** — zero users outside `doc_stub.h` anywhere:
   `m_AliasList`, `m_TriggerList`, `m_CommandHistory` + the inline
   `AddToCommandHistory`, `m_CurrentCommand`, `m_MudMap`. (Full rebuild after —
   class layout changes.)
2. **Delete the stub-only `Trace(const CString&){}` overload** (doc_stub.h:3786).
   This is a *live parity bug*: it wins overload resolution over the real
   `Trace(LPCTSTR,...)` (methods_tracing.cpp, already in mushcore), silently
   dropping lua_scripting.cpp:564/795 "Executing %s script" trace lines headless.
   Deleting it lets CString→LPCTSTR conversion bind the real body. Update any test
   baseline that now sees trace output.
3. **One-token signature/default alignment** of the stub onto `doc.h`:
   `CheckConnected` BOOL→bool (doc.h:1852); `ShowStatusLine(bool)` gains `= false`
   (doc.h:2331); `FormatTime` gains `bFixHTML = false` (doc.h:2505);
   `DoFixMenus(void*)`→`(CCmdUI*)` (doc.h:2636); `m_bMapping` bool→BOOL
   (doc.h:2800); `m_strCustomColourName[16]`→`[255]` (doc.h:877);
   `m_nBytesIn/m_nBytesOut/m_iInputPacketCount/m_nTotalCompressed/m_nTotalUncompressed`
   long→`__int64` (doc.h:1081, 1260-1261); `LoggingInput` drop the stray const
   (doc.h:2463); stub CMainFrame `OnUpdateBtnWorlds(int,void*)`→`(int,CCmdUI*)`.
4. **`TriggerToPane`**: either delete the stub decl + empty portable body or wrap
   both in `#ifdef PANE` to match doc.h:2154-2167 (PANE is defined in no build here).
5. **`CheckObjectName`**: delete the free-function twin (doc_stub.h:1573, body
   doc_portable.cpp:383) in favour of the doc.h:2299 *member* with identical
   signature/default; adjust the few call sites to member calls now, so the flip
   doesn't rebind them silently.

**Green criteria:** full build + ctest 21/21; `test_lua_scripting` re-baselined for
the Trace fix; GUI smoke unchanged.
**Rollback:** each commit reverts independently; no commit changes `doc.h`.
**Risk notes:** low. The only intended behaviour change is Trace (commit 2) —
isolate it so a surprise can be reverted alone.

---

## Stage 2 — `doc.h` port-section decls + per-TU declaration self-sufficiency

*Additive only; every TU still compiles against the stub. This front-loads the flip
so Stage 4–6 commits are pure include-head swaps.*

1. **Add to `doc.h`'s existing `#ifndef _WIN32` port section** (precedent
   doc.h:1885-1888): `BOOL LoadWorldFromFile(const char*)`,
   `void AllocateLinePositions()`, `void ResolveAllEntryPoints()`,
   `time_t m_scriptFileMtime` (sits beside the already-present
   `m_bInScriptFileChanged`), `time_t m_tWorldOpenTime` (or retire it onto the
   existing `m_whenWorldStarted` — decide here), and a declaration home for
   `Generate256colours` (currently only doc_stub.h:1683). Mirror all into
   `doc_stub.h` in the same commit (layout lockstep).
2. **Per-TU includes/externs** (Windows convention — cf. world_debug.cpp:25,
   serialize.cpp:27): add `#include "scripting/errors.h"` to
   `platform/doc_gui_method_stubs.cpp` (clears 69/72 of its probe errors: eOK,
   eNoSuchWindow, eNoChatConnections), `tests/test_display.cpp` (35 errors) and any
   other TU the probe flagged; add the `OptionsTable`/`AlphaOptionsTable` externs
   to `scripting/world_debug_portable.cpp` (all 7 of its errors).
3. **`CMultiDocTemplate` reconciliation**: the stub invented `AddDoc`/`RemoveDoc` +
   a doc list (doc_stub.h:1135-1156); the real shim `platform/mfc_ui.h:2954` has
   neither. Give the shim MFC-real `AddDocument`/`RemoveDocument` (+ the
   `GetFirstDocPosition`/`GetNextDoc` iteration `doc.cpp`/`plugins.cpp` actually
   use) and switch `doc_portable.cpp`'s ctor/dtor registry calls to those names.

**Green criteria:** full build + ctest; no test churn expected.
**Rollback:** purely additive decls — revert freely.
**Risk notes:** low. Watch that new `doc.h` port decls don't collide with Windows
names (`m_scriptFileMtime` vs the Windows `m_eventScriptFileChanged`/`m_pThread`
auto-reload machinery — they coexist; the port polls mtime instead).

---

## Stage 3 — Headless support TU + App/Frame unification prep

*Creates the strong-symbol safety net the flipped TUs will need, while everything
still links via the stub's weak inlines (strong defs coexist with and win over
weak inline defs — the `windows_stubs.cpp` pattern, proven in-tree).*

1. **New TU `platform/doc_headless_stubs.cpp`** (compiled into mushcore only,
   against the real headers like windows_stubs.cpp): the link census' 12-method
   post-flip undefined set that lives in TUs mushcore won't compile —
   `Redraw`, `GetWorldWindowPositionX`, `GetChildFrame` (methods_output.cpp family;
   *not* covered by doc_gui_method_stubs.cpp's 116), `ActivateNotepad`
   (methods_notepad.cpp:210-241 — route the existing
   `g_wxhook_activate_notepad`), `SortTimers` (timers.cpp:431 is `#ifdef _WIN32`;
   copy the mushapp no-op at platform/mushapp_stubs.cpp:358-361), `ShowErrorLines`
   + `WriteErrorLines` + `FindAllEntryPoints` (scripting/scripting.cpp),
   `DrawMappingStatusLine` (view no-op), and the **free** functions `::ShowHelp` /
   `::ShowFunctionslist` (stdafx.h:799-802; real bodies scripting/functionlist.cpp:526+,
   which is mushapp-only because it also defines `CMainFrame::OnGameFunctionslist`
   — lift its existing port arm at :633). Note `ChangeFont`/`ChangeInputFont`/
   `DoSave`/`FixInputWrap`/`FixUpOutputBuffer`/`OnConnectionConnect`/`Screendraw`/
   `SetTitle` are *not* stubbed here — they arrive with `doc.cpp`/methods_output.cpp
   adoption in Stage 7; until then the stub inlines still satisfy stub-side TUs, so
   sequence Stage 4 batches so no flipped TU needs them before Stage 7 (probe data
   lists the referencing objects per method — use it to order the batches; if a
   batch can't wait, add a temporary strong no-op here and delete it in Stage 7).
2. **`AddToMap` decision** (parity bug behind a stub no-op): `AddToMap` is doc-side
   logic (Mapping.cpp — parses typed movement into `m_strMapList` with
   reverse-removal) and is called from doc_shared.cpp:2444, which mushcore
   compiles; the stub `{}` silently disables typed-direction auto-mapping headless.
   **Adopt Mapping.cpp's doc-side half** (guard or split the view part) rather than
   stubbing; add a regression test in `test_speedwalk`/`test_utilities`.
3. **Frame plan**: mushcore converges on real `mainfrm.h` (→ `mainfrm_stub.h`
   :128-273 off-Windows), which compiles clean under mushcore flags (probe 0) and
   already links as `CMainFrame Frame` in windows_stubs.cpp:27. The doc-side call
   surface is the ~15 methods the current stub fakes (SetStatusMessage/Now,
   SetStatusNormal, FixUpTitleBar, OnUpdateBtnWorlds, CheckTimerFallback,
   PlaySoundFile/CancelSound, ReturnStatusRect/ReturnToolbarRect, IsFullScreen,
   PostMessage) — either adopt `mainfrm.cpp` (its wx hooks are null-safe and
   `wx_mfc_hooks_storage.cpp` is already a mushcore TU) or define those ~15 in
   the support TU. Start with the ~15-stub route (smaller); revisit mainfrm.cpp
   adoption at Stage 9.
4. **App plan** (the probe's one declared blind spot — link, not compile): the real
   `CMUSHclientApp` ctor lives in `MUSHclient.cpp`, which mushcore does not
   compile. Add `platform/headless_app.cpp` (mushcore/test link only) defining the
   real-type `CMUSHclientApp App;` plus a headless ctor whose spec is the current
   stub's inert bodies (including the MXP_Load* parity work the stub ctor does)
   and the db_*/GetProfile* no-ops the stub carries; `globalregistryoptions.cpp`
   already requires MUSHclient.h-exact layout, so the real header is the only
   correct shape. mushgui keeps `mushapp_globals.cpp` + real `MUSHclient.cpp` —
   two ctor *bodies* across disjoint link targets is legal; document it as
   deliberate. Also give the stub App's missing real members
   (`m_nUniqueNumber` + `__int64 GetUniqueNumber()`) their real semantics here.

**Green criteria:** full build + ctest 21/21 (the new strong defs must not
conflict with existing ones — run the per-symbol audit); GUI smoke;
`selftest_pal_miniwindow` still green (doc_headless_stubs/doc_gui_method_stubs
must stay out of the mushgui link).
**Rollback:** the new TUs are additive files; revert = drop them from
`MUSHCORE_SOURCES`.
**Risk notes:** medium. This stage contains the two real unknowns — the App-ctor
link gap and the AddToMap adoption. Land AddToMap as its own commit with a test.

---

## Stage 4 — Flip the 63 include-switched shared TUs, in batches

Every shared TU carries
`#if defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD) → real headers #else → doc_stub.h`.
Flipping = **delete the `#else` arm and the switch**, keeping the real head
unconditional (identical to what Windows and mushapp compile today — vcxproj
no-op). ODR during the mixed window is the *already-tolerated* converged-layout
state (serialize.cpp precedent), so batches can land independently. Batch order,
cheapest-proven first:

1. **xml/** (3 TUs) — probe C: `xml_load_world.cpp` flips with **zero errors**.
2. **mxp/** (14 TUs).
3. **scripting/methods/** (25 TUs) — Stage 2's errors.h includes cover the enum uses.
4. **Top-level engine** (21 TUs: ansi, doc_shared, doc_display_shared,
   doc_scripting_shared, telnet_phases, Line, ProcessPreviousLine, evaluate,
   speedwalk, timers, regexp, plugins, UDPsocket, accelerators,
   globalregistryoptions, scriptingoptions, errors, scriptengine, lua_scripting,
   lua_methods, utilities_shared). Per the Campaign-C gotcha bank the real-arm
   heads *vary* (mainfrm.h/MUSHview.h/sendvw.h extras) — copy each TU's existing
   real arm verbatim, never a template head.
5. **Body-level guards in the same TUs** — re-home, don't blindly delete:
   doc_shared.cpp:1113 (CPasswordDialog vs headless empty password — the null
   `g_run_mfc_dialog` hook reproduces the headless arm exactly: DoModal→IDCANCEL→
   empty; delete the fork once the dialog TU links, else keep until Stage 7);
   doc_shared.cpp:1810 (Hyperlink works headless post-flip — delete the Note
   fallback); timers.cpp:79 (chat reap — un-guard once real chatsock links);
   timers.cpp:418 (compiles over shim CCmdUI — un-guard);
   **lua_scripting.cpp:201 `RegisterPortableLuaExtras`** (`world.ExpandVariables`,
   mushcore-only branch, used by test_aliases/test_lua_scripting/test_triggers) —
   **decision**: register always off-Windows (recommended; it's additive API) or
   drop + test churn. lua_scripting.cpp:246 package.path GUI logic stays.

**Green criteria per batch:** full build (mushcore *and* mushapp — the switch
deletion touches TUs mushapp compiles too, though its arm is unchanged) + ctest
21/21 + GUI smoke after batches 4–5.
**Rollback:** per-batch revert restores the switch; no cross-batch coupling.
**Risk notes:** medium-low. The probes covered representatives, not all 63; expect
a handful of unprobed TUs to surface one-line issues of the *same classes*
(errors.h, externs, a stray stub-only name). Budget slack for that, not for new
architecture.

---

## Stage 5 — Flip the port-only TUs (doc_portable.cpp compiles the real headers)

`doc_portable.cpp` stays *alive* here — it flips its include head and keeps
providing the portable ctor/dtor and bodies until Stage 7 replaces them. Probe A's
20 errors are all pre-cleared by Stages 1–3 except:

1. Replace the stub-class globals: delete `CMainFrame g_stub_frame` (line 153) and
   the `#define Frame g_stub_frame` macro (doc_stub.h:1524-1525) once the last
   stub consumer flips — all TUs now bind windows_stubs.cpp:27's real `Frame`;
   `CMUSHclientApp App` moves to `platform/headless_app.cpp` (Stage 3.4).
2. Delete the in-TU duplicates: `ENDLINE` (doc.h:80 defines it in-header),
   out-of-line `GetScriptEngine` (doc.h:2612 inline body is semantically
   identical — plugin-aware; no parity loss).
3. Registry calls → `AddDocument`/`RemoveDocument` (Stage 2.3).
4. Flip `platform/doc_gui_method_stubs.cpp` (errors.h include from Stage 2 clears
   71/72; the one remaining error is clang typo-correction fallout, not real) and
   `scripting/world_debug_portable.cpp` (externs from Stage 2 clear all 7).
   `scripting/lua_utils_portable.cpp` flips its head too but its twin-adoption is
   explicitly **post-Phase-2**.

**Green criteria:** full build + ctest 21/21 + GUI smoke. After this stage **no
non-test TU includes doc_stub.h**.
**Rollback:** single-TU reverts; the Frame-macro deletion commit is the one to
sequence last within the stage (it's the textual hazard).
**Risk notes:** medium. This is where the App/Frame link question (Stage 3.4)
gets proven for real. If ld64 turns up missing `CMainFrame`/`CWinApp` vtable or
method symbols, the fix is adding them to mainfrm_stub-backed no-ops in the
support TU — mechanical, but budget for a day of whack-a-mole.

---

## Stage 6 — Flip the tests + define-set lockstep

20 of 26 test TUs include doc_stub.h. Flip them all in one commit *series* (the
`mc_test` define pin makes partial flips fragile):

1. Mechanical refactors the probe identified: `make_doc()` returns
   `CMUSHclientDoc` by value — copy/move are implicitly deleted under doc.h
   (m_LineList's CTypedPtrList, doc.h:1113; compat.h deletes copy) → construct in
   place (static/`std::optional`/`unique_ptr`) in test_info.cpp:22-27,
   test_display.cpp:~27; `OnGameTrace` is protected afx_msg (doc.h:2741) → friend
   declaration or drive via the public command path; `new CChatSocket()` →
   `CChatSocket(CMUSHclientDoc*)` (chatsock.h:103). test_world_load's 74 errors
   are already cleared by the Stage 2 `LoadWorldFromFile` decl.
2. Update tests/CMakeLists.txt:21-25 (`mc_test` defines) **in the same commit** as
   any core define change (there should be none yet — WIN32/_NOSOUND persist
   until Stage 9), and re-verify the "layout identical" comment still tells the truth.
3. Full clean rebuild + ctest before declaring green (stale-archive layout is the
   historical SIGSEGV source).

**Green criteria:** ctest 21/21 from a *clean* build directory.
**Rollback:** revert the test commits; core is untouched by this stage.
**Risk notes:** low-medium. Purely mechanical, but it's the stage where a silent
layout mismatch would manifest as inexplicable test crashes — hence the clean-build rule.

---

## Stage 7 — Adopt `doc.cpp` + `doc_construct.cpp`; shrink `doc_portable.cpp` to death

The Campaign-B title goal. `doc.cpp` has 94 method defs: 66 clean, 11 shim-only
(UMessageBox/Frame./UpdateAllViews), 17 hard-GUI — the view-walks are inert once
the CDocument shim's `GetFirstViewPosition()` returns NULL, and the DoModal
launchers are IDCANCEL-safe via the null `g_run_mfc_dialog` hook (mfc_ui.h:3310).
Commit series:

1. **Link-closure pre-pass** (the ld64 landmine): `doc.cpp`'s BEGIN_MESSAGE_MAP
   (:112, 89 entries) + dispatch map (:208, `doc_dispatch_map.inc`) take the
   address of every declared handler — under ld64 each of the 518 afx_msg members
   needs *some* definition. Beyond the 116 already in doc_gui_method_stubs.cpp and
   Stage 3's 12, the gap concentrates in the world-prefs family (GamePreferences,
   Load/Save/ChangedPrefsP1-P23, ~70 methods, defined in mushapp-only
   dialogs/world_prefs/configuration.cpp) plus stragglers (RecallText/DoRecallText,
   OnEditFliptonotepad, SwitchToNotepad, OnGameChatsessions, ChatCallGeneral,
   CreateMonitoringThread, SendNAWS, SetUpOutputWindow, EditFileWithEditor,
   ExecuteHotspotScript, AppendMUDName, GetDefaultAccelerator…). **Decision
   (recommended per Lens 3): adopt the dialog/view TUs that `doc.cpp`/
   `doc_construct.cpp`/`doc_shared.cpp` name into the core link (they compile
   headless over the mfc_ui.h shims — mushapp proves it — and no-op via null
   hooks), and stub nothing new by hand** — a hand-maintained stub list is exactly
   what Campaigns B/C killed. Generate the concrete undefined-symbol list by
   linking a scratch target with doc.cpp.o added, before committing anything.
2. **Adopt `doc.cpp`**, deleting the 12 colliding strong defs in the same commit:
   doc_portable.cpp {SeeIfHandlerCanExecute, GetLinePosition, Debug_Packets,
   CheckConnected, PlaySoundFile, CancelSound} and windows_stubs.cpp
   {CheckScriptingAvailable, FindStyle, SortTriggers, SortAliases, DoFixMenus,
   ShowStatusLine}. Keep the **portable PlaySoundFile/CancelSound behaviour**:
   doc.cpp:3751/3792 route to Frame (MCI on Windows); off-Windows the
   miniaudia-hook bodies must win via an `#ifndef _WIN32` arm or the Frame-level
   no-op + g_wxhook_sound_* path (methods_sounds.cpp already does this; verify
   test_… sound expectations).
3. **Adopt `doc_construct.cpp`** (ctor 473 ln + dtor already internally
   platform-forked: DirectSound/CTimerWnd/CreateWinWorldView under `#ifdef _WIN32`,
   doc_construct.cpp:202-215, 620-628). Remaining deps: enum-only decls for
   CPrefsP6/7/8/16/18::eColumnCount (prefspropertypages.h is Windows-only in
   doc.h), CEvent(FALSE,TRUE) member-init and EnableAutomation/AfxOleLockApp shim
   no-ops (:402/:405), `setupstrings()` (comes with doc.cpp). `OnNewDocument`
   (QuickConnectDlg path) is unreachable headless — compile-or-guard. Relocate the
   portable-ctor-only work in the same commit: `Generate256colours()` once,
   `m_tWorldOpenTime`, the display-step-7 line seeding (m_pCurrentLine + AddTail +
   AllocateLinePositions — Windows defers to SetUpOutputWindow, which never runs
   headless) and the doc-template registration (→ CDocument-shim responsibility).
   **Parity deltas to re-baseline deliberately** (the real ctor is *more* correct):
   m_bInSendToScript true (stub false; consulted by doc_shared/methods_output/
   methods_commands), m_bTabCompleteFunctions true, m_bDisconnectOK true,
   SetDefaultCustomColours (Utilities.cpp:1138 — 16 curated RGBs + "Custom1..16"
   names vs the portable's ANSI-derived fakes). Keep the intentional headless
   default m_bNotesInRGB=true (doc_stub.h:2387-2390 rationale) via the port arm.
   **Real-dtor adoption fixes today's leaks** (CloseLog postamble, MCCP
   compress-buffer free + inflateEnd — MCCP runs headless — m_MapFailureRegexp,
   CustomElementMap/ActiveTagList/ActionList, DisableScripting vs raw delete);
   note the memory note *portable-linelist-ownership* is stale (both dtors
   blanket-free m_LineList now) — update it.
4. **Serialize rewire**: post-flip `serialize.cpp`'s `Serialize(CArchive&)` is
   live and layout-correct; shrink `LoadWorldFromFile` (doc_portable.cpp:829-932
   inlined loading branch) to a thin driver over the real Serialize/
   `OnOpenDocument` path. Do **not** leave two reachable world-load entries.
5. **Adopt `world_debug.cpp`** (10 methods, 8 clean, Debug's view-walk null-safe)
   and delete the twin `scripting/world_debug_portable.cpp` (CMakeLists.txt:609) —
   optional within Phase 2 but cheap here and kills a drift surface.
6. **Delete what remains of `doc_portable.cpp`.** Survivors have homes by now:
   LoadWorldFromFile driver + AllocateLinePositions + GetUniqueID/GetGUID +
   OnScriptFileChanged/m_scriptFileMtime + TFormat/TranslateGeneric
   (byte-check vs Utilities.cpp originals first) land in a small
   `platform/doc_port_glue.cpp`; ResolveAllEntryPoints folds under the real
   FindAllEntryPoints (scripting/scripting.cpp:169 — CMissingEntryPoints compiles
   headless); the inert lua_* stubs (:1164-1171) just die.

**Green criteria:** full build + ctest 21/21 + coverage run still functional +
**thorough GUI verification** (this stage changes the code mushgui links only via
deleted windows_stubs duplicates — mushgui links mushapp, not mushcore — but the
shared-TU guard deletions from Stage 4 and any mfc_ui.h edits do reach it): world
open/close/connect, logging, MCCP world, sound, script reload, Trace on.
**Rollback:** the stage is a series of ~8-12 commits, each green; the riskiest
(message-map link closure) is first and purely additive to a scratch target until
proven.
**Risk notes:** **high — this is the big one.** The link-closure fan-out is the
main schedule risk; the ctor/dtor parity deltas are the main behaviour risk.
Re-baseline tests intentionally, never by copy-paste of new output.

---

## Stage 8 — Collapse the `MUSHCLIENT_FULL_BUILD` GDI class-shape fork

The define's ~95% (include-switch heads) died in Stage 4. What remains is the one
place it changes **class layout/ODR**: platform/mfc_ui.h's GDI switch (:2023-2482,
:3521, :4278-4304, :6890, :7033-7053) — CDC/CBitmap/CPen/CBrush/CFont etc. are
inline no-op classes without the define, declaration-only (impls in
wx_gdi_compat.cpp) with it. One-library linking is impossible until this is one shape.

1. Collapse to the **declaration-only shape universally**; add a headless
   implementation TU to the test-fakes set: no-op GDI plus a *real* font-metrics
   fake for `ChangeFont`'s GetTextMetrics (the Phase-2 "output-pane font-metrics
   fake"); wx_gdi_compat.cpp remains the mushgui implementation. Two
   *implementations* of one class shape across disjoint link targets is fine; two
   *shapes* is not.
2. After this, the define's only consumers are the 5 engine-TU guards +
   wx_surface/wx_gdi_compat internals — rename to an honest name (e.g.
   `MUSHCLIENT_GUI_ADAPTERS`) or reduce the engine guards to pure
   `MUSHCLIENT_<LANG>_SCRIPTING`.

**Green criteria:** full build + ctest + **pixel-level GUI check** (miniwindow
selftests are the canary: `selftest_pal_surface`/`selftest_pal_miniwindow` must
stay green) — this stage can silently break rendering if a shape/impl mismatch
sneaks through.
**Rollback:** self-contained mfc_ui.h + new-TU change; revert restores the fork.
**Risk notes:** medium-high. ODR-sensitive; do it as one reviewed commit series,
full clean rebuilds only.

---

## Stage 9 — FINAL: delete `doc_stub.h`; one-library CMake restructure

1. **Delete `platform/doc_stub.h`** (now include-orphaned) + the residue that
   referenced it: stdafx.h sentinels/comments, doc.h "Mirror of the doc_stub.h
   twin" seam notes, mfc_ui.h's MC_QPC_SHIMS_DEFINED / MC_WSACANCEL_SHIM_DEFINED
   mixed-include workarounds, platform/mc_dispatch.h:83's "needs doc.h not
   doc_stub.h" constraint (which makes `mc_dispatch.cpp` core-eligible — the
   non-Lua dispatch substrate becomes headless-testable), comment-only refs in
   output_types.h, wx_outputpane.h, speedwalk.h, pal/iworldview.h.
2. **CMake restructure** — mushapp **dissolves** (its raison d'être was compiling
   the same 63 TUs twice under a different include head):
   - **core** (linked by every test and by mushgui) = current MUSHCORE_SOURCES
     (CMakeLists.txt:397) − {doc_portable.cpp (gone), doc_gui_method_stubs.cpp
     (→ test link), windows_stubs.cpp-as-is} + {doc.cpp, doc_construct.cpp,
     scripting/scripting.cpp, functionlist.cpp, mc_dispatch.cpp, the adopted
     dialog/view TUs from Stage 7, world_debug.cpp} + the once-only singletons
     (wx_mfc_hooks_storage.cpp, posix_transport.cpp, vendored md5/sha256/shs/
     lpeg/lsqlite3/… — today deliberately compiled twice under "no target links
     both", which stops being true).
   - **gui adapters** (mushgui only): all platform/wx_* TUs, views/frames
     (mushview.cpp — defines **zero** CMUSHclientDoc methods, it is not core —
     sendvw, mainfrm, childfrm, MDI*, TimerWnd, chat*, Activity*, Text*,
     miniwindow, genprint, Mapping.cpp's view part, remaining dialogs/**), the 7
     GUI methods files (methods.cpp, accelerators, chat, miniwindows, notepad,
     output, panes), the 5 embedded engines (Python/Perl/JS/Tcl/Ruby link weight
     must NOT leak into 18 test executables).
   - **test fakes** (`core_testsupport`): doc_gui_method_stubs.cpp,
     windows_stubs.cpp residue (CRuntimeClass statics, Sort* real bodies,
     FindStyle survivors after Stage 7 deletions), headless_app.cpp / one globals
     TU (real-typed App/Frame), the Stage-8 headless GDI/font-metrics TU,
     FakeWorldView/FakeTransport.
3. **Guardrails in the same commit series**: tests/CMakeLists.txt `mc_test`
   defines follow core's exactly (:21-25); coverage instrumentation targets the
   literal name `mushcore` (CMakeLists.txt:562-566) and tests/coverage.sh assumes
   it — rename-safe edit + **re-baseline docs/COVERAGE.md** (the measured
   denominator grows when doc.cpp/doc_construct.cpp/dialog TUs enter; the
   headline % will *drop* at this commit — pre-announced here so it isn't read as
   regression). PAL selftests survive iff doc_gui_method_stubs.cpp stays out of
   the lib mushgui links and miniwindow.cpp + miniwindow_selftest.cpp +
   wx_surface.cpp stay GUI-side; the `if(TARGET mushgui)` ctest registration
   (tests/CMakeLists.txt:70-82) is untouched.
4. **Windows revalidation flag**: everything is `if(NOT WIN32)` CMake +
   `#ifndef _WIN32` doc.h port sections, but the flip touched doc.h/doc.cpp heads
   — put it on the Windows-VM revalidation list exactly like the display flip.
5. Update CLAUDE.md + the memory notes (campaign-b, portable-twin-drift,
   portable-linelist-ownership are all partially stale after this).

**Green criteria:** clean-tree clone builds all targets; ctest 21/21; packaging
run (`packaging/build-app.sh`) produces a working .app; full GUI regression pass
(worlds, plugins incl. Quow layout, all six script languages, sound, chat,
miniwindows); coverage job green with re-baselined numbers.
**Rollback:** the restructure lands as: (a) source-set moves with targets intact,
(b) mushapp dissolution, (c) doc_stub.h deletion — three separately revertable
steps, in that order.
**Risk notes:** medium. The technical risk was retired in Stages 7–8; what's left
is CMake bookkeeping with two known traps (coverage rename, PAL-selftest link
placement) and the singleton collapse (exactly one globals TU must survive:
mushapp_globals.cpp vs windows_stubs.cpp's Frame vs headless_app.cpp — per-link
audit).

---

## Open decisions (resolve at the flagged stage, not before)

| Decision | Stage | Recommendation |
|---|---|---|
| `m_tWorldOpenTime` vs existing `m_whenWorldStarted` | 2 | retire onto existing member if test churn is small |
| `RegisterPortableLuaExtras` (`world.ExpandVariables`) fate | 4 | register always off-Windows (additive API; zero Windows impact) |
| GetTitle semantics: stub returned `m_mush_name`, CDocument shim returns `m_strTitle` | 5 | set m_strTitle from m_mush_name at load; audit call sites |
| Message-map closure: adopt dialog TUs vs hand-stub list | 7 | adopt (option (a)); stub nothing new by hand |
| Status line owner: IWorldView::setStatus vs IAppShell | 7+ | decide before handler bodies get exercised by tests; PAL.md leans doc-scoped |
| mainfrm.cpp adoption vs ~15 frame no-ops | 3/9 | no-ops first, revisit at 9 |
| IHostUi/IAppShell/ISound formalisation | post-9 | land the flip on null hooks first; introduce interfaces as refactors of *working* code (Lens 3 sequencing) |

## Effort estimates (honest, focused-work days; GUI verification included)

| Stage | Scope | Estimate |
|---|---|---|
| 1 | Hygiene + drift kill (incl. Trace parity fix) | 0.5–1 d |
| 2 | doc.h port decls + per-TU self-sufficiency + CMultiDocTemplate | 0.5–1 d |
| 3 | Headless support TU, AddToMap adoption, App/Frame prep | 1.5–2.5 d |
| 4 | 63-TU batch flip + body-guard re-homing | 1–2 d (probes held) / +1 d slack for unprobed TUs |
| 5 | doc_portable.cpp + port-only TU flip, Frame-macro death | 1–2 d (App link unknown lives here) |
| 6 | Test flip + lockstep + clean-rebuild validation | 1 d |
| 7 | doc.cpp + doc_construct adoption, link closure, Serialize rewire, doc_portable deletion | **3–5 d** (the wide-tail stage) |
| 8 | GDI shape collapse + font-metrics fake | 1–2 d |
| 9 | doc_stub.h deletion + one-library CMake + coverage/docs | 2–3 d |
| **Total** | | **~12–20 focused days** (2.5–4 working weeks with verification discipline) |

## Top 5 things most likely to go wrong

1. **Stage-7 link-closure fan-out.** doc.cpp's message map + dispatch map take the
   address of all 518 afx_msg handlers; the ~70-method prefs family lives in
   mushapp-only dialogs/world_prefs/configuration.cpp, and every adopted dialog TU
   can pull in further view/dialog symbols. This is the estimate's fat tail — if
   the scratch-link inventory shows the transitive closure dragging in most of
   dialogs/**, the "adopt, don't stub" rule turns Stage 7 into a week on its own.
2. **The App/Frame global link gap** (the probe's declared blind spot).
   -fsyntax-only proved nothing about linking a real-typed `CMUSHclientApp App`
   headless; the headless ctor in `headless_app.cpp` must reproduce the stub
   ctor's parity work (MXP_Load*) and satisfy CWinApp-shim vtable/member expectations.
   Divergent ctor bodies across the two links is also a standing maintenance trap.
3. **Layout-lockstep violations during the mixed window (Stages 2–6).** Any doc.h
   member change not mirrored into doc_stub.h in the same commit, any mc_test
   define drift, or any partial rebuild leaving stale .o layouts in
   libmushcore.a reproduces the historical no-symptom-until-SIGSEGV destructor
   crash. Mitigation is procedural (same-commit mirrors, clean rebuilds at stage
   boundaries) — i.e. exactly the kind of discipline that fails once.
4. **Silent behaviour re-baselines from real ctor/dtor/no-op removals.** The flip
   deliberately changes headless behaviour in ~8 places (Trace no longer
   swallowed, AddToMap live, m_bInSendToScript/m_bDisconnectOK/custom-colour
   defaults, real dtor freeing formerly-leaked MCCP buffers and running CloseLog,
   Serialize becoming the live load path). Each is a *fix*, but each can flip a
   test baseline or expose a latent use-after-free (the m_LineList free-ordering
   history) — misread one as "tests flaky" and a real regression ships.
5. **The GDI shape collapse (Stage 8) breaking mushgui rendering subtly.** Turning
   the mfc_ui.h GDI classes from two ODR shapes into one decl-only shape touches
   every paint path in the product; an inline-vs-out-of-line mismatch or a missed
   `#ifndef MUSHCLIENT_FULL_BUILD` site (there are at least 5 regions: 2023-2482,
   3521, 4278-4304, 6890, 7033-7053) fails not at build time but as wrong pixels
   or a crash in wx_gdi_compat.cpp. The PAL surface/miniwindow selftests are the
   canary — run them plus a real Quow-plugin visual check before and after.
