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

    SplitFixture ()
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
        doc->ChangeFont (10, "Courier New", FW_NORMAL, ANSI_CHARSET, FALSE, FALSE, FALSE, 0);
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
