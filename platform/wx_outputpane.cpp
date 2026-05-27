// platform/wx_outputpane.cpp — Custom output pane implementation

#ifndef _WIN32

#include "platform/wx_outputpane.h"
#include <wx/dcbuffer.h>
#include <wx/clipbrd.h>
#include <wx/tooltip.h>
#include <algorithm>
// compat.h provides RGB(), GetRValue(), GetGValue(), GetBValue().
#include "platform/compat.h"

// Forward declarations for font variant helpers defined after the constructor.
static wxFont MakeFontVariant(const wxFont& base, bool bold, bool underline, bool italic = false);
static void RebuildFontVariants(const wxFont& base,
                                wxFont& outBold,
                                wxFont& outUnderline,
                                wxFont& outBoldUnderline,
                                wxFont& outItalic,
                                wxFont& outBoldItalic,
                                wxFont& outItalicUnderline,
                                wxFont& outBoldItalicUnderline);

// ── Context-menu IDs ─────────────────────────────────────────────────────────
static const int ID_COPY       = wxID_COPY;
static const int ID_SELECTALL  = wxID_SELECTALL;
static const int ID_CLEAR      = wxID_HIGHEST + 100;

// ── UTF-8 character counting ──────────────────────────────────────────────────
// Returns the number of Unicode code points in a UTF-8 encoded std::string.
// This is always <= s.size() (equal for ASCII, less for multi-byte sequences).
// Used wherever we treat character positions as column indices for selection,
// wrapping, and link hit-testing — byte counts are wrong for non-ASCII text.
static inline int UTF8CharLen(const std::string& s)
{
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0u) != 0x80u) ++n;   // count lead bytes; skip continuation bytes
    return n;
}

// ── Hint bar ──────────────────────────────────────────────────────────────────
// Height (px) of the "↓ new output below" banner painted at the bottom of the
// client area when the user has scrolled up and new lines are arriving.
// Kept as a file-scope constant so OnPaint and OnLeftDown stay in sync.
static constexpr int kHintBarH = 22;

// ── Event table ───────────────────────────────────────────────────────────────
wxBEGIN_EVENT_TABLE(COutputPane, wxScrolledWindow)
    EVT_PAINT(                      COutputPane::OnPaint)
    EVT_SIZE(                       COutputPane::OnSize)
    EVT_LEFT_DOWN(                  COutputPane::OnLeftDown)
    EVT_LEFT_UP(                    COutputPane::OnLeftUp)
    EVT_LEFT_DCLICK(                COutputPane::OnLeftDClick)
    EVT_MIDDLE_UP(                  COutputPane::OnMiddleUp)
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
    RebuildFontVariants(m_font, m_fontBold, m_fontUnderline, m_fontBoldUnderline,
                        m_fontItalic, m_fontBoldItalic,
                        m_fontItalicUnderline, m_fontBoldItalicUnderline);

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

    // Horizontal scrolling is disabled: MUD output is pre-wrapped at the server
    // side (typically 80 columns) so a horizontal scrollbar is never useful and
    // breaks the feel of a terminal.  Set the horizontal scroll rate to 0 so
    // wxScrolledWindow never shows or reacts to an H-scrollbar.
    SetScrollRate(0, m_lineH);
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

// ── Font variant cache ────────────────────────────────────────────────────────
// Rebuilds all 8 font variants (bold × italic × underline) from m_font.
// Call whenever m_font changes so DrawRunChunk uses pre-built variants.
static wxFont MakeFontVariant(const wxFont& base, bool bold, bool underline, bool italic)
{
    wxFontStyle style = italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL;
    return wxFont(base.GetPointSize(), base.GetFamily(), style,
                  bold ? wxFONTWEIGHT_BOLD : base.GetWeight(),
                  underline, base.GetFaceName());
}

static void RebuildFontVariants(const wxFont& base,
                                wxFont& outBold,
                                wxFont& outUnderline,
                                wxFont& outBoldUnderline,
                                wxFont& outItalic,
                                wxFont& outBoldItalic,
                                wxFont& outItalicUnderline,
                                wxFont& outBoldItalicUnderline)
{
    outBold                = MakeFontVariant(base, true,  false, false);
    outUnderline           = MakeFontVariant(base, false, true,  false);
    outBoldUnderline       = MakeFontVariant(base, true,  true,  false);
    outItalic              = MakeFontVariant(base, false, false, true);
    outBoldItalic          = MakeFontVariant(base, true,  false, true);
    outItalicUnderline     = MakeFontVariant(base, false, true,  true);
    outBoldItalicUnderline = MakeFontVariant(base, true,  true,  true);
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
    // Pending lines (m_pCurrentLine in the telnet engine, not yet completed)
    // are rendered as a dim trailing row below the committed buffer.  They are
    // never added to m_lines; they simply replace m_pendingLine.
    if (line.pending) {
        m_pendingLine = std::move(line);
        if (m_batchDepth == 0) {
            UpdateScrollbars(m_autoScroll);
            Refresh(false);
        }
        return;
    }

    // A completed line arriving always clears any pending content (the engine
    // has committed the line; the next pending update will carry new partial text).
    m_pendingLine.clear();
    m_pendingLine.pending = false;

    // Auto-detect plain http:// / https:// URLs and split them into link runs.
    line = DetectURLs(std::move(line));

    int lineChars = 0;
    for (const auto& run : line) lineChars += UTF8CharLen(run.text);
    if (lineChars > m_maxLineChars) m_maxLineChars = lineChars;

    // Append to wrap cache before the line is in m_lines so WrapCount() works.
    {
        int wc = (m_wrapCols > 0)
                 ? std::max(1, (lineChars > 0 ? (lineChars + m_wrapCols - 1) / m_wrapCols : 1))
                 : 1;
        m_visualRowStart.push_back(m_totalVisualRows);
        m_totalVisualRows += wc;
    }

    m_lines.push_back(std::move(line));

    int trimCount = 0;
    while ((int)m_lines.size() > m_maxLines) {
        // Accumulate visual rows being removed so UpdateScrollbars can compensate
        // the scroll position (prevents the viewport jumping to different content).
        if (!m_visualRowStart.empty()) {
            int firstStart = m_visualRowStart[0];
            int nextStart  = (m_visualRowStart.size() >= 2)
                             ? m_visualRowStart[1] : m_totalVisualRows;
            m_trimmedRowsRemoved += (nextStart - firstStart);
        }
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
    // Rebuild wrap cache after trimming so all row indices are correct.
    if (trimCount > 0)
        RebuildWrapCache();
    m_trimmedCount += trimCount;

    // Notify the logging hook (if any) immediately after the line is accepted.
    // Done before the batch-depth check so every line is logged even in batches.
    if (m_fn_on_line_appended) m_fn_on_line_appended(m_lines.back());

    if (m_batchDepth == 0) {
        UpdateScrollbars(m_autoScroll);
        // Always repaint: when not auto-scrolling the hint bar needs to be
        // drawn (or kept visible) as new content arrives.
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
        RebuildWrapCache();
        UpdateScrollbars(m_autoScroll);
        Refresh(false);
    }
}

void COutputPane::DeleteTailLines(long count)
{
    if (count <= 0 || m_lines.empty()) return;
    long toRemove = std::min(count, (long)m_lines.size());
    for (long i = 0; i < toRemove; ++i)
        m_lines.pop_back();
    // Clear any selection that may have referenced deleted lines.
    m_selAnchor = SelPoint{};
    m_selFocus  = SelPoint{};
    RebuildWrapCache();
    UpdateScrollbars(m_autoScroll);
    Refresh(false);
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

void COutputPane::SetPaused(bool paused)
{
    if (paused) {
        // Disengage auto-scroll without moving the viewport: new lines will
        // continue to be appended to the buffer but won't scroll into view.
        bool wasAuto = m_autoScroll;
        m_autoScroll = false;
        if (wasAuto && m_fn_autoscroll_changed) m_fn_autoscroll_changed(false);
        Refresh(false);
    } else {
        ScrollToBottom();
    }
}

long COutputPane::GetScrollPositionY() const
{
    int scrollY = 0, unitH = 0;
    const_cast<COutputPane*>(this)->GetViewStart(nullptr, &scrollY);
    const_cast<COutputPane*>(this)->GetScrollPixelsPerUnit(nullptr, &unitH);
    return (long)scrollY * (long)(unitH > 0 ? unitH : m_lineH);
}

void COutputPane::ScrollToTop()
{
    int scrollX;
    GetViewStart(&scrollX, nullptr);
    Scroll(scrollX, 0);
    bool wasAuto = m_autoScroll;
    m_autoScroll = false;
    if (wasAuto && m_fn_autoscroll_changed) m_fn_autoscroll_changed(false);
    Refresh(false);
}

void COutputPane::ScrollToLine(int lineIdx)
{
    if (m_lines.empty()) return;
    lineIdx = std::max(0, std::min((int)m_lines.size() - 1, lineIdx));
    int unitH;
    GetScrollPixelsPerUnit(nullptr, &unitH);
    if (unitH > 0) {
        int vr = (lineIdx < (int)m_visualRowStart.size()) ? m_visualRowStart[lineIdx] : 0;
        Scroll(0, vr * m_lineH / unitH);
    }
    bool wasAuto = m_autoScroll;
    m_autoScroll = false;
    if (wasAuto && m_fn_autoscroll_changed) m_fn_autoscroll_changed(false);
    Refresh(false);
}

void COutputPane::SetSelectionRange(int startLine, int endLine,
                                     int startCol, int endCol)
{
    if (startLine == 0 && endLine == 0 && startCol == 0 && endCol == 0) {
        // Clear selection.
        m_selAnchor = SelPoint{};
        m_selFocus  = SelPoint{};
        Refresh(false);
        return;
    }

    int n = (int)m_lines.size();
    // Convert 1-based → 0-based; clamp to buffer.
    int sl = std::max(0, std::min(n - 1, startLine - 1));
    int el = std::max(0, std::min(n - 1, endLine   - 1));

    // Column 0 in the API means "start / end of line".
    auto lineLen = [&](int li) {
        int len = 0;
        for (const auto& run : m_lines[li]) len += UTF8CharLen(run.text);
        return len;
    };
    int sc = (startCol <= 0) ? 0
           : std::min(lineLen(sl), startCol - 1);
    int ec = (endCol   <= 0) ? lineLen(el)
           : std::min(lineLen(el), endCol - 1);

    m_selAnchor = SelPoint{sl, sc};
    m_selFocus  = SelPoint{el, ec};
    Refresh(false);
}

bool COutputPane::GetSelectionInfo(long& startLine, long& startCol,
                                    long& endLine,   long& endCol) const
{
    SelPoint s, e;
    if (!GetNormalizedSel(s, e)) return false;
    if (s == e) return false;  // empty / collapsed selection

    // Convert 0-based internal coords to 1-based Windows-compatible values.
    startLine = (long)s.line + 1;
    startCol  = (long)s.col  + 1;
    endLine   = (long)e.line + 1;
    endCol    = (long)e.col  + 1;
    return true;
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
    int totalH = m_totalVisualRows * m_lineH;
    bool wasAuto = m_autoScroll;
    m_autoScroll = (unitH > 0) && (newY * unitH + clientH >= totalH - m_lineH);
    if (m_autoScroll != wasAuto && m_fn_autoscroll_changed)
        m_fn_autoscroll_changed(m_autoScroll);
}

void COutputPane::ClearOutput()
{
    m_lines.clear();
    m_selAnchor          = SelPoint{};
    m_selFocus           = SelPoint{};
    m_maxLineChars       = 80;
    m_trimmedCount       = 0;
    m_trimmedRowsRemoved = 0;
    m_visualRowStart.clear();
    m_totalVisualRows    = 0;
    UpdateScrollbars(true);
    Refresh();
}

void COutputPane::SetOutputFont(const wxFont& font)
{
    m_font     = font;
    m_fontSize = font.GetPointSize();
    SetFont(m_font);
    RebuildFontVariants(m_font, m_fontBold, m_fontUnderline, m_fontBoldUnderline,
                        m_fontItalic, m_fontBoldItalic,
                        m_fontItalicUnderline, m_fontBoldItalicUnderline);

    // wxMemoryDC for reliable measurement (same reason as constructor).
    wxBitmap bmp(1, 1, 24);
    wxMemoryDC mdc(bmp);
    mdc.SetFont(m_font);
    wxCoord w, h;
    mdc.GetTextExtent("M", &w, &h);
    if (h > 0) m_lineH = h + 2;
    if (w > 0) m_charW = w;

    m_maxLineChars = 80;
    // Keep horizontal scrolling disabled (same as constructor).
    SetScrollRate(0, m_lineH);
    RebuildWrapCache();
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
    // wxAutoBufferedPaintDC uses native double-buffering when the platform
    // already provides it (macOS Cocoa, GTK3) and a manual backbuffer only when
    // needed.  wxBufferedPaintDC always allocates a bitmap, which on Retina
    // displays produces a blurry 1× bitmap blown up to 2× physical pixels.
    wxAutoBufferedPaintDC dc(this);
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

    if (!m_lines.empty() && m_lineH > 0) {
        // When the hint bar is visible, don't render lines that fall behind it
        // — saves work and avoids paint artifacts on the boundary.
        int usableH = m_autoScroll ? clientH : clientH - kHintBarH;

        // Map visible pixel range → visible logical lines using the wrap cache.
        int firstVR = yPixOffset / m_lineH;
        int lastVR  = (yPixOffset + usableH + m_lineH - 1) / m_lineH;
        int firstLine = VisualRowToLogical(firstVR);
        int lastLine  = VisualRowToLogical(lastVR);
        firstLine = std::max(0, firstLine);
        lastLine  = std::min((int)m_lines.size() - 1, lastLine);

        dc.SetFont(m_font);

        for (int i = firstLine; i <= lastLine; ++i) {
            // yOff of the first visual row of logical line i.
            int lineVR = (i < (int)m_visualRowStart.size()) ? m_visualRowStart[i] : 0;
            DrawLine(dc, i, lineVR * m_lineH);
        }
    }

    // ── In-progress (pending) line ─────────────────────────────────────────
    // The telnet engine's m_pCurrentLine is emitted each timer tick as a
    // pending SOutputLine and rendered here as a dim trailing row.  It shows
    // partial server output before the '\n' arrives (useful for MUDs that send
    // a prompt without a trailing newline, e.g. "> ").
    if (!m_pendingLine.empty() && m_lineH > 0) {
        int pendingY = m_totalVisualRows * m_lineH;  // virtual y, just after committed lines
        int clientTop = scrollY * unitH;
        if (pendingY >= clientTop && pendingY < clientTop + clientH) {
            dc.SetFont(m_font);
            // Draw each run of the pending line at reduced opacity by blending
            // the foreground halfway toward the background (gives a dim look).
            int x = 0;
            for (const auto& run : m_pendingLine) {
                if (run.text.empty()) continue;
                // Dim the fg colour: blend 50% toward bg.
                auto blend = [](uint8_t fg, uint8_t bg) -> uint8_t {
                    return (uint8_t)((int(fg) + int(bg)) / 2);
                };
                uint8_t bgR = GetRValue(m_bgColor), bgG = GetGValue(m_bgColor),
                        bgB = GetBValue(m_bgColor);
                wxColour fg(blend(GetRValue(run.fg), bgR),
                            blend(GetGValue(run.fg), bgG),
                            blend(GetBValue(run.fg), bgB));
                wxColour bg(bgR, bgG, bgB);
                dc.SetTextForeground(fg);
                dc.SetTextBackground(bg);
                dc.SetBackgroundMode(wxSOLID);
                wxString text = wxString::FromUTF8(run.text.c_str());
                dc.DrawText(text, x, pendingY);
                wxCoord tw, th;
                dc.GetTextExtent(text, &tw, &th);
                x += tw;
            }
        }
    }

    // ── Miniwindows: draw-on-top pass ──────────────────────────────────────
    if (m_fn_composite_miniwindows) {
        dc.SetDeviceOrigin(0, 0);
        m_fn_composite_miniwindows(dc, clientW, clientH, /*bUnder=*/false);
    }

    // ── "Scroll to bottom" hint bar ────────────────────────────────────────
    // When the user has scrolled up and new lines are arriving, paint a
    // yellow banner at the bottom of the visible client area.  Clicking it
    // (handled in OnLeftDown) or pressing Escape in the input bar snaps the
    // view back to the live bottom.  Drawn after miniwindows so it's always
    // on top.  Uses client coordinates (device origin reset to 0,0).
    if (!m_autoScroll && !m_lines.empty()) {
        dc.SetDeviceOrigin(0, 0);

        // Background bar.
        dc.SetBrush(wxBrush(wxColour(255, 200, 0)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, clientH - kHintBarH, clientW, kHintBarH);

        // Centred label.
        dc.SetFont(m_font);
        dc.SetTextForeground(wxColour(30, 30, 30));
        dc.SetBackgroundMode(wxTRANSPARENT);
        wxString msg =
            wxT("↓  New output below — click here or press Escape to scroll down");
        wxCoord tw, th;
        dc.GetTextExtent(msg, &tw, &th);
        dc.DrawText(msg,
                    (clientW - tw) / 2,
                    clientH - kHintBarH + (kHintBarH - th) / 2);
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

    // ── Shared geometry ───────────────────────────────────────────────────────
    int clientW;
    GetClientSize(&clientW, nullptr);
    // Horizontal scrolling is disabled, so the right edge is always clientW.
    // (PrepareDC has already set up the virtual coordinate system.)
    const int rightEdge = clientW;
    wxColour bgC(GetRValue(m_bgColor), GetGValue(m_bgColor), GetBValue(m_bgColor));
    const int wrapCols = std::max(1, m_wrapCols);

    // ── Compute selection range for this line ─────────────────────────────────
    SelPoint selStart, selEnd;
    bool hasSel = GetNormalizedSel(selStart, selEnd);
    bool lineInSel = hasSel &&
                     lineIdx >= selStart.line &&
                     lineIdx <= selEnd.line;

    // Selection boundaries as absolute char offsets within this logical line.
    // selColEnd == INT_MAX means "selection continues past end of line".
    int selColStart = 0;
    int selColEnd   = INT_MAX;
    if (lineInSel) {
        selColStart = (lineIdx == selStart.line) ? selStart.col : 0;
        selColEnd   = (lineIdx == selEnd.line)   ? selEnd.col   : INT_MAX;
    }

    // ── Render runs with visual wrapping ─────────────────────────────────────
    // x:          pixel x within current visual row (horizontal scroll = 0 always)
    // currentCol: character column within current visual row [0, wrapCols)
    // currentY:   y-pixel top of current visual row
    // globalChar: absolute char offset in this logical line (for selection)
    int x          = 0;
    int currentCol = 0;
    int currentY   = yOff;
    int globalChar = 0;

    for (const auto& run : line) {
        if (run.text.empty()) continue;

        wxString wxText = wxString::FromUTF8(run.text.c_str(), run.text.size());
        int runLen = (int)wxText.size();
        int pos    = 0;

        while (pos < runLen) {
            // How many chars fit on the current visual row.
            int avail = wrapCols - currentCol;
            int chunk = std::min(avail, runLen - pos);

            // Selection for this sub-chunk (absolute char range).
            int chunkAbs0 = globalChar + pos;
            int chunkAbs1 = chunkAbs0 + chunk;
            bool inSel    = lineInSel
                            && chunkAbs1 > selColStart
                            && chunkAbs0 < (selColEnd == INT_MAX ? INT_MAX : selColEnd);

            if (!inSel) {
                // Whole sub-chunk: no selection overlap.
                DrawRunChunk(dc, wxText.Mid(pos, chunk), x, currentY, run, false);
                x += chunk * m_charW;
            } else {
                // Split into up to three sub-segments: before / inside / after selection.
                int lo = std::max(0, selColStart - chunkAbs0);
                int hi = std::min(chunk, (selColEnd == INT_MAX ? chunk
                                                               : selColEnd - chunkAbs0));
                if (lo > 0) {
                    DrawRunChunk(dc, wxText.Mid(pos, lo), x, currentY, run, false);
                    x += lo * m_charW;
                }
                if (hi > lo) {
                    DrawRunChunk(dc, wxText.Mid(pos + lo, hi - lo), x, currentY, run, true);
                    x += (hi - lo) * m_charW;
                }
                if (hi < chunk) {
                    DrawRunChunk(dc, wxText.Mid(pos + hi, chunk - hi), x, currentY, run, false);
                    x += (chunk - hi) * m_charW;
                }
            }

            currentCol += chunk;
            pos        += chunk;

            // Wrap to next visual row when this row is full and more text follows.
            if (currentCol >= wrapCols && pos < runLen) {
                // Fill remainder of this row with background.
                if (x < rightEdge) {
                    dc.SetBrush(wxBrush(bgC));
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.DrawRectangle(x, currentY, rightEdge - x, m_lineH);
                }
                currentCol = 0;
                x          = 0;
                currentY  += m_lineH;
            }
        }

        globalChar += runLen;
    }

    // Fill the remainder of the last visual row.
    // When the selection extends past the end of this line (selColEnd == INT_MAX
    // means "selected through to the next line"), fill with the system highlight
    // colour so the selection band stretches to the right margin — this is the
    // standard visual for multi-line selections.
    if (x < rightEdge) {
        bool trailSelected = lineInSel && (selColEnd == INT_MAX);
        wxColour fillCol = trailSelected
            ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT)
            : bgC;
        dc.SetBrush(wxBrush(fillCol));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(x, currentY, rightEdge - x, m_lineH);
    }

    // ── Search term: left-margin marker + in-line text highlight ─────────────
    if (!m_searchTerm.IsEmpty()) {
        // Build plain text for the whole line.
        wxString plain;
        for (const auto& run : line)
            if (!run.horiz_rule)
                plain += wxString::FromUTF8(run.text.c_str(), run.text.size());

        // Apply case folding to both the line text and the search term unless
        // we're in case-sensitive mode.
        wxString cmpPlain = m_searchCaseSensitive ? plain         : plain.Lower();
        wxString cmpTerm  = m_searchCaseSensitive ? m_searchTerm  : m_searchTerm.Lower();
        int termLen = (int)cmpTerm.size();

        struct HitRange { int start, end; };
        std::vector<HitRange> hits;
        if (termLen > 0) {
            std::wstring pw = cmpPlain.ToStdWstring();
            std::wstring tw = cmpTerm.ToStdWstring();
            size_t p = 0;
            while ((p = pw.find(tw, p)) != std::wstring::npos) {
                hits.push_back({(int)p, (int)(p + tw.size())});
                ++p;
            }
        }

        if (!hits.empty()) {
            // Left-margin bar: 3-pixel yellow stripe on each visual row of this line.
            dc.SetBrush(wxBrush(wxColour(255, 220, 0)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            int wc = WrapCount(lineIdx);
            for (int vr = 0; vr < wc; ++vr)
                dc.DrawRectangle(0, yOff + vr * m_lineH, 3, m_lineH);

            // In-line highlight: re-render each matching span with yellow bg + black text.
            // Pre-fill background rectangles (consistent with DrawRunChunk strategy).
            dc.SetTextForeground(wxColour(0, 0, 0));
            dc.SetBackgroundMode(wxTRANSPARENT);

            int cx = 0;  // absolute char index in the logical line
            for (const auto& run : line) {
                if (run.text.empty() || run.horiz_rule) continue;
                wxString wxText = wxString::FromUTF8(run.text.c_str(), run.text.size());
                int runLen = (int)wxText.size();

                bool drawUnderline = run.underline || !run.action.empty();
                if      (run.bold && run.italic && drawUnderline) dc.SetFont(m_fontBoldItalicUnderline);
                else if (run.bold && run.italic)                  dc.SetFont(m_fontBoldItalic);
                else if (run.italic && drawUnderline)             dc.SetFont(m_fontItalicUnderline);
                else if (run.italic)                              dc.SetFont(m_fontItalic);
                else if (run.bold && drawUnderline)               dc.SetFont(m_fontBoldUnderline);
                else if (run.bold)                                dc.SetFont(m_fontBold);
                else if (drawUnderline)                           dc.SetFont(m_fontUnderline);
                else                                              dc.SetFont(m_font);

                for (const auto& hit : hits) {
                    int lo = std::max(cx, hit.start);
                    int hi = std::min(cx + runLen, hit.end);
                    if (lo >= hi) continue;

                    // Render matching chars at their wrapped visual position,
                    // batching contiguous characters on the same visual row.
                    int c = lo;
                    while (c < hi) {
                        int vr       = c / wrapCols;   // visual row index within this line
                        int vcol     = c % wrapCols;   // column within that visual row
                        int py       = yOff + vr * m_lineH;
                        int px       = vcol * m_charW;
                        // Batch to end-of-match or end-of-visual-row, whichever comes first.
                        int batchEnd = std::min(hi, (vr + 1) * wrapCols);
                        int nChars   = batchEnd - c;
                        // Pre-fill the highlight background rectangle.
                        dc.SetBrush(wxBrush(wxColour(255, 220, 0)));
                        dc.SetPen(*wxTRANSPARENT_PEN);
                        dc.DrawRectangle(px, py, nChars * m_charW, m_lineH);
                        // c - cx = offset of c within wxText.
                        dc.DrawText(wxText.substr(c - cx, nChars), px, py);
                        c = batchEnd;
                    }
                }

                cx += runLen;
            }

            // Restore DC state so subsequent DrawLine calls start clean.
            dc.SetFont(m_font);
        }
    }
}

// ── DrawRunChunk ──────────────────────────────────────────────────────────────
// Renders one sub-chunk of a run at pixel position (x, yOff).
//
// Rendering strategy: pre-fill the background rectangle first, then draw text
// with wxTRANSPARENT background mode.  This is superior to wxSOLID mode for
// two reasons:
//   1. The fill width is exactly text.size() * m_charW — our integer grid —
//      which prevents sub-pixel gaps between chunks on HiDPI / Retina displays
//      where the font's ink bounding box may differ from the character cell by
//      a fraction of a pixel.
//   2. Transparent text drawing lets Core Text / Pango anti-alias the glyphs
//      against the already-correct background rather than against a blank DC.

void COutputPane::DrawRunChunk(wxDC& dc, const wxString& text, int x, int yOff,
                                const SOutputRun& run, bool selected)
{
    if (text.IsEmpty()) return;

    wxColour fg(GetRValue(run.fg), GetGValue(run.fg), GetBValue(run.fg));
    wxColour bg(GetRValue(run.bg), GetGValue(run.bg), GetBValue(run.bg));

    bool isLink = !run.action.empty();

    // ── Background fill ───────────────────────────────────────────────────────
    // For selected text, use the OS system selection colours (wxSYS_COLOUR_HIGHLIGHT
    // / wxSYS_COLOUR_HIGHLIGHTTEXT) rather than inverting the run's own fg/bg.
    // Inverting can produce invisible results when fg == bg (e.g. white-on-white
    // runs in some MUD colour schemes); the system colours are always legible.
    wxColour bgFill;
    wxColour textFg;
    if (selected) {
        bgFill  = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
        textFg  = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);
    } else if (isLink) {
        bgFill  = bg;
        textFg  = wxColour(0x44, 0xAA, 0xFF);  // link blue
    } else {
        bgFill  = bg;
        textFg  = fg;
    }

    dc.SetBrush(wxBrush(bgFill));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(x, yOff, (int)text.size() * m_charW, m_lineH);

    dc.SetTextForeground(textFg);
    dc.SetBackgroundMode(wxTRANSPARENT);

    // Select the pre-built font variant to avoid per-run wxFont allocation.
    // 8 variants: bold × italic × underline (link text is treated as underlined).
    bool drawUnderline = run.underline || isLink;
    if      (run.bold && run.italic && drawUnderline) dc.SetFont(m_fontBoldItalicUnderline);
    else if (run.bold && run.italic)                  dc.SetFont(m_fontBoldItalic);
    else if (run.italic && drawUnderline)             dc.SetFont(m_fontItalicUnderline);
    else if (run.italic)                              dc.SetFont(m_fontItalic);
    else if (run.bold && drawUnderline)               dc.SetFont(m_fontBoldUnderline);
    else if (run.bold)                                dc.SetFont(m_fontBold);
    else if (drawUnderline)                           dc.SetFont(m_fontUnderline);
    else                                              dc.SetFont(m_font);

    dc.DrawText(text, x, yOff);

    // Strikethrough: draw a horizontal line through the vertical centre of the glyph.
    if (run.strikethrough) {
        int midY = yOff + m_lineH / 2;
        int textW = (int)text.size() * m_charW;
        dc.SetPen(wxPen(textFg, 1));
        dc.DrawLine(x, midY, x + textW, midY);
    }
}

// ── Wrap cache helpers ────────────────────────────────────────────────────────

int COutputPane::WrapCount(int lineIdx) const
{
    if (lineIdx < 0 || lineIdx >= (int)m_lines.size()) return 0;
    const SOutputLine& line = m_lines[lineIdx];
    if (line.size() == 1 && line[0].horiz_rule) return 1;
    int chars = 0;
    for (const auto& run : line)
        chars += UTF8CharLen(run.text);
    if (chars == 0 || m_wrapCols <= 0) return 1;
    return (chars + m_wrapCols - 1) / m_wrapCols;
}

void COutputPane::RebuildWrapCache()
{
    int clientW;
    GetClientSize(&clientW, nullptr);
    int newCols = (m_charW > 0 && clientW > 0)
                  ? std::max(1, clientW / m_charW)
                  : 80;
    m_wrapCols = newCols;

    int n = (int)m_lines.size();
    m_visualRowStart.resize(n);
    m_totalVisualRows = 0;
    for (int i = 0; i < n; ++i) {
        m_visualRowStart[i] = m_totalVisualRows;
        m_totalVisualRows  += WrapCount(i);
    }
}

int COutputPane::VisualRowToLogical(int vr) const
{
    if (m_visualRowStart.empty()) return 0;
    vr = std::max(0, vr);
    // Binary search: largest i such that m_visualRowStart[i] <= vr.
    int lo = 0, hi = (int)m_visualRowStart.size() - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (m_visualRowStart[mid] <= vr) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

// ── Scroll helpers ────────────────────────────────────────────────────────────

void COutputPane::UpdateScrollbars(bool scrollToBottom)
{
    int clientW, clientH;
    GetClientSize(&clientW, &clientH);

    // Add one row for the in-progress (pending) line when it has content.
    int pendingRows = (!m_pendingLine.empty()) ? 1 : 0;
    int totalH = (m_totalVisualRows + pendingRows) * m_lineH;

    // Virtual width = client width: horizontal scrolling is disabled; long
    // lines are visually wrapped rather than requiring H-scroll.
    SetVirtualSize(clientW, totalH);

    if (scrollToBottom && !m_lines.empty()) {
        int unitH;
        GetScrollPixelsPerUnit(nullptr, &unitH);
        if (unitH > 0) {
            // Scroll() clamps to (scrollRange - pageSize), which is the position
            // that shows the last line at the bottom.  Passing scrollRange directly
            // is equivalent to Scroll(-1, -1) but without the ambiguity.
            Scroll(0, GetScrollRange(wxVERTICAL));
        }
        m_trimmedCount = 0;
    } else if (m_trimmedCount > 0 && !m_autoScroll) {
        // The buffer trimmed lines from the front while the user was scrolled up.
        // Compensate: shift the view up by the number of visual rows removed so
        // the visible content stays stable (no jump when old lines scroll off).
        int scrollY, unitH;
        GetViewStart(nullptr, &scrollY);
        GetScrollPixelsPerUnit(nullptr, &unitH);
        if (unitH > 0) {
            int shift = (m_trimmedRowsRemoved * m_lineH + unitH - 1) / unitH;
            Scroll(0, std::max(0, scrollY - shift));
        }
        m_trimmedCount      = 0;
        m_trimmedRowsRemoved = 0;
    }
}

void COutputPane::OnScrollWinUpdate(wxScrollWinEvent& evt)
{
    evt.Skip();
    CallAfter([this]() {
        int scrollY;
        GetViewStart(nullptr, &scrollY);
        int unitH;
        GetScrollPixelsPerUnit(nullptr, &unitH);
        int clientH;
        GetClientSize(nullptr, &clientH);
        int totalH = m_totalVisualRows * m_lineH;
        bool wasAuto = m_autoScroll;
        bool atBottom = (unitH > 0) &&
                        (scrollY * unitH + clientH >= totalH - m_lineH);
        // If auto-freeze is disabled the pane stays in auto-scroll mode
        // regardless of scroll position; new output always scrolls into view.
        m_autoScroll = atBottom || !m_allowAutoFreeze;
        if (m_autoScroll != wasAuto && m_fn_autoscroll_changed)
            m_fn_autoscroll_changed(m_autoScroll);
    });
}

void COutputPane::OnSize(wxSizeEvent& evt)
{
    RebuildWrapCache();
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
    int yPix   = scrollY * unitH + clientY;
    int visRow = (m_lineH > 0) ? yPix / m_lineH : 0;
    // Map visual row → logical line via the wrap cache.
    return std::max(0, std::min((int)m_lines.size() - 1,
                                const_cast<COutputPane*>(this)->VisualRowToLogical(visRow)));
}

int COutputPane::XToChar(int lineIdx, int clientX, int clientY) const
{
    if (lineIdx < 0 || lineIdx >= (int)m_lines.size() || m_charW <= 0)
        return 0;

    // Count total characters on the line (Unicode code points, not UTF-8 bytes).
    int totalChars = 0;
    for (const auto& run : m_lines[lineIdx])
        totalChars += UTF8CharLen(run.text);

    // Convert client x to virtual (unscrolled) x.
    // Horizontal scroll is always 0 (H-scrolling is disabled), but keep the
    // arithmetic correct in case that ever changes.
    int scrollX, scrollY;
    const_cast<COutputPane*>(this)->GetViewStart(&scrollX, &scrollY);
    int unitW, unitH;
    const_cast<COutputPane*>(this)->GetScrollPixelsPerUnit(&unitW, &unitH);
    int virtualX = clientX + scrollX * unitW;

    // For wrapped lines we need to know which visual row within the logical
    // line was clicked, so we can add (vrOffset * m_wrapCols) to the column.
    int vrOffset = 0;
    if (m_wrapCols > 0 && m_lineH > 0) {
        int yPix       = scrollY * unitH + clientY;
        int clickVR    = yPix / m_lineH;
        int lineStartVR = (lineIdx < (int)m_visualRowStart.size())
                          ? m_visualRowStart[lineIdx] : 0;
        vrOffset = std::max(0, clickVR - lineStartVR);
    }

    // Column within the visual row, then offset by wrapped rows.
    int colInRow = (virtualX + m_charW / 2) / m_charW;
    int col      = vrOffset * m_wrapCols + std::max(0, colInRow);
    return std::max(0, std::min(totalChars, col));
}

SelPoint COutputPane::MouseToSelPoint(const wxMouseEvent& evt) const
{
    return PointToChar(evt.GetX(), evt.GetY());
}

SelPoint COutputPane::PointToChar(int clientX, int clientY) const
{
    int line = YToLine(clientY);
    int col  = XToChar(line, clientX, clientY);
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
    // ── Hint bar: click snaps to the live bottom ───────────────────────────
    // The hint bar is painted on top of everything (miniwindows included), so
    // its click region takes priority.  We check it before the miniwindow hit
    // test so the visual layering is respected.
    if (!m_autoScroll && !m_lines.empty()) {
        int clientH;
        GetClientSize(nullptr, &clientH);
        if (evt.GetY() >= clientH - kHintBarH) {
            ScrollToBottom();
            if (m_fn_return_focus) m_fn_return_focus();
            return;  // consume — don't start a selection
        }
    }

    // ── Triple-click: select the entire line ──────────────────────────────────
    // wxWidgets has no EVT_LEFT_TCLICK.  Detect triple-click as a left-down
    // that arrives within 500 ms of the last double-click on the same line.
    if (!m_lines.empty() && m_lastDClickLine >= 0) {
        wxLongLong elapsed = wxGetLocalTimeMillis() - m_lastDClickTimeMs;
        int clickLine = YToLine(evt.GetY());
        if (elapsed < 500 && clickLine == m_lastDClickLine) {
            // Build the full line length and select it end-to-end.
            int lineLen = 0;
            for (const auto& run : m_lines[clickLine])
                if (!run.horiz_rule) lineLen += UTF8CharLen(run.text);
            m_selAnchor      = SelPoint{clickLine, 0};
            m_selFocus       = SelPoint{clickLine, lineLen};
            m_lastDClickLine = -1;   // consume so a 4th click starts fresh
            Refresh(false);
            evt.Skip();
            return;
        }
        // Stale or wrong line — reset.
        if (elapsed >= 500) m_lastDClickLine = -1;
    }

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
    // XToChar returns the absolute character column within the logical line,
    // correctly accounting for visual-row wrapping.
    if (m_lineH > 0 && !m_lines.empty()) {
        int lineIdx = YToLine(evt.GetY());
        if (lineIdx >= 0 && lineIdx < (int)m_lines.size()) {
            int clickCol = XToChar(lineIdx, evt.GetX(), evt.GetY());
            int c = 0;
            for (const auto& run : m_lines[lineIdx]) {
                int runLen = UTF8CharLen(run.text);
                if (!run.action.empty() &&
                    clickCol >= c && clickCol < c + runLen)
                {
                    m_linkAtDown = run.action;
                    break;
                }
                c += runLen;
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
            if (!text.IsEmpty()) {
                if (wxTheClipboard->Open()) {
                    wxTheClipboard->SetData(new wxTextDataObject(text));
                    wxTheClipboard->Close();
                }
#ifdef __WXGTK__
                // X11 primary selection: also write to the PRIMARY clipboard
                // so the user can paste with middle-click in other GTK windows.
                if (wxTheClipboard->Open()) {
                    wxTheClipboard->UsePrimarySelection(true);
                    wxTheClipboard->SetData(new wxTextDataObject(text));
                    wxTheClipboard->UsePrimarySelection(false);
                    wxTheClipboard->Close();
                }
#endif
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

            int col = XToChar(lineIdx, evt.GetX(), evt.GetY());
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

            // Record for triple-click line selection.
            m_lastDClickTimeMs = wxGetLocalTimeMillis();
            m_lastDClickLine   = lineIdx;
        }
    }

    evt.Skip();
}

// ── OnMiddleUp ────────────────────────────────────────────────────────────────
// Middle-click: paste the primary selection (X11/Linux) or system clipboard
// (macOS) into the command input bar — mirrors standard terminal behaviour.
// The text is inserted verbatim without sending it to the MUD; the user can
// review and edit it before pressing Enter.
void COutputPane::OnMiddleUp(wxMouseEvent& evt)
{
    wxString text;

#ifdef __WXGTK__
    // On GTK (Linux/X11), primary selection = last text highlighted anywhere.
    // wxWidgets exposes it by temporarily switching the clipboard into
    // primary-selection mode.
    if (wxTheClipboard->Open()) {
        wxTheClipboard->UsePrimarySelection(true);
        if (wxTheClipboard->IsSupported(wxDF_TEXT) ||
            wxTheClipboard->IsSupported(wxDF_UNICODETEXT))
        {
            wxTextDataObject data;
            wxTheClipboard->GetData(data);
            text = data.GetText();
        }
        wxTheClipboard->UsePrimarySelection(false);
        wxTheClipboard->Close();
    }
#else
    // On macOS (and Windows, if ever reached) fall back to the system clipboard.
    if (wxTheClipboard->Open()) {
        if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT) ||
            wxTheClipboard->IsSupported(wxDF_TEXT))
        {
            wxTextDataObject data;
            wxTheClipboard->GetData(data);
            text = data.GetText();
        }
        wxTheClipboard->Close();
    }
#endif

    if (!text.IsEmpty() && m_fn_put_in_input)
        m_fn_put_in_input(text);

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

        // Auto-scroll when dragging past the top or bottom edges.
        // When the hint bar is visible, treat its upper edge as the bottom
        // boundary so the drag doesn't fight with the hint-bar click region.
        int clientW, clientH;
        GetClientSize(&clientW, &clientH);
        int bottomEdge = (!m_autoScroll && !m_lines.empty())
                       ? clientH - kHintBarH - 10
                       : clientH - 10;
        if (evt.GetY() > bottomEdge)
            ScrollLines(1);
        else if (evt.GetY() < 10)
            ScrollLines(-1);
    } else {
        // Update cursor.  Priority order:
        //   1. Hint bar area → pointing hand (clicking snaps to bottom).
        //   2. Over an MXP / URL link run → pointing hand.
        //   3. Anywhere else → I-beam text cursor.
        int clientH;
        GetClientSize(nullptr, &clientH);
        if (!m_autoScroll && !m_lines.empty() &&
            evt.GetY() >= clientH - kHintBarH)
        {
            SetCursor(wxCursor(wxCURSOR_HAND));
        } else {
            SelPoint pt = MouseToSelPoint(evt);
            bool overLink = false;
            if (pt.valid() && pt.line < (int)m_lines.size()) {
                // pt.col is the absolute char column in the logical line
                // (already wrap-aware via XToChar in MouseToSelPoint).
                int c = 0;
                for (const auto& run : m_lines[pt.line]) {
                    int runLen = UTF8CharLen(run.text);
                    if (!run.action.empty() &&
                        pt.col >= c && pt.col < c + runLen)
                    {
                        overLink = true;
                        break;
                    }
                    c += runLen;
                }
            }
            SetCursor(overLink ? wxCursor(wxCURSOR_HAND)
                               : wxCursor(wxCURSOR_IBEAM));
        }
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
    // XToChar returns the absolute char column in the logical line,
    // correctly accounting for visual-row wrapping.
    std::string linkAtClick;
    {
        int lineIdx = YToLine(evt.GetY());
        if (lineIdx >= 0 && lineIdx < (int)m_lines.size()) {
            int col = XToChar(lineIdx, evt.GetX(), evt.GetY());
            int c = 0;
            for (const auto& run : m_lines[lineIdx]) {
                int runLen = UTF8CharLen(run.text);
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
    menu.Enable(ID_COPY, hasSel);

    // When text is selected, offer quick-send shortcuts.
    if (hasSel) {
        menu.AppendSeparator();

        // "Put in Input Bar": copy selection into the command input for editing.
        // Ctrl+G is also wired in OnKeyDown for keyboard-only use.
        wxMenuItem* itemInput = menu.Append(wxID_ANY,
            wxT("Put in Input Bar\tCtrl+G"));
        menu.Enable(itemInput->GetId(), m_fn_put_in_input != nullptr);
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            if (m_fn_put_in_input) m_fn_put_in_input(SelectedText());
        }, itemInput->GetId());

        // "Send to MUD": evaluate the selection as a command immediately.
        wxMenuItem* itemSend = menu.Append(wxID_ANY,
            wxT("Send to MUD"));
        menu.Enable(itemSend->GetId(), m_fn_send_to_mud != nullptr);
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            if (m_fn_send_to_mud) m_fn_send_to_mud(SelectedText());
        }, itemSend->GetId());
    }

    menu.AppendSeparator();
    menu.Append(ID_CLEAR, "Clear Output\tCtrl+L");

    // Zoom and find — for discoverability (same as keyboard shortcuts).
    menu.AppendSeparator();
    {
        wxMenuItem* fi = menu.Append(wxID_ANY, "Find…\tCtrl+F");
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            if (m_fn_find_next_key) m_fn_find_next_key(true);
        }, fi->GetId());

        wxMenuItem* zi = menu.Append(wxID_ANY, "Zoom In\tCtrl++");
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { AdjustFontSize(1); }, zi->GetId());
        wxMenuItem* zo = menu.Append(wxID_ANY, "Zoom Out\tCtrl+-");
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { AdjustFontSize(-1); }, zo->GetId());
        wxMenuItem* zr = menu.Append(wxID_ANY, "Reset Zoom\tCtrl+0");
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { ResetFontSize(); }, zr->GetId());
    }

    // Session log toggle.
    if (m_fn_toggle_log) {
        menu.AppendSeparator();
        bool logging = m_fn_is_logging && m_fn_is_logging();
        wxMenuItem* itemLog = menu.Append(wxID_ANY,
            logging ? wxT("Stop Logging") : wxT("Start Logging…"));
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            if (m_fn_toggle_log) m_fn_toggle_log();
        }, itemLog->GetId());
    }

    // If a link was right-clicked, add appropriate context menu options.
    if (!linkAtClick.empty()) {
        bool isURL = (linkAtClick.compare(0, 7, "http://")  == 0 ||
                      linkAtClick.compare(0, 8, "https://") == 0);
        menu.AppendSeparator();

        if (isURL) {
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
        } else {
            // MXP send-link or Lua Hyperlink() action.
            // Split pipe-delimited alternatives and show each as a menu item.
            std::vector<std::string> choices;
            {
                size_t start = 0;
                for (;;) {
                    size_t sep = linkAtClick.find('|', start);
                    std::string part = linkAtClick.substr(start,
                        (sep == std::string::npos) ? std::string::npos : sep - start);
                    if (!part.empty()) choices.push_back(std::move(part));
                    if (sep == std::string::npos) break;
                    start = sep + 1;
                }
            }
            for (const std::string& ch : choices) {
                wxString label = wxString::Format(wxT("Send: %s"),
                    wxString::FromUTF8(ch.c_str(), ch.size()));
                wxMenuItem* item = menu.Append(wxID_ANY, label);
                menu.Bind(wxEVT_MENU,
                    [this, cmd = ch](wxCommandEvent&) {
                        if (m_fn_link_clicked) m_fn_link_clicked(cmd);
                    }, item->GetId());
            }
            if (choices.size() > 1) {
                // Also offer "Send first" as a quick one-click action.
                wxMenuItem* itemAll = menu.Append(wxID_ANY,
                    wxT("Send All (in order)"));
                menu.Bind(wxEVT_MENU,
                    [this, linkAtClick](wxCommandEvent&) {
                        if (m_fn_link_clicked) m_fn_link_clicked(linkAtClick);
                    }, itemAll->GetId());
            }
            wxMenuItem* itemCopy = menu.Append(wxID_ANY, "Copy Action");
            menu.Bind(wxEVT_MENU,
                [linkAtClick](wxCommandEvent&) {
                    if (wxTheClipboard->Open()) {
                        wxTheClipboard->SetData(new wxTextDataObject(
                            wxString::FromUTF8(linkAtClick.c_str())));
                        wxTheClipboard->Close();
                    }
                }, itemCopy->GetId());
        }
    }

    PopupMenu(&menu, evt.GetPosition());
}

void COutputPane::OnMouseWheel(wxMouseEvent& evt)
{
    if (evt.RawControlDown()) {
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
        int totalH = m_totalVisualRows * m_lineH;
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

    // Escape: scroll to bottom (if paused) then return focus to the input bar.
    if (code == WXK_ESCAPE) {
        if (!m_autoScroll && !m_lines.empty()) ScrollToBottom();
        if (m_fn_return_focus) m_fn_return_focus();
        return;
    }

    if (code == 'C' && evt.RawControlDown()) { CopySelection(); return; }
    if (code == 'A' && evt.RawControlDown()) { SelectAll();     return; }
    if (code == 'L' && evt.RawControlDown()) { ClearOutput();   return; }

    // Ctrl+G: put selected text into the input bar (output→input shortcut).
    if (code == 'G' && evt.RawControlDown()) {
        if (m_fn_put_in_input) {
            wxString sel = SelectedText();
            if (!sel.IsEmpty()) m_fn_put_in_input(sel);
        }
        return;
    }

    if (code == WXK_F3) {
        if (m_fn_find_next_key) m_fn_find_next_key(!evt.ShiftDown());
        return;
    }

    // Ctrl+F: show the find bar and search forward.
    if (code == 'F' && evt.RawControlDown()) {
        if (m_fn_find_next_key) m_fn_find_next_key(true);
        return;
    }

    if (evt.RawControlDown() &&
        (code == '=' || code == '+' || code == WXK_NUMPAD_ADD)) {
        AdjustFontSize(1); return;
    }
    if (evt.RawControlDown() &&
        (code == '-' || code == WXK_NUMPAD_SUBTRACT)) {
        AdjustFontSize(-1); return;
    }
    if (evt.RawControlDown() && code == '0') { ResetFontSize(); return; }

    if (evt.RawControlDown() && code == WXK_HOME) {
        // Ctrl+Home: scroll to the very top of the buffer.
        int scrollX;
        GetViewStart(&scrollX, nullptr);
        Scroll(scrollX, 0);
        bool wasAuto = m_autoScroll;
        m_autoScroll = false;
        if (wasAuto && m_fn_autoscroll_changed) m_fn_autoscroll_changed(false);
        return;
    }
    if (evt.RawControlDown() && code == WXK_END) {
        // Ctrl+End: scroll to the bottom and re-engage auto-scroll.
        ScrollToBottom();
        return;
    }

    // Forward printable characters to the command input bar so the user can
    // type without having to click back into it after scrolling through output.
    // Only forward when no modifier is held (Ctrl/Alt combos are shortcuts).
    if (!evt.RawControlDown() && !evt.AltDown() && !evt.MetaDown()) {
        wxChar ch = (wxChar)evt.GetUnicodeKey();
        if (ch != WXK_NONE && wxIsprint(ch)) {
            if (m_fn_key_to_input) { m_fn_key_to_input(ch); return; }
        }
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
            int totalH = m_totalVisualRows * m_lineH;
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
        lastCol += UTF8CharLen(run.text);

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
        if (i < hi) result += "\n";   // newline between lines, not after the last
    }
    return result;
}

// ── Find / Search ─────────────────────────────────────────────────────────────

bool COutputPane::FindNext(const wxString& term, bool forward,
                            int* outMatchLine, int* outOrdinal, int* outTotalMatches,
                            bool caseSensitive)
{
    if (outMatchLine)    *outMatchLine    = -1;
    if (outOrdinal)      *outOrdinal      = 0;
    if (outTotalMatches) *outTotalMatches = 0;

    if (term.IsEmpty() || m_lines.empty()) return false;

    // For case-insensitive search, lower-case the term once and compare against
    // lower-cased line text.  For case-sensitive search, compare as-is.
    wxString searchTerm = caseSensitive ? term : term.Lower();
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

    // Returns the (optionally lowercased) plain text of line i for matching.
    auto lineText = [this, caseSensitive](int i) -> wxString {
        wxString s;
        for (const auto& run : m_lines[i])
            if (!run.horiz_rule)
                s += wxString::FromUTF8(run.text.c_str(), run.text.size());
        return caseSensitive ? s : s.Lower();
    };

    int foundIdx = -1;
    int idx = startIdx;
    for (int pass = 0; pass < n; ++pass) {
        if (lineText(idx).Contains(searchTerm)) {
            foundIdx = idx;
            break;
        }
        if (forward) { if (++idx >= n) idx = 0; }
        else         { if (--idx < 0) idx = n - 1; }
    }

    if (foundIdx < 0) return false;

    // Select the precise matched substring (not the whole line) so the user
    // can see exactly which occurrence was found.
    {
        wxString lt      = lineText(foundIdx);
        size_t   matchOff = lt.find(searchTerm);
        int selStart = (matchOff != wxString::npos) ? (int)matchOff : 0;
        int selEnd   = selStart + (int)searchTerm.size();
        m_selAnchor  = SelPoint{foundIdx, selStart};
        m_selFocus   = SelPoint{foundIdx, selEnd};
    }

    {
        int unitH;
        GetScrollPixelsPerUnit(nullptr, &unitH);
        if (unitH > 0) {
            int clientH;
            GetClientSize(nullptr, &clientH);
            int vr = (foundIdx < (int)m_visualRowStart.size())
                     ? m_visualRowStart[foundIdx] : foundIdx;
            // Center the matched line in the viewport rather than placing it at
            // the top: subtract half the client height so the match lands near
            // the middle of the screen.  std::max(0,…) clamps near the buffer top.
            int yPixel    = vr * m_lineH;
            int scrollPix = std::max(0, yPixel - (clientH - m_lineH) / 2);
            Scroll(0, scrollPix / unitH);
            m_autoScroll = false;
            if (m_fn_autoscroll_changed) m_fn_autoscroll_changed(false);
        }
    }
    Refresh(false);

    if (outMatchLine) *outMatchLine = foundIdx;

    // Count total matching lines and the 1-based ordinal of the match in one
    // linear scan.  Capped at 9999 to avoid stalling on huge buffers.
    if (outOrdinal || outTotalMatches) {
        int total   = 0;
        int ordinal = 0;
        for (int i = 0; i < n && total < 9999; ++i) {
            if (lineText(i).Contains(searchTerm)) {
                ++total;
                if (i <= foundIdx) ++ordinal;
            }
        }
        if (outOrdinal)      *outOrdinal      = ordinal;
        if (outTotalMatches) *outTotalMatches = total;
    }

    return true;
}

void COutputPane::HighlightSearch(const wxString& term, bool caseSensitive)
{
    m_searchTerm          = term;
    m_searchCaseSensitive = caseSensitive;
    Refresh(false);
}

#endif // !_WIN32
