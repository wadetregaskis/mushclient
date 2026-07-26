// wx_print.cpp — see wx_print.h.
//
// wx-only translation unit (NO MFC-compat headers), so the Windows-style macros
// that stdafx.h/compat.h define can't clobber wxHTML's headers.

#include <wx/wx.h>
#include <wx/html/htmprint.h>

#include "platform/wx_print.h"
#include "platform/wx_outputpane.h"   // COutputPane, SOutputLine, SOutputRun

namespace {

// Convert a UTF-8 run string to HTML-safe text (whole-string, so multibyte
// UTF-8 survives).  '&' must be replaced first.
wxString EscapeHtml(const std::string& utf8)
{
    wxString s = wxString::FromUTF8(utf8.c_str());
    s.Replace("&", "&amp;");
    s.Replace("<", "&lt;");
    s.Replace(">", "&gt;");
    return s;
}

} // namespace

void PrintOutputPane(wxWindow* parent, const wxString& title, COutputPane* pane,
                     const wxString& fontFace, int fontPoints,
                     int leftMarginMm, int topMarginMm)
{
    if (!pane)
        return;

    // Build an HTML <pre> document from the scrollback.  <pre> keeps the MUD's
    // column alignment and renders monospaced; plain dark text prints legibly
    // on white (the on-screen grey-on-black palette would not).
    wxString html;
    html.reserve(64 * 1024);
    html << wxT("<html><body><pre>");

    const int n = pane->LineCount();
    for (int i = 0; i < n; ++i)
    {
        const SOutputLine* line = pane->GetLine(i);
        if (!line)
            continue;
        for (const SOutputRun& run : *line)
            html << EscapeHtml(run.text);
        html << wxT("\n");
    }

    html << wxT("</pre></body></html>");

    // PreviewText() shows a NON-modal preview frame and returns immediately; that
    // frame's wxPrintPreview holds a pointer to this object's print data.  A stack
    // wxHtmlEasyPrinting would be destroyed here, leaving the live preview with a
    // dangling pointer → use-after-free on scroll / zoom / Print.  Heap-allocate
    // and intentionally leak it: one small object per Print invocation, far simpler
    // and safer than threading the (internally-created) preview frame's lifetime.
    auto* printer = new wxHtmlEasyPrinting(title.IsEmpty() ? wxT("MUSHclient") : title, parent);

    // Global Preferences ▸ Printing.  These were hardcoded (9pt, default faces,
    // default margins), so the printer font and margin settings did nothing.
    // SetStandardFonts also rebuilds the relative size table, so it is the right
    // entry point for the size as well as the face; pass the face for both the
    // proportional and the fixed slot, since the document is one big <pre>.
    // A proportional face will cost the MUD's column alignment — Windows does
    // the same (genprint.cpp uses the chosen face verbatim), so that is parity.
    printer->SetStandardFonts(fontPoints > 0 ? fontPoints : 10,
                              fontFace, fontFace);

    // Known wxHTML limitation, verified in the preview: <pre> never wraps, so a
    // large font or a wide margin can make the widest line exceed the content
    // width — and wxHTML then renders the whole block as NOTHING, with a
    // "doesn't fit on the page horizontally" banner across the preview.  At the
    // shipped defaults (10pt Courier, 15mm) it fits; ~14pt or ~40mm is where it
    // tips over.  Windows truncates the over-long lines instead, and matching
    // that needs the real wxPrintout that "lines per page" also wants — the two
    // are one job, tracked together.  The preview always opens before printing,
    // so the user does see the banner rather than discovering it on paper.

    // Margins are millimetres on both sides of this boundary — the dialog
    // labels them "mm." and wxPageSetupDialogData is mm too — so no conversion
    // (Windows converts mm→device inside the printer DC; doing it here as well
    // would double-convert).  wxPoint is (x = LEFT, y = TOP); swapping them is
    // invisible at the defaults, which are 15 for both.
    if (wxPageSetupDialogData* ps = printer->GetPageSetupData())
        ps->SetMarginTopLeft(wxPoint(leftMarginMm, topMarginMm));

    printer->PreviewText(html);
}
