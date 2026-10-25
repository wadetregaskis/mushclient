// platform/wx_tree_property_sheet.cpp
//
// Portable CTreePropertySheet — the World Properties container (24 pages).
// See dialogs/world_prefs/TreePropertySheet.h (#ifndef _WIN32 branch) for the
// class.  The canonical CPropertyPage subclasses are reused verbatim; this
// lightweight sheet only records the pages + their tree structure as the
// canonical GamePreferences() adds them, then DoModal() hands the recorded list
// to the wx layer (g_run_mfc_property_sheet), which renders a native wxTreebook.
//
// Deliberately free of GDI / CMemDC / CTreeCtrl: constructing those (as the real
// Windows CTreePropertySheet does) would dereference the null port
// CWnd::GetDesktopWindow().  The heavy TreePropertySheet.cpp is excluded from
// the port build (CMakeLists.txt).

#ifndef _WIN32

#include "stdafx.h"      // MFC-shim umbrella: CWnd, CPropertySheet, UINT, CString…
#include "../dialogs/world_prefs/TreePropertySheet.h"
#include "wx_mfc_hooks.h"

CTreePropertySheet::CTreePropertySheet(CWnd* pParentWnd, UINT iSelectPage)
    : CPropertySheet((LPCTSTR)nullptr, pParentWnd, iSelectPage)
{
    m_iWantedPage = (int) iSelectPage;
}

CTreePropertySheet::CTreePropertySheet(int /*nTreeWidth*/, UINT nIDCaption,
                                       CWnd* pParentWnd, UINT iSelectPage)
    : CPropertySheet((UINT)nIDCaption, pParentWnd, iSelectPage)
{
    m_iWantedPage = (int) iSelectPage;
}

CTreePropertySheet::CTreePropertySheet(int /*nTreeWidth*/, LPCTSTR pszCaption,
                                       CWnd* pParentWnd, UINT iSelectPage)
    : CPropertySheet(pszCaption, pParentWnd, iSelectPage)
{
    m_iWantedPage = (int) iSelectPage;
    if (pszCaption)
        m_caption = pszCaption;
}

void CTreePropertySheet::AddPage(int nType, CPropertyPage* pPage,
                                 LPCTSTR lpszCaptionDescr)
{
    PortPage p;
    p.nType   = nType;
    p.pPage   = pPage;
    p.caption = lpszCaptionDescr ? lpszCaptionDescr : _T("");
    m_portPages.push_back(p);
    // Also record it in the base list, so the base page accessors
    // (GetPage/GetPageCount/GetPageIndex and, above all, GetActivePage — which
    // every sheet's OnKickIdle dereferences unconditionally) answer for World
    // Properties too.  Nothing iterates m_flatPages for a tree sheet: DoModal is
    // overridden above, so the flat-sheet bridge is never reached from here.
    CPropertySheet::AddPage(pPage);
}

INT_PTR CTreePropertySheet::DoModal()
{
    if (g_run_mfc_property_sheet)
        return (INT_PTR) g_run_mfc_property_sheet(this);
    return IDCANCEL;
}

// Base CPropertySheet::DoModal — plain tabbed sheets (e.g. CGlobalPrefsSheet,
// the File ▸ Preferences dialog).  The pages were recorded by the base AddPage()
// into m_flatPages; the flat-sheet hook renders them as a wxNotebook running each
// canonical CPropertyPage's OnInitDialog/DDX/DDV.  CTreePropertySheet overrides
// this (above) for its tree layout, so only non-tree sheets reach here.
INT_PTR CPropertySheet::DoModal()
{
    if (g_run_mfc_property_sheet_flat)
        return (INT_PTR) g_run_mfc_property_sheet_flat(this);
    return IDCANCEL;
}

#endif // !_WIN32
