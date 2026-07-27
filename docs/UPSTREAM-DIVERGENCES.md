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

## Verification status

All three changes are in `sendvw.cpp`, which compiles on macOS (`CMakeLists.txt:557`)
but whose class `CSendView` is never instantiated on the port — so the macOS
build type-checks them and cannot exercise them. The behaviour was verified
live through the port's own equivalent in `AltArrowRecall`. **A Windows build
run is still required to confirm them on that side.** See
`docs/WINDOWS-VALIDATION.md`.
