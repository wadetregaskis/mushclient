// methods_notepad.cpp

// Stuff for the internal notepad window

#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../errors.h"

#include "../../TextDocument.h"
#include "../../dialogs/ChooseNotepadDlg.h"
#include "../../TextView.h"
#include "../../winplace.h"


// Implements:

//    ActivateNotepad
//    AppendToNotepad
//    CloseNotepad
//    GetNotepadLength
//    GetNotepadList
//    GetNotepadText
//    GetNotepadWindowPosition
//    MoveNotepadWindow
//    NotepadColour
//    NotepadFont
//    NotepadReadOnly
//    NotepadSaveMethod
//    ReplaceNotepad
//    SaveNotepad
//    SendToNotepad


bool CMUSHclientDoc::SwitchToNotepad (void)
  {
#ifndef _WIN32
  // Non-Windows: activate the first notepad belonging to this world, if any.
  for (POSITION docPos = App.m_pNormalDocTemplate->GetFirstDocPosition();
      docPos != NULL; )
    {
    CTextDocument * pTextDoc = (CTextDocument *) App.m_pNormalDocTemplate->GetNextDoc(docPos);
    if (pTextDoc->m_pRelatedWorld == this &&
       pTextDoc->m_iUniqueDocumentNumber == m_iUniqueDocumentNumber)
      {
      if (pTextDoc->m_fn_activate)
        pTextDoc->m_fn_activate();
      return true;
      }
    }
  return false;
#else
  int iCount = 0;

  for (POSITION docPos = App.m_pNormalDocTemplate->GetFirstDocPosition();
      docPos != NULL; )
    {
    CTextDocument * pTextDoc = (CTextDocument *) App.m_pNormalDocTemplate->GetNextDoc(docPos);

    // ignore unrelated worlds
    if (pTextDoc->m_pRelatedWorld == this &&
       pTextDoc->m_iUniqueDocumentNumber == m_iUniqueDocumentNumber)
      iCount++;
    } // end of doing each document

  if (iCount)
    {
    CChooseNotepadDlg dlg;

    dlg.m_pWorld = this;

    if (dlg.DoModal () != IDOK)
      return true;   // they gave up

    if (dlg.m_pTextDocument)  // they chose an existing one
      {
      // activate the view
      POSITION pos=dlg.m_pTextDocument->GetFirstViewPosition();

      if (pos)
        {
        CView* pView = dlg.m_pTextDocument->GetNextView(pos);

        if (pView->IsKindOf(RUNTIME_CLASS(CTextView)))
          {
          CTextView* pmyView = (CTextView*)pView;

          pmyView->GetParentFrame ()->ActivateFrame ();
          pmyView->GetParentFrame ()->SetActiveView(pmyView);
          return true;
          } // end of having the right type of view
        }   // end of having a view

      }   // end of choosing an existing document

    }   // end of having other documents


  return false;
#endif // _WIN32
  }   // end of CMUSHclientDoc::SwitchToNotepad

bool CMUSHclientDoc::AppendToTheNotepad (const CString strTitle,
                                      const CString strText,
                                      const bool bReplace,
                                      const int  iNotepadType)
  {
CTextDocument * pTextDoc = FindNotepad (strTitle);

  if (pTextDoc)
    {
#ifndef _WIN32
    // Non-Windows: use the m_fn_* callbacks wired by CWxNotepadFrame.
    if (pTextDoc->m_fn_get_text_length && pTextDoc->m_fn_set_sel &&
        pTextDoc->m_fn_replace_sel)
      {
      int iLength = pTextDoc->m_fn_get_text_length();
      if (bReplace)
        pTextDoc->m_fn_set_sel(0, -1);
      else
        pTextDoc->m_fn_set_sel(iLength, iLength);
      pTextDoc->m_fn_replace_sel(std::string(strText));
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
#endif // _WIN32
    } // end of having an existing notepad document

  BOOL bOK = CreateTextWindow ((LPCTSTR) strText,     // contents
                      (LPCTSTR) strTitle,     // title
                      this,   // document
                      m_iUniqueDocumentNumber,      // document number
                      m_input_font_name,
                      m_input_font_height,
                      m_input_font_weight,
                      m_input_font_charset,
                      m_input_text_colour,
                      m_input_background_colour,
                      "",       // search string
                      "",       // line preamble
                      false,
                      false,
                      false,
                      false,  
                      false,
                      false,
                      iNotepadType
                      );

  // re-activate the main window
  Activate ();

  return bOK;
  }   // end of CMUSHclientDoc::AppendToTheNotepad

BOOL CMUSHclientDoc::SendToNotepad(LPCTSTR Title, LPCTSTR Contents) 
{
  return CreateTextWindow (Contents,     // contents
                      Title,     // title
                      this,   // document
                      m_iUniqueDocumentNumber,      // document number
                      m_input_font_name,
                      m_input_font_height,
                      m_input_font_weight,
                      m_input_font_charset,
                      m_input_text_colour,
                      m_input_background_colour,
                      "",       // search string
                      "",       // line preamble
                      false,
                      false,
                      false,
                      false,  
                      false,
                      false,
                      eNotepadScript
                      );
}  // end of CMUSHclientDoc::SendToNotepad

BOOL CMUSHclientDoc::AppendToNotepad(LPCTSTR Title, LPCTSTR Contents) 
{
  return AppendToTheNotepad (Title, Contents, false);   // append mode
}    // end of CMUSHclientDoc::AppendToNotepad

BOOL CMUSHclientDoc::ActivateNotepad(LPCTSTR Title)
{
CTextDocument * pTextDoc = FindNotepad (Title);

  if (pTextDoc)
    {
#ifndef _WIN32
    if (pTextDoc->m_fn_activate)
      {
      pTextDoc->m_fn_activate();
      return true;
      }
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
#endif // _WIN32
    } // end of having an existing notepad document
 return false;
}   // end of CMUSHclientDoc::ActivateNotepad


BOOL CMUSHclientDoc::ReplaceNotepad(LPCTSTR Title, LPCTSTR Contents) 
{
  return AppendToTheNotepad (Title, Contents, true);    // replace mode
}   // end of CMUSHclientDoc::ReplaceNotepad


CTextDocument * CMUSHclientDoc::FindNotepad (const CString strTitle)
  {
CTextDocument * pTextDoc = NULL;

  for (POSITION docPos = App.m_pNormalDocTemplate->GetFirstDocPosition();
      docPos != NULL; )
    {
    pTextDoc = (CTextDocument *) App.m_pNormalDocTemplate->GetNextDoc(docPos);

    // ignore unrelated worlds
    if (pTextDoc->m_pRelatedWorld == this &&
       pTextDoc->m_iUniqueDocumentNumber == m_iUniqueDocumentNumber &&
       pTextDoc->m_strTitle.CompareNoCase (strTitle) == 0)
      return pTextDoc;      // right title, world, document number

    } // end of doing each document


  return NULL;    // not found
  }   // end of CMUSHclientDoc::ReplaceNotepad


long CMUSHclientDoc::SaveNotepad(LPCTSTR Title, LPCTSTR FileName, BOOL ReplaceExisting) 
{
CTextDocument * pTextDoc = FindNotepad (Title);

  if (pTextDoc)
    return pTextDoc->DoSave(FileName, ReplaceExisting);

 return false;
}  // end of CMUSHclientDoc::SaveNotepad



long CMUSHclientDoc::CloseNotepad(LPCTSTR Title, BOOL QuerySave) 
{
CTextDocument * pTextDoc = FindNotepad (Title);

  if (pTextDoc)
    {
      // see if they want to save it
    if (QuerySave)
	    if (!pTextDoc->SaveModified())
		    return false;

    // saved OK, let's close it
    pTextDoc->OnCloseDocument ();
    
    return true;
    } // end of having an existing notepad document


return false;
}   // end of CMUSHclientDoc::CloseNotepad


long CMUSHclientDoc::GetNotepadLength(LPCTSTR Title)
{
CTextDocument * pTextDoc = FindNotepad (Title);

int iLength = 0;

  if (pTextDoc)
    {
#ifndef _WIN32
    if (pTextDoc->m_fn_get_text_length)
      iLength = pTextDoc->m_fn_get_text_length();
#else
    // find the view
    POSITION pos=pTextDoc->GetFirstViewPosition();

    if (pos)
      {
      CView* pView = pTextDoc->GetNextView(pos);
      if (pView->IsKindOf(RUNTIME_CLASS(CTextView)))
        {
        CTextView* pmyView = (CTextView*)pView;
        iLength = pmyView->GetWindowTextLength ();
        } // end of having the right type of view
      }   // end of having a view
#endif // _WIN32
    } // end of having an existing notepad document

	return iLength;
}   // end of CMUSHclientDoc::GetNotepadLength

BSTR CMUSHclientDoc::GetNotepadText(LPCTSTR Title)
{
CString strResult;
CTextDocument * pTextDoc = FindNotepad (Title);

  if (pTextDoc)
    {
#ifndef _WIN32
    if (pTextDoc->m_fn_get_text)
      strResult = CString(pTextDoc->m_fn_get_text().c_str());
#else
    // find the view
    POSITION pos=pTextDoc->GetFirstViewPosition();

    if (pos)
      {
      CView* pView = pTextDoc->GetNextView(pos);
      if (pView->IsKindOf(RUNTIME_CLASS(CTextView)))
        {
        CTextView* pmyView = (CTextView*)pView;
        pmyView->GetWindowText (strResult);
        } // end of having the right type of view
      }   // end of having a view
#endif // _WIN32
    } // end of having an existing notepad document


	return strResult.AllocSysString();
}   // end of CMUSHclientDoc::GetNotepadText


// warning - different in Lua
BSTR CMUSHclientDoc::GetNotepadWindowPosition(LPCTSTR Title)
{
	CString strResult;

  CTextDocument * pTextDoc = FindNotepad (Title);

  if (pTextDoc)
    {
#ifndef _WIN32
    if (pTextDoc->m_fn_get_rect)
      {
      // m_fn_get_rect yields "left top right bottom" (space-separated); the
      // Windows BSTR contract is "left,top,width,height" (comma-separated, via
      // windowPositionHelper). Reformat so the IDispatch consumers (Python/Perl/
      // JS/Tcl/Ruby) match Windows. (The Lua getter parses m_fn_get_rect itself.)
      int l = 0, t = 0, r = 0, b = 0;
      if (sscanf(pTextDoc->m_fn_get_rect().c_str(), "%d %d %d %d", &l, &t, &r, &b) == 4)
        strResult.Format("%d,%d,%d,%d", l, t, r - l, b - t);
      }
#else
    CWindowPlacement wp;

    // get the view
    POSITION pos=pTextDoc->GetFirstViewPosition();

    if (pos)
      {
      CView* pView = pTextDoc->GetNextView(pos);

      if (pView->IsKindOf(RUNTIME_CLASS(CTextView)))
        {
        CTextView* pmyView = (CTextView*)pView;
        pmyView->GetParentFrame ()->GetWindowPlacement(&wp);
      	windowPositionHelper (strResult, wp.rcNormalPosition);
        } // end of having the right type of view
      }   // end of having a view
#endif // _WIN32
    } // end of having an existing notepad document

	return strResult.AllocSysString();
}  // end of  CMUSHclientDoc::GetNotepadWindowPosition



long CMUSHclientDoc::NotepadColour (LPCTSTR Title, 
                                      LPCTSTR TextColour, 
                                      LPCTSTR BackgroundColour) 
{
CTextDocument * pTextDoc = FindNotepad (Title);

  if (!pTextDoc)
    return false;

  COLORREF iTextColour,
           iBackColour;

  if (SetColour (TextColour, iTextColour))
    return false;    // can't find colour
  if (SetColour (BackgroundColour, iBackColour))
    return false;    // can't find colour

  pTextDoc->m_textColour = iTextColour;
  pTextDoc->m_backColour = iBackColour;

#ifndef _WIN32
  if (pTextDoc->m_fn_apply_colours)
    pTextDoc->m_fn_apply_colours();
#else
  pTextDoc->UpdateAllViews (NULL);
#endif
  return true;  // did it!

}   // end of CMUSHclientDoc::NotepadColour



long CMUSHclientDoc::NotepadFont(LPCTSTR Title, 
                                 LPCTSTR FontName, 
                                 short Size, 
                                 short Style, 
                                 short Charset) 
{
CTextDocument * pTextDoc = FindNotepad (Title);

  if (!pTextDoc)
    return false;

  // font name provided?
  if (strlen (FontName) > 0)
   pTextDoc->m_strFontName = FontName;

  // size provided?
  if (Size > 0)
   pTextDoc->m_iFontSize = Size;

  if (Style & 1)   // bold
    pTextDoc->m_iFontWeight = FW_BOLD;
  else
    pTextDoc->m_iFontWeight = FW_NORMAL;

  pTextDoc->m_bFontItalic = (Style & 2) != 0;
  pTextDoc->m_bFontUnderline = (Style & 4) != 0;
  pTextDoc->m_bFontStrikeout = (Style & 8) != 0;

  pTextDoc->m_iFontCharset = Charset;

  pTextDoc->SetTheFont ();

  return true;    // done it OK
}  // end of CMUSHclientDoc::NotepadFont


BOOL CMUSHclientDoc::NotepadSaveMethod(LPCTSTR Title, short Method) 
{
CTextDocument * pTextDoc = FindNotepad (Title);

  if (!pTextDoc)
    return false;  // can't find it

  switch (Method)
    {
    case 0: pTextDoc->m_iSaveOnChange = eNotepadSaveDefault; break;
    case 1: pTextDoc->m_iSaveOnChange = eNotepadSaveAlways; break;
    case 2: pTextDoc->m_iSaveOnChange = eNotepadSaveNever; break;
    default: return false;  // bad selector
    }

	return true;
}   // end of NotepadSaveMethod


VARIANT CMUSHclientDoc::GetNotepadList(BOOL All) 
{
  COleSafeArray sa;   // for list

  CTextDocument * pTextDoc;
  long iCount = 0;
  POSITION pos;

  // count number of notepads
  for (pos = App.m_pNormalDocTemplate->GetFirstDocPosition(); pos != NULL; )
    {
    pTextDoc = (CTextDocument *) App.m_pNormalDocTemplate->GetNextDoc(pos);

    if (All || (pTextDoc->m_pRelatedWorld == this &&
       pTextDoc->m_iUniqueDocumentNumber == m_iUniqueDocumentNumber))
      iCount++;

    }

  if (iCount) // cannot create empty array dimension
    {
    sa.CreateOneDim (VT_VARIANT, iCount);
  
    // put the worlds into the array
    for (iCount = 0, pos = App.m_pNormalDocTemplate->GetFirstDocPosition(); pos != NULL; )
      {
      pTextDoc = (CTextDocument *) App.m_pNormalDocTemplate->GetNextDoc(pos);

      // ignore unrelated worlds
      if (!All)
        if (pTextDoc->m_pRelatedWorld != this ||
           pTextDoc->m_iUniqueDocumentNumber != m_iUniqueDocumentNumber)
           continue;

      // the array must be a bloody array of variants, or VBscript kicks up
      COleVariant v (pTextDoc->m_strTitle);
      sa.PutElement (&iCount, &v);

      iCount++;
      }      // end of looping through each notepad
    } // end of having at least one

	return sa.Detach ();
}  // end of CMUSHclientDoc::GetNotepadList


// make a notepad read-only
long CMUSHclientDoc::NotepadReadOnly(LPCTSTR Title, BOOL ReadOnly) 
{
CTextDocument * pTextDoc = FindNotepad (Title);

  if (!pTextDoc)
    return false;

  pTextDoc->SetReadOnly (ReadOnly);

	return true;
}   // end of CMUSHclientDoc::NotepadReadOnly



long CMUSHclientDoc::MoveNotepadWindow(LPCTSTR Title, long Left, long Top, long Width, long Height)
{
CTextDocument * pTextDoc = FindNotepad (Title);

  if (pTextDoc)
    {
#ifndef _WIN32
    if (pTextDoc->m_fn_move_window)
      {
      pTextDoc->m_fn_move_window(
          static_cast<int>(Left), static_cast<int>(Top),
          static_cast<int>(Width), static_cast<int>(Height));
      return true;
      }
#else
    // get the view
    POSITION pos=pTextDoc->GetFirstViewPosition();

    if (pos)
      {
      CView* pView = pTextDoc->GetNextView(pos);

      if (pView->IsKindOf(RUNTIME_CLASS(CTextView)))
        {
        CTextView* pmyView = (CTextView*)pView;
        pmyView->GetParentFrame ()->MoveWindow (Left, Top, Width, Height);
        return true;
        } // end of having the right type of view
      }   // end of having a view
#endif // _WIN32
    } // end of having an existing notepad document


return false;
}   // end of CMUSHclientDoc::MoveNotepadWindow
