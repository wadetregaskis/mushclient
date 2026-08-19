// tests/fake_worldview.h — the recording pal::IWorldView test frontend.
// Extracted from test_pal_worldview.cpp (round 4) so other suites can drive
// engine paths that need a live view sink (SendTo eSendToCommand, NAWS, ...).

#pragma once

#include "pal/iworldview.h"
#include <algorithm>
#include <string>
#include <vector>

struct FakeWorldView : pal::IWorldView {
    std::vector<SOutputLine> lines;
    std::vector<std::string> notes;
    std::string title, status;
    bool cleared = false, paused = false, flashed = false;
    pal::Colour bg = 0;
    int maxLines = 0, repaints = 0;

    void appendLine(SOutputLine&& l) override { lines.push_back(std::move(l)); }
    void note(const char* t) override    { notes.emplace_back(t); }
    // Every setBookmark(lineIndex, set) the engine emitted, in order.
    std::vector<std::pair<long, bool> > bookmarks;
    void setBookmark(long lineIndex, bool set) override {
        bookmarks.push_back(std::make_pair(lineIndex, set));
    }
    void setTitle(const char* t) override { title.assign(t); }
    void setStatus(const char* t) override { status.assign(t); }
    void setBackgroundColour(pal::Colour c) override { bg = c; }
    void setMaxLines(int n) override { maxLines = n; }
    void requestRepaint(pal::Rect) override { ++repaints; }
    void clearOutput() override { cleared = true; lines.clear(); }
    void setPaused(bool p) override { paused = p; }
    bool isPaused() const override { return paused; }
    void flash() override { flashed = true; }
    std::string    cmd;                     // returned by currentCommand()
    int            scroll = 0;              // returned by scrollPos()
    pal::TextRange sel;                     // returned by outputSelection()
    std::string    currentCommand() const override { return cmd; }
    pal::TextRange outputSelection() const override { return sel; }
    int            scrollPos() const override { return scroll; }
    // Deliberately NOT the wrap-column default (80): a caller that silently
    // falls back to the wrap column instead of asking the view would pass a
    // fixture seeded at 80 x 25 without anyone noticing.  Settable so a future
    // consumer of the seam (currently dead, K-781) can be tested discriminately.
    pal::Size areaChars{ 61, 17 };
    pal::Size      outputAreaChars() const override { return areaChars; }

    // ── the command input bar (P2 command family) ──────────────────────────
    std::vector<std::string> history;       // newest last, like the wx frame's
    // Mirrors CWorldChildFrame::HistoryLimit(): both real IWorldView adapters
    // evict at the world's history-lines setting, so a fake with unbounded
    // history has no failing-capable test through the seam for that eviction.
    int historyCap = 1000;
    pal::Range cmdSel;                      // the bar's selection
    bool selectedAll = false;
    std::string fontFace; int fontPts = 0; bool fontBold = false, fontItalic = false;

    bool setCommand(const char* text) override {
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
    // Raw 0-based splice, as the real edit controls do.  Out-of-range offsets
    // are clamped rather than asserted so a buggy caller shows up as a wrong
    // string, not a crash.
    void replaceCommandSelection(long start, long end, const char* text) override {
        long len = (long)cmd.size();
        long s = std::max(0L, std::min(start, len));
        long e = std::max(s,   std::min(end,   len));
        std::string ins(text ? text : "");
        cmd = cmd.substr(0, (size_t)s) + ins + cmd.substr((size_t)e);
        cmdSel.start = cmdSel.end = (int)(s + (long)ins.size());
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
    void addToCommandHistory(const char* text) override {
        if (!text || !*text) return;
        std::string entry(text);
        if (!history.empty() && history.back() == entry) return;
        history.push_back(entry);
        if (historyCap > 0 && (int)history.size() > historyCap)
            history.erase(history.begin());
    }
    void deleteCommandHistory() override { history.clear(); }
    void setInputFont(const char* face, int pointSize,
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
    long deletedHeadLines = 0;
    void deleteHeadLines(long n) override { deletedHeadLines += n; }
    void setScroll(long line) override { scrolledTo = line; }
    void setOutputSelection(int sl, int el, int sc, int ec) override {
        selSL = sl; selEL = el; selSC = sc; selEC = ec;
    }
    // world.SetCommandWindowHeight.  `commandHeightFloor` makes the fake
    // decline anything shorter, the way both real adapters decline a height
    // that would leave a pane unusable, so the caller's error arm is reachable.
    int commandHeight      = 0;
    int commandHeightCalls = 0;
    int commandHeightFloor = 0;
    bool setCommandWindowHeight(int px) override {
        ++commandHeightCalls;
        if (px < commandHeightFloor) return false;
        commandHeight = px;
        return true;
    }

    void setOutputFont(const char* face, int pointSize,
                       bool bold, bool italic) override {
        outFontFace.assign(face); outFontPts = pointSize;
        outFontBold = bold; outFontItalic = italic;
    }
    void textRectChanged() override { ++textRectChanges; }
    pal::Rect outputRect() const override { return outRect; }

    // "Adjust size to width".  Records the text width the engine asked for;
    // `widthRefused` makes the fake decline, so the caller's error arm has a
    // way to be reached (the real adapters decline while maximised).
    int  outputWidthPixels = 0;
    int  outputWidthCalls  = 0;
    bool widthRefused      = false;
    bool setOutputWidthPixels(int textWidth) override {
        ++outputWidthCalls;
        if (widthRefused) return false;
        outputWidthPixels = textWidth;
        return true;
    }

    // ── world/main window management (P2 window family) ────────────────────
    std::string mainTitle;
    long mwX = -1, mwY = -1, mwW = -1, mwH = -1;
    int  windowStatus = 0;
    int  activations = 0;
    bool fullscreen = false;
    int  saves = 0;
    pal::Rect mainR{ 5, 6, 1024, 768 };   // returned by mainRect()
    // returned by mainClientRect(); deliberately NOT a multiple of the
    // m_gui_* grid, so a test cannot pass on the headless fallback by accident
    pal::Rect mainClientR{ 0, 0, 793, 517 };

    void setMainTitle(const char* t) override { mainTitle.assign(t); }
    void moveWorldWindow(long l, long t, long w, long h) override {
        mwX = l; mwY = t; mwW = w; mwH = h;
    }
    void setWindowStatus(int s) override { windowStatus = s; }
    void activateClient() override { ++activations; }
    bool isFullscreen() const override { return fullscreen; }
    bool saveWorld() override { ++saves; return true; }
    pal::Rect mainRect() const override { return mainR; }
    pal::Rect mainClientRect() const override { return mainClientR; }

    // ── miniwindow mouse routing ────────────────────────────────────────────
    // WindowAddHotspot's create-under-the-cursor re-detect lands here.
    int  mouseMoves = 0;
    long lastMouseMoveX = -1, lastMouseMoveY = -1;

    void mouseMoveMiniWindow(long x, long y) override {
        ++mouseMoves; lastMouseMoveX = x; lastMouseMoveY = y;
    }

    // ── the info bar (P2 infobar family) ────────────────────────────────────
    bool infobarVisible = false;
    int  infobarClears = 0;
    std::string infobarBuf;
    pal::Colour infobarFg = 0, infobarBg = 0;
    std::string infobarFace; int infobarPts = 0, infobarStyle = 0;
    pal::Size infobarSz{ 0, 0 };   // returned by infobarSize()

    void infobarShow(bool visible) override { infobarVisible = visible; }
    void infobarClear() override { ++infobarClears; infobarBuf.clear(); }
    void infobarText(const char* t) override { infobarBuf.append(t); }
    void infobarColour(pal::Colour c) override { infobarFg = c; }
    void infobarBackground(pal::Colour c) override { infobarBg = c; }
    void infobarFont(const char* face, int pointSize, int style) override {
        infobarFace.assign(face); infobarPts = pointSize; infobarStyle = style;
    }
    pal::Size infobarSize() const override { return infobarSz; }
};

