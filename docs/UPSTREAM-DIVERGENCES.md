# Deliberate divergences from upstream MUSHclient

This file records places where **both** builds intentionally behave differently
from Nick Gammon's shipped MUSHclient. It is not a bug list — see
`docs/UPSTREAM-BUGS.md` for defects reproduced or worked around, and it is not
about macOS-vs-Windows differences, which belong in the code as port comments.

Everything here is a decision by the project owner, applied to the canonical
source so Windows and macOS keep behaving identically. Anyone merging upstream
changes should expect a conflict at each of these sites and keep the version
below unless the decision is revisited.

---

## 1 — Partial command recall: an exact match is recallable

**Where:** `sendvw.cpp`, `CSendView::OnAltUparrow` and `CSendView::OnAltDownarrow`
(the `do … while` search condition); mirrored in
`platform/wx_world_child.cpp`, `CWorldChildFrame::AltArrowRecall`.

**Upstream:** the search skipped any history entry identical to what you had
typed, with the comment *"don't accept an exact match (see forum subject 2313 —
they won't want to recall exactly what they typed :)"*.

**Now:** prefix match only; an exact match is a match.

**Why:** the exclusion makes a command you have run before unreachable by
partial recall. Type `north` with `north` and `northeast` in the history and
upstream can only ever offer you `northeast`. Typing a prefix is usually done
*because* you want the thing you ran last time, so skipping it is surprising
rather than helpful.

---

## 2 — Partial command recall: down-arrow restores your typing

**Where:** `sendvw.cpp`, `CSendView::OnAltDownarrow`, the `if (!m_HistoryPosition)`
arm; mirrored in `platform/wx_world_child.cpp`, `CWorldChildFrame::AltArrowRecall`.

**Upstream:** on walking forward past the newest match it echoed a **blank**
command, discarding whatever you had been typing when you started recalling.

**Now:** it puts the typed text back. The partial is still discarded, so the
next arrow press falls through to an ordinary full-history walk exactly as
before — only the text shown changes.

**Why:** the partial is already saved; blanking the bar throws away the one
thing the user cannot get back, and forces them to retype it. Up-arrow past the
*oldest* entry still clears, as upstream does, so the two ends stay distinct.

**Note:** the *ordinary* (non-partial) down-arrow is covered separately by
entry 3 below — the two paths now agree on both builds.

---

## 3 — Ordinary down-arrow restores your typing too

**Where:** `sendvw.cpp`, `CSendView::OnKeysNextcommand` — both of its
end-of-walk arms; capture added to `OnKeysNextcommand` and
`OnKeysPrevcommand`, with the new `m_strTypedBeforeRecall` in `sendvw.h`. The
port has behaved this way since `86e3f6f`.

**Upstream:** walking forward past the newest history entry called
`SetCommand("")`, blanking the command bar.

**Now:** it puts back whatever the user was typing when the walk began. Type
`abc`, press Up twice to reach `abcd` then `abcde`, and pressing Down twice
walks back to `abcd` and then to `abc` — instead of `abc` being lost.

**Why a new member rather than `m_strPartialCommand`:** that one is the Alt
handlers' prefix-match key. Writing it from the ordinary handlers would change
which entries Alt+Arrow matches, which is a different behaviour change wearing
this one's clothes. Captured under the same `m_bChanged` test the Alt handlers
use, so only text the user personally typed is remembered — script- or
macro-placed text is not.

**Deliberately unchanged:** up-arrow past the *oldest* entry still blanks, on
both builds. The two ends stay distinct, and only the direction the user can
walk *back* from restores.

## 4 — Default fonts apply when you press OK, not when you find a menu item

**Where:** `dialogs/global_prefs/GlobalPrefs.cpp`, the per-document loop at the
end of `DoGlobalPrefs`; the work itself is factored into
`CMUSHclientDoc::ApplyDefaultFonts` (`scripting/methods/methods_defaults.cpp`),
which `OnFileReloaddefaults` now calls too so the two cannot drift.

**Upstream:** changing the default input or output font in global preferences
updated `App.m_strDefaultOutputFont` and friends, and stopped there. Open worlds
kept their current font — including worlds with "use default output font"
ticked, which is precisely the set that ought to follow it. The new font only
arrived on **File ▸ Reload Defaults**, or on the next world you opened.
`m_bUseDefaultOutputFont` is a one-shot seed, never consulted while drawing.

**Now:** dismissing the dialog with OK re-applies the default fonts to every
open world that follows them.

**Why:** a preference that visibly does nothing is indistinguishable from a
broken one, and "there is a menu item elsewhere that makes your setting take
effect" is not a thing a user should have to know.

**Scope — fonts only.** `Reload Defaults` also *loads the default trigger,
alias, timer, macro and colour files*. Those stay on the explicit command:
re-importing entity sets silently on every OK could duplicate or overwrite live
triggers, which is a far worse surprise than the one being fixed.

**Do not add a "skip if unchanged" test** to `ApplyDefaultFonts`. It reads like
a free optimisation and is a bug: the function also runs during world open,
before the world's view exists, where the emit is dropped but the doc's font
fields are still updated. A guard therefore makes the later, view-attached call
conclude there is nothing to do, and the font never reaches the screen. This was
written, shipped into a local build, and caught by instrumenting the live app;
`tests/test_pal_worldview.cpp` (`DefaultFonts.RepeatCallStillReachesTheView`)
now pins it.

---

## 5 — Shift+Return / Alt+Return insert a line break instead of sending

**Files:** `sendvw.cpp` (`CSendView::PreTranslateMessage`),
`platform/wx_world_child.cpp` (`CWorldChildFrame::OnInputKeyDown`).

**Was:** `CSendView::OnChar` tests `nChar == VK_RETURN` and sends, with no
modifier check. The command window is `ES_MULTILINE` and can hold several
lines, but no keystroke could ever *put* one there — the only way to get a
newline into it was to paste one.

**Now:** Shift+Return and Alt+Return (Option+Return on macOS) insert a line
break at the caret. Plain Return still always sends, unchanged.

**Why:** the multi-line command window is already there and already sends
line-by-line on Return; the only thing missing was a way to compose those lines
by hand. Shift+Return for "newline, don't submit" is also the near-universal
convention in modern chat and message inputs, so the key most users will already
try did nothing (Windows) or, before `265b1ac`, was unreachable.

**Placement note.** On Windows this sits in the *view's*
`PreTranslateMessage`, which `WalkPreTranslateTree` runs before the frame
translates accelerators — so it wins over a script-bound Shift/Alt+Return. On
macOS `FireAcceleratorIfMatch` is deliberately checked first (scripts override
any key), so there the script would win. This differs only for someone who binds
Shift+Return or Alt+Return through the Lua `Accelerator()` API; it was not worth
reordering either platform's key dispatch to unify.

---

## 6 — Un-binding an accelerator forgets it completely

**Where:** `scripting/methods/methods_accelerators.cpp`,
`CMUSHclientDoc::AcceleratorTo` — the erase block after the HACCEL rebuild,
which used to be inside `#else` (non-Windows only).

**Upstream:** an empty `Send` half-deletes. The Win32 HACCEL is rebuilt without
the key — so it genuinely stops firing — but `m_AcceleratorToCommandMap`,
`m_CommandToMacroMap`, `m_CommandToSendToMap` and `m_CommandToPluginMap` are
written only inside the `if (Send [0])` arm and are never erased. The entries
outlive the binding.

**Now:** the erase runs on both platforms, so an empty `Send` clears the maps
as well as the table.

**Why:** `AcceleratorList` is built from those maps, so upstream went on
reporting `Ctrl+F8 = hello` for a key that could no longer do anything —
confirmed on Windows against a stock 5.06, which behaves the same way
(`docs/WINDOWS-VALIDATION.md` §8g, §8j). A script's only way to ask "is this
key already taken?" therefore answered wrongly, and `m_CommandToPluginMap` kept
naming a plugin as the owner of a dead binding, which is exactly the
bookkeeping that plugin unloading consults.

Off Windows this erase was always required rather than merely tidy: there is no
HACCEL at all, and `CWorldChildFrame::FireAcceleratorIfMatch` dispatches
straight out of `m_AcceleratorToCommandMap`, so the map *is* the table. That is
why the block already existed in the `#else` arm; all that changed is that
Windows now runs it too.

**Observable change on Windows:** `AcceleratorList()` stops listing keys that
were already unable to fire. The HACCEL rebuild is untouched, so no key that
fired before stops firing.

**There is a second, real consequence, and this entry used to deny it** — in a
sentence that contradicted itself inside its own parentheses ("re-binding a key
still re-uses its previously allocated command id … (a fresh id is allocated
instead)"). The truth is the parenthesis. Un-binding destroys the
`m_AcceleratorToCommandMap` entry, so a later re-bind of the same key MISSES
the lookup at `methods_accelerators.cpp` and takes `m_nextAcceleratorCommand++`.
Nothing ever returns an id — the counter is seeded once in `doc_construct.cpp`
and only incremented — so **each unbind/rebind cycle permanently consumes one
of the 1000** (`ACCELERATOR_FIRST_COMMAND` 12000 `.. + ACCELERATOR_COUNT`,
`mainfrm.h`). At exhaustion `AcceleratorTo` returns `eBadParameter` and no
further key can be bound in that world for the rest of the session.

**Fixed — the free list.** The two looked mutually exclusive, because
`AcceleratorList` enumerates `m_AcceleratorToCommandMap` itself, so hiding an
un-bound key from the list requires destroying the entry that would have
supplied the old id. Releasing that id to `m_freeAcceleratorCommands` on erase,
and preferring it on the next allocation, buys both: the list stops naming dead
keys **and** binding the same key twice costs one id, which is the property
upstream had (it got it by never erasing, at the price of listing dead keys
forever). So this is no longer a divergence in the pool's behaviour at all —
only in what `AcceleratorList` reports, which is the intended change.

Note the leak was **not new to the port** — off Windows the erase always ran, so
macOS had always paid it; `c24926f` merely gave Windows the same bill, and the
free list now cancels it on both. Pinned by `tests/test_accelerators.cpp`:
`UnbindingReleasesTheCommandIdForReuse` (the same id comes back) and
`ManyUnbindRebindCyclesDoNotExhaustThePool` (`ACCELERATOR_COUNT + 50` cycles,
which is what the old code could not survive), with a control asserting two
keys bound *simultaneously* still get distinct ids — so the free list has not
quietly turned a fixed pool into an unbounded one. The neighbouring
`RebindingAKeyReusesTheCommandId` covers none of this: it never unbinds, so it
only ever exercises the map-hit branch and passed before and after.

## 7 — wrap_input wraps at the window edge, not at the wrap column

`wrap_input` ("match input wrap to output") makes Windows' command window
wrap typed text at `min(wrap column + 1, window width)` characters, by
setting the edit control's RIGHT margin (`CSendView::UpdateWrap`,
sendvw.cpp).  wxWidgets has no right-margin API — `wxTextCtrl::SetMargins`
is left/top-only on wxOSX — so the port's bar (rebuilt with a wrapping
style when the option is on; see `CWorldChildFrame::BuildInputBar`) wraps
at the WINDOW width instead.

Identical whenever the window is narrower than the wrap column — the case
the option exists for — and later than Windows when the window is wider.
Exact column parity would need an AppKit shim pinning the NSTextView's
text-container width (wx_osx_text.mm territory); deferred until someone
actually hits the wide-window case.

This is port-side only (platform/wx_world_child.cpp); no canonical source
was changed, so no Windows run is owed for it.

## 8 — DrawEdge with no BF_ side flags now draws nothing (a divergence removed)

**Where:** `platform/wx_gdi_compat.cpp`, `CDC::DrawEdge`.

**Windows:** `DrawEdge`'s `nFlags` says which sides of the rect to stroke
(`BF_LEFT`/`BF_TOP`/`BF_RIGHT`/`BF_BOTTOM`); passing 0 (none of the four
bits set) draws no border at all.

**The port used to invent** its own rule -- "no sides named means all
sides" -- forcing `BF_RECT` in whenever `nFlags` named none of the four,
so `world.WindowRectOp` action 5 with `EdgeFlags` 0 painted a complete
border where Windows paints nothing.

**Now:** `nFlags == 0` returns immediately, drawing nothing, matching
Windows. This undoes the invented rule rather than adding a new one, so
it is listed here as a correction, not a fresh design decision.

**Correction: `BF_MIDDLE` had never been implemented, and the early return
made that worse.** The interior fill is a separate flag (`BF_MIDDLE`,
0x0800) that `miniwindow.cpp:370` lets straight through -- it validates
only `(Colour2 & 0xFF) > 0x1F` -- and no version of this body has ever
filled anything: the pre-fix code called `Draw3dRect`, the post-fix code
strokes two rings. So `BF_RECT|BF_MIDDLE` drew the rings over an untouched
interior, and `BF_MIDDLE` on its own, naming none of the four side bits,
hit the new early return and became a complete no-op where Win32 fills the
rectangle with the button-face colour and draws no border. The missing fill
predates the side-flag fix; the early return only changed which wrong
picture a fill-only call draws. `nFlags` is now tested as
`(BF_RECT | BF_MIDDLE)`, and `BF_MIDDLE` fills the whole rect with
`COLOR_3DFACE` before the rings are stroked over it (the same picture Win32
leaves, since the rings paint last either way).

**Correction: both rings were a pixel too long on every side.** The commit
that introduced the side-flag-honouring `ring` lambda compensated for GDI's
endpoint-EXCLUSIVE `LineTo` by passing `rr + 1` / `b + 1` as the far end of
each stroke -- but this adapter's line primitive INCLUDES its endpoint (the
port's own measured deviation, `docs/PAL.md` and the `WindowLine` probe in
`--pal-miniwindow-selftest`), so every stroke ran one pixel past the extent
`::DrawEdge` uses. Win32 `RECT`s are right/bottom-exclusive and `::DrawEdge`
strokes strictly inside, so the outer ring painted at `x == right` and
`y == bottom`, one pixel OUTSIDE the caller's rectangle, and the inner ring
(drawn second, inset by one) overwrote the outer ring's corner pixels --
four light notches in the corners of a raised border, visible on the
full-canvas `rect_draw_edge` in `plugins/Messages_Window.xml`. `CDC::Draw3dRect`
twenty lines above does the same job with no `+1` and was already right.
Each side is now a one-pixel `DrawRectangle` STRIP rather than a line, which
states the extent outright: it stays correct whether or not the inclusive-line
deviation is ever repaired, and has no zero-length-stroke edge case on a
1px-wide rect.

**Correction: four `nFlags` bits were dropped in silence, and three are now
honoured.** `miniwindow.cpp:377` gates only `(Colour2 & 0xFF) > 0x1F`, so
`BF_DIAGONAL` (0x0010) and the whole high byte -- `BF_SOFT` 0x1000,
`BF_FLAT` 0x4000, `BF_MONO` 0x8000 -- reach the shim, and
`scripting/lua_methods.cpp` publishes all of them as `miniwin.*` constants
(`rect_diagonal_end_top_left`, `rect_option_flat_borders`,
`rect_option_monochrom_borders`, `rect_option_softer_buttons`). The shim read
only the side bits, `BF_MIDDLE` and `BF_ADJUST`, so a caller got `eOK` and a
silently different picture -- worst for `BF_DIAGONAL`, where Win32 draws a
single diagonal vector and the side bits name its END point, and the shim
instead read `BF_DIAGONAL_ENDTOPLEFT` as `BF_TOP|BF_LEFT` and drew two sides
of a box.

- `BF_DIAGONAL` now draws the vector, with the end point the side bits name
  and `ENDBOTTOMRIGHT` for `BF_DIAGONAL` alone.
- `BF_MONO` and `BF_FLAT` now replace the bevel with a FLAT border -- one
  colour the whole way round each ring: `COLOR_WINDOWFRAME` over
  `COLOR_WINDOW` for mono, `COLOR_3DSHADOW` over `COLOR_3DFACE` for flat.
  `BF_MONO` wins when both are named.
- **`BF_SOFT` is still dropped, deliberately.** Unlike the other two it does
  not change the SHAPE, only which grey the bevel uses, so the port cannot
  tell right from wrong without measuring Windows -- and choosing the shade
  from this side would be inventing a measurement. Left as an honest gap
  rather than a plausible guess.

Two further things about the diagonal that only a Windows run can settle:
Win32 SHADES it (both ring colours, one either side of the vector), which is
not reproduced -- a two-pixel band necessarily leaves the rectangle on one
side unless it is shortened, and which side carries which shade is not
derivable here; and `BF_MIDDLE` combined with `BF_DIAGONAL` fills the whole
rectangle here, where Win32 may fill only the triangle on one side of the
vector.

**Status:** port-side only (platform/wx_gdi_compat.cpp); no canonical
source changed. Still needs a Windows run to confirm real `DrawEdge`
truly no-ops on `nFlags == 0`, to confirm the `BF_MIDDLE` fill colour
and extent, to confirm the ring colours at the two mixed corners, and to
settle the four `BF_DIAGONAL`/`BF_FLAT`/`BF_MONO`/`BF_SOFT` questions above,
before this can be called verified. Covered in-app by the
`DrawEdge` block of `--pal-miniwindow-selftest`, which now also pins the
ring extents (each side reaches `right-1`/`bottom-1` and nothing is painted
at `right`/`bottom`), the surviving outer-ring corners, both diagonal end
points, and the flat/mono colour tables.

## 9 — The miniwindow hit test measures from left/top by size, and only for windows that were drawn

**Where:** `miniwindow_compose.cpp`, `MouseOverMiniWindow` (upstream
`CMUSHView::Mouse_Over_Miniwindow`).

**Upstream:** `if (!mw->m_rect.PtInRect (point)) continue;`.

**The problem with that:** `m_rect` is not a rectangle after a wall layout.
`CalculateMiniWindowRectangles` builds the wall-centred cases as
`CRect (start, 0, mw->GetWidth (), mw->GetHeight ())` — right/bottom hold the
window's *width* and *height*, not its right and bottom edges — and
`Blit_Bitmap` leaves it that way. `PtInRect` on such a rect requires
`left <= x < width`, which for any window placed further right than it is wide
is unsatisfiable, so its hotspots were silently unclickable on Windows too.

**Now:** the test is explicit —
`point.x < left || point.y < top || point.x >= left + GetWidth () || point.y >= top + GetHeight ()`.
For every window the layout pass actually drew this answers exactly what
`PtInRect` answered; for the wall-placed ones it answers what the user can see.

**And it is bounded by `m_bTemporarilyHide`,** which is the half that was
missing. A window the pass could not fit is never laid out, so it keeps the
constructor's `(0, 0, 0, 0)` rect — which `PtInRect` matched nothing against,
but which the size test reads as the pane's top-left `GetWidth ()` x
`GetHeight ()` corner. Left unbounded it swallowed clicks meant for the text
underneath (`MouseDownMiniWindow` returns true and
`platform/wx_outputpane.cpp:2488` gives up on selection and MXP links) and
hit-tested an undrawn window's hotspots. `MouseOverMiniWindow` now skips
`!GetShow () || m_bTemporarilyHide`, the same pair both paint loops already
test (`mushview.cpp:1007` and `:1507`,
`platform/wx_world_child.cpp:262`): if it is not drawn it cannot be clicked.

**Why this is here and not in UPSTREAM-BUGS.md:** the widened test is a
deliberate change to shared source, not a repair of a defect upstream would
recognise. The visibility bound, taken on its own, restores exactly what
`PtInRect` did.

**Correction:** the file-head comment used to call the widened test "a strict
extension: every point the old test matched still matches". The first half is
true, the conclusion was not — the extra points are not all harmless, which is
what the `m_bTemporarilyHide` skip exists to fix. Covered by
`LuaSurface.LayoutPopsWindowsThatDoNotFitAndTheyStopBeingClickable`
(`tests/test_lua_surface.cpp`), which asserts both halves: the popped window
does not answer at the pane's corner, and the one that *was* laid out still
answers at its own coordinates.

**Second correction: `m_bTemporarilyHide` was only half the bound.** It is
written by `CalculateMiniWindowRectangles` alone, so it only ever describes a
window the layout pass *rejected*. A shown window that no layout pass has seen
**at all** has the flag false and the same constructor rect — and
`WindowAddHotspot`'s create-under-the-cursor re-detect
(`scripting/methods/methods_miniwindows.cpp`) reaches `MouseOverMiniWindow`
synchronously inside the creating script, before any paint, so this is not a
theoretical window: a full-window hotspot whose rect contains the constructor's
`m_last_mouseposition` (0,0) could fire a `MouseOver` for a window drawn
somewhere else entirely. `CMiniWindow::m_bLaidOut` (false in the constructor,
set true by the pass that owns the window, beside the `m_bTemporarilyHide`
reset) is the other half, and `MouseOverMiniWindow` skips on it too. Covered by
`LuaSurface.AWindowNoLayoutPassHasSeenIsNotClickableAtThePanesCorner`, which
asserts both halves the same way.

## 10 — WriteLog's trailing-newline guard actually checks one character

**Where:** `scripting/methods/methods_logging.cpp`, `CMUSHclientDoc::WriteLog`.

**Upstream:** the "append a newline if the message doesn't already end in
one" guard reads `if (strMessage.Right (2) != "\n") strMessage += "\n";` —
`Right (2)` takes the last **two** characters, compared against a **one**
character literal. `CString`'s `operator!=` is a length-sensitive string
compare, so a two-character (or longer) tail can never equal a one-character
string; the guard fires on every call regardless of the message's actual
last character, and any message the caller already terminated with `\n`
comes out `\n\n` in the log file.

**Now:** `Right (1)` — the length the guard tests matches the length of the
delimiter it tests against and the length of what gets appended, so a
message already ending in `\n` is recognised and left alone.

**Why this belongs here and not in `UPSTREAM-BUGS.md`:** the guard's mismatch
is real on Windows too (this file has no `#ifdef` — it single-sources), so
the fix changes behaviour Windows has always shipped. `world.WriteLog` still
never returns an error for it, so nothing but the log file's bytes differ.

**Status:** `scripting/methods/methods_logging.cpp` is canonical Windows
source (`MUSHclient.vcxproj`); this needs a Windows run to confirm the
before/after log bytes match this description. Covered headlessly by
`LoggingTest.WriteLogDoesNotDoubleAnExistingTrailingNewline`
(`tests/test_display.cpp`).

## 11 — Colour-matching triggers match the colour you can see, not the colour number

**Where:** `ProcessPreviousLine.cpp`, `CMUSHclientDoc::ProcessOneTriggerSequence` —
the `TRIGGER_MATCH_TEXT` / `TRIGGER_MATCH_BACK` comparison; the resolver is the
shared `pal::resolveStyleColours` (`pal/style_colour.h`).

**Upstream:** the trigger's stored 0-7 ANSI number is compared straight against the
style run's raw `iForeColour` / `iBackColour`, without consulting
`iFlags & COLOURTYPE`. See `docs/UPSTREAM-BUGS.md` 75 for why that makes colour
conditions unmatchable on xterm-256, 24-bit, MXP and custom-palette runs.

**Now:** both sides are resolved to RGB through the same body `GetStyleRGB` paints
with, and compared there. Matching is by **appearance**, not by colour *number*.

**Why:** "the colour I can see" is the question a trigger author is actually asking,
and it is the only question that has an answer once a run can be painted from four
different colour spaces. Two consequences follow and are intended:

* an `ESC[38;5;1m` run and an `ESC[31m` run both display the world's red, so both now
  satisfy a "red" condition — that is the point of the fix;
* if a user configures two ANSI slots to the same RGB, a condition on either matches
  text painted with the other. Unavoidable once appearance is the question, and
  strictly rarer than the false positives the old code produced.

Everything that matched before still matches, with one exception, which is the
*removal* of a false positive: a `COLOUR_RGB` / `COLOUR_CUSTOM` run whose stored
number happened to equal an ANSI colour number no longer matches unless it really
displays that colour.

**Status:** `ProcessPreviousLine.cpp` is canonical Windows source, so a Windows build
from this tree carries the new semantics. Needs a Windows run against a real
xterm-256 world to see what existing trigger sets do with it — that risk is exactly
why this needed a decision (`docs/DECISION-QUEUE-2026-08.md` D-02) rather than a
patch. Covered headlessly by `TriggerColourMatch.*` in `tests/test_triggers.cpp`.

---

## 12 — WindowFilter's kernel is centred, and the last pixel column is filtered

**Where:** `miniwindow.cpp`, `GeneralFilter` — the vertical pass's look-ahead load and
the horizontal pass's loop bound. Reached by `CMiniWindow::Filter` operations 3 (blur),
4 (sharpen), 5 (edge detect), 6 (emboss), 25 (lesser blur) and 26 (minor blur), i.e.
`world.WindowFilter`.

**Upstream:** the vertical pass loads its `+2` tap from `row + 3`, centring the kernel
one row below the row it writes and skipping region row 2 entirely; the horizontal loop
stops at `iLastByte - 4`, leaving the rightmost pixel column unfiltered.

**Now:** `row + 2` and `col < iLastByte`. See `docs/UPSTREAM-BUGS.md` 78 and 79 for the
derivation and the safety argument.

**Why:** both are plainly defects rather than intent — the two passes are written as
mirror images of each other and disagree only here — but fixing them changes the pixels
**every existing plugin's filter call produces, on both platforms**, which is why it
needed a decision rather than a patch (`docs/DECISION-QUEUE-2026-08.md` D-06). A plugin
that visually tuned a blur against the off-centre kernel will render one row
differently and gain a filtered right edge. Nothing can depend on the old behaviour
deliberately, since neither is documented or reachable as an option, and the displaced
row plus the unfiltered stripe are artefacts a plugin author would have worked around
rather than relied on.

**Not changed:** the left-edge clamp still resolves an off-region tap to byte index 0
(pixel 0's blue byte for all three channels) rather than to the same channel of pixel 0,
and the B/G/R passes still run in place in that order, so a filtered left edge is still
slightly channel-skewed. That is upstream behaviour, it is not what D-06 asked about,
and the covering test pins the exact values so a future change to it cannot pass
unnoticed.

---

## 13 — `world.ReplaceNotepad` returns its boolean to Lua

**Where:** `scripting/lua_methods.cpp`, `L_ReplaceNotepad` — the result count.

**Upstream:** the body pushes its success boolean and then returns 0 result fields, so
Lua callers get nil. Python, Perl, JavaScript, Tcl and Ruby receive the boolean, because
they reach the method through the automation dispatch map, which registers it `VT_BOOL`
as `mushclient.odl` declares.

**Now:** `return 1;` — Lua gets the same boolean as the other five engines.

**Why:** the inconsistency has no defender. `git log -S` over the function finds only
the original import, there is no comment justifying the discard, nothing in the tree
documents a nil return, and both siblings on the same underlying body
(`AppendToNotepad`, `SendToNotepad`) return their boolean. It was the only
push-with-`return 0` body in the file.

**Compatibility:** this is a Lua-visible API change on Windows as well as the port.
Returning a value is additive for the overwhelmingly common uses — calling it as a
statement, or testing the result for truthiness — but a script written as
`if world.ReplaceNotepad(…) == nil then` would now take the other branch. That is the
intended correction, not a casualty of it.

---

## 14 — Lua's `world.SetTrace` prints the "Trace on/off" marker

**Where:** `scripting/lua_methods.cpp`, `L_SetTrace`.

**Upstream:** the Lua binding assigns `pDoc->m_bTrace` directly, bypassing
`CMUSHclientDoc::SetTrace` (`scripting/methods/methods_tracing.cpp`) and therefore
`OnGameTrace` — which flushes a half-built output line and emits the
`TRACE: Trace on` / `TRACE: Trace off` marker. Lua's `world.SetTrace` is consequently
silent.

**Now:** the binding calls `pDoc->SetTrace (…)`.

**Why:** the automation dispatch map binds the real accessor, so `world.Trace = 1` from
Python, Perl, JavaScript, Tcl and Ruby has always printed the marker. Lua was the only
language of the six whose SetTrace did nothing visible, and the asymmetry is invisible
from a script — a round-trip through `world.GetTrace()` reports success either way. The
port's bar is that a plugin behaves the same in every supported language.

**Behaviour preserved:** `SetTrace` acts only on an actual transition and `OnGameTrace`
owns the `m_bTrace` write, so a repeated `world.SetTrace(1)` remains a no-op, and
`world.GetTrace()` still reports the member. Ordering is unchanged too: the
half-built-line flush happens before `m_bTrace = true`, so a pending prompt is emitted
ahead of the marker, and the "off" marker is emitted while tracing is still on
(`Trace()` early-returns when it is not).

**Re-entrancy checked:** `Trace()` clears `m_bTrace` around the `ON_PLUGIN_TRACE`
callback as a loop guard and restores it afterwards, so a plugin's trace handler that
calls `world.SetTrace(false)` re-entrantly sees the guard's `false`, finds no
transition, and is swallowed. That is pre-existing and identical on the dispatch-map
path the other five languages use; this change inherits it rather than introducing it.

**Status:** no new Windows-only code, but the marker now appears in Lua worlds on both
builds — worth an eyeball that the flushed prompt lands on its own line there too.
Covered by `LuaScripting.SetTraceEmitsTheTraceMarker` and
`LuaScripting.SetTraceFlushesAHalfBuiltLine` in `tests/test_lua_scripting.cpp`,
mutation-verified.

---

## 15 — Bright SGR colours (90-97 / 100-107) are understood

**Where:** `ansi.cpp` — the custom-to-RGB switch (`:57-66`), the RGB-mode arms
(`:144-157`) and the ANSI-table `case` block (`:212-231`). Added in `f06e369`.

**Upstream:** stock MUSHclient handles SGR 30-37 / 40-47 (and the `39`/`49` defaults)
only. `ESC[91m` — "bright red", emitted by a large fraction of modern MUDs and by
anything that assumes an xterm-class terminal — falls through the switch and is
silently dropped, so bright text renders in the preceding colour.

**Now:** 90-97 set the foreground and 100-107 the background from the bold palette
(`m_boldcolour`) in RGB mode, and map to xterm palette indices 8-15 in the ANSI-table
path, which `GetStyleRGB` already resolves to the bright entries. `INVERSE` is honoured
on both arms, and a world in `COLOUR_CUSTOM` mode is promoted to `COLOUR_RGB` for these
codes exactly as it already is for 30-47.

**Why this belongs here:** `ansi.cpp` is compiled unconditionally, so a Windows build
from this tree carries the feature too. That is the intent — it is a
terminal-compatibility improvement, not a port workaround, and gating it behind
`_WIN32` would leave the Windows build failing to render colours its users' MUDs
actually send.

**Status:** needs a Windows run only to confirm the rendering matches macOS; there is
no Windows-specific code path. Covered by `tests/test_ansi.cpp` and
`tests/test_colour.cpp`.

---

## 16 — OSC sequences are swallowed instead of leaking into the output

**Where:** `telnet_phases.cpp` — `Phase_ESC`'s `]` arm (`:63-72`) and `Phase_OSC`
(`:88-96`). Added in `f06e369`.

**Upstream:** `ESC ] <text> BEL` (Operating System Command — window titles, the xterm
colour-palette protocol MUSHclient itself advertises support for in its terminal-type
negotiation) has no phase of its own, so the payload text falls through to `DisplayMsg`
and is printed to the user as literal garbage.

**Now:** `ESC ]` enters `HAVE_OSC`, which discards bytes until `BEL` or `ST` (`ESC \`).
The sequence is consumed, not displayed.

**Why this belongs here:** `telnet_phases.cpp` is compiled unconditionally and this is a
strict improvement on both platforms — Windows leaks the payload today. An earlier
revision of this code carried an `#ifndef _WIN32` guard on the mistaken belief that
Windows never entered `HAVE_OSC`; in fact Windows entered no such phase at all, which is
precisely the bug. The guard was removed and the reasoning is recorded in a comment at
the site.

**Status:** needs a Windows run to confirm the payload no longer appears in the output
window. Covered by `tests/test_telnet.cpp`.

---

## 17 — A world option can make short argument lists an error

**Where:** `platform/mc_dispatch.cpp`, `MC_DispatchInvoke` (the strict block after the
`kArgDefaults` padding); the option row lives in `scriptingoptions_numeric.inc` and the
member in `doc.h` (`m_bStrictScriptArguments`), both of which Windows compiles.

**Upstream:** there is no such option. On Windows the question cannot arise — the
alternate languages ride the real `IDispatch::Invoke`, whose dispatch map
(`doc_dispatch_map.inc`) declares **zero** `VTS_VARIANT`, so every declared parameter is
required and a short list is refused with `DISP_E_BADPARAMCOUNT`.

**Now:** the shared world-option table gains `script_strict_arguments`, default **off**.
Off, the port keeps its existing behaviour (a missing parameter is zero-filled). On, the
portable dispatch refuses the call with `MC_DISP_BADARGS` and the engine raises a
catchable native error naming the method and the counts. The option is **inert on
Windows**, where COM has already made the same check before any port code could run.

**Why:** the leniency is the port's own invention, and it is the only place where the six
languages disagree with each other — Lua's hand-written binding already errors
(`my_checkstring` on a missing argument) and Windows already errors, so only the five
embedded engines silently accept `world.SetVariable("x")`. Making it unconditional would
be the honest parity fix but would break existing plugins that (however accidentally)
rely on the zero-fill, so it is opt-in and off by default. The option is carried in the
**shared** table rather than a port-only one so that a world file written on macOS with
the option set still loads on Windows — which is the whole reason this is a divergence
and not a port-local setting.

**Refinement (`8fa8baa`, 2026-08-19) — an explicitly-passed nil is an omitted
argument.** The `kArgDefaults` padding used to fill only the slots *beyond* the
supplied argument count, so `world.EnableTrigger("t", None)` reached the method with a
zero where `world.EnableTrigger("t")` reached it with the documented default — the
first disabled the trigger the second enabled, both returning `eOK`. Python `None`,
Ruby `nil`, Perl `undef` and JavaScript `undefined`/`null` all marshal to `VT_EMPTY`,
which is *inside* the count. The padding now fills a `VT_EMPTY`/`VT_NULL` slot too.
**This is port-only** — `mc_dispatch.cpp` does not exist upstream — but it interacts
with the option above and the two readings must stay apart: strict counts the
arguments the caller *wrote*, so an explicit nil is a **supplied** argument for the
arity check while being an **unfilled** slot for the padding. Collapsing them would
make strict reject a call that the same option leaves working when spelled with the
argument omitted. The authority for the rule is Lua's own shipping binding, where
`optboolean` (`scripting/lua_methods.cpp:330-345`) says in so many words that nil
takes the default — and `kArgDefaults` is by its own comment the Lua `opt*` defaults
exactly, so Lua's rule *is* the table's rule.

**Note for anyone merging upstream:** this adds a row to `scriptingoptions_numeric.inc`
and a member to `CMUSHclientDoc`. Both are shared source; expect a conflict at both
sites. The rationale rests on a claim never observed on Windows in this project — that
`IDispatch::Invoke` really does refuse a short list for these methods; see
`docs/WINDOWS-VALIDATION.md` §4c-duoquadragies for the probe, and note that if COM turns
out to supply a missing optional instead, this section's parity argument is wrong and
the option becomes a port-only strictness rather than a return to Windows behaviour.

---

## 18 — The dead `DelayedSend` send-to target is deleted

**Where:** `doc.cpp` (the ~48-line `/* Delayed sending: … */` explanatory block and the
79-line `CMUSHclientDoc::DelayedSend` body), `doc.h` (the declaration), `doc_shared.cpp`
(the commented-out `case eSendDelayed:` arm inside `SendToTarget`'s switch),
`OtherTypes.h` (the commented `eSendDelayed` enumerator) and `flags.h` (the commented
`eDelayed` alias flag).

**Upstream:** all five pieces are present, but **four of the five are already commented
out** — and were commented out in the initial 2010-01-21 import (`ad0cd12`), with the
original author's own annotation `// ??` next to `eSendDelayed`. Only the function body
and its declaration were live code, and nothing called them. `git log -L` over the body
returns the import and nothing else.

**Now:** all five are removed.

**Why:** the feature is unreachable and always has been. `eSendDelayed` is not a member
of the send-to enum at all, so no value of the trigger/alias/timer `m_iSendTo` can name
it; the `IDC_SEND_TO` combo binds `DDX_CBIndex`, making the combo index the enum value,
and the resource supplies 15 entries for triggers and 14 for aliases/timers — exactly
the live enumerators, with no "delayed" row. The live replacement is `world.DoAfter*`,
which is what the deleted body itself called.

The "documented functionality" objection dissolves on inspection: the syntax is
documented in exactly **one** place and that place is the dead code itself. A tree-wide
grep finds no other copy — not in `plugins/MUSHclient_Help.xml`, not in
`install/readme.txt`, not in `tips.txt`, not in `docs/`. It is an internal design note
for functionality that was never shipped.

**Behaviour-neutral on both platforms.** Removing a *commented* enumerator shifts no
enum value — `eSendDelayed` never contributed one — so saved world files, plugin XML and
the `send_to` script API keep their exact numbering, and `eSendToLast` is unchanged.

**Latent defects removed with it** (context, not fixes — none was reachable): the
`iSeconds > 0` arm has `DoAfter` and `DoAfterSpeedWalk` **swapped** relative to the
`bSpeedWalk` flag it tests; the immediate arm strips `m_speed_walk_prefix` a **second**
time from a string it had already stripped; `bSemicolon` is computed but never decides
anything, so `3 north` schedules the whole string including the digit, contradicting the
block's own documented rule 5; and the returned `strSendNow` is declared, never assigned,
and returned with the comment `// any world.notes`, making the call site's
`strExtraOutput += DelayedSend (…)` a guaranteed no-op.

**Out-of-scope residue, deliberately left.** Removing `eDelayed` from `flags.h` leaves
two commented-out references to it in `scripting/methods/methods_aliases.cpp` (lines 205
and 290), now naming an enumerator that exists nowhere. They are the visible edge of a
wider dead "delayed alias" web — `OtherTypes.h:400,442`, `methods_aliases.cpp:205,289,290,413`,
`xml/xml_save_world.cpp:500`, `prefspropertypages.cpp:2638,2661,2705,2913`,
`aliasdlg.h:82` / `aliasdlg.cpp:341`, `world_debug.cpp:579` — every member of which is
already commented out. D-27 sanctioned removing the *enumerators*; the `bDelayed` web is
a separate consolidated feature that deserves its own decision. It is a pure
comment-deletion job with no build risk, but it touches seven more canonical files and so
widens the upstream-merge conflict surface.

**Update (`893395a`, 2026-08-19): the web is now deleted too.** Pure comment removal,
verified genuinely dead first — `bDelayed`, `eDelayed`, `IDC_DELAYED` and
`OnUpdateDelayed` were grepped across every `.cpp`/`.h`/`.vcxproj`/`.rc` and the
`plugins/` XML samples, and the only other hit was unrelated prose in
`plugins/sample_plugin.xml` ("the announcement is delayed a second"). So the
conflict surface this paragraph warned about has been taken deliberately rather than
avoided: seven more canonical files (`OtherTypes.h`, `methods_aliases.cpp`,
`xml/xml_save_world.cpp`, `prefspropertypages.cpp`, `aliasdlg.h`, `aliasdlg.cpp`,
`world_debug.cpp`) now differ from upstream by deleted comments. Nothing executable
changed, on either platform.

---

## 19 — The retired per-world "Sounds" prefs page (`CPrefsP2`) is deleted

**Where:** `dialogs/world_prefs/prefspropertypages.h` (the class),
`dialogs/world_prefs/prefspropertypages.cpp` (its implementation plus the
separately-located `CPrefsP2::OnSetActive`), `dialogs/world_prefs/configuration.cpp`
(the three doc helpers `LoadPrefsP2` / `ChangedPrefsP2` / `SavePrefsP2` and the five
commented-out sites that used to construct, load, add, compare and save the page) and
`doc.h` (the forward declaration and the three helper declarations).

**Upstream:** the class and its three helpers are live code, but every site that would
*use* them is commented out, and all five were already commented out in the 2010-01-21
import (`ad0cd12`). The page was consolidated into `CPrefsP14` ("Output"), which is
constructed two lines below the commented-out `page2` in the same "appearance" group.

**Now:** the class, its `OnSetActive`, the three helpers and the five dead comment sites
are removed.

**Why:** `CPrefsP14` carries a strict superset of what `CPrefsP2` touched, and carries it
*correctly*, which `CPrefsP2` no longer did. `CPrefsP2` had exactly **one** data member,
`m_sound_pathname`, standing for the doc field `m_new_activity_sound`:

| doc field | via `CPrefsP2` | via `CPrefsP14` (live) |
|---|---|---|
| `m_new_activity_sound` | load: **absent** — `LoadPrefsP2` only set `page2.m_doc` | load: `page14.m_sound_pathname = m_new_activity_sound` |
| | changed: `m_new_activity_sound != page2.m_sound_pathname` | changed: `m_new_activity_sound != page14.m_sound_pathname` |
| | save: **absent** — `SavePrefsP2` had only a `DelayDebugMsg` | save: `m_new_activity_sound = page14.m_sound_pathname` |
| `m_strBeepSound` | not handled at all | full load / changed / save, bound to `IDC_SOUND_PATH` |

So the deletion is strictly an improvement: had `CPrefsP2` ever been re-enabled as it
stood, it would have been **broken** — its load never populated the field so the page
would open blank, its "changed?" test would then report a change on every visit, and its
save wrote nothing, so the user's edit would be silently discarded. (A correction to the
queue's own text, which said all three helpers "are empty bodies": only `SavePrefsP2` is.
`LoadPrefsP2` is gutted but not empty, and `ChangedPrefsP2` has a live body.)

**Behaviour-neutral on both platforms.** `CPrefsP2` was never constructed on either
platform, so no user-visible setting changes. `m_new_activity_sound` keeps its full
dialog round-trip through `CPrefsP14`, its XML attribute `new_activity_sound`, and its
script accessor (`methods_info.cpp` case 9).

**Deliberately left in place:** `IDD_PREFS_P2` — the dialog template in
`platform/mushclient_dialogs.rc` and its `resource.h` define — now referenced by no code,
kept as inert resource data to keep the divergence minimal (the port's `.rc` parser only
builds a template on demand, so an unreferenced one costs nothing). `ePageSounds` is
already commented out at `doc.h:392` and at its one other (also commented) use, and is
self-consistent as it stands.

**Merge guidance for 18 and 19:** anyone merging upstream will get a conflict at each
deleted site. Keep the deletion unless the decision is revisited — upstream's own copy of
every one of these sites is either commented out or uncalled, so taking upstream's side
re-imports dead code rather than restoring a feature.

## Verification status

Entries 1-3 are in `sendvw.cpp`, which compiles on macOS (`CMakeLists.txt:557`)
but whose class `CSendView` is never instantiated on the port — so the macOS
build type-checks them and cannot exercise them. The behaviour was verified
live through the port's own equivalent in `AltArrowRecall`.

**All four `sendvw.cpp` entries (1, 2, 3 and 5) were run on Windows on
2026-08-10** against this build and stock 5.06 side by side, with the same
history (`abc, abcd, abcde, northeast, north`) and the same keystrokes. The two
traces differ at exactly these four points and nowhere else —
`docs/WINDOWS-VALIDATION.md` §8v has them in full:

| | this build | stock 5.06 |
|---|---|---|
| 1 — Alt+Up with `north` typed, `north` newest | `north` | `northeast` |
| 2 — Alt+Down past the newest match | `no` restored | bar blanked |
| 3 — ordinary Down past the newest entry | `abc` restored, then blank | blank at once |
| 5 — Shift+Return / Alt+Return | `\r\n` inserted, nothing sent | sent |

Entry 5's two specific hazards are both answered. `ES_MULTILINE` +
`ReplaceSel("\r\n")` does put a real line break in the control — the bar read
back as `"line one\r\nline two\r\nline three"` — and Alt is **not** left
latched into menu-activation state: typing straight after Alt+Return still
lands in the bar, the focus window is still the edit, and no menu is owned.
Plain Return still sends and still clears the bar.

The macOS half of entry 5 (in `wx_world_child.cpp`) was already verified live
(Shift+Return and Option+Return insert a line break; plain Return still sends).

Entry 4 is different: it lives in `GlobalPrefs.cpp` and
`scripting/methods/methods_defaults.cpp`, both of which the port *does* execute,
and its seam is covered headlessly by `DefaultFonts.*` in
`tests/test_pal_worldview.cpp`.  The engine-side behaviour is therefore verified
here; what a Windows run would still add is confirmation that `CMUSHView`
repaints from the same `ChangeFont` call, which it already does for the
World Properties font path.

Entry 6 was found and verified on Windows (`docs/WINDOWS-VALIDATION.md` §8g,
§8j): a stock 5.06 and this build both listed an un-bound key until the erase
was hoisted, and `tests/lua/api_conformance.lua` reports the list either way,
so re-running it against both clients is the check.

**Entries 11-19 (the 2026-08-19 decision campaign) are NOT Windows-verified.** All nine
are type-checked on macOS and covered headlessly, and none has a Windows-only code path,
but no Windows build was made this campaign. What each still wants is in
`docs/WINDOWS-VALIDATION.md` §4c-quintricies through §4c-duoquadragies. Three of them
change something a user or a script can see on Windows and are worth singling out:

| Entry | What a Windows run has to answer |
|---|---|
| 11 — colour matching by appearance | What existing trigger sets do against a real xterm-256 world. This is the one with genuine regression risk: the semantics of a matcher every colour trigger goes through have changed. |
| 12 — WindowFilter | Every plugin's blur/sharpen output shifts by one row and gains a filtered right edge. Visual only, but visible. |
| 13 — `ReplaceNotepad` | A Lua script written as `if world.ReplaceNotepad(…) == nil then` now takes the other branch. |

Entries 18 and 19 need no behavioural run at all and cannot have a behavioural test —
the removed code was unreachable on **both** platforms, which is the whole claim. What
was verified instead is absence: after the removals, a word-boundary grep across the tree
(excluding `.git`, `build/` and `docs/`) returns no match for `DelayedSend`,
`eSendDelayed`, `CPrefsP2`, `LoadPrefsP2`, `ChangedPrefsP2`, `SavePrefsP2` or the `page2`
local, and the `ctest` count is identical before and after — the intended result, since
no test referenced any of it. A Windows **compile** is still needed: both touch canonical
files in `MUSHclient.vcxproj`.

**Two proposals were considered for this file and deliberately not applied**, on the
strength of its own header rule (this file is for behaviour that differs from upstream on
**both** builds; macOS-vs-Windows differences belong in the code as port comments):

* **`utils.menufontsize()` returning nil** when `SystemParametersInfo` fails. On Windows
  `SPI_GETNONCLIENTMETRICS` succeeds, so the new guard is never taken and Windows
  behaviour is bit-identical to stock in every reachable state. It is a canonical-source
  edit a merger will hit, so the reasoning lives in a comment at
  `scripting/lua_utils.cpp` and the close-out is recorded under
  `docs/DECISION-QUEUE-2026-08.md` D-25.
* **`CMainFrame::LeftTrayClick` acting on the menu choice it receives.** The captured
  `TrackPopupMenu` return is explicitly discarded under `_WIN32` (`(void) nSelected;`),
  so Windows is byte-for-byte unchanged; the port-only arm is the same shape as the one
  `CMainFrame::OnTrayMenu` already carries two functions below, which is likewise not an
  entry here. The canonical-source change that does need confirming — the
  `CollectWorldMenuItems` extraction — is in `docs/WINDOWS-VALIDATION.md`
  §4c-unquadragies.

The notepad's four line-ending conversions were considered too and fail the same test:
`CTextDocument::m_iLineEndings` is inside `#ifndef _WIN32`, so the Windows build converts
bytes exactly as upstream does. The port's representation and the two behavioural
differences that follow from it are recorded in CLAUDE.md, and the upstream data-loss
defect it sidesteps is `docs/UPSTREAM-BUGS.md` 82.

**Two more were considered in the second consolidation round (2026-08-19) and fail the
same test.** Both were written up by their branches as proposed entries for this file;
both are macOS-vs-Windows differences, so both are recorded as comments at their sites
and in `docs/DECISION-QUEUE-2026-08.md` instead:

* **The status bar's connected-time pane reporting the connection rather than the
  window's age** (D-13, `65fa4ed`). Windows fills `ID_STATUSLINE_TIME` from
  `CMUSHclientDoc::OnUpdateStatuslineTime` (`doc.cpp:1069`), which renders
  `CTime::GetCurrentTime() - m_tStatusTime` — wall-clock time since the world *window*
  opened or was last reset, counting on while disconnected. The port now shows the
  current connection, with the cumulative total in parentheses and `past` marking a
  finished one. The whole change is in `platform/wx_world_child.cpp` and
  `wx_world_child.h`; **no canonical source is touched**, `doc.cpp:1069`,
  `doc.cpp:3607` and `methods_output.cpp:87` are untouched, and the Windows pane and
  both Windows resets behave exactly as before. The mechanism that keeps Game ▸ Reset
  Connected Time observable without editing canonical source — reading `m_tStatusTime`
  as a display epoch that upper-bounds both figures — is the load-bearing part and is
  recorded at the site.
* **Right-click resolving the click to a word when nothing is selected** (D-29,
  `3fe8413`). Windows does not: `CMUSHView::OnRButtonDown` (`mushview.cpp:2283`) offers
  the point to the miniwindow layer and falls through to `CView`, and every
  context-dependent handler reads the selection the user made earlier. The port follows
  the macOS platform convention instead, and only in `platform/wx_outputpane.cpp`
  (`COutputPane::OnRightDown`). An existing selection is left *exactly* alone, which is
  the half that matters: Copy is the menu's most-used item and copies the selection, so
  a right-click that moved it first would make Copy copy the wrong thing. The branch
  reached this conclusion itself and placed the note at the site; it is repeated here
  so the exclusion is visible from this file.

## 20 — "Bytes sent" is counted once, where upstream counts it twice

**Where:** `doc_shared.cpp`, `CMUSHclientDoc::SendPacket`.

**Was:** `m_nBytesOut += nBufLen` in `SendPacket` **and**
`pDoc->m_nBytesOut += count` in `CWorldSocket::OnSend` for every chunk that
drained — and `SendPacket` calls `OnSend (0)` itself, so both ran on every send.
The reported figure is double the bytes actually sent
(`docs/UPSTREAM-BUGS.md` 92).

**Now:** counted once, at the point the engine hands the buffer to the
transport. The wire-side count went with `CWorldSocket` in the T5 relocation and
was deliberately not reinstated; the comment at the site says so.

**Why it is here rather than only in UPSTREAM-BUGS:** it changes a number the
user can see. Anyone comparing this build's world-properties Info page, or
`world.GetInfo (217)`, against stock 5.06 on the same session will get half the
figure — and half is the correct half. Received bytes were always counted once
and are unchanged, so the visible effect is that the out:in ratio stops being
inflated.

**Note on WHERE it is counted.** Upstream's second count was at the wire;
counting once at `SendPacket` means the figure is "bytes handed to the
transport", which can briefly exceed "bytes on the wire" while the send queue is
draining, and permanently so if a session drops with data still queued. That is
the right trade for a statistics display — the alternative reinstates the
double-count — but it is a real difference from "bytes the peer could have
received", so it is written down.

**Verification:** `WinTransport.BytesSentAreCountedOncePerSendPacket` sends 5
then 6 bytes through a real `pal::WinTransport` over loopback, asserts the
counter reads 5 then 11, and then asserts the drain does not add any more.

## 21 — MXP element/entity collection is capped, where upstream is unbounded

**Where:** `doc_shared.cpp`, `CMUSHclientDoc::DisplayMsg` (the MXP-phase dispatch).

**Was:** while collecting an MXP element (`<…`), comment, quote or entity
(`&…`), each received byte was appended to `m_strMXPstring` with no length
limit. Collection ends only on a terminator — `>`/`;`, or one of newline, CR,
ESC, IAC (which abandon it as unterminated). So a hostile MXP-enabled server
could send `<` (or `&`) followed by megabytes containing none of those bytes and
grow `m_strMXPstring` without bound — a remote memory-exhaustion denial of
service. The analogous telnet-subnegotiation buffer was already capped for
exactly this reason (`MAX_SUBNEGOTIATION_LENGTH`, `71c6afd`); the MXP collection
phases were not.

**Now:** the four collection phases are capped at 256 KB. On reaching it the
element is abandoned (`MXP_unterminated_element`, the buffer cleared and the
phase reset to `NONE`, so the overflow bytes become ordinary text) exactly as
the newline/ESC/IAC terminators already do. No legitimate MXP construct
approaches 256 KB — real tags, entities and `<!ELEMENT>`/`<!ENTITY>` definitions
are at most a few KB — so nothing well-formed is affected.

**Why it is here rather than only in UPSTREAM-BUGS:** it is shared code compiled
on Windows too, and it changes behaviour against a (pathological) server —
upstream would keep collecting until it ran out of memory; this build stops at
256 KB and logs an unterminated-element error. A server that legitimately needed
a larger MXP payload (none is known to exist) would see it truncated here.

**Verification:** `MXPFlood.UnterminatedElementCollectionIsBounded` feeds a
600 KB unterminated element and asserts `m_strMXPstring` stays under 300 KB;
without the cap it reaches the full flood size.

## 22 — Control characters in XML attribute values: reader and writer are symmetric

**Where:** reader — `xml/xmlparse.cpp`, `CXMLparser::Get_XML_Entity` (the
disallowed-number check); writer — `xml/xml_save_world.cpp`,
`Save_XML_string`, via the new `FixXMLAttributeString`
(`utilities_shared.cpp`, declared in `stdafx.h`).

**Was (upstream):** `Get_XML_Entity` accepted only `&#9;` among the control
characters, rejecting `&#10;`/`&#13;` with *"Disallowed number in XML
entity"*. The writer removed only the exact CRLF pair
(`Replace (str, ENDLINE, " ")`) and `FixHTMLString` escaped only
`< > & "` — so a member holding a lone LF, lone CR, or a TAB was written
**raw** into the quoted attribute value, and `CXMLparser::GetValue` refuses a
raw control character there (*"not terminated"* for CR/LF, the `< ' '` reject
for tab). Upstream therefore already had the asymmetry for TAB: it could load
`&#9;` but never re-emit it, and a tab that reached a trigger/alias/timer
member (e.g. `world.AddTrigger` stores match text unfiltered) produced a world
file its own loader rejects. The same script route with an embedded `\n`
produced an unopenable file on both platforms.

**Now:** the reader accepts `&#9;` / `&#10;` / `&#13;` (widened in `f96a15c`,
recorded here retroactively — it had no entry), and the writer escapes a
residual tab/LF/CR in an attribute value as the matching numeric entity. The
ENDLINE collapse is kept, so a CRLF pair still becomes a space exactly as
upstream writes it and the output for every existing loadable world file is
byte-identical; only values that today produce a file the parser rejects
change form. `FixHTMLString` itself is untouched (it is shared with
`WriteToLog`, the log-file-name `%`-expansions and `world.FixupHTML`, and its
exact output is pinned by `tests/test_utilities.cpp`).

**Compatibility:** a file that actually contains `&#10;`/`&#13;` is refused by
stock (un-widened) Windows MUSHclient at load time — but the same content was
unloadable there before too (as a raw control character), so nothing that
round-trips today stops doing so. It does make the reader widening
load-bearing for round-tripping on this build: both halves travel together.

**Verification:** `WorldLoad.NumericEntitySurvivesSaveAndReload` (load
`&#10;`/`&#13;`, save, assert the entities are re-emitted and the file
reloads byte-identically — fails without the writer half),
`WorldLoad.AddTriggerEmbeddedControlCharsSurviveSaveAndReload` (the script
route, LF and TAB), and `WorldLoad.RawLinefeedInAttributeValueIsRejected`
(negative control: the parser still rejects the raw form, which is what makes
the escape load-bearing).
