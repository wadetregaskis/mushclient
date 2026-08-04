// platform/outputpane_selftest.cpp — in-app conformance test for COutputPane
// (--outputpane-selftest).
//
// COutputPane lives in mushgui_adapters, so the 30 headless mushcore tests
// cannot reach it: it is a wxScrolledWindow and it owns the port's whole
// selection / freeze / scrollback behaviour.  This harness builds a real (never
// shown) pane inside the real wx app, feeds it real styled lines, and dispatches
// real wxMouseEvents through the real event table — the same shape as the
// surface / miniwindow / engines selftests, and registered with ctest the same
// way.
//
// What it pins, and against which canonical Windows arm:
//
//   selection freezes output        CMUSHView::OnLButtonUp   mushview.cpp:2453
//                                   CMUSHView::OnMouseMove   mushview.cpp:2613
//                                   CMUSHView::OnLButtonDblClk mushview.cpp:4837
//   ...and Select All does NOT      CMUSHView::OnEditSelectAll mushview.cpp:4577
//   keep_pause_at_bottom latch      CMUSHView::ScrollToPosition mushview.cpp:4680
//   double-click auto-copies        CMUSHView::OnLButtonDblClk mushview.cpp:4884
//   TextRectangle offset form       CMUSHView::GetTextRectangle mushview.cpp:6698
//   Clear Output empties the pane   DELETE_LIST in CMUSHclientDoc::ClearOutput
//
// Every assertion is written so that "the code did nothing" FAILS.

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/clipbrd.h>
#include "platform/wx_outputpane.h"

#include <cstdio>
#include <string>

namespace {

int g_pass = 0, g_fail = 0;

void opCheck(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

#define EXPECT_FIND(expr, what) opCheck((expr), (what))

void opCheckEq(int got, int want, const char* what)
{
    const bool ok = (got == want);
    printf("  [%s] %s (got %d, want %d)\n", ok ? "PASS" : "FAIL", what, got, want);
    (ok ? g_pass : g_fail)++;
}

// One plain white run.
SOutputLine MakeLine(const char* text, bool pending = false)
{
    SOutputRun run;
    run.text = text;
    run.fg   = 0x00FFFFFFu;
    SOutputLine line;
    line.push_back(run);
    line.pending = pending;
    return line;
}

// Dispatch a mouse event through the pane's real event table.
void SendMouse(COutputPane* pane, wxEventType type, int x, int y,
               bool leftDown = false)
{
    wxMouseEvent evt(type);
    evt.SetPosition(wxPoint(x, y));
    evt.SetEventObject(pane);
    evt.SetId(pane->GetId());
    evt.m_leftDown = leftDown;
    pane->ProcessWindowEvent(evt);
}

// A hidden host frame + pane, rebuilt per test group so no state leaks.
struct Harness {
    wxFrame*      frame = nullptr;
    COutputPane*  pane  = nullptr;

    explicit Harness(int w = 800, int h = 400)
    {
        frame = new wxFrame(nullptr, wxID_ANY, wxT("outputpane-selftest"),
                            wxPoint(-4000, -4000), wxSize(w + 40, h + 40));
        pane  = new COutputPane(frame, 5000);
        pane->SetSize(0, 0, w, h);
        // wxScrolledWindow needs a layout pass before GetClientSize is honest.
        frame->Layout();
        pane->SendSizeEvent();
    }
    ~Harness() { if (frame) frame->Destroy(); }

    void Feed(int n, const char* prefix = "line")
    {
        for (int i = 0; i < n; ++i) {
            char buf[64];
            snprintf(buf, sizeof buf, "%s %04d alpha bravo charlie", prefix, i);
            pane->AppendLine(MakeLine(buf));
        }
    }
};

// ── Clipboard save/restore ───────────────────────────────────────────────────
// The auto-copy test has to look at the real system clipboard — that IS the
// observable behaviour — so it puts back whatever text the user had there when
// it is done, and clears the sentinel outright if there was none.  (A non-text
// clipboard, e.g. an image, cannot be preserved; clearing beats leaving a
// "SENTINEL-…" string behind.)
std::string ClipboardText()
{
    std::string out;
    if (wxTheClipboard->Open()) {
        if (wxTheClipboard->IsSupported(wxDF_TEXT)) {
            wxTextDataObject data;
            if (wxTheClipboard->GetData(data))
                out = std::string(data.GetText().utf8_str());
        }
        wxTheClipboard->Close();
    }
    return out;
}

void SetClipboardText(const std::string& s)
{
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(s.c_str())));
        wxTheClipboard->Close();
    }
}

// ── 1. Selection freezes output (world option auto_pause / m_bAutoFreeze) ────
void TestSelectionFreezesOutput()
{
    printf("selection freezes output (mushview.cpp:2453 / :2613 / :4837)\n");

    {
        Harness h;
        h.Feed(10);
        opCheck(!h.pane->IsPaused(), "a fresh pane is auto-scrolling");

        const int cw = h.pane->CharWidth();
        const int lh = h.pane->LineHeight();
        // Drag across five characters of the first visible row.
        SendMouse(h.pane, wxEVT_LEFT_DOWN, 0, lh / 2);
        opCheck(!h.pane->IsPaused(),
                "pressing the button (an EMPTY region) does not freeze");
        SendMouse(h.pane, wxEVT_MOTION, cw * 5, lh / 2, /*leftDown*/ true);
        opCheck(h.pane->IsPaused(),
                "dragging out a region freezes DURING the drag, not at release");
        SendMouse(h.pane, wxEVT_LEFT_UP, cw * 5, lh / 2);
        opCheck(h.pane->IsPaused(), "...and it is still frozen after the release");
    }

    {
        // A plain click (down and up in the same place) leaves an empty region,
        // and get_selection returning TRUE means Windows does NOT freeze.
        Harness h;
        h.Feed(10);
        const int lh = h.pane->LineHeight();
        SendMouse(h.pane, wxEVT_LEFT_DOWN, 30, lh / 2);
        SendMouse(h.pane, wxEVT_LEFT_UP,   30, lh / 2);
        opCheck(!h.pane->IsPaused(), "a click with no drag does not freeze");
    }

    {
        // auto_pause off (m_bAutoFreeze == 0): the `&& pDoc->m_bAutoFreeze` arm.
        Harness h;
        h.Feed(10);
        h.pane->SetAllowAutoFreeze(false);
        const int cw = h.pane->CharWidth();
        const int lh = h.pane->LineHeight();
        SendMouse(h.pane, wxEVT_LEFT_DOWN, 0, lh / 2);
        SendMouse(h.pane, wxEVT_MOTION, cw * 5, lh / 2, true);
        SendMouse(h.pane, wxEVT_LEFT_UP, cw * 5, lh / 2);
        opCheck(!h.pane->IsPaused(),
                "with auto-freeze off a selection does NOT freeze");
    }

    {
        // The scripting arm: world.SetSelection → IWorldView::setOutputSelection
        // → COutputPane::SetSelectionRange + FreezeIfSelection
        // (pal/win_worldview.cpp:639 is the Windows twin).  SetSelectionRange on
        // its own must NOT freeze, because Select All shares it and
        // CMUSHView::OnEditSelectAll has no freeze.
        Harness h;
        h.Feed(10);
        h.pane->SelectAll();
        opCheck(!h.pane->IsPaused(), "Select All does not freeze (Windows does not)");
        h.pane->FreezeIfSelection();
        opCheck(h.pane->IsPaused(), "world.SetSelection's explicit freeze does");
    }

    {
        // Nothing selected: the freeze must not fire on an empty region even
        // when it is asked for directly.
        Harness h;
        h.Feed(10);
        h.pane->FreezeIfSelection();
        opCheck(!h.pane->IsPaused(), "no selection means nothing to freeze for");
    }
}

// ── 2. Double-click selects a word AND copies it ────────────────────────────
void TestDoubleClickAutoCopy()
{
    printf("double-click honours copy_selection_to_clipboard (mushview.cpp:4884)\n");

    const std::string saved = ClipboardText();

    {
        Harness h;
        h.pane->AppendLine(MakeLine("alpha bravo charlie"));
        h.pane->SetCopySelectionToClipboard(true);
        h.pane->SetAutoCopyInHTML(false);
        SetClipboardText("SENTINEL-not-copied");

        const int cw = h.pane->CharWidth();
        const int lh = h.pane->LineHeight();
        // "bravo" starts at column 6; aim at its middle (column 8).
        SendMouse(h.pane, wxEVT_LEFT_DOWN,   cw * 8 + cw / 2, lh / 2);
        SendMouse(h.pane, wxEVT_LEFT_DCLICK, cw * 8 + cw / 2, lh / 2);
        SendMouse(h.pane, wxEVT_LEFT_UP,     cw * 8 + cw / 2, lh / 2);

        opCheck(h.pane->GetSelectionText() == wxT("bravo"),
                "double-click selects the word under the cursor");
        opCheck(ClipboardText() == "bravo",
                "...and copy_selection_to_clipboard puts it on the clipboard");
        // The THIRD site of the same two Windows lines (mushview.cpp:4837),
        // which runs before the word is dispatched.  Without its own assertion
        // the drag-path checks in section 1 would cover for it.
        opCheck(h.pane->IsPaused(),
                "...and the double-click froze output, as the drag does");
    }

    {
        // ...but only when the world wants auto-freeze: the Windows condition
        // is `!get_selection (newrgn) && pDoc->m_bAutoFreeze`, so a world with
        // auto_pause off must keep scrolling even with a word selected.
        // (Without this arm, a freeze that ignores the option also passes.)
        Harness h;
        h.pane->AppendLine(MakeLine("alpha bravo charlie"));
        h.pane->SetAllowAutoFreeze(false);

        const int cw = h.pane->CharWidth();
        const int lh = h.pane->LineHeight();
        SendMouse(h.pane, wxEVT_LEFT_DOWN,   cw * 8 + cw / 2, lh / 2);
        SendMouse(h.pane, wxEVT_LEFT_DCLICK, cw * 8 + cw / 2, lh / 2);
        SendMouse(h.pane, wxEVT_LEFT_UP,     cw * 8 + cw / 2, lh / 2);

        opCheck(h.pane->GetSelectionText() == wxT("bravo"),
                "the word is still selected with auto-freeze off");
        opCheck(!h.pane->IsPaused(),
                "...but output is NOT frozen (m_bAutoFreeze gates it)");
    }

    {
        // Option off: the clipboard must be left alone.
        Harness h;
        h.pane->AppendLine(MakeLine("alpha bravo charlie"));
        h.pane->SetCopySelectionToClipboard(false);
        SetClipboardText("SENTINEL-untouched");

        const int cw = h.pane->CharWidth();
        const int lh = h.pane->LineHeight();
        SendMouse(h.pane, wxEVT_LEFT_DOWN,   cw * 8 + cw / 2, lh / 2);
        SendMouse(h.pane, wxEVT_LEFT_DCLICK, cw * 8 + cw / 2, lh / 2);
        SendMouse(h.pane, wxEVT_LEFT_UP,     cw * 8 + cw / 2, lh / 2);

        opCheck(h.pane->GetSelectionText() == wxT("bravo"),
                "the word is still selected with the option off");
        opCheck(ClipboardText() == "SENTINEL-untouched",
                "...but the clipboard is not touched");
    }

    {
        // With a handler wired: it is given the word under the cursor, and its
        // answer decides whether the auto-copy runs.  CMUSHView returns straight
        // out of OnLButtonDblClk on eDblClickAbort (mushview.cpp:4860, the "not
        // connected, and the user cancelled the prompt" case), which skips both
        // the selection tidy-up and the OnEditCopy below it — so a pane that
        // copied anyway would put text on the clipboard for a click the user
        // just cancelled.
        Harness h;
        h.pane->AppendLine(MakeLine("alpha bravo charlie"));
        h.pane->SetCopySelectionToClipboard(true);
        SetClipboardText("SENTINEL-aborted");

        wxString seen;
        h.pane->m_fn_word_double_clicked =
            [&seen](const wxString& word, bool) -> bool {
                seen = word;
                return false;             // eDblClickAbort
            };

        const int cw = h.pane->CharWidth();
        const int lh = h.pane->LineHeight();
        SendMouse(h.pane, wxEVT_LEFT_DOWN,   cw * 8 + cw / 2, lh / 2);
        SendMouse(h.pane, wxEVT_LEFT_DCLICK, cw * 8 + cw / 2, lh / 2);
        SendMouse(h.pane, wxEVT_LEFT_UP,     cw * 8 + cw / 2, lh / 2);

        opCheck(seen == wxT("bravo"),
                "the handler is given the word under the cursor");
        opCheck(ClipboardText() == "SENTINEL-aborted",
                "...and eDblClickAbort skips the auto-copy, as Windows does");
        // The capture/drag reset moved AHEAD of the dispatch, so it holds
        // whether or not a handler is wired: the platform's trailing LEFT_UP
        // must not re-enter the drag branch and collapse the word.
        opCheck(h.pane->GetSelectionText() == wxT("bravo"),
                "...and the trailing mouse-up leaves the word selected");
    }

    {
        // The same wiring, but the handler accepts: the copy DOES run.  Without
        // this arm a pane that never copies once a handler exists also passes.
        Harness h;
        h.pane->AppendLine(MakeLine("alpha bravo charlie"));
        h.pane->SetCopySelectionToClipboard(true);
        SetClipboardText("SENTINEL-accepted");
        h.pane->m_fn_word_double_clicked =
            [](const wxString&, bool) -> bool { return true; };

        const int cw = h.pane->CharWidth();
        const int lh = h.pane->LineHeight();
        SendMouse(h.pane, wxEVT_LEFT_DOWN,   cw * 8 + cw / 2, lh / 2);
        SendMouse(h.pane, wxEVT_LEFT_DCLICK, cw * 8 + cw / 2, lh / 2);
        SendMouse(h.pane, wxEVT_LEFT_UP,     cw * 8 + cw / 2, lh / 2);

        opCheck(ClipboardText() == "bravo",
                "a handler that does not abort still gets the auto-copy");
    }

    if (!saved.empty())
        SetClipboardText(saved);
    else if (wxTheClipboard->Open()) {
        wxTheClipboard->Clear();
        wxTheClipboard->Close();
    }
}

// ── 3. Clear Output takes the in-progress prompt row with it ────────────────
void TestClearOutputDropsPendingLine()
{
    printf("Clear Output empties the pane completely\n");

    Harness h;
    h.pane->AppendLine(MakeLine("You are in a dark room."));
    h.pane->AppendLine(MakeLine("HP:100 MP:50 > ", /*pending*/ true));
    opCheckEq(h.pane->LineCount(), 1, "one committed line before the clear");
    opCheck(h.pane->HasPendingLine(), "...and a pending prompt row");

    h.pane->ClearOutput();
    opCheckEq(h.pane->LineCount(), 0, "Clear Output removes the committed lines");
    opCheck(!h.pane->HasPendingLine(),
            "...and the pending prompt row, which used to be stranded forever");

    // And the normal path still works: a completed line clears the pending row.
    h.pane->AppendLine(MakeLine("HP:99 MP:50 > ", true));
    opCheck(h.pane->HasPendingLine(), "a new pending row shows again");
    h.pane->AppendLine(MakeLine("A goblin arrives."));
    opCheck(!h.pane->HasPendingLine(),
            "a completed line still supersedes the pending row");
}

// ── 4. keep_pause_at_bottom (m_bKeepFreezeAtBottom) ─────────────────────────
void TestKeepPauseAtBottom()
{
    printf("keep_pause_at_bottom latches the freeze (mushview.cpp:4680)\n");

    // Off (the default): scrolling back down resumes output.
    {
        Harness h;
        h.Feed(400);
        h.pane->SetKeepPauseAtBottom(false);
        h.pane->ScrollPage(-1);
        opCheck(h.pane->IsPaused(), "scrolling up freezes");
        for (int i = 0; i < 8; ++i) h.pane->ScrollPage(+1);
        opCheck(!h.pane->IsPaused(),
                "...and with the option OFF, reaching the bottom resumes");
    }

    // On: reaching the bottom must NOT resume — that is the whole option.
    {
        Harness h;
        h.Feed(400);
        h.pane->SetKeepPauseAtBottom(true);
        h.pane->ScrollPage(-1);
        opCheck(h.pane->IsPaused(), "scrolling up freezes");
        for (int i = 0; i < 8; ++i) h.pane->ScrollPage(+1);
        opCheck(h.pane->IsPaused(),
                "...and with the option ON, reaching the bottom stays frozen");

        // ...but an EXPLICIT unfreeze still works, else the pane would be stuck.
        h.pane->ScrollToBottom();
        opCheck(!h.pane->IsPaused(), "an explicit unfreeze still resumes");
        // ...and stays resumed once more scrolling happens at the bottom.
        h.pane->ScrollPage(+1);
        opCheck(!h.pane->IsPaused(),
                "the latch does not re-freeze a pane that is already running");
    }
}

// ── 5. world.TextRectangle's offset-from-edge form ──────────────────────────
void TestTextRectangleOffsetForm()
{
    printf("world.TextRectangle(0,0,-200,0) reserves the right 200px "
           "(mushview.cpp:6698)\n");

    Harness h(800, 400);
    int clientW = 0;
    h.pane->GetClientSize(&clientW, nullptr);
    const int cw = h.pane->CharWidth();
    if (cw <= 0 || clientW <= 0) {
        opCheck(false, "pane has a measurable client size and font");
        return;
    }

    h.Feed(5);
    const int fullCols = h.pane->WrapColumns();
    opCheckEq(fullCols, clientW / cw, "no rectangle means the whole client width");

    // The documented offset form: right <= 0 means "in from the right edge".
    int rl = 0, rt = 0, rr = -200, rb = 0;
    h.pane->m_fn_text_rect = [&](int& l, int& t, int& r, int& b) -> bool {
        l = rl; t = rt; r = rr; b = rb;
        return true;
    };
    h.pane->SendSizeEvent();          // forces RebuildWrapCache
    opCheckEq(h.pane->WrapColumns(), (clientW - 200) / cw,
              "offset form narrows the text to clientW - 200");
    opCheck(h.pane->WrapColumns() < fullCols,
            "...which is genuinely narrower than the unreserved pane");

    // The absolute form still works unchanged.
    rr = 300;
    h.pane->SendSizeEvent();
    opCheckEq(h.pane->WrapColumns(), 300 / cw,
              "an absolute right edge is still taken literally");

    // An offset larger than the window cannot invert the rectangle: Windows
    // floors it at left + 20.
    rl = 0; rr = -5000;
    h.pane->SendSizeEvent();
    opCheckEq(h.pane->WrapColumns(), 20 / cw > 1 ? 20 / cw : 1,
              "an over-large offset is floored at 20 pixels wide");
}

// ── 6. Leaving the pane cancels a hotspot mouse-over ────────────────────────
// CMUSHView::WindowProc handles WM_MOUSELEAVE by feeding the miniwindow layer a
// move to (-1,-1) (mushview.cpp:6785), which miniwindow_compose.cpp:680 turns
// into CancelMouseOver for whatever hotspot was lit.  Without it a hovered
// button stays highlighted forever once the pointer leaves the pane — straight
// down onto the command bar is enough — and the plugin's internal "currently
// hovered" state stays set until the pointer comes back and lands somewhere
// else.  Nearly every WindowAddHotspot user passes a CancelMouseOver.
void TestLeaveWindowCancelsMouseOver()
{
    printf("leaving the pane cancels the hotspot mouse-over (mushview.cpp:6785)\n");

    Harness h;
    h.pane->AppendLine(MakeLine("alpha bravo charlie"));

    int  calls = 0, lastX = -99, lastY = -99;
    h.pane->m_fn_miniwindow_mouse_move = [&](int x, int y) -> bool {
        ++calls; lastX = x; lastY = y;
        return false;                    // not handled: fall through as usual
    };

    // A real move first, so "the handler is never called at all" cannot pass
    // the leave check by accident.
    SendMouse(h.pane, wxEVT_MOTION, 40, 8);
    opCheckEq(calls, 1, "a move over the pane reaches the miniwindow layer");
    opCheck(lastX == 40 && lastY == 8, "...with the real pointer position");

    SendMouse(h.pane, wxEVT_LEAVE_WINDOW, 40, 500);
    opCheckEq(calls, 2, "leaving the pane reaches it again");
    opCheck(lastX == -1 && lastY == -1,
            "...as the out-of-bounds point Windows sends (-1,-1)");
}

// ── 7. Find: same answers, one scan ───────────────────────────────────────
//
// FindNext used to build a fresh wxString for EVERY line (decode + allocate)
// and then a second one from .Lower(), and then scan the whole buffer AGAIN to
// label the match "n of m" -- the 9999 cap bounds the match count, not the
// lines walked.  Two full passes with two allocations per line, per keypress.
//
// It now matches on the raw UTF-8 run bytes with ASCII folding into one reused
// buffer, and caches the matching line indices per (term, case flag, buffer
// generation) so repeated Find Next is a binary search.
//
// The assertions are about ANSWERS, not speed: same match sequence, same
// ordinal and total, same case behaviour, and the cache invalidating when the
// buffer changes underneath it.
void TestFindNext()
{
    printf("Find: same answers, one scan\n");

    Harness h;
    h.pane->AppendLine(MakeLine("alpha needle one"));
    h.pane->AppendLine(MakeLine("beta nothing"));
    h.pane->AppendLine(MakeLine("gamma NEEDLE two"));
    h.pane->AppendLine(MakeLine("delta nothing"));
    h.pane->AppendLine(MakeLine("epsilon needle three"));

    int line = -1, ord = 0, total = 0;

    // Case-INsensitive: all three, and the ordinal/total come off the cache.
    EXPECT_FIND(h.pane->FindNext("needle", true, &line, &ord, &total, false),
                "case-insensitive find succeeds");
    opCheckEq(total, 3, "...and reports every matching line");
    const int first = line;
    opCheck(first >= 0, "...at a real line");

    EXPECT_FIND(h.pane->FindNext("needle", true, &line, &ord, &total, false),
                "Find Next advances");
    opCheck(line != first, "...to a different line");
    opCheckEq(total, 3, "...with the same total");

    // Case-SENSITIVE: only the lower-case two.  Without this arm an ASCII fold
    // applied unconditionally would pass everything above.
    EXPECT_FIND(h.pane->FindNext("NEEDLE", true, &line, &ord, &total, true),
                "case-sensitive find succeeds");
    opCheckEq(total, 1, "...and matches ONLY the upper-case line");
    opCheckEq(line, 2, "...which is the one that really has it");

    EXPECT_FIND(h.pane->FindNext("needle", true, &line, &ord, &total, true),
                "case-sensitive lower-case find succeeds");
    opCheckEq(total, 2, "...matching the two lower-case lines");

    // A term that is not there at all.
    opCheck(!h.pane->FindNext("haystack", true, &line, &ord, &total, false),
            "a term with no match returns false");

    // The cache must notice the buffer changing under it.  This has to repeat
    // the SAME (term, case flag) as the search immediately before it, or a
    // differing cache KEY would force the rebuild and the invalidation would go
    // untested -- which is exactly what the first version of this block did.
    EXPECT_FIND(h.pane->FindNext("needle", true, &line, &ord, &total, false),
                "a case-insensitive search, to prime the cache");
    opCheckEq(total, 3, "...priming total");

    h.pane->AppendLine(MakeLine("zeta needle four"));
    EXPECT_FIND(h.pane->FindNext("needle", true, &line, &ord, &total, false),
                "the same search again after an append still succeeds");
    opCheckEq(total, 4, "...and sees the new line (the cache was invalidated)");

    // ...and the same for a TRIM, which is the other way the buffer moves.
    // The cap has a floor of 100 in the pane (SetMaxLines), so the fixture has
    // to be bigger than that for a trim to happen at all -- asking for 2 would
    // silently be 100 and drop nothing.
    h.Feed(120, "filler");                       // 6 + 120 = 126 lines
    // Prime the cache AFTER the last append, so the only thing that can
    // invalidate it before the next search is the trim itself.  Priming before
    // the Feed would let the appends do the invalidating and leave the trim's
    // own bookkeeping untested -- which is what the first version did.
    EXPECT_FIND(h.pane->FindNext("needle", true, &line, &ord, &total, false),
                "priming the cache with the buffer at full size");
    opCheckEq(total, 4, "...still four matches before the trim");

    h.pane->SetMaxLines(100);                    // drops the oldest 26...
    EXPECT_FIND(!h.pane->FindNext("needle", true, &line, &ord, &total, false),
                "the same search after a trim finds nothing");
    opCheckEq(total, 0, "...because every match was trimmed away");
    h.pane->SetMaxLines(5000);

    // ...and clearing invalidates it the other way.
    h.pane->ClearOutput();
    opCheck(!h.pane->FindNext("needle", true, &line, &ord, &total, false),
            "after Clear Output there is nothing to find");
}

} // namespace

int RunOutputPaneSelfTest()
{
    printf("OUTPUTPANE-SELFTEST: driving the real COutputPane\n");

    TestSelectionFreezesOutput();
    TestDoubleClickAutoCopy();
    TestClearOutputDropsPendingLine();
    TestKeepPauseAtBottom();
    TestTextRectangleOffsetForm();
    TestLeaveWindowCancelsMouseOver();
    TestFindNext();

    printf("OUTPUTPANE-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

#endif // !_WIN32
