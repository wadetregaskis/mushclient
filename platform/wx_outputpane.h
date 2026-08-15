// platform/wx_outputpane.h — Custom scrolled output pane for MUSHclient (macOS/Linux)
//
// COutputPane is a wxScrolledWindow subclass that renders MUD output as a
// sequence of styled lines.  Each line is a vector<SOutputRun> — the same
// data produced by the ANSI/MXP parser.
//
// Features:
//   • Per-run fg/bg colour, bold, underline using wxDC primitives
//   • Monospace font, configurable size (Ctrl+scroll to zoom)
//   • Capped scrollback buffer (m_maxLines, default 5000)
//   • Auto-scroll to bottom on new output; pauses when user scrolls up
//   • Character-precise click/drag text selection with clipboard copy
//   • Right-click context menu: Copy Selection, Select All, Clear
//   • Double-buffered paint via wxBufferedPaintDC
//   • I-beam cursor over text; focus returned to input bar after selection

#pragma once

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <deque>
#include <unordered_map>
#include <functional>
#include <string>
#include <vector>
#include <cassert>
#include "platform/output_types.h"   // SOutputLine, SOutputRun
#include "pal/style_colour.h"        // pal::StylePalette (RecolourFromPalette)

// ── SelPoint — cursor position within the text buffer ────────────────────────
// Represents a position between characters: col=0 is before the first
// character on a line, col=N is after the Nth character.
// line=-1 means "invalid / no selection".
struct SelPoint {
    int line = -1;
    int col  = 0;

    bool valid() const { return line >= 0; }

    bool operator==(const SelPoint& o) const {
        return line == o.line && col == o.col;
    }
    bool operator!=(const SelPoint& o) const { return !(*this == o); }
    bool operator<(const SelPoint& o) const {
        if (line != o.line) return line < o.line;
        return col < o.col;
    }
    bool operator<=(const SelPoint& o) const { return !(o < *this); }
};

// Decode engine output bytes to a wxString: UTF-8 worlds go straight through,
// otherwise the world's font charset with a Latin-1 fallback (so a failed
// conversion shows the text rather than dropping the run).  Shared with Print
// World, which decodes the doc's style bytes directly.
wxString DecodeOutputBytes(const char* bytes, size_t n, bool utf8, int charset);

// Decode bytes that are MEANT to be UTF-8 but may not be, substituting U+FFFD
// per ill-formed subpart the way MultiByteToWideChar(CP_UTF8, 0, ...) does on
// Windows.  Use this anywhere wxString::FromUTF8 would otherwise be handed bytes
// of unproven validity: FromUTF8 is all-or-nothing and returns an EMPTY string,
// so one bad byte silently discards the whole text.
wxString DecodeUTF8Lenient(const char* bytes, size_t n);

// FromUTF8 that never comes back empty for non-empty input: falls back to
// Latin-1 (From8BitData cannot fail).  This is the decode for UI CHROME —
// dialog fields, window titles, the status bar, list rows — where a
// legacy-charset string (a CP1252 world name or trigger text migrated from
// Windows) must stay visible and ROUND-TRIP through a control rather than
// become "" (which DDX then saves back over the member: silent data loss).
// DecodeUTF8Lenient above is for the OUTPUT pipeline, where Windows shows
// U+FFFD; here Windows' ANSI codepage shows the letters, and Latin-1 is the
// closest faithful reading.  NUL-safe: pass the true length.
wxString DecodeTextLenient(const char* bytes, size_t n);

// NUL-terminated convenience form (titles, hook text, CString casts).
inline wxString DecodeTextLenient(const char* sz)
{
    return sz ? DecodeTextLenient(sz, strlen(sz)) : wxString();
}

// Rendered character count of a run — the metric DrawLine measures with, so any
// walk that maps a column onto a run must use THIS and not run.text.size(),
// which is a BYTE count.
int RunCharLen(const SOutputRun& run);

// Rendered character count of a whole line: the sum of its runs' RunCharLen.
// Seven walks derived this independently, two of them skipping horiz_rule runs
// first -- a guard that is redundant, not a divergence, because a rule run
// carries no text at all (pinned in outputpane_selftest.cpp).  Having one body
// is what keeps "how long is this line" from drifting between the selection,
// the hit-test, the wrap cache and the API.
int LineCharLen(const SOutputLine& line);

// ── COutputPane ───────────────────────────────────────────────────────────────

class COutputPane : public wxScrolledWindow
{
public:
    // parent    — owning wxWindow
    // maxLines  — scrollback buffer size (default 5000, mirrors max_output_lines)
    explicit COutputPane(wxWindow* parent, int maxLines = 5000);

    // Append one styled line from the protocol engine.
    // Called from the wxTimer poll callback on the GUI thread.
    // When called inside a Begin/EndBatch pair, defers scrollbar update and
    // repaint until EndBatch().
    void AppendLine(SOutputLine&& line);

    // Append a plain note/status line.
    // fg is a COLORREF for the text colour; defaults to dim grey (0x808080).
    void AppendNote(const std::string& text, unsigned long fg = 0x808080);

    // Batch update: accumulate lines without repainting.
    void BeginBatch() { m_batchDepth++; }
    void EndBatch();

    // Remove all lines from the buffer and repaint.
    void ClearOutput();

    // ── Find / search ─────────────────────────────────────────────────────────
    // FindNext: jumps to the next/previous match and returns true if found.
    // *outMatchLine    receives the 0-based line index of the match (or -1).
    // *outOrdinal      receives the 1-based ordinal of the match among all hits.
    // *outTotalMatches receives a count of all matching lines (capped at 9999).
    // All three are computed in one linear scan to avoid redundant work.
    // caseSensitive: when true, the search is case-sensitive (default: false).
    bool FindNext(const wxString& term, bool forward = true,
                  int* outMatchLine    = nullptr,
                  int* outOrdinal      = nullptr,
                  int* outTotalMatches = nullptr,
                  bool caseSensitive   = false);
    void HighlightSearch(const wxString& term, bool caseSensitive = false);

    // Change the display font (e.g. from preferences or scripting API).
    void SetOutputFont(const wxFont& font);
    // ── Output-buffer fade ────────────────────────────────────────────────
    // Windows fades old text in CMUSHView::OnDraw: each line's opacity comes
    // from how long ago it arrived, and the colours are then run through
    // CMUSHclientDoc::TranslateColour (which is also what world.MapColour
    // works through).  Both live on the doc, so the pane asks rather than
    // reimplementing: give it these two and the fade is live.
    //   line opacity     — 1.0 = full intensity, down to the world's limit
    //   translate colour — (colour, opacity) -> colour
    std::function<double(time_t arrived)>            m_fn_line_opacity;
    std::function<uint32_t(uint32_t, double)>        m_fn_translate_colour;
    // Did the last paint see anything still fading?  Drives the repaint tick.
    bool AnyLineFading() const { return m_anyFading; }

    // ── Line preamble (timestamps — 2026-08-12) ───────────────────────────
    // Windows prefixes every output/input/note line with a formatted preamble
    // in its own colours (the nine timestamp_* world options; CMUSHView::
    // OnDraw's "line preamble" block).  The doc owns the options, the format
    // codes (%e elapsed, %D delta, strftime the rest) and the colours, so the
    // FRAME builds the preamble; the pane draws it in the plain font on the
    // line's first visual row and shifts the text right by its width.  This
    // pane is strictly monospace, so the preamble occupies whole character
    // cells — which keeps click-to-column exact (XToChar subtracts
    // PreambleCells on the first row; Windows' m_iPreambleOffset twin).
    // Returns false (or empty text) when the option is off for that kind.
    // Colours are raw COLORREFs: the pane routes them through the same
    // m_fn_translate_colour as run colours, so the fade and world.MapColour
    // apply to the preamble exactly as they do to the text (as on Windows,
    // which draws it through TranslateColour too).
    struct SLinePreamble { wxString text; uint32_t fg = 0; uint32_t bg = 0; };
    std::function<bool(const SOutputLine& line, time_t prevArrived,
                       bool isFirstLine, SLinePreamble& out)> m_fn_line_preamble;

    // Ctrl+click alias menu (2026-08-13): the pane detects the PHYSICAL Ctrl
    // modifier (RawControlDown -- the key Windows' VK_CONTROL test reads) and
    // reports the containment hit; the FRAME owns the doc and the wxMenu.
    // Consulted from BOTH button paths: macOS convention turns Ctrl+click
    // into a right-click, so the gesture can arrive as either event.
    std::function<void(int lineIdx, int col, wxPoint clientPos)> m_fn_alias_menu;
    // The preamble's width in character cells for a line (0 = none).
    int PreambleCells(int lineIdx) const;
    // Selftest probe: render one logical line into an arbitrary DC exactly as
    // OnPaint would.  The paint path never runs under the OnInit-scoped
    // harnesses (CLAUDE.md's structural hazard), so pixel assertions call the
    // engine-shaped function directly — the composite-selftest pattern.
    void RenderLineForTest(wxDC& dc, int lineIdx, int yOff);

    // World option line_spacing: the row pitch in pixels, or 0 to derive it
    // from the font's own height.  Windows applies this in ChangeFont
    // (doc.cpp:1179, `m_FontHeight = iLineSpacing`); off Windows the pane owns
    // the row pitch, so it has to be told.
    void SetLineSpacing(int px);
    int  LineSpacing() const { return m_lineSpacing; }

    // Read-only access to the current display font (used by scripting callbacks
    // to build a modified font to pass back into SetOutputFont).
    const wxFont& GetCurrentFont() const { return m_font; }

    // Character ranges of the current search term within one line, as the paint
    // path computes them.  Public so the in-app selftest can pin the two paths
    // against each other: the fast one works on raw UTF-8 bytes and is only
    // valid where byte offsets equal character offsets, and these offsets drive
    // where the highlight rectangle is drawn -- so a byte offset leaking out on
    // a non-ASCII line puts the box on the wrong characters.
    struct HighlightHit { int start, end; };
    void HighlightHitsForLine(int lineIdx, std::vector<HighlightHit>& out);

    // Re-resolve every buffered run's colours against a new palette, in place.
    //
    // The alternative is what the port used to do: have the engine re-emit the
    // entire scrollback (RebuildOutputView).  That reallocates every run's
    // string and rebuilds the whole wrap cache to change nothing but two
    // integers per run — O(buffer) work on a path world.SetNormalColour can
    // reach, and unbounded once max_output_lines is 0.  Colours cannot affect
    // wrapping, so this needs no cache work at all: walk, re-resolve, repaint.
    void RecolourFromPalette(const pal::StylePalette& palette);

    // Update the scrollback buffer cap.
    void SetMaxLines(int n);
    // Remove up to `count` lines from the tail of m_lines (most-recent output).
    // Called when Lua scripts call world.DeleteLines(n).
    void DeleteTailLines(long count);
    // Remove `count` lines from the FRONT (oldest).  Clear Scrollback and
    // Clear to Previous Mark, after the engine has freed the same lines.
    void DeleteHeadLines(long count);

    // Adjust font size by delta points.  Returns new size.
    int AdjustFontSize(int delta);
    void ResetFontSize();
    int FontSize() const { return m_fontSize; }

    static constexpr int kDefaultFontSize = 12;
    static constexpr int kMinFontSize     = 6;
    static constexpr int kMaxFontSize     = 32;

    // Copy the selected text to the clipboard.
    void CopySelection();

    // Copy the selection to the clipboard as HTML markup (Display ▸ Copy as
    // HTML).  Mirrors the canonical CMUSHView::OnEditCopyashtml: emits a
    // <table>/<pre><code> block with per-run <font>/<span> colour markup and
    // places that markup *as text* on the clipboard.  No-op if no selection.
    // fontName feeds the <font face=...> list (the world's display font).
    void CopySelectionAsHTML(const wxString& fontName);

    // Public accessor for the current selection text (Go To URL / Send Mail To
    // / Highlight Word reimplementations in CWorldChildFrame).  Empty if none.
    wxString GetSelectionText() const { return SelectedText(); }

    // Mirror the engine's BOOKMARK flag for line `lineIdx` (0-based, the same
    // space the doc's CLine list uses); shows a left-margin marker.  Driven
    // ONLY from CMUSHclientDoc::Bookmark through pal::IWorldView::setBookmark
    // -- the doc owns the flag, this is the rendering copy.  (It used to be an
    // independent store the Display menu toggled directly, so scripts and the
    // menu could not see each other's bookmarks.)
    void SetBookmark(int lineIdx, bool set);
    // Line the Display ▸ Bookmark Selection command acts on: the selection's
    // start line, else the last line.  0-based; -1 when the buffer is empty.
    int  BookmarkTargetLine() const;
    // Display ▸ Go To Bookmark — scroll to the next bookmarked line (cycling).
    void GotoNextBookmark();

    // Programmatically select a range of text.  Arguments are 1-based line and
    // column numbers, matching the MUSHclient SetSelection() Lua API.
    // Pass all-zero arguments to clear the selection.
    void SetSelectionRange(int startLine, int endLine, int startCol, int endCol);

    // Select all text in the buffer.
    void SelectAll();

    // Whether the Edit ▸ Select All command should act on this pane rather
    // than the command bar — a stand-in for "this view has focus" (Windows'
    // real routing test), since nothing ever gives the pane real wx focus.
    // Set by every real click in OnLeftDown; given up by ClearSelectAllClaim,
    // called on a genuine click or keystroke in the command bar.  See the
    // usage in CWorldChildFrame::EditSelectAll.
    bool WantsSelectAll() const { return m_wantsSelectAll; }
    void ClearSelectAllClaim() { m_wantsSelectAll = false; }

    // Scroll to the very bottom of the buffer and re-engage auto-scroll.
    // Safe to call at any time; no-op if already at the bottom.
    void ScrollToBottom();

    // Programmatically pause (disengage auto-scroll) or unpause (re-engage).
    // Mirrors world.Pause() scripting API.  When pausing, the scroll position
    // is not changed so the user sees the same content; new lines are still
    // appended to the buffer but will not scroll into view.
    void SetPaused(bool paused);

    // Scroll to a specific 0-based line index.  Clamps to [0, lineCount-1].
    // Disengages auto-scroll (same as the user manually scrolling up).
    void ScrollToLine(int lineIdx);
    // Scroll so the given device-pixel Y is the view top (world.SetScroll's
    // real contract; see the .cpp).  Disengages auto-scroll like ScrollToLine.
    void ScrollToPixelY(long y);

    // Return the current scroll position as a y-pixel offset from the top of
    // the virtual (full scrollback) area.  Equivalent to CScrollView::
    // GetScrollPosition().y on Windows; used to implement GetInfo(296).
    long GetScrollPositionY() const;

    // Scroll to the very top of the buffer (disengages auto-scroll).
    // Safe to call at any time; no-op if already at the top.
    void ScrollToTop();

    // 0-based index of the topmost LOGICAL line with any part of it on screen —
    // the cut point for Clear Scrollback, which frees exactly what has scrolled
    // out of sight above the viewport.  A wrapped line only partly visible
    // counts as visible, so the command never takes half of one.  Returns 0
    // when the buffer is empty or already scrolled to the top.
    // Adopt character metrics measured from a live paint DC; returns true when
    // they differ from what the pane is using and a reflow is needed.  Public
    // because OnPaint is unreachable from the test suite -- see the body.
    bool AdoptMeasuredMetrics(int dcW, int dcH);
    // The reflow that must follow a true from the above.
    void ApplyMetricsChange();

    // Re-read the plugin text rectangle (world.TextRectangle) and store it;
    // returns true when it CHANGED, i.e. when the text band moved or resized
    // and the wrap cache has to be rebuilt.  Same split, and for the same
    // reason, as AdoptMeasuredMetrics: this is the only thing in the pane that
    // notices a script narrowed the band, and it sat inside OnPaint, which no
    // in-app selftest can reach.  Public so the test can call it.
    bool AdoptTextRect();
    // The two TextRectangle decoration stages (fill under the background
    // image, border over it — the Windows OnDraw order).  Callers own the
    // device-origin; these draw in CLIENT coordinates.
    void DrawTextRectOutsideFill(wxDC& dc, int clientW, int clientH,
                                 const wxColour& bgCol);
    void DrawTextRectBorder(wxDC& dc);

    // ── ON_PLUGIN_DRAW_OUTPUT_WINDOW re-entrancy window ───────────────────
    // OnPaint fires the plugin draw callback in the middle of the paint, which
    // runs arbitrary plugin script -- and script prints (Quow-class status
    // plugins do it on their error paths).  Anything appended while this window
    // is open is deferred to the next event-loop turn rather than mutating
    // m_lines / m_lineChars / m_visualRowStart / m_totalVisualRows underneath
    // the draw loop that is about to walk them.
    //
    // Public for the same reason as AdoptMeasuredMetrics / AdoptTextRect:
    // OnPaint needs an event loop and every in-app selftest exits from
    // wxApp::OnInit before there is one, so while the flag was private to the
    // paint handler the whole defer-and-replay branch was unreachable -- and
    // silently losing a plugin's message is as bad as the crash it prevents.
    //
    // A depth COUNT, not a flag: a plugin callback that forces a synchronous
    // repaint re-enters OnPaint, and the inner window's close must not reopen
    // the buffer to the outer callback still running above it.
    void BeginPluginDraw() { ++m_pluginDrawDepth; }
    // No clamp: an End without a Begin is a bug in the caller, and silently
    // absorbing it would leave the buffer open for the rest of the session --
    // every subsequent append deferred, for ever.  Better to trip here.
    void EndPluginDraw()   { assert(m_pluginDrawDepth > 0); --m_pluginDrawDepth; }
    bool InPluginDraw() const { return m_pluginDrawDepth > 0; }

    // RAII form -- what OnPaint uses, so an early return out of the paint (or
    // a throw through the plugin callback) cannot leave the pane deferring
    // every append for the rest of the session.
    class PluginDrawScope {
    public:
        explicit PluginDrawScope(COutputPane& pane) : m_pane(pane)
        { m_pane.BeginPluginDraw(); }
        ~PluginDrawScope() { m_pane.EndPluginDraw(); }
        PluginDrawScope(const PluginDrawScope&)            = delete;
        PluginDrawScope& operator=(const PluginDrawScope&) = delete;
    private:
        COutputPane& m_pane;
    };

    // Fire m_fn_draw_output_window with that window held open -- OnPaint's one
    // line, extracted so a test can prove the callback really runs INSIDE the
    // guard.  The pairing is the part that rots (a later edit moves the call
    // out from under the scope and nothing notices until a plugin crashes a
    // user's paint), and it cannot be observed from outside OnPaint otherwise.
    void FirePluginDraw(int firstVisiblePaneLine, int pixelOffset);

    // The rectangle AdoptTextRect last stored -- what the NEXT call compares
    // against.  Read-only, for the in-app selftest: nothing else can observe
    // it, and "the values were stored" is exactly what a broken adopt (one
    // that returns the right bool but keeps the old rectangle) gets wrong.
    bool AdoptedTextRect(int& l, int& t, int& r, int& b) const
    { l = m_trL; t = m_trT; r = m_trR; b = m_trB; return m_trActive; }

    // What a wheel event should do, decided before anything is done about it.
    enum class WheelAction { HotspotTookIt, Zoom, Scroll };

    // The ORDERING rule, as a pure decision: a miniwindow hotspot that consumed
    // the wheel wins outright, and only then is Ctrl considered.  Windows orders
    // it that way and this shipped the other way round once (see OnMouseWheel),
    // which made every Ctrl+wheel plugin callback unreachable while the pointer
    // was over a miniwindow.  Static and wx-free so the rule is testable without
    // an event loop.
    static WheelAction DecideWheelAction(bool hotspotTookIt, bool rawCtrlDown);

    // The run covering character column `col` of `line`, or nullptr when the
    // column falls past the end of the line (or is negative).  `col` is a
    // RENDERED character column -- RunCharLen space, the same axis the
    // selection, the wrap cache and DrawLine use -- and *colWithinRun, when
    // asked for, receives the offset of that column inside the returned run.
    //
    // Five walks derived this by hand; a zero-length run can never contain a
    // column, so a horiz_rule run is skipped without needing a guard.
    static const SOutputRun* RunAtCharColumn(const SOutputLine& line, int col,
                                             int* colWithinRun = nullptr);

    int FirstVisibleLine() const;

    // How many lines the pane's DERIVED data actually holds -- the wrap cache's
    // length.  Read-only, for the in-app selftest.
    //
    // This is THE invariant of the pull model: the pane owns no content, only
    // what it derives from the engine's line set, so the two must be the same
    // length.  Nothing else can see a violation.  LineCount() reports the
    // ENGINE's count when the pull seam is wired, so it agrees with the engine
    // by construction and stays right even when the wrap cache has drifted --
    // and a drifted wrap cache is wrong scroll geometry, wrong
    // FirstVisibleLine, and a Clear Scrollback that cuts in the wrong place.
    int DerivedLength() const { return (int) m_visualRowStart.size(); }

    // Clear Screen (⌃⌘L): scroll the last line to the TOP of the viewport,
    // erasing nothing.  Opens a blank region below it to scroll into, which
    // the output that follows then fills — Terminal.app's overscroll, and the
    // one clear that costs no scrollback.  Returns false when the buffer is
    // empty or the viewport cannot be measured.
    bool ClearScreen();

    // Blank rows currently reserved below the last line.  Read-only, for the
    // in-app selftest: the effect of ClearScreen is otherwise only visible as
    // a scroll position, which is exactly what a broken implementation also
    // produces.
    long long OverscrollRows() const { return m_overscrollRows; }

    // Scroll one page up (direction=-1) or down (+1).
    // Updates the auto-scroll state accordingly.
    void ScrollPage(int direction);

    // Line height / char width in pixels (updated on font change).
    int LineHeight() const { return m_lineH; }
    int CharWidth()  const { return m_charW; }

    // Convert a client-space point to a buffer position (logical line + char
    // column).  Wrap-aware: correctly resolves which visual row within a
    // wrapped logical line was hit.  Returns an invalid SelPoint if the buffer
    // is empty.  Exposed for callers (e.g. CWorldChildFrame tooltip handler)
    // that need to map pixel coordinates to text without owning a wxMouseEvent.
    SelPoint PointToChar(int clientX, int clientY, bool halfway = true) const;

    // Number of lines currently in the buffer.
    int LineCount() const { return LineCountI(); }

    // True when the fetch seam is wired, i.e. the pane keeps no line content
    // of its own and reads through the engine (branch pull-model-worldview).
    bool PullModelActive() const { return (bool) m_fn_fetch_line; }

    // True while an in-progress (not yet newline-terminated) line is being shown
    // as the dim trailing row — the engine's m_pCurrentLine.  Read-only; exists
    // so the in-app selftest can pin that ClearOutput takes it away.
    bool HasPendingLine() const { return !m_pendingLine.empty(); }

    // Current wrap width in characters, i.e. what RebuildWrapCache resolved from
    // the client width, the text rectangle and the pixel offset.  Read-only;
    // the selftest uses it to observe world.TextRectangle's effect on the text.
    int WrapColumns() const { return m_wrapCols; }

    // Read-only access to a single buffered line.
    const SOutputLine* GetLine(int idx) const {
        if (idx < 0 || idx >= LineCountI()) return nullptr;
        const SOutputLine& l = LineAt(idx);
        return &l;
    }

    // Query the current output-pane text selection.
    // Returns true if there is a non-empty selection and fills the four
    // out-parameters with 1-based (line, col) values matching the Windows
    // GetSelectionStartLine / GetSelectionEndLine / …Column API.
    // Returns false (and leaves parameters unchanged) if nothing is selected.
    bool GetSelectionInfo(long& startLine, long& startCol,
                          long& endLine,   long& endCol) const;

    // True while the pane is pinned to the bottom.
    bool IsAutoScrolling() const { return m_autoScroll; }
    bool IsPaused()        const { return !m_autoScroll; }

    // Recompute m_autoScroll from where the view actually is, and fire
    // m_fn_autoscroll_changed if it flipped.  EVERY user-scroll path (wheel,
    // scrollbar, keys, page up/down) funnels through here, so they cannot
    // disagree about what counts as "at the bottom" — they used to, four ways.
    // The predicate itself is pal::isScrolledToBottom (pal/scroll_geometry.h).
    void RefreshAutoScrollFromPosition();

    // Controls whether scrolling up engages the freeze (stops auto-scroll).
    // When false the pane always keeps auto-scrolling; new output is always
    // visible regardless of where the user has scrolled.  Mirrors the world's
    // m_bAutoFreeze field.  Default is true (freeze on scroll-up).
    void SetAllowAutoFreeze(bool allow) { m_allowAutoFreeze = allow; }

    // World option keep_pause_at_bottom (m_bKeepFreezeAtBottom): once the pane
    // is frozen, scrolling back down to the bottom must NOT resume output — the
    // user unfreezes explicitly.  That is the whole purpose of the option, and
    // it is a LATCH, not a state: Windows writes
    //   m_freeze = m_freeze || (pt.y < bottom)     (mushview.cpp:4680-4685)
    // instead of the plain assignment it uses when the option is off.
    void SetKeepPauseAtBottom(bool v) { m_keepPauseAtBottom = v; }

    // Windows freezes output the moment a selection exists, so the text the user
    // is about to copy or print stops scrolling away under the cursor:
    //   if (!get_selection (newrgn) && pDoc->m_bAutoFreeze)
    //      m_freeze = true;   // freeze output so they can copy or print it
    // (get_selection returns TRUE for an EMPTY region, so this is "non-empty
    // selection AND auto-freeze enabled".)  CMUSHView does it on every path that
    // ends a selection — LButtonUp, MouseMove during the drag, double-click —
    // and pal/win_worldview.cpp:639 repeats it for world.SetSelection.
    // Public so CWorldChildFrame::setOutputSelection can do the same.
    void FreezeIfSelection();

    // Whether to auto-copy text selection to the clipboard on mouse release.
    // Corresponds to world option copy_selection_to_clipboard.
    void SetCopySelectionToClipboard(bool v) { m_copySelToClip = v; }
    // copy_selection_to_clipboard_in_html: auto-copy uses HTML instead of text.
    void SetAutoCopyInHTML(bool v) { m_autoCopyInHTML = v; }

    // Word delimiters used for double-click word selection.
    // Characters in this string break words; defaults to ".,()[]\"\'" matching
    // the MUSHclient Windows default.
    void SetWordDelimitersDblClick(const std::string& delims) {
        m_wordDelimitersDblClick = delims;
    }

    // File > Preferences > Output.  "Bleed background colour to edge": the
    // trailing space on a line takes the LAST RUN's background rather than the
    // pane background, so a coloured line reaches the right margin
    // (CMUSHView::OnDraw does this on Windows).  "Smoother scrolling": repaint
    // synchronously after appending instead of waiting for the next idle paint
    // (MFC: Invalidate(); if (App.m_bSmootherScrolling) UpdateWindow();).
    void SetBleedBackground(bool v) { if (v != m_bleedBackground) { m_bleedBackground = v; Refresh(false); } }
    void SetSmootherScrolling(bool v) { m_smootherScroll = v; }
    bool BleedBackground() const { return m_bleedBackground; }

    // "Very smooth scrolling" (App.m_bSmoothScrolling), the stronger sibling of
    // the above: animate the scroll to the bottom a step at a time instead of
    // jumping, so you can see which way the text moved.  Windows does this in
    // CMUSHView::ScrollToPosition (mushview.cpp:4638-4653), one PIXEL per step;
    // this pane's scroll unit is a whole text row (SetScrollRate(0, m_lineH)),
    // so it steps by rows.  Same effect, coarser grain -- true per-pixel would
    // mean rescaling every scroll-unit calculation in this file.
    void SetSmoothScrolling(bool v) { m_smoothScroll = v; }

    // World Properties > Appearance > Output > "Pixel offset" (m_iPixelOffset):
    // inset the text from the left edge of the window, and leave the same gap
    // below the last line when scrolled to the bottom.  Windows applies it in
    // the wrap computation as well as the drawing (mushview.cpp:3220), so the
    // visible width shrinks by this many pixels -- see EffectiveTextCols in
    // wx_world_child.cpp, which MUST stay in step with RebuildWrapCache.
    void SetTextPixelOffset(int px);
    int  TextPixelOffset() const { return m_pixelOffset; }

    // Set the pane's default background colour (COLORREF, 0 = black).
    void SetPaneBackground(unsigned long col);
    unsigned long PaneBackground() const { return m_bgColor; }

    // ── Callbacks ─────────────────────────────────────────────────────────────

    // Called whenever font metrics (m_lineH, m_charW) change.
    std::function<void()> m_fn_grid_changed;

    // Called whenever the pane is resized (after the new client size is in
    // effect) — including layout-driven changes that don't resize the frame, so
    // the doc's GUI geometry (GetInfo 280/281) stays current.
    std::function<void()> m_fn_resized;

    // Returns the tooltip text for a client-space point; "" = clear tooltip.
    std::function<std::string(int x, int y)> m_fn_get_tooltip;

    // Called when auto-scroll state changes (true = pinned to bottom).
    std::function<void(bool)> m_fn_autoscroll_changed;

    // Called after the user finishes a mouse selection (or clicks without
    // selecting).  Typically wired to re-focus the command input bar.
    std::function<void()> m_fn_return_focus;

    // Composite miniwindows onto the output DC.
    std::function<void(wxDC& dc, int clientW, int clientH, bool bUnder)>
        m_fn_composite_miniwindows;

    // Plugin text rectangle (world.TextRectangle): when a plugin reserves part
    // of the output area for a docked miniwindow, the text must avoid it.  The
    // frame wires this to the world's m_TextRectangle.  Returns true and fills
    // left/top/right/bottom (client pixels) when a rectangle is active; false
    // means use the full client area.
    std::function<bool(int& l, int& t, int& r, int& b)> m_fn_text_rect;

    // world.TextRectangle's five DECORATION parameters (2026-08-12): Windows
    // paints an outside fill and a border around the reserved area
    // (CMUSHView::OnDraw); the port honoured only the geometry.  Style codes
    // are ValidateBrushStyle's: 0 solid, 1 null (no fill), 2-7 hatches drawn
    // in the BORDER colour over an opaque ground of the fill colour, 8+
    // patterns (approximated as cross-hatch here).  Read live per paint.
    struct STextRectDecor {
        int      borderOffset     = 0;
        int      borderWidth      = 0;
        uint32_t borderColour     = 0;
        uint32_t outsideFillColour= 0;
        int      outsideFillStyle = 1;   // BS_NULL
    };
    std::function<bool(STextRectDecor&)> m_fn_text_rect_decor;
    // Selftest probe: run both decoration stages into an arbitrary DC (the
    // paint path never runs under the OnInit harnesses).
    void RenderTextRectDecorForTest(wxDC& dc, int clientW, int clientH,
                                    const wxColour& bgCol);

    // Draw the world's background / foreground images (SetBackgroundImage /
    // SetForegroundImage), placed per their mode by the frame. Background
    // paints right after the colour fill (under everything); foreground
    // after the text, before the on-top miniwindows — the Windows OnDraw
    // order. (Formerly a mac-view gap: the images loaded but never drew.)
    std::function<void(wxDC& dc, int clientW, int clientH)> m_fn_draw_background_image;
    std::function<void(wxDC& dc, int clientW, int clientH)> m_fn_draw_foreground_image;

    // Fired once per paint between the underneath-miniwindow composite and
    // the text pass — ON_PLUGIN_DRAW_OUTPUT_WINDOW (docs/PAL.md S4c), the
    // same point Windows' OnDraw fires it. Args: the first visible logical
    // line (PANE index, 0-based; the frame maps it to a 1-based doc line)
    // and the scroll offset in pixels. The callback runs plugin script
    // mid-paint; AppendLine defers list mutations while painting.
    std::function<void(int firstVisiblePaneLine, int pixelOffset)>
        m_fn_draw_output_window;

    // Miniwindow mouse routing.  Down/up return true if the event was
    // consumed.  Move returns the hotspot cursor code under the point
    // (CHotspot::m_Cursor, -1..12), 0 (arrow) over a miniwindow without a
    // hotspot, or -999 when not over a miniwindow — the pane then applies
    // its own text cursors.  (The -1..12 mapping mirrors Windows'
    // CMUSHView::OnSetCursor; docs/PAL.md S4.)
    std::function<bool(int x, int y, long flags)> m_fn_miniwindow_mouse_down;
    std::function<bool(int x, int y, long flags)> m_fn_miniwindow_mouse_up;
    std::function<int(int x, int y)>               m_fn_miniwindow_mouse_move;
    std::function<bool(int x, int y, int delta)>   m_fn_miniwindow_mouse_wheel;

    // Called when the user clicks an MXP hyperlink run.
    // action           the MXP/Lua link action (pipe-delimited alternatives)
    // actionType       the style's ACTIONTYPE bits (SOutputRun::action_type)
    // bCommentLine     the clicked line was a note, not MUD output
    // The last two decide what the click MEANS -- a prompt-link fills the
    // command bar, a send-link on a note line executes -- so they travel with
    // the action rather than being re-derived (or ignored) by the handler.
    std::function<void(const std::string& action,
                       int actionType,
                       bool bCommentLine)> m_fn_link_clicked;

    // Double-clicked a word in the output ("Double-click Sends Word" /
    // "…Pastes Word", both world options, both default OFF).  Fired ONLY when
    // the click actually resolved a word -- see OnLeftDClick -- so the handler
    // never has to distinguish a fresh word from a stale selection.  bShift is
    // passed because Windows treats Shift+double-click as a whole-paragraph
    // operation, which the port declines rather than approximating with a word.
    // Returns false for CMUSHclientDoc::eDblClickAbort (not connected and the
    // user cancelled the prompt) — Windows returns straight out of
    // OnLButtonDblClk there, skipping the selection tidy-up and the auto-copy
    // (mushview.cpp:4860), so the pane must skip the auto-copy too.
    std::function<bool(const wxString& word, bool bShift)> m_fn_word_double_clicked;

    // Called from the right-click context menu "Put in Input Bar" action.
    // Receives the currently selected text; the frame wires this to set the
    // input bar's value and move focus there for review before sending.
    std::function<void(const wxString& text)> m_fn_put_in_input;

    // Called from the right-click context menu "Send to MUD" action.
    // Receives the currently selected text; the frame wires this to call
    // EvaluateCommand() so the text is sent via the normal command pipeline.
    std::function<void(const wxString& text)> m_fn_send_to_mud;

    // Called once per appended line (after trimming/batching).
    // Wired by CWorldChildFrame to write to the session log file.
    std::function<void(const SOutputLine&)> m_fn_on_line_appended;

    // ── PULL MODEL (spike, branch pull-model-worldview) ───────────────────
    // Fetch committed line `i` from the engine.  When wired, the pane stops
    // KEEPING the lines it is pushed: AppendLine still receives one (that is
    // how it learns the line exists and how wide it is) but drops the payload
    // instead of storing it, and every read goes through here.
    //
    // The duplicate store is the point: the engine's CLine already holds the
    // line, so m_lines was a second full copy of the whole scrollback --
    // ~64 bytes per run plus its strings, against the 4 bytes per line the wrap
    // cache actually needs.
    std::function<bool(int i, SOutputLine& out)> m_fn_fetch_line;
    // How many COMMITTED lines the engine currently holds.  Authoritative; the
    // pane's own count is derived from it rather than the other way round.
    std::function<int()> m_fn_line_count;

    // Returns true if a log file is currently open (used by context menu label).
    std::function<bool()> m_fn_is_logging;

    // Toggles logging on/off: shows a file-save dialog when starting.
    std::function<void()> m_fn_toggle_log;

    // Display ▸ Clear Output Buffer, the canonical command (prompt, engine
    // buffer, pane, find state).  Ctrl+L and the context-menu item route here
    // rather than calling ClearOutput() directly: clearing the pane behind the
    // engine's back leaves world.GetLinesInBufferCount() counting the cleared
    // lines, and RebuildOutputView (any palette change) puts them all back.
    // Wired by CWorldChildFrame; if unwired, the pane-only clear is the
    // fallback so a pane with no frame is still usable.
    std::function<void()> m_fn_clear_output_buffer;

    // Called when F3 (forward) or Shift+F3 (backward) is pressed while the
    // output pane has focus.  Wired by CWorldChildFrame to DoFind().
    std::function<void(bool forward)> m_fn_find_next_key;

    // Called when the user types a printable character while the output pane
    // has focus (and no modifier key is held).  Wired by CWorldChildFrame to
    // append the character to the command input bar and focus it so the user
    // can keep typing without having to click back into the input bar.
    std::function<void(wxChar ch)> m_fn_key_to_input;

    // Called for every key-down event in the output pane before built-in
    // output-pane bindings are checked.  Return true to consume the event.
    // Wired by CWorldChildFrame to CWorldChildFrame::FireAcceleratorIfMatch.
    std::function<bool(wxKeyEvent&)> m_fn_accelerator_check;

private:
    // ── Paint ────────────────────────────────────────────────────────────────
    void OnPaint(wxPaintEvent& evt);
    void OnSize(wxSizeEvent& evt);

    // Render one logical line (all its visual rows).
    // lineIdx is the index in m_lines; yOff is the y-pixel of the FIRST
    // visual row of this line in virtual (unscrolled) coordinates.
    void DrawLine(wxDC& dc, int lineIdx, int yOff);

    // ── Wrap cache ────────────────────────────────────────────────────────────
    // m_visualRowStart[i] = index of the first visual row for logical line i.
    // m_totalVisualRows   = total visual rows across all logical lines.
    // m_wrapCols          = column count for wrapping (updated on resize/font change).
    //
    // Rebuild the wrap cache from scratch (called on resize and font change).
    void RebuildWrapCache();

    // Number of visual rows occupied by logical line lineIdx at m_wrapCols.
    int WrapCount(int lineIdx) const;

    // Convert a visual row index to its logical line index (binary search).
    int VisualRowToLogical(int vr) const;

    // Draw a single text chunk from a run with the given selection inversion.
    // Paint-scoped: the opacity DrawLine worked out for the line it is drawing,
    // read by DrawRunChunk.  A parameter would have to be threaded through five
    // call sites that all sit inside DrawLine itself; this is DC-like scratch
    // state, valid only between DrawLine setting it and returning.
    double m_paintOpacity = 1.0;

    void DrawRunChunk(wxDC& dc, const wxString& text, int x, int yOff,
                      const SOutputRun& run, bool selected);

    // ── Scroll ────────────────────────────────────────────────────────────────
    void OnScrollWinUpdate(wxScrollWinEvent& evt);
    void UpdateScrollbars(bool scrollToBottom);

    // ── Mouse / keyboard ─────────────────────────────────────────────────────
    void OnLeftDown(wxMouseEvent& evt);
    void OnLeftUp(wxMouseEvent& evt);
    void OnLeftDClick(wxMouseEvent& evt);
    void OnMiddleDown(wxMouseEvent& evt);
    void OnMiddleUp(wxMouseEvent& evt);
    void OnRightDown(wxMouseEvent& evt);
    void OnRightDClick(wxMouseEvent& evt);
    void OnLeaveWindow(wxMouseEvent& evt);
    void OnRightUp(wxMouseEvent& evt);
    void OnMotion(wxMouseEvent& evt);
    void OnMouseWheel(wxMouseEvent& evt);
    void OnKeyDown(wxKeyEvent& evt);

    // ── Context menu ─────────────────────────────────────────────────────────
    void OnMenuCopy(wxCommandEvent& evt);
    void OnMenuSelectAll(wxCommandEvent& evt);
    void OnMenuClear(wxCommandEvent& evt);
    // Dispatch the canonical clear-buffer command (or fall back to the
    // pane-only clear when no frame is wired).
    void ClearOutputBuffer();

    // ── Selection helpers ────────────────────────────────────────────────────

    // Convert client-coordinates y to a line index.
    int YToLine(int clientY) const;

    // Convert a line index + client-coordinates (x, y) to a character column.
    // clientY is required to resolve which visual row within a wrapped line was
    // clicked so that the column offset is computed correctly.
    // Column is a cursor position: 0 = before first char, n = after nth char.
    // halfway=true: caret semantics — round to the NEAREST cell boundary
    // (selection anchors/foci).  halfway=false: containment — the character
    // whose cell holds the pixel (link firing, hover cursor, hints).  The
    // exact bHalfWay split CMUSHView::calculate_line_and_column has; using
    // the rounding form for containment displaced every link hit zone half
    // a character left (2026-08-12).
    int XToChar(int lineIdx, int clientX, int clientY,
                bool halfway = true) const;
    bool TryAliasMenu(wxMouseEvent& evt);   // Ctrl+click → m_fn_alias_menu

    // Build a SelPoint from a mouse event position.
    SelPoint MouseToSelPoint(const wxMouseEvent& evt) const;

    // Fills start/end with the normalised (start <= end) selection.
    // Returns false if there is no selection (both points invalid or equal).
    bool GetNormalizedSel(SelPoint& start, SelPoint& end) const;

    // ── PULL MODEL: the read path ─────────────────────────────────────────
    // Committed line count.  The engine's when the pull seam is wired, the
    // pane's own deque otherwise, so both models share every caller.
    int  LineCountI() const;
    // How many lines the pane's DERIVED data -- wrap cache, char counts, and in
    // the push model the content deque -- should currently hold.  -1 means "no
    // limit, do not trim".
    //
    // This is the one asymmetry between the two models, and every trim goes
    // through it.  In the PUSH model the pane owns the line set and trims to its
    // own cap.  In the PULL model the ENGINE owns it: it has already evicted at
    // its own cap (RemoveChunk) or through DeleteHeadLines, and the pane simply
    // follows.  Trimming to m_maxLines there could never terminate -- popping
    // the pane's wrap cache does not change what LineCountI() reports, because
    // that number comes from the engine.
    int  DerivedTargetLength() const;
    // Drop `n` lines from the front of everything derived from the buffer, and
    // from the content deque if this build has one.  Shifts the selection down
    // with them.  Returns how many were actually dropped.
    long DropFrontLines(long n, bool bAccumulateTrimmedRows);
    // Line `i`, fetched and cached.  The reference is valid until the NEXT
    // LineAt for a different line evicts it, which the cache is sized to make
    // impossible within a paint (see kLineCacheMin) -- no caller holds two at
    // once, and that invariant is what keeps this safe.
    const SOutputLine& LineAt(int i) const;
    // Drop cached conversions.  Called whenever the engine's buffer moves under
    // us: a trim, a clear, a palette change, or a re-emit.
    void InvalidateLineCache() const;


    static const int kLineCacheMin = 512;   // >> any viewport's row count
    mutable std::unordered_map<int, SOutputLine> m_lineCache;
    mutable std::deque<int>                      m_lineCacheOrder;   // LRU, front = oldest

    // Rebuild selected text for the clipboard.
    wxString SelectedText() const;

    // ── Find ──────────────────────────────────────────────────────────────
    // Does line `i` contain `term`?  `scratch` is a caller-owned buffer reused
    // across the whole scan, so a search allocates once rather than once per
    // line.  Matches on the raw UTF-8 run bytes with ASCII case folding, which
    // is exact for an ASCII term against ASCII text; anything else (a non-UTF-8
    // world, or a high byte anywhere in the line under a case-INsensitive
    // search) falls back to the decode-and-Lower path so behaviour is
    // unchanged.  termLower must already be lower-cased when !caseSensitive.
    bool LineContains(int i, const wxString& term, const std::string& termLowerAscii,
                      bool caseSensitive, std::string& scratch) const;

    // Line indices matching m_findCacheTerm, ascending.  Rebuilt only when the
    // term, the case flag or the buffer generation changes -- so pressing Find
    // Next repeatedly costs one scan in total, not one per keypress, and the
    // "n of m" label is an upper_bound rather than a second full scan.
    void RebuildFindCache(const wxString& term, bool caseSensitive);

    // Bumped whenever m_lines changes shape.  Invalidates the find cache.
    unsigned long long m_bufferGen = 0;
    std::vector<int>   m_findCache;
    wxString           m_findCacheTerm;
    bool               m_findCacheCase = false;
    unsigned long long m_findCacheGen  = (unsigned long long) -1;
    bool               m_findCacheValid = false;

    // Put the current selection on the clipboard when the world asks for it
    // (copy_selection_to_clipboard, and …_in_html for the format).  Shared by
    // the drag path and the double-click path so they cannot drift: Windows
    // ends BOTH extend_selection (mushview.cpp:1982) and OnLButtonDblClk
    // (mushview.cpp:4884) with the same four lines.
    void AutoCopySelection();

    // ── State ────────────────────────────────────────────────────────────────
    std::deque<SOutputLine> m_lines;
    int                     m_maxLines;

    // In-progress (pending) line — the telnet engine's m_pCurrentLine.
    // Updated each timer tick via AppendLine(line with pending=true).
    // Rendered as a dim trailing row below the committed lines.
    // Cleared (empty) when there is no partial line in progress.
    SOutputLine m_pendingLine;

    wxFont m_font;
    // Cached font variants — rebuilt whenever m_font changes so DrawRunChunk
    // never constructs wxFont objects on the hot rendering path.
    // 8 variants covering all combinations of bold × italic × underline.
    wxFont m_fontBold;                  // bold
    wxFont m_fontUnderline;             // underline
    wxFont m_fontBoldUnderline;         // bold + underline
    wxFont m_fontItalic;                // italic
    wxFont m_fontBoldItalic;            // bold + italic
    wxFont m_fontItalicUnderline;       // italic + underline
    wxFont m_fontBoldItalicUnderline;   // bold + italic + underline

    int    m_fontSize = kDefaultFontSize;
    int    m_lineH    = 18;   // line height pixels (set from font metrics)
    int    m_lineSpacing = 0; // world option line_spacing; 0 = use the font's
    // True when the last paint saw a line still part-way through its fade, so
    // the frame knows to keep repainting.  Once every visible line has reached
    // the final opacity there is nothing left to animate and the ticking stops.
    bool   m_anyFading = false;
    int    m_charW    = 10;   // avg char width pixels

    int    m_maxLineChars = 80;   // widest line seen, in characters (stats only)

    // ── Wrap cache ────────────────────────────────────────────────────────────
    // Entries are ABSOLUTE row numbers on a monotonically increasing axis, and
    // m_rowBase is the absolute row of the first line still in the buffer.  A
    // head trim therefore only pops the front entry and re-reads the base — it
    // does not have to rewrite every remaining entry, which is what let the
    // at-capacity append path (i.e. the steady state of any long session) do a
    // full-buffer rebuild for every single line of output.  Read them through
    // RowStartOf(), never directly.
    std::deque<long long> m_visualRowStart;
    long long             m_rowBase         = 0;
    // 64-bit: at a 15px row, rows * m_lineH in int wraps negative past
    // ~143M rows, which max_output_lines = 0 (unlimited) makes reachable.
    // Every product with m_lineH goes through pal::rowsToPixels.
    long long             m_totalVisualRows = 0;
    int                   m_wrapCols        = 80;
    // Blank rows below the last line, opened by Clear Screen (⌃⌘L) so the last
    // line can be scrolled to the TOP of the viewport.  Part of the scrollable
    // extent only: no line maps into it, so it paints as background and every
    // pixel-to-line mapping ignores it.  Consumed row for row by the output
    // that follows, and cleared outright by anything that rebuilds the buffer.
    long long             m_overscrollRows  = 0;

    // Rendered character count per line, parallel to m_lines.  AppendLine
    // already computes this to update m_maxLineChars; caching it turns
    // WrapCount() into one integer division instead of a re-scan of every run
    // (a UTF-8 walk, or a whole wxString decode + allocation per run in a
    // non-UTF-8 world) and so turns RebuildWrapCache from O(total bytes) into
    // O(lines) integer arithmetic.
    std::deque<int>       m_lineChars;

    // First visual row of logical line i, relative to the top of the buffer.
    int RowStartOf(int i) const {
        return (i >= 0 && i < (int) m_visualRowStart.size())
               ? (int) (m_visualRowStart[i] - m_rowBase) : 0;
    }
    // Drop the front line's cache entries, keeping the row axis consistent.
    void PopFrontWrapCache();

    // ── Text rectangle (world.TextRectangle) ────────────────────────────────────
    // Resolved client-pixel rect that the text must stay within (origin offset
    // + wrap width).  m_trActive is false when no rectangle is set (full width).
    // Returns the resolved values via m_fn_text_rect, clamped to the client.
    bool TextRect(int& l, int& t, int& r, int& b) const;
    // Height (client px) of the band the text is allowed to occupy: the text
    // rectangle's height when one is set, else the full client height.  Used by
    // the scroll/autoscroll math so the newest line sits at the rectangle's
    // bottom rather than the window's.
    int  TextViewportH() const;
    bool m_trActive = false;             // last-seen rect state (change detection)
    int  m_trL = 0, m_trT = 0, m_trR = 0, m_trB = 0;

    // Selection: anchor is where the mouse went down; focus tracks the drag.
    // When anchor == focus, there is no selection (just a cursor position).
    SelPoint m_selAnchor;
    SelPoint m_selFocus;

    // See WantsSelectAll().
    bool m_wantsSelectAll = false;

    // Index of the last bookmark jumped to, so Go To Bookmark cycles forward.
    int m_lastBookmarkLine = -1;

    wxString m_searchTerm;
    // The term already case-folded, so the paint path does not refold it per
    // line, and a scratch buffer reused across lines so the highlight scan
    // allocates nothing per line.  Both maintained by HighlightSearch.
    wxString    m_searchTermLower;
    std::string m_hlScratch;
    bool     m_searchCaseSensitive = false;

    unsigned long m_bgColor  = 0;   // COLORREF
    int           m_batchDepth = 0;

    bool        m_dragging   = false;
    int         m_dragStartY = 0;

    // Stored at mouse-down time when the cursor is over a link run.
    // Fired as a link click on mouse-up only if no drag occurred.
    std::string m_linkAtDown;
    int         m_linkTypeAtDown = 0;      // its run.action_type
    bool        m_linkNoteAtDown = false;  // its line was a note (COMMENT)

    bool m_autoScroll      = true;
    // Nesting depth of the mid-paint plugin callback window
    // (ON_PLUGIN_DRAW_OUTPUT_WINDOW): AppendLine defers mutations of m_lines
    // while it is open, so the pass the paint is iterating stays stable even
    // if the script prints (docs/PAL.md S4c re-entrancy guard).  See
    // BeginPluginDraw for why it counts rather than flags.
    int  m_pluginDrawDepth = 0;
    // When false, scrolling up does NOT freeze output — new lines keep
    // scrolling into view.  Mirrors the world's m_bAutoFreeze flag.
    bool        m_allowAutoFreeze    = true;
    // World option keep_pause_at_bottom: returning to the bottom does not
    // resume output once the pane is frozen.  Default off, as on Windows.
    bool        m_keepPauseAtBottom  = false;
    bool        m_copySelToClip     = false;  // copy_selection_to_clipboard
    bool        m_autoCopyInHTML    = false;  // …and do it as HTML
    bool m_bleedBackground = false;   // App.m_bBleedBackground
    bool m_smootherScroll  = false;   // App.m_bSmootherScrolling
    bool m_smoothScroll    = false;   // App.m_bSmoothScrolling ("very smooth")
    bool m_animatingScroll = false;   // re-entrancy guard for the animation
    int  m_pixelOffset     = 0;       // doc->m_iPixelOffset (0..20)
    std::string m_wordDelimitersDblClick = ".,()[]\"\'";

    // Triple-click detection: record the timestamp and line of the last
    // double-click so OnLeftDown can promote the third click to a line select.
    wxLongLong m_lastDClickTimeMs = 0;
    int        m_lastDClickLine   = -1;

    // Lines trimmed from the front since the last UpdateScrollbars call.
    // Used to compensate the scroll position when the buffer wraps while the
    // user is scrolled up (prevents the view jumping to different content).
    int m_trimmedCount       = 0;
    int m_trimmedRowsRemoved = 0;  // visual rows lost due to trimming

    wxDECLARE_EVENT_TABLE();
};

#endif // !_WIN32
