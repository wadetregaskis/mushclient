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
`NoteColourFore` / `NoteColourBack` calls — no dialog involved. NOT FIXED here:
the root is the option row's maximum (and `echo_colour`'s, which is also wrong
for an index), and tightening it changes what `SetOption` accepts on Windows,
so it wants a deliberate decision rather than a drive-by patch.

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
