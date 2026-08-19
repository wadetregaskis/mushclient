# Decision queue — MUSHclient audit campaign

**32 items — all STILL-OPEN.** Every one needs a product/design call from Wade, not a
mechanical fix. Ordered by **user-visible impact**, not by severity label.

Reconciled against HEAD `56c72d5` ("Merge F54: audit round-3 wave-2") on 2026-08-19.
Since the queue was assembled, three fix campaigns ran (F01…F54, ~160 commits, plus the
R4 follow-ups). **None of the 32 decision items was closed by any of them**: every cited
site was read at this HEAD and is unchanged, and no D-item finding appears in any fix
backlog or in the round-1/2/3 *refuted* / *already-done* lists. Two mechanical *halves*
had already landed before the queue was built (D-32's dead `RegisterPortableLuaExtras`
removal, `831da36`; D-29's sibling `.rc` mislabels, `42be7dc`/F17); in both the decision
half is what survives here. See *Resolved since the queue was built* at the end for the
adjacent fixes that touched — but did not close — these items.

`⚡` marks the ten where the fix is already drafted/trivial and only the call is missing —
each **re-verified NOT landed at this HEAD** (if the patch had landed the item would be
resolved, not open). Every item carries a **was D-NN** note so older cross-references
still resolve.

---

### D-01 — Auto-detected plain URLs are no longer clickable ⚡  *(was D-01)*
**Question.** Restore URL auto-detection (one line in `LineAt`, no wrap-metric change), or
accept that on the port only MXP/Lua hyperlinks are clickable and say so?

**Context.** `DetectURLs` still runs on every appended line (`wx_outputpane.cpp:493`), but
under the pull model the line's content is dropped and the pane re-fetches it from the
engine in `LineAt` (`:1102`), which never runs the detector — so a bare `http://…` in MUD
output is neither coloured/underlined nor clickable, where Windows makes it a link. It is
not mechanical because it is a visible behaviour the port has shipped without for a while;
someone has to say whether the port's link surface is "explicit hyperlinks only".

**Refs.** P03-F2 (MODIFIED, medium, port) · `platform/wx_outputpane.cpp:343,493,1102`

---

### D-02 — Colour-matching triggers stop working on any non-ANSI line  *(was D-02)*
**Question.** Change the matcher to consult `iFlags & COLOURTYPE` (fixing colour triggers on
xterm-256/MXP/RGB output, and altering a matcher every colour trigger goes through on both
platforms), or leave a 2010 upstream behaviour alone until a Windows run shows what users
depend on?

**Context.** `ProcessPreviousLine.cpp` (~1082-1090) compares the trigger's 0-7 ANSI index
straight against `iForeColour`/`iBackColour` without checking the style's colour type. The
moment a style becomes `COLOUR_RGB` (xterm-256, 24-bit, MXP colour tags, or an earlier
trigger's own colour change) those members hold a COLORREF, so conditions on colours 1-7 can
never match — while black keeps matching by coincidence and `COLOUR_CUSTOM` compares a
palette index as if it were an ANSI index. Unmodified upstream; the risk of "fixing" it is
that a large body of existing triggers is tuned to today's behaviour, which only a Windows
run against a real xterm-256 world can show.

**Refs.** E10-F3 (MODIFIED, medium, upstream, cluster C49; deferred group 3) ·
`ProcessPreviousLine.cpp` colour-match block · trigger-dialog clamp
`dialogs/world_prefs/prefspropertypages.cpp` (`IDC_TRIGGER_COLOUR`)

---

### D-03 — New/opened worlds flip `utf_8` against the option table ⚡  *(was D-03)*
**Question.** Delete the seeding line so the constructor's `SetDefaults(false)` stands
(worlds already flipped change behaviour on their next open), or keep UTF-8-by-default on
macOS and record it as a deliberate divergence?

**Context.** `wx_seed_new_world_defaults` sets `doc->m_bUTF_8 = 1`
(`platform/wx_mfc_impl.cpp:2322`), the opposite of the option table's default, and it runs
for **Open World File** as well as New World. A world file carrying `defaults="y"` and no
`utf_8` attribute therefore opens in UTF-8 here and in ANSI on Windows — and the flip is
saved back into the user's file. Either answer changes something someone may already rely
on, which is why it is a call and not a patch.

**Refs.** P02-F5 (CONFIRMED, low, port) · `platform/wx_mfc_impl.cpp:2322`

---

### D-04 — Passwords reach the session log: `bOmitFromLog` is write-only in `EvaluateCommand`  *(was D-32 / R2-06-F5)*
**Question.** Wire `bOmitFromLog` through the port's raw-send logging (`bLogIt =
LoggingInput() && !bOmitFromLog`) so a password typed at a masked prompt stays out of the
log, or document the divergence? This looks like it should simply be fixed; it is on the
queue because the port has no single log-write choke point for commands, so the wiring point
is a layer choice — say it's a plain bug and it becomes a fix.

**Context.** `EvaluateCommand`'s send-now path uses `LoggingInput()` **unconditionally**
(`evaluate.cpp:146`; the speed-walk arm at `:54-56` too), so the incoming `bOmitFromLog`
is never read to gate logging. F45 (`45dffeb`) hardened the *command bar* password path by
forcing `m_bNoEcho` true across the send, but `m_bNoEcho` gates only **echo/display**, not
the log write (`SendMsg`/`DoSendMsg` log purely on their `bLogIt` argument) — so a non-blank
password the caller passes with `bOmitFromLog = isPassword` (`wx_world_child.cpp:4821,4937`)
is still written to an open world log when `log_input` is on. The finding stands verbatim at
HEAD; only the blank-Enter case is already suppressed (`SendMsg(…, !bOmitFromLog)`).

**Refs.** R2-06-F5 (CONFIRMED, low, upstream; r2 deferred, needs-decision) ·
`evaluate.cpp:141-146,54-56` · `platform/wx_world_child.cpp:4821,4937` ·
`doc_shared.cpp:2360-2513` (SendMsg/DoSendMsg log gate)

---

### D-05 — A hotspot callback can free a *different* miniwindow it still uses (use-after-free)  *(was D-30 / R2-08-F4)*
**Question.** Extend the guard so every miniwindow the routine still holds a pointer to is
protected for the duration of a dispatch (or re-look-up `mw` by id after each callback), or
document that deleting another window from inside a hotspot callback is unsupported?

**Context.** The `m_bExecutingScript` guard that stops a hotspot callback deleting *its own*
miniwindow protects only the window the callback was dispatched **on**
(`methods_miniwindows.cpp:832`), so a `CancelMouseOver`/`ReleaseCallback` that calls
`world.WindowDelete` on a **different** miniwindow frees the `mw` pointer the same routine
keeps using — a genuine use-after-free/crash. F02/F40 fixed the self-delete and layout-pass
cases but not this cross-window one; nothing touched `m_bExecutingScript` since the queue was
built. (Note: an earlier draft of this item mis-described it as a `WindowImageFromWindow`
deep-copy issue — that is a *different*, unrelated finding; the cited R2-08-F4 is this
cross-window callback UAF.)

**Refs.** R2-08-F4 (CONFIRMED, medium, upstream; r2 deferred, needs-decision) ·
`scripting/methods/methods_miniwindows.cpp:832` · `miniwindow.cpp` (`m_bExecutingScript`) ·
`miniwindow_compose.cpp`

---

### D-06 — Every `WindowFilter` blur/sharpen is off-centre and skips the right column ⚡  *(was D-04)*
**Question.** Correct the kernel centring and the loop bound — which changes the pixels
*every* existing plugin's filter call produces, on both platforms — or record both as sacred
upstream behaviour and document them? (⚡ applies to the right-column half, whose fix is
written; the centring half is one token but has no written patch.)

**Context.** `GeneralFilter`'s vertical pass samples `row + 3` where the horizontal pass
samples `+2` (`miniwindow.cpp:2755`), so the vertical kernel is centred one row below the row
it writes and row 2 of every region is never sampled; separately the horizontal loop stops at
`iLastByte - 4` (`:2712`), leaving the rightmost pixel column unfiltered. Both are one token
each and the clamped window reads already cover the edges — but they are the same call about
the same function, so decide them together. Unchanged at HEAD (`GeneralFilter` last touched
only by the original import).

**Refs.** E12-F3 + E12-F4 (both CONFIRMED, upstream) · `miniwindow.cpp:2680-2760`
(callers `:3142`, `:3148`)

---

### D-07 — Database columns truncate at 32 bits in all four readers  *(was D-05)*
**Question.** Move all four call sites to `sqlite3_column_int64` and pick one representation
for values beyond 2^53 (string? double? error?) that Lua *and* VARIANT can both carry, or
accept 32-bit truncation as the documented limit of the database API?

**Context.** `sqlite3_column_int` is used at `methods_database.cpp:401` and `:687` (the
dispatch engines' path) and at `lua_methods.cpp:1894` and `:2088` (the Lua path, which does
not go through the doc methods), so any 64-bit column silently truncates in all six
languages. `DatabaseLastInsertRowid` already formats an `sqlite3_int64`, so the precedent
exists — but choosing the wide representation is a cross-engine type decision, not a
one-token change.

**Refs.** E06-F3 (MODIFIED, medium, upstream; deferred group 1) ·
`scripting/methods/methods_database.cpp:401,687` · `scripting/lua_methods.cpp:1894,2088`

---

### D-08 — Cmd+W and Cmd+Q ask different questions when closing a world  *(was D-06)*
**Question.** Thread connection state into `DecideWorldClose` so both paths ask Windows'
questions (Cmd+W gains the "this will end your session" warning and loses the extra prompt
for a disconnected, unmodified world), or keep the port's simpler single rule and document
the divergence?

**Context.** `DecideWorldClose` takes no connection argument
(`platform/world_close_decision.h:45-62` — unchanged at HEAD), so Cmd+W never shows the
connected-session warning that Cmd+Q's canonical `SaveModified()` issues, and it repurposes
`m_bConfirmBeforeClosingWorld` into a general "Close <world>?" that Windows only shows when
connected. Nothing is lost silently — both paths still present a cancelable prompt — so this
is a parity design task with a product-visible outcome, not a bug fix.

**Refs.** P01-F1 (MODIFIED, low, port; deferred group 1) · ledger K-339 ·
`platform/world_close_decision.h:45-62`

---

### D-09 — Cancelled quit leaves worlds half-closed  *(was D-31 / R2-04-F4; adjacent to D-08)*
**Question.** Run a non-committing "may I close?" pass over all worlds before any world-close
side effects (Windows-like all-or-nothing), or accept the current leave-them-closed and
clear `m_bWorldClosing` / re-arm MXP for already-prompted worlds when the quit is abandoned?

**Context.** The quit loop closes each world as it prompts, so cancelling at world 3 leaves
worlds 1-2 with their `on_world_close` scripts already run, MXP shut down, and
`m_bWorldClosing` stuck true for the rest of the session — permanently suppressing those
worlds' `ON_PLUGIN_GETFOCUS`/`ON_PLUGIN_LOSEFOCUS` callbacks. Windows prompts all worlds
first. Nothing on the quit path (`platform/wx_app.cpp` / `wx_mfc_impl.cpp`) was changed to
address this since the queue was built.

**Refs.** R2-04-F4 (CONFIRMED, medium, upstream; r2 deferred, needs-decision) ·
`platform/wx_mfc_impl.cpp:3175-3182` · `platform/wx_app.cpp:350` (`OSXOnShouldTerminate`)

---

### D-10 — The four notepad line-ending conversions cannot do anything  *(was D-07)*
**Question.** Track the file's line-ending style out-of-band from the control's LF-only
buffer (a real feature: tag the document, transform only at load/save), or retire
DOS↔Mac/Unix↔DOS on macOS and add them to the "not coming back" table?

**Context.** wxOSX's `wxTextCtrl` collapses every CR variant to LF before
`NotepadConvertText` sees the text, and `NotepadTextToCanonical`/`FromCanonical` make the
round trip the identity for *any* input — so all four ops (`notepad_convert_shared.cpp`) are
provable no-ops, and the test that "covers" them cannot fail. The destructive half of K-651
was already fixed by the CRLF-boundary approach; this is the residual half, and no code-level
test can distinguish "fixed" from "still a no-op" until the representation gap is decided.
(F's Quote-Lines fix `e18f228` touched a different op.)

**Refs.** X04-F9 (CONFIRMED, medium, port) · ledger K-651 ·
`notepad_convert_shared.cpp:38-80,225-240` · `tests/test_notepad_convert.cpp`

---

### D-11 — Global Prefs "Icon Placement: System tray" silently does nothing  *(was D-08)*
**Question.** Implement an `NSStatusItem`-backed menu-bar item, or grey the setting out and
add it to the "not coming back" table?

**Context.** `CMainFrame::AddTrayIcon` (`mainfrm.cpp:1980-2080`) can never show anything on
the port because `::LoadImage` returns nullptr for `IMAGE_ICON`; both callers run and achieve
nothing, so the pref round-trips perfectly and has no effect — the classic dead-reader shape
this project has been burned by. Unlike the sibling sound methods in the same file, there is
no hook to route to, so answering it means either new AppKit code or an explicit retirement.
(`mainfrm.cpp` untouched since the queue was built.)

**Refs.** E18-F2 (CONFIRMED, low, port) · `mainfrm.cpp:1980-2080`

---

### D-12 — Two window-geometry features are dead through the unbuilt view family  *(was D-09)*
**Question.** Build a window-geometry seam on `pal::IWorldView` (command-bar height +
frame resize), which revives both, or declare both unsupported on macOS and hide/annotate
them?

**Context.** `world.SetCommandWindowHeight` is canonical MFC that resizes the `CSplitterWnd`
through a `CSendView` the port never constructs (`methods_commands.cpp:422-465`), so its
`GetClientRect` stub yields a degenerate rect and the function effectively always returns
`eBadParameter`; the world-prefs **Adjust size to width** button is visible but inert because
its whole body sits inside `if (m_doc->m_pActiveCommandView || m_doc->m_pActiveOutputView)`
(`prefspropertypages.cpp` ~9054), both members being written only from the dead views. They
share one root cause and one remedy; its sibling "Adjust width to size" already landed
because it needed only an output-width accessor.

**Refs.** E07-F4 + E21-F3 (both CONFIRMED, medium, port; deferred group 1) ·
`scripting/methods/methods_commands.cpp:422-465` ·
`dialogs/world_prefs/prefspropertypages.cpp:8993-9054`

---

### D-13 — The status bar's click shortcuts have no port equivalent ⚡  *(was D-10)*
**Question.** Implement the two that map cleanly — double-click connected-time to reset, and
the lines pane to open Go To Line — on the per-world `wxStatusBar`, or record all four as not
coming back?

**Context.** `CMyStatusBar::OnLButtonDblClk` (`MyStatusBar.cpp:38-101`) gives Windows four
shortcuts (reset connected time, toggle freeze, Go To Line, tray action on the world name);
`CMyStatusBar` is not even a member of the port's frame, and the per-world status bar
(`platform/wx_world_child.cpp:1631`) binds no mouse handlers. Freeze and tray have no port
equivalent either way, so the answer is about the other two. (`MyStatusBar.cpp` untouched.)

**Refs.** E16-F2 (CONFIRMED, low, port) · `MyStatusBar.cpp:38-101` ·
`platform/wx_world_child.cpp:1631`

---

### D-14 — The trigger-colour dropdown shows text where Windows shows swatches  *(was D-11)*
**Question.** Add a second owner-draw path to the dialog bridge (for combo items, alongside
the existing `OwnerDrawButton`), or accept plain colour *names* in the colour combos?

**Context.** `CColourComboBox::DrawItem` (`dialogs/ColourComboBox.cpp:46-121`) never runs: the
bridge builds every combo as a plain `wxChoice`/`wxComboBox` and never inspects
`CBS_OWNERDRAWFIXED` (grep for `CBS_OWNERDRAW` across `platform/` returns zero hits, and
`IDC_TRIGGER_COLOUR`'s style carries the bit). It is a new bridge capability, so whether the
affordance is worth that is a product call. (`ColourComboBox.cpp` untouched.)

**Refs.** E26-F2 (CONFIRMED, low, port; deferred group 1) · `dialogs/ColourComboBox.cpp:46-121`

---

### D-15 — Three "Add …" dialogs add one file per click  *(was D-12)*
**Question.** Add multi-select through the shim (hook signature change plus real
`GetStartPosition`/`GetNextPathName` semantics), or accept one-at-a-time?

**Context.** The shim's `CFileDialog` is single-select only — `wxFD_MULTIPLE` appears nowhere
in the port and `GetNextPathName` (`platform/mfc_ui.h:5055`) hands back the single path — so
"Add Startup Worlds", "Add Global Plugins" and "Add plugin" all lose the batch affordance.
Nothing is lost but convenience, which is exactly why it needs a priority call rather than a
fix.

**Refs.** E23-F2 (MODIFIED, low, port) · `platform/mfc_ui.h:5052-5055` ·
`dialogs/global_prefs/GlobalPrefs.cpp` · `dialogs/plugins/PluginsDlg.cpp`

---

### D-16 — `world.ReplaceNotepad` returns nil in Lua but a boolean everywhere else  *(was D-13)*
**Question.** Match upstream Lua (scripts relying on nil keep working, cross-engine
inconsistency stays), or `return 1;` for correctness (Lua joins the other five engines, a
Windows-visible Lua API change plus a divergence entry)?

**Context.** `L_ReplaceNotepad` computes and pushes its success boolean and then `return 0;`
— the only push-with-return-0 body among all the `L_` entries — while the dispatch map
registers `ReplaceNotepad` as VT_BOOL, so Python/Perl/JS/Tcl/Ruby do receive it. The current
behaviour is deliberately pinned by `tests/test_lua_surface.cpp:3787-3789`
(`assert(world.ReplaceNotepad('t','text') == nil)`). The edit is one token plus that
assertion; only the parity-vs-correctness call is missing.

**Refs.** E04-F5 (CONFIRMED, low, upstream) · `scripting/lua_methods.cpp` (L_ReplaceNotepad) ·
`tests/test_lua_surface.cpp:3787-3789`

---

### D-17 — Lua's `world.SetTrace` prints no "Trace on/off" marker  *(was D-14)*
**Question.** Diverge from upstream Lua in shared source (one line: call `pDoc->SetTrace(…)`)
so Lua matches the five embedded engines, or keep Lua's silent assignment as upstream ships it?

**Context.** `L_SetTrace` assigns `m_bTrace` directly (`scripting/lua_methods.cpp:5850-5855`),
skipping `CMUSHclientDoc::SetTrace` (`methods_tracing.cpp:72-82`) and therefore `OnGameTrace`,
which flushes a half-built output line and emits the marker. The dispatch map binds the real
accessor, so `world.Trace = 1` from Python/Perl/JS/Tcl/Ruby *does* print it. The asymmetry
ships on Windows too, so correcting it is a deliberate upstream divergence.

**Refs.** X03-F4 (CONFIRMED, low, upstream) · `scripting/lua_methods.cpp:5850-5855` ·
`scripting/methods/methods_tracing.cpp:72-82`

---

### D-18 — `GetLineInfo(n).time` goes negative in 2038 ⚡  *(was D-15)*
**Question.** Delete the `(int)` cast — changing a script-visible value on both platforms —
or leave the upstream body alone?

**Context.** `scripting/lua_methods.cpp:3028` casts the line timestamp to `int` before
widening it to a Lua number, so from 2038-01-19 the value comes back negative. Present since
the first commit and untouched by the port; the fix is one token but it edits canonical
shared source, so it wants a divergence/upstream-bug entry and a call. (Verified still
`(int) pLine->m_theTime.GetTime()` at HEAD.)

**Refs.** X03-F6 (CONFIRMED, low, upstream) · `scripting/lua_methods.cpp:3028`

---

### D-19 — Middle-click over the output pane pastes on macOS  *(was D-16)*
**Question.** Keep the GTK-style middle-click paste on macOS (a convention neither macOS nor
Windows has), or swallow the event so middle-click does nothing outside a miniwindow hotspot?

**Context.** `COutputPane::OnMiddleDown` (`platform/wx_outputpane.cpp:2779`) offers the click
to a miniwindow hotspot and otherwise `evt.Skip()`s, and the observed result on macOS is a
paste; on GTK the primary-selection behaviour (`OnMiddleUp`, `#ifdef __WXGTK__`) is deliberate
and wanted, and Windows' `CView` middle handlers do nothing. Pre-existing and never changed by
the port — recorded as an open question for Wade since the GUI-drive campaign.

**Refs.** ledger K-232 (`unclear`) · `platform/wx_outputpane.cpp:2779-2800` ·
`docs/WINDOWS-VALIDATION.md`

---

### D-20 — Vetoing an atomic MXP tag still leaves the tag on the active list  *(was D-17)*
**Question.** Change the veto's semantics so a vetoed built-in tag is suppressed like a
vetoed custom element (upstream behaviour change, both platforms), or accept the asymmetry
and document that a veto only skips styling?

**Context.** The veto is asked inside `MXP_OpenAtomicTag` (`mxp/mxpOpenAtomic.cpp:83-84`)
*after* `MXP_StartTag` has already pushed the tag and added its START_TAG style, so
`MXP_StartTag` cannot see it — while a custom element's veto returns before the bookkeeping.
What survives is cosmetic rather than corrupting: a spurious `StartNewLine` from
`</p>`/`</ul>`/`</ol>`, the dangling tag counting toward the outstanding-tag warning, and a
close-tag callback for a suppressed open. Reachable from `OnPluginMXPopenTag` /
`OnMXP_OpenTag`; the close-tag consequence wants a Windows run against a real MXP server
before the semantics are changed. (`mxpOpenAtomic.cpp` untouched; `mxpStart.cpp`'s only change
since the queue was the F21 leak drains, not the veto.)

**Refs.** E11-F1 (MODIFIED, low, upstream, cluster C50; deferred group 3) ·
`mxp/mxpStart.cpp:159-335` · `mxp/mxpOpenAtomic.cpp:83-84`

---

### D-21 — Miniwindow fonts carry the 96/72 factor twice, including on the parity reference  *(was D-18)*
**Question.** Keep the empirical double factor (macOS text stays the size it is now, and the
headless `CpuRaster` reference keeps modelling the calibration rather than Windows), or apply
it once and/or confine it to the wx adapter — pending a side-by-side Windows measurement?

**Context.** `CMiniWindow::Font` multiplies by 96/72 twice under `#ifndef _WIN32`
(`miniwindow.cpp:760`), and because `miniwindow.cpp` is compiled once for both consumers the
correction also lands on the headless adapter that `pal/cpu_raster.h:87` labels the
Windows-parity reference: a 10pt request becomes a 23px cell there against Windows' 13px.
That half is provable; the magnitude itself rests on an unrecorded eyeball measurement, and a
40-line in-code comment argues the opposite way. Separating the reference adapter needs a
design call because the TU cannot be compile-time gated per consumer.

**Refs.** E12-F7 (MODIFIED, low, port, cluster C19; deferred group 3) ·
`miniwindow.cpp:686-760` · `pal/cpu_raster.h:87`

---

### D-22 — What is "the main window" on a platform that hides it?  *(was D-19)*
**Question.** Define macOS semantics for `GetInfo(249)/(250)` — the active world child's
client rect (closest to what the user sees), the hidden MDI parent's rect (literal Windows
parity), or keep today's approximation and document it?

**Context.** Windows returns `Frame.GetClientRect`; on the port `Frame` is a real global but
the `wxMDIParentFrame` it wraps is hidden whenever a world child exists, so
`GetInfo(249)/(250)` (`scripting/methods/methods_info.cpp:834-857`) neither matches Windows'
client-rect semantics nor tracks the visible window. Any fix needs a new client-rect seam,
and the seam cannot be designed until the semantics are chosen. (`GetMainWindowPosition` at
`methods_info.cpp:1781` carries the same caveat; the file's only change since the queue was
the InfoTypes focus codes, unrelated.)

**Refs.** E05-F3 (MODIFIED, low, port; deferred group 1) ·
`scripting/methods/methods_info.cpp:834-857,1781`

---

### D-23 — Bright-SGR (and OSC) support is an undocumented change to canonical source ⚡  *(was D-20)*
**Question.** Record it in `docs/UPSTREAM-DIVERGENCES.md` as an accepted divergence that a
Windows build from this tree also carries, or gate it behind `_WIN32` so the Windows build
stays stock?

**Context.** This project added SGR 90-97/100-107 handling to unconditionally-compiled
`ansi.cpp` (`:61-62`, `:144-157`, `:212-231`) in `f06e369`, and the same commit added OSC
(`ESC ] … BEL/ST`) swallowing to `telnet_phases.cpp`; **neither appears in
`docs/UPSTREAM-DIVERGENCES.md`** (verified: no SGR/bright/OSC entry at HEAD), which CLAUDE.md
requires for exactly this shape. `ESC[91m` is ignored by stock MUSHclient and sets bright red
here. The doc entry is drafted; the call is whether the feature belongs in the Windows build
at all.

**Refs.** E10-F2 (CONFIRMED, low, port, cluster C19) · `ansi.cpp:61-62,144-157,212-231` ·
`telnet_phases.cpp` (OSC arm) · `docs/UPSTREAM-DIVERGENCES.md`

---

### D-24 — The portable dispatch never rejects bad arguments  *(was D-21)*
**Question.** Delete the unreachable failure branch and document leniency (zero-fill and
coercion) as the contract of the five embedded engines, or introduce real COM-style argument
validation — which changes the observable error behaviour of five languages at once?

**Context.** `call_impl` (`platform/mc_dispatch.cpp`) has exactly one `return true;` (`:246`)
and every holder coerces rather than fails, so `MC_DISP_BADARGS` (`:451`) is unreachable and
each engine's whole `rc != MC_DISP_OK` branch is dead (all five pre-check `MC_DispatchHas`).
The leniency is deliberate and test-pinned (`McDispatch.MissingArgumentsAreTheZeroValueNotGarbage`
and two siblings) and the `kArgDefaults` table is built on it — so this is a contract
question, and only the answer decides whether the dead code is a bug or a leftover. (The
`c88dbf9` change to this file was about default-window resolution, not arg validation.)

**Refs.** P06-F3 (MODIFIED, low, port) · `platform/mc_dispatch.cpp:246,451` ·
`tests/test_mc_dispatch.cpp`

---

### D-25 — `utils.menufontsize()` returns a fabricated 0 ⚡  *(was D-22)*
**Question.** Document it as inert (AppKit owns the menu font) and leave the 0, or return
`nil` so a plugin's save/restore idiom can detect that there is nothing to restore — a
script-visible API change?

**Context.** `SystemParametersInfo` is an unconditional `return FALSE` on the port, so
`menufontsize` (`scripting/lua_utils.cpp:2162-2183`) zero-inits its metrics, reports an old
size of 0 and sets nothing. The common plugin idiom — read the size, change it, restore it —
therefore passes 0 back and silently cannot restore anything. Both answers are one line; they
differ in whether existing plugins start seeing `nil`. (The `33ee87d` change to this file was
`directorypicker`, not `menufontsize`.)

**Refs.** X01-F7 (CONFIRMED, low, port) · `scripting/lua_utils.cpp:2162-2183`

---

### D-26 — A click in a UTF-8 world resolves one character right (Windows only)  *(was D-23)*
**Question.** Change the one token (`col++` → `col--`) in canonical `mushview.cpp` to
correct a mid-character column *backward* like its sibling arm does, or leave sacred upstream
behaviour alone since nothing on this platform can observe it?

**Context.** `CMUSHView::calculate_line_and_column`'s first UTF-8 fix-up walks *forward* to
the next character's lead byte (`mushview.cpp:1931-1933`, `col++`) while the half-way arm
below walks *backward* (`:1943-1947`, `col--`), so on Windows the two arms can resolve the
same click to different characters and a click just left of a multi-byte character selects
one too far right. `CMUSHView` is never constructed on the port, so the change is unverifiable
from this side and needs both a Windows run and a call about editing upstream code on that
evidence. (The `733ebbb` change to `mushview.cpp` was the double-click word scan, a different
function.)

**Refs.** CN-01 (MODIFIED, low, port; deferred group 3) · `mushview.cpp:1931-1948`

---

### D-27 — `DelayedSend`: delete the dead feature or revive it ⚡  *(was D-24)*
**Question.** Delete it (pure dead-code removal, zero behaviour change — the fix is written),
or revive the feature and fix the three defects that would fire the moment it ran?

**Context.** `CMUSHclientDoc::DelayedSend` (`doc.cpp:3222+`) and its user-facing syntax
documentation are unreachable: the only call site is commented out
(`doc_shared.cpp:5487-5492`, inside `/* … */`) and so is `eSendDelayed` (`OtherTypes.h:57`).
If revived it would send to the wrong destinations (`eSendToWorld`/`eSendToSpeedwalk`
inverted), strip the speed-walk prefix twice, and treat "3 north" as delayed because
`bSemicolon` never selects delayed-vs-immediate. Deleting documented (if dead) functionality
is a product call. (Verified still commented at HEAD.)

**Refs.** E01-F5 (CONFIRMED, low, upstream) · `doc.cpp:3222` · `doc.h` · `doc_shared.cpp:5487-5492`
· `OtherTypes.h:57`

---

### D-28 — The retired per-world "Sounds" prefs page: delete or keep as reference ⚡  *(was D-25)*
**Question.** Delete `CPrefsP2` and its three doc helpers (pure removal of inert code, fix
written), or keep the class as reference for its live replacement `CPrefsP14`?

**Context.** `CPrefsP2` (`dialogs/world_prefs/prefspropertypages.cpp:279-345`) is never
constructed — its construction in `configuration.cpp` is commented out
(`// CPrefsP2 page2; // sounds`) and its three handlers
(`LoadPrefsP2`/`ChangedPrefsP2`/`SavePrefsP2` at `configuration.cpp:127,891,…`) are empty
bodies. It costs nothing to keep and nothing to delete; only the project's taste for keeping
retired upstream code decides. (Verified unchanged at HEAD.)

**Refs.** E21-F10 (CONFIRMED, info, upstream) · `dialogs/world_prefs/prefspropertypages.cpp:279-345`
· `dialogs/world_prefs/configuration.cpp:127,891,2057`

---

### D-29 — The two dead-view context menus: revive or delete their recovery path  *(was D-29)*
**Question.** Wire the recovered `.rc` MENU resources for CMUSHView/CSendView's context menus
up as real macOS context menus (revive), or delete them from the recovery path as permanently
unreachable?

**Context.** `RcResources::FindMenu()` (`platform/wx_dialog_resource.cpp:347`) has no shipping
caller anywhere — the `FindMenu` hits elsewhere are `wxMenuBar::FindMenu`, a different method —
so the parser keeps 14 menus alive (`MenuCount()==14`, pinned as a structural invariant at
`wx_dialog_resource.cpp:413`) for two consumers that never existed on the port. Revive gains
two canonical right-click menus; delete simplifies but needs the `MenuCount()==14` check
re-baselined. F17 (wave 4) surfaced it and declined to choose; `CMenu::LoadMenu` is still the
`FALSE` stub.

**Refs.** P04-F1 (CONFIRMED, low, port; r1 fix-backlog batch F17, decision half deferred) ·
`platform/wx_dialog_resource.cpp:347,413` · `platform/mfc_ui.h` (`CMenu::LoadMenu` stub)

---

### D-30 — "Disable keyboard menu activation" is vacuous on macOS ⚡  *(was D-26)*
**Question.** Add a row to CLAUDE.md's "deliberately NOT coming back" table (macOS has no
Alt/F10 menu activation, so the setting is vacuously satisfied), or hide the checkbox so the
prefs page stops offering it?

**Context.** The pref round-trips fully but all three readers are `OnSysCommand`/`SC_KEYMENU`
handlers (`mainfrm.cpp:2358-2359`, `mushview.cpp:6145`, `sendvw.cpp:2593`) that no wx pump
ever dispatches. The port's convention is to hide controls Windows hides — this one Windows
shows, so the choice is documentation versus a port-only UI departure. (All three readers
verified present and dead at HEAD.)

**Refs.** X02-F8 (CONFIRMED, info, port) · `mainfrm.cpp:2355-2368` ·
`mushview.cpp:6145` · `sendvw.cpp:2593` · `dialogs/global_prefs/GlobalPrefs.cpp`

---

### D-31 — Which `GetScrollInfo` contract is intended for a NULL window?  *(was D-27)*
**Question.** FALSE-for-null, matching both `CWnd` overloads and Win32, or "always succeeds
with benign zeroes" as a deliberate shim simplification?

**Context.** The free function returns TRUE with zeroed fields for a NULL window
(`platform/mfc_ui.h:650`) while the two member overloads return FALSE, and the test suite
*pins* the inconsistency (`tests/test_mfc_ui.cpp:3406`,
`EXPECT_TRUE(GetScrollInfo((HWND)nullptr, …))`) — so the shim and its test change together
whichever way it goes. This is the shape the campaign learned to distrust: a test defending a
stub's contract, which no amount of code reading can arbitrate.

**Refs.** X04-F17 (CONFIRMED, low, port) · `platform/mfc_ui.h:650` (+ member overloads) ·
`tests/test_mfc_ui.cpp:3406`

---

### D-32 — `world.ExpandVariables`: expose it in the product or drop it  *(was D-28)*
**Question.** Expose the port-invented `world.ExpandVariables` in the shipping app (a
`world.*` method Windows does not have, so it needs a divergence entry and an implementation
in `mushgui_adapters`), or drop it and rewrite the test that is its only consumer?

**Context.** The dead `RegisterPortableLuaExtras` call and its stale comments were removed in
`831da36`, so the mechanical half is done; what survives is the original question. The extra
has never been callable from a real world or plugin — its guard was false for the shipping GUI
from the day it was written — yet `TEST(LuaScripting, ExpandVariables)` still tests it through
the fixture's own registration (`tests/lua_test_helpers.h:34`), while `test_scriptengine.cpp`
now asserts the engine-built state does *not* have it. The two tests currently encode both
answers.

**Refs.** E08-F2 (MODIFIED, low, port; r1 fix-backlog, mechanical half landed `831da36`,
decision half = ledger K-231/K-534) · `scripting/lua_utils_portable.cpp:858-867` ·
`tests/lua_test_helpers.h:34` · `tests/test_lua_scripting.cpp` · `tests/test_scriptengine.cpp`

---

## Resolved since the queue was built

**None of the 32 decision items above was closed by the F34…F54 campaigns or the R4
follow-ups.** Each cited site was read at HEAD `56c72d5` and is unchanged; no D-item finding
appears in any round's fix-backlog, nor in the round-1/2/3 *refuted* or *already-done* lists.
The ten ⚡ items were individually re-checked: every drafted fix is still absent from the tree
(the `(int)` cast, the `utf_8` seed, the commented `DelayedSend`, the `SC_KEYMENU` readers,
the missing UPSTREAM-DIVERGENCES SGR/OSC entry, etc. all remain), so all ten stay open.

Adjacent fixes that *touched but did not close* a queue item (worth knowing before working
one):

- **D-04 (passwords → log).** F45 (`45dffeb`) hardened the command-bar password path — forcing
  `m_bNoEcho` true across the send and using `!bOmitFromLog` for the blank-Enter case — but
  `m_bNoEcho` gates only echo, not the log write, and `EvaluateCommand`'s raw send still
  ignores `bOmitFromLog` (`evaluate.cpp:146`). The finding stands; F45 narrowed it, and its
  in-code comment slightly overstates the coverage.
- **D-05 (cross-window miniwindow UAF).** F02 (`60a1fd2`) and F40 (`c2b4a85`) fixed the
  hotspot *self*-delete and layout-pass cases, but left the cross-window `m_bExecutingScript`
  gap (R2-08-F4) that this item is about.
- **D-29 (dead-view context menus).** F17 landed the sibling `.rc` id-collision fixes
  (`42be7dc`) from the same batch but declined the revive-vs-delete choice.

Historical context — items dropped **while** the queue was being assembled (not since), kept
here so their K-ids still resolve:

- **Miniwindow hit-test semantics** (E12-F2 / K-481) — fixed by F02 (`60a1fd2`),
  `docs/UPSTREAM-DIVERGENCES.md` §9.
- **`DrawEdge` with no `BF_` side flags** (P05-F5) — fixed in `b29d5c9`,
  `docs/UPSTREAM-DIVERGENCES.md` §8. These two set the precedent for D-23: when the port
  changes canonical behaviour, the house answer so far is *fix and document*.
- **`GetLineInfo` "timestr" rendering** (K-245, `unclear`) — a rendering was chosen and is in
  code (`scripting/lua_utils_portable.cpp:144-148`); what remains is a Windows comparison, not
  a decision.
- **`RegisterPortableLuaExtras`' dead branch** (K-534) — deleted in `831da36`; the
  keep-vs-expose half survives as D-32.
- **Build/packaging** (C37 / X05-F1…F5) and the **twelve ledger status flips** (C41) — done in
  `deferred.md` group 6; never decisions.

Not carried into the queue, and why: **CN-05** (message-box parent) and **P08-F6**
(`DDX_CBString` save direction) are empirical questions a live-app run answers, not calls;
**E17-F8**, **E02-F5** and **X04-F1** are mechanical once a Windows run or a test sweep says
what the current behaviour is; **P09-F1/P09-F2** (POSITION ownership, `CWnd::FromHandle`'s
unbounded map) are internal shim redesigns with no user-visible either/or; ledger
**K-243/K-244** (should `editSettings` render from a shared model; should the Windows pane
switch from pull to push) are pre-existing PAL-direction questions from `docs/PAL.md` rather
than audit findings.
