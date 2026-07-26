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
#include <functional>
#include <string>
#include <vector>
#include "platform/output_types.h"   // SOutputLine, SOutputRun

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

    // Read-only access to the current display font (used by scripting callbacks
    // to build a modified font to pass back into SetOutputFont).
    const wxFont& GetCurrentFont() const { return m_font; }

    // Update the scrollback buffer cap.
    void SetMaxLines(int n);
    // Remove up to `count` lines from the tail of m_lines (most-recent output).
    // Called when Lua scripts call world.DeleteLines(n).
    void DeleteTailLines(long count);

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

    // Display ▸ Bookmark Selection — toggle a bookmark on the selected line (or
    // the last line if nothing is selected); shows a left-margin marker.
    // Display ▸ Go To Bookmark — scroll to the next bookmarked line (cycling).
    void ToggleBookmark();
    void GotoNextBookmark();

    // Programmatically select a range of text.  Arguments are 1-based line and
    // column numbers, matching the MUSHclient SetSelection() Lua API.
    // Pass all-zero arguments to clear the selection.
    void SetSelectionRange(int startLine, int endLine, int startCol, int endCol);

    // Select all text in the buffer.
    void SelectAll();

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

    // Return the current scroll position as a y-pixel offset from the top of
    // the virtual (full scrollback) area.  Equivalent to CScrollView::
    // GetScrollPosition().y on Windows; used to implement GetInfo(296).
    long GetScrollPositionY() const;

    // Scroll to the very top of the buffer (disengages auto-scroll).
    // Safe to call at any time; no-op if already at the top.
    void ScrollToTop();

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
    SelPoint PointToChar(int clientX, int clientY) const;

    // Number of lines currently in the buffer.
    int LineCount() const { return (int)m_lines.size(); }

    // Read-only access to a single buffered line.
    const SOutputLine* GetLine(int idx) const {
        if (idx < 0 || idx >= (int)m_lines.size()) return nullptr;
        return &m_lines[idx];
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

    // Controls whether scrolling up engages the freeze (stops auto-scroll).
    // When false the pane always keeps auto-scrolling; new output is always
    // visible regardless of where the user has scrolled.  Mirrors the world's
    // m_bAutoFreeze field.  Default is true (freeze on scroll-up).
    void SetAllowAutoFreeze(bool allow) { m_allowAutoFreeze = allow; }

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

    // Returns true if a log file is currently open (used by context menu label).
    std::function<bool()> m_fn_is_logging;

    // Toggles logging on/off: shows a file-save dialog when starting.
    std::function<void()> m_fn_toggle_log;

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
    void DrawRunChunk(wxDC& dc, const wxString& text, int x, int yOff,
                      const SOutputRun& run, bool selected);

    // ── Scroll ────────────────────────────────────────────────────────────────
    void OnScrollWinUpdate(wxScrollWinEvent& evt);
    void UpdateScrollbars(bool scrollToBottom);

    // ── Mouse / keyboard ─────────────────────────────────────────────────────
    void OnLeftDown(wxMouseEvent& evt);
    void OnLeftUp(wxMouseEvent& evt);
    void OnLeftDClick(wxMouseEvent& evt);
    void OnMiddleUp(wxMouseEvent& evt);
    void OnRightDown(wxMouseEvent& evt);
    void OnRightUp(wxMouseEvent& evt);
    void OnMotion(wxMouseEvent& evt);
    void OnMouseWheel(wxMouseEvent& evt);
    void OnKeyDown(wxKeyEvent& evt);

    // ── Context menu ─────────────────────────────────────────────────────────
    void OnMenuCopy(wxCommandEvent& evt);
    void OnMenuSelectAll(wxCommandEvent& evt);
    void OnMenuClear(wxCommandEvent& evt);

    // ── Selection helpers ────────────────────────────────────────────────────

    // Convert client-coordinates y to a line index.
    int YToLine(int clientY) const;

    // Convert a line index + client-coordinates (x, y) to a character column.
    // clientY is required to resolve which visual row within a wrapped line was
    // clicked so that the column offset is computed correctly.
    // Column is a cursor position: 0 = before first char, n = after nth char.
    int XToChar(int lineIdx, int clientX, int clientY) const;

    // Build a SelPoint from a mouse event position.
    SelPoint MouseToSelPoint(const wxMouseEvent& evt) const;

    // Fills start/end with the normalised (start <= end) selection.
    // Returns false if there is no selection (both points invalid or equal).
    bool GetNormalizedSel(SelPoint& start, SelPoint& end) const;

    // Rebuild selected text for the clipboard.
    wxString SelectedText() const;

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
    int    m_charW    = 10;   // avg char width pixels

    int    m_maxLineChars = 80;   // widest line seen, in characters (stats only)

    // ── Wrap cache ────────────────────────────────────────────────────────────
    std::vector<int> m_visualRowStart;   // m_visualRowStart[i] = visual row of logical line i
    int              m_totalVisualRows = 0;
    int              m_wrapCols        = 80;

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

    // Index of the last bookmark jumped to, so Go To Bookmark cycles forward.
    int m_lastBookmarkLine = -1;

    wxString m_searchTerm;
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
    // True while OnPaint runs its mid-paint plugin callback
    // (ON_PLUGIN_DRAW_OUTPUT_WINDOW): AppendLine defers mutations of m_lines
    // until the paint completes, so the pass the paint is iterating stays
    // stable even if the script prints (docs/PAL.md S4c re-entrancy guard).
    bool m_inPaint         = false;
    // When false, scrolling up does NOT freeze output — new lines keep
    // scrolling into view.  Mirrors the world's m_bAutoFreeze flag.
    bool        m_allowAutoFreeze    = true;
    bool        m_copySelToClip     = false;  // copy_selection_to_clipboard
    bool        m_autoCopyInHTML    = false;  // …and do it as HTML
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
