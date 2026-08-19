// methods_miniwindows.cpp

// Methods for miniwindows - main implementation in miniwindows.cpp

#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../../MUSHview.h"
#include "../../blending.h"
#include "../../color.h"
#include "../errors.h"

// Implements:

//    BlendPixel
//    FilterPixel
//    WindowAddHotspot
//    WindowArc
//    WindowBezier
//    WindowBlendImage
//    WindowCircleOp
//    WindowCreate
//    WindowCreateImage
//    WindowDelete
//    WindowDeleteAllHotspots
//    WindowDeleteHotspot
//    WindowDragHandler
//    WindowDrawImage
//    WindowDrawImageAlpha
//    WindowFilter
//    WindowFont
//    WindowFontInfo
//    WindowFontList
//    WindowGetImageAlpha
//    WindowGetPixel
//    WindowGradient
//    WindowHotspotInfo
//    WindowHotspotList
//    WindowHotspotTooltip
//    WindowImageFromWindow
//    WindowImageInfo
//    WindowImageList
//    WindowImageOp
//    WindowInfo
//    WindowLine
//    WindowList
//    WindowLoadImage
//    WindowMenu
//    WindowMergeImageAlpha
//    WindowMoveHotspot
//    WindowPolygon
//    WindowPosition
//    WindowRectOp
//    WindowResize
//    WindowScrollwheelHandler
//    WindowSetPixel
//    WindowSetZOrder
//    WindowShow
//    WindowText
//    WindowTextWidth
//    WindowTransformImage
//    WindowWrite


/* positions: 

  0 = stretch to output view size 
  1 = stretch with aspect ratio

  2 = stretch to owner size 
  3 = stretch with aspect ratio
  
  -- going clockwise here:

  -- top
  4 = top left
  5 = center left-right at top
  6 = top right

  -- rh side
  7 = on right, center top-bottom
  8 = on right, at bottom

  -- bottom
  9 = center left-right at bottom

  -- lh side
  10 = on left, at bottom  
  11 = on left, center top-bottom 

  -- middle
  12 = center all

  13 = tile


  */

// create (or re-create) a mini-window
long CMUSHclientDoc::WindowCreate(LPCTSTR Name, 
                                  long Left, long Top, 
                                  long Width, long Height, 
                                  short Position, 
                                  long Flags, 
                                  long BackgroundColour) 
  {

  if (strlen (Name) == 0) 
    return eNoNameSpecified;    // empty name throws out various things

  if (Width < 0 || Height < 0)
    return eBadParameter;

  MiniWindowMapIterator it = m_MiniWindows.find (Name);

  CMiniWindow * pMiniWindow = NULL;

  if (it == m_MiniWindows.end ())
    {
    pMiniWindow = new CMiniWindow ();
    m_MiniWindows [Name] = pMiniWindow;
    }
  else
    pMiniWindow = it->second;

  pMiniWindow->Create (Left, Top, Width, Height,
                       Position, Flags, 
                       BackgroundColour);

  pMiniWindow->m_sCreatingPlugin.erase ();
  if (m_CurrentPlugin)
    pMiniWindow->m_sCreatingPlugin = m_CurrentPlugin->m_strID;

  SortWindows ();  // need to re-make sorted list in Z-order
  UpdateAllViews (NULL);

	return eOK;
}     // end of CMUSHclientDoc::WindowCreate

// set/clear the show flag so the window becomes visible
long CMUSHclientDoc::WindowShow(LPCTSTR Name, BOOL Show) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  it->second->Show (Show);

  UpdateAllViews (NULL);

	return eOK;
}     // end of CMUSHclientDoc::WindowShow

/*

------------------------------------------------------------------------------------
See: http://www.functionx.com/win32/Lesson11.htm

  for some nice examples of what they look like
------------------------------------------------------------------------------------

  */

// various rectangle operations
long CMUSHclientDoc::WindowRectOp(LPCTSTR Name, short Action, 
                                  long Left, long Top, long Right, long Bottom, 
                                  long Colour1, long Colour2) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->RectOp (Action, 
                             Left, Top, Right, Bottom, 
                             Colour1, Colour2);

}   // end of CMUSHclientDoc::WindowRectOp


// various circle/ellipse/pie operations
long CMUSHclientDoc::WindowCircleOp(LPCTSTR Name, short Action, 
                                    long Left, long Top, long Right, long Bottom, 
                                    long PenColour, long PenStyle, long PenWidth, 
                                    long BrushColour, long BrushStyle, 
                                    long Extra1, long Extra2, long Extra3, long Extra4) 
{
  if (strlen (Name) == 0) 
    return eNoNameSpecified;    // empty name throws out various things

  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->CircleOp (Action, 
                               Left, Top, Right, Bottom, 
                               PenColour, PenStyle, PenWidth, 
                               BrushColour, BrushStyle,
                               Extra1, Extra2, Extra3, Extra4);

}     // end of CMUSHclientDoc::WindowCircleOp 


// add a font to our map of fonts by name (eg. "inventory")
long CMUSHclientDoc::WindowFont(LPCTSTR Name,        // which window
                                LPCTSTR FontId,      // eg. Inventory
                                LPCTSTR FontName,    // eg. Courier New
                                double Size,         // eg. 9 pt
                                BOOL Bold, BOOL Italic, BOOL Underline, BOOL Strikeout, 
                                short Charset, 
                                short PitchAndFamily) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->Font (FontId, FontName,
                           Size, 
                           Bold, Italic, Underline, Strikeout, 
                           Charset, PitchAndFamily);
}    // end of CMUSHclientDoc::WindowFont

// output text, ordinary or UTF8 - returns length of text
long CMUSHclientDoc::WindowText(LPCTSTR Name,      // which window
                                LPCTSTR FontId,    // eg. Inventory
                                LPCTSTR Text,      // what to say
                                long Left, long Top, long Right, long Bottom,   // where to put it
                                long Colour,       // colour to show it in
                                BOOL Unicode)      // true if UTF8
{
  if (strlen (Name) == 0) 
    return -1;    // empty name throws out various things

  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return -1;

  return it->second->Text (FontId, Text,  Left,  Top,  Right,  Bottom, Colour, Unicode );
}    // end of CMUSHclientDoc::WindowText 

// measure text, ordinary or UTF8
long CMUSHclientDoc::WindowTextWidth(LPCTSTR Name, LPCTSTR FontId, LPCTSTR Text, BOOL Unicode) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return -1;

  return it->second->TextWidth (FontId, Text, Unicode );
}     // end of CMUSHclientDoc::WindowTextWidth

// return info about the select font
VARIANT CMUSHclientDoc::WindowFontInfo(LPCTSTR Name, LPCTSTR FontId, long InfoType) 
{

	VARIANT vaResult;
	VariantInit(&vaResult);

  vaResult.vt = VT_EMPTY;  

  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
  	return vaResult;

  it->second->FontInfo (FontId, InfoType, vaResult);
	return vaResult;
}    // end of CMUSHclientDoc::WindowFontInfo

// draws a straight line
long CMUSHclientDoc::WindowLine(LPCTSTR Name, 
                                long x1, long y1, long x2, long y2, 
                                long PenColour, long PenStyle, long PenWidth) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->Line (x1, y1, x2, y2, PenColour, PenStyle, PenWidth);
}


// draws an arc
long CMUSHclientDoc::WindowArc(LPCTSTR Name, 
                               long Left, long Top, long Right, long Bottom, 
                               long x1, long y1, 
                               long x2, long y2, 
                               long PenColour, long PenStyle, long PenWidth) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->Arc (Left, Top, Right, Bottom, x1, y1, x2, y2, PenColour, PenStyle, PenWidth);
}      // end of CMUSHclientDoc::WindowArc


// return list of windows we have made
VARIANT CMUSHclientDoc::WindowList() 
{
  COleSafeArray sa;   // for array list

  long iCount = 0;
  
  // put the arrays into the array
  if (!m_MiniWindows.empty ())    // cannot create empty dimension
    {
    sa.CreateOneDim (VT_VARIANT, m_MiniWindows.size ());

    for (MiniWindowMapIterator it = m_MiniWindows.begin (); 
         it != m_MiniWindows.end ();
         it++)
           {
            // the array must be a bloody array of variants, or VBscript kicks up
            COleVariant v (it->first.c_str ());
            sa.PutElement (&iCount, &v);
            iCount++;
           }

    } // end of having at least one

	return sa.Detach ();
}    // end of CMUSHclientDoc::WindowList

// return list of fonts created for this window
VARIANT CMUSHclientDoc::WindowFontList(LPCTSTR Name) 
{
	VARIANT vaResult;
	VariantInit(&vaResult);

  vaResult.vt = VT_EMPTY;  

  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
  	return vaResult;

  it->second->FontList (vaResult);
	return vaResult;
}    // end of CMUSHclientDoc::WindowFontList

// information about a window
VARIANT CMUSHclientDoc::WindowInfo(LPCTSTR Name, long InfoType) 
{
	VARIANT vaResult;
	VariantInit(&vaResult);

  vaResult.vt = VT_EMPTY;  

  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
  	return vaResult;

  it->second->Info (InfoType, vaResult);
	return vaResult;
}   // end of CMUSHclientDoc::WindowInfo

// load an image from disk
long CMUSHclientDoc::WindowLoadImage(LPCTSTR Name, 
                                     LPCTSTR ImageId, 
                                     LPCTSTR FileName) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->LoadImage (ImageId, FileName);
}   // end of  CMUSHclientDoc::WindowLoadImage

// load an image from memory
long CMUSHclientDoc::WindowLoadImageMemory(LPCTSTR Name, 
                                           LPCTSTR ImageId, 
                                           unsigned char * Buffer, 
                                           const size_t Length,
                                           const bool bAlpha) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->LoadImageMemory (ImageId, Buffer, Length, bAlpha);
}   // end of  CMUSHclientDoc::WindowLoadImageMemory


long CMUSHclientDoc::WindowWrite(LPCTSTR Name, LPCTSTR FileName) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->Write (FileName);
}   // CMUSHclientDoc::WindowWrite


// draw an image
long CMUSHclientDoc::WindowDrawImage(LPCTSTR Name, 
                                     LPCTSTR ImageId, 
                                     long Left, long Top, long Right, long Bottom, 
                                     short Mode,
                                     long SrcLeft, long SrcTop, long SrcRight, long SrcBottom) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->DrawImage (ImageId, Left, Top, Right, Bottom, Mode, 
                                SrcLeft,   SrcTop,   SrcRight,   SrcBottom);
}     // end of CMUSHclientDoc::WindowDrawImage

// list loaded images
VARIANT CMUSHclientDoc::WindowImageList(LPCTSTR Name) 
{
	VARIANT vaResult;
	VariantInit(&vaResult);

  vaResult.vt = VT_EMPTY;  

  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
  	return vaResult;

  it->second->ImageList (vaResult);
	return vaResult;
}   // end of CMUSHclientDoc::WindowImageList


// info about an image
VARIANT CMUSHclientDoc::WindowImageInfo(LPCTSTR Name, LPCTSTR ImageId, long InfoType) 
{
	VARIANT vaResult;
	VariantInit(&vaResult);

  vaResult.vt = VT_EMPTY;  

  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
  	return vaResult;

  it->second->ImageInfo (ImageId, InfoType, vaResult);
	return vaResult;
}  // end of CMUSHclientDoc::WindowImageInfo



// draw a bezier curve
long CMUSHclientDoc::WindowBezier(LPCTSTR Name, 
                                  LPCTSTR Points, 
                                  long PenColour, long PenStyle, long PenWidth) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->Bezier (Points, PenColour, PenStyle, PenWidth);
}  // end of CMUSHclientDoc::WindowBezier


// draw an open or closed polygon
long CMUSHclientDoc::WindowPolygon(LPCTSTR Name, 
                                   LPCTSTR Points, 
                                   long PenColour, long PenStyle, long PenWidth, 
                                   long BrushColour, long BrushStyle,
                                   BOOL Close,       // close the polygon
                                   BOOL Winding)     // ALTERNATE or WINDING fill
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->Polygon (Points, PenColour, PenStyle, PenWidth, BrushColour, BrushStyle, Close, Winding);
}  // end of CMUSHclientDoc::WindowPolygon


// move an existing window
long CMUSHclientDoc::WindowPosition(LPCTSTR Name, 
                                    long Left, long Top, 
                                    short Position, 
                                    long Flags) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  long status = it->second->Position (Left, Top, Position, Flags);

  if (status == eOK)
    UpdateAllViews (NULL);

  return status;

}    // end of CMUSHclientDoc::WindowPosition


/*

  Cursor values:

  0:  arrow                                
  1:  hand                                 
  2:  I-beam                               
  3:  + symbol                           
  4:  wait (hour-glass)                    
  5:  up arrow                             
  6:  arrow nw-se                          
  7:  arrow ne-sw                          
  8:  arrow e-w                            
  9:  arrow n-s                            
 10:  arrow - all ways                     
 11:  (X) no, no, I won't do that, but ... 
 12:  help  (? symbol)                     

  */


// add a hotspot for handling mouse-over, mouse up/down events
long CMUSHclientDoc::WindowAddHotspot(LPCTSTR Name, 
                                     LPCTSTR HotspotId, 
                                     long Left, long Top, long Right, long Bottom, 
                                     LPCTSTR MouseOver, 
                                     LPCTSTR CancelMouseOver, 
                                     LPCTSTR MouseDown, 
                                     LPCTSTR CancelMouseDown, 
                                     LPCTSTR MouseUp, 
                                     LPCTSTR TooltipText, 
                                     long Cursor, 
                                     long Flags) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  CMiniWindow * mw = it->second;

  // don't recurse into infinite loops
  if (m_bInWindowAddHotspot)
    return eItemInUse;

  m_bInWindowAddHotspot = true;

  string sPluginID;

  if (m_CurrentPlugin)                            
    sPluginID = m_CurrentPlugin->m_strID;

  long status;

  status = mw->AddHotspot (this,
                           HotspotId,
                           sPluginID,
                           Left, Top, Right, Bottom,
                           MouseOver,
                           CancelMouseOver,
                           MouseDown,
                           CancelMouseDown,
                           MouseUp,
                           TooltipText,
                           Cursor,
                           Flags);

  // in mouse was over hotspot when it was created, do a "mouse move" to detect this
  // Through the typed view seam as well (pal::IWorldView::mouseCaptured): the
  // Windows adapter answers with the same ::GetCapture() this used to call, but
  // the free function is a permanent nullptr off Windows, so the guard was dead
  // there and a hotspot added from a drag callback re-entered the routing.
  if ((status == eOK) && !(m_pWorldView && m_pWorldView->mouseCaptured ()))
    {

    // only check the current hotspot, not all of them
    HotspotMapIterator it = mw->m_Hotspots.find (HotspotId);
    if (it != mw->m_Hotspots.end ())
      {
      CHotspot * pHotspot = it->second;
      if ( pHotspot->m_rect.PtInRect(mw->m_last_mouseposition) )
        {
        // Through the typed view seam (pal::IWorldView) rather than this
        // doc's view list: the Windows adapter runs the identical CMUSHView
        // walk (pal/win_worldview.cpp mouseMoveMiniWindow), while the ports —
        // which never construct a CMUSHView, so the walk found nothing — get
        // the same immediate MouseOver through their own frontend.  The
        // m_bInWindowAddHotspot guard above still covers the re-entrancy.
        if (m_pWorldView)
          m_pWorldView->mouseMoveMiniWindow (m_lastMousePosition.x,
                                             m_lastMousePosition.y);
        }
      } // end of hotspot found
    } // end of added hotspot OK


  m_bInWindowAddHotspot = false;

  return status;

}  // end of CMUSHclientDoc::WindowHotspot

// remove a previously-installed hotspot
long CMUSHclientDoc::WindowDeleteHotspot(LPCTSTR Name, LPCTSTR HotspotId) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->DeleteHotspot (HotspotId);
}   // end of CMUSHclientDoc::WindowDeleteHotspot

// return list of all hotspots in this miniwindow
VARIANT CMUSHclientDoc::WindowHotspotList(LPCTSTR Name) 
{
	VARIANT vaResult;
	VariantInit(&vaResult);

  vaResult.vt = VT_EMPTY;  

  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
  	return vaResult;

  it->second->HotspotList (vaResult);
	return vaResult;
}    // end of CMUSHclientDoc::WindowHotspotList

// delete all hotspots
long CMUSHclientDoc::WindowDeleteAllHotspots(LPCTSTR Name) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->DeleteAllHotspots ();
}    // end of CMUSHclientDoc::WindowDeleteAllHotspots


// get information about a hotspot
VARIANT CMUSHclientDoc::WindowHotspotInfo(LPCTSTR Name, LPCTSTR HotspotId, long InfoType) 
{
	VARIANT vaResult;
	VariantInit(&vaResult);

  vaResult.vt = VT_EMPTY;  

  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
  	return vaResult;

  it->second->HotspotInfo (HotspotId, InfoType, vaResult);
	return vaResult;
}      // end of CMUSHclientDoc::WindowHotspotInfo


long CMUSHclientDoc::WindowImageOp(LPCTSTR Name, short Action, 
                                   long Left, long Top, long Right, long Bottom, 
                                   long PenColour, long PenStyle, long PenWidth,
                                   long BrushColour,  // 0-bits are drawn in this colour
                                   LPCTSTR ImageId, 
                                   long EllipseWidth, long EllipseHeight) 
  {
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->ImageOp (Action, Left, Top, Right, Bottom, 
                              PenColour, PenStyle, PenWidth, 
                              BrushColour, ImageId, 
                              EllipseWidth, EllipseHeight);
}    // end of CMUSHclientDoc::WindowImageOp

long CMUSHclientDoc::WindowCreateImage(LPCTSTR Name, LPCTSTR ImageId, 
                                       long Row1, long Row2, long Row3, long Row4, long Row5, long Row6, long Row7, long Row8) 
{

  if (strlen (Name) == 0) 
    return eNoNameSpecified;    // empty name throws out various things

  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->CreateImage (ImageId, Row1, Row2, Row3, Row4, Row5, Row6, Row7, Row8);
}     // end of CMUSHclientDoc::WindowCreateImage


long CMUSHclientDoc::WindowBlendImage(LPCTSTR Name, LPCTSTR ImageId, 
                                      long Left, long Top, long Right, long Bottom, 
                                      short Mode, double Opacity, 
                                      long SrcLeft, long SrcTop, long SrcRight, long SrcBottom) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->BlendImage (ImageId, Left, Top, Right, Bottom, Mode, Opacity, SrcLeft, SrcTop, SrcRight, SrcBottom);
}   // end of CMUSHclientDoc::WindowBlendImage


long CMUSHclientDoc::WindowImageFromWindow(LPCTSTR Name, LPCTSTR ImageId, LPCTSTR SourceWindow) 
  {
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  CMiniWindow * pDestWindow = it->second;

  it = m_MiniWindows.find (SourceWindow);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  CMiniWindow * pSrcWindow = it->second;

  return pDestWindow->ImageFromWindow (ImageId, pSrcWindow);
}  // end of CMUSHclientDoc::WindowImageFromWindow



long CMUSHclientDoc::WindowGradient(LPCTSTR Name, 
                                    long Left, long Top, long Right, long Bottom, 
                                    long StartColour, long EndColour, 
                                    short Mode) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->Gradient (Left, Top, Right, Bottom, 
                                    StartColour, EndColour, 
                                    Mode);
}    // end of CMUSHclientDoc::WindowGradient

  
long CMUSHclientDoc::WindowFilter(LPCTSTR Name, long Left, long Top, long Right, long Bottom, 
                                  short Operation, double Options) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->Filter (Left, Top, Right, Bottom, 
                                    Operation, Options);
}    // CMUSHclientDoc::WindowFilter
                                       

long CMUSHclientDoc::WindowSetPixel(LPCTSTR Name, long x, long y, long Colour) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->SetPixel (x, y, Colour);
}   // end of CMUSHclientDoc::WindowSetPixel


long CMUSHclientDoc::WindowSetZOrder(LPCTSTR Name, long Order) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  long status = it->second->SetZOrder (Order);

  SortWindows ();   // need to re-make sorted list in Z-order

  return status;   // now we can return status

}   // end of CMUSHclientDoc::WindowSetZOrder


long CMUSHclientDoc::WindowGetPixel(LPCTSTR Name, long x, long y) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return -2;

  return it->second->GetPixel (x, y);
}     // end of CMUSHclientDoc::WindowGetPixel





long CMUSHclientDoc::WindowMergeImageAlpha(LPCTSTR Name, 
                                           LPCTSTR ImageId, LPCTSTR MaskId, 
                                           long Left, long Top, long Right, long Bottom, 
                                           short Mode, 
                                           double Opacity, 
                                           long SrcLeft, long SrcTop, long SrcRight, long SrcBottom) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->MergeImageAlpha (ImageId, MaskId, Left, Top, Right, Bottom, Mode, Opacity, SrcLeft, SrcTop, SrcRight, SrcBottom);
}    // end of CMUSHclientDoc::WindowMergeImageAlpha


long CMUSHclientDoc::WindowDelete(LPCTSTR Name) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  if (it->second->m_bExecutingScript)
    return eItemInUse;

  delete it->second;

  m_MiniWindows.erase (it);

  SortWindows ();   // need to re-make sorted list in Z-order

  UpdateAllViews (NULL);

	return eOK;
}    // end of CMUSHclientDoc::WindowDelete


BSTR CMUSHclientDoc::WindowMenu(LPCTSTR Name, long Left, long Top, LPCTSTR Items) 
{

  CString strResult;

  MiniWindowMapIterator it = m_MiniWindows.find (Name);

	CView* pView = NULL;
  
  for(POSITION pos = GetFirstViewPosition(); pos != NULL; )
	  {
	  pView = GetNextView(pos);

	  if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
      break;

    }

#ifdef _WIN32
  if (pView && it != m_MiniWindows.end ())
#else
  // The wx port has no CMUSHView registered in the document's view list, so the
  // loop above leaves pView null.  CMiniWindow::Menu takes the document instead
  // off-Windows: the anchor is in this world's output-pane client coordinates,
  // and the pane is what the port converts through in place of the view.
  if (it != m_MiniWindows.end ())
#endif
    strResult = it->second->Menu (Left, Top, Items, (CMUSHView *) pView, this);

	return strResult.AllocSysString();

}   // end of CMUSHclientDoc::WindowMenu



long CMUSHclientDoc::WindowHotspotTooltip(LPCTSTR Name, LPCTSTR HotspotId, LPCTSTR TooltipText) 
{

  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
  	return eNoSuchWindow;

  return it->second->HotspotTooltip (HotspotId, TooltipText);
}    // end of CMUSHclientDoc::WindowHotspotTooltip

long CMUSHclientDoc::WindowDrawImageAlpha(LPCTSTR Name, LPCTSTR ImageId, long Left, long Top, long Right, long Bottom, double Opacity, long SrcLeft, long SrcTop) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->DrawImageAlpha (ImageId, Left, Top, Right, Bottom, Opacity, 
                                    SrcLeft,   SrcTop);

}  // end of CMUSHclientDoc::WindowDrawImageAlpha

long CMUSHclientDoc::WindowGetImageAlpha(LPCTSTR Name, LPCTSTR ImageId, long Left, long Top, long Right, long Bottom, long SrcLeft, long SrcTop) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->GetImageAlpha (ImageId, Left, Top, Right, Bottom, 
                                    SrcLeft,   SrcTop);

}   // end of CMUSHclientDoc::WindowGetImageAlpha



long CMUSHclientDoc::WindowResize(LPCTSTR Name, long Width, long Height, long BackgroundColour) 
{

  if (Width < 0 || Height < 0)
    return eBadParameter;

  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->Resize (Width, Height, BackgroundColour);
}  // end of CMUSHclientDoc::WindowResize
     

long CMUSHclientDoc::WindowMoveHotspot(LPCTSTR Name, LPCTSTR HotspotId, long Left, long Top, long Right, long Bottom) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
  	return eNoSuchWindow;

  return it->second->MoveHotspot (HotspotId, Left, Top, Right, Bottom);
}   // end of CMUSHclientDoc::WindowMoveHotspot


long CMUSHclientDoc::WindowTransformImage(LPCTSTR Name, LPCTSTR ImageId, float Left, float Top, short Mode, float Mxx, float Mxy, float Myx, float Myy) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  return it->second->TransformImage (ImageId, Left, Top, Mode, Mxx, Mxy, Myx, Myy);
}  // end of CMUSHclientDoc::WindowTransformImage



long CMUSHclientDoc::WindowDragHandler(LPCTSTR Name, LPCTSTR HotspotId, LPCTSTR MoveCallback, LPCTSTR ReleaseCallback, long Flags) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  string sPluginID;

  if (m_CurrentPlugin)                            
    sPluginID = m_CurrentPlugin->m_strID;

  return it->second->DragHandler (this, HotspotId, sPluginID, MoveCallback, ReleaseCallback, Flags);
}   // end of CMUSHclientDoc::WindowDragHandler


long CMUSHclientDoc::WindowScrollwheelHandler(LPCTSTR Name, LPCTSTR HotspotId, LPCTSTR MoveCallback) 
{
  MiniWindowMapIterator it = m_MiniWindows.find (Name);
    
  if (it == m_MiniWindows.end ())
    return eNoSuchWindow;

  string sPluginID;

  if (m_CurrentPlugin)                            
    sPluginID = m_CurrentPlugin->m_strID;

  return it->second->ScrollwheelHandler (this, HotspotId, sPluginID, MoveCallback);
}   // end of CMUSHclientDoc::WindowScrollwheelHandler

