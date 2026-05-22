// platform/wx_outputpane.cpp — Custom output pane implementation

#ifndef _WIN32

#include "platform/wx_outputpane.h"
#include <wx/dcbuffer.h>
#include <wx/clipbrd.h>
#include <wx/tooltip.h>
#include <algorithm>
// compat.h provides RGB(), GetRValue(), GetGValue(), GetBValue().
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
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(wxColour(0, 0, 0));

    m_font = wxFont(kDefaultFontSize, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                    wxFONTWEIGHT_NORMAL);
    SetFont(m_font);

    // Use wxMemoryDC for reliable font measurement before the window is shown.
    // wxClientDC in the constructor may return zero metrics on macOS/GTK because
    // the native view doesn't exist yet at that point.
    {
        wxBitmap bmp(1, 1, 24);
        wxMemoryDC mdc(bmp);
        mdc.SetFont(m_font);
        wxCoord w, h;
        mdc.GetTextExtent("M", &w, &h);
        if (h > 0) m_lineH = h + 2;   // +2 px inter-line gap
        if (w > 0) m_charW = w;
    }

    SetScrollRate(m_charW, m_lineH);
    UpdateScrollbars(false);

    // Show an I-beam cursor so the pane feels like a text view.
    SetCursor(wxCursor(wxCURSOR_IBEAM));

    // Update m_autoScroll whenever the user scrolls via the scrollbar.
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

// ── URL detection ─────────────────────────────────────────────────────────────
// Scans plain-text runs for http:// or https:// URLs and splits them into
// separate link runs.  Each URL run gets action = URL text so OnLeftDown fires
// m_fn_link_clicked, which detects the http prefix and calls
// wxLaunchDefaultBrowser().  Only runs with no existing action and no
// horiz_rule flag are scanned (MXP links are already labelled).

static SOutputLine DetectURLs(SOutputLine line)
{
    // Quick scan: skip the whole line if it has no "http" substring at all.
    bool hasHttp = false;
    for (const auto& r : line)
        if (r.action.empty() && !r.horiz_rule &&
            r.text.find("http") != std::string::npos)
            { hasHttp = true; break; }
    if (!hasHttp) return line;

    SOutputLine result;
    result.reserve(line.size() * 2);

    for (SOutputRun run : line) {
        // Pass through runs that already have a label or are decorative.
        if (!run.action.empty() || run.text.empty() || run.horiz_rule) {
            result.push_back(std::move(run));
            continue;
        }

        const std::string text = run.text;
        size_t pos = 0;
        bool splitOccurred = false;

        while (pos < text.size()) {
            size_t h1 = text.find("http://",  pos);
            size_t h2 = text.find("https://", pos);
            size_t urlStart = std::min(h1, h2);

            if (urlStart == std::string::npos) {
                // No more URLs.
                if (splitOccurred || pos > 0) {
                    SOutputRun tail = run;
                    tail.text = text.substr(pos);
                    result.push_back(std::move(tail));
                } else {
                    // No URL anywhere in this run — pass it through unmodified.
                    result.push_back(std::move(run));
                }
                break;
            }

            splitOccurred = true;

            // Text before the URL.
            if (urlStart > pos) {
                SOutputRun before = run;
                before.text = text.substr(pos, urlStart - pos);
                result.push_back(std::move(before));
            }

            // URL extends to the next whitespace (or end of run).
            size_t urlEnd = urlStart;
            while (urlEnd < text.size() &&
                   !std::isspace((unsigned char)text[urlEnd]))
                ++urlEnd;

            // Strip trailing punctuation that is almost certainly not part of
            // the URL itself: sentence-ending or bracketing characters.
            // e.g. "See http://example.com." or "link (http://x.com)"
            while (urlEnd > urlStart) {
                char last = text[urlEnd - 1];
                if (last == '.' || last == ',' || last == '!' || last == '?' ||
                    last == ')' || last == ']' || last == '}' || last == '\'' ||
                    last == '"' || last == ':' || last == ';')
                    --urlEnd;
                else
                    break;
            }

            // Emit the URL as a link run.  action = URL so m_fn_link_clicked
            // can distinguish it from MXP commands via the "http" prefix.
            SOutputRun urlRun  = run;
            urlRun.text        = text.substr(urlStart, urlEnd - urlStart);
            urlRun.action      = urlRun.text;
            result.push_back(std::move(urlRun));

            pos = urlEnd;
        }
        // If the loop ran out of text normally all segments were already pushed.
    }
    return result;
}

// ── Public API ────────────────────────────────────────────────────────────────

void COutputPane::AppendLine(SOutputLine&& line)
{
    // Auto-detect plain http:// / https:// URLs and split them into link runs.
    line = DetectURLs(std::move(line));

    int lineChars = 0;
    for (const auto& run : line) lineChars += (int)run.text.size();
    if (lineChars > m_maxLineChars) m_maxLineChars = lineChars;

    m_lines.push_back(std::move(line));

    int trimCount = 0;
    while ((int)m_lines.size() > m_maxLines) {
        m_lines.pop_front();
        ++trimCount;
        // Adjust selection indices downward; clear if the anchor line was removed.
        if (m_selAnchor.valid()) {
            if (m_selAnchor.line == 0) m_selAnchor = SelPoint{};
            else --m_selAnchor.line;
        }
        if (m_selFocus.valid()) {
            if (m_selFocus.line == 0) m_selFocus = SelPoint{};
            else --m_selFocus.line;
        }
    }
    m_trimmedCount += trimCount;

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
    if (n < 100) n = 100;
    m_maxLines = n;
    bool trimmed = false;
    while ((int)m_lines.size() > m_maxLines) {
        m_lines.pop_front();
        trimmed = true;
    }
    if (trimmed) {
        m_selAnchor = SelPoint{};
        m_selFocus  = SelPoint{};
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
    noteRun.bg        = m_bgColor;
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

void COutputPane::ScrollToBottom()
{
    bool wasAuto = m_autoScroll;
    m_autoScroll = true;
    UpdateScrollbars(true);
    if (!wasAuto && m_fn_autoscroll_changed)
        m_fn_autoscroll_changed(true);
}

void COutputPane::ScrollPage(int direction)
{
    int clientH;
    GetClientSize(nullptr, &clientH);
    int unitH;
    GetScrollPixelsPerUnit(nullptr, &unitH);
    if (unitH <= 0) return;

    int scrollX, scrollY;
    GetViewStart(&scrollX, &scrollY);
    int pageUnits = std::max(1, clientH / unitH - 1);

    int newY;
    if (direction < 0)
        newY = std::max(0, scrollY - pageUnits);
    else
        newY = scrollY + pageUnits;   // Scroll() clamps to valid range

    Scroll(scrollX, newY);

    // Re-evaluate whether we're now at the bottom.
    GetViewStart(nullptr, &newY);   // read back clamped position
    int totalH = (int)m_lines.size() * m_lineH;
    bool wasAuto = m_autoScroll;
    m_autoScroll = (unitH > 0) && (newY * unitH + clientH >= totalH - m_lineH);
    if (m_autoScroll != wasAuto && m_fn_autoscroll_changed)
        m_fn_autoscroll_changed(m_autoScroll);
}

void COutputPane::ClearOutput()
{
    m_lines.clear();
    m_selAnchor    = SelPoint{};
    m_selFocus     = SelPoint{};
    m_maxLineChars = 80;
    m_trimmedCount = 0;
    UpdateScrollbars(true);
    Refresh();
}

void COutputPane::SetOutputFont(const wxFont& font)
{
    m_font     = font;
    m_fontSize = font.GetPointSize();
    SetFont(m_font);

    // wxMemoryDC for reliable measurement (same reason as constructor).
    wxBitmap bmp(1, 1, 24);
    wxMemoryDC mdc(bmp);
    mdc.SetFont(m_font);
    wxCoord w, h;
    mdc.GetTextExtent("M", &w, &h);
    if (h > 0) m_lineH = h + 2;
    if (w > 0) m_charW = w;

    m_maxLineChars = 80;
    SetScrollRate(m_charW, m_lineH);
    UpdateScrollbars(m_autoScroll);
    Refresh();

    if (m_fn_grid_changed) m_fn_grid_changed();
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

    wxColour bgCol(GetRValue(m_bgColor), GetGValue(m_bgColor), GetBValue(m_bgColor));
    dc.SetBackground(wxBrush(bgCol));
    dc.Clear();

    int scrollX, scrollY;
    GetViewStart(&scrollX, &scrollY);
    int unitW, unitH;
    GetScrollPixelsPerUnit(&unitW, &unitH);

    int clientW, clientH;
    GetClientSize(&clientW, &clientH);

    int yPixOffset = scrollY * unitH;

    // ── Miniwindows: draw-underneath pass ─────────────────────────────────
    if (m_fn_composite_miniwindows) {
        dc.SetDeviceOrigin(0, 0);
        m_fn_composite_miniwindows(dc, clientW, clientH, /*bUnder=*/true);
        dc.SetDeviceOrigin(-scrollX * unitW, -scrollY * unitH);
    }

    if (!m_lines.empty()) {
        int firstLine = yPixOffset / m_lineH;
        int lastLine  = (yPixOffset + clientH) / m_lineH + 1;
        firstLine = std::max(0, firstLine);
        lastLine  = std::min((int)m_lines.size() - 1, lastLine);

        dc.SetFont(m_font);

        for (int i = firstLine; i <= lastLine; ++i)
            DrawLine(dc, i, i * m_lineH);
    }

    // ── Miniwindows: draw-on-top pass ──────────────────────────────────────
    if (m_fn_composite_miniwindows) {
        dc.SetDeviceOrigin(0, 0);
        m_fn_composite_miniwindows(dc, clientW, clientH, /*bUnder=*/false);
    }
}

// ── DrawLine ──────────────────────────────────────────────────────────────────

void COutputPane::DrawLine(wxDC& dc, int lineIdx, int yOff)
{
    const SOutputLine& line = m_lines[lineIdx];

    // ── Horizontal rule ───────────────────────────────────────────────────────
    if (line.size() == 1 && line[0].horiz_rule) {
        int clientW, clientH;
        GetClientSize(&clientW, &clientH);
        int scrollX, scrollY;
        GetViewStart(&scrollX, &scrollY);
        int unitW, unitH;
        GetScrollPixelsPerUnit(&unitW, &unitH);
        int xLeft  = scrollX * unitW;
        int xRight = xLeft + clientW;
        int midY   = yOff + m_lineH / 2;

        dc.SetPen(wxPen(wxColour(96, 96, 96), 1));
        dc.DrawLine(xLeft + 4, midY, xRight - 4, midY);

        wxColour bgC(GetRValue(m_bgColor), GetGValue(m_bgColor), GetBValue(m_bgColor));
        dc.SetBrush(wxBrush(bgC));
        dc.SetPen(*wxTRANSPARENT_PEN);
        if (midY > yOff)
            dc.DrawRectangle(xLeft, yOff, clientW, midY - yOff);
        if (midY + 1 < yOff + m_lineH)
            dc.DrawRectangle(xLeft, midY + 1, clientW, yOff + m_lineH - midY - 1);
        return;
    }

    // ── Compute selection range for this line ─────────────────────────────────
    SelPoint selStart, selEnd;
    bool hasSel = GetNormalizedSel(selStart, selEnd);
    bool lineInSel = hasSel &&
                     lineIdx >= selStart.line &&
                     lineIdx <= selEnd.line;

    // Character columns of selection start/end within this line.
    // selColEnd == INT_MAX means "to end of line" (selection continues on next line).
    int selColStart = 0;
    int selColEnd   = INT_MAX;
    if (lineInSel) {
        selColStart = (lineIdx == selStart.line) ? selStart.col : 0;
        selColEnd   = (lineIdx == selEnd.line)   ? selEnd.col   : INT_MAX;
    }

    // ── Render runs ───────────────────────────────────────────────────────────
    int x          = 0;
    int currentCol = 0;

    for (const auto& run : line) {
        if (run.text.empty()) continue;

        wxString wxText = wxString::FromUTF8(run.text.c_str(), run.text.size());
        int runLen = (int)wxText.size();

        if (!lineInSel) {
            // Fast path: no selection on this line — draw the whole run at once.
            DrawRunChunk(dc, wxText, x, yOff, run, false);
            x += runLen * m_charW;
        } else {
            // Split the run into up to three segments:
            //   Part 1  [0,       splitA)  — before selection
            //   Part 2  [splitA,  splitB)  — within selection
            //   Part 3  [splitB,  runLen)  — after selection
            int splitA = std::max(0, std::min(runLen, selColStart - currentCol));
            int splitB = std::max(0, std::min(runLen,
                                  (selColEnd == INT_MAX) ? runLen
                                                         : selColEnd - currentCol));

            if (splitA > 0) {
                wxString before = wxText.substr(0, splitA);
                DrawRunChunk(dc, before, x, yOff, run, false);
                x += splitA * m_charW;
            }
            if (splitB > splitA) {
                wxString sel = wxText.substr(splitA, splitB - splitA);
                DrawRunChunk(dc, sel, x, yOff, run, /*selected=*/true);
                x += (splitB - splitA) * m_charW;
            }
            if (splitB < runLen) {
                wxString after = wxText.substr(splitB);
                DrawRunChunk(dc, after, x, yOff, run, false);
                x += (runLen - splitB) * m_charW;
            }
        }

        currentCol += runLen;
    }

    // Fill the remainder of the line with the pane background.
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

    // Yellow left-margin search marker.
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
            int xLeft = scrollX * unitW;
            dc.SetBrush(wxBrush(wxColour(255, 220, 0)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(xLeft, yOff, 3, m_lineH);
        }
    }
}

// ── DrawRunChunk ──────────────────────────────────────────────────────────────

void COutputPane::DrawRunChunk(wxDC& dc, const wxString& text, int x, int yOff,
                                const SOutputRun& run, bool selected)
{
    if (text.IsEmpty()) return;

    wxColour fg(GetRValue(run.fg), GetGValue(run.fg), GetBValue(run.fg));
    wxColour bg(GetRValue(run.bg), GetGValue(run.bg), GetBValue(run.bg));

    bool isLink = !run.action.empty();
    wxColour drawFg = (isLink && !selected) ? wxColour(0x44, 0xAA, 0xFF) : fg;

    if (selected) {
        // Invert: text in background colour on foreground-colour background.
        dc.SetTextForeground(bg);
        dc.SetTextBackground(isLink ? wxColour(0x44, 0xAA, 0xFF) : fg);
    } else {
        dc.SetTextForeground(drawFg);
        dc.SetTextBackground(bg);
    }
    dc.SetBackgroundMode(wxSOLID);

    bool drawUnderline = run.underline || isLink;
    if (run.bold && drawUnderline)
        dc.SetFont(wxFont(m_font.GetPointSize(), m_font.GetFamily(),
                          m_font.GetStyle(), wxFONTWEIGHT_BOLD, true));
    else if (run.bold)
        dc.SetFont(wxFont(m_font.GetPointSize(), m_font.GetFamily(),
                          m_font.GetStyle(), wxFONTWEIGHT_BOLD, false));
    else if (drawUnderline)
        dc.SetFont(wxFont(m_font.GetPointSize(), m_font.GetFamily(),
                          m_font.GetStyle(), m_font.GetWeight(), true));
    else
        dc.SetFont(m_font);

    dc.DrawText(text, x, yOff);
}

// ── Scroll helpers ────────────────────────────────────────────────────────────

void COutputPane::UpdateScrollbars(bool scrollToBottom)
{
    int clientW, clientH;
    GetClientSize(&clientW, &clientH);

    int totalH = (int)m_lines.size() * m_lineH;
    int totalW = std::max(clientW, m_maxLineChars * m_charW + m_charW);

    SetVirtualSize(totalW, totalH);

    if (scrollToBottom && !m_lines.empty()) {
        int unitH;
        GetScrollPixelsPerUnit(nullptr, &unitH);
        if (unitH > 0) {
            int scrollX;
            GetViewStart(&scrollX, nullptr);

            // GetScrollRange returns the total number of scroll units.
            // Scroll() clamps to (scrollRange - pageSize) = (totalH - clientH)/unitH,
            // which is exactly the position that shows the last line at the bottom.
            int scrollRange = GetScrollRange(wxVERTICAL);
            Scroll(scrollX, scrollRange);
        }
        m_trimmedCount = 0;
    } else if (m_trimmedCount > 0 && !m_autoScroll) {
        // The buffer trimmed lines from the front while the user was scrolled up.
        // Compensate by shifting the view upward by the same number of line-heights
        // so the visible content stays stable.
        int scrollX, scrollY, unitH;
        GetViewStart(&scrollX, &scrollY);
        GetScrollPixelsPerUnit(nullptr, &unitH);
        if (unitH > 0) {
            int shift = (m_trimmedCount * m_lineH + unitH - 1) / unitH;
            Scroll(scrollX, std::max(0, scrollY - shift));
        }
        m_trimmedCount = 0;
    }
}

void COutputPane::OnScrollWinUpdate(wxScrollWinEvent& evt)
{
    evt.Skip();
    CallAfter([this]() {
        if (!this) return;
        int scrollY;
        GetViewStart(nullptr, &scrollY);
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

// ── Selection helpers ─────────────────────────────────────────────────────────

int COutputPane::YToLine(int clientY) const
{
    if (m_lines.empty()) return 0;

    int scrollY;
    const_cast<COutputPane*>(this)->GetViewStart(nullptr, &scrollY);
    int unitH;
    const_cast<COutputPane*>(this)->GetScrollPixelsPerUnit(nullptr, &unitH);
    int yPix = scrollY * unitH + clientY;
    int line  = (m_lineH > 0) ? yPix / m_lineH : 0;
    return std::max(0, std::min((int)m_lines.size() - 1, line));
}

int COutputPane::XToChar(int lineIdx, int clientX) const
{
    if (lineIdx < 0 || lineIdx >= (int)m_lines.size() || m_charW <= 0)
        return 0;

    // Convert client x to virtual (unscrolled) x.
    int scrollX;
    const_cast<COutputPane*>(this)->GetViewStart(&scrollX, nullptr);
    int unitW;
    const_cast<COutputPane*>(this)->GetScrollPixelsPerUnit(&unitW, nullptr);
    int virtualX = clientX + scrollX * unitW;

    if (virtualX <= 0) return 0;

    // Count total characters on the line.
    int totalChars = 0;
    for (const auto& run : m_lines[lineIdx])
        totalChars += (int)run.text.size();

    // For a monospace font every character is m_charW pixels wide.
    // Round to nearest character boundary.
    int col = (virtualX + m_charW / 2) / m_charW;
    return std::max(0, std::min(totalChars, col));
}

SelPoint COutputPane::MouseToSelPoint(const wxMouseEvent& evt) const
{
    int line = YToLine(evt.GetY());
    int col  = XToChar(line, evt.GetX());
    return SelPoint{line, col};
}

bool COutputPane::GetNormalizedSel(SelPoint& start, SelPoint& end) const
{
    if (!m_selAnchor.valid() || !m_selFocus.valid()) return false;
    if (m_selAnchor == m_selFocus) return false;

    if (m_selAnchor < m_selFocus) {
        start = m_selAnchor;
        end   = m_selFocus;
    } else {
        start = m_selFocus;
        end   = m_selAnchor;
    }
    return true;
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

void COutputPane::OnLeftDown(wxMouseEvent& evt)
{
    // Give miniwindows first chance.
    if (m_fn_miniwindow_mouse_down &&
        m_fn_miniwindow_mouse_down(evt.GetX(), evt.GetY(), 0x10 /*MW_MOUSE_LH*/))
    {
        evt.Skip();
        return;
    }

    m_linkAtDown.clear();

    // Shift+click: extend the existing selection (keep anchor, move focus).
    if (evt.ShiftDown() && m_selAnchor.valid()) {
        m_selFocus = MouseToSelPoint(evt);
        Refresh(false);
        evt.Skip();
        return;
    }

    // Check whether the click landed on a link run.  We store the action now
    // but don't fire it yet — we fire on mouse-up only if no drag occurred so
    // that the user can still select/copy link text by dragging over it.
    if (m_lineH > 0 && !m_lines.empty()) {
        int lineIdx = YToLine(evt.GetY());
        if (lineIdx >= 0 && lineIdx < (int)m_lines.size()) {
            int scrollX, scrollY;
            GetViewStart(&scrollX, &scrollY);
            int unitW, unitH;
            GetScrollPixelsPerUnit(&unitW, &unitH);
            int clickVirtualX = evt.GetX() + scrollX * unitW;

            int runX = 0;
            for (const auto& run : m_lines[lineIdx]) {
                if (run.text.empty()) continue;
                int runW = (int)run.text.size() * m_charW;
                if (!run.action.empty() &&
                    clickVirtualX >= runX &&
                    clickVirtualX <  runX + runW)
                {
                    m_linkAtDown = run.action;
                    break;
                }
                runX += runW;
            }
        }
    }

    // Begin a new selection.  Anchor = focus = click position (empty selection).
    m_selAnchor = MouseToSelPoint(evt);
    m_selFocus  = m_selAnchor;
    m_dragging  = true;
    m_dragStartY = evt.GetY();
    CaptureMouse();
    Refresh(false);
    evt.Skip();
}

void COutputPane::OnLeftUp(wxMouseEvent& evt)
{
    if (m_fn_miniwindow_mouse_up)
        m_fn_miniwindow_mouse_up(evt.GetX(), evt.GetY(), 0x10 /*MW_MOUSE_LH*/);

    if (m_dragging) {
        m_dragging  = false;
        m_selFocus  = MouseToSelPoint(evt);
        if (HasCapture()) ReleaseMouse();
        Refresh(false);

        // Fire a link click only when the mouse didn't move (no drag).
        // This lets the user drag over link text to select/copy it.
        if (!m_linkAtDown.empty() && m_selAnchor == m_selFocus) {
            if (m_fn_link_clicked)
                m_fn_link_clicked(m_linkAtDown);
        }
        m_linkAtDown.clear();

        // Auto-copy non-empty selection to clipboard (terminal-style).
        SelPoint s, e;
        if (GetNormalizedSel(s, e)) {
            wxString text = SelectedText();
            if (!text.IsEmpty() && wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(text));
                wxTheClipboard->Close();
            }
        }
    }

    // Return focus to the input bar so the user can keep typing.
    if (m_fn_return_focus)
        m_fn_return_focus();

    evt.Skip();
}

void COutputPane::OnLeftDClick(wxMouseEvent& evt)
{
    if (m_fn_miniwindow_mouse_down &&
        m_fn_miniwindow_mouse_down(evt.GetX(), evt.GetY(), 0x50 /*LH|DBL*/))
    {
        evt.Skip();
        return;
    }

    // Double-click: select the word under the cursor.
    if (!m_lines.empty() && m_lineH > 0) {
        int lineIdx = YToLine(evt.GetY());
        if (lineIdx >= 0 && lineIdx < (int)m_lines.size()) {
            // Build plain text for the line.
            wxString lineText;
            for (const auto& run : m_lines[lineIdx])
                if (!run.horiz_rule)
                    lineText += wxString::FromUTF8(run.text.c_str(),
                                                   run.text.size());

            int col = XToChar(lineIdx, evt.GetX());
            col = std::max(0, std::min((int)lineText.size(), col));

            // Expand left to word start.
            int wordStart = col;
            while (wordStart > 0 &&
                   (wxIsalnum(lineText[wordStart - 1]) ||
                    lineText[wordStart - 1] == '_'))
                --wordStart;

            // Expand right to word end.
            int wordEnd = col;
            while (wordEnd < (int)lineText.size() &&
                   (wxIsalnum(lineText[wordEnd]) ||
                    lineText[wordEnd] == '_'))
                ++wordEnd;

            if (wordEnd > wordStart) {
                m_selAnchor = SelPoint{lineIdx, wordStart};
                m_selFocus  = SelPoint{lineIdx, wordEnd};
                Refresh(false);
            }
        }
    }

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
    if (m_fn_miniwindow_mouse_move)
        m_fn_miniwindow_mouse_move(evt.GetX(), evt.GetY());

    // Tooltip update (skip during drag to avoid flicker).
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
        m_selFocus = MouseToSelPoint(evt);
        Refresh(false);

        // Auto-scroll when dragging past edges.
        int clientW, clientH;
        GetClientSize(&clientW, &clientH);
        if (evt.GetY() > clientH - 10)
            ScrollLines(1);
        else if (evt.GetY() < 10)
            ScrollLines(-1);
    } else {
        // Update cursor: hand when over a link run, I-beam otherwise.
        SelPoint pt = MouseToSelPoint(evt);
        bool overLink = false;
        if (pt.valid() && pt.line < (int)m_lines.size()) {
            const SOutputLine& ln = m_lines[pt.line];
            int col = 0;
            for (const auto& run : ln) {
                int runLen = (int)run.text.size();
                if (!run.action.empty() &&
                    pt.col >= col && pt.col < col + runLen)
                {
                    overLink = true;
                    break;
                }
                col += runLen;
            }
        }
        SetCursor(overLink ? wxCursor(wxCURSOR_HAND)
                           : wxCursor(wxCURSOR_IBEAM));
    }
    evt.Skip();
}

void COutputPane::OnRightDown(wxMouseEvent& evt)
{
    if (m_fn_miniwindow_mouse_down &&
        m_fn_miniwindow_mouse_down(evt.GetX(), evt.GetY(), 0x20 /*MW_MOUSE_RH*/))
    {
        evt.Skip();
        return;
    }
    SelPoint s, e;
    bool hasSel = GetNormalizedSel(s, e);

    // Detect whether the click landed on a link run.
    // Use the same character-column approximation as OnMotion.
    std::string linkAtClick;
    {
        int lineIdx = YToLine(evt.GetY());
        int col     = XToChar(lineIdx, evt.GetX());
        if (lineIdx >= 0 && lineIdx < (int)m_lines.size()) {
            int c = 0;
            for (const auto& run : m_lines[lineIdx]) {
                int runLen = (int)run.text.size();
                if (!run.action.empty() &&
                    col >= c && col < c + runLen)
                {
                    linkAtClick = run.action;
                    break;
                }
                c += runLen;
            }
        }
    }

    wxMenu menu;
    menu.Append(ID_COPY,      "Copy\tCtrl+C");
    menu.Append(ID_SELECTALL, "Select All\tCtrl+A");
    menu.AppendSeparator();
    menu.Append(ID_CLEAR,     "Clear Output");
    menu.Enable(ID_COPY, hasSel);

    // If a URL link was right-clicked, add browser/clipboard options.
    bool isURL = !linkAtClick.empty() &&
                 (linkAtClick.compare(0, 7, "http://")  == 0 ||
                  linkAtClick.compare(0, 8, "https://") == 0);
    if (isURL) {
        menu.AppendSeparator();

        wxMenuItem* itemOpen = menu.Append(wxID_ANY, "Open URL in Browser");
        menu.Bind(wxEVT_MENU,
            [this, linkAtClick](wxCommandEvent&) {
                if (m_fn_link_clicked) m_fn_link_clicked(linkAtClick);
            }, itemOpen->GetId());

        wxMenuItem* itemCopyURL = menu.Append(wxID_ANY, "Copy URL");
        menu.Bind(wxEVT_MENU,
            [linkAtClick](wxCommandEvent&) {
                if (wxTheClipboard->Open()) {
                    wxTheClipboard->SetData(new wxTextDataObject(
                        wxString::FromUTF8(linkAtClick.c_str())));
                    wxTheClipboard->Close();
                }
            }, itemCopyURL->GetId());
    }

    PopupMenu(&menu, evt.GetPosition());
}

void COutputPane::OnMouseWheel(wxMouseEvent& evt)
{
    if (evt.ControlDown()) {
        AdjustFontSize((evt.GetWheelRotation() > 0) ? 1 : -1);
        return;
    }

    if (m_fn_miniwindow_mouse_wheel) {
        if (m_fn_miniwindow_mouse_wheel(evt.GetX(), evt.GetY(),
                                        evt.GetWheelRotation()))
            return;
    }

    evt.Skip();
    CallAfter([this]() {
        int scrollY;
        GetViewStart(nullptr, &scrollY);
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

    if (code == 'C' && evt.ControlDown()) { CopySelection(); return; }
    if (code == 'A' && evt.ControlDown()) { SelectAll();     return; }
    if (code == 'L' && evt.ControlDown()) { ClearOutput();   return; }

    if (evt.ControlDown() &&
        (code == '=' || code == '+' || code == WXK_NUMPAD_ADD)) {
        AdjustFontSize(1); return;
    }
    if (evt.ControlDown() &&
        (code == '-' || code == WXK_NUMPAD_SUBTRACT)) {
        AdjustFontSize(-1); return;
    }
    if (evt.ControlDown() && code == '0') { ResetFontSize(); return; }

    if (evt.ControlDown() && code == WXK_HOME) {
        // Ctrl+Home: scroll to the very top of the buffer.
        int scrollX;
        GetViewStart(&scrollX, nullptr);
        Scroll(scrollX, 0);
        bool wasAuto = m_autoScroll;
        m_autoScroll = false;
        if (wasAuto && m_fn_autoscroll_changed) m_fn_autoscroll_changed(false);
        return;
    }
    if (evt.ControlDown() && code == WXK_END) {
        // Ctrl+End: scroll to the bottom and re-engage auto-scroll.
        ScrollToBottom();
        return;
    }

    bool isScrollKey = (code == WXK_PAGEUP   || code == WXK_PAGEDOWN ||
                        code == WXK_HOME      || code == WXK_END      ||
                        code == WXK_UP        || code == WXK_DOWN);
    evt.Skip();

    if (isScrollKey) {
        CallAfter([this]() {
            int scrollY;
            GetViewStart(nullptr, &scrollY);
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
}

// ── Context menu handlers ─────────────────────────────────────────────────────

void COutputPane::OnMenuCopy(wxCommandEvent&)      { CopySelection(); }
void COutputPane::OnMenuSelectAll(wxCommandEvent&) { SelectAll(); }
void COutputPane::OnMenuClear(wxCommandEvent&)     { ClearOutput(); }

// ── Selection / clipboard ─────────────────────────────────────────────────────

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
    int lastLine = (int)m_lines.size() - 1;
    int lastCol  = 0;
    for (const auto& run : m_lines[lastLine])
        lastCol += (int)run.text.size();

    m_selAnchor = SelPoint{0, 0};
    m_selFocus  = SelPoint{lastLine, lastCol};
    Refresh(false);
}

wxString COutputPane::SelectedText() const
{
    SelPoint selStart, selEnd;
    if (!GetNormalizedSel(selStart, selEnd)) return wxEmptyString;

    int lo = std::max(0, selStart.line);
    int hi = std::min((int)m_lines.size() - 1, selEnd.line);

    wxString result;
    for (int i = lo; i <= hi; ++i) {
        if (!m_lines[i].empty() && m_lines[i][0].horiz_rule) {
            result += wxT("────────────────────────────────────────\n");
            continue;
        }

        // Build plain text for this line.
        wxString lineText;
        for (const auto& run : m_lines[i])
            if (!run.horiz_rule)
                lineText += wxString::FromUTF8(run.text.c_str(),
                                               run.text.size());

        int startCol = (i == lo) ? selStart.col : 0;
        int endCol   = (i == hi) ? selEnd.col
                                 : (int)lineText.size();
        startCol = std::max(0, std::min((int)lineText.size(), startCol));
        endCol   = std::max(startCol, std::min((int)lineText.size(), endCol));

        result += lineText.substr(startCol, endCol - startCol);
        result += "\n";
    }
    return result;
}

// ── Find / Search ─────────────────────────────────────────────────────────────

bool COutputPane::FindNext(const wxString& term, bool forward,
                            int* outMatchLine, int* outTotalMatches)
{
    if (outMatchLine)    *outMatchLine    = -1;
    if (outTotalMatches) *outTotalMatches = 0;

    if (term.IsEmpty() || m_lines.empty()) return false;

    wxString lowerTerm = term.Lower();
    int n = (int)m_lines.size();

    SelPoint selStart, selEnd;
    GetNormalizedSel(selStart, selEnd);

    int startIdx;
    if (forward) {
        startIdx = selEnd.valid() ? selEnd.line + 1 : n - 1;
        if (startIdx >= n) startIdx = 0;
    } else {
        startIdx = selStart.valid() ? selStart.line - 1 : 0;
        if (startIdx < 0) startIdx = n - 1;
    }

    auto lineText = [this](int i) -> wxString {
        wxString s;
        for (const auto& run : m_lines[i])
            if (!run.horiz_rule)
                s += wxString::FromUTF8(run.text.c_str(), run.text.size());
        return s.Lower();
    };

    int foundIdx = -1;
    int idx = startIdx;
    for (int pass = 0; pass < n; ++pass) {
        if (lineText(idx).Contains(lowerTerm)) {
            foundIdx = idx;
            break;
        }
        if (forward) { if (++idx >= n) idx = 0; }
        else         { if (--idx < 0) idx = n - 1; }
    }

    if (foundIdx < 0) return false;

    // Select and scroll to the matching line.
    wxString lt = lineText(foundIdx);
    int lineLen = (int)lt.size();
    m_selAnchor = SelPoint{foundIdx, 0};
    m_selFocus  = SelPoint{foundIdx, lineLen};

    int unitH;
    GetScrollPixelsPerUnit(nullptr, &unitH);
    if (unitH > 0) {
        int scrollX;
        GetViewStart(&scrollX, nullptr);
        Scroll(scrollX, foundIdx * m_lineH / unitH);
        m_autoScroll = false;
        if (m_fn_autoscroll_changed) m_fn_autoscroll_changed(false);
    }
    Refresh(false);

    if (outMatchLine)    *outMatchLine = foundIdx;

    // Count total matching lines (capped at 9999 to avoid stalling on huge buffers).
    if (outTotalMatches) {
        int total = 0;
        for (int i = 0; i < n && total < 9999; ++i)
            if (lineText(i).Contains(lowerTerm))
                ++total;
        *outTotalMatches = total;
    }

    return true;
}

void COutputPane::HighlightSearch(const wxString& term)
{
    m_searchTerm = term;
    Refresh(false);
}

#endif // !_WIN32
