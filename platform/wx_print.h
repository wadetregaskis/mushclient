// wx_print.h — print the world output buffer (File ▸ Print World).
//
// The canonical print path is MFC CView::OnFilePrint / PrintWorld, which the
// port doesn't run.  This renders the wx COutputPane's scrollback as an HTML
// document and prints it via wxHtmlEasyPrinting (which handles pagination,
// scaling, the print dialog and a preview window for us).
#pragma once

#include <wx/string.h>

class wxWindow;
class COutputPane;

// Show a print-preview of the output pane's scrollback (with a Print button).
//
// The font face/size and the margins come from Global Preferences ▸ Printing
// (App.m_strPrinterFont / m_nPrinterFontSize / m_nPrinterLeftMargin /
// m_nPrinterTopMargin) — the caller reads them, because this TU deliberately
// includes no MFC-compat headers.  Margins are millimetres, as on the dialog.
// NOT honoured here: "lines per page" (m_nPrinterLinesPerPage).  wxHTML
// paginates by paper height and has no lines-per-page concept; that needs a
// real wxPrintout subclass, which is tracked separately.
void PrintOutputPane(wxWindow* parent, const wxString& title, COutputPane* pane,
                     const wxString& fontFace, int fontPoints,
                     int leftMarginMm, int topMarginMm);
