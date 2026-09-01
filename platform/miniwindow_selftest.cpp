// platform/miniwindow_selftest.cpp — in-app conformance test for the REAL
// world.Window* engine (--pal-miniwindow-selftest).
//
// Drives the actual CMUSHclientDoc::Window* method bodies
// (scripting/methods/methods_miniwindows.cpp + miniwindow.cpp) through the
// actual installed pal::ISurface bridge, with pixel probes and error-code
// pins — the first automated coverage of the miniwindow subsystem (it was
// previously verifiable only by live GUI plugins).  Runs before the GUI
// boots and exits, like the surface/transport selftests; registered with
// ctest (Phase 4's first slice — see docs/PAL.md).
//
// Colour values are 0x00BBGGRR (COLORREF), as the world API takes them.

#ifndef _WIN32

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "MUSHview.h"          // MXP_FIRST_MENU (the WindowMenu item id base)
#include "miniwindow.h"           // CMiniWindow, MouseMoveMiniWindow, MINIWINDOW_*
#include "scripting/errors.h"
#include "tests/fake_worldview.h" // the recording pal::IWorldView (header-only)
#include "platform/wx_surface.h"  // WxSurfaceFontMetricsPassesForTesting

#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

int g_mwPass = 0, g_mwFail = 0;
// Accumulator for the timing probe below, so the optimiser cannot delete the
// reads it is measuring.
volatile long g_mwSink = 0;
void mwCheck(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_mwPass : g_mwFail)++;
}

const char* const W = "selftest_mw";

const long kRed   = 0x0000FF;   // BGR
const long kGreen = 0x00FF00;
const long kBlue  = 0xFF0000;
const long kWhite = 0xFFFFFF;
const long kBlack = 0x000000;

} // namespace

int RunPalMiniWindowSelfTest()
{
    printf("PAL-MINIWINDOW-SELFTEST: driving the real world.Window* engine\n");

    CMUSHclientDoc* doc = new CMUSHclientDoc();

    // ── creation + error pins ──────────────────────────────────────────────
    mwCheck(doc->WindowCreate("", 0, 0, 10, 10, 0, 0, kBlack) == eNoNameSpecified,
            "WindowCreate refuses an empty name");
    mwCheck(doc->WindowCreate(W, 0, 0, 64, 48, 0, 0, kBlue) == eOK,
            "WindowCreate 64x48 succeeds");
    mwCheck(doc->WindowRectOp("nosuch", 2, 0, 0, 1, 1, kRed, 0) == eNoSuchWindow,
            "ops on an unknown window return eNoSuchWindow");

    // background fill happened at create: probe a pixel
    mwCheck(doc->WindowGetPixel(W, 5, 5) == kBlue,
            "create's background fill is readable (WindowGetPixel)");
    // -1, not 0xFFFFFFFF: Windows returns CLR_INVALID through a 32-bit long,
    // so that is what a script sees there, and it is what WindowGetPixel is
    // documented to return for a pixel outside the window.  This used to assert
    // the LP64 widening (4294967295) as though it were the contract.
    mwCheck(doc->WindowGetPixel(W, 9999, 5) == -1L,
            "WindowGetPixel outside the window returns -1");

    // ── CircleOp action 3 = RoundRect: right/bottom-EXCLUSIVE like Win32 ───
    // Win32 RoundRect(x1,y1,x2,y2,...) occupies x1..x2-1.  The shim's polygon
    // branch used to put its extreme points at x2/y2, one pixel past GDI's last
    // pixel, while its own degenerate branch and Rectangle/Ellipse beside it were
    // exclusive.  Probe both sides of the right edge on the vertical midline,
    // where the corner rounding does not reach.
    {
        const long L = 2, T = 20, R = 22, B = 40;      // 20x20, midline y = 30
        mwCheck(doc->WindowCircleOp(W, 3, L, T, R, B,
                                    kGreen, 0, 1, kGreen, 0 /*solid*/,
                                    6, 6, 0, 0) == eOK,
                "WindowCircleOp (round rectangle) succeeds");
        mwCheck(doc->WindowGetPixel(W, R - 1, 30) == kGreen,
                "RoundRect fills up to x2-1 (Win32 is right-exclusive)");
        mwCheck(doc->WindowGetPixel(W, R, 30) != kGreen,
                "...and NOT x2 itself");
        mwCheck(doc->WindowGetPixel(W, 12, B - 1) == kGreen,
                "RoundRect fills up to y2-1 (bottom-exclusive too)");
        mwCheck(doc->WindowGetPixel(W, 12, B) != kGreen,
                "...and NOT y2 itself");
    }

    // ── CircleOp action 4 = Chord: a chord, not a pie ──────────────────────
    // Win32 bounds the figure with the arc plus the straight line joining its
    // endpoints.  wxDC::DrawEllipticArc draws the PIE when the brush is opaque,
    // so the shim used to fill two radii back to the centre.  A shallow chord
    // therefore fills near its arc but must leave the ellipse CENTRE alone --
    // which is exactly what a pie would fill.
    {
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlack, 0) == eOK,
                "clear before the chord probe");
        const long L = 4, T = 4, R = 44, B = 44;       // centre (24, 24)
        // Order matters: the arc runs ANTICLOCKWISE from the start point to the
        // end point, so (40,8) -> (8,8) is the short cap over the top.  Giving
        // them the other way round sweeps 270 degrees the long way and the
        // centre is legitimately inside the figure.
        mwCheck(doc->WindowCircleOp(W, 4, L, T, R, B,
                                    kRed, 0, 1, kRed, 0 /*solid*/,
                                    40, 8, 8, 8) == eOK,
                "WindowCircleOp (chord) succeeds");
        // The endpoints given are projected onto the ellipse radially, so the
        // real arc runs (38.1, 9.9) -> (24, 4) -> (9.9, 9.9): the cap is the
        // sliver between y=4 and y~9.9.  Probe INSIDE that sliver, and then at a
        // point that is inside the PIE's wedge but below the chord line -- the
        // ellipse centre itself is a vertex of the wedge and so sits on the
        // boundary, which makes it useless as a discriminator (it passed either
        // way).
        mwCheck(doc->WindowGetPixel(W, 24, 6) == kRed,
                "the chord fills the cap between its arc and its chord line");
        mwCheck(doc->WindowGetPixel(W, 24, 18) != kRed,
                "...and NOT below the chord line, which a pie would fill");

        // ...and the figure's EXTENT is the exclusive one, like every other
        // shape op.  Chord traces its own outline, so unlike Ellipse (which
        // gets it from wx's (x, y, w, h) form) it has to say so -- and it used
        // to use the inclusive centre (x1+x2)/2 and radius (x2-x1)/2, putting
        // its extremes on x2/y2, a pixel past GDI's last pixel and a pixel
        // outside the rectangle the caller gave.  The right HALF of the ellipse
        // (start at the bottom, end at the top, sweeping anticlockwise through
        // 3 o'clock) puts arc extremes exactly on the right and bottom edges.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlack, 0) == eOK,
                "clear before the chord extent probe");
        mwCheck(doc->WindowCircleOp(W, 4, L, T, R, B,
                                    kRed, 0, 1, kRed, 0 /*solid*/,
                                    24, 100, 24, 4) == eOK,
                "WindowCircleOp (chord, right half) succeeds");
        mwCheck(doc->WindowGetPixel(W, R - 1, 24) == kRed,
                "the chord reaches x2-1 on the midline");
        mwCheck(doc->WindowGetPixel(W, R, 24) != kRed,
                "...and NOT x2 itself (Win32's bounding rect is right-exclusive)");
        mwCheck(doc->WindowGetPixel(W, 24, B - 1) == kRed,
                "...reaches y2-1 at the bottom of the arc");
        mwCheck(doc->WindowGetPixel(W, 24, B) != kRed,
                "...and NOT y2 itself (bottom-exclusive too)");

        // Leave the canvas as we found it for the assertions that follow.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the chord probe");
    }

    // ── CircleOp action 5 = Pie: the radials stop ON the ellipse ──────────
    // Win32 treats Extra1..4 as DIRECTION points defining two rays from the
    // ellipse centre; the wedge's sides run centre -> ellipse and the points
    // themselves are never stroked to, which is why the API invites putting
    // them well outside the rectangle.  The shim used to draw wx's elliptic-arc
    // pie and then stroke two literal lines to the raw points, so each ray
    // point outside the ellipse left a pen-coloured spike running across the
    // miniwindow.  Rays straight down and straight right, both far outside, so
    // the wedge is the bottom-right quadrant.
    {
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the pie probe");
        mwCheck(doc->WindowCircleOp(W, 5, 4, 4, 44, 44,
                                    kGreen, 0 /*PS_SOLID*/, 1,
                                    kRed, 0 /*solid*/,
                                    24, 100, 100, 24) == eOK,
                "WindowCircleOp (pie, rays far outside the ellipse) succeeds");
        mwCheck(doc->WindowGetPixel(W, 32, 32) == kRed,
                "the wedge itself is filled");
        mwCheck(doc->WindowGetPixel(W, 50, 24) == kBlue,
                "nothing is painted along the ray BEYOND the ellipse");
        mwCheck(doc->WindowGetPixel(W, 24, 46) == kBlue,
                "...nor along the other one");

        // With a NULL brush wx's DrawEllipticArc strokes only the arc, so the
        // radii came SOLELY from those two mis-aimed lines: the figure was an
        // arc plus two spikes and no radii at all.  Windows strokes arc + two
        // correct radii here.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the null-brush pie probe");
        mwCheck(doc->WindowCircleOp(W, 5, 4, 4, 44, 44,
                                    kGreen, 0 /*PS_SOLID*/, 1,
                                    kRed, 1 /*BS_NULL*/,
                                    24, 100, 100, 24) == eOK,
                "WindowCircleOp (pie, null brush) succeeds");
        mwCheck(doc->WindowGetPixel(W, 35, 24) != kBlue,
                "the horizontal radius IS stroked with no brush to close it");
        mwCheck(doc->WindowGetPixel(W, 24, 35) != kBlue,
                "...and the vertical one");
        mwCheck(doc->WindowGetPixel(W, 50, 24) == kBlue,
                "...and each stops at the ellipse, not at the caller's point");
        mwCheck(doc->WindowGetPixel(W, 32, 32) == kBlue,
                "...and a null brush fills nothing");

        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the pie probe");
    }

    // ── CircleOp brush style 8-12 = the built-in mono patterns ────────────
    // GDI colours a 1-bpp pattern brush from the DC (0 bits -> text colour,
    // 1 bits -> background colour).  wx ignores those and paints the stipple
    // black/white, so these came out monochrome whatever the plugin asked for.
    // "Fine" (style 8) is a sparse pattern, so a filled area contains BOTH
    // colours -- scan a row and assert neither black nor white appears.
    {
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the mono-pattern probe");
        mwCheck(doc->WindowCircleOp(W, 2, 4, 4, 40, 40,
                                    kGreen, 5 /*PS_NULL: no outline*/, 1,
                                    kRed, 8 /*mono pattern "fine"*/,
                                    0, 0, 0, 0) == eOK,
                "WindowCircleOp (mono pattern brush) succeeds");
        int sawRed = 0, sawGreen = 0, sawMono = 0;
        for (long x = 10; x < 34; ++x)
            for (long y = 10; y < 34; ++y) {
                const long c = doc->WindowGetPixel(W, x, y);
                if      (c == kRed)   sawRed++;
                else if (c == kGreen) sawGreen++;
                else if (c == 0x000000 || c == 0xFFFFFF) sawMono++;
            }
        // The bug this fixes: the stipple used to be painted literally
        // black/white because wx's CreatePatternBrush ignores the DC's
        // text/background colours.  That is what is asserted here.
        mwCheck(sawMono == 0, "mono pattern never falls back to black/white");
        mwCheck(sawRed > 0 || sawGreen > 0,
                "...and paints in the colours the caller asked for");

        // kFine is a 50/50 checkerboard (0xAA/0x55), so BOTH colours must
        // appear.  They did not until the tile was built at PHYSICAL
        // resolution: an 8x8 logical stipple on a 2x canvas is scaled up by wx
        // and its alternating pixels average away to one flat colour.
        mwCheck(sawRed > 0 && sawGreen > 0,
                "a checkerboard pattern keeps BOTH colours (not averaged away)");
        // ...and the TRANSPARENT request is keyed on BrushColour, not
        // PenColour.  For a mono pattern the engine puts BrushColour in
        // spec.fg (SetTextColor) and PenColour in spec.bg (SetBkColor) -- the
        // reverse of the hatch arm -- and upstream keys SetBkMode on
        // BrushColour.  The adapter tested spec.bg, which broke BOTH inputs
        // that carry a -1, in opposite ways.  Probed here in both directions;
        // neither is reachable from the headless suite, because pal::CpuRaster
        // drops BrushSpec::monoPattern entirely.

        // (i) PenColour == -1 is an ordinary opaque background on Windows, and
        // must NOT collapse the tile: it used to substitute BrushColour for it
        // and paint a single flat block where Windows draws two tones.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the -1 PenColour pattern probe");
        mwCheck(doc->WindowCircleOp(W, 2, 4, 4, 40, 40,
                                    -1 /*PenColour*/, 5 /*PS_NULL*/, 1,
                                    kRed /*BrushColour*/, 8 /*mono "fine"*/,
                                    0, 0, 0, 0) == eOK,
                "WindowCircleOp (mono pattern, PenColour -1) succeeds");
        {
            int sawRed = 0, sawWhite = 0, sawBlue = 0;
            for (long x = 10; x < 34; ++x)
                for (long y = 10; y < 34; ++y) {
                    const long c = doc->WindowGetPixel(W, x, y);
                    if      (c == kRed)   sawRed++;
                    else if (c == kWhite) sawWhite++;
                    else if (c == kBlue)  sawBlue++;
                }
            mwCheck(sawRed > 0 && sawWhite > 0,
                    "a -1 background resolves to a colour, keeping BOTH tones");
            mwCheck(sawBlue == 0, "...and the pattern really did cover the area");
        }

        // (ii) BrushColour == -1 IS the transparent request.  A tiled brush has
        // no mask, so the port cannot leave the destination alone -- the
        // documented fallback is a flat tile in the resolved foreground.  What
        // must NOT happen, and did, is the 1-bits being painted in PenColour:
        // that obliterates exactly the content the plugin asked to show
        // through.  (The eventual correct answer is that the underlying blue
        // survives on the 1-bits; that needs the mask/composite work, so this
        // assertion is deliberately the weaker fallback.)
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the transparent-gap pattern probe");
        mwCheck(doc->WindowCircleOp(W, 2, 4, 4, 40, 40,
                                    kRed /*PenColour*/, 5 /*PS_NULL*/, 1,
                                    -1 /*BrushColour*/, 8 /*mono "fine"*/,
                                    0, 0, 0, 0) == eOK,
                "WindowCircleOp (mono pattern, BrushColour -1) succeeds");
        {
            int sawRed = 0;
            for (long x = 10; x < 34; ++x)
                for (long y = 10; y < 34; ++y)
                    if (doc->WindowGetPixel(W, x, y) == kRed) sawRed++;
            mwCheck(sawRed == 0,
                    "a transparent request never paints PenColour into the gaps");
        }

        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the mono-pattern probe");
    }

    // ── ImageOp's arbitrary-image pattern brush, at physical resolution ────
    // The same defect one arm over: the tile for a LOADED image was built at
    // 1x with no scale factor, so on a Retina canvas wx scaled it up and
    // interpolated it and a fine-detail pattern averaged toward a flat colour.
    // 1px alternating columns are the worst case, as above.
    {
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlack, 0) == eOK,
                "clear before the image-pattern probe");
        for (long x = 0; x < 64; ++x)
            doc->WindowRectOp(W, 2, x, 0, x + 1, 48,
                              (x % 2) ? kRed : kGreen, 0);
        mwCheck(doc->WindowImageFromWindow(W, "patImg", W) == eOK,
                "WindowImageFromWindow captures the striped canvas as an image");
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before filling with the image brush");
        mwCheck(doc->WindowImageOp(W, 2 /*rectangle*/, 4, 4, 40, 40,
                                   kWhite, 5 /*PS_NULL: no outline*/, 1,
                                   kWhite, "patImg", 0, 0) == eOK,
                "WindowImageOp (rectangle, image pattern brush) succeeds");
        int sawRed = 0, sawGreen = 0;
        for (long x = 10; x < 34; ++x)
            for (long y = 10; y < 34; ++y) {
                const long c = doc->WindowGetPixel(W, x, y);
                if      (c == kRed)   sawRed++;
                else if (c == kGreen) sawGreen++;
            }
        mwCheck(sawRed > 0 && sawGreen > 0,
                "an image brush keeps BOTH colours of a 1px checkerboard");
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the image-pattern probe");
    }

    // ── RectOp: solid fill (action 2) ──────────────────────────────────────
    mwCheck(doc->WindowRectOp(W, 2, 10, 10, 20, 20, kRed, 0) == eOK,
            "WindowRectOp fill succeeds");
    mwCheck(doc->WindowGetPixel(W, 15, 15) == kRed, "fill interior is the fill colour");
    mwCheck(doc->WindowGetPixel(W, 25, 15) == kBlue, "outside the fill is untouched");
    // GDI rectangles are right/bottom-exclusive
    mwCheck(doc->WindowGetPixel(W, 20, 15) == kBlue, "fill right edge is exclusive");

    // ── Line (solid pen, width 1) ──────────────────────────────────────────
    mwCheck(doc->WindowLine(W, 0, 40, 63, 40, kGreen, 0 /*PS_SOLID*/, 1) == eOK,
            "WindowLine succeeds");
    mwCheck(doc->WindowGetPixel(W, 30, 40) == kGreen, "line pixel present mid-span");
    // the shim draws lines endpoint-INCLUSIVE (documented deviation from
    // GDI's LineTo exclusion — wx_surface.cpp line())
    mwCheck(doc->WindowGetPixel(W, 63, 40) == kGreen,
            "line endpoint drawn (port semantics: inclusive)");

    // ── CircleOp: filled rectangle-shape (action 2 = rectangle) ───────────
    mwCheck(doc->WindowCircleOp(W, 2, 30, 2, 40, 8,
                                kWhite, 0, 1, kGreen, 0 /*solid brush*/,
                                0, 0, 0, 0) == eOK,
            "WindowCircleOp (rectangle, solid brush) succeeds");
    mwCheck(doc->WindowGetPixel(W, 35, 5) == kGreen, "shape interior is brush colour");
    // wxOSX antialiases stroked edges (the S2 finding that drove the hatch
    // reimpls), so the outline pixel is a blend — assert only that the pen
    // visibly drew (the corner is neither pure background nor pure brush)
    {
        long edge = doc->WindowGetPixel(W, 30, 2);
        mwCheck(edge != kBlue && edge != kGreen,
                "shape outline drawn (blend of pen over edge; wxOSX AA)");
    }

    // ── CircleOp rectangle with a NULL pen: GDI's null-pen shrink ─────────
    // With NULL_PEN selected, ::Rectangle draws the figure one pixel narrower
    // and shorter -- the right column and bottom row are omitted.  docs/PAL.md
    // calls that normative and pal::CpuRaster implements it, pinned by
    // tests/test_pal_surface.cpp -- but the shipping wx adapter filled the full
    // w x h, so a headless golden and the app answered the same script
    // differently.  Same probe here, so both adapters are pinned by one rule.
    // A 3x2 rect makes the shrunk column and row a majority of the figure.
    {
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the null-pen shrink probe");
        mwCheck(doc->WindowCircleOp(W, 2, 50, 30, 53, 32,
                                    kWhite, 5 /*PS_NULL*/, 1,
                                    kRed, 0 /*solid*/, 0, 0, 0, 0) == eOK,
                "WindowCircleOp (rectangle, null pen) succeeds");
        mwCheck(doc->WindowGetPixel(W, 50, 30) == kRed &&
                doc->WindowGetPixel(W, 51, 30) == kRed,
                "a null-pen rectangle still fills (w-1) x (h-1)");
        mwCheck(doc->WindowGetPixel(W, 52, 30) == kBlue,
                "...but not its right column (GDI's null-pen shrink)");
        mwCheck(doc->WindowGetPixel(W, 50, 31) == kBlue &&
                doc->WindowGetPixel(W, 52, 31) == kBlue,
                "...nor its bottom row");

        // Control: the same figure with a real pen must still reach the last
        // column and row, so the shrink is keyed on the pen being transparent
        // and is not just "every rectangle got smaller".
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the stroked-rectangle control");
        mwCheck(doc->WindowCircleOp(W, 2, 50, 30, 53, 32,
                                    kWhite, 0 /*PS_SOLID*/, 1,
                                    kRed, 0 /*solid*/, 0, 0, 0, 0) == eOK,
                "WindowCircleOp (rectangle, solid pen) succeeds");
        mwCheck(doc->WindowGetPixel(W, 52, 30) != kBlue &&
                doc->WindowGetPixel(W, 52, 31) != kBlue,
                "a STROKED rectangle still reaches x2-1/y2-1");

        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the null-pen shrink probe");
    }

    // ── Gradient: horizontal red→blue (mode 1) ─────────────────────────────
    mwCheck(doc->WindowGradient(W, 0, 44, 64, 48, kRed, kBlue, 1) == eOK,
            "WindowGradient (horizontal) succeeds");
    {
        long l = doc->WindowGetPixel(W, 1, 46);
        long r = doc->WindowGetPixel(W, 62, 46);
        mwCheck((l & 0xFF) > 0xC0 && (l >> 16) < 0x40,
                "gradient left end is red-dominant");
        mwCheck((r >> 16) > 0xC0 && (r & 0xFF) < 0x40,
                "gradient right end is blue-dominant");
    }

    // ── fonts + text ───────────────────────────────────────────────────────
    mwCheck(doc->WindowFont(W, "f1", "Courier New", 10, FALSE, FALSE, FALSE,
                            FALSE, 0, 0) == eOK,
            "WindowFont creates a font");
    mwCheck(doc->WindowText(W, "nofont", "x", 0, 0, 0, 0, kWhite, FALSE) == -2,
            "WindowText with an unknown font id returns -2");
    // ── D-21: a 10pt font is a WINDOWS-sized font, in canvas pixels ────────
    // A miniwindow is a pixel canvas and the plugin's layout constants are
    // canvas pixels, so parity means a 10pt request occupies the pixels it
    // occupies on Windows: MulDiv(10, 96, 72) = 13 for the em.  The engine now
    // uses the plain Windows formula (MulDiv(points, dpi(), 72)) and the wx
    // adapter converts for its non-96 display PPI; the headless CpuRaster twin
    // pins the same number exactly (tests/test_lua_surface.cpp), which is the
    // point -- both adapters have to land on Windows' figure.
    //
    // Bands, not equalities, because these are real measured face metrics.
    // Both ends have teeth; measured here (D-21, Courier New, 144-PPI panel):
    //   correct                                    -> tmHeight 15, 10xM 79
    //   the old (96/72)^2 factor above the seam    -> tmHeight 20, 10xM 103
    //   adapter compensation dropped               -> tmHeight 12, 10xM 61
    // tmHeight includes leading so it sits above the em; the 10xM bound is on
    // the ADVANCE, which for a monospace face is a fixed fraction of the em
    // (Courier New 0.6), so 55-68% brackets the correct 79 while excluding both
    // failures.  Pinned in em terms rather than as literals so the relation,
    // not this machine's numbers, is what is being asserted.
    {
        const long kEm = MulDiv(10, 96, 72);     // 13 -- Windows' pixel em
        VARIANT vh = doc->WindowFontInfo(W, "f1", 1);        // tmHeight
        long w10 = doc->WindowTextWidth(W, "f1", "MMMMMMMMMM", FALSE);
        mwCheck(vh.vt == VT_I4 && vh.lVal >= kEm && vh.lVal <= kEm + 3,
                "a 10pt miniwindow font is Windows-sized (tmHeight straddles the em)");
        mwCheck(w10 >= kEm * 10 * 55 / 100 && w10 <= kEm * 10 * 68 / 100,
                "...and ten 'M's advance the Windows width, not 1.33x it");
    }
    {
        long w = doc->WindowText(W, "f1", "Hi", 2, 24, 0, 0, kWhite, FALSE);
        mwCheck(w > 0, "WindowText draws and returns a positive width");
        long tw = doc->WindowTextWidth(W, "f1", "Hi", FALSE);
        mwCheck(tw == w, "WindowTextWidth matches WindowText's return");
    }

    // WindowFontInfo(19) is tmPitchAndFamily.  In a TEXTMETRIC (unlike a
    // LOGFONT) bit 0 is TMPF_FIXED_PITCH and its meaning is INVERTED: SET means
    // the font is VARIABLE pitch.  The shim used to return the literal
    // FIXED_PITCH (1) for every font, which therefore said "variable" for all of
    // them, with family FF_DONTCARE.  Courier New is fixed-pitch and modern.
    {
        VARIANT vpf = doc->WindowFontInfo(W, "f1", 19);
        const long pf = (vpf.vt == VT_I4) ? vpf.lVal : -1;
        mwCheck(pf >= 0, "WindowFontInfo(19) returns a pitch/family");
        mwCheck((pf & 1) == 0,
                "a fixed-pitch font does NOT set TMPF_FIXED_PITCH (inverted flag)");
        mwCheck((pf & 0xF0) == 48 /*FF_MODERN*/,
                "...and reports a real family, not FF_DONTCARE");

        // WindowFontInfo(7) is tmMaxCharWidth, which used to be the invented
        // averageWidth * 2.  This used to assert max == average for a
        // fixed-pitch font; a Windows run (2026-08-08) showed that is FALSE
        // even there -- real GDI reports Courier New 10pt as average 8, max 10,
        // because tmAveCharWidth is about the width of 'x' while tmMaxCharWidth
        // is the widest glyph cell, and side bearings put the cell above the
        // advance.  `max >= average` is the only relation that holds on both,
        // and the shim now measures instead of assuming (wx_gdi_compat.cpp).
        //
        // `>=` ALONE would not be a regression test, though: the invented value
        // was averageWidth * 2, which is also >= averageWidth, so relaxing the
        // check to it would have let the original defect straight back in.  The
        // upper bound is what keeps the teeth.  Both measurements sit far below
        // it -- Windows 10/8 = 1.25x, and a monospace font here measures every
        // printable advance identically for 1.0x -- while the invented value is
        // exactly 2.0x, so it is the one thing this bound excludes.
        VARIANT vavg = doc->WindowFontInfo(W, "f1", 6);   // tmAveCharWidth
        VARIANT vmax = doc->WindowFontInfo(W, "f1", 7);
        if (vavg.vt == VT_I4 && vmax.vt == VT_I4 && vavg.lVal > 0)
            {
            mwCheck(vmax.lVal >= vavg.lVal,
                    "max char width is at least the average");
            mwCheck(vmax.lVal < vavg.lVal * 2,
                    "...and is measured, not the old invented average * 2");
            }
        else
            mwCheck(false, "WindowFontInfo(6)/(7) return numbers");

        // Neither check above can detect the MEASUREMENT ITSELF going away, and
        // that is not hypothetical -- it is the shape of the original defect.
        // The measuring loop seeds `widest` with fm.averageWidth and only ever
        // raises it (wx_gdi_compat.cpp), so deleting the loop leaves
        // tmMaxCharWidth == tmAveCharWidth, which satisfies BOTH `>=` and the
        // upper bound.  Measured here 2026-08-10, a monospace face gives
        // avg 11 / max 11 -- identical -- so a fixed-pitch font can never tell
        // the two apart, whatever relation is asserted over it.
        //
        // A PROPORTIONAL face can: its widest printable glyph is genuinely wider
        // than its average advance (measured 11 / 20 here), so a strict `>` goes
        // red the moment the measurement stops happening.  The pitch bit is
        // checked first so that a font substitution -- which would silently make
        // this a second monospace fixture, and the comparison an identity again
        // -- reports itself instead of masquerading as a metric failure.
        mwCheck(doc->WindowFont(W, "fprop", "Times New Roman", 12,
                                FALSE, FALSE, FALSE, FALSE, 0, 0) == eOK,
                "WindowFont creates a proportional font");
        {
            VARIANT ppf   = doc->WindowFontInfo(W, "fprop", 19);
            VARIANT pavg  = doc->WindowFontInfo(W, "fprop", 6);
            VARIANT pmax  = doc->WindowFontInfo(W, "fprop", 7);
            // bit 0 SET means variable pitch (the flag is inverted in a
            // TEXTMETRIC) -- i.e. the fixture really is the opposite of "f1"
            mwCheck(ppf.vt == VT_I4 && (ppf.lVal & 1) != 0,
                    "...that really is proportional, not a substituted monospace");
            if (pavg.vt == VT_I4 && pmax.vt == VT_I4 && pavg.lVal > 0)
                mwCheck(pmax.lVal > pavg.lVal,
                        "a proportional font's max char width EXCEEDS its average "
                        "(so the measurement is real, not a fallback)");
            else
                mwCheck(false, "WindowFontInfo(6)/(7) return numbers for it");
        }

        // ── the metrics are MEASURED ONCE per font, not once per query ─────
        // tmMaxCharWidth is filled by measuring all 95 printable ASCII glyphs,
        // each a Core Text layout; GDI's GetTextMetrics is O(1) because it
        // copies the font's cached TEXTMETRIC.  WindowFontInfo pays that sweep
        // for EVERY InfoType, not only code 7, so asking a font its height cost
        // 95 measurements -- and lua/mw.lua's popup helper issues seven such
        // calls before drawing anything.  Nothing cached it.
        //
        // Perf cannot be pinned by a clock here (flaky under Guard Malloc and
        // ASan), so pin the COUNT: repeated queries on one font must measure
        // exactly once.
        {
            const long before = pal::WxSurfaceFontMetricsPassesForTesting();
            for (int i = 0; i < 12; ++i) {
                VARIANT v = doc->WindowFontInfo(W, "f1", 1);
                g_mwSink += (v.vt == VT_I4) ? v.lVal : 0;
            }
            const long after = pal::WxSurfaceFontMetricsPassesForTesting();
            mwCheck(after - before == 0,
                    "12 more WindowFontInfo calls on a measured font measure 0 times");
        }

        // ...and the cache must be keyed on the FONT, not on the plugin's name
        // for it.  Redefining an id destroys the old entry and creates a NEW
        // one, so a name-keyed cache would hand back the previous face's
        // metrics -- and pass every assertion above.  "fprop" was proportional;
        // redefine it as a monospace and the pitch bit must flip back.
        {
            const long before = pal::WxSurfaceFontMetricsPassesForTesting();
            mwCheck(doc->WindowFont(W, "fprop", "Courier New", 12,
                                    FALSE, FALSE, FALSE, FALSE, 0, 0) == eOK,
                    "WindowFont redefines an existing font id");
            VARIANT ppf  = doc->WindowFontInfo(W, "fprop", 19);
            VARIANT pavg = doc->WindowFontInfo(W, "fprop", 6);
            VARIANT pmax = doc->WindowFontInfo(W, "fprop", 7);
            mwCheck(ppf.vt == VT_I4 && (ppf.lVal & 1) == 0,
                    "...and the redefined font reports its OWN pitch (fixed now)");
            mwCheck(pavg.vt == VT_I4 && pmax.vt == VT_I4 &&
                    pavg.lVal > 0 && pmax.lVal >= pavg.lVal,
                    "...with its own metrics, not the previous face's");
            mwCheck(pal::WxSurfaceFontMetricsPassesForTesting() - before == 1,
                    "...measured exactly once for the new font");
        }
    }

    // ── RectOp action 5 = DrawEdge: BF_ side flags are honoured ────────────
    // Win32 draws two 1px rings and strokes ONLY the sides named in the BF_
    // flags.  The shim ignored the flags entirely (calling Draw3dRect, which
    // always strokes all four) and switched on nEdge as an enum, so EDGE_ETCHED
    // and EDGE_BUMP fell through to a flat grey box.
    {
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the DrawEdge probe");
        // EDGE_RAISED (0x05), left + top only (BF_LEFT|BF_TOP = 0x03).
        mwCheck(doc->WindowRectOp(W, 5, 10, 10, 40, 40, 0x05, 0x03) == eOK,
                "WindowRectOp (DrawEdge, left+top only) succeeds");
        mwCheck(doc->WindowGetPixel(W, 10, 25) != kBlue,
                "DrawEdge strokes the LEFT side when BF_LEFT is set");
        mwCheck(doc->WindowGetPixel(W, 25, 10) != kBlue,
                "...and the TOP side when BF_TOP is set");
        mwCheck(doc->WindowGetPixel(W, 39, 25) == kBlue,
                "...and leaves the RIGHT side alone when BF_RIGHT is not");
        mwCheck(doc->WindowGetPixel(W, 25, 39) == kBlue,
                "...and the BOTTOM side likewise");

        // EdgeFlags 0 (no BF_ side bits at all): Win32 draws NOTHING.  The
        // shim used to invent "no sides named = all sides" and paint a full
        // border here.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the no-flags probe");
        mwCheck(doc->WindowRectOp(W, 5, 10, 10, 40, 40, 0x05, 0) == eOK,
                "WindowRectOp (DrawEdge, EdgeFlags 0) succeeds");
        mwCheck(doc->WindowGetPixel(W, 10, 25) == kBlue &&
                doc->WindowGetPixel(W, 25, 10) == kBlue &&
                doc->WindowGetPixel(W, 39, 25) == kBlue &&
                doc->WindowGetPixel(W, 25, 39) == kBlue,
                "...EdgeFlags 0 draws no border at all, matching Win32");

        // EDGE_ETCHED (0x06) used to hit `default:` and paint flat grey.  It is
        // sunken-outer + raised-inner, so the outer and inner rings differ.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the etched probe");
        mwCheck(doc->WindowRectOp(W, 5, 10, 10, 40, 40, 0x06, 0x0F) == eOK,
                "WindowRectOp (DrawEdge, EDGE_ETCHED) succeeds");
        mwCheck(doc->WindowGetPixel(W, 25, 10) != doc->WindowGetPixel(W, 25, 11),
                "EDGE_ETCHED's outer and inner rings differ (not a flat box)");

        // BF_MIDDLE (0x0800) fills the interior with the button-face colour.
        // It was never implemented: with BF_RECT|BF_MIDDLE the two rings
        // appeared over an untouched interior, and BF_MIDDLE on its own fell
        // into the "no sides named" early return and drew nothing at all.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the BF_MIDDLE probe");
        mwCheck(doc->WindowRectOp(W, 5, 10, 10, 40, 40, 0x05, 0x080F) == eOK,
                "WindowRectOp (DrawEdge, BF_RECT|BF_MIDDLE) succeeds");
        {
            const long middle = doc->WindowGetPixel(W, 25, 25);
            mwCheck(middle != kBlue, "BF_MIDDLE fills the interior");
            mwCheck(doc->WindowGetPixel(W, 25, 10) != middle,
                    "...and the rings are still stroked over that fill");
        }

        // BF_MIDDLE on its own: Win32 fills and draws NO border.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the fill-only probe");
        mwCheck(doc->WindowRectOp(W, 5, 10, 10, 40, 40, 0x05, 0x0800) == eOK,
                "WindowRectOp (DrawEdge, BF_MIDDLE alone) succeeds");
        {
            const long middle = doc->WindowGetPixel(W, 25, 25);
            mwCheck(middle != kBlue, "BF_MIDDLE alone still fills the interior");
            mwCheck(doc->WindowGetPixel(W, 25, 10) == middle &&
                    doc->WindowGetPixel(W, 10, 25) == middle,
                    "...and strokes no edges over it");
        }

        // ...and every ring stays INSIDE the rectangle it was given.  Win32
        // RECTs are right/bottom-exclusive, so the outer ring occupies
        // left..right-1 and ::DrawEdge never paints at right or bottom.  The
        // shim used to pass rr+1 / b+1 as the far end of each stroke to
        // compensate for GDI's endpoint-EXCLUSIVE LineTo, but this adapter's
        // line primitive INCLUDES its endpoint (the deviation the WindowLine
        // probe above pins), so each side ran a pixel too far: outside the rect
        // on the outer ring, and over the outer ring's corners on the inner one.
        // One PAIR of sides at a time, so no other stroke can cover for a
        // mis-sized one: with BF_LEFT|BF_TOP the top strip's far end is the
        // only thing that can paint (39,10), and nothing at all may paint
        // (40,10).
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the DrawEdge extent probe");
        mwCheck(doc->WindowRectOp(W, 5, 10, 10, 40, 40, 0x05, 0x03) == eOK,
                "WindowRectOp (DrawEdge, EDGE_RAISED, left+top) succeeds");
        mwCheck(doc->WindowGetPixel(W, 39, 10) != kBlue,
                "the TOP side reaches x == right-1");
        mwCheck(doc->WindowGetPixel(W, 40, 10) == kBlue,
                "...and stops there: nothing is painted at x == right");
        mwCheck(doc->WindowGetPixel(W, 10, 39) != kBlue,
                "the LEFT side reaches y == bottom-1");
        mwCheck(doc->WindowGetPixel(W, 10, 40) == kBlue,
                "...and stops there: nothing is painted at y == bottom");

        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the DrawEdge far-side extent probe");
        mwCheck(doc->WindowRectOp(W, 5, 10, 10, 40, 40, 0x05, 0x0C) == eOK,
                "WindowRectOp (DrawEdge, EDGE_RAISED, right+bottom) succeeds");
        mwCheck(doc->WindowGetPixel(W, 39, 39) != kBlue,
                "the RIGHT and BOTTOM sides reach right-1/bottom-1");
        mwCheck(doc->WindowGetPixel(W, 40, 39) == kBlue &&
                doc->WindowGetPixel(W, 39, 40) == kBlue,
                "...and neither runs past the rectangle");

        // The inner ring is inset by one on every named side, so it must not
        // reach the outer ring's right column -- the outer colour at the corner
        // must be the same as halfway down the side.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the DrawEdge corner probe");
        mwCheck(doc->WindowRectOp(W, 5, 10, 10, 40, 40, 0x05, 0x0F) == eOK,
                "WindowRectOp (DrawEdge, EDGE_RAISED, BF_RECT) succeeds");
        mwCheck(doc->WindowGetPixel(W, 39, 11) == doc->WindowGetPixel(W, 39, 25),
                "the inner ring does not overwrite the outer ring's corners");
        mwCheck(doc->WindowGetPixel(W, 11, 39) == doc->WindowGetPixel(W, 25, 39),
                "...on the bottom edge either");

        // BF_DIAGONAL (0x0010) replaces the box with a diagonal vector, and the
        // side bits then name its END POINT.  It was dropped entirely, so
        // BF_DIAGONAL_ENDTOPLEFT (0x13) was read as BF_TOP|BF_LEFT and drew two
        // sides of a box -- a completely different picture, returned as eOK.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the BF_DIAGONAL probe");
        mwCheck(doc->WindowRectOp(W, 5, 10, 10, 40, 40, 0x05, 0x13) == eOK,
                "WindowRectOp (DrawEdge, BF_DIAGONAL_ENDTOPLEFT) succeeds");
        mwCheck(doc->WindowGetPixel(W, 25, 25) != kBlue,
                "BF_DIAGONAL draws a diagonal across the rectangle");
        mwCheck(doc->WindowGetPixel(W, 25, 10) == kBlue,
                "...INSTEAD of the box: the top side is not stroked");
        mwCheck(doc->WindowGetPixel(W, 10, 25) == kBlue,
                "...nor the left side");
        mwCheck(doc->WindowGetPixel(W, 39, 25) == kBlue &&
                doc->WindowGetPixel(W, 25, 39) == kBlue,
                "...and neither of the unnamed sides either");
        // The other end point puts the vector on the ANTI-diagonal, so a probe
        // on one diagonal must miss the other -- an implementation that ignored
        // the corner bits and always drew top-left to bottom-right would pass
        // the block above and fail here.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the second BF_DIAGONAL probe");
        mwCheck(doc->WindowRectOp(W, 5, 10, 10, 40, 40, 0x05, 0x16) == eOK,
                "WindowRectOp (DrawEdge, BF_DIAGONAL_ENDTOPRIGHT) succeeds");
        mwCheck(doc->WindowGetPixel(W, 15, 34) != kBlue &&
                doc->WindowGetPixel(W, 34, 15) != kBlue,
                "ENDTOPRIGHT runs bottom-left to top-right");
        mwCheck(doc->WindowGetPixel(W, 15, 15) == kBlue &&
                doc->WindowGetPixel(W, 34, 34) == kBlue,
                "...and NOT along the other diagonal");

        // BF_MONO (0x8000) and BF_FLAT (0x4000) replace the bevel with a FLAT
        // border -- one colour the whole way round each ring.  Both were
        // dropped, so a caller asking for either got the two-tone 3D box.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the BF_MONO probe");
        mwCheck(doc->WindowRectOp(W, 5, 10, 10, 40, 40, 0x05, 0x800F) == eOK,
                "WindowRectOp (DrawEdge, BF_RECT|BF_MONO) succeeds");
        {
            const long o = doc->WindowGetPixel(W, 10, 25);
            mwCheck(o == doc->WindowGetPixel(W, 39, 25) &&
                    o == doc->WindowGetPixel(W, 25, 10) &&
                    o == doc->WindowGetPixel(W, 25, 39),
                    "BF_MONO's outer ring is ONE colour all round (no bevel)");
            mwCheck(o == kBlack, "...COLOR_WINDOWFRAME, i.e. black");
            mwCheck(doc->WindowGetPixel(W, 11, 25) == kWhite,
                    "...over an inner ring of COLOR_WINDOW, i.e. white");
        }

        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the BF_FLAT probe");
        mwCheck(doc->WindowRectOp(W, 5, 10, 10, 40, 40, 0x05, 0x400F) == eOK,
                "WindowRectOp (DrawEdge, BF_RECT|BF_FLAT) succeeds");
        {
            const long o = doc->WindowGetPixel(W, 10, 25);
            mwCheck(o == doc->WindowGetPixel(W, 39, 25) &&
                    o == doc->WindowGetPixel(W, 25, 10) &&
                    o == doc->WindowGetPixel(W, 25, 39),
                    "BF_FLAT's outer ring is ONE colour all round (no bevel)");
            mwCheck(o == 0x808080L, "...COLOR_3DSHADOW");
            mwCheck(doc->WindowGetPixel(W, 11, 25) == 0xC0C0C0L,
                    "...over an inner ring of COLOR_3DFACE");
            mwCheck(o != kBlack,
                    "...and BF_FLAT is not BF_MONO (different colour tables)");
        }

        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the DrawEdge probe");
    }

    // ── a wide pen's endcap is the one the caller asked for ───────────────
    // WindowLine / WindowPolygon / WindowCircleOp accept and validate the
    // PS_ENDCAP_* / PS_JOIN_* bits, but the adapter passed only the STYLE word
    // to CPen::CreatePen and PS_ENDCAP_ROUND is 0, so every wide pen came out
    // round-capped.  A projecting (SQUARE) cap extends half the pen width past
    // the endpoint; a FLAT one stops dead there.
    {
        const long kEnd = 40, kProbe = 43;   // 3 past the endpoint, half-width 4
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the flat-endcap probe");
        mwCheck(doc->WindowLine(W, 10, 24, kEnd, 24, kGreen,
                                0x0200 /*PS_SOLID|PS_ENDCAP_FLAT*/, 9) == eOK,
                "WindowLine (width 9, flat endcap) succeeds");
        mwCheck(doc->WindowGetPixel(W, 25, 24) == kGreen,
                "the wide line drew along its span");
        mwCheck(doc->WindowGetPixel(W, kProbe, 24) == kBlue,
                "a FLAT endcap stops at the endpoint");

        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the square-endcap probe");
        mwCheck(doc->WindowLine(W, 10, 24, kEnd, 24, kGreen,
                                0x0100 /*PS_SOLID|PS_ENDCAP_SQUARE*/, 9) == eOK,
                "WindowLine (width 9, square endcap) succeeds");
        mwCheck(doc->WindowGetPixel(W, 25, 24) == kGreen,
                "the wide line drew along its span");
        mwCheck(doc->WindowGetPixel(W, kProbe, 24) == kGreen,
                "...and a SQUARE endcap projects past it");

        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the endcap probe");
    }

    // ── PS_INSIDEFRAME keeps the whole pen inside the bounding rectangle ──
    // Win32's PS_INSIDEFRAME shrinks the figure so the entire pen width fits
    // within the rectangle; nothing is drawn outside it.  Real GDI does that in
    // CreatePen, so the Windows adapter gets it free -- but the shim maps the
    // style to an ordinary solid pen and wx strokes centred on the path, so a
    // wide PS_INSIDEFRAME pen rendered exactly like PS_SOLID and overflowed by
    // half its width on every side.  A 9-wide pen on a rect starting at x = 10
    // therefore reached x = 6 instead of stopping at 10.
    {
        const long L = 10, T = 10, R = 40, B = 40;
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the PS_INSIDEFRAME probe");
        mwCheck(doc->WindowCircleOp(W, 2, L, T, R, B,
                                    kGreen, 6 /*PS_INSIDEFRAME*/, 9,
                                    kBlue, 1 /*BS_NULL*/, 0, 0, 0, 0) == eOK,
                "WindowCircleOp (rectangle, PS_INSIDEFRAME width 9) succeeds");
        mwCheck(doc->WindowGetPixel(W, 12, 25) == kGreen,
                "the border is drawn INSIDE the rectangle");
        mwCheck(doc->WindowGetPixel(W, 7, 25) == kBlue,
                "...and no ink lands outside it");
        mwCheck(doc->WindowGetPixel(W, 25, 7) == kBlue,
                "...on the top edge either");

        // Control, so "the code stopped drawing" cannot pass: the SAME figure
        // with an ordinary solid pen really does overflow, which is what
        // PS_INSIDEFRAME exists to prevent.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the PS_SOLID overflow control");
        mwCheck(doc->WindowCircleOp(W, 2, L, T, R, B,
                                    kGreen, 0 /*PS_SOLID*/, 9,
                                    kBlue, 1 /*BS_NULL*/, 0, 0, 0, 0) == eOK,
                "WindowCircleOp (rectangle, PS_SOLID width 9) succeeds");
        mwCheck(doc->WindowGetPixel(W, 7, 25) == kGreen,
                "a centred wide pen DOES overflow the rectangle");

        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the PS_INSIDEFRAME probe");
    }

    // ── read-modify-write is lossless on a HiDPI canvas ───────────────────
    // WindowFilter reads the region, edits the bytes and writes them back.
    // Both halves used to go through an UNSCALED 1x DIB, so on a Retina canvas
    // the read averaged each 2x2 physical block down and the write scaled the
    // result back up -- adjacent single-pixel columns of different colours
    // bled into each other.  Operation 7 is additive brightness, so an amount
    // of 0 is an identity: whatever comes out must be exactly what went in.
    {
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlack, 0) == eOK,
                "clear before the filter round-trip");
        // Alternating 1px columns: the worst case for any resampling.
        for (long x = 10; x < 20; ++x)
            doc->WindowRectOp(W, 2, x, 10, x + 1, 20,
                              (x % 2) ? kRed : kGreen, 0);
        const long before11 = doc->WindowGetPixel(W, 11, 15);
        const long before12 = doc->WindowGetPixel(W, 12, 15);
        mwCheck(before11 != before12,
                "adjacent 1px columns really are different colours to start");

        mwCheck(doc->WindowFilter(W, 8, 8, 24, 24, 7 /*brightness*/, 0.0) == eOK,
                "WindowFilter (identity brightness) succeeds");

        mwCheck(doc->WindowGetPixel(W, 11, 15) == before11,
                "a read-modify-write leaves an exact colour exactly alone");
        mwCheck(doc->WindowGetPixel(W, 12, 15) == before12,
                "...and does not bleed its neighbour into it");

        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the filter probe");
    }

    // ── DrawImageAlpha's base readback is lossless on a HiDPI canvas ───────
    // DrawImageAlpha reads its OWN canvas as the blend "base" via readPixels
    // BGRA32, then writes the blended result straight back.  A fully
    // alpha-0 overlay forces the blend to output exactly what it read as the
    // base (Blend_Mask with mask 0 keeps B), so this isolates that read: only
    // the plain BGRA32 arm still took the unscaled temp-DIB BitBlt, which
    // resamples on a Retina canvas -- see LEDGER-CONFLICT K-652.  If it reads
    // exactly, DrawImageAlpha is a no-op here.
    {
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlack, 0) == eOK,
                "clear before the BGRA32 readback probe");
        // Alternating 1px columns: the worst case for any resampling.
        for (long x = 10; x < 20; ++x)
            doc->WindowRectOp(W, 2, x, 10, x + 1, 20,
                              (x % 2) ? kRed : kGreen, 0);
        const long before11 = doc->WindowGetPixel(W, 11, 15);
        const long before12 = doc->WindowGetPixel(W, 12, 15);
        mwCheck(before11 != before12,
                "adjacent 1px columns really are different colours to start");

        // DrawImageAlpha refuses anything but a 32bpp image; capturing the
        // canvas itself is a convenient source.  WindowImageFromWindow pins
        // its alpha to 0 (BGRA32, no real alpha channel), so the merge below
        // keeps the base untouched regardless of the captured content -- this
        // exists only to exercise the base's readPixels(..., BGRA32) call.
        mwCheck(doc->WindowImageFromWindow(W, "canvasImg", W) == eOK,
                "WindowImageFromWindow captures the canvas as a 32bpp image");
        mwCheck(doc->WindowDrawImageAlpha(W, "canvasImg", 0, 0, 64, 48, 1.0,
                                          0, 0) == eOK,
                "WindowDrawImageAlpha (opacity 1, alpha-0 overlay) succeeds");

        mwCheck(doc->WindowGetPixel(W, 11, 15) == before11,
                "DrawImageAlpha's base readback leaves an exact colour exactly alone");
        mwCheck(doc->WindowGetPixel(W, 12, 15) == before12,
                "...and does not bleed its neighbour into it");

        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the BGRA32 readback probe");
    }

    // ── an image reports the DIB's depth and stride, not the wxBitmap's ────
    // Every image is materialised as `new wxBitmap(img, 32)`, so
    // wxBitmap::GetDepth() answers 32 for everything and there is no stride at
    // all.  bmBitsPixel was therefore 32 for a 24-bit RGB PNG -- where Windows
    // and the headless twin both say 24 -- and bmWidthBytes was a flat 0.  Two
    // things followed: world.WindowImageInfo returned wrong numbers, and the
    // `bmBitsPixel != 32` gate in DrawImageAlpha / GetImageAlpha could NEVER
    // fire on this platform, so both silently accepted images Windows refuses.
    //
    // WindowWrite emits PNG_COLOR_TYPE_RGB, so the file it produces reloads as
    // a 24bpp DIB -- a real fixture rather than a hand-built one.
    {
        char png[256];
        snprintf(png, sizeof(png), "/tmp/mc_selftest_depth_%d.png", (int) getpid());
        ::unlink(png);
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the image-depth probe");
        mwCheck(doc->WindowCreate("depthsrc", 0, 0, 9, 5, 0, 0, kRed) == eOK,
                "9x5 fixture window created");
        mwCheck(doc->WindowWrite("depthsrc", png) == eOK,
                "...written out as an RGB (no alpha) PNG");
        mwCheck(doc->WindowLoadImage(W, "rgb24", png) == eOK,
                "WindowLoadImage loads it back");

        VARIANT vw = doc->WindowImageInfo(W, "rgb24", 2);   // bmWidth
        VARIANT vs = doc->WindowImageInfo(W, "rgb24", 4);   // bmWidthBytes
        VARIANT vd = doc->WindowImageInfo(W, "rgb24", 6);   // bmBitsPixel
        mwCheck(vw.vt == VT_I4 && vw.lVal == 9,
                "the loaded image is 9 pixels wide");
        mwCheck(vd.vt == VT_I4 && vd.lVal == 24,
                "...reports 24bpp, the DIB's depth, not the wxBitmap's 32");
        // 9 x 3 bytes = 27, padded to a 4-byte boundary = 28.  Anchored to the
        // width and depth asserted above rather than being a magic number, and
        // it is the value tests/test_lua_surface.cpp already pins on the
        // headless twin for the same image.
        mwCheck(vs.vt == VT_I4 && vs.lVal == 28,
                "...and a 4-byte-aligned scan line of 28 bytes (was always 0)");

        // The two ops that gate on 32bpp must now refuse it, as Windows does.
        mwCheck(doc->WindowDrawImageAlpha(W, "rgb24", 0, 0, 0, 0, 1.0, 0, 0)
                    == eImageNotInstalled,
                "WindowDrawImageAlpha refuses a 24bpp image (no alpha channel)");
        mwCheck(doc->WindowGetImageAlpha(W, "rgb24", 0, 0, 0, 0, 0, 0)
                    == eImageNotInstalled,
                "...and so does WindowGetImageAlpha");
        // Negative control: MergeImageAlpha has NO depth gate on either
        // platform, so a fix that simply started refusing 24bpp everywhere
        // fails here.
        mwCheck(doc->WindowMergeImageAlpha(W, "rgb24", "rgb24",
                                           0, 0, 9, 5, 1, 1.0, 0, 0, 9, 5) == eOK,
                "...but WindowMergeImageAlpha, which has no gate, still accepts it");

        // The other control: a 32bpp source must still be ACCEPTED, so the gate
        // is really reading the depth and not refusing everything.
        mwCheck(doc->WindowImageFromWindow(W, "cap32", W) == eOK,
                "WindowImageFromWindow captures a 32bpp image");
        {
            VARIANT c = doc->WindowImageInfo(W, "cap32", 6);
            mwCheck(c.vt == VT_I4 && c.lVal == 32, "...which reports 32bpp");
        }
        mwCheck(doc->WindowDrawImageAlpha(W, "cap32", 0, 0, 0, 0, 1.0, 0, 0) == eOK,
                "...and DrawImageAlpha still accepts THAT");

        // WindowCreateImage's 8x8 pattern is a 1-bpp DDB on Windows, where the
        // wxBitmap behind it is 32 like everything else.
        mwCheck(doc->WindowCreateImage(W, "mono1",
                                       0x80, 0xC0, 0xE0, 0xF8,
                                       0xFC, 0xFE, 0xFF, 0xF0) == eOK,
                "WindowCreateImage makes an 8x8 mono pattern");
        {
            VARIANT m = doc->WindowImageInfo(W, "mono1", 6);
            mwCheck(m.vt == VT_I4 && m.lVal == 1,
                    "...which reports 1bpp, the bit count CreateBitmap was given");
        }

        doc->WindowDelete("depthsrc");
        ::unlink(png);
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the image-depth probe");
    }

    // ── CreateImage (8x8 mono) + WindowDrawImage round-trip ────────────────
    // Row-distinct "leading 1s" pattern (0x80, 0xC0, 0xE0, 0xF8, 0xFC, 0xFE,
    // 0xFF, 0xF0 -- one bit count each, no two rows equal), so a scanline-
    // stride bug that reads only every OTHER row -- leaving the skipped ones
    // as a solid 0x00 (all-clear) line -- cannot pass by accident.  Each row
    // has AT MOST one 0/1 transition, and only x=0 (always inside the leading
    // run) is probed, staying clear of the 2:1 HiDPI blit's known edge blur
    // at a transition (unrelated to this fix -- see the WindowFilter/BGRA32
    // probes above for that one). The top row (0xF0) also pins the 1-bit/
    // 0-bit colour polarity: GDI mono is 1 = background colour, 0 = text
    // colour -- probed at x=0 and x=7, on opposite sides of its one
    // transition and both away from it.
    {
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the CreateImage probe");
        mwCheck(doc->WindowCreateImage(W, "img1",
                                       0x80, 0xC0, 0xE0, 0xF8,
                                       0xFC, 0xFE, 0xFF, 0xF0) == eOK,
                "WindowCreateImage (8x8 mono, row-distinct pattern) succeeds");
        mwCheck(doc->WindowDrawImage(W, "img1", 0, 0, 8, 8, 1 /*straight copy*/,
                                     0, 0, 8, 8) == eOK,
                "WindowDrawImage (8x8 mono, 1:1) succeeds");

        // (a) every row drew its leading 1-bit at x=0.  A 1-byte stride reads
        // only every other row (see the finding), so the skipped rows come
        // out as a solid 0x00 line -- no white pixel anywhere in them.
        bool everyRowHasWhite = true;
        for (long y = 0; y < 8; ++y)
            everyRowHasWhite = everyRowHasWhite &&
                               (doc->WindowGetPixel(W, 0, y) == kWhite);
        mwCheck(everyRowHasWhite, "CreateImage: all eight rows drew "
                                  "(a 1-byte stride would blank half of them)");

        // (b) a 1-bit and a 0-bit pixel in the same row differ, and follow
        // GDI's polarity rather than its inverse.
        mwCheck(doc->WindowGetPixel(W, 0, 0) == kWhite,
                "CreateImage: a 1-bit pixel draws the background colour (white)");
        mwCheck(doc->WindowGetPixel(W, 7, 0) == kBlack,
                "CreateImage: a 0-bit pixel draws the text colour (black)");

        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the CreateImage probe");
    }

    // ── resize keeps working: re-create at a new size, probe again ────────
    mwCheck(doc->WindowCreate(W, 0, 0, 32, 32, 0, 0, kGreen) == eOK,
            "WindowCreate on an existing name resizes it");
    mwCheck(doc->WindowGetPixel(W, 16, 16) == kGreen,
            "resized window repainted with the new background");

    // ── WindowMenu: the anchor and the "~" alignment reach the popup ───────
    // world.WindowMenu(win, Left, Top, "~xy|item|item") asks for a menu at a
    // SPECIFIC spot inside the miniwindow, with the "~" prefix saying which
    // corner of the menu goes there.  CMiniWindow::Menu makes the point
    // absolute (Left += m_rect.left) and builds the TPM_* alignment word; the
    // port used to throw both away and pop the menu up at the mouse cursor, so
    // a menu raised from an alias/trigger/timer landed wherever the pointer was
    // and "drop this under my toolbar button" never lined up.
    //
    // g_wxhook_popup_menu is not installed yet at selftest time (the hooks go in
    // after this runs), so borrow it: capture what TrackPopupMenu was handed and
    // answer as though the user picked an item.
    {
        static int      s_x = -1, s_y = -1;
        static unsigned s_flags = 0;
        static int      s_items = 0;
        int (*saved)(CMenu*, int, int, unsigned) = g_wxhook_popup_menu;

        g_wxhook_popup_menu = [](CMenu* m, int x, int y, unsigned flags) -> int {
            s_x = x; s_y = y; s_flags = flags;
            s_items = m ? (int) m->GetMenuItemCount() : 0;
            return MXP_FIRST_MENU + 1;      // "user picked the second item"
        };

        const char* const MW = "selftest_menu_mw";
        mwCheck(doc->WindowCreate(MW, 30, 20, 40, 40, 0 /*absolute*/, 0, kBlack) == eOK,
                "WindowMenu: create a window at (30,20)");
        // CMiniWindow::Menu refuses to open a menu over a hidden window.
        mwCheck(doc->WindowShow(MW, TRUE) == eOK, "WindowMenu: show the window");

        VARIANT vaLeft = doc->WindowInfo(MW, 10);   // m_rect.left
        VARIANT vaTop  = doc->WindowInfo(MW, 11);   // m_rect.top
        const long rectLeft = vaLeft.lVal, rectTop = vaTop.lVal;

        // Plain menu, no "~": default (left/top) alignment, anchor honoured.
        BSTR r1 = doc->WindowMenu(MW, 5, 7, "One|Two|Three");
        mwCheck(s_x == (int)(rectLeft + 5) && s_y == (int)(rectTop + 7),
                "WindowMenu anchors the popup at the requested Left/Top");
        mwCheck(s_items == 3, "WindowMenu built all three items");
        mwCheck((s_flags & (TPM_CENTERALIGN | TPM_RIGHTALIGN |
                            TPM_VCENTERALIGN | TPM_BOTTOMALIGN)) == 0,
                "no \"~\" prefix leaves the default left/top alignment");
        mwCheck(strcmp((const char*) r1, "Two") == 0,
                "WindowMenu returns the item the user picked");
        SysFreeString(r1);

        // "~cb" = centre horizontally, bottom vertically — the drop-below-a-
        // button idiom.  Both bits must survive to the popup.
        s_flags = 0;
        BSTR r2 = doc->WindowMenu(MW, 12, 34, "~cb|One|Two|Three");
        mwCheck(s_x == (int)(rectLeft + 12) && s_y == (int)(rectTop + 34),
                "WindowMenu keeps the anchor when an alignment is given");
        mwCheck((s_flags & TPM_CENTERALIGN) != 0,
                "\"~c?\" reaches the popup as TPM_CENTERALIGN");
        mwCheck((s_flags & TPM_BOTTOMALIGN) != 0,
                "\"~?b\" reaches the popup as TPM_BOTTOMALIGN");
        SysFreeString(r2);

        // The anchor must arrive in SCREEN coordinates, as Win32's
        // TrackPopupMenu takes them.  Windows converts through the CMUSHView
        // (ClientToScreen(pView->m_hWnd, …)); the port has no view, so it
        // converts through the world's output pane instead.  Stand in for that
        // pane with a hook that offsets by a known amount.
        {
            void (*savedConv)(void*, int*, int*) = g_wxhook_output_client_to_screen;
            g_wxhook_output_client_to_screen = [](void*, int* x, int* y) {
                if (x) *x += 1000;
                if (y) *y += 2000;
            };
            BSTR rc = doc->WindowMenu(MW, 5, 7, "One|Two");
            mwCheck(s_x == (int)(rectLeft + 5 + 1000) &&
                    s_y == (int)(rectTop  + 7 + 2000),
                    "the anchor reaches the popup in SCREEN coordinates");
            SysFreeString(rc);
            g_wxhook_output_client_to_screen = savedConv;
        }

        // "~rt" = right/top: the other horizontal bit, and NOT the bottom one.
        s_flags = 0;
        BSTR r3 = doc->WindowMenu(MW, 0, 0, "~rt|Only");
        mwCheck((s_flags & TPM_RIGHTALIGN) != 0,
                "\"~r?\" reaches the popup as TPM_RIGHTALIGN");
        mwCheck((s_flags & TPM_BOTTOMALIGN) == 0,
                "...without inventing a vertical alignment");
        SysFreeString(r3);

        // TPM_RETURNCMD is what makes TrackPopupMenu hand the id back rather
        // than posting a WM_COMMAND — CMiniWindow::Menu depends on it.
        mwCheck((s_flags & TPM_RETURNCMD) != 0,
                "TPM_RETURNCMD survives to the popup");

        mwCheck(doc->WindowDelete(MW) == eOK, "WindowMenu: clean up the window");
        g_wxhook_popup_menu = saved;
    }

    // ── WindowAddHotspot: created under a resting pointer ──────────────────
    // Windows re-runs the mouse-move hit test when a hotspot is added under
    // the cursor, so the MouseOver fires (and m_sMouseOverHotspot updates)
    // without waiting for the pointer to twitch — methods_miniwindows.cpp's
    // `if (pHotspot->m_rect.PtInRect (mw->m_last_mouseposition))` arm.  That
    // arm used to walk the doc's view list for a CMUSHView, which the port
    // never constructs, so on macOS a plugin that redraws its button bar
    // (WindowDeleteAllHotspots + WindowAddHotspot) silently lost the hover
    // state under a motionless pointer.  It now dispatches through
    // pal::IWorldView::mouseMoveMiniWindow, which every frontend implements.
    {
        // A view that does what the real frontends do with the callback:
        // count it, and run the shared routing (CWorldChildFrame does exactly
        // this with its own hooks; the Windows adapter calls CMUSHView's
        // Mouse_Move_MiniWindow, which is the same body).
        struct MwSelfTestView : FakeWorldView {
            CMUSHclientDoc* doc = nullptr;
            int moves = 0;
            void mouseMoveMiniWindow(long x, long y) override {
                ++moves;
                FakeWorldView::mouseMoveMiniWindow(x, y);   // record the args
                MiniWindowMouseHooks hooks;      // no-op view services
                MouseMoveMiniWindow(doc, CPoint((int) x, (int) y), hooks);
            }
        };

        const char* const H = "selftest_mw_hs";
        MwSelfTestView view;
        view.doc = doc;

        mwCheck(doc->WindowCreate(H, 100, 50, 64, 48,
                                  0 /*position, ignored*/,
                                  MINIWINDOW_ABSOLUTE_LOCATION, kBlue) == eOK,
                "hotspot fixture: absolute-location window at (100,50) 64x48");
        // WindowCreate leaves a window hidden; placement and hit-testing both
        // skip hidden windows.
        mwCheck(doc->WindowShow(H, TRUE) == eOK, "hotspot fixture: shown");

        // Land m_rect the way a paint pass does, then park the pointer inside
        // the window at client (120, 70) = window-relative (20, 20).
        RECT clientrect = { 0, 0, 300, 200 };
        CalculateMiniWindowRectangles(doc, false, clientrect, clientrect);
        {
            MiniWindowMouseHooks hooks;
            MouseMoveMiniWindow(doc, CPoint(120, 70), hooks);
        }

        MiniWindowMapIterator mit = doc->m_MiniWindows.find(H);
        CMiniWindow* mw = (mit == doc->m_MiniWindows.end()) ? NULL : mit->second;
        mwCheck(mw != NULL, "hotspot fixture: the window is in the doc's map");
        mwCheck(mw && mw->m_last_mouseposition == CPoint(20, 20),
                "hotspot fixture: pointer parked at window-relative (20,20)");
        mwCheck(doc->m_lastMousePosition == CPoint(120, 70),
                "hotspot fixture: doc remembers client (120,70)");

        doc->m_pWorldView = &view;

        // (a) a hotspot created UNDER the pointer re-detects immediately
        mwCheck(doc->WindowAddHotspot(H, "under", 10, 10, 30, 30,
                                      "", "", "", "", "", "", 0, 0) == eOK,
                "WindowAddHotspot (10,10)-(30,30) succeeds");
        mwCheck(view.moves == 1,
                "a hotspot created under the pointer re-runs the mouse move");
        mwCheck(view.lastMouseMoveX == 120 && view.lastMouseMoveY == 70,
                "...at the doc's last CLIENT mouse position, not the "
                "window-relative one");
        mwCheck(mw && mw->m_sMouseOverHotspot == "under",
                "...so the window's mouse-over state names the new hotspot");

        // (b) one created AWAY from the pointer must NOT: the PtInRect guard
        // is what keeps this from firing MouseOver for every hotspot a plugin
        // installs.  (Without this half, a fix that always dispatches passes.)
        mwCheck(doc->WindowAddHotspot(H, "elsewhere", 40, 40, 60, 46,
                                      "", "", "", "", "", "", 0, 0) == eOK,
                "WindowAddHotspot (40,40)-(60,46) succeeds");
        mwCheck(view.moves == 1,
                "a hotspot created away from the pointer does NOT re-run it");
        mwCheck(mw && mw->m_sMouseOverHotspot == "under",
                "...and the mouse-over state is unchanged");

        // (c) the wheel callback DOES receive the keyboard modifiers.
        //
        // wx_outputpane.cpp's DecideWheelAction comment asserts the opposite --
        // "The callback does NOT see the ctrl bit, on either platform: the
        // flags are just (delta < 0 ? MW_MOUSE_SCROLL_BACK : 0) |
        // (abs (delta) << 16)" -- citing a Windows measurement of 0x780000.
        // That expression is only what the CALLER passes in.  Both frontends
        // then funnel into the shared MouseWheelMiniWindow
        // (mushview.cpp Mouse_Wheel_MiniWindow here, wx_world_child.cpp there),
        // which calls SendMouseEventToPlugin WITHOUT the dont_modify_flags
        // argument -- and that defaults to false (miniwindow.h), so
        // `Flags |= hooks.KeyboardModifiers ()` runs before the callback is
        // dispatched.  Both frontends' KeyboardModifiers set MW_MOUSE_CTRL from
        // the live key state, so a Ctrl+wheel over a hotspot arrives as
        // 0x780002.  The measurement was almost certainly taken at the call
        // site rather than at the callback.
        //
        // Asserting the CONSULTATION rather than the final value keeps this
        // free of a script engine: SendMouseEventToPlugin reaches the modifier
        // OR before it does anything with the routine, and the OR is the only
        // thing that calls KeyboardModifiers on this path.  So "was it asked?"
        // is exactly "is dont_modify_flags false here?", which is the claim.
        {
            struct CtrlHooks : MiniWindowMouseHooks {
                int asked = 0;
                long KeyboardModifiers() override
                    { ++asked; return MW_MOUSE_CTRL; }
            };
            CtrlHooks wheelHooks;

            mwCheck(doc->WindowScrollwheelHandler(H, "under", "OnWheel") == eOK,
                    "wheel: the hotspot gets a scrollwheel callback");

            // pointer is still parked inside "under" at client (120,70)
            const long delta = (120 << 16);   // one forward notch, as Win32 packs it
            const bool overMw = MouseWheelMiniWindow(doc, CPoint(120, 70),
                                                     delta, wheelHooks);
            mwCheck(overMw, "wheel: the event lands on the miniwindow");
            mwCheck(wheelHooks.asked == 1,
                    "wheel: the modifiers ARE consulted, so Ctrl reaches the "
                    "callback (dont_modify_flags is false on this path)");

            // The control: a hotspot with no scrollwheel callback returns from
            // SendMouseEventToPlugin BEFORE the modifier OR, so a bare
            // "asked >= 1" could otherwise be satisfied by some other path.
            mwCheck(doc->WindowScrollwheelHandler(H, "under", "") == eOK,
                    "wheel: clear the callback again");
            wheelHooks.asked = 0;
            MouseWheelMiniWindow(doc, CPoint(120, 70), delta, wheelHooks);
            mwCheck(wheelHooks.asked == 0,
                    "...and with no callback installed they are not consulted");
        }

        doc->m_pWorldView = NULL;
        mwCheck(doc->WindowDelete(H) == eOK, "hotspot fixture cleaned up");
    }

    // ── WindowGetPixel is O(1) in the SURFACE size, not O(surface) ─────────
    //
    // A complexity pin, not a stopwatch.  WindowGetPixel → CMiniWindow::GetPixel
    // → pal::ISurface::getPixel → WxSurface::readPixels(…, 1, 1) →
    // wxgdi_read_bgra.  A miniwindow canvas is a plain wxBitmap, never a
    // DIBSection, so it always misses the fast path and lands in the wxImage
    // branch — which used to run bmp.ConvertToImage() over the WHOLE surface, at
    // physical resolution (2x on Retina), to answer a single-pixel probe.  Cost
    // per probe therefore scaled with the window's area: a plugin sweeping a
    // 400x300 miniwindow did 120,000 conversions of an 800x600x4 buffer and the
    // app appeared to hang.  The same shape sat in the hot blit path, where a
    // transparent DrawImage reads the source's pixel (0,0) as its colour key.
    //
    // Time the SAME number of probes against a small window and a 256x-larger
    // one.  With an area-proportional read the big one costs ~256x more; with a
    // region read the two are within call-overhead of each other.  The bound is
    // deliberately loose (16x) so ordinary machine noise cannot fail it while
    // still being two orders of magnitude below the regression.
    {
        const char* const SMALL = "selftest_mw_small";
        const char* const BIG   = "selftest_mw_big";
        const int  kProbes = 400;
        mwCheck(doc->WindowCreate(SMALL, 0, 0, 32, 32, 0, 0, kRed) == eOK,
                "perf pin: 32x32 window created");
        mwCheck(doc->WindowCreate(BIG, 0, 0, 512, 512, 0, 0, kRed) == eOK,
                "perf pin: 512x512 window created (256x the area)");

        auto probe = [&](const char* name, int extent) {
            const auto t0 = std::chrono::steady_clock::now();
            long acc = 0;
            for (int i = 0; i < kProbes; ++i)
                acc += doc->WindowGetPixel(name, i % extent, (i * 7) % extent);
            const auto t1 = std::chrono::steady_clock::now();
            g_mwSink += acc;   // keep the reads alive
            return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0)
                   .count();
        };
        probe(SMALL, 32);                          // warm the code paths
        const long long tSmall = probe(SMALL, 32);
        const long long tBig   = probe(BIG, 512);

        // +500us of slack so a sub-millisecond small-window measurement (where
        // clock granularity dominates) cannot make the ratio meaningless.
        const bool ok = tBig <= tSmall * 16 + 500;
        printf("  [%s] %d WindowGetPixel probes: 32x32 %lldus vs 512x512 "
               "%lldus (area x256; must stay under %lldus)\n",
               ok ? "PASS" : "FAIL", kProbes, tSmall, tBig, tSmall * 16 + 500);
        (ok ? g_mwPass : g_mwFail)++;

        mwCheck(doc->WindowGetPixel(BIG, 500, 500) == kRed,
                "...and the probes still read the right colour");
        doc->WindowDelete(SMALL);
        doc->WindowDelete(BIG);
    }

    // ── drawing survives a resize, even after a pixel-write op ─────────────
    // (2026-08-12)  wxgdi_write_bgra used to swap a detached wxBitmap into the
    // wxMemoryDC, orphaning the CBitmap's m_impl at pre-write content.  Every
    // pixel-write op (WindowSetPixel, BlendImage, the alpha draws...) took
    // that path, and CMiniWindow::Resize copies old content through the
    // BITMAP OBJECT — so everything drawn after the first pixel op vanished
    // on the next WindowResize.  Quow-style draw-then-resize-to-fit plugins
    // hit this on every layout pass.
    {
        const char* RW = "selftest_mw_resize";
        mwCheck(doc->WindowCreate(RW, 0, 0, 32, 32, 0, 0, kBlack) == eOK,
                "resize fixture window created");
        // A line op BEFORE any pixel write (control), then the pixel write.
        mwCheck(doc->WindowRectOp(RW, 2, 1, 1, 9, 9, kGreen, 0) == eOK,
                "...rect drawn (pre-pixel-write path)");
        mwCheck(doc->WindowSetPixel(RW, 20, 20, kRed) == eOK,
                "...pixel written (the write_bgra path)");
        mwCheck(doc->WindowGetPixel(RW, 20, 20) == kRed,
                "...and reads back before the resize");

        mwCheck(doc->WindowResize(RW, 48, 48, kBlue) == eOK,
                "WindowResize grows the window");
        mwCheck(doc->WindowGetPixel(RW, 4, 4) == kGreen,
                "resize keeps content drawn BEFORE the pixel write");
        mwCheck(doc->WindowGetPixel(RW, 20, 20) == kRed,
                "resize keeps the written pixel (it used to revert)");
        doc->WindowDelete(RW);
    }

    // ── SetBackgroundImage('') clears the image (wx CBitmap::Attach token) ──
    // The shared clear guard reads the m_hObject token, which Attach left
    // null after every load — so the delete was skipped and the old image
    // painted forever.  Same assertion as the headless twin in
    // tests/test_lua_surface.cpp, here against the REAL wx bitmap.
    {
        char png[256];
        snprintf(png, sizeof(png), "/tmp/mc_selftest_bg_%d.png", (int) getpid());
        ::unlink(png);
        mwCheck(doc->WindowCreate("bgsrc", 0, 0, 9, 5, 0, 0, kBlue) == eOK,
                "background fixture window created");
        mwCheck(doc->WindowWrite("bgsrc", png) == eOK, "...written as a PNG");
        mwCheck(doc->SetBackgroundImage(png, 0) == eOK,
                "SetBackgroundImage loads it");
        mwCheck((HBITMAP) doc->m_BackgroundBitmap != NULL,
                "...and the ownership token is set (it used to stay null)");
        BITMAP bmInfo = {};
        mwCheck(doc->m_BackgroundBitmap.GetBitmap(&bmInfo) &&
                    bmInfo.bmWidth == 9,
                "...and the bitmap is real (9 wide)");

        // GetObject honours nCount -- the Splash.cpp protocol.  Splash passes
        // sizeof(DIBSECTION) and reads dsBmih.biClrUsed / biBitCount; the old
        // body ignored nCount, filled the leading BITMAP and left the rest as
        // stack garbage.  POISONED first, so "wrote nothing there" fails.
        DIBSECTION ds;
        memset(&ds, 0x7F, sizeof ds);
        mwCheck(doc->m_BackgroundBitmap.GetObject((int) sizeof ds, &ds)
                    == (int) sizeof(DIBSECTION),
                "GetObject(sizeof DIBSECTION) answers the DIBSECTION size");
        mwCheck(ds.dsBm.bmWidth == 9 && ds.dsBm.bmHeight == 5,
                "...its BITMAP half matches GetBitmap");
        mwCheck(ds.dsBmih.biSize == sizeof(BITMAPINFOHEADER) &&
                ds.dsBmih.biWidth == 9 && ds.dsBmih.biHeight == 5 &&
                ds.dsBmih.biPlanes == 1 &&
                ds.dsBmih.biBitCount == ds.dsBm.bmBitsPixel &&
                ds.dsBmih.biCompression == 0 && ds.dsBmih.biClrUsed == 0,
                "...its DIB half is coherent, no byte left poisoned");
        mwCheck(ds.dsBmih.biSizeImage != 0 &&
                ds.dsBmih.biSizeImage ==
                    (DWORD) ds.dsBm.bmWidthBytes * (DWORD) ds.dsBm.bmHeight,
                "...and biSizeImage is the stride times the height, not 0");
        mwCheck(doc->m_BackgroundBitmap.GetObject((int) sizeof ds, NULL)
                    == (int) sizeof(DIBSECTION),
                "GetObject(NULL) is Win32's size query and answers the size");
        // The other direction of "honours nCount": a BITMAP-sized buffer gets
        // the BITMAP and NOT ONE BYTE past it.
        memset(&ds, 0x7F, sizeof ds);
        mwCheck(doc->m_BackgroundBitmap.GetObject((int) sizeof(BITMAP), &ds)
                    == (int) sizeof(BITMAP),
                "GetObject(sizeof BITMAP) keeps the BITMAP-only contract");
        mwCheck(ds.dsBm.bmWidth == 9 && ds.dsBmih.biSize == 0x7F7F7F7Fu,
                "...and writes nothing past the caller's declared size");
        memset(&ds, 0x7F, sizeof ds);
        mwCheck(doc->m_BackgroundBitmap.GetObject(4, &ds) == 0 &&
                ds.dsBm.bmType == 0x7F7F7F7F,
                "...and a buffer smaller than a BITMAP is refused untouched");
        mwCheck(doc->SetBackgroundImage("", 0) == eOK,
                "SetBackgroundImage('') accepts the clear");
        // The zero-first contract, which this build did not keep: ten call sites
        // in miniwindow.cpp declare `BITMAP bi;` uninitialised and read
        // bi.bmBitsPixel without checking the return, so a bare `return FALSE`
        // lets them branch on stack garbage.  POISON the struct first -- without
        // that, the assertion below passes on the broken code, because the
        // struct still holds the ZEROES it was initialised with.  Same shape as
        // the headless twin's pin in tests/test_gdi_headless.cpp.
        memset(&bmInfo, 0x7F, sizeof bmInfo);
        mwCheck(doc->m_BackgroundBitmap.GetBitmap(&bmInfo) == FALSE,
                "...and the image is really GONE (it used to survive)");
        mwCheck(bmInfo.bmWidth == 0 && bmInfo.bmHeight == 0 &&
                bmInfo.bmWidthBytes == 0 && bmInfo.bmPlanes == 0 &&
                bmInfo.bmBitsPixel == 0 && bmInfo.bmType == 0 &&
                bmInfo.bmBits == NULL,
                "...and GetBitmap zeroed the caller's BITMAP before failing");
        memset(&ds, 0x7F, sizeof ds);
        mwCheck(doc->m_BackgroundBitmap.GetObject((int) sizeof ds, &ds) == 0 &&
                ds.dsBm.bmWidth == 0 && ds.dsBmih.biSize == 0 &&
                ds.dsBmih.biBitCount == 0,
                "...and GetObject zeroes the whole declared buffer before failing");
        mwCheck(doc->m_BackgroundBitmap.GetObject((int) sizeof ds, NULL) == 0,
                "...and the size query answers 0 with no bitmap to size");
        doc->WindowDelete("bgsrc");
        ::unlink(png);
    }

    // ── deletion ───────────────────────────────────────────────────────────
    mwCheck(doc->WindowDelete(W) == eOK, "WindowDelete succeeds");
    mwCheck(doc->WindowDelete(W) == eNoSuchWindow,
            "double delete returns eNoSuchWindow");
    mwCheck(doc->WindowGetPixel(W, 1, 1) == -2,
            "pixel probe on a deleted window returns -2 (no such window)");

    delete doc;

    printf("PAL-MINIWINDOW-SELFTEST: %d passed, %d failed\n", g_mwPass, g_mwFail);
    return g_mwFail;
}

#endif // !_WIN32
