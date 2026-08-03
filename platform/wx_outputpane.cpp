// platform/wx_outputpane.cpp — Custom output pane implementation

#ifndef _WIN32

#include "platform/wx_outputpane.h"
#include <wx/dcbuffer.h>
#include <wx/clipbrd.h>
#include <wx/tooltip.h>
#include <algorithm>
// compat.h provides RGB(), GetRValue(), GetGValue(), GetBValue().
#include "platform/compat.h"
#include "pal/scroll_geometry.h"   // pal::isScrolledToBottom — the shared predicate

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

// ── Non-UTF-8 world text decoding ─────────────────────────────────────────────
// Map a Windows LOGFONT charset (the world's m_font_charset) to a wxFontEncoding,
// so a non-UTF-8 world's bytes decode the way the Windows 8-bit render path shows
// them (GDI maps bytes→glyphs via the selected font's charset).  Unmapped/ANSI/
// DEFAULT fall through to CP1252 (Windows Latin-1).
// External linkage: wx_surface.cpp reuses it for legacy-charset WindowText
// bytes (the decided ISurface charset fix — see docs/PAL.md).
wxFontEncoding WinCharsetToEncoding(int cs)
{
    switch (cs) {
        case 128: return wxFONTENCODING_CP932;   // SHIFTJIS_CHARSET    (Japanese)
        case 129: return wxFONTENCODING_CP949;   // HANGUL_CHARSET      (Korean)
        case 134: return wxFONTENCODING_CP936;   // GB2312_CHARSET      (Simplified Chinese)
        case 136: return wxFONTENCODING_CP950;   // CHINESEBIG5_CHARSET (Traditional Chinese)
        case 161: return wxFONTENCODING_CP1253;  // GREEK_CHARSET
        case 162: return wxFONTENCODING_CP1254;  // TURKISH_CHARSET
        case 163: return wxFONTENCODING_CP1258;  // VIETNAMESE_CHARSET
        case 177: return wxFONTENCODING_CP1255;  // HEBREW_CHARSET
        case 178: return wxFONTENCODING_CP1256;  // ARABIC_CHARSET
        case 186: return wxFONTENCODING_CP1257;  // BALTIC_CHARSET
        case 204: return wxFONTENCODING_CP1251;  // RUSSIAN_CHARSET     (Cyrillic)
        case 222: return wxFONTENCODING_CP874;   // THAI_CHARSET
        case 238: return wxFONTENCODING_CP1250;  // EASTEUROPE_CHARSET
        case 255: return wxFONTENCODING_CP437;   // OEM_CHARSET
        default:  return wxFONTENCODING_CP1252;  // ANSI / DEFAULT / unmapped
    }
}

// Decode a run's bytes to a wxString.  UTF-8 worlds (the default, and the common
// modern case) go straight through FromUTF8.  A non-UTF-8 world's bytes are
// decoded with its font charset; if that conversion fails outright (wxCSConv
// yields empty for non-empty input) we fall back to Latin-1 (From8BitData never
// fails), so text is always visible rather than silently dropped — the bug this
// fixes was FromUTF8() of non-UTF-8 bytes returning "" and the whole run vanishing.
// Exposed (declared in wx_outputpane.h) because Print World decodes the doc's
// raw style bytes with the same rule — it reads m_LineList rather than the pane,
// and a bare FromUTF8 on legacy-charset bytes makes whole runs vanish.
wxString DecodeOutputBytes(const char* bytes, size_t n, bool utf8, int charset)
{
    if (utf8)
        return wxString::FromUTF8(bytes, n);
    if (n == 0)
        return wxString();
    wxCSConv conv(WinCharsetToEncoding(charset));
    wxString s(bytes, conv, n);
    if (s.empty())
        s = wxString::From8BitData(bytes, n);
    return s;
}

static wxString DecodeRunText(const SOutputRun& run)
{
    return DecodeOutputBytes(run.text.c_str(), run.text.size(),
                             run.utf8, run.charset);
}

// Rendered character count of a run — the metric DrawLine measures with, so all
// column/wrap/selection/hit-test bookkeeping must use THIS, not raw lead-byte
// counting.  For UTF-8 runs it equals the lead-byte count; for a non-UTF-8 world
// the bytes decode through the world charset, and a byte in 0x80–0xBF (e.g. a
// CP1252 curly quote) looks like a UTF-8 continuation byte — counting lead bytes
// would undercount and drift the selection/wrap against the rendered grid.
static inline int RunCharLen(const SOutputRun& run)
{
    return run.utf8 ? UTF8CharLen(run.text) : (int)DecodeRunText(run).size();
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
            // These runs are synthesised by the port, not by MXP, so no engine
            // option governs them and nothing else gives them a link look.  The
            // painter no longer force-colours/underlines action-bearing runs (so
            // that the world's link-colour and underline options are honoured),
            // which means the affordance has to be baked in here or a bare
            // http:// in MUD output would render as ordinary text.
            urlRun.fg        = 0x00FFAA44;   // COLORREF 0x00BBGGRR = RGB(0x44,0xAA,0xFF)
            urlRun.underline = true;
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
    // Script running inside the mid-paint ON_PLUGIN_DRAW_OUTPUT_WINDOW
    // callback may print (Note etc.), which lands here while OnPaint is
    // iterating m_lines.  Defer such appends until the paint completes
    // (docs/PAL.md S4c re-entrancy guard; Windows relies on the documented
    // draw-only discipline instead).
    if (m_inPaint) {
        auto pending = std::make_shared<SOutputLine>(std::move(line));
        CallAfter([this, pending]() { AppendLine(std::move(*pending)); });
        return;
    }

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
    for (const auto& run : line) lineChars += RunCharLen(run);
    if (lineChars > m_maxLineChars) m_maxLineChars = lineChars;

    // Append to wrap cache before the line is in m_lines so WrapCount() works.
    {
        int wc = (m_wrapCols > 0)
                 ? std::max(1, (lineChars > 0 ? (lineChars + m_wrapCols - 1) / m_wrapCols : 1))
                 : 1;
        m_visualRowStart.push_back(m_rowBase + m_totalVisualRows);
        m_totalVisualRows += wc;
    }

    m_lineChars.push_back(lineChars);
    m_lines.push_back(std::move(line));

    int trimCount = 0;
    while ((int)m_lines.size() > m_maxLines) {
        // Accumulate visual rows being removed so UpdateScrollbars can compensate
        // the scroll position (prevents the viewport jumping to different content).
        if (!m_visualRowStart.empty()) {
            const long long firstStart = m_visualRowStart[0];
            const long long nextStart  = (m_visualRowStart.size() >= 2)
                             ? m_visualRowStart[1] : (m_rowBase + m_totalVisualRows);
            m_trimmedRowsRemoved += (int)(nextStart - firstStart);
        }
        m_lines.pop_front();
        PopFrontWrapCache();
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
    // No wrap-cache rebuild here: PopFrontWrapCache already kept the cache
    // consistent in O(1) per trimmed line.  This used to be a full O(N) rebuild
    // per APPENDED line once the buffer was at capacity -- i.e. permanently, for
    // any long-running session -- and each rebuild re-scanned every run's bytes.
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
        // "Smoother scrolling": paint now rather than at the next idle, so a
        // burst of MUD output scrolls in step instead of in jumps.  This is
        // MFC's "Invalidate(); if (App.m_bSmootherScrolling) UpdateWindow();".
        if (m_smootherScroll) Update();
    }
}

void COutputPane::SetMaxLines(int n)
{
    if (n < 100) n = 100;
    m_maxLines = n;
    bool trimmed = false;
    while ((int)m_lines.size() > m_maxLines) {
        m_lines.pop_front();
        if (!m_lineChars.empty()) m_lineChars.pop_front();
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
    for (long i = 0; i < toRemove; ++i) {
        m_lines.pop_back();
        if (!m_lineChars.empty()) m_lineChars.pop_back();
    }
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

// World Properties > Appearance > Output > "Pixel offset".  Changing it changes
// how many characters fit on a row, so the wrap cache and the scrollbars have to
// be rebuilt -- the same order the pane's deferred font-metrics path uses.
void COutputPane::SetTextPixelOffset(int px)
{
    // The dialog and the scripting option both clamp to 0..20
    // (DDV_MinMaxInt at prefspropertypages.cpp:5849, and the 0/20 bounds on
    // "pixel_offset" in scriptingoptions_numeric.inc:122).  Clamp again here so
    // a value written straight into the doc can't produce a negative width.
    // No DPI scaling: wx logical pixels already are Windows' 100%-scale pixels.
    px = std::max(0, std::min(20, px));
    if (px == m_pixelOffset) return;
    m_pixelOffset = px;
    RebuildWrapCache();
    UpdateScrollbars(m_autoScroll);
    Refresh();
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
        int vr = RowStartOf(lineIdx);
        Scroll(0, vr * m_lineH / unitH);
    }
    bool wasAuto = m_autoScroll;
    m_autoScroll = false;
    if (wasAuto && m_fn_autoscroll_changed) m_fn_autoscroll_changed(false);
    Refresh(false);
}

// ── Bookmarks (Display ▸ Bookmark Selection / Go To Bookmark) ─────────────────
void COutputPane::ToggleBookmark()
{
    int line = -1;
    SelPoint s, e;
    if (GetNormalizedSel(s, e))      line = (int) s.line;             // selection start
    else if (!m_lines.empty())       line = (int) m_lines.size() - 1; // else the last line
    if (line < 0 || line >= (int) m_lines.size()) return;
    m_lines[line].bookmark = !m_lines[line].bookmark;
    Refresh(false);
}

void COutputPane::GotoNextBookmark()
{
    const int n = (int) m_lines.size();
    if (n == 0) return;
    for (int i = 1; i <= n; ++i) {     // start just past the last one we jumped to
        int idx = (((m_lastBookmarkLine % n) + n + i) % n);
        if (m_lines[idx].bookmark) {
            m_lastBookmarkLine = idx;
            ScrollToLine(idx);
            return;
        }
    }
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

    // Nothing to select.  With an empty buffer both indices clamp to 0 and the
    // lineLen lambda below indexes m_lines[0] on an empty deque — world.Note
    // has not run yet, or world.DeleteOutput just emptied the pane, and a
    // script calling world.SetSelection reads past the end.
    //
    // Windows cannot reach this state (ClearOutput re-seeds one CLine,
    // doc.cpp:2358) and its setOutputSelection only stores the four indices
    // without touching the line list, so a selection over a nonexistent line is
    // simply not visible or copyable there.  Clearing the selection reproduces
    // that observable result.
    if (m_lines.empty()) {
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
        for (const auto& run : m_lines[li]) len += RunCharLen(run);
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
    int viewH = TextViewportH();   // text band height (honours world.TextRectangle)
    int pageUnits = std::max(1, viewH / unitH - 1);

    int newY;
    if (direction < 0)
        newY = std::max(0, scrollY - pageUnits);
    else
        newY = scrollY + pageUnits;   // Scroll() clamps to valid range

    Scroll(scrollX, newY);

    // Re-evaluate whether we're now at the bottom.
    RefreshAutoScrollFromPosition();
}

// ── RefreshAutoScrollFromPosition ─────────────────────────────────────────
// The single answer to "should new output still drag the view down?", shared by
// every path the user can scroll through.  See pal/scroll_geometry.h for why it
// asks the scrollbar rather than recomputing the document height: the two used
// to disagree by up to a row, and the fudge that hid that gave the pane a
// one-row dead zone at the bottom where a gentle scroll up was ignored and the
// next line of output snapped the view back down.
void COutputPane::RefreshAutoScrollFromPosition()
{
    int scrollY = 0;
    GetViewStart(nullptr, &scrollY);
    int unitH = 0;
    GetScrollPixelsPerUnit(nullptr, &unitH);
    int clientH = 0;
    GetClientSize(nullptr, &clientH);

    const bool atBottom = pal::isScrolledToBottom(scrollY, unitH, clientH,
                                                  GetVirtualSize().y);

    const bool wasAuto = m_autoScroll;
    // With auto-freeze disabled (world option m_bAutoFreeze) the pane never
    // freezes: new output is always visible wherever the user has scrolled to.
    // Only the scrollbar path used to honour that; wheel and keyboard scrolling
    // froze the pane anyway.
    m_autoScroll = atBottom || !m_allowAutoFreeze;
    if (m_autoScroll != wasAuto && m_fn_autoscroll_changed)
        m_fn_autoscroll_changed(m_autoScroll);
}

std::vector<int> COutputPane::BookmarkOffsetsFromEnd() const
{
    std::vector<int> out;
    const int n = (int) m_lines.size();
    for (int i = 0; i < n; ++i)
        if (m_lines[i].bookmark)
            out.push_back(n - 1 - i);
    return out;
}

void COutputPane::RestoreBookmarkOffsetsFromEnd(const std::vector<int>& offsets)
{
    const int n = (int) m_lines.size();
    bool any = false;
    for (int off : offsets) {
        const int idx = n - 1 - off;
        if (idx >= 0 && idx < n) { m_lines[idx].bookmark = true; any = true; }
    }
    if (any) Refresh(false);
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
    m_lineChars.clear();
    m_rowBase = 0;
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

    // Use wxClientDC here — the window already exists at this point (SetOutputFont
    // is called from wx_world_child.cpp after creation).  wxClientDC inherits the
    // window's DPI and scale factor, so GetTextExtent returns logical pixel widths
    // that match what DrawText will actually render.  A bitmap-backed wxMemoryDC
    // (used in the constructor) may report physical pixel widths on Retina/HiDPI
    // displays (2× the logical width), causing selection rectangles and click-to-
    // character mapping to be off by a factor of ~2.
    wxCoord w = 0, h = 0;
    {
        wxClientDC cdc(this);
        cdc.SetFont(m_font);
        cdc.GetTextExtent("M", &w, &h);
    }
    // Fallback: if wxClientDC didn't give useful metrics (can happen on some GTK
    // configurations before the first paint), retry with a bitmap-backed MemoryDC.
    if (w <= 0 || h <= 0) {
        wxBitmap bmp(1, 1, 24);
        wxMemoryDC mdc(bmp);
        mdc.SetFont(m_font);
        mdc.GetTextExtent("M", &w, &h);
    }
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

    // Re-measure character metrics from the live rendering DC.  On macOS/HiDPI
    // the wxMemoryDC used during construction may report physical pixel widths
    // (2× logical), while the paint DC works in logical pixels.  If the stored
    // m_charW / m_lineH don't match the DC, selection rectangles and click-to-
    // char mapping are off by the scale factor.  Rebuilding the wrap cache is
    // deferred via CallAfter so it doesn't happen inside the paint handler itself.
    {
        dc.SetFont(m_font);
        wxCoord dcW = 0, dcH = 0;
        dc.GetTextExtent("M", &dcW, &dcH);
        if (dcW > 0 && dcH > 0) {
            int newLineH = dcH + 2;
            if (dcW != m_charW || newLineH != m_lineH) {
                m_charW = dcW;
                m_lineH = newLineH;
                // Defer the wrap-cache rebuild so we don't invalidate layout
                // while in the middle of a paint pass.
                CallAfter([this]() {
                    SetScrollRate(0, m_lineH);
                    RebuildWrapCache();
                    UpdateScrollbars(m_autoScroll);
                    Refresh();
                    if (m_fn_grid_changed) m_fn_grid_changed();
                });
            }
        }
    }

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

    // ── Background image (SetBackgroundImage) ────────────────────────────
    // Right after the colour fill, under everything — the Windows OnDraw
    // order. Viewport-anchored (the Windows view has no scroll transform).
    if (m_fn_draw_background_image) {
        dc.SetDeviceOrigin(0, 0);
        m_fn_draw_background_image(dc, clientW, clientH);
    }

    // ── Text rectangle (world.TextRectangle) ──────────────────────────────
    // A plugin may reserve part of the output area for a docked miniwindow;
    // the text then shifts to its top-left and wraps within its width.
    int trL, trT, trR, trB;
    bool trActive = TextRect(trL, trT, trR, trB);
    if (!trActive) { trL = 0; trT = 0; }
    // If the rectangle changed since the last paint, rebuild the wrap cache
    // (deferred so we don't reflow layout mid-paint).
    if (trActive != m_trActive || trL != m_trL || trT != m_trT ||
        trR != m_trR || trB != m_trB)
    {
        m_trActive = trActive; m_trL = trL; m_trT = trT; m_trR = trR; m_trB = trB;
        CallAfter([this]() {
            RebuildWrapCache();
            UpdateScrollbars(m_autoScroll);
            Refresh();
        });
    }

    // ── Miniwindows: draw-underneath pass ─────────────────────────────────
    if (m_fn_composite_miniwindows) {
        dc.SetDeviceOrigin(0, 0);
        m_fn_composite_miniwindows(dc, clientW, clientH, /*bUnder=*/true);
        dc.SetDeviceOrigin(-scrollX * unitW, -scrollY * unitH);
    }

    // ── ON_PLUGIN_DRAW_OUTPUT_WINDOW (docs/PAL.md S4c) ─────────────────────
    // Give plugins a chance to update miniwindows to match the visible text —
    // fired between the underneath composite and the text pass, exactly where
    // Windows' OnDraw fires it.  The callback runs script: m_inPaint makes
    // AppendLine defer any prints it causes until this paint completes.
    if (m_fn_draw_output_window) {
        m_inPaint = true;
        m_fn_draw_output_window(YToLine(0), yPixOffset);
        m_inPaint = false;
    }

    // Shift the text-drawing origin to the text rectangle's top-left so output
    // (and its selection highlight, which DrawLine renders at the same origin)
    // avoids any docked miniwindows.  No-op when no rectangle is set.
    //
    // m_pixelOffset ("pixel offset of text from side of window") is added here
    // rather than to each DrawText x, so text, selection rectangles, search
    // highlights and the pending line all stay in lockstep automatically.  Only
    // the few things deliberately drawn against the TRUE window edge have to
    // compensate; DrawLine names them.  Everything after the text pass resets
    // the origin to (0,0), so background/foreground images, miniwindows and the
    // hint bar are unaffected -- which matches Windows, where the offset applies
    // to the text pass alone.
    dc.SetDeviceOrigin(trL + m_pixelOffset - scrollX * unitW,
                       trT - scrollY * unitH);

    // Clip output (and its selection highlight) to the text rectangle so nothing
    // spills past its edges — in particular below the bottom margin.  Device
    // coordinates, so the shifted origin above doesn't affect it.  Reset after
    // the text + pending line, before the miniwindow over-pass.
    if (trActive)
        dc.SetDeviceClippingRegion(wxRegion(trL, trT, trR - trL, trB - trT));

    if (!m_lines.empty() && m_lineH > 0) {
        // Rows are drawn within the text viewport: the rectangle's height when a
        // plugin reserved one, else the client (less the hint bar when showing).
        int usableH = trActive ? (trB - trT)
                    : (m_autoScroll ? clientH : clientH - kHintBarH);

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
            int lineVR = RowStartOf(i);
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
                wxString text = DecodeRunText(run);
                dc.DrawText(text, x, pendingY);
                wxCoord tw, th;
                dc.GetTextExtent(text, &tw, &th);
                x += tw;
            }
        }
    }

    if (trActive)
        dc.DestroyClippingRegion();   // text drawing done; miniwindows aren't clipped

    // ── Foreground image (SetForegroundImage) ─────────────────────────────
    // After the text, before the on-top miniwindows — the Windows OnDraw order.
    if (m_fn_draw_foreground_image) {
        dc.SetDeviceOrigin(0, 0);
        m_fn_draw_foreground_image(dc, clientW, clientH);
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
        // The origin is shifted right by m_pixelOffset, so the true window edges
        // are at -m_pixelOffset and clientW - m_pixelOffset in these coordinates.
        // The row's background covers the full window width; the rule itself is
        // inset from the text edges, as on Windows (mushview.cpp:1460-1464 insets
        // it by m_iPixelOffset + 10 on both sides).
        int xLeft  = scrollX * unitW - m_pixelOffset;
        int xRight = xLeft + clientW;
        int midY   = yOff + m_lineH / 2;

        dc.SetPen(wxPen(wxColour(96, 96, 96), 1));
        dc.DrawLine(xLeft + m_pixelOffset + 4, midY,
                    xRight - m_pixelOffset - 4, midY);

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
    // Horizontal scrolling is disabled, so the right edge is always clientW —
    // less m_pixelOffset, because OnPaint shifted the origin right by that much
    // and Windows bleeds to the true client right edge (mushview.cpp:1439 uses
    // cr.right, i.e. the offset insets the LEFT only, never the right).
    const int rightEdge = clientW - m_pixelOffset;
    wxColour bgC(GetRValue(m_bgColor), GetGValue(m_bgColor), GetBValue(m_bgColor));
    auto RunBg = [](const SOutputRun& r) {
        return wxColour(GetRValue(r.bg), GetGValue(r.bg), GetBValue(r.bg));
    };
    const int wrapCols = std::max(1, m_wrapCols);

    // ── Bleed to left ─────────────────────────────────────────────────────────
    // With a pixel offset there is now a real left margin, and Windows fills it
    // with the FIRST run's background when "bleed background colour to edge" is
    // on (mushview.cpp:1335-1388, styleList.GetHead()).  Drawn before the text,
    // across every visual row of this logical line.
    if (m_bleedBackground && m_pixelOffset > 0 && !line.empty()) {
        dc.SetBrush(wxBrush(RunBg(line.front())));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(-m_pixelOffset, yOff,
                         m_pixelOffset, WrapCount(lineIdx) * m_lineH);
    }

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

        wxString wxText = DecodeRunText(run);
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
                // Fill remainder of this row with background.  With "bleed
                // background colour to edge" the run that just wrapped owns the
                // gap, so a coloured line stays coloured to the margin.
                if (x < rightEdge) {
                    dc.SetBrush(wxBrush(m_bleedBackground ? RunBg(run) : bgC));
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
        // Selection wins over bleed, so the selection band still looks like one.
        wxColour fillCol = trailSelected
            ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT)
            : ((m_bleedBackground && !line.empty()) ? RunBg(line.back()) : bgC);
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
                plain += DecodeRunText(run);

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
            // Left-margin bar: 3-pixel yellow stripe on each visual row of this
            // line, against the true window edge (-m_pixelOffset in the shifted
            // coordinates) so it stays a margin marker instead of sliding in
            // over the first character.
            dc.SetBrush(wxBrush(wxColour(255, 220, 0)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            int wc = WrapCount(lineIdx);
            for (int vr = 0; vr < wc; ++vr)
                dc.DrawRectangle(-m_pixelOffset, yOff + vr * m_lineH, 3, m_lineH);

            // In-line highlight: re-render each matching span with yellow bg + black text.
            // Pre-fill background rectangles (consistent with DrawRunChunk strategy).
            dc.SetTextForeground(wxColour(0, 0, 0));
            dc.SetBackgroundMode(wxTRANSPARENT);

            int cx = 0;  // absolute char index in the logical line
            for (const auto& run : line) {
                if (run.text.empty() || run.horiz_rule) continue;
                wxString wxText = DecodeRunText(run);
                int runLen = (int)wxText.size();

                // Underline only when the run actually carries it.  It used to be
                // forced on for any action-bearing run, which overrode the world's
                // "Underline hyperlinks" setting (see DrawRunChunk below).
                bool drawUnderline = run.underline;
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

    // ── Bookmark marker ───────────────────────────────────────────────────────
    // A 4-pixel blue bar in the left margin on each visual row of a bookmarked
    // line (drawn last so it sits on top of the text background).
    if (line.bookmark) {
        dc.SetBrush(wxBrush(wxColour(60, 140, 255)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        int wc = WrapCount(lineIdx);
        for (int vr = 0; vr < wc; ++vr)
            dc.DrawRectangle(-m_pixelOffset, yOff + vr * m_lineH, 4, m_lineH);
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
    // 8 variants: bold × italic × underline.
    //
    // Link runs are NOT force-underlined here any more.  The engine already
    // decides: mxp/mxpOpenAtomic.cpp:180-181,773-774 set UNDERLINE only when the
    // world's "Underline hyperlinks" option is on, and doc_display_shared.cpp
    // carries that through as run.underline.  ORing isLink in overrode the
    // setting so it could never be turned off.  (MXP_ACTION_HYPERLINK ORs
    // UNDERLINE unconditionally at mxpOpenAtomic.cpp:213 — stock Windows
    // behaviour, left alone; the option governs send-links and image links.)
    bool drawUnderline = run.underline;
    if      (run.bold && run.italic && drawUnderline) dc.SetFont(m_fontBoldItalicUnderline);
    else if (run.bold && run.italic)                  dc.SetFont(m_fontBoldItalic);
    else if (run.italic && drawUnderline)             dc.SetFont(m_fontItalicUnderline);
    else if (run.italic)                              dc.SetFont(m_fontItalic);
    else if (run.bold && drawUnderline)               dc.SetFont(m_fontBoldUnderline);
    else if (run.bold)                                dc.SetFont(m_fontBold);
    else if (drawUnderline)                           dc.SetFont(m_fontUnderline);
    else                                              dc.SetFont(m_font);

    // Draw each character at its fixed grid cell (x + i*m_charW) rather than
    // letting one DrawText flow the whole string at the font's natural advances.
    // A monospace font's natural advance can differ from the integer cell width
    // m_charW on macOS, so a single DrawText drifts off the grid — which both
    // misaligns the grid-computed selection rectangle with the glyphs and shifts
    // the text when a run is split for a partial selection.  Per-cell drawing
    // keeps glyph positions identical whether or not the run is split, and lined
    // up with the selection rectangle and click hit-testing (both grid-based).
    for (size_t i = 0; i < text.size(); ++i)
        dc.DrawText(text.Mid(i, 1), x + (int) i * m_charW, yOff);

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
    // m_lineChars is maintained alongside m_lines, so this costs one division
    // rather than a re-scan of every run's bytes (which for a non-UTF-8 world
    // means decoding each run into a fresh wxString just to take its length).
    const int chars = (lineIdx < (int)m_lineChars.size()) ? m_lineChars[lineIdx] : 0;
    if (chars == 0 || m_wrapCols <= 0) return 1;
    return (chars + m_wrapCols - 1) / m_wrapCols;
}

// Resolve the active text rectangle (world.TextRectangle) into client pixels.
// Returns false (and the full client area) when no rectangle is set.
bool COutputPane::TextRect(int& l, int& t, int& r, int& b) const
{
    int clientW = 0, clientH = 0;
    const_cast<COutputPane*>(this)->GetClientSize(&clientW, &clientH);
    l = 0; t = 0; r = clientW; b = clientH;
    if (!m_fn_text_rect) return false;
    int rl = 0, rt = 0, rr = 0, rb = 0;
    if (!m_fn_text_rect(rl, rt, rr, rb)) return false;
    // Clamp to the client area; ignore degenerate rectangles.
    l = std::max(0, rl);
    t = std::max(0, rt);
    r = (rr > 0) ? std::min(clientW, rr) : clientW;
    b = (rb > 0) ? std::min(clientH, rb) : clientH;
    if (r <= l || b <= t) { l = 0; t = 0; r = clientW; b = clientH; return false; }
    return true;
}

int COutputPane::TextViewportH() const
{
    int l, t, r, b;
    if (TextRect(l, t, r, b)) return b - t;
    int h = 0;
    const_cast<COutputPane*>(this)->GetClientSize(nullptr, &h);
    return h;
}

void COutputPane::RebuildWrapCache()
{
    int clientW;
    GetClientSize(&clientW, nullptr);
    // Narrow the wrap width to the text rectangle when a plugin has reserved
    // part of the output area (e.g. a docked miniwindow on the left/right).
    int trL, trT, trR, trB;
    // ... and by the pixel offset, which insets the text from the left edge.
    // Windows does exactly this in its own wrap computation:
    //   mushview.cpp:3220  (GetOutputWindowWidth () - pDoc->m_iPixelOffset) / charW
    // where GetOutputWindowWidth() is already the text rectangle's width.
    // CWorldChildFrame::EffectiveTextCols computes the ENGINE's wrap column from
    // the same two terms and the two must agree: if this one came out narrower,
    // every full-width engine line would gain a one- or two-character orphan
    // continuation row.
    int wrapW = (TextRect(trL, trT, trR, trB) ? (trR - trL) : clientW) - m_pixelOffset;
    int newCols = (m_charW > 0 && wrapW > 0)
                  ? std::max(1, wrapW / m_charW)
                  : 80;
    m_wrapCols = newCols;

    // Renumber from zero: a full rebuild is the one place the absolute row axis
    // may be reset, since every consumer reads through RowStartOf().
    const int n = (int)m_lines.size();

    // Self-heal the char-count cache if it ever falls out of step with m_lines.
    // Every mutation site keeps them paired, so this should never fire — but a
    // silently wrong wrap count is a scrambled display, and a rebuild is the
    // natural place to re-establish the invariant rather than assert on it.
    if ((int)m_lineChars.size() != n) {
        m_lineChars.assign((size_t)n, 0);
        for (int i = 0; i < n; ++i) {
            int chars = 0;
            for (const auto& run : m_lines[i]) chars += RunCharLen(run);
            m_lineChars[(size_t)i] = chars;
        }
    }

    m_visualRowStart.assign((size_t)n, 0);
    m_rowBase         = 0;
    m_totalVisualRows = 0;
    for (int i = 0; i < n; ++i) {
        m_visualRowStart[(size_t)i] = m_totalVisualRows;
        m_totalVisualRows          += WrapCount(i);
    }
}

// Drop logical line 0 from both caches in O(1), leaving the remaining absolute
// row numbers untouched and moving the base instead.
void COutputPane::PopFrontWrapCache()
{
    if (m_visualRowStart.empty()) return;
    m_visualRowStart.pop_front();
    if (!m_lineChars.empty()) m_lineChars.pop_front();
    // The new first line's absolute row becomes the base, so RowStartOf(0) == 0
    // and every other entry keeps its meaning.
    const long long newBase = m_visualRowStart.empty()
                              ? m_rowBase + m_totalVisualRows
                              : m_visualRowStart.front();
    m_totalVisualRows -= (int)(newBase - m_rowBase);
    if (m_totalVisualRows < 0) m_totalVisualRows = 0;
    m_rowBase = newBase;
}

int COutputPane::VisualRowToLogical(int vr) const
{
    if (m_visualRowStart.empty()) return 0;
    vr = std::max(0, vr);
    // Binary search: largest i such that m_visualRowStart[i] <= vr.
    int lo = 0, hi = (int)m_visualRowStart.size() - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (m_visualRowStart[mid] - m_rowBase <= vr) lo = mid;
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
    // When a plugin reserved part of the area (world.TextRectangle), the text's
    // visible band is shorter than the client; pad the virtual height by the
    // reserved amount so "scroll to bottom" lands the newest line at the
    // rectangle's bottom (not the window's, where the clip would hide it).
    //
    // The pixel offset pads it further, which is what leaves a gap under the
    // last line to match the one to the left of the text -- Windows calls this
    // "text offset from bottom" (mushview.cpp:1747) and gets it by drawing the
    // text -m_iPixelOffset up the document.  Padding the virtual height instead
    // produces the same visible gap without Windows' side effect of clipping the
    // top of the first line when you scroll all the way up, and it leaves every
    // pixel-to-line mapping (YToLine, XToChar) untouched.
    // Not pixel-exact: SetScrollRate quantises scrolling to whole rows, so the
    // gap you actually see is the offset rounded UP to a multiple of m_lineH
    // (20 px with a 15 px row = 30 px).  Windows scrolls by pixels and gets
    // exactly 20.  Making it exact would mean scrolling this pane by pixels
    // rather than rows, which is a much larger change than this setting is
    // worth; the gap is a margin, and erring wide is the harmless direction.
    SetVirtualSize(clientW, totalH + (clientH - TextViewportH()) + m_pixelOffset);

    if (scrollToBottom && !m_lines.empty()) {
        int unitH;
        GetScrollPixelsPerUnit(nullptr, &unitH);
        if (unitH > 0) {
            // Scroll() clamps to (scrollRange - pageSize), which is the position
            // that shows the last line at the bottom.  Passing scrollRange directly
            // is equivalent to Scroll(-1, -1) but without the ambiguity.
            //
            // "Very smooth scrolling" walks there instead of jumping.  Windows
            // (mushview.cpp:4632-4653) animates only when the move is LESS THAN
            // one screenful and steps one pixel at a time, repainting each step;
            // a bigger jump is redrawn in one go.  Same rule here, in rows.
            // m_animatingScroll keeps output arriving mid-animation -- Update()
            // pumps paints, and a paint handler can append -- from starting a
            // second one on top of this.
            const int target = GetScrollRange(wxVERTICAL);
            if (m_smoothScroll && !m_animatingScroll) {
                int curY = 0;
                GetViewStart(nullptr, &curY);
                // Scroll() CLAMPS to (range - page), so the range is not where
                // we actually land -- the real move is a page shorter.  Measure
                // the destination, not the range, or the delta is overstated by
                // a whole screenful and never qualifies as "less than one".
                int pageRows = GetScrollThumb(wxVERTICAL);
                if (pageRows <= 0) {
                    GetClientSize(nullptr, &pageRows);
                    pageRows = (m_lineH > 0) ? pageRows / m_lineH : 0;
                }
                const int dest  = (target > pageRows) ? target - pageRows : 0;
                const int delta = dest - curY;
                if (delta > 0 && pageRows > 0 && delta < pageRows) {
                    m_animatingScroll = true;
                    for (int row = 1; row <= delta; ++row) {
                        Scroll(0, curY + row);
                        Update();          // paint this step before the next
                    }
                    m_animatingScroll = false;
                }
            }
            Scroll(0, target);   // land exactly on target either way
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
    CallAfter([this]() { RefreshAutoScrollFromPosition(); });
}

void COutputPane::OnSize(wxSizeEvent& evt)
{
    RebuildWrapCache();
    UpdateScrollbars(m_autoScroll);
    // Notify so the world frame refreshes the doc's GUI geometry (GetInfo
    // 280/281) — the pane resizes on layout changes that don't resize the frame
    // (e.g. the input bar taking its space at startup), which the frame's own
    // OnSize would miss.
    if (m_fn_resized) m_fn_resized();
    evt.Skip();
}

// ── Selection helpers ─────────────────────────────────────────────────────────

int COutputPane::YToLine(int clientY) const
{
    if (m_lines.empty()) return 0;

    // Honor the text rectangle's vertical offset (text starts at its top).
    { int l, t, r, b; if (TextRect(l, t, r, b)) { clientY -= t; } }

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

    // Honor the text rectangle: text is drawn from its top-left, so hit-testing
    // must subtract that offset before mapping pixels to characters.
    { int l, t, r, b; if (TextRect(l, t, r, b)) { clientX -= l; clientY -= t; } }
    // The pixel offset shifts the text right whether or not a rectangle is set,
    // so it comes off unconditionally -- inside the block above it would leave
    // click-to-char wrong by m_pixelOffset in the common no-rectangle case.
    clientX -= m_pixelOffset;

    // Count total characters on the line (Unicode code points, not UTF-8 bytes).
    int totalChars = 0;
    for (const auto& run : m_lines[lineIdx])
        totalChars += RunCharLen(run);

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
        int lineStartVR = RowStartOf(lineIdx);
        vrOffset = std::max(0, clickVR - lineStartVR);
    }

    // Column within the visual row, then offset by wrapped rows. Clamp the
    // per-row column to the wrap width so a click past the text on a non-final
    // visual row can't spill into a later row's character range.
    int colInRow = std::max(0, (virtualX + m_charW / 2) / m_charW);
    if (m_wrapCols > 0) colInRow = std::min(colInRow, m_wrapCols);
    int col      = vrOffset * m_wrapCols + colInRow;
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
                if (!run.horiz_rule) lineLen += RunCharLen(run);
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
                int runLen = RunCharLen(run);
                if (!run.action.empty() &&
                    clickCol >= c && clickCol < c + runLen)
                {
                    m_linkAtDown     = run.action;
                    m_linkTypeAtDown = run.action_type;
                    m_linkNoteAtDown = m_lines[lineIdx].comment;
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
                m_fn_link_clicked(m_linkAtDown, m_linkTypeAtDown,
                                  m_linkNoteAtDown);
        }
        m_linkAtDown.clear();

        // Auto-copy non-empty selection to clipboard when
        // copy_selection_to_clipboard is enabled (off by default).
        // On GTK we always write the X11 primary selection regardless, because
        // middle-click paste is a universal Unix convention users rely on.
        {
            SelPoint s, e;
            if (GetNormalizedSel(s, e)) {
                wxString text = SelectedText();
                if (!text.IsEmpty()) {
                    if (m_copySelToClip) {
                        // "…in HTML" (m_bAutoCopyInHTML) was previously ignored
                        // here: the auto-copy always put plain text on the
                        // clipboard, so ticking the HTML box did nothing.
                        if (m_autoCopyInHTML) {
                            CopySelectionAsHTML(GetFont().GetFaceName());
                        } else if (wxTheClipboard->Open()) {
                            wxTheClipboard->SetData(new wxTextDataObject(text));
                            wxTheClipboard->Close();
                        }
                    }
#ifdef __WXGTK__
                    // X11 primary selection: always set so middle-click works.
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
                    lineText += DecodeRunText(run);

            int col = XToChar(lineIdx, evt.GetX(), evt.GetY());
            col = std::max(0, std::min((int)lineText.size(), col));

            // Expand left to word start.
            // A character is a word character if it is not in m_wordDelimitersDblClick
            // and not whitespace — matching the Windows MUSHclient convention.
            auto isWordChar = [this](wxUniChar ch) -> bool {
                if (wxIsspace(ch)) return false;
                // Convert to a single-byte char for the delimiter lookup; for
                // non-ASCII code points (which can't be delimiters) treat as word.
                if (ch > 127) return true;
                char c = static_cast<char>(ch.GetValue());
                return m_wordDelimitersDblClick.find(c) == std::string::npos;
            };
            int wordStart = col;
            while (wordStart > 0 && isWordChar(lineText[wordStart - 1]))
                --wordStart;

            // Expand right to word end.
            int wordEnd = col;
            while (wordEnd < (int)lineText.size() && isWordChar(lineText[wordEnd]))
                ++wordEnd;

            if (wordEnd > wordStart) {
                m_selAnchor = SelPoint{lineIdx, wordStart};
                m_selFocus  = SelPoint{lineIdx, wordEnd};
                Refresh(false);

                // "Double-click Sends Word" / "…Pastes Word".  Dispatch the word
                // computed RIGHT HERE, not SelectedText(): the selection is only
                // written inside this guard, so a double-click that lands on a
                // space or a delimiter leaves the PREVIOUS selection in place --
                // possibly a whole dragged paragraph -- and sending that would be
                // badly wrong.  No word, no dispatch, which is also what Windows
                // does (it computes an empty word and tests IsEmpty first).
                if (m_fn_word_double_clicked) {
                    // The handler can open a modal (the not-connected prompt).
                    // On macOS a nested modal loop swallows the following
                    // mouse-up, which would strand the pane mid-drag with the
                    // mouse still captured, so let go before dispatching.
                    m_dragging = false;
                    if (HasCapture()) ReleaseMouse();
                    m_fn_word_double_clicked(
                        lineText.Mid(wordStart, wordEnd - wordStart),
                        evt.ShiftDown());
                }
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

// CHotspot::m_Cursor (-1..12) -> wx stock cursor, mirroring the exact
// Windows mapping in CMUSHView::OnSetCursor (docs/PAL.md S4: the cursor
// parity fix — the port previously ignored hotspot cursors entirely).
// 5 (up arrow) has no wx stock equivalent -> plain arrow (best effort).
static wxCursor MiniWindowCursor(int id)
{
    switch (id) {
    default:
    case -1: return wxCursor(wxCURSOR_BLANK);           // no cursor
    case  0: return wxCursor(wxCURSOR_ARROW);           // arrow
    case  1: return wxCursor(wxCURSOR_HAND);            // hand
    case  2: return wxCursor(wxCURSOR_IBEAM);           // I-beam
    case  3: return wxCursor(wxCURSOR_CROSS);           // + (cross)
    case  4: return wxCursor(wxCURSOR_WAIT);            // wait
    case  5: return wxCursor(wxCURSOR_ARROW);           // up arrow (approx.)
    case  6: return wxCursor(wxCURSOR_SIZENWSE);        // arrow nw-se
    case  7: return wxCursor(wxCURSOR_SIZENESW);        // arrow ne-sw
    case  8: return wxCursor(wxCURSOR_SIZEWE);          // arrow e-w
    case  9: return wxCursor(wxCURSOR_SIZENS);          // arrow n-s
    case 10: return wxCursor(wxCURSOR_SIZING);          // all ways
    case 11: return wxCursor(wxCURSOR_NO_ENTRY);        // (X) no
    case 12: return wxCursor(wxCURSOR_QUESTION_ARROW);  // help
    }
}

void COutputPane::OnMotion(wxMouseEvent& evt)
{
    int mwCursor = -999;
    if (m_fn_miniwindow_mouse_move)
        mwCursor = m_fn_miniwindow_mouse_move(evt.GetX(), evt.GetY());

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
        //   2. Over a miniwindow → its hotspot cursor (-1..12), or arrow.
        //   3. Over an MXP / URL link run → pointing hand.
        //   4. Anywhere else → I-beam text cursor.
        int clientH;
        GetClientSize(nullptr, &clientH);
        if (!m_autoScroll && !m_lines.empty() &&
            evt.GetY() >= clientH - kHintBarH)
        {
            SetCursor(wxCursor(wxCURSOR_HAND));
        } else if (mwCursor != -999) {
            SetCursor(MiniWindowCursor(mwCursor));
        } else {
            SelPoint pt = MouseToSelPoint(evt);
            bool overLink = false;
            if (pt.valid() && pt.line < (int)m_lines.size()) {
                // pt.col is the absolute char column in the logical line
                // (already wrap-aware via XToChar in MouseToSelPoint).
                int c = 0;
                for (const auto& run : m_lines[pt.line]) {
                    int runLen = RunCharLen(run);
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
    int  linkTypeAtClick = 0;
    bool linkNoteAtClick = false;
    {
        int lineIdx = YToLine(evt.GetY());
        if (lineIdx >= 0 && lineIdx < (int)m_lines.size()) {
            int col = XToChar(lineIdx, evt.GetX(), evt.GetY());
            int c = 0;
            for (const auto& run : m_lines[lineIdx]) {
                int runLen = RunCharLen(run);
                if (!run.action.empty() &&
                    col >= c && col < c + runLen)
                {
                    linkAtClick     = run.action;
                    linkTypeAtClick = run.action_type;
                    linkNoteAtClick = m_lines[lineIdx].comment;
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

    // "Copy as HTML": same selection, but as colour-preserving HTML markup
    // (canonical ID_EDIT_COPYASHTML).  Only meaningful with a selection.
    {
        wxMenuItem* itemHtml = menu.Append(wxID_ANY, wxT("Copy as HTML"));
        menu.Enable(itemHtml->GetId(), hasSel);
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            CopySelectionAsHTML(GetFont().GetFaceName());
        }, itemHtml->GetId());
    }

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
        wxMenuItem* fi = menu.Append(wxID_ANY, wxT("Find…\tCtrl+F"));
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
                [this, linkAtClick, linkTypeAtClick, linkNoteAtClick](wxCommandEvent&) {
                    if (m_fn_link_clicked)
                        m_fn_link_clicked(linkAtClick, linkTypeAtClick, linkNoteAtClick);
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
                    [this, cmd = ch, linkTypeAtClick, linkNoteAtClick](wxCommandEvent&) {
                        if (m_fn_link_clicked)
                            m_fn_link_clicked(cmd, linkTypeAtClick, linkNoteAtClick);
                    }, item->GetId());
            }
            if (choices.size() > 1) {
                // Also offer "Send first" as a quick one-click action.
                wxMenuItem* itemAll = menu.Append(wxID_ANY,
                    wxT("Send All (in order)"));
                menu.Bind(wxEVT_MENU,
                    [this, linkAtClick, linkTypeAtClick, linkNoteAtClick](wxCommandEvent&) {
                        if (m_fn_link_clicked)
                            m_fn_link_clicked(linkAtClick, linkTypeAtClick, linkNoteAtClick);
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
    CallAfter([this]() { RefreshAutoScrollFromPosition(); });
}

void COutputPane::OnKeyDown(wxKeyEvent& evt)
{
    int code = evt.GetKeyCode();

    // Lua Accelerator() API — check before built-in key bindings so scripts
    // can override any key.
    if (m_fn_accelerator_check && m_fn_accelerator_check(evt))
        return;

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

    if (isScrollKey)
        CallAfter([this]() { RefreshAutoScrollFromPosition(); });
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

void COutputPane::CopySelectionAsHTML(const wxString& fontName)
{
    SelPoint s, e;
    if (!GetNormalizedSel(s, e)) return;

    int lo = std::max(0, s.line);
    int hi = std::min((int)m_lines.size() - 1, e.line);
    if (lo > hi) return;

    // HTML-escape one byte string (matches Utilities.cpp FixHTMLString:
    // < > & " → entities; UTF-8 bytes pass through untouched).
    auto fixHTML = [](const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (char ch : in)
            switch (ch) {
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '&': out += "&amp;";  break;
            case '"': out += "&quot;"; break;
            default:  out += ch;       break;
            }
        return out;
    };
    // Slice `count` rendered characters from a run starting at character index
    // `startChar`, returned as UTF-8 bytes.  DecodeRunText handles both UTF-8 and
    // legacy-charset worlds and slices in the same RunCharLen space the selection
    // columns use, so this is correct for every world (the HTML is emitted as
    // UTF-8 — see the charset <meta> below).
    auto runSlice = [](const SOutputRun& run, int startChar, int count) -> std::string {
        if (count <= 0) return std::string();
        wxString s = DecodeRunText(run).Mid(startChar, count);
        return std::string(s.utf8_str());
    };

    // Default background = first run's bg on the start line (canonical uses the
    // start line's first style); fall back to the pane background.
    uint32_t defaultBack = m_bgColor;
    if (!m_lines[lo].empty()) defaultBack = m_lines[lo][0].bg;

    char buf[256];
    std::string html;
    html += "<!-- Produced by MUSHclient - www.mushclient.com -->\n";
    // The slices below are emitted as UTF-8 regardless of the world's charset, so
    // declare it (legacy-charset worlds decode to UTF-8 rather than copying raw
    // 8-bit bytes that a clipboard consumer would mis-render).
    html += "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\n";
    std::snprintf(buf, sizeof buf,
                  "<table border=0 cellpadding=5 bgcolor=\"#%02X%02X%02X\">\n",
                  (int)GetRValue(defaultBack), (int)GetGValue(defaultBack),
                  (int)GetBValue(defaultBack));
    html += buf;
    html += "<tr><td>\n";

    std::string face;
    if (!fontName.IsEmpty() && fontName != wxT("FixedSys"))
        face = std::string(fontName.utf8_str()) + ", ";
    html += "<pre><code><font size=2 face=\"" + face +
            "FixedSys, Lucida Console, Courier New, Courier\"><font color=\"#0\">\n";

    uint32_t lastFore = 0, lastBack = 0;
    bool bInSpan = false, firstLine = true;

    for (int li = lo; li <= hi; ++li) {
        const SOutputLine& line = m_lines[li];

        int lineLen = 0;
        for (const auto& run : line)
            if (!run.horiz_rule) lineLen += RunCharLen(run);

        int startCol = (li == lo) ? std::max(0, (int)s.col) : 0;
        int endCol   = (li == hi) ? std::min(lineLen, (int)e.col) : lineLen;

        if (!firstLine) html += "\n";   // newline between selected lines
        firstLine = false;

        if (!line.empty() && line[0].horiz_rule) {
            html += "&mdash;&mdash;&mdash;&mdash;&mdash;&mdash;&mdash;&mdash;";
            continue;
        }
        if (endCol <= startCol) continue;   // nothing selected on this line

        int col = 0;   // running char position within the line
        for (const auto& run : line) {
            if (run.horiz_rule) continue;
            int runLen = RunCharLen(run);
            if (runLen <= 0) continue;

            int runStart = col;
            int runEnd   = col + runLen;
            col = runEnd;

            int from = std::max(runStart, startCol);
            int to   = std::min(runEnd,   endCol);
            if (to <= from) {
                if (runStart >= endCol) break;   // past the selection on this line
                continue;
            }

            uint32_t colour1 = run.fg, colour2 = run.bg;
            if (colour1 != lastFore || colour2 != lastBack) {
                if (bInSpan) { html += "</span>"; bInSpan = false; }
                std::snprintf(buf, sizeof buf, "</font><font color=\"#%02X%02X%02X\">",
                              (int)GetRValue(colour1), (int)GetGValue(colour1),
                              (int)GetBValue(colour1));
                html += buf;
                // Only open a <span> when the background differs from the default
                // (canonical optimisation — keeps the markup small).
                if (colour2 != defaultBack) {
                    std::snprintf(buf, sizeof buf,
                        "<span style=\"color: #%02X%02X%02X; background: #%02X%02X%02X\">",
                        (int)GetRValue(colour1), (int)GetGValue(colour1), (int)GetBValue(colour1),
                        (int)GetRValue(colour2), (int)GetGValue(colour2), (int)GetBValue(colour2));
                    html += buf;
                    bInSpan = true;
                }
                lastFore = colour1;
                lastBack = colour2;
            }

            if (run.underline) html += "<u>";
            html += fixHTML(runSlice(run, from - runStart, to - from));
            if (run.underline) html += "</u>";

            if (col >= endCol) break;
        }
    }

    if (bInSpan) html += "</span>";
    html += "</font></font></code></pre>\n</td></tr></table>\n";

    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(
            new wxTextDataObject(wxString::FromUTF8(html.c_str(), html.size())));
        wxTheClipboard->Close();
    }
}

void COutputPane::SelectAll()
{
    if (m_lines.empty()) return;
    int lastLine = (int)m_lines.size() - 1;
    int lastCol  = 0;
    for (const auto& run : m_lines[lastLine])
        lastCol += RunCharLen(run);

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
                lineText += DecodeRunText(run);

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
                s += DecodeRunText(run);
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
                     ? RowStartOf(foundIdx) : foundIdx;
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
