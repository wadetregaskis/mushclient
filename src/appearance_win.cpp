// appearance_win.cpp : dark chrome on Windows (see appearance.h).
//
// Windows has no supported way to make a Win32 program dark.  The parts:
//
//  - The title bar: DWMWA_USE_IMMERSIVE_DARK_MODE, documented.
//
//  - Popup menus and scroll bars, and the dark variants of the common controls:
//    uxtheme's unnamed exports (by ordinal) and "DarkMode_*" theme names.  Those are
//    undocumented, which is why everything here is behind a build check and a
//    GetProcAddress, and quietly stays light if it does not find what it wants.
//
//  - Everything that has no dark variant at all (tabs, the status bar, the menu bar,
//    check boxes' text, group boxes): painted here.
//
// One thread-local CBT hook sees every window this thread creates and subclasses the
// ones we know about, so the 115 dialog templates and 118 dialog classes do not each
// need to know about any of this.  Common dialogs (file, colour, font) are the OS's:
// they are recognised by the module their template came from and left alone.
//
// While the client is light, the hook does nothing and no window is touched: a light
// client is the client it always was.  Subclasses installed during a dark spell stay
// installed, and just pass everything along when light.

#include "stdafx.h"
#include "appearance.h"

#include <commctrl.h>
#include <vector>

#pragma comment (lib, "comctl32.lib")

#ifndef TBCDRF_USECDCOLORS
#define TBCDRF_USECDCOLORS 0x00800000
#endif

namespace {

// ---------------------------------------------------------------------------
// The palette

const COLORREF kFace        = RGB (32, 32, 32);
const COLORREF kControl     = RGB (43, 43, 43);
const COLORREF kHot         = RGB (62, 62, 62);
const COLORREF kPressed     = RGB (80, 80, 80);
const COLORREF kEdge        = RGB (85, 85, 85);
const COLORREF kText        = RGB (240, 240, 240);
const COLORREF kDisabled    = RGB (150, 150, 150);    // 5.5:1 on kFace
const COLORREF kLink        = RGB (120, 185, 255);

// ---------------------------------------------------------------------------
// What we found on this system

enum { eAppModeDefault = 0, eAppModeAllowDark = 1, eAppModeForceDark = 2, eAppModeForceLight = 3 };

typedef int  (WINAPI * SetPreferredAppModeFn) (int);            // ordinal 135, 1903 on
typedef BOOL (WINAPI * AllowDarkModeForAppFn) (BOOL);           // ordinal 135, 1809
typedef BOOL (WINAPI * AllowDarkModeForWindowFn) (HWND, BOOL);  // ordinal 133
typedef void (WINAPI * RefreshImmersiveColorPolicyStateFn) ();  // ordinal 104
typedef void (WINAPI * FlushMenuThemesFn) ();                   // ordinal 136

typedef HANDLE HTHEME_;
typedef HRESULT (WINAPI * SetWindowThemeFn) (HWND, LPCWSTR, LPCWSTR);
typedef HTHEME_ (WINAPI * OpenThemeDataFn) (HWND, LPCWSTR);
typedef HRESULT (WINAPI * CloseThemeDataFn) (HTHEME_);
typedef HRESULT (WINAPI * DrawThemeBackgroundFn) (HTHEME_, HDC, int, int, const RECT *, const RECT *);
typedef HRESULT (WINAPI * GetThemePartSizeFn) (HTHEME_, HDC, int, int, const RECT *, int, SIZE *);
typedef HRESULT (WINAPI * EnableThemeDialogTextureFn) (HWND, DWORD);
typedef HRESULT (WINAPI * DwmSetWindowAttributeFn) (HWND, DWORD, LPCVOID, DWORD);

SetPreferredAppModeFn               pSetPreferredAppMode = NULL;
AllowDarkModeForAppFn               pAllowDarkModeForApp = NULL;
AllowDarkModeForWindowFn            pAllowDarkModeForWindow = NULL;
RefreshImmersiveColorPolicyStateFn  pRefreshImmersiveColorPolicyState = NULL;
FlushMenuThemesFn                   pFlushMenuThemes = NULL;
SetWindowThemeFn                    pSetWindowTheme = NULL;
OpenThemeDataFn                     pOpenThemeData = NULL;
CloseThemeDataFn                    pCloseThemeData = NULL;
DrawThemeBackgroundFn               pDrawThemeBackground = NULL;
GetThemePartSizeFn                  pGetThemePartSize = NULL;
EnableThemeDialogTextureFn          pEnableThemeDialogTexture = NULL;
DwmSetWindowAttributeFn             pDwmSetWindowAttribute = NULL;

unsigned long iBuild = 0;
bool bSupported = false;
int  iPreference = APPEARANCE_SYSTEM;
bool bDark = false;
HHOOK hHook = NULL;
DWORD iHookThread = 0;
void (* pChangeHandler) () = NULL;

HBRUSH hbrFace = NULL;
HBRUSH hbrControl = NULL;
HBRUSH hbrHot = NULL;
HBRUSH hbrPressed = NULL;
HBRUSH hbrEdge = NULL;

HMODULE hUser32 = NULL;

void LoadBrushes ()
  {
  if (hbrFace)
    return;
  hbrFace = ::CreateSolidBrush (kFace);
  hbrControl = ::CreateSolidBrush (kControl);
  hbrHot = ::CreateSolidBrush (kHot);
  hbrPressed = ::CreateSolidBrush (kPressed);
  hbrEdge = ::CreateSolidBrush (kEdge);
  }

unsigned long WindowsBuild ()
  {
  // GetVersionEx answers what the manifest declares; RtlGetVersion answers the truth.
  typedef LONG (WINAPI * RtlGetVersionFn) (OSVERSIONINFOW *);
  if (HMODULE hNtdll = ::GetModuleHandleA ("ntdll.dll"))
    if (RtlGetVersionFn pRtlGetVersion = (RtlGetVersionFn) ::GetProcAddress (hNtdll, "RtlGetVersion"))
      {
      OSVERSIONINFOW info;
      ZeroMemory (&info, sizeof info);
      info.dwOSVersionInfoSize = sizeof info;
      if (pRtlGetVersion (&info) == 0 && info.dwMajorVersion >= 10)
        return info.dwBuildNumber;
      }
  return 0;
  }

void FindApis ()
  {
  iBuild = WindowsBuild ();

  HMODULE hTheme = ::LoadLibraryA ("uxtheme.dll");
  if (hTheme)
    {
    pSetWindowTheme = (SetWindowThemeFn) ::GetProcAddress (hTheme, "SetWindowTheme");
    pOpenThemeData = (OpenThemeDataFn) ::GetProcAddress (hTheme, "OpenThemeData");
    pCloseThemeData = (CloseThemeDataFn) ::GetProcAddress (hTheme, "CloseThemeData");
    pDrawThemeBackground = (DrawThemeBackgroundFn) ::GetProcAddress (hTheme, "DrawThemeBackground");
    pGetThemePartSize = (GetThemePartSizeFn) ::GetProcAddress (hTheme, "GetThemePartSize");
    pEnableThemeDialogTexture = (EnableThemeDialogTextureFn) ::GetProcAddress (hTheme, "EnableThemeDialogTexture");

    if (Appearance_WindowsBuildSupportsDark (iBuild))
      {
      if (iBuild >= 18362)
        pSetPreferredAppMode = (SetPreferredAppModeFn) ::GetProcAddress (hTheme, MAKEINTRESOURCEA (135));
      else
        pAllowDarkModeForApp = (AllowDarkModeForAppFn) ::GetProcAddress (hTheme, MAKEINTRESOURCEA (135));
      pAllowDarkModeForWindow = (AllowDarkModeForWindowFn) ::GetProcAddress (hTheme, MAKEINTRESOURCEA (133));
      pRefreshImmersiveColorPolicyState = (RefreshImmersiveColorPolicyStateFn) ::GetProcAddress (hTheme, MAKEINTRESOURCEA (104));
      pFlushMenuThemes = (FlushMenuThemesFn) ::GetProcAddress (hTheme, MAKEINTRESOURCEA (136));
      }
    }

  if (HMODULE hDwm = ::LoadLibraryA ("dwmapi.dll"))
    pDwmSetWindowAttribute = (DwmSetWindowAttributeFn) ::GetProcAddress (hDwm, "DwmSetWindowAttribute");

  hUser32 = ::GetModuleHandleA ("user32.dll");

  // Without all of these the result would be half dark, which is worse than light.
  bSupported = Appearance_WindowsBuildSupportsDark (iBuild)
            && (pSetPreferredAppMode || pAllowDarkModeForApp)
            && pAllowDarkModeForWindow
            && pSetWindowTheme && pOpenThemeData && pCloseThemeData
            && pDrawThemeBackground && pGetThemePartSize
            && pDwmSetWindowAttribute;
  }

// ---------------------------------------------------------------------------
// What the system says

bool SystemIsDark ()
  {
  // There is no way to test following the system short of changing the system.
  // MUSHCLIENT_TEST_APPS_USE_LIGHT_FILE names a file holding what the registry
  // value would hold, so that the live-change path can be driven for real.
  char sTestFile [MAX_PATH];
  if (::GetEnvironmentVariableA ("MUSHCLIENT_TEST_APPS_USE_LIGHT_FILE", sTestFile, sizeof sTestFile))
    {
    FILE * f = fopen (sTestFile, "r");
    if (f)
      {
      const int c = fgetc (f);
      fclose (f);
      return c == '0';
      }
    }

  // Windows' high contrast themes have their own colours, and they matter to those
  // who chose them.
  HIGHCONTRASTA hc;
  ZeroMemory (&hc, sizeof hc);
  hc.cbSize = sizeof hc;
  if (::SystemParametersInfoA (SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON))
    return false;

  DWORD iValue = 1;
  DWORD iSize = sizeof iValue;
  HKEY hKey;
  if (::RegOpenKeyExA (HKEY_CURRENT_USER,
                       "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                       0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
    if (::RegQueryValueExA (hKey, "AppsUseLightTheme", NULL, NULL, (LPBYTE) &iValue, &iSize) != ERROR_SUCCESS)
      iValue = 1;
    ::RegCloseKey (hKey);
    }
  return iValue == 0;
  }

// ---------------------------------------------------------------------------
// Small drawing helpers

// Double buffering for what we paint ourselves.
class CDarkPaint
  {
  public:
    CDarkPaint (HWND hwnd) : m_hwnd (hwnd)
      {
      m_hdcScreen = ::BeginPaint (hwnd, &m_ps);
      ::GetClientRect (hwnd, &m_rect);
      m_hdc = ::CreateCompatibleDC (m_hdcScreen);
      m_hbm = ::CreateCompatibleBitmap (m_hdcScreen, m_rect.right, m_rect.bottom);
      m_hbmOld = (HBITMAP) ::SelectObject (m_hdc, m_hbm);
      }
    ~CDarkPaint ()
      {
      ::BitBlt (m_hdcScreen, 0, 0, m_rect.right, m_rect.bottom, m_hdc, 0, 0, SRCCOPY);
      ::SelectObject (m_hdc, m_hbmOld);
      ::DeleteObject (m_hbm);
      ::DeleteDC (m_hdc);
      ::EndPaint (m_hwnd, &m_ps);
      }
    HDC hdc () const { return m_hdc; }
    const RECT & rect () const { return m_rect; }
  private:
    HWND m_hwnd;
    PAINTSTRUCT m_ps;
    HDC m_hdcScreen, m_hdc;
    HBITMAP m_hbm, m_hbmOld;
    RECT m_rect;
  };

// Fills whatever the non-client area is (a client edge, a control bar's border).
void PaintNonClient (HWND hwnd, HBRUSH hbr)
  {
  HDC hdc = ::GetWindowDC (hwnd);
  if (!hdc)
    return;
  RECT rcWindow, rcClient;
  ::GetWindowRect (hwnd, &rcWindow);
  ::GetClientRect (hwnd, &rcClient);
  ::MapWindowPoints (hwnd, NULL, (POINT *) &rcClient, 2);
  ::OffsetRect (&rcClient, -rcWindow.left, -rcWindow.top);
  ::OffsetRect (&rcWindow, -rcWindow.left, -rcWindow.top);
  ::ExcludeClipRect (hdc, rcClient.left, rcClient.top, rcClient.right, rcClient.bottom);
  ::FillRect (hdc, &rcWindow, hbr);
  ::ReleaseDC (hwnd, hdc);
  }

// A client edge is two pixels of bevel in the light palette, which on a dark window
// is a glaring white rectangle.  Scroll bars are in the non-client area too, so this
// has to be done after the default painting and only over the edge itself.
void PaintEdge (HWND hwnd)
  {
  const DWORD iExStyle = ::GetWindowLong (hwnd, GWL_EXSTYLE);
  int iEdge = 0;
  if (iExStyle & WS_EX_CLIENTEDGE)
    iEdge = ::GetSystemMetrics (SM_CXEDGE);
  else if (iExStyle & (WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME))
    iEdge = ::GetSystemMetrics (SM_CXBORDER);
  else if ((::GetWindowLong (hwnd, GWL_STYLE) & WS_CAPTION) == WS_BORDER)
    iEdge = ::GetSystemMetrics (SM_CXBORDER);
  if (!iEdge)
    return;

  HDC hdc = ::GetWindowDC (hwnd);
  if (!hdc)
    return;
  RECT rc;
  ::GetWindowRect (hwnd, &rc);
  ::OffsetRect (&rc, -rc.left, -rc.top);
  ::FrameRect (hdc, &rc, hbrEdge);
  for (int i = 1; i < iEdge; i++)
    {
    ::InflateRect (&rc, -1, -1);
    ::FrameRect (hdc, &rc, hbrFace);
    }
  ::ReleaseDC (hwnd, hdc);
  }

UINT PrefixFlag (HWND hwnd)
  {
  return (::SendMessage (hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0;
  }

// ---------------------------------------------------------------------------
// Check boxes, radio buttons and group boxes.
//
// A themed check box paints its text in the theme's colour and takes no notice of
// SetTextColor, which on a dark theme comes out black on near-black.  So their
// painting is taken over.  The glyphs still come from the dark theme.

enum { BP_RADIOBUTTON_ = 2, BP_CHECKBOX_ = 3 };

bool IsCheckOrRadio (DWORD iStyle)
  {
  if (iStyle & (BS_PUSHLIKE | BS_BITMAP | BS_ICON))
    return false;
  switch (iStyle & BS_TYPEMASK)
    {
    case BS_CHECKBOX: case BS_AUTOCHECKBOX: case BS_3STATE: case BS_AUTO3STATE:
    case BS_RADIOBUTTON: case BS_AUTORADIOBUTTON:
      return true;
    default:
      return false;
    }
  }

bool IsGroupBox (DWORD iStyle)
  {
  return (iStyle & BS_TYPEMASK) == BS_GROUPBOX;
  }

void PaintCheckOrRadio (HWND hwnd)
  {
  CDarkPaint paint (hwnd);
  HDC hdc = paint.hdc ();
  RECT rc = paint.rect ();
  ::FillRect (hdc, &rc, hbrFace);

  const DWORD iStyle = ::GetWindowLong (hwnd, GWL_STYLE);
  const LRESULT iState = ::SendMessage (hwnd, BM_GETSTATE, 0, 0);
  const bool bEnabled = ::IsWindowEnabled (hwnd) != FALSE;
  const bool bRadio = (iStyle & BS_TYPEMASK) == BS_RADIOBUTTON || (iStyle & BS_TYPEMASK) == BS_AUTORADIOBUTTON;

  // the themes number their states normal, hot, pressed, disabled, in rows of four
  int iStateId = !bEnabled ? 4 : (iState & BST_PUSHED) ? 3 : (iState & BST_HOT) ? 2 : 1;
  if (iState & BST_CHECKED)
    iStateId += 4;
  else if (iState & BST_INDETERMINATE)
    iStateId += 8;

  int iPart = bRadio ? BP_RADIOBUTTON_ : BP_CHECKBOX_;

  SIZE sizeGlyph = { 13, 13 };
  HTHEME_ hTheme = pOpenThemeData (hwnd, L"Button");
  if (hTheme)
    pGetThemePartSize (hTheme, hdc, iPart, iStateId, NULL, 1 /* TS_TRUE */, &sizeGlyph);

  const bool bTextLeft = (iStyle & BS_LEFTTEXT) != 0;

  RECT rcGlyph;
  rcGlyph.left = bTextLeft ? rc.right - sizeGlyph.cx : rc.left;
  rcGlyph.right = rcGlyph.left + sizeGlyph.cx;
  rcGlyph.top = (rc.bottom - sizeGlyph.cy) / 2;
  rcGlyph.bottom = rcGlyph.top + sizeGlyph.cy;

  if (hTheme)
    {
    pDrawThemeBackground (hTheme, hdc, iPart, iStateId, &rcGlyph, NULL);
    pCloseThemeData (hTheme);
    }

  WCHAR sText [512];
  const int iLength = ::GetWindowTextW (hwnd, sText, sizeof sText / sizeof sText [0]);

  RECT rcText = rc;
  const int iGap = sizeGlyph.cx / 3 + 1;
  if (bTextLeft)
    rcText.right = rcGlyph.left - iGap;
  else
    rcText.left = rcGlyph.right + iGap;

  HFONT hFont = (HFONT) ::SendMessage (hwnd, WM_GETFONT, 0, 0);
  HGDIOBJ hOld = hFont ? ::SelectObject (hdc, hFont) : NULL;
  ::SetBkMode (hdc, TRANSPARENT);
  ::SetTextColor (hdc, bEnabled ? kText : kDisabled);

  UINT iFormat = PrefixFlag (hwnd) | (bTextLeft && (iStyle & BS_RIGHT) ? DT_RIGHT : DT_LEFT);
  if (iStyle & BS_MULTILINE)
    {
    RECT rcMeasure = rcText;
    ::DrawTextW (hdc, sText, iLength, &rcMeasure, iFormat | DT_WORDBREAK | DT_CALCRECT);
    rcText.top += ((rcText.bottom - rcText.top) - (rcMeasure.bottom - rcMeasure.top)) / 2;
    iFormat |= DT_WORDBREAK;
    }
  else
    iFormat |= DT_SINGLELINE | DT_VCENTER;

  ::DrawTextW (hdc, sText, iLength, &rcText, iFormat);

  if (::GetFocus () == hwnd && iLength > 0
      && !(::SendMessage (hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
    {
    RECT rcFocus = rcText;
    ::DrawTextW (hdc, sText, iLength, &rcFocus, iFormat | DT_CALCRECT);
    if (!(iStyle & BS_MULTILINE))
      {
      const int iHeight = rcFocus.bottom - rcFocus.top;
      rcFocus.top = (rc.bottom - iHeight) / 2;
      rcFocus.bottom = rcFocus.top + iHeight;
      }
    ::InflateRect (&rcFocus, 1, 0);
    ::SetTextColor (hdc, kText);
    ::SetBkColor (hdc, kFace);
    ::DrawFocusRect (hdc, &rcFocus);
    }

  if (hOld)
    ::SelectObject (hdc, hOld);
  }

void PaintGroupBox (HWND hwnd)
  {
  // No buffer and no fill: a group box is behind its contents, and paints round them.
  PAINTSTRUCT ps;
  HDC hdc = ::BeginPaint (hwnd, &ps);
  RECT rc;
  ::GetClientRect (hwnd, &rc);

  HFONT hFont = (HFONT) ::SendMessage (hwnd, WM_GETFONT, 0, 0);
  HGDIOBJ hOld = hFont ? ::SelectObject (hdc, hFont) : NULL;

  TEXTMETRICW tm;
  ::GetTextMetricsW (hdc, &tm);

  WCHAR sText [512];
  const int iLength = ::GetWindowTextW (hwnd, sText, sizeof sText / sizeof sText [0]);

  RECT rcFrame = rc;
  rcFrame.top += tm.tmHeight / 2;
  ::FrameRect (hdc, &rcFrame, hbrEdge);

  if (iLength > 0)
    {
    const UINT iPrefix = PrefixFlag (hwnd);
    RECT rcText = { tm.tmAveCharWidth, 0, rc.right - tm.tmAveCharWidth, tm.tmHeight };
    RECT rcMeasure = rcText;
    ::DrawTextW (hdc, sText, iLength, &rcMeasure, DT_SINGLELINE | DT_CALCRECT | iPrefix);
    rcMeasure.left -= 2;
    rcMeasure.right += 2;
    ::FillRect (hdc, &rcMeasure, hbrFace);
    ::SetBkMode (hdc, TRANSPARENT);
    ::SetTextColor (hdc, ::IsWindowEnabled (hwnd) ? kText : kDisabled);
    ::DrawTextW (hdc, sText, iLength, &rcText, DT_SINGLELINE | iPrefix);
    }

  if (hOld)
    ::SelectObject (hdc, hOld);
  ::EndPaint (hwnd, &ps);
  }

LRESULT CALLBACK ButtonProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
  {
  if (msg == WM_NCDESTROY)
    ::RemoveWindowSubclass (hwnd, ButtonProc, id);
  else if (bDark)
    {
    const DWORD iStyle = ::GetWindowLong (hwnd, GWL_STYLE);
    const bool bCheck = IsCheckOrRadio (iStyle);
    const bool bGroup = IsGroupBox (iStyle);

    if (bCheck || bGroup)
      switch (msg)
        {
        case WM_PAINT:
          if (bCheck)
            PaintCheckOrRadio (hwnd);
          else
            PaintGroupBox (hwnd);
          return 0;

        case WM_ERASEBKGND:
          return 1;

        case WM_ENABLE:
        case WM_SETTEXT:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case BM_SETCHECK:
        case BM_SETSTATE:
        case WM_UPDATEUISTATE:
          {
          // These paint directly as well as updating their state, in the native
          // colours.  Let them, and paint over it.
          const LRESULT iResult = ::DefSubclassProc (hwnd, msg, wParam, lParam);
          ::InvalidateRect (hwnd, NULL, FALSE);
          return iResult;
          }
        }
    }
  return ::DefSubclassProc (hwnd, msg, wParam, lParam);
  }

// ---------------------------------------------------------------------------
// Tab controls: property sheets' and the MDI tabs.  No theme offers a dark one.

void PaintTabs (HWND hwnd)
  {
  CDarkPaint paint (hwnd);
  HDC hdc = paint.hdc ();
  RECT rc = paint.rect ();
  ::FillRect (hdc, &rc, hbrFace);

  HFONT hFont = (HFONT) ::SendMessage (hwnd, WM_GETFONT, 0, 0);
  HGDIOBJ hOld = hFont ? ::SelectObject (hdc, hFont) : NULL;
  ::SetBkMode (hdc, TRANSPARENT);

  const int iCount = TabCtrl_GetItemCount (hwnd);
  const int iSelected = TabCtrl_GetCurSel (hwnd);
  HIMAGELIST hImages = TabCtrl_GetImageList (hwnd);

  // the page's frame, if this control has pages inside it
  RECT rcDisplay = rc;
  TabCtrl_AdjustRect (hwnd, FALSE, &rcDisplay);
  if (rcDisplay.bottom - rcDisplay.top > 8)
    {
    ::InflateRect (&rcDisplay, 2, 2);
    ::FrameRect (hdc, &rcDisplay, hbrEdge);
    }

  POINT ptCursor;
  ::GetCursorPos (&ptCursor);
  ::ScreenToClient (hwnd, &ptCursor);

  for (int i = 0; i < iCount; i++)
    {
    RECT rcItem;
    if (!TabCtrl_GetItemRect (hwnd, i, &rcItem))
      continue;

    const bool bSelected = i == iSelected;
    ::FillRect (hdc, &rcItem, bSelected ? hbrPressed : ::PtInRect (&rcItem, ptCursor) ? hbrHot : hbrControl);
    ::FrameRect (hdc, &rcItem, hbrEdge);

    char sText [300];
    TCITEMA item;
    ZeroMemory (&item, sizeof item);
    item.mask = TCIF_TEXT | TCIF_IMAGE;
    item.pszText = sText;
    item.cchTextMax = sizeof sText;
    sText [0] = 0;
    ::SendMessageA (hwnd, TCM_GETITEMA, i, (LPARAM) &item);

    RECT rcText = rcItem;
    if (hImages && item.iImage >= 0)
      {
      int cx = 0, cy = 0;
      ::ImageList_GetIconSize (hImages, &cx, &cy);
      ::ImageList_Draw (hImages, item.iImage, hdc, rcItem.left + 6,
                        (rcItem.top + rcItem.bottom - cy) / 2, ILD_TRANSPARENT);
      rcText.left += cx + 6;
      }

    ::SetTextColor (hdc, ::IsWindowEnabled (hwnd) ? kText : kDisabled);
    ::DrawTextA (hdc, sText, -1, &rcText, DT_SINGLELINE | DT_CENTER | DT_VCENTER | PrefixFlag (hwnd));
    }

  if (hOld)
    ::SelectObject (hdc, hOld);

  }

LRESULT CALLBACK TabProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
  {
  if (msg == WM_NCDESTROY)
    ::RemoveWindowSubclass (hwnd, TabProc, id);
  else if (bDark)
    switch (msg)
      {
      case WM_PAINT:
        PaintTabs (hwnd);
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_NCPAINT:
        PaintNonClient (hwnd, hbrFace);
        return 0;
      }
  return ::DefSubclassProc (hwnd, msg, wParam, lParam);
  }

// ---------------------------------------------------------------------------
// The status bar.  The control paints its own text in the theme's colour, unless
// each pane is owner-drawn, and it isn't: MFC owns what is in the panes.

void PaintStatusBar (HWND hwnd)
  {
  CDarkPaint paint (hwnd);
  HDC hdc = paint.hdc ();
  RECT rc = paint.rect ();

  // Let the control (and MFC beneath it) paint first.  That runs WM_DRAWITEM for
  // the owner-drawn pane ("MORE"), whose pointer we could not get back ourselves.
  ::DefSubclassProc (hwnd, WM_PAINT, (WPARAM) hdc, 0);

  int iParts [32];
  const int iCount = (int) ::SendMessage (hwnd, SB_GETPARTS, 32, (LPARAM) iParts);

  // now cover everything except those panes
  HRGN hrgn = ::CreateRectRgnIndirect (&rc);
  for (int i = 0; i < iCount; i++)
    if (HIWORD (::SendMessage (hwnd, SB_GETTEXTLENGTHA, i, 0)) & SBT_OWNERDRAW)
      {
      RECT rcPart;
      ::SendMessage (hwnd, SB_GETRECT, i, (LPARAM) &rcPart);
      HRGN hrgnPart = ::CreateRectRgnIndirect (&rcPart);
      ::CombineRgn (hrgn, hrgn, hrgnPart, RGN_DIFF);
      ::DeleteObject (hrgnPart);
      }
  ::SelectClipRgn (hdc, hrgn);
  ::DeleteObject (hrgn);

  ::FillRect (hdc, &rc, hbrFace);

  // a line along the top, to separate it from what's above
  RECT rcLine = { rc.left, rc.top, rc.right, rc.top + 1 };
  ::FillRect (hdc, &rcLine, hbrEdge);

  HFONT hFont = (HFONT) ::SendMessage (hwnd, WM_GETFONT, 0, 0);
  HGDIOBJ hOld = hFont ? ::SelectObject (hdc, hFont) : NULL;
  ::SetBkMode (hdc, TRANSPARENT);
  ::SetTextColor (hdc, kText);

  for (int i = 0; i < iCount; i++)
    {
    const LRESULT iInfo = ::SendMessage (hwnd, SB_GETTEXTLENGTHA, i, 0);
    if (HIWORD (iInfo) & SBT_OWNERDRAW)
      continue;

    RECT rcPart;
    ::SendMessage (hwnd, SB_GETRECT, i, (LPARAM) &rcPart);

    if (i > 0)
      {
      RECT rcDivider = { rcPart.left - 1, rcPart.top + 2, rcPart.left, rcPart.bottom - 2 };
      ::FillRect (hdc, &rcDivider, hbrEdge);
      }

    std::vector<char> text (LOWORD (iInfo) + 1);
    ::SendMessage (hwnd, SB_GETTEXTA, i, (LPARAM) &text [0]);

    RECT rcText = rcPart;
    rcText.left += 2;
    rcText.right -= 1;

    // the control's convention: \t centres what follows, \t\t right-aligns it
    const char * p = &text [0];
    UINT iAlign = DT_LEFT;
    if (*p == '\t')
      {
      p++;
      iAlign = DT_CENTER;
      if (*p == '\t')
        {
        p++;
        iAlign = DT_RIGHT;
        }
      }

    ::DrawTextA (hdc, p, -1, &rcText, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | iAlign);
    }

  if (hOld)
    ::SelectObject (hdc, hOld);

  // the size grip: three diagonal rows of dots
  ::SelectClipRgn (hdc, NULL);
  if (::GetWindowLong (hwnd, GWL_STYLE) & SBARS_SIZEGRIP)
    {
    if (!::IsZoomed (::GetParent (hwnd)))
      for (int iRow = 0; iRow < 3; iRow++)
        for (int iDot = 0; iDot <= iRow; iDot++)
          {
          RECT rcDot;
          rcDot.left = rc.right - 4 - (iRow - iDot) * 4;
          rcDot.top = rc.bottom - 4 - iDot * 4;
          rcDot.right = rcDot.left + 2;
          rcDot.bottom = rcDot.top + 2;
          ::FillRect (hdc, &rcDot, hbrEdge);
          }
    }
  }

LRESULT CALLBACK StatusBarProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
  {
  if (msg == WM_NCDESTROY)
    ::RemoveWindowSubclass (hwnd, StatusBarProc, id);
  else if (bDark)
    switch (msg)
      {
      case WM_PAINT:
        if (wParam == 0)
          {
          PaintStatusBar (hwnd);
          return 0;
          }
        break;
      case WM_ERASEBKGND:
        return 1;
      case WM_NCPAINT:
        PaintNonClient (hwnd, hbrFace);
        return 0;
      }
  return ::DefSubclassProc (hwnd, msg, wParam, lParam);
  }

// ---------------------------------------------------------------------------
// Toolbars, and the dock bars they sit in.

LRESULT DrawToolbarItem (NMTBCUSTOMDRAW * pDraw)
  {
  switch (pDraw->nmcd.dwDrawStage)
    {
    case CDDS_PREPAINT:
      // By now the control has filled its background, from the theme or the light
      // palette, whatever WM_ERASEBKGND said.  Cover it.
      ::FillRect (pDraw->nmcd.hdc, &pDraw->nmcd.rc, hbrFace);
      return CDRF_NOTIFYITEMDRAW;

    case CDDS_ITEMPREPAINT:
      {
      const UINT iState = pDraw->nmcd.uItemState;
      HBRUSH hbr = NULL;
      if (iState & (CDIS_SELECTED | CDIS_CHECKED))
        hbr = hbrPressed;
      else if (iState & CDIS_HOT)
        hbr = hbrHot;
      if (hbr)
        ::FillRect (pDraw->nmcd.hdc, &pDraw->nmcd.rc, hbr);
      pDraw->clrText = kText;
      return TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES | TBCDRF_NOOFFSET | TBCDRF_NOETCHEDEFFECT
           | TBCDRF_USECDCOLORS;
      }
    }
  return CDRF_DODEFAULT;
  }

LRESULT CALLBACK ToolbarProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
  {
  if (msg == WM_NCDESTROY)
    ::RemoveWindowSubclass (hwnd, ToolbarProc, id);
  else if (bDark && msg == WM_ERASEBKGND)
    {
    RECT rc;
    ::GetClientRect (hwnd, &rc);
    ::FillRect ((HDC) wParam, &rc, hbrFace);
    return 1;
    }
  return ::DefSubclassProc (hwnd, msg, wParam, lParam);
  }

bool IsToolbar (HWND hwnd)
  {
  char sClass [32];
  return ::GetClassNameA (hwnd, sClass, sizeof sClass) && strcmp (sClass, TOOLBARCLASSNAMEA) == 0;
  }

// MFC's CDockBar (and its other control bars) draw their own borders, from the light
// palette it cached at startup.
LRESULT CALLBACK ControlBarProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
  {
  if (msg == WM_NCDESTROY)
    ::RemoveWindowSubclass (hwnd, ControlBarProc, id);
  else if (bDark)
    switch (msg)
      {
      case WM_ERASEBKGND:
        {
        RECT rc;
        ::GetClientRect (hwnd, &rc);
        ::FillRect ((HDC) wParam, &rc, hbrFace);
        return 1;
        }
      case WM_PAINT:
        {
        PAINTSTRUCT ps;
        HDC hdc = ::BeginPaint (hwnd, &ps);
        ::FillRect (hdc, &ps.rcPaint, hbrFace);
        ::EndPaint (hwnd, &ps);
        return 0;
        }
      case WM_NCPAINT:
        PaintNonClient (hwnd, hbrFace);
        return 0;
      case WM_NOTIFY:
        {
        NMHDR * pHdr = (NMHDR *) lParam;
        if (pHdr->code == NM_CUSTOMDRAW && IsToolbar (pHdr->hwndFrom))
          return DrawToolbarItem ((NMTBCUSTOMDRAW *) lParam);
        }
        break;
      }
  return ::DefSubclassProc (hwnd, msg, wParam, lParam);
  }

// ---------------------------------------------------------------------------
// Lists and trees: they take colours.  Their headers take a theme, but the text only
// by custom draw.  Grid lines take nothing: they come out bright white, whatever the
// colours are.  So the control is told not to, and they are drawn over it here.

const char * const sGridProp = "McDarkGrid";

void PaintGridLines (HWND hwnd)
  {
  if ((::GetWindowLong (hwnd, GWL_STYLE) & LVS_TYPEMASK) != LVS_REPORT)
    return;

  HWND hwndHeader = ListView_GetHeader (hwnd);
  RECT rcClient, rcHeader;
  ::GetClientRect (hwnd, &rcClient);
  ::GetClientRect (hwndHeader, &rcHeader);
  const int iTop = (::GetWindowLong (hwndHeader, GWL_STYLE) & WS_VISIBLE) ? rcHeader.bottom : 0;

  HDC hdc = ::GetDC (hwnd);
  ::ExcludeClipRect (hdc, 0, 0, rcClient.right, iTop);

  const int iScroll = ::GetScrollPos (hwnd, SB_HORZ);
  const int iColumns = Header_GetItemCount (hwndHeader);
  for (int i = 0; i < iColumns; i++)
    {
    RECT rcItem;
    if (Header_GetItemRect (hwndHeader, i, &rcItem))
      {
      RECT rcLine = { rcItem.right - iScroll - 1, iTop, rcItem.right - iScroll, rcClient.bottom };
      ::FillRect (hdc, &rcLine, hbrPressed);
      }
    }

  RECT rcRow;
  if (ListView_GetItemCount (hwnd) > 0
      && ListView_GetItemRect (hwnd, ListView_GetTopIndex (hwnd), &rcRow, LVIR_BOUNDS))
    {
    const int iHeight = rcRow.bottom - rcRow.top;
    if (iHeight > 0)
      for (int y = rcRow.bottom - 1; y < rcClient.bottom; y += iHeight)
        {
        RECT rcLine = { 0, y, rcClient.right, y + 1 };
        ::FillRect (hdc, &rcLine, hbrPressed);
        }
    }

  ::ReleaseDC (hwnd, hdc);
  }

void ColourListView (HWND hwnd, bool bMakeDark)
  {
  const COLORREF cBack = bMakeDark ? kControl : ::GetSysColor (COLOR_WINDOW);
  const COLORREF cText = bMakeDark ? kText : ::GetSysColor (COLOR_WINDOWTEXT);
  ListView_SetBkColor (hwnd, cBack);
  ListView_SetTextBkColor (hwnd, cBack);
  ListView_SetTextColor (hwnd, cText);

  const DWORD iExStyle = ListView_GetExtendedListViewStyle (hwnd);
  if (bMakeDark && (iExStyle & LVS_EX_GRIDLINES))
    {
    ::SetPropA (hwnd, sGridProp, (HANDLE) 1);
    ListView_SetExtendedListViewStyleEx (hwnd, LVS_EX_GRIDLINES, 0);
    }
  else if (!bMakeDark && ::GetPropA (hwnd, sGridProp))
    {
    ::RemovePropA (hwnd, sGridProp);
    ListView_SetExtendedListViewStyleEx (hwnd, LVS_EX_GRIDLINES, LVS_EX_GRIDLINES);
    }
  }

void ColourTreeView (HWND hwnd, bool bMakeDark)
  {
  TreeView_SetBkColor (hwnd, bMakeDark ? kControl : (COLORREF) -1);
  TreeView_SetTextColor (hwnd, bMakeDark ? kText : (COLORREF) -1);
  TreeView_SetLineColor (hwnd, bMakeDark ? kEdge : CLR_DEFAULT);
  }

LRESULT CALLBACK ListViewProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
  {
  if (msg == WM_NCDESTROY)
    {
    ::RemovePropA (hwnd, sGridProp);
    ::RemoveWindowSubclass (hwnd, ListViewProc, id);
    }
  else if (bDark)
    {
    if (msg == LVM_SETEXTENDEDLISTVIEWSTYLE)
      {
      if (wParam == 0 || (wParam & LVS_EX_GRIDLINES))
        {
        if (lParam & LVS_EX_GRIDLINES)
          ::SetPropA (hwnd, sGridProp, (HANDLE) 1);
        else
          ::RemovePropA (hwnd, sGridProp);
        lParam &= ~LVS_EX_GRIDLINES;
        }
      }
    else if (msg == WM_PAINT && ::GetPropA (hwnd, sGridProp))
      {
      const LRESULT iResult = ::DefSubclassProc (hwnd, msg, wParam, lParam);
      PaintGridLines (hwnd);
      return iResult;
      }
    if (msg == WM_NOTIFY)
      {
      NMHDR * pHdr = (NMHDR *) lParam;
      if (pHdr->code == NM_CUSTOMDRAW && pHdr->hwndFrom == ListView_GetHeader (hwnd))
        {
        NMCUSTOMDRAW * pDraw = (NMCUSTOMDRAW *) lParam;
        if (pDraw->dwDrawStage == CDDS_PREPAINT)
          return CDRF_NOTIFYITEMDRAW;
        if (pDraw->dwDrawStage == CDDS_ITEMPREPAINT)
          {
          ::SetTextColor (pDraw->hdc, kText);
          return CDRF_DODEFAULT;
          }
        }
      }
    else if (msg == WM_NCPAINT)
      {
      const LRESULT iResult = ::DefSubclassProc (hwnd, msg, wParam, lParam);
      PaintEdge (hwnd);
      return iResult;
      }
    }
  return ::DefSubclassProc (hwnd, msg, wParam, lParam);
  }

// ---------------------------------------------------------------------------
// Anything else with a client edge or a non-client scroll bar: edit boxes, list boxes,
// trees, MFC's views.  The theme does the scroll bars; this does the edge.

LRESULT CALLBACK EdgeProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
  {
  if (msg == WM_NCDESTROY)
    ::RemoveWindowSubclass (hwnd, EdgeProc, id);
  else if (bDark && msg == WM_NCPAINT)
    {
    const LRESULT iResult = ::DefSubclassProc (hwnd, msg, wParam, lParam);
    PaintEdge (hwnd);
    return iResult;
    }
  return ::DefSubclassProc (hwnd, msg, wParam, lParam);
  }

// ---------------------------------------------------------------------------
// Dialogs

bool ClassIs (HWND hwnd, const char * sName)
  {
  char sClass [32];
  return ::GetClassNameA (hwnd, sClass, sizeof sClass) && _stricmp (sClass, sName) == 0;
  }

LRESULT DialogCtlColor (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
  {
  HBRUSH hbr = (HBRUSH) ::DefSubclassProc (hwnd, msg, wParam, lParam);

  // Somebody who answered with a brush of their own (the Tip of the Day, a hyperlink)
  // meant it.  What the dialog manager answers is one of the system's.
  if (hbr && hbr != ::GetSysColorBrush (COLOR_3DFACE) && hbr != ::GetSysColorBrush (COLOR_WINDOW)
          && hbr != (HBRUSH) (COLOR_3DFACE + 1) && hbr != (HBRUSH) (COLOR_WINDOW + 1))
    return (LRESULT) hbr;

  HDC hdc = (HDC) wParam;
  HWND hwndControl = (HWND) lParam;

  bool bField = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX;

  // a read-only edit box asks with WM_CTLCOLORSTATIC, and should still look like one
  if (msg == WM_CTLCOLORSTATIC && ClassIs (hwndControl, "Edit"))
    bField = true;

  ::SetTextColor (hdc, ::IsWindowEnabled (hwndControl) ? kText : kDisabled);
  ::SetBkColor (hdc, bField ? kControl : kFace);
  return (LRESULT) (bField ? hbrControl : hbrFace);
  }

// A message box paints its own white panel and grey button strip, unconditionally,
// in WM_PAINT.  With its text being a Static, all that is needed is to paint nothing.
LRESULT CALLBACK DialogProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR iIsMessageBox)
  {
  if (msg == WM_NCDESTROY)
    ::RemoveWindowSubclass (hwnd, DialogProc, id);
  else if (bDark)
    switch (msg)
      {
      case WM_CTLCOLORDLG:
      case WM_CTLCOLORSTATIC:
      case WM_CTLCOLORBTN:
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX:
      case WM_CTLCOLORSCROLLBAR:
      case WM_CTLCOLORMSGBOX:
        return DialogCtlColor (hwnd, msg, wParam, lParam);

      case WM_ERASEBKGND:
        {
        RECT rc;
        ::GetClientRect (hwnd, &rc);
        ::FillRect ((HDC) wParam, &rc, hbrFace);
        return 1;
        }

      case WM_PAINT:
        if (iIsMessageBox)
          {
          PAINTSTRUCT ps;
          HDC hdc = ::BeginPaint (hwnd, &ps);
          ::FillRect (hdc, &ps.rcPaint, hbrFace);
          ::EndPaint (hwnd, &ps);
          return 0;
          }
        break;

      case WM_INITDIALOG:
        {
        // Property pages draw the tab control's texture on their backgrounds.
        const LRESULT iResult = ::DefSubclassProc (hwnd, msg, wParam, lParam);
        if (pEnableThemeDialogTexture)
          pEnableThemeDialogTexture (hwnd, 1 /* ETDT_DISABLE */);
        return iResult;
        }
      }
  return ::DefSubclassProc (hwnd, msg, wParam, lParam);
  }

// ---------------------------------------------------------------------------
// The menu bar.  Popups follow the app mode, but no mode reaches the menu bar: it
// is painted through the undocumented WM_UAHDRAWMENU* messages, which are what the
// default window procedure sends itself when a window has a themed menu bar.

#define WM_UAHDRAWMENU_      0x0091
#define WM_UAHDRAWMENUITEM_  0x0092

struct UAHMENU_
  {
  HMENU hmenu;
  HDC hdc;
  DWORD dwFlags;
  };

union UAHMENUITEMMETRICS_
  {
  struct { DWORD cx; DWORD cy; } rgsizeBar [2];
  struct { DWORD cx; DWORD cy; } rgsizePopup [4];
  };

struct UAHMENUPOPUPMETRICS_
  {
  DWORD rgcx [4];
  DWORD fUpdateMaxWidths : 2;
  };

struct UAHMENUITEM_
  {
  int iPosition;
  UAHMENUITEMMETRICS_ umim;
  UAHMENUPOPUPMETRICS_ umpm;
  };

struct UAHDRAWMENUITEM_
  {
  DRAWITEMSTRUCT dis;
  UAHMENU_ um;
  UAHMENUITEM_ umi;
  };

void DrawMenuBarBackground (HWND hwnd, UAHMENU_ * pMenu)
  {
  MENUBARINFO info;
  ZeroMemory (&info, sizeof info);
  info.cbSize = sizeof info;
  if (!::GetMenuBarInfo (hwnd, OBJID_MENU, 0, &info))
    return;
  RECT rcWindow;
  ::GetWindowRect (hwnd, &rcWindow);
  RECT rc = info.rcBar;
  ::OffsetRect (&rc, -rcWindow.left, -rcWindow.top);
  ::FillRect (pMenu->hdc, &rc, hbrFace);
  }

bool DrawMenuBarItem (UAHDRAWMENUITEM_ * pItem)
  {
  // The maximised MDI child's icon and buttons are bitmap items.  Those are the
  // system's to draw.
  WCHAR sText [256];
  MENUITEMINFOW info;
  ZeroMemory (&info, sizeof info);
  info.cbSize = sizeof info;
  info.fMask = MIIM_STRING | MIIM_FTYPE;
  info.dwTypeData = sText;
  info.cch = sizeof sText / sizeof sText [0];
  if (!::GetMenuItemInfoW (pItem->um.hmenu, pItem->umi.iPosition, TRUE, &info))
    return false;
  if (info.fType & (MFT_BITMAP | MFT_OWNERDRAW))
    return false;

  const UINT iState = pItem->dis.itemState;
  const bool bDisabled = (iState & (ODS_GRAYED | ODS_DISABLED | ODS_INACTIVE)) != 0;

  HBRUSH hbr = hbrFace;
  if (iState & ODS_SELECTED)
    hbr = hbrPressed;
  else if (iState & ODS_HOTLIGHT)
    hbr = hbrHot;

  ::FillRect (pItem->um.hdc, &pItem->dis.rcItem, hbr);
  ::SetBkMode (pItem->um.hdc, TRANSPARENT);
  ::SetTextColor (pItem->um.hdc, bDisabled ? kDisabled : kText);
  ::DrawTextW (pItem->um.hdc, sText, (int) info.cch, &pItem->dis.rcItem,
               DT_CENTER | DT_SINGLELINE | DT_VCENTER | ((iState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
  return true;
  }

// The system draws one light line under the menu bar, after everything else.
void CoverLineUnderMenu (HWND hwnd)
  {
  MENUBARINFO info;
  ZeroMemory (&info, sizeof info);
  info.cbSize = sizeof info;
  if (!::GetMenuBarInfo (hwnd, OBJID_MENU, 0, &info))
    return;

  RECT rcClient, rcWindow;
  ::GetClientRect (hwnd, &rcClient);
  ::MapWindowPoints (hwnd, NULL, (POINT *) &rcClient, 2);
  ::GetWindowRect (hwnd, &rcWindow);
  ::OffsetRect (&rcClient, -rcWindow.left, -rcWindow.top);

  RECT rcLine = rcClient;
  rcLine.bottom = rcLine.top;
  rcLine.top--;

  HDC hdc = ::GetWindowDC (hwnd);
  ::FillRect (hdc, &rcLine, hbrFace);
  ::ReleaseDC (hwnd, hdc);
  }

LRESULT CALLBACK FrameProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
  {
  if (msg == WM_NCDESTROY)
    ::RemoveWindowSubclass (hwnd, FrameProc, id);
  else if (bDark)
    switch (msg)
      {
      case WM_NOTIFY:
        {
        // MFC gives a toolbar the frame as its parent for notifications
        NMHDR * pHdr = (NMHDR *) lParam;
        if (pHdr->code == NM_CUSTOMDRAW && IsToolbar (pHdr->hwndFrom))
          return DrawToolbarItem ((NMTBCUSTOMDRAW *) lParam);
        }
        break;

      case WM_UAHDRAWMENU_:
        DrawMenuBarBackground (hwnd, (UAHMENU_ *) lParam);
        return 0;

      case WM_UAHDRAWMENUITEM_:
        // popups come this way too, and draw themselves perfectly well
        if (((UAHDRAWMENUITEM_ *) lParam)->um.hmenu == ::GetMenu (hwnd)
            && DrawMenuBarItem ((UAHDRAWMENUITEM_ *) lParam))
          return 0;
        break;

      case WM_NCPAINT:
      case WM_NCACTIVATE:
        {
        const LRESULT iResult = ::DefSubclassProc (hwnd, msg, wParam, lParam);
        if (::GetMenu (hwnd))
          CoverLineUnderMenu (hwnd);
        return iResult;
        }
      }
  return ::DefSubclassProc (hwnd, msg, wParam, lParam);
  }

// ---------------------------------------------------------------------------
// MDI child windows.  DWM doesn't draw their frames: USER does, from the theme's
// classic-looking caption, and no theme name darkens it.  So the frame, the caption
// and its three buttons are painted here, while the system goes on deciding what a
// click on them means.  The buttons need the press tracked by hand, or the system
// would paint its own light ones over ours while the mouse is down.

#define WM_GETTITLEBARINFOEX_  0x033F

struct TITLEBARINFOEX_
  {
  DWORD cbSize;
  RECT  rcTitleBar;
  DWORD rgstate [6];
  RECT  rgrect [6];
  };

enum { eCaptionMin = 2, eCaptionMax = 3, eCaptionClose = 5 };

int iCaptionHot = 0;          // which button the mouse is over, in the one window it's in
int iCaptionPressed = 0;

int CaptionIndexFromHit (WPARAM iHit)
  {
  switch (iHit)
    {
    case HTMINBUTTON: return eCaptionMin;
    case HTMAXBUTTON: return eCaptionMax;
    case HTCLOSE:     return eCaptionClose;
    default:          return 0;
    }
  }

void DrawCaptionButton (HDC hdc, HWND hwnd, const RECT & rcButton, int iIndex)
  {
  const bool bPressed = iCaptionPressed == iIndex;
  const bool bHot = iCaptionHot == iIndex;

  if (bPressed || bHot)
    {
    HBRUSH hbr = iIndex == eCaptionClose
               ? (bPressed ? ::CreateSolidBrush (RGB (150, 30, 30)) : ::CreateSolidBrush (RGB (196, 43, 28)))
               : (bPressed ? hbrPressed : hbrHot);
    ::FillRect (hdc, &rcButton, hbr);
    if (iIndex == eCaptionClose)
      ::DeleteObject (hbr);
    }

  const int cx = (rcButton.left + rcButton.right) / 2;
  const int cy = (rcButton.top + rcButton.bottom) / 2;
  const int r = max (3, (rcButton.bottom - rcButton.top) / 4);

  HPEN hPen = ::CreatePen (PS_SOLID, 1, kText);
  HGDIOBJ hOldPen = ::SelectObject (hdc, hPen);
  HGDIOBJ hOldBrush = ::SelectObject (hdc, ::GetStockObject (NULL_BRUSH));

  switch (iIndex)
    {
    case eCaptionMin:
      if (::IsIconic (hwnd))      // it's a restore button
        {
        ::Rectangle (hdc, cx - r, cy - r + 2, cx + r - 1, cy + r + 1);
        ::MoveToEx (hdc, cx - r + 2, cy - r, NULL);
        ::LineTo (hdc, cx + r + 1, cy - r);
        ::LineTo (hdc, cx + r + 1, cy + r - 1);
        }
      else
        {
        ::MoveToEx (hdc, cx - r, cy, NULL);
        ::LineTo (hdc, cx + r + 1, cy);
        }
      break;
    case eCaptionMax:
      ::Rectangle (hdc, cx - r, cy - r, cx + r + 1, cy + r + 1);
      break;
    case eCaptionClose:
      ::MoveToEx (hdc, cx - r, cy - r, NULL);
      ::LineTo (hdc, cx + r + 1, cy + r + 1);
      ::MoveToEx (hdc, cx + r, cy - r, NULL);
      ::LineTo (hdc, cx - r - 1, cy + r + 1);
      break;
    }

  ::SelectObject (hdc, hOldBrush);
  ::SelectObject (hdc, hOldPen);
  ::DeleteObject (hPen);
  }

void PaintMdiChildFrame (HWND hwnd)
  {
  HDC hdc = ::GetWindowDC (hwnd);
  if (!hdc)
    return;

  RECT rcWindow, rcClient;
  ::GetWindowRect (hwnd, &rcWindow);
  ::GetClientRect (hwnd, &rcClient);
  ::MapWindowPoints (hwnd, NULL, (POINT *) &rcClient, 2);
  const POINT ptOrigin = { rcWindow.left, rcWindow.top };
  ::OffsetRect (&rcClient, -ptOrigin.x, -ptOrigin.y);
  ::OffsetRect (&rcWindow, -ptOrigin.x, -ptOrigin.y);

  ::ExcludeClipRect (hdc, rcClient.left, rcClient.top, rcClient.right, rcClient.bottom);

  const bool bActive = (HWND) ::SendMessage (::GetParent (hwnd), WM_MDIGETACTIVE, 0, 0) == hwnd;

  ::FillRect (hdc, &rcWindow, bActive ? hbrControl : hbrFace);
  ::FrameRect (hdc, &rcWindow, hbrEdge);

  TITLEBARINFOEX_ info;
  ZeroMemory (&info, sizeof info);
  info.cbSize = sizeof info;
  ::SendMessage (hwnd, WM_GETTITLEBARINFOEX_, 0, (LPARAM) &info);

  RECT rcTitle = info.rcTitleBar;
  ::OffsetRect (&rcTitle, -ptOrigin.x, -ptOrigin.y);

  // the icon
  int iTextLeft = rcTitle.left + 4;
  HICON hIcon = (HICON) ::SendMessage (hwnd, WM_GETICON, ICON_SMALL, 0);
  if (!hIcon)
    hIcon = (HICON) ::GetClassLongPtr (hwnd, GCLP_HICONSM);
  if (hIcon)
    {
    const int iSize = ::GetSystemMetrics (SM_CXSMICON);
    ::DrawIconEx (hdc, iTextLeft, (rcTitle.top + rcTitle.bottom - iSize) / 2, hIcon, iSize, iSize, 0, NULL, DI_NORMAL);
    iTextLeft += iSize + 4;
    }

  // the buttons, and how far the text can go
  int iTextRight = rcTitle.right;
  const int iButtons [] = { eCaptionMin, eCaptionMax, eCaptionClose };
  for (int i = 0; i < 3; i++)
    {
    const int iIndex = iButtons [i];
    if (info.rgstate [iIndex] & STATE_SYSTEM_INVISIBLE)
      continue;
    RECT rcButton = info.rgrect [iIndex];
    ::OffsetRect (&rcButton, -ptOrigin.x, -ptOrigin.y);
    if (rcButton.right <= rcButton.left)
      continue;
    DrawCaptionButton (hdc, hwnd, rcButton, iIndex);
    iTextRight = min (iTextRight, rcButton.left);
    }

  // the title
  NONCLIENTMETRICSA ncm;
  ZeroMemory (&ncm, sizeof ncm);
  ncm.cbSize = sizeof ncm;
  ::SystemParametersInfoA (SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
  HFONT hFont = ::CreateFontIndirectA (&ncm.lfCaptionFont);
  HGDIOBJ hOldFont = ::SelectObject (hdc, hFont);
  ::SetBkMode (hdc, TRANSPARENT);
  ::SetTextColor (hdc, bActive ? kText : kDisabled);

  WCHAR sText [512];
  const int iLength = ::GetWindowTextW (hwnd, sText, sizeof sText / sizeof sText [0]);
  RECT rcText = { iTextLeft, rcTitle.top, iTextRight - 4, rcTitle.bottom };
  ::DrawTextW (hdc, sText, iLength, &rcText, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

  ::SelectObject (hdc, hOldFont);
  ::DeleteObject (hFont);
  ::ReleaseDC (hwnd, hdc);
  }

// Holds the mouse until the button comes up, as the system would.
void TrackCaptionButton (HWND hwnd, int iIndex)
  {
  iCaptionPressed = iIndex;
  iCaptionHot = 0;
  PaintMdiChildFrame (hwnd);
  ::SetCapture (hwnd);

  bool bInside = true;
  MSG msg;
  while (::GetCapture () == hwnd && ::GetMessage (&msg, NULL, 0, 0) > 0)
    {
    if (msg.message == WM_LBUTTONUP)
      break;
    if (msg.message == WM_MOUSEMOVE)
      {
      POINT pt;
      ::GetCursorPos (&pt);
      const bool bNow = CaptionIndexFromHit (::SendMessage (hwnd, WM_NCHITTEST, 0, MAKELPARAM (pt.x, pt.y))) == iIndex;
      if (bNow != bInside)
        {
        bInside = bNow;
        iCaptionPressed = bInside ? iIndex : 0;
        PaintMdiChildFrame (hwnd);
        }
      continue;
      }
    ::TranslateMessage (&msg);
    ::DispatchMessage (&msg);
    }

  if (::GetCapture () == hwnd)
    ::ReleaseCapture ();

  iCaptionPressed = 0;
  PaintMdiChildFrame (hwnd);

  if (bInside)
    {
    WPARAM iCommand = SC_CLOSE;
    if (iIndex == eCaptionMin)
      iCommand = ::IsIconic (hwnd) ? SC_RESTORE : SC_MINIMIZE;
    else if (iIndex == eCaptionMax)
      iCommand = SC_MAXIMIZE;
    ::PostMessage (hwnd, WM_SYSCOMMAND, iCommand, 0);
    }
  }

LRESULT CALLBACK MdiChildProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
  {
  if (msg == WM_NCDESTROY)
    ::RemoveWindowSubclass (hwnd, MdiChildProc, id);
  else if (bDark && !::IsZoomed (hwnd))    // maximised, it has no frame: its buttons are the menu bar's
    switch (msg)
      {
      case WM_NCPAINT:
        PaintMdiChildFrame (hwnd);
        return 0;

      case WM_NCACTIVATE:
        {
        // -1: do the activating, but paint nothing
        const LRESULT iResult = ::DefSubclassProc (hwnd, msg, wParam, (LPARAM) -1);
        PaintMdiChildFrame (hwnd);
        return iResult;
        }

      case WM_SETTEXT:
      case WM_SETICON:
        {
        const LRESULT iResult = ::DefSubclassProc (hwnd, msg, wParam, lParam);
        PaintMdiChildFrame (hwnd);
        return iResult;
        }

      case WM_NCMOUSEMOVE:
        {
        const int iIndex = CaptionIndexFromHit (wParam);
        if (iIndex != iCaptionHot)
          {
          iCaptionHot = iIndex;
          PaintMdiChildFrame (hwnd);
          TRACKMOUSEEVENT track;
          track.cbSize = sizeof track;
          track.dwFlags = TME_LEAVE | TME_NONCLIENT;
          track.hwndTrack = hwnd;
          track.dwHoverTime = 0;
          ::TrackMouseEvent (&track);
          }
        if (iIndex)
          return 0;
        }
        break;

      case WM_NCMOUSELEAVE:
        iCaptionHot = 0;
        PaintMdiChildFrame (hwnd);
        break;

      case WM_NCLBUTTONDOWN:
      case WM_NCLBUTTONDBLCLK:
        if (const int iIndex = CaptionIndexFromHit (wParam))
          {
          // an MDI child is activated by a click anywhere on it
          ::SendMessage (::GetParent (hwnd), WM_MDIACTIVATE, (WPARAM) hwnd, 0);
          TrackCaptionButton (hwnd, iIndex);
          return 0;
          }
        break;
      }
  return ::DefSubclassProc (hwnd, msg, wParam, lParam);
  }

// ---------------------------------------------------------------------------
// Which windows, and what to do to them

// The roots whose descendants we look after, marked by this subclass.  A control
// whose root isn't one belongs to a common dialog.
LRESULT CALLBACK RootMarkerProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
  {
  if (msg == WM_NCDESTROY)
    ::RemoveWindowSubclass (hwnd, RootMarkerProc, id);
  return ::DefSubclassProc (hwnd, msg, wParam, lParam);
  }

bool IsMarkedRoot (HWND hwnd)
  {
  DWORD_PTR iData;
  return hwnd && ::GetWindowSubclass (hwnd, RootMarkerProc, 1, &iData) != FALSE;
  }

void SetWindowDark (HWND hwnd, const WCHAR * sTheme)
  {
  pAllowDarkModeForWindow (hwnd, bDark);
  if (bDark)
    pSetWindowTheme (hwnd, sTheme, NULL);
  else
    pSetWindowTheme (hwnd, NULL, NULL);
  }

void SetTitleBarDark (HWND hwnd)
  {
  BOOL bValue = bDark ? TRUE : FALSE;
  pAllowDarkModeForWindow (hwnd, bDark);
  pDwmSetWindowAttribute (hwnd, Appearance_ImmersiveDarkAttribute (iBuild), &bValue, sizeof bValue);
  }

enum eKind { eNone, eMdiChild, eDialog, eMessageBox, eButton, eEdit, eCombo, eListBox, eListView, eTree,
             eTab, eStatusBar, eToolbar, eControlBar, eScrollBar, eUpDown, eToolTip, eMdiClient,
             eScrollHost, eTopLevel };

eKind Classify (const char * sClass, bool bTopLevel)
  {
  struct { const char * sName; eKind iKind; } static const table [] = {
    { "Button",               eButton },
    { "Edit",                 eEdit },
    { "ComboBox",             eCombo },
    { "ListBox",              eListBox },
    { "SysListView32",        eListView },
    { "SysTreeView32",        eTree },
    { "SysTabControl32",      eTab },
    { "msctls_statusbar32",   eStatusBar },
    { "ToolbarWindow32",      eToolbar },
    { "ScrollBar",            eScrollBar },
    { "msctls_updown32",      eUpDown },
    { "tooltips_class32",     eToolTip },
    { "MDIClient",            eMdiClient },
    };
  if (strcmp (sClass, "#32770") == 0)
    return eDialog;
  for (size_t i = 0; i < sizeof table / sizeof table [0]; i++)
    if (_stricmp (sClass, table [i].sName) == 0)
      return table [i].iKind;
  if (bTopLevel)
    return eTopLevel;
  if (strncmp (sClass, "AfxControlBar", 13) == 0)
    return eControlBar;
  if (strncmp (sClass, "Afx", 3) == 0)
    return eScrollHost;
  return eNone;
  }

// The dialogs that are ours: our resources, or (a message box) user32's.  The file,
// colour and font dialogs come from comdlg32, and the file dialog's children from
// all sorts of places.
bool DialogIsOurs (HWND hwnd, HINSTANCE hInstance, bool * pIsMessageBox)
  {
  *pIsMessageBox = false;

  // Anything MFC is creating has been attached by now, since its hook is called before
  // ours.  That includes our property sheets, whose template is comctl32's.
  CWnd * pWnd = CWnd::FromHandlePermanent (hwnd);
  if (pWnd)
    return !pWnd->IsKindOf (RUNTIME_CLASS (CCommonDialog));

  if (hInstance == (HINSTANCE) hUser32)
    {
    *pIsMessageBox = true;
    return true;
    }
  return hInstance == ::GetModuleHandle (NULL) || hInstance == AfxGetResourceHandle ();
  }

// Colours and themes: these work only once the control has been created.
void Colourise (HWND hwnd, eKind iKind)
  {
  switch (iKind)
    {
    case eButton:     SetWindowDark (hwnd, L"DarkMode_Explorer"); break;
    case eEdit:
      SetWindowDark (hwnd, (::GetWindowLong (hwnd, GWL_STYLE) & ES_MULTILINE)
                     ? L"DarkMode_Explorer" : L"DarkMode_CFD");
      break;
    case eCombo:
      {
      SetWindowDark (hwnd, L"DarkMode_CFD");
      COMBOBOXINFO info;
      ZeroMemory (&info, sizeof info);
      info.cbSize = sizeof info;
      if (::GetComboBoxInfo (hwnd, &info))
        {
        if (info.hwndList)
          SetWindowDark (info.hwndList, L"DarkMode_Explorer");
        if (info.hwndItem)
          SetWindowDark (info.hwndItem, L"DarkMode_CFD");
        }
      }
      break;
    case eListBox:    SetWindowDark (hwnd, L"DarkMode_Explorer"); break;
    case eListView:
      {
      SetWindowDark (hwnd, L"DarkMode_Explorer");
      ColourListView (hwnd, bDark);
      HWND hwndHeader = ListView_GetHeader (hwnd);
      if (hwndHeader)
        SetWindowDark (hwndHeader, L"ItemsView");
      }
      break;
    case eTree:
      SetWindowDark (hwnd, L"DarkMode_Explorer");
      ColourTreeView (hwnd, bDark);
      break;
    case eToolbar:    SetWindowDark (hwnd, L"DarkMode_Explorer"); break;
    case eScrollBar:  SetWindowDark (hwnd, L"DarkMode_Explorer"); break;
    case eUpDown:     SetWindowDark (hwnd, L"DarkMode_Explorer"); break;
    case eToolTip:    SetWindowDark (hwnd, L"DarkMode_Explorer"); break;
    case eScrollHost: SetWindowDark (hwnd, L"DarkMode_Explorer"); break;
    case eMdiClient:  SetWindowDark (hwnd, L"DarkMode_Explorer"); break;
    case eDialog:
    case eMessageBox:
    case eTopLevel:
      SetTitleBarDark (hwnd);
      break;
    default:
      break;
    }
  }

// Applied when WM_CREATE comes back, for controls found by the hook.  Then it goes.
LRESULT CALLBACK CreateWatchProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR iKind)
  {
  if (msg == WM_CREATE || msg == WM_NCDESTROY || msg == WM_INITDIALOG)
    {
    const LRESULT iResult = ::DefSubclassProc (hwnd, msg, wParam, lParam);
    ::RemoveWindowSubclass (hwnd, CreateWatchProc, id);
    if (msg != WM_NCDESTROY)
      Colourise (hwnd, (eKind) iKind);
    return iResult;
    }
  return ::DefSubclassProc (hwnd, msg, wParam, lParam);
  }

void InstallSubclass (HWND hwnd, eKind iKind, DWORD_PTR iData)
  {
  SUBCLASSPROC proc = NULL;
  switch (iKind)
    {
    case eDialog:
    case eMessageBox: proc = DialogProc; break;
    case eButton:     proc = ButtonProc; break;
    case eTab:        proc = TabProc; break;
    case eStatusBar:  proc = StatusBarProc; break;
    case eToolbar:    proc = ToolbarProc; break;
    case eControlBar: proc = ControlBarProc; break;
    case eListView:   proc = ListViewProc; break;
    case eEdit:
    case eListBox:
    case eTree:
    case eMdiClient:
    case eScrollHost: proc = EdgeProc; break;
    case eTopLevel:   proc = FrameProc; break;
    case eMdiChild:   proc = MdiChildProc; break;
    default: break;
    }
  if (proc)
    ::SetWindowSubclass (hwnd, proc, 1, iData);
  }

// Decides what a window is, whether it is ours to touch, and installs what it needs.
// Returns the kind, or eNone if it is to be left alone.
eKind Adopt (HWND hwnd, HWND hwndParent, DWORD iStyle, HINSTANCE hInstance)
  {
  char sClass [64];
  if (!::GetClassNameA (hwnd, sClass, sizeof sClass))
    return eNone;

  const bool bTopLevel = !(iStyle & WS_CHILD);
  eKind iKind = Classify (sClass, bTopLevel);
  if (iKind == eNone)
    return eNone;

  if (iKind == eScrollHost && (::GetWindowLong (hwnd, GWL_EXSTYLE) & WS_EX_MDICHILD))
    iKind = eMdiChild;

  if (iKind == eToolTip)
    return iKind;   // owned by whoever it is shown for, and has nothing to subclass

  if (bTopLevel)
    {
    bool bIsMessageBox = false;
    if (iKind == eDialog)
      {
      if (!DialogIsOurs (hwnd, hInstance, &bIsMessageBox))
        return eNone;
      if (bIsMessageBox)
        iKind = eMessageBox;
      }
    else if (iKind != eTopLevel)
      return eNone;

    if (!IsMarkedRoot (hwnd))
      {
      ::SetWindowSubclass (hwnd, RootMarkerProc, 1, 0);
      InstallSubclass (hwnd, iKind, bIsMessageBox);
      }
    return iKind;
    }

  // children only if the root is one of ours
  HWND hwndRoot = hwndParent ? ::GetAncestor (hwndParent, GA_ROOT) : NULL;
  if (!IsMarkedRoot (hwndRoot))
    return eNone;

  DWORD_PTR iData;
  if (!::GetWindowSubclass (hwnd, RootMarkerProc, 2, &iData))
    {
    ::SetWindowSubclass (hwnd, RootMarkerProc, 2, 0);   // so that we do this only once
    InstallSubclass (hwnd, iKind, 0);
    }
  return iKind;
  }

LRESULT CALLBACK CbtHook (int nCode, WPARAM wParam, LPARAM lParam)
  {
  if (nCode == HCBT_CREATEWND && bDark)
    {
    HWND hwnd = (HWND) wParam;
    const CREATESTRUCTA * pCreate = ((CBT_CREATEWNDA *) lParam)->lpcs;
    const eKind iKind = Adopt (hwnd, pCreate->hwndParent, pCreate->style, pCreate->hInstance);
    if (iKind != eNone)
      ::SetWindowSubclass (hwnd, CreateWatchProc, 1, iKind);
    }
  return ::CallNextHookEx (hHook, nCode, wParam, lParam);
  }

// ---------------------------------------------------------------------------
// Applying a change of mind to what already exists

BOOL CALLBACK ApplyToChild (HWND hwnd, LPARAM)
  {
  const eKind iKind = Adopt (hwnd, ::GetParent (hwnd), ::GetWindowLong (hwnd, GWL_STYLE),
                             (HINSTANCE) ::GetWindowLongPtr (hwnd, GWLP_HINSTANCE));
  if (iKind != eNone)
    Colourise (hwnd, iKind);
  return TRUE;
  }

BOOL CALLBACK ApplyToTopLevel (HWND hwnd, LPARAM)
  {
  const eKind iKind = Adopt (hwnd, NULL, ::GetWindowLong (hwnd, GWL_STYLE),
                             (HINSTANCE) ::GetWindowLongPtr (hwnd, GWLP_HINSTANCE));
  if (iKind == eNone)
    return TRUE;

  Colourise (hwnd, iKind);
  ::EnumChildWindows (hwnd, ApplyToChild, 0);

  // SWP_FRAMECHANGED for the menu bar's and the title bar's sake
  ::SetWindowPos (hwnd, NULL, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  ::RedrawWindow (hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
  return TRUE;
  }

void SetAppMode ()
  {
  if (pSetPreferredAppMode)
    pSetPreferredAppMode (bDark ? eAppModeForceDark : eAppModeForceLight);
  else if (pAllowDarkModeForApp)
    pAllowDarkModeForApp (bDark);

  if (pRefreshImmersiveColorPolicyState)
    pRefreshImmersiveColorPolicyState ();
  if (pFlushMenuThemes)
    pFlushMenuThemes ();
  }

// Called after bDark may have changed.
void ApplyChange (bool bWasDark)
  {
  if (bDark == bWasDark)
    return;

  SetAppMode ();

  ::EnumThreadWindows (::GetCurrentThreadId (), ApplyToTopLevel, 0);

  if (pChangeHandler)
    pChangeHandler ();
  }

void Recalculate ()
  {
  const bool bWasDark = bDark;
  bDark = Appearance_ResolveDark (iPreference, bSupported && SystemIsDark (), bSupported);
  if (bDark)
    LoadBrushes ();
  ApplyChange (bWasDark);
  }

} // end of anonymous namespace

// ---------------------------------------------------------------------------
// The interface

void Appearance_Init (int iNewPreference)
  {
  iPreference = iNewPreference;
  FindApis ();

  bDark = Appearance_ResolveDark (iPreference, bSupported && SystemIsDark (), bSupported);

  // "Unsupported" has to mean untouched, or the file picker would change on 1803.
  if (!bSupported)
    return;

  if (bDark)
    {
    LoadBrushes ();
    SetAppMode ();
    }

  iHookThread = ::GetCurrentThreadId ();
  hHook = ::SetWindowsHookExA (WH_CBT, CbtHook, NULL, iHookThread);
  }

void Appearance_SetPreference (int iNewPreference)
  {
  iPreference = iNewPreference;
  if (bSupported)
    Recalculate ();
  }

bool Appearance_IsDark ()
  {
  return bDark;
  }

void Appearance_SetChangeHandler (void (* pfnHandler) ())
  {
  pChangeHandler = pfnHandler;
  }

void Appearance_OnSettingChange (LPCTSTR lpszSection)
  {
  if (bSupported && lpszSection && _tcsicmp (lpszSection, _T ("ImmersiveColorSet")) == 0)
    Recalculate ();
  }

COLORREF Appearance_FaceColour ()         { return kFace; }
COLORREF Appearance_ControlColour ()      { return kControl; }
COLORREF Appearance_TextColour ()         { return kText; }
COLORREF Appearance_DisabledTextColour () { return kDisabled; }
COLORREF Appearance_EdgeColour ()         { return kEdge; }
COLORREF Appearance_LinkColour ()         { return kLink; }
HBRUSH   Appearance_FaceBrush ()          { LoadBrushes (); return hbrFace; }

// ---------------------------------------------------------------------------
// Toolbar images
//
// A bitmap toolbar with a recoloured bitmap can't draw disabled buttons: the control
// makes its etched effect by comparing pixels with COLOR_BTNFACE.  So it gets an image
// list with an alpha channel, and a second one for its disabled buttons.

static HBITMAP Make32BitBitmap (const std::vector<unsigned int> & pixels, int iWidth, int iHeight)
  {
  BITMAPINFO bmi;
  ZeroMemory (&bmi, sizeof bmi);
  bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
  bmi.bmiHeader.biWidth = iWidth;
  bmi.bmiHeader.biHeight = -iHeight;    // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void * pBits = NULL;
  HBITMAP hbm = ::CreateDIBSection (NULL, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
  if (hbm && pBits)
    memcpy (pBits, &pixels [0], pixels.size () * sizeof pixels [0]);
  return hbm;
  }

// One bit per pixel, set where the image is transparent.
static HBITMAP MakeMaskBitmap (const std::vector<unsigned int> & pixels, int iWidth, int iHeight)
  {
  const int iStride = ((iWidth + 15) / 16) * 2;
  std::vector<unsigned char> bits (iStride * iHeight, 0);
  for (int y = 0; y < iHeight; y++)
    for (int x = 0; x < iWidth; x++)
      if ((pixels [y * iWidth + x] >> 24) < 128)
        bits [y * iStride + x / 8] |= (unsigned char) (0x80 >> (x % 8));
  return ::CreateBitmap (iWidth, iHeight, 1, 1, &bits [0]);
  }

void Appearance_ApplyToolbarImages (HWND hToolbar, UINT iBitmapId, int iImageWidth, int iImageHeight, int iDpi)
  {
  const bool bHaveImageList = ::SendMessage (hToolbar, TB_GETIMAGELIST, 0, 0) != 0
                           && ::GetPropA (hToolbar, "McToolbarImages") != NULL;

  // A light client at 100%: the bitmap MFC loaded, as always.
  if (!bDark && iDpi == 96 && !bHaveImageList)
    return;

  HINSTANCE hInst = AfxFindResourceHandle (MAKEINTRESOURCE (iBitmapId), RT_BITMAP);
  HBITMAP hbmSource = (HBITMAP) ::LoadImage (hInst, MAKEINTRESOURCE (iBitmapId), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
  if (!hbmSource)
    return;

  BITMAP bm;
  ::GetObject (hbmSource, sizeof bm, &bm);

  std::vector<unsigned int> source (bm.bmWidth * bm.bmHeight);
  BITMAPINFO bmi;
  ZeroMemory (&bmi, sizeof bmi);
  bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
  bmi.bmiHeader.biWidth = bm.bmWidth;
  bmi.bmiHeader.biHeight = -bm.bmHeight;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  HDC hdc = ::CreateCompatibleDC (NULL);
  ::GetDIBits (hdc, hbmSource, 0, bm.bmHeight, &source [0], &bmi, DIB_RGB_COLORS);
  ::DeleteDC (hdc);
  ::DeleteObject (hbmSource);

  // The bitmaps' background is the silver that MFC's loader maps to the button face.
  for (size_t i = 0; i < source.size (); i++)
    {
    const unsigned int p = source [i] & 0xFFFFFF;
    if (p == 0xC0C0C0)
      source [i] = 0;
    else if (bDark)
      {
      // our pixels are BGRA in memory: 0xAARRGGBB as an integer
      const unsigned long iColourRef = ((p >> 16) & 0xFF) | (p & 0xFF00) | ((p & 0xFF) << 16);
      const unsigned long iNew = Appearance_ToolbarPixelForDark (iColourRef);
      source [i] = 0xFF000000 | ((iNew & 0xFF) << 16) | (iNew & 0xFF00) | ((iNew >> 16) & 0xFF);
      }
    else
      source [i] = 0xFF000000 | p;
    }

  const int iCount = iImageWidth > 0 ? bm.bmWidth / iImageWidth : 0;
  if (iCount <= 0)
    return;

  const int iScaledWidth = ::MulDiv (iImageWidth, iDpi, 96);
  const int iScaledHeight = ::MulDiv (iImageHeight, iDpi, 96);

  HIMAGELIST hNormal = ::ImageList_Create (iScaledWidth, iScaledHeight, ILC_COLOR32 | ILC_MASK, iCount, 1);
  HIMAGELIST hDisabled = ::ImageList_Create (iScaledWidth, iScaledHeight, ILC_COLOR32 | ILC_MASK, iCount, 1);

  std::vector<unsigned int> one (iImageWidth * iImageHeight);
  std::vector<unsigned int> scaled (iScaledWidth * iScaledHeight);

  for (int i = 0; i < iCount; i++)
    {
    for (int y = 0; y < iImageHeight; y++)
      memcpy (&one [y * iImageWidth], &source [y * bm.bmWidth + i * iImageWidth], iImageWidth * sizeof one [0]);

    Appearance_ScaleImage (&one [0], iImageWidth, iImageHeight, &scaled [0], iScaledWidth, iScaledHeight);

    HBITMAP hbm = Make32BitBitmap (scaled, iScaledWidth, iScaledHeight);
    HBITMAP hbmMask = MakeMaskBitmap (scaled, iScaledWidth, iScaledHeight);
    ::ImageList_Add (hNormal, hbm, hbmMask);
    ::DeleteObject (hbm);

    std::vector<unsigned int> grey (scaled);
    for (size_t j = 0; j < grey.size (); j++)
      grey [j] = Appearance_DisabledPixel (grey [j], bDark);
    hbm = Make32BitBitmap (grey, iScaledWidth, iScaledHeight);
    ::ImageList_Add (hDisabled, hbm, hbmMask);
    ::DeleteObject (hbm);
    ::DeleteObject (hbmMask);
    }

  HIMAGELIST hOldNormal = (HIMAGELIST) ::SendMessage (hToolbar, TB_SETIMAGELIST, 0, (LPARAM) hNormal);
  HIMAGELIST hOldDisabled = (HIMAGELIST) ::SendMessage (hToolbar, TB_SETDISABLEDIMAGELIST, 0, (LPARAM) hDisabled);
  if (bHaveImageList)
    {
    if (hOldNormal)
      ::ImageList_Destroy (hOldNormal);
    if (hOldDisabled)
      ::ImageList_Destroy (hOldDisabled);
    }
  ::SetPropA (hToolbar, "McToolbarImages", (HANDLE) 1);
  ::InvalidateRect (hToolbar, NULL, TRUE);
  }
