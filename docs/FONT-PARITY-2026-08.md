# Miniwindow font parity — what is settled, and what is not (2026-08-19)

Written after `bc6ad30` (D-21) removed a doubled 96/72 points-to-pixels factor from
`CMiniWindow::Font`. The question that prompted it was "Quow's miniwindows still look
smaller than on Windows" — and the answer is that **the size pipeline is now exact, and
the residual difference is face substitution, not sizing**. This file exists because that
conclusion is easy to un-learn: the obvious next move after "still too small" is to add a
scale factor, which would re-introduce precisely the fudge `bc6ad30` removed.

**Read §1 before touching any font arithmetic in `miniwindow.cpp`, `wx_surface.cpp` or
`wx_gdi_compat.cpp`.**

---

## 1. The size pipeline is exact. Do not add a factor.

### What Windows asks for

Upstream, unmodified (`miniwindow.cpp`, still the `_WIN32` behaviour today):

```c
int lfHeight = -MulDiv(Size ? Size : 10.0, dc.GetDeviceCaps(LOGPIXELSY), 72);
```

**Negative.** In Win32 a negative `LOGFONT::lfHeight` is the **character (em) height**;
positive is the **cell height** (em + internal leading). MUSHclient is not DPI-aware, so
`LOGPIXELSY` is 96 and the request is `em = MulDiv(pt, 96, 72)`. The resulting *cell*
(`tmHeight`, which is what `WindowFontInfo` code 1 returns) is therefore *larger* than
the request, by `tmInternalLeading`.

### What the wx path realizes

`spec.pixelHeight` at the PAL seam carries a **positive** number
(`MulDiv(dPoints, srf->dpi(), 72)`); the sign convention is re-applied by each adapter,
which is why `spec.pixelHeight` is not self-describing as em-or-cell. `pal::WxSurface`
converts back through the display PPI and calls `CreateFont` with a **negative** height,
and `wxFontInfo(points)` on macOS is a Core Text point size — which is an em, and a wx
point is one logical canvas pixel. Working the arithmetic through at every size the
plugins actually request (the port's `MulDiv` rounds to nearest,
`platform/mfc_ui.h:6137`):

| pt | Windows em `MulDiv(pt,96,72)` | realized macOS em |
|---|---|---|
| 8  | **11** | **11** |
| 9  | **12** | **12** |
| 10 | **13** | **13** |
| 12 | **16** | **16** |
| 14 | **19** | **19** |

**The macOS em equals the Windows em exactly at every size.** There is no em/cell
confusion in the product path and no residual factor.

The measured GUI metrics corroborate it and identify the face that answers: dividing the
reported `tmHeight` and `WindowTextWidth("MMMMMMMMMM")` by the em above gives
`tmHeight/em` ≈ 1.15-1.19 and `advance/em` a **constant 0.605-0.609 across a 1.7x size
range** — the signature of a single scalable monospaced face with a 0.60 em advance
(Menlo is 0.6023, Courier New 0.600). Predicted-vs-measured widths agree within 1 px at
every size.

### The latent landmine in `CFont::CreateFont`

The shim collapses the sign (`pixH = (nHeight < 0) ? -nHeight : nHeight`,
`platform/wx_gdi_compat.cpp`), so a **positive** `lfHeight` would be wrongly realized as
an em — too big by the internal leading. No miniwindow caller passes positive, so it is
latent, not live. It is a real hazard for any future `CreateFontIndirect` caller whose
`LOGFONT` came from elsewhere.

---

## 2. The residual difference is face substitution

### Neither face the plugins ask for exists on macOS

`system_profiler SPFontsDataType` on the development machine: **`Dina` absent**,
**`Fixedsys` absent**, and so are `Lucida Console`, `Terminal`, `MS Sans Serif` and
`Small Fonts`. Present: `DIN Alternate`, `DIN Condensed`, `Courier New`, `Arial`,
`Tahoma`, `Lucida Grande`.

`"Dina"` does **not** match `DIN Alternate` / `DINAlternate-Bold`: `spec.face` is passed
verbatim to `wxFont`, which reaches Core Text, and Core Text matches font *names*
(family / PostScript / full name) **exactly** — no substring, stem or fuzzy matching. The
space and the differing suffix both break it. The substitution goes to the generic
default instead, which is consistent with §1's finding that every size resolved to one
0.60-advance monospace face.

### The asymmetry is on the Windows side, not the macOS side

The tempting reading is "FixedSys windows are fine, Dina windows are small". The font
inventory does not support it: **all ten Quow miniwindows are running on the same macOS
fallback face**. The macOS side is uniform. What differs is Windows:

* **FixedSys ships with Windows.** Those five windows really do get a raster FixedSys
  strike there — a single fixed cell, materially taller than the
  `MulDiv(8,96,72) = 11 px` em requested.
* **Dina ships with nothing.** `QuowMinimap.xml:22905` appends `"Dina"` to
  `objAvailableFonts` **unconditionally, outside** the `utils.getfontfamilies()`
  enumeration loop, under the author's own comment `-- Manually add Dina font?`. The
  author knew it would not enumerate. No `.fon`/`.ttf`/`.fnt` ships anywhere in the tree
  and no install instructions mention a font, so on any Windows box Dina is present only
  if the user installed it by hand.

**That is why some Quow miniwindows match and some do not** — it is a question about
which Windows face each window actually got, and it is answered by info code 21 (§5).

### Why a bitmap face can make Windows render *larger*

Dina is a raster (`.fon`) font: fixed pixel strikes at a few nominal sizes, no outlines.
GDI cannot scale a raster face freely — it picks the nearest strike and **cannot render
smaller than the smallest one**. "8 pt" for a programmer bitmap font is a label, not a
96-dpi derivation, so if Dina's smallest strike exceeds the 11 px em requested, Windows
renders bigger than the request while macOS, with a freely scalable substitute, renders
exactly the 11 px asked for. Raster strikes also usually have `tmInternalLeading = 0`, so
their `tmHeight` is essentially all glyph, where a scalable face at the same *cell* spends
~15% of it on leading — the text reads smaller even at matched `tmHeight`.

**Dina's actual strike dimensions are deliberately not asserted here.** There is no
Windows measurement, and this project has been burned by a number "measured on Windows"
that was sampled at the wrong layer. §5 settles it directly.

### Two genuine, small parity gaps in our code

1. **`CFont::CreateFont` drops `pitchAndFamily`.** The signature comments the parameter
   out (`BYTE /*nPitchFamily*/`, `platform/wx_gdi_compat.cpp:495`), and `nCharSet` with
   it; `spec.charsetHint` is used later for byte *decoding* but never for font
   *selection*. Windows' GDI mapper **honours** `lfPitchAndFamily` when a face is
   unknown: `FIXED_PITCH | FF_MODERN` substitutes a *monospaced* face, `FF_DONTCARE`
   the proportional default. So for any plugin passing a Windows-only face **plus** a
   pitch hint, the two platforms substitute by different rules. This one is unambiguously
   a defect rather than a judgement call.
2. **`GetTextFace` returns the requested face, not the realized one.**
   `WxSurface::fontMetrics` sets `m.face` from `CDC::GetTextFace`, which the shim
   implements as `wxFont::GetFaceName()` — on macOS that generally reports the name
   stored in the font's *descriptor*, i.e. what was asked for. On Windows GDI's
   `GetTextFace` returns the face **after** the mapper substitutes. So
   `WindowFontInfo(…, 21)` is a truthful fallback detector on Windows and very likely a
   lie on macOS, returning `"Dina"` for a font that is plainly Menlo.

   **This is a hypothesis, not a measurement** — it is falsifiable in one line in the
   macOS app: `WindowFont(w,"f","Dina",8) ; print(WindowFontInfo(w,"f",21))`.

Neither gap is a *size* effect. Fixing (1) makes the two platforms substitute by the same
rule; fixing (2) makes the fallback visible. Neither makes the text bigger.

---

## 3. `pal::CpuRaster` under-constrains the headless parity assertions

`CpuRaster::dpi()` is 96, so `spec.pixelHeight` arrives as the Windows **em**.
`pal/cpu_raster.cpp:459` assigns that em to `f.cellH`, and `:507` reports `cellH` as
`m.height` — i.e. as `tmHeight`, a **cell**. The headless reference therefore reports
`tmHeight` = 11 at 8 pt where Windows reports ~13: **short by exactly the internal
leading, ratio ≈ 1.18** (13/11).

This is the *golden adapter, plausible-wrong* shape already recorded in this project: the
test double answers with a number that looks right (it equals the Windows em, a real
quantity) and is pinned by the very tests meant to protect parity. It does not affect the
shipped app — `CpuRaster` is only the headless surface — but it means **the headless
assertions under-constrain the contract**, and anyone using them as a parity target would
"fix" the GUI toward a wrong number. Read the `tests/test_lua_surface.cpp` and
`platform/miniwindow_selftest.cpp` expectations added in `bc6ad30`/`0c1820b` with that in
mind.

Correcting it is one line plus its fallout: carry `f.emH = spec.pixelHeight` and report
`m.height = emH + internalLeading` with `m.internalLeading = emH * 3 / 20` (~15%), keeping
`m.ascent`/`m.descent` consistent. **It will turn several existing assertions red** —
those pin the current, wrong value, which is the "stubs defend wrong contracts" trap. It
changes nothing a user sees. Left undone deliberately; recorded so it is not rediscovered.

---

## 4. Why the coverage never caught any of this

`--pal-miniwindow-selftest` (157 checks) and `test_lua_surface` (79) covered the font
family throughout, and **both were green the whole time the factor was 1.78x wrong**.
Every assertion over the family was *relational* — max >= average, a longer string
measures wider, drawing inks pixels — and not one pinned an absolute size, so the entire
family was **scale-invariant by construction**. That is coverage without a contract.
`0c1820b` added the absolute pins on both adapters.

Two related lessons worth keeping:

* **The comment above the defect derived the right answer and the line below it produced
  a different one.** `MulDiv(10, 96, 72) = 13` was stated explicitly in the prose; the
  code produced 17.8. Forty lines of justification, the first half a correct derivation
  for ONE factor and the second half labelling the extra factor "empirical … a measured
  correction, not a derived one". Where a comment's own arithmetic disagrees with the
  code beneath it, believe neither — measure. An "empirical correction" with no
  reproducible measurement recorded beside it is a defect with a good alibi.
* **The old comment named its own falsifier** — that plugin text reaching the edge of its
  miniwindow (the xp counter's trailing `)`) would mean the factor was wrong — and
  recorded it as "probably a plugin bug". It was not. A comment that volunteers a
  falsifier is a test that was never written.

---

## 5. The Windows measurement script

Paste into the Windows MUSHclient **Game > Immediate** window (Lua). `print` is
MUSHclient's own and writes to the world output, so the result can be selected and pasted
straight back.

```lua
local W, S = "fontprobe", "MMMMMMMMMM"
WindowCreate(W, 0, 0, 500, 200, 0, 0, ColourNameToRGB("black"))
local faces = { "Dina", "Courier New", "Lucida Console", "Fixedsys", "Arial", "Tahoma" }
local sizes = { 8, 9, 10, 12, 14 }
local function I(c) return tostring(WindowFontInfo(W, "p", c)) end
print("face|pt|REALIZED|tmHeight|tmAscent|tmDescent|tmIntLead|tmExtLead|tmAveChar|tmMaxChar|tmPitch|w(MMMMMMMMMM)|w(0 xp gained in)")
for _, f in ipairs(faces) do
  for _, s in ipairs(sizes) do
    local rc = WindowFont(W, "p", f, s, false, false, false, false, 0, 0)
    if rc ~= 0 then
      print(f .. "|" .. s .. "|WindowFont FAILED rc=" .. rc)
    else
      print(table.concat({ f, s, I(21), I(1), I(2), I(3), I(4), I(5), I(6), I(7), I(19),
        tostring(WindowTextWidth(W, "p", S)),
        tostring(WindowTextWidth(W, "p", "0 xp gained in")) }, "|"))
    end
  end
end
WindowDelete(W)
```

**Reading it.** Column 3 (`REALIZED`, info code 21 = `GetTextFace`) is the honoured face
**after** GDI's mapper substitutes — if it does not say `Dina` for the Dina rows, Dina is
not installed on that machine and §2's bitmap-strike branch is moot. Column 4
(`tmHeight`) is the cell that plugin layout is written against; compare it to
`MulDiv(pt,96,72)` = 11/12/13/16/19 to get the internal leading, which column 8
(`tmIntLead`) reports directly — a raster strike should show ~0 there, a scalable face
~2-3. Columns 13/14 give the horizontal fit `WindowTextWidth` drives, against a monospace
probe and against real plugin text.

The last two arguments (`0, 0` = `DEFAULT_CHARSET`, `FF_DONTCARE`) are the Lua defaults.
If a plugin survey shows Quow passes non-zero `charset`/`pitchAndFamily`, re-run with
those values — the pitch hint is what steers GDI's substitution (§2), so it can change
the answer.

## 6. What to do with the result

| Code 21 says | Code 1 at 8 pt | Conclusion | Action |
|---|---|---|---|
| `Dina` | ≈ 11-13 | metrics already match; the complaint is typeface appearance | a face-substitution table, to pick a better-looking mono. Stop there. |
| `Dina` | meaningfully > 13 | Windows genuinely renders larger, via the raster-strike floor | substitution table **plus** a *derived, documented* per-face size correction (requested em -> the strike height Windows would have picked). Reconsider bundling a metric-compatible font. |
| anything else | — | the plugins were never getting Dina on that box either | honour `pitchAndFamily` (§2) so both platforms substitute by the same rule. The Dina line of enquiry collapses. |

In every branch: **do not touch the wx sizing arithmetic.** §1 shows it is exact, so any
further "make it bigger" dial is the fudge factor coming back.

## 7. What would falsify this

* **§1 (the size pipeline is exact):** any measured Windows `tmHeight` at a
  *known-present, scalable* face (Courier New / Arial) differing from the macOS
  `tmHeight` for the same face and size by more than ±1 px. That would mean a residual
  scale factor still exists.
* **§2 (face substitution is the residual cause):** a miniwindow using a face macOS
  *does* have (Courier New / Arial / Tahoma / empty) also being too small. Then the cause
  is not the face.
* **§3 (`CpuRaster` is wrong):** `tmInternalLeading` measured as 0 on Windows for the
  common faces — then `tmHeight == em` and `CpuRaster` is right. Unlikely, since the
  macOS measurement already shows a 1.17 ratio, but it is the honest test.

## 8. One plugin defect found in passing

`QuowMinimap.xml:24171` centres the minimap title with
`iFontSizes[winMinimap]["lineheight"]` — the **body** font's line height (FixedSys 8), not
the title font's (`"titlefont"..winMinimap`, Dina 8). On Windows the two are close enough
to hide it; once the two faces resolve differently the title sits off-centre inside
`TITLE_HEIGHT = 17` as well as being the wrong size. **This is a plugin defect, not a
port defect** — recorded so it is not chased in our code.

Note also that the `sizem`/`sizel` tiers (11-15 pt) are gated on window width >= 250 and
every Dina window ships at 190-200 px wide, so **in practice only 8 pt and 9 pt are ever
requested** by that plugin pack.

## 9. The Windows measurement, taken — 2026-08-22

§5's script existed; this is the run. Measured in the **real GUI app** (`WinDebug\MUSHclient.exe`,
Debug/Win32, DPI-unaware so `LOGPIXELSY = 96`) with a world script that creates a
miniwindow, calls `world.WindowFont`, and reads back `world.WindowFontInfo` — including
**info code 21, the realized face**, which is the code §2 says answers the question.

### 9a. §3 item 1 is CONFIRMED, and the substitutes are named

Requesting a 10 pt font by name, varying only the `PitchAndFamily` argument
(`world.WindowFont`'s 10th, which the port's `CFont::CreateFont` drops):

| pitch hint | unknown face → realized | `Courier New` → | `Arial` → |
|---|---|---|---|
| `0` (DEFAULT_PITCH \| FF_DONTCARE) | **Arial** | Courier New | Arial |
| `1` (FIXED_PITCH) | **Courier New** | Courier New | Arial |
| `49` (FIXED_PITCH \| FF_MODERN) | **Courier New** | Courier New | Arial |
| `2` (VARIABLE_PITCH) | **Arial** | Courier New | Arial |
| `34` (VARIABLE_PITCH \| FF_SWISS) | **Arial** | Courier New | Arial |

`"NoSuchFaceAtAll"`, `"Menlo"` (macOS-only) and `""` all behaved identically — the mapper
does not distinguish "absent" from "never heard of it".

Two things follow, and the second matters more than the first:

1. The hypothesis was right. A fixed-pitch hint really does buy a monospaced substitute,
   and dropping `pitchAndFamily` really does cost it.
2. **But the hint only decides anything when the face is unknown.** A face Windows *has*
   wins over the hint in every row — `Courier New` with a VARIABLE_PITCH hint is still
   Courier New. And the default hint is `0`, which Windows itself answers with **Arial**,
   the same proportional fallback the port produces. So the port's dropped field changes
   the result **only** for a script that passes an unavailable face *together with* a
   fixed-pitch hint. That is a narrower blast radius than §3 implied, and it is worth
   knowing before spending anything on `CFont::CreateFont`.

### 9b. §3 item 3 is CONFIRMED with a number: `tmHeight = em + tmInternalLeading`

Across four sizes and every scalable face measured, `height - internalLeading` equals
`MulDiv (points, 96, 72)` **exactly** — and note `MulDiv` rounds, which is why 8 pt gives
11 and not 10:

| face | pt | height | internalLeading | height − IL | `MulDiv(pt,96,72)` |
|---|---|---|---|---|---|
| Courier New | 8 | 14 | 3 | 11 | 11 |
| Courier New | 10 | 16 | 3 | 13 | 13 |
| Courier New | 12 | 18 | 2 | 16 | 16 |
| Courier New | 24 | 36 | 4 | 32 | 32 |
| Consolas | 10 | 15 | 2 | 13 | 13 |
| Lucida Console | 10 | 13 | 0 | 13 | 13 |
| Arial | 10 | 16 | 3 | 13 | 13 |

So `pal::CpuRaster` reporting the em as `tmHeight` is short by exactly
`tmInternalLeading` — **3 px out of 16 at 10 pt Courier New, about 19%**. That is the
size of the slack in the headless parity assertions, now quantified.

The two bitmap faces behave as §2's "Why a bitmap face can make Windows render *larger*"
predicts and are excluded from the identity above: **Fixedsys** answers `height = 15` at
8, 10 *and* 12 pt (one strike, no scaling) and 40 at 24 pt; **Terminal** answers 8/12/16/32
with `internalLeading = 0` throughout and `charset = 255` (OEM).

### 9c. Method note

The first pass passed only eight arguments to `world.WindowFont`, so `Charset` and
`PitchAndFamily` both defaulted to 0 — and it therefore measured only the `FF_DONTCARE`
row, from which "GDI substitutes Arial" would have been the wrong general conclusion.
**A measurement that leaves the variable under test at its default has not tested it.**
The second pass varies the hint and is the one the table above comes from.
