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

  -- ...and its EXTENT is the exclusive one, like the RoundRect probe above.
  -- Chord traces its own outline, so unlike Ellipse it does not get the
  -- exclusive rectangle for free.  The right HALF of the ellipse (start at the
  -- bottom, end at the top, sweeping anticlockwise through 3 o'clock) puts arc
  -- extremes exactly on the right and bottom edges.  NO platform split: the
  -- expected pixels are identical on both sides, which is the point.
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLACK, 0), eOK,
           "clear before the chord extent probe")
  checkEq (WindowCircleOp (W, 4, 4, 4, 44, 44, RED, 0, 1, RED, 0, 24, 100, 24, 4), eOK,
           "WindowCircleOp (chord, right half) succeeds")
  checkEq (WindowGetPixel (W, 43, 24), RED,
           "the chord reaches x2-1 on the midline")
  check   (WindowGetPixel (W, 44, 24) ~= RED,
           "...and NOT x2 itself (the bounding rect is right-exclusive)")
  checkEq (WindowGetPixel (W, 24, 43), RED,
           "...reaches y2-1 at the bottom of the arc")
  check   (WindowGetPixel (W, 24, 44) ~= RED,
           "...and NOT y2 itself (bottom-exclusive too)")

  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "restore the background after the chord probe")
end

-- ── CircleOp action 5 = Pie: the radials stop ON the ellipse ───────────────
-- Extra1..4 are DIRECTION points: they define two rays from the ellipse
-- centre, the wedge's sides run centre -> ellipse, and the points themselves
-- are never stroked to -- which is why the API invites putting them well
-- outside the rectangle.  Rays straight down and straight right, both far
-- outside, so the wedge is the bottom-right quadrant.
do
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "clear before the pie probe")
  checkEq (WindowCircleOp (W, 5, 4, 4, 44, 44, GREEN, 0, 1, RED, 0,
                           24, 100, 100, 24), eOK,
           "WindowCircleOp (pie, rays far outside the ellipse) succeeds")
  checkEq (WindowGetPixel (W, 32, 32), RED, "the wedge itself is filled")
  checkEq (WindowGetPixel (W, 50, 24), BLUE,
           "nothing is painted along the ray BEYOND the ellipse")
  checkEq (WindowGetPixel (W, 24, 46), BLUE, "...nor along the other one")

  -- With a NULL brush Windows still strokes the two radii.
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "clear before the null-brush pie probe")
  checkEq (WindowCircleOp (W, 5, 4, 4, 44, 44, GREEN, 0, 1, RED, 1,
                           24, 100, 100, 24), eOK,
           "WindowCircleOp (pie, null brush) succeeds")
  check   (WindowGetPixel (W, 35, 24) ~= BLUE,
           "the horizontal radius IS stroked with no brush to close it")
  check   (WindowGetPixel (W, 24, 35) ~= BLUE, "...and the vertical one")
  checkEq (WindowGetPixel (W, 50, 24), BLUE,
           "...and each stops at the ellipse, not at the caller's point")
  checkEq (WindowGetPixel (W, 32, 32), BLUE, "...and a null brush fills nothing")

  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "restore the background after the pie probe")
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

  -- ::DrawEdge strokes strictly INSIDE the rectangle -- Win32 RECTs are
  -- right/bottom-exclusive, so the outer ring occupies left..right-1 and
  -- nothing is painted at right or bottom.  One PAIR of sides at a time, so no
  -- other stroke can cover for a mis-sized one.  NO platform split: these
  -- pixels are expected to be identical on both sides.
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "clear before the DrawEdge extent probe")
  checkEq (WindowRectOp (W, 5, 10, 10, 40, 40, 0x05, 0x03), eOK,
           "WindowRectOp (DrawEdge, EDGE_RAISED, left+top) succeeds")
  check   (WindowGetPixel (W, 39, 10) ~= BLUE, "the TOP side reaches x == right-1")
  checkEq (WindowGetPixel (W, 40, 10), BLUE,   "...and paints nothing at x == right")
  check   (WindowGetPixel (W, 10, 39) ~= BLUE, "the LEFT side reaches y == bottom-1")
  checkEq (WindowGetPixel (W, 10, 40), BLUE,   "...and paints nothing at y == bottom")

  -- The inner ring is inset by one on every named side, so it must not reach
  -- the outer ring's right column: the outer colour at the corner must equal
  -- its colour halfway down the side.
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "clear before the DrawEdge corner probe")
  checkEq (WindowRectOp (W, 5, 10, 10, 40, 40, 0x05, 0x0F), eOK,
           "WindowRectOp (DrawEdge, EDGE_RAISED, BF_RECT) succeeds")
  check   (WindowGetPixel (W, 39, 11) == WindowGetPixel (W, 39, 25),
           "the inner ring does not overwrite the outer ring's corners")
  check   (WindowGetPixel (W, 11, 39) == WindowGetPixel (W, 25, 39),
           "...on the bottom edge either")

  -- BF_DIAGONAL (0x0010) replaces the box with a diagonal vector, and the side
  -- bits then name its END POINT.  Only the SHAPE is asserted -- Win32 shades
  -- the vector with both ring colours and the port draws one line, which is
  -- recorded as an open question in docs/UPSTREAM-DIVERGENCES.md section 8.
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "clear before the BF_DIAGONAL probe")
  checkEq (WindowRectOp (W, 5, 10, 10, 40, 40, 0x05, 0x13), eOK,
           "WindowRectOp (DrawEdge, BF_DIAGONAL_ENDTOPLEFT) succeeds")
  check   (WindowGetPixel (W, 25, 25) ~= BLUE,
           "BF_DIAGONAL draws a diagonal across the rectangle")
  checkEq (WindowGetPixel (W, 25, 10), BLUE,
           "...INSTEAD of the box: the top side is not stroked")
  checkEq (WindowGetPixel (W, 10, 25), BLUE, "...nor the left side")
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "clear before the second BF_DIAGONAL probe")
  checkEq (WindowRectOp (W, 5, 10, 10, 40, 40, 0x05, 0x16), eOK,
           "WindowRectOp (DrawEdge, BF_DIAGONAL_ENDTOPRIGHT) succeeds")
  check   (WindowGetPixel (W, 15, 34) ~= BLUE and WindowGetPixel (W, 34, 15) ~= BLUE,
           "ENDTOPRIGHT runs bottom-left to top-right")
  check   (WindowGetPixel (W, 15, 15) == BLUE and WindowGetPixel (W, 34, 34) == BLUE,
           "...and NOT along the other diagonal")

  -- BF_MONO (0x8000) and BF_FLAT (0x4000) draw a FLAT border: one colour the
  -- whole way round each ring, no bevel.  Only that structure is asserted --
  -- the port's exact colour tables (WINDOWFRAME/WINDOW and 3DSHADOW/3DFACE)
  -- are its reading of Win32, not a measurement, so a colour mismatch here is
  -- information rather than a port bug.  See docs/UPSTREAM-DIVERGENCES.md 8.
  for _, f in ipairs { { 0x800F, "BF_MONO" }, { 0x400F, "BF_FLAT" } } do
    checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
             "clear before the " .. f [2] .. " probe")
    checkEq (WindowRectOp (W, 5, 10, 10, 40, 40, 0x05, f [1]), eOK,
             "WindowRectOp (DrawEdge, BF_RECT|" .. f [2] .. ") succeeds")
    local o = WindowGetPixel (W, 10, 25)
    check (o == WindowGetPixel (W, 39, 25) and
           o == WindowGetPixel (W, 25, 10) and
           o == WindowGetPixel (W, 25, 39),
           f [2] .. "'s outer ring is ONE colour all round (no bevel)")
    check (WindowGetPixel (W, 11, 25) ~= o,
           "...over a differently-coloured inner ring")
  end

  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "restore the background after the DrawEdge probe")
end

-- ── a NULL pen shrinks the figure by one on right/bottom ───────────────────
-- GDI's documented null-pen shrink, which docs/PAL.md calls normative and
-- pal::CpuRaster implements.  This probe IS the question docs/WINDOWS-
-- VALIDATION.md section 10az asks: a failure here on Windows is the answer,
-- not a port bug -- see that section for which files change if it fails.
do
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "clear before the null-pen shrink probe")
  checkEq (WindowCircleOp (W, 2, 50, 30, 53, 32, WHITE, 5, 1, RED, 0, 0, 0, 0, 0), eOK,
           "WindowCircleOp (rectangle, null pen) succeeds")
  check   (WindowGetPixel (W, 50, 30) == RED and WindowGetPixel (W, 51, 30) == RED,
           "a null-pen rectangle still fills (w-1) x (h-1)")
  checkEq (WindowGetPixel (W, 52, 30), BLUE,
           "...but not its right column (GDI's null-pen shrink)")
  check   (WindowGetPixel (W, 50, 31) == BLUE and WindowGetPixel (W, 52, 31) == BLUE,
           "...nor its bottom row")

  -- Control: a real pen must still reach the last column and row.
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "clear before the stroked-rectangle control")
  checkEq (WindowCircleOp (W, 2, 50, 30, 53, 32, WHITE, 0, 1, RED, 0, 0, 0, 0, 0), eOK,
           "WindowCircleOp (rectangle, solid pen) succeeds")
  check   (WindowGetPixel (W, 52, 30) ~= BLUE and WindowGetPixel (W, 52, 31) ~= BLUE,
           "a STROKED rectangle still reaches x2-1/y2-1")

  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "restore the background after the null-pen shrink probe")
end

-- ── PS_INSIDEFRAME keeps the whole pen inside the rectangle ────────────────
-- The invariant the style names; the exact band edges are NOT asserted,
-- because GDI's rounding for even widths and whether the fill moves with the
-- border are open questions (docs/WINDOWS-VALIDATION.md section 10ba).
do
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "clear before the PS_INSIDEFRAME probe")
  checkEq (WindowCircleOp (W, 2, 10, 10, 40, 40, GREEN, 6, 9, BLUE, 1, 0, 0, 0, 0), eOK,
           "WindowCircleOp (rectangle, PS_INSIDEFRAME width 9) succeeds")
  checkEq (WindowGetPixel (W, 12, 25), GREEN,
           "the border is drawn INSIDE the rectangle")
  checkEq (WindowGetPixel (W, 7, 25), BLUE, "...and no ink lands outside it")
  checkEq (WindowGetPixel (W, 25, 7), BLUE, "...on the top edge either")

  -- Control: the same figure with an ordinary pen really does overflow.
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "clear before the PS_SOLID overflow control")
  checkEq (WindowCircleOp (W, 2, 10, 10, 40, 40, GREEN, 0, 9, BLUE, 1, 0, 0, 0, 0), eOK,
           "WindowCircleOp (rectangle, PS_SOLID width 9) succeeds")
  checkEq (WindowGetPixel (W, 7, 25), GREEN,
           "a centred wide pen DOES overflow the rectangle")

  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "restore the background after the PS_INSIDEFRAME probe")
end

-- ── mono pattern brushes: which colour slot means "transparent" ────────────
-- For styles 8-12 the engine puts BrushColour in the mono FOREGROUND
-- (SetTextColor) and PenColour in the background (SetBkColor), and upstream
-- keys SetBkMode on BrushColour.  Only the structure is asserted, since what
-- GDI renders for a CLR_INVALID colour slot is itself an open question
-- (docs/WINDOWS-VALIDATION.md 4c-sexdecies).
do
  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "clear before the -1 PenColour pattern probe")
  checkEq (WindowCircleOp (W, 2, 4, 4, 40, 40, -1, 5, 1, RED, 8, 0, 0, 0, 0), eOK,
           "WindowCircleOp (mono pattern, PenColour -1) succeeds")
  local sawBrush, sawOther, sawBg = 0, 0, 0
  for x = 10, 33 do
    for y = 10, 33 do
      local c = WindowGetPixel (W, x, y)
      if     c == RED  then sawBrush = sawBrush + 1
      elseif c == BLUE then sawBg    = sawBg    + 1
      else                  sawOther = sawOther + 1 end
    end
  end
  check (sawBrush > 0 and sawOther > 0,
         "a -1 background resolves to a colour, keeping BOTH tones")
  checkEq (sawBg, 0, "...and the pattern really did cover the area")

  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "clear before the transparent-gap pattern probe")
  checkEq (WindowCircleOp (W, 2, 4, 4, 40, 40, RED, 5, 1, -1, 8, 0, 0, 0, 0), eOK,
           "WindowCircleOp (mono pattern, BrushColour -1) succeeds")
  local sawPen = 0
  for x = 10, 33 do
    for y = 10, 33 do
      if WindowGetPixel (W, x, y) == RED then sawPen = sawPen + 1 end
    end
  end
  checkEq (sawPen, 0,
           "a transparent request never paints PenColour into the gaps")

  checkEq (WindowRectOp (W, 2, 0, 0, 64, 48, BLUE, 0), eOK,
           "restore the background after the mono-pattern probe")
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
