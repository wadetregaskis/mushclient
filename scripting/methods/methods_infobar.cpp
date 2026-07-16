// methods_infobar.cpp

// Stuff for drawing in the "info" bar

#if defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD)
#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../../mainfrm.h"
#include "../errors.h"
#else
#include "platform/doc_stub.h"
#endif

// Implements:

//    Info
//    InfoBackground
//    InfoClear
//    InfoColour
//    InfoFont
//    ShowInfoBar


void CMUSHclientDoc::ShowInfoBar(BOOL Visible)
{
  EmitInfobarShow(Visible != 0);
} // end of CMUSHclientDoc::ShowInfoBar


void CMUSHclientDoc::InfoClear()
{
  EmitInfobarClear();
} // end of CMUSHclientDoc::InfoClear


void CMUSHclientDoc::Info(LPCTSTR Message)
{
  if (Message) EmitInfobarText(Message);
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
  EmitInfobarFont(FontName ? FontName : "", Size, Style);
}   // end of CMUSHclientDoc::InfoFont


void CMUSHclientDoc::InfoColour(LPCTSTR Name)
{
  COLORREF c = 0;
  // SetColour returns non-zero on FAILURE (bad name); apply on success.
  if (!SetColour(Name, c)) EmitInfobarColour(c);
} // end of CMUSHclientDoc::InfoColour

void CMUSHclientDoc::InfoBackground(LPCTSTR Name)
{
  COLORREF c = 0;
  if (!SetColour(Name, c)) EmitInfobarBackground(c);
} // end of CMUSHclientDoc::InfoBackground



