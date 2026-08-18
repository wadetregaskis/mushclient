#include "stdafx.h"
#include "MUSHclient.h"

#include "doc.h"
#include "dialogs/MapDlg.h"
#include "dialogs/MapMoveDlg.h"
#include "dialogs/MapCommentDlg.h"
#include "mainfrm.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

// MapDirectionsMap + LoadMapDirections moved to doc_shared.cpp (shared by
// every build so the portable engine sees the same directions - P1 de-drift).


void CMUSHclientDoc::OnGameMapper() 
{
CMapDlg dlg;


  dlg.m_bEnable = m_bMapping;	
  dlg.m_bRemoveMapReverses = m_bRemoveMapReverses;	
  dlg.m_pDoc = this;
  dlg.m_bMapFailureRegexp = m_bMapFailureRegexp;
  dlg.m_strMappingFailure = m_strMappingFailure;

  // if they open the mapper with no path, assume they want to start one
  if (m_strMapList.IsEmpty ())
    dlg.m_bEnable = true;

  // what I am worried about here is that they will do a special move while
  // the map dialog is open, and that the error message won't be detected,
  // thus, I will force mapping on while we look at the dialog, so that any
  // moves made within it are removed from the list if they cause an error

  bool bSave = m_bMapping;
  
  m_bMapping = true;
  if (dlg.DoModal () != IDOK)
    {
    m_bMapping = bSave;

    // check for the case where they open the map dialog box, see enabled is checked
    // then press Cancel. This would in fact not enable it, so we should warn them.

    if (!m_bMapping && m_strMapList.IsEmpty ())
      if (::TMessageBox ("Warning - mapping has not been turned on because you pressed 'Cancel'.\n\n"
                       "Do you want mapping enabled now?",  MB_ICONQUESTION | MB_YESNO) == IDYES)
         m_bMapping = true;

    return;
    }

  m_bMapping = dlg.m_bEnable;	
  m_bRemoveMapReverses = dlg.m_bRemoveMapReverses;	

  // document is modified if they have changed these
  if (m_bMapFailureRegexp  != dlg.m_bMapFailureRegexp ||
      m_strMappingFailure != dlg.m_strMappingFailure)
        SetModifiedFlag ();

  m_bMapFailureRegexp  = dlg.m_bMapFailureRegexp;
  m_strMappingFailure = dlg.m_strMappingFailure;

  if (m_bMapFailureRegexp && !m_strMappingFailure.IsEmpty ())
    {
    try
      {
      m_MapFailureRegexp = regcomp (m_strMappingFailure, (m_bUTF_8 ? PCRE_UTF8 : 0));
      }
    catch (CException* e)
      {
      char sMessage [1000];
      e->GetErrorMessage (sMessage, sizeof sMessage);
      ::UMessageBox (TFormat ("Error \"%s\" processing mapping failure regular expression \"%s\"", 
                       sMessage,
                       (LPCTSTR) m_strMappingFailure));
      e->Delete ();
      m_bMapFailureRegexp = FALSE; 
      m_MapFailureRegexp = NULL;
      } // end of catch

    } // end compiling regular expression

}   // end of CMUSHclientDoc::OnGameMapper

void CMUSHclientDoc::OnUpdateGameMapper(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->Enable ();
  pCmdUI->SetCheck (m_bMapping);	
}   // end of CMUSHclientDoc::OnUpdateGameMapper


// CMUSHclientDoc::AddToMap moved to doc_shared.cpp (compiled into both builds)
// so the doc-side auto-mapper logic is single-sourced; the headless stub used
// to disable it entirely.  DrawMappingStatusLine (the view half) stays below.



void CMUSHclientDoc::OnGameDomapperspecial() 
{
CMapMoveDlg dlg;

  dlg.m_bSendToMUD = true;
  dlg.m_strAction  =  m_strSpecialForwards;
  dlg.m_strReverse =  m_strSpecialBackwards;
  
  if (dlg.DoModal () != IDOK)
    return;

  // remember it in case they misspelt it and want to try again
  m_strSpecialForwards  = dlg.m_strAction;     
  m_strSpecialBackwards = dlg.m_strReverse;

  if (dlg.m_bSendToMUD && !dlg.m_strAction.IsEmpty ())
    {
    // don't log it twice (eg. if they enter "ne")
    bool bSave = m_bMapping;
    m_bMapping = false;
    SendMsg (dlg.m_strAction, m_display_my_input, false, LoggingInput ());
    m_bMapping = bSave;
    }

  CString strDirection;

  strDirection = dlg.m_strAction + "/" + dlg.m_strReverse;

  m_strMapList.AddTail (strDirection);
	
  // update status line
  DrawMappingStatusLine ();

}

void CMUSHclientDoc::OnUpdateGameDomapperspecial(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->Enable (m_bMapping);	
	
}


void CMUSHclientDoc::OnGameDomappercomment() 
{
CMapCommentDlg dlg;
  
  if (dlg.DoModal () != IDOK)
    return;

  CString str = "{";
  str += dlg.m_strComment;
  str += "}";

  m_strMapList.AddTail (str);
	
  // update status line
  DrawMappingStatusLine ();
	
}

void CMUSHclientDoc::OnUpdateGameDomappercomment(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->Enable (m_bMapping);	
}

// OUTPUT_PREVIOUS_ONE + CalculateSpeedWalkString are single-sourced in
// doc_shared.cpp (compiled into every build).


void CMUSHclientDoc::DrawMappingStatusLine (void)
  {
  CString strSpeedWalk = CalculateSpeedWalkString (true); // omit comments
  if (!strSpeedWalk.IsEmpty ())
    {
    if (strSpeedWalk.GetLength () > 50)
      strSpeedWalk = strSpeedWalk.Left (50) + " ...";
#ifdef _WIN32
    Frame.SetStatusMessageNow (TFormat ("Mapper: %s", (LPCTSTR) strSpeedWalk));
#else
    // Port: attribute to THIS doc, so a background world's mapper status
    // lands on its own status bar, not the frontmost world's.
    Frame.SetStatusMessageNow (this, TFormat ("Mapper: %s", (LPCTSTR) strSpeedWalk));
#endif
    m_tStatusDisplayed = CTime::GetCurrentTime ();
    m_bShowingMapperStatus = true;
    }
  }   // end of CMUSHclientDoc::DrawMappingStatusLine 

