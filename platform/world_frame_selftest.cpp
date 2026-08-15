// platform/world_frame_selftest.cpp — in-app conformance test for the world
// window adapter (--world-frame-selftest).
//
// CWorldChildFrame is the wx pal::IWorldView: it is what makes world.Bookmark,
// world.GetWorldWindowPosition / MoveWorldWindow and world.SetTitle visible on
// screen.  None of it can be reached from the 30 headless mushcore executables
// (no wx, no frame), so it followed the miniwindow/engines selftests: build a
// REAL doc and a REAL world window inside the app, drive the real engine
// entry points, and read back the real widget state, before the GUI boots.
//
// Every check here is written so that "the code under test did nothing" FAILS:
// the geometry checks compare against the frame's own rect (not a tolerance),
// and the title checks bracket the interesting case with a control that only
// passes if the frame's connection-phase tick really ran.

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/mdi.h>
#include <wx/config.h>    // the child-geometry keys OnClose writes

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "childfrm.h"     // CChildFrame  ) the world doc template's
#include "MUSHview.h"     // CMUSHView    ) runtime classes
#include "resource.h"     // IDR_MUSHCLTYPE
#include "platform/app_config.h"   // OpenAppConfig -- never the raw wxConfig
#include <memory>
#include "mainfrm.h"      // Frame — doc-attributed status overloads
#include "platform/wx_world_child.h"
#include "platform/wx_outputpane.h"
#include "platform/selftest_support.h"   // plugin-loading fixture
#include "platform/wx_mfc_hooks.h"   // g_wxhook_message_box — answer the
                                     // clear-buffer confirmation prompt
#include "dialogs/TextAttributesDlg.h"    // Display ▸ Text Attributes
#include "dialogs/HighlightPhraseDlg.h"   // Display ▸ Highlight Word

#include <climits>        // LONG_MIN — "no saved child_x/child_y" sentinel below
#include <cstdio>
#include <vector>

// The world option table, so the font-size check below can assert against the
// declared range for output_font_height rather than a number copied into a test.
extern tConfigurationNumericOption OptionsTable [];

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

// Let the frame's 100 ms world timer run: the connection-phase block that
// maintains the title lives there.  Pumps for a bit over two ticks.
void pumpTicks(int ms = 350)
{
    const wxLongLong until = wxGetLocalTimeMillis() + ms;
    while (wxGetLocalTimeMillis() < until) {
        wxYield();
        wxMilliSleep(10);
    }
    wxYield();
}

bool sameRect(const pal::Rect& r, const wxRect& w)
{
    return r.x == w.x && r.y == w.y && r.w == w.width && r.h == w.height;
}

} // namespace

int RunWorldFrameSelfTest()
{
    setvbuf(stdout, NULL, _IONBF, 0);   // unbuffered: a crash must not eat the log
    printf("WORLD-FRAME-SELFTEST: driving the real CWorldChildFrame adapter\n");

    // InitInstance has not run (we exit before it), so give the app the world
    // registry its window furniture walks — the activity toolbar enumerates it
    // on every timer tick.  Same shim the engines selftest installs.
    if (App.m_pWorldDocTemplate == NULL)
        App.m_pWorldDocTemplate = new CMultiDocTemplate(
            IDR_MUSHCLTYPE,
            RUNTIME_CLASS(CMUSHclientDoc),
            RUNTIME_CLASS(CChildFrame),
            RUNTIME_CLASS(CMUSHView));

    // A world document with just enough state to display lines.  (Same recipe
    // the New World hook uses before OnNewDocument; no dialogs here.)
    CMUSHclientDoc* doc = new CMUSHclientDoc();
    doc->m_mush_name     = "SelfTest";
    doc->m_nWrapColumn   = 80;
    doc->m_bUTF_8        = 1;
    doc->m_maxlines      = 5000;
    doc->m_iConnectPhase = eConnectConnectedToMud;
    // OnNewDocument would do this (it also shows dialogs, which we must not):
    // allocate the line-position index and seed the first line, without which
    // nothing can be displayed.
    doc->SetUpOutputWindow();

    wxMDIParentFrame* parent =
        new wxMDIParentFrame(NULL, wxID_ANY, wxT("selftest"),
                             wxPoint(50, 50), wxSize(900, 700));
    CWorldChildFrame* child = new CWorldChildFrame(parent, doc);
    child->Show();
    parent->Show();
    wxYield();

    COutputPane* pane = child->OutputPane();
    check(pane != NULL, "the world window has an output pane");
    check(doc->m_pWorldView == child, "the frame is the doc's IWorldView");

    // ── output for the bookmark checks ─────────────────────────────────────
    doc->Note("line one");
    doc->Note("line two");
    doc->Note("line three");
    doc->UpdateAllViews(NULL);
    wxYield();

    check(doc->m_LineList.GetCount() >= 3,
          "three notes reached the engine's line list");
    // The pane holds the COMMITTED lines; the engine's list also carries the
    // in-progress one, so pane index i is engine line i + 1 for every line the
    // pane has.
    const int paneLines = pane->LineCount();
    check(paneLines >= 3, "and reached the output pane");

    // ── world.Bookmark draws ───────────────────────────────────────────────
    // The engine flag and what the pane renders used to be two disconnected
    // stores: world.Bookmark set the CLine flag and notified only the CMUSHView
    // family (never constructed here), so it drew nothing at all.
    {
        const int  idx = paneLines - 1;      // last committed line, 0-based
        const long n   = idx + 1;            // the same line, 1-based

        doc->Bookmark(n, TRUE);
        const SOutputLine* shown = pane->GetLine(idx);
        check(shown != NULL && shown->bookmark,
              "world.Bookmark marks the line in the output pane");
        check(doc->GetLineInfo(n, 7).boolVal != 0,
              "...and GetLineInfo(n, 7) agrees");

        doc->Bookmark(n, FALSE);
        shown = pane->GetLine(idx);
        check(shown != NULL && !shown->bookmark,
              "clearing it un-marks the line in the pane");
    }

    // ── Display > Bookmark Selection is the same store ─────────────────────
    // With no selection it toggles the last line (CMUSHView::
    // OnDisplayBookmarkselection).  It used to flip a pane-private flag that
    // scripts could not see; now it goes through CMUSHclientDoc::Bookmark.
    {
        const int  idx = pane->BookmarkTargetLine();
        const long n   = idx + 1;
        check(idx == paneLines - 1,
              "Bookmark Selection targets the last line when nothing is selected");

        child->BookmarkSelection();
        check(doc->GetLineInfo(n, 7).boolVal != 0,
              "Bookmark Selection is visible to GetLineInfo (scripts)");
        const SOutputLine* shown = pane->GetLine(idx);
        check(shown != NULL && shown->bookmark,
              "...and is drawn in the pane");

        child->BookmarkSelection();       // toggles back off
        check(doc->GetLineInfo(n, 7).boolVal == 0,
              "a second Bookmark Selection clears it again");
        shown = pane->GetLine(idx);
        check(shown != NULL && !shown->bookmark, "...in both stores");
    }

    // ── GetWorldWindowPosition / MoveWorldWindow describe one window ───────
    // The getter used to report the OUTPUT PANE's screen rect while the setter
    // moved the FRAME, so the standard save-and-restore idiom walked the window
    // down the screen and shrank it by the title bar + input bar on every trip.
    {
        child->SetSize(120, 140, 640, 480);
        wxYield();

        int x = 0, y = 0, w = 0, h = 0;
        check(doc->GetOutputRect(x, y, w, h),
              "GetWorldWindowPosition answers while a window is open");
        const pal::Rect got{ x, y, w, h };
        check(sameRect(got, child->GetRect()),
              "GetWorldWindowPosition reports the world window's own rect");

        // The round trip a plugin does at every world open must be a no-op.
        child->moveWorldWindow(x, y, w, h);
        wxYield();
        int x2 = 0, y2 = 0, w2 = 0, h2 = 0;
        doc->GetOutputRect(x2, y2, w2, h2);
        check(x2 == x && y2 == y && w2 == w && h2 == h,
              "MoveWorldWindow(GetWorldWindowPosition()) leaves it put");

        // …and MoveWorldWindow really moves it (so the check above cannot pass
        // by both calls being inert).
        child->moveWorldWindow(200, 220, 700, 500);
        wxYield();
        doc->GetOutputRect(x2, y2, w2, h2);
        check(x2 == 200 && y2 == 220 && w2 == 700 && h2 == 500,
              "MoveWorldWindow moves the window it reports");
    }

    // ── world.SetTitle survives a connection-phase change ──────────────────
    {
        // Control: with no script title the phase tick owns the title.  This
        // also proves the tick is running, so the survival check below cannot
        // pass vacuously.
        doc->SetTitle("");
        doc->m_iConnectPhase = eConnectNotConnected;
        pumpTicks();
        check(child->GetTitle() == wxT("SelfTest (Closed)"),
              "the phase tick titles an untitled world <name> (<phase>)");

        doc->SetTitle("Rincewind");
        check(child->GetTitle() == wxT("Rincewind"),
              "world.SetTitle reaches the window title");

        doc->m_iConnectPhase = eConnectConnectedToMud;   // a phase CHANGE
        pumpTicks();
        check(child->GetTitle() == wxT("Rincewind"),
              "...and survives a connection-phase change");

        doc->m_iConnectPhase = eConnectDisconnecting;
        pumpTicks();
        check(child->GetTitle() == wxT("Rincewind"),
              "...and a disconnect");

        // Clearing it hands the title back to the phase (second control: the
        // tick is still live at this point).
        doc->SetTitle("");
        doc->m_iConnectPhase = eConnectNotConnected;
        pumpTicks();
        check(child->GetTitle() == wxT("SelfTest (Closed)"),
              "clearing the script title restores <name> (<phase>)");
    }

    // ── a legacy-charset world name still titles the window (2026-08-12) ────
    // "Caf\xE9" is CP1252/Latin-1, not valid UTF-8.  The title path decoded
    // with bare all-or-nothing FromUTF8, so the whole composed title became ""
    // and the phase tick SET an empty title bar for any world migrated from a
    // Windows install with an accented name.  DecodeTextLenient reads it as
    // Latin-1 instead.
    {
        doc->m_mush_name = "Caf\xE9";
        doc->m_iConnectPhase = eConnectConnectedToMud;   // force a re-title
        pumpTicks();
        check(!child->GetTitle().empty(),
              "a legacy-charset world name still yields a title (was: empty)");
        check(child->GetTitle().Contains(wxT("Caf")),
              "...containing the name's ASCII prefix");

        // Restore the fixture for the blocks below.
        doc->m_mush_name = "SelfTest";
        doc->m_iConnectPhase = eConnectNotConnected;
        pumpTicks();
        check(child->GetTitle() == wxT("SelfTest (Closed)"),
              "fixture restored after the charset check");
    }

    // ── doc-attributed status: a background world cannot stomp the bar ──────
    // (2026-08-12)  Frame.SetStatusMessage resolved its target purely by
    // GetActiveChild(), so a BACKGROUND world's "Connecting to B, port 4000"
    // landed on the ACTIVE world's status bar and stuck there.  The per-doc
    // callers in doc_shared.cpp now attribute their doc, and the new hook
    // delivers to that doc's own frame.  App-scoped messages (no doc) keep
    // the old active-child route — the control.
    {
        CMUSHclientDoc* bg = new CMUSHclientDoc();
        bg->m_mush_name = "zz_selftest_bgstatus";
        bg->m_nWrapColumn = 80;
        bg->m_maxlines = 5000;
        bg->SetUpOutputWindow();
        // (doc ctor already ran AddDocument -- doc_construct.cpp:521;
        //  a second add left a DANGLING duplicate entry after close)
        CWorldChildFrame* bgFrame =
            new CWorldChildFrame(parent, bg, wxT("zz_selftest_bgstatus"));

        // hook_set_status resolves the active child through GetTopWindow():
        // true in the booted app, not in this harness — point it at the
        // fixture's MDI parent for the duration of the block.
        wxWindow* savedTop = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
        if (wxTheApp) wxTheApp->SetTopWindow(parent);
        // ...and the 1-arg SetStatusMessage no-ops while App.m_pMainWnd is
        // null (true only pre-InitInstance, i.e. in this harness).
        CWnd* savedMainWnd = App.m_pMainWnd;
        if (!App.m_pMainWnd) App.m_pMainWnd = &Frame;

        // Make the MAIN fixture the active child again, with a known pane 0.
        child->Activate();
        wxYield();
        child->SetTransientStatus(wxT("A's own status"));

        // The background doc announces a connection — attributed to ITSELF.
        Frame.SetStatusMessageNow(bg, CString("Connecting to B, port 4000"));
        check(child->StatusPane0() == wxT("A's own status"),
              "a background world's status does NOT stomp the active bar");
        check(bgFrame->StatusPane0() == wxT("Connecting to B, port 4000"),
              "...it lands on the background world's own bar");

        // Control: an app-scoped message (no doc) still reaches the ACTIVE
        // frame — proving the assertion above discriminates attribution, not
        // a broken status path.  MDI activation under a test harness is
        // asynchronous, so ask wx who IS active rather than assuming.
        Frame.SetStatusMessageNow(CString("app-scoped message"));
        auto* active = dynamic_cast<CWorldChildFrame*>(parent->GetActiveChild());
        check(active && active->StatusPane0() == wxT("app-scoped message"),
              "an unattributed message still lands on the active bar");

        App.m_pMainWnd = savedMainWnd;
        if (wxTheApp) wxTheApp->SetTopWindow(savedTop);
        bgFrame->Close(true);      // forced: CanVeto() is false
        wxYield();
    }

    // ── auto_resize_command_window sizes the input bar (2026-08-12) ─────────
    // Windows keeps the command area between auto_resize_minimum_lines and
    // auto_resize_maximum_lines (CSendView::AdjustCommandWindowSize); the
    // port hardcoded 1..8 and read none of the three options.  The option
    // poll on the world timer applies changes; the floor must lift the bar
    // even with a single line of content.
    {
        wxTextCtrl* inp = child->ActiveInputPublic();
        check(inp != nullptr, "auto-resize: the input bar exists");
        const int before = inp ? inp->GetMinSize().y : 0;

        doc->m_bAutoResizeCommandWindow = TRUE;
        doc->m_iAutoResizeMinimumLines  = 3;
        doc->m_iAutoResizeMaximumLines  = 6;
        pumpTicks();
        const int floored = inp ? inp->GetMinSize().y : 0;
        check(inp && floored >= before + 2 * inp->GetCharHeight(),
              "a 3-line floor lifts the bar by two lines (was: fixed 1 line)");

        doc->m_bAutoResizeCommandWindow = FALSE;
        pumpTicks();
        const int off = inp ? inp->GetMinSize().y : 0;
        check(inp && off < floored,
              "turning the option off shrinks the bar back (control)");
    }

    // ── line preambles (timestamps) — the nine timestamp_* options ──────────
    // (2026-08-12)  Windows prefixes every line with a per-kind formatted
    // preamble (CMUSHView::OnDraw); the port read none of the nine options —
    // the bundled Timestamps.xml plugin was a silent no-op.  The paint path
    // cannot run in this harness, so the assertions drive the SAME callback
    // the paint uses, plus the public PreambleCells/PointToChar arithmetic
    // that keeps click-to-column exact.
    {
        doc->ClearOutput();
        doc->m_strOutputLinePreambleOutput = "[TS] ";        // 5 cells
        doc->m_strOutputLinePreambleNotes  = "note> ";       // 6 cells
        doc->m_strOutputLinePreambleInput  = "";             // input: off
        doc->DisplayMsg("0123456789012345678901234567890123456789\n", 41, 0);
        doc->UpdateAllViews(NULL);
        wxYield();

        check((bool) pane->m_fn_line_preamble,
              "preamble: the frame wired the callback");

        // Kind selection, driven exactly as the paint drives it.
        COutputPane::SLinePreamble pre;
        SOutputLine mud;  mud.arrived = time(nullptr);
        SOutputLine note; note.arrived = mud.arrived; note.comment = true;
        SOutputLine inp;  inp.arrived = mud.arrived;  inp.userInput = true;
        check(pane->m_fn_line_preamble &&
                  pane->m_fn_line_preamble(mud, 0, true, pre) &&
                  pre.text == wxT("[TS] "),
              "preamble: an output line gets the output preamble");
        check(pane->m_fn_line_preamble(note, 0, true, pre) &&
                  pre.text == wxT("note> "),
              "preamble: a Note line gets the notes preamble");
        check(!pane->m_fn_line_preamble(inp, 0, true, pre),
              "preamble: an empty option means none for that kind");

        // Format codes: %H:%M:%S via FormatTime, %e elapsed, %D delta.
        doc->m_strOutputLinePreambleOutput = "%H:%M:%S ";
        check(pane->m_fn_line_preamble(mud, 0, true, pre) &&
                  pre.text.length() == 9 && !pre.text.Contains(wxT("%")),
              "preamble: strftime codes expand (HH:MM:SS + space = 9 cells)");
        doc->m_strOutputLinePreambleOutput = "%D";
        check(pane->m_fn_line_preamble(mud, mud.arrived - 3, false, pre) &&
                  pre.text.Contains(wxT("3.000000")),
              "preamble: %D is the delta from the previous line");

        // Click-to-column: with a preamble on, the same pixel resolves 5
        // columns EARLIER (the stamp occupies cells).  Comparing on/off at
        // one pixel avoids caring what the char width is.
        doc->m_strOutputLinePreambleOutput = "[TS] ";
        // Line 0 is the MUD line: ClearOutput's seed line is consumed by the
        // first DisplayMsg (the click test below confirms it independently).
        check(pane->PreambleCells(0) == 5,
              "preamble: PreambleCells reports the stamp width");

        // A REAL echoed-command line carries USER_INPUT through
        // BuildOutputLine's userInput stamp — the hand-built structs above
        // cannot see that stamp, so this is what pins it.  Different widths
        // (4 vs 5) make a dropped stamp fall to the output preamble and fail.
        doc->m_strOutputLinePreambleInput = "in> ";           // 4 cells
        doc->DisplayMsg("look\n", 5, USER_INPUT);
        doc->UpdateAllViews(NULL);
        wxYield();
        check(pane->PreambleCells(1) == 4,
              "preamble: a real echoed command gets the INPUT preamble");
        doc->m_strOutputLinePreambleInput = "";
        const SelPoint on  = pane->PointToChar(200, 2);
        doc->m_strOutputLinePreambleOutput = "";
        const SelPoint off = pane->PointToChar(200, 2);
        check(off.col - on.col == 5,
              "preamble: click-to-column shifts by exactly the stamp width");

        // The DRAW itself, pixel-proven: render line 0 into a memory DC
        // exactly as OnPaint would (RenderLineForTest — no paint can run
        // here).  A loud red stamp background must reach the pixels, and must
        // NOT when the option is off.  This is the arm the fade bug taught us
        // to cover: callback green, screen unchanged.
        doc->m_strOutputLinePreambleOutput = "[TS] ";
        const COLORREF savedBack = doc->m_OutputLinePreambleOutputBackColour;
        doc->m_OutputLinePreambleOutputBackColour = RGB(255, 0, 0);
        auto stampPixelRed = [&]() -> bool {
            wxBitmap bmp(400, 40, 32);
            wxMemoryDC mdc(bmp);
            mdc.SetBackground(*wxBLACK_BRUSH);
            mdc.Clear();
            pane->RenderLineForTest(mdc, 0, 0);
            mdc.SelectObject(wxNullBitmap);
            wxImage img = bmp.ConvertToImage();
            return img.GetRed(2, 2) > 200 && img.GetGreen(2, 2) < 60;
        };
        check(stampPixelRed(),
              "preamble draw: the stamp's red background reaches the PIXELS");
        doc->m_strOutputLinePreambleOutput = "";
        check(!stampPixelRed(),
              "preamble draw: ...and not when the option is off (control)");
        doc->m_OutputLinePreambleOutputBackColour = savedBack;

        doc->m_strOutputLinePreambleOutput = "";
        doc->m_strOutputLinePreambleNotes  = "";
        doc->ClearOutput();
    }

    // ── File ▸ Print Selection (2026-08-12) ─────────────────────────────────
    // A separate menu item on the port (Windows puts the choice on its print
    // dialog, which the preview frame replaces).  BuildPrintLines is the
    // testable half: full-buffer and selection-clipped builds, with the clip
    // in the seam's BYTE space — the multibyte fixture proves a stamp cannot
    // split a character.
    {
        doc->m_bAutoWrapWindowWidth = false;
        doc->ClearOutput();
        doc->DisplayMsg("alpha bravo charlie\n", 20, 0);
        doc->DisplayMsg("delta echo foxtrot\n", 19, 0);
        doc->DisplayMsg("golf hotel india\n", 17, 0);
        doc->UpdateAllViews(NULL);
        wxYield();

        std::vector<SPrintLine> all;
        check(child->BuildPrintLines(false, all) && all.size() >= 3,
              "print: the full-buffer build sees every line");

        // No selection: the selection build refuses (the item is greyed too).
        pane->SetSelectionRange(0, 0, 0, 0);
        std::vector<SPrintLine> none;
        check(!child->HasOutputSelection() &&
                  !child->BuildPrintLines(true, none),
              "print: no selection means no selection build");

        // Select "bravo char" — line 1, byte cols 7..17 (1-based, end-excl).
        doc->SetSelection(1, 1, 7, 17);
        check(child->HasOutputSelection(),
              "print: the selection is visible to the menu rule");
        std::vector<SPrintLine> sel;
        check(child->BuildPrintLines(true, sel) && sel.size() == 1,
              "print: a one-line selection builds one line");
        wxString joined;
        if (!sel.empty())
            for (const SPrintRun& r : sel[0].runs) joined += r.text;
        check(joined == wxT("bravo char"),
              "print: ...clipped to exactly the selected bytes");

        // A multi-line selection: from "echo" (line 2 col 7) through
        // "hotel" (line 3 col 11) — first line clipped left, last clipped
        // right, and nothing from line 1.
        doc->SetSelection(2, 3, 7, 11);
        std::vector<SPrintLine> multi;
        check(child->BuildPrintLines(true, multi) && multi.size() == 2,
              "print: a two-line selection builds two lines");
        wxString first, last;
        if (multi.size() == 2) {
            for (const SPrintRun& r : multi[0].runs) first += r.text;
            for (const SPrintRun& r : multi[1].runs) last  += r.text;
        }
        check(first == wxT("echo foxtrot") && last == wxT("golf hotel"),
              "print: ...first line clipped left, last clipped right");

        pane->SetSelectionRange(0, 0, 0, 0);
        doc->ClearOutput();
    }

    // ── world.SetScroll scrolls to the PIXEL, not a row-mistaken-for-a-line ─
    // (2026-08-12)  setScroll divided the documented device-pixel position by
    // the line height — a visual-ROW ordinal — and passed it to ScrollToLine,
    // which expects a LOGICAL line and re-maps through RowStartOf.  With any
    // wrapped line above the target the view landed below the requested
    // pixel, so the documented GetInfo(296) -> SetScroll round-trip drifted.
    // The fixture's long line wraps into several visual rows on purpose: with
    // only short lines, row == line and the old code passes (the degenerate-
    // fixture trap the conformance script fell into).
    {
        doc->m_bAutoWrapWindowWidth = false;
        doc->ClearOutput();
        std::string longLine(500, 'x');
        longLine += "\n";
        doc->DisplayMsg(longLine.c_str(), (long) longLine.size(), 0);
        for (int i = 0; i < 30; ++i) {
            char buf[32];
            const int n = snprintf(buf, sizeof buf, "short %02d\n", i);
            doc->DisplayMsg(buf, n, 0);
        }
        doc->UpdateAllViews(NULL);
        wxYield();

        pane->ScrollToLine(10);
        const long p = pane->GetScrollPositionY();
        check(p > 0, "setScroll: the fixture really scrolled somewhere");

        child->setScroll(p);           // the PAL seam under test
        check(pane->GetScrollPositionY() == p,
              "setScroll: the pixel round-trip is exact (was: drifted below "
              "by the long line's wrap rows)");

        child->setScroll(-1);          // control: -1 resumes the bottom
        check(pane->IsAutoScrolling(),
              "setScroll(-1) resumes auto-scroll (control)");
        doc->ClearOutput();
    }

    // ── world.TextRectangle decorations paint (2026-08-12) ──────────────────
    // The five decoration parameters (border offset/colour/width, outside
    // fill colour/style) round-tripped while painting nothing — the consumer
    // was CMUSHView::OnDraw.  Pixel-proven through the render probe: border
    // red on the inflated edge, fill blue outside, and the BS_NULL control
    // leaves the outside at the pane background.
    {
        // TextRectangle(l, t, r, b, offset, borderColour, width,
        //               fillColour, fillStyle)
        check(doc->TextRectangle(40, 40, 300, 200, 2, RGB(255, 0, 0), 3,
                                 RGB(0, 0, 255), 0 /*solid*/) == 0 /*eOK*/,
              "textrect: world.TextRectangle accepts the decoration set");

        auto probe = [&](int x, int y, unsigned char& rr, unsigned char& gg,
                         unsigned char& bb) {
            wxBitmap bmp(400, 300, 32);
            wxMemoryDC mdc(bmp);
            mdc.SetBackground(*wxBLACK_BRUSH);
            mdc.Clear();
            pane->RenderTextRectDecorForTest(mdc, 400, 300, *wxBLACK);
            mdc.SelectObject(wxNullBitmap);
            wxImage img = bmp.ConvertToImage();
            rr = img.GetRed(x, y); gg = img.GetGreen(x, y); bb = img.GetBlue(x, y);
        };

        unsigned char rr, gg, bb;
        probe(5, 5, rr, gg, bb);           // well outside the rect: the fill
        check(bb > 200 && rr < 60, "textrect: outside fill is blue");
        // the border sits on the rect inflated by offset+width (5px): the
        // stroke is centred on the inflated rect edge at 35
        probe(35, 120, rr, gg, bb);
        check(rr > 200 && bb < 60, "textrect: the border strokes red");
        probe(150, 120, rr, gg, bb);       // inside the reserved area
        check(rr < 60 && gg < 60 && bb < 60,
              "textrect: the text area returns to the pane background");

        // Control: BS_NULL (1) fills nothing — outside stays background.
        check(doc->TextRectangle(40, 40, 300, 200, 2, RGB(255, 0, 0), 3,
                                 RGB(0, 0, 255), 1 /*BS_NULL*/) == 0 /*eOK*/,
              "textrect: the null-fill control applies");
        probe(5, 5, rr, gg, bb);
        check(rr < 60 && gg < 60 && bb < 60,
              "textrect: BS_NULL leaves the outside at the background");

        doc->TextRectangle(0, 0, 0, 0, 0, 0, 0, 0, 1);   // clear
    }

    // ── caret vs containment hit-testing (2026-08-12) ───────────────────────
    // XToChar rounded to the NEAREST cell boundary — right for placing a
    // selection caret, wrong for containment: link firing, the hover hand
    // cursor and MXP hints all shifted half a character left (clicking the
    // right half of the character BEFORE a link fired it).  Windows keeps the
    // two apart with calculate_line_and_column's bHalfWay flag; the pane now
    // has the same split.  A pixel in the RIGHT half of cell 2 must answer
    // col 3 as a caret and col 2 as containment.
    {
        doc->ClearOutput();
        doc->DisplayMsg("ABCDEF\n", 7, 0);
        doc->UpdateAllViews(NULL);
        wxYield();

        const int cw = pane->CharWidth();
        check(cw > 0, "hit-test: the pane has a character width");
        const int x = 2 * cw + (3 * cw) / 4;   // right half of cell 2 ('C')
        const SelPoint caret = pane->PointToChar(x, 2, true);
        const SelPoint cont  = pane->PointToChar(x, 2, false);
        check(caret.line == 0 && caret.col == 3,
              "hit-test: caret rounding answers the nearest boundary (3)");
        check(cont.line == 0 && cont.col == 2,
              "hit-test: containment answers the clicked cell (2)");
        doc->ClearOutput();
    }

    // ── wrap_input rebuilds the bar and wraps typed text (2026-08-13) ───────
    // The world option was a dead reader (CSendView::UpdateWrap).  The wrap
    // mode is a creation-time style on wxOSX, so the toggle rebuilds the
    // control through the same BuildInputBar the constructor uses — text,
    // caret and bindings must survive the swap.
    {
        wxTextCtrl* before = child->ActiveInputPublic();
        check(before != nullptr, "wrap_input: the bar exists");
        before->SetValue(wxT("carried text"));
        before->SetInsertionPoint(7);

        doc->m_bAutoWrapInput = TRUE;
        pumpTicks();
        wxTextCtrl* on = child->ActiveInputPublic();
        check(on != nullptr && on != before,
              "wrap_input: the toggle rebuilt the control");
        check(on && on->GetValue() == wxT("carried text"),
              "...the text carried across");
        check(on && on->GetInsertionPoint() == 7,
              "...and the caret");
        // The discriminating pair (wxOSX does not retain wxTE_DONTWRAP in
        // the readable style flags, so the assertion is BEHAVIOURAL): the
        // same 300-char single command grows the bar to several display
        // rows with wrap on, and stays one line with wrap off.
        const std::string longcmd(300, 'k');
        const int oneRowH = on->GetMinSize().y;
        on->SetValue(wxString(longcmd));
        check(on->GetMinSize().y >= oneRowH + 2 * on->GetCharHeight(),
              "...a long command grows the bar by display rows (was: 1 line)");

        doc->m_bAutoWrapInput = FALSE;
        pumpTicks();
        wxTextCtrl* off = child->ActiveInputPublic();
        check(off != nullptr && off != on,
              "wrap_input: toggling off rebuilds again");
        check(off && off->GetValue() == wxString(longcmd),
              "...still carrying the text");
        const int offH = off->GetMinSize().y;
        check(offH < oneRowH + 2 * off->GetCharHeight(),
              "...and the non-wrapping bar stays one line (control)");

        // The TEXT binding survived the rebuilds: three hard lines must grow
        // the bar (the wxEVT_TEXT → UpdateInputBarHeight chain).
        const int oneLineH = off->GetMinSize().y;
        off->SetValue(wxT("a\nb\nc"));
        check(off->GetMinSize().y > oneLineH,
              "...the TEXT binding survived the rebuilds (bar grew)");
        off->SetValue(wxEmptyString);
    }

    // ── world slot numbers stay dense across a close (2026-08-12) ───────────
    // Windows renumbers m_view_number from OnIdle; the port never pumped it,
    // so closing world B of three left a dead toolbar slot and WorldInSlot's
    // lazy fallback could hand a NEW world a number an existing one already
    // held.  Renumbering now runs at the close moment (and the toolbar tick).
    // Three worlds on purpose — with fewer, lazy and dense numbering coincide.
    {
        CMUSHclientDoc* w2 = new CMUSHclientDoc();
        w2->m_mush_name = "zz_selftest_slot2"; w2->m_nWrapColumn = 80;
        w2->m_maxlines = 5000; w2->SetUpOutputWindow();
        // (doc ctor already ran AddDocument -- doc_construct.cpp:521;
        //  a second add left a DANGLING duplicate entry after close)
        CWorldChildFrame* f2 = new CWorldChildFrame(parent, w2, wxT("slot2"));
        CMUSHclientDoc* w3 = new CMUSHclientDoc();
        w3->m_mush_name = "zz_selftest_slot3"; w3->m_nWrapColumn = 80;
        w3->m_maxlines = 5000; w3->SetUpOutputWindow();
        // (doc ctor already ran AddDocument -- doc_construct.cpp:521;
        //  a second add left a DANGLING duplicate entry after close)
        CWorldChildFrame* f3 = new CWorldChildFrame(parent, w3, wxT("slot3"));

        App.UpdateWorldCounts();
        check(doc->m_view_number == 1 && w2->m_view_number == 2 &&
                  w3->m_view_number == 3,
              "three worlds number densely 1..3");

        // Close the MIDDLE world; the close path itself must renumber.
        f2->Close(true);
        wxYield();
        check(doc->m_view_number == 1 && w3->m_view_number == 2,
              "closing the middle world renumbers the survivors (was: gap)");

        f3->Close(true);
        wxYield();
        check(doc->m_view_number == 1,
              "...and the fixture world is number 1 again");
    }

    // ── activating the window marks its lines read ─────────────────────────
    // Windows does this in CMUSHView/CSendView::OnActivateView; those views do
    // not exist here, so nothing ever reset the counter and every world stayed
    // "has new messages" from its first line onwards.
    {
        doc->SetWorldViewActive(false);
        doc->m_new_lines = 0;
        doc->Note("something happened");
        doc->UpdateAllViews(NULL);
        check(doc->m_new_lines > 0,
              "output in a background world counts as unread");

        wxActivateEvent act(wxEVT_ACTIVATE, true, child->GetId());
        act.SetEventObject(child);
        child->GetEventHandler()->ProcessEvent(act);
        check(doc->IsWorldViewActive(),
              "activating the world window marks it active");
        check(doc->m_new_lines == 0,
              "...and marks its unread lines read");

        wxActivateEvent deact(wxEVT_ACTIVATE, false, child->GetId());
        deact.SetEventObject(child);
        child->GetEventHandler()->ProcessEvent(deact);
        check(!doc->IsWorldViewActive(),
              "deactivating it marks it inactive again");
    }

    // ── world.TextRectangle reaches the ENGINE wrap column, not just the paint ─
    // EffectiveTextCols (wx_world_child.cpp) is the other consumer of
    // pal::textRectWidth, and it is the one that sets m_nWrapColumn and the NAWS
    // width.  It used to compute `tr.right - tr.left` raw, which goes NEGATIVE
    // for the documented offset-from-edge form (right <= 0 means "that many
    // pixels in from the right edge"), so the reservation was silently dropped
    // and the MUD was told the full pane width.  The pane's own selftest pins
    // the VISUAL half of the same helper; this pins the engine half, through the
    // real frame, so the two cannot be fixed apart.
    {
        const int charW = pane->CharWidth();
        int paneW = 0, paneH = 0;
        pane->GetClientSize(&paneW, &paneH);
        check(charW > 0 && paneW > charW * 4,
              "wrap: the pane has a measurable font and width");

        // The wrap column only follows the window when the world asks it to;
        // without this the whole path returns "leave it alone" (doc_shared.cpp
        // AutoWrapColumnFor) and the assertions below would be vacuous.
        doc->m_bAutoWrapWindowWidth = true;
        doc->m_iPixelOffset         = 0;

        doc->m_TextRectangle = CRect(0, 0, 0, 0);      // no rectangle
        check(child->ApplyWrapColumnAndNAWS(), "wrap: recomputed with no rectangle");
        const int fullCols = (int) doc->m_nWrapColumn;
        check(fullCols == paneW / charW,
              "wrap: with no rectangle the engine wraps at the full pane width");

        // Reserve the rightmost 200 pixels, the way a docked miniwindow does.
        doc->m_TextRectangle = CRect(0, 0, -200, 0);
        check(child->ApplyWrapColumnAndNAWS(), "wrap: recomputed with a reserved edge");
        const int reservedCols = (int) doc->m_nWrapColumn;
        check(reservedCols == (paneW - 200) / charW,
              "wrap: -200 right edge narrows the ENGINE wrap column too");
        check(reservedCols < fullCols,
              "...and that is genuinely narrower than the unreserved pane");

        // An absolute right edge is still taken literally, so the fix cannot be
        // "always treat right as an offset".
        doc->m_TextRectangle = CRect(0, 0, 300, 0);
        check(child->ApplyWrapColumnAndNAWS(), "wrap: recomputed with an absolute edge");
        check((int) doc->m_nWrapColumn == 300 / charW,
              "wrap: a positive right edge is an absolute position, not an offset");

        doc->m_TextRectangle        = CRect(0, 0, 0, 0);
        doc->m_bAutoWrapWindowWidth = false;
        doc->m_nWrapColumn          = 80;
    }

    // ── A palette change recolours in place; it does not re-emit ─────────────
    // The old path was CMUSHclientDoc::RebuildOutputView: clear the pane, rewind
    // the emission cursor, re-materialise every line/run/std::string, rebuild
    // the whole wrap cache.  O(buffer) to change two integers per run, on a path
    // world.SetNormalColour reaches from a script, and unbounded once
    // max_output_lines is 0.
    //
    // Three observable consequences of NOT re-emitting are asserted here,
    // because each was previously papered over with a workaround: the line
    // objects survive (so an open session log gains no second copy of the
    // buffer), the scroll position survives, and the colours actually change.
    {
        doc->m_bAutoWrapWindowWidth = false;
        // Reset through the DOC, not the pane.  Under the pull model the pane
        // holds no lines of its own, so a pane-only clear leaves the engine's
        // buffer intact and the two disagree about how many lines exist.
        doc->ClearOutput();

        // An ANSI-coloured line (index 2 = green) and an RGB one.  Only the
        // first may move when the palette does.
        doc->DisplayMsg("\x1b[32mansi green\x1b[0m\n", 20, 0);
        doc->UpdateAllViews(NULL);
        wxYield();

        const int   lines0 = pane->LineCount();
        check(lines0 >= 1, "palette: a coloured line reached the pane");
        const SOutputLine* l0 = pane->GetLine(0);
        check(l0 != NULL && !l0->empty(), "palette: ...with at least one run");
        const unsigned long before = l0 ? l0->front().fg : 0;

        // Count how many lines the log hook sees from here on.  A re-emit would
        // push the whole buffer through it again; an in-place recolour pushes
        // nothing.
        static int s_logged = 0;
        s_logged = 0;
        pane->m_fn_on_line_appended = [](const SOutputLine&) { ++s_logged; };

        // Move ANSI colour 2 to something unmistakable and let the poll see it.
        doc->m_normalcolour[2] = RGB(1, 2, 3);
        pumpTicks(400);

        const SOutputLine* l1 = pane->GetLine(0);
        check(l1 != NULL && !l1->empty(), "palette: the line is still there");
        check(pane->LineCount() == lines0,
              "palette: the line count is unchanged (no re-emit)");
        check(s_logged == 0,
              "palette: no line was re-appended, so an open log gains no copy");
        check(l1 && l1->front().fg != before,
              "palette: ...but the colour really changed");
        check(l1 && l1->front().fg == (unsigned long) RGB(1, 2, 3),
              "palette: ...to the new palette entry");

        pane->m_fn_on_line_appended = nullptr;
        doc->m_normalcolour[2] = RGB(0, 128, 0);   // put it back
        pumpTicks(250);
        // Reset through the DOC, not the pane.  Under the pull model the pane
        // holds no lines of its own, so a pane-only clear leaves the engine's
        // buffer intact and the two disagree about how many lines exist.
        doc->ClearOutput();
    }

    // ── Unlimited scrollback reaches the PANE, not just the engine ───────────
    // max_output_lines = 0 has to be pushed through to COutputPane::SetMaxLines,
    // and the pane's own floor (n < 100 -> 100) must not clamp the sentinel —
    // that would have made "no limit" the smallest buffer in the app.  Headless
    // tests cover the engine half; only this can see the pane.
    {
        const long savedMax = doc->m_maxlines;

        doc->m_maxlines = 300;             // a real cap first (the option's
        pane->SetMaxLines(300);            //  floor is 200)
        for (int i = 0; i < 800; ++i)
            doc->Note("bounded line");
        doc->UpdateAllViews(NULL);
        wxYield();
        check(pane->LineCount() <= 300,
              "a bounded cap trims the pane");

        // Reset through the DOC, not the pane.  Under the pull model the pane
        // holds no lines of its own, so a pane-only clear leaves the engine's
        // buffer intact and the two disagree about how many lines exist.
        doc->ClearOutput();
        doc->m_maxlines = 0;               // unlimited
        pane->SetMaxLines(0);
        for (int i = 0; i < 400; ++i)
            doc->Note("unlimited line");
        doc->UpdateAllViews(NULL);
        wxYield();
        check(pane->LineCount() >= 400,
              "...and 0 keeps every line, rather than clamping to the floor");

        doc->m_maxlines = savedMax;
        pane->SetMaxLines((int) savedMax);
        // Reset through the DOC, not the pane.  Under the pull model the pane
        // holds no lines of its own, so a pane-only clear leaves the engine's
        // buffer intact and the two disagree about how many lines exist.
        doc->ClearOutput();
    }

    // ── Display ▸ Clear to Start (⌘K) keeps the prompt block ─────────────────
    // The engine side is covered headlessly (Utilities.ClearToStart*); what
    // only runs here is the GUI half: DeleteHeadLines has to drive the pane
    // through DeleteOutputHeadLines and wind m_gui_lines_emitted back by the
    // same amount, or the pane and the engine buffer drift apart -- the pane
    // keeps rows the engine has freed, and the next drain appends at the wrong
    // offset.
    {
        pane->ClearOutput();
        doc->ClearOutput();
        doc->m_bConvertGAtoNewline = 1;    // so the prompt becomes a real line

        for (int i = 1; i <= 6; ++i) {
            char buf[32];
            snprintf(buf, sizeof buf, "scrollback %d\n", i);
            doc->DisplayMsg(buf, (int) strlen(buf), 0);
        }
        doc->DisplayMsg("HP:100 MP:50 > ", 15, 0);
        unsigned char ga = GO_AHEAD;
        doc->Phase_IAC(ga);                // stamps PROMPT, substitutes '\n'
        if (ga) { char nl = (char) ga; doc->DisplayMsg(&nl, 1, 0); }
        doc->UpdateAllViews(NULL);
        wxYield();

        const long engineBefore = doc->m_LineList.GetCount();
        const int  paneBefore   = pane->LineCount();
        check(engineBefore == 8, "wrap-up: 6 lines, a prompt and the pending row");
        check(paneBefore == 7, "...seven of them committed to the pane");

        auto* childFrame = dynamic_cast<CWorldChildFrame*>(child);
        check(childFrame != nullptr, "the world child frame is what handles Cmd-K");
        const long dropped = childFrame ? childFrame->ClearToStart() : -1;
        wxYield();

        check(dropped == 6, "Cmd-K cuts the six lines above the prompt");
        check(doc->m_LineList.GetCount() == 2,
              "...leaving the prompt and the pending row in the engine buffer");
        check(pane->LineCount() == 1,
              "...and the PANE drops exactly the same six, through "
              "DeleteOutputHeadLines");

        // The surviving row must be the prompt itself, not a fragment or the
        // wrong end of the buffer.
        std::string first;
        if (const SOutputLine* l = pane->GetLine(0))
            for (const SOutputRun& r : *l) first += r.text;
        check(first == "HP:100 MP:50 > ",
              "...and the row that survived IS the prompt");

        // A second ⌘K with nothing above the prompt must be a no-op, not an
        // off-by-one that eats the prompt too.
        check(childFrame && childFrame->ClearToStart() == 0,
              "Cmd-K again cuts nothing: the prompt is already at the top");
        wxYield();
        check(pane->LineCount() == 1, "...and the pane is untouched");

        doc->m_bConvertGAtoNewline = 0;
        pane->ClearOutput();
        doc->ClearOutput();
    }

    // ── Clear to Previous Mark / Clear Scrollback ────────────────────────────
    // The other two macOS-shaped clears.  Clear to Previous Mark's cut point is
    // engine-side and covered headlessly; what is checked here is that the menu
    // command reaches it and the pane follows.  Clear Scrollback's cut point is
    // NOT reachable headlessly at all -- only the pane knows how the buffer is
    // wrapped and where the viewport sits.
    {
        pane->ClearOutput();
        doc->ClearOutput();
        auto* childFrame = dynamic_cast<CWorldChildFrame*>(child);

        // ── Clear to Previous Mark ──
        doc->DisplayMsg("older output\n", 13, 0);
        doc->DisplayMsg("look\n", 5, USER_INPUT);
        doc->DisplayMsg("A clearing.\n", 12, 0);
        doc->UpdateAllViews(NULL);
        wxYield();
        check(pane->LineCount() == 3, "wrap-up: three lines, the middle one typed");

        const long cut = childFrame ? childFrame->ClearToPreviousMark() : -1;
        wxYield();
        check(cut == 1, "Cmd-L cuts back to the last command the user typed");
        check(pane->LineCount() == 2, "...and the pane drops that one line");
        std::string top;
        if (const SOutputLine* l = pane->GetLine(0))
            for (const SOutputRun& r : *l) top += r.text;
        check(top == "look", "...leaving the command itself at the top");

        check(childFrame && childFrame->ClearToPreviousMark() == 0,
              "Cmd-L again cuts nothing: the mark is already at the top");

        // ── Clear Scrollback ──
        pane->ClearOutput();
        doc->ClearOutput();
        // The lines must be long enough that the PANE wraps them: with one
        // visual row per logical line, a visual row index and a line index are
        // the same number, and a FirstVisibleLine that skipped the wrap
        // mapping entirely would still pass.  The pane wraps narrower than the
        // engine does (its width is the client area, the engine's is
        // m_nWrapColumn), so a full-width engine line always spans two rows --
        // asserted below rather than assumed, because if the two ever agreed
        // this test would quietly stop testing anything.
        const std::string pad(200, 'x');
        for (int i = 1; i <= 200; ++i) {
            char buf[64];
            snprintf(buf, sizeof buf, "scroll line %d ", i);
            const std::string s = std::string(buf) + pad + "\n";
            doc->DisplayMsg(s.c_str(), (int) s.size(), 0);
        }
        doc->UpdateAllViews(NULL);
        wxYield();

        const int paneN = pane->LineCount();
        check(paneN > 200, "wrap-up: the engine split the long lines");
        check(pane->WrapColumns() > 0 &&
              pane->WrapColumns() < (int) doc->m_nWrapColumn,
              "...and the pane is narrower still, so its rows outnumber its lines");

        const int target = paneN / 2;
        std::string wanted;      // the text that is about to become line 0
        if (const SOutputLine* l = pane->GetLine(target))
            for (const SOutputRun& r : *l) wanted += r.text;
        check(!wanted.empty(), "...and the line we scroll to has text");

        pane->ScrollToLine(target);
        wxYield();
        check(pane->FirstVisibleLine() == target,
              "the pane reports the line it was scrolled to as the first visible");

        const long freed = childFrame ? childFrame->ClearScrollback() : -1;
        wxYield();
        check(freed == target, "Alt-Cmd-K frees exactly what was above the viewport");
        check(pane->LineCount() == paneN - target,
              "...and the pane keeps everything from the viewport down");
        std::string kept;
        if (const SOutputLine* l = pane->GetLine(0))
            for (const SOutputRun& r : *l) kept += r.text;
        check(kept == wanted,
              "...with the formerly-topmost VISIBLE line now at the head");
        check(doc->m_LineList.GetCount() == (paneN - target) + 1,
              "...and the engine buffer agrees (the survivors + the pending row)");

        pane->ClearOutput();
        doc->ClearOutput();
    }

    // ── Clear Screen erases nothing and scrolls the last line to the top ─────
    // The fourth clear, and the odd one out: no line is freed, so the only
    // observable effects are the scroll position and the blank region opened
    // below the last line.  Both are checked, because a version that merely
    // scrolled to the bottom would look identical from the outside on a buffer
    // shorter than a screenful.
    {
        pane->ClearOutput();
        doc->ClearOutput();
        for (int i = 1; i <= 300; ++i) {
            char buf[32];
            snprintf(buf, sizeof buf, "screen line %d\n", i);
            doc->DisplayMsg(buf, (int) strlen(buf), 0);
        }
        doc->UpdateAllViews(NULL);
        wxYield();

        const int  lines  = pane->LineCount();
        const long before = doc->m_LineList.GetCount();
        check(lines == 300, "wrap-up: 300 lines to scroll through");
        check(pane->OverscrollRows() == 0, "...and no blank region yet");

        auto* childFrame = dynamic_cast<CWorldChildFrame*>(child);
        check(childFrame && childFrame->ClearScreen(),
              "Ctrl-Cmd-L reports it moved the view");
        wxYield();

        check(doc->m_LineList.GetCount() == before,
              "...and freed NOTHING: every line is still in the engine buffer");
        check(pane->LineCount() == lines, "...and still in the pane");
        check(pane->OverscrollRows() > 0,
              "...having opened a blank region to scroll the last line into");
        check(pane->FirstVisibleLine() == lines - 1,
              "...which puts the LAST line at the top of the viewport");

        // The blank region is consumed by output, not added to: the next line
        // lands below the last one rather than pushing the view down again.
        const long long blank = pane->OverscrollRows();
        doc->DisplayMsg("and one more\n", 13, 0);
        doc->UpdateAllViews(NULL);
        wxYield();
        check(pane->OverscrollRows() < blank,
              "new output eats into the blank region rather than extending it");

        // Scrolling back up must still reach the very first line -- "erases
        // nothing" is the whole claim.
        pane->ScrollToTop();
        wxYield();
        check(pane->FirstVisibleLine() == 0,
              "...and the scrollback is all still reachable");

        pane->ClearOutput();
        doc->ClearOutput();
        check(pane->OverscrollRows() == 0,
              "clearing the buffer takes the blank region with it");
    }

    // ── the four clears' accelerators are ones wx can actually parse ─────────
    // An accelerator string wx rejects produces a menu item with no shortcut
    // and no error of any kind, so the key simply does nothing and nobody
    // finds out until they reach for it.  These are the same constants the
    // Display menu is built from (wx_mfc_impl.cpp), so a typo is caught here
    // rather than by a user.  "Ctrl" is Command on macOS; "RawCtrl" is the
    // physical Control key, and the pairing in Clear Screen is the unusual one.
    {
        struct { const char* accel; int flags; int key; const char* what; } kAccels[] = {
            { CWorldChildFrame::kAccelClearToStart,    wxACCEL_CTRL,
              'K', "Clear to Start is Cmd-K" },
            { CWorldChildFrame::kAccelClearToMark,     wxACCEL_CTRL,
              'L', "Clear to Previous Mark is Cmd-L" },
            { CWorldChildFrame::kAccelClearScrollback, wxACCEL_ALT | wxACCEL_CTRL,
              'K', "Clear Scrollback is Alt-Cmd-K" },
            { CWorldChildFrame::kAccelClearScreen,     wxACCEL_RAW_CTRL | wxACCEL_CTRL,
              'L', "Clear Screen is Ctrl-Cmd-L" },
        };
        for (const auto& a : kAccels) {
            wxAcceleratorEntry e;
            const bool ok = e.FromString(wxString::FromUTF8(a.accel)) &&
                            e.GetFlags() == a.flags && e.GetKeyCode() == a.key;
            check(ok, a.what);
        }
    }

    // ── Ctrl+L is the canonical Clear Output Buffer, not a pane-only wipe ─────
    // COutputPane::ClearOutput touches the pane's own deque only; the engine's
    // m_LineList is what world.GetLinesInBufferCount() reports and what
    // RebuildOutputView re-emits on any palette change, so a pane-only clear
    // came straight back the next time the user touched ANSI Colour or a script
    // called SetNormalColour.  Ctrl+L now dispatches ID_DISPLAY_CLEAR_OUTPUT
    // (doc.cpp:2396) like the Display menu item does, prompt included.
    {
        // Answer the "Are you SURE…" prompt.  The first arm says No, which must
        // leave everything alone — without it, a Ctrl+L that ignored the prompt
        // and always cleared would pass the second arm just as well.
        static int s_answer = IDNO;
        static int s_prompts = 0;
        int (*savedMB)(const char*, const char*, unsigned) = g_wxhook_message_box;
        g_wxhook_message_box = [](const char*, const char*, unsigned) -> int {
            ++s_prompts;
            return s_answer;
        };

        doc->Note("clear me one");
        doc->Note("clear me two");
        doc->UpdateAllViews(NULL);
        wxYield();
        const long before = doc->m_LineList.GetCount();
        check(before >= 2, "wrap-up: the engine buffer has lines to clear");

        auto sendCtrlL = [&]() {
            wxKeyEvent key(wxEVT_KEY_DOWN);
            key.m_keyCode      = 'L';
            key.m_rawControlDown = true;
            key.m_controlDown  = true;
            key.SetEventObject(pane);
            pane->GetEventHandler()->ProcessEvent(key);
            wxYield();
        };

        s_answer = IDNO;
        sendCtrlL();
        check(s_prompts == 1, "Ctrl+L prompts, as the canonical command does");
        check(doc->m_LineList.GetCount() == before,
              "...and answering No clears nothing");
        check(pane->LineCount() > 0, "...the pane keeps its lines too");

        s_answer = IDYES;
        sendCtrlL();
        check(s_prompts == 2, "Ctrl+L prompts again");
        check(doc->m_LineList.GetCount() == 1,
              "...and Yes clears the ENGINE buffer, not just the pane "
              "(one re-seeded empty line, as CMUSHclientDoc::ClearOutput leaves)");
        check(pane->LineCount() == 0,
              "...and the pane follows through EmitClearOutput");

        g_wxhook_message_box = savedMB;
    }

    // ── PULL MODEL: what it actually saves ───────────────────────────────
    // Feed a large buffer and report resident memory, so the branch can be
    // judged on a number rather than an argument.  Printed, not asserted: the
    // figure is machine-dependent and the point is the comparison against the
    // same run on main.
    {
        doc->m_maxlines = 0;                 // unlimited, which is the case at issue
        doc->ClearOutput();
        pane->SetMaxLines(0);

        auto rssMB = []() -> double {
            FILE* f = popen("ps -o rss= -p $PPID", "r");
            if (!f) return -1;
            long kb = 0;
            if (fscanf(f, "%ld", &kb) != 1) kb = 0;
            pclose(f);
            return kb / 1024.0;
        };

        const double before = rssMB();
        const int kLines = 200000;
        char buf[96];
        for (int i = 0; i < kLines; ++i) {
            snprintf(buf, sizeof buf,
                     "\x1b[32m%06d\x1b[0m the quick brown fox jumps over the lazy dog\n", i);
            doc->DisplayMsg(buf, (int) strlen(buf), 0);
        }
        doc->UpdateAllViews(NULL);
        wxYield();

        const double after = rssMB();
        printf("    MEMORY: %d lines, RSS %.1f MB -> %.1f MB (delta %.1f MB, "
               "%.0f bytes/line); pane holds %d lines of content\n",
               kLines, before, after, after - before,
               (after - before) * 1024.0 * 1024.0 / kLines,
               pane->PullModelActive() ? 0 : pane->LineCount());
        check(pane->LineCount() >= kLines,
              "memory probe: every line is addressable through the pane");
        check(pane->PullModelActive(),
              "memory probe: ...with the pull seam actually wired");

        doc->ClearOutput();
        doc->m_maxlines = 5000;
    }

    // ── the pull model's MUTATORS ────────────────────────────────────────────
    // The half the spike left undone.  It converted the pane's line-count
    // READERS to LineCountI() and left every mutator guarding with a count and
    // then operating on m_lines -- which the pull model empties.  Each of these
    // three paths popped an empty deque; the crash was reachable from
    // world.DeleteLines(n), which has nothing to do with the clear commands.
    //
    // The invariant they all now share: in the pull model the ENGINE owns the
    // line set and the pane owns only what it derives from it, so a pane-side
    // trim targets the length of that derived data, never m_maxLines.
    {
        pane->ClearOutput();
        doc->ClearOutput();
        doc->m_maxlines = 0;                     // unlimited: only we trim
        check(pane->PullModelActive(), "wrap-up: the pull seam is still wired");

        for (int i = 1; i <= 40; ++i) {
            char buf[32];
            snprintf(buf, sizeof buf, "mutator line %d\n", i);
            doc->DisplayMsg(buf, (int) strlen(buf), 0);
        }
        doc->UpdateAllViews(NULL);
        wxYield();
        check(pane->LineCount() == 40, "40 lines, and the pane reads them all");

        // world.DeleteLines(n) -- the tail mutator.  This is the one that
        // segfaulted, and it predates every clear command.
        // m_bInSendToScript starts TRUE (doc_construct.cpp:98) and DeleteLines
        // refuses while it is set -- the method is only callable from inside a
        // trigger or alias script, which is where ProcessPreviousLine clears
        // it.  Reproduce that window rather than skipping the check.
        doc->m_bInSendToScript = false;
        doc->DeleteLines(10);
        doc->m_bInSendToScript = true;
        doc->UpdateAllViews(NULL);
        wxYield();
        // Exactly 10: DeleteLines folds the empty in-progress line into the
        // count (methods_output_shared.cpp:79) and then re-seeds one, so those
        // two cancel and the committed lines lose 10.
        check(pane->LineCount() == 30, "world.DeleteLines takes 10 off the tail");
        check(pane->DerivedLength() == pane->LineCount(),
              "...and the pane's derived data is exactly as long as the engine's");
        {
            std::string last;
            if (const SOutputLine* l = pane->GetLine(pane->LineCount() - 1))
                for (const SOutputRun& r : *l) last += r.text;
            check(last == "mutator line 30",
                  "...and the pane still reads the right last line");
        }

        // The head mutator, through Clear to Start's fallback (no GA here).
        auto* childFrame = dynamic_cast<CWorldChildFrame*>(child);
        const long cut = childFrame ? childFrame->ClearToStart() : -1;
        wxYield();
        check(cut == 29, "Clear to Start cuts the 29 lines above the last");
        check(pane->LineCount() == 1, "...and the pane follows to one line");
        check(pane->DerivedLength() == pane->LineCount(),
              "...with the wrap cache cut to match, not left 30 long");
        {
            std::string only;
            if (const SOutputLine* l = pane->GetLine(0))
                for (const SOutputRun& r : *l) only += r.text;
            check(only == "mutator line 30",
                  "...reading the surviving line at its NEW index");
        }

        // The capacity trim.  In the pull model the engine evicts at its own
        // cap and the pane has to follow it -- trimming to the pane's m_maxLines
        // instead could not even terminate, because popping the pane's wrap
        // cache does not change what the engine reports.
        pane->ClearOutput();
        doc->ClearOutput();
        doc->m_maxlines = 300;                   // engine evicts in JUMP_SIZE chunks

        // DRAIN AS WE GO, which is what a connected world does: the pane sees
        // each line before the engine's RemoveChunk takes it away again.  A
        // single drain at the end would prove nothing -- the engine would have
        // finished evicting first, so the pane could never be holding more
        // lines than the engine, which is the only state the capacity trim
        // exists to resolve.
        for (int i = 1; i <= 500; ++i) {
            char buf[32];
            snprintf(buf, sizeof buf, "cap line %d\n", i);
            doc->DisplayMsg(buf, (int) strlen(buf), 0);
            if (i % 25 == 0) doc->UpdateAllViews(NULL);
        }
        doc->UpdateAllViews(NULL);
        wxYield();
        check(pane->LineCount() == (int) doc->m_LineList.GetCount() - 1,
              "the pane's line count tracks the ENGINE's, not its own cap");
        // The load-bearing one.  LineCount() reports the engine's number under
        // the pull seam, so it agrees no matter what the pane did; only the
        // derived length can show the pane keeping rows for lines the engine
        // has already evicted.
        check(pane->DerivedLength() == pane->LineCount(),
              "...and so does its wrap cache, which is the only way to tell");
        {
            std::string first;
            if (const SOutputLine* l = pane->GetLine(0))
                for (const SOutputRun& r : *l) first += r.text;
            check(first.rfind("cap line ", 0) == 0,
                  "...and line 0 is a real line, not a fetch past the end");
        }

        doc->m_maxlines = 5000;
        pane->ClearOutput();
        doc->ClearOutput();
    }

    // ── the line number plugins are given to align to ───────────────────────
    // ON_PLUGIN_DRAW_OUTPUT_WINDOW hands plugins a 1-based DOC line for the top
    // of the viewport; Quow's pack positions miniwindows from it.  The
    // arithmetic lived inside the paint callback, which no selftest reaches, so
    // it had never executed under test -- and it was wrong.
    {
        pane->ClearOutput();
        doc->ClearOutput();
        auto* childFrame = dynamic_cast<CWorldChildFrame*>(child);
        check(childFrame != nullptr, "wrap-up: the frame is what plugins ask");

        for (int i = 1; i <= 10; ++i) {
            char buf[32];
            snprintf(buf, sizeof buf, "aligned %d\n", i);
            doc->DisplayMsg(buf, (int) strlen(buf), 0);
        }
        doc->UpdateAllViews(NULL);
        wxYield();
        check(pane->LineCount() == 10, "ten committed lines");

        // With nothing pending, pane row 0 is doc line 1.
        check(childFrame && childFrame->PaneLineToDocLine(0) == 1,
              "pane row 0 is doc line 1");
        check(childFrame && childFrame->PaneLineToDocLine(4) == 5,
              "...and pane row 4 is doc line 5");

        // NOW the case that was broken.  An unterminated prompt is the pending
        // row, not a committed line, so it must not move the mapping at all --
        // but GetLastLine() counts it as soon as it has any text, which shifted
        // every plugin's idea of the viewport down by one for as long as a
        // prompt was showing.  On a MUD that is most of the time.
        doc->DisplayMsg("HP:100 MP:50 > ", 15, 0);
        doc->UpdateAllViews(NULL);
        wxYield();
        check(pane->LineCount() == 10,
              "a pending prompt adds no committed line");
        check(childFrame && childFrame->PaneLineToDocLine(0) == 1,
              "...and does not move pane row 0 off doc line 1");
        check(childFrame && childFrame->PaneLineToDocLine(4) == 5,
              "...nor any other row");

        // Never below 1, whatever it is asked.
        check(childFrame && childFrame->PaneLineToDocLine(-50) == 1,
              "a nonsensical row clamps to line 1, not to zero or negative");

        pane->ClearOutput();
        doc->ClearOutput();
    }

    // ── closing a world: what it asks, and the ORDER of the teardown ────────
    // Neither half could be tested before.  The prompts were raw wxMessageBox
    // calls with no seam, and the order was unobservable because the frame and
    // its document are both gone by the time OnClose returns -- so
    // CWorldChildFrame::RunCloseTeardown now reports each step as it completes.
    //
    // The order is the content.  A world timer that outlives the doc calls into
    // freed memory; ON_PLUGIN_CLOSE / SaveState is where Quow-class packs
    // persist their miniwindow layout, so it has to run while the pane is still
    // attached; and the view must be detached before anything is deleted or the
    // doc's final flush paints into a dying pane.
    {
        // OnClose writes the window size AND position to the user's REAL
        // preferences.  Put the keys back afterwards: a selftest must not
        // resize or reposition the next launch.
        long savedW = -1, savedH = -1, savedX = -1, savedY = -1;
        {
            const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
            if (!cfg.Read(wxT("/geometry/child_w"), &savedW)) savedW = -1;
            if (!cfg.Read(wxT("/geometry/child_h"), &savedH)) savedH = -1;
            // -1 legitimately can be a saved X or Y (e.g. a second-monitor
            // window left of the primary), so track presence separately
            // rather than reusing "-1 means absent" for these two.
            if (!cfg.Read(wxT("/geometry/child_x"), &savedX)) savedX = LONG_MIN;
            if (!cfg.Read(wxT("/geometry/child_y"), &savedY)) savedY = LONG_MIN;
        }
        const unsigned savedConfirmVars  = App.m_bConfirmBeforeSavingVariables;
        const unsigned savedConfirmClose = App.m_bConfirmBeforeClosingWorld;

        static int  s_prompts = 0;      // prompts put up by the chain
        static int  s_answer  = IDNO;   // what the user "presses"
        static int  s_saves   = 0;      // calls to the save hook
        static bool s_saveOK  = true;   // whether that save succeeds
        int  (*savedMB)(const char*, const char*, unsigned) = g_wxhook_message_box;
        bool (*savedSaveWorld)(void*, bool, void*)          = g_wxhook_save_world;
        g_wxhook_message_box = [](const char*, const char*, unsigned) -> int {
            ++s_prompts;
            return s_answer;
        };
        // A save that neither touches the disk nor opens the Save As dialog the
        // real hook would open for these file-less documents.
        g_wxhook_save_world = [](void* d, bool, void*) -> bool {
            ++s_saves;
            if (s_saveOK && d)
                static_cast<CMUSHclientDoc*>(d)->SetModifiedFlag(FALSE);
            return s_saveOK;
        };

        // A world window with just enough state to be closed.  The name is one
        // no real world uses, so the per-world history file cannot collide with
        // the user's own (nothing is written: the history is empty).
        auto makeWorld = [&](const char* name) -> CWorldChildFrame* {
            CMUSHclientDoc* d = new CMUSHclientDoc();
            d->m_mush_name   = name;
            d->m_nWrapColumn = 80;
            d->m_maxlines    = 5000;
            d->SetUpOutputWindow();
            // (doc ctor already ran AddDocument -- doc_construct.cpp:521;
            //  a second add left a DANGLING duplicate entry after close)
            return new CWorldChildFrame(parent, d, wxString::FromUTF8(name));
        };

        // ── a forced close asks nothing, and still tears down in order ──────
        {
            CWorldChildFrame* f = makeWorld("zz_selftest_close_order");
            CMUSHclientDoc*   d = f->GetDoc();
            // Everything that could possibly prompt is switched ON, so "no
            // prompts" is a statement about CanVeto and not about the fixture.
            d->SetModifiedFlag(TRUE);
            d->m_bVariablesChanged = true;
            App.m_bConfirmBeforeSavingVariables = 1;
            App.m_bConfirmBeforeClosingWorld    = 1;

            static std::vector<int> steps, timerOn, viewOn;
            steps.clear(); timerOn.clear(); viewOn.clear();
            f->SetCloseTeardownObserver([f, d](CWorldChildFrame::CloseStep s) {
                steps.push_back((int) s);
                timerOn.push_back(f->IsWorldTimerRunning() ? 1 : 0);
                // `d` is freed by the last step; never dereference it there.
                viewOn.push_back(s == CWorldChildFrame::CloseStep::DocDeleted
                                     ? -1 : (d->m_pWorldView == f ? 1 : 0));
            });

            s_prompts = 0; s_saves = 0; s_answer = IDCANCEL;
            const bool closed = f->Close(true);      // forced: CanVeto() is false
            wxYield();

        // (Deliberately NOT asserting GetVeto() here: it returns
        // m_canVeto && m_veto, and Close(true) clears m_canVeto, so it is true
        // whatever OnClose does.  The three checks below carry this block.)
            check(s_prompts == 0,
                  "...asking nothing, because a forced close cannot be vetoed");
            check(s_saves == 0, "...and saving nothing behind the user's back");

            const std::vector<int> wanted = {
                (int) CWorldChildFrame::CloseStep::TimerStopped,
                (int) CWorldChildFrame::CloseStep::WorldCloseFired,
                (int) CWorldChildFrame::CloseStep::ViewDetached,
                (int) CWorldChildFrame::CloseStep::Deregistered,
                (int) CWorldChildFrame::CloseStep::DocDeleted,
            };
            // Five, not seven.  The two that used to sit between
            // WorldCloseFired and ViewDetached were an explicit
            // ON_PLUGIN_CLOSE + SaveState pass over every plugin -- which
            // `delete doc` already does, once, through ~CPlugin.  Every enabled
            // plugin's close handler therefore ran TWICE per close.  This list
            // is what fails if the pass comes back.
            check(steps == wanted, "the teardown runs all five steps, in order");

            if (steps.size() == wanted.size()) {
                bool everRunning = false;
                for (int on : timerOn) everRunning = everRunning || on;
                check(!everRunning,
                      "the world timer is stopped before the doc is touched");
                check(viewOn[1] == 1,
                      "the world-close script runs while the pane is still "
                      "attached, so anything it prints can reach the screen");
                check(viewOn[2] == 0 && viewOn[3] == 0,
                      "...and the view is detached before anything is deleted");
            } else {
                check(false, "(teardown order wrong - dependent checks skipped)");
                check(false, "(teardown order wrong - dependent checks skipped)");
                check(false, "(teardown order wrong - dependent checks skipped)");
            }
        }

        // ── the prompt chain: Cancel vetoes, No discards ────────────────────
        {
            CWorldChildFrame* f = makeWorld("zz_selftest_close_prompt");
            CMUSHclientDoc*   d = f->GetDoc();
            d->SetModifiedFlag(TRUE);

            static bool s_toreDown = false;
            s_toreDown = false;
            f->SetCloseTeardownObserver(
                [](CWorldChildFrame::CloseStep) { s_toreDown = true; });

            s_prompts = 0; s_saves = 0; s_answer = IDCANCEL;
            const bool vetoed = !f->Close(false);    // vetoable: the chain runs
            check(vetoed, "Cancel on the unsaved-changes prompt vetoes the close");
            check(s_prompts == 1, "...after exactly one question");
            check(!s_toreDown, "...and nothing is torn down");
            check(f->GetDoc() == d, "...so the world is still open");

            s_prompts = 0; s_saves = 0; s_answer = IDNO;
            const bool closed = f->Close(false);
            wxYield();
            check(closed, "No on the same prompt closes the world");
            check(s_prompts == 1, "...after one question");
            check(s_saves == 0, "...discarding the changes rather than saving");
            check(s_toreDown, "...and the teardown runs this time");
        }

        // ── "save world automatically on close" replaces the question ───────
        {
            App.m_bConfirmBeforeClosingWorld = 0;   // isolate the save arm
            CWorldChildFrame* f = makeWorld("zz_selftest_close_autosave");
            CMUSHclientDoc*   d = f->GetDoc();
            d->m_bSaveWorldAutomatically = 1;
            d->SetModifiedFlag(TRUE);

            // Cancel is the answer, so if a prompt DID appear the close would be
            // vetoed -- "closed" is what makes this check falsifiable.
            s_prompts = 0; s_saves = 0; s_answer = IDCANCEL; s_saveOK = true;
            const bool closed = f->Close(false);
            wxYield();
            check(s_saves == 1, "save_world_automatically saves without asking");
            check(s_prompts == 0, "...and no unsaved-changes prompt follows it");
            check(closed, "...so the world closes");
        }

        // ── ...but only if it worked ───────────────────────────────────────
        {
            CWorldChildFrame* f = makeWorld("zz_selftest_close_savefail");
            CMUSHclientDoc*   d = f->GetDoc();
            d->m_bSaveWorldAutomatically = 1;
            d->SetModifiedFlag(TRUE);

            s_prompts = 0; s_saves = 0; s_answer = IDNO; s_saveOK = false;
            const bool closed = f->Close(false);
            wxYield();
            check(s_saves == 1, "a failed automatic save is still attempted");
            check(s_prompts == 1,
                  "...and the document is still modified, so the user IS asked");
            check(closed, "...and No closes it");
            s_saveOK = true;
        }

        // ── the command history reaches disk on BOTH close paths ───────────
        // Cmd+W runs OnClose, which persists it.  Cmd+Q does not run OnClose at
        // all -- [NSApp terminate:] tears the app down without it -- so quitting,
        // the ordinary way to leave the app, never saved the up-arrow history:
        // what survived was whatever the last Cmd+W had written.  Both paths now
        // go through PersistBeforeTeardown, which is what this drives.
        {
            const std::string worldName = "zz_selftest_history";
            std::string path;
            {
                const char* home = getenv("HOME");
                if (home && *home)
                    path = std::string(home) + "/.mushclient/history_"
                         + worldName + ".txt";
            }
            check(!path.empty(), "history: a per-world history path exists");
            if (!path.empty()) {
                remove(path.c_str());

                CWorldChildFrame* hf = makeWorld(worldName.c_str());
                hf->addToCommandHistory("north");
                hf->addToCommandHistory("look at the dragon");

                // Nothing on disk until a close path runs.
                check(fopen(path.c_str(), "rb") == NULL,
                      "history: nothing is written while the world is open");

                hf->PersistBeforeTeardown();

                std::string got;
                if (FILE* rf = fopen(path.c_str(), "rb")) {
                    int ch; while ((ch = fgetc(rf)) != EOF) got += (char) ch;
                    fclose(rf);
                }
                check(got == "north\nlook at the dragon\n",
                      "history: the quit path writes every command, in order");

                remove(path.c_str());
                hf->Close(true);
                wxYield();
            }
        }

        g_wxhook_message_box = savedMB;
        g_wxhook_save_world  = savedSaveWorld;
        App.m_bConfirmBeforeSavingVariables = savedConfirmVars;
        App.m_bConfirmBeforeClosingWorld    = savedConfirmClose;
        {
            const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
            if (savedW > 0) cfg.Write(wxT("/geometry/child_w"), savedW);
            else            cfg.DeleteEntry(wxT("/geometry/child_w"));
            if (savedH > 0) cfg.Write(wxT("/geometry/child_h"), savedH);
            else            cfg.DeleteEntry(wxT("/geometry/child_h"));
            if (savedX != LONG_MIN) cfg.Write(wxT("/geometry/child_x"), savedX);
            else                    cfg.DeleteEntry(wxT("/geometry/child_x"));
            if (savedY != LONG_MIN) cfg.Write(wxT("/geometry/child_y"), savedY);
            else                    cfg.DeleteEntry(wxT("/geometry/child_y"));
        }
    }

    // ── a saved position is actually restored on the NEXT launch ────────────
    // Confirmed port bug (GUI-drive run 2026-08-12, P1-08): a world window
    // dragged to a new position and resized, then the app quit and the world
    // reopened, came back at the default position (size was fine).  Root
    // cause: the writer captured the visible child's x/y correctly, but nothing
    // on the READ side ever looked at child_x/child_y — SetPosition was only
    // ever called for the centred FIRST-run placement.  This seeds specific
    // child_x/child_y (and child_w/child_h) into the user's real config —
    // the same OpenAppConfig() the app itself opens on launch — and
    // constructs a real CWorldChildFrame to check the constructor actually
    // reads them back, not just that something writes them (the write side
    // is covered separately by --gui-bridge-selftest).
    {
        long savedW = -1, savedH = -1, savedX = -1, savedY = -1;
        {
            const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
            if (!cfg.Read(wxT("/geometry/child_w"), &savedW)) savedW = -1;
            if (!cfg.Read(wxT("/geometry/child_h"), &savedH)) savedH = -1;
            if (!cfg.Read(wxT("/geometry/child_x"), &savedX)) savedX = LONG_MIN;
            if (!cfg.Read(wxT("/geometry/child_y"), &savedY)) savedY = LONG_MIN;
        }

        // Comfortably inside any real display's usable area (so the ctor's
        // off-screen clamp is not what would make this pass), and NOT the
        // centred first-run placement, so a constructor that silently fell
        // back to that default would be caught rather than accidentally
        // matching.  550 wide echoes the exact width from the field report.
        const long wantW = 550, wantH = 400, wantX = 60, wantY = 80;
        {
            const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
            cfg.Write(wxT("/geometry/child_w"), wantW);
            cfg.Write(wxT("/geometry/child_h"), wantH);
            cfg.Write(wxT("/geometry/child_x"), wantX);
            cfg.Write(wxT("/geometry/child_y"), wantY);
        }

        CMUSHclientDoc* d = new CMUSHclientDoc();
        d->m_mush_name   = "zz_selftest_geometry_restore";
        d->m_nWrapColumn = 80;
        d->m_maxlines    = 5000;
        d->SetUpOutputWindow();
        CWorldChildFrame* f = new CWorldChildFrame(
            parent, d, wxT("zz_selftest_geometry_restore"));

        check(f->GetSize().GetWidth()  == wantW &&
              f->GetSize().GetHeight() == wantH,
              "a freshly constructed world frame restores the saved SIZE");
        check(f->GetPosition().x == wantX && f->GetPosition().y == wantY,
              "...and the saved POSITION, not the centred first-run default "
              "(this is the exact bug: size round-tripped, position did not)");

        f->Close(true);      // forced: no doc changes, nothing to ask
        wxYield();

        {
            const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
            if (savedW > 0) cfg.Write(wxT("/geometry/child_w"), savedW);
            else            cfg.DeleteEntry(wxT("/geometry/child_w"));
            if (savedH > 0) cfg.Write(wxT("/geometry/child_h"), savedH);
            else            cfg.DeleteEntry(wxT("/geometry/child_h"));
            if (savedX != LONG_MIN) cfg.Write(wxT("/geometry/child_x"), savedX);
            else                    cfg.DeleteEntry(wxT("/geometry/child_x"));
            if (savedY != LONG_MIN) cfg.Write(wxT("/geometry/child_y"), savedY);
            else                    cfg.DeleteEntry(wxT("/geometry/child_y"));
        }
    }

    // ── The selection's coordinate space, at the two dialogs that read it ────
    // COutputPane::GetSelectionInfo answers in the 1-based line/column space
    // world.GetSelection uses.  Both of these functions are the canonical
    // CMUSHView bodies, which work in the 0-based m_selstart_line /
    // m_selstart_col space — so both used to describe the character one line
    // BELOW and one column RIGHT of the one the user had selected.
    //
    // The fixture is deliberately non-degenerate: the two lines differ in text
    // AND in colour, and no letter repeats, so an off-by-one in either axis
    // produces a different answer rather than the same one.
    {
        doc->m_bAutoWrapWindowWidth = false;
        doc->ClearOutput();
        doc->DisplayMsg("\x1b[31mABCDEFGH\x1b[0m\n", 18, 0);
        doc->DisplayMsg("\x1b[32mijklmnop\x1b[0m\n", 18, 0);
        doc->UpdateAllViews(NULL);
        wxYield();

        // Find each line by its text rather than assuming an index: ClearOutput
        // re-seeds a line of its own, so the two are not at 0 and 1.
        auto lineIndexOf = [&](const char* want) -> long {
            const int n = (int) strlen(want);
            long i = 0;
            for (POSITION p = doc->m_LineList.GetHeadPosition(); p; ++i) {
                const CLine* l = doc->m_LineList.GetNext(p);
                if (l && l->len >= n && memcmp(l->text, want, n) == 0) return i;
            }
            return -1;
        };
        const long idxA = lineIndexOf("ABCDEFGH");   // 0-based
        const long idxB = lineIndexOf("ijklmnop");
        check(idxA >= 0 && idxB == idxA + 1,
              "selection coords: the two-line fixture reached the buffer");

        // What the engine says about the FIRST SELECTED character of each line,
        // computed here from the line found by text.  This is the oracle: if the
        // code under test reads a different line, it cannot match.
        auto engineRGB = [&](long idx, long col0) -> COLORREF {
            CLine* l = doc->m_LineList.GetAt(doc->GetLinePosition(idx));
            CStyle* st = NULL; int iCol = 0; POSITION fp = NULL;
            if (!l || !doc->FindStyle(l, col0 + 1, iCol, st, fp)) return (COLORREF) -1;
            COLORREF fg = 0, bg = 0;
            doc->GetStyleRGB(st, fg, bg);
            return fg;
        };
        const COLORREF rgbA = engineRGB(idxA, 2);
        const COLORREF rgbB = engineRGB(idxB, 2);
        check(rgbA != rgbB && rgbA != (COLORREF) -1,
              "selection coords: ...with the two lines in different colours");

        // Capture what each dialog was handed, then decline it.
        static CString s_letter, s_textColour;
        static int     s_lineNo = 0;
        static COLORREF s_hlFore = 0;
        static int     s_seenTA = 0, s_seenHL = 0;
        intptr_t (*const savedRunDlg)(void*, unsigned) = g_run_mfc_dialog;
        g_run_mfc_dialog = [](void* self, unsigned idd) -> intptr_t {
            if (idd == IDD_TEXT_ATTRIBUTES) {
                const CTextAttributesDlg* d = (const CTextAttributesDlg*) self;
                s_letter = d->m_strLetter; s_textColour = d->m_strTextColour;
                s_lineNo = d->m_iLine; ++s_seenTA;
            } else if (idd == IDD_HIGHLIGHT) {
                const CHighlightPhraseDlg* d = (const CHighlightPhraseDlg*) self;
                s_hlFore = d->m_iOtherForeground; ++s_seenHL;
            }
            return IDCANCEL;
        };

        // Select "CDEF" on the first line: 1-based columns 3..7, so the first
        // selected character is 'C'.  One column right is 'D'; the same column
        // one line down is 'k'.  All three are distinguishable.
        pane->SetSelectionRange((int)(idxA + 1), (int)(idxA + 1), 3, 7);
        s_seenTA = 0;
        child->ShowTextAttributes();
        check(s_seenTA == 1, "Text Attributes: the dialog was reached");
        check(s_letter == "C",
              "Text Attributes: reports the SELECTED character, not the next one");
        check(s_lineNo == (int)(idxA + 1),
              "Text Attributes: ...on the selected line, 1-relative");
        const CString colourA = s_textColour;

        // Same selection on the second line — the answer must move with it.
        pane->SetSelectionRange((int)(idxB + 1), (int)(idxB + 1), 3, 7);
        s_seenTA = 0;
        child->ShowTextAttributes();
        check(s_seenTA == 1, "Text Attributes: the dialog was reached again");
        check(s_letter == "k", "Text Attributes: ...and follows the selection down");
        check(s_lineNo == (int)(idxB + 1), "Text Attributes: ...as does the line number");
        check(colourA != s_textColour,
              "Text Attributes: the two lines really do report different colours");

        // Highlight Word takes the same coordinates to seed the trigger colour.
        pane->SetSelectionRange((int)(idxA + 1), (int)(idxA + 1), 3, 7);
        s_seenHL = 0;
        child->HighlightSelectedWord();
        check(s_seenHL == 1, "Highlight Word: the dialog was reached");
        check(s_hlFore == rgbA,
              "Highlight Word: seeds the SELECTED line's colour, not the next line's");

        pane->SetSelectionRange((int)(idxB + 1), (int)(idxB + 1), 3, 7);
        s_seenHL = 0;
        child->HighlightSelectedWord();
        check(s_seenHL == 1, "Highlight Word: the dialog was reached again");
        check(s_hlFore == rgbB, "Highlight Word: ...and follows the selection down");

        // ── BYTE columns at the seam, CHARACTER columns in the pane ─────────
        // (2026-08-12)  On Windows world.GetSelection*/SetSelection speak BYTE
        // offsets into CLine::text (m_selstart_col steps whole UTF-8
        // sequences); the pane's columns are rendered characters.  The seam
        // passed them through unconverted, so the two spaces disagreed on any
        // line with a multibyte character — plugins slicing GetLineInfo text
        // with the selection columns (string.sub is byte-based) got the wrong
        // substring, and both dialogs above read the wrong style.  The fixture
        // makes bytes != chars on purpose: three UTF-8 arrows (9 bytes, 3
        // chars) in red, then ABC in green.
        {
            const BOOL savedUtf8 = doc->m_bUTF_8;
            doc->m_bUTF_8 = TRUE;
            doc->DisplayMsg("\x1b[31m\xe2\x86\x92\xe2\x86\x92\xe2\x86\x92"
                            "\x1b[32mABC\x1b[0m\n", 27, 0);
            doc->UpdateAllViews(NULL);
            wxYield();

            const long idxC =
                lineIndexOf("\xe2\x86\x92\xe2\x86\x92\xe2\x86\x92" "ABC");
            check(idxC >= 0, "byte cols: the multibyte fixture line arrived");

            // A Windows-written script selects "AB": 1-based BYTE columns
            // 10..12 (bytes 0-8 are the arrows).
            doc->SetSelection(idxC + 1, idxC + 1, 10, 12);
            check(pane->GetSelectionText() == wxString::FromUTF8("AB"),
                  "byte cols: SetSelection(10,12) selects AB, not arrow bytes");

            // ...and reading it back answers in the same byte space.
            check(doc->GetSelectionStartColumn() == 10,
                  "byte cols: GetSelectionStartColumn round-trips as 10");
            check(doc->GetSelectionEndColumn() == 12,
                  "byte cols: GetSelectionEndColumn round-trips as 12");

            // The pane itself holds the CHARACTER form: chars 4..6, 1-based.
            long psl = 0, psc = 0, pel = 0, pec = 0;
            check(pane->GetSelectionInfo(psl, psc, pel, pec) &&
                      psc == 4 && pec == 6,
                  "byte cols: the pane's own columns stay characters (4..6)");

            // Text Attributes on that selection: first selected char is the
            // GREEN 'A'.  Unconverted, char col 3 read byte 3 — the middle of
            // an arrow — a lone continuation byte in the red run.
            s_seenTA = 0;
            child->ShowTextAttributes();
            check(s_seenTA == 1, "byte cols: Text Attributes reached");
            check(s_letter == "A",
                  "byte cols: Text Attributes reports 'A' (was: arrow byte)");

            // Highlight Word seeds the selection's style.  It passes the
            // 0-based column to FindStyle (the canonical Windows boundary
            // quirk), so a selection starting exactly ON the style boundary
            // seeds the PREVIOUS style on both platforms — where old and new
            // code agree.  Select "B" (bytes 11..12, green run interior)
            // instead: byte space gives green, the old char-space read hands
            // FindStyle column 4 — inside the red arrows.
            const COLORREF rgbGreen = engineRGB(idxC, 9);   // byte 9 = 'A'
            const COLORREF rgbRed   = engineRGB(idxC, 3);   // inside arrows
            check(rgbGreen != rgbRed && rgbGreen != (COLORREF) -1,
                  "byte cols: the fixture's two styles really differ");
            doc->SetSelection(idxC + 1, idxC + 1, 11, 13);   // "BC"
            s_seenHL = 0;
            child->HighlightSelectedWord();
            check(s_seenHL == 1, "byte cols: Highlight Word reached");
            check(s_hlFore == rgbGreen,
                  "byte cols: Highlight Word seeds green (was: red)");

            doc->m_bUTF_8 = savedUtf8;
        }

        g_run_mfc_dialog = savedRunDlg;
        pane->SetSelectionRange(0, 0, 0, 0);
        doc->ClearOutput();
    }

    // ── The stored output font size is a point size, not a LOGFONT height ────
    // m_font_height IS the world option output_font_height.  Its table row
    // bounds it to a point size, every canonical reader assumes that
    // (configuration.cpp prints "%i pt.", mushview.cpp derives the LOGFONT
    // height from it and guards on > 0), and the world-file loader range-checks
    // it on the way back in.  Writing a LOGFONT pixel height here — negative,
    // by Win32 convention — put an out-of-range number in the world file, which
    // the loader then threw away: the font size silently reverted on reload,
    // the prefs page read "-24 pt.", and world.GetOption handed scripts a
    // negative.
    {
        const long optIdx = doc->GetOptionIndex("output_font_height");
        check(optIdx >= 0, "font size: output_font_height is a known world option");
        const double lo = (optIdx >= 0) ? OptionsTable[optIdx].iMinimum : 0;
        const double hi = (optIdx >= 0) ? OptionsTable[optIdx].iMaximum : 0;
        check(lo >= 1 && hi > lo,
              "font size: ...whose declared range admits only positive sizes");

        // TWO writers reach m_font_height and the later one wins, so each is
        // checked at the only moment it is observable: setOutputFont writes
        // synchronously, then the zoom handler's CallAfter re-derives it from
        // the pane's actual font on the next turn of the loop.  (The CallAfter
        // early-outs on a zero-sized pane, so the synchronous write is the only
        // one in that case — checking only after the yield would leave it
        // unpinned.)  Two different sizes, so "the code did nothing" cannot pass.
        for (int wanted : { 14, 18 }) {
            const bool first = (wanted == 14);
            child->setOutputFont("Menlo", wanted, false, false);
            const long sync = doc->GetOption("output_font_height");
            check(sync == wanted,
                  first ? "font size: setOutputFont stores the point size asked for"
                        : "font size: ...and follows a second, different size");
            check((double) sync >= lo && (double) sync <= hi,
                  first ? "font size: ...within the range the loader enforces"
                        : "font size: ...still within it");

            wxYield();          // let the zoom path's CallAfter run
            const long async = doc->GetOption("output_font_height");
            check(async == wanted,
                  first ? "font size: the zoom handler agrees, not a LOGFONT height"
                        : "font size: ...and agrees on the second size too");
            check((double) async >= lo && (double) async <= hi,
                  first ? "font size: ...also in range" : "font size: ...still in range");
            check(pane->GetCurrentFont().GetPointSize() == wanted,
                  first ? "font size: ...and the pane really is at that size"
                        : "font size: ...and the pane followed too");
        }
    }

    // ── the line_spacing world option reaches the row pitch ─────────────────
    // Windows applies it inside ChangeFont -- `if (iLineSpacing) m_FontHeight =
    // iLineSpacing` (doc.cpp:1179) -- where m_FontHeight is the pitch CMUSHView
    // paints with.  There is no CMUSHView here; the wx pane owns the pitch and
    // derived it from the font alone, so the option round-tripped through the
    // dialog, the XML and GetOption while nothing ever read it.
    {
        const long savedSpacing = doc->m_iLineSpacing;

        doc->m_iLineSpacing = 0;
        child->setOutputFont("Menlo", 12, false, false);
        wxYield();
        const int fromFont = pane->LineHeight();
        check(fromFont > 0, "line spacing: the font's own pitch is a real number");

        // A value the font cannot coincidentally equal, so "it did nothing"
        // cannot pass -- and asserted to differ, not assumed to.
        const int wanted = fromFont + 11;
        doc->m_iLineSpacing = (unsigned short) wanted;
        child->setOutputFont("Menlo", 12, false, false);
        wxYield();
        check(pane->LineHeight() == wanted,
              "line spacing: a non-zero line_spacing IS the row pitch");
        check(wanted != fromFont,
              "line spacing: ...and the fixture's two pitches really differ");

        // 0 is the documented default: "use the font's own height".  A setter
        // that only ever raised the pitch would pass the check above.
        doc->m_iLineSpacing = 0;
        child->setOutputFont("Menlo", 12, false, false);
        wxYield();
        check(pane->LineHeight() == fromFont,
              "line spacing: zero goes back to the font's height");

        // The paint path re-measures the font and adopts the result, and that
        // is a THIRD writer of the row pitch -- the one the screen actually
        // sees.  It has to honour the override too, or the option is live only
        // until the next repaint: GetInfo(212) reported the new pitch while the
        // window kept the old one, which is exactly what happened when this was
        // first written.  Driven directly, because the selftests exit before
        // OnRun and so never paint.
        doc->m_iLineSpacing = (unsigned short) wanted;
        child->setOutputFont("Menlo", 12, false, false);
        wxYield();
        pane->AdoptMeasuredMetrics(7, fromFont - 2);   // a plausible measurement
        check(pane->LineHeight() == wanted,
              "line spacing: survives the paint path's own re-measurement");

        doc->m_iLineSpacing = 0;
        child->setOutputFont("Menlo", 12, false, false);
        wxYield();
        pane->AdoptMeasuredMetrics(7, fromFont - 2);
        check(pane->LineHeight() == fromFont - 2 + 2,
              "line spacing: ...and with no override the measurement still wins");

        doc->m_iLineSpacing = (unsigned short) savedSpacing;
        child->setOutputFont("Menlo", 12, false, false);
        wxYield();
    }

    // ── "Show Line Information" produces a tooltip ──────────────────────────
    // Windows answers a hover over ordinary output with "Line N, <weekday,
    // month day, time>", plus ", (note)" or ", (input)" for those two kinds of
    // line (CMUSHView::OnToolTipNeedText, mushview.cpp:4406-4427).  The port's
    // tooltip callback handled miniwindows and MXP hints and then returned
    // EMPTY, so the option -- which defaults ON -- had nothing behind it.
    {
        doc->m_bAutoWrapWindowWidth = false;
        doc->ClearOutput();
        doc->Note("a note line");
        doc->DisplayMsg("typed line\n", 11, USER_INPUT);
        doc->DisplayMsg("plain mud line\n", 15, 0);
        doc->UpdateAllViews(NULL);
        wxYield();

        check(pane->m_fn_get_tooltip != nullptr,
              "line info: the pane has a tooltip callback at all");

        const int lh = pane->LineHeight();
        auto tipForRow = [&](int row) -> std::string {
            return pane->m_fn_get_tooltip
                 ? pane->m_fn_get_tooltip(4, row * lh + lh / 2) : std::string();
        };

        // Find the three lines by what their tooltip says, rather than assuming
        // which pane row each landed on (ClearOutput re-seeds a line of its own).
        std::string note, input, plain;
        for (int row = 0; row < pane->LineCount(); ++row) {
            const std::string t = tipForRow(row);
            if (t.find("(note)")  != std::string::npos) note  = t;
            else if (t.find("(input)") != std::string::npos) input = t;
            else if (!t.empty() && plain.empty())            plain = t;
        }

        check(!plain.empty(), "line info: an ordinary line gets a tooltip");
        check(plain.compare(0, 5, "Line ") == 0,
              "line info: ...which begins with the line number");
        // The date half really is a date, not an empty format run: the weekday
        // name is spelled out, so at least three letters follow the comma.
        check(plain.find(", ") != std::string::npos &&
              plain.size() > 12,
              "line info: ...and carries the timestamp too");
        check(!note.empty(),  "line info: a note line is marked (note)");
        check(!input.empty(), "line info: a typed line is marked (input)");
        check(note != input,  "line info: ...and the two are not the same string");

        // The control: the option gates it.  Without this, a callback that
        // ignored m_bLineInformation would pass everything above.
        doc->m_bLineInformation = 0;
        bool anyTip = false;
        for (int row = 0; row < pane->LineCount(); ++row)
            if (!tipForRow(row).empty()) anyTip = true;
        check(!anyTip, "line info: turning the option off silences it");
        doc->m_bLineInformation = 1;

        doc->ClearOutput();
    }

    // ── the output-buffer fade ──────────────────────────────────────────────
    // Windows dims old text in CMUSHView::OnDraw (mushview.cpp:1096-1121): a
    // line is full intensity for fade_output_buffer_after_seconds, then ramps
    // over fade_output_seconds down to fade_output_opacity_percent.  There is
    // no CMUSHView here, so all three options round-tripped while nothing dimmed.
    //
    // The arithmetic is what is tested -- the pane asks the frame for a line's
    // opacity, so that callback IS the feature.  Times are fed as "arrived N
    // seconds ago" rather than by waiting.
    {
        const long savedAfter   = doc->m_iFadeOutputBufferAfterSeconds;
        const long savedPercent = doc->m_FadeOutputOpacityPercent;
        const long savedSeconds = doc->m_FadeOutputSeconds;

        check(pane->m_fn_line_opacity != nullptr,
              "fade: the pane has an opacity callback");

        doc->m_iFadeOutputBufferAfterSeconds = 10;
        doc->m_FadeOutputOpacityPercent      = 20;    // fade down to 0.2
        doc->m_FadeOutputSeconds             = 8;     // over 8 seconds
        doc->m_timeFadeCancelled             = CTime((time_t) 0);
        if (pane->IsPaused()) pane->SetPaused(false);

        const time_t now = time(NULL);
        auto opacityAgo = [&](int secondsAgo) -> double {
            return pane->m_fn_line_opacity ? pane->m_fn_line_opacity(now - secondsAgo)
                                           : -1.0;
        };
        auto near = [](double a, double b) { return a > b - 0.01 && a < b + 0.01; };

        check(near(opacityAgo(0), 1.0),  "fade: a brand new line is full intensity");
        check(near(opacityAgo(10), 1.0), "fade: ...and still is at the delay itself");
        // Half way down the 8-second ramp: 1.0 - (0.8/8)*4 = 0.6.
        check(near(opacityAgo(14), 0.6), "fade: half way through the ramp it is half dimmed");
        check(near(opacityAgo(18), 0.2), "fade: at the end of the ramp it reaches the limit");
        check(near(opacityAgo(600), 0.2),
              "fade: ...and stops there rather than going to nothing");

        // 0 = disabled, which is the option's default and must not dim at all.
        doc->m_iFadeOutputBufferAfterSeconds = 0;
        check(near(opacityAgo(600), 1.0), "fade: zero seconds disables it entirely");
        doc->m_iFadeOutputBufferAfterSeconds = 10;

        // A line with no timestamp (arrived == 0) must not be treated as
        // ancient and blanked -- that is what an un-stamped line would do.
        check(pane->m_fn_line_opacity && near(pane->m_fn_line_opacity(0), 1.0),
              "fade: a line with no arrival time is left alone");

        // Freezing the output stops the fade: you froze it in order to read it.
        pane->SetPaused(true);
        check(near(opacityAgo(600), 1.0), "fade: frozen output does not fade");
        pane->SetPaused(false);

        // ...and freezing counts as a cancel, so text stays readable for one
        // fade's worth of time after letting it run again.
        check(near(opacityAgo(600), 1.0),
              "fade: ...and unfreezing leaves it readable for a while");

        // A cancel long ago must NOT keep everything bright for ever.
        doc->m_timeFadeCancelled = CTime((time_t) (now - 600));
        check(near(opacityAgo(600), 0.2), "fade: an old cancel has expired");
        // ...and a RECENT one does hold old text at full intensity, which is
        // what scrolling back is for.
        doc->m_timeFadeCancelled = CTime((time_t) (now - 2));
        check(near(opacityAgo(600), 1.0),
              "fade: a recent cancel brings old lines back to full intensity");
        doc->m_timeFadeCancelled = CTime((time_t) 0);

        // Lines really do reach the pane carrying an arrival time.  This is the
        // assertion that matters most, because the arithmetic above is driven
        // with times the test made up: BuildOutputLine ends with
        // `out = std::move (outline)`, so a stamp written to `out` is silently
        // overwritten -- which is exactly what the first version of this fix
        // did, and the app faded nothing at all while every arithmetic check
        // above still passed.
        doc->DisplayMsg("a line with a real arrival time\n", 32, 0);
        doc->UpdateAllViews(NULL);
        wxYield();
        bool anyStamped = false;
        for (int i = 0; i < pane->LineCount(); ++i) {
            const SOutputLine* l = pane->GetLine(i);
            if (l && l->arrived != 0) anyStamped = true;
        }
        check(anyStamped, "fade: a line reaching the pane carries its arrival time");

        // The colour hook is the doc's own TranslateColour, so it dims.
        check(pane->m_fn_translate_colour != nullptr,
              "fade: the pane has a colour-translation hook");
        if (pane->m_fn_translate_colour) {
            const uint32_t white = 0x00FFFFFFu;
            check(pane->m_fn_translate_colour(white, 1.0) == white,
                  "fade: full opacity leaves a colour untouched");
            const uint32_t dim = pane->m_fn_translate_colour(white, 0.5);
            check(dim != white && GetRValue(dim) < 200,
                  "fade: half opacity really dims it");
        }

        doc->m_iFadeOutputBufferAfterSeconds = (unsigned short) savedAfter;
        doc->m_FadeOutputOpacityPercent      = (unsigned short) savedPercent;
        doc->m_FadeOutputSeconds             = (unsigned short) savedSeconds;
        doc->m_timeFadeCancelled             = CTime((time_t) 0);
    }

    // ── a plugin's OnPluginClose fires ONCE per close ───────────────────────
    // ~CPlugin fires ON_PLUGIN_CLOSE and calls SaveState when the doc is
    // deleted, which the teardown's `delete doc` reaches.  An explicit pass
    // earlier in the teardown made every enabled plugin's close handler run
    // TWICE, and the second SaveState persisted the doubled state -- so a
    // plugin counting sessions counted two.  Removed in b356c11; this is the
    // test that was not possible then, because a selftest could not load a
    // plugin at all until SelfTestEnablePluginLoading existed.
    //
    // Counted through a REAL plugin with a REAL script engine, appending to a
    // file, because that is the only way to tell one fire from two.
    {
        SelfTestEnablePluginLoading();

        char logPath[512];
        snprintf(logPath, sizeof(logPath), "/tmp/mc_selftest_close_%d.log",
                 (int) getpid());
        remove(logPath);

        char body[1024];
        snprintf(body, sizeof(body),
                 "function OnPluginClose()\n"
                 "  local h = io.open(\"%s\", \"a\")\n"
                 "  if h then h:write(\"closed\\n\") h:close() end\n"
                 "end", logPath);

        const std::string pluginPath = SelfTestWritePlugin(
            "close", "abcdefabcdefabcdef990011", "CloseCounter", body);
        check(!pluginPath.empty(), "close-count: wrote a plugin file");

        if (!pluginPath.empty()) {
            CMUSHclientDoc* cd = new CMUSHclientDoc();
            cd->m_mush_name   = "zz_selftest_plugin_close";
            cd->m_nWrapColumn = 80;
            cd->m_maxlines    = 5000;
            cd->SetUpOutputWindow();
            // (doc ctor already ran AddDocument -- doc_construct.cpp:521;
            //  a second add left a DANGLING duplicate entry after close)
            CWorldChildFrame* cf =
                new CWorldChildFrame(parent, cd, wxT("zz_selftest_plugin_close"));

            const long rc = cd->LoadPlugin(CString(pluginPath.c_str()));
            check(rc == 0 && cd->m_PluginList.size() == 1,
                  "close-count: the plugin loaded, with its script engine");

            cf->Close(true);          // forced: no prompts
            wxYield();

            int fires = 0;
            if (FILE* lf = fopen(logPath, "rb")) {
                int ch;
                while ((ch = fgetc(lf)) != EOF) if (ch == '\n') ++fires;
                fclose(lf);
            }
            check(fires == 1,
                  "close-count: OnPluginClose fires exactly ONCE per world close");

            remove(logPath);
            remove(pluginPath.c_str());
        }
    }

    // Tear down without running the close/save flow (this doc has no file).
    doc->m_pWorldView = NULL;
    child->Destroy();
    parent->Destroy();
    wxYield();

    printf("WORLD-FRAME-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

#endif // !_WIN32
