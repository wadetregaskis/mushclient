// platform/wx_outputpane.h — Custom scrolled output pane for MUSHclient (macOS/Linux)
//
// COutputPane is a wxScrolledWindow subclass that renders MUD output as a
// sequence of styled lines.  Each line is a vector<SOutputRun> — the same
// data produced by the ANSI/MXP parser (doc_stub.h).
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
    bool FindNext(const wxString& term, bool forward = true,
                  int* outMatchLine    = nullptr,
                  int* outOrdinal      = nullptr,
                  int* outTotalMatches = nullptr);
    void HighlightSearch(const wxString& term);

    // Change the display font (e.g. from preferences).
    void SetOutputFont(const wxFont& font);

    // Update the scrollback buffer cap.
    void SetMaxLines(int n);

    // Adjust font size by delta points.  Returns new size.
    int AdjustFontSize(int delta);
    void ResetFontSize();
    int FontSize() const { return m_fontSize; }

    static constexpr int kDefaultFontSize = 12;
    static constexpr int kMinFontSize     = 6;
    static constexpr int kMaxFontSize     = 32;

    // Copy the selected text to the clipboard.
    void CopySelection();

    // Select all text in the buffer.
    void SelectAll();

    // Scroll to the very bottom of the buffer and re-engage auto-scroll.
    // Safe to call at any time; no-op if already at the bottom.
    void ScrollToBottom();

    // Scroll one page up (direction=-1) or down (+1).
    // Updates the auto-scroll state accordingly.
    void ScrollPage(int direction);

    // Line height / char width in pixels (updated on font change).
    int LineHeight() const { return m_lineH; }
    int CharWidth()  const { return m_charW; }

    // Number of lines currently in the buffer.
    int LineCount() const { return (int)m_lines.size(); }

    // Read-only access to a single buffered line.
    const SOutputLine* GetLine(int idx) const {
        if (idx < 0 || idx >= (int)m_lines.size()) return nullptr;
        return &m_lines[idx];
    }

    // True while the pane is pinned to the bottom.
    bool IsAutoScrolling() const { return m_autoScroll; }

    // Set the pane's default background colour (COLORREF, 0 = black).
    void SetPaneBackground(unsigned long col);
    unsigned long PaneBackground() const { return m_bgColor; }

    // ── Callbacks ─────────────────────────────────────────────────────────────

    // Called whenever font metrics (m_lineH, m_charW) change.
    std::function<void()> m_fn_grid_changed;

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

    // Miniwindow mouse routing.  Return true if the event was consumed.
    std::function<bool(int x, int y, long flags)> m_fn_miniwindow_mouse_down;
    std::function<bool(int x, int y, long flags)> m_fn_miniwindow_mouse_up;
    std::function<void(int x, int y)>              m_fn_miniwindow_mouse_move;
    std::function<bool(int x, int y, int delta)>   m_fn_miniwindow_mouse_wheel;

    // Called when the user clicks an MXP hyperlink run.
    std::function<void(const std::string& action)> m_fn_link_clicked;

    // Called when F3 (forward) or Shift+F3 (backward) is pressed while the
    // output pane has focus.  Wired by CWorldChildFrame to DoFind().
    std::function<void(bool forward)> m_fn_find_next_key;

private:
    // ── Paint ────────────────────────────────────────────────────────────────
    void OnPaint(wxPaintEvent& evt);
    void OnSize(wxSizeEvent& evt);

    // Render one line.  lineIdx is the index in m_lines; yOff is the
    // y-pixel of the line's top edge in virtual (unscrolled) coordinates.
    void DrawLine(wxDC& dc, int lineIdx, int yOff);

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

    // Convert a line index + client-coordinates x to a character column.
    // Column is a cursor position: 0 = before first char, n = after nth char.
    int XToChar(int lineIdx, int clientX) const;

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

    wxFont m_font;
    // Cached font variants — rebuilt whenever m_font changes so DrawRunChunk
    // never constructs wxFont objects on the hot rendering path.
    wxFont m_fontBold;           // bold only
    wxFont m_fontUnderline;      // underline only
    wxFont m_fontBoldUnderline;  // bold + underline

    int    m_fontSize = kDefaultFontSize;
    int    m_lineH    = 18;   // line height pixels (set from font metrics)
    int    m_charW    = 10;   // avg char width pixels

    int    m_maxLineChars = 80;   // widest line seen, in characters

    // Selection: anchor is where the mouse went down; focus tracks the drag.
    // When anchor == focus, there is no selection (just a cursor position).
    SelPoint m_selAnchor;
    SelPoint m_selFocus;

    wxString m_searchTerm;

    unsigned long m_bgColor  = 0;   // COLORREF
    int           m_batchDepth = 0;

    bool        m_dragging   = false;
    int         m_dragStartY = 0;

    // Stored at mouse-down time when the cursor is over a link run.
    // Fired as a link click on mouse-up only if no drag occurred.
    std::string m_linkAtDown;

    bool m_autoScroll = true;

    // Number of lines trimmed from the front of m_lines since the last
    // UpdateScrollbars call.  Used to compensate the scroll position when the
    // buffer wraps and the user has scrolled up (so the view doesn't jump).
    int m_trimmedCount = 0;

    wxDECLARE_EVENT_TABLE();
};

#endif // !_WIN32
