// platform/wx_composite_selftest.cpp — in-app conformance test for the wx
// miniwindow compositor and its tooltip gate (--pal-composite-selftest).
//
// wx_composite_miniwindows() and wx_miniwindow_tooltip() live in the mushgui
// executable (platform/wx_world_child.cpp): they touch wxDC, so mushcore —
// which the 30 headless test executables link — cannot see them, and the
// headless build stubs the whole world.Window* family out anyway
// (platform/doc_gui_method_stubs.cpp).  An in-app selftest is therefore the
// only place this code can be executed, exactly like the surface / transport /
// miniwindow ones beside it.  Runs before the GUI boots and exits.
//
// What it pins is Windows parity with CMUSHView::OnDraw + Blit_Bitmap
// (mushview.cpp:684-847, :996-1026, :1497-1524) and
// CMUSHView::Mouse_Tooltip_MiniWindow (mushview.cpp:6582-6640):
//
//   * miniwindows are drawn 1:1 at their landed origin in EVERY position mode
//     (OnDraw passes bUseRect = position != 13, which switches Blit_Bitmap's
//     StretchBlt arm off; the stretch belongs to DrawImage's background /
//     foreground bitmaps, not to miniwindows);
//   * after a paint m_rect is a NORMALIZED rectangle — Blit_Bitmap's closing
//     `rect.right += rect.left; rect.bottom += rect.top;` — which is what
//     WindowInfo 10..13 hands to scripts;
//   * a point over a miniwindow gets that window's answer and no other: a bare
//     spot (or a hotspot with no tooltip text) shows NOTHING rather than the
//     tooltip of the output text the window is covering.
//
// Colour values passed to the world API are 0x00BBGGRR (COLORREF).

#ifndef _WIN32

// wx first, then the MUSHclient/MFC headers (platform include-order rule).
#include <wx/wx.h>
#include <wx/dcmemory.h>
#include <wx/image.h>

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "miniwindow.h"                 // CMiniWindow, MINIWINDOW_* flags
#include "scripting/errors.h"
#include "platform/wx_world_child.h"    // wx_composite_miniwindows / _tooltip

#include <cstdio>
#include <string>

namespace {

int g_pass = 0, g_fail = 0;
void ckCheck(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

const long kRed   = 0x0000FF;   // COLORREF is BGR
const long kGreen = 0x00FF00;
const long kBlue  = 0xFF0000;

// The canvas the compositor paints onto, plus a pixel reader.  wxDC::GetPixel
// is unreliable on wxOSX, so the bitmap is converted to a wxImage once the
// memory DC has released it.
struct DestCanvas {
    wxBitmap bmp;
    wxMemoryDC dc;
    wxImage   img;

    DestCanvas(int w, int h, const wxColour& fill) : bmp(w, h, 32)
    {
        dc.SelectObject(bmp);
        dc.SetBackground(wxBrush(fill));
        dc.Clear();
    }
    void Finish()                       // deselect, then snapshot the pixels
    {
        dc.SelectObject(wxNullBitmap);
        img = bmp.ConvertToImage();
    }
    bool Is(int x, int y, const wxColour& c) const
    {
        if (!img.IsOk() || x < 0 || y < 0 ||
            x >= img.GetWidth() || y >= img.GetHeight())
            return false;
        return img.GetRed(x, y)   == c.Red()   &&
               img.GetGreen(x, y) == c.Green() &&
               img.GetBlue(x, y)  == c.Blue();
    }
};

// WindowInfo 10..13 as a plain long (they are all SetUpVariantLong/VT_I4).
long WinInfoLong(CMUSHclientDoc* doc, const char* name, long what)
{
    VARIANT v = doc->WindowInfo(name, what);
    return (v.vt == VT_I4) ? v.lVal : -999999;
}

} // namespace

int RunWxCompositeSelfTest()
{
    printf("PAL-COMPOSITE-SELFTEST: driving the real wx miniwindow compositor\n");

    const wxColour paneColour(40, 40, 40);      // the "output pane" backdrop
    const wxColour blueColour(0, 0, 255);       // kBlue drawn
    const wxColour greenColour(0, 255, 0);      // kGreen drawn
    const wxColour redColour(255, 0, 0);        // kRed drawn

    CMUSHclientDoc* doc = new CMUSHclientDoc();

    // The compositor's two geometry inputs: the output pane's client size and
    // the owning frame's.  Equal here — no mode under test distinguishes them.
    const int kW = 400, kH = 300;

    // ── 1. position 0 ("stretch to output window") is NOT stretched ────────
    // The port used to StretchBlit modes 0-3 to the landed rect, which for
    // mode 0 is the whole pane: a 64x48 canvas was blown up to 400x300 while
    // MouseOverMiniWindow kept hit-testing the top-left 64x48
    // (miniwindow_compose.cpp reads m_rect.left/top + GetWidth/GetHeight), so
    // every hotspot fired somewhere other than where it was drawn.
    {
        const char* const S = "selftest_stretch";
        ckCheck(doc->WindowCreate(S, 0, 0, 64, 48,
                                  0 /*stretch to output window*/, 0, kBlue) == eOK,
                "WindowCreate 64x48 at position 0 (stretch to output window)");
        ckCheck(doc->WindowShow(S, TRUE) == eOK, "...and shown");

        DestCanvas canvas(kW, kH, paneColour);
        wx_composite_miniwindows(doc, canvas.dc, kW, kH, kW, kH, false);
        canvas.Finish();

        ckCheck(canvas.Is(5, 5, blueColour),
                "the canvas is drawn at its own origin");
        ckCheck(canvas.Is(63, 47, blueColour),
                "...out to its own bottom-right pixel");
        ckCheck(canvas.Is(64, 48, paneColour),
                "...and NOT one pixel further (Windows blits 1:1, never "
                "stretches a miniwindow)");
        ckCheck(canvas.Is(kW / 2, kH / 2, paneColour),
                "...so the middle of the pane is untouched");

        // m_rect for a stretch mode starts as the whole client rect; the paint
        // must leave it describing the 64x48 the user can actually click.
        ckCheck(WinInfoLong(doc, S, 10) == 0 && WinInfoLong(doc, S, 11) == 0,
                "WindowInfo 10/11 (left/top) = 0,0");
        ckCheck(WinInfoLong(doc, S, 12) == 64 && WinInfoLong(doc, S, 13) == 48,
                "WindowInfo 12/13 (right/bottom) = 64,48 - the drawn extent, "
                "not the pane");

        ckCheck(doc->WindowDelete(S) == eOK, "stretch fixture cleaned up");
    }

    // ── 1b. tile (position 13): the one mode whose rect Blit_Bitmap owns ────
    // CalculateMiniWindowRectangles skips it (`case 13: continue`) because
    // Blit_Bitmap's own `case 13` does `rect = r` (the view's client rect) and
    // returns before the normalization.  The port skipped it in BOTH places,
    // so a tiled window reported 0,0,0,0.
    {
        const char* const T = "selftest_tile";
        ckCheck(doc->WindowCreate(T, 0, 0, 32, 24, 13 /*tile*/, 0, kRed) == eOK,
                "WindowCreate 32x24 at position 13 (tile)");
        ckCheck(doc->WindowShow(T, TRUE) == eOK, "...and shown");

        DestCanvas canvas(kW, kH, paneColour);
        wx_composite_miniwindows(doc, canvas.dc, kW, kH, kW, kH, false);
        canvas.Finish();

        ckCheck(canvas.Is(5, 5, redColour) && canvas.Is(kW - 5, kH - 5, redColour),
                "the tile covers the whole pane");
        ckCheck(WinInfoLong(doc, T, 10) == 0 && WinInfoLong(doc, T, 11) == 0,
                "tile: WindowInfo 10/11 = 0,0");
        ckCheck(WinInfoLong(doc, T, 12) == kW && WinInfoLong(doc, T, 13) == kH,
                "tile: WindowInfo 12/13 = the client rect, as Blit_Bitmap "
                "leaves it");

        ckCheck(doc->WindowDelete(T) == eOK, "tile fixture cleaned up");
    }

    // ── 2. m_rect is a rectangle after a paint, not position + size ────────
    // Blit_Bitmap ends with `rect.right += rect.left; rect.bottom += rect.top;`
    // (mushview.cpp:844-846).  The port stopped at the width/height half, so a
    // window at x=150 reported right=64 — a right edge to the LEFT of its own
    // left edge, and any plugin computing width as info(12)-info(10) got a
    // negative number.
    {
        const char* const A = "selftest_abs";
        const char* const C = "selftest_corner";

        ckCheck(doc->WindowCreate(A, 150, 100, 64, 48, 0,
                                  MINIWINDOW_ABSOLUTE_LOCATION, kGreen) == eOK,
                "WindowCreate 64x48 at absolute (150,100)");
        ckCheck(doc->WindowShow(A, TRUE) == eOK, "...and shown");
        ckCheck(doc->WindowCreate(C, 0, 0, 64, 48,
                                  6 /*top right*/, 0, kBlue) == eOK,
                "WindowCreate 64x48 at position 6 (top right)");
        ckCheck(doc->WindowShow(C, TRUE) == eOK, "...and shown");

        DestCanvas canvas(kW, kH, paneColour);
        wx_composite_miniwindows(doc, canvas.dc, kW, kH, kW, kH, false);
        canvas.Finish();

        ckCheck(canvas.Is(151, 101, greenColour),
                "the absolute-location window is drawn at (150,100)");
        ckCheck(canvas.Is(kW - 1, 1, blueColour),
                "the top-right window reaches the right edge of the pane");

        ckCheck(WinInfoLong(doc, A, 10) == 150 && WinInfoLong(doc, A, 11) == 100,
                "absolute: WindowInfo 10/11 = 150,100");
        ckCheck(WinInfoLong(doc, A, 12) == 214,
                "absolute: WindowInfo 12 = 214 (left + width), not 64");
        ckCheck(WinInfoLong(doc, A, 13) == 148,
                "absolute: WindowInfo 13 = 148 (top + height), not 48");

        ckCheck(WinInfoLong(doc, C, 10) == kW - 64 && WinInfoLong(doc, C, 11) == 0,
                "top right: WindowInfo 10/11 = 336,0");
        ckCheck(WinInfoLong(doc, C, 12) == kW,
                "top right: WindowInfo 12 = 400 - the pane's right edge");
        ckCheck(WinInfoLong(doc, C, 13) == 48,
                "top right: WindowInfo 13 = 48 (top 0 + height)");
        ckCheck(WinInfoLong(doc, C, 12) - WinInfoLong(doc, C, 10) == 64 &&
                WinInfoLong(doc, A, 12) - WinInfoLong(doc, A, 10) == 64,
                "...so info(12) - info(10) is the width, as plugins assume");

        ckCheck(doc->WindowDelete(C) == eOK, "corner fixture cleaned up");

        // ── 3. the tooltip gate ────────────────────────────────────────────
        // Windows' Mouse_Tooltip_MiniWindow returns TRUE for ANY point over a
        // miniwindow — with an empty string when there is no hotspot tooltip —
        // and its caller then returns before the MXP hint code
        // (mushview.cpp:4319).  The port used to drop the CMiniWindow* on the
        // floor and fall through, so hovering an opaque window leaked the hint
        // of the output text hidden underneath it.
        //
        // The window from step 2 is still in place: absolute (150,100) 64x48,
        // so window-relative (20,20) is client (170,120).
        ckCheck(doc->WindowAddHotspot(A, "tip", 10, 10, 30, 30,
                                      "", "", "", "", "", "hello", 0, 0) == eOK,
                "WindowAddHotspot with a tooltip");
        ckCheck(doc->WindowAddHotspot(A, "bare", 10, 35, 30, 45,
                                      "", "", "", "", "", "", 0, 0) == eOK,
                "WindowAddHotspot with NO tooltip");

        std::string tip;
        ckCheck(wx_miniwindow_tooltip(doc, 170, 120, tip) && tip == "hello",
                "over a hotspot with a tooltip: handled, with its text");

        tip = "not touched";
        ckCheck(wx_miniwindow_tooltip(doc, 170, 140, tip) && tip.empty(),
                "over a hotspot with NO tooltip: handled, with empty text "
                "(the text underneath must not show through)");

        tip = "not touched";
        ckCheck(wx_miniwindow_tooltip(doc, 200, 140, tip) && tip.empty(),
                "over a bare part of the window: handled, with empty text");

        tip = "not touched";
        ckCheck(!wx_miniwindow_tooltip(doc, 10, 250, tip) && tip.empty(),
                "away from every miniwindow: NOT handled (the caller falls "
                "through to the MXP hint)");

        // Two-part "title\tbody" tooltips: the tab must not survive.
        ckCheck(doc->WindowAddHotspot(A, "tip", 10, 10, 30, 30,
                                      "", "", "", "", "", "Title\tBody",
                                      0, 0) == eOK,
                "WindowAddHotspot with a title\\tbody tooltip");
        tip.clear();
        ckCheck(wx_miniwindow_tooltip(doc, 170, 120, tip) && tip == "Title\nBody",
                "the tab in a two-part tooltip becomes a newline");

        ckCheck(doc->WindowDelete(A) == eOK, "absolute fixture cleaned up");
    }

    // ── 4. the transparency-mask cache ─────────────────────────────────────
    // A transparent miniwindow is composited by building a masked bitmap: a
    // bitmap allocation, a full-canvas blit, and a wxMask, which is an
    // O(w x h) scan of every pixel against the key colour.  That ran on every
    // paint for every transparent window; it is now cached against the
    // surface adapter's content generation.
    //
    // Counting rebuilds is NOT enough on its own: a cache that never
    // invalidates would show a beautiful hit rate and draw stale pixels
    // forever, and the drawn result looks identical either way.  So every
    // arm below checks the PIXELS as well as the counters.
    {
        const char* const T = "selftest_maskcache";
        // Transparent, keyed on its own background (red): the red pixels drop
        // out and whatever was drawn in green shows through.
        ckCheck(doc->WindowCreate(T, 10, 10, 64, 48, 0,
                                  MINIWINDOW_ABSOLUTE_LOCATION | MINIWINDOW_TRANSPARENT,
                                  kRed) == eOK,
                "mask cache: transparent window created");
        ckCheck(doc->WindowShow(T, TRUE) == eOK, "...and shown");
        ckCheck(doc->WindowRectOp(T, 2 /*fill*/, 0, 0, 20, 20, kGreen, 0) == eOK,
                "...with a green patch drawn on it");

        long builds = 0, hits = 0;
        wx_miniwindow_mask_cache_reset();

        // First composite: nothing cached, so it must build.
        {
            DestCanvas canvas(kW, kH, paneColour);
            wx_composite_miniwindows(doc, canvas.dc, kW, kH, kW, kH, false);
            canvas.Finish();
            wx_miniwindow_mask_cache_stats(builds, hits);
            ckCheck(builds == 1 && hits == 0, "mask cache: first paint builds it");
            ckCheck(canvas.Is(15, 15, greenColour),
                    "...the green patch is drawn");
            ckCheck(canvas.Is(50, 40, paneColour),
                    "...and the keyed background is transparent");
        }

        // Second composite, nothing touched: must reuse, and draw the same.
        {
            DestCanvas canvas(kW, kH, paneColour);
            wx_composite_miniwindows(doc, canvas.dc, kW, kH, kW, kH, false);
            canvas.Finish();
            wx_miniwindow_mask_cache_stats(builds, hits);
            ckCheck(builds == 1 && hits == 1,
                    "mask cache: an unchanged canvas is NOT rebuilt");
            ckCheck(canvas.Is(15, 15, greenColour) && canvas.Is(50, 40, paneColour),
                    "...and draws exactly what the built one did");
        }

        // Now change the canvas.  The generation moves, so the mask must be
        // rebuilt -- and, the part that actually matters, the NEW contents
        // must appear.  A cache that ignored the generation would still show
        // the old green-only canvas here.
        ckCheck(doc->WindowRectOp(T, 2 /*fill*/, 30, 0, 50, 20, kBlue, 0) == eOK,
                "mask cache: a blue patch added to the canvas");
        {
            DestCanvas canvas(kW, kH, paneColour);
            wx_composite_miniwindows(doc, canvas.dc, kW, kH, kW, kH, false);
            canvas.Finish();
            wx_miniwindow_mask_cache_stats(builds, hits);
            ckCheck(builds == 2, "mask cache: a changed canvas IS rebuilt");
            ckCheck(canvas.Is(45, 15, blueColour),
                    "...and the NEW pixels are what gets drawn");
            ckCheck(canvas.Is(15, 15, greenColour),
                    "...alongside the old ones");
        }

        // Deleting the window must not leave its bitmap in the cache.
        ckCheck(doc->WindowDelete(T) == eOK, "mask cache: window deleted");
        {
            DestCanvas canvas(kW, kH, paneColour);
            wx_composite_miniwindows(doc, canvas.dc, kW, kH, kW, kH, false);
            canvas.Finish();
        }
        ckCheck(wx_miniwindow_mask_cache_size() == 0,
                "...and its cache entry went with it");
        wx_miniwindow_mask_cache_reset();
    }

    // ── 5. the mask cache with TWO worlds open ─────────────────────────────
    // The cache is one process-wide map, but the sweep above can only see the
    // miniwindow order of the document being painted.  Sweeping the WHOLE map
    // against it therefore threw away every other open world's entries on every
    // paint, so with two worlds showing transparent miniwindows nothing ever
    // hit: A's paint deleted B's bitmaps, B's paint deleted A's, for ever.
    // Alternating the two paints is what makes that visible; one doc alone
    // cannot see it at all.
    {
        const char* const W = "selftest_twoworlds";
        CMUSHclientDoc* docB = new CMUSHclientDoc();

        auto makeWindow = [&](CMUSHclientDoc* d, long fill) {
            return d->WindowCreate(W, 10, 10, 64, 48, 0,
                                   MINIWINDOW_ABSOLUTE_LOCATION | MINIWINDOW_TRANSPARENT,
                                   kRed) == eOK &&
                   d->WindowShow(W, TRUE) == eOK &&
                   d->WindowRectOp(W, 2 /*fill*/, 0, 0, 20, 20, fill, 0) == eOK;
        };
        ckCheck(makeWindow(doc,  kGreen), "two worlds: world A has a transparent window");
        ckCheck(makeWindow(docB, kBlue),  "two worlds: world B has one of its own");

        wx_miniwindow_mask_cache_reset();
        long builds = 0, hits = 0;

        auto composite = [&](CMUSHclientDoc* d, const wxColour& expect,
                             const char* what) {
            DestCanvas canvas(kW, kH, paneColour);
            wx_composite_miniwindows(d, canvas.dc, kW, kH, kW, kH, false);
            canvas.Finish();
            ckCheck(canvas.Is(15, 15, expect), what);
        };

        composite(doc,  greenColour, "two worlds: A draws its own green patch");
        composite(docB, blueColour,  "two worlds: B draws its own blue patch");
        wx_miniwindow_mask_cache_stats(builds, hits);
        ckCheck(builds == 2 && hits == 0,
                "two worlds: one build each, nothing reused yet");

        // Round two: neither canvas changed, so BOTH must come out of the
        // cache.  Before the fix each paint had just erased the other's entry,
        // so builds climbed for ever and hits stayed at zero.
        composite(doc,  greenColour, "two worlds: A still draws green");
        composite(docB, blueColour,  "two worlds: B still draws blue");
        wx_miniwindow_mask_cache_stats(builds, hits);
        ckCheck(builds == 2 && hits == 2,
                "two worlds: the second round is all cache hits -- one world's "
                "paint no longer evicts the other's mask");

        // A closing world hands its own entries back: the sweep will not
        // collect them any more, because it only looks at the painting doc's.
        wx_miniwindow_mask_cache_forget(docB);
        ckCheck(wx_miniwindow_mask_cache_size() == 1,
                "two worlds: closing B releases B's bitmap and keeps A's");

        delete docB;
        ckCheck(doc->WindowDelete(W) == eOK, "two worlds: A's window cleaned up");
        {
            DestCanvas canvas(kW, kH, paneColour);
            wx_composite_miniwindows(doc, canvas.dc, kW, kH, kW, kH, false);
            canvas.Finish();
        }
        ckCheck(wx_miniwindow_mask_cache_size() == 0,
                "two worlds: ...and A's own sweep still collects its entry");
        wx_miniwindow_mask_cache_reset();
    }

    delete doc;

    printf("PAL-COMPOSITE-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

#endif // !_WIN32
