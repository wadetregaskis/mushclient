-- The GetInfo window family and the command bar, where a CMUSHView, a
-- CSendView and a CMainFrame all exist.  Every one of these goes through
-- pal/win_worldview.cpp, which the unit target cannot reach: a document with no
-- window answers the documented "nothing there" values instead
-- (tests/win/test_win_worldview.cpp pins THAT side).
--
-- Assertions are on internal consistency and on bounds, never on absolute
-- pixel values -- this must pass on any screen, at any window size.

local H = dofile (assert (MC_PROBE_DIR) .. "/harness.lua")

function probeMain ()
 H.run ("worldview", function ()

  local function gi (n) return world.GetInfo (n) end

  -- The world is open, so the output window exists.  If this is false every
  -- assertion below is measuring the no-view fallbacks and means nothing.
  H.eq (gi (285), true, "GetInfo 285: output window available")

  -- Main client rect.  249/250 are the pair that used to be FABRICATED from
  -- gui_rows/gui_cols when the client was minimised; they must now be real
  -- geometry, which means positive and no larger than the screen.
  local scrW, scrH = world.GetSystemMetrics (0), world.GetSystemMetrics (1)
  H.ok (scrW > 0 and scrH > 0, "GetSystemMetrics returns a real screen",
        scrW .. "x" .. scrH)
  H.ok (gi (250) > 0 and gi (250) <= scrW, "GetInfo 250 (main width) is on-screen", gi (250))
  H.ok (gi (249) > 0 and gi (249) <= scrH, "GetInfo 249 (main height) is on-screen", gi (249))

  -- The output view's client size and its text-area extent describe the same
  -- rectangle from two different code paths, so they must agree.
  H.eq (gi (292), gi (281), "text-area right edge == output view width")
  H.eq (gi (293), gi (280), "text-area bottom edge == output view height")
  H.ok (gi (280) > 0 and gi (281) > 0, "the output view has a non-empty client rect",
        gi (281) .. "x" .. gi (280))

  -- No TextRectangle has been set, so its four members are zero.  This is the
  -- control for the pair above: it shows the non-zero answers are not simply
  -- "every GetInfo returns something".
  H.eq (gi (272), 0, "TextRectangle left defaults to 0")
  H.eq (gi (273), 0, "TextRectangle top defaults to 0")

  -- The command bar, through a real CSendView.
  --
  -- SetCommand REFUSES a bar that is not empty, returning eCommandNotEmpty.
  -- That is upstream behaviour and not a port quirk -- methods_commands.cpp has
  -- had the `if (strCurrent.IsEmpty ())` guard since e362b37 -- and it is
  -- surprising enough that the first draft of this probe asserted the opposite
  -- and read the refusal as a stale selection.
  H.eq (world.GetCommand (), "", "the command bar starts empty")
  H.eq (world.SetCommand ("hi"), 0, "SetCommand fills an empty bar")
  H.eq (world.GetCommand (), "hi", "SetCommand round-trips")

  -- SelectCommand selects all of it.  Asserting the END against a length this
  -- probe chose is what distinguishes a real measurement from a sentinel.
  world.SelectCommand ()
  H.eq (gi (236), 1, "selection starts at 1")
  H.eq (gi (237), 2, "selection ends at the end of a two-character command")

  H.neq (world.SetCommand ("something much longer"), 0,
         "SetCommand refuses a bar that is not empty")
  H.eq (world.GetCommand (), "hi", "...and leaves the existing text alone")
  H.eq (gi (237), 2, "...so the selection still describes the real text")


  -- The output buffer has lines in it (the world opened and said so), and the
  -- selection getters report "no selection" as all-zero rather than rubbish.
  H.ok (world.GetLinesInBufferCount () > 0, "the output buffer has lines",
        world.GetLinesInBufferCount ())
  H.eq (world.GetSelectionStartLine (), 0, "no output selection: start line 0")
  H.eq (world.GetSelectionEndLine (), 0, "no output selection: end line 0")

  -- GetSysColor reaches the real Win32 one: COLOR_3DFACE is a light grey on
  -- every stock theme, so assert it is light rather than pinning a value.
  local face = world.GetSysColor (15)
  local r, g, b = face % 256, math.floor (face / 256) % 256, math.floor (face / 65536) % 256
  H.ok (r > 128 and g > 128 and b > 128, "GetSysColor(COLOR_3DFACE) is a light grey",
        H.hex (face))

  -- A timer is running us, so the session counter must have counted it.  This
  -- is the assertion that would fail if CheckTimers were not driving anything.
  H.ok (gi (288) >= 1, "GetInfo 288 counted the timer that called this", gi (288))
 end)
end
