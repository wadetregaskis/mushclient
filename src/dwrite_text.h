// dwrite_text.h : colour emoji and shaped text in the output window, on Windows.
//
// GDI goes on drawing nearly everything: it is what the client's looks are made of, it
// is fast, and DirectWrite can't do bitmap fonts at all.  Runs that GDI can't do justice
// to (emoji, combining marks, scripts that need shaping: unicode::Utf8NeedsShaping) come
// here instead.  DirectWrite and Direct2D are loaded by name, so that anything without them
// just keeps GDI, and a font they can't use (FixedSys, Terminal, Dina) says no and keeps it too.

#ifndef MUSHCLIENT_DWRITE_TEXT_H
#define MUSHCLIENT_DWRITE_TEXT_H

#ifdef _WIN32

namespace dwtext {

// Can this run be done here, in the font selected into hdc?  If so, its width in pixels.
// iScale is how many times over that font is being drawn.  If it's a bitmap font, which
// DirectWrite can't use, Consolas stands in at that size: font fallback then finds the
// emoji or the ideograph, which the bitmap font never had anyway.
bool Measure (HDC hdc, const WCHAR * pText, int iLength, int & iWidth, int iScale = 1, bool bAllowStandIn = false);

// Draws it at r.left, in the font selected into hdc, on the same baseline as GDI would, and
// clipped to r.  False, having drawn nothing, if GDI has to do it.
bool Draw (HDC hdc, const RECT & r, const WCHAR * pText, int iLength, COLORREF colour, int iScale = 1, bool bAllowStandIn = false);

}  // namespace dwtext

#endif  // _WIN32

#endif  // MUSHCLIENT_DWRITE_TEXT_H
