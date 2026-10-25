// wx_print.cpp — see wx_print.h.
//
// wx-only translation unit (NO MFC-compat headers), so the Windows-style macros
// that stdafx.h/compat.h define can't clobber wx's own headers.

#include <wx/wx.h>
#include <wx/print.h>
#include <wx/printdlg.h>

#include "platform/wx_print.h"

#include <algorithm>

namespace {

// ── CWorldPrintout ───────────────────────────────────────────────────────────
//
// A transcription of genprint.cpp's page model, which is what makes Global
// Preferences ▸ Printing ▸ "lines per page" mean anything.  The port used to
// render the scrollback as one HTML <pre> and hand it to wxHtmlEasyPrinting,
// which paginates by paper height and has no lines-per-page concept at all — so
// that setting was inert, and a large font or wide margin could make wxHTML
// decide the <pre> did not fit horizontally and render the whole block as
// nothing.  Both problems are the same missing piece: a real wxPrintout.
//
// The page layout follows mushview.cpp's PrintWorld accounting exactly:
//
//     row 0                    header, bold + underlined: "<world> - <time>"
//     row 1                    blank
//     rows 2 .. linesPerPage-2 body
//     blank
//     final row                footer, bold: "Page N"
//
// where the header's time is the arrival time of the first line ON THAT PAGE.
class CWorldPrintout : public wxPrintout
{
public:
    CWorldPrintout(const wxString& title,
                   const std::vector<SPrintLine>& lines,
                   const wxString& fontFace, int fontPoints,
                   int leftMarginMm, int topMarginMm, int linesPerPage)
        : wxPrintout(title),
          m_title(title), m_lines(lines), m_face(fontFace),
          m_points(fontPoints > 0 ? fontPoints : 10),
          m_leftMm(std::max(0, leftMarginMm)),
          m_topMm(std::max(0, topMarginMm))
    {
        // Body rows per page.  Windows seeds current_line = 2 after the header
        // and breaks when it exceeds linesPerPage - 2, so rows 2..linesPerPage-2
        // carry text — linesPerPage - 3 of them.  Guard the degenerate settings
        // (the prefs dialog allows small numbers) rather than dividing by zero.
        m_bodyRows = linesPerPage - 3;
        if (m_bodyRows < 1)
            m_bodyRows = 1;

        int n = (int) m_lines.size();
        m_pages = (n + m_bodyRows - 1) / m_bodyRows;
        if (m_pages < 1)
            m_pages = 1;
    }

    bool HasPage(int page) override { return page >= 1 && page <= m_pages; }

    void GetPageInfo(int* minPage, int* maxPage,
                     int* selFrom, int* selTo) override
    {
        *minPage = 1;  *maxPage = m_pages;
        *selFrom = 1;  *selTo   = m_pages;
    }

    bool OnPrintPage(int page) override;

private:
    wxFont MakeFont(bool bold, bool italic, bool underline, int pixelH) const
    {
        wxFont f(wxSize(0, pixelH), wxFONTFAMILY_TELETYPE,
                 italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL,
                 bold   ? wxFONTWEIGHT_BOLD  : wxFONTWEIGHT_NORMAL,
                 underline, m_face);
        return f;
    }

    wxString                      m_title;
    const std::vector<SPrintLine>& m_lines;
    wxString                      m_face;
    int                           m_points;
    int                           m_leftMm;
    int                           m_topMm;
    int                           m_bodyRows = 1;
    int                           m_pages    = 1;
};

bool CWorldPrintout::OnPrintPage(int page)
{
    wxDC* dc = GetDC();
    if (!dc)
        return false;

    int ppiX = 0, ppiY = 0;
    GetPPIPrinter(&ppiX, &ppiY);
    int pageW = 0, pageH = 0;
    GetPageSizePixels(&pageW, &pageH);
    if (ppiX <= 0 || ppiY <= 0 || pageW <= 0 || pageH <= 0)
        return false;

    // Draw in PRINTER pixels throughout, then scale the DC so the identical
    // code renders into the (much smaller) preview DC as well.  This is the
    // standard wx printing recipe and keeps one coordinate system.
    int dcW = 0, dcH = 0;
    dc->GetSize(&dcW, &dcH);
    const double scale = (double) dcW / (double) pageW;
    dc->SetUserScale(scale, scale);

    // Line pitch from the point size, exactly as genprint.cpp does
    // (pcb.point_size / 72.0 * logpelsY).  Deliberately NOT the font's own
    // height: "lines per page" has to mean the number the user typed, and
    // Windows lets a silly size/spacing combination run off the paper rather
    // than quietly re-paginating.
    const int lineH = std::max(1, (int) (m_points / 72.0 * ppiY));

    // Margins are millimetres on the dialog and here.  genprint measures them
    // from the PAPER edge and then subtracts the printer's non-printable
    // margin, clamping at zero; GetPaperRectPixels() gives that offset as a
    // negative origin, so this is the same arithmetic.
    const wxRect paper = GetPaperRectPixels();
    int left = paper.x + (int) (m_leftMm / 25.4 * ppiX);
    int top  = paper.y + (int) (m_topMm  / 25.4 * ppiY);
    if (left < 0) left = 0;
    if (top  < 0) top  = 0;

    const int fontPixels = std::max(1, (int) (m_points / 72.0 * ppiY));
    const int first = (page - 1) * m_bodyRows;
    const int last  = std::min((int) m_lines.size(), first + m_bodyRows);

    dc->SetTextForeground(*wxBLACK);
    dc->SetBackgroundMode(wxTRANSPARENT);

    // ── Header: bold + underlined "<world> - <time of the page's first line>"
    {
        dc->SetFont(MakeFont(true, false, true, fontPixels));
        wxString stamp;
        if (first < (int) m_lines.size())
            stamp = m_lines[first].stamp;
        wxString header = stamp.IsEmpty() ? m_title
                                          : (m_title + wxT(" - ") + stamp);
        dc->DrawText(header, left, top);
    }

    // ── Body, one world line per row, each run in its own style ─────────────
    int y = top + lineH * 2;      // header row + the blank row after it
    for (int i = first; i < last; ++i, y += lineH)
    {
        int x = left;
        for (const SPrintRun& run : m_lines[i].runs)
        {
            if (run.text.IsEmpty())
                continue;
            dc->SetFont(MakeFont(run.bold, run.italic, run.underline, fontPixels));
            dc->DrawText(run.text, x, y);
            // Advance by the measured width, as genprint's print_printline does
            // for a same-line piece (skip == 0 → GetTextExtentPoint32 + add).
            wxCoord tw = 0, th = 0;
            dc->GetTextExtent(run.text, &tw, &th);
            x += tw;
        }
    }

    // ── Footer: bold "Page N", after a blank row ────────────────────────────
    {
        dc->SetFont(MakeFont(true, false, false, fontPixels));
        const int footerY = top + lineH * (m_bodyRows + 3);
        dc->DrawText(wxString::Format(wxT("Page %d"), page), left, footerY);
    }

    return true;
}

} // namespace

void PrintStyledDocument(wxWindow* parent, const wxString& title,
                         const std::vector<SPrintLine>& lines,
                         const wxString& fontFace, int fontPoints,
                         int leftMarginMm, int topMarginMm, int linesPerPage)
{
    const wxString name = title.IsEmpty() ? wxT("MUSHclient") : title;

    // wxPrintPreview needs two printouts: one it renders with, one it hands to
    // the printer if the user presses Print.  Both read `lines`, which the
    // caller owns for the duration of this call only — so the preview must not
    // outlive it... except a preview frame is non-modal.  Copy the data into
    // the heap and let both printouts share it, released when the frame dies.
    auto* owned = new std::vector<SPrintLine>(lines);

    struct PreviewFrame : public wxPreviewFrame
    {
        PreviewFrame(wxPrintPreview* p, wxWindow* par, const wxString& t,
                     std::vector<SPrintLine>* data)
            : wxPreviewFrame(p, par, t), m_data(data) {}
        ~PreviewFrame() override { delete m_data; }
        std::vector<SPrintLine>* m_data;
    };

    auto* preview = new wxPrintPreview(
        new CWorldPrintout(name, *owned, fontFace, fontPoints,
                           leftMarginMm, topMarginMm, linesPerPage),
        new CWorldPrintout(name, *owned, fontFace, fontPoints,
                           leftMarginMm, topMarginMm, linesPerPage));

    if (!preview->IsOk())
    {
        delete preview;
        delete owned;
        wxMessageBox(wxT("Could not start the print preview — check that a "
                         "printer is configured."),
                     wxT("Print World"), wxOK | wxICON_ERROR, parent);
        return;
    }

    auto* frame = new PreviewFrame(preview, parent, name + wxT(" Preview"), owned);
    frame->Centre();
    frame->Initialize();
    frame->Show();
}
