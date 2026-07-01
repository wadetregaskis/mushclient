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

MC_TEST_MAIN()
