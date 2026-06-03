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
void PrintOutputPane(wxWindow* parent, const wxString& title, COutputPane* pane);
