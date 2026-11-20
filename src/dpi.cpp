// dpi.cpp : see dpi.h

#include "stdafx.h"
#include "dpi.h"

#ifdef _WIN32

namespace {

typedef UINT (WINAPI * GetDpiForWindowFn) (HWND);
typedef UINT (WINAPI * GetDpiForSystemFn) ();
typedef int  (WINAPI * GetSystemMetricsForDpiFn) (int, UINT);
typedef HRESULT (WINAPI * GetDpiForMonitorFn) (HMONITOR, int, UINT *, UINT *);

struct DpiApis
  {
  GetDpiForWindowFn         pGetDpiForWindow;
  GetDpiForSystemFn         pGetDpiForSystem;
  GetSystemMetricsForDpiFn  pGetSystemMetricsForDpi;
  GetDpiForMonitorFn        pGetDpiForMonitor;
  mutable int               iOverride;

  DpiApis ()
    {
    HMODULE hUser = ::GetModuleHandleA ("user32.dll");
    pGetDpiForWindow = hUser ? (GetDpiForWindowFn) ::GetProcAddress (hUser, "GetDpiForWindow") : NULL;
    pGetDpiForSystem = hUser ? (GetDpiForSystemFn) ::GetProcAddress (hUser, "GetDpiForSystem") : NULL;
    pGetSystemMetricsForDpi = hUser ? (GetSystemMetricsForDpiFn) ::GetProcAddress (hUser, "GetSystemMetricsForDpi") : NULL;
    HMODULE hShcore = ::LoadLibraryA ("shcore.dll");   // Windows 8.1 on; kept for the process
    pGetDpiForMonitor = hShcore ? (GetDpiForMonitorFn) ::GetProcAddress (hShcore, "GetDpiForMonitor") : NULL;

    iOverride = 0;
    char sValue [16];
    if (::GetEnvironmentVariableA ("MUSHCLIENT_TEST_DPI", sValue, sizeof sValue))
      {
      iOverride = atoi (sValue);
      if (iOverride < 48 || iOverride > 960)
        iOverride = 0;
      }
    }
  };

const DpiApis & Apis ()
  {
  static const DpiApis apis;
  return apis;
  }

}  // end of anonymous namespace

int Dpi_ForSystem ()
  {
  const DpiApis & apis = Apis ();
  if (apis.iOverride)
    return apis.iOverride;
  if (apis.pGetDpiForSystem)
    return (int) apis.pGetDpiForSystem ();

  HDC hdc = ::GetDC (NULL);
  const int iDpi = hdc ? ::GetDeviceCaps (hdc, LOGPIXELSY) : 96;
  if (hdc)
    ::ReleaseDC (NULL, hdc);
  return iDpi;
  }

int Dpi_ForWindow (HWND hwnd)
  {
  const DpiApis & apis = Apis ();
  if (apis.iOverride)
    return apis.iOverride;
  if (hwnd && apis.pGetDpiForWindow)
    {
    const UINT iDpi = apis.pGetDpiForWindow (hwnd);
    if (iDpi)
      return (int) iDpi;
    }
  return Dpi_ForSystem ();
  }

int Dpi_ActualForWindow (HWND hwnd)
  {
  const DpiApis & apis = Apis ();
  if (hwnd && apis.pGetDpiForWindow)
    {
    const UINT iDpi = apis.pGetDpiForWindow (hwnd);
    if (iDpi)
      return (int) iDpi;
    }
  HDC hdc = ::GetDC (NULL);
  const int iDpi = hdc ? ::GetDeviceCaps (hdc, LOGPIXELSY) : 96;
  if (hdc)
    ::ReleaseDC (NULL, hdc);
  return iDpi;
  }

int Dpi_ActualForRect (const RECT & rect)
  {
  const DpiApis & apis = Apis ();
  UINT x = 0, y = 0;
  if (apis.pGetDpiForMonitor &&
      apis.pGetDpiForMonitor (::MonitorFromRect (&rect, MONITOR_DEFAULTTONEAREST), 0 /* MDT_EFFECTIVE_DPI */, &x, &y) == S_OK &&
      y)
    return (int) y;
  return Dpi_ActualForWindow (NULL);
  }

void Dpi_SetOverride (const int iDpi)
  {
  // 0 (or less) is "ask the system again"; anything else within what the environment
  // variable accepts, so that world.Debug ("dpi 1") can't make every size zero
  Apis ().iOverride = iDpi <= 0 ? 0 : iDpi < 48 ? 48 : iDpi > 960 ? 960 : iDpi;
  }

bool Dpi_Overridden ()
  {
  return Apis ().iOverride != 0;
  }

int Dpi_LogicalSystemMetric (int iIndex)
  {
  // the size of the screen doesn't depend on a dpi, as far as the API is concerned
  switch (iIndex)
    {
    case SM_CXSCREEN: case SM_CYSCREEN: case SM_CXFULLSCREEN: case SM_CYFULLSCREEN:
    case SM_CXVIRTUALSCREEN: case SM_CYVIRTUALSCREEN:
    case SM_XVIRTUALSCREEN: case SM_YVIRTUALSCREEN:
    case SM_CXMAXIMIZED: case SM_CYMAXIMIZED:
    case SM_CXMAXTRACK: case SM_CYMAXTRACK:
      return ::MulDiv (::GetSystemMetrics (iIndex), 96, Dpi_ForSystem ());
    }

  const DpiApis & apis = Apis ();
  if (apis.pGetSystemMetricsForDpi)
    return apis.pGetSystemMetricsForDpi (iIndex, 96);
  return ::GetSystemMetrics (iIndex);
  }

#endif  // _WIN32
