--[[ tests/lua/miniwindow_conformance.lua — the miniwindow drawing conformance
     checklist, expressed as a script that runs inside a REAL client.

     Paste into Game ▸ Immediate (Ctrl+I) in any open world, or
     `require`/dofile it, or run it as an alias.  It creates one hidden 64x48
     miniwindow, draws into it, reads pixels back, prints a PASS/FAIL line per
     probe and deletes the window.  Nothing touches the MUD.

     WHY IT EXISTS.  platform/miniwindow_selftest.cpp drives the same engine
     bodies through pal::ISurface on macOS and is run by ctest; there is no
     equivalent harness in the Windows build, so the same ~40 assertions had
     never been executed against pal/gdi_surface.cpp.  This is that checklist,
     rewritten against the scripting API so it runs anywhere the product runs
     — which also makes it usable against a stock, official MUSHclient as the
     reference (docs/WINDOWS-VALIDATION.md §3 wants both baselines).

     THREE PROBES EXPECT DIFFERENT ANSWERS ON THE TWO PLATFORMS, by design.
     They are marked PLATFORM below and the script picks the expectation from
     `windows`, derived from Lua's own directory separator so it needs nothing
     from the host application.

       * line endpoints: GDI LineTo excludes the final pixel, the port's
         surface draws it.
       * a stroked outline: GDI pens are not antialiased, so the edge pixel is
         exactly the pen colour; wxOSX blends it with what is underneath.
       * WindowFontInfo(7) (tmMaxCharWidth) vs (6) (tmAveCharWidth): real GDI
         reports Courier New 10pt as max 10 / average 8, because the glyph cell
         is wider than the average advance even when every advance is the same;
         the port measures the widest printable advance, which for a monospace
         face IS the average.  So only "max >= average" holds on both, and
         neither side may assert equality.

     WindowGetPixel outside the window was a fourth until 2026-08-10, and is
     deliberately NOT one now: CLR_INVALID reaches a script as -1 on BOTH
     platforms.  miniwindow.cpp folds 0xFFFFFFFF to -1L at the doc boundary
     precisely because a straight widening cast gives 4294967295 on LP64 and
     `if WindowGetPixel(...) == -1` then never matches.  Expecting the widened
     value here would have reported the FIX as the regression.

     Everything else must agree on both platforms; a mismatch is a finding.
]]

local W = "conformance_mw"

-- COLORREF, 0x00BBGGRR — the order the world API takes.
local RED, GREEN, BLUE   = 0x0000FF, 0x00FF00, 0xFF0000
local WHITE, BLACK       = 0xFFFFFF, 0x000000

local eOK               = 0
local eNoNameSpecified  = 30003
local eNoSuchWindow     = 30073

local pass, fail, notes = 0, 0, {}

local function check (ok, what)
  if ok then
    pass = pass + 1
    ColourNote ("lime", "", "  [PASS] " .. what)
  else
    fail = fail + 1
    notes [#notes + 1] = what
    ColourNote ("white", "red", "  [FAIL] " .. what)
  end
end

-- Report the value too when a probe fails, so a mismatch is diagnosable
-- without re-running by hand.
local function checkEq (got, want, what)
  check (got == want,
         string.format ("%s  (got %s, wanted %s)", what, tostring (got), tostring (want)))
end

-- Which side of the port are we on?  package.config's first line is the
-- directory separator: "\" on Windows, "/" everywhere else.  Asking Lua rather
-- than the host keeps this correct whatever GetInfo happens to return.
local windows = package.config:sub (1, 1) == "\\"

Note ("")
ColourNote ("yellow", "", "Miniwindow conformance — MUSHclient " ..
                          tostring (GetInfo (72)) ..
                          (windows and "  (Windows/GDI expectations)"
                                    or "  (port expectations)"))

-- Start from a clean slate even if a previous run died half way.
WindowDelete (W)

-- ── creation + error pins ──────────────────────────────────────────────────
checkEq (WindowCreate ("", 0, 0, 10, 10, 0, 0, BLACK), eNoNameSpecified,
         "WindowCreate refuses an empty name")
checkEq (WindowCreate (W, 0, 0, 64, 48, 0, 0, BLUE), eOK,
         "WindowCreate 64x48 succeeds")
checkEq (WindowRectOp ("nosuch", 2, 0, 0, 1, 1, RED, 0), eNoSuchWindow,
         "ops on an unknown window return eNoSuchWindow")

checkEq (WindowGetPixel (W, 5, 5), BLUE,
         "create's background fill is readable (WindowGetPixel)")
-- NOT a platform difference: miniwindow.cpp folds CLR_INVALID to -1L on every
-- platform, so that `WindowGetPixel(...) == -1` matches on LP64 too.
checkEq (WindowGetPixel (W, 9999, 5), -1,
         "WindowGetPixel outside the window is CLR_INVALID")
checkEq (WindowGetPixel ("nosuch", 5, 5), -2,
         "WindowGetPixel on an unknown window returns -2")

-- ── CircleOp action 3 = RoundRect: right/bottom-EXCLUSIVE like Win32 ───────
do
  local L, T, R, B = 2, 20, 22, 40            -- 20x20, midline y = 30
  checkEq (WindowCircleOp (W, 3, L, T, R, B, GREEN, 0, 1, GREEN, 0, 6, 6, 0, 0), eOK,
           "WindowCircleOp (round rectangle) succeeds")
  checkEq (WindowGetPixel (W, R - 1, 30), GREEN,
           "RoundRect fills up to x2-1 (right-exclusive)")
  check   (WindowGetPixel (W, R, 30) ~= GREEN,
           "...and NOT x2 itself")
  checkEq (WindowGetPixel (W, 12, B - 1), GREEN,
           "RoundRect fills up to y2-1 (bottom-exclusive)")
  check   (WindowGetPixel (W, 12, B) ~= GREEN,
           "...and NOT y2 itself")
end

-- ── CircleOp action 4 = Chord: a chord, not a pie ──────────────────────────
do
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLACK, 0), eOK,
           "clear before the chord probe")
  -- The arc runs anticlockwise from the start point, so (40,8) -> (8,8) is the
  -- short cap over the top; the reverse sweeps 270 degrees and legitimately
  -- covers the centre.
  checkEq (WindowCircleOp (W, 4, 4, 4, 44, 44, RED, 0, 1, RED, 0, 40, 8, 8, 8), eOK,
           "WindowCircleOp (chord) succeeds")
  checkEq (WindowGetPixel (W, 24, 6), RED,
           "the chord fills the cap between its arc and its chord line")
  check   (WindowGetPixel (W, 24, 18) ~= RED,
           "...and NOT below the chord line, which a pie would fill")
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "restore the background after the chord probe")
end

-- ── CircleOp brush styles 8-12: the built-in mono patterns ─────────────────
-- GDI colours a 1-bpp pattern brush from the DC: 0 bits take the text colour,
-- 1 bits the background.  Style 8 ("fine") is a 50/50 checkerboard, so a
-- filled area must contain BOTH the pen and brush colours and NEITHER black
-- nor white.
do
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "clear before the mono-pattern probe")
  checkEq (WindowCircleOp (W, 2, 4, 4, 40, 40,
                           GREEN, 5 --[[PS_NULL: no outline]], 1,
                           RED, 8 --[[mono pattern "fine"]], 0, 0, 0, 0), eOK,
           "WindowCircleOp (mono pattern brush) succeeds")
  local sawRed, sawGreen, sawMono = 0, 0, 0
  for x = 10, 33 do
    for y = 10, 33 do
      local c = WindowGetPixel (W, x, y)
      if     c == RED   then sawRed   = sawRed   + 1
      elseif c == GREEN then sawGreen = sawGreen + 1
      elseif c == BLACK or c == WHITE then sawMono = sawMono + 1
      end
    end
  end
  checkEq (sawMono, 0, "mono pattern never falls back to black/white")
  check   (sawRed > 0 and sawGreen > 0,
           string.format ("a checkerboard keeps BOTH colours (red %d, green %d)",
                          sawRed, sawGreen))
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "restore the background after the mono-pattern probe")
end

-- ── RectOp action 2 = solid fill ───────────────────────────────────────────
checkEq (WindowRectOp (W, 2, 10, 10, 20, 20, RED, 0), eOK, "WindowRectOp fill succeeds")
checkEq (WindowGetPixel (W, 15, 15), RED,  "fill interior is the fill colour")
checkEq (WindowGetPixel (W, 25, 15), BLUE, "outside the fill is untouched")
checkEq (WindowGetPixel (W, 20, 15), BLUE, "fill right edge is exclusive")

-- ── Line (solid pen, width 1) ──────────────────────────────────────────────
checkEq (WindowLine (W, 0, 40, 63, 40, GREEN, 0 --[[PS_SOLID]], 1), eOK,
         "WindowLine succeeds")
checkEq (WindowGetPixel (W, 30, 40), GREEN, "line pixel present mid-span")
-- PLATFORM: GDI's LineTo excludes the last pixel; the port's surface includes it.
if windows then
  check (WindowGetPixel (W, 63, 40) ~= GREEN,
         "line endpoint NOT drawn (GDI LineTo is exclusive)")
else
  checkEq (WindowGetPixel (W, 63, 40), GREEN,
           "line endpoint drawn (port semantics: inclusive)")
end

-- ── CircleOp action 2 = rectangle, solid brush ─────────────────────────────
checkEq (WindowCircleOp (W, 2, 30, 2, 40, 8, WHITE, 0, 1, GREEN, 0, 0, 0, 0, 0), eOK,
         "WindowCircleOp (rectangle, solid brush) succeeds")
checkEq (WindowGetPixel (W, 35, 5), GREEN, "shape interior is brush colour")
do
  local edge = WindowGetPixel (W, 30, 2)
  -- PLATFORM: GDI pens are not antialiased, so the edge is exactly the pen
  -- colour; wxOSX blends, so all we can say there is "something was drawn".
  if windows then
    checkEq (edge, WHITE, "shape outline is the crisp pen colour (no AA)")
  else
    check (edge ~= BLUE and edge ~= GREEN,
           "shape outline drawn (blend of pen over edge; wxOSX AA)")
  end
end

-- ── Gradient: horizontal red -> blue (mode 1) ──────────────────────────────
checkEq (WindowGradient (W, 0, 44, 64, 48, RED, BLUE, 1), eOK,
         "WindowGradient (horizontal) succeeds")
do
  local l, r = WindowGetPixel (W, 1, 46), WindowGetPixel (W, 62, 46)
  check (l % 256 > 0xC0 and math.floor (l / 65536) < 0x40,
         string.format ("gradient left end is red-dominant (0x%06X)", l))
  check (math.floor (r / 65536) > 0xC0 and r % 256 < 0x40,
         string.format ("gradient right end is blue-dominant (0x%06X)", r))
end

-- ── fonts + text ───────────────────────────────────────────────────────────
checkEq (WindowFont (W, "f1", "Courier New", 10, false, false, false, false, 0, 0), eOK,
         "WindowFont creates a font")
checkEq (WindowText (W, "nofont", "x", 0, 0, 0, 0, WHITE, false), -2,
         "WindowText with an unknown font id returns -2")
do
  local w  = WindowText (W, "f1", "Hi", 2, 24, 0, 0, WHITE, false)
  check (w > 0, "WindowText draws and returns a positive width")
  checkEq (WindowTextWidth (W, "f1", "Hi", false), w,
           "WindowTextWidth matches WindowText's return")
end

-- WindowFontInfo(19) is TEXTMETRIC::tmPitchAndFamily, where (unlike a LOGFONT)
-- bit 0 is INVERTED: set means VARIABLE pitch.  Courier New is fixed + modern.
do
  local pf = WindowFontInfo (W, "f1", 19)
  check (type (pf) == "number" and pf >= 0, "WindowFontInfo(19) returns a pitch/family")
  if type (pf) == "number" then
    checkEq (pf % 2, 0,
             "a fixed-pitch font does NOT set TMPF_FIXED_PITCH (inverted flag)")
    checkEq (math.floor (pf / 16) % 16 * 16, 48 --[[FF_MODERN]],
             "...and reports a real family, not FF_DONTCARE")
  end
  local avg, max = WindowFontInfo (W, "f1", 6), WindowFontInfo (W, "f1", 7)
  if type (avg) == "number" and type (max) == "number" and avg > 0 then
    -- PLATFORM: real GDI reports Courier New 10pt as max 10 / average 8, while
    -- the port measures the widest printable advance -- which for a monospace
    -- face is the average, so it reports 11 / 11.  Only the inequality is
    -- common ground.  This arm used to assert EQUALITY off Windows, on the
    -- since-retired premise that the port synthesised both from one
    -- measurement; wx_gdi_compat.cpp has measured tmMaxCharWidth separately,
    -- for every font, since 2026-08-10.
    check (max >= avg,
           string.format ("max char width >= average (%d >= %d)", max, avg))
  else
    check (false, "WindowFontInfo(6)/(7) return numbers")
  end
end

-- ── RectOp action 5 = DrawEdge: the BF_ side flags are honoured ────────────
do
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK, "clear before the DrawEdge probe")
  -- EDGE_RAISED (0x05), left + top only (BF_LEFT|BF_TOP = 0x03)
  checkEq (WindowRectOp (W, 5, 10, 10, 40, 40, 0x05, 0x03), eOK,
           "WindowRectOp (DrawEdge, left+top only) succeeds")
  check   (WindowGetPixel (W, 10, 25) ~= BLUE, "DrawEdge strokes LEFT when BF_LEFT is set")
  check   (WindowGetPixel (W, 25, 10) ~= BLUE, "...and TOP when BF_TOP is set")
  checkEq (WindowGetPixel (W, 39, 25), BLUE,   "...and leaves RIGHT alone when BF_RIGHT is not")
  checkEq (WindowGetPixel (W, 25, 39), BLUE,   "...and BOTTOM likewise")

  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK, "clear before the etched probe")
  -- EDGE_ETCHED (0x06) is sunken-outer + raised-inner: the rings must differ.
  checkEq (WindowRectOp (W, 5, 10, 10, 40, 40, 0x06, 0x0F), eOK,
           "WindowRectOp (DrawEdge, EDGE_ETCHED) succeeds")
  check   (WindowGetPixel (W, 25, 10) ~= WindowGetPixel (W, 25, 11),
           "EDGE_ETCHED's outer and inner rings differ (not a flat box)")
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "restore the background after the DrawEdge probe")
end

-- ── read-modify-write is lossless ──────────────────────────────────────────
-- WindowFilter reads a region, edits the bytes and writes them back.  Op 7 is
-- additive brightness, so an amount of 0 is the identity: alternating 1px
-- columns must survive byte-identical.  (This caught a HiDPI resampling bug on
-- the port; on Windows the canvas is 1:1, so it is a control.)
do
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLACK, 0), eOK,
           "clear before the filter round-trip")
  for x = 10, 19 do
    WindowRectOp (W, 2, x, 10, x + 1, 20, (x % 2 == 1) and RED or GREEN, 0)
  end
  local before11, before12 = WindowGetPixel (W, 11, 15), WindowGetPixel (W, 12, 15)
  check   (before11 ~= before12, "adjacent 1px columns really are different colours")
  checkEq (WindowFilter (W, 8, 8, 24, 24, 7 --[[brightness]], 0.0), eOK,
           "WindowFilter (identity brightness) succeeds")
  checkEq (WindowGetPixel (W, 11, 15), before11,
           "a read-modify-write leaves an exact colour exactly alone")
  checkEq (WindowGetPixel (W, 12, 15), before12,
           "...and does not bleed its neighbour into it")
end

-- ── lifecycle: resize, position, info, delete ──────────────────────────────
do
  checkEq (WindowResize (W, 32, 24, GREEN), eOK, "WindowResize succeeds")
  checkEq (WindowInfo (W, 3), 32, "WindowInfo(3) reports the new width")
  checkEq (WindowInfo (W, 4), 24, "WindowInfo(4) reports the new height")
  checkEq (WindowGetPixel (W, 16, 12), GREEN, "resize repaints with the new background")
  check   (WindowGetPixel (W, 40, 12) < 0, "...and the old area is gone (out of bounds)")
  checkEq (WindowDelete (W), eOK, "WindowDelete succeeds")
  checkEq (WindowDelete (W), eNoSuchWindow, "...and deleting again says eNoSuchWindow")
end

Note ("")
if fail == 0 then
  ColourNote ("lime", "", string.format ("Miniwindow conformance: %d/%d PASSED", pass, pass))
else
  ColourNote ("white", "red",
              string.format ("Miniwindow conformance: %d passed, %d FAILED", pass, fail))
  for _, what in ipairs (notes) do
    ColourNote ("white", "red", "    " .. what)
  end
end
