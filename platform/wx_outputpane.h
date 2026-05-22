// platform/wx_outputpane.h — Custom scrolled output pane for MUSHclient (macOS/Linux)
//
// COutputPane is a wxScrolledWindow subclass that renders MUD output as a
// sequence of styled lines.  Each line is a vector<SOutputRun> — the same
// data produced by the ANSI/MXP parser (doc_stub.h).
//
// Features:
//   • Per-run fg/bg colour, bold, underline using wxDC primitives
//   • Monospace font, configurable size
//   • Capped scrollback buffer (m_maxLines, default 5000)
//   • Auto-scroll to bottom on new output; pauses when user scrolls up
//   • Right-click context menu: Copy Selection, Select All, Clear
//   • Double-buffered paint via wxBufferedPaintDC

#pragma once

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "platform/output_types.h"   // SOutputLine, SOutputRun

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
    // Use a distinct colour for connection events:
    //   connected    → 0x44CC44 (green)
    //   disconnected → 0xFF8800 (amber)
    //   failed       → 0xFF4444 (red)
    //   reconnecting → 0xFFCC00 (yellow)
    //   connecting   → 0x00CCCC (cyan)
    void AppendNote(const std::string& text, unsigned long fg = 0x808080);

    // Batch update: accumulate lines without repainting.
    // Call BeginBatch() before a group of AppendLine() calls, then EndBatch()
    // to trigger a single scrollbar update + repaint.
    void BeginBatch() { m_batchDepth++; }
    void EndBatch();   // triggers UpdateScrollbars + Refresh if batch depth drops to 0

    // Remove all lines from the buffer and repaint.
    void ClearOutput();

    // ── Find / search ─────────────────────────────────────────────────────────
    // Search the buffer for the next/previous line whose plain text contains
    // 'term' (case-insensitive).  On a match, selects that line and scrolls to
    // it; returns true.  Returns false if no match is found.
    // 'forward' == true searches downward from the current selection (or the
    // bottom of the buffer); false searches upward.
    bool FindNext(const wxString& term, bool forward = true);

    // Highlight all lines that contain 'term'.  Pass an empty string to clear.
    void HighlightSearch(const wxString& term);

    // Change the display font (e.g. from preferences).
    void SetOutputFont(const wxFont& font);

    // Update the scrollback buffer cap.  Lines already over the new limit are
    // trimmed from the top immediately.  Mirrors the doc's m_maxlines setting.
    void SetMaxLines(int n);

    // Adjust the font size by delta points (positive = larger, negative = smaller).
    // Clamped to [kMinFontSize, kMaxFontSize].  Returns the new size.
    int AdjustFontSize(int delta);

    // Reset font size to the default (kDefaultFontSize).
    void ResetFontSize();

    // Current font point size.
    int FontSize() const { return m_fontSize; }

    static constexpr int kDefaultFontSize = 12;
    static constexpr int kMinFontSize     = 6;
    static constexpr int kMaxFontSize     = 32;

    // Copy the visible selected text to the clipboard.
    void CopySelection();

    // Select all text in the buffer.
    void SelectAll();

    // Line height in pixels (updated whenever the font changes).
    int LineHeight() const { return m_lineH; }

    // Average character width in pixels (used for NAWS column calculation).
    int CharWidth()  const { return m_charW; }

    // Number of lines currently in the buffer.
    int LineCount() const { return (int)m_lines.size(); }

    // Read-only access to a single buffered line by index.
    // Returns nullptr if idx is out of range.
    const SOutputLine* GetLine(int idx) const {
        if (idx < 0 || idx >= (int)m_lines.size()) return nullptr;
        return &m_lines[idx];
    }

    // True when the pane is locked to the bottom (auto-scroll active).
    bool IsAutoScrolling() const { return m_autoScroll; }

    // Set the pane's default background colour (the colour painted behind text
    // and in empty space).  col is a COLORREF; pass 0xFF000000 (or omit) for
    // the default black.  Also colours the wxWindow background so that areas
    // outside the virtual canvas don't flash.  Called by the frame when the
    // world file specifies a custom background (doc->m_iBackgroundColour).
    void SetPaneBackground(unsigned long col);

    // Current pane background as COLORREF.
    unsigned long PaneBackground() const { return m_bgColor; }

    // Optional callback fired whenever font metrics (m_lineH, m_charW) change.
    // Set by CWorldChildFrame to forward NAWS updates on font-size changes.
    std::function<void()> m_fn_grid_changed;

    // Optional callback: returns the tooltip text for a client-space point.
    // Called from OnMotion; the pane uses the result to update wxToolTip.
    // Return an empty string to clear any active tooltip.
    std::function<std::string(int x, int y)> m_fn_get_tooltip;

    // Optional callback fired when the auto-scroll state changes.
    // Called with true when the view locks to the bottom (auto-scroll on),
    // and false when the user scrolls up (auto-scroll paused).
    // Use this to show/hide a "scrolled back" indicator in the status bar.
    std::function<void(bool)> m_fn_autoscroll_changed;

    // Optional callback for compositing miniwindows onto the output DC.
    // Called twice per paint: once before text (bUnder=true, for windows with
    // MINIWINDOW_DRAW_UNDERNEATH) and once after text (bUnder=false).
    // dc is positioned at client origin (scroll offset is removed before the
    // call and restored after).  clientW / clientH are the pane's client size.
    std::function<void(wxDC& dc, int clientW, int clientH, bool bUnder)>
        m_fn_composite_miniwindows;

    // Optional mouse-event callbacks for miniwindow hotspot routing.
    // Coordinates are client-space (same system as mw->m_rect).
    // flags carries the button identity: 0x10=LH, 0x20=RH; add 0x40 for dbl.
    // Return true if the event was consumed by a miniwindow (caller should
    // then skip the normal text-selection handling for that event).
    std::function<bool(int x, int y, long flags)> m_fn_miniwindow_mouse_down;
    std::function<bool(int x, int y, long flags)> m_fn_miniwindow_mouse_up;

    // Mouse-move callback: always called (no return value; fires hover scripts).
    std::function<void(int x, int y)> m_fn_miniwindow_mouse_move;

    // Mouse-wheel callback for miniwindow hotspots.
    // delta > 0 = scroll up (forward), delta < 0 = scroll down (backward).
    // Return true if the event was consumed by a miniwindow hotspot.
    std::function<bool(int x, int y, int delta)> m_fn_miniwindow_mouse_wheel;

    // Optional callback invoked when the user clicks an MXP hyperlink run.
    // action is the pipe-delimited command string from the MXP anchor.
    // The frame should pass this to EvaluateCommand() on the active doc.
    std::function<void(const std::string& action)> m_fn_link_clicked;

private:
    // ── Paint ────────────────────────────────────────────────────────────────
    void OnPaint(wxPaintEvent& evt);
    void OnSize(wxSizeEvent& evt);

    // ── Scroll ────────────────────────────────────────────────────────────────
    // Handles scrollbar thumb drag and button clicks so m_autoScroll is updated
    // whenever the user scrolls via the scrollbar (not just wheel/keyboard).
    // Bound in the constructor via Bind() for each wxEVT_SCROLLWIN_* type.
    void OnScrollWinUpdate(wxScrollWinEvent& evt);

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

    // ── Scroll helpers ───────────────────────────────────────────────────────
    // Recalculate virtual size and (optionally) scroll to the bottom.
    void UpdateScrollbars(bool scrollToBottom);

    // Convert a client-coordinates y pixel to a line index in m_lines.
    int  YToLine(int y) const;

    // Render one line onto dc at y-pixel offset yOff.
    void DrawLine(wxDC& dc, const SOutputLine& line, int yOff);

    // Rebuild the plain-text representation of selected lines for the clipboard.
    wxString SelectedText() const;

    // ── State ────────────────────────────────────────────────────────────────
    std::deque<SOutputLine> m_lines;     // scrollback buffer
    int                     m_maxLines;  // buffer cap

    wxFont m_font;                    // current display font
    int    m_fontSize = kDefaultFontSize;  // current point size (drives font zoom)
    int    m_lineH    = 16;          // line height in pixels (updated from font metrics)
    int    m_charW    = 8;           // average char width (for column estimates)

    // Estimated character count for the widest line seen; used to size the
    // horizontal virtual extent.  Tracked cheaply (per-line char count only;
    // pixel-exact measurement would require a DC).  Cleared on ClearOutput
    // and font change (where charW changes).
    int    m_maxLineChars = 80;       // tracks widest line in char units

    // Selection: line indices (inclusive), -1 = no selection
    int  m_selStartLine = -1;
    int  m_selEndLine   = -1;

    // Current search term (empty = no active search).
    // Lines containing this term are highlighted with a dim underline tint.
    wxString m_searchTerm;

    // Pane default background colour as COLORREF (RGB packed, 0 = black).
    unsigned long m_bgColor = 0;

    // Batch-update depth counter: when > 0, AppendLine defers refresh.
    int  m_batchDepth = 0;

    // Mouse tracking for click-drag selection
    bool m_dragging   = false;
    int  m_dragStartY = 0;

    // Auto-scroll: true while the view is pinned to the bottom
    bool m_autoScroll = true;

    wxDECLARE_EVENT_TABLE();
};

#endif // !_WIN32
