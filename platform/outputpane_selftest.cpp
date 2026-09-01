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
//   mid-paint prints are deferred   (port-only: Windows relies on the
//                                    documented draw-only discipline instead)
//
// Every assertion is written so that "the code did nothing" FAILS.

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/clipbrd.h>
#include "platform/wx_outputpane.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

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
// observable behaviour of copy_selection_to_clipboard, and a fake would only
// prove the pane can call a fake.  So these read and write the real one.
std::string ClipboardText()
{
    std::string out;
    if (!wxTheClipboard->Open()) return out;
    // NOT `IsSupported(wxDF_TEXT)` on its own, which is what this used to ask.
    // Measured on macOS 15 with a string put there by another process (pbcopy):
    // IsSupported(wxDF_TEXT) is FALSE while IsSupported(wxDF_UNICODETEXT) is
    // true and GetData succeeds -- so every read of text this app did not
    // itself write came back empty.  The save half of save/restore was
    // therefore dead: the tests below concluded the clipboard had been empty
    // and CLEARED it on the way out, wiping whatever the user had there on
    // every single run.  wx_mfc_impl.cpp:2568 (world.GetClipboard) already
    // tests the pair; this is the same rule.
    if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT) ||
        wxTheClipboard->IsSupported(wxDF_TEXT)) {
        wxTextDataObject data;
        if (wxTheClipboard->GetData(data)) {
            // Named locals, not `std::string(data.GetText().utf8_str())`:
            // utf8_str() can hand back a NON-owning view of the wxString it was
            // called on, and this project has already shipped one clipboard bug
            // from exactly that shape.  Keep the wxString alive, then copy with
            // an explicit length so an embedded NUL cannot truncate it.
            const wxString           text = data.GetText();
            const wxScopedCharBuffer buf  = text.utf8_str();
            if (buf.data()) out.assign(buf.data(), buf.length());
        }
    }
    wxTheClipboard->Close();
    return out;
}

void SetClipboardText(const std::string& s)
{
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(s.c_str())));
        wxTheClipboard->Close();
    }
}

// RAII: put back whatever text the user had there, on EVERY way out — a return,
// an assertion that ends the arm early, a throw — not just the last line of the
// happy path.  Two of the three clipboard-using tests here used to restore only
// on the fall-through, and one of those only when there had been text to begin
// with, so an empty starting clipboard kept the copied selection afterwards.
// (A non-text clipboard, e.g. an image, cannot be preserved; clearing beats
// leaving a "SENTINEL-…" string behind.)
struct ClipboardGuard {
    std::string saved;
    bool        hadText;
    ClipboardGuard() : saved(ClipboardText()), hadText(!saved.empty()) {}
    ~ClipboardGuard()
    {
        if (hadText) {
            SetClipboardText(saved);
        } else if (wxTheClipboard->Open()) {
            wxTheClipboard->Clear();
            wxTheClipboard->Close();
        }
    }
    ClipboardGuard(const ClipboardGuard&)            = delete;
    ClipboardGuard& operator=(const ClipboardGuard&) = delete;
};

// ── Making a real-clipboard assertion hermetic ──────────────────────────────
// The system clipboard is machine-wide, so anything else running on the box can
// land between the write and the read: another build agent's own selftest, or
// the user pressing Cmd-C.  That turns a green test red for a reason that has
// nothing to do with the pane, and a spurious failure in a GUI selftest is
// exactly the kind that gets chased as a real defect.
//
// ctest's RESOURCE_LOCK cannot help: every in-app selftest here is already
// RUN_SERIAL, so no two of them overlap WITHIN a run, and a lock is scoped to
// one ctest invocation — it cannot see a second ctest in another worktree, let
// alone a human.  wx exposes no pasteboard change-count to check ownership with
// either.  What is left, and what actually matches the failure mode, is to
// repeat the whole experiment: re-plant the sentinel, drive the pane again, and
// read again.  Interference is a momentary collision, so a second run gets a
// clean window; a pane that has stopped copying answers wrong every time.
//
// That last sentence is the load-bearing one: `attempt` must re-establish the
// sentinel AND re-drive the pane, so a wrong answer can only mean the pane, and
// the assertion still fails if the copy is removed.  Retrying the READ alone
// would weaken it into "the clipboard eventually said something we like".
const int kClipboardTries = 6;

std::string ClipboardAttempt(const std::string& want,
                             const std::function<std::string()>& attempt)
{
    std::string got;
    for (int i = 0; i < kClipboardTries; ++i) {
        got = attempt();
        if (got == want) return got;
        if (i == 0)
            printf("    (clipboard said \"%s\", wanted \"%s\" -- retrying, "
                   "something else on this machine may own it)\n",
                   got.c_str(), want.c_str());
        wxMilliSleep(20 * (i + 1));
    }
    return got;   // the LAST answer, so a genuine failure prints what it saw
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

// ── 1b. WantsSelectAll: the pane's stand-in for "this view has focus" ───────
// BUG-3 (2026-08-12 GUI-drive run): Edit ▸ Select All did nothing at all with
// the output pane clicked into, because nothing ever gives the pane real wx
// keyboard focus (COutputPane::OnLeftUp always hands it back to the command
// bar so typing keeps working) — so the Edit-menu dispatcher's old
// wxWindow::FindFocus() test was permanently false.  WantsSelectAll() is the
// real signal CWorldChildFrame::EditSelectAll uses instead; this pins the
// pane's half of it: a click claims it, even one (like these, on blank space)
// that makes no text selection, and ClearSelectAllClaim gives it up again.
void TestWantsSelectAll()
{
    printf("WantsSelectAll claims/releases like a real per-view focus flag\n");

    Harness h;
    h.Feed(10);
    const int lh = h.pane->LineHeight();

    opCheck(!h.pane->WantsSelectAll(),
            "a fresh pane has not claimed Select All");

    // A plain click on blank space -- no drag, no text selection -- is
    // exactly BUG-3's repro step 2, and must still claim it.
    SendMouse(h.pane, wxEVT_LEFT_DOWN, 0, lh / 2);
    opCheck(h.pane->WantsSelectAll(),
            "a click with no selection still claims Select All");
    SendMouse(h.pane, wxEVT_LEFT_UP, 0, lh / 2);
    opCheck(h.pane->WantsSelectAll(),
            "...and releasing the button does not give it up");

    h.pane->ClearSelectAllClaim();
    opCheck(!h.pane->WantsSelectAll(),
            "ClearSelectAllClaim (a real command-bar click/keystroke) gives it up");

    // The claim survives an unrelated selection-clearing call -- it is not
    // aliased to the selection state, only to "was the pane clicked".
    SendMouse(h.pane, wxEVT_LEFT_DOWN, 0, lh / 2);
    SendMouse(h.pane, wxEVT_LEFT_UP, 0, lh / 2);
    h.pane->SetSelectionRange(0, 0, 0, 0);   // world.SetSelection(0,0,0,0): clear
    opCheck(h.pane->WantsSelectAll(),
            "clearing the text selection does not also clear the claim");
}

// ── 2. Double-click selects a word AND copies it ────────────────────────────
void TestDoubleClickAutoCopy()
{
    printf("double-click honours copy_selection_to_clipboard (mushview.cpp:4884)\n");

    const ClipboardGuard clip;

    {
        wxString selected;
        bool     paused = false;
        const std::string got = ClipboardAttempt("bravo", [&]() {
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

            selected = h.pane->GetSelectionText();
            paused   = h.pane->IsPaused();
            return ClipboardText();
        });

        opCheck(selected == wxT("bravo"),
                "double-click selects the word under the cursor");
        opCheck(got == "bravo",
                "...and copy_selection_to_clipboard puts it on the clipboard");
        // The THIRD site of the same two Windows lines (mushview.cpp:4837),
        // which runs before the word is dispatched.  Without its own assertion
        // the drag-path checks in section 1 would cover for it.
        opCheck(paused,
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
        // Option off: the clipboard must be left alone.  Retried the same way,
        // and for the same reason -- a foreign write over the sentinel would
        // otherwise read as "the pane copied something", which is the opposite
        // of what happened.
        wxString selected;
        const std::string got = ClipboardAttempt("SENTINEL-untouched", [&]() {
            Harness h;
            h.pane->AppendLine(MakeLine("alpha bravo charlie"));
            h.pane->SetCopySelectionToClipboard(false);
            SetClipboardText("SENTINEL-untouched");

            const int cw = h.pane->CharWidth();
            const int lh = h.pane->LineHeight();
            SendMouse(h.pane, wxEVT_LEFT_DOWN,   cw * 8 + cw / 2, lh / 2);
            SendMouse(h.pane, wxEVT_LEFT_DCLICK, cw * 8 + cw / 2, lh / 2);
            SendMouse(h.pane, wxEVT_LEFT_UP,     cw * 8 + cw / 2, lh / 2);

            selected = h.pane->GetSelectionText();
            return ClipboardText();
        });

        opCheck(selected == wxT("bravo"),
                "the word is still selected with the option off");
        opCheck(got == "SENTINEL-untouched",
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
        wxString seen, selected;
        const std::string got = ClipboardAttempt("SENTINEL-aborted", [&]() {
            Harness h;
            h.pane->AppendLine(MakeLine("alpha bravo charlie"));
            h.pane->SetCopySelectionToClipboard(true);
            SetClipboardText("SENTINEL-aborted");

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

            selected = h.pane->GetSelectionText();
            return ClipboardText();
        });

        opCheck(seen == wxT("bravo"),
                "the handler is given the word under the cursor");
        opCheck(got == "SENTINEL-aborted",
                "...and eDblClickAbort skips the auto-copy, as Windows does");
        // The capture/drag reset moved AHEAD of the dispatch, so it holds
        // whether or not a handler is wired: the platform's trailing LEFT_UP
        // must not re-enter the drag branch and collapse the word.
        opCheck(selected == wxT("bravo"),
                "...and the trailing mouse-up leaves the word selected");
    }

    {
        // The same wiring, but the handler accepts: the copy DOES run.  Without
        // this arm a pane that never copies once a handler exists also passes.
        const std::string got = ClipboardAttempt("bravo", [&]() {
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

            return ClipboardText();
        });

        opCheck(got == "bravo",
                "a handler that does not abort still gets the auto-copy");
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

// ── 4b. "frozen" and "at the buffer end" are two different questions ───────
// The status line needs BOTH to pick among its four values
// (CMUSHView::OnUpdateStatuslineFreeze_helper, mushview.cpp:3247): the freeze
// flag chooses between blank and PAUSE/MORE, and m_bAtBufferEnd chooses between
// those two.  The port only ever asked the first, so COutputPane::IsAtBufferEnd
// is new and this is its coverage.  Two of the four states below are the ones
// that discriminate -- (2) frozen but still AT the end, and (4) away from the
// end but NOT frozen -- and without them a pane answering both questions from
// one bit would pass.
void TestFrozenVersusAtBufferEnd()
{
    printf("frozen vs at-buffer-end: the four status-line states "
           "(mushview.cpp:3247)\n");

    // (1) running, at the end: the blank state.
    {
        Harness h;
        h.Feed(400);
        opCheck(!h.pane->IsPaused(),
                "blank state: a fed pane is still auto-scrolling");
        opCheck(h.pane->IsAtBufferEnd(),
                "blank state: ...and auto-scroll leaves it at the buffer end");
    }

    // (2) frozen, AT the end: PAUSE.  Freezing does not move the viewport, so
    // the newest line is still on screen.  One bit cannot say this.
    {
        Harness h;
        h.Feed(400);
        h.pane->SetPaused(true);
        opCheck(h.pane->IsPaused(), "PAUSE state: Freeze Output froze it");
        opCheck(h.pane->IsAtBufferEnd(),
                "PAUSE state: ...and it is STILL at the buffer end "
                "(freezing scrolls nothing)");
    }

    // (3) frozen, with output arriving underneath: MORE.  The same freeze and
    // no scrolling by the user at all -- what changed is that there is now
    // output below the window that has not been seen.
    {
        Harness h;
        h.Feed(400);
        h.pane->SetPaused(true);
        opCheck(h.pane->IsAtBufferEnd(),
                "MORE state: control -- at the buffer end beforehand");
        h.Feed(40, "more");
        opCheck(h.pane->IsPaused(),
                "MORE state: the new output did not thaw it");
        opCheck(!h.pane->IsAtBufferEnd(),
                "MORE state: ...and it is no longer at the end -- there IS more");
    }

    // (4) away from the end but NOT frozen.  Reachable only with the world's
    // auto_pause off, and the other state a single bit cannot express.
    {
        Harness h;
        h.Feed(400);
        h.pane->SetAllowAutoFreeze(false);
        h.pane->ScrollPage(-1);
        opCheck(!h.pane->IsAtBufferEnd(),
                "not-frozen: scrolling back really did leave the buffer end");
        opCheck(!h.pane->IsPaused(),
                "not-frozen: ...but with auto_pause off it is NOT frozen, so "
                "the status line stays blank");
    }

    // (5) ...and with auto_pause ON (its default) the same scroll DOES freeze,
    // which is the control that makes (4) a branch rather than a coincidence.
    // This is also the answer a script gets from GetInfo(114), on both
    // platforms: Windows' ScrollToPosition writes m_freeze here too.
    {
        Harness h;
        h.Feed(400);
        h.pane->ScrollPage(-1);
        opCheck(!h.pane->IsAtBufferEnd(), "control: the scroll moved it");
        opCheck(h.pane->IsPaused(),
                "control: ...and with auto_pause ON that scroll freezes it");
    }
}

// ── 4c. auto_pause off must not CANCEL an explicit freeze ──────────────────
// Windows, CMUSHView::ScrollToPosition (mushview.cpp:4598): the entire freeze
// assignment sits inside `if (pDoc->m_bAutoFreeze)`, so with the option off a
// scroll neither freezes nor thaws.  The port's expression ended
// `|| !m_allowAutoFreeze`, which FORCED auto-scroll back on, so in such a world
// every explicit freeze -- Display ▸ Freeze Output, world.Pause(true),
// start_paused, the scroll-to-top and find-match freezes -- was cancelled by
// the next scroll event.
void TestAutoPauseOffLeavesTheFreezeAlone()
{
    printf("auto_pause off: a scroll neither freezes nor thaws "
           "(mushview.cpp:4598)\n");

    {
        Harness h;
        h.Feed(400);
        h.pane->SetAllowAutoFreeze(false);
        h.pane->SetPaused(true);          // Display ▸ Freeze Output / world.Pause
        opCheck(h.pane->IsPaused(), "control: the explicit freeze took");
        h.pane->ScrollPage(-1);
        opCheck(h.pane->IsPaused(),
                "...scrolling UP leaves it frozen");
        for (int i = 0; i < 8; ++i) h.pane->ScrollPage(+1);
        opCheck(h.pane->IsPaused(),
                "...and so does scrolling all the way back to the bottom");
        h.pane->SetPaused(false);
        opCheck(!h.pane->IsPaused(),
                "...while an EXPLICIT unfreeze still resumes, so it is not stuck");
    }

    // Control: with auto_pause ON the scroll really is what decides, so the
    // arm above is a branch and not a pane that simply never thaws.
    {
        Harness h;
        h.Feed(400);
        h.pane->SetPaused(true);
        for (int i = 0; i < 8; ++i) h.pane->ScrollPage(+1);
        opCheck(!h.pane->IsPaused(),
                "control: with auto_pause ON, reaching the bottom thaws");
    }

    // The other half of the option, which the old expression got right and
    // which must keep working: with auto_pause off, scrolling up must not
    // freeze a pane that was running.
    {
        Harness h;
        h.Feed(400);
        h.pane->SetAllowAutoFreeze(false);
        h.pane->ScrollPage(-1);
        opCheck(!h.pane->IsPaused(),
                "with auto_pause off, scrolling up does not freeze either");
    }
}

// ── 4d. EnsureSelectionVisible: the ONE rule Find / Go To Line / Go To
//        Bookmark share (mushview.cpp:5002) ───────────────────────
// Windows has exactly one function for all three.  Go To Line
// (OnDisplayGotoline, mushview.cpp:5577) and Go To Bookmark
// (OnDisplayGotobookmark, :4920) reach it through SelectLine (:5595); Find
// calls it directly as the last thing CSendView::DoFind does (sendvw.cpp:1475).
// The port had grown three DIFFERENT behaviours instead -- Find centred the
// match and always froze, Go To Line had a visibility guard (the wrong one) and
// no clamp, Go To Bookmark had neither -- so the same jump to the same line left
// the pane in a different state depending on which menu item you used.
//
// Two pieces of arithmetic carry the whole behaviour:
//
//  (a) the visible band is shrunk by a row at EACH end --
//        startline = (pt.y - m_iPixelOffset) / m_FontHeight + 1
//        endline   = ((pt.y + GetOutputWindowHeight ()) / m_FontHeight) - 1
//      -- so a row only PARTLY on screen counts as NOT visible.  The pane's own
//      IsLineVisible is the any-pixel rule and answers the opposite, which is
//      what these tests use as the discriminator: each case is asserted against
//      the rule that would pass if the guard were merely "is it on screen".
//
//  (b) the clamp thaws.  When centring the target would scroll past the end of
//      the buffer the position is clamped instead and the `m_freeze = true` is
//      SKIPPED, after which ScrollToPosition's own `m_freeze = pt.y < bottom`
//      evaluates false.  So a jump near the end parks there with output RUNNING.
//
// Every case asserts the viewport POSITION as well as the freeze flag, so that
// "it froze but did not scroll" cannot pass for "it did nothing".

enum NavKind { kGotoLine = 0, kBookmark = 1, kFind = 2 };

const char* NavName(int k)
{
    return k == kGotoLine ? "Go To Line"
         : k == kBookmark ? "Go To Bookmark"
                          : "Find";
}

struct NavResult { long parked = 0; long pos = 0; bool frozen = false; };

// A pane whose viewport is deliberately NOT a whole number of rows, so that a
// row can be genuinely HALF on screen -- the case guard (a) exists for, and one
// an exact-fit viewport cannot express at the bottom edge at all.  The
// constructor's own assertion (below) is what stops the fixture degenerating
// silently if the default font ever changes.
struct NavFixture {
    Harness h;
    int lineH = 0, clientH = 0, page = 0, lines = 0;

    NavFixture() : h(800, 400)
    {
        h.Feed(400, "nav");
        lineH = h.pane->LineHeight();
        for (int extra = 0; extra < 64 && lineH > 0; ++extra) {
            h.pane->SetSize(0, 0, 800, 400 + extra);
            h.frame->Layout();
            h.pane->SendSizeEvent();
            h.pane->GetClientSize(nullptr, &clientH);
            if (clientH % lineH != 0) break;
        }
        page  = (lineH > 0) ? clientH / lineH : 0;
        lines = h.pane->LineCount();
    }
};

// Park the viewport at visual row `parkRow` in the given freeze state, then run
// one of the three navigations to logical line `target`.
//
// Parking goes through wxScrolled::Scroll, which fires no scroll event on
// wxOSX -- precisely why the navigation sites have to write the freeze flag
// themselves -- so it is a viewport setter with no side effect on the flag
// under test.
NavResult Navigate(NavFixture& f, int nav, int target, int parkRow, bool startFrozen)
{
    f.h.pane->SetSelectionRange(0, 0, 0, 0);   // no selection left from before
    f.h.pane->SetBookmark(target, false);      // ...and no bookmark either
    f.h.pane->ScrollToBottom();                // a known, thawed starting point
    f.h.pane->Scroll(0, parkRow);
    if (startFrozen) f.h.pane->SetPaused(true);

    NavResult r;
    r.parked = f.h.pane->GetScrollPositionY();

    if (nav == kGotoLine) {
        f.h.pane->SelectLine(target);
    } else if (nav == kBookmark) {
        f.h.pane->SetBookmark(target, true);
        f.h.pane->GotoNextBookmark();
    } else {
        char term[64];
        snprintf(term, sizeof term, "nav %04d", target);   // unique per line
        int ml = -1, ord = 0, tot = 0;
        f.h.pane->FindNext(term, true, &ml, &ord, &tot, false);
    }

    r.pos    = f.h.pane->GetScrollPositionY();
    r.frozen = f.h.pane->IsPaused();
    return r;
}

// Run one scenario through all three navigations and require the same answer
// from each -- the assertion the shared helper exists for.
void NavCase(NavFixture& f, const char* what, int target, int parkRow,
             bool startFrozen, bool wantMoved, bool wantFrozen)
{
    long firstPos = 0;
    char msg[300];
    for (int nav = kGotoLine; nav <= kFind; ++nav) {
        const NavResult r = Navigate(f, nav, target, parkRow, startFrozen);

        snprintf(msg, sizeof msg, "%s [%s]: %s", what, NavName(nav),
                 wantMoved ? "scrolls" : "does NOT scroll");
        opCheck(wantMoved ? (r.pos != r.parked) : (r.pos == r.parked), msg);

        snprintf(msg, sizeof msg, "%s [%s]: %s", what, NavName(nav),
                 wantFrozen ? "freezes" : "leaves output running");
        opCheck(r.frozen == wantFrozen, msg);

        if (nav == kGotoLine) {
            firstPos = r.pos;
        } else {
            snprintf(msg, sizeof msg,
                     "%s [%s]: lands in exactly the same place as Go To Line",
                     what, NavName(nav));
            opCheck(r.pos == firstPos, msg);
        }
    }
}

void TestEnsureSelectionVisible()
{
    printf("EnsureSelectionVisible: one rule for Find / Go To Line / "
           "Go To Bookmark (mushview.cpp:5002)\n");

    NavFixture f;
    opCheck(f.lineH > 0 && f.page > 4 && f.lines == 400,
            "fixture: a real pane, several screens deep");
    opCheck(f.clientH % f.lineH != 0,
            "fixture: the viewport is NOT a whole number of rows, so a row CAN "
            "be half on screen");
    opCheck(f.h.pane->VisualRowsForLine(0) == 1,
            "fixture: these lines do not wrap, so row == line for these cases");
    opCheck(f.h.pane->TextPixelOffset() == 0,
            "fixture: no pixel offset, so the band is scrollY/lineH +1 .. -1");

    const int K = 100;                      // the row the viewport is parked at

    // The discriminator.  Both of the rows the cases below jump to are
    // "visible" by the pane's any-pixel rule -- which is what Go To Line used to
    // be guarded by -- and NOT visible by Windows' shrunk band.
    {
        f.h.pane->ScrollToBottom();
        f.h.pane->Scroll(0, K);
        opCheck(f.h.pane->IsLineVisible(K),
                "guard (a) control: the TOP row is on screen by the any-pixel rule");
        opCheck(f.h.pane->IsLineVisible(K + f.page),
                "guard (a) control: ...and so is the half-shown row at the bottom");
    }

    // (a) a target already FULLY inside the band: no scroll, no freeze.
    NavCase(f, "(a) already fully visible", K + f.page / 2, K,
            /*startFrozen*/ false, /*moved*/ false, /*frozen*/ false);

    // ...and the early return writes NOTHING, so a pane that was already frozen
    // stays frozen rather than being thawed by a jump that moved nothing.
    NavCase(f, "(a) already visible, pane frozen", K + f.page / 2, K,
            /*startFrozen*/ true, /*moved*/ false, /*frozen*/ true);

    // ...and the LAST fully visible row is the far edge of the band.  Without a
    // case ON that edge, an endline shrunk by two rows instead of one passes
    // everything above: the targets are all comfortably inside.
    NavCase(f, "(a) the last fully visible row", K + f.page - 1, K,
            false, /*moved*/ false, /*frozen*/ false);

    // (b) PARTLY visible counts as hidden: the row at the bottom of the
    // viewport with only a few pixels of it showing.  This is the arithmetic
    // nobody would get right by accident, and the control above says "visible".
    NavCase(f, "(b) half-visible bottom row", K + f.page, K,
            false, /*moved*/ true, /*frozen*/ true);

    // (c) plainly off screen -- the parity control.  Without it a helper that
    // never freezes would pass (a), and (b) alone would not catch it either.
    NavCase(f, "(c) off screen", K + 200, K, false, /*moved*/ true, /*frozen*/ true);

    // (d) the clamp thaws: centring the LAST line would scroll past the end of
    // the buffer, so the position is clamped and the freeze SKIPPED.
    NavCase(f, "(d) clamped to the buffer end", f.lines - 1, 0, false,
            /*moved*/ true, /*frozen*/ false);

    // ...and it really did CLAMP rather than centre.  A pure centring lands
    // further down the buffer than the clamp allows, so this separates "clamped"
    // from "scrolled somewhere and happened not to freeze".
    {
        const NavResult r = Navigate(f, kGotoLine, f.lines - 1, 0, false);
        const long centre = (long) (f.lines - 1) * f.lineH - f.clientH / 2;
        opCheck(r.pos > 0 && r.pos < centre,
                "(d) the position is the buffer END, not the centring target");
        opCheck(f.h.pane->IsLineVisible(f.lines - 1),
                "(d) ...and the target line is on screen");
    }

    // ...and the clamp's THAW, in the form where it is a transition rather than
    // a flag that was already false: an explicitly frozen pane, jumped to the
    // end of the buffer, comes back RUNNING.  This is the case that pins the
    // `if (m_allowAutoFreeze)` gate the right way round -- skip the tail here
    // and the pane would stay frozen.
    NavCase(f, "(d) clamped, pane already frozen", f.lines - 1, 0,
            /*startFrozen*/ true, /*moved*/ true, /*frozen*/ false);

    // ...unless keep_pause_at_bottom is on, which turns ScrollToPosition's
    // assignment into a LATCH (`m_freeze = m_freeze || …`, mushview.cpp:4680).
    // Same jump, same clamp, opposite answer -- and the pair is what stops the
    // latch being mistaken for a plain assignment.
    {
        NavFixture k;
        k.h.pane->SetKeepPauseAtBottom(true);
        NavCase(k, "(d) clamped, keep_pause_at_bottom ON", k.lines - 1, 0,
                /*startFrozen*/ true, /*moved*/ true, /*frozen*/ true);
    }

    // (e) the top row: the one place Windows itself does NOT make the three
    // agree, and reproducing that is the whole point of routing them through one
    // function rather than making them "consistent".  CMUSHView::SelectLine sets
    // m_selend_line = iLine + 1, so Go To Line's selection SPANS
    // [iLine, iLine + 1] while a Find match sits inside a single line.
    // startline is the first FULLY visible row, so at the top edge a Find match
    // ON that row is out of band and scrolls, while Go To Line's span reaches
    // one row further down, stays in band, and does not.
    {
        const NavResult g = Navigate(f, kGotoLine, K, K, false);
        opCheck(g.pos == g.parked && !g.frozen,
                "(e) top row: Go To Line's [iLine, iLine+1] span is still in "
                "band, so it does not scroll");
        const NavResult b = Navigate(f, kBookmark, K, K, false);
        opCheck(b.pos == g.pos && b.frozen == g.frozen,
                "(e) top row: ...Go To Bookmark is the same SelectLine, so it "
                "agrees with Go To Line exactly");
        const NavResult n = Navigate(f, kFind, K, K, false);
        opCheck(n.pos != n.parked && n.frozen,
                "(e) top row: ...while a Find match on that row IS out of band, "
                "so it scrolls and freezes");
    }

    // (g) the boundary that is easy to lose once both rules live in one
    // function: EnsureSelectionVisible's `m_freeze = true` is NOT gated on
    // auto_pause, while ScrollToPosition's tail is entirely inside
    // `if (pDoc->m_bAutoFreeze)`.
    {
        NavFixture g;
        g.h.pane->SetAllowAutoFreeze(false);
        NavCase(g, "(g) auto_pause OFF, off-screen target", K + 200, K,
                false, /*moved*/ true, /*frozen*/ true);
        // ...and the clamp still declines to freeze, from a thawed start: with
        // the tail skipped the flag simply keeps the value it came in with.
        NavCase(g, "(g) auto_pause OFF, clamped to the end", g.lines - 1, 0,
                false, /*moved*/ true, /*frozen*/ false);
        // ...which cuts the other way too, and this is the half that pins the
        // gate: the SAME clamp that thaws a frozen pane in a world with
        // auto_pause ON must leave it frozen here, because the assignment that
        // would have thawed it never runs.
        NavCase(g, "(g) auto_pause OFF, clamped, pane frozen", g.lines - 1, 0,
                /*startFrozen*/ true, /*moved*/ true, /*frozen*/ true);
    }

    // The row axis is NOT the line axis.  Every case above uses short lines,
    // where visual row == logical line and the conversion inside
    // EnsureSelectionVisible is an identity -- exactly the degenerate fixture
    // that lets a line-indexed implementation pass.  Here every line wraps, so a
    // jump computed in LINES lands hundreds of rows above the target.
    {
        Harness w(400, 400);
        for (int i = 0; i < 200; ++i) {
            char head[32];
            snprintf(head, sizeof head, "wrap %04d ", i);
            std::string s(head);
            s.append(300, 'x');
            w.pane->AppendLine(MakeLine(s.c_str()));
        }
        opCheck(w.pane->VisualRowsForLine(5) > 1,
                "wrapped fixture: a line really does span several rows");
        const int target = w.pane->LineCount() - 20;
        w.pane->ScrollToBottom();
        w.pane->Scroll(0, 0);
        w.pane->SelectLine(target);
        opCheck(w.pane->IsLineVisible(target),
                "wrapped: Go To Line puts the target on screen (a scroll "
                "computed in lines would stop far above it)");
        opCheck(w.pane->IsPaused(),
                "wrapped: ...and freezes, the target having been off screen");
    }
}

// ── 4e. A plain scroll obeys auto_pause; the selection jumps do not ──────
// Windows reaches every plain scroll through ScrollToPosition, whose ENTIRE
// freeze assignment sits inside `if (pDoc->m_bAutoFreeze)` (mushview.cpp:4600).
// ScrollToTop (Ctrl+Home, and world.SetScroll(0)) and ScrollToPixelY
// (world.SetScroll's pixel form) each wrote a flat `m_autoScroll = false`
// instead, so in a world with auto_pause OFF they froze the output that option
// says must never stop -- and a script asking to scroll past the end froze the
// world at the very bottom, where Windows leaves it running.
void TestPlainScrollsObeyAutoPause()
{
    printf("Ctrl+Home / world.SetScroll take ScrollToPosition's gate "
           "(mushview.cpp:4600)\n");

    {
        Harness h; h.Feed(400);
        h.pane->ScrollToTop();
        opCheckEq((int) h.pane->GetScrollPositionY(), 0,
                  "scroll-to-top: the view really is at the top");
        opCheck(h.pane->IsPaused(),
                "scroll-to-top: ...and with auto_pause ON it freezes");
    }
    {
        Harness h; h.Feed(400);
        h.pane->SetAllowAutoFreeze(false);
        h.pane->ScrollToTop();
        opCheckEq((int) h.pane->GetScrollPositionY(), 0,
                  "scroll-to-top: the view still PARKS with auto_pause off");
        opCheck(!h.pane->IsPaused(),
                "scroll-to-top: ...but does NOT freeze (it used to stop the "
                "output of the world whose option says it never should)");
    }
    {
        Harness h; h.Feed(400);
        const int lh = h.pane->LineHeight();
        h.pane->ScrollToPixelY(50L * lh);
        opCheck(h.pane->GetScrollPositionY() == 50L * lh,
                "world.SetScroll: parks at the requested pixel");
        opCheck(h.pane->IsPaused(),
                "world.SetScroll: ...and freezes with auto_pause ON");
    }
    {
        Harness h; h.Feed(400);
        h.pane->SetAllowAutoFreeze(false);
        const int lh = h.pane->LineHeight();
        h.pane->ScrollToPixelY(50L * lh);
        opCheck(h.pane->GetScrollPositionY() == 50L * lh,
                "world.SetScroll: still parks with auto_pause off");
        opCheck(!h.pane->IsPaused(),
                "world.SetScroll: ...and does NOT freeze");
    }
    {
        // Past the end: Windows' `m_freeze = pt.y < bottom` is FALSE there, so
        // the view parks at the buffer end with output running.  Starting from
        // an explicitly frozen, scrolled-back pane makes this a real transition
        // rather than a flag that merely never changed.
        Harness h; h.Feed(400);
        h.pane->ScrollToTop();
        opCheck(h.pane->IsPaused(), "control: parked at the top and frozen");
        h.pane->ScrollToPixelY(100000L);
        opCheck(h.pane->IsAtBufferEnd(),
                "world.SetScroll past the end clamps to the buffer end");
        opCheck(!h.pane->IsPaused(),
                "...and THAWS, because the clamped position is not < bottom");
    }
    {
        // (h) None of this touches the explicit freeze.
        Harness h; h.Feed(400);
        h.pane->SetPaused(true);
        opCheck(h.pane->IsPaused(), "Display > Freeze Output still freezes");
        opCheck(h.pane->IsAtBufferEnd(),
                "...without moving the viewport (PAUSE, not MORE)");
        h.Feed(40, "more");
        opCheck(h.pane->IsPaused() && !h.pane->IsAtBufferEnd(),
                "...and output arriving underneath makes it MORE, still frozen");
        h.pane->SetPaused(false);
        opCheck(!h.pane->IsPaused(), "...while an explicit unfreeze resumes");
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

// ── 7b. Find Next visits every OCCURRENCE, not every matching line ────────
//
// Windows' Find sets m_bRepeatOnSameLine (sendvw.cpp:1399, comment "we can find
// multiple instances on the same line") and FindRoutine builds the line's whole
// match list, popping one entry per press and moving to another line only when
// it is empty (Finding.cpp:123-130, :189-219).  The port's cache is per LINE and
// it resumed from selEnd.line + 1, always selecting the FIRST occurrence on
// whatever line it landed on -- so the second "gold" in "Gold coins and more
// gold dust" could not be reached at all, and if that line were the only match
// the same first occurrence came back for ever.
//
// The fixture puts the two occurrences in DIFFERENT case and searches
// case-insensitively, so the selected TEXT says which one was found: a test
// that only asserted the line number could not tell "stayed put" from "advanced
// within the line".
void TestFindVisitsEveryOccurrenceOnALine()
{
    printf("Find Next: every occurrence, including several on one line "
           "(sendvw.cpp:1399)\n");

    Harness h;
    h.pane->AppendLine(MakeLine("gold nugget"));                   // line 0: one
    h.pane->AppendLine(MakeLine("Gold coins and more gold dust")); // line 1: two
    h.pane->AppendLine(MakeLine("silver only"));                   // line 2: none

    int line = -1, ord = 0, total = 0;
    auto press = [&](bool forward) {
        h.pane->FindNext("gold", forward, &line, &ord, &total, false);
        // NOTE the named local: utf8_str() can hand back a NON-owning view into
        // the wxString's own buffer, so taking it from the temporary would leave
        // it dangling at the semicolon -- and read back as "".
        const wxString sel = h.pane->GetSelectionText();
        const wxScopedCharBuffer b = sel.utf8_str();
        return std::string(b.data() ? b.data() : "");
    };
    auto step = [&](bool forward, int wantLine, const char* wantText,
                    const char* what) {
        const std::string got = press(forward);
        const bool ok = (line == wantLine && got == wantText);
        printf("  [%s] %s (line %d %s, got \"%s\" wanted \"%s\")\n",
               ok ? "PASS" : "FAIL", what, line,
               line == wantLine ? "as wanted" : "WRONG", got.c_str(), wantText);
        (ok ? g_pass : g_fail)++;
    };

    // Forward, from the top.  Note steps 2 and 3: the SAME line twice, with a
    // different occurrence each time.
    step(true, 0, "gold", "1st: the only match on line 0");
    step(true, 1, "Gold", "2nd: the first match on line 1");
    step(true, 1, "gold", "3rd: the SECOND match on the SAME line "
                          "(this is the whole finding)");
    step(true, 0, "gold", "4th: only now does it leave the line, wrapping");

    // Backward is the mirror image, and it needs its own arm: the direction
    // decides both which occurrence a landing selects (the LAST, so the next
    // press has somewhere to go) and which end of the selection the in-line
    // scan measures from.  Start below line 1 so the first press walks back
    // onto it.
    // 1-based, and it must not be EMPTY: GetNormalizedSel refuses a collapsed
    // region, which would leave Find with no start point at all.
    h.pane->SetSelectionRange(3, 3, 1, 2);     // line 2, its first character
    step(false, 1, "gold", "back 1st: the LAST match on line 1");
    step(false, 1, "Gold", "back 2nd: the earlier match on the SAME line");
    step(false, 0, "gold", "back 3rd: and only then the previous line");

    // The control that keeps the case-insensitive arms honest: searching
    // case-SENSITIVELY for "gold" leaves line 1 with a single occurrence, so
    // the second press must move OFF the line.  A same-line scan that ignored
    // the case flag would find "Gold" here and stay.
    h.pane->SetSelectionRange(0, 0, 0, 0);
    h.pane->FindNext("gold", true, &line, &ord, &total, /*caseSensitive*/ true);
    opCheckEq(total, 2, "control: case-sensitive \"gold\" matches two lines");
    while (line != 1)                          // walk to the two-occurrence line
        h.pane->FindNext("gold", true, &line, &ord, &total, true);
    opCheck(h.pane->GetSelectionText() == wxT("gold"),
            "control: on line 1 it selects the lower-case occurrence");
    h.pane->FindNext("gold", true, &line, &ord, &total, true);
    opCheckEq(line, 0, "control: with one match on the line it moves on");
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

    // The "on" arm below really does write the clipboard, so guard it for the
    // whole function: the old code saved it late and restored it only when
    // there had been text there to begin with, which left a whole scrollback
    // on an empty clipboard.
    const ClipboardGuard clip;

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
    // vacuous.  (It also touches the clipboard; ClipboardGuard has it.)
    h.pane->SetCopySelectionToClipboard(true);
    releases(3);
    const double on = releases(30);
    h.pane->SetCopySelectionToClipboard(false);

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

// -- Counting characters must not cost a decode, nor a converter -------------
// The non-UTF-8 branch stopped being exotic the day worlds stopped being
// silently flipped to UTF-8: it is now what an ordinary ANSI world takes for
// every run of every appended line, every painted line, every line a mouse move
// walks, and every line of the whole scrollback on every Find keypress.  It
// used to build -- and destroy -- a wxCSConv per run (an iconv_open apiece) and
// force a full byte->wxString conversion merely to COUNT characters.
//
// This counts the work rather than timing it.  A timing assertion on a shared
// machine is a coin toss, and the project has already been bitten once by a
// shim that was functionally correct, quadratic, and defended by a comment
// nobody could falsify (the CTypedPtrList POSITION scan, 595ms per drain at
// 50k lines).  So: exact before/after deltas on two counters, plus the
// equality the whole optimisation rests on, checked over every byte value of
// every charset the mapper knows.
void TestCharCountAvoidsTheDecode()
{
    printf("legacy-charset counting: no converter per call, no decode to count\n");

    // Every LOGFONT charset WinCharsetToEncoding has a row for, plus two it
    // does not (1 = DEFAULT_CHARSET, 2 = SYMBOL_CHARSET) which fall to CP1252.
    const int kSingleByte[] = { 0, 1, 2, 161, 162, 163, 177, 178,
                                186, 204, 222, 238, 255 };
    const int kDbcs[]       = { 128, 129, 134, 136 };


    auto makeRun = [](const std::string& bytes, int charset) {
        SOutputRun r;
        r.text    = bytes;
        r.utf8    = 0;
        r.charset = (short) charset;
        return r;
    };
    auto decodedLen = [](const SOutputRun& r) {
        return (int) DecodeOutputBytes(r.text.c_str(), r.text.size(),
                                       r.utf8 != 0, r.charset).size();
    };

    // -- 1. the equality the fast path is only allowed to exist because of --
    // Every single byte, on its own and in a pair, under every charset.  If any
    // single-byte codepage ever DROPS an unmapped byte instead of failing the
    // whole conversion (0x81/0x8D/0x8F/0x90/0x9D are undefined in CP1252), the
    // byte count and the drawn count part company and this fails -- which is the
    // only reason it is safe to skip the decode at all.
    {
        int mismatches = 0;
        std::string firstBad;
        for (int cs : kSingleByte) {
            for (int b = 0; b <= 0xFF; ++b) {
                const std::string one(1, (char)(unsigned char) b);
                const std::string two = one + std::string(1, (char) 0x41);
                for (const std::string& bytes : { one, two }) {
                    SOutputRun r = makeRun(bytes, cs);
                    if (RunCharLen(r) != decodedLen(r)) {
                        if (mismatches == 0) {
                            char buf[96];
                            snprintf(buf, sizeof buf,
                                     "charset %d byte 0x%02X: counted %d, drawn %d",
                                     cs, b, RunCharLen(r), decodedLen(r));
                            firstBad = buf;
                        }
                        ++mismatches;
                    }
                }
            }
        }
        if (mismatches)
            printf("    %d mismatches, first: %s\n", mismatches, firstBad.c_str());
        opCheck(mismatches == 0,
                "single-byte charsets: the byte count IS the drawn count, "
                "for all 256 bytes of all 13");
    }
    // ...and the DBCS charsets, where the count still goes through the decode:
    // the memo must not change the answer, including for a truncated lead byte
    // (which fails the conversion and lands on the Latin-1 fallback).
    {
        const std::string kCases[] = {
            "", "plain", "\xA4\xA4", "\xA4\xA4\xA4\xE5", "\xA4",
            "a\xA4\xA4" "b", std::string("\x00\xA4\xA4", 3),
        };
        int mismatches = 0;
        for (int cs : kDbcs)
            for (const std::string& bytes : kCases) {
                SOutputRun r = makeRun(bytes, cs);
                if (RunCharLen(r) != decodedLen(r)) ++mismatches;
                if (RunCharLen(r) != decodedLen(r)) ++mismatches;   // twice: memo hit
            }
        opCheck(mismatches == 0,
                "DBCS charsets: the memoised count equals the drawn count, "
                "on a repeat as well as the first ask");
    }

    // -- 2. one converter per charset, not one per call ----------------------
    {
        const unsigned long c0 = g_outputConvBuilds.load();
        const unsigned long d0 = g_outputDecodes.load();
        for (int i = 0; i < 500; ++i)
            (void) DecodeOutputBytes("a\x93" "quoted\x94 word", 14, false, 0);
        const unsigned long convs   = g_outputConvBuilds.load() - c0;
        const unsigned long decodes = g_outputDecodes.load() - d0;
        // The decode count is the control: without it, a DecodeOutputBytes that
        // stopped decoding entirely would also build no converters and pass.
        opCheckEq((int) decodes, 500, "500 decodes really ran");
        opCheck(convs <= 1,
                "...through at most ONE wxCSConv, not 500 (was one per call)");
        if (convs > 1) printf("    built %lu converters\n", convs);
    }

    // -- 3. counting a single-byte run does no work at all -------------------
    {
        SOutputRun r = makeRun("a\x93" "quoted\x94 word", 0);
        const unsigned long c0 = g_outputConvBuilds.load();
        const unsigned long d0 = g_outputDecodes.load();
        long total = 0;
        for (int i = 0; i < 500; ++i) total += RunCharLen(r);
        opCheckEq((int)(total / 500), 14, "the count is right (14 characters)");
        opCheckEq((int)(g_outputDecodes.load() - d0), 0,
                  "500 RunCharLen calls on a CP1252 run decode NOTHING");
        opCheckEq((int)(g_outputConvBuilds.load() - c0), 0,
                  "...and build no converter either");
    }

    // -- 4. a DBCS run is decoded once, then remembered ----------------------
    {
        SOutputRun r = makeRun("\xA4\xA4\xA4\xE5" "abc", 136);   // Big5
        (void) RunCharLen(r);                       // prime (and build the conv)
        const unsigned long c0 = g_outputConvBuilds.load();
        const unsigned long d0 = g_outputDecodes.load();
        long total = 0;
        for (int i = 0; i < 200; ++i) total += RunCharLen(r);
        opCheckEq((int)(g_outputDecodes.load() - d0), 0,
                  "200 more RunCharLen calls on the SAME Big5 run re-decode nothing");
        opCheckEq((int)(g_outputConvBuilds.load() - c0), 0,
                  "...and build no further converter");
        opCheckEq((int)(total / 200), decodedLen(r),
                  "...and the memo answers what the decode would have");
    }

    // -- 5. the memo is keyed on the CONTENT and on the CHARSET --------------
    // Two runs, alternating: each must be decoded once and then remembered, so
    // a one-entry cache (which would thrash) and a cache that ignored the bytes
    // (which would answer the other run's length) both fail here.
    {
        SOutputRun a = makeRun("\xA4\xA4", 136);            // Big5: one char
        SOutputRun b = makeRun("\xA4\xA4\xA4\xE5", 136);    // Big5: two chars
        opCheckEq(RunCharLen(a), 1, "Big5 A4A4 is one rendered character");
        opCheckEq(RunCharLen(b), 2, "Big5 A4A4A4E5 is two");
        const unsigned long d0 = g_outputDecodes.load();
        for (int i = 0; i < 100; ++i) { (void) RunCharLen(a); (void) RunCharLen(b); }
        opCheckEq((int)(g_outputDecodes.load() - d0), 0,
                  "alternating two known runs re-decodes neither");
    }
    {
        // The SAME bytes under a different DBCS charset are a different length:
        // 0xA4 is a lead byte in Big5 but a halfwidth-katakana character of its
        // own in Shift-JIS.  A memo keyed only on the bytes returns the stale
        // answer here -- which is exactly the runtime charset change the cache
        // has to survive.
        SOutputRun big5 = makeRun("\xA4\xA4", 136);
        SOutputRun sjis = makeRun("\xA4\xA4", 128);
        const int nBig5 = RunCharLen(big5);
        const int nSjis = RunCharLen(sjis);
        opCheckEq(nBig5, decodedLen(big5), "A4A4 as Big5 counts as Big5 draws it");
        opCheckEq(nSjis, decodedLen(sjis), "A4A4 as Shift-JIS counts as Shift-JIS draws it");
        opCheck(nBig5 != nSjis,
                "...and the two really do differ, so the charset key is load-bearing");
        // Re-ask in the other order: neither answer may have been overwritten.
        opCheckEq(RunCharLen(sjis), nSjis, "the Shift-JIS answer survives the Big5 ask");
        opCheckEq(RunCharLen(big5), nBig5, "...and vice versa");
    }

    // -- 6. more distinct runs than the memo holds: still all correct --------
    // The bound is a memory bound, never a correctness one.
    {
        std::vector<SOutputRun> runs;
        std::vector<int>        want;
        for (int i = 0; i < 40; ++i) {
            std::string bytes = "\xA4\xA4";
            bytes += std::string((size_t) i % 7 + 1, 'x');
            runs.push_back(makeRun(bytes, 136));
            want.push_back(decodedLen(runs.back()));
        }
        int wrong = 0;
        for (int pass = 0; pass < 3; ++pass)
            for (size_t i = 0; i < runs.size(); ++i)
                if (RunCharLen(runs[i]) != want[i]) ++wrong;
        opCheckEq(wrong, 0, "40 distinct runs cycled past a 16-entry memo stay correct");
    }

    // -- 7. a world that changes charset at runtime -------------------------
    // Nothing invalidates the converter cache, because nothing has to: the key
    // IS the encoding.  Flipping between four charsets many times must build at
    // most one converter each (and none at all for the ones already used).
    {
        const unsigned long c0 = g_outputConvBuilds.load();
        int wrong = 0;
        for (int i = 0; i < 100; ++i)
            for (int cs : { 0, 204, 238, 136 }) {
                SOutputRun r = makeRun("\xA4\xA4" "text", cs);
                if (RunCharLen(r) != decodedLen(r)) ++wrong;
            }
        opCheckEq(wrong, 0, "flipping charset 400 times never miscounts");
        opCheck(g_outputConvBuilds.load() - c0 <= 4,
                "...and builds at most one converter per charset, ever");
    }
}

// ── the plugin text rectangle, adopted ───────────────────────────────────────
// world.TextRectangle is POLLED, not signalled: nothing tells the pane a plugin
// narrowed the text band, so this comparison in OnPaint is the only thing that
// ever notices.  Like the metric correction above it had never executed under
// test, because no in-app selftest reaches the paint handler at all.  If it
// stops firing, the text keeps wrapping at the OLD width and draws straight
// underneath the miniwindow the plugin reserved the space for.
void TestAdoptTextRect()
{
    printf("world.TextRectangle change detection (COutputPane::AdoptTextRect)\n");

    Harness h(800, 400);
    int clientW = 0, clientH = 0;
    h.pane->GetClientSize(&clientW, &clientH);

    int l = -1, t = -1, r = -1, b = -1;

    // First look: the pane starts with a zeroed rectangle, so even "no rectangle
    // at all" is a change -- the full client area has to be adopted once.
    opCheck(h.pane->AdoptTextRect(), "textrect: the first look is always a change");
    opCheck(!h.pane->AdoptedTextRect(l, t, r, b), "...with no rectangle active");
    opCheckEq(r, clientW, "...spanning the full client width");
    opCheckEq(b, clientH, "...and the full client height");

    // Nothing moved -> no reflow.  Without this the pane would rebuild its wrap
    // cache on every single paint, which is what the comparison exists to stop.
    opCheck(!h.pane->AdoptTextRect(),
            "textrect: an unchanged rectangle asks for no reflow");
    opCheck(!h.pane->AdoptTextRect(), "...still, on a third look");

    // A plugin reserves the right-hand strip.
    int rl = 0, rt = 0, rr = 300, rb = 200;
    h.pane->m_fn_text_rect = [&](int& ll, int& tt, int& rrr, int& bb) -> bool {
        ll = rl; tt = rt; rrr = rr; bb = rb;
        return true;
    };
    opCheck(h.pane->AdoptTextRect(), "textrect: a newly set rectangle IS a change");
    opCheck(h.pane->AdoptedTextRect(l, t, r, b), "...and it is active");
    opCheckEq(r, 300, "...the stored right edge is the one just set");
    opCheckEq(b, 200, "...and the stored bottom edge too");

    // THE point of storing it: the next look compares against these values.
    opCheck(!h.pane->AdoptTextRect(),
            "textrect: the stored rectangle is what the next look compares against");

    // Each edge on its own must count.  An implementation that compared only
    // (say) the width would miss a band that MOVED without resizing -- which is
    // exactly what a plugin docking its miniwindow to the other side produces.
    rr = 301;
    opCheck(h.pane->AdoptTextRect(), "textrect: a right-edge-only change counts");
    rb = 201;
    opCheck(h.pane->AdoptTextRect(), "textrect: a bottom-edge-only change counts");
    rl = 20;
    opCheck(h.pane->AdoptTextRect(), "textrect: a left-edge-only change counts");
    rt = 10;
    opCheck(h.pane->AdoptTextRect(), "textrect: a top-edge-only change counts");
    opCheck(!h.pane->AdoptTextRect(), "...and then it settles again");

    // Switching the rectangle back off is a change too, and clears the active
    // flag -- the text has to be given the full width back.
    h.pane->m_fn_text_rect = nullptr;
    opCheck(h.pane->AdoptTextRect(), "textrect: removing the rectangle is a change");
    opCheck(!h.pane->AdoptedTextRect(l, t, r, b), "...and it is no longer active");
    opCheckEq(l, 0, "...the origin goes back to the window's top-left");
    opCheckEq(t, 0, "...on both axes");

    // The transition the four edges CANNOT see: a plugin whose rectangle covers
    // exactly the client area.  Its resolved geometry is identical to having no
    // rectangle at all, so setting it -- and later dropping it -- moves nothing
    // but the active flag.  A comparison that only looked at the edges reports
    // "unchanged", never stores, and leaves the pane's idea of whether a
    // rectangle is set permanently stuck at whichever value it had first.
    rl = 0; rt = 0; rr = clientW; rb = clientH;
    h.pane->m_fn_text_rect = [&](int& ll, int& tt, int& rrr, int& bb) -> bool {
        ll = rl; tt = rt; rrr = rr; bb = rb;
        return true;
    };
    opCheck(h.pane->AdoptTextRect(),
            "textrect: a rectangle covering the whole client area is still a change");
    opCheck(h.pane->AdoptedTextRect(l, t, r, b),
            "...because the pane now HAS one, though no edge moved");
    opCheckEq(r, clientW, "...spanning the client width exactly");

    h.pane->m_fn_text_rect = nullptr;
    opCheck(h.pane->AdoptTextRect(),
            "textrect: dropping it again is a change, on the flag alone");
    opCheck(!h.pane->AdoptedTextRect(l, t, r, b),
            "...and the pane stops believing a rectangle is set");
}

// ── the wheel's ordering rule ────────────────────────────────────────────────
// A wheel event cannot be delivered here (no event loop), so the rule is tested
// as the pure decision it is.  The case that matters is the last one: Ctrl+wheel
// OVER A HOTSPOT.  Windows gives that to the plugin with MW_MOUSE_CTRL set, and
// the port once tested the modifier first -- which silently made every
// Ctrl+wheel plugin callback unreachable while the pointer was over a
// miniwindow.
void TestWheelActionOrdering()
{
    printf("wheel: hotspot before modifier (mushview.cpp:5160)\n");

    using WA = COutputPane::WheelAction;

    opCheck(COutputPane::DecideWheelAction(false, false) == WA::Scroll,
            "wheel: plain wheel scrolls");
    opCheck(COutputPane::DecideWheelAction(false, true) == WA::Zoom,
            "wheel: Ctrl+wheel zooms");
    opCheck(COutputPane::DecideWheelAction(true, false) == WA::HotspotTookIt,
            "wheel: a hotspot that consumed the wheel keeps it");
    opCheck(COutputPane::DecideWheelAction(true, true) == WA::HotspotTookIt,
            "wheel: ...and still keeps it with Ctrl held, rather than zooming");
}

// ── the zoom clamp ───────────────────────────────────────────────────────────
// AdjustFontSize is reachable from four places (Ctrl+wheel, Ctrl+/Ctrl-, the
// context menu, and the saved per-world size on world open) and appeared in no
// test at all.  The clamp is what stops a saved or scripted size from producing
// an unreadable or un-renderable font.
void TestFontSizeClamp()
{
    printf("zoom clamp (COutputPane::AdjustFontSize)\n");

    Harness h(800, 400);
    opCheckEq(h.pane->FontSize(), COutputPane::kDefaultFontSize,
              "zoom: a fresh pane is at the default size");

    // A no-op delta changes nothing and reports the size unchanged.
    const int cw0 = h.pane->CharWidth(), lh0 = h.pane->LineHeight();
    opCheckEq(h.pane->AdjustFontSize(0), COutputPane::kDefaultFontSize,
              "zoom: a zero delta returns the current size");
    opCheck(h.pane->CharWidth() == cw0 && h.pane->LineHeight() == lh0,
            "...and leaves the grid alone");

    // An ordinary step really moves.
    opCheckEq(h.pane->AdjustFontSize(1), COutputPane::kDefaultFontSize + 1,
              "zoom: one step up is one point");
    opCheckEq(h.pane->AdjustFontSize(-1), COutputPane::kDefaultFontSize,
              "zoom: and one step down comes back");

    // The top end.  A huge delta must land ON the cap, not past it.
    opCheckEq(h.pane->AdjustFontSize(1000), COutputPane::kMaxFontSize,
              "zoom: an over-large step clamps to the maximum");
    opCheckEq(h.pane->AdjustFontSize(1), COutputPane::kMaxFontSize,
              "zoom: ...and stepping up again stays there");
    const int cwMax = h.pane->CharWidth();

    // The bottom end.
    opCheckEq(h.pane->AdjustFontSize(-1000), COutputPane::kMinFontSize,
              "zoom: an over-large step down clamps to the minimum");
    opCheckEq(h.pane->AdjustFontSize(-1), COutputPane::kMinFontSize,
              "zoom: ...and stepping down again stays there");
    const int cwMin = h.pane->CharWidth();

    // The size is not just a number being reported back: the FONT has to have
    // followed it, or the clamp is guarding nothing.  A monospaced 6pt cell is
    // narrower than a 32pt one on any font the pane will accept.
    opCheck(cwMin < cwMax,
            "zoom: the character cell really shrank between the two ends");

    opCheckEq((h.pane->ResetFontSize(), h.pane->FontSize()),
              COutputPane::kDefaultFontSize,
              "zoom: reset goes back to the default from the minimum");
}

// ── one answer to "which run is at column N" ─────────────────────────────────
// Five walks in this file derived it by hand, plus the tooltip in
// wx_world_child.cpp.  They all decide which MXP link a click lands on, so a
// boundary that is off by one sends the wrong command to the MUD -- and an
// implementation that clamped to the last run instead of answering "past the
// end" would fire a link from a click in the blank space after the text.
void TestRunAtCharColumn()
{
    printf("RunAtCharColumn / LineCharLen\n");

    auto run = [](const char* text) {
        SOutputRun r;
        r.text = text;
        r.fg   = 0x00FFFFFFu;
        return r;
    };

    SOutputLine line;
    line.push_back(run("abc"));      // [0] cols 0..2
    line.push_back(run(""));         // [1] zero length -- covers NO column
    line.push_back(run("de"));       // [2] cols 3..4
    SOutputRun hr;                   // [3] a horizontal rule: also zero length
    hr.horiz_rule = 1;
    line.push_back(hr);
    line.push_back(run("fghi"));     // [4] cols 5..8

    opCheckEq(LineCharLen(line), 9, "line: nine rendered columns across five runs");

    int within = -1;
    const SOutputRun* r = nullptr;

    r = COutputPane::RunAtCharColumn(line, 0, &within);
    opCheck(r == &line[0], "col 0 is in the first run");
    opCheckEq(within, 0, "...at offset 0 within it");

    r = COutputPane::RunAtCharColumn(line, 2, &within);
    opCheck(r == &line[0], "col 2 is still in the first run");
    opCheckEq(within, 2, "...at its last offset");

    // THE boundary.  col 3 is the first column of "de", and it must not be
    // answered by the run that ends at 3 -- nor by either zero-length run,
    // which sit exactly here and cover nothing.
    r = COutputPane::RunAtCharColumn(line, 3, &within);
    opCheck(r == &line[2], "col 3 is the first column of the NEXT run, not the last of the previous");
    opCheckEq(within, 0, "...at offset 0 within it");

    r = COutputPane::RunAtCharColumn(line, 4, &within);
    opCheck(r == &line[2], "col 4 is the second column of that run");
    opCheckEq(within, 1, "...at offset 1");

    // The second boundary, this time with a horiz_rule run sitting on it.
    r = COutputPane::RunAtCharColumn(line, 5, &within);
    opCheck(r == &line[4], "col 5 skips the rule run and lands in the last run");
    opCheckEq(within, 0, "...at offset 0");

    r = COutputPane::RunAtCharColumn(line, 8, &within);
    opCheck(r == &line[4], "col 8 is the last column of the line");
    opCheckEq(within, 3, "...at offset 3");

    // Past the end must be nullptr, NOT the last run.
    opCheck(COutputPane::RunAtCharColumn(line, 9) == nullptr,
            "col 9 is one past the end: no run");
    opCheck(COutputPane::RunAtCharColumn(line, 1000) == nullptr,
            "a column far past the end: no run");
    opCheck(COutputPane::RunAtCharColumn(line, -1) == nullptr,
            "a negative column: no run");

    // An empty line answers nothing at all, at any column.
    SOutputLine empty;
    opCheckEq(LineCharLen(empty), 0, "an empty line is zero columns long");
    opCheck(COutputPane::RunAtCharColumn(empty, 0) == nullptr,
            "an empty line has no run at column 0");

    // A line that is NOTHING but zero-length runs likewise.
    SOutputLine blanks;
    blanks.push_back(run(""));
    blanks.push_back(hr);
    opCheckEq(LineCharLen(blanks), 0, "zero-length runs contribute no columns");
    opCheck(COutputPane::RunAtCharColumn(blanks, 0) == nullptr,
            "...so no column can land in one");

    // The column axis is RENDERED CHARACTERS, not bytes.  A byte-counting
    // implementation puts every boundary after a multi-byte run in the wrong
    // place, which is the same defect the highlight offsets were fixed for.
    SOutputLine utf8;
    utf8.push_back(run("\xE2\x82\xAC\xE2\x82\xAC"));   // 2 euro signs, 6 bytes
    utf8.push_back(run("x"));                          // col 2
    opCheckEq(LineCharLen(utf8), 3, "two euro signs plus 'x' is three columns, not seven");
    opCheck(COutputPane::RunAtCharColumn(utf8, 1) == &utf8[0],
            "col 1 is the second euro sign");
    opCheck(COutputPane::RunAtCharColumn(utf8, 2) == &utf8[1],
            "col 2 is the 'x' after them, by CHARACTER not byte");
    opCheck(COutputPane::RunAtCharColumn(utf8, 3) == nullptr,
            "...and col 3 is past the end");
}

// ── a plugin that prints from ON_PLUGIN_DRAW_OUTPUT_WINDOW ──────────────────
// OnPaint fires that callback between the underneath-miniwindow composite and
// the text pass, so it runs arbitrary plugin script in the middle of a paint --
// and plugins DO print from it (Quow-class status plugins on their error
// paths).  AppendLine therefore defers anything that arrives while the window
// is open instead of mutating m_lines / m_lineChars / m_visualRowStart under
// the draw loop that is about to walk them.
//
// That branch had never executed: the flag was private, only OnPaint wrote it,
// and OnPaint needs an event loop this harness (like every in-app selftest)
// exits before reaching.  Both failure modes are user-visible -- no deferral is
// a crash inside the paint handler, and a replay that drops the line is a
// plugin message that silently never appears.
std::string LineText(const SOutputLine* line)
{
    std::string out;
    if (line)
        for (const SOutputRun& r : *line) out += r.text;
    return out;
}

// CallAfter queues a wxAsyncMethodCallEvent on the pane; it is delivered by
// ProcessPendingEvents, which wxYield's temporary loop drives.  One turn is
// enough -- the replays are queued, not timed.
void PumpDeferred() { wxYield(); }

void TestPluginDrawDefersAppends()
{
    printf("prints from ON_PLUGIN_DRAW_OUTPUT_WINDOW are deferred, not lost\n");

    // ── the callback really runs inside the guard ────────────────────────
    // FirePluginDraw is OnPaint's one line, lifted out so the pairing itself
    // can be pinned: a later edit that moves the call out from under the scope
    // would otherwise go unnoticed until it crashed a user's paint.
    {
        Harness h;
        h.Feed(3);
        bool sawGuard = false;
        int  gotLine = -1, gotOffset = -1;
        h.pane->m_fn_draw_output_window = [&](int firstLine, int pixelOffset) {
            sawGuard  = h.pane->InPluginDraw();
            gotLine   = firstLine;
            gotOffset = pixelOffset;
        };
        opCheck(!h.pane->InPluginDraw(), "the window is shut before the callback");
        h.pane->FirePluginDraw(7, 42);
        opCheck(sawGuard, "the plugin callback runs with the re-entrancy window OPEN");
        opCheckEq(gotLine,   7,  "...and is handed the first visible line");
        opCheckEq(gotOffset, 42, "...and the pixel offset");
        opCheck(!h.pane->InPluginDraw(), "the window shuts again when it returns");
        h.pane->m_fn_draw_output_window = nullptr;
    }

    // ── one line, deferred and replayed exactly once ─────────────────────
    {
        Harness h;
        h.pane->AppendLine(MakeLine("before the paint"));
        opCheckEq(h.pane->LineCount(), 1,
                  "control: a line appended OUTSIDE the window lands at once");

        {
            COutputPane::PluginDrawScope guard(*h.pane);
            opCheck(h.pane->InPluginDraw(), "the scope reports itself open");
            h.pane->AppendLine(MakeLine("printed mid-paint"));
            opCheckEq(h.pane->LineCount(), 1,
                      "a line appended INSIDE the window is not in the buffer yet");
            // The load-bearing one, and the only one of the two that means the
            // same thing in the app.  This harness builds a bare COutputPane,
            // so it runs the PUSH model and LineCount() is m_lines.size();
            // CWorldChildFrame wires the pull seam unconditionally, so in
            // production LineCount() is the ENGINE's count and the engine has
            // already appended before it calls the pane -- the deferral does
            // not move it at all there.  The wrap cache is what is genuinely
            // held back in both models, so assert on that.
            opCheckEq(h.pane->DerivedLength(), 1,
                      "...and the wrap cache the draw loop is walking is untouched");
        }
        opCheck(!h.pane->InPluginDraw(), "closing the scope reopens the pane");
        opCheckEq(h.pane->LineCount(), 1,
                  "closing it does not itself replay -- that needs the event loop");

        PumpDeferred();
        opCheckEq(h.pane->LineCount(), 2,
                  "one turn later the deferred line has arrived -- exactly once");
        opCheck(LineText(h.pane->GetLine(1)) == "printed mid-paint",
                "...with its text intact, after the line that preceded it");
        opCheckEq(h.pane->DerivedLength(), h.pane->LineCount(),
                  "wrap cache and line count agree after the replay");

        // A second turn must not conjure a third copy: the replay is consumed.
        PumpDeferred();
        opCheckEq(h.pane->LineCount(), 2, "and it does not arrive again on the next turn");
    }

    // ── NESTED scopes: the whole reason this is a count, not a flag ──────
    // A plugin callback can force a synchronous repaint, which re-enters
    // OnPaint and opens a second scope inside the first.  With a bool, the
    // INNER close reopens the buffer while the OUTER callback is still
    // running -- which is exactly the hazard the guard exists to prevent, and
    // it is the one behaviour the switch from bool to int actually changes.
    // Without this group, replacing EndPluginDraw's decrement with "= 0"
    // passes the whole suite, so a later "simplify this back to a flag" would
    // sail through.
    {
        Harness h;
        h.pane->AppendLine(MakeLine("before"));
        opCheckEq(h.pane->LineCount(), 1, "nested: one line to start");

        {
            COutputPane::PluginDrawScope outer(*h.pane);
            {
                COutputPane::PluginDrawScope inner(*h.pane);
                opCheck(h.pane->InPluginDraw(), "nested: the inner scope is open");
                h.pane->AppendLine(MakeLine("from the inner paint"));
                opCheckEq(h.pane->DerivedLength(), 1,
                          "nested: an append inside the inner scope is deferred");
            }
            // THE assertion.  A bool would report closed here.
            opCheck(h.pane->InPluginDraw(),
                    "nested: closing the INNER scope leaves the window open");
            h.pane->AppendLine(MakeLine("from the outer paint"));
            opCheckEq(h.pane->DerivedLength(), 1,
                      "nested: ...so the outer callback's append is deferred too");
        }
        opCheck(!h.pane->InPluginDraw(),
                "nested: closing the outer scope finally reopens the pane");

        PumpDeferred();
        opCheckEq(h.pane->LineCount(), 3, "nested: both deferred lines arrive");
        opCheck(LineText(h.pane->GetLine(1)) == "from the inner paint" &&
                LineText(h.pane->GetLine(2)) == "from the outer paint",
                "nested: ...in the order they were printed");
        opCheckEq(h.pane->DerivedLength(), h.pane->LineCount(),
                  "nested: wrap cache agrees afterwards");
    }

    // ── several lines from one callback, in order ────────────────────────
    {
        Harness h;
        {
            COutputPane::PluginDrawScope guard(*h.pane);
            h.pane->AppendLine(MakeLine("alpha"));
            h.pane->AppendLine(MakeLine("bravo"));
            h.pane->AppendLine(MakeLine("charlie"));
            opCheckEq(h.pane->LineCount(), 0, "three prints inside one paint, none applied");
        }
        PumpDeferred();
        opCheckEq(h.pane->LineCount(), 3, "all three arrive");
        opCheck(LineText(h.pane->GetLine(0)) == "alpha" &&
                LineText(h.pane->GetLine(1)) == "bravo" &&
                LineText(h.pane->GetLine(2)) == "charlie",
                "...IN ORDER -- a plugin's multi-line report is not shuffled");
        opCheckEq(h.pane->DerivedLength(), 3, "wrap cache holds the same three");
    }

    // ── the in-progress prompt row is not disturbed ──────────────────────
    // A guard placed after the pending-row handling would clear the prompt the
    // paint is still drawing, and only put the replacement back a turn later.
    {
        Harness h;
        h.pane->AppendLine(MakeLine("You are in a dark room."));
        h.pane->AppendLine(MakeLine("HP:100 MP:50 > ", /*pending*/ true));
        opCheck(h.pane->HasPendingLine(), "a prompt row is showing before the paint");

        {
            COutputPane::PluginDrawScope guard(*h.pane);
            h.pane->AppendLine(MakeLine("[status] connection wobbled"));
            opCheck(h.pane->HasPendingLine(),
                    "the deferral leaves the prompt row the paint is drawing alone");
            opCheckEq(h.pane->LineCount(), 1, "...and adds nothing to the buffer");
        }
        PumpDeferred();
        opCheckEq(h.pane->LineCount(), 2, "the plugin's note arrives after the paint");
        opCheck(!h.pane->HasPendingLine(),
                "...and supersedes the prompt exactly as an immediate append would");
    }

    // ── a PENDING row printed inside the window defers too ───────────────
    {
        Harness h;
        h.pane->AppendLine(MakeLine("committed"));
        {
            COutputPane::PluginDrawScope guard(*h.pane);
            h.pane->AppendLine(MakeLine("mid-paint prompt > ", /*pending*/ true));
            opCheck(!h.pane->HasPendingLine(),
                    "a pending row printed inside the window is deferred as well");
        }
        PumpDeferred();
        opCheck(h.pane->HasPendingLine(), "...and shows once the paint is over");
        opCheckEq(h.pane->LineCount(), 1, "a pending row is still not a buffered line");
    }

    // ── batching does not swallow the replay ─────────────────────────────
    // The replay lands on the next event-loop turn, which in principle can be
    // inside a Begin/EndBatch pair opened by the display drain.  A batch only
    // defers the scrollbar update and the repaint, so the line itself must
    // still be appended.
    {
        Harness h;
        h.pane->BeginBatch();
        {
            COutputPane::PluginDrawScope guard(*h.pane);
            h.pane->AppendLine(MakeLine("deferred into a batch"));
        }
        PumpDeferred();
        opCheckEq(h.pane->LineCount(), 1,
                  "the replay appends even with a batch still open");
        h.pane->EndBatch();
        opCheckEq(h.pane->LineCount(), 1, "closing the batch neither drops nor doubles it");
        opCheckEq(h.pane->DerivedLength(), 1, "wrap cache agrees");
    }

    // ── the capacity trim runs on the replayed line ──────────────────────
    // The deferral moves the append, not its bookkeeping: a full buffer must
    // still trim its head, or a plugin that prints every paint grows it without
    // bound.  (100 is SetMaxLines' floor for any non-zero cap.)
    {
        Harness h;
        h.pane->SetMaxLines(100);
        h.Feed(100, "keep");
        opCheckEq(h.pane->LineCount(), 100, "the buffer starts full");
        const std::string firstBefore = LineText(h.pane->GetLine(0));

        {
            COutputPane::PluginDrawScope guard(*h.pane);
            h.pane->AppendLine(MakeLine("trimmer"));
            opCheckEq(h.pane->LineCount(), 100, "nothing trimmed during the paint");
        }
        PumpDeferred();
        opCheckEq(h.pane->LineCount(), 100, "still at the cap after the replay");
        opCheck(LineText(h.pane->GetLine(99)) == "trimmer",
                "the replayed line is the newest");
        opCheck(LineText(h.pane->GetLine(0)) != firstBefore,
                "...and the oldest was trimmed to make room for it");
        opCheckEq(h.pane->DerivedLength(), h.pane->LineCount(),
                  "wrap cache and line count agree after a replay that trimmed");
    }
}

// ── 18. the PULL seam: cached fetches and the wrap metrics ──────────────────
// Everything above builds a pane that keeps its own content, which is the model
// the app does NOT use: CWorldChildFrame wires m_fn_fetch_line and the pane
// then holds only wrap metrics, converting a line from the engine's CLine on
// demand and caching the conversion.  Two defects live only there.
namespace {

// The fake engine buffer, plus the counter that makes "did this fetch?"
// observable at all -- the whole point of the pull model is that a fetch is a
// full CLine -> SOutputLine conversion, so an unnecessary one is a real cost.
std::vector<std::string> g_pullBuf;
long                     g_pullFetches = 0;
bool                     g_pullRule    = false;   // is the last line a rule?

void WirePullSeam(COutputPane* pane)
{
    pane->m_fn_line_count = []() { return (int) g_pullBuf.size(); };
    pane->m_fn_fetch_line = [](int i, SOutputLine& out) -> bool {
        if (i < 0 || i >= (int) g_pullBuf.size()) return false;
        ++g_pullFetches;
        if (g_pullRule && i == (int) g_pullBuf.size() - 1) {
            SOutputRun rule;
            rule.horiz_rule = true;
            out.clear();
            out.push_back(rule);
        } else {
            out = MakeLine(g_pullBuf[(size_t) i].c_str());
        }
        return true;
    };
}

// Push one line through BOTH sides at once: the engine gains it, then the pane
// is told about it, exactly as the display drain does.
void PullAppend(COutputPane* pane, const std::string& text)
{
    g_pullBuf.push_back(text);
    pane->AppendLine(MakeLine(text.c_str()));
}

} // namespace

void TestPullSeamReads()
{
    printf("pull seam: the line cache and the wrap metrics\n");

    // ── the wrap cache must not fetch ────────────────────────────────────
    // WrapCount is called once per BUFFERED line by every RebuildWrapCache --
    // a resize, a font change, a text-rectangle change.  It used to fetch the
    // whole line first, only to test a horiz_rule guard its own arithmetic
    // already answers, so a rebuild converted the entire scrollback.
    {
        g_pullBuf.clear();
        g_pullFetches = 0;
        g_pullRule    = false;
        Harness h;
        WirePullSeam(h.pane);
        for (int i = 0; i < 700; ++i) {
            char buf[64];
            snprintf(buf, sizeof buf, "pull line %04d alpha bravo charlie", i);
            PullAppend(h.pane, buf);
        }
        // ...and one horizontal rule as the last line, which is the case the
        // deleted guard existed for.
        g_pullRule = true;
        g_pullBuf.push_back("(rule)");
        {
            SOutputLine rule;
            SOutputRun run;
            run.horiz_rule = true;
            rule.push_back(run);
            h.pane->AppendLine(std::move(rule));
        }
        const int n = h.pane->LineCount();
        opCheckEq(n, 701, "pull: the engine's count is what the pane reports");
        opCheckEq(h.pane->DerivedLength(), n, "pull: ...and the wrap cache agrees");

        const long long rowsBefore = h.pane->TotalVisualRows();
        const int ruleRowsBefore   = h.pane->VisualRowsForLine(n - 1);
        opCheckEq(ruleRowsBefore, 1, "pull: a horizontal rule is one visual row");

        g_pullFetches = 0;
        h.pane->SetTextPixelOffset(7);          // forces a full RebuildWrapCache
        opCheckEq((int) g_pullFetches, 0,
                  "pull: rebuilding the wrap cache fetches NOTHING from the engine");
        opCheck(h.pane->TotalVisualRows() == rowsBefore,
                "pull: ...and the row total is unchanged (it still computed them)");
        opCheckEq(h.pane->VisualRowsForLine(n - 1), 1,
                  "pull: ...including the rule line, still one row");
    }

    // ── one paint's second fetch must not free the line being painted ────
    // DrawLine held the reference LineAt(lineIdx) handed back across a second
    // LineAt(lineIdx - 1) for the preamble's delta.  The cache is sized so a
    // viewport's forward walk cannot evict what it is looking at -- but
    // lineIdx-1 walks BACKWARDS out of the cached region, which is exactly the
    // eviction the sizing does not cover, and the victim is the line being
    // drawn.  The defined, observable consequence is which entries survive the
    // paint, so that is what is asserted.
    {
        g_pullBuf.clear();
        g_pullFetches = 0;
        g_pullRule    = false;
        Harness h;
        WirePullSeam(h.pane);
        const int cap = COutputPane::LineCacheCapacity();
        const int n   = cap + 100;
        for (int i = 0; i < n; ++i) {
            char buf[64];
            snprintf(buf, sizeof buf, "cache line %04d", i);
            PullAppend(h.pane, buf);
        }

        // Prime the cache with an ASCENDING walk, the shape RebuildWrapCache
        // and a forward Find both make.  The cache then holds the last `cap`
        // indices and the OLDEST of them is the eviction victim.
        for (int i = 0; i < n; ++i) h.pane->GetLine(i);
        const int oldest = n - cap;
        opCheck(oldest > 0, "cache: the oldest cached line has a predecessor "
                            "(control: with none, DrawLine fetches nothing)");

        // The preamble callback is what makes DrawLine want line-1 at all, and
        // it is handed the very reference the old code had already freed.
        int  preambleCalls = 0;
        bool sawLiveLine   = false;
        h.pane->m_fn_line_preamble =
            [&](const SOutputLine& line, time_t, bool,
                COutputPane::SLinePreamble& out) {
                ++preambleCalls;
                sawLiveLine = !line.empty() &&
                              line.front().text.find("cache line") != std::string::npos;
                out.text = wxT("[TS] ");
                return true;
            };

        wxBitmap bmp(400, 40, 32);
        wxMemoryDC mdc(bmp);
        mdc.SetBackground(*wxBLACK_BRUSH);
        mdc.Clear();
        h.pane->RenderLineForTest(mdc, oldest, 0);
        mdc.SelectObject(wxNullBitmap);

        opCheck(preambleCalls > 0,
                "cache: the paint really did ask for the previous line "
                "(control: without this the check below is vacuous)");
        opCheck(sawLiveLine,
                "cache: the line being painted still carries its own text");

        // ...and the entry for it is still cached, because it was fetched
        // AFTER the predecessor rather than before it.  A fetch here means the
        // paint's own line had been evicted out from under it.
        const long before = g_pullFetches;
        h.pane->GetLine(oldest);
        opCheckEq((int)(g_pullFetches - before), 0,
                  "cache: ...and its cached conversion survived the paint");

        h.pane->m_fn_line_preamble = nullptr;
    }

    // ── an append must not throw the cache away ──────────────────────────
    // A line arriving at the TAIL shifts no index, so every conversion already
    // cached is still valid.  AppendLine dropped the whole cache anyway, which
    // meant that while a MUD was talking -- i.e. continuously -- no cached
    // conversion survived from one paint to the next and each paint
    // re-materialised the visible lines from their CLines.  The count is exact
    // rather than a bound, because a cache that silently stopped caching
    // altogether would satisfy any "<= N" form.
    {
        g_pullBuf.clear();
        g_pullFetches = 0;
        g_pullRule    = false;
        Harness h;
        WirePullSeam(h.pane);
        const int n = 50;                       // well inside the cache capacity
        opCheck(n < COutputPane::LineCacheCapacity(),
                "append cache: the fixture fits the cache (control: it must not "
                "be the eviction that causes a re-fetch)");
        for (int i = 0; i < n; ++i) {
            char buf[64];
            snprintf(buf, sizeof buf, "append line %04d", i);
            PullAppend(h.pane, buf);
        }

        g_pullFetches = 0;
        for (int i = 0; i < n; ++i) h.pane->GetLine(i);
        opCheckEq((int) g_pullFetches, n,
                  "append cache: the first walk converts every line (control)");

        PullAppend(h.pane, "append line 0050");
        g_pullFetches = 0;
        for (int i = 0; i <= n; ++i) h.pane->GetLine(i);
        opCheckEq((int) g_pullFetches, 1,
                  "append cache: the second walk converts ONLY the new line");
    }

    // ── click containment stops at the end of the visual row ─────────────
    // XToChar clamped the per-row column to m_wrapCols for BOTH forms.  For the
    // caret that is right (the caret sits after the last cell); for containment
    // m_wrapCols is the FIRST column of the NEXT visual row, so a click to the
    // right of the text on a wrapped line resolved into that row and could fire
    // its hyperlink.
    {
        Harness h;
        const int cols = h.pane->WrapColumns();
        opCheck(cols > 8, "containment: the pane has a real wrap column (control)");

        SOutputLine line;
        SOutputRun plain;
        plain.text = std::string((size_t) cols, 'a');
        line.push_back(plain);
        SOutputRun link;
        link.text = "LINKLINKLINK";
        link.setLink("go north", "a hint", 0);
        line.push_back(link);
        h.pane->AppendLine(std::move(line));

        const int y     = h.pane->LineHeight() / 2;              // visual row 0
        const int xPast = (cols + 3) * h.pane->CharWidth();      // past the text
        const SelPoint caret  = h.pane->PointToChar(xPast, y, /*halfway=*/true);
        const SelPoint inside = h.pane->PointToChar(xPast, y, /*halfway=*/false);
        opCheckEq(caret.col, cols,
                  "containment: the caret form still reaches the wrap column");
        opCheck(inside.col == cols - 1,
                "containment: the hit-test form stops at the last cell of row 0");

        const SOutputLine* l = h.pane->GetLine(0);
        const SOutputRun* hit = l ? COutputPane::RunAtCharColumn(*l, inside.col)
                                  : nullptr;
        opCheck(hit && !hit->isLink(),
                "containment: a click past the text hits no hyperlink");
        const SOutputRun* rowOne = l ? COutputPane::RunAtCharColumn(*l, cols)
                                     : nullptr;
        opCheck(rowOne && rowOne->isLink(),
                "containment: control -- the next visual row really does start "
                "with the link the old clamp resolved into");
    }
}

// ── 19. the in-progress (pending) row is drawn like any other line ──────────
// It used to be hand-painted: one DrawText per run against the PANE's
// background, in the plain font, advancing by the font's natural extents.  So
// the prompt -- exactly the kind of line a MUD colours -- lost its run
// background, its bold/italic/underline/strikethrough attributes and the fixed
// character grid, then changed appearance the moment the '\n' arrived and the
// same text came back through DrawRunChunk as a committed line.
//
// Pixels are the only observable here, and the paint path never runs under an
// OnInit-scoped harness, so this calls the drawing entry point straight into a
// wxMemoryDC -- the composite-selftest trick.
// ── 18b. the PULL seam: URL auto-detection must reach what is DRAWN ─────────
// DetectURLs runs on the append path, but under the pull model the appended
// line's content is dropped and re-fetched through m_fn_fetch_line, so the
// detection done at AppendLine never reached the line the pane actually draws
// and hit-tests: a bare http:// in MUD output was neither underlined nor
// clickable, where Windows makes it a link.  LineAt now detects as well.
void TestPullSeamDetectsURLs()
{
    printf("pull seam: a bare URL fetched from the engine is a link\n");

    Harness h;
    g_pullBuf.clear();
    g_pullFetches = 0;
    g_pullRule    = false;
    WirePullSeam(h.pane);

    // Line 0 is the URL alone so a click anywhere in its first characters is
    // inside the link run; line 1 is the control.  Asserting through a real
    // click rather than through the cached line is deliberate: "is it
    // clickable?" is the behaviour the decision was about, and the click path
    // is what regressed -- the detector itself never stopped working.
    const std::string kUrl   = "http://example.com/a?b=c";
    const std::string kPlain = "see the notice board for details";

    PullAppend(h.pane, kUrl);
    PullAppend(h.pane, kPlain);

    int         fired = 0;
    std::string clicked;
    h.pane->m_fn_link_clicked =
        [&fired, &clicked](const std::string& action, int, bool, SRunEncoding) {
            ++fired;
            clicked = action;
        };

    const int cw = h.pane->CharWidth();
    const int lh = h.pane->LineHeight();

    // Column 2 of row 0 -- well inside "http://…".
    SendMouse(h.pane, wxEVT_LEFT_DOWN, cw * 2 + cw / 2, lh / 2);
    SendMouse(h.pane, wxEVT_LEFT_UP,   cw * 2 + cw / 2, lh / 2);

    opCheck(fired == 1, "pull: clicking a bare http:// URL fires the link handler");
    opCheck(clicked == kUrl, "pull: ...with the URL itself as the action");

    // ── the control ──────────────────────────────────────────────────────
    // Without this, both assertions above would also pass a build that made
    // every run a link.
    fired = 0;
    clicked.clear();
    SendMouse(h.pane, wxEVT_LEFT_DOWN, cw * 2 + cw / 2, lh + lh / 2);
    SendMouse(h.pane, wxEVT_LEFT_UP,   cw * 2 + cw / 2, lh + lh / 2);

    opCheck(fired == 0, "pull (control): a line with no URL fires nothing");

    // ── the link's EXTENT, which is where an off-by-one would hide ───────
    // The last character of the URL must still be the link, and the first
    // column past it must not be.  This is the assertion that would catch a
    // split that ate or duplicated a byte: the columns would shift and one of
    // these two clicks would land on the wrong run.
    const int lastCol = (int) kUrl.size() - 1;

    fired = 0;
    SendMouse(h.pane, wxEVT_LEFT_DOWN, cw * lastCol + cw / 2, lh / 2);
    SendMouse(h.pane, wxEVT_LEFT_UP,   cw * lastCol + cw / 2, lh / 2);
    opCheck(fired == 1 && clicked == kUrl,
            "pull: the URL's last character is still inside the link");

    fired = 0;
    SendMouse(h.pane, wxEVT_LEFT_DOWN, cw * (lastCol + 1) + cw / 2, lh / 2);
    SendMouse(h.pane, wxEVT_LEFT_UP,   cw * (lastCol + 1) + cw / 2, lh / 2);
    opCheck(fired == 0,
            "pull: the column just past the URL is NOT part of the link");
}

// ── 18c. a link's ENCODING has to travel with its action ─────────────────
// The action/hint bytes of an MXP link are the world's bytes, in the world's
// charset -- exactly like the run text beside them.  The pane handed the action
// on as a bare std::string, so every DISPLAY site downstream reached for
// wxString::FromUTF8, which is all-or-nothing: one legacy byte and the label,
// the tooltip, or (worst) the command bar came back EMPTY.  The run knows the
// answer; it just has to be carried.
//
// The fixture is deliberately non-degenerate: CP1251 (charset 204) and CP1252
// read these six bytes as completely different words, so an assertion on the
// decoded text discriminates BOTH halves of the encoding -- a build that
// dropped the flag and a build that hardcoded the charset each fail it.
void TestLinkEncodingTravelsWithTheAction()
{
    printf("link encoding: the run's charset reaches the click handler\n");

    // "\xCF\xF0\xE8\xE2\xE5\xF2" is CP1251 for the Russian for "hello".
    // It is not valid UTF-8 (0xCF is a 2-byte lead, 0xF0 is not a continuation).
    const std::string kAction = "say \xCF\xF0\xE8\xE2\xE5\xF2";
    const wxString    kCp1251 = wxString::FromUTF8(
        "say \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");

    opCheck(wxString::FromUTF8(kAction.c_str(), kAction.size()).empty(),
          "fixture: FromUTF8 of the action really does yield \"\" "
          "(without this the test proves nothing)");

    Harness h;

    auto linkLine = [&](bool utf8, int charset) {
        SOutputRun run;
        run.text    = "click here";       // ASCII, so the column maths is plain
        run.fg      = 0x00FFFFFFu;
        run.utf8    = utf8 ? 1 : 0;
        run.charset = (int16_t) charset;
        run.setLink(kAction, "the hint", 0x0400 /*ACTION_SEND*/);
        SOutputLine line;
        line.push_back(run);
        return line;
    };

    h.pane->AppendLine(linkLine(false, 204));   // row 0: a CP1251 world
    h.pane->AppendLine(linkLine(true, 0));      // row 1: the UTF-8 control

    int          fired = 0;
    std::string  gotAction;
    SRunEncoding gotEnc{ true, 999 };           // neither value a fix can luck into
    h.pane->m_fn_link_clicked =
        [&](const std::string& action, int, bool, SRunEncoding enc) {
            ++fired;
            gotAction = action;
            gotEnc    = enc;
        };

    const int cw = h.pane->CharWidth();
    const int lh = h.pane->LineHeight();
    auto clickRow = [&](int row) {
        SendMouse(h.pane, wxEVT_LEFT_DOWN, cw * 2 + cw / 2, row * lh + lh / 2);
        SendMouse(h.pane, wxEVT_LEFT_UP,   cw * 2 + cw / 2, row * lh + lh / 2);
    };

    clickRow(0);
    opCheck(fired == 1, "the click reaches the link handler");
    opCheck(gotAction == kAction,
            "the action arrives BYTE-EXACT (what gets sent must not change)");
    opCheck(gotEnc.utf8 == false,
            "...and the run's utf8 flag arrives with it (was: not carried at all)");
    opCheck(gotEnc.charset == 204,
            "...and so does the run's charset");
    opCheck(DecodeRunBytes(gotAction, gotEnc) == kCp1251,
            "...so the handler can render the action as its CP1251 text "
            "(was: FromUTF8 -> \"\", a blank menu label / cleared command bar)");

    // ── the control ──────────────────────────────────────────────
    // A build that simply hardcoded {false, 204} would pass everything above.
    fired = 0;
    gotEnc = SRunEncoding{ false, 999 };
    clickRow(1);
    opCheck(fired == 1, "control: the UTF-8 row's link fires too");
    opCheck(gotEnc.utf8 == true && gotEnc.charset == 0,
            "control: ...and reports ITS encoding, not the other row's");
}

void TestPendingRowUsesTheRunPipeline()
{
    printf("pending row: the prompt is drawn through DrawRunChunk\n");

    Harness h;
    const int cw = h.pane->CharWidth();
    const int lh = h.pane->LineHeight();
    opCheck(cw > 2 && lh > 7,
            "pending: the pane has real character metrics (control)");
    opCheckEq((int) h.pane->PaneBackground(), 0,
              "pending: ...and a black background, which the dim halves toward "
              "(control: the expected colours below are derived from it)");

    // SPACES, not glyphs: nothing inks the cells, so every non-background pixel
    // sampled below comes from the run's own background fill or its
    // strikethrough rule, with no anti-aliasing to reason about.  The
    // strikethrough stands in for the whole attribute set -- it is the one
    // DrawRunChunk draws itself rather than delegating to a font variant.
    SOutputRun run;
    run.text          = "    ";               // 4 character cells
    run.fg            = 0x00FFFFFFu;          // white
    run.bg            = 0x000000FFu;          // COLORREF 0x00BBGGRR — red
    run.bold          = true;
    run.strikethrough = true;
    // A second run, so where it STARTS pins the x advance (the cell grid) and
    // not merely the fill width of the first.
    SOutputRun tail;
    tail.text = "  ";                         // 2 more cells
    tail.fg   = 0x00FFFFFFu;
    tail.bg   = 0x0000FF00u;                  // green
    SOutputLine line;
    line.push_back(run);
    line.push_back(tail);
    line.pending = true;
    h.pane->AppendLine(std::move(line));

    wxBitmap bmp(cw * 8, lh + 4, 32);
    {
        wxMemoryDC mdc(bmp);
        // A colour neither the run nor the dim can produce, so "untouched" is
        // distinguishable from "painted".
        mdc.SetBackground(*wxBLUE_BRUSH);
        mdc.Clear();
        h.pane->RenderPendingRowForTest(mdc, 0);
        mdc.SelectObject(wxNullBitmap);
    }
    const wxImage img = bmp.ConvertToImage();

    auto pixelIs = [&](int x, int y, int r, int g, int b, const char* what) {
        const bool ok = img.GetRed(x, y) == r && img.GetGreen(x, y) == g &&
                        img.GetBlue(x, y) == b;
        printf("  [%s] %s (got %d,%d,%d want %d,%d,%d at %d,%d)\n",
               ok ? "PASS" : "FAIL", what,
               img.GetRed(x, y), img.GetGreen(x, y), img.GetBlue(x, y),
               r, g, b, x, y);
        (ok ? g_pass : g_fail)++;
    };

    // Off the strikethrough row, so this samples the background fill alone.
    const int yFill = lh / 4;

    // The run's OWN background, dimmed 50% toward the pane's: red -> (127,0,0).
    // The hand-painted row filled with the pane background instead, so this
    // pixel was black.
    pixelIs(cw + cw / 2, yFill, 127, 0, 0,
            "pending: the run's background colour reaches the row");

    // The strikethrough rule, in the dimmed foreground: white -> (127,127,127).
    // Nothing drew it before.
    pixelIs(cw / 2, lh / 2, 127, 127, 127,
            "pending: ...and so do the run's text attributes");

    // The fixed grid: exactly four cells wide, whatever the font's natural
    // advance is.  The last cell of the first run is painted...
    pixelIs(3 * cw + cw / 2, yFill, 127, 0, 0,
            "pending: the fourth cell is painted (grid width, not font extent)");
    // ...the second run starts on the very next cell...
    pixelIs(4 * cw + cw / 2, yFill, 0, 127, 0,
            "pending: the next run starts at exactly 4 * m_charW");
    // ...and nothing past the six cells is touched.
    pixelIs(6 * cw + 1, yFill, 0, 0, 255,
            "pending: ...and the seventh cell is untouched");

    // Whether the two assertions above can tell the grid from the font's own
    // advances at all depends on the font: if they agree to the pixel here, the
    // fixture is degenerate and those two prove nothing.  Reported rather than
    // asserted, because which it is is a property of the machine's font.
    {
        wxBitmap probe(8, 8, 32);
        wxMemoryDC pdc(probe);
        pdc.SetFont(h.pane->GetCurrentFont());
        wxCoord tw = 0, th = 0;
        pdc.GetTextExtent(wxT("    "), &tw, &th);
        pdc.SelectObject(wxNullBitmap);
        printf("  [INFO] pending: natural extent of 4 cells = %d, grid = %d%s\n",
               (int) tw, 4 * cw,
               (tw == 4 * cw) ? "  (EQUAL: the grid assertions are degenerate here)"
                              : "");
    }
}

// ── 20. Middle-click over the output pane does nothing ───────────────────────
// The pane used to read the system clipboard on middle-up and drop it into the
// command bar.  That is a convention neither macOS nor Windows has: Windows'
// CMUSHView::OnMButtonDown/OnMButtonUp (mushview.cpp:2304 / :2420) offer the
// point to the miniwindow hotspot layer and otherwise chain to
// CView::OnMButtonDown/Up — the MFC defaults, straight to DefWindowProc — and
// CSendView has no middle-button handling at all.  A stray middle click
// therefore silently rewrote whatever the user was typing.
//
// The GTK primary-selection paste is a different thing and is kept, so the
// disposition assertions are per-platform.
//
// "No paste happened" on its own is a weak assertion: it also passes if no
// handler ran at all.  What discriminates DECLINED from NOTHING-RAN is
// ProcessWindowEvent's return — true when a handler consumed the event, false
// when it skipped and nobody else wanted it — so that is asserted directly,
// with a control proving false is reachable through this same harness.
void TestMiddleClickIsInert()
{
    printf("middle-click over the output pane (mushview.cpp:2304 / :2420)\n");

    // Dispatch and report whether the pane CONSUMED the event.
    auto send = [](COutputPane* pane, wxEventType type, int x, int y) -> bool {
        wxMouseEvent evt(type);
        evt.SetPosition(wxPoint(x, y));
        evt.SetEventObject(pane);
        evt.SetId(pane->GetId());
        return pane->ProcessWindowEvent(evt);
    };

    // ── the instrument first ─────────────────────────────────────────────
    // COutputPane::OnRightUp ends in evt.Skip() unconditionally, so a right-up
    // through this same path must come back FALSE.  Without this, "middle-up
    // was consumed" would be unfalsifiable if ProcessWindowEvent happened to
    // return true for everything.
    {
        Harness h;
        h.Feed(5);
        opCheck(!send(h.pane, wxEVT_RIGHT_UP, 10, 10),
                "control: an event the pane passes on reports NOT consumed");
    }

    // ── the hotspot layer still sees both edges ──────────────────────────
    {
        Harness h;
        h.Feed(5);

        int  downCalls = 0, upCalls = 0;
        long downFlags = 0, upFlags = 0;
        int  downX = -1, downY = -1, upX = -1, upY = -1;

        h.pane->m_fn_miniwindow_mouse_down =
            [&](int x, int y, long f) { downCalls++; downX = x; downY = y;
                                        downFlags = f; return false; };
        h.pane->m_fn_miniwindow_mouse_up =
            [&](int x, int y, long f) { upCalls++; upX = x; upY = y;
                                        upFlags = f; return false; };

        send(h.pane, wxEVT_MIDDLE_DOWN, 37, 21);
        send(h.pane, wxEVT_MIDDLE_UP,   37, 21);

        opCheckEq(downCalls, 1, "middle-down is offered to the hotspot layer");
        opCheckEq(upCalls,   1, "middle-up is offered to the hotspot layer");
        opCheckEq((int) downFlags, 0x200, "...as MW_MOUSE_MIDDLE (down)");
        opCheckEq((int) upFlags,   0x200, "...as MW_MOUSE_MIDDLE (up)");
        opCheck(downX == 37 && downY == 21 && upX == 37 && upY == 21,
                "...at the click point");
    }

    // ── a hotspot that claims the click consumes it, and never pastes ────
    {
        Harness h;
        h.Feed(5);

        int pasted = 0;
        h.pane->m_fn_put_in_input   = [&](const wxString&) { pasted++; };
        h.pane->m_fn_miniwindow_mouse_up   = [](int, int, long) { return true; };
        h.pane->m_fn_miniwindow_mouse_down = [](int, int, long) { return true; };

        opCheck(send(h.pane, wxEVT_MIDDLE_DOWN, 12, 12),
                "a claimed middle-down is consumed by the hotspot layer");
        opCheck(send(h.pane, wxEVT_MIDDLE_UP, 12, 12),
                "a claimed middle-up is consumed by the hotspot layer");
        opCheckEq(pasted, 0, "...and a claimed click never reaches the input bar");
    }

    // ── the real case: no hotspot under the click ────────────────────────
    {
        // A sentinel on the system clipboard, so that if the old arm were still
        // there it would have something non-empty to paste.  Restored after.
        const ClipboardGuard clip;
        SetClipboardText("SENTINEL-MIDDLE-CLICK-PASTE");

        Harness h;
        h.Feed(5);

        int      pasted = 0;
        wxString pastedText;
        h.pane->m_fn_put_in_input = [&](const wxString& t) { pasted++; pastedText = t; };

        // No hotspot layer installed at all — the plain "user middle-clicked
        // the scrollback" case.
        send(h.pane, wxEVT_MIDDLE_DOWN, 30, 15);
        const bool upConsumed = send(h.pane, wxEVT_MIDDLE_UP, 30, 15);

#ifdef __WXGTK__
        // Linux keeps the X11 primary-selection convention; the handler runs it
        // and passes the event on, exactly as before.  Whether anything is IN
        // the primary selection is an X server property this harness cannot
        // set, so only the disposition is pinned here.
        opCheck(!upConsumed,
                "GTK: middle-up runs the primary-selection paste and passes the event on");
#else
        opCheckEq(pasted, 0,
                  "middle-click does NOT paste the clipboard into the input bar");
        opCheck(upConsumed,
                "...because the handler DECLINED it: consumed, not skipped");
        opCheck(pastedText.IsEmpty(), "...and no text was handed to the input bar");
#endif

    }
}

// ── The right-click context menu ─────────────────────────────────────────────
// Two things are pinned here and they are different in kind:
//
//   * the MENU — its items, their order, their grouping and their enable /
//     check state.  Built by COutputPane::BuildContextMenu, which is called
//     directly rather than through a popup: a selftest exits from
//     wxApp::OnInit, so PopupMenu's nested tracking loop would never return.
//   * the SELECTION side-effect of the right-click itself, which is a
//     deliberate macOS-vs-Windows difference (wx_outputpane.cpp, OnRightDown)
//     and therefore has to be driven through the REAL event table.  That needs
//     a real EVT_RIGHT_DOWN, so m_fn_popup_context_menu stands in for the
//     popup — and doubles as the observation that a menu was raised at all.

// One row of the expected menu.  `id` < 0 means "do not care" (the port's own
// items are built on wxID_ANY); kSep marks a separator.
struct MenuRow { int id; const char* label; };
const int kSep = -2;
const int kAny = -1;

// Flatten a built menu to (id, label) rows.  GetItemLabelText strips the
// accelerator suffix and the mnemonic, so "Copy\tCtrl+C" reads as "Copy".
std::vector<std::pair<int, std::string>> FlattenMenu(const wxMenu& menu)
{
    std::vector<std::pair<int, std::string>> out;
    for (size_t i = 0; i < menu.GetMenuItemCount(); ++i) {
        const wxMenuItem* it = menu.FindItemByPosition(i);
        if (!it) continue;
        if (it->IsSeparator()) { out.push_back({kSep, "---"}); continue; }
        const wxString    label = it->GetItemLabelText();
        const wxScopedCharBuffer buf = label.utf8_str();
        out.push_back({it->GetId(), buf.data() ? std::string(buf.data(), buf.length())
                                               : std::string()});
    }
    return out;
}

// A menu item by its (unique) label.
const wxMenuItem* ItemNamed(const wxMenu& menu, const char* label)
{
    for (size_t i = 0; i < menu.GetMenuItemCount(); ++i) {
        const wxMenuItem* it = menu.FindItemByPosition(i);
        if (it && !it->IsSeparator() &&
            it->GetItemLabelText() == wxString::FromUTF8(label))
            return it;
    }
    return nullptr;
}

bool ItemEnabled(const wxMenu& menu, const char* label)
{
    const wxMenuItem* it = ItemNamed(menu, label);
    return it && it->IsEnabled();
}

// Wire the two canonical-command seams onto a pane, recording what is asked
// and what is run.  `uiState` decides the answers; null means "own nothing",
// which is the seeded enabled=true / checked=-1.
struct CanonSpy {
    std::vector<int> dispatched;
    std::vector<int> asked;

    void Attach(COutputPane* pane,
                std::function<bool(int, bool&, int&)> uiState = nullptr)
    {
        pane->m_fn_menu_command = [this](int id) { dispatched.push_back(id); };
        pane->m_fn_menu_ui_state =
            [this, uiState](int id, bool& enabled, int& checked) {
                asked.push_back(id);
                enabled = true;
                checked = -1;
                return uiState ? uiState(id, enabled, checked) : false;
            };
    }
};

// ── 18d. the link cluster: what the items SAY and what they SEND ─────────
//
// Windows' IDR_MXP_MENU is exactly one item per alternative, labelled with the
// MUD's HINT and falling back to the action when there is none, after dropping
// an extra leading hint that belongs to the run's tooltip
// (CMUSHView::OnContextMenu, mushview.cpp:3555-3595).  The port labelled every
// item "Send: <action>" and never read the hints at all, so a MUD that supplied
// "Go north" / "Go south" got "Send: n" / "Send: s".
//
// It also offered "Send All (in order)", which bound the WHOLE pipe-delimited
// string: the receiver splits it, sees more than one alternative and pops up
// the very chooser the item was picked from (wx_world_child.cpp:1526-1547), so
// it never sent anything.  It is gone; these assertions pin that it is, and
// that what each remaining item sends is its OWN action.
void TestLinkContextMenuLabelsAndDispatch()
{
    printf("MXP link menu: hint labels, one item per alternative "
           "(mushview.cpp:3555-3595)\n");

    // A pane carrying one link run, and a recorder for what a picked item sends.
    struct LinkFixture {
        Harness h;
        std::vector<std::string> sent;

        LinkFixture(const char* action, const char* hint)
        {
            SOutputRun run;
            run.text = "click here";        // ASCII: the column maths is plain
            run.fg   = 0x00FFFFFFu;
            run.setLink(action, hint, 0x0400 /*ACTION_SEND*/);
            SOutputLine line;
            line.push_back(run);
            h.pane->AppendLine(std::move(line));
            h.pane->m_fn_link_clicked =
                [this](const std::string& a, int, bool, SRunEncoding) {
                    sent.push_back(a);
                };
        }

        void Build(wxMenu& menu)
        {
            // Inside the run, and inside the first row of the first line.
            h.pane->BuildContextMenu(menu, h.pane->CharWidth() * 2 + 1,
                                     h.pane->LineHeight() / 2);
        }
    };

    // The labels of the items BEFORE the first separator: the link cluster.
    auto clusterLabels = [](const wxMenu& menu) {
        std::vector<std::string> out;
        for (const auto& row : FlattenMenu(menu)) {
            if (row.first == kSep) break;
            out.push_back(row.second);
        }
        return out;
    };
    auto joined = [](const std::vector<std::string>& v) {
        std::string s;
        for (const auto& e : v) { if (!s.empty()) s += " | "; s += e; }
        return s;
    };

    {
        // One hint per alternative: each item is labelled with its own hint.
        LinkFixture f("north|south|east", "Go north|Go south|Go east");
        wxMenu menu;
        f.Build(menu);

        const std::vector<std::string> got = clusterLabels(menu);
        const std::vector<std::string> want = {
            "Go north", "Go south", "Go east", "Copy Action" };
        opCheck(got == want,
                ("each alternative is labelled with the MUD's hint [got: "
                 + joined(got) + "]").c_str());
        opCheck(ItemNamed(menu, "Send All (in order)") == nullptr,
                "...and there is no \"Send All\" item, which could never send");

        // What the FIRST item sends: its own action, not the pipe string that
        // "Send All" used to hand over (and that the receiver answers with a
        // second chooser).
        const wxMenuItem* first = ItemNamed(menu, "Go north");
        opCheck(first != nullptr, "the first alternative is a real item");
        if (first) {
            wxCommandEvent pick(wxEVT_MENU, first->GetId());
            menu.ProcessEvent(pick);
        }
        opCheck(f.sent.size() == 1 && f.sent[0] == "north",
                ("picking the first alternative sends \"north\" [got: "
                 + joined(f.sent) + "]").c_str());

        // The control: a DIFFERENT item must send a different action, so the
        // assertion above cannot be passing on a hardcoded first choice.
        const wxMenuItem* second = ItemNamed(menu, "Go south");
        if (second) {
            wxCommandEvent pick(wxEVT_MENU, second->GetId());
            menu.ProcessEvent(pick);
        }
        opCheck(f.sent.size() == 2 && f.sent[1] == "south",
                "control: the second alternative sends its own action");
    }
    {
        // More hints than actions: the FIRST hint is the run's tooltip and is
        // dropped, so the labels still pair up (mushview.cpp:3556-3558).
        LinkFixture f("n|s", "Exits|Go north|Go south");
        wxMenu menu;
        f.Build(menu);
        const std::vector<std::string> got = clusterLabels(menu);
        const std::vector<std::string> want = { "Go north", "Go south", "Copy Action" };
        opCheck(got == want,
                ("an extra leading hint is the tooltip and is dropped [got: "
                 + joined(got) + "]").c_str());
    }
    {
        // No hints at all: fall back to the action, exactly as Windows does.
        LinkFixture f("n|s", "");
        wxMenu menu;
        f.Build(menu);
        const std::vector<std::string> got = clusterLabels(menu);
        const std::vector<std::string> want = { "n", "s", "Copy Action" };
        opCheck(got == want,
                ("with no hint the label falls back to the action [got: "
                 + joined(got) + "]").c_str());
    }
    {
        // A single alternative: one send item, and no menu is ever raised for a
        // choice that does not exist.  This is also the control that makes the
        // multi-alternative arms above provably the multi branch.
        LinkFixture f("north", "Go north");
        wxMenu menu;
        f.Build(menu);
        const std::vector<std::string> got = clusterLabels(menu);
        const std::vector<std::string> want = { "Go north", "Copy Action" };
        opCheck(got == want,
                ("a single-alternative link offers exactly one send item [got: "
                 + joined(got) + "]").c_str());
    }
    {
        // "ampersands won't come out right" (mushview.cpp:3583).  The label is
        // read back with GetItemLabelText, which undoes the mnemonic escaping,
        // so an unescaped '&' would show up here as a LOST character.
        LinkFixture f("buy", "Buy A&W");
        wxMenu menu;
        f.Build(menu);
        const std::vector<std::string> got = clusterLabels(menu);
        const std::vector<std::string> want = { "Buy A&W", "Copy Action" };
        opCheck(got == want,
                ("an ampersand in a hint survives as text [got: "
                 + joined(got) + "]").c_str());
    }
}

void TestContextMenuShape()
{
    printf("output-pane right-click menu: CG_IDR_POPUP_MUSHVIEW merged in\n");

    // ── (a) the items, in order, with their groups ───────────────────────
    // Written out in full, so ANY reordering, insertion or deletion fails.
    // The canonical ids are spelled here too: the whole point of carrying them
    // on the items is that menu_ui_rules.cpp can be asked about them by name,
    // and a silent switch to wxID_ANY would leave the labels looking right.
    {
        Harness h;
        h.pane->AppendLine(MakeLine("alpha bravo charlie"));
        CanonSpy spy;
        spy.Attach(h.pane);
        h.pane->m_fn_put_in_input = [](const wxString&) {};
        h.pane->m_fn_send_to_mud  = [](const wxString&) {};


        wxMenu menu;
        h.pane->BuildContextMenu(menu, 0, h.pane->LineHeight() / 2);

        static const MenuRow kWant[] = {
            // 2. clipboard
            { kAny,  "Copy" },
            { kAny,  "Copy as HTML" },
            { kAny,  "Select All" },
            { kSep,  "---" },
            // 3. do something with the selection, right now
            { kAny,  "Put in Input Bar" },
            { kAny,  "Send to MUD" },
            { 32275, "Go To URL\xE2\x80\xA6" },
            { 32276, "Send Mail To\xE2\x80\xA6" },
            { kSep,  "---" },
            // 4. turn the selection into a standing rule
            { 33036, "Highlight Word\xE2\x80\xA6" },
            { 33044, "Multi-Line Trigger\xE2\x80\xA6" },
            { kSep,  "---" },
            // 5. search and navigate the scrollback
            { 32248, "Find\xE2\x80\xA6" },
            { 32249, "Find Again" },
            { 32324, "Recall Text\xE2\x80\xA6" },
            { 32318, "Bookmark Selection" },
            { 32319, "Go To Bookmark" },
            { kSep,  "---" },
            // 6. how the output looks and behaves
            { 32201, "Pause Output" },
            { 32118, "Wrap Output" },
            { kAny,  "Zoom In" },
            { kAny,  "Zoom Out" },
            { kAny,  "Reset Zoom" },
            { 32332, "Full Screen Mode" },
            { kSep,  "---" },
            // 7. this world and this session
            { 32117, "Log Session\xE2\x80\xA6" },
            { 32203, "Activity List" },
            { 32333, "Notepad" },
            { 32158, "World Configuration\xE2\x80\xA6" },
            { kSep,  "---" },
            // 8. destructive, alone at the far end
            { kAny,  "Clear Output Buffer\xE2\x80\xA6" },
        };
        const size_t want = sizeof(kWant) / sizeof(kWant[0]);

        const std::vector<std::pair<int, std::string>> got = FlattenMenu(menu);
        opCheckEq((int) got.size(), (int) want, "the menu has exactly the merged item set");

        bool orderOk = (got.size() == want), idsOk = true;
        std::string firstBad;
        for (size_t i = 0; i < want && i < got.size(); ++i) {
            if (got[i].second != kWant[i].label) {
                orderOk = false;
                if (firstBad.empty())
                    firstBad = "position " + std::to_string(i) + ": got \"" +
                               got[i].second + "\", want \"" + kWant[i].label + "\"";
            }
            if (kWant[i].id >= 0 && got[i].first != kWant[i].id) {
                idsOk = false;
                if (firstBad.empty())
                    firstBad = "\"" + got[i].second + "\" carries id " +
                               std::to_string(got[i].first) + ", want " +
                               std::to_string(kWant[i].id);
            }
        }
        opCheck(orderOk, ("items and separators are in the designed order"
                          + (firstBad.empty() ? std::string() : " [" + firstBad + "]")).c_str());
        opCheck(idsOk, "...and the canonical items carry their canonical ids");

        // Every canonical item must have ASKED the rules about itself; an item
        // that quietly skipped the question would still look right above.
        opCheckEq((int) spy.asked.size(), 16,
                  "every canonical item asked menu_ui_rules about its own id");

        // The session-log item is a CHECK item, because that is how the canonical
        // update-UI reports an open log (SetCheck(m_logfile != NULL),
        // doc.cpp:1008).  The port's item used to rename itself instead, which
        // no amount of ON_UPDATE_COMMAND_UI could ever have driven.
        {
            const wxMenuItem* log = ItemNamed(menu, "Log Session\xE2\x80\xA6");
            opCheck(log && log->IsCheckable(),
                    "...and carries a tick for \"a log is open\", not a caption");
        }

        // ── the id map the brief calls out ───────────────────────────────
        // Notepad is the one item whose id is not the command: it carries
        // ID_EDIT_NOTESWORKAREA (32333) and runs the port's 40001.
        {
            wxCommandEvent pick(wxEVT_MENU, 32333);
            menu.ProcessEvent(pick);
            opCheck(spy.dispatched.size() == 1 && spy.dispatched[0] == 40001,
                    "choosing Notepad (id 32333) dispatches 40001, not 32333");
        }
        // ...and a canonical item with no map dispatches its own id, so the
        // assertion above is about the MAP and not about the plumbing.
        {
            wxCommandEvent pick(wxEVT_MENU, 32248);
            menu.ProcessEvent(pick);
            opCheck(spy.dispatched.size() == 2 && spy.dispatched[1] == 32248,
                    "control: Find\xE2\x80\xA6 dispatches its own id");
        }

        // ── ONE session log, reached from both menus ─────────────────────
        // The assertion the unification exists for.  "Log Session…" here and
        // "Start/Stop Logging" in the Connection menu used to be different
        // COMMANDS driving different implementations onto different files --
        // this one a port-local fopen(path, "a") on the world frame, that one
        // the canonical CMUSHclientDoc::OnFileLogsession.  Choosing this item
        // must now dispatch ID_FILE_LOGSESSION, the same id wx_mfc_impl.cpp's
        // ID_CONN_LOG_TOGGLE arm hands to the document, so both entry points
        // run one handler against one m_logfile.  (What the two then WRITE is
        // compared byte for byte in menubar_selftest.cpp, which has the real
        // dispatcher this spy stands in for.)
        //
        // On the DISPATCHED id, not the item's own: Notepad just above proves
        // the two can differ.  Last of the three, so the exact-size checks
        // above keep their teeth.
        {
            wxCommandEvent pick(wxEVT_MENU, 32117);
            menu.ProcessEvent(pick);
            opCheck(spy.dispatched.size() == 3 &&
                    spy.dispatched[2] == 32117 /*ID_FILE_LOGSESSION*/,
                    "Log Session dispatches ID_FILE_LOGSESSION -- the SAME "
                    "command the menu bar's logging item runs");
        }

        // ── no accelerator text lies about what actually runs the command ──
        // FlattenMenu above strips accelerator text (GetItemLabelText), so it
        // cannot see this: on macOS wx renders a label's "\tCtrl+X" suffix as
        // Cmd+X, and this item's only working keyboard binding is the PHYSICAL
        // Control key (RawControlDown, wired in COutputPane::OnKeyDown /
        // CWorldChildFrame::OnInputKeyDown) -- so a "\tCtrl+L" suffix here
        // would promise a shortcut the item does not answer to. Worse: Cmd+L
        // is not even free -- it is the real, natively-bound accelerator for
        // the View menu's "Clear To Mark" (kAccelClearToMark), a much less
        // destructive command. Read the RAW label (GetItemLabel, accelerator
        // intact) rather than the stripped one FlattenMenu uses above.
        {
            const wxMenuItem* clr =
                ItemNamed(menu, "Clear Output Buffer\xE2\x80\xA6");
            opCheck(clr != nullptr, "Clear Output Buffer is in the menu");
            if (clr) {
                const wxString raw = clr->GetItemLabel();
                opCheck(raw.Find('\t') == wxNOT_FOUND,
                        "...and its label carries no accelerator suffix "
                        "(nothing here can make good on one)");
            }
        }
    }

    // ── (a-bis) the link cluster still leads the menu ────────────────────
    // The port's stand-in for Windows' IDR_MXP_MENU arm.  Losing it would
    // regress MXP link handling, and the rest of the menu would still look
    // perfect above.
    {
        Harness h;
        SOutputRun plain;  plain.text = "click ";  plain.fg = 0x00FFFFFFu;
        SOutputRun link;   link.text  = "here";    link.fg  = 0x0000FFFFu;
        link.setLink("https://example.com/", "", 1);
        SOutputLine line;  line.push_back(plain);  line.push_back(link);
        h.pane->AppendLine(std::move(line));

        CanonSpy spy;
        spy.Attach(h.pane);

        const int cw = h.pane->CharWidth();
        wxMenu menu;
        h.pane->BuildContextMenu(menu, cw * 7 + cw / 2, h.pane->LineHeight() / 2);

        const std::vector<std::pair<int, std::string>> got = FlattenMenu(menu);
        opCheck(got.size() > 3 && got[0].second == "Open URL in Browser" &&
                got[1].second == "Copy URL" && got[2].second == "---" &&
                got[3].second == "Copy",
                "a right-click ON a link leads with the link cluster, then Copy");
    }

    // ── (b) enable state: no selection vs selection ──────────────────────
    {
        Harness h;
        h.pane->AppendLine(MakeLine("alpha bravo charlie"));
        CanonSpy spy;
        spy.Attach(h.pane);
        h.pane->m_fn_put_in_input = [](const wxString&) {};
        h.pane->m_fn_send_to_mud  = [](const wxString&) {};

        wxMenu none;
        h.pane->BuildContextMenu(none, 0, h.pane->LineHeight() / 2);
        opCheck(!ItemEnabled(none, "Copy"),          "no selection: Copy is greyed");
        opCheck(!ItemEnabled(none, "Copy as HTML"),  "no selection: Copy as HTML is greyed");
        opCheck(!ItemEnabled(none, "Put in Input Bar"),
                "no selection: Put in Input Bar is greyed");
        opCheck(!ItemEnabled(none, "Send to MUD"),   "no selection: Send to MUD is greyed");
        opCheck(!ItemEnabled(none, "Go To URL\xE2\x80\xA6"),
                "no selection: Go To URL is greyed (OnUpdateNeedSelection)");
        opCheck(!ItemEnabled(none, "Send Mail To\xE2\x80\xA6"),
                "no selection: Send Mail To is greyed (OnUpdateNeedSelection)");
        // ...and the items that do NOT depend on one stay live, so the six
        // above cannot be passing because everything is greyed.
        opCheck(ItemEnabled(none, "Select All"),  "no selection: Select All stays live");
        opCheck(ItemEnabled(none, "Find\xE2\x80\xA6"), "no selection: Find stays live");
        opCheck(ItemEnabled(none, "Highlight Word\xE2\x80\xA6"),
                "no selection: Highlight Word stays live, as on Windows");

        h.pane->SelectAll();
        wxMenu some;
        h.pane->BuildContextMenu(some, 0, h.pane->LineHeight() / 2);
        opCheck(ItemEnabled(some, "Copy"),          "with a selection: Copy is live");
        opCheck(ItemEnabled(some, "Copy as HTML"),  "with a selection: Copy as HTML is live");
        opCheck(ItemEnabled(some, "Put in Input Bar"),
                "with a selection: Put in Input Bar is live");
        opCheck(ItemEnabled(some, "Send to MUD"),   "with a selection: Send to MUD is live");
        opCheck(ItemEnabled(some, "Go To URL\xE2\x80\xA6"),
                "with a selection: Go To URL is live");
        opCheck(ItemEnabled(some, "Send Mail To\xE2\x80\xA6"),
                "with a selection: Send Mail To is live");
    }

    // ── (b-bis) a second stateful item: the rules' own answer ────────────
    // Pause Output and Wrap Output are check items whose tick comes from the
    // canonical ON_UPDATE_COMMAND_UI through wx_canonical_menu_ui_state.  The
    // fake answers for exactly one of them, so a build that ignored `checked`
    // (or applied it to every item) fails.  World Configuration is answered
    // DISABLED, which no other rule here would produce.
    {
        Harness h;
        h.pane->AppendLine(MakeLine("alpha bravo charlie"));
        CanonSpy spy;
        spy.Attach(h.pane, [](int id, bool& enabled, int& checked) {
            if (id == 32201) { checked = 1; return true; }   // Pause Output ticked
            if (id == 32118) { checked = 0; return true; }   // Wrap Output not
            if (id == 32158) { enabled = false; return true; }  // World Configuration
            return false;
        });

        wxMenu menu;
        h.pane->BuildContextMenu(menu, 0, h.pane->LineHeight() / 2);

        const wxMenuItem* pause = ItemNamed(menu, "Pause Output");
        const wxMenuItem* wrap  = ItemNamed(menu, "Wrap Output");
        opCheck(pause && pause->IsCheckable() && pause->IsChecked(),
                "Pause Output is ticked when the rules say so");
        opCheck(wrap && wrap->IsCheckable() && !wrap->IsChecked(),
                "...and Wrap Output is NOT, from the same call");
        opCheck(!ItemEnabled(menu, "World Configuration\xE2\x80\xA6"),
                "World Configuration is greyed when the rules disable it");
        opCheck(ItemEnabled(menu, "Activity List"),
                "...while an id the rules do not own stays live");
    }

    // ── (b-ter) unwired ⇒ greyed ─────────────────────────────────────────
    // A canonical item with nowhere to send its command must not look
    // available.  This is also the state the pane is in before a frame wires
    // it, so "did nothing" cannot masquerade as "worked".
    {
        Harness h;
        h.pane->AppendLine(MakeLine("alpha bravo charlie"));
        wxMenu menu;
        h.pane->BuildContextMenu(menu, 0, h.pane->LineHeight() / 2);
        opCheck(!ItemEnabled(menu, "Find\xE2\x80\xA6"),
                "with no frame wired, the canonical items are greyed");
        opCheck(ItemEnabled(menu, "Select All"),
                "...but the pane's own items still work");
    }
}

void TestRightClickSelectsWord()
{
    printf("right-click selects the word under the pointer (macOS convention)\n");

    // The popup stands in for PopupMenu — see m_fn_popup_context_menu.  It
    // also records THAT a menu was raised, which the hotspot control needs.
    struct RightClick {
        Harness  h;
        int      menus = 0;
        wxString firstItem;

        RightClick() {
            h.pane->AppendLine(MakeLine("alpha bravo charlie"));
            // Line 1 has a RUN of spaces (indices 3..8), so a caret can land
            // with whitespace on both sides -- a single gap cannot, because one
            // of its two sides is always a word character.
            h.pane->AppendLine(MakeLine("gap      here"));
            h.pane->AppendLine(MakeLine(""));            // line 2: nothing at all
            h.pane->m_fn_popup_context_menu = [this](wxMenu& m, const wxPoint&) {
                menus++;
                if (const wxMenuItem* it = m.FindItemByPosition(0))
                    firstItem = it->GetItemLabelText();
            };
        }
        // Aim at the middle of cell `col` on line `line`.  Halfway rounding
        // then puts the caret on the boundary AFTER that cell.
        void At(int col, int line = 0) {
            const int cw = h.pane->CharWidth();
            const int lh = h.pane->LineHeight();
            SendMouse(h.pane, wxEVT_RIGHT_DOWN, cw * col + cw / 2,
                      lh * line + lh / 2);
        }
    };

    // ── (c) no selection → the word under the pointer is selected ────────
    {
        RightClick rc;
        rc.At(8);                       // inside "bravo" (columns 6..10)
        opCheck(rc.h.pane->GetSelectionText() == wxT("bravo"),
                "no selection: right-click selects the word under the pointer");
        opCheckEq(rc.menus, 1, "...and the menu was raised");
    }

    // ── (d) an existing selection is left EXACTLY alone ──────────────────
    // The important half.  Copy acts on the selection, so a right-click that
    // moved it would make the menu's most-used item copy the wrong thing.
    {
        RightClick rc;
        const int cw = rc.h.pane->CharWidth();
        const int lh = rc.h.pane->LineHeight();
        // Double-click "alpha" (columns 0..5) to establish a selection...
        SendMouse(rc.h.pane, wxEVT_LEFT_DOWN,   cw * 2 + cw / 2, lh / 2);
        SendMouse(rc.h.pane, wxEVT_LEFT_DCLICK, cw * 2 + cw / 2, lh / 2);
        SendMouse(rc.h.pane, wxEVT_LEFT_UP,     cw * 2 + cw / 2, lh / 2);
        opCheck(rc.h.pane->GetSelectionText() == wxT("alpha"),
                "setup: a double-click selected \"alpha\"");

        // ...then right-click a DIFFERENT word.
        rc.At(8);                       // "bravo"
        opCheck(rc.h.pane->GetSelectionText() == wxT("alpha"),
                "an existing selection survives a right-click elsewhere");
        opCheckEq(rc.menus, 1, "...and the menu still came up");
    }

    // ── (e) a right-click that resolves to no word ────────────────────
    // The pointer resolves to a caret position and the word grows outwards, so
    // "not on a word" means neither side of that caret is a word character:
    // inside a RUN of spaces, or on an empty line.  Selecting nothing (and
    // clearing nothing) is both NSTextView's answer and this pane's own
    // double-click answer -- see OnRightDown.
    {
        RightClick rc;
        rc.At(5, 1);                    // mid-run of line 1's six spaces
        opCheck(rc.h.pane->GetSelectionText().IsEmpty(),
                "right-click inside a run of spaces selects nothing");
        opCheckEq(rc.menus, 1, "...and the menu is still offered");
        opCheck(rc.firstItem == wxT("Copy"),
                "...led by Copy, greyed, since there is nothing to copy");
    }
    {
        RightClick rc;
        rc.At(0, 2);                    // the empty line
        opCheck(rc.h.pane->GetSelectionText().IsEmpty(),
                "right-click on an empty line selects nothing");
    }
    {
        // ...and it does not CLEAR one either: same branch, selection present.
        RightClick rc;
        rc.h.pane->SelectAll();
        const wxString before = rc.h.pane->GetSelectionText();
        rc.At(5, 1);
        opCheck(!before.IsEmpty() && rc.h.pane->GetSelectionText() == before,
                "a no-word right-click does not clear an existing selection");
    }
    {
        // The two cases that look like "no word" but are NOT, asserted so the
        // three above cannot be passing because the scan gave up too easily --
        // and so the documented rule is pinned rather than merely commented.
        RightClick rc;
        rc.At(5);                       // the single gap between alpha and bravo
        opCheck(rc.h.pane->GetSelectionText() == wxT("bravo"),
                "the single gap beside a word still takes that word");
    }
    {
        RightClick rc;
        rc.At(60);                      // far past the end of the line
        opCheck(rc.h.pane->GetSelectionText() == wxT("charlie"),
                "past the end of the line takes the last word, as NSTextView does");
    }
    {
        // The one-scanner guarantee: a right-click and a double-click at the
        // SAME point must agree.  A second word scanner would drift from this.
        RightClick rc;
        const int cw = rc.h.pane->CharWidth();
        const int lh = rc.h.pane->LineHeight();
        SendMouse(rc.h.pane, wxEVT_LEFT_DOWN,   cw * 5 + cw / 2, lh / 2);
        SendMouse(rc.h.pane, wxEVT_LEFT_DCLICK, cw * 5 + cw / 2, lh / 2);
        SendMouse(rc.h.pane, wxEVT_LEFT_UP,     cw * 5 + cw / 2, lh / 2);
        opCheck(rc.h.pane->GetSelectionText() == wxT("bravo"),
                "...and a double-click at that same point agrees");
    }

    // ── (f) a miniwindow hotspot takes the click; nothing else happens ───
    {
        RightClick rc;
        long gotFlags = 0;
        rc.h.pane->m_fn_miniwindow_mouse_down =
            [&](int, int, long f) { gotFlags = f; return true; };

        rc.At(8);                       // over "bravo", but a hotspot claims it

        opCheckEq((int) gotFlags, 0x20, "the hotspot layer saw MW_MOUSE_RH");
        opCheckEq(rc.menus, 0, "a claimed hotspot raises NO context menu");
        opCheck(rc.h.pane->GetSelectionText().IsEmpty(),
                "...and changes no selection");
    }
    {
        // The control for the arm above: the SAME click with a hotspot layer
        // that declines must do both things, so "nothing happened" cannot pass
        // for "the hotspot won".
        RightClick rc;
        rc.h.pane->m_fn_miniwindow_mouse_down = [](int, int, long) { return false; };
        rc.At(8);
        opCheckEq(rc.menus, 1, "control: a declined hotspot lets the menu through");
        opCheck(rc.h.pane->GetSelectionText() == wxT("bravo"),
                "...and the word is selected as usual");
    }
}


// ── Copy respects the engine's soft wrap (mushview.cpp:4069-4085) ─────────
//
// The port's "logical lines" ARE the engine's CLines, and the engine splits a
// long line at the wrap column into several of them (doc_shared.cpp:2975,
// StartNewLine (false, flags)).  CMUSHView::GetSelection writes ENDLINE only
// after a line whose hard_return is set, so on Windows a wrapped paragraph
// arrives on the clipboard -- and at Send to MUD -- as ONE run.  SelectedText
// used to put a newline between every pair of selected lines, so a 200-char
// paragraph copied as three lines and "Send to MUD" sent three commands.
//
// Both polarities are asserted here: an "always join" is as wrong as the
// "always break" this replaces, and neither passes both arms.
void TestCopyJoinsSoftWrappedLines()
{
    printf("Copy breaks only where the LINE ended (mushview.cpp:4069-4085)\n");

    auto wrapped = [](const char* text, bool hardReturn) {
        SOutputLine l = MakeLine(text);
        l.hardReturn = hardReturn;
        return l;
    };

    {
        // The engine wrapped "alphabravo" into two CLines; "charlie" is a line
        // of its own.
        Harness h;
        h.pane->AppendLine(wrapped("alpha",   false));
        h.pane->AppendLine(wrapped("bravo",   true));
        h.pane->AppendLine(wrapped("charlie", true));
        h.pane->SelectAll();
        const wxString got = h.pane->GetSelectionText();
        opCheck(got == wxT("alphabravo\ncharlie"),
                "a soft wrap copies as one run, a real line end as a break");
        if (got != wxT("alphabravo\ncharlie"))
            printf("    (got \"%s\")\n", (const char*) got.utf8_str());
    }
    {
        // The control: the SAME three lines with every hard_return set must
        // still copy with a break at each one, so "join everything" fails.
        Harness h;
        h.pane->AppendLine(wrapped("alpha",   true));
        h.pane->AppendLine(wrapped("bravo",   true));
        h.pane->AppendLine(wrapped("charlie", true));
        h.pane->SelectAll();
        opCheck(h.pane->GetSelectionText() == wxT("alpha\nbravo\ncharlie"),
                "control: three hard-ended lines still copy as three lines");
    }
    {
        // A partial selection: the last selected line never gets a trailing
        // separator on either platform, and the interior join still follows the
        // flag rather than the selection's shape.
        Harness h;
        h.pane->AppendLine(wrapped("alpha",   false));
        h.pane->AppendLine(wrapped("bravo",   true));
        h.pane->AppendLine(wrapped("charlie", true));
        h.pane->SetSelectionRange(1, 2, 3, 4);   // 1-based: line 1 col 3 -> line 2 col 4
        opCheck(h.pane->GetSelectionText() == wxT("phabra"),
                "a partial selection joins across the wrap and ends bare");
    }
}

// ── Drag auto-scroll repeats while the pointer sits still ────────────────
//
// Windows keeps scrolling AND extending the selection for as long as the button
// is held past the edge of the text area: CMUSHclientApp::OnIdle calls
// CMUSHView::mouse_still_down (MUSHclient.cpp:1148), which scrolls one line per
// idle pass and re-extends from the remembered pointer position
// (mushview.cpp:2550-2634).  The port scrolled exactly once per EVT_MOTION and
// had no repeat at all, so holding the hand still below the pane stopped the
// scroll -- and the selection -- dead, and selecting 200 lines meant jiggling
// the mouse the whole way.
//
// The repeat is driven here by calling AutoScrollTick directly rather than by
// waiting for the wxTimer: this harness exits from wxApp::OnInit, so there is
// no event loop to deliver a timer event, and wxYield is a heap-corruption
// hazard here (CLAUDE.md, 8f12c1c).  The tick IS the behaviour; the timer is
// the binding.
void TestDragAutoScrollRepeats()
{
    printf("drag auto-scroll: the repeat past the edge "
           "(mushview.cpp:2550-2634)\n");

    // Park well above the bottom so there is somewhere to scroll TO -- at the
    // buffer end ScrollLines(1) is a no-op and every assertion below would be
    // vacuous.
    auto parked = [](Harness& h) {
        h.Feed(400);
        h.pane->ScrollToBottom();
        h.pane->Scroll(0, 5);
    };
    auto selEndLine = [](Harness& h) {
        long sl = 0, sc = 0, el = 0, ec = 0;
        return h.pane->GetSelectionInfo(sl, sc, el, ec) ? el : -1;
    };
    // GetScrollPositionY answers in PIXELS; the assertions below are about
    // ROWS, so they divide rather than comparing raw positions.
    auto rowPos = [](Harness& h) {
        const int lh = h.pane->LineHeight();
        return lh > 0 ? (int)(h.pane->GetScrollPositionY() / lh) : -1;
    };
    // Every arm below must leave the pane with no drag, no capture and no armed
    // timer.  wxFrame::Destroy only QUEUES the deletion and this harness runs no
    // event loop, so a pane left mid-drag outlives its own test -- and then
    // meets the one wxYield in this file (PumpDeferred), which processes the
    // queued deletion and any pending timer event together.  A window destroyed
    // while it still holds the mouse capture is the crash that follows, in a
    // later, unrelated test.
    auto endDrag = [](Harness& h, int x, int y) {
        SendMouse(h.pane, wxEVT_LEFT_UP, x, y);
        if (h.pane->HasCapture()) h.pane->ReleaseMouse();
    };

    {
        Harness h;
        parked(h);
        int clientH = 0;
        h.pane->GetClientSize(nullptr, &clientH);
        const int lh = h.pane->LineHeight();

        const int start = rowPos(h);
        opCheck(start > 0 && clientH > 4 * lh,
                "fixture: parked above the end, with room to scroll");

        SendMouse(h.pane, wxEVT_LEFT_DOWN, 10, lh / 2);
        SendMouse(h.pane, wxEVT_MOTION, 20, clientH + 50, /*leftDown*/ true);

        // The motion's OWN scroll, asserted separately: without this a broken
        // tick could hide behind the one scroll the motion already does.
        opCheckEq(rowPos(h) - start, 1,
                  "one motion past the edge scrolls exactly one line");
        const int afterMotion = rowPos(h);
        const long selAfterMotion = selEndLine(h);
        opCheck(selAfterMotion > 0, "...and the drag really selected something");

        // Now the mouse does NOTHING.  Three repeats, three lines, and three
        // more lines of selection: the exact delta, not merely "it changed".
        for (int i = 1; i <= 3; ++i) {
            const bool again = h.pane->AutoScrollTick();
            opCheck(again, "the tick asks to be called again while past the edge");
            opCheckEq(rowPos(h) - afterMotion, i,
                      "...and each repeat scrolls exactly one more line");
            // i + 1, not i: OnMotion extends the selection to the pointer
            // BEFORE it scrolls, so the first repeat also catches up the line
            // the motion's own scroll had already moved past.  Windows has the
            // same shape -- extend_selection runs before doLinedown in
            // OnMouseMove, and mouse_still_down re-extends afterwards.
            opCheckEq((int)(selEndLine(h) - selAfterMotion), i + 1,
                      "...and drags the selection out with it");
        }

        // The stop: after the release the tick must do nothing at all.  This is
        // the runaway-timer guard -- a repeat that outlives its drag scrolls
        // the pane for ever.
        endDrag(h, 20, clientH + 50);
        const int atRelease = rowPos(h);
        opCheck(!h.pane->AutoScrollTick(),
                "after the release the tick refuses");
        opCheckEq(rowPos(h) - atRelease, 0, "...and moves nothing");
    }
    {
        // The pointer back INSIDE the text: Windows' `else return 0; // don't
        // want any more idle calls` (mushview.cpp:2604).  Without this arm the
        // repeat would never stop until the button came up.
        Harness h;
        parked(h);
        int clientH = 0;
        h.pane->GetClientSize(nullptr, &clientH);
        const int lh = h.pane->LineHeight();

        SendMouse(h.pane, wxEVT_LEFT_DOWN, 10, lh / 2);
        SendMouse(h.pane, wxEVT_MOTION, 20, clientH / 2, /*leftDown*/ true);
        const int inside = rowPos(h);
        opCheck(!h.pane->AutoScrollTick(),
                "a tick with the pointer inside the text refuses");
        opCheckEq(rowPos(h) - inside, 0, "...and moves nothing");
        endDrag(h, 20, clientH / 2);
    }
    {
        // Upwards, which is the other half of the same handler: y < 10 scrolls
        // BACK, so a fixture parked at the top could not tell the two apart.
        Harness h;
        parked(h);
        h.pane->Scroll(0, 40);
        const int lh = h.pane->LineHeight();
        const int start = rowPos(h);

        SendMouse(h.pane, wxEVT_LEFT_DOWN, 10, 5 * lh);
        SendMouse(h.pane, wxEVT_MOTION, 20, 2, /*leftDown*/ true);
        opCheckEq(rowPos(h) - start, -1,
                  "one motion past the TOP edge scrolls one line back");
        const int afterMotion = rowPos(h);
        opCheck(h.pane->AutoScrollTick(), "...and the repeat runs upwards too");
        opCheckEq(rowPos(h) - afterMotion, -1,
                  "...one line per repeat, the other way");
        // End the drag before the fixture goes away.  wxFrame::Destroy only
        // QUEUES the deletion, and this harness runs no event loop, so a pane
        // left mid-drag survives with its auto-scroll timer still armed -- and
        // then fires it inside the one wxYield this file does have
        // (PumpDeferred), against a window that is by then being destroyed.
        endDrag(h, 20, 2);
    }
    {
        // Capture loss ends the drag exactly as a release does -- the second
        // half of the runaway-timer guard, since a capture snatched away by a
        // modal never produces a LEFT_UP.
        Harness h;
        parked(h);
        int clientH = 0;
        h.pane->GetClientSize(nullptr, &clientH);
        const int lh = h.pane->LineHeight();

        SendMouse(h.pane, wxEVT_LEFT_DOWN, 10, lh / 2);
        SendMouse(h.pane, wxEVT_MOTION, 20, clientH + 50, /*leftDown*/ true);
        opCheck(h.pane->AutoScrollTick(), "control: the repeat is running");

        wxMouseCaptureLostEvent lost(h.pane->GetId());
        lost.SetEventObject(h.pane);
        h.pane->ProcessWindowEvent(lost);

        const int atLoss = rowPos(h);
        opCheck(!h.pane->AutoScrollTick(),
                "losing the capture stops the repeat");
        opCheckEq(rowPos(h) - atLoss, 0, "...and it moves nothing afterwards");
        endDrag(h, 20, clientH + 50);
    }
}

int RunOutputPaneSelfTest()
{
    printf("OUTPUTPANE-SELFTEST: driving the real COutputPane\n");

    // An outer net over the whole harness.  The three tests that mean to touch
    // the clipboard guard themselves, but a mouse release with the auto-copy
    // option on writes it from anywhere, so a future test could reach it
    // without meaning to -- and this runs on a real machine belonging to a real
    // person whose clipboard is none of its business.
    const ClipboardGuard clipAll;

    TestSelectionFreezesOutput();
    TestWantsSelectAll();
    TestDoubleClickAutoCopy();
    TestClearOutputDropsPendingLine();
    TestKeepPauseAtBottom();
    TestFrozenVersusAtBufferEnd();
    TestAutoPauseOffLeavesTheFreezeAlone();
    TestEnsureSelectionVisible();
    TestPlainScrollsObeyAutoPause();
    TestTextRectangleOffsetForm();
    TestLeaveWindowCancelsMouseOver();
    TestDragAutoScrollRepeats();
    TestFindNext();
    TestFindVisitsEveryOccurrenceOnALine();
    TestCopyJoinsSoftWrappedLines();
    TestReleaseDoesNotMaterialiseUnlessAsked();
    TestSearchHighlightOffsets();
    TestMeasuredMetrics();
    TestRunCharLenMatchesTheDecode();
    TestCharCountAvoidsTheDecode();
    TestAdoptTextRect();
    TestWheelActionOrdering();
    TestFontSizeClamp();
    TestRunAtCharColumn();
    TestPluginDrawDefersAppends();
    TestPullSeamReads();
    TestPullSeamDetectsURLs();
    TestLinkEncodingTravelsWithTheAction();
    TestPendingRowUsesTheRunPipeline();
    TestMiddleClickIsInert();
    TestContextMenuShape();
    TestLinkContextMenuLabelsAndDispatch();
    TestRightClickSelectsWord();

    printf("OUTPUTPANE-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

#endif // !_WIN32
