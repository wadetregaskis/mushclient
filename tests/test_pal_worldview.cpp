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
#include "pal/scroll_geometry.h"
#include "pal/style_colour.h"
#include "pal/text_rect.h"
#include "pal/image_placement.h"
#include "platform/activity_refresh.h"

#include <algorithm>
#include <chrono>
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
    // "Show bold" (show_bold) defaults to FALSE, and it gates whether the ANSI
    // HILITE attribute is rendered as a bold FACE.  With the default off, ANSI
    // bold must show up as the bright colour only — matching Windows, where
    // ChangeFont gives every style the world's base weight unless bShowBold is
    // set (doc.cpp: `bShowBold ? ((i & HILITE) ? FW_BOLD : FW_NORMAL) : nWeight`).
    EXPECT_FALSE(lines[0].front().bold);
    // …but the two runs must still be visibly distinct, via the bold palette.
    EXPECT_TRUE(lines[0].front().fg != lines[0].back().fg);
}

// The other half of the same gate: turning "Show bold" on must let HILITE reach
// the view as a bold run.  Together these pin both directions of the option.
TEST(PalWorldView, ShowBoldOptionGatesBoldRuns)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bShowBold  = 1;               // World Properties > Appearance > Output
    doc.m_pWorldView = &view;

    feed(doc, "\x1b[1;32mgreen\x1b[0m normal\n");

    auto lines = committed(view);
    ASSERT_EQ(1u, (unsigned)lines.size());
    ASSERT_TRUE(lines[0].size() >= 2);
    EXPECT_TRUE(lines[0].front().bold);    // HILITE now emitted as bold
    EXPECT_FALSE(lines[0].back().bold);    // the normal run is unaffected
}

// Underline defaults ON, so clearing it must suppress the attribute.
TEST(PalWorldView, ShowUnderlineOptionGatesUnderlineRuns)
{
    {
        FakeWorldView view;
        CMUSHclientDoc doc; make_doc(doc);
        doc.m_pWorldView = &view;
        feed(doc, "\x1b[4munder\x1b[0m\n");
        auto lines = committed(view);
        ASSERT_EQ(1u, (unsigned)lines.size());
        ASSERT_TRUE(lines[0].size() >= 1);
        EXPECT_TRUE(lines[0].front().underline);   // default show_underline = true
    }
    {
        FakeWorldView view;
        CMUSHclientDoc doc; make_doc(doc);
        doc.m_bShowUnderline = 0;                  // user cleared the checkbox
        doc.m_pWorldView = &view;
        feed(doc, "\x1b[4munder\x1b[0m\n");
        auto lines = committed(view);
        ASSERT_EQ(1u, (unsigned)lines.size());
        ASSERT_TRUE(lines[0].size() >= 1);
        EXPECT_FALSE(lines[0].front().underline);  // …and it is suppressed
    }
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

// world.SetInputFont sets the INPUT font.  It used to fork on the port and drive
// EmitOutputFont instead, on the premise that the two shared one face -- so the
// script API restyled the MUD output, never touched the command bar, and (since
// the port's setOutputFont also writes the doc's output-font members) a save
// afterwards persisted an output font the user never chose.
//
// The two halves are asserted TOGETHER: "the input font changed" alone would
// still pass with the fork in place if some other path had also set it, and "the
// output font is untouched" alone would pass if the call did nothing at all.
TEST(PalWorldView, SetInputFontSetsTheInputFontAndOnlyThat)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    // Seed the output font with something recognisable, so a stray write shows.
    doc.EmitOutputFont("Courier", 13, false, false);
    EXPECT_STREQ("Courier", view.outFontFace.c_str());
    ASSERT_EQ(13, view.outFontPts);

    doc.SetInputFont("Menlo", 17, 700, TRUE);

    EXPECT_STREQ("Menlo", view.fontFace.c_str());   // ...the command bar
    EXPECT_EQ(17, view.fontPts);
    EXPECT_TRUE(view.fontBold);                     // 700 >= FW_SEMIBOLD
    EXPECT_TRUE(view.fontItalic);

    EXPECT_STREQ("Courier", view.outFontFace.c_str());   // ...and NOT the output
    EXPECT_EQ(13, view.outFontPts);

    // Reaching view.fontFace at all is what proves the canonical body ran:
    // EmitInputFont has exactly ONE caller in the tree, ChangeInputFont
    // (doc.cpp:1284).  (m_InputFontHeight/Width are deliberately zero in this
    // build -- gdi_headless.cpp zeroes the TEXTMETRIC on purpose -- so they are
    // not an oracle here.)

    doc.m_pWorldView = nullptr;
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

// Both real IWorldView adapters (CWorldChildFrame::HistoryLimit and the
// Windows pal::win_worldview equivalent) evict the OLDEST entry once history
// exceeds the world's "Lines in command history" setting.  Before this test,
// FakeWorldView's history grew unbounded, so that eviction had no
// failing-capable test through the seam: a stub that forgot to trim would
// still pass every existing assertion.
TEST(PalWorldView, CommandHistoryEvictsOldestPastTheCap)
{
    FakeWorldView view;
    view.historyCap = 3;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    doc.AddCommandToHistory("north");
    doc.AddCommandToHistory("south");
    doc.AddCommandToHistory("east");
    doc.AddCommandToHistory("west");   // pushes the cap; "north" must go

    auto hist = doc.GetCommandListEntries(0);   // newest first
    ASSERT_EQ(3u, (unsigned)hist.size());
    EXPECT_STREQ("west",  hist[0].c_str());
    EXPECT_STREQ("east",  hist[1].c_str());
    EXPECT_STREQ("south", hist[2].c_str());
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


// ── the world window's title text ───────────────────────────────────────────
//
// One expression for "what should this window's title read", so the frame's
// connection-phase tick and IWorldView::setTitle cannot disagree.  They did:
// the tick rebuilt "<world name> (<phase>)" from scratch, so a title a plugin
// had set with world.SetTitle was destroyed by the next connect, disconnect or
// close and never came back.  Windows suppresses the status suffix entirely
// while a custom title is set (CMUSHView::FixupTitle, mushview.cpp).
TEST(PalWorldView, WindowTitleDerivedFromPhaseUntilAScriptSetsOne)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_mush_name = "MyWorld";

    // No custom title: world name + the connection phase.
    doc.m_iConnectPhase = eConnectConnectedToMud;
    EXPECT_STREQ("MyWorld", (const char*)doc.WorldWindowTitle());
    doc.m_iConnectPhase = eConnectNotConnected;
    EXPECT_STREQ("MyWorld (Closed)", (const char*)doc.WorldWindowTitle());
    doc.m_iConnectPhase = eConnectDisconnecting;
    EXPECT_STREQ("MyWorld (Disconnecting\xe2\x80\xa6)",
                 (const char*)doc.WorldWindowTitle());
    doc.m_iConnectPhase = eConnectConnectingToMud;
    EXPECT_STREQ("MyWorld (Connecting\xe2\x80\xa6)",
                 (const char*)doc.WorldWindowTitle());

    // An unnamed world still gets a title.
    doc.m_mush_name = "";
    doc.m_iConnectPhase = eConnectConnectedToMud;
    EXPECT_STREQ("World", (const char*)doc.WorldWindowTitle());
    doc.m_mush_name = "MyWorld";

    // world.SetTitle wins outright — including across every phase change.
    FakeWorldView view;
    doc.m_pWorldView = &view;
    doc.SetTitle("Rincewind");
    EXPECT_STREQ("Rincewind", view.title.c_str());     // reached the view
    for (int phase = eConnectNotConnected; phase <= eConnectConnectedToMud; ++phase) {
        doc.m_iConnectPhase = phase;
        EXPECT_STREQ("Rincewind", (const char*)doc.WorldWindowTitle());
    }

    // Clearing it restores the derived title, suffix and all.
    doc.SetTitle("");
    doc.m_iConnectPhase = eConnectNotConnected;
    EXPECT_STREQ("MyWorld (Closed)", (const char*)doc.WorldWindowTitle());
}

// ── bookmarks: one store, the engine's CLine flag ──────────────────────────
//
// world.Bookmark used to set the CLine's BOOKMARK flag and then notify only the
// dead CMUSHView family, so on the port it drew nothing; the wx pane kept its
// own independent per-line flag that the Display menu toggled and GetLineInfo
// could not see.  The doc now emits the change through the typed seam
// (IWorldView::setBookmark), which is what an adapter mirrors.
TEST(PalWorldView, BookmarkEmitsThroughTheSeam)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    feed(doc, "one\ntwo\nthree\n");
    view.bookmarks.clear();

    doc.Bookmark(2, TRUE);                       // 1-based line number
    ASSERT_EQ(1u, (unsigned)view.bookmarks.size());
    EXPECT_EQ(1L, view.bookmarks[0].first);      // 0-based index reaches the view
    EXPECT_TRUE(view.bookmarks[0].second);
    // …and the engine's own store agrees (GetLineInfo type 7).
    EXPECT_TRUE(doc.GetLineInfo(2, 7).boolVal);

    doc.Bookmark(2, FALSE);                      // clearing emits too
    ASSERT_EQ(2u, (unsigned)view.bookmarks.size());
    EXPECT_EQ(1L, view.bookmarks[1].first);
    EXPECT_FALSE(view.bookmarks[1].second);
    EXPECT_FALSE(doc.GetLineInfo(2, 7).boolVal);

    // Out-of-range line numbers change nothing and emit nothing.
    doc.Bookmark(0, TRUE);
    doc.Bookmark(doc.m_LineList.GetCount() + 1, TRUE);
    EXPECT_EQ(2u, (unsigned)view.bookmarks.size());
}

// The flag rides out with the line, so a view that re-reads the buffer
// (RebuildOutputView, e.g. after a palette change) gets the bookmarks back
// without saving and restoring them itself.
TEST(PalWorldView, RebuiltLinesCarryTheBookmarkFlag)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    feed(doc, "one\ntwo\nthree\n");
    doc.Bookmark(2, TRUE);

    doc.RebuildOutputView();                     // clear + re-emit the buffer

    auto lines = committed(view);
    ASSERT_EQ(3u, (unsigned)lines.size());
    EXPECT_FALSE(lines[0].bookmark);
    EXPECT_TRUE (lines[1].bookmark);             // "two" is still bookmarked
    EXPECT_FALSE(lines[2].bookmark);
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
                EXPECT_STREQ("do this", run.action().c_str());
                EXPECT_STREQ("a hint",  run.hint().c_str());
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
            EXPECT_STREQ("do this", run.action().c_str());
            EXPECT_STREQ("a hint",  run.hint().c_str());
        }
    EXPECT_TRUE(found);
}

// ── doc_dblclick_shared.cpp ────────────────────────────────────────────────
// The decision half of "Double-click Sends Word" / "…Pastes Word".  Both
// options' only Windows reader was CMUSHView::OnLButtonDblClk, a view the port
// never constructs, so this is the seam that makes them testable at all.
//
// Only OutputWordDoubleClicked is exercised here: the send, the history add and
// the command-bar splice are deliberately left to each frontend (see the TU
// header), so there is nothing doc-side to assert about them.

TEST(DoubleClick, EmptyWordDoesNothingEvenWithBothOptionsSet)
{
    CMUSHclientDoc doc;
    make_doc(doc);
    doc.m_bDoubleClickSends   = true;
    doc.m_bDoubleClickInserts = true;
    EXPECT_EQ((int) eDblClickNothing, (int) doc.OutputWordDoubleClicked(""));
}

TEST(DoubleClick, NeitherOptionSetDoesNothing)
{
    CMUSHclientDoc doc;
    make_doc(doc);
    doc.m_bDoubleClickSends   = false;
    doc.m_bDoubleClickInserts = false;
    EXPECT_EQ((int) eDblClickNothing, (int) doc.OutputWordDoubleClicked("orc"));
}

TEST(DoubleClick, InsertsWhenOnlyInsertsIsSet)
{
    CMUSHclientDoc doc;
    make_doc(doc);
    doc.m_bDoubleClickSends   = false;
    doc.m_bDoubleClickInserts = true;
    EXPECT_EQ((int) eDblClickInsert, (int) doc.OutputWordDoubleClicked("orc"));
}

TEST(DoubleClick, SendsBeatsInsertsWhenBothAreSet)
{
    // Windows' if / else-if: ticking both is not an error, sending just wins.
    //
    // Asserted as "never Insert" rather than "is Send", because the Send arm is
    // not reachable headless: CheckConnected() requires m_iConnectPhase ==
    // eConnectConnectedToMud AND ConnectionOpen() (doc.cpp:2729), and there is
    // no real socket here, so it always reports not-connected.  What matters is
    // that the inserts arm is never taken while sends is set -- which this and
    // the abort case below pin between them.
    CMUSHclientDoc doc;
    make_doc(doc);
    doc.m_bDoubleClickSends   = true;
    doc.m_bDoubleClickInserts = true;
    EXPECT_NE((int) eDblClickInsert, (int) doc.OutputWordDoubleClicked("orc"));
}

TEST(DoubleClick, NotConnectedAbortsRatherThanFallingBackToInsert)
{
    // CheckConnected() prompts and reports "not connected"; Windows returns
    // outright, so the caller must do nothing -- in particular it must NOT
    // quietly insert instead, which is why this is a distinct outcome.
    CMUSHclientDoc doc;
    make_doc(doc);
    doc.m_iConnectPhase       = eConnectNotConnected;
    doc.m_bDoubleClickSends   = true;
    doc.m_bDoubleClickInserts = true;
    EXPECT_EQ((int) eDblClickAbort, (int) doc.OutputWordDoubleClicked("orc"));
}

// ── ApplyDefaultFonts ─────────────────────────────────────────────────────
// The global preferences dialog calls this for every open world when it
// closes, so a default font chosen there reaches the screen at once instead of
// waiting for File > Reload Defaults.  These pin the seam it depends on.

TEST(DefaultFonts, PushesTheGlobalDefaultToTheView)
{
    CMUSHclientDoc doc; make_doc(doc);
    FakeWorldView view;
    doc.m_pWorldView = &view;

    App.m_strDefaultOutputFont      = "Menlo";
    App.m_iDefaultOutputFontHeight  = 18;
    App.m_iDefaultOutputFontCharset = DEFAULT_CHARSET;
    doc.m_bUseDefaultOutputFont     = TRUE;
    doc.m_bUseDefaultInputFont      = FALSE;

    doc.ApplyDefaultFonts();

    EXPECT_STREQ("Menlo", view.outFontFace.c_str());
    EXPECT_EQ(18, view.outFontPts);

    doc.m_pWorldView = nullptr;
}

TEST(DefaultFonts, RepeatCallStillReachesTheView)
{
    // Regression: an earlier version skipped the work when the doc's font
    // fields already matched the defaults.  That looks like a harmless
    // optimisation and is not: ApplyDefaultFonts also runs during world open,
    // BEFORE the view is attached, where the emit is silently dropped but the
    // doc fields are still updated.  The later, view-attached call then saw
    // "already correct" and the font never reached the screen.
    CMUSHclientDoc doc; make_doc(doc);

    App.m_strDefaultOutputFont      = "Menlo";
    App.m_iDefaultOutputFontHeight  = 18;
    App.m_iDefaultOutputFontCharset = DEFAULT_CHARSET;
    doc.m_bUseDefaultOutputFont     = TRUE;
    doc.m_bUseDefaultInputFont      = FALSE;

    doc.ApplyDefaultFonts();          // no view yet: updates the doc, draws nothing

    FakeWorldView view;
    doc.m_pWorldView = &view;
    doc.ApplyDefaultFonts();          // view attached: MUST still emit

    EXPECT_STREQ("Menlo", view.outFontFace.c_str());
    EXPECT_EQ(18, view.outFontPts);

    doc.m_pWorldView = nullptr;
}

TEST(DefaultFonts, WorldThatOverridesTheFontIsLeftAlone)
{
    CMUSHclientDoc doc; make_doc(doc);
    FakeWorldView view;
    doc.m_pWorldView = &view;

    App.m_strDefaultOutputFont     = "Menlo";
    App.m_iDefaultOutputFontHeight = 18;
    doc.m_bUseDefaultOutputFont    = FALSE;   // this world picked its own font
    doc.m_bUseDefaultInputFont     = FALSE;

    doc.ApplyDefaultFonts();

    EXPECT_TRUE(view.outFontFace.empty());

    doc.m_pWorldView = nullptr;
}

// ── EditCopyTarget ────────────────────────────────────────────────────────
// Edit > Copy.  On Windows this rule was written twice — once to enable the
// menu item (CMUSHView::OnUpdateNeedSelectionForCopy) and once to decide what
// to copy (the GetSelection test in CMUSHView::OnEditCopy) — and the wx port
// had replaced it with a third, wrong test: "does the output pane have the
// keyboard focus".  Nothing ever focuses COutputPane, so Copy was greyed out
// and inert with output text selected.  These pin the one shared rule.
//
// Selection ranges here use the seam's documented conventions: outputSelection()
// is 1-based line/col with all-zeros meaning "nothing selected"; commandSelection()
// is the raw half-open [start, end) character range.

TEST(EditCopy, NothingSelectedAnywhereMeansNothingToCopy)
{
    CMUSHclientDoc doc; make_doc(doc);
    FakeWorldView view;
    doc.m_pWorldView = &view;

    EXPECT_EQ((int) eCopyNothing, (int) doc.EditCopyTarget());

    doc.m_pWorldView = nullptr;
}

TEST(EditCopy, OutputSelectionIsCopiedRegardlessOfFocus)
{
    // The regression: a real output selection, no command-bar selection.  The
    // old port test asked about focus and answered "nothing"; the rule asks
    // about the selection and answers "the output window".
    CMUSHclientDoc doc; make_doc(doc);
    FakeWorldView view;
    view.sel = { 3, 5, 3, 12 };        // line 3, columns 5..12
    doc.m_pWorldView = &view;

    EXPECT_EQ((int) eCopyOutputSelection, (int) doc.EditCopyTarget());

    doc.m_pWorldView = nullptr;
}

TEST(EditCopy, MultiLineOutputSelectionCountsEvenWhenEndColIsSmaller)
{
    // endCol < startCol is normal when the selection spans lines, so the
    // column comparison must only apply within a single line.
    CMUSHclientDoc doc; make_doc(doc);
    FakeWorldView view;
    view.sel = { 3, 40, 7, 2 };
    doc.m_pWorldView = &view;

    EXPECT_EQ((int) eCopyOutputSelection, (int) doc.EditCopyTarget());

    doc.m_pWorldView = nullptr;
}

TEST(EditCopy, CaretInTheOutputWindowIsNotASelection)
{
    // Same line, same column: a click, not a drag.  Windows' test is
    // endcol > startcol, strictly.
    CMUSHclientDoc doc; make_doc(doc);
    FakeWorldView view;
    view.sel = { 4, 9, 4, 9 };
    doc.m_pWorldView = &view;

    EXPECT_EQ((int) eCopyNothing, (int) doc.EditCopyTarget());

    doc.m_pWorldView = nullptr;
}

TEST(EditCopy, FallsBackToTheCommandBarSelection)
{
    CMUSHclientDoc doc; make_doc(doc);
    FakeWorldView view;
    view.cmd    = "look at the sign";
    view.cmdSel = { 8, 11 };           // "the" selected in the command bar
    doc.m_pWorldView = &view;

    EXPECT_EQ((int) eCopyCommandSelection, (int) doc.EditCopyTarget());

    doc.m_pWorldView = nullptr;
}

TEST(EditCopy, CaretInTheCommandBarIsNotASelection)
{
    // Half-open range: start == end is an insertion point.
    CMUSHclientDoc doc; make_doc(doc);
    FakeWorldView view;
    view.cmd    = "look at the sign";
    view.cmdSel = { 8, 8 };
    doc.m_pWorldView = &view;

    EXPECT_EQ((int) eCopyNothing, (int) doc.EditCopyTarget());

    doc.m_pWorldView = nullptr;
}

TEST(EditCopy, OutputSelectionBeatsTheCommandBarWhenBothHaveOne)
{
    // CMUSHView::OnEditCopy tries the output window first and only falls
    // through to the command window when there is nothing there.
    CMUSHclientDoc doc; make_doc(doc);
    FakeWorldView view;
    view.sel    = { 2, 1, 2, 6 };
    view.cmd    = "look at the sign";
    view.cmdSel = { 8, 11 };
    doc.m_pWorldView = &view;

    EXPECT_EQ((int) eCopyOutputSelection, (int) doc.EditCopyTarget());

    doc.m_pWorldView = nullptr;
}

TEST(EditCopy, NoViewAttachedMeansNothingToCopy)
{
    // Headless, or a world tearing down: must answer, not dereference.
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = nullptr;
    EXPECT_EQ((int) eCopyNothing, (int) doc.EditCopyTarget());
}

// ── isScrolledToBottom ────────────────────────────────────────────────────
// The output pane pins itself to the newest line while "at the bottom", and
// the four hand-written copies of that test all carried a one-row tolerance to
// paper over the fact that they reconstructed the document height instead of
// asking the scrollbar.  The tolerance made a one-row scroll up read as "still
// at the bottom", so auto-scroll stayed on and the next line of output yanked
// the view back down — the wheel "resisting" unless you scrolled harder.
//
// Units: 15 px rows, a 300 px viewport (20 rows), so a 100-row document is
// 1500 px and the bottom-most scroll position is (1500 - 300) / 15 = row 80.

// The drain used to resume by skipping FORWARD from the head over every line
// already emitted, so its cost was proportional to the whole scrollback — and
// it runs once per received packet, once per 100 ms timer tick, and once per
// Note/WindowCreate/WindowShow scripting call.  It now walks backwards from the
// tail over the un-emitted lines instead, which is proportional to the new
// output.  This pins that: draining the same amount of new text must not get
// dramatically more expensive just because the buffer behind it is larger.
TEST(PalWorldView, DrainCostFollowsNewLinesNotBufferSize)
{
    struct Timed {
        static double drain_ms(int prefill, int newLines) {
            FakeWorldView view;
            CMUSHclientDoc doc; make_doc(doc);
            doc.m_maxlines = 200000;          // no eviction: isolate the walk
            doc.AllocateLinePositions();
            doc.m_pWorldView = &view;

            char buf[32];
            for (int i = 0; i < prefill; ++i) {
                snprintf(buf, sizeof buf, "p %d\n", i);
                feed(doc, buf);               // feed() drains each time
            }

            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < newLines; ++i) {
                snprintf(buf, sizeof buf, "n %d\n", i);
                feed(doc, buf);
            }
            auto t1 = std::chrono::steady_clock::now();

            doc.m_pWorldView = nullptr;
            return std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
    };

    Timed::drain_ms(500, 200);                       // warm up
    const double shallow = Timed::drain_ms(1000,  200);
    const double deep    = Timed::drain_ms(40000, 200);

    // 40x the buffer for the same 200 new lines.  O(buffer) would be ~40x the
    // time; O(new) is ~1x.  Allow 8x plus a floor for timer noise — still far
    // below what a return to the forward skip would cost.
    const double budget = std::max(shallow * 8.0, 5.0);
    EXPECT_TRUE(deep < budget);
    if (deep >= budget)
        printf("    drain after 1k lines=%.2fms after 40k=%.2fms budget=%.2fms\n",
               shallow, deep, budget);
}

TEST(ScrollGeometry, BottomMostPositionIsAtTheBottom)
{
    EXPECT_TRUE(pal::isScrolledToBottom(80, 15, 300, 1500));
}

TEST(ScrollGeometry, OneRowUpIsNotAtTheBottom)
{
    // THE regression.  With the old "- one row" slack this answered true, which
    // is what made the pane snap back.
    EXPECT_FALSE(pal::isScrolledToBottom(79, 15, 300, 1500));
}

TEST(ScrollGeometry, WellBackInTheScrollbackIsNotAtTheBottom)
{
    EXPECT_FALSE(pal::isScrolledToBottom(0,  15, 300, 1500));
    EXPECT_FALSE(pal::isScrolledToBottom(40, 15, 300, 1500));
}

TEST(ScrollGeometry, PastTheBottomStillCountsAsAtTheBottom)
{
    // Can happen transiently while the buffer is being trimmed: the position is
    // clamped a moment later, and reporting "not at the bottom" in between
    // would freeze the pane for no reason.
    EXPECT_TRUE(pal::isScrolledToBottom(999, 15, 300, 1500));
}

TEST(ScrollGeometry, PartialLastRowIsReachableSoRoundsUp)
{
    // 1495 px of document, 300 px viewport: 1195 px hidden = 79.67 rows.  Row 80
    // is a real position (it shows the tail, with the top row part-scrolled), so
    // the maximum must round UP.  Truncating would leave the pane one row short
    // of the bottom for ever, and auto-scroll would never re-engage.
    EXPECT_EQ(80, pal::maxScrollUnit(15, 300, 1495));
    EXPECT_TRUE (pal::isScrolledToBottom(80, 15, 300, 1495));
    EXPECT_FALSE(pal::isScrolledToBottom(79, 15, 300, 1495));
}

TEST(ScrollGeometry, DocumentShorterThanTheViewportIsAlwaysAtTheBottom)
{
    // Nothing to scroll: a nearly-empty world must not come up frozen.
    EXPECT_EQ(0, pal::maxScrollUnit(15, 300, 150));
    EXPECT_TRUE(pal::isScrolledToBottom(0, 15, 300, 150));
}

TEST(ScrollGeometry, UnmeasuredPaneIsAtTheBottom)
{
    // Before the first font measurement the row height is 0.  Answer true, or a
    // brand-new window would freeze before its first line ever arrived.
    EXPECT_TRUE(pal::isScrolledToBottom(0, 0, 300, 1500));
    EXPECT_EQ(0, pal::maxScrollUnit(0, 300, 1500));
}

// ── A palette change re-resolves runs; it does not re-emit them ───────────
//
// SOutputRun now carries the style INPUTS its fg/bg were resolved from, so a
// palette change can be applied in place.  These pin the arithmetic itself
// (pal::resolveStyleColours, the one body CMUSHclientDoc::GetStyleRGB also
// calls) rather than the wx pane, which the headless build cannot construct.
namespace {
pal::StylePalette make_palette()
{
    pal::StylePalette p;
    for (int i = 0; i < 8; ++i)  { p.normal[i] = 0x111100u + i; p.bold[i] = 0x222200u + i; }
    for (int i = 0; i < 16; ++i) { p.customText[i] = 0x330000u + i; p.customBack[i] = 0x440000u + i; }
    p.alternativeInverse = false;
    return p;
}
// A recognisable stand-in for the xterm table: entry k is 0x50000 + k, so a
// wrong index is obvious and no entry collides with the palette above.
struct Xterm { unsigned int t[256]; Xterm() { for (int i = 0; i < 256; ++i) t[i] = 0x50000u + i; } };
const unsigned int* xt() { static Xterm x; return x.t; }
}

TEST(StyleColour, AnsiIndicesReadTheNormalAndBoldTables)
{
    const pal::StylePalette p = make_palette();
    unsigned int fg = 0, bg = 0;

    pal::resolveStyleColours(0, 3, 5, p, xt(), fg, bg);
    EXPECT_EQ(0x111103u, fg);                 // normal[3]
    EXPECT_EQ(0x111105u, bg);                 // background is ALWAYS the normal table

    // HILITE moves only the FOREGROUND to the bold table — a resolver that
    // bolded both halves would pass a same-table check.
    pal::resolveStyleColours(pal::kStyleHilite, 3, 5, p, xt(), fg, bg);
    EXPECT_EQ(0x222203u, fg);                 // bold[3]
    EXPECT_EQ(0x111105u, bg);                 // still normal[5]
}

TEST(StyleColour, InverseSwapsTheIndicesBeforeTheBoldLookup)
{
    // The subtle half of GetStyleRGB: with the default (non-alternative)
    // inverse, the INDICES swap first, so bold applies to what was the
    // background index.  Alternative inverse keeps the roles and swaps the
    // OUTPUTS, so bold stays on the original foreground.  Getting these the
    // same way round is a colour shift on every inverse+bold run.
    pal::StylePalette p = make_palette();
    unsigned int fg = 0, bg = 0;

    p.alternativeInverse = false;
    pal::resolveStyleColours(pal::kStyleHilite | pal::kStyleInverse, 3, 5, p, xt(), fg, bg);
    EXPECT_EQ(0x222205u, fg);                 // bold[5]  — the OLD background index
    EXPECT_EQ(0x111103u, bg);                 // normal[3]

    p.alternativeInverse = true;
    pal::resolveStyleColours(pal::kStyleHilite | pal::kStyleInverse, 3, 5, p, xt(), fg, bg);
    EXPECT_EQ(0x111105u, fg);                 // normal[5] — background, unbolded
    EXPECT_EQ(0x222203u, bg);                 // bold[3]   — bold stayed on the fg index
}

TEST(StyleColour, XtermIndicesArePaletteIndependent)
{
    // Indices >= 8 come from the static xterm table, in BOTH tables' place and
    // regardless of HILITE.  This is why editing the eight ANSI pairs must not
    // change an xterm-256 run — and it is the case the first version of the
    // resolver got wrong by casting a 32-bit COLORREF array through
    // `const unsigned long*`, which read two entries per index on LP64.
    const pal::StylePalette p = make_palette();
    unsigned int fg = 0, bg = 0;

    pal::resolveStyleColours(0, 196, 21, p, xt(), fg, bg);
    EXPECT_EQ(0x50000u + 196, fg);
    EXPECT_EQ(0x50000u + 21,  bg);
    pal::resolveStyleColours(pal::kStyleHilite, 196, 21, p, xt(), fg, bg);
    EXPECT_EQ(0x50000u + 196, fg);            // HILITE does not move it
}

TEST(StyleColour, CustomColoursComeFromTheForegroundIndexAlone)
{
    // COLOUR_CUSTOM stores the custom index in iForeColour ONLY; the pair is
    // customText[i] / customBack[i].  A resolver that read backIdx for the
    // background would produce a plausible-looking but wrong pair.
    const pal::StylePalette p = make_palette();
    unsigned int fg = 0, bg = 0;

    pal::resolveStyleColours(pal::kColourCustom, 4, 9, p, xt(), fg, bg);
    EXPECT_EQ(0x330004u, fg);
    EXPECT_EQ(0x440004u, bg);                 // index 4, NOT 9

    pal::resolveStyleColours(pal::kColourCustom | pal::kStyleInverse, 4, 9, p, xt(), fg, bg);
    EXPECT_EQ(0x440004u, fg);
    EXPECT_EQ(0x330004u, bg);
}

TEST(StyleColour, RgbRunsAreLiteralAndSurviveAnyPalette)
{
    // MXP <color> and trigger "other" colours are COLOUR_RGB: the indices ARE
    // the colours.  They must re-resolve to themselves, which is what lets the
    // pane recolour every run unconditionally instead of filtering first.
    pal::StylePalette p = make_palette();
    unsigned int fg = 0, bg = 0;

    pal::resolveStyleColours(pal::kColourRgb, 0x123456, 0x654321, p, xt(), fg, bg);
    EXPECT_EQ(0x123456u, fg);
    EXPECT_EQ(0x654321u, bg);

    // ...including after the palette moves underneath them.
    for (int i = 0; i < 8; ++i) { p.normal[i] = 0x999999u; p.bold[i] = 0x888888u; }
    pal::resolveStyleColours(pal::kColourRgb, 0x123456, 0x654321, p, xt(), fg, bg);
    EXPECT_EQ(0x123456u, fg);
    EXPECT_EQ(0x654321u, bg);
}

TEST(StyleColour, ResolvingTwiceWithTheSamePaletteIsStable)
{
    // The property the in-place recolour depends on: re-resolving a run that
    // was already resolved must be a no-op, so a spurious palette poll cannot
    // drift the colours.  (It would not be, if the resolver ever consumed its
    // own output — e.g. by writing the resolved value back into fore_idx.)
    const pal::StylePalette p = make_palette();
    unsigned int fg1 = 0, bg1 = 0, fg2 = 0, bg2 = 0;
    pal::resolveStyleColours(pal::kStyleHilite, 3, 5, p, xt(), fg1, bg1);
    pal::resolveStyleColours(pal::kStyleHilite, 3, 5, p, xt(), fg2, bg2);
    EXPECT_EQ(fg1, fg2);
    EXPECT_EQ(bg1, bg2);
}

// ── BuildOutputLine must reproduce the push path exactly ──────────────────
//
// The drain (push) and any fetch-at-paint consumer (pull) now share one
// conversion, CMUSHclientDoc::BuildOutputLine.  This is the property that makes
// that safe: for every committed line, what the view was PUSHED and what a
// later fetch BUILDS must be indistinguishable -- text, colours, flags, link
// payload and the line-level comment/bookmark bits.
//
// It is the whole basis of a pull model, so it is asserted over a mixture that
// exercises every branch of the converter: ANSI colour, a bold run, a
// horizontal rule, a note line, a bookmarked line and a line with an MXP link.
TEST(BuildOutputLine, ReproducesWhatThePushPathEmitted)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    feed(doc, "plain text\n");
    feed(doc, "\x1b[1;32mbold green\x1b[0m and normal\n");
    doc.Note("a note line");
    doc.UpdateAllViews(nullptr);
    doc.Hyperlink("do this", "click", "a hint", "", "", FALSE);
    doc.Note("");
    doc.UpdateAllViews(nullptr);
    doc.NoteHr();
    doc.UpdateAllViews(nullptr);

    const auto pushed = committed(view);
    ASSERT_TRUE(pushed.size() >= 4);

    for (size_t i = 0; i < pushed.size(); ++i) {
        SOutputLine built;
        ASSERT_TRUE(doc.BuildOutputLine((long) i, built));

        EXPECT_EQ(pushed[i].size(),     built.size());
        EXPECT_EQ(pushed[i].comment,    built.comment);
        EXPECT_EQ(pushed[i].bookmark,   built.bookmark);
        if (pushed[i].size() != built.size()) continue;

        for (size_t r = 0; r < built.size(); ++r) {
            const SOutputRun& a = pushed[i][r];
            const SOutputRun& b = built[r];
            EXPECT_EQ(a.text,          b.text);
            EXPECT_EQ(a.fg,            b.fg);
            EXPECT_EQ(a.bg,            b.bg);
            EXPECT_EQ((int) a.bold,    (int) b.bold);
            EXPECT_EQ((int) a.underline, (int) b.underline);
            EXPECT_EQ((int) a.italic,  (int) b.italic);
            EXPECT_EQ((int) a.horiz_rule, (int) b.horiz_rule);
            EXPECT_EQ((int) a.utf8,    (int) b.utf8);
            EXPECT_EQ(a.style_flags,   b.style_flags);
            EXPECT_EQ(a.fore_idx,      b.fore_idx);
            EXPECT_EQ(a.back_idx,      b.back_idx);
            EXPECT_EQ(a.action(),      b.action());
            EXPECT_EQ(a.hint(),        b.hint());
            EXPECT_EQ(a.action_type(), b.action_type());
        }
    }
}

// ...and where the two DELIBERATELY differ, which is the argument for a pull
// model in the first place.  A pushed SOutputLine is a SNAPSHOT: once emitted,
// nothing keeps it in step with the CLine it came from.  world.Bookmark writes
// the CLine flag, so a later BuildOutputLine sees it while the pushed copy
// still says false -- which is exactly why the push path needs a separate
// setBookmark seam to patch the view's copy, and a pull path would not.
TEST(BuildOutputLine, SeesLiveFlagsThatAPushedSnapshotHasMissed)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    feed(doc, "one\n");
    feed(doc, "two\n");
    feed(doc, "three\n");

    const auto pushed = committed(view);
    ASSERT_TRUE(pushed.size() >= 3);
    EXPECT_FALSE(pushed[1].bookmark);

    doc.Bookmark(2, TRUE);             // 1-based: the second line

    SOutputLine built;
    ASSERT_TRUE(doc.BuildOutputLine(1, built));
    EXPECT_TRUE(built.bookmark);       // the fetch sees the live flag...
    EXPECT_FALSE(pushed[1].bookmark);  // ...the snapshot never will

    // The push path keeps them in step by patching the view separately.
    ASSERT_TRUE(!view.bookmarks.empty());
    EXPECT_EQ(1L,   view.bookmarks.back().first);
    EXPECT_TRUE(view.bookmarks.back().second);
}

TEST(BuildOutputLine, RefusesOutOfRangeAndTheInProgressLine)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    feed(doc, "one\n");
    SOutputLine out;
    EXPECT_FALSE(doc.BuildOutputLine(-1, out));
    EXPECT_FALSE(doc.BuildOutputLine(9999, out));

    // The in-progress line is NOT a committed line: the drain stops before it
    // and emits it separately as the pending row, so a fetch must refuse it too
    // or a pull consumer would show the prompt twice.
    doc.DisplayMsg("prompt without a newline", 24, 0);
    const long committedCount = doc.m_LineList.GetCount() - 1;
    EXPECT_FALSE(doc.BuildOutputLine(committedCount, out));
    EXPECT_TRUE(doc.BuildOutputLine(committedCount - 1, out));
}

// ── SOutputRun's size is part of the contract ─────────────────────────────
//
// One SOutputRun exists per styled span of every line in the scrollback, so its
// size IS the pane-side memory cost of unbounded scrollback: at ~2.5 runs per
// line, every 8 bytes is 20 MB per million lines.
//
// It was 112 bytes, of which 48 were two std::strings (action and hint) that
// are empty on roughly ninety-nine runs in a hundred -- costing every run two
// constructions and two destructions as well as the space.  Moving them behind
// a shared_ptr, allocated only for link runs, and packing the six flags into
// bitfields brings it to 64.
//
// Pinned rather than commented, because a struct grows back one innocuous
// member at a time.  The number is allowed to CHANGE -- update it deliberately
// and say why -- but not to drift unnoticed.
TEST(OutputRun, StaysSmall)
{
    // 64 on LP64 with a 24-byte std::string and a 16-byte shared_ptr.
    EXPECT_TRUE(sizeof(SOutputRun) <= 64);
    if (sizeof(SOutputRun) > 64)
        printf("    sizeof(SOutputRun) = %zu, was 112, budget 64\n",
               sizeof(SOutputRun));
}

TEST(OutputRun, AnOrdinaryRunAllocatesNoLink)
{
    // The whole point: a non-link run must not pay for the link payload, and
    // must still answer the same things the old plain members did.
    SOutputRun r;
    EXPECT_TRUE(r.link == nullptr);
    EXPECT_FALSE(r.isLink());
    EXPECT_TRUE(r.action().empty());
    EXPECT_TRUE(r.hint().empty());
    EXPECT_EQ(0, r.action_type());
}

TEST(OutputRun, SetLinkRoundTripsAndSharesOnCopy)
{
    SOutputRun r;
    r.setLink("send this", "a hint", 4);
    EXPECT_TRUE(r.isLink());
    EXPECT_STREQ("send this", r.action().c_str());
    EXPECT_STREQ("a hint",    r.hint().c_str());
    EXPECT_EQ(4, r.action_type());

    // A copy SHARES the payload rather than duplicating two strings -- which is
    // what makes copying a run cheap on the DetectURLs and paint paths.
    SOutputRun c = r;
    EXPECT_TRUE(c.link == r.link);
    EXPECT_STREQ("send this", c.action().c_str());

    // ...and an all-empty setLink clears it rather than allocating an empty
    // payload, so a run that had a link and lost it costs nothing again.
    r.setLink("", "", 0);
    EXPECT_TRUE(r.link == nullptr);
    EXPECT_FALSE(r.isLink());
}

TEST(OutputRun, FlagsSurviveBeingBitfields)
{
    // Packed into bitfields; assigning a bool must still round-trip, and the
    // fields must not alias each other.
    SOutputRun r;
    EXPECT_FALSE(r.bold);
    EXPECT_TRUE(r.utf8);             // the one that defaults TRUE
    r.bold = true;  r.italic = true;
    EXPECT_TRUE(r.bold);
    EXPECT_TRUE(r.italic);
    EXPECT_FALSE(r.underline);       // untouched neighbours stay clear
    EXPECT_FALSE(r.strikethrough);
    EXPECT_FALSE(r.horiz_rule);
    r.utf8 = false;
    EXPECT_FALSE(r.utf8);
    EXPECT_TRUE(r.bold);             // ...and clearing one did not clear another
}

// ── 64-bit row -> pixel geometry ──────────────────────────────────────────
//
// rows * rowHeight was computed in int.  At a 15-pixel row that wraps NEGATIVE
// past 143,165,576 rows — unreachable while the scrollback was capped at
// 500,000 lines, reachable now that max_output_lines can be 0 (unlimited).
// A negative virtual height does not fail loudly; the scrollbar, hit-testing
// and the find-scroll all quietly produce nonsense.
TEST(ScrollGeometry64, TheProductThatUsedToWrapIsNowExact)
{
    // 200,000,000 rows * 15 px = 3,000,000,000 — past INT_MAX (2,147,483,647),
    // and as a signed int it lands at -1,294,967,296.
    const long long rows = 200000000LL;
    EXPECT_EQ(3000000000LL, pal::rowsToPixels(rows, 15));
    EXPECT_TRUE(pal::rowsToPixels(rows, 15) > 0);      // the actual regression
}

TEST(ScrollGeometry64, DeviceExtentSaturatesInsteadOfWrapping)
{
    // wxScrolledWindow's virtual size is an int, so the ceiling is real however
    // the arithmetic is done.  Saturating turns "undefined behaviour and a
    // negative height" into "the scrollbar stops growing", which is a bad day
    // rather than a broken window.
    EXPECT_EQ(2147483647, pal::clampToDeviceExtent(3000000000LL));
    EXPECT_EQ(2147483647, pal::clampToDeviceExtent(2147483647LL));
    EXPECT_EQ(2147483646, pal::clampToDeviceExtent(2147483646LL));
    EXPECT_EQ(0,          pal::clampToDeviceExtent(-5LL));
    EXPECT_EQ(1234,       pal::clampToDeviceExtent(1234LL));
}

TEST(ScrollGeometry64, ScrollUnitsDivideBeforeSaturating)
{
    // Dividing in 64 bits first is what keeps a reachable scroll POSITION
    // reachable even when the pixel extent is past INT_MAX: 3e9 px at 15 px per
    // unit is 200,000,000 units, comfortably inside an int.
    EXPECT_EQ(200000000, pal::pixelsToScrollUnits(3000000000LL, 15));
    EXPECT_EQ(0,         pal::pixelsToScrollUnits(3000000000LL, 0));   // unmeasured
    EXPECT_EQ(0,         pal::pixelsToScrollUnits(-1LL, 15));
    EXPECT_EQ(66,        pal::pixelsToScrollUnits(1000LL, 15));
}

TEST(ScrollGeometry64, DegenerateInputsAreZeroNotGarbage)
{
    EXPECT_EQ(0LL, pal::rowsToPixels(0, 15));
    EXPECT_EQ(0LL, pal::rowsToPixels(-100, 15));
    EXPECT_EQ(0LL, pal::rowsToPixels(100, 0));     // font not measured yet
    EXPECT_EQ(0LL, pal::rowsToPixels(100, -3));
}

// ── One invalidation per packet, not three ────────────────────────────────
//
// The ordinary shape of MUD output is "some finished lines, then a prompt with
// no newline".  The drain used to close its paint batch BEFORE emitting the
// prompt row, so the prompt landed outside any batch and paid its own scrollbar
// update and invalidation — and then the unconditional EmitRepaint at the end
// added a third.  Three invalidations per packet is the multiplier on every
// O(buffer) paint path, which is why it is the first thing the
// unbounded-scrollback work fixes.
//
// A count, not a stopwatch: the batch must open once, close once, and no
// separate repaint may be requested when the batch already covered it.
TEST(DrainBatching, ALineAndAPromptAreOneBatchAndNoExtraRepaint)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    // Prime: get past the initial empty-line bookkeeping so the counts below
    // describe one steady-state packet only.
    feed(doc, "priming\n");
    view.batchBegins = view.batchEnds = view.repaints = 0;

    // One packet: a completed line plus an unterminated prompt.
    doc.DisplayMsg("you are here\nHP:100 > ", 22, 0);
    doc.UpdateAllViews(nullptr);

    EXPECT_EQ(1, view.batchBegins);
    EXPECT_EQ(1, view.batchEnds);
    EXPECT_EQ(0, view.repaints);      // the batch already invalidated
}

TEST(DrainBatching, APromptOnItsOwnAsksForNoSeparateRepaint)
{
    // No completed line, so no batch is opened at all — the pending AppendLine
    // does its own single update.  A repaint on top of that would be the third
    // invalidation again, just via a different route.
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    feed(doc, "priming\n");
    view.batchBegins = view.batchEnds = view.repaints = 0;

    doc.DisplayMsg("HP:100 > ", 9, 0);
    doc.UpdateAllViews(nullptr);

    EXPECT_EQ(0, view.batchBegins);
    EXPECT_EQ(0, view.repaints);
}

TEST(DrainBatching, AMiniwindowOnlyUpdateStillGetsItsRepaint)
{
    // The other direction, and the one that would break Quow's plugins: a
    // caller that emits nothing — world.WindowShow, WindowPosition, WindowCreate
    // — calls UpdateAllViews purely to have the canvas recomposited, and MUST
    // still get an invalidation.  Without this arm, "never repaint" passes.
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    feed(doc, "priming\n");
    view.batchBegins = view.batchEnds = view.repaints = 0;

    doc.UpdateAllViews(nullptr);      // nothing new to emit

    EXPECT_EQ(0, view.batchBegins);
    EXPECT_EQ(1, view.repaints);
}

// ── HasUnflushedOutput: the world timer's idle skip ───────────────────────
//
// The 100ms world timer used to call UpdateAllViews unconditionally, and the
// drain's fast-exit path still ends in EmitRepaint — so an idle connected world
// repainted its whole viewport 10 times a second forever.  HasUnflushedOutput
// is the predicate that stops it, and it has to answer TRUE for every way the
// screen can legitimately change, or output stalls until the next packet.
TEST(UnflushedOutput, NothingToFlushAfterADrain)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    feed(doc, "hello\n");                 // feed() drains
    EXPECT_FALSE(doc.HasUnflushedOutput());
}

TEST(UnflushedOutput, ACompletedLineIsUnflushed)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    feed(doc, "hello\n");
    ASSERT_FALSE(doc.HasUnflushedOutput());

    doc.DisplayMsg("world\n", 6, 0);      // NO drain
    EXPECT_TRUE(doc.HasUnflushedOutput());

    doc.UpdateAllViews(nullptr);
    EXPECT_FALSE(doc.HasUnflushedOutput());
}

TEST(UnflushedOutput, AGrowingPromptIsUnflushed)
{
    // The case the fingerprint exists for: a MUD prompt with no newline is an
    // in-progress line, never a completed one, so the line COUNT never changes
    // as it grows.  A predicate that only compared counts would leave a
    // half-typed prompt on screen until the next full line arrived.
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    doc.DisplayMsg("HP:100 ", 7, 0);
    EXPECT_TRUE(doc.HasUnflushedOutput());       // the prompt appearing
    doc.UpdateAllViews(nullptr);
    EXPECT_FALSE(doc.HasUnflushedOutput());

    doc.DisplayMsg("MP:50 > ", 8, 0);
    EXPECT_TRUE(doc.HasUnflushedOutput());       // ...and growing
    doc.UpdateAllViews(nullptr);
    EXPECT_FALSE(doc.HasUnflushedOutput());

    // ...and finishing it: the pending row becomes a committed line, which the
    // count arm catches, and the fingerprint must go back to "no pending row"
    // rather than staying at the prompt's value.
    doc.DisplayMsg("\n", 1, 0);
    EXPECT_TRUE(doc.HasUnflushedOutput());
    doc.UpdateAllViews(nullptr);
    EXPECT_FALSE(doc.HasUnflushedOutput());
}

TEST(UnflushedOutput, ARestyledPromptOfTheSameLengthIsUnflushed)
{
    // Length alone is not enough: world.ColourTell and an ANSI colour change
    // mid-prompt add a style run without adding characters.  The fingerprint
    // carries the style count for exactly this.
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    doc.DisplayMsg("abc", 3, 0);
    doc.UpdateAllViews(nullptr);
    ASSERT_FALSE(doc.HasUnflushedOutput());

    doc.DisplayMsg("\x1b[31m", 5, 0);     // colour change: no new characters
    EXPECT_TRUE(doc.HasUnflushedOutput());
}

TEST(UnflushedOutput, NoSinkMeansNothingToFlush)
{
    // A world with no view attached (every headless test that does not set
    // m_pWorldView) must not claim work: the drain would return immediately
    // anyway, and answering true would make the timer spin.
    CMUSHclientDoc doc; make_doc(doc);
    doc.DisplayMsg("hello\n", 6, 0);
    EXPECT_FALSE(doc.HasUnflushedOutput());
}

// ── world.TextRectangle: the offset-from-edge form ────────────────────────
//
// A right or bottom of <= 0 means "that many pixels in from the far edge" —
// world.TextRectangle(0, 0, -200, 0) reserves the rightmost 200 pixels for a
// docked miniwindow.  Windows resolves it in CMUSHView::GetTextRectangle; both
// wx consumers (COutputPane::TextRect for the clip + visual wrap,
// EffectiveTextCols for m_nWrapColumn + NAWS) used to treat a non-positive edge
// as "not given" and hand the text the FULL client width, so it wrapped late and
// drew underneath the miniwindow.
TEST(TextRect, NegativeRightIsAnOffsetFromTheRightEdge)
{
    int l = 0, t = 0, r = -200, b = 0;
    pal::resolveTextRect(l, t, r, b, 800, 600);
    EXPECT_EQ(0,   l);
    EXPECT_EQ(600, r);            // 800 - 200
    EXPECT_EQ(600, b);            // bottom 0 → the full client height
    EXPECT_EQ(600, pal::textRectWidth(0, 0, -200, 0, 800));
}

TEST(TextRect, ZeroFarEdgeMeansTheClientEdge)
{
    // 0 is the <= 0 case too: 0 + clientW == clientW.
    int l = 100, t = 0, r = 0, b = 0;
    pal::resolveTextRect(l, t, r, b, 800, 600);
    EXPECT_EQ(800, r);
    EXPECT_EQ(600, b);
}

TEST(TextRect, PositiveEdgesArePassedThroughUnchanged)
{
    int l = 10, t = 20, r = 300, b = 400;
    pal::resolveTextRect(l, t, r, b, 800, 600);
    EXPECT_EQ(10, l); EXPECT_EQ(20, t); EXPECT_EQ(300, r); EXPECT_EQ(400, b);
    EXPECT_EQ(290, pal::textRectWidth(10, 20, 300, 400, 800));
}

TEST(TextRect, AnOverlargeOffsetIsFlooredAtTwentyPixels)
{
    // mushview.cpp: max (TextRectangle.right, TextRectangle.left + 20) — an
    // offset bigger than the window must not invert the rectangle.
    int l = 50, t = 0, r = -900, b = -900;
    pal::resolveTextRect(l, t, r, b, 800, 600);
    EXPECT_EQ(70, r);             // left + 20
    EXPECT_EQ(20, b);             // top + 20
}

TEST(TextRect, AllZeroMeansNoRectangleAtAll)
{
    // CMUSHView::HaveTextRectangle: the all-zero rect is the "unset" sentinel
    // and must NOT be resolved — resolving it would produce the whole client
    // rect anyway, but textRectWidth has to answer the full pane width without
    // the caller having to special-case it.
    EXPECT_TRUE (pal::textRectIsUnset(0, 0, 0, 0));
    EXPECT_FALSE(pal::textRectIsUnset(0, 0, -200, 0));
    EXPECT_EQ(800, pal::textRectWidth(0, 0, 0, 0, 800));
}

TEST(TextRect, AWiderRectangleDoesNotWidenTheText)
{
    // A rectangle wider than the pane cannot buy extra columns: the text is
    // still bounded by the window (GetOutputWindowWidth is a client measure).
    EXPECT_EQ(800, pal::textRectWidth(0, 0, 5000, 0, 800));
}

TEST(TextRect, WrapColumnsSubtractThePixelOffsetOnce)
{
    // mushview.cpp:3220 — (GetOutputWindowWidth() - m_iPixelOffset) / charW.
    EXPECT_EQ(80, pal::textWrapColumns(800, 0,  10));
    EXPECT_EQ(79, pal::textWrapColumns(800, 10, 10));
    EXPECT_EQ(1,  pal::textWrapColumns(800, 0,  0));    // font not measured yet
    EXPECT_EQ(1,  pal::textWrapColumns(5,   20, 10));   // offset wider than the pane
}

TEST(TextRect, TheEngineWrapAndTheVisualWrapAgreeOnTheOffsetForm)
{
    // The whole point of sharing the two helpers: EffectiveTextCols (engine
    // wrap + NAWS) and RebuildWrapCache (visual wrap) must land on the same
    // number for the same reserved rectangle, offset form included.
    const int paneW = 800, charW = 10, pixOff = 4;
    const int reserved = pal::textRectWidth(0, 0, -200, 0, paneW);
    EXPECT_EQ(600, reserved);
    EXPECT_EQ(59, pal::textWrapColumns(reserved, pixOff, charW));  // (600-4)/10
    // ...and that is narrower than the unreserved pane, which is the bug the
    // dropped offset form hid: it used to answer the full 79 columns.
    EXPECT_EQ(79, pal::textWrapColumns(paneW, pixOff, charW));
}

// ── omit-from-output must rewind the GUI emission cursor ──────────────────
//
// The non-Windows drain (doc_display_shared.cpp) works out what to paint from
// m_gui_lines_emitted: toEmit = list size - emitted.  A gag trigger
// ("omit from output") deletes lines that the drain may ALREADY have painted,
// and ProcessPreviousLine used to delete them without touching the counter --
// leaving it higher than the list, so `toEmit` went negative and the next lines
// the MUD sent were silently never painted.  Every other deletion site rewinds
// (methods_output.cpp DeleteLines, doc_shared.cpp RemoveChunk); this was the
// outlier.
TEST(PalWorldView, GagTriggerDoesNotSwallowTheLinesAfterIt)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;
    doc.AddStyle(COLOUR_ANSI, WHITE, BLACK, 0, static_cast<CAction*>(nullptr));

    // NOTE ON WHAT THIS DOES AND DOES NOT PROVE.  Removing the rewind does NOT
    // make this test fail -- two attempts at the reported scenario (a gag on a
    // wrapped paragraph arriving in two reads with a drain between) could not
    // produce the deficit; in this harness the gagged paragraph is not painted
    // before it is deleted, so there is nothing to rewind.  So this is
    // CHARACTERISATION of the gag path, not a regression test for the counter.
    // It still earns its place: it pins that lines after a gag keep reaching the
    // view, and that the counter never exceeds the list.
    doc.m_nWrapColumn = 20;

    CTrigger* t = new CTrigger;
    t->trigger         = CString("*spam*");
    t->strLabel        = CString("gag");
    t->bEnabled        = TRUE;
    t->bOmitFromOutput = TRUE;
    t->iSendTo         = eSendToWorld;
    t->iSequence       = DEFAULT_TRIGGER_SEQUENCE;
    doc.GetTriggerMap().SetAt(t->strLabel, t);
    doc.GetTriggerArray().Add(t);

    auto drain = [&] { doc.UpdateAllViews(nullptr); };
    auto put   = [&](const char* s) { doc.DisplayMsg(s, (int) strlen(s), 0); };
    auto sawInView = [&](const char* needle) {
        for (const auto& l : view.lines)
            if (line_text(l).find(needle) != std::string::npos) return true;
        return false;
    };

    put("before the gag\n");   drain();
    EXPECT_TRUE(sawInView("before the gag"));

    // Segment 1: enough to fill and wrap several lines, NO newline yet.  The
    // drain paints them -- m_gui_lines_emitted now counts lines that are about
    // to be deleted.
    put("spam spam spam spam spam spam spam spam spam ");
    drain();
    const long emittedAfterPartial = doc.m_gui_lines_emitted;
    EXPECT_TRUE(emittedAfterPartial > 1);       // the wrap really did paint rows

    // Segment 2 completes the paragraph; the trigger fires and deletes all of it.
    put("and more spam\n");    drain();
    EXPECT_EQ(1L, t->nMatched);

    // The counter must have been rewound to something the list can justify.
    EXPECT_TRUE(doc.m_gui_lines_emitted <= (long) doc.m_LineList.GetCount());

    // The real assertion: lines after the gag still reach the view.  With the
    // rewind removed, toEmit goes <= 0 and these are silently never painted.
    put("after the gag\n");    drain();
    EXPECT_TRUE(sawInView("after the gag"));

    put("and another\n");      drain();
    EXPECT_TRUE(sawInView("and another"));
}

// ── pal::imagePlacement — world.SetBackgroundImage's 13 modes ───────────────
// This arithmetic lived inside the paint-time drawing lambda, which no test can
// reach (COutputPane::OnPaint is never called by any selftest -- see
// docs/AUDIT-2026-08.md), so none of the thirteen placements had ever been
// checked.  The numbering is script API and cannot change.
//
// Fixture: a 40x20 image on a 200x100 client inside a 300x160 owner.  Every
// number below is deliberately distinct -- image, client and owner all differ
// in both axes, and neither client dimension is a whole multiple of the image,
// so a mode that confused width with height, client with owner, or that got the
// tile rounding wrong, lands somewhere visibly different.

static pal::ImagePlacement place(int mode)
{
    return pal::imagePlacement(mode, 40, 20, 200, 100, 300, 160);
}

TEST(PalImagePlacement, StretchModesScaleToTheRightRectangle)
{
    // 0: fill the output window.
    pal::ImagePlacement p = place(0);
    EXPECT_TRUE(p.stretch);
    EXPECT_EQ(0, p.x); EXPECT_EQ(0, p.y);
    EXPECT_EQ(200, p.w); EXPECT_EQ(100, p.h);

    // 1: fill the height, width follows the source's 2:1 aspect => 200.
    p = place(1);
    EXPECT_TRUE(p.stretch);
    EXPECT_EQ(200, p.w); EXPECT_EQ(100, p.h);

    // 2: the OWNER, not the client -- the two differ here on purpose.
    p = place(2);
    EXPECT_TRUE(p.stretch);
    EXPECT_EQ(300, p.w); EXPECT_EQ(160, p.h);

    // 3: the owner's height, aspect-preserved => 160 * 2 = 320.
    p = place(3);
    EXPECT_TRUE(p.stretch);
    EXPECT_EQ(320, p.w); EXPECT_EQ(160, p.h);
}

TEST(PalImagePlacement, AlignedModesPutTheImageAtTheRightCorner)
{
    // Modes 4-12 blit 1:1, so the size is always the image's.
    for (int mode = 4; mode <= 12; ++mode) {
        pal::ImagePlacement p = place(mode);
        EXPECT_FALSE(p.stretch);
        EXPECT_FALSE(p.tile);
        EXPECT_EQ(40, p.w);
        EXPECT_EQ(20, p.h);
    }

    const int right = 200 - 40, bottom = 100 - 20;   // 160, 80
    const int hmid  = (200 - 40) / 2, vmid = (100 - 20) / 2;   // 80, 40

    struct { int mode, x, y; const char* what; } kCases[] = {
        {  4, 0,     0,      "top left" },
        {  5, hmid,  0,      "top centre" },
        {  6, right, 0,      "top right" },
        {  7, right, vmid,   "right, centred vertically" },
        {  8, right, bottom, "bottom right" },
        {  9, hmid,  bottom, "bottom centre" },
        { 10, 0,     bottom, "bottom left" },
        { 11, 0,     vmid,   "left, centred vertically" },
        { 12, hmid,  vmid,   "centred both ways" },
    };
    for (const auto& c : kCases) {
        pal::ImagePlacement p = place(c.mode);
        if (p.x != c.x || p.y != c.y)
            printf("  mode %d (%s): got (%d,%d) want (%d,%d)\n",
                   c.mode, c.what, p.x, p.y, c.x, c.y);
        EXPECT_EQ(c.x, p.x);
        EXPECT_EQ(c.y, p.y);
    }
}

TEST(PalImagePlacement, TileCoversThePartialLastRowAndColumn)
{
    pal::ImagePlacement p = place(13);
    EXPECT_TRUE(p.tile);
    EXPECT_FALSE(p.stretch);
    // 200/40 = 5 exactly, 100/20 = 5 exactly -- and the +1 is deliberate, so a
    // client area that is NOT a whole multiple still gets its edge covered.
    EXPECT_EQ(6, p.across);
    EXPECT_EQ(6, p.down);

    // The case the +1 exists for: 210x110 needs 6 across and 6 down to cover
    // the ragged edge, where a bare division would give 5 and leave a strip.
    pal::ImagePlacement q = pal::imagePlacement(13, 40, 20, 210, 110, 300, 160);
    EXPECT_EQ(6, q.across);
    EXPECT_EQ(6, q.down);
    EXPECT_TRUE(q.across * 40 >= 210);
    EXPECT_TRUE(q.down * 20 >= 110);
}

TEST(PalImagePlacement, AnImageBiggerThanTheClientGivesNegativeOffsets)
{
    // Windows blits 1:1 and lets the image overhang; the aligned modes then
    // produce negative coordinates, which is correct -- the image is clipped by
    // the DC, not repositioned.  Clamping these to 0 would silently move a
    // bottom-right image to the top-left.
    pal::ImagePlacement p = pal::imagePlacement(8, 400, 300, 200, 100, 300, 160);
    EXPECT_EQ(200 - 400, p.x);
    EXPECT_EQ(100 - 300, p.y);
}

TEST(PalImagePlacement, NonsenseIsRefusedRatherThanGuessed)
{
    // A degenerate image: the tile arithmetic would divide by zero.
    pal::ImagePlacement p = pal::imagePlacement(13, 0, 20, 200, 100, 300, 160);
    EXPECT_EQ(0, p.w); EXPECT_EQ(0, p.h);
    p = pal::imagePlacement(4, 40, 0, 200, 100, 300, 160);
    EXPECT_EQ(0, p.w); EXPECT_EQ(0, p.h);

    // An unknown mode draws NOTHING.  The option is script-settable, so a typo
    // must not land the image at some arbitrary default corner.
    p = place(14);
    EXPECT_EQ(0, p.w); EXPECT_EQ(0, p.h);
    p = place(-1);
    EXPECT_EQ(0, p.w); EXPECT_EQ(0, p.h);
}

// ── the Activity window's refresh policy ──────────────────────────────────
//
// platform/activity_refresh.h, lifted out of the wxTimer handler in
// platform/wx_activity.cpp.  Three preferences ride on it (refresh type,
// interval, and the engine's new-activity flag), all three of which the 2026-07
// settings audit found DEAD on the port because the two Windows pumps that read
// them -- CMainFrame::OnTimer and CMUSHclientApp::OnIdle -- do not run here.
// Nothing under test can deliver a wxEVT_TIMER (every selftest exits from
// wxApp::OnInit), so before the lift this policy had never executed in a test.

// One Activity-window frame's worth of state, ticked by hand.
struct ActivityTicker {
    unsigned int type;
    int          intervalSecs;
    long long    accumulatorMs = 0;
    int          refreshes     = 0;

    bool tick(long long elapsedMs, bool activityPending = false) {
        const ActivityRefreshDecision d = ActivityRefreshForTick(
            type, intervalSecs, activityPending, elapsedMs, accumulatorMs);
        accumulatorMs = d.accumulatorMs;
        if (d.refresh)
            ++refreshes;
        return d.refresh;
    }
};

static const int kTickMs = 250;   // wx_activity.cpp's tick

TEST(ActivityRefresh, OnActivityModeIgnoresTheClock)
{
    ActivityTicker t{kActivityRefreshOnActivity, 1};

    // A whole minute of ticks, and the interval is one second: "On New
    // Activity" must still not have refreshed once.
    for (int i = 0; i < 60; i++)
        EXPECT_FALSE(t.tick(1000));
    EXPECT_EQ(0, t.refreshes);
    EXPECT_EQ(0, (int) t.accumulatorMs);   // the clock is not even running

    EXPECT_TRUE(t.tick(1000, /*activityPending*/ true));
    EXPECT_EQ(1, t.refreshes);
}

TEST(ActivityRefresh, PeriodicallyModeIgnoresTheFlag)
{
    ActivityTicker t{kActivityRefreshPeriodically, 15};

    // The flag is raised on EVERY tick; "Periodically" must not listen to it.
    for (int i = 0; i < 59; i++)
        EXPECT_FALSE(t.tick(kTickMs, /*activityPending*/ true));
    EXPECT_EQ(0, t.refreshes);

    // 60 x 250 ms == the 15 s interval, so this one is due.
    EXPECT_TRUE(t.tick(kTickMs, true));
    EXPECT_EQ(1, t.refreshes);
}

TEST(ActivityRefresh, BothModeAnswersEitherTrigger)
{
    ActivityTicker flag{kActivityRefreshBoth, 15};
    EXPECT_TRUE(flag.tick(kTickMs, /*activityPending*/ true));   // no waiting

    ActivityTicker clock{kActivityRefreshBoth, 15};
    for (int i = 0; i < 59; i++)
        EXPECT_FALSE(clock.tick(kTickMs));
    EXPECT_TRUE(clock.tick(kTickMs));                            // no flag needed
}

TEST(ActivityRefresh, NonPositiveIntervalFallsBackToTheDefaultNotToEveryTick)
{
    // The prefs dialog's DDV bounds the interval to 1..300, but the prefs DB is
    // not validated, so a 0 can reach us -- and 0 seconds means rebuilding the
    // whole world list four times a second, forever.
    //
    // A NEGATIVE value did the same thing, not the opposite.  The member is
    // unsigned, so -5 reads back as 4294967291 and passed the old `> 0` guard;
    // the old code then cast it to int INSIDE that guard, getting -5 again, and
    // compared `elapsed >= -5000`, which is true on every tick.  Both mistakes
    // therefore land here, and both mean "refresh constantly".
    const int kBadIntervals[] = { 0, -1, -15, (int) 4294967291u /* = -5 */ };

    for (int bad : kBadIntervals) {
        ActivityTicker t{kActivityRefreshBoth, bad};

        // 59 ticks == 14.75 s: nothing yet, because the fallback is 15 s.
        for (int i = 0; i < 59; i++)
            t.tick(kTickMs);
        if (t.refreshes != 0)
            printf("      (interval %d refreshed %d times in 14.75 s)\n",
                   bad, t.refreshes);
        EXPECT_EQ(0, t.refreshes);

        EXPECT_TRUE(t.tick(kTickMs));
        EXPECT_EQ(kActivityRefreshDefaultSecs, 15);
    }
}

// ── ActivityCompareKeys: the column-sort comparator (2026-08-12) ────────────
// Windows sorts the Activity list by any clicked column, toggling direction;
// the port bound no handler at all.  The comparator was hoisted next to the
// refresh policy so it can be pinned here.  The fixture makes lexical and
// numeric order DISAGREE (9 vs 10), per the degenerate-fixture lesson.
TEST(ActivityRefresh, CompareKeysSortsEachColumnInItsOwnDomain)
{
    ActivitySortKey a, b;
    a.name = "Aardwolf";  b.name = "discworld";       // case-insensitive
    a.newLines = 9;       b.newLines = 10;            // numeric, not lexical
    a.totalLines = 200;   b.totalLines = 30;
    a.status = "Open";    b.status = "closed";
    a.connectTime = 1000; b.connectTime = 2000;
    a.durationSecs = 50;  b.durationSecs = 5;

    // World: case-insensitive text — "Aardwolf" < "discworld" despite 'A'<'d'
    // only holding case-insensitively.
    EXPECT_TRUE(ActivityCompareKeys(a, b, kActivityColWorld, false) < 0);
    // New: 9 < 10 numerically (lexically "9" > "10" — the discriminator).
    EXPECT_TRUE(ActivityCompareKeys(a, b, kActivityColNew, false) < 0);
    // Lines: 200 > 30 numerically (lexically "200" < "30").
    EXPECT_TRUE(ActivityCompareKeys(a, b, kActivityColLines, false) > 0);
    // Status: case-insensitive — "closed" < "Open".
    EXPECT_TRUE(ActivityCompareKeys(a, b, kActivityColStatus, false) > 0);
    // Since / Duration: numeric.
    EXPECT_TRUE(ActivityCompareKeys(a, b, kActivityColSince, false) < 0);
    EXPECT_TRUE(ActivityCompareKeys(a, b, kActivityColDuration, false) > 0);

    // The reverse toggle flips every column's answer.
    EXPECT_TRUE(ActivityCompareKeys(a, b, kActivityColNew, true) > 0);
    EXPECT_TRUE(ActivityCompareKeys(a, b, kActivityColLines, true) < 0);

    // Equal keys stay equal either way (stability's precondition).
    EXPECT_EQ(0, ActivityCompareKeys(a, a, kActivityColNew, false));
    EXPECT_EQ(0, ActivityCompareKeys(a, a, kActivityColNew, true));
}

TEST(ActivityRefresh, RemainderIsCarriedSoTheCadenceDoesNotDrift)
{
    // 300 ms ticks against a 1 s interval: the deadline never lands on a tick
    // boundary, so each firing overshoots and the overshoot has to be carried.
    // Carried:  fires at ticks 4, 7, 10 (1200, 2100, 3000 ms) -- 3 refreshes.
    // Dropped:  fires at ticks 4, 8     (1200, 2400 ms)       -- 2, and the gap
    //           grows with every period, so a "1 second" refresh silently
    //           becomes 1.2 seconds.
    ActivityTicker t{kActivityRefreshPeriodically, 1};

    int firedAt[8];
    int fired = 0;
    for (int i = 1; i <= 10; i++)
        if (t.tick(300) && fired < 8)
            firedAt[fired++] = i;

    EXPECT_EQ(3, fired);
    if (fired == 3) {
        EXPECT_EQ(4,  firedAt[0]);
        EXPECT_EQ(7,  firedAt[1]);
        EXPECT_EQ(10, firedAt[2]);
    }
}

TEST(ActivityRefresh, ALongStallCostsOneRefreshNotAStorm)
{
    // The machine slept for an hour (or the timer was coalesced hard).  Windows
    // compares stamps, so it refreshes once and moves on; a carry by repeated
    // subtraction would instead leave an hour of credit in the accumulator and
    // refresh on every tick until it drained -- 240 rebuilds of the list.
    ActivityTicker t{kActivityRefreshPeriodically, 15};

    EXPECT_TRUE(t.tick(60LL * 60LL * 1000LL));
    EXPECT_LT(t.accumulatorMs, 15LL * 1000LL);

    EXPECT_FALSE(t.tick(kTickMs));
    EXPECT_EQ(1, t.refreshes);
}

TEST(ActivityRefresh, AFlagDrivenRefreshRestartsThePeriodicClock)
{
    // The list has just been rebuilt, so the next periodic refresh is a whole
    // interval away -- that is what stamping m_timeLastActivityUpdate = now
    // does on Windows (ActivityView.cpp:158).
    ActivityTicker t{kActivityRefreshBoth, 15};

    // Stop TWO ticks short of the interval, not one: the flag has to arrive on
    // a tick the periodic half does not fire on, or that half zeroes the
    // accumulator itself and this proves nothing.
    for (int i = 0; i < 58; i++)          // 14.5 s banked
        t.tick(kTickMs);
    EXPECT_EQ(0, t.refreshes);

    EXPECT_TRUE(t.tick(kTickMs, /*activityPending*/ true));   // 14.75 s banked
    EXPECT_EQ(0, (int) t.accumulatorMs);  // banked time discarded, not carried

    // Without the restart, the very next tick would have found 15 s banked.
    EXPECT_FALSE(t.tick(kTickMs));
    for (int i = 0; i < 58; i++)
        t.tick(kTickMs);
    EXPECT_EQ(1, t.refreshes);            // still just the flag-driven one
    EXPECT_TRUE(t.tick(kTickMs));         // a full interval later
}

TEST(ActivityRefresh, AnUnknownRefreshTypeBehavesLikeBoth)
{
    // A garbled preference must not leave the window frozen: fail towards
    // refreshing, not towards silence.
    ActivityTicker flag{7, 15};
    EXPECT_TRUE(flag.tick(kTickMs, /*activityPending*/ true));

    ActivityTicker clock{7, 15};
    for (int i = 0; i < 59; i++)
        EXPECT_FALSE(clock.tick(kTickMs));
    EXPECT_TRUE(clock.tick(kTickMs));
}

TEST(ActivityRefresh, ABackwardsClockDoesNotRewindTheAccumulator)
{
    // The elapsed time is wall clock (as it is on Windows), so a DST shift or a
    // time-server correction can hand us a negative interval.  It must cost
    // nothing, not push the next refresh minutes into the future.
    ActivityTicker t{kActivityRefreshPeriodically, 15};

    for (int i = 0; i < 59; i++)
        t.tick(kTickMs);
    EXPECT_EQ(14750, (int) t.accumulatorMs);

    EXPECT_FALSE(t.tick(-5000));
    EXPECT_EQ(14750, (int) t.accumulatorMs);

    EXPECT_TRUE(t.tick(kTickMs));
}

// ── world.Redraw / world.Repaint actually drain ─────────────────────────────
//
// Both were no-ops in the headless engine: platform/windows_stubs.cpp defined
// Redraw as `{}` under a comment claiming its real body was "mushview.cpp
// (mushapp-only)", and platform/doc_gui_method_stubs.cpp did the same for
// Repaint.  Both halves of that claim were wrong.  The real bodies are
// methods_output.cpp:50 and :509, and each opens with UpdateAllViews (NULL) —
// which is itself the REAL override in doc_display_shared.cpp:175, compiled
// into mushcore all along.  Only Repaint's trailing CMUSHView walk is GUI-bound.
//
// The consequence was that a script which wrote output and then asked for it to
// be shown got nothing until some unrelated drain happened to run.
//
// The discriminator here is feed-WITHOUT-drain, then Redraw: against the `{}`
// bodies the view never sees the line.
TEST(PalWorldView, RedrawAndRepaintDrainPendingOutput)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    // Feed but deliberately do NOT call UpdateAllViews — this is the state a
    // script is in between a Note and its redraw request.
    const char* bytes = "pending line\n";
    doc.DisplayMsg((char*) bytes, (int) strlen(bytes), 0);
    ASSERT_EQ(0u, (unsigned) committed(view).size());   // fixture precondition

    doc.Redraw();
    auto after = committed(view);
    ASSERT_EQ(1u, (unsigned) after.size());
    EXPECT_STREQ("pending line", line_text(after[0]).c_str());

    // Repaint takes the same path (its real body opens with the same call), and
    // must likewise not be a no-op.
    const char* more = "second line\n";
    doc.DisplayMsg((char*) more, (int) strlen(more), 0);
    ASSERT_EQ(1u, (unsigned) committed(view).size());   // still undrained
    doc.Repaint();
    ASSERT_EQ(2u, (unsigned) committed(view).size());
    EXPECT_STREQ("second line", line_text(committed(view)[1]).c_str());
}

// ── the drain and the fetch build the SAME line ───────────────────────────
//
// doc_display_shared.cpp used to carry THREE hand-written copies of the same
// CLine -> SOutputLine run loop -- BuildOutputLine (the pull path) plus one for
// the drain's committed lines and one for its pending row -- while the comment
// above BuildOutputLine claimed there was one, "two copies of this would be the
// twin drift the port keeps paying for elsewhere".  They had already drifted:
// neither drain copy stamped userInput, so an echoed command reached the view
// as ordinary MUD output on the push path and as input on the pull one, and the
// line-preamble family picks its text and colours by exactly that flag.
TEST(PalWorldView, EchoedCommandIsMarkedAsInputOnEveryPath)
{
    FakeWorldView view;
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pWorldView = &view;

    // A completed echoed command, through the push drain.
    doc.DisplayMsg("look\n", 5, USER_INPUT);
    doc.UpdateAllViews(nullptr);
    auto pushed = committed(view);
    ASSERT_EQ(1u, (unsigned) pushed.size());
    EXPECT_STREQ("look", line_text(pushed[0]).c_str());
    EXPECT_TRUE(pushed[0].userInput);

    // ...and the same line fetched through the PULL path must agree.
    SOutputLine fetched;
    ASSERT_TRUE(doc.BuildOutputLine(0, fetched));
    EXPECT_TRUE(fetched.userInput);

    // An ordinary MUD line is the control: if the flag were simply stamped on
    // everything, this would fail.
    doc.DisplayMsg("You see nothing special.\n", 25, 0);
    doc.UpdateAllViews(nullptr);
    auto both = committed(view);
    ASSERT_EQ(2u, (unsigned) both.size());
    EXPECT_FALSE(both[1].userInput);

    // The in-progress row is built by the same converter now, so it carries the
    // flag too -- and its arrival time, which only the pending copy used to set.
    view.lines.clear();
    doc.DisplayMsg("say hello", 9, USER_INPUT);   // no newline: still pending
    doc.UpdateAllViews(nullptr);
    ASSERT_FALSE(view.lines.empty());
    const SOutputLine& pending = view.lines.back();
    ASSERT_TRUE(pending.pending);
    EXPECT_STREQ("say hello", line_text(pending).c_str());
    EXPECT_TRUE(pending.userInput);
    EXPECT_NE((time_t) 0, pending.arrived);
}

MC_TEST_MAIN()
