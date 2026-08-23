-- Miniwindow font metrics against the real GDI font mapper.
--
-- These two facts were open questions in docs/FONT-PARITY-2026-08.md until they
-- were measured (section 9); this probe is what stops them drifting back.  Both
-- are written against faces every Windows install has, so the probe does not
-- depend on what fonts this machine happens to carry.
--
-- It DOES depend on the client being DPI-unaware -- MUSHclient.vcxproj sets
-- EnableDpiAwareness=false in both configurations, so GetDeviceCaps(LOGPIXELSY)
-- answers 96 whatever the monitor is doing.  If that ever changes, this probe
-- fails, which is the correct outcome: every pixel measurement in the port's
-- parity work assumes it.

local H = dofile (assert (MC_PROBE_DIR) .. "/harness.lua")

-- MulDiv ROUNDS, which is why 8 pt is 11 px and not 10.
local function muldiv (a, b, c) return math.floor ((a * b) / c + 0.5) end

function probeMain ()
 H.run ("fontmetrics", function ()

  world.WindowCreate ("fp", 0, 0, 400, 200, 0, 0, 0xFFFFFF)
  local function info (i) return world.WindowFontInfo ("fp", "f", i) end
  local function load (face, pts, hint)
    return world.WindowFont ("fp", "f", face, pts, 0, 0, 0, 0, 0, hint or 0)
  end

  -- 1. tmHeight = em + tmInternalLeading, exactly.
  --
  -- This is the identity that quantifies pal::CpuRaster's under-report: it
  -- reports the em where Windows reports tmHeight, so it is short by exactly
  -- the internal leading (3 px in 16 at 10 pt Courier New).
  for _, face in ipairs {"Courier New", "Arial"} do
    for _, pts in ipairs {8, 10, 12, 24} do
      H.eq (load (face, pts), 0, "WindowFont " .. face .. " " .. pts)
      local em = info (1) - info (4)                 -- height - internalLeading
      H.eq (em, muldiv (pts, 96, 72),
            "em == MulDiv(" .. pts .. ",96,72) for " .. face)
    end
  end

  -- 2. The pitchAndFamily hint decides the substitute for an UNKNOWN face --
  --    and decides nothing for a known one.
  --
  -- This is the field platform/mfc_*.h's CFont::CreateFont drops.  Both halves
  -- matter: the second is what bounds how much the dropped field actually
  -- costs, and a probe asserting only the first would imply it costs more.
  local FIXED, DONTCARE = 1, 0          -- FIXED_PITCH, DEFAULT_PITCH|FF_DONTCARE
  for _, absent in ipairs {"NoSuchFaceAtAll", "Menlo", ""} do
    H.eq (load (absent, 10, FIXED), 0, "WindowFont absent+FIXED_PITCH")
    H.eq (info (21), "Courier New", "a fixed-pitch hint substitutes a monospaced face")
    H.eq (load (absent, 10, DONTCARE), 0, "WindowFont absent+FF_DONTCARE")
    H.eq (info (21), "Arial", "no hint substitutes the proportional default")
  end

  -- A face Windows HAS beats the hint in both directions.
  H.eq (load ("Courier New", 10, 2), 0, "WindowFont Courier New + VARIABLE_PITCH")
  H.eq (info (21), "Courier New", "a known monospaced face survives a variable hint")
  H.eq (load ("Arial", 10, FIXED), 0, "WindowFont Arial + FIXED_PITCH")
  H.eq (info (21), "Arial", "a known proportional face survives a fixed hint")

  -- 2b. The ascent/descent split -- the one font quantity still unmeasured on
  --     Windows, and the one pal::CpuRaster still invents
  --     (pal/cpu_raster.cpp:519-520 hands back descent = cellH / 5 and nothing
  --     pins it).  Section 9's run read codes 1 and 4 and never read 2 and 3,
  --     so docs/FONT-PARITY-2026-08.md section 3 has nothing to compare that
  --     invention against.
  --
  -- Two halves, deliberately.  The assertions are the Win32 contract, so they
  -- are falsifiable today without knowing the answer: GetTextMetrics guarantees
  -- tmHeight == tmAscent + tmDescent, and tmInternalLeading is the part of the
  -- ascent ABOVE the em, so the ascent can never be smaller than it.  The
  -- H.say lines are the measurement -- record the numbers here rather than
  -- writing another probe once somebody wants them.
  for _, face in ipairs {"Courier New", "Arial", "Consolas", "Lucida Console"} do
    for _, pts in ipairs {8, 10, 12, 24} do
      if load (face, pts) == 0 then
        local h, asc, desc, il = info (1), info (2), info (3), info (4)
        H.eq (asc + desc, h,
              "tmAscent + tmDescent == tmHeight for " .. face .. " " .. pts)
        H.ok (asc >= il,
              "tmAscent covers tmInternalLeading for " .. face .. " " .. pts,
              "ascent " .. asc .. " < internalLeading " .. il)
        -- The REALIZED face, not the requested one: this machine may not have
        -- Consolas or Lucida Console, and GDI substitutes silently.  Recording
        -- Arial's split under the label "Consolas" is exactly the mistake
        -- section 9c caught the first pass making.
        H.say ("SPLIT ", face, " -> ", info (21), " ", pts,
               "pt: height=", h, " ascent=", asc, " descent=", desc,
               " internalLeading=", il, " em=", h - il,
               " descent/height=", desc / h)
      end
    end
  end

  -- 3. Text measurement agrees with the metrics on a monospaced face and does
  --    not on a proportional one -- which is what proves TextWidth is really
  --    measuring rather than multiplying.
  H.eq (load ("Courier New", 10), 0, "WindowFont Courier New for widths")
  local ave = info (6)
  H.eq (world.WindowTextWidth ("fp", "f", string.rep ("M", 10)), ave * 10,
        "ten M's are ten average widths on a monospaced face")
  H.eq (load ("Arial", 10), 0, "WindowFont Arial for widths")
  H.neq (world.WindowTextWidth ("fp", "f", string.rep ("M", 10)), info (6) * 10,
         "ten M's are NOT ten average widths on a proportional face")

  world.WindowDelete ("fp")
 end)
end
