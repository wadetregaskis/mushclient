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
| 14 | Menu-triggered alias's own `%1` is dropped (subscript computed, never used) | this commit | mushview.cpp `CMUSHView::OnMXPMenu` ACTION_ALIAS arm (now `doc_aliasmenu_shared.cpp` `ExecuteAliasMenuItem`) |
| 15 | NAWS sends a dimension byte of 255 as a bare IAC, corrupting the subnegotiation | this commit | doc_shared.cpp ← doc.cpp `CMUSHclientDoc::SendWindowSizes` |
| 16 | MXP hyperlink from the right-click menu refuses `https://` | this commit | mushview.cpp `CMUSHView::OnMXPMenu` ACTION_HYPERLINK arm |
| 17 | Double-click word scan reads one byte past the line buffer | this commit | mushview.cpp `CMUSHView::OnLButtonDblClk`, `OnDisplayTextattributes` |
| 18 | `CmcDateTime::operator-`'s Windows arm discards a null/invalid span status | this commit | mcdatetime.cpp `CmcDateTime::operator-(const CmcDateTime&)` |
| 19 | Doc ctor's unqualified `GetUniqueID()` picks the automation member and leaks its BSTR once per document | this commit | doc_construct.cpp (ex doc.cpp ctor) `m_strWorldID = GetUniqueID()` — since the 2010 import; fix is the `::` the three sibling callers already use |
| 20 | A MouseOver callback that deletes its own hotspot crashes the client | this commit | mushview.cpp `CMUSHView::Mouse_Move_MiniWindow` (now `miniwindow_compose.cpp` `MouseMoveMiniWindow`) |
| 21 | The second layout pass wipes the first pass's "would not fit" decision | this commit | mushview.cpp `CMUSHView::Calculate_MiniWindow_Rectangles` (now `miniwindow_compose.cpp` `CalculateMiniWindowRectangles`) |
| 22 | Trigger "multi-line" tick is not compared, so un-ticking it is discarded | this commit | prefspropertypages.cpp `CPrefsP8::CheckIfChanged` |
| 23 | Command-window colours are lost by visiting another prefs page | this commit | prefspropertypages.cpp `CPrefsP9` (no `OnKillActive`) |
| 24 | The default echo colour always reports a change (SAMECOLOUR vs -1) | this commit | configuration.cpp `CMUSHclientDoc::ChangedPrefsP9` |
| 25 | The default note colour always reports a change (the same defect) | this commit | configuration.cpp `CMUSHclientDoc::ChangedPrefsP17` |
| 26 | "Fit the last column to the list view" computes a width nothing applies | this commit | prefspropertypages.cpp `CPrefsP7`/`CPrefsP16`/`CPrefsP18::OnInitDialog` |
| 27 | The Timers list's Contents column header does not sort | this commit | prefspropertypages.cpp `CPrefsP16::CompareObjects` |
| 28 | The alias page's column arrays are indexed with the trigger page's enum | this commit | configuration.cpp `CMUSHclientDoc::LoadPrefsP7` |
| 29 | The printer-font picker opens on the font from before the sheet opened | this commit | GlobalPrefs.cpp `CGlobalPrefsP4::OnFont` |
| 30 | `metaphone()` copies a word into a 61-byte stack buffer with no bound | this commit | Utilities.cpp `metaphone` |
| 31 | Greyscale PNGs decode garbled, and palette PNGs do too from memory | this commit | png_load_shared.cpp ← Utilities.cpp `LoadPng` / `LoadPngMemory` |
| 32 | `world.BlendPixel` mode 64 (HSL) ignores the blend colour entirely | this commit | methods_pixel_shared.cpp ← methods_miniwindows.cpp `CMUSHclientDoc::BlendPixel` |
| 33 | `world.WindowWrite` writes a .bmp strict readers refuse to open | this commit | miniwindow.cpp `CMiniWindow::Write` |
| 34 | `Blend_ColorBurn`'s underflow clamp is dead code (unsigned comparison) | this commit | blending.h `Blend_ColorBurn` |
| 35 | `WindowAddHotspot`'s re-entrancy guard is shared by every open world | this commit | methods_miniwindows.cpp `CMUSHclientDoc::WindowAddHotspot` |
| 36 | `world.ArrayImport`'s table form crashes on any non-string value | this commit | lua_methods.cpp `L_ArrayImport` |
| 37 | `world.CallPlugin` keeps pointers into two Lua strings it has removed from the stack | this commit | lua_methods.cpp `L_CallPlugin` |
| 38 | `world.SetRemoveMapReverses` raises an error when passed a boolean | this commit | lua_methods.cpp `L_SetRemoveMapReverses` |
| 39 | Three world-table entries reject the documented `other:Method (…)` form | this commit | lua_methods.cpp `L_StripANSI`, `L_GetWorld`, `L_GetWorldById` |
| 40 | `pushVariant` hands an uninitialised `VARIANT` to `SafeArrayGetElement` | this commit | lua_methods.cpp `pushVariant` (VT_ARRAY arm) |
| 41 | The note colours a deferred note carries are never initialised | this commit | doc_construct.cpp `CMUSHclientDoc::CMUSHclientDoc` |
| 42 | `GetRecentLines` narrows its start position to `int`, so a large Count reads outside the buffer | this commit | methods_output.cpp `CMUSHclientDoc::GetRecentLines` |
| 43 | A names file with an empty section makes name generation divide by zero and subscript an empty array | this commit | NameGeneration.cpp `ReadNames`, `GenerateName` |
| 44 | `DebugHelper`'s `callbacklist` branch dereferences a NULL plugin | this commit | world_debug.cpp `CMUSHclientDoc::DebugHelper` |
| 45 | Five `DebugHelper` early returns leave the document stuck in a plugin's context | this commit | world_debug.cpp `CMUSHclientDoc::DebugHelper` |

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

**The row now validates too (option D, 2026-08-05).** A new `OPT_CUSTOM_INDEX`
flag makes the range check say what the field actually is — 0..`MAX_CUSTOM`-1,
or `SAMECOLOUR` — so `SetOption("note_text_colour", 300)` returns
`eOptionOutOfRange` instead of being stored. It follows the shape
`OPT_ZERO_UNLIMITED` already established for `max_output_lines`: a legal set
that a plain minimum/maximum cannot express. Only the range check consults it.

It deliberately changes NOTHING else — not the stored encoding, and not the
serialisation. The off-by-one between the two script APIs is also deliberately
left: `world.NoteColour = 4` stores 3 and `SetOption("note_text_colour", 4)`
stores 4, and fixing that breaks either scripts or world files. After this
change both are at least VALID values rather than one of them being a silent
read past the end of a 16-entry array. If it is ever worth fixing, it is a
world-file format change with a version bump, not an option-row edit.

**Why the row was not simply RE-FLAGGED, which is the tempting one-liner:** `OPT_RGB_COLOUR` does not merely set a maximum: it is also
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

---

## 14 — a menu-triggered alias's own `%1` is silently dropped

`CMUSHView::OnMXPMenu`'s `ACTION_ALIAS` arm — used for both an MXP `<a>` menu's
alias action and the Ctrl+click word-menu (`CMUSHView::AliasMenu`) — has its own
hand-rolled `%`-substitution loop (it does not call `FixSendText`, which is what
a normally-typed/matched alias or trigger uses). For a `%<digit>` sequence it
computes `subscript = *++p - '0'` and then, immediately after, resumes
scanning — `subscript` is never read again. So an alias like `greet *` → send
`echo greeting %1`, fired from either menu, always sent `echo greeting ` with
the wildcard silently dropped, regardless of what the menu was invoked on.

This is not a case of the value being unavailable: `AliasMenu` already scans
the word under the mouse into `m_strWordUnderMenu` for exactly this purpose —
`GetInfo(86)` documents it as "word under menu in output window" and a script
attached to the alias could always read it that way. The plain `%1`
substitution path into the alias's own Send text, which is the obvious first
thing to try before reaching for a script, was simply never wired up.

**Fix:** `%1` now substitutes `m_strWordUnderMenu`. `%0` and `%2`-`%9` are
unchanged (still empty) — there is only ever one captured word for a menu
click, and `GetInfo(86)`'s own phrasing ("word", singular) is the closest
thing to a spec this feature has. Fixed in `doc_aliasmenu_shared.cpp`
`ExecuteAliasMenuItem`, which is now the single body `OnMXPMenu` calls into on
Windows too (2026-08-13's hoist), so this fixes both platforms from one edit.

Regression guard: `tests/test_aliases.cpp`
`AliasMenu.ExecuteSubstitutesWordUnderMenuIntoPercentOne`.

---

## 15 — a NAWS dimension of exactly 255 goes out as a bare IAC

`CMUSHclientDoc::SendWindowSizes` announces the window size as

    IAC SB NAWS <cols-hi> <cols-lo> <rows-hi> <rows-lo> IAC SE

building those nine bytes as a plain array and handing it to `SendPacket`. The
four payload bytes are written raw. RFC 854 is explicit that inside the telnet
stream "if a 255 is to be sent as data, it must be doubled", and RFC 1073 —
which defines NAWS — repeats it for exactly these bytes. So a width or height
whose high or low byte is 255 emits an *unescaped* IAC in the middle of the
subnegotiation: the MUD's telnet parser reads it as the start of a command,
consumes the following byte as that command, and the negotiation goes out of
step. With a wrap column of 255 the low byte is 0xFF and the very next byte is
the rows high byte, so the server sees `IAC <rows-hi>` and then never finds a
matching `IAC SE` where it expects one.

255 is reachable without contrivance: the wrap column is user-settable and the
"auto wrap to window width" path derives it from the pane, and both 255 columns
and (on a tall enough window) 255 rows are ordinary values. It is only the
single value 255 per byte that trips it, which is why it has survived — 256
comes out as `01 00` and is fine.

**Fix:** both arms now append the four payload bytes through a tiny
`AppendNAWSByte` helper that doubles a 0xFF, and send the resulting length
rather than `sizeof p`. The buffer is sized for the worst case (all four bytes
255). The Windows arm is fixed with the same helper in the same file, so this
is one edit for both platforms; nothing changes for any dimension that does not
contain a 255 byte.

Regression guard: `tests/test_connection.cpp`
`Naws.EscapesADimensionByteOfExactly255`.

---

## PNG loader reads past the end of every row libpng allocated

`LoadPng` and `LoadPngMemory` (now `png_load_shared.cpp:193` and `:353`;
`Utilities.cpp` upstream) size their row copy with

```
  long bpl = BytesPerLine (width, bit_per_pixel);   // padded to 4 bytes
  ...
  memcpy (p, row_pointers [iHeight - row - 1], bpl);
```

but `png_read_png` allocates each `row_pointers[i]` at exactly
`png_get_rowbytes()`, which is `width * channels` with **no padding**. For any
24bpp image whose width is not a multiple of 4, the copy therefore reads 1–3
bytes past the end of every row allocation — once per row, on every PNG load.

Measured at HEAD, printing both sizes from inside the loop:

| width | `png_get_rowbytes()` | `bpl` | over-read |
|---|---|---|---|
| 5 | 15 | 16 | 1 |
| 6 | 18 | 20 | 2 |
| 7 | 21 | 24 | 3 |
| 9 | 27 | 28 | 1 |

Confirmed under AddressSanitizer, reached straight from a script:

```
ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 28
    #1 LoadPng(char const*, void*&) png_load_shared.cpp:182
    #2 CMiniWindow::LoadPngImage(...) miniwindow.cpp:1205
    #5 L_WindowLoadImage(lua_State*) lua_methods.cpp:6480
```

32bpp is unaffected — `width * 4` is always 4-aligned, so `bpl` and `rowbytes`
agree. That is also why this went unnoticed: the alpha images are safe, and the
padding bytes are never read as pixel data, so nothing renders wrongly. It is a
latent crash, not a visible defect — which is precisely the kind that survives
for years.

**Fixed 2026-08-07** by copying `min(png_get_rowbytes(), bpl)` at both sites and
leaving the pad bytes as the zero the DIB came back with. **Not one visible
pixel changes on either platform**, so this needs no `UPSTREAM-DIVERGENCES.md`
entry — but it IS a change to shared code that ships on Windows, and belongs in
the Windows validation pass.

Regression guard: `tests/test_lua_surface.cpp`'s
`LoadImageMemoryDecodesAPngWithAPaddedStride` covers the second site, which had
no positive test at all before. **Both sites were reverted independently and
each was caught by ASAN**, so neither is guarded only by the other. Note that
the fixture must be **24bpp** — a first draft used the existing RGBA helper,
whose stride is 4-aligned at every width, and so could never have failed.

---

## `AltV` is published as a command but has no implementation, on either platform

`accelerators.cpp` is the table that gives MUSHclient commands their public
NAMES — the strings `world.DoCommand()` accepts and the Macros/Keypad UI binds
keys to. It publishes **eighteen** `Alt<letter>` names:

    AltA AltB AltJ AltK AltL AltM AltN AltO AltP AltQ AltR AltS AltT AltU
    AltV AltX AltY AltZ

but only **seventeen** macro slots exist. `OtherTypes.h`'s `MAC_ALT_*` enum has
A, B, J–U, X, Y, Z, and `sendvw.cpp`'s id→slot switch has exactly 17 `case`
labels. `ID_ALT_V` appears in precisely two places in the whole tree — the
accelerators table and its `#define` — with no `ON_COMMAND_EX` in `mushview.cpp`
or `sendvw.cpp` and no `case` label. So binding a key to `AltV`, or calling
`world.DoCommand("AltV")`, does nothing **on Windows too**. `DoCommand` cannot
report this: it posts `WM_COMMAND` and returns `eOK` regardless.

The excluded letters are the menu access keys — C, D, E, F, G, H, I, V, W —
so dropping V (View) from the macro slots was deliberate. What looks accidental
is that its *table entry* survived: `ID_ALT_W` (Window) is not published at all,
which is what V should have been. A leftover, not a design.

**Deliberately not fixed.** Adding a handler would invent a macro slot Windows
does not have, and removing the table entry would change what
`world.DoCommand("AltV")` returns (`eOK` → `eNoSuchCommand`) — a visible
behaviour change to shared code for a command nobody can be using, since it has
never done anything. Recorded so it is not re-triaged as a port defect.

**How it was found, and the correction that came with it.** A sweep for
"published command ids with no handler reference" flagged four candidates.
Three — `ActivityViewCloseWorld`, `ActivityViewSaveWorld`,
`ActivityViewSaveWorldAs` — were **false positives**: they have full
`ON_COMMAND` + `ON_UPDATE_COMMAND_UI` handlers in `ActivityView.cpp` and entries
in two recovered menus, and the sweep simply had not read that file. That sweep
was later deleted for failing its own mutation check; this entry is the one
real thing it found.

## 15 — MXP hyperlink opened from the popup menu refuses `https://` (this commit)
**Bug:** `OnMXPMenu`'s ACTION_HYPERLINK arm guards against arbitrary OS
commands with a scheme allowlist, but its copy lists only `http://` and
`mailto:`.  The other two copies of the same guard — the left-click arm a few
thousand lines earlier in the same file, and `CMUSHclientDoc::OpenBrowser`
(methods_utilities.cpp) — both list `https://` as well.  So an MXP hyperlink
to an https URL is rejected as "permission denied" when opened from the
right-click popup, while left-clicking the identical link opens it.
**Fix:** add the `https://` arm, matching the other two copies.  The three
copies should really be one helper; that hoist is deliberately deferred so it
does not collide with other in-flight work on utilities_shared.cpp.

## 18 — `CmcDateTime::operator-`'s Windows arm discards a null/invalid span status (this commit)
**Bug:** on the `_WIN32` arm, the null/invalid early-outs build a real
`COleDateTimeSpan`, set ITS status to `null`/`invalid`, then `return (double)
spanResult;`. The function's return type is `CmcDateTimeSpan`, so that double
goes straight into `CmcDateTimeSpan(double)` — a constructor that unconditionally
calls `SetStatus(valid)` — so the caller always observes `valid` regardless of
which of the two null/invalid arms actually ran. The POSIX arm two lines below
does not have this bug: it already constructs and returns a `CmcDateTimeSpan`
directly.
**Fix:** construct a `CmcDateTimeSpan` and set its status directly in both
early-out arms, mirroring the POSIX arm, instead of routing the result through
`(double)`. `needs: windows-run` — unreachable from this platform; the POSIX arm
was already correct and is what `tests/test_mcdatetime.cpp`'s
`CmcDateTimeR4.NullAndInvalidStatusPropagation` exercises.

## 16 — Double-click word scan reads one byte past the line buffer (this commit)
**Bug:** once a line is complete its `text` buffer is reallocated to exactly
`MAX (len, 1)` bytes, with no terminator (doc.cpp — `ProcessPreviousLine`;
doc_shared.cpp in this tree).  `calculate_line_and_column` returns
`col == pLine->len` when the click lands to the right of the text, and
`OnLButtonDblClk` then starts its backward word scan at `text [col]` — one
past the allocation.  The over-read decides the result: a byte that happens to
look like a space or a word delimiter yields no selection at all, anything
else selects the last word, so double-clicking to the right of a line behaves
differently run to run.  `OnDisplayTextattributes` has the same shape: it
clamps `startcol` to `pStartLine->len` and then reads `text [startcol]`, which
is one past the end whenever the selection spans lines (the same-line case is
bounded by its own `endcol > startcol` test).
**Fix:** clamp the column into `[0, len - 1]` before the scan, with an
early-out for an empty line — the same clamp `SetWordUnderMenu` already
carries in this tree, so the two copies of the scan agree again.  The
text-attributes clamp is applied *after* the valid-selection test, leaving the
set of accepted selections unchanged.

## 20 — A MouseOver callback that deletes its own hotspot crashes the client (this commit)
**Bug:** after dispatching a hotspot's MouseOver, `Mouse_Move_MiniWindow`
re-looks-up the hotspot by id "in case previous callback deleted it" and sets
`pHotspot = NULL` when it is gone — then, five lines later, dereferences it
anyway:

```
    if (!pHotspot->m_sCancelMouseOver.empty ())
      hooks.TrackMouseLeave ();
```

so the exact scenario the re-lookup was added to survive takes the client down
instead.  Deleting a hotspot from its own MouseOver is ordinary plugin practice
— a button bar that redraws itself calls `WindowDeleteAllHotspots` and installs
a new set — and every other one of the five re-establish sites in the file
re-tests `pHotspot` before using it, so this one is simply the copy that was
missed.
**Fix:** `if (pHotspot && !pHotspot->m_sCancelMouseOver.empty ())`.  With the
hotspot gone there is no cancel-mouse-over to deliver, so not arming the
mouse-leave notification is also the right answer.  `needs: windows-run` only
to confirm the crash reproduces there; the logic is platform-independent and
`LuaSurface.MouseOverCallbackMayDeleteItsOwnHotspot`
(`tests/test_lua_surface.cpp`) exercises it headless — before the fix that test
SIGSEGVs, and its control (a MouseOver that leaves its hotspot alone) still
arms the notification, so "never call `TrackMouseLeave`" fails too.

## 21 — The second layout pass wipes the first pass's "would not fit" decision (this commit)
**Bug:** `Calculate_MiniWindow_Rectangles` runs twice per paint — once for the
draw-underneath windows, once for the rest — and clears `m_bTemporarilyHide`
for **every** miniwindow at the top of its loop, before the pass filter that
`continue`s past the windows this pass is not laying out.  The two passes run
back to back, so the non-underneath pass resets the flag on the underneath
windows the previous pass had just decided would not fit.  `WindowInfo (name,
6)` therefore always answers false for a draw-underneath window that had no
room, and any frontend that consults the flag between the two passes sees it
flap.
**Fix:** guard the reset with the same pass test the filter below uses —
`if (((mw->GetFlags () & MINIWINDOW_DRAW_UNDERNEATH) != 0) == bUnderneath)` —
so only the pass that actually lays a window out clears its flag.  Guarding
rather than moving the line below the filters keeps the reset happening for
windows that are not currently shown, which is what upstream's placement
achieved.  `needs: windows-run`.  Covered headless by
`LuaSurface.TemporarilyHideSurvivesTheOtherLayoutPass`
(`tests/test_lua_surface.cpp`), which also runs both passes against a wall wide
enough to fit the window and asserts the flag comes back false — so deleting
the reset outright fails as well.
## 22 — Trigger "multi-line" is not compared, so un-ticking it is discarded (this commit)
**Bug:** `CPrefsP8::CheckIfChanged` decides whether editing a trigger dirties
the world, and its comparison chain covers every field of `CTrigger` — including
`iLinesToMatch`, `bMultiLine`'s own sibling — except `bMultiLine` itself.  A user
who un-ticks "multi-line" and changes nothing else is told nothing changed, the
document is never marked modified, and the edit is thrown away at the next save
decision.
**Fix:** add a `bMultiLine` term next to the `iLinesToMatch` one, normalising
both sides to `bool` (the member is an `unsigned short`, the dialog field a
`BOOL`).

## 23 — Command-window colours are lost by visiting another prefs page (this commit)
**Bug:** `CPrefsP9::OnSetActive` re-seeds `m_ctlTextSwatch` and
`m_ctlBackgroundSwatch` from `m_input_text_colour` / `m_input_background_colour`,
but the only place those two members are ever written from the swatches is
`OnOK`.  So a colour picked on the Commands page and then left — by clicking any
other page in the sheet — is reverted to the value the sheet opened with the
moment the user comes back.  `CPrefsP3` and `CPrefsP5` do not have this defect:
both save their swatches in `OnKillActive`.
**Fix:** give `CPrefsP9` the same `OnKillActive`, copying the two swatches into
the two members.  `OnOK`'s identical copy becomes idempotent.

## 24 — The default echo colour always reports a change (this commit)
**Bug:** `LoadPrefsP9` encodes the world's `m_echo_colour` for the page as
`m_echo_colour == SAMECOLOUR ? 0 : m_echo_colour + 1`, but `ChangedPrefsP9`
undoes it arithmetically: `m_echo_colour != (page9.m_echo_colour - 1)`.  For the
default "same as normal text" that compares 65535 (the `unsigned short`
`SAMECOLOUR`) against `0 - 1` — an `int` -1 that no `unsigned short` can equal.
Opening World Properties and pressing OK therefore marks the document modified
every time, prompting to save a world nothing was changed in.
**Fix:** compare in the encoding `Load` wrote —
`(m_echo_colour == SAMECOLOUR ? 0 : m_echo_colour + 1) != page9.m_echo_colour`.
`SavePrefsP9` is deliberately left alone: its `-1`-wraps-to-65535 assignment is
what existing world files depend on.

## 25 — The default note colour always reports a change (this commit)
**Bug:** `ChangedPrefsP17` has the identical `SAMECOLOUR`-vs-`-1` defect on
`m_iNoteTextColour`, with the identical consequence.
**Fix:** mirror `LoadPrefsP17`'s transform, leaving `SavePrefsP17` untouched.

## 26 — "Fit the last column to the list view" computes a width nothing applies (this commit)
**Bug:** the alias, timer and variable pages each end `OnInitDialog` with a
block that measures the list control and stores a fitted width into
`m_iColWidth [eColumnContents]`.  By then `CGenPropertyPage::OnInitDialog` has
already restored the saved widths into that array AND inserted the columns from
it, and nothing reads `m_iColWidth` again — so the block has no effect at all and
the Contents column keeps the 150-pixel seed from configuration.cpp.
**Fix:** apply the figure with `SetColumnWidth`.  Only when the user has no
saved width of their own, though: the restore happens over the seed, so fitting
unconditionally would discard their width on every open.  The seed is captured
before the base call and the two compared afterwards; the decision itself is
`FittedLastColumnWidth` in prefspropertypages.h, which the headless suite drives.

## 27 — The Timers list's Contents column header does not sort (this commit)
**Bug:** `CPrefsP16::CompareObjects` has cases for Type, When, Group, Label and
Next, but none for `eColumnContents`, so clicking that header falls to
`default: iResult = 0` and the list does not move.  The alias and trigger pages
both compare their Contents column.
**Fix:** add `case eColumnContents: iResult = timer1->strContents.CompareNoCase
(timer2->strContents); break;`, following the sibling pages.

## 28 — The alias page's column arrays are indexed with the trigger page's enum (this commit)
**Bug:** `LoadPrefsP7` fills `page7`'s column arrays, but two of the subscripts
name `CPrefsP8::eColumnSequence` — the *trigger* page's enumerator — rather than
`CPrefsP7`'s own.  The two enums are independent declarations; this is correct
today only because both happen to give that name the value 1, and reordering
either one silently writes the alias page's justification and heading into the
wrong column.
**Fix:** use `CPrefsP7::eColumnSequence` at both sites.  No behaviour changes
today; the point is that it can no longer break from an unrelated edit.

## 29 — The printer-font picker opens on the font from before the sheet opened (this commit)
**Bug:** `CGlobalPrefsP4::OnFont` seeds `lf.lfFaceName` from
`App.m_strPrinterFont`, but the page's own choice lives in `m_strPrinterFont`
(DDX_Text on `IDC_FONT_NAME`), and `OnFont` writes the chosen face straight to
the control with `SetWindowText`.  So a second visit to the picker within one
Global Preferences session opens showing the pre-dialog font rather than the one
just chosen, and confirming it without re-selecting the face silently reverts the
earlier pick — most easily hit when reopening it only to change the size, since
the size half already uses the page's own `m_nPrinterFontSize`.
`CGlobalPrefsP10::OnFont` already does it the right way round.
**Fix:** start with `UpdateData (TRUE)` and seed from `m_strPrinterFont`,
matching `CGlobalPrefsP10::OnFont`.  `needs: windows-run` — the port's font
picker is the wx one behind `g_wxhook_font_dialog`, so only a Windows build
confirms the `CFontDialog` seeding end to end.

## 30 — `metaphone()` copies a word into a 61-byte stack buffer with no bound (this commit)
**Bug:** `metaphone` clamps its *output* length to `sizeof (wname) - 1` and then
fills `wname[61]` from the input with `for (ii=0; name[ii] != '\0'; ii++) { if
(isalpha(...)) ename[jj++] = ...; }` — no check on `jj`.  Any word with more
than 60 letters writes past the end of the buffer and smashes the stack frame.
It is latent rather than live: the function has **no caller anywhere in the
tree** (the only near-call, methods_spellchecker.cpp:328, is commented out, and
`utils.metaphone` goes through `MString::DoubleMetaphone` instead), so the first
caller added would be the one to find it.
**Fix:** `break` out of the fill loop once `jj` reaches `sizeof (wname) - 1`,
which is the same clamp the `metalen` line above it already applies.  Kept
rather than deleted so the reference implementation stays available.
## 31 — Greyscale PNGs decode garbled, and palette PNGs do too from memory (this commit)
**Bug:** both decoders ask libpng for `STRIP_16 | PACKING | BGR`, and only
`LoadPng` adds `EXPAND`.  Neither asks for `GRAY_TO_RGB`, which is a *separate*
transform: `EXPAND` promotes sub-8-bit samples and expands a palette, but leaves
an 8-bit greyscale image at one byte per pixel and a grey+alpha one at two.  The
row bytes are then `memcpy`'d straight into a 24/32bpp DIB as if they were BGR,
so every greyscale PNG renders as coloured noise a third of its width — and
`world.WindowLoadImage`/`WindowLoadImageMemory` still return `eOK`.  The missing
`EXPAND` in `LoadPngMemory` does the same to palette-indexed images, so the same
file loads correctly from disk and garbled from memory.
**Fix:** add `PNG_TRANSFORM_GRAY_TO_RGB` to both, and `PNG_TRANSFORM_EXPAND` to
`LoadPngMemory`.  Both are no-ops for truecolour input, so existing RGB/RGBA
decodes are byte-identical; `png_read_png` calls `png_read_update_info`, so the
`png_get_color_type` test that picks 24 vs 32bpp already sees the transformed
image.

## 32 — `world.BlendPixel` mode 64 (HSL) ignores the blend colour entirely (this commit)
**Bug:** case 64 declares `CColor cA, cB`, populates only `cA`, and then reads
`cB.GetRed/GetGreen/GetBlue` — a default-constructed colour, i.e. black.  Each
channel is therefore `Simple_Opacity (base, 0, Opacity)`: the base scaled by
`(1 - Opacity)`, which is pure black at opacity 1.0 and merely darkened below
it.  The blend colour never participates.
**Fix:** mirror what `CMiniWindow::BlendImage` case 64 does over a whole
surface — hue/360*255 to red, saturation*255 to green, luminance*255 to blue,
each `Simple_Opacity`-blended against the base — and drop the unused `cB`.

## 33 — `world.WindowWrite` writes a .bmp strict readers refuse to open (this commit)
**Bug:** `CMiniWindow::Write` fills a `BITMAPINFO` with
`biSize = sizeof bmi` — 44 bytes, because `BITMAPINFO` carries a trailing
`bmiColors [1]` — and then writes out only `bmi.bmiHeader`, which is 40.  The
`biSize` field is how a reader knows which header dialect follows, so the file
claims a 44-byte header it does not have.  Windows' own GDI is lenient enough to
open it; macOS ImageIO (Preview, Quick Look, `sips`) is not, and `WindowWrite`
returns `eOK` either way.
**Fix:** `bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;`.  The same `bmi` goes on
to `WritePng`, which reads only `biWidth`/`biHeight`, so the PNG path is
unchanged.

## 34 — `Blend_ColorBurn`'s underflow clamp is dead code (this commit)
**Bug:** the macro tests `(255 - (((unsigned long)(255 - B)) << 8) / A) < 0`.
The division yields an `unsigned long`, so the subtraction is done unsigned and
the result can never be negative: the clamp never fires, and the `(uint8)` cast
truncates the wrapped value instead.  For the 32640 of 65536 (A, B) pairs where
the burn *should* clamp to 0, the macro returns an arbitrary byte — `A=1, B=0`
gives 255 (the opposite of the intended answer) and `A=100, B=0` gives 115.  It
corrupts modes 7 (ColorBurn), 9 (InverseColorBurn) and 20 (VividLight) of both
`world.WindowBlendImage` and `world.BlendPixel`.
**Fix:** cast the division result to `long` so the subtraction is signed, in the
test and in the value — the shape `Blend_Freeze` already uses two lines below.

## 35 — `WindowAddHotspot`'s re-entrancy guard is shared by every open world (this commit)
**Bug:** the guard that stops a MouseOver callback recursing back into
`WindowAddHotspot` for ever is a function-local `static bool`, so it is one flag
for the whole process rather than one per document.  While world A is inside
`WindowAddHotspot`, a MouseOver script that adds a hotspot to an unrelated,
simultaneously-open world B is refused with `eItemInUse` — a world's hotspots
depending on what another world happens to be doing.
**Fix:** make it the `CMUSHclientDoc` member `m_bInWindowAddHotspot`, cleared in
the constructor.  Per-document scope still terminates any A→B→A recursion,
because the document's own flag is set on the way in.

## 36 — `world.ArrayImport`'s table form crashes on any non-string value (this commit)
**Bug:** the table extension checks the *key* (`lua_type (L, -2) != LUA_TSTRING`
→ "table must have string keys") but not the value: it builds
`string sValue = lua_tostring (L, -1)` unconditionally.  `lua_tostring` returns
NULL for a boolean, table, function, thread, userdatum or nil, and constructing
a `std::string` from NULL is undefined — in practice MUSHclient dies on the
spot.  `world.ArrayImport ('t', {a = true})` — the natural way to import a set
of flags — takes the whole client down, taking any unsaved world with it.
**Fix:** check the value alongside the key with
`if (!lua_isstring (L, -1)) luaL_error (L, "table must have string values");`.
`lua_isstring` still accepts a number, which `lua_tostring` coerces in place;
that is safe here because it is the value slot, not the key `lua_next` is
iterating on.

## 37 — `world.CallPlugin` keeps pointers into two Lua strings it has removed from the stack (this commit)
**Bug:** `sPluginID` and `sRoutine` are raw `const char *` into Lua-owned
strings, and the next two statements are `lua_remove (L, 1)` twice — a
deliberate step, so a plugin calling back into us finds a clean stack.  Both
strings are then unreferenced for the entire call while eleven later sites read
them, including every error message.  A `collectgarbage` anywhere inside the
called routine frees them; the error text then reports whatever now occupies
that memory.  A plugin calling a routine in itself is the sharpest case, since
caller and callee share one Lua state.
**Fix:** copy both into `CString`s before the removes and point the two locals
at those copies, so every later read is of memory this frame owns.

## 38 — `world.SetRemoveMapReverses` raises an error when passed a boolean (this commit)
**Bug:** it is the only flag setter in the Lua world table that uses
`my_checknumber` rather than `optboolean`; its ten siblings (`SetMapping`,
`SetTrace`, `LogInput`, `Pause`, …) all take `optboolean (L, 1, 1)`.
`world.SetRemoveMapReverses (true)` — the form the documentation shows, and the
form every other flag accepts — therefore raises "number expected, got boolean"
and aborts the script.
**Fix:** `optboolean (L, 1, 1) != 0`.  `optboolean` falls through to
`my_checknumber`, so the numeric form is unchanged, and a missing argument now
defaults to on exactly as it does for the siblings.

## 39 — Three world-table entries reject the documented `other:Method (…)` form (this commit)
**Bug:** every entry in the world table is supposed to start with `doc (L)`,
which is what makes the file's own documented alternative form —
`other:AddMapperComment ("…")` against a world userdatum from
`world.GetWorld`  — work: `doc()` removes the userdatum from index 1 so the
arguments line up.  `L_StripANSI`, `L_GetWorld` and `L_GetWorldById` never call
it, so all three see the userdatum where they check for a string and raise
"string expected, got userdata".  `StripANSI` is the one a script would
naturally write that way.
**Fix:** `doc (L);` as the first statement of each.  The plain
`world.StripANSI (s)` form is unaffected — `doc()` only removes argument 1 when
it really is a `mushclient.world` userdatum.

## 40 — `pushVariant` hands an uninitialised `VARIANT` to `SafeArrayGetElement` (this commit)
**Bug:** the `VT_ARRAY + VT_VARIANT` arm declares `VARIANT val;` and passes it
straight to `SafeArrayGetElement`, which on Windows calls `VariantClear` on the
destination before filling it — i.e. it frees whatever the uninitialised `vt`
and pointer happen to name.  It is the same defect the file already fixes for
`L_GetTrigger`/`L_GetAlias`/`L_GetTimer` 2,480 lines below, which do call
`VariantInit`.  Every array-returning world method (`ArrayListAll`,
`GetTriggerList`, `GetVariableList`, …) goes through this arm.
**Fix:** `VariantInit (&val);` after the declaration.  No behaviour change on
the port — its `SafeArrayGetElement` shim copies without clearing — so this one
needs a **Windows debug run** (poisoned stack) to see the original fault.

## 41 — The note colours a deferred note carries are never initialised (this commit)
**Bug:** `m_iNoteColourFore` / `m_iNoteColourBack` get no value from the
document constructor and none from the options table, yet `Tell` reads them on
its deferred arm — the one taken while `m_bNotesNotWantedNow` is set, i.e.
inside a telnet-negotiation or plugin callback — and pushes them into
`m_OutstandingLines`.  The custom-palette lookup just below only overwrites
them when `NoteCustomColourIndex ()` returns an index, so a world whose
`note_text_colour` is the `SAMECOLOUR` sentinel (`world.NoteColour = 0`)
carries whatever the heap held into `OutputOutstandingLines`, and from there
into a real `AddStyle`.  On the port the read is unconditional, because the
headless constructor sets `m_bNotesInRGB` true.
**Fix:** initialise both beside `m_bNotesInRGB` / `m_iNoteStyle` with
literals — `RGB (255, 255, 255)` on `RGB (0, 0, 0)`, the pair
`SetNoteColourRGB` already falls back to.  Literals rather than
`m_normalcolour [WHITE]` / `[BLACK]` because `SetDefaultAnsiColours` does not
run until ~50 lines later, so the palette is not valid at that point.
## 42 — `GetRecentLines` narrows its start position to `int` before the clamp (this commit)
**Bug:** the first line to return is `m_sRecentLines.size () - Count`, an
unsigned `size_t` minus a signed `long`, stored into an `int`.  When `Count`
exceeds the number of lines held the difference wraps, and the narrowing keeps
only its low 32 bits — with three lines held and `Count` = 3,000,000,000 that
is +1,294,967,299, which is positive, so the `if (iPos < 0) iPos = 0;` clamp
below never fires.  The loop then subscripts the deque a billion entries past
its end, with `iPos != m_sRecentLines.size ()` — an equality that the runaway
index steps straight over — as its only stop condition.  Any plugin reaches it
with one bad argument to `world.GetRecentLines`; headless it is a segfault.
**Fix:** do the comparison at deque width and only subtract when it cannot
wrap (`if (Count > 0 && (size_t) Count < m_sRecentLines.size ())`), walk with a
`size_t`, and stop on `iPos < m_sRecentLines.size ()`.  A `Count` larger than
the buffer now returns the whole buffer, which is what a smaller overshoot
always did.  Covered by `LuaSurface.GetRecentLinesClampsACountBiggerThanTheBuffer`.

## 43 — An empty section in the names file is loaded as a valid names list (this commit)
**Bug:** `ReadNames` checks that each of `[start]`, `[middle]`, `[end]` and
`[stop]` is present, but not that any lines were collected between them, so a
file whose section markers are adjacent sets `bNamesRead = true` with one or
more of `FirstNames` / `MiddleNames` / `LastNames` empty.  `GenerateName` then
evaluates `rand () % FirstNames.GetSize ()` — a division by zero — and
subscripts the empty array.  It reaches the user through Edit ▸ Generate Name
and through `world.GenerateName`, and `world.ReadNamesFile` reports `eOK` for
the file that causes it.
**Fix:** after each collecting loop, refuse the file the same way the missing
markers already do — `if (FirstNames.GetSize () == 0) ThrowErrorException
("No names in [start] section of names file");` — so `bNamesRead` is never set
with an empty list and `world.ReadNamesFile` answers `eCouldNotOpenFile`.
NameGeneration.cpp is a `mushgui_adapters` TU (the headless build takes the
`platform/windows_stubs.cpp` `ReadNames`, which always throws), so there is no
headless coverage for this one and it wants a **Windows or GUI run**.
## 44 — `DebugHelper`'s `callbacklist` branch dereferences a NULL plugin (this commit)
**Bug:** `DebugHelper` starts by setting `m_CurrentPlugin = NULL` and only
fills it in when the argument carries a `_<plugin id>_` prefix.  The
`callbacklist` branch then walks `m_CurrentPlugin->m_PluginCallbacks` with no
null guard — unlike the `editplugin` branch beside it, which is written
`if (m_CurrentPlugin)`.  Reaching it with no plugin in context is an immediate
segfault.  Every link the client itself emits carries a validated prefix, so
the route is a script-authored `world.Hyperlink` whose action is
`!!138a692642ab4f9e7a1af63b:callbacklist()`; it is **not** reachable from MUD
output, MXP, or any UI-generated link, which is why it has survived.  No
platform ifdef is involved, so Windows crashes identically.
**Fix:** wrap the loop in `if (m_CurrentPlugin)`, with an
`else Note (TFormat ("No plugin selected."));`, and let control fall through to
the existing restore at the end of the function.  Needs a **Windows run** to
confirm the same link crashes there and no longer does.

## 45 — Five `DebugHelper` early returns leave the document stuck in a plugin's context (this commit)
**Bug:** `DebugHelper` saves the caller's plugin in `pSavedPlugin`, switches
`m_CurrentPlugin` to whichever plugin the clicked link named, and puts it back
in a single assignment at the very end.  Five branches return before reaching
it — `showtrigger`, `showalias` and `showtimer` when the named item does not
exist, and `editplugin`/`editscript` when the built-in notepad is used.  After
any of those the document is left in the clicked plugin's context, so every
later trigger/alias/timer/variable lookup (`GetTriggerMap()` and friends
already branch on `m_CurrentPlugin`) reads that plugin's tables instead of the
world's.  Clicking a stale item on the Debug summary page is enough; the sixth
early return, the one for an unknown plugin ID, already restores, which is what
makes the omission look like an oversight rather than a design.
**Fix:** repeat that branch's existing idiom —
`m_CurrentPlugin = pSavedPlugin;` immediately before each of the five
`return;`s.
