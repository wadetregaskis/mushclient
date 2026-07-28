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

## Verification status

Entries 1-3 are in `sendvw.cpp`, which compiles on macOS (`CMakeLists.txt:557`)
but whose class `CSendView` is never instantiated on the port — so the macOS
build type-checks them and cannot exercise them. The behaviour was verified
live through the port's own equivalent in `AltArrowRecall`. **A Windows build
run is still required to confirm them on that side.** See
`docs/WINDOWS-VALIDATION.md`.

Entry 4 is different: it lives in `GlobalPrefs.cpp` and
`scripting/methods/methods_defaults.cpp`, both of which the port *does* execute,
and its seam is covered headlessly by `DefaultFonts.*` in
`tests/test_pal_worldview.cpp`.  The engine-side behaviour is therefore verified
here; what a Windows run would still add is confirmation that `CMUSHView`
repaints from the same `ChangeFont` call, which it already does for the
World Properties font path.
