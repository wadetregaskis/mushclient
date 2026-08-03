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
