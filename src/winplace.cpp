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
   return TRUE;
}

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
  // What's stored is in 96-dpi pixels (below), so the defaults have to be too.
  const int iDpi = Dpi_ForSystem ();
  rc.left   = ::MulDiv (rc.left, 96, iDpi);
  rc.right  = ::MulDiv (rc.right, 96, iDpi);
  rc.top    = ::MulDiv (rc.top, 96, iDpi);
  rc.bottom = ::MulDiv (rc.bottom, 96, iDpi);
#endif

  rc.left   = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.left"),   rc.left);
  rc.right  = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.right"),  rc.right);
  rc.top    = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.top"),    rc.top);
  rc.bottom = App.db_get_int ("worlds", (LPCTSTR) CFormat ("%s:%s", lpKeyName, "wp.bottom"), rc.bottom);

#ifdef _WIN32
  // Kept in 96-dpi pixels, which is what a dpi-unaware 5.x or 6.0 saved too.  Asked for
  // on another monitor, it gets WM_DPICHANGED and comes right.
  rc.left   = ::MulDiv (rc.left, iDpi, 96);
  rc.right  = ::MulDiv (rc.right, iDpi, 96);
  rc.top    = ::MulDiv (rc.top, iDpi, 96);
  rc.bottom = ::MulDiv (rc.bottom, iDpi, 96);
#endif

}

////////////////
// Save window placement in app profile
void CWindowPlacement::Save(LPCSTR lpKeyName, CWnd* pWnd)
{
   pWnd->GetWindowPlacement(this);

   // don't save if minimized, it doesn't work properly
   if (showCmd != SW_SHOWMINIMIZED)
     #ifdef _WIN32
   {
   const int iDpi = Dpi_ForWindow (pWnd->m_hWnd);
   rcNormalPosition.left   = ::MulDiv (rcNormalPosition.left, 96, iDpi);
   rcNormalPosition.right  = ::MulDiv (rcNormalPosition.right, 96, iDpi);
   rcNormalPosition.top    = ::MulDiv (rcNormalPosition.top, 96, iDpi);
   rcNormalPosition.bottom = ::MulDiv (rcNormalPosition.bottom, 96, iDpi);
   }
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
