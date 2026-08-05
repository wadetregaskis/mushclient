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

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "childfrm.h"     // CChildFrame  ) the world doc template's
#include "MUSHview.h"     // CMUSHView    ) runtime classes
#include "resource.h"     // IDR_MUSHCLTYPE
#include "platform/wx_world_child.h"
#include "platform/wx_outputpane.h"
#include "platform/wx_mfc_hooks.h"   // g_wxhook_message_box — answer the
                                     // clear-buffer confirmation prompt

#include <cstdio>

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
        pane->ClearOutput();

        // An ANSI-coloured line (index 2 = green) and an RGB one.  Only the
        // first may move when the palette does.
        doc->DisplayMsg("\x1b[32mansi green\x1b[0m\n", 21, 0);
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
        pane->ClearOutput();
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

        pane->ClearOutput();
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
        pane->ClearOutput();
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

    // Tear down without running the close/save flow (this doc has no file).
    doc->m_pWorldView = NULL;
    child->Destroy();
    parent->Destroy();
    wxYield();

    printf("WORLD-FRAME-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

#endif // !_WIN32
