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

void PrintOutputPane(wxWindow* parent, const wxString& title, COutputPane* pane)
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

    wxHtmlEasyPrinting printer(title.IsEmpty() ? wxT("MUSHclient") : title, parent);
    printer.SetStandardFonts(9);
    printer.PreviewText(html);
}
