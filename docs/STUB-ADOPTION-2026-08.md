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
3. ~~**Miniwindow stage 1 (47)**~~ — **done**, `650edaa`. Three one-line
   `CBitmap` bodies in `gdi_headless.cpp`, `LoadPng`/`LoadPngMemory` hoisted to
   `png_load_shared.cpp` (added to `MUSHclient.vcxproj`), four TUs moved.
   `doc_gui_method_stubs.cpp` went from 108 definitions to 21. Fixed a live leak
   — the empty `~CMiniWindow` released nothing, invisible because the members it
   dropped are pointers — and removed the output family's `ValidateBrushStyle`
   blocker for free.
4. ~~**Output/view (22)**~~ — **done**, `ae8b153` + `dec4a81`. All three
   blockers dissolved rather than being solved: `ValidateBrushStyle` came with
   miniwindow, `AutoWrapWindowWidth` is address-only, and the `App.m_pMainWnd`
   "permanent contract" was answered by pointing it at the `CMainFrame` that
   `windows_stubs.cpp` already owns. **`platform/doc_gui_method_stubs.cpp` is
   deleted** — 108 stubs to zero. Surfaced two macOS product gaps (`AddFont`,
   `SetToolBarPosition`) and five more twins, bringing this family to six too.
5. **Headless surface bridge + DIB store** — **done**: the bridge in `ae50c1a`,
   the DIB store in `414d808`.

   `platform/headless_surface.cpp` installs `pal::CpuRaster`, so the miniwindow
   drawing ops paint for real: fill/line/gradient/setPixel/getPixel/blit, and
   fonts realise (CpuRaster's synthetic test font), so `WindowTextWidth` and
   `WindowText` return real measurements. Asserted by reading pixels back, not
   by return codes.

   The ordering was the whole difficulty: `CMiniWindow::Create` fills the
   background BEFORE adopting, so a headless `adopt` that *created* a surface
   would discard every miniwindow's background colour. Ownership therefore
   follows GDI's model — the bitmap owns the pixels, `SelectObject` associates
   the DC, `adopt` only looks up — which also makes
   `g_releaseAdoptedSurface` necessarily inert.

   **The DIB store** (`414d808`) is the image family's pixel source, and it came
   out much smaller than the wx version it mirrors, because a DIB here is a
   short-lived raw buffer with no drawing behaviour: nothing renders through
   one, so there is no `wxBitmap` to keep in step. `::CreateDIBSection`
   allocates (sign of `biHeight` picks the row order, rows pad to 4 bytes) and
   `CBitmap::Attach` turns the buffer into a `CpuRaster` surface, consuming the
   handle the way wx's `MaterializeDIBSection` does. `world.WindowLoadImage`
   now loads a real PNG, `DrawImage` blits it, `ImageInfo` measures it, and the
   two alpha ops read its alpha.

   Three things worth carrying forward:

   - **`CBitmap::GetBitmap` must zero its output before anything else.** The ten
     call sites in `miniwindow.cpp` declare `BITMAP bi;` uninitialised and
     several branch on `bi.bmBitsPixel` without checking the return, so a bare
     `return FALSE` decides those branches from stack garbage.
   - **The depth check in `Attach` is load-bearing only ABOVE 32bpp.** Below 24,
     a buffer fails `PixelBuffer::valid()` on its own (its stride is too small
     to be a BGR24 row), so dropping the check still rejects 8 and 16bpp — which
     is why the first mutation of it survived. At 48bpp the stride is comfortably
     large enough and the check is the only thing stopping a garbled read.
   - **`pal::CpuRaster` had to stop pinning alpha in `writePixels`,** and the
     rule it took is the one the wx adapter already used: the *format* decides.
     `BGRA32_Alpha` carries byte 3, plain `BGRA32` and `BGR24` pin it, in both
     directions. `isurface.h`'s "`_Alpha`: readPixels only" was already stale —
     `wx_surface.cpp:266` derives `wxgdi_write_bgra`'s `hasAlpha` flag from
     exactly that format comparison. Drawing ops still pin, so alpha cannot
     survive being painted over, matching GDI.

   Still not covered: `WindowMergeImageAlpha` (the two-image mask variant),
   `BlendImage`, `WindowFilter`, `WindowTransformImage`, `WindowImageOp` and
   `WindowCreateImage`. **Four of those six are "only a matter of writing the
   assertions"; two are not, and an earlier version of this line said all of
   them were.** Merge/Blend/Filter touch only `readPixels`/`writePixels`, and
   `TransformImage` needs the matrix blit, which *is* implemented
   (`pal/cpu_raster.cpp`'s `blit` `if (m)` arm — `cpu_raster.h`'s gap list was
   stale about it). `WindowImageOp` and `WindowCreateImage` are blocked one
   layer down, in `CpuRaster`: `shape` draws only `ShapeKind::Rect`, and
   `fillSpan` silently drops `BrushSpec::monoPattern`. The mono gap is wider
   than `ImageOp` — `BrushSpecFromWin32` (`miniwindow.cpp:583-588`) means
   `CircleOp`/`Polygon`/`RectOp` brush styles 8-12 fill nothing headless too,
   which is worth knowing before pricing the ~8-line fix.

   `WindowDrawImage` **mode 2** is a third case, and the interesting one: the
   obvious reading is that a stretch cannot be tested against an adapter that
   refuses to stretch. That is wrong — `miniwindow.cpp:1471-1478` computes its
   destination and source rects from *independent* expressions, so an
   equal-extent fixture passes `CpuRaster`'s guard and the arm really draws,
   pinning that mode 2 alone derives its destination through
   `FixRight`/`FixBottom`. Only genuine magnification is out of reach.
6. **`scripting/scripting.cpp`** — carries a real behavioural question (see
   below), so keep it separate.
7. **`dialogs/ProgDlg.cpp`** — opportunistic; retires 14 definitions.

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


## 5b. Live constraints on the miniwindow path (do not lose these)

These lived as comments in `platform/doc_gui_method_stubs.cpp`, which is deleted
now that it holds no definitions. Stage 2 is done, but they are constraints on
any *further* work on this path, not history.

**Keep the `eOK`-on-null-surface arms exactly as they are.** Every drawing site
in `miniwindow.cpp` is guarded `if (!srf) return eOK` (or the family's equivalent).
That guard is what makes stage 1 safe without a rasteriser — the window exists,
its geometry and hotspots are real, and only the pixels are absent. It is also
the arm **both shipping platforms** take if an adapter ever fails to install, so
turning it into an error would be a real behaviour change on Windows too, not a
headless-only tightening.

**One exception, and it is still a defect — just an unreachable one.**
`WindowWrite` (`miniwindow.cpp:1246-1247`) returns `eOK` with no surface, so it
claims to have written a PNG it never wrote; every other surface-dependent
method merely fails to paint. The note here used to say "do not let it stay
`eOK` once a headless rasteriser exists, because then the claim becomes
checkable and wrong". The rasteriser exists now, and the answer turned out to be
that **the arm cannot be reached on any platform**: every frontend installs an
adapter before any script runs (headless from a file-scope constructor, wx from
`InitInstance`, Windows from its own), so `pal::g_surface` is never null by the
time a script can call `WindowWrite`. Left as-is deliberately — it is shared
code and the change would be unobservable. The same shape sits two lines below
at `miniwindow.cpp:1263`, where a failed `readPixels` also answers `eOK` under a
"cannot happen" comment; that one becomes reachable if `CreateCanvasBitmap` ever
fails, which is the more plausible of the two.

**Do not widen `pal::g_adoptSurface(void*, int, int)`.** It is shared with
Windows (`pal/gdi_surface.cpp`). The headless side must key its map on the
`CBitmap` pointer instead, leaving `platform/mfc_ui.h` layout untouched.

**`CreateCanvasBitmap` / `Attach` / `GetBitmap` / `DeleteObject` and
`::CreateDIBSection` are no longer in `platform/gdi_headless.cpp`** — stage 2
moved them to `platform/headless_surface.cpp` with real bodies over
`pal::CpuRaster`. What is left in `gdi_headless.cpp` (`CreateCompatibleBitmap`,
`CreateBitmap`) is still contract rather than placeholder — **but not for the
reason first recorded here.** That reason was "nothing in mushcore calls
either", and both halves of it were false: `CreateBitmap` has six unguarded
callers in `miniwindow.cpp`, which IS in mushcore (the five built-in mono brush
patterns in `ValidateBrushStyle` at `:498-538`, plus `CreateImage` at `:2000`);
`CreateCompatibleBitmap` is called from `CMiniWindow::Resize` at `:4039`. Only
the canvas call at `:239` is `#ifdef _WIN32`.

The real reason is better: the port keeps each mono pattern in a **parallel
portable store** (`m_MonoImages`, `miniwindow.cpp:2005-2011`) so `ImageOp` can
rebuild a real mono brush through `pal::BrushSpec` and never reads the
`CBitmap`. The `CBitmap` failing is by design; the gap is one layer down, where
`CpuRaster::fillSpan` drops `monoPattern`. `tests/test_mfc_ui.cpp:2559-2560`
pins both returns, so changing either means editing that test first.

This mistake is worth leaving on the record. It is the *blocker stated at the
wrong layer* pattern in its other guise — a claim about who calls a function,
made by reasoning about what the function is *for* rather than by grepping for
its callers.

## 5c. Found while covering the image ops (2026-08-07)

The four unblocked ops are covered (`ad434ad`, 17 tests, 61 mutations, 56
killed). Two findings outlast the tests.

### `CpuRaster`'s surface-to-surface blit silently mis-drew a stretch — FIXED

`pal/cpu_raster.cpp`'s `SurfaceId -> SurfaceId` overload **overwrote the
caller's destination extent with the clipped source extent** before delegating:

```
    dstRect = { dstRect.x + (clipped.x - srcRect.x),
                dstRect.y + (clipped.y - srcRect.y), clipped.w, clipped.h };
```

so the `PixelBuffer` overload's no-stretch guard compared two values that could
never disagree. **The guard was dead code**, and a stretch was not declined but
answered with a 1:1 blit at the destination origin: a requested 2x1 destination
painted a whole 9x5 image.

That is worse than an unimplemented gap. A no-op is visibly missing; a wrong
answer at the right place looks correct, so a test written against it passes and
pins the wrong behaviour. **This actually happened**: the mode-2 test written
hours earlier had assertions describing the 1:1 mis-draw, and implementing the
stretch turned them red. Its first call was safe only by luck — the fixture made
the destination extent 9x5, exactly the image size, so that call was never a
stretch at all.

**Fixed 2026-08-07 (Wade's call: implement it).** Nearest neighbour, GDI's
COLORONCOLOR rule, in the `PixelBuffer` overload; the surface overload now keeps
the caller's extent when the extents differ. Two COMBINATIONS stay declined,
both matching the wx adapter's own S2 gaps so the two agree: a stretch whose
source rect was clipped (`wx_surface.cpp:728`) and a colour-KEYED stretch
(`:734`). Six mutations, all killed — including reinstating the original defect.

**The constraint this buys, and it must not be forgotten:** the stretch
resamples but does not interpolate, so it will never reproduce the wx adapter's
HALFTONE (`BlitQuality::Smooth`) colours. A test over a stretch must assert
**placement and extent**, never an interpolated colour value. The shipping
rasteriser remains the parity bearer for the latter.

### `BlendImage` has an unreachable-looking silent-success arm

`if (!bufA.valid () || !bufB.valid () || bufA.bytes.size () != bufB.bytes.size ())
return eOK;` — a size mismatch produces no blend and no error. Every route to it
appears headed off by the earlier clamps, but that argument could not be
completed, and a refactor of those clamps would make it reachable with no test
noticing. Recorded rather than tested.

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
(`platform/doc_flip_gui_stubs.cpp:147` and `platform/doc_port_glue.cpp:420` —
these line numbers were stale in the first version of this section) that have
drifted from each other and from `scripting/scripting.cpp:23-101`. Three
differences, none recorded in `docs/UPSTREAM-DIVERGENCES.md`:

1. The re-entrancy guard (`scripting.cpp:29-33`) is absent, so a second call
   overwrites `m_ScriptEngine` and **leaks** the previous one.
2. The real body creates the engine *before* the `m_strScriptFilename.IsEmpty()`
   bail, so on Windows a world with scripting enabled and no script file still
   has a live engine — immediate mode, `world.Execute` — and the port does not.
3. On script-open failure the real body `ColourNote`s and calls
   `DisableScripting()`; the twin prints to stderr, keeps the empty engine, and
   returns **false meaning success**.

### Re-surveyed 2026-08-07, and the question is not the one above

All three still hold at HEAD. But the framing was wrong in the way that decides
whether to act, and a fourth difference outranks all of them.

**This is a headless-only cleanup. It fixes no product bug.**
`scripting/scripting.cpp` is already in `MUSHGUI_ADAPTERS_SOURCES`
(`CMakeLists.txt:587`), so the macOS *product* has run the canonical body all
along; both twins live in `mushcore_testsupport` alone. The two are not
one-per-consumer either — they are two copies in ONE consumer serving two entry
paths (`CMUSHclientDoc::CreateScriptEngine` from `OpenSession`, and the
file-local `PortCreateWorldScriptEngine` from the port-only `LoadWorldFromFile`,
which is the path 98 of the ~100 test world loads actually take).

**The blocker, which the first survey missed entirely:
`xml/xml_load_world.cpp:527-528`.** Canonical `FindAllEntryPoints` returns
**true** — meaning "errors found" — for any unresolvable procedure name
(`scripting.cpp:222-234`, via `strErrorMessage` accumulation), and the plugin
loader turns that into `ThrowErrorException ("Could not find all required script
routines")`. The port's twin hard-returns `false` **on purpose**, and says so at
`platform/doc_port_glue.cpp:403-405`. So adopting the canonical body makes
headless plugin loads start FAILING where they succeed today, and constructs a
`CMissingEntryPoints` + `DoModal` during load. On Windows the throw is exactly
what happens, so the twin is *port-invented behaviour* — but the blast radius
lands entirely on the test suite.

Three further things anyone acting on this must know:

- **`tests/test_world_load.cpp:692`** (`WorldLoad.NoScriptNoEngine`) asserts
  `EXPECT_EQ(nullptr, doc.m_ScriptEngine)` for a scripting-enabled world with no
  script file. That assertion **is** difference (2). The "decision" this section
  asks for is not a design question in the abstract — it is whether to flip that
  line. (Found by the *grep the test suite for the stub's contract* check; the
  other pin is `tests/test_lua_scripting.cpp:1937-1982`.)
- **Blast radius is ~46 test blocks, not two.** Any `test_world_load.cpp` block
  with no `<world>` tag keeps the ctor's `enable_scripts = TRUE`, so after
  adoption it gains a real Lua state and flips `doc_port_glue.cpp:558`'s
  `if (m_ScriptEngine)` gate. Budget a full-suite triage, not a two-test fix.
- **The default reload answer inverts.** `m_nReloadOption` defaults to
  `eReloadConfirm`, and headless `MB_YESNO` answers `IDNO`
  (`HeadlessMessageBoxAnswer`), so after adoption the default headless script
  reload is **declined**, where the twin always reloads.

Three drifts beyond the recorded three, all verified:

4. The twins set the port-only `m_scriptFileMtime` and never
   `m_timeScriptFileMod`, so `world.GetInfo`'s script-modification item
   (`scripting/methods/methods_info.cpp:1147`) is **permanently dead headless**.
   The tests missed it because they assign the member by hand.
5. Both twins are **Lua-only** and stderr-reject any other language, five years
   after the other five engines started working.
6. The port's `OnScriptFileChanged` ignores `m_nReloadOption` entirely.

There are **six** doc-level `CreateScriptEngine` call sites, not three (add
`dialogs/world_prefs/configuration.cpp:1740`, `platform/wx_mfc_impl.cpp:1932`,
`scripting/scripting.cpp:412`); all six are safely gated, so difference (1) is a
latent hazard rather than a live leak. Adoption needs **nine** stub deletions in
the one commit, and the proof afterwards is `nm -A` over `libmushcore.a` +
`libmushcore_testsupport.a` showing exactly one definition per symbol — not a
green suite, which is what the §5a hazard produces.

**Recommendation: adopt, but as its own deliberate piece of work with a triage
pass budgeted, and only once the plugin-load throw is a decision rather than a
surprise.** It buys the shipping TU its first headless coverage and kills six
drifts, but it is a test-fidelity change, not a product fix, and it is the only
remaining item that can turn the suite red for reasons that are *correct*.

## 8. A documentation correction

CLAUDE.md credits `ccd094b` (the `StepIt`/`OffsetPos`/`SetWindowText` fix) with
making "the plugin-loading bar" move. **It cannot have.** The plugin-loading
progress block at `xml/xml_load_world.cpp:734` sits inside
`#if IT_GET_GOATS_BLOOD_OUT`, and that macro occurs exactly once in the code
tree — at that `#if`. It is defined nowhere, so the block is dead code on every
platform, Windows included.

The fix was still real and still worth having; it just benefited the *other*
`CProgressDlg` users — Recall (`doc.cpp:2432`), Send-to-world / paste-file
(`doc.cpp:1429`), Find and Global Change (`Finding.cpp:169`), the world-prefs
Info page's *Calculate memory usage*, and the Lua `progress` library
(`scripting/lua_progressdlg.cpp`) — rather than plugin loading. Worth
correcting because the claim reads as evidence that the progress bar is
exercised during world load, which would make it a natural place to hang an
adoption test — and it is not.

**Landed 2026-08-07** (`eb39282` for the adoption, and the corrections with it).
The false claim had been copied into **five** places, not one: `CLAUDE.md`,
`dialogs/ProgDlg.cpp`, `dialogs/ProgDlg.h`, `platform/wx_mfc_hooks.h` and
`platform/gui_bridge_selftest.cpp`. All five are fixed. That spread is the
lesson worth keeping — a wrong explanation propagates into comments faster than
a wrong line of code does, because each copy looks like corroboration for the
next reader.
