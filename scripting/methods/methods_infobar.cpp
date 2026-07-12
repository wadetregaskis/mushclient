// methods_infobar.cpp

// Stuff for drawing in the "info" bar

#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../../mainfrm.h"
#include "../errors.h"

// Implements:

//    Info
//    InfoBackground
//    InfoClear
//    InfoColour
//    InfoFont
//    ShowInfoBar


void CMUSHclientDoc::ShowInfoBar(BOOL Visible)
{
#ifndef _WIN32
  EmitInfobarShow(Visible != 0);
  return;
#endif
  if (Frame.m_wndInfoBar.m_hWnd)
  	Frame.ShowControlBar(&Frame.m_wndInfoBar, Visible != 0, FALSE);
} // end of CMUSHclientDoc::ShowInfoBar


void CMUSHclientDoc::InfoClear()
{
#ifndef _WIN32
  EmitInfobarClear();
  return;
#endif
  if (!Frame.m_wndInfoBar.m_hWnd)
    return;

  CRichEditCtrl * pRichEdit = (CRichEditCtrl *) Frame.m_wndInfoBar.GetDlgItem (IDC_INFOTEXT);

  // back to default colour, font etc.
  pRichEdit->SetDefaultCharFormat (Frame.m_defaultInfoBarFormat);

  // select all
  pRichEdit->SetSel (0, -1);

  // delete everything
  pRichEdit->ReplaceSel ("");


} // end of CMUSHclientDoc::InfoClear


void CMUSHclientDoc::Info(LPCTSTR Message)
{
#ifndef _WIN32
  if (Message) EmitInfobarText(Message);
  return;
#endif
  if (!Frame.m_wndInfoBar.m_hWnd)
    return;

  CRichEditCtrl * pRichEdit = (CRichEditCtrl *) Frame.m_wndInfoBar.GetDlgItem (IDC_INFOTEXT);

  pRichEdit->ReplaceSel (Message);

}   // end of CMUSHclientDoc::Info

/*

  Style bits:

    1 = bold
    2 = italic
    4 = underline
    8 = strikeout

*/


void CMUSHclientDoc::InfoFont(LPCTSTR FontName, short Size, short Style)
{
#ifndef _WIN32
  EmitInfobarFont(FontName ? FontName : "", Size, Style);
  return;
#endif
  if (!Frame.m_wndInfoBar.m_hWnd)
    return;


CHARFORMAT cf;

 CRichEditCtrl * pRichEdit = (CRichEditCtrl *) Frame.m_wndInfoBar.GetDlgItem (IDC_INFOTEXT);

 memset (&cf, 0, sizeof (cf));
 cf.cbSize = sizeof (cf);

 // font name provided?
 if (strlen (FontName) > 0)
   {
   cf.dwMask = CFM_FACE;
   strncpy (cf.szFaceName, FontName, sizeof (cf.szFaceName) - 1);

   // do this so fonts like webdings, marlett and so on work properly
   // without it, such fonts would take (say) "g" and substitute a "g" from
   // the default font, not show the "g" equivalent in the selected font

   cf.dwMask |= CFM_CHARSET;
   cf.bCharSet = SYMBOL_CHARSET;

   }

 // size provided?
 if (Size > 0)
   {
   cf.dwMask |= CFM_SIZE;
   cf.yHeight  = Size * 20; // points
   }

 // style

 cf.dwMask |= CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_UNDERLINE;

 if (Style & 1)   // bold
  cf.dwEffects |= CFE_BOLD;

 if (Style & 2)   // italic
  cf.dwEffects |= CFE_ITALIC;

 if (Style & 4)   // underline
  cf.dwEffects |= CFE_UNDERLINE;

 if (Style & 8)   // strikeout
  cf.dwEffects |= CFE_STRIKEOUT;

 pRichEdit->SetSelectionCharFormat (cf);


}   // end of CMUSHclientDoc::InfoFont


void CMUSHclientDoc::InfoColour(LPCTSTR Name)
{
#ifndef _WIN32
  {
    COLORREF c = 0;
    // SetColour returns non-zero on FAILURE (bad name); apply on success.
    if (!SetColour(Name, c)) EmitInfobarColour(c);
  }
  return;
#endif
  if (!Frame.m_wndInfoBar.m_hWnd)
    return;

COLORREF cValue;

CHARFORMAT cf;

 CRichEditCtrl * pRichEdit = (CRichEditCtrl *) Frame.m_wndInfoBar.GetDlgItem (IDC_INFOTEXT);

 // do foreground colour if possible
 if (SetColour (Name, cValue)) 
   return;

 memset (&cf, 0, sizeof (cf));
 cf.cbSize = sizeof (cf);
 cf.dwMask = CFM_COLOR;
 cf.crTextColor = cValue;
 pRichEdit->SetSelectionCharFormat (cf);

} // end of CMUSHclientDoc::InfoColour

void CMUSHclientDoc::InfoBackground(LPCTSTR Name)
{
#ifndef _WIN32
  {
    COLORREF c = 0;
    if (!SetColour(Name, c)) EmitInfobarBackground(c);
  }
  return;
#endif
  if (!Frame.m_wndInfoBar.m_hWnd)
    return;

  COLORREF cValue;

 CRichEditCtrl * pRichEdit = (CRichEditCtrl *) Frame.m_wndInfoBar.GetDlgItem (IDC_INFOTEXT);

 // do background colour if possible
 if (SetColour (Name, cValue)) 
   return;

 pRichEdit->SetBackgroundColor (FALSE, cValue);

} // end of CMUSHclientDoc::InfoBackground



