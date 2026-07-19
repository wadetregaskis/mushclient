# Phase 2 Recovery — Decision Memo

**Date:** 2026-07-19
**Subject:** How (and whether) to finish putting `mushcore` on the real `doc.h`
**Status:** the incremental staged flip stalled — Stage 4 (xml batch only) built clean
but SIGSEGV'd 4 test binaries (`e3de755`); `doc_stub.h` is now *proven*
layout-incompatible with `doc.h`.

This memo is a synthesis of four measurement lenses (A-feasibility, B-scope,
exists+doc.cpp, risk+worth-it). Every scope number below is cited to a lens
finding and, where load-bearing, to `file:line`.

---

## 0. The stall, in one paragraph

`mushcore` is compiled `-DWIN32 -D_NOSOUND` **without** `_WIN32` or
`MUSHCLIENT_FULL_BUILD` (`CMakeLists:580-584`), so its 63 include-switched TUs take
the `#else → platform/doc_stub.h` arm. `doc_stub.h`'s `CMUSHclientDoc`
(`doc_stub.h:1944`, `class CMUSHclientDoc {` — **no base**, first member
`m_normalcolour[8]` at `:1947`) has a **different memory layout** from the shipping
`doc.h`'s `CMUSHclientDoc` (`doc.h:632`, `: public CDocument` — vtable + base
subobject, first member `m_pTransport` at `:665`). Different base, different member
order. There is **no** `static_assert(sizeof)` and **no** `offsetof` guardrail
anywhere in the tree, so a TU left on the old layout during a partial flip fails
only at **runtime** (SIGSEGV writing past the allocation). That is exactly what the
Stage-4 attempt hit. The residual routes are therefore: **A** converge the layout
first, **B** big-bang the whole flip, or **Stop**.

---

## 1. The three options, with measured scope

### Option A — Converge `doc_stub.h`'s layout onto `doc.h`, then flip incrementally

**What it is:** rewrite `doc_stub.h`'s `CMUSHclientDoc` so its base + member order +
types are byte-identical to `doc.h`, giving a mixed-layout-safe window in which TUs
can be flipped a batch at a time.

**Measured scope (Lens A):**
- The two class bodies are *already* nearly closed. Robust unique-`m_`-token diff:
  `doc.h`-only = **75** tokens (**~71** real GUI-only members after removing 4
  dead/commented); `doc_stub.h`-only = **9** tokens reducing to **exactly one**
  genuine data member — `m_strPathName` (`doc_stub.h:2225`), which exists *only*
  because the stub lacks the `CDocument` base that would supply it.
- The members you'd *expect* to be port-only — `m_gui_*`, the `IWorldView` seam, the
  transport, `m_scriptFileMtime` — are **not** stub-only. `doc.h` already carries
  every one, unguarded: `m_pTransport`/`m_pTransportEvents` (`doc.h:665-666`),
  `m_gui_lines_emitted` (`:1512`), `m_pWorldView` + seam (`:1521-1691`),
  `m_gui_rows/cols/font_height/font_width` (`:1695-1698`), `m_gui_paused` (`:1702`),
  the Stage-2 port block (`:1890-1913`).
- Layout-match is **not a reorder — it is a from-scratch reconstruction** of the
  full ~400-520-member data section in `doc.h`'s declaration (version-history) order,
  because the stub is grouped by feature-family. You interleave the ~71 GUI members
  at their `doc.h` offsets, drop `m_strPathName`, and add the `CDocument` base.
- **No compile-time guardrail is possible** (Lens A, Q5): the two same-named classes
  cannot coexist in one TU, so an in-TU `offsetof` assert is impossible; a
  compiled-twice offset-dumper is the only mechanical option, but `CMUSHclientDoc` is
  non-standard-layout (polymorphic base; `CString`/`std::map`/`CTime` members), so
  `offsetof` is conditionally-supported and unreliable. The de-facto tripwire stays
  the runtime SIGSEGV.

**Honest effort:** roughly **big-bang's hardest 90% (reconstruct the layout by hand)
PLUS the eventual delete of the twin** — strictly *more* work than B, spread over a
hand-maintained header that must stay byte-identical to `doc.h` forever on pain of the
same SIGSEGV. Call it the Stage-7 layout reconstruction (a chunk of the 3-5 day
"wide-tail" stage) *before* you even begin the incremental flips B would skip.

**Top-3 risks:**
1. The hand-copied layout silently drifts from `doc.h` on any future `doc.h` change →
   the exact no-symptom-until-SIGSEGV trap, now made *permanent* rather than retired.
2. No guardrail exists to catch (1) at build time; every regression is runtime-only.
3. Sunk-cost lock-in: having paid to reconstruct the layout, the twin still must be
   deleted, so the risky link-closure work (below) is merely deferred, not avoided.

### Option B — Big-bang: flip every doc TU to real `doc.h` atomically

**What it is:** in one commit, define `MUSHCLIENT_FULL_BUILD` for core; fix
`regexp.cpp`'s `_WIN32`-only switch; convert the 4 port-only TUs; activate
`MUSHCLIENT_HEADLESS_APP`; retype `g_stub_frame`; **delete `doc_stub.h` +
`doc_portable.cpp`**; and adopt real `doc.cpp` + `doc_construct.cpp` headless. No
mixed-layout window ever exists.

B has **two separable pieces** the task partly conflates (Lens B):

**Piece (A) — the header flip (nearly de-risked):**
- **87** TUs `#include doc_stub.h`: 63 shared include-switched (xml 3, mxp 14,
  scripting/methods 25, top-level 21) + 4 port-only + 20 test.
- **Member-home gap = 0.** A comment-filtered declared-member diff (`doc.h` 533 vs
  `doc_stub.h` 463) leaves one residual name, `m_strPathName`, satisfied by the
  `CDocument` base. Stage 2 already landed `m_gui_lines_emitted`/`m_pWorldView` in
  `doc.h`. **No new `doc.h` members needed.**
- **Inline-body gap = 0.** The stub's 86 inline-bodied methods all have post-flip
  homes: 76 already inline in `doc.h`, 8 from the `CDocument` shim
  (`mfc_ui.h:3021-3075`), 5+2 from `doc_headless_stubs.cpp`. The separate 116-method
  GUI family already has out-of-line bodies staged in `doc_gui_method_stubs.cpp`
  (784 ln).
- The `regexp.cpp` `_WIN32`-only switch is the one shared TU that won't flip by the
  mechanical "delete the `#else`" recipe — it needs a `FULL_BUILD` arm added.

**Piece (B) — doc-under-test (carries the one high-risk stage):**
- Compile real `doc.cpp` (4056 ln) + `doc_construct.cpp` (839 ln) headless and delete
  `doc_portable.cpp` (1279 ln). `mushapp` **already compiles both off-Windows daily**,
  so *compilation* is proven; the open cost is **`mushcore` link closure**, not
  compile (Lens 3).
- The portable ctor **cannot** simply be re-headered: it leans on `doc_stub.h`'s
  **391** in-class member initializers; `doc.h` carries only **10**, so ~380 members
  would be uninitialized garbage. The big-bang **must adopt** `doc_construct.cpp`'s
  real ctor (255 explicit body assignments) whole (Lens 3). The real ctor/dtor are
  *more* correct than the stub (fixes MCCP-buffer/`CloseLog` leaks; sets
  `m_bInSendToScript=true`, real custom-colour defaults) — but that means **~8 headless
  behaviours re-baseline at once**.
- Concrete adoption blockers (Lens 3): `CPrefsP*::eColumnCount` (5 refs,
  `doc_construct.cpp:388-392`) needs enum-only decls exposed headless
  (`prefspropertypages.h` is `#ifdef _WIN32`-only in `doc.h:9-12`);
  `SetDefaultAnsiColours`/`SetDefaultCustomColours` live in `Utilities.cpp`, which
  `mushcore` does not compile.

**Honest effort:** PHASE2-PLAN estimates the full flip (Stages 1-9) at **~12-20
focused days**; Stage 7 (adopt `doc.cpp`+`doc_construct.cpp`, delete
`doc_portable.cpp`) alone at **3-5 days**. Scope of the atomic commit: **100+ files**
vs the largest precedent flip's **11**.

**Top-3 risks:**
1. **Stage-7 link-closure fan-out** — `doc.cpp`'s message map (89 entries) + dispatch
   map take the address of all **518** `afx_msg` handlers; the gap beyond the
   116+15 already stubbed concentrates in a **~70-method world-prefs family** defined
   in the **mushapp-only** `dialogs/world_prefs/configuration.cpp`. Each adopted
   dialog/view TU can transitively pull in more symbols. This is **unknowable until
   you actually link** — the true schedule driver, not the header swap.
2. **GDI ODR shape fork** — `mfc_ui.h`'s `CDC/CBitmap/CPen/CBrush/CFont` are two
   different class *shapes* under `MUSHCLIENT_FULL_BUILD` (`mfc_ui.h:2023-2482` +
   ≥4 more regions). A single-library link is impossible until they are collapsed to
   one shape, and a bad collapse fails as **wrong pixels / crash at runtime** in
   `wx_gdi_compat.cpp`, not at build.
3. **Missed-TU SIGSEGV + forfeited bisectability** — 100+ TUs must flip with **zero**
   stragglers, with no guardrail; and the atomic landing gives up the per-subsystem
   green checkpoint that made the three precedent flips safe. (Note: the big-bang is
   the one path with *no* mixed-layout window, so this is a *landing* risk, not a
   design flaw — see §2.)

### Option Stop — Keep `doc_stub.h`; stop after Stage 3

**What it is:** accept two libraries. `mushcore` keeps `doc_portable.cpp` + the stub;
the shipping app keeps `doc.h`. Revisit only if a future `doc.h` change makes the
lockstep tax painful.

**Measured scope of what Stop leaves on the table (Lens 4):**
- Residual `doc_portable.cpp` = **1279 lines / 19 `CMUSHclientDoc` methods** (down from
  2186 pre-display-flip): ctor/dtor, thin drivers (`LoadWorldFromFile` over real
  `Serialize`, `Load_Set`), GUI stubs (`GetChildFrame`), COM-unseamable stubs
  (`SeeIfHandlerCanExecute`), sound routing, `OnScriptFileChanged` mtime poll. This is
  **glue, not the drift-prone business logic** Campaigns B/C already deleted.
- Ongoing cost: a layout-lockstep discipline tax on every future `doc.h` member change
  (+ full rebuild or the stale-`.o` SIGSEGV trap), a 63-TU double compile, and a
  residual **test-fidelity** risk (the headless engine can silently diverge — and
  *has*: the stub-only `Trace(CString){}` swallowed trace lines, fixed `56e5203`;
  `AddToMap {}` disabled typed-direction mapping headless, fixed `71f9673` — **both
  headless-only**).

**Honest effort:** **~0 incremental** (the work is already banked at Stage 3). The
cost is a bounded, ongoing discipline tax the team already knows how to detect
(diff twin-vs-original).

**Top-3 risks:**
1. A future `doc.h` change is mirrored wrong into the stub → runtime SIGSEGV in tests
   with no compile error (the known trap, but bounded and detectable).
2. The headless test engine silently behaves unlike the shipping product on some new
   path, masking or inventing a bug (test-fidelity gap, est. residual ~10%).
3. Reviewer friction / "unfinished diagram" — the two-library shape is a standing
   invitation to re-open this exact question later.

---

## 2. Is Option A's end-state genuinely distinct from B? (the Lens-1 key question)

**No. Option A is B-with-a-throwaway-scaffold, and it is strictly dominated by B.**

Once A has made base + members + order + types identical, the **only** residue
distinguishing `doc_stub.h` from `doc.h` is the inline stub method bodies — no-op GUI
stubs, plugin-context accessors, `SDisplayFindInfoStub`, and headless decls already
implemented in `doc_portable.cpp` / `doc_headless_stubs.cpp`. That end-state is
definitionally *"`doc.h`'s data layout re-typed by hand + a bolt-on of inline stub
bodies"* — which the existing stub-bodies TUs already provide out-of-line. A converges
the stub **onto** `doc.h`; the rational terminal move from there is to **delete the
twin header and `#include` real `doc.h`** — which is precisely B's flip.

So A does **the hardest 90% of Phase 2 (reconstruct the layout)** while *keeping the
very twin Phase 2 exists to delete*, and it buys the incremental window at the price
of a permanently hand-maintained layout with **no** compile-time guardrail. A is not a
distinct architecture; it is a more expensive road to B's doorstep. **Eliminate A.**

---

## 3. Are both user goals already met? (is Stop defensible — Lens 4)

**Yes, both are substantially met, and Stop is defensible.**

- **Goal (a): business logic compiled identically everywhere.** `mushcore` already
  compiles the **real** shipping TUs headless — `lua_methods.cpp` (434-method API),
  `xml_load_world`/`xml_save_world`, the option machinery + 244 option members, the
  display/note/send engine (`doc_shared.cpp`/`doc_display_shared.cpp`, single-sourced
  *verbatim* from `doc.cpp`), the `methods_*` family, plus ansi/telnet/mxp/triggers/
  aliases/timers/regexp/plugins. The high-value drift twins are **already dead**.
- **Goal (b): testable headless.** **Fully** met — 1,426 tests, ~90.5%
  reachable-adjusted shipping-line coverage, `ctest` 21/21.

What Phase 2 still delivers on top is: **one library instead of two**, retirement of
the lockstep tax, and closing the last **~10%** test-fidelity gap (ctor/dtor parity;
real `Serialize` as the live load path). That is real, but it is **architectural
tidiness + diminishing-returns fidelity — not a new user capability and not the bulk
of either headline goal.** The goals were the reason to *start*; they are now mostly
*achieved*. Stop forfeits **nothing the user actually asked for**; its cost is a
bounded, detectable discipline tax.

---

## 4. Recommendation

**Kill A outright. Adopt Stop as the default posture now. Keep B as the only sound way
to ever "finish," gated behind one cheap measurement that would flip the decision.**

Reasoning:

1. **A is dominated (§2).** It is more work than B, produces a redundant end-state,
   and manufactures a permanent guardrail-free layout twin. There is no scenario in
   which A is the right choice — every reason to do A is a reason to do B instead.

2. **Stop is defensible today (§3).** Both goals are banked; the residual
   `doc_portable.cpp` is 1279 lines of glue, not logic; the product is **immune by
   construction** to stub drift (it never compiles the stub). "Stop after Stage 3,
   keep `doc_stub.h`, revisit only if a `doc.h` change makes the lockstep tax painful"
   is a legitimate, low-risk resting point — not a failure.

3. **If Phase 2 is finished at all, it must be B, and B must be a true big-bang — not
   the staged incremental flip.** The staged premise was *proven unsafe*: Stage 4's
   xml-only flip built clean and SIGSEGV'd 4 binaries (`e3de755`) because of the
   mixed-layout window. The big-bang is the one approach in which **no mixed-layout
   window ever exists**, so it is paradoxically the *safer* design even though it is
   the *least* landable-in-one-shot.

4. **The real unknown is link closure, not the header flip.** B's schedule is
   dominated by the Stage-7 fan-out (518 `afx_msg` addresses; ~70-method world-prefs
   family in mushapp-only `configuration.cpp`) and the GDI ODR collapse — both of which
   only resolve at link time. **Before committing to B, spend ≤1 day on a
   scratch/throwaway link target** that: defines `MUSHCLIENT_FULL_BUILD` for core,
   pulls in `doc.cpp`/`doc_construct.cpp`, and **dumps the undefined-symbol list**
   without shipping anything. That converts B's biggest risk into a number.
   - If the closure comes back **small** (the world-prefs family + a handful of
     stragglers, resolvable by *adopting* the headless-compilable dialog/view TUs into
     core rather than hand-stubbing) **and** the team values the one-library
     architecture for its own sake → **greenlight B as a single atomic commit**,
     landing the whole flip + `doc.cpp`/`doc_construct.cpp` adoption + GDI-shape
     collapse together, verified `ctest 21/21` (run twice) on a scratch branch before
     merge.
   - If the closure comes back **large or ambiguous** → **stay stopped.** The marginal
     deliverable does not justify a 100+-file un-bisectable landing.

**Bottom line:** the goals are met; A is a trap; Stop is the honest default; B is worth
doing only if a one-day scratch-link probe proves the link closure is small. Do not let
"finish the diagram" pull the project into a 12-20-day, 100+-file big-bang whose payoff
is tidiness plus the last 10% of test fidelity — unless that closure number comes back
cheap.

---

## 5. Link-closure probe result (the §4 measurement, executed 2026-07-19)

Ground-truth `ld -force_load` probe (layout-independent; net of the 116+48 already-staged
stubs): a "frankenlib" mushcore with `doc.cpp`+`doc_construct.cpp`+`Utilities.cpp` (real,
`MUSHCLIENT_FULL_BUILD`) swapped in for the portable twins.

**NET GUI-only undefined symbols: 78** (≈69 distinct entities) for the doc-only swap;
**93** for the full faithful flip (also flipping lua_utils/world_debug). Plus 18
relocation artifacts = `doc_portable.cpp`'s own glue moving to the staged
`doc_headless_stubs.cpp`/`headless_app.cpp` landing pads (existing code, not new work).

Disposition of the 78:
- **~66 pure no-op stubs** — extend `doc_gui_method_stubs.cpp`'s existing pattern. The
  memo's feared "~70-method world-prefs family" is really **26 address-only command
  handlers** (message-map takes their pointer; never invoked headless) + 11 dialog
  ctor/vtable pairs.
- **~2 tiny real headless bodies** (`CreateScriptEngine`/`DisableScripting`, ~10 lines).
- **~10 GDI shim entries** (`CDC`/`CFont`/`CBitmap`/`CreateDIBSection` — used by
  `LoadPng`/font metrics) → a bounded headless GDI shim over the existing
  `pal::ISurface`/`cpu_raster` CPU path. This is the memo's GDI-ODR risk #2, now scoped
  to ~10 entries with an existing CPU backend.
- **Zero genuinely un-seamable symbols.** No live-GUI-object dependency.

**VERDICT: B's link closure is SMALL and stub-resolvable — the §4 greenlight condition
is MET.** Confidence HIGH (ground-truth linker output, verified net of staged stubs, zero
scripting-engine contamination). Caveats: `-force_load` is an upper bound (real link pulls
a subset); the count does NOT de-risk the two *runtime* Stage-7 concerns — GDI-shape ODR
correctness and the ctor/dtor re-baseline (~8 headless behaviours shift when adopting the
real ctor) — which remain, bounded, whatever path is chosen.

**Net decision inputs:** A dead; Stop fully defensible (both goals met, 0 incremental
cost); B viable with its dominant unknown now quantified-cheap, remaining cost ≈ a large
atomic ~100-file landing + ctor re-baseline + a ~10-entry GDI shim, for one-library
tidiness + the last ~10% of test fidelity. The choice is now a clean value judgment.

---

## 6. DECISION: Option B, incrementally staged (2026-07-19)

Chosen path. The mixed-layout SIGSEGV constraint forces the header+ctor flip to be ONE
atomic commit (no partial-flip window), but everything additive is pre-staged as green
checkpoints to shrink it:

- **B-1 (pre-flip, additive, green on the current stub build):** extend
  `doc_gui_method_stubs.cpp` / `doc_headless_stubs.cpp` with the ~66 no-op GUI stubs +
  the 2 tiny real bodies from §5. Strong defs coexist with the stub's weak inlines
  (harmless now, load-bearing post-flip). Identify what CANNOT pre-stage (GDI shape fork)
  and defer to the flip.
- **B-2 (pre-flip):** the ~10-entry headless GDI shim over `pal::ISurface`/`cpu_raster`,
  IF separable from the GDI-shape ODR fork; else folded into the flip.
- **B-3 (THE FLIP — one atomic commit):** `MUSHCLIENT_FULL_BUILD` for core; flip all 63
  shared + 4 port-only TUs; `regexp.cpp` FULL_BUILD arm; activate `headless_app.cpp`;
  retype `g_stub_frame`; adopt real `doc.cpp` + `doc_construct.cpp`; delete `doc_stub.h`
  + `doc_portable.cpp`; collapse the GDI shape fork. Verified `ctest` ×2 + GUI smoke on a
  scratch branch before merge. The ~8 ctor/dtor behaviour re-baselines land here.
- **B-4+ (post-flip cleanup):** test re-baselines (cited), docs (CLAUDE.md/PAL.md/
  COVERAGE denominator shrinks/PHASE2-PLAN done), optional one-library CMake restructure.
