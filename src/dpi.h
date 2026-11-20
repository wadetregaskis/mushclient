// dpi.h : what scale the display is at.  Windows only: the port asks wxWidgets.
//
// The client declares per-monitor v2 awareness in its manifest.  What that means:
//
//  - Inside the view everything is PHYSICAL pixels: fonts are real sizes, text is sharp,
//    and m_FontHeight and the client rectangle are what they say.
//
//  - What plugins see is LOGICAL: 96 dpi, permanently and on every platform.  A miniwindow
//    of 100 x 50 is 100 x 50, wherever it is.  Text rectangles, GetInfo's sizes, the scroll
//    position and mouse positions all convert at the edge (CMUSHclientDoc::ToLogical and
//    ToPhysical), so a plugin written for 96 dpi lays out the same on everything.
//
// Miniwindows are drawn sharply over those logical coordinates: see CMiniWindow::Create.
//
// At 96 dpi every conversion is the identity and no drawing path changes.

#ifndef MUSHCLIENT_DPI_H
#define MUSHCLIENT_DPI_H

#ifdef _WIN32

// The dpi of the monitor a window is on: 96 for a dpi-unaware process (the test binary).
// MUSHCLIENT_TEST_DPI in the environment overrides it for every window, which exercises
// the output pane, miniwindows and toolbars at any scale on any monitor.  Windows goes on
// scaling the frame and dialogs for the real monitor.
int Dpi_ForWindow (HWND hwnd);

// The dpi of the primary monitor, as of when the process started.
int Dpi_ForSystem ();

// Where a window really is and how big, which the override above doesn't change: the
// dpi Windows has the window at, and the dpi of the monitor nearest a rectangle in
// screen coordinates.  For saving and restoring window positions (winplace.cpp).
int Dpi_ActualForWindow (HWND hwnd);
int Dpi_ActualForRect (const RECT & rect);

// Sets it, for world.Debug ("dpi 144").  0 goes back to asking the system; anything
// else is held to 48 .. 960, as MUSHCLIENT_TEST_DPI is.
void Dpi_SetOverride (const int iDpi);

// Is that override in force?
bool Dpi_Overridden ();

// GetSystemMetrics as it would answer at 96 dpi, for what plugins can ask.
int Dpi_LogicalSystemMetric (int iIndex);

// How many times over to draw a bitmap font for this dpi.  A raster face comes in a few
// fixed sizes and answers the same whatever size is asked for, so it has to be
// drawn at its own size and blown up: whole numbers only, or the strokes come out uneven.
inline int Dpi_RasterFactor (int iDpi)
  {
  const int iFactor = (iDpi + 48) / 96;
  return iFactor < 1 ? 1 : iFactor;
  }

#endif  // _WIN32

#endif  // MUSHCLIENT_DPI_H
