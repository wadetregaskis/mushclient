# Upstream bugs found & fixed during the macOS port

These are defects in shared, platform-independent logic — present in the
upstream Windows MUSHclient source, not port artifacts. Each was found while
bringing the real shipping code under the headless test suite, fixed in this
tree, and is suitable to report/patch upstream. Port file names are given with
their upstream equivalent where the port single-sources a method into a shared
TU (e.g. doc_shared.cpp ← doc.cpp).

| # | Area | Port commit | Upstream file / function |
|---|------|-------------|--------------------------|
| 1 | MXP element delete UAF | `2cae10b` | mxpDefs.cpp `CMUSHclientDoc::MXP_Element` |
| 2 | MCCP decompress realloc | `ae6bbe4` | doc.cpp (MCCP decompress grow loop) |
| 3 | Deferred-note drain UAF | `f0f2866` | doc.cpp `CMUSHclientDoc::OutputOutstandingLines` |
| 4 | XML double signed exponent | `49e5ba6` | xmlparse.cpp `Get_XML_double` |
| 5 | XML date day-range message | `49e5ba6` | xmlparse.cpp `Get_XML_date` |
| 6 | XML date second-range message | `49e5ba6` | xmlparse.cpp `Get_XML_date` |
| 7 | Timer at_time toggle HOUR→MINUTE | `eea4a45` | methods_timers.cpp `CMUSHclientDoc::SetTimerOption` |
| 8 | Custom-colour index used unchecked (`note_text_colour`) | `36db6c1` (partial) | methods_noting.cpp note-colour getters; prefspropertypages.cpp `FixSwatch` |
| 9 | xterm-256 colour index subscripts the 8-entry print-style arrays | port-guarded only | mushview.cpp `CMUSHView::PrintWorld` |
| 10 | DBCS wrap splits the character when `indent_paras` is off | this commit | doc_shared.cpp ← doc.cpp `CMUSHclientDoc::AddToLine` |
| 11 | `ClearOutput` leaves `m_last_line_with_IAC_GA` naming a deleted line | this commit | doc.cpp `CMUSHclientDoc::ClearOutput` |
| 12 | The queued-command "no log" flag can never suppress logging | this commit | TimerWnd.cpp `CTimerWnd::OnTimer`; mirrored in platform/wx_world_child.cpp |
| 13 | Windows' two queue drains disagree about case, so a `nolog` command echoes from one and not the other | this commit | TimerWnd.cpp `OnTimer` vs `ChangeTimerRate` |

## 1 — MXP `<!ELEMENT … DELETE>` use-after-free (`2cae10b`)
**Bug:** redefining or deleting a custom MXP element frees the old `CElement`
(`delete pElement`) but does not remove its key from `m_CustomElementMap`. The
map key is left pointing at freed memory; any later `<x>` use or redefinition of
the same element dereferences/deletes the dangling pointer.
**Fix:** call `m_CustomElementMap.RemoveKey(strName)` immediately after deleting
the element, so the map never holds a freed pointer.

## 2 — MCCP decompress buffer realloc dangling pointer (`ae6bbe4`)
**Bug:** when inflate returns `Z_BUF_ERROR` the output buffer is grown with
`realloc`, but `z_stream::next_out` still points into the *old* block. `realloc`
may move the allocation, after which inflate writes decompressed bytes through
`next_out` into freed memory.
**Fix:** record `next_out`'s offset from the buffer base before the `realloc`,
then re-point `next_out = buffer + offset` afterwards, so it tracks the moved
block.

## 3 — Deferred-note drain re-entrancy UAF (`f0f2866`)
**Bug:** `OutputOutstandingLines` iterates `m_OutstandingLines` while calling
`Tell()` inside the loop. `Tell` can re-enter the function (Tell → forced line
change → StartNewLine → ProcessPreviousLine → OutputOutstandingLines); the
re-entrant `m_OutstandingLines.clear()` invalidates the outer loop's iterator →
use-after-free. Latent (current call sites drain with `hard_return` set / onto a
comment line, so the recursion guard bails) but fragile.
**Fix:** `swap` the list into a local before iterating. The member is emptied up
front (a re-entrant call sees it empty and returns immediately) and the loop
walks the detached copy, immune to re-entrant mutation. Non-re-entrant behaviour
is unchanged.

## 4 — `Get_XML_double` rejects signed exponents (`49e5ba6`)
**Bug:** in the numeric-attribute validator, on seeing `'E'` the exponent-sign
check tests `*p` — but `*p` is still the `'E'` (the loop's `p++` has not run), so
a following `'-'`/`'+'` is never consumed and falls through to the
invalid-character branch. Values like `"1E-5"` are wrongly rejected.
**Fix:** inspect `p[1]` (the character after the `'E'`) and consume the sign when
present, so signed exponents parse. Strictly more permissive.

## 5 — `Get_XML_date` day-range error message (`49e5ba6`)
**Bug:** the day-of-month range check (`iDay < 1 || iDay > 31`) throws
`"Month must be in range 1 to 31"` — a copy-paste from the month check.
**Fix:** message corrected to `"Day must be in range 1 to 31"`. (Message text
only; no behavioural change.)

## 6 — `Get_XML_date` second-range error message (`49e5ba6`)
**Bug:** the seconds range check (`iSecond < 0 || iSecond > 59`) throws
`"Minute must be in range 0 to 59"` — a copy-paste from the minute check.
**Fix:** message corrected to `"Second must be in range 0 to 59"`. (Message text
only; no behavioural change.)

## 7 — `SetTimerOption("at_time")` copies HOUR into the MINUTE field (`eea4a45`)
**Bug:** toggling a timer between interval (`every`) and at-time modes copies the
component fields across, but the MINUTE copy takes the HOUR source on both arms —
a duplicated-line typo: `iEveryMinute = iAtHour` (should be `iAtMinute`) and
`iAtMinute = iEveryHour` (should be `iEveryMinute`). An "every 2h 30m 15s" timer
flipped to at-time becomes "at 2:02:15" (minute forced to the hour value), and
vice-versa; the hour and second fields copy correctly.
**Fix:** copy the matching minute field (`iEveryMinute = iAtMinute`,
`iAtMinute = iEveryMinute`). Behavioural change for any script/user that toggles a
timer's `at_time` option on a timer whose hour and minute differ.


## 8 — an unvalidated `note_text_colour` is used as an array index

`m_iNoteTextColour` is meant to be a 0..`MAX_CUSTOM` custom-colour index, but
its option-table row permits far more (`scriptingoptions_numeric.inc`):

```
{"note_text_colour", 4, O(m_iNoteTextColour), 0, 0xFFFFFF, OPT_RGB_COLOUR},
```

so `world.SetOption("note_text_colour", 300)` is accepted and stored. Nothing
downstream range-checks it.

**The serious instance — always live, both platforms.** `methods_noting.cpp`
uses it as a bare subscript in roughly ten places, guarded only by an
`== SAMECOLOUR` special case, e.g.:

```cpp
return m_customtext [m_iNoteTextColour];     // GetNoteColourFore
m_iNoteColourBack = m_customback [m_iNoteTextColour];
```

`m_customtext` / `m_customback` hold `MAX_CUSTOM` (16) entries, so any value in
16..65534 reads hundreds of elements past the array on ordinary `Note` /
`NoteColourFore` / `NoteColourBack` calls — no dialog involved.

**The readers are fixed (2026-08-05).** All seven now go through one guarded
accessor, `CMUSHclientDoc::NoteCustomColourIndex`, which returns -1 for
anything that is not a usable index; out of range then behaves exactly as
`SAMECOLOUR` already did, which is what the two readers that *did* check were
already doing. Removing that guard segfaults the test suite — the overrun was
real, not theoretical.

**The option row itself is NOT fixed, and tightening it is not the small change
it looks like.** `OPT_RGB_COLOUR` does not merely set a maximum: it is also
what routes the value through `Save_XML_colour` → `ColourToName` on the way to
the world file, so `note_text_colour` is stored on disk as a **colour name**,
not a number. Drop the flag and:

* the writer switches to `Save_XML_number` and the reader to the number branch,
  so **every existing world file's stored colour string stops parsing** and the
  setting silently reverts to the default; and
* `OPT_CUSTOM_COLOUR` — the flag `echo_colour` uses, and the one that matches
  how this field is actually consumed — carries a ±1 encoding
  (`doc_shared.cpp`: subtract 1 on set, add 1 on get, 0 ⇄ SAMECOLOUR), so every
  value would additionally **shift by one**.

**It is a singleton, not a class (swept 2026-08-05).** The obvious worry after
finding this is that other option rows lie about their type the same way. They
do not:

* Every one of the twelve `OPT_RGB_COLOUR` rows has a genuine `RGB(r,g,b)`
  default — **except `note_text_colour`, whose default is a bare `4`.** That is
  the mechanical tell, and it is worth remembering: a colour-flagged row whose
  default is a small integer is declaring the wrong thing.
* `echo_colour` is the only `OPT_CUSTOM_COLOUR` row on the world, correctly
  flagged and correctly capped at `MAX_CUSTOM`.
* The trigger table's `custom_colour` row is capped at `MAX_CUSTOM + 1`, which
  looks like an off-by-one and is not: `OTHER_CUSTOM` is 16, the "use the RGB
  fields instead" sentinel, and `ProcessPreviousLine.cpp:1445` tests for it
  before any subscript. Stored 0..15 index the arrays; 16 never reaches them.
* Every other site that subscripts `m_customtext` / `m_customback` is either
  range-guarded already (`world_debug.cpp:966`, `:1831`) or provably in range
  (`doc_shared.cpp:735` reads `m_echo_colour`, whose row caps it correctly).

So the semantically correct row (`OPT_CUSTOM_COLOUR`, max `MAX_CUSTOM`) needs a
world-file migration, not a one-line edit. Note also that
`world.NoteColour` (`methods_noting.cpp:139-146`) already implements that exact
1-based convention by hand and validates it — so the two script APIs onto this
one field disagree by one, and only the property validates. That is the real
shape of the defect, and it is still **Wade's call**.

**The instance fixed in `36db6c1`.** The same bad value reaches
`CPrefsP9::FixSwatch` / `CPrefsP17::FixSwatch`, which special-case only
`iNewColour == 0` and otherwise index `m_customtext [iNewColour - 1]`. The
argument is a `CComboBox::GetCurSel()`, which returns **`CB_ERR` (-1)** when the
stored value is outside the combo's range (`SetCurSel` rejects it), so the else
arm reads index **-2**. The two `OnSelchange*` handlers next door already wrap
the identical call in `if (... != CB_ERR)`; the `OnSetActive` call sites did
not. Fixed by moving the guard inside `FixSwatch` so every caller is covered.

Scope, after an adversarial review of the original report:
* The `CPrefsP17` path is real — reachable by `SetOption("note_text_colour", N)`
  with N in 16..65534.
* The `CPrefsP9` path is **not** reachable: `echo_colour` carries
  `OPT_CUSTOM_COLOUR`, and `SetBaseOptionItem` decrements after the range check,
  so its maximum lands on the last valid combo index. The guard there is
  harmless belt-and-braces, not a fix.
* Index -2 lands *inside* the same `CPrefsP3` object (about 46 `CColourButton`
  members precede the array), so the consequence is two swatch buttons painted
  with arbitrary colours — not a fault, and not something ASan detects by
  default (it does not catch intra-object overflow).
* Windows runs the same unguarded code. This was latent on the port only
  because the dialog bridge never invoked `OnSetActive` until `d762469`.

## 9 — an xterm-256 foreground index subscripts the 8-entry print-style arrays

**Where:** `mushview.cpp:3952-3967`, inside `CMUSHView::PrintWorld`'s per-style
loop.

**Bug:** the world's print styles live in `int m_nNormalPrintStyle [8]` and
`int m_nBoldPrintStyle [8]` (`doc.h:830-831`) — one entry per ANSI colour.
`PrintWorld` subscripts them with `pStyle->iForeColour` whenever the style is
`COLOUR_ANSI`:

```
    if (style & HILITE)
      print_font (pcb, pDoc->m_nBoldPrintStyle [pStyle->iForeColour]);
    else
      print_font (pcb, pDoc->m_nNormalPrintStyle [pStyle->iForeColour]);
```

but for an xterm-256 style `iForeColour` holds 0..255, not 0..7 — the same
field, in the same union of cases, that `CMUSHView::OnDraw` reads as
`xterm_256_colours [iForeColour]` after testing `iForeColour >= 8`
(`mushview.cpp:1365`), and that the drawing code asserts is `< 256`
(`mushview.cpp:1361`). So printing a world whose output contains any 256-colour
escape reads up to 248 ints past the end of an 8-int member array, and the
resulting garbage is used as a `FONT_*` bitmask.

**Consequence:** wrong bold/italic/underline on printed lines at best; the read
stays inside the `CMUSHclientDoc` object (both arrays are members, with plenty
following them), so it is an intra-object overflow — no fault, and not something
ASan catches by default. Same class as bug 8.

**Status:** not fixed upstream-style here, because the fix belongs in a Windows
TU this port does not exercise. The port's own print path
(`platform/wx_world_child.cpp` `CWorldChildFrame::PrintWorld`) declines to
reproduce it: it treats an index outside 0..7 as `WHITE`, which is already what
the non-ANSI arm of the same `if` does. A Windows fix would be the same clamp on
those four lines.

## 10 — DBCS wrap splits the character when "indent paragraphs" is off

**Found 2026-08-04 while writing coverage for the wrap split-point; fixed
2026-08-05 after Wade confirmed it should be corrected on Windows too.**

`CMUSHclientDoc::AddToLine`'s auto-wrap looks for a split point at "a multibyte
character or a space, whichever comes last". The DBCS arm records the index of
the **lead** byte of the pair, precisely so the wrap lands before the character
rather than inside it.

The `!m_indent_paras` block that follows was written for the SPACE arm:

```
if (!m_indent_paras)
  {
  saved_count--;    // one less to copy
  last_space++;     // one more on this line (the space)
  m_pCurrentLine->len = last_space; // this line is longer
  }
```

Keeping the space on the old line is right — you do not want a continuation line
starting with a space. But `last_space` may have come from the DBCS arm, where it
is the index of a **lead byte**, and incrementing it leaves the lead byte at the
end of the old line and starts the continuation line on the orphaned trail byte.
The double-byte character is split exactly as the DBCS arm exists to prevent.

**Reproduction** (now `WrapTest.DbcsPairStaysWholeWithIndentParagraphsOff`,
which fails without the fix): a non-UTF-8 world,
`wrap_column` 20, `indent_paras` off, and a line of GB2312 pairs `D6 D0` behind
a 5-byte ASCII prefix so the pairs sit on odd offsets. The split point is found
at byte 17 (a lead byte); `last_space++` makes it 18, so line 0 ends `… D6` and
line 1 begins `D0 …`. It happens on pair-aligned Big5 input too.

**Severity is limited by the default:** `indent_paras` defaults to **true**
(`scriptingoptions_numeric.inc:85`), so the block does not run unless the user
turns "indent paragraphs" off. A CJK world with that option off would see torn
characters at every wrap.

**Fix:** record WHICH arm found the split point (`bSplitAtSpace`, set false by
the DBCS arm and true by both space arms) and gate the `!m_indent_paras`
adjustment on it. A DBCS split point needs no adjustment at all — it is already
on a character boundary, so the continuation starts on a lead byte either way,
and there is no space to absorb.

The adjustment itself is untouched for the case it was written for, which the
new `WrapTest.SpaceSplitKeepsTheSpaceOnTheOldLineWithIndentParagraphsOff` pins:
with indenting off, a space split still leaves the space at the end of the old
line so the continuation does not begin with one. Mutating the gate *both* ways
kills tests — restoring the unconditional form fails the DBCS test, and
removing the adjustment entirely fails four space-arm tests.

**This changes Windows rendering** for a non-UTF-8 CJK world with "indent
paragraphs" off: the wrap now falls one byte earlier and the character stays
whole. That is the point of the fix, and it is on the Windows validation list
(`docs/WINDOWS-VALIDATION.md` § 4c-duodetricies).

The two earlier tests (`WrapTest.DbcsPairStraddlingWrapColumnMovesWholeToNextLine`,
`WrapTest.TrailByteInTheGapBetweenDbcsRangesIsNotASplitPoint`) still set
`m_indent_paras = 1` explicitly, and now say what the other setting does rather
than avoiding it.

## 11 — `ClearOutput` leaves the recorded IAC GA line number stale

**Bug:** `CMUSHclientDoc::ClearOutput` deletes every line and restarts the
numbering (`m_total_lines = 0`, then the re-seeded line becomes number 1), but
leaves `m_last_line_with_IAC_GA` holding the number it had before. That number
now names a line which does not exist, and — because the counter went back to
zero — usually one that *cannot* exist: `world.GetInfo(289)` will happily
report line 4,312 of a buffer holding one line.

A script using it the obvious way (`GetLineInfo(GetInfo(289), 1)` to re-read
the last prompt) gets a silently wrong line rather than an error, because
`GetLinePosition` clamps out-of-range numbers to the last line.

**Fix:** zero it alongside `m_total_lines`, which is the only place the two can
disagree. The port's own `m_last_prompt_line` — the line the PROMPT flag was
stamped on, used by Clear to Start — is reset in the same place and for the
same reason.

Covered by `Utilities.ClearOutputForgetsTheRememberedPromptAndGaLine`.


## 12 — the queued-command "no log" flag can never suppress logging

`SendMsg` queues a speed-walk command as a one-byte flag plus the text
(`doc_shared.cpp:2360-2377`). The flag is `E`/`N`/`I`/`W` for
queue/immediate x echo/no-echo, **lowercased when the caller asked for no
logging**:

```
   // nolog is the lower-case version of the flag
  if (!bLogIt)
    strEchoFlag.MakeLower ();
```

Every drain then recovers the two booleans the same way:

```
  m_pDoc->DoSendMsg (strCommand.Mid (1),
                     toupper (cMessageType) == QUEUE_WITH_ECHO ||
                     toupper (cMessageType) == IMMEDIATE_WITH_ECHO,
                     cMessageType >= 'A');      // log flag
```

`toupper` correctly makes the ECHO decision case-insensitive. But the log flag
is `cMessageType >= 'A'`, and every one of the eight codes is a letter:
`'E'` is 69 and `'e'` is 101 — **both are >= 'A' (65)**. The test is a
tautology, so `bLogIt` is true for every queued command and the lower-casing
above accomplishes nothing for logging. A speed-walk sent with logging
suppressed is logged anyway.

The intended test is almost certainly `cMessageType < 'a'` (upper case = log).

**Fix (Wade's call, 2026-08-05: make it work):** `c < 'a'` — upper case logs,
lower case does not. Done in the shared drain, so all three former copies get
it. **This changes log-file contents on both platforms**: a speed-walk sent
with logging suppressed stops appearing, which is what the flag was always
meant to do. See `docs/WINDOWS-VALIDATION.md` § 4c-duodetricies-bis.

## 13 — Windows' two queue drains disagree about case

`CTimerWnd::OnTimer` (`TimerWnd.cpp:52`) — the per-tick drain — compares with
`toupper`, so a lower-case `e` counts as QUEUE_WITH_ECHO and the command is
echoed. `CTimerWnd::ChangeTimerRate` (`TimerWnd.cpp:101`) — the "the delay was
just set to zero, flush everything" path — compares **case-sensitively**:

```
  m_pDoc->DoSendMsg (strCommand.Mid (1),
                     cMessageType == QUEUE_WITH_ECHO ||
                     cMessageType == IMMEDIATE_WITH_ECHO,
                     cMessageType >= 'A');
```

so the same command echoes when drained on a tick and does not when flushed.
Given that the paired upper/lower codes exist only to carry the log flag,
`OnTimer`'s reading is the intended one and `ChangeTimerRate` is the defect.

**The port already diverges here, accidentally in the right direction.** Its
`delay <= 0` arm (`platform/wx_world_child.cpp:4131-4140`) applies `toupper`,
matching `OnTimer` rather than `ChangeTimerRate`. So a no-log speed-walk
flushed by setting the delay to zero echoes on macOS and does not on Windows.
**Fix (Wade's call, 2026-08-05: `toupper` everywhere):** the flush arm now
calls the same shared body as the tick drain, so the two cannot disagree again.
The port was already on this side, so only Windows changes — a no-log command
flushed by setting the delay to zero now echoes, as it always did when drained
on a tick.
