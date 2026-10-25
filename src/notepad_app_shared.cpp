// notepad_app_shared.cpp
//
// The APP-level notepad helpers — notepads with no owning world, as opposed to
// the world-scoped family in scripting/methods/methods_notepad.cpp.  Compiled by
// every build (Windows via MUSHclient.vcxproj, the macOS/Linux product, and the
// headless test engine), so there is exactly one definition.
//
// Moved verbatim out of MUSHclient.cpp, which is the app main and therefore
// mushgui_adapters-only.  The headless build previously supplied a twin in
// platform/headless_app.cpp that returned false/FALSE unconditionally, and that
// twin was reachable: utils.appendtonotepad and utils.activatenotepad
// (scripting/lua_utils_portable.cpp) call these, so the whole utils notepad
// surface was inert headless AND untestable.
//
// The three belong together and are hoisted together because FindNotepad is the
// lookup both others open with, and its match rule is the interesting part:
// m_pRelatedWorld == NULL.  That is what distinguishes an APP notepad from a
// world's — CMUSHclientDoc::FindNotepad matches on world AND docnum instead —
// and splitting them would put that rule two files away from its callers.
//
// Nothing here needs the GUI: the non-Windows arms go through CTextDocument's
// std::function m_fn_* members, exactly as the world-scoped family does.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "TextDocument.h"

#ifdef _WIN32
#include "TextView.h"
#endif

bool CMUSHclientApp::AppendToTheNotepad (const CString strTitle,
                                      const CString strText,
                                      const bool bReplace,
                                      const int  iNotepadType)
  {
CTextDocument * pTextDoc = FindNotepad (strTitle);

  if (pTextDoc)
    {
#ifndef _WIN32
    // Non-Windows: CTextView is not registered in the MFC view system.
    // Use the wx callbacks wired by CWxNotepadFrame instead.
    if (pTextDoc->m_fn_replace_sel && pTextDoc->m_fn_set_sel)
      {
      int iLength = pTextDoc->m_fn_get_text_length
                      ? pTextDoc->m_fn_get_text_length () : 0;
      if (bReplace)
        pTextDoc->m_fn_set_sel (0, -1);
      else
        pTextDoc->m_fn_set_sel (iLength, iLength);
      pTextDoc->m_fn_replace_sel (std::string ((LPCTSTR) strText));
      return true;
      }
    return false;
#else
    // append to the view
    POSITION pos=pTextDoc->GetFirstViewPosition();

    if (pos)
      {
      CView* pView = pTextDoc->GetNextView(pos);

      if (pView->IsKindOf(RUNTIME_CLASS(CTextView)))
        {
        CTextView* pmyView = (CTextView*)pView;

        // find actual window length for appending [#422]

        int iLength = pmyView->GetWindowTextLength ();

        if (bReplace)
          pmyView->GetEditCtrl ().SetSel (0, -1, FALSE);
        else
          pmyView->GetEditCtrl ().SetSel (iLength, iLength, FALSE);
        pmyView->GetEditCtrl ().ReplaceSel (strText);
        return true;
        } // end of having the right type of view
      }   // end of having a view
    return false;
#endif
    } // end of having an existing notepad document

  BOOL bOK = CreateTextWindow ((LPCTSTR) strText,     // contents
                      (LPCTSTR) strTitle,     // title
                      NULL,   // document
                      0,      // document number
                      App.m_strDefaultInputFont,
                      App.m_iDefaultInputFontHeight,
                      App.m_iDefaultInputFontWeight,
                      App.m_iDefaultOutputFontCharset,
                      RGB (0, 0, 0),
                      RGB (255, 255, 255),
                      "",     // search string
                      "",       // line preamble
                      false,
                      false,
                      false,
                      false,  
                      false,
                      false,
                      iNotepadType
                      );

  return bOK;
  }   // end of CMUSHclientApp::AppendToTheNotepad


CTextDocument * CMUSHclientApp::FindNotepad (const CString strTitle)
  {
CTextDocument * pTextDoc = NULL;

  // during startup, may not exist
  if (App.m_pNormalDocTemplate == NULL)
    return NULL;

  for (POSITION docPos = App.m_pNormalDocTemplate->GetFirstDocPosition();
      docPos != NULL; )
    {
    // Advance through the same template the position came from (see the note in
    // CMUSHclientDoc::OnEditFliptonotepad): notepad docs live in
    // m_pNormalDocTemplate, and the portable POSITION shim is per-template.
    pTextDoc = (CTextDocument *) App.m_pNormalDocTemplate->GetNextDoc(docPos);

    // ignore related worlds
    if (pTextDoc->m_pRelatedWorld == NULL &&
       pTextDoc->m_strTitle.CompareNoCase (strTitle) == 0)
      return pTextDoc;      // right title, not attached to a world

    } // end of doing each document


  return NULL;    // not found
  }  // end of CMUSHclientApp::FindNotepad

BOOL CMUSHclientApp::ActivateNotepad(LPCTSTR Title)
{
CTextDocument * pTextDoc = FindNotepad (Title);

  if (pTextDoc)
    {
#ifndef _WIN32
    // Non-Windows: use the wx callback wired by CWxNotepadFrame.
    if (pTextDoc->m_fn_activate)
      {
      pTextDoc->m_fn_activate ();
      return true;
      }
    return false;
#else
    // activate the view
    POSITION pos=pTextDoc->GetFirstViewPosition();

    if (pos)
      {
      CView* pView = pTextDoc->GetNextView(pos);

      if (pView->IsKindOf(RUNTIME_CLASS(CTextView)))
        {
        CTextView* pmyView = (CTextView*)pView;
        pmyView->GetParentFrame ()->ActivateFrame ();
        pmyView->GetParentFrame ()->SetActiveView(pmyView);
        return true;
        } // end of having the right type of view
      }   // end of having a view
#endif
    } // end of having an existing notepad document
 return false;
}   // end of CMUSHclientApp::ActivateNotepad
