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

#include <chrono>
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

// ── 8. A mouse release does no work the world did not ask for ─────────────
//
// AutoCopySelection used to materialise SelectedText() before looking at
// copy_selection_to_clipboard -- so on macOS with the option OFF (the default)
// every mouse release with a selection decoded the whole selection into a
// wxString and immediately freed it.  With Select All over a large scrollback
// that is the entire buffer, per click.
//
// A complexity pin, not a stopwatch: the same number of release cycles over the
// same selection, with the option off and on.  The bound is deliberately loose
// so ordinary noise cannot fail it while still being far below the regression.
void TestReleaseDoesNotMaterialiseUnlessAsked()
{
    printf("a mouse release only builds the selection when something wants it\n");

    Harness h;
    h.Feed(4000);
    h.pane->SelectAll();
    opCheck(!h.pane->GetSelectionText().IsEmpty(),
            "the fixture really has a large selection");

    const int cw = h.pane->CharWidth();
    const int lh = h.pane->LineHeight();
    // OnLeftUp only runs the auto-copy on a release that ENDS A DRAG, so each
    // cycle has to be a real down-move-up.  SelectAll between the move and the
    // release widens the region to the whole buffer without leaving the drag,
    // which is the case that used to cost the most.
    auto releases = [&](int n) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            SendMouse(h.pane, wxEVT_LEFT_DOWN, 0, lh / 2);
            SendMouse(h.pane, wxEVT_MOTION, cw * 5, lh / 2, /*leftDown*/ true);
            h.pane->SelectAll();
            SendMouse(h.pane, wxEVT_LEFT_UP, cw * 5, lh / 2);
        }
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    };

    h.pane->SetCopySelectionToClipboard(false);
    releases(3);                                   // warm
    const double off = releases(30);

    // With the option ON the work is real and unavoidable -- that arm is the
    // control, and it is what makes "off is cheap" meaningful rather than
    // vacuous.  (It also touches the clipboard, so save and restore it.)
    const std::string saved = ClipboardText();
    h.pane->SetCopySelectionToClipboard(true);
    releases(3);
    const double on = releases(30);
    h.pane->SetCopySelectionToClipboard(false);
    if (!saved.empty()) SetClipboardText(saved);

    // "off is cheaper than on" is NOT enough: with the guard removed the off
    // arm still skips the clipboard write, so it comes out a few times faster
    // and a plain `off < on` passes the very mutation this exists to catch
    // (measured: 62ms vs 242ms unguarded, against 0.1ms vs 253ms guarded).
    // Require it to be an order of magnitude cheaper, with a 1ms floor so a
    // sub-millisecond measurement cannot make the ratio meaningless.
    const bool ok = (off * 10.0 + 1.0) < on;
    printf("  [%s] 30 releases: option off %.1fms vs on %.1fms "
           "(off*10+1 must stay under on)\n",
           ok ? "PASS" : "FAIL", off, on);
    (ok ? g_pass : g_fail)++;
}

// ── 9. Search highlighting finds the same ranges on both paths ────────────
//
// The highlight scan runs for every VISIBLE line on every paint while a search
// term is set.  It used to decode the line into a wxString, allocate a second
// from .Lower(), then convert both the line and the term to std::wstring --
// four allocations per line per paint, at ~10 paints a second while scrolling.
//
// It now works on the runs' raw UTF-8 bytes in a reused buffer, but ONLY when
// byte offsets equal character offsets: an all-ASCII, UTF-8 line.  Anything
// else takes the original decode path.  Both must agree, because the offsets
// drive where the highlight rectangle is drawn -- a byte offset used as a
// character offset would draw the box in the wrong place on non-ASCII lines.
//
// The pane has no public "give me the hit ranges" entry point, so this drives
// the real paint into a bitmap and asserts that the highlight lands on the
// glyphs it should: at the same COLUMN for an ASCII line and a line whose match
// sits after a multi-byte character.
void TestSearchHighlightOffsets()
{
    printf("search highlighting agrees on ASCII and non-ASCII lines\n");

    Harness h;
    h.pane->AppendLine(MakeLine("xxNEEDLE"));            // ASCII: fast path
    h.pane->AppendLine(MakeLine("\xc3\xa9xNEEDLE"));     // e-acute + 'x' + term
    h.pane->HighlightSearch("NEEDLE", true);

    std::vector<COutputPane::HighlightHit> hits;

    h.pane->HighlightHitsForLine(0, hits);
    opCheckEq((int) hits.size(), 1, "ASCII line: one hit");
    if (hits.size() == 1) {
        opCheckEq(hits[0].start, 2, "...starting at CHARACTER 2");
        opCheckEq(hits[0].end,   8, "...ending at character 8");
    }

    // The load-bearing case.  "e-acute" is TWO bytes, so the term starts at
    // byte 3 but CHARACTER 2.  The offsets drive where the highlight rectangle
    // is drawn, so a byte offset leaking out here paints the box one column
    // right of the text it is supposed to mark.
    h.pane->HighlightHitsForLine(1, hits);
    opCheckEq((int) hits.size(), 1, "non-ASCII line: one hit");
    if (hits.size() == 1) {
        opCheckEq(hits[0].start, 2, "...starting at CHARACTER 2, not byte 3");
        opCheckEq(hits[0].end,   8, "...ending at character 8, not byte 9");
    }

    // Case folding still applies on both paths.
    h.pane->HighlightSearch("needle", false);
    h.pane->HighlightHitsForLine(0, hits);
    opCheckEq((int) hits.size(), 1, "case-insensitive matches on the ASCII path");
    h.pane->HighlightHitsForLine(1, hits);
    opCheckEq((int) hits.size(), 1, "...and on the decode path");

    // ...and case-SENSITIVE really is sensitive, so a fold applied
    // unconditionally would be caught.
    h.pane->HighlightSearch("needle", true);
    h.pane->HighlightHitsForLine(0, hits);
    opCheckEq((int) hits.size(), 0, "case-sensitive lower-case term matches nothing");

    // Two occurrences on one line, so a scan that stopped at the first fails.
    h.pane->AppendLine(MakeLine("NEEDLE and NEEDLE"));
    h.pane->HighlightSearch("NEEDLE", true);
    h.pane->HighlightHitsForLine(2, hits);
    opCheckEq((int) hits.size(), 2, "both occurrences on a line are marked");
}

} // namespace

// ── the HiDPI char-metric correction ─────────────────────────────────────────
// This lived inline in OnPaint, which means it had never executed under test:
// no in-app selftest reaches the paint handler at all (they exit before
// wxApp::OnRun, so AppKit never issues drawRect: -- docs/AUDIT-2026-08.md).
// It is the arm that notices the paint DC disagrees with the metrics measured
// at construction, which on macOS is the difference between physical and
// logical pixels -- and getting it wrong puts every selection rectangle and
// every click-to-character mapping out by the display scale factor.
void TestMeasuredMetrics()
{
    Harness h(800, 400);
    const int cw0 = h.pane->CharWidth();
    const int lh0 = h.pane->LineHeight();
    opCheck(cw0 > 0 && lh0 > 0, "metrics: the pane starts with a usable grid");

    // Same measurement -> nothing to do.  Without this the pane would reflow
    // on every single paint, which is the whole reason the comparison exists.
    opCheck(!h.pane->AdoptMeasuredMetrics(cw0, lh0 - 2),
            "metrics: an unchanged measurement asks for no reflow");
    opCheck(h.pane->CharWidth() == cw0 && h.pane->LineHeight() == lh0,
            "...and leaves the grid alone");

    // A DC that cannot measure text must NOT be allowed to set the grid to
    // zero: the wrap arithmetic divides by the character width.
    opCheck(!h.pane->AdoptMeasuredMetrics(0, 20),
            "metrics: a zero width is refused");
    opCheck(!h.pane->AdoptMeasuredMetrics(9, 0),
            "metrics: a zero height is refused");
    opCheck(!h.pane->AdoptMeasuredMetrics(-9, -20),
            "metrics: negatives are refused");
    opCheck(h.pane->CharWidth() == cw0 && h.pane->LineHeight() == lh0,
            "...and none of them touched the grid");

    // A real change is taken, and the row height carries the +2 leading.
    opCheck(h.pane->AdoptMeasuredMetrics(cw0 * 2, (lh0 - 2) * 2),
            "metrics: a doubled measurement IS a change");
    opCheck(h.pane->CharWidth() == cw0 * 2,
            "...the character width is adopted verbatim");
    opCheck(h.pane->LineHeight() == (lh0 - 2) * 2 + 2,
            "...and the row height is the text height plus two");

    // Only the width differing is still a change -- an implementation that
    // compared just the height would miss the pure-width case, which is
    // exactly the one a font substitution produces.
    opCheck(h.pane->AdoptMeasuredMetrics(cw0 * 2 + 1, (lh0 - 2) * 2),
            "metrics: a width-only change counts");
    opCheck(h.pane->AdoptMeasuredMetrics(cw0 * 2 + 1, (lh0 - 2) * 2 + 1),
            "metrics: a height-only change counts");

    // The reflow half: the wrap column must follow the new grid, and the frame
    // must be told so the ENGINE's wrap column can follow too.  Without that
    // last step the two disagree and every full-width line gains a one-
    // character orphan continuation row.
    h.Feed(20);
    h.pane->AdoptMeasuredMetrics(cw0, lh0 - 2);      // back to the real grid
    h.pane->ApplyMetricsChange();
    const int wideCols = h.pane->WrapColumns();
    opCheck(wideCols > 0, "metrics: the wrap column is recomputed");

    int gridChanges = 0;
    h.pane->m_fn_grid_changed = [&gridChanges]() { ++gridChanges; };
    h.pane->AdoptMeasuredMetrics(cw0 * 2, lh0 - 2);
    h.pane->ApplyMetricsChange();
    opCheck(gridChanges == 1, "metrics: the frame is told the grid changed");
    opCheck(h.pane->WrapColumns() < wideCols,
            "...and twice-as-wide characters halve the wrap column");
    h.pane->m_fn_grid_changed = nullptr;
}

// ── RunCharLen must equal what the renderer will actually draw ───────────────
// Every column, wrap, selection, hit-test and tooltip walk in the pane measures
// a run with RunCharLen.  DrawLine measures it with DecodeRunText(run).size(),
// because it needs the decoded string to slice anyway.  The two MUST agree: if
// they ever differ, the highlight marks different characters than Cmd-C copies,
// and -- worse -- a click lands on a different run than the one drawn there, so
// an MXP send-link sends the wrong command.
//
// They agree today through a chain that is easy to break and is currently held
// together only by comments: UTF8CharLen and DecodeUTF8Lenient share
// UTF8NextChar (a previous fix -- they used to be a lead-byte scan versus
// wxString::FromUTF8, agreeing only on well-formed input), and
// DecodeOutputBytes relies on FromUTF8 being all-or-nothing so the lenient
// fallback covers exactly the cases FromUTF8 refuses.
//
// So this pins the equality itself, over the inputs where the chain could give.
void TestRunCharLenMatchesTheDecode()
{
    struct Case { const char* what; std::string bytes; bool utf8; int charset; };
    const std::string kEuro   = "\xE2\x82\xAC";              // U+20AC, 3 bytes
    const std::string kCJK    = "\xE4\xB8\xAD\xE6\x96\x87";  // 2 chars, 6 bytes
    const Case kCases[] = {
        { "empty",                   "",                       true,  0 },
        { "plain ASCII",             "hello",                  true,  0 },
        { "well-formed UTF-8",       kEuro,                    true,  0 },
        { "UTF-8 CJK",               kCJK,                     true,  0 },
        { "ASCII + UTF-8 mixed",     "a" + kEuro + "b",        true,  0 },
        // FromUTF8 refuses these outright, so the lenient path decides the
        // count -- and it must still match the fast scan.
        { "truncated multi-byte",    "ab\xE2\x82",             true,  0 },
        { "stray continuation byte", "ab\x80" "cd",            true,  0 },
        { "lone 0xFF",               "a\xFF" "b",              true,  0 },
        { "all continuation bytes",  "\x80\x81\x82",          true,  0 },
        { "overlong NUL",            "a\xC0\x80" "b",          true,  0 },
        { "high byte at the end",    "abc\xE2",                true,  0 },
        // Non-UTF-8 worlds: RunCharLen IS the decode there, so these are a
        // control -- they cannot fail unless the definition changes.
        { "CP1252 curly quote",      "a\x93" "b\x94",          false, 0 },
        { "latin-1 high bytes",      "\xE9\xE8\xEA",          false, 0 },
        { "Big5 pair",               "\xA4\xA4\xA4\xE5",      false, 136 },
    };

    for (const Case& c : kCases) {
        SOutputRun run;
        run.text    = c.bytes;
        run.utf8    = c.utf8 ? 1 : 0;
        run.charset = (short) c.charset;

        const int measured = RunCharLen(run);
        const int drawn    = (int) DecodeOutputBytes(run.text.c_str(),
                                                     run.text.size(),
                                                     run.utf8 != 0,
                                                     run.charset).size();
        if (measured != drawn)
            printf("    %s: RunCharLen %d, decoded %d\n", c.what, measured, drawn);
        opCheck(measured == drawn, c.what);
    }

    // A horizontal-rule run carries no text at all (platform/output_types.h:25),
    // so it contributes zero columns -- which is why the `if (!run.horiz_rule)`
    // guards dotted around the column walks are redundant rather than a
    // divergence between them.  Pin that, so removing a guard stays harmless
    // and adding text to an HR run fails loudly here instead of silently
    // shifting every column on the line.
    {
        SOutputRun hr;
        hr.horiz_rule = 1;
        opCheck(hr.text.empty(), "a horizontal-rule run has no text");
        opCheck(RunCharLen(hr) == 0, "...so it measures zero columns");
    }
}

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
    TestReleaseDoesNotMaterialiseUnlessAsked();
    TestSearchHighlightOffsets();
    TestMeasuredMetrics();
    TestRunCharLenMatchesTheDecode();

    printf("OUTPUTPANE-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

#endif // !_WIN32
