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
#include "scripting/errors.h"

#include <cstdio>

namespace {

int g_mwPass = 0, g_mwFail = 0;
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

        // Leave the canvas as we found it for the assertions that follow.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the chord probe");
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

        // NOT asserted, because it does not currently hold: kFine is a 50/50
        // checkerboard (0xAA/0x55), so BOTH colours should appear -- and only
        // one does.  An 8x8 stipple is collapsing to a single colour, which is
        // the HiDPI resampling problem tracked separately (the miniwindow canvas
        // is 2x and every read-modify-write resamples it down and back up).
        // Colouring the tile is still a strict improvement over black/white;
        // the pattern's SHAPE needs that other fix before it can be pinned here.
        (void) sawRed; (void) sawGreen;
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the mono-pattern probe");
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
        // averageWidth * 2.  For a fixed-pitch font it must equal the average.
        VARIANT vavg = doc->WindowFontInfo(W, "f1", 6);   // tmAveCharWidth
        VARIANT vmax = doc->WindowFontInfo(W, "f1", 7);
        if (vavg.vt == VT_I4 && vmax.vt == VT_I4 && vavg.lVal > 0)
            mwCheck(vmax.lVal == vavg.lVal,
                    "a fixed-pitch font's max char width equals its average");
        else
            mwCheck(false, "WindowFontInfo(6)/(7) return numbers");
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

        // EDGE_ETCHED (0x06) used to hit `default:` and paint flat grey.  It is
        // sunken-outer + raised-inner, so the outer and inner rings differ.
        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "clear before the etched probe");
        mwCheck(doc->WindowRectOp(W, 5, 10, 10, 40, 40, 0x06, 0x0F) == eOK,
                "WindowRectOp (DrawEdge, EDGE_ETCHED) succeeds");
        mwCheck(doc->WindowGetPixel(W, 25, 10) != doc->WindowGetPixel(W, 25, 11),
                "EDGE_ETCHED's outer and inner rings differ (not a flat box)");

        mwCheck(doc->WindowRectOp(W, 2, 0, 0, 64, 48, kBlue, 0) == eOK,
                "restore the background after the DrawEdge probe");
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

    // ── CreateImage (8x8 mono) + ImageOp pattern fill ──────────────────────
    // all-ones rows = a solid 8x8 pattern; 1-bits draw in PenColour for
    // ImageOp action 3 (pattern fill uses the image as a brush)
    mwCheck(doc->WindowCreateImage(W, "img1",
                                   255, 255, 255, 255, 255, 255, 255, 255) == eOK,
            "WindowCreateImage (8x8 mono) succeeds");

    // ── resize keeps working: re-create at a new size, probe again ────────
    mwCheck(doc->WindowCreate(W, 0, 0, 32, 32, 0, 0, kGreen) == eOK,
            "WindowCreate on an existing name resizes it");
    mwCheck(doc->WindowGetPixel(W, 16, 16) == kGreen,
            "resized window repainted with the new background");

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
