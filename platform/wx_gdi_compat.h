// platform/wx_gdi_compat.h
// Accessor functions that let wx_world_child.cpp and wx_mfc_impl.cpp extract
// live wxDC/wxBitmap objects from the opaque CDC/CBitmap impl pointers, without
// needing to know the CDCImpl struct layout.
//
// Implementations live in platform/wx_gdi_compat.cpp (compiled into mushgui).
// Only meaningful when MUSHCLIENT_WX_BACKEND is defined (mushgui target).

#pragma once

#ifndef _WIN32
#ifdef MUSHCLIENT_WX_BACKEND

// Forward-declare just enough for the prototypes below.  Including full
// wx headers here would drag them into every file that includes this header,
// which conflicts with the wx-before-MFC include ordering rule.
class wxDC;
class wxBitmap;
class CDC;
class CBitmap;

// Return the wxDC* that backs dc (a mushapp CDC with an active CDCImpl).
// Returns nullptr if dc is null or has no impl (e.g. a mushcore stub CDC).
wxDC*     WxDCFromCDC(CDC* dc);

// Return the wxBitmap* stored in bmp->m_impl.
// Returns nullptr if bmp is null or has no backing bitmap.
wxBitmap* WxBitmapFromCBitmap(CBitmap* bmp);

#endif // MUSHCLIENT_WX_BACKEND
#endif // !_WIN32
