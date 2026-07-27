// wx_print.h — print the world output buffer (File ▸ Print World).
//
// The canonical print path is MFC CView::OnFilePrint / PrintWorld, which the
// port doesn't run.  This renders the scrollback as an HTML document and prints
// it via wxHtmlEasyPrinting (which handles pagination, scaling, the print dialog
// and a preview window for us).
//
// Why not re-route to the canonical path instead: it lands in genprint.cpp, and
// every Win32 print primitive the shim provides is a degenerate stub —
// CreateFontIndirect returns nullptr (mfc_ui.h), there is no printer HDC, and
// StartDoc/StartPage/TextOut do nothing.  genprint would fail before emitting a
// glyph.  Making it work would mean implementing a real printer DC, font
// creation, the page/document calls and the text metrics behind new hooks —
// strictly more work than this, and a duplicate of what wx's printing
// architecture already provides.
#pragma once

#include <wx/string.h>
#include <vector>

class wxWindow;

// One styled span of a printed line.  The caller resolves the world's print
// styles (CMUSHclientDoc::m_nNormalPrintStyle / m_nBoldPrintStyle) into these
// three flags and decodes the bytes, because this TU deliberately includes no
// MFC-compat headers — the Windows-style macros stdafx.h/compat.h define would
// clobber wxHTML's own.
struct SPrintRun
{
    wxString text;
    bool     bold      = false;
    bool     italic    = false;
    bool     underline = false;
};

typedef std::vector<SPrintRun> SPrintLine;

// Show a print-preview of the world's scrollback (with a Print button).
//
// The font face/size and the margins come from Global Preferences ▸ Printing
// (App.m_strPrinterFont / m_nPrinterFontSize / m_nPrinterLeftMargin /
// m_nPrinterTopMargin) — the caller reads them, for the same reason.  Margins
// are millimetres, as on the dialog.
//
// NOT honoured here: "lines per page" (m_nPrinterLinesPerPage).  wxHTML
// paginates by paper height and has no lines-per-page concept; that needs a real
// wxPrintout subclass, which is tracked separately.
void PrintStyledDocument(wxWindow* parent, const wxString& title,
                         const std::vector<SPrintLine>& lines,
                         const wxString& fontFace, int fontPoints,
                         int leftMarginMm, int topMarginMm);
