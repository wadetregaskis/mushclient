// tests/test_pal_worldview.cpp
// First PAL prototype + characterization test.
//
// Proves pal::IWorldView is a real, implementable interface by wiring a
// recording implementation (the "headless test frontend") to the engine's
// existing output path (m_fn_line_ready), then pinning the current DisplayMsg
// styled-output behaviour as a golden — the safety net for later rewiring the
// output path onto IWorldView directly.

#include "mc_test.h"
#include "platform/doc_stub.h"
#include "pal/iworldview.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// ── the headless test frontend ─────────────────────────────────────────────
struct FakeWorldView : pal::IWorldView {
    std::vector<SOutputLine> lines;
    std::vector<std::string> notes;
    std::string title, status;
    bool cleared = false, paused = false, flashed = false;
    pal::Colour bg = 0;
    int maxLines = 0, repaints = 0;

    void appendLine(SOutputLine&& l) override { lines.push_back(std::move(l)); }
    void note(std::string_view t) override    { notes.emplace_back(t); }
    void setTitle(std::string_view t) override { title.assign(t); }
    void setStatus(std::string_view t) override { status.assign(t); }
    void setBackgroundColour(pal::Colour c) override { bg = c; }
    void setMaxLines(int n) override { maxLines = n; }
    void requestRepaint(pal::Rect) override { ++repaints; }
    void clearOutput() override { cleared = true; lines.clear(); }
    void setPaused(bool p) override { paused = p; }
    void flash() override { flashed = true; }
    std::string    cmd;                     // returned by currentCommand()
    int            scroll = 0;              // returned by scrollPos()
    pal::TextRange sel;                     // returned by outputSelection()
    std::string    currentCommand() const override { return cmd; }
    pal::TextRange outputSelection() const override { return sel; }
    int            scrollPos() const override { return scroll; }
    pal::Size      outputAreaChars() const override { return { 80, 25 }; }

    // ── the command input bar (P2 command family) ──────────────────────────
    std::vector<std::string> history;       // newest last, like the wx frame's
    pal::Range cmdSel;                      // the bar's selection
    bool selectedAll = false;
    std::string fontFace; int fontPts = 0; bool fontBold = false, fontItalic = false;

    bool setCommand(std::string_view text) override {
        if (!cmd.empty()) return false;     // Windows contract: only if empty
        cmd.assign(text);
        return true;
    }
    void selectCommand() override { selectedAll = true; }
    pal::Range commandSelection() const override { return cmdSel; }
    void setCommandSelection(long first, long last) override {
        long len = (long)cmd.size();
        cmdSel.start = (int)((first <= 0) ? len : first - 1);
        cmdSel.end   = (int)((last  <= 0) ? len : last);
    }
    std::string pushCommand() override {
        std::string text = cmd;
        if (!text.empty() && (history.empty() || history.back() != text))
            history.push_back(text);
        cmd.clear();
        return text;
    }
    std::vector<std::string> commandList(long count) const override {
        std::vector<std::string> out;
        long total = (long)history.size();
        long n = (count <= 0) ? total : std::min(count, total);
        for (long i = total - 1; i >= total - n; --i)
            out.push_back(history[(size_t)i]);
        return out;
    }
    void addToCommandHistory(std::string_view text) override {
        if (text.empty()) return;
        std::string entry(text);
        if (!history.empty() && history.back() == entry) return;
        history.push_back(entry);
    }
    void deleteCommandHistory() override { history.clear(); }
    void setInputFont(std::string_view face, int pointSize,
                      bool bold, bool italic) override {
        fontFace.assign(face); fontPts = pointSize;
        fontBold = bold; fontItalic = italic;
    }

    // ── output-pane management (P2 output family) ──────────────────────────
    int  batchBegins = 0, batchEnds = 0;
    long deletedLines = 0;
    long scrolledTo = -999;
    int  selSL = 0, selEL = 0, selSC = 0, selEC = 0;
    std::string outFontFace; int outFontPts = 0;
    bool outFontBold = false, outFontItalic = false;
    int  textRectChanges = 0;
    pal::Rect outRect{ 10, 20, 640, 480 };   // returned by outputRect()

    void beginBatch() override { ++batchBegins; }
    void endBatch() override   { ++batchEnds; }
    void deleteLines(long n) override { deletedLines += n; }
    void setScroll(long line) override { scrolledTo = line; }
    void setOutputSelection(int sl, int el, int sc, int ec) override {
        selSL = sl; selEL = el; selSC = sc; selEC = ec;
    }
    void setOutputFont(std::string_view face, int pointSize,
                       bool bold, bool italic) override {
        outFontFace.assign(face); outFontPts = pointSize;
        outFontBold = bold; outFontItalic = italic;
    }
    void textRectChanged() override { ++textRectChanges; }
    pal::Rect outputRect() const override { return outRect; }
};

// ── helpers ─────────────────────────────────────────────────────────────────
static CMUSHclientDoc make_doc()
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_iFlags        = COLOUR_ANSI;
    doc.m_iForeColour   = WHITE;
    doc.m_iBackColour   = BLACK;
    doc.m_pCurrentLine  = new CLine();
    doc.AddStyle(COLOUR_ANSI, WHITE, BLACK, 0, static_cast<CAction*>(nullptr));
    return doc;
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
}

// ── tests ─────────────────────────────────────────────────────────────────
TEST(PalWorldView, InterfaceRecordsStyledOutput)
{
    FakeWorldView view;
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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

// With no view attached, the helpers fall back to the m_fn_* callbacks so any
// non-opted-in host is unaffected.
TEST(PalWorldView, StateHelpersFallBackToCallbacks)
{
    CMUSHclientDoc doc = make_doc();
    bool paused = false; std::string title, status;
    doc.m_fn_set_paused = [&](bool p) { paused = p; };
    doc.m_fn_set_title  = [&](const std::string& t) { title = t; };
    doc.m_fn_status     = [&](const std::string& s) { status = s; };

    doc.EmitPaused(true);
    doc.EmitTitle("t1");
    doc.EmitStatus("s1");
    EXPECT_TRUE(paused);
    EXPECT_STREQ("t1", title.c_str());
    EXPECT_STREQ("s1", status.c_str());
}

// Clear / background colour / max-lines / repaint drive the view via their
// Emit* helpers as well.
TEST(PalWorldView, CanvasHelpersDriveTheView)
{
    FakeWorldView view;
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();

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

// The output-pane family drives the view through its typed-only helpers,
// including Note()'s routing (view attached → note(), no stdout).
TEST(PalWorldView, OutputFamilyDrivesTheView)
{
    FakeWorldView view;
    CMUSHclientDoc doc = make_doc();
    doc.m_pWorldView = &view;

    // Note() goes to the view (the doc_portable path flushes + emits).
    doc.Note("a wild note appears");
    ASSERT_EQ(1u, (unsigned)view.notes.size());
    EXPECT_STREQ("a wild note appears", view.notes[0].c_str());

    doc.BeginOutputBatch();
    doc.EndOutputBatch();
    EXPECT_TRUE(view.batchBegins == 1 && view.batchEnds == 1);

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
    CMUSHclientDoc doc = make_doc();

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

MC_TEST_MAIN()
