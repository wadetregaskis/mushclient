// tests/win/test_split_win.cpp -- the scrollback split in the output view: when it shows,
// where, and which line a point in it means.  A real CMUSHView over a real (hidden) window,
// for GetClientRect; the painting was looked at in the real client.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "mushview.h"
#include "dpi.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// As in test_gap_coverage.cpp's ClickFixture: the document goes first, and takes the
// view off its list as it does.
struct SplitFixture
{
    CMUSHView *      view;
    CMUSHclientDoc * doc;
    HWND             hwnd;
    const unsigned int iSavedPref;
    const unsigned int iSavedPercent;

    explicit SplitFixture (const int iDpi = 96, const char * sFace = "Courier New",
                           const int iPoints = 10, const DWORD iCharset = ANSI_CHARSET)
      : view ((CMUSHView *) RUNTIME_CLASS (CMUSHView)->CreateObject ()),
        doc (new CMUSHclientDoc),
        hwnd (::CreateWindowA ("STATIC", "", WS_POPUP, 0, 0, 800, 400, NULL, NULL, NULL, NULL)),
        iSavedPref (App.m_bSplitScrollback),
        iSavedPercent (App.m_iSplitScrollbackPercent)
    {
        App.m_bSplitScrollback = 1;
        App.m_iSplitScrollbackPercent = 25;
        view->Attach (hwnd);
        doc->m_bUTF_8 = TRUE;
        doc->m_iConnectPhase = eConnectConnectedToMud;
        if (iDpi != 96)
            doc->m_iDpi = iDpi;     // (otherwise what the host says: 96, for an unaware process)
        doc->ChangeFont (iPoints, sFace, FW_NORMAL, iCharset, FALSE, FALSE, FALSE, 0);
        for (int i = 0; i < 200; i++)
        {
            char line [32];
            const int n = sprintf (line, "line %d\n", i);
            doc->OnBytesReceived (line, n);
        }
        doc->AddView (view);      // after the lines, or it would be asked to scroll to them
        const long iLineHeight = doc->m_FontHeight;
        view->m_ScrollbarSizeTotal = CSize (800, doc->GetLastLine () * iLineHeight);
        view->m_ScrollbarSizePage = CSize (800, 400);
        view->m_scroll_position = CPoint (0, iLineHeight * 10);   // scrolled well back
        view->m_freeze = TRUE;
    }
    ~SplitFixture ()
    {
        App.m_bSplitScrollback = iSavedPref;
        App.m_iSplitScrollbackPercent = iSavedPercent;
        view->Detach ();
        ::DestroyWindow (hwnd);
        delete doc;
        delete view;
    }

    int lineAt (const CPoint & point)
    {
        CClientDC dc (NULL);
        int line = -1, col = -1;
        view->calculate_line_and_column (point, dc, line, col, false);
        return line;
    }
};

}   // namespace

TEST(SplitScrollback, ShowsOnlyWhenFrozenAwayFromTheEnd)
{
    SplitFixture f;
    EXPECT_TRUE(f.view->IsSplit ());

    f.view->m_freeze = FALSE;
    EXPECT_FALSE(f.view->IsSplit ());       // following the output: nothing to split
    f.view->m_freeze = TRUE;

    App.m_bSplitScrollback = 0;
    EXPECT_FALSE(f.view->IsSplit ());       // the preference
    App.m_bSplitScrollback = 1;

    // at the end there's nothing below it to show
    f.view->m_scroll_position.y = f.view->m_ScrollbarSizeTotal.cy - f.view->m_ScrollbarSizePage.cy;
    EXPECT_FALSE(f.view->IsSplit ());
}

TEST(SplitScrollback, NotInAWindowTooSmallForIt)
{
    SplitFixture f;
    ::SetWindowPos (f.hwnd, NULL, 0, 0, 800, f.doc->m_FontHeight * 5, SWP_NOMOVE | SWP_NOZORDER);
    EXPECT_FALSE(f.view->IsSplit ());
}

TEST(SplitScrollback, TheStripIsWholeLinesAtTheBottomInItsShare)
{
    SplitFixture f;
    EXPECT_EQ(96, f.doc->m_iDpi);      // the host's doing: the numbers below are at 96 anywhere
    const CRect rect = f.view->SplitStripRect ();
    RECT client;
    ::GetClientRect (f.hwnd, &client);

    EXPECT_EQ(client.bottom, rect.bottom);
    EXPECT_EQ(client.left, rect.left);
    EXPECT_EQ(client.right, rect.right);

    const int iRows = (rect.Height () - 2) / f.doc->m_FontHeight;
    EXPECT_EQ(0, (rect.Height () - 2) % f.doc->m_FontHeight);
    EXPECT_EQ(::MulDiv (client.bottom, 25, 100) / f.doc->m_FontHeight, iRows);

    App.m_iSplitScrollbackPercent = 50;
    EXPECT_GT(f.view->SplitStripRect ().Height (), rect.Height ());
}

TEST(SplitScrollback, APointMeansTheLineThatIsDrawnThere)
{
    SplitFixture f;
    const CRect rect = f.view->SplitStripRect ();
    const long iHeight = f.doc->m_FontHeight;
    const long iScrollBefore = f.view->m_scroll_position.y;

    // above the bar: history, from where the view is scrolled to
    EXPECT_EQ(10, f.lineAt (CPoint (5, 2)));
    EXPECT_EQ(12, f.lineAt (CPoint (5, (int) iHeight * 2 + 2)));

    // below it: the end of the buffer
    const int iLast = f.doc->GetLastLine ();
    EXPECT_EQ(iLast - 1, f.lineAt (CPoint (5, rect.bottom - 2)));
    EXPECT_EQ(iLast - 2, f.lineAt (CPoint (5, rect.bottom - (int) iHeight - 2)));

    // what plugins read hasn't moved
    EXPECT_EQ(iScrollBefore, f.view->m_scroll_position.y);
    EXPECT_EQ(iScrollBefore, f.view->GetScrollPosition ().y);
}

TEST(SplitScrollback, TheSameCoordinatesWithoutIt)
{
    SplitFixture f;
    const CRect rect = f.view->SplitStripRect ();
    App.m_bSplitScrollback = 0;
    const long iHeight = f.doc->m_FontHeight;
    EXPECT_EQ((int) (f.view->m_scroll_position.y + rect.bottom - 2) / iHeight,
              f.lineAt (CPoint (5, rect.bottom - 2)));
}

TEST(SplitScrollback, TheBarIsFoundOnlyWhereItIs)
{
    SplitFixture f;
    const CRect rect = f.view->SplitStripRect ();
    EXPECT_TRUE(f.view->PointOnSplitBar (CPoint (100, rect.top)));
    EXPECT_FALSE(f.view->PointOnSplitBar (CPoint (100, rect.top - 20)));
    EXPECT_FALSE(f.view->PointOnSplitBar (CPoint (100, rect.top + 20)));
    EXPECT_TRUE(f.view->PointInSplitStrip (CPoint (100, rect.top + 20)));
    EXPECT_FALSE(f.view->PointInSplitStrip (CPoint (100, rect.top - 20)));
}

// ---------------------------------------------------------------------------------------
// Paused at the end, the strip waits until the view moves; Page Down and Page Up step by
// what can be seen of the history; and a drag of the bar ends with the mouse capture.

namespace {

// WindowProc is protected, and a view over an attached STATIC never gets messages sent
// to the window: this is the way in.
struct ViewProbe : public CMUSHView
{
    LRESULT Send (UINT message, WPARAM wParam, LPARAM lParam) { return WindowProc (message, wParam, lParam); }
};

// ScrollToPosition reads the scroll bar back, and a view finds its scroll bar through its
// parent (CView::GetScrollBarCtrl): this gives it one, an ordinary window.
struct ScrollFixture : public SplitFixture
{
    HWND parent;
    ScrollFixture ()
      : parent (::CreateWindowA ("STATIC", "", WS_POPUP, 0, 0, 900, 500, NULL, NULL, NULL, NULL))
    {
        ::SetWindowLongA (hwnd, GWL_STYLE, (::GetWindowLongA (hwnd, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
        ::SetParent (hwnd, parent);
    }
    ~ScrollFixture ()
    {
        ::SetParent (hwnd, NULL);
        ::SetWindowLongA (hwnd, GWL_STYLE, (::GetWindowLongA (hwnd, GWL_STYLE) & ~WS_CHILD) | WS_POPUP);
        ::DestroyWindow (parent);
    }
};

}   // namespace

TEST(SplitScrollback, PausedAtTheEndShowsNothingUntilTheViewMoves)
{
    ScrollFixture f;
    const long iHeight = f.doc->m_FontHeight;
    const long iEnd = f.view->m_ScrollbarSizeTotal.cy - f.view->m_ScrollbarSizePage.cy;

    f.view->m_freeze = FALSE;
    f.view->ScrollToPosition (CPoint (0, iEnd), false);      // following the output
    f.view->m_freeze = TRUE;                                  // Pause, at the end
    EXPECT_FALSE(f.view->IsSplit ());

    // output arrives: the end goes on down and the view stays where it is
    f.view->m_ScrollbarSizeTotal.cy += iHeight * 5;
    EXPECT_FALSE(f.view->IsSplit ());                         // the newest lines aren't covered

    // the user scrolls back a line: now it's scrolled back, and the strip shows
    f.view->ScrollToPosition (CPoint (0, iEnd - iHeight), false);
    f.view->m_freeze = TRUE;
    EXPECT_TRUE(f.view->IsSplit ());

    // and coming back to where it was paused doesn't hide it again
    f.view->ScrollToPosition (CPoint (0, iEnd), false);
    f.view->m_freeze = TRUE;
    EXPECT_TRUE(f.view->IsSplit ());
}

TEST(SplitScrollback, PageDownAndUpStepByWhatCanBeSeen)
{
    ScrollFixture f;
    ASSERT_TRUE(f.view->IsSplit ());
    const long iHeight = f.doc->m_FontHeight;
    const long iSeen = f.view->SplitStripRect ().top - f.view->GetTextRectangle ().top;
    ASSERT_TRUE(iSeen < f.view->GetOutputWindowHeight ());

    const long iBefore = f.view->m_scroll_position.y;
    f.view->doPagedown ();
    f.view->m_freeze = TRUE;
    // the last line that could be seen above the strip is now the first
    EXPECT_EQ(iBefore + iSeen - iHeight, f.view->m_scroll_position.y);
    f.view->doPageup ();
    EXPECT_EQ(iBefore, f.view->m_scroll_position.y);

    // without the strip, a whole window, as always
    App.m_bSplitScrollback = 0;
    f.view->doPagedown ();
    EXPECT_EQ(iBefore + f.view->GetOutputWindowHeight () - iHeight, f.view->m_scroll_position.y);
}

TEST(SplitScrollback, LosingTheMouseEndsADragOfTheBar)
{
    SplitFixture f;
    f.view->m_bSplitDragging = true;
    static_cast<ViewProbe *> (f.view)->Send (WM_CAPTURECHANGED, 0, (LPARAM) ::GetDesktopWindow ());
    EXPECT_FALSE(f.view->m_bSplitDragging);
}

TEST(SplitScrollback, ATooSmallWindowHasNoStripToKeepItPaused)
{
    SplitFixture f;
    EXPECT_TRUE(f.view->SplitPossible ());
    ::SetWindowPos (f.hwnd, NULL, 0, 0, 800, f.doc->m_FontHeight * 5, SWP_NOMOVE | SWP_NOZORDER);
    EXPECT_FALSE(f.view->SplitPossible ());    // did_jump catches up, as without the strip
}

// ---------------------------------------------------------------------------------------
// The output view at other scales, and what plugins are told of it.

TEST(OutputViewDpi, AutoWrapKeepsTheLineBufferAndItsSizeInStep)
{
    // docs/UPSTREAM-BUGS.md #100: this site reallocated the current line to the column
    // count, a quarter of a UTF-8 line's bytes, and left iMemoryAllocated saying the old size
    SplitFixture f;
    f.doc->m_bAutoWrapWindowWidth = TRUE;
    f.doc->m_font_name = "Courier New";
    f.doc->m_font_height = 10;
    f.doc->m_font_weight = FW_NORMAL;
    f.doc->m_font_charset = ANSI_CHARSET;
    // (output can't arrive here: this view has no frame to count new lines in its title)
    CLine * pLine = f.doc->m_pCurrentLine;
    ASSERT_TRUE(pLine != NULL);
    ASSERT_TRUE(pLine->iMemoryAllocated >= 7);
    memcpy (pLine->text, "partial", 7);
    pLine->len = 7;

    for (const int iWidth : { 800, 300, 1200 })
    {
        ::SetWindowPos (f.hwnd, NULL, 0, 0, iWidth, 400, SWP_NOMOVE | SWP_NOZORDER);
        f.view->AutoWrapWindowWidth (f.doc);
        EXPECT_EQ((int) f.doc->m_nWrapColumn * 4, pLine->iMemoryAllocated);
        EXPECT_EQ(0, memcmp (pLine->text, "partial", 7));
        // and all of what it says it has is there: the debug heap notices a byte past the end
        memset (pLine->text + 7, 'x', pLine->iMemoryAllocated - 7);
#if defined(_DEBUG)
        EXPECT_TRUE(_CrtCheckMemory () != FALSE);
#endif
    }
}

TEST(OutputViewDpi, AutoWrapMeasuresABitmapFontAsChangeFontMadeIt)
{
    // At 125% a bitmap font is drawn at its own size (Dpi_RasterFactor (120) is 1), and
    // ChangeFont asks for it at 96 dpi; auto-wrap asked at 120 and got another size.
    SplitFixture f (120, "Terminal", 12, OEM_CHARSET);
    ASSERT_TRUE(f.doc->m_bRasterOutputFont);
    ASSERT_EQ(1, f.doc->m_iRasterScale);
    f.doc->m_bAutoWrapWindowWidth = TRUE;
    f.doc->m_font_name = "Terminal";
    f.doc->m_font_height = 12;
    f.doc->m_font_weight = FW_NORMAL;
    f.doc->m_font_charset = OEM_CHARSET;
    f.doc->m_iPixelOffset = 0;

    f.view->AutoWrapWindowWidth (f.doc);
    const int iExpected = CMUSHclientDoc::ClampWrapColumn (
            (int) (f.view->GetOutputWindowWidth () - f.doc->PixelOffset ()) / f.doc->m_FontWidth);
    EXPECT_EQ(iExpected, (int) f.doc->m_nWrapColumn);

    // (and that it matters: the size asked for at 120 is another face of Terminal)
    CDC dc;
    dc.CreateCompatibleDC (NULL);
    CFont font;
    font.CreateFont (-MulDiv (12, 120, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, OEM_CHARSET, 0, 0, 0, FIXED_PITCH, "Terminal");
    CFont * pOld = dc.SelectObject (&font);
    TEXTMETRIC tm;
    dc.GetTextMetrics (&tm);
    dc.SelectObject (pOld);
    if (tm.tmAveCharWidth == f.doc->m_FontWidth)
        printf ("      (Terminal is the same width at both sizes here: the check above proves less)\n");
}

TEST(OutputViewDpi, TheTextRectangleIsInPluginPixels)
{
    SplitFixture f;
    for (const int iDpi : { 96, 144, 192 })
    {
        f.doc->m_iDpi = iDpi;
        VARIANT v = f.doc->GetInfo (292);
        EXPECT_EQ(::MulDiv (800, 96, iDpi), (int) v.lVal);
        VariantClear (&v);
        v = f.doc->GetInfo (293);
        EXPECT_EQ(::MulDiv (400, 96, iDpi), (int) v.lVal);
        VariantClear (&v);
        v = f.doc->GetInfo (290);
        EXPECT_EQ(0, (int) v.lVal);
        VariantClear (&v);
    }
}

// Two miniwindows side by side, at 125%: the first ends where the second starts.  The
// width used to be rounded on its own: logical 5 wide from 5 is real 6 wide from 6, and
// the next window starts at 13, so real pixel 12 was neither.
TEST(OutputViewDpi, TouchingMiniwindowsLeaveNoGap)
{
    SplitFixture f;
    f.doc->m_iDpi = 120;
    ASSERT_EQ(0, (int) f.doc->WindowCreate ("a", 0, 0, 5, 10, 4, 0, RGB (255, 0, 0)));
    ASSERT_EQ(0, (int) f.doc->WindowCreate ("b", 0, 0, 5, 10, 4, 0, RGB (0, 0, 255)));

    CDC dc;
    dc.CreateCompatibleDC (NULL);
    CBitmap bitmap;
    {
        CClientDC screen (NULL);
        bitmap.CreateCompatibleBitmap (&screen, 64, 16);
    }
    CBitmap * pOld = dc.SelectObject (&bitmap);
    dc.FillSolidRect (0, 0, 64, 16, RGB (0, 0, 0));

    CRect ra (5, 0, 0, 0), rb (10, 0, 0, 0);
    f.view->Blit_Bitmap (&dc, f.doc->m_MiniWindows ["a"]->GetDC (), 5, 10, 4, ra, true, false, 0);
    f.view->Blit_Bitmap (&dc, f.doc->m_MiniWindows ["b"]->GetDC (), 5, 10, 4, rb, true, false, 0);

    EXPECT_EQ(RGB (255, 0, 0), dc.GetPixel (6, 3));      // a starts at real 6
    EXPECT_EQ(RGB (0, 0, 0), dc.GetPixel (5, 3));
    EXPECT_EQ(RGB (255, 0, 0), dc.GetPixel (12, 3));     // and runs right up to b
    EXPECT_EQ(RGB (0, 0, 255), dc.GetPixel (13, 3));
    EXPECT_EQ(RGB (0, 0, 255), dc.GetPixel (18, 3));
    EXPECT_EQ(RGB (0, 0, 0), dc.GetPixel (19, 3));
    dc.SelectObject (pOld);
}

// A bitmap font keeps what it has, and only what it lacks goes to DirectWrite.
TEST(OutputViewDpi, ABitmapFontKeepsTheCharactersItHas)
{
    CDC dc;
    dc.CreateCompatibleDC (NULL);

    CFont fixedsys;
    fixedsys.CreateFont (-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, 0, FIXED_PITCH, "Fixedsys");
    CFont * pOld = dc.SelectObject (&fixedsys);
    TEXTMETRIC tm;
    dc.GetTextMetrics (&tm);
    ASSERT_EQ(0, (int) (tm.tmPitchAndFamily & (TMPF_VECTOR | TMPF_TRUETYPE)));   // really a bitmap font

    const char * const kHas [] = {
        "plain ASCII",
        "caf\xC3\xA9",                                       // Latin-1
        "\xE2\x82\xAC 5 \xE2\x80\x9Cquoted\xE2\x80\x9D",     // the euro and smart quotes, in 1252
    };
    for (const char * s : kHas)
        EXPECT_FALSE(CMUSHView::RunNeedsFallbackFont (dc.m_hDC, s, (int) strlen (s)));

    const char * const kHasNot [] = {
        "\xD0\x96\xD0\xB8\xD0\xB2",            // Cyrillic, not in 1252
        "\xE2\x94\x80\xE2\x94\x82",            // box drawing, not in 1252
        "\xF0\x9F\x98\x80",                    // an emoji
        "e\xCC\x81",                           // a letter and its mark, two code points
    };
    for (const char * s : kHasNot)
        EXPECT_TRUE(CMUSHView::RunNeedsFallbackFont (dc.m_hDC, s, (int) strlen (s)));

    // Terminal is an OEM font, but what GDI draws of it from Unicode is the ANSI page's
    // (GetFontUnicodeRanges says so, and GetGlyphIndicesW has no box drawing in it):
    // a UTF-8 world never got the line drawing it has for code page 437
    CFont terminal;
    terminal.CreateFont (-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, OEM_CHARSET, 0, 0, 0, FIXED_PITCH, "Terminal");
    dc.SelectObject (&terminal);
    EXPECT_FALSE(CMUSHView::RunNeedsFallbackFont (dc.m_hDC, "\xE2\x82\xAC \xE2\x80\x9Cquoted\xE2\x80\x9D", 16));
    EXPECT_TRUE(CMUSHView::RunNeedsFallbackFont (dc.m_hDC, "\xE2\x94\x80\xE2\x94\x82\xE2\x94\x8C", 9));
    EXPECT_TRUE(CMUSHView::RunNeedsFallbackFont (dc.m_hDC, "\xF0\x9F\x98\x80", 4));

    dc.SelectObject (pOld);
}
