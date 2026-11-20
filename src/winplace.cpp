////////////////////////////////////////////////////////////////
// CWindowPlacement 1996 Microsoft Systems Journal.
// If this code works, it was written by Paul DiLascia.
// If not, I don't know who wrote it.
#include "stdafx.h"
#include "dpi.h"
#include "MUSHclient.h"
#include "mainfrm.h"
#include "winplace.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

// Version 10 of the MFC framework removed functions we need.
// Which means we need to force generation of stubs in that case.
// See: http://cppdepend.wordpress.com/2010/08/29/visual-c-2010-what�s-new-for-mfc-library/
#if _MFC_VER >= 0x0A00
#define COMPILE_MULTIMON_STUBS
#endif

#include <MULTIMON.H>

CWindowPlacement::CWindowPlacement()
{
   // Zeroed: GetProfileWP's defaults are these fields, so for a key never saved
   // (a world's first open) Restore used to apply whatever was on the stack, and
   // a world could open far off-screen.  An empty rcNormalPosition fails
   // Restore's intersect test instead, and the window keeps its default place.
   memset (static_cast <WINDOWPLACEMENT *> (this), 0, sizeof (WINDOWPLACEMENT));
   // Note: "length" is inherited from WINDOWPLACEMENT
   length = sizeof(WINDOWPLACEMENT);
   m_iSavedDpi = 0;
}

CWindowPlacement::~CWindowPlacement()
{
}


//////////////////
// Restore window placement from profile key
BOOL CWindowPlacement::Restore(LPCSTR lpKeyName, 
                               CWnd* pWnd,
                               const bool bDesktop)
{
#ifndef _WIN32
   // On macOS / Linux, the main window geometry is managed exclusively by
   // wxConfig (preferences.cfg in hook_load_mdi_frame / SaveWindowGeometryTo).
   // CWindowPlacement::Restore ("Main window") would overwrite the wxConfig-restored
   // geometry with uninitialized/default placement values before the event loop runs.
   if (lpKeyName && strcmp(lpKeyName, "Main window") == 0)
      return FALSE;
#endif
   GetProfileWP(lpKeyName);

#ifdef _WIN32
   // scaled for where it's going (ScaleSaved)
   {
     const bool bInsideFrame = pWnd->m_hWnd && (pWnd->GetStyle () & WS_CHILD) != 0;
     HWND hwndParent = bInsideFrame ? ::GetParent (pWnd->m_hWnd) : NULL;
     int iNowDpi = 0;   // (no window: as saved)
     if (pWnd->m_hWnd)
       iNowDpi = bInsideFrame ? Dpi_ActualForWindow (hwndParent ? hwndParent : pWnd->m_hWnd)
                              : Dpi_ActualForRect (rcNormalPosition);
     ScaleSaved (rcNormalPosition, m_iSavedDpi, iNowDpi, Dpi_ActualForWindow (NULL), bInsideFrame);
   }
#endif

   // Only restore if window intersets the screen.
   //
   CRect rcTemp, rcScreen;

   if (bDesktop)
     {
      HMONITOR hMonitor;
      MONITORINFO mi;

      //
      // get the nearest monitor to the "normal" rectangle
      //
      hMonitor = MonitorFromRect(&rcNormalPosition, MONITOR_DEFAULTTONEAREST);

      //
      // get the work area rect.
      //
      mi.cbSize = sizeof(mi);
      GetMonitorInfo(hMonitor, &mi);

      rcScreen = mi.rcWork;
//      Frame.GetDesktopWindow ()->GetClientRect (rcScreen);  // prior to version 3.85
     }
   else
     {
#ifdef _WIN32
     ::GetClientRect (((CMDIFrameWnd *)&Frame)->m_hWndMDIClient, &rcScreen);
#else
     // Port only.  There is no MDI client window here (the active child IS the
     // visible window), so m_hWndMDIClient is always NULL, ::GetClientRect left
     // rcScreen empty and the intersect test below could never pass: EVERY
     // saved dialog and world position was silently discarded and every window
     // opened at its default place.  The rectangle being tested is in screen
     // coordinates on this platform (the placement hook reports
     // wxWindow::GetRect), so the screen is what it has to be checked against.
     rcScreen.SetRect (0, 0,
                       GetSystemMetrics (SM_CXSCREEN),
                       GetSystemMetrics (SM_CYSCREEN));
#endif
     rcScreen.InflateRect (-10, -10); // margin for error
     }


   // see if rectangle will fit inside parent

   if (!::IntersectRect(&rcTemp, &rcNormalPosition, &rcScreen))
      return FALSE;

   pWnd->SetWindowPlacement(this);  // set placement

#ifdef _WIN32
   // A window put on a monitor at another scale from the one it was made on is told
   // so (WM_DPICHANGED) and resizes itself by the ratio, which would scale a size
   // already right for that monitor a second time.  Now it is on the monitor, put it
   // where it belongs once more: the second time the scale doesn't change.
   if (pWnd->m_hWnd && !(pWnd->GetStyle () & WS_CHILD) && m_iSavedDpi > 0)
     {
     WINDOWPLACEMENT wpNow;
     wpNow.length = sizeof wpNow;
     pWnd->GetWindowPlacement (&wpNow);
     if (!::EqualRect (&wpNow.rcNormalPosition, &rcNormalPosition))
       pWnd->SetWindowPlacement (this);
     }
#endif
   return TRUE;
}

#ifdef _WIN32
// Where a saved rectangle goes now (see GetProfileWP).  Saved by this version, it is
// real pixels at iSavedDpi; a window inside the frame (a world, the activity list) is
// placed in the frame's client area, which grows with the scale, so all of it scales
// by iNowDpi / iSavedDpi.  A window on the desktop is placed on the screen, where a
// position doesn't scale with anything (the monitors are where they are), so it stays
// where it was and only its size scales.  Saved by 5.x or an older 6.00 (iSavedDpi 0)
// it is in 96-dpi pixels, which is what a dpi-unaware client saw on its primary
// monitor: all of it scales by the system dpi, as before.
void CWindowPlacement::ScaleSaved (RECT & rc, const int iSavedDpi, const int iNowDpi,
                                   const int iSystemDpi, const bool bInsideFrame)
  {
  if (iSavedDpi <= 0)
    {
    rc.left   = ::MulDiv (rc.left, iSystemDpi, 96);
    rc.right  = ::MulDiv (rc.right, iSystemDpi, 96);
    rc.top    = ::MulDiv (rc.top, iSystemDpi, 96);
    rc.bottom = ::MulDiv (rc.bottom, iSystemDpi, 96);
    return;
    }

  if (iNowDpi == iSavedDpi || iNowDpi <= 0)
    return;

  const int iWidth  = ::MulDiv (rc.right - rc.left, iNowDpi, iSavedDpi);
  const int iHeight = ::MulDiv (rc.bottom - rc.top, iNowDpi, iSavedDpi);
  if (bInsideFrame)
    {
    rc.left = ::MulDiv (rc.left, iNowDpi, iSavedDpi);
    rc.top  = ::MulDiv (rc.top, iNowDpi, iSavedDpi);
    }
  rc.right  = rc.left + iWidth;
  rc.bottom = rc.top + iHeight;
  }
#endif

//////////////////
// Get window placement from profile.
void CWindowPlacement::GetProfileWP(LPCSTR lpKeyName)
{
  showCmd         = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.showCmd"), showCmd);
  flags           = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.flags"), flags);

  ptMinPosition.x = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.ptMinPosition.x"), ptMinPosition.x);
  ptMinPosition.y = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.ptMinPosition.y"), ptMinPosition.y);
  ptMaxPosition.x = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.ptMaxPosition.x"), ptMaxPosition.x);
  ptMaxPosition.y = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.ptMaxPosition.y"), ptMaxPosition.y);


  RECT& rc = rcNormalPosition;  // because I hate typing

#ifdef _WIN32
  // Saved by this version: real pixels, and the dpi of the window when it was saved
  // (Save).  Saved by 5.x, or a 6.00 before that: 96-dpi pixels, and no dpi.  Either
  // way Restore scales it for where it's going (ScaleSaved); this only reads it.
  m_iSavedDpi = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.dpi"), 0);

  // The defaults (the window's own placement, for a key never saved) are real pixels:
  // for the old form, in which ScaleSaved will scale them up again, they have to be
  // 96-dpi ones.
  const int iSystemDpi = Dpi_ActualForWindow (NULL);
  if (m_iSavedDpi <= 0)
    {
    rc.left   = ::MulDiv (rc.left, 96, iSystemDpi);
    rc.right  = ::MulDiv (rc.right, 96, iSystemDpi);
    rc.top    = ::MulDiv (rc.top, 96, iSystemDpi);
    rc.bottom = ::MulDiv (rc.bottom, 96, iSystemDpi);
    }
#endif

  rc.left   = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.left"),   rc.left);
  rc.right  = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.right"),  rc.right);
  rc.top    = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.top"),    rc.top);
  rc.bottom = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.bottom"), rc.bottom);

}

////////////////
// Save window placement in app profile
void CWindowPlacement::Save(LPCSTR lpKeyName, CWnd* pWnd)
{
   pWnd->GetWindowPlacement(this);

   // don't save if minimized, it doesn't work properly
   if (showCmd == SW_SHOWMINIMIZED)
     return;

#ifdef _WIN32
   // Real pixels, as Windows has them, and the dpi they are at: Restore scales from
   // that to wherever the window goes (ScaleSaved).  The real dpi, not the one
   // world.Debug ("dpi N") pretends, which doesn't move or resize any window.
   m_iSavedDpi = Dpi_ActualForWindow (pWnd->m_hWnd);
#endif
   WriteProfileWP(lpKeyName);
}

//////////////////
// Write window placement to app profile
void CWindowPlacement::WriteProfileWP(LPCSTR lpKeyName)
{
   int rc = App.db_execute ("BEGIN TRANSACTION", true);

   // give it up if we can't get a transaction going - it's no big deal
   if (rc == SQLITE_OK || rc == SQLITE_DONE)
    {

    App.db_write_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.showCmd"), showCmd);
    App.db_write_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.flags"), flags);

    App.db_write_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.ptMinPosition.x"), ptMinPosition.x);
    App.db_write_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.ptMinPosition.y"), ptMinPosition.y);
    App.db_write_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.ptMaxPosition.x"), ptMaxPosition.x);
    App.db_write_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.ptMaxPosition.y"), ptMaxPosition.y);

    App.db_write_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.left"),   rcNormalPosition.left);
    App.db_write_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.right"),  rcNormalPosition.right);
    App.db_write_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.top"),    rcNormalPosition.top);
    App.db_write_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.bottom"), rcNormalPosition.bottom);
#ifdef _WIN32
    // what the rectangle's pixels are (GetProfileWP); 5.x ignores it
    if (m_iSavedDpi > 0)
      App.db_write_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.dpi"), m_iSavedDpi);
#endif

    App.db_execute ("COMMIT", true);

    }

}


//////////////////
// Write window placement to archive
// WARNING: archiving functions are untested.
CArchive& operator<<(CArchive& ar, const CWindowPlacement& wp)
{
   ar << wp.length;
   ar << wp.flags;
   ar << wp.showCmd;
   ar << wp.ptMinPosition;
   ar << wp.ptMaxPosition;
   ar << wp.rcNormalPosition;
   return ar;
}

//////////////////
// Read window placement from archive
// WARNING: archiving functions are untested.
CArchive& operator>>(CArchive& ar, CWindowPlacement& wp)
{
   ar >> wp.length;
   ar >> wp.flags;
   ar >> wp.showCmd;
   ar >> wp.ptMinPosition;
   ar >> wp.ptMaxPosition;
   ar >> wp.rcNormalPosition;
   return ar;
}
