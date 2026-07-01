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

    void appendLine(SOutputLine&& l) override { lines.push_back(std::move(l)); }
    void note(std::string_view t) override    { notes.emplace_back(t); }
    void setTitle(std::string_view t) override { title.assign(t); }
    void setStatus(std::string_view t) override { status.assign(t); }
    void setBackgroundColour(pal::Colour) override {}
    void setMaxLines(int) override {}
    void requestRepaint(pal::Rect) override {}
    void clearOutput() override { cleared = true; lines.clear(); }
    void setPaused(bool p) override { paused = p; }
    void flash() override { flashed = true; }
    std::string currentCommand() const override { return {}; }
    pal::Range  outputSelection() const override { return {}; }
    int         scrollPos() const override { return 0; }
    pal::Size   outputAreaChars() const override { return { 80, 25 }; }
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
    doc.m_fn_line_ready = [&](SOutputLine&& l) { view.appendLine(std::move(l)); };

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
    doc.m_fn_line_ready = [&](SOutputLine&& l) { view.appendLine(std::move(l)); };

    feed(doc, "alpha\nbeta\ngamma\n");

    auto lines = committed(view);
    ASSERT_EQ(3u, (unsigned)lines.size());
    EXPECT_STREQ("alpha", line_text(lines[0]).c_str());
    EXPECT_STREQ("beta",  line_text(lines[1]).c_str());
    EXPECT_STREQ("gamma", line_text(lines[2]).c_str());
}

MC_TEST_MAIN()
