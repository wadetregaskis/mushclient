// tests/test_pal_worldview.cpp
// First PAL prototype + characterization test.
//
// Proves pal::IWorldView is a real, implementable interface by wiring a
// recording implementation (the "headless test frontend") to the engine's
// output path, pinning the DisplayMsg styled-output behaviour as a golden —
// the safety net for the P2 seam migration (now typed-only end to end).

#include "mc_test.h"
// B-3 atomic flip: real headers (the headless stub is gone; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "pal/iworldview.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// ── the headless test frontend ─────────────────────────────────────────────
#include "fake_worldview.h"

// ── helpers ─────────────────────────────────────────────────────────────────
static void make_doc(CMUSHclientDoc& doc)
{
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_iFlags        = COLOUR_ANSI;
    doc.m_iForeColour   = WHITE;
    doc.m_iBackColour   = BLACK;
    // display step 7: the ctor seeds the in-list first line + its style and
    // the line-position index — no hand-rolled current line here any more
}

static std::string line_text(const SOutputLine& l)
{
    std::string s;
    for (const auto& r : l) s += r.text;
    return s;
}

// committed (non-pending) lines only — the frontend appends these; pending lines
// are the live typing-indicator row.
static std::vector<SOutputLine> committed(const FakeWorldView& v)
{
    std::vector<SOutputLine> out;
    for (const auto& l : v.lines) if (!l.pending) out.push_back(l);
    return out;
}

static void feed(CMUSHclientDoc& doc, const char* bytes)
{
    doc.DisplayMsg(bytes, (int)strlen(bytes), 0);
    // display step 7: the real engine builds m_LineList and a drain emits —
    // feed-then-drain is the contract (the GUI drains after each packet).
    doc.UpdateAllViews(nullptr);
}

// ── tests ─────────────────────────────────────────────────────────────────
TEST(PalWorldView, InterfaceRecordsStyledOutput)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;   // drive the engine's output through the typed seam

    feed(doc, "\x1b[1;32mgreen\x1b[0m normal\n");

    auto lines = committed(view);
    ASSERT_EQ(1u, (unsigned)lines.size());
    EXPECT_STREQ("green normal", line_text(lines[0]).c_str());
    ASSERT_TRUE(lines[0].size() >= 2);      // bold-green run, then normal run
    EXPECT_TRUE(lines[0].front().bold);     // first run is bold
    // style genuinely changes between the runs (colour and/or weight)
    EXPECT_TRUE(lines[0].front().fg   != lines[0].back().fg ||
                lines[0].front().bold != lines[0].back().bold);
}

TEST(PalWorldView, OneCommittedLinePerNewline)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;   // drive the engine's output through the typed seam

    feed(doc, "alpha\nbeta\ngamma\n");

    auto lines = committed(view);
    ASSERT_EQ(3u, (unsigned)lines.size());
    EXPECT_STREQ("alpha", line_text(lines[0]).c_str());
    EXPECT_STREQ("beta",  line_text(lines[1]).c_str());
    EXPECT_STREQ("gamma", line_text(lines[2]).c_str());
}

// The state callbacks drive the typed seam too, not just line output: the
// engine's Pause / SetTitle / SetStatus call sites now go through these helpers.
// (Pause()/SetTitle() themselves are GUI-only method wrappers not in the headless
// build; the EmitPaused/EmitTitle/EmitStatus routing they call IS the migration.)
TEST(PalWorldView, StateHelpersDriveTheView)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    doc.EmitPaused(true);
    EXPECT_TRUE(view.paused);
    doc.EmitPaused(false);
    EXPECT_TRUE(!view.paused);

    doc.EmitTitle("HP: 42/50");
    EXPECT_STREQ("HP: 42/50", view.title.c_str());

    doc.EmitStatus("connected");
    EXPECT_STREQ("connected", view.status.c_str());
}

// With no view attached the state helpers are inert (typed-only seam; the
// old m_fn_* fallbacks are gone) and the pulls report empty/zero.
TEST(PalWorldView, StateHelpersNullHostIsInert)
{
    CMUSHclientDoc doc; make_doc(doc);

    doc.EmitPaused(true);
    doc.EmitTitle("t1");
    doc.EmitStatus("s1");
    doc.EmitClearOutput();
    doc.EmitBackgroundColour(0x123456);
    doc.EmitMaxLines(99);
    doc.EmitRepaint();

    EXPECT_TRUE(!doc.HaveOutputSink());
    EXPECT_TRUE(!doc.HaveCommandSink());
    EXPECT_TRUE(!doc.HaveScrollSink());
    EXPECT_STREQ("", doc.GetCommandText().c_str());
    EXPECT_TRUE(doc.GetScrollPos() == 0);
    long sl = 9, sc = 9, el = 9, ec = 9;
    doc.GetOutputSelection(sl, sc, el, ec);
    EXPECT_TRUE(sl == 0 && sc == 0 && el == 0 && ec == 0);
}

// Clear / background colour / max-lines / repaint drive the view via their
// Emit* helpers as well.
TEST(PalWorldView, CanvasHelpersDriveTheView)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    doc.EmitBackgroundColour(0x00204060);
    EXPECT_TRUE(view.bg == 0x00204060u);

    doc.EmitMaxLines(1234);
    EXPECT_TRUE(view.maxLines == 1234);

    doc.EmitRepaint();
    doc.EmitRepaint();
    EXPECT_TRUE(view.repaints == 2);

    feed(doc, "seed line\n");
    ASSERT_TRUE(!committed(view).empty());
    doc.EmitClearOutput();
    EXPECT_TRUE(view.cleared);
    EXPECT_TRUE(view.lines.empty());
}

// The engine reads frontend state back through the view (pulls): command text,
// scroll position, and the output selection.
TEST(PalWorldView, PullsReadFromTheView)
{
    FakeWorldView view;
    view.cmd    = "look north";
    view.scroll = 42;
    view.sel    = { 3, 5, 7, 9 };
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    EXPECT_STREQ("look north", doc.GetCommandText().c_str());
    EXPECT_TRUE(doc.GetScrollPos() == 42);
    long sl = 0, sc = 0, el = 0, ec = 0;
    doc.GetOutputSelection(sl, sc, el, ec);
    EXPECT_TRUE(sl == 3 && sc == 5 && el == 7 && ec == 9);
}

// The command-input family drives the view through its typed-only helpers
// (no m_fn_* fallback): set/push/history/selection/font.
TEST(PalWorldView, CommandFamilyDrivesTheView)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    // SetCommand honours the only-if-empty contract.
    EXPECT_TRUE(doc.SetCommandText("kill rat"));
    EXPECT_TRUE(!doc.SetCommandText("flee"));            // bar not empty
    EXPECT_STREQ("kill rat", doc.GetCommandText().c_str());

    // PushCommand saves to history and clears the bar.
    EXPECT_STREQ("kill rat", doc.PushCommandText().c_str());
    EXPECT_STREQ("", doc.GetCommandText().c_str());

    // History: duplicates suppressed, newest first on read-back.
    doc.AddCommandToHistory("north");
    doc.AddCommandToHistory("north");                    // suppressed
    doc.AddCommandToHistory("south");
    auto hist = doc.GetCommandListEntries(0);
    ASSERT_EQ(3u, (unsigned)hist.size());
    EXPECT_STREQ("south",    hist[0].c_str());
    EXPECT_STREQ("north",    hist[1].c_str());
    EXPECT_STREQ("kill rat", hist[2].c_str());
    ASSERT_EQ(1u, (unsigned)doc.GetCommandListEntries(1).size());

    doc.DeleteCommandHistoryEntries();
    EXPECT_TRUE(doc.GetCommandListEntries(0).empty());

    // Selection: CEdit::SetSel(First - 1, Last) semantics.
    doc.SetCommandText("look");
    doc.SetCommandSelectionRange(2, 3);
    long s = 0, e = 0;
    doc.GetCommandSelection(s, e);
    EXPECT_TRUE(s == 1 && e == 3);
    doc.SelectCommandText();
    EXPECT_TRUE(view.selectedAll);

    doc.EmitInputFont("Menlo", 14, true, false);
    EXPECT_STREQ("Menlo", view.fontFace.c_str());
    EXPECT_TRUE(view.fontPts == 14 && view.fontBold && !view.fontItalic);
}

// With no view attached the command helpers are inert and report "no host":
// SetCommandText false, empty pulls, no crashes.
TEST(PalWorldView, CommandFamilyNullHostIsInert)
{
    CMUSHclientDoc doc; make_doc(doc);

    EXPECT_TRUE(!doc.SetCommandText("kill rat"));
    EXPECT_STREQ("", doc.PushCommandText().c_str());
    EXPECT_TRUE(doc.GetCommandListEntries(0).empty());
    doc.AddCommandToHistory("north");
    doc.DeleteCommandHistoryEntries();
    doc.SelectCommandText();
    doc.SetCommandSelectionRange(1, 2);
    long s = 99, e = 99;
    doc.GetCommandSelection(s, e);
    EXPECT_TRUE(s == 0 && e == 0);
    doc.EmitInputFont("Menlo", 14, true, false);
}

// The output-pane family drives the view through its typed-only helpers.
// Note() routes through the REAL Tell → DisplayMsg → m_LineList (display
// steps 6+7).  The ctor seeds the in-list first line and the line-position
// index, so notes are IMMEDIATE (no deferral), and they reach the view as
// appendLine content when the drain runs.  IWorldView::note() never fires.
TEST(PalWorldView, OutputFamilyDrivesTheView)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    doc.Note("a wild note appears");
    EXPECT_EQ(0u, (unsigned)view.notes.size());          // note() seam is dormant
    EXPECT_EQ(0u, (unsigned)doc.m_OutstandingLines.size());  // no deferral post-flip
    doc.UpdateAllViews(nullptr);                         // drain
    bool noteArrived = false;
    for (const auto& line : view.lines)
        for (const auto& run : line)
            if (run.text.find("a wild note appears") != std::string::npos)
                noteArrived = true;
    EXPECT_TRUE(noteArrived);
    EXPECT_EQ(0u, (unsigned)view.notes.size());          // still nothing via note()

    // the drain above already opened/closed one batch while emitting
    int b0 = view.batchBegins, e0 = view.batchEnds;
    doc.BeginOutputBatch();
    doc.EndOutputBatch();
    EXPECT_TRUE(view.batchBegins == b0 + 1 && view.batchEnds == e0 + 1);

    doc.DeleteOutputLines(7);
    EXPECT_TRUE(view.deletedLines == 7);

    doc.EmitScroll(-1);
    EXPECT_TRUE(view.scrolledTo == -1);

    doc.SetOutputSelectionRange(2, 5, 3, 9);
    EXPECT_TRUE(view.selSL == 2 && view.selEL == 5 &&
                view.selSC == 3 && view.selEC == 9);

    doc.EmitOutputFont("Menlo", 13, true, false);
    EXPECT_STREQ("Menlo", view.outFontFace.c_str());
    EXPECT_TRUE(view.outFontPts == 13 && view.outFontBold && !view.outFontItalic);

    doc.EmitTextRectChanged();
    EXPECT_TRUE(view.textRectChanges == 1);

    int x = 0, y = 0, w = 0, h = 0;
    EXPECT_TRUE(doc.GetOutputRect(x, y, w, h));
    EXPECT_TRUE(x == 10 && y == 20 && w == 640 && h == 480);
}

// With no view: GetOutputRect reports false and leaves its outputs untouched
// (callers keep their own defaults), and the rest are inert.
TEST(PalWorldView, OutputFamilyNullHostIsInert)
{
    CMUSHclientDoc doc; make_doc(doc);

    int x = 1, y = 2, w = 3, h = 4;
    EXPECT_TRUE(!doc.GetOutputRect(x, y, w, h));
    EXPECT_TRUE(x == 1 && y == 2 && w == 3 && h == 4);

    doc.BeginOutputBatch();
    doc.EndOutputBatch();
    doc.DeleteOutputLines(7);
    doc.EmitScroll(5);
    doc.SetOutputSelectionRange(1, 2, 3, 4);
    doc.EmitOutputFont("Menlo", 13, false, false);
    doc.EmitTextRectChanged();
}

// The world/main window family drives the view through its typed-only helpers.
TEST(PalWorldView, WindowFamilyDrivesTheView)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    doc.EmitMainTitle("MUSHclient - test");
    EXPECT_STREQ("MUSHclient - test", view.mainTitle.c_str());

    doc.MoveWorldWindowRect(10, 20, 300, 200);
    EXPECT_TRUE(view.mwX == 10 && view.mwY == 20 &&
                view.mwW == 300 && view.mwH == 200);

    doc.EmitWindowStatus(2);
    EXPECT_TRUE(view.windowStatus == 2);

    doc.ActivateClientWindow();
    EXPECT_TRUE(view.activations == 1);

    EXPECT_TRUE(!doc.IsClientFullscreen());
    view.fullscreen = true;
    EXPECT_TRUE(doc.IsClientFullscreen());

    EXPECT_TRUE(doc.SaveWorldViaView());
    EXPECT_TRUE(view.saves == 1);

    int x = 0, y = 0, w = 0, h = 0;
    EXPECT_TRUE(doc.GetMainRect(x, y, w, h));
    EXPECT_TRUE(x == 5 && y == 6 && w == 1024 && h == 768);
}

// With no view: main-rect reports false with outputs untouched, Save is
// trivially successful (the old null-callback path), the rest inert.
TEST(PalWorldView, WindowFamilyNullHostIsInert)
{
    CMUSHclientDoc doc; make_doc(doc);

    doc.EmitMainTitle("t");
    doc.MoveWorldWindowRect(1, 2, 3, 4);
    doc.EmitWindowStatus(1);
    doc.ActivateClientWindow();
    EXPECT_TRUE(!doc.IsClientFullscreen());
    EXPECT_TRUE(doc.SaveWorldViaView());

    int x = 9, y = 9, w = 9, h = 9;
    EXPECT_TRUE(!doc.GetMainRect(x, y, w, h));
    EXPECT_TRUE(x == 9 && y == 9 && w == 9 && h == 9);
}

// The info bar family drives the view through its typed-only helpers.
TEST(PalWorldView, InfobarFamilyDrivesTheView)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    doc.EmitInfobarShow(true);
    EXPECT_TRUE(view.infobarVisible);

    doc.EmitInfobarText("HP: ");
    doc.EmitInfobarText("42");
    EXPECT_STREQ("HP: 42", view.infobarBuf.c_str());

    doc.EmitInfobarColour(0x0000FF);       // COLORREF red
    doc.EmitInfobarBackground(0xFFFFFF);
    EXPECT_TRUE(view.infobarFg == 0x0000FFu && view.infobarBg == 0xFFFFFFu);

    doc.EmitInfobarFont("Menlo", 12, 1 | 4);   // bold + underline
    EXPECT_STREQ("Menlo", view.infobarFace.c_str());
    EXPECT_TRUE(view.infobarPts == 12 && view.infobarStyle == 5);

    doc.EmitInfobarClear();
    EXPECT_TRUE(view.infobarClears == 1 && view.infobarBuf.empty());

    view.infobarSz = { 320, 24 };
    int w = 0, h = 0;
    doc.GetInfobarSize(w, h);
    EXPECT_TRUE(w == 320 && h == 24);
}

// With no view every infobar helper is inert; size reads back (0,0).
TEST(PalWorldView, InfobarFamilyNullHostIsInert)
{
    CMUSHclientDoc doc; make_doc(doc);

    doc.EmitInfobarShow(true);
    doc.EmitInfobarClear();
    doc.EmitInfobarText("x");
    doc.EmitInfobarColour(1);
    doc.EmitInfobarBackground(2);
    doc.EmitInfobarFont("f", 10, 3);
    int w = 7, h = 7;
    doc.GetInfobarSize(w, h);
    EXPECT_TRUE(w == 0 && h == 0);
}

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────

// doc_display_shared.cpp: "MUDs often send a prompt without a newline (e.g.
// '> ') — we emit it as a pending SOutputLine so the output pane can display
// it as a dim trailing row even before the '\n' arrives."
TEST(PalWorldView, PromptWithoutNewlineEmittedAsPendingRow)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    feed(doc, "> ");                              // prompt, no newline
    EXPECT_EQ(0u, (unsigned)committed(view).size());   // not committed
    ASSERT_TRUE(!view.lines.empty());
    const SOutputLine& last = view.lines.back();
    EXPECT_TRUE(last.pending);                    // emitted as pending row
    EXPECT_STREQ("> ", line_text(last).c_str());

    feed(doc, "\n");                              // now the newline arrives
    auto done = committed(view);
    ASSERT_EQ(1u, (unsigned)done.size());
    EXPECT_STREQ("> ", line_text(done[0]).c_str());   // same text, committed
}

// doc_display_shared.cpp: "MUSHclient maps BLINK → italic [run.italic =
// (pStyle->iFlags & BLINK) != 0]" — ANSI SGR 5 comes out the seam as italic.
TEST(PalWorldView, BlinkFlagEmitsItalicRun)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    feed(doc, "\033[5mblinky\033[0m plain\n");

    auto lines = committed(view);
    ASSERT_EQ(1u, (unsigned)lines.size());
    ASSERT_TRUE(lines[0].size() >= 2);
    EXPECT_TRUE(lines[0].front().italic);         // BLINK run → italic
    EXPECT_FALSE(lines[0].back().italic);         // plain run stays plain
}

// doc_shared.cpp RemoveChunk: "Keep the GUI emission counter in sync with
// lines removed from the head" — across a buffer eviction the drain neither
// re-emits already-forwarded lines nor skips new ones.
TEST(PalWorldView, EmissionCursorStaysAlignedAcrossBufferEviction)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;
    doc.m_maxlines = 200;                 // low ceiling → eviction kicks in
    doc.AllocateLinePositions();

    char buf[32];
    for (int i = 1; i <= 350; i++) {      // several RemoveChunk evictions
        snprintf(buf, sizeof buf, "line %d\n", i);
        feed(doc, buf);
    }

    // the committed sequence must be exactly line 1..350: contiguous, no
    // duplicates, none skipped
    auto lines = committed(view);
    ASSERT_EQ(350u, (unsigned)lines.size());
    for (int i = 1; i <= 350; i++) {
        snprintf(buf, sizeof buf, "line %d", i);
        EXPECT_STREQ(buf, line_text(lines[(size_t)(i - 1)]).c_str());
        if (line_text(lines[(size_t)(i - 1)]) != buf) break;   // don't spam
    }
    // and the buffer itself really was evicted (the point of the test)
    EXPECT_LE((long)doc.m_LineList.GetCount(), 200L);
}


// doc_display_shared.cpp UpdateAllViews (~68-77): a committed line flagged
// HORIZ_RULE is emitted as a single sentinel SOutputRun (horiz_rule=true, no
// text) rather than as styled text.  world.NoteHr sets the flag, commits the
// line via StartNewLine, and drains through the seam (ProcessPreviousLine
// returns false for HORIZ_RULE, so the line commits and m_pCurrentLine moves on)
// -- with a view attached the sentinel must reach it.  The existing NoteHr/
// hyperlink display tests attach NO view, so this branch was never exercised.
TEST(PalWorldView, NoteHrEmitsHorizRuleSentinelRun)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;   // typed seam attached

    doc.NoteHr();               // flags the line HORIZ_RULE, commits it, drains

    bool hrFound = false;
    for (const auto& line : view.lines)
        for (const auto& run : line)
            if (run.horiz_rule) {
                hrFound = true;
                EXPECT_STREQ("", run.text.c_str());   // sentinel carries no text
            }
    EXPECT_TRUE(hrFound);
}

// doc_display_shared.cpp UpdateAllViews (~95-99): for a COMMITTED (non-current)
// line, a style carrying a CAction copies pAction->m_strAction/m_strHint into
// SOutputRun.action/hint.  Hyperlink() builds a send-link run ("click", action
// "do this", hint "a hint" -- same values pinned by test_display's
// HyperlinkSendStampsActionAndUnderline) on the current note line; a MUD newline
// forces the note line closed (COMMENT->MUD transition commits it before the
// '\n' char is added, so the text stays exactly "click") and the committed run
// must carry the action + hint strings.
TEST(PalWorldView, CommittedLineCarriesHyperlinkActionAndHint)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    doc.Hyperlink("do this", "click", "a hint", "", "", FALSE);  // send-link on current line
    feed(doc, "\n");    // MUD newline commits the note line, then drains

    bool found = false;
    for (const auto& line : committed(view))
        for (const auto& run : line)
            if (run.text == "click") {
                found = true;
                EXPECT_STREQ("do this", run.action.c_str());
                EXPECT_STREQ("a hint",  run.hint.c_str());
            }
    EXPECT_TRUE(found);
}

// doc_display_shared.cpp UpdateAllViews (~135-138): the in-progress (pending)
// line copies a style's CAction m_strAction/m_strHint into the run identically.
// Hyperlink() leaves the send-link on the CURRENT line (it does not close it),
// so with no newline the committed-line loop breaks immediately and the drain
// emits the link through the pending arm -- carrying the action + hint.
TEST(PalWorldView, PendingLineCarriesHyperlinkActionAndHint)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    doc.Hyperlink("do this", "click", "a hint", "", "", FALSE);  // stays on the current line
    doc.UpdateAllViews(nullptr);                                 // drain (no newline)

    ASSERT_TRUE(!view.lines.empty());
    const SOutputLine& last = view.lines.back();
    EXPECT_TRUE(last.pending);         // emitted through the pending arm
    bool found = false;
    for (const auto& run : last)
        if (run.text == "click") {
            found = true;
            EXPECT_STREQ("do this", run.action.c_str());
            EXPECT_STREQ("a hint",  run.hint.c_str());
        }
    EXPECT_TRUE(found);
}

MC_TEST_MAIN()
