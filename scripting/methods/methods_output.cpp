// methods_output.cpp

// General output (and frame) window management

#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../../MUSHview.h"
#include "../../mainfrm.h"
#include "../errors.h"

#include "../../childfrm.h"
#include "../../sendvw.h"
#include "../../winplace.h"
#include "../../MakeWindowTransparent.h"

// Implements:

//    ActivateClient
//    AddFont
//    Bookmark
//    DeleteLines
//    DeleteOutput
//    FlashIcon
//    GetFrame
//    GetRecentLines
//    GetWorldWindowPositionX
//    MoveMainWindow
//    MoveWorldWindow
//    MoveWorldWindowX
//    Pause
//    Redraw
//    Repaint
//    ResetStatusTime
//    SetBackgroundColour
//    SetBackgroundImage
//    SetCursor
//    SetForegroundImage
//    SetOutputFont
//    SetScroll
//    SetTitle
//    SetToolBarPosition
//    SetUnseenLines
//    SetWorldWindowStatus
//    TextRectangle
//    Transparency



void CMUSHclientDoc::Redraw() 
{
  UpdateAllViews (NULL);
}


void CMUSHclientDoc::SetOutputFont(LPCTSTR FontName, short PointSize)
{
  EmitOutputFont(std::string(FontName ? FontName : ""), (int)PointSize,
                 false, false);
}  // end of CMUSHclientDoc::SetOutputFont



void CMUSHclientDoc::DeleteOutput() 
{
  ClearOutput ();
}   // end of CMUSHclientDoc::DeleteOutput


// set pause to true or false for all views
void CMUSHclientDoc::Pause(BOOL Flag)
{
  EmitPaused(Flag != 0);
  m_gui_paused = (Flag != 0);
}   // end of CMUSHclientDoc::Pause



long CMUSHclientDoc::GetFrame() 
{
	return (long) App.m_pMainWnd->m_hWnd;
}   // end of CMUSHclientDoc::GetFrame

// reset time on status bar
void CMUSHclientDoc::ResetStatusTime() 
{
  m_tStatusTime = CTime::GetCurrentTime(); 
}   // end of CMUSHclientDoc::ResetStatusTime


// restores the main frame and brings it to the front
void CMUSHclientDoc::ActivateClient()
{
  ActivateClientWindow();
}  // end of CMUSHclientDoc::ActivateClient



BSTR CMUSHclientDoc::GetRecentLines(long Count) 
{
	CString strResult;

  // assemble multi-line match text - start Count lines back, or at the start
  // of the buffer if it holds fewer lines than that.  Done at deque width: a
  // Count larger than the buffer used to make the difference wrap to a large
  // positive subscript, which then read outside the deque.
  size_t iPos = 0;
  if (Count > 0 && (size_t) Count < m_sRecentLines.size ())
    iPos = m_sRecentLines.size () - Count;

  string s;

  for (int iCount = 0; 
        iCount < Count &&
        iPos < m_sRecentLines.size ()
        ; iPos++, iCount++
      )
    {
    if (!s.empty ())
      s += '\n';
    s += m_sRecentLines [iPos];
    } // end of assembling text
  strResult = s.c_str ();

	return strResult.AllocSysString();
}  // end of CMUSHclientDoc::GetRecentLines


void CMUSHclientDoc::MoveMainWindow(long Left, long Top, long Width, long Height) 
{
  Frame.MoveWindow (Left, Top, Width, Height);
}   // end of CMUSHclientDoc::MoveMainWindow


void CMUSHclientDoc::MoveWorldWindow(long Left, long Top, long Width, long Height) 
{
MoveWorldWindowX (Left, Top, Width, Height, 1);   // move first window
}  // end of CMUSHclientDoc::MoveWorldWindow


void CMUSHclientDoc::MoveWorldWindowX(long Left,
                                      long Top,
                                      long Width,
                                      long Height,
                                      short Which)
{
#ifndef _WIN32
  // each world has exactly one MDI child frame here; Which is always 1
  if (Which == 1)
    MoveWorldWindowRect (Left, Top, Width, Height);
#else
int i = 0;

  for(POSITION pos=GetFirstViewPosition();pos!=NULL;)
    {
    CView* pView = GetNextView(pos);

    if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
      {
      CSendView* pmyView = (CSendView*)pView;

      i++;

      if (i != Which)
        continue;   // wrong one

      pmyView->GetParentFrame ()->MoveWindow (Left, Top, Width, Height);

      break;

      }
    }
#endif

}   // end of CMUSHclientDoc::MoveWorldWindowX

// warning - different in Lua
BSTR CMUSHclientDoc::GetWorldWindowPositionX(short Which)
{
CString strResult;

#ifndef _WIN32
  // each world has exactly one MDI child frame here; Which is always 1
  int x = 0, y = 0, w = 800, h = 600;
  if (Which == 1 && GetOutputRect (x, y, w, h))
    strResult.Format ("%d,%d,%d,%d", x, y, w, h);
#else
int i = 0;

  CWindowPlacement wp;

  for(POSITION pos=GetFirstViewPosition();pos!=NULL;)
    {
    CView* pView = GetNextView(pos);

    if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
      {
      CSendView* pmyView = (CSendView*)pView;

      i++;

      if (i != Which)
        continue;   // wrong one

      pmyView->GetParentFrame ()->GetWindowPlacement(&wp);
    	windowPositionHelper (strResult, wp.rcNormalPosition);

      break;

      }
    }
#endif

	return strResult.AllocSysString();
}   // end of CMUSHclientDoc::GetWorldWindowPositionX





CChildFrame * CMUSHclientDoc::GetChildFrame (void)
  {
  CChildFrame * pFrame = NULL;

  // find the child frame
  for(POSITION pos=GetFirstViewPosition();pos!=NULL;)
    {
    CView* pView = GetNextView(pos);

    if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
      {
      CSendView* pmyView = (CSendView*)pView;

      pFrame = pmyView->m_owner_frame;
      break;
      }	  // end of being a CSendView
    }
  
  return pFrame;

  } // end of CMUSHclientDoc::GetChildFrame



BOOL CMUSHclientDoc::Transparency(long Key, short Amount) 
{
  if (Amount < MWT_MIN_FACTOR)
    Amount = MWT_MIN_FACTOR;
  else if (Amount > MWT_MAX_FACTOR)
    Amount = MWT_MAX_FACTOR;

  return MakeWindowTransparent (App.m_pMainWnd->m_hWnd, Key, (unsigned char) Amount);
}  // end of CMUSHclientDoc::Transparency


void CMUSHclientDoc::SetWorldWindowStatus(short Parameter)
{
  EmitWindowStatus (Parameter);
}  // end of CMUSHclientDoc::SetWorldWindowStatus


// move a toolbar
long CMUSHclientDoc::SetToolBarPosition(short Which, BOOL Float, short Side, long Top, long Left) 
{
CMyToolBar * pToolBar = NULL;

  Frame.RecalcLayout(TRUE);

  switch (Which)
    {
    case 1: pToolBar = &Frame.m_wndToolBar; break;  // main toolbar
    case 2: pToolBar = &Frame.m_wndGameToolBar; break;  // world toolbar
    case 3: pToolBar = &Frame.m_wndActivityToolBar; break;  // activity toolbar
    case 4: pToolBar = (CMyToolBar *) &Frame.m_wndInfoBar; break;  // info toolbar
    default: return eBadParameter;
    } // end of switch

  if (!pToolBar || !pToolBar->m_hWnd)
      return eBadParameter;  // oops - no window

	CRect rectBar;
	pToolBar->GetWindowRect(&rectBar);

  if (Float)
    {
    DWORD dwStyle;
    CPoint point (Left, Top);
    Frame.ClientToScreen (&point);
    switch (Side)
      {
      case 1: dwStyle = CBRS_ALIGN_TOP ; break;
      case 3: dwStyle = CBRS_ALIGN_LEFT; break;
      default: return eBadParameter;
      }
    Frame.FloatControlBar (pToolBar, point, dwStyle);
    }
  else
    {
    UINT nDockBarID = AFX_IDW_DOCKBAR_TOP;
    CRect rect (Left, Top, Left + rectBar.right - rectBar.left, Top + rectBar.bottom - rectBar.top);
    Frame.ClientToScreen (rect);
    switch (Side)
      {
      case 0: break;   // zero is OK
      case 1: nDockBarID = AFX_IDW_DOCKBAR_TOP; break;
      case 2: nDockBarID = AFX_IDW_DOCKBAR_BOTTOM; break;
      case 3: nDockBarID = AFX_IDW_DOCKBAR_LEFT; break;
      case 4: nDockBarID = AFX_IDW_DOCKBAR_RIGHT; break;
      default: return eBadParameter;
      }
    Frame.DockControlBar (pToolBar, nDockBarID, rect);
    }

	return eOK;
}    // end of CMUSHclientDoc::SetToolBarPosition


// add a  new font from a file
long CMUSHclientDoc::AddFont(LPCTSTR PathName) 
{

  if (strlen (PathName) <= 0)
    return eBadParameter;  // need a string

  return AddSpecialFont (PathName);

}  // end of  CMUSHclientDoc::AddFont



/* modes: 

  0 = stretch to output view size 
  1 = stretch with aspect ratio

  -- next two will not be affected by command window size changes:

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


// set main window background image
long CMUSHclientDoc::SetBackgroundImage(LPCTSTR FileName, short Mode) 
{

  if (Mode < 0 || Mode > 13)
    return eBadParameter; 

  if ((HBITMAP) m_BackgroundBitmap)
    m_BackgroundBitmap.DeleteObject ();

  m_strBackgroundImageName.Empty ();

  UpdateAllViews (NULL);

  CString strFileName = FileName;

  strFileName.TrimLeft ();
  strFileName.TrimRight ();

  // no file name means get rid of image
  if (strFileName.IsEmpty ())
    return eOK;

  // have to be long enough to have x.bmp
  if (strFileName.GetLength () < 5)
    return eBadParameter;

  HBITMAP hBmp;

  // handle PNG files separately
  if (strFileName.Right (4).CompareNoCase (".png") == 0)
    {
    long result =  LoadPng (FileName, hBmp);

    if (result != eOK) 
      return result;

    }    // png file
  else
    {

    // must be .bmp or .png file
    if (strFileName.Right (4).CompareNoCase (".bmp") != 0)
      return eBadParameter;

    hBmp = (HBITMAP)::LoadImage(
                    NULL,
                    FileName,
                    IMAGE_BITMAP,
                    0,
                    0,
                    LR_LOADFROMFILE|LR_CREATEDIBSECTION
                    );
    }  // bmp file

   if (hBmp) 
     {
      m_BackgroundBitmap.Attach (hBmp);
      m_iBackgroundMode = Mode;
      m_strBackgroundImageName = FileName;
    	return eOK;
     }  // end of having a bitmap loaded


   if (GetLastError () == 2)
     return eFileNotFound;

   return eCouldNotOpenFile;
  
  } // end of CMUSHclientDoc::SetBackgroundImage

// set main window foreground image
long CMUSHclientDoc::SetForegroundImage(LPCTSTR FileName, short Mode) 
{
  if (Mode < 0 || Mode > 13)
    return eBadParameter;  

  if ((HBITMAP) m_ForegroundBitmap)
    m_ForegroundBitmap.DeleteObject ();

  m_strForegroundImageName.Empty ();

  UpdateAllViews (NULL);

  CString strFileName = FileName;

  strFileName.TrimLeft ();
  strFileName.TrimRight ();

  // no file name means get rid of image
  if (strFileName.IsEmpty ())
    return eOK;

  // have to be long enough to have x.bmp
  if (strFileName.GetLength () < 5)
    return eBadParameter;

  HBITMAP hBmp;

  // handle PNG files separately
  if (strFileName.Right (4).CompareNoCase (".png") == 0)
    {
    long result =  LoadPng (FileName, hBmp);

    if (result != eOK) 
      return result;

    }    // png file
  else
    {

    // must be .bmp or .png file
    if (strFileName.Right (4).CompareNoCase (".bmp") != 0)
      return eBadParameter;

    hBmp = (HBITMAP)::LoadImage(
                    NULL,
                    FileName,
                    IMAGE_BITMAP,
                    0,
                    0,
                    LR_LOADFROMFILE|LR_CREATEDIBSECTION
                    );
    }  // bmp file

   if (hBmp) 
     {
      m_ForegroundBitmap.Attach (hBmp);
      m_iForegroundMode = Mode;
      m_strForegroundImageName = FileName;
    	return eOK;
     }  // end of having a bitmap loaded


   if (GetLastError () == 2)
     return eFileNotFound;

   return eCouldNotOpenFile;
  
}   // end of CMUSHclientDoc::SetForegroundImage

// set main window background colour


void CMUSHclientDoc::Repaint() 
{
  UpdateAllViews (NULL);

  for(POSITION pos = GetFirstViewPosition(); pos != NULL; )
	  {
	  CView* pView = GetNextView(pos);
	  
	  if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
  	  {
		  CMUSHView* pmyView = (CMUSHView*)pView;
      pmyView->UpdateWindow ();
	    }	  // end of being a CMUSHView
    }   // end of loop through views

}    // end of CMUSHclientDoc::Repaint



long CMUSHclientDoc::TextRectangle(long Left, long Top, long Right, long Bottom, 
                                   long BorderOffset, 
                                   long BorderColour, long BorderWidth, 
                                   long OutsideFillColour, long OutsideFillStyle) 
{


  CBrush br;

  if (ValidateBrushStyle (OutsideFillStyle, BorderColour, OutsideFillColour, br) != eOK)
    return eBrushStyleNotValid;

  m_TextRectangle = CRect (Left, Top, Right, Bottom);

  m_TextRectangleBorderOffset =       BorderOffset;      
  m_TextRectangleBorderColour =       BorderColour;        
  m_TextRectangleBorderWidth =        BorderWidth;         
  m_TextRectangleOutsideFillColour =  OutsideFillColour;
  m_TextRectangleOutsideFillStyle =   OutsideFillStyle;

  // Port: the reserved text area just changed, so the GUI must recompute the
  // engine wrap column + re-send NAWS for the new visible width — otherwise the
  // MUD keeps wrapping to the full window width and the client re-wraps long
  // lines.  No-op on builds that don't wire it (headless / Windows).
  EmitTextRectChanged ();

  bool bFixedWindowWidth = false;

  // refresh views  - get scroll bars right

  for(POSITION pos=GetFirstViewPosition();pos!=NULL;)
	  {
	  CView* pView = GetNextView(pos);
	  
	  if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
  	  {
		  CMUSHView* pmyView = (CMUSHView*)pView;

		  pmyView->addedstuff();

      // now fix up auto-wrapping column if required
      if (!bFixedWindowWidth)
        {
        bFixedWindowWidth = true;
        pmyView->AutoWrapWindowWidth(this);
        }   // if not already done
      }	  // if CMUSHView
    }  // end for
       
  SendWindowSizes (m_nWrapColumn);  // notify of different window height
  Redraw ();
	return eOK;
}   // end of CMUSHclientDoc::TextRectangle
  

long CMUSHclientDoc::SetCursor(long Cursor)
{
#ifndef _WIN32
  // Port: set the output pane's wx cursor (the Win32 ::SetCursor + LoadCursor
  // path below is meaningless without an HCURSOR registry).  Validate the range
  // first so out-of-range values still report eBadParameter as on Windows.
  if (Cursor < -1 || Cursor > 12)
    return eBadParameter;
  if (g_wxhook_set_cursor)
    g_wxhook_set_cursor (this, (int) Cursor);
  return eOK;
#else
  switch (Cursor)
    {
    case -1:  ::SetCursor (NULL);                              break;    // no cursor
    case  0:  ::SetCursor (::LoadCursor (NULL, IDC_ARROW));    break;    // arrow
    case  1:  ::SetCursor (CStaticLink::g_hCursorLink);        break;    // hand
    case  2:  ::SetCursor (::LoadCursor (NULL, IDC_IBEAM));    break;    // I-beam
//    case  2:  ::SetCursor (App.g_hCursorIbeam);                break;    // I-beam
    case  3:  ::SetCursor (::LoadCursor (NULL, IDC_CROSS));    break;    // + (cross)
    case  4:  ::SetCursor (::LoadCursor (NULL, IDC_WAIT));     break;    // wait (hour-glass)
    case  5:  ::SetCursor (::LoadCursor (NULL, IDC_UPARROW));  break;    // up arrow
    case  6:  ::SetCursor (::LoadCursor (NULL, IDC_SIZENWSE)); break;    // arrow nw-se
    case  7:  ::SetCursor (::LoadCursor (NULL, IDC_SIZENESW)); break;    // arrow ne-sw
    case  8:  ::SetCursor (::LoadCursor (NULL, IDC_SIZEWE));   break;    // arrow e-w
    case  9:  ::SetCursor (::LoadCursor (NULL, IDC_SIZENS));   break;    // arrow n-s
    case 10:  ::SetCursor (::LoadCursor (NULL, IDC_SIZEALL));  break;    // arrow - all ways
    case 11:  ::SetCursor (::LoadCursor (NULL, IDC_NO));       break;    // (X) no, no, I won't do that, but ...
    case 12:  ::SetCursor (::LoadCursor (NULL, IDC_HELP));     break;    // help  (? symbol)
    default:  return eBadParameter;
    } // end of switch

  return eOK;
#endif // _WIN32
}   // end of CMUSHclientDoc::SetCursor


void CMUSHclientDoc::FlashIcon() 
{
Frame.FlashWindow (TRUE);
}    // end of CMUSHclientDoc::FlashIcon


long CMUSHclientDoc::SetScroll(long Position, BOOL Visible)
{
#ifndef _WIN32
  // Position is a device-pixel Y (-1 = bottom, -2 = no change), matching the
  // Windows arm below and the seam contract in pal/win_worldview.cpp.
  //
  // Visible must be RECORDED even though the port's pane always shows its
  // scrollbar: m_bScrollBarWanted has no other writer besides its
  // initialisation (doc_construct.cpp:136), and GetInfo(120) reports it
  // (methods_info.cpp:606) -- so without this the port answered a hard-coded
  // "true" no matter what a script had set.  Windows also calls
  // EnableScrollBarCtrl here; the port does not hide the bar (a wxScrolledWindow
  // that can scroll shows one), which is a cosmetic difference, not a lie about
  // the setting.
  m_bScrollBarWanted = Visible;

  if (Position != -2)
    EmitScroll(Position);
  return eOK;
#else

CPoint pt (0, 0);
int lastline = GetLastLine ();
bool will_change_visibility = (Visible != m_bScrollBarWanted);

  for(POSITION pos = GetFirstViewPosition(); pos != NULL; )
	  {
	  CView* pView = GetNextView(pos);

	  if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
  	  {
		  CMUSHView* pmyView = (CMUSHView*)pView;

      CPoint cur_pt = pmyView->GetScrollPosition();
      bool will_scroll = (Position != cur_pt.y) && (Position != -2); // if -2, do not change position
      int highest = (lastline * m_FontHeight) - pmyView->GetOutputWindowHeight ();
      will_scroll = will_scroll && ((Position != -1) || (cur_pt.y != highest));

      if (will_change_visibility) {
        m_bScrollBarWanted = Visible;
        pmyView->EnableScrollBarCtrl (SB_VERT, Visible);
      }

      if (will_scroll) {
        // -1 goes to the end
        if (Position == -1)
          pt.y = highest;
        else
          pt.y = Position;

        if (pt.y < 0)
          pt.y = 0;
        if (pt.y > highest)
          pt.y = highest;

        pmyView->ScrollToPosition (pt, false);
      }

      if (will_scroll || will_change_visibility)
        pmyView->Invalidate ();

	    }	  // end of being a CMUSHView
    }   // end of loop through views

	return eOK;
#endif
}   // end of CMUSHclientDoc::SetScroll


void windowPositionHelper (CString & str, const RECT & r)
  {
  str.Format ("%ld,%ld,%ld,%ld", 
              r.left, 
              r.top, 
              r.right - r.left,  // Width
              r.bottom - r.top);  // Height
  } // end of windowPositionHelper



// CMUSHclientDoc::Bookmark moved to doc_shared.cpp — it is pure line-list work
// plus the view notification, and the notification is now the pal::IWorldView
// seam (the CMUSHView walk that used to be here lives in the Windows adapter,
// pal/win_worldview.cpp setBookmark).  Sharing the body is what lets the
// headless tests reach it: this TU is GUI-only, so mushcore used to link an
// inert no-op stub of Bookmark instead.


void CMUSHclientDoc::SetTitle(LPCTSTR Title)
{
	m_strWindowTitle = Title;

  // propagate to the world's IWorldView (WV-3: the wx MDI child frame, or
  // the native adapter's FixupTitle view loop — pal/win_worldview.cpp)
  EmitTitle(static_cast<const char*>(m_strWindowTitle));

}   // end of CMUSHclientDoc::SetTitle


