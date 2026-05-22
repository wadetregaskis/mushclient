// platform/wx_outputpane.cpp — Custom output pane implementation

#ifndef _WIN32

#include "platform/wx_outputpane.h"
#include <wx/dcbuffer.h>
#include <wx/clipbrd.h>
#include <wx/tooltip.h>
#include <algorithm>
// compat.h provides RGB(), GetRValue(), GetGValue(), GetBValue().
// output_types.h no longer pulls it in transitively (to stay safe inside doc.h),
// so we include it explicitly here — after all wx headers are already included.
#include "platform/compat.h"

// ── Context-menu IDs ─────────────────────────────────────────────────────────
static const int ID_COPY       = wxID_COPY;
static const int ID_SELECTALL  = wxID_SELECTALL;
static const int ID_CLEAR      = wxID_HIGHEST + 100;

// ── Event table ───────────────────────────────────────────────────────────────
wxBEGIN_EVENT_TABLE(COutputPane, wxScrolledWindow)
    EVT_PAINT(                      COutputPane::OnPaint)
    EVT_SIZE(                       COutputPane::OnSize)
    EVT_LEFT_DOWN(                  COutputPane::OnLeftDown)
    EVT_LEFT_UP(                    COutputPane::OnLeftUp)
    EVT_LEFT_DCLICK(                COutputPane::OnLeftDClick)
    EVT_RIGHT_DOWN(                 COutputPane::OnRightDown)
    EVT_RIGHT_UP(                   COutputPane::OnRightUp)
    EVT_MOTION(                     COutputPane::OnMotion)
    EVT_MOUSEWHEEL(                 COutputPane::OnMouseWheel)
    EVT_KEY_DOWN(                   COutputPane::OnKeyDown)
    EVT_MENU(ID_COPY,               COutputPane::OnMenuCopy)
    EVT_MENU(ID_SELECTALL,          COutputPane::OnMenuSelectAll)
    EVT_MENU(ID_CLEAR,              COutputPane::OnMenuClear)
wxEND_EVENT_TABLE()

// ── Constructor ───────────────────────────────────────────────────────────────
COutputPane::COutputPane(wxWindow* parent, int maxLines)
    : wxScrolledWindow(parent, wxID_ANY,
                       wxDefaultPosition, wxDefaultSize,
                       wxHSCROLL | wxVSCROLL | wxFULL_REPAINT_ON_RESIZE)
    , m_maxLines(maxLines)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);   // needed for wxBufferedPaintDC
    SetBackgroundColour(wxColour(0, 0, 0));

    // Default: 12pt monospace
    m_font = wxFont(12, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                    wxFONTWEIGHT_NORMAL);
    SetFont(m_font);

    // Measure initial line metrics using a temporary DC
    {
        wxClientDC dc(this);
        dc.SetFont(m_font);
        wxCoord w, h;
        dc.GetTextExtent("M", &w, &h);
        m_lineH = h + 2;   // +2px inter-line gap
        m_charW = w;
    }

    SetScrollRate(m_charW, m_lineH);
    UpdateScrollbars(false);

    // Update m_autoScroll whenever the user scrolls via the scrollbar itself
    // (wheel and keyboard scroll are handled in OnMouseWheel / OnKeyDown).
    // Bind each event type individually — wxWidgets 3.x has no single
    // EVT_SCROLLWIN catch-all macro.
    for (auto evtType : {
            wxEVT_SCROLLWIN_THUMBTRACK,
            wxEVT_SCROLLWIN_THUMBRELEASE,
            wxEVT_SCROLLWIN_LINEUP,
            wxEVT_SCROLLWIN_LINEDOWN,
            wxEVT_SCROLLWIN_PAGEUP,
            wxEVT_SCROLLWIN_PAGEDOWN,
            wxEVT_SCROLLWIN_TOP,
            wxEVT_SCROLLWIN_BOTTOM })
    {
        Bind(evtType, &COutputPane::OnScrollWinUpdate, this);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void COutputPane::AppendLine(SOutputLine&& line)
{
    // Count characters in this line to track the widest line seen.
    int lineChars = 0;
    for (const auto& run : line) lineChars += (int)run.text.size();
    if (lineChars > m_maxLineChars) m_maxLineChars = lineChars;

    m_lines.push_back(std::move(line));

    // If we've exceeded the scrollback cap, trim from the front.
    // Any selection indices are now stale — clear them.
    bool trimmed = false;
    while ((int)m_lines.size() > m_maxLines) {
        m_lines.pop_front();
        trimmed = true;
    }
    if (trimmed) {
        m_selStartLine = -1;
        m_selEndLine   = -1;
    }

    if (m_batchDepth == 0) {
        UpdateScrollbars(m_autoScroll);
        Refresh(false);
    }
}

void COutputPane::EndBatch()
{
    if (m_batchDepth <= 0) return;
    if (--m_batchDepth == 0) {
        UpdateScrollbars(m_autoScroll);
        Refresh(false);
    }
}

void COutputPane::SetMaxLines(int n)
{
    if (n < 100) n = 100;  // enforce a sensible floor
    m_maxLines = n;
    // Trim immediately if the buffer is already over the new limit.
    bool trimmed = false;
    while ((int)m_lines.size() > m_maxLines) {
        m_lines.pop_front();
        trimmed = true;
    }
    if (trimmed) {
        // Selection indices are now invalid — clear them.
        m_selStartLine = -1;
        m_selEndLine   = -1;
        UpdateScrollbars(m_autoScroll);
        Refresh(false);
    }
}

void COutputPane::AppendNote(const std::string& text, unsigned long fg)
{
    SOutputLine noteLine;
    SOutputRun  noteRun;
    noteRun.text      = text;
    noteRun.fg        = fg;
    noteRun.bg        = m_bgColor;  // use the configured pane background
    noteRun.bold      = false;
    noteRun.underline = false;
    noteLine.push_back(std::move(noteRun));
    AppendLine(std::move(noteLine));
}

void COutputPane::SetPaneBackground(unsigned long col)
{
    m_bgColor = col;
    wxColour wxbg(GetRValue(col), GetGValue(col), GetBValue(col));
    SetBackgroundColour(wxbg);
    Refresh(false);
}

void COutputPane::ClearOutput()
{
    m_lines.clear();
    m_selStartLine  = -1;
    m_selEndLine    = -1;
    m_maxLineChars  = 80;
    UpdateScrollbars(true);
    Refresh();
}

void COutputPane::SetOutputFont(const wxFont& font)
{
    m_font     = font;
    m_fontSize = font.GetPointSize();
    SetFont(m_font);

    wxClientDC dc(this);
    dc.SetFont(m_font);
    wxCoord w, h;
    dc.GetTextExtent("M", &w, &h);
    m_lineH = h + 2;
    m_charW = w;

    // charW changed — old max-width estimate in pixels is stale, reset it.
    m_maxLineChars = 80;
    SetScrollRate(m_charW, m_lineH);
    UpdateScrollbars(m_autoScroll);
    Refresh();

    // Notify owner (e.g. CWorldChildFrame) that the character grid has changed.
    // This lets the frame send an updated NAWS packet to the MUD.
    if (m_fn_grid_changed)
        m_fn_grid_changed();
}

int COutputPane::AdjustFontSize(int delta)
{
    int newSize = std::max(kMinFontSize,
                  std::min(kMaxFontSize, m_fontSize + delta));
    if (newSize == m_fontSize) return m_fontSize;

    m_fontSize = newSize;
    wxFont f(m_fontSize, m_font.GetFamily(), m_font.GetStyle(),
             m_font.GetWeight(), m_font.GetUnderlined(),
             m_font.GetFaceName());
    SetOutputFont(f);
    return m_fontSize;
}

void COutputPane::ResetFontSize()
{
    AdjustFontSize(kDefaultFontSize - m_fontSize);
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void COutputPane::OnPaint(wxPaintEvent&)
{
    wxBufferedPaintDC dc(this);
    PrepareDC(dc);

    // Clear background using the configured pane background colour.
    wxColour bgCol(GetRValue(m_bgColor), GetGValue(m_bgColor), GetBValue(m_bgColor));
    dc.SetBackground(wxBrush(bgCol));
    dc.Clear();

    // Determine which lines are visible
    int scrollX, scrollY;
    GetViewStart(&scrollX, &scrollY);   // in scroll units
    int unitW, unitH;
    GetScrollPixelsPerUnit(&unitW, &unitH);

    int clientW, clientH;
    GetClientSize(&clientW, &clientH);

    // Convert scroll position to pixels
    int yPixOffset = scrollY * unitH;

    // ── Miniwindows: draw-underneath pass (bUnder=true) ───────────────────
    // Miniwindow coordinates are always in client-space (not scroll-space), so
    // we temporarily remove the scroll device-origin before compositing.
    if (m_fn_composite_miniwindows) {
        dc.SetDeviceOrigin(0, 0);
        m_fn_composite_miniwindows(dc, clientW, clientH, /*bUnder=*/true);
        // Restore the scroll transform that PrepareDC set.
        dc.SetDeviceOrigin(-scrollX * unitW, -scrollY * unitH);
    }

    if (!m_lines.empty()) {
        int firstLine = yPixOffset / m_lineH;
        int lastLine  = (yPixOffset + clientH) / m_lineH + 1;
        firstLine = std::max(0, firstLine);
        lastLine  = std::min((int)m_lines.size() - 1, lastLine);

        dc.SetFont(m_font);

        for (int i = firstLine; i <= lastLine; ++i) {
            int yOff = i * m_lineH;
            DrawLine(dc, m_lines[i], yOff);
        }
    }

    // ── Miniwindows: draw-on-top pass (bUnder=false) ──────────────────────
    if (m_fn_composite_miniwindows) {
        dc.SetDeviceOrigin(0, 0);
        m_fn_composite_miniwindows(dc, clientW, clientH, /*bUnder=*/false);
        // No need to restore the scroll transform — paint is finished.
    }
}

void COutputPane::DrawLine(wxDC& dc, const SOutputLine& line, int yOff)
{
    int x = 0;

    // ── Horizontal rule ────────────────────────────────────────────────────────
    // A line consisting of a single sentinel run with horiz_rule==true is
    // rendered as a full-width grey separator rather than a text line.
    if (line.size() == 1 && line[0].horiz_rule) {
        int clientW, clientH;
        GetClientSize(&clientW, &clientH);
        int scrollX, scrollY;
        GetViewStart(&scrollX, &scrollY);
        int unitW, unitH;
        GetScrollPixelsPerUnit(&unitW, &unitH);
        int xLeft  = scrollX * unitW;
        int xRight = xLeft + clientW;
        int midY = yOff + m_lineH / 2;
        dc.SetPen(wxPen(wxColour(96, 96, 96), 1));
        dc.DrawLine(xLeft + 4, midY, xRight - 4, midY);
        // Fill above and below the rule with the pane background colour.
        wxColour bgC(GetRValue(m_bgColor), GetGValue(m_bgColor), GetBValue(m_bgColor));
        dc.SetBrush(wxBrush(bgC));
        dc.SetPen(*wxTRANSPARENT_PEN);
        if (midY > yOff)
            dc.DrawRectangle(xLeft, yOff, clientW, midY - yOff);
        if (midY + 1 < yOff + m_lineH)
            dc.DrawRectangle(xLeft, midY + 1, clientW, yOff + m_lineH - midY - 1);
        return;
    }

    // Is this line selected?
    // (We track selection as a range of line indices, no intra-line selection yet)
    bool lineSelected = false;
    if (m_selStartLine >= 0 && m_selEndLine >= 0) {
        int lo = std::min(m_selStartLine, m_selEndLine);
        int hi = std::max(m_selStartLine, m_selEndLine);
        // Find index of this line in m_lines — caller passes the SOutputLine ref,
        // not the index, so we use yOff to compute it
        int idx = yOff / m_lineH;
        lineSelected = (idx >= lo && idx <= hi);
    }

    for (const auto& run : line) {
        if (run.text.empty()) continue;

        wxColour fg(GetRValue(run.fg), GetGValue(run.fg), GetBValue(run.fg));
        wxColour bg(GetRValue(run.bg), GetGValue(run.bg), GetBValue(run.bg));

        if (lineSelected) {
            // Invert colours for selected lines
            std::swap(fg, bg);
        }

        // MXP hyperlink: override foreground colour and force underline so the
        // link is visually distinct regardless of the MUD's own colour choices.
        bool isLink = !run.action.empty();
        wxColour effectiveFg = isLink ? wxColour(0x44, 0xAA, 0xFF) : fg;

        dc.SetTextForeground(effectiveFg);
        dc.SetTextBackground(bg);
        dc.SetBackgroundMode(wxSOLID);

        bool drawUnderline = run.underline || isLink;
        if (run.bold)
            dc.SetFont(wxFont(m_font.GetPointSize(), m_font.GetFamily(),
                              m_font.GetStyle(), wxFONTWEIGHT_BOLD,
                              drawUnderline));
        else if (drawUnderline)
            dc.SetFont(wxFont(m_font.GetPointSize(), m_font.GetFamily(),
                              m_font.GetStyle(), m_font.GetWeight(),
                              true));
        else
            dc.SetFont(m_font);

        wxString wxText = wxString::FromUTF8(run.text.c_str(), run.text.size());
        dc.DrawText(wxText, x, yOff);

        wxCoord tw, th;
        dc.GetTextExtent(wxText, &tw, &th);
        x += tw;
    }

    // Fill the remainder of the line with the pane background colour.
    {
        int clientW, clientH;
        GetClientSize(&clientW, &clientH);
        int scrollX, scrollY;
        GetViewStart(&scrollX, &scrollY);
        int unitW, unitH;
        GetScrollPixelsPerUnit(&unitW, &unitH);
        int rightEdge = scrollX * unitW + clientW;
        if (x < rightEdge) {
            wxColour bgC(GetRValue(m_bgColor), GetGValue(m_bgColor), GetBValue(m_bgColor));
            dc.SetBrush(wxBrush(bgC));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(x, yOff, rightEdge - x, m_lineH);
        }
    }

    // If there's an active search term, draw a yellow left-margin marker
    // on lines that contain the term.  A 3-pixel strip is unambiguous and
    // works without alpha blending across all wxWidgets backends.
    if (!m_searchTerm.IsEmpty()) {
        wxString plain;
        for (const auto& run : line)
            if (!run.horiz_rule)
                plain += wxString::FromUTF8(run.text.c_str(), run.text.size());
        if (plain.Lower().Contains(m_searchTerm.Lower())) {
            int scrollX, scrollY;
            GetViewStart(&scrollX, &scrollY);
            int unitW, unitH;
            GetScrollPixelsPerUnit(&unitW, &unitH);
            int xLeft = scrollX * unitW;  // left edge in logical coords
            dc.SetBrush(wxBrush(wxColour(255, 220, 0)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(xLeft, yOff, 3, m_lineH);
        }
    }
}

void COutputPane::OnScrollWinUpdate(wxScrollWinEvent& evt)
{
    evt.Skip();   // let wxScrolledWindow apply the scroll first
    CallAfter([this]() {
        if (!this) return;
        int scrollX, scrollY;
        GetViewStart(&scrollX, &scrollY);
        int unitH;
        GetScrollPixelsPerUnit(nullptr, &unitH);
        int clientH;
        GetClientSize(nullptr, &clientH);
        int totalH = (int)m_lines.size() * m_lineH;
        bool wasAuto = m_autoScroll;
        m_autoScroll = (unitH > 0) &&
                       (scrollY * unitH + clientH >= totalH - m_lineH);
        if (m_autoScroll != wasAuto && m_fn_autoscroll_changed)
            m_fn_autoscroll_changed(m_autoScroll);
    });
}

void COutputPane::OnSize(wxSizeEvent& evt)
{
    UpdateScrollbars(m_autoScroll);
    evt.Skip();
}

// ── Scroll helpers ────────────────────────────────────────────────────────────

void COutputPane::UpdateScrollbars(bool scrollToBottom)
{
    int clientW, clientH;
    GetClientSize(&clientW, &clientH);

    int totalH = (int)m_lines.size() * m_lineH;
    // Allow horizontal scrolling for lines wider than the pane.
    // Use character-count estimate × charW (cheap: no DC required).
    int totalW = std::max(clientW, m_maxLineChars * m_charW + m_charW);

    // SetVirtualSize triggers the scrollbars
    SetVirtualSize(totalW, totalH);

    if (scrollToBottom && !m_lines.empty()) {
        int maxScroll = std::max(0, totalH - clientH);
        int unitW, unitH;
        GetScrollPixelsPerUnit(&unitW, &unitH);
        if (unitH > 0) {
            // Preserve current horizontal scroll position; only move vertical.
            int scrollX, scrollY;
            GetViewStart(&scrollX, &scrollY);
            (void)scrollY;
            Scroll(scrollX, maxScroll / unitH);
        }
    }
}

int COutputPane::YToLine(int y) const
{
    if (m_lines.empty()) return 0;

    // y is in client coordinates (unscrolled)
    int scrollX, scrollY;
    const_cast<COutputPane*>(this)->GetViewStart(&scrollX, &scrollY);
    int unitW, unitH;
    const_cast<COutputPane*>(this)->GetScrollPixelsPerUnit(&unitW, &unitH);
    int yPix = scrollY * unitH + y;
    int line  = (m_lineH > 0) ? yPix / m_lineH : 0;
    return std::max(0, std::min((int)m_lines.size() - 1, line));
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

void COutputPane::OnLeftDown(wxMouseEvent& evt)
{
    // Give miniwindow hotspot routing first chance at this click.
    // 0x10 = MW_MOUSE_LH (left-hand button flag, matches mushview.h constant).
    if (m_fn_miniwindow_mouse_down &&
        m_fn_miniwindow_mouse_down(evt.GetX(), evt.GetY(), 0x10 /*MW_MOUSE_LH*/))
    {
        evt.Skip();
        return;
    }

    // Check for MXP link click: find the line under the cursor, then walk its
    // runs (measuring each run's pixel width with a client DC) to find which
    // run was clicked.  If it has an action, fire it and skip text selection.
    if (m_fn_link_clicked && m_lineH > 0 && !m_lines.empty()) {
        int lineIdx = YToLine(evt.GetY());
        if (lineIdx >= 0 && lineIdx < (int)m_lines.size()) {
            const SOutputLine& line = m_lines[lineIdx];
            bool hasLink = false;
            for (const auto& run : line)
                if (!run.action.empty()) { hasLink = true; break; }

            if (hasLink) {
                // Runs are drawn starting at virtual x=0.  Convert the click's
                // client x to virtual x by adding the horizontal scroll offset.
                int scrollX, scrollY;
                GetViewStart(&scrollX, &scrollY);
                int unitW, unitH;
                GetScrollPixelsPerUnit(&unitW, &unitH);
                int clickVirtualX = evt.GetX() + scrollX * unitW;

                wxClientDC cdc(this);
                cdc.SetFont(m_font);
                int runX = 0;
                for (const auto& run : line) {
                    if (run.text.empty()) continue;
                    wxString wxText = wxString::FromUTF8(run.text.c_str(), run.text.size());
                    wxCoord tw, th;
                    cdc.GetTextExtent(wxText, &tw, &th);
                    if (!run.action.empty() &&
                        clickVirtualX >= runX && clickVirtualX < runX + tw)
                    {
                        m_fn_link_clicked(run.action);
                        evt.Skip();
                        return;
                    }
                    runX += tw;
                }
            }
        }
    }

    m_dragging     = true;
    m_dragStartY   = evt.GetY();
    m_selStartLine = YToLine(evt.GetY());
    m_selEndLine   = m_selStartLine;
    CaptureMouse();
    Refresh(false);
    evt.Skip();
}

void COutputPane::OnLeftUp(wxMouseEvent& evt)
{
    if (m_fn_miniwindow_mouse_up)
        m_fn_miniwindow_mouse_up(evt.GetX(), evt.GetY(), 0x10 /*MW_MOUSE_LH*/);

    if (m_dragging) {
        m_dragging   = false;
        m_selEndLine = YToLine(evt.GetY());
        if (HasCapture()) ReleaseMouse();
        Refresh(false);
    }
    evt.Skip();
}

void COutputPane::OnLeftDClick(wxMouseEvent& evt)
{
    // Double-click: same routing as single click but adds MW_MOUSE_DBL (0x40).
    // 0x50 = MW_MOUSE_LH (0x10) | MW_MOUSE_DBL (0x40).
    if (m_fn_miniwindow_mouse_down &&
        m_fn_miniwindow_mouse_down(evt.GetX(), evt.GetY(), 0x50 /*LH|DBL*/))
    {
        evt.Skip();
        return;
    }
    // Fall through to normal selection behaviour (select word, etc.).
    evt.Skip();
}

void COutputPane::OnRightUp(wxMouseEvent& evt)
{
    if (m_fn_miniwindow_mouse_up)
        m_fn_miniwindow_mouse_up(evt.GetX(), evt.GetY(), 0x20 /*MW_MOUSE_RH*/);
    evt.Skip();
}

void COutputPane::OnMotion(wxMouseEvent& evt)
{
    // Always route mouse-move to miniwindow hover detection.
    if (m_fn_miniwindow_mouse_move)
        m_fn_miniwindow_mouse_move(evt.GetX(), evt.GetY());

    // Update tooltip: ask the frame for the tooltip text at this position.
    // We only update when not dragging (don't flicker during text selection).
    if (!m_dragging && m_fn_get_tooltip) {
        std::string tip = m_fn_get_tooltip(evt.GetX(), evt.GetY());
        if (tip.empty()) {
            UnsetToolTip();
        } else {
            wxToolTip* existing = GetToolTip();
            wxString wxTip = wxString::FromUTF8(tip.c_str(), tip.size());
            if (!existing || existing->GetTip() != wxTip)
                SetToolTip(wxTip);
        }
    }

    if (m_dragging && evt.LeftIsDown()) {
        m_selEndLine = YToLine(evt.GetY());
        Refresh(false);

        // Auto-scroll when dragging past edges
        int clientW, clientH;
        GetClientSize(&clientW, &clientH);
        if (evt.GetY() > clientH - 10)
            ScrollLines(1);
        else if (evt.GetY() < 10)
            ScrollLines(-1);
    }
    evt.Skip();
}

void COutputPane::OnRightDown(wxMouseEvent& evt)
{
    // Give miniwindow hotspot routing first chance (RH flag).
    // 0x20 = MW_MOUSE_RH (right-hand button flag).
    if (m_fn_miniwindow_mouse_down &&
        m_fn_miniwindow_mouse_down(evt.GetX(), evt.GetY(), 0x20 /*MW_MOUSE_RH*/))
    {
        evt.Skip();
        return;
    }
    wxMenu menu;
    menu.Append(ID_COPY,      "Copy\tCtrl+C");
    menu.Append(ID_SELECTALL, "Select All\tCtrl+A");
    menu.AppendSeparator();
    menu.Append(ID_CLEAR,     "Clear Output");
    menu.Enable(ID_COPY, m_selStartLine >= 0 && m_selEndLine >= 0);
    PopupMenu(&menu, evt.GetPosition());
}

void COutputPane::OnMouseWheel(wxMouseEvent& evt)
{
    // Ctrl+scroll → zoom font size (each wheel notch = 1pt)
    if (evt.ControlDown()) {
        int delta = (evt.GetWheelRotation() > 0) ? 1 : -1;
        AdjustFontSize(delta);
        return;   // consume — don't also scroll the view
    }

    // Check if a miniwindow hotspot wants this wheel event.
    if (m_fn_miniwindow_mouse_wheel) {
        int delta = evt.GetWheelRotation();  // positive = up, negative = down
        if (m_fn_miniwindow_mouse_wheel(evt.GetX(), evt.GetY(), delta)) {
            return;   // consumed by a miniwindow — don't scroll the text
        }
    }

    // Plain scroll: let wxScrolledWindow handle it, then update auto-scroll flag.
    evt.Skip();
    CallAfter([this]() {
        int scrollX, scrollY;
        GetViewStart(&scrollX, &scrollY);
        int unitH;
        GetScrollPixelsPerUnit(nullptr, &unitH);
        int clientH;
        GetClientSize(nullptr, &clientH);
        int totalH = (int)m_lines.size() * m_lineH;
        bool wasAuto = m_autoScroll;
        m_autoScroll = (unitH > 0) &&
                       (scrollY * unitH + clientH >= totalH - m_lineH);
        if (m_autoScroll != wasAuto && m_fn_autoscroll_changed)
            m_fn_autoscroll_changed(m_autoScroll);
    });
}

void COutputPane::OnKeyDown(wxKeyEvent& evt)
{
    int code = evt.GetKeyCode();

    // Ctrl+C → copy selection
    if (code == 'C' && evt.ControlDown()) {
        CopySelection();
        return;
    }
    // Ctrl+A → select all
    if (code == 'A' && evt.ControlDown()) {
        SelectAll();
        return;
    }
    // Ctrl+= or Ctrl++ → increase font size by 1pt
    if (evt.ControlDown() &&
        (code == '=' || code == '+' ||
         code == WXK_NUMPAD_ADD)) {
        AdjustFontSize(1);
        return;
    }
    // Ctrl+- → decrease font size by 1pt
    if (evt.ControlDown() &&
        (code == '-' || code == WXK_NUMPAD_SUBTRACT)) {
        AdjustFontSize(-1);
        return;
    }
    // Ctrl+0 → reset font size to default
    if (evt.ControlDown() && code == '0') {
        ResetFontSize();
        return;
    }

    // For scroll keys, pass through then update the auto-scroll flag.
    bool isScrollKey = (code == WXK_PAGEUP   || code == WXK_PAGEDOWN ||
                        code == WXK_HOME      || code == WXK_END      ||
                        code == WXK_UP        || code == WXK_DOWN);
    evt.Skip();  // let wxScrolledWindow process the key

    if (isScrollKey) {
        // Defer the check until after the scroll has been applied.
        CallAfter([this]() {
            int scrollX, scrollY;
            GetViewStart(&scrollX, &scrollY);
            int unitH;
            GetScrollPixelsPerUnit(nullptr, &unitH);
            int clientH;
            GetClientSize(nullptr, &clientH);
            int totalH = (int)m_lines.size() * m_lineH;
            bool wasAuto = m_autoScroll;
            m_autoScroll = (unitH > 0) &&
                           (scrollY * unitH + clientH >= totalH - m_lineH);
            if (m_autoScroll != wasAuto && m_fn_autoscroll_changed)
                m_fn_autoscroll_changed(m_autoScroll);
        });
        return;  // already called evt.Skip() above
    }
}

// ── Context menu handlers ─────────────────────────────────────────────────────

void COutputPane::OnMenuCopy(wxCommandEvent&) { CopySelection(); }
void COutputPane::OnMenuSelectAll(wxCommandEvent&) { SelectAll(); }
void COutputPane::OnMenuClear(wxCommandEvent&) { ClearOutput(); }

// ── Selection / clipboard ─────────────────────────────────────────────────────

// ── Find / Search ─────────────────────────────────────────────────────────────

bool COutputPane::FindNext(const wxString& term, bool forward)
{
    if (term.IsEmpty() || m_lines.empty()) return false;

    wxString lowerTerm = term.Lower();
    int n = (int)m_lines.size();

    // Start searching from just past (or before) the current selection.
    int startIdx;
    if (forward) {
        startIdx = (m_selEndLine >= 0) ? m_selEndLine + 1 : n - 1;
        if (startIdx >= n) startIdx = 0;  // wrap
    } else {
        startIdx = (m_selStartLine >= 0) ? m_selStartLine - 1 : 0;
        if (startIdx < 0) startIdx = n - 1;  // wrap
    }

    // Build plain text for a single line (reused below).
    auto lineText = [this](int i) -> wxString {
        wxString s;
        for (const auto& run : m_lines[i])
            if (!run.horiz_rule)
                s += wxString::FromUTF8(run.text.c_str(), run.text.size());
        return s.Lower();
    };

    int idx = startIdx;
    for (int pass = 0; pass < n; ++pass) {
        if (lineText(idx).Contains(lowerTerm)) {
            m_selStartLine = idx;
            m_selEndLine   = idx;
            // Scroll to show the found line.
            int unitH;
            GetScrollPixelsPerUnit(nullptr, &unitH);
            if (unitH > 0) {
                int scrollX, scrollY;
                GetViewStart(&scrollX, &scrollY);
                Scroll(scrollX, idx * m_lineH / unitH);
                m_autoScroll = false;
                if (m_fn_autoscroll_changed) m_fn_autoscroll_changed(false);
            }
            Refresh(false);
            return true;
        }
        if (forward) {
            if (++idx >= n) idx = 0;
        } else {
            if (--idx < 0) idx = n - 1;
        }
    }
    return false;
}

void COutputPane::HighlightSearch(const wxString& term)
{
    m_searchTerm = term;
    Refresh(false);
}

void COutputPane::CopySelection()
{
    wxString text = SelectedText();
    if (text.IsEmpty()) return;
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(text));
        wxTheClipboard->Close();
    }
}

void COutputPane::SelectAll()
{
    if (m_lines.empty()) return;
    m_selStartLine = 0;
    m_selEndLine   = (int)m_lines.size() - 1;
    Refresh(false);
}

wxString COutputPane::SelectedText() const
{
    if (m_selStartLine < 0 || m_selEndLine < 0) return wxEmptyString;
    int lo = std::min(m_selStartLine, m_selEndLine);
    int hi = std::max(m_selStartLine, m_selEndLine);
    lo = std::max(0, lo);
    hi = std::min((int)m_lines.size() - 1, hi);

    wxString result;
    for (int i = lo; i <= hi; ++i) {
        bool isHRule = !m_lines[i].empty() && m_lines[i][0].horiz_rule;
        if (isHRule) {
            result += wxT("────────────────────────────────────────\n");
        } else {
            for (const auto& run : m_lines[i])
                if (!run.horiz_rule)
                    result += wxString::FromUTF8(run.text.c_str(), run.text.size());
            result += "\n";
        }
    }
    return result;
}

#endif // !_WIN32
