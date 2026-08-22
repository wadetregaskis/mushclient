-- The five miniwindow image operations CLAUDE.md lists as having no assertions
-- anywhere: WindowMergeImageAlpha's mask variant, BlendImage, Filter,
-- TransformImage and ImageOp.  Driven over known pixels against REAL GDI and
-- read back with WindowGetPixel, so "returned eOK and did nothing" fails.
--
-- COLORREF is 0x00BBGGRR throughout.  Reference values and the reasoning are
-- docs/WINDOWS-VALIDATION.md section 10ax.

local H = dofile (assert (MC_PROBE_DIR) .. "/harness.lua")

local RED, BLUE, GREEN, WHITE, BLACK = 0x0000FF, 0xFF0000, 0x00FF00, 0xFFFFFF, 0x000000

function probeMain ()
 H.run ("imageops", function ()

  local function px (w, x, y) return world.WindowGetPixel (w, x, y) end

  -- Source: pure red left half, pure blue right half.
  world.WindowCreate ("src", 0, 0, 40, 20, 0, 0, BLACK)
  world.WindowRectOp ("src", 2,  0, 0, 20, 20, RED)
  world.WindowRectOp ("src", 2, 20, 0, 40, 20, BLUE)
  H.eqhex (px ("src", 5, 5),  RED,  "RectOp fills the left half red")
  H.eqhex (px ("src", 25, 5), BLUE, "RectOp fills the right half blue")

  world.WindowCreate ("dst", 0, 0, 40, 20, 0, 0, WHITE)
  H.eq (world.WindowImageFromWindow ("dst", "img", "src"), 0, "ImageFromWindow returns eOK")

  -- DrawImage is the control: this one already has coverage elsewhere.
  H.eq (world.WindowDrawImage ("dst", "img", 0, 0, 0, 0, 1, 0, 0, 0, 0), 0, "DrawImage returns eOK")
  H.eqhex (px ("dst", 5, 5),  RED,  "DrawImage copies the red half exactly")
  H.eqhex (px ("dst", 25, 5), BLUE, "DrawImage copies the blue half exactly")

  -- BlendImage at 0.5 over white lands exactly half way on every channel.
  world.WindowRectOp ("dst", 2, 0, 0, 40, 20, WHITE)
  H.eq (world.WindowBlendImage ("dst", "img", 0, 0, 0, 0, 1, 0.5, 0, 0, 0, 0), 0,
        "BlendImage returns eOK")
  H.eqhex (px ("dst", 5, 5),  0x8080FF, "BlendImage 0.5 of red over white")
  H.eqhex (px ("dst", 25, 5), 0xFF8080, "BlendImage 0.5 of blue over white")

  -- TransformImage: identity, then a half scale that must move the halves.
  world.WindowRectOp ("dst", 2, 0, 0, 40, 20, WHITE)
  H.eq (world.WindowTransformImage ("dst", "img", 0, 0, 1, 1, 0, 0, 1), 0,
        "TransformImage identity returns eOK")
  H.eqhex (px ("dst", 25, 5), BLUE, "identity leaves the blue half at x=25")

  world.WindowRectOp ("dst", 2, 0, 0, 40, 20, WHITE)
  H.eq (world.WindowTransformImage ("dst", "img", 0, 0, 1, 0.5, 0, 0, 0.5), 0,
        "TransformImage 0.5 returns eOK")
  -- Halved, the 40 px image spans x 0-20: red 0-10, blue 10-20.  x=15 is the
  -- assertion that matters -- at full scale it would still be red.
  H.eqhex (px ("dst", 5, 5),  RED,  "half scale keeps red at x=5")
  H.eqhex (px ("dst", 15, 5), BLUE, "half scale moves blue to x=15")

  -- ImageOp brushes the image inside its rectangle and nowhere else.
  world.WindowRectOp ("dst", 2, 0, 0, 40, 20, WHITE)
  H.eq (world.WindowImageOp ("dst", 1, 0, 0, 20, 20, BLACK, 0, 1, WHITE, "img", 0, 0), 0,
        "ImageOp returns eOK")
  H.eqhex (px ("dst", 5, 5),  RED,   "ImageOp brushes the image inside the rect")
  H.eqhex (px ("dst", 25, 5), WHITE, "ImageOp leaves outside the rect alone")

  -- MergeImageAlpha's MASK variant: black mask = transparent, white = opaque.
  -- The green ground is what proves transparency; without it a failure to draw
  -- and a correct transparent result look the same.
  world.WindowCreate ("msk", 0, 0, 40, 20, 0, 0, BLACK)
  world.WindowRectOp ("msk", 2, 20, 0, 40, 20, WHITE)
  H.eq (world.WindowImageFromWindow ("dst", "mask", "msk"), 0, "mask ImageFromWindow returns eOK")
  world.WindowRectOp ("dst", 2, 0, 0, 40, 20, GREEN)
  H.eq (world.WindowMergeImageAlpha ("dst", "img", "mask", 0, 0, 0, 0, 1, 1.0, 0, 0, 0, 0), 0,
        "MergeImageAlpha returns eOK")
  H.eqhex (px ("dst", 5, 5),  GREEN, "black mask leaves the ground showing")
  H.eqhex (px ("dst", 25, 5), BLUE,  "white mask lets the image through")

  -- Filter.  Options is a STRENGTH for these...
  world.WindowRectOp ("dst", 2, 0, 0, 40, 20, 0x808080)
  H.eq (world.WindowFilter ("dst", 0, 0, 40, 20, 9, 0.5), 0, "Filter gamma returns eOK")
  H.eqhex (px ("dst", 10, 10), 0xB4B4B4, "gamma 0.5 lightens mid-grey")
  world.WindowRectOp ("dst", 2, 0, 0, 40, 20, 0x808080)
  world.WindowFilter ("dst", 0, 0, 40, 20, 9, 2.0)
  H.eqhex (px ("dst", 10, 10), 0x404040, "gamma 2.0 darkens mid-grey")

  -- ...and the per-channel gammas confirm the channel-to-index mapping that
  -- miniwindow.cpp asserts in comments only: 2 = red, 1 = green, 0 = blue.
  for _, t in ipairs {{12, 0x8080B4, "red"}, {15, 0x80B480, "green"}, {18, 0xB48080, "blue"}} do
    world.WindowRectOp ("dst", 2, 0, 0, 40, 20, 0x808080)
    world.WindowFilter ("dst", 0, 0, 40, 20, t [1], 0.5)
    H.eqhex (px ("dst", 10, 10), t [2], "ColourGamma moves only " .. t [3])
  end

  -- ...but a DIRECTION SELECTOR for the convolutions: GeneralFilter guards its
  -- horizontal pass with `if (Options != 2)` and its vertical with
  -- `if (Options != 1)`.  Reading it as a strength once made blur look inert.
  -- The edge here runs VERTICALLY down x=30, so a horizontal pass sees it and
  -- a vertical-only pass correctly does not.  Both halves are asserted, which
  -- is what makes this a test of the selector rather than of blur.
  local function blurEdge (opt)
    world.WindowRectOp ("dst", 2, 0, 0, 40, 20, 0x808080)
    world.WindowRectOp ("dst", 2, 30, 0, 40, 20, BLACK)
    world.WindowFilter ("dst", 0, 0, 40, 20, 3, opt)
    return px ("dst", 29, 10)
  end
  H.neq   (blurEdge (0.5), 0x808080, "blur both-directions changes a vertical edge")
  H.eqhex (blurEdge (2.0), 0x808080, "blur vertical-only leaves a vertical edge alone")

  world.WindowDelete ("src") world.WindowDelete ("dst") world.WindowDelete ("msk")
 end)
end
