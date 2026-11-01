// appearance.h : light and dark chrome.
//
// What the user's colours paint -- the output window, the command box, notepads,
// plugin miniwindows -- is theirs, and nothing here touches it.  This is about
// what the OS would otherwise paint: frames, menus, toolbars, the status bar,
// dialogs.  The preference is Global Preferences' "Appearance": follow the system
// (the default), or force light or dark.
//
// Windows: appearance_win.cpp, which is the only file that knows about the
// undocumented parts of the theme API.  Anything older than Windows 10 1809 stays
// light.  macOS and Linux: platform/wx_appearance.cpp, which asks wxWidgets.
//
// world.GetSysColor and the rest of what plugins can see do NOT change: on Windows
// GetSysColor still answers the light palette in dark mode, so a plugin that draws
// with it looks as it always has.

#ifndef MUSHCLIENT_APPEARANCE_H
#define MUSHCLIENT_APPEARANCE_H

// App.m_iAppearance, and the order of the combo box's entries in Global Preferences.
enum
  {
  APPEARANCE_SYSTEM = 0,
  APPEARANCE_LIGHT  = 1,
  APPEARANCE_DARK   = 2,
  };

// ---------------------------------------------------------------------------
// Everything in this first part is plain arithmetic, on every platform, so that
// tests can reach it.

// Does the user's preference, on this system, come out dark?
bool Appearance_ResolveDark (int iPreference, bool bSystemDark, bool bDarkSupported);

// Windows 10 1809 is the first with any of the dark theme parts.
bool Appearance_WindowsBuildSupportsDark (unsigned long iBuild);

// DWMWA_USE_IMMERSIVE_DARK_MODE is 20, but 19 before Windows 10 20H1.
int Appearance_ImmersiveDarkAttribute (unsigned long iBuild);

// One pixel of a 16-colour toolbar bitmap, as it should appear on a dark toolbar.
// Black outlines become light, greys invert around the middle, dark colours are
// lifted until they can be seen, and bright colours are left as they were.
// Colours are 0x00BBGGRR (a COLORREF).
unsigned long Appearance_ToolbarPixelForDark (unsigned long iColour);

// A disabled toolbar button's image: pixels are 0xAARRGGBB, straight alpha.
unsigned int Appearance_DisabledPixel (unsigned int iPixel, bool bDark);

// Scales a straight-alpha 0xAARRGGBB image.  Integer factors stay crisp (each
// source pixel becomes a square block).  Anything else goes up to the next integer
// by blocks and then down by area averaging, so that toolbar icons at 150% stay
// recognisably what they were, rather than picking up uneven pixels.
void Appearance_ScaleImage (const unsigned int * pSrc, int iSrcWidth, int iSrcHeight,
                            unsigned int * pDst, int iDstWidth, int iDstHeight);

// ---------------------------------------------------------------------------
// The live part.

// Called once, as soon as the preference has been loaded, before any window exists.
void Appearance_Init (int iPreference);

// Called when Global Preferences changes it.  Repaints what is on the screen.
void Appearance_SetPreference (int iPreference);

// Is the chrome dark just now?
bool Appearance_IsDark ();

#ifdef _WIN32

// The palette for chrome this file does not paint itself (MDI tabs, splitter bars,
// the empty MDI background).  Only meaningful when Appearance_IsDark ().
COLORREF Appearance_FaceColour ();      // dialogs, toolbars, the frame
COLORREF Appearance_ControlColour ();   // edit boxes, lists
COLORREF Appearance_TextColour ();
COLORREF Appearance_DisabledTextColour ();
COLORREF Appearance_EdgeColour ();
COLORREF Appearance_LinkColour ();
HBRUSH   Appearance_FaceBrush ();

// Called after the chrome has changed, for what only the frame knows about.
void Appearance_SetChangeHandler (void (* pfnHandler) ());

// CMainFrame's WM_SETTINGCHANGE: the system's light/dark setting may have changed.
void Appearance_OnSettingChange (LPCTSTR lpszSection);

// Replaces a toolbar's image with one suited to the current appearance, and to the
// scale in iDpi.  At 96 dpi in light mode it does nothing, unless an earlier call
// on this toolbar has already replaced something, so that a light client at 100%
// shows exactly the bitmap it always has.
void Appearance_ApplyToolbarImages (HWND hToolbar, UINT iBitmapId, int iImageWidth, int iImageHeight, int iDpi);

#endif  // _WIN32

#endif  // MUSHCLIENT_APPEARANCE_H
