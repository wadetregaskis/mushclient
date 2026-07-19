// platform/gdi_headless.cpp — Phase 2 Unit B-2 (docs/PHASE2-RECOVERY.md §5/§6).
//
// The headless providers for the GDI-shim symbols that the real doc.cpp /
// doc_construct.cpp / Utilities.cpp reference under MUSHCLIENT_FULL_BUILD.  These
// are the symbols B-1 (e64b2cf) deliberately DEFERRED because — unlike the
// CMUSHclientDoc / dialog stubs it landed — the GDI classes CDC / CFont / CBitmap
// / CGdiObject have a DIFFERENT class *shape* under MUSHCLIENT_FULL_BUILD than
// without it (mfc_ui.h forks them: full build = declarations-only + extra members
// e.g. CDC::m_bPrinting + out-of-line virtual dtors → vtables; stub build = inline
// no-ops, no out-of-line symbols).  So these bodies MUST be compiled
// MUSHCLIENT_FULL_BUILD to match the shape doc.cpp calls them through.
//
// ─── WHY THIS TU IS NOT (YET) IN MUSHCORE ─────────────────────────────────────
// The current mushcore is built WITHOUT MUSHCLIENT_FULL_BUILD.  Adding a
// full-build GDI TU into it would put two incompatible CDC/CFont/CBitmap *shapes*
// in one library (ODR fork → the exact class of runtime SIGSEGV the memo warns
// about).  So this TU is NOT wired into MUSHCORE_SOURCES.  It is proven in
// isolation by the dedicated full-build test target `test_gdi_headless`
// (tests/CMakeLists.txt), and B-3 folds it into MUSHCORE_SOURCES at the atomic
// flip, when the whole library becomes MUSHCLIENT_FULL_BUILD and the fork closes.
//
// ─── WHAT THESE BODIES DO (and why there is no wx / no cpu_raster) ────────────
// On mushgui these same symbols are the wx-backed impls in wx_gdi_compat.cpp
// (real font metrics via wxFontMetrics, real DIBSection pixel buffers).  Headless
// there is no display and no wx, and — crucially — the reachable callers do not
// NEED real pixels or real metrics:
//
//   * The image path (::CreateDIBSection, CBitmap::~CBitmap, CBitmap vtable) is
//     DEAD headless.  Its only callers are Utilities.cpp's LoadPng / LoadPngMemory,
//     which are reached only from mushapp-only TUs (methods_output.cpp
//     SetBackgroundImage / SetForegroundImage, miniwindow.cpp) — none compiled
//     into mushcore.  Headless, SetBackground/ForegroundImage are the eOK stubs in
//     doc_gui_method_stubs.cpp and world.WindowLoadImage returns eNoSuchWindow
//     before ever reaching LoadPng.  So CreateDIBSection returning nullptr is
//     never observed (and LoadPng degrades to eUnableToLoadImage if it ever were).
//
//   * The font-metric path (CDC::CreateCompatibleDC / GetDeviceCaps / GetTextMetrics
//     / SelectObject, CFont::CreateFont / ~CFont, CDC::~CDC, the CDC/CFont/
//     CGdiObject vtables) IS executed headless: doc.cpp's ChangeFont /
//     ChangeInputFont run from methods_defaults.cpp / methods_commands.cpp
//     (world.SetInputFont) / scriptingoptions.cpp (world load), and the result
//     (m_FontHeight / m_FontWidth) is observable via world.GetInfo(212..215).
//     BUT the result is legitimately DEGENERATE headless: doc_construct.cpp's real
//     ctor already zeroes m_FontHeight/m_FontWidth, every headless read-site guards
//     zero (doc_shared.cpp's m_FontHeight divide is in the Windows #else branch;
//     world_debug.cpp / mushview.cpp / sendvw.cpp are mushapp-only) and no test
//     asserts the *value* (test_info only checks the VARIANT type).  So the correct
//     headless behaviour is: GetTextMetrics zeroes the TEXTMETRIC (→ m_FontHeight
//     = 0, matching the ctor) and the rest degrade benignly.  No wxFontMetrics, no
//     cpu_raster — a pixel rasteriser would compute nothing anyone reads.
//
// Return values mirror the mfc_ui.h non-full inline stubs exactly (GetDeviceCaps
// 0, CreateCompatibleDC/GetTextMetrics/CreateFont false, SelectObject/CreateDIBSection
// nullptr) so that when B-3 flips test_mfc_ui.cpp to the full build its existing
// GDI assertions (EXPECT_FALSE(GetTextMetrics), GetDeviceCaps==0, …) stay green.
// The ONE deviation — zeroing the TEXTMETRIC — is invisible to those assertions
// (they check only the bool return) and is required to avoid ChangeFont reading an
// uninitialised struct.
//
// ─── Duplicate audit ──────────────────────────────────────────────────────────
// Compiled ONLY into the standalone test target `test_gdi_headless` (never into
// mushcore, mushapp or mushgui — wx_gdi_compat.cpp owns these there).  Under
// MUSHCLIENT_FULL_BUILD the mfc_ui.h GDI members are declarations only, so these
// are the sole definitions in the test link; there is no other headless provider
// of CDC/CFont/CBitmap/CGdiObject out-of-line symbols (windows_stubs.cpp /
// doc_headless_stubs.cpp / doc_gui_method_stubs.cpp / doc_flip_gui_stubs.cpp
// provide none — verified by grep).

#if !defined(_WIN32) && defined(MUSHCLIENT_FULL_BUILD)

#include "stdafx.h"

#include <cstring>   // std::memset

// ════════════════════════════════════════════════════════════════════════════
// CGdiObject — base of CFont / CBitmap / CPen / CBrush.  Defining the virtual
// dtor emits `vtable for CGdiObject` (its key function).  CFont::~CFont chains
// into it, so it must exist even though nothing calls it directly.
// ════════════════════════════════════════════════════════════════════════════

CGdiObject::~CGdiObject () {}

// ════════════════════════════════════════════════════════════════════════════
// CFont — ChangeFont / ChangeInputFont (doc.cpp) + FixFont (Utilities.cpp).
// `new CFont` / `delete m_font[i]` need the (virtual) dtor → `vtable for CFont`;
// CreateFont configures the face on Windows/wx, no-op headless.
// ════════════════════════════════════════════════════════════════════════════

CFont::~CFont () {}

bool CFont::CreateFont (int /*nHeight*/, int /*nWidth*/, int /*nEscapement*/,
                        int /*nOrientation*/, int /*nWeight*/, BYTE /*bItalic*/,
                        BYTE /*bUnderline*/, BYTE /*cStrikeOut*/, BYTE /*nCharSet*/,
                        BYTE /*nOutPrecision*/, BYTE /*nClipPrecision*/,
                        BYTE /*nQuality*/, BYTE /*nPitchAndFamily*/,
                        LPCTSTR /*lpszFacename*/)
{
  return false;                     // no font realised headless
}

// ════════════════════════════════════════════════════════════════════════════
// CBitmap — LoadPng / LoadPngMemory (Utilities.cpp) stack a `CBitmap B_bmp;`
// (never used) + doc_construct.cpp's m_BackgroundBitmap / m_ForegroundBitmap
// members.  The dtor emits `vtable for CBitmap`.  DEAD headless (LoadPng is never
// reached), but its symbols must resolve for the flip to link.
// ════════════════════════════════════════════════════════════════════════════

CBitmap::~CBitmap () {}

// ════════════════════════════════════════════════════════════════════════════
// CDC — the screenless font-measurement DC in ChangeFont / ChangeInputFont /
// FixFont / LoadPng.  `CDC dc;` has a virtual dtor → `vtable for CDC`.
// ════════════════════════════════════════════════════════════════════════════

CDC::~CDC () {}

bool CDC::CreateCompatibleDC (CDC* /*pDC*/)
{
  return false;                     // no memory DC headless (return ignored by callers)
}

int CDC::GetDeviceCaps (int /*nIndex*/)
{
  return 0;                         // matches the mfc_ui.h non-full stub
}

CFont* CDC::SelectObject (CFont* /*pFont*/)
{
  return nullptr;                   // no previously-selected font
}

bool CDC::GetTextMetrics (TEXTMETRIC* lpMetrics)
{
  // Zero the struct so ChangeFont's `m_FontHeight = tm.tmHeight` reads 0 (the
  // value doc_construct.cpp's ctor already sets), NOT uninitialised stack.
  if (lpMetrics)
    std::memset (lpMetrics, 0, sizeof (*lpMetrics));
  return false;                     // no real metrics headless (mirrors the stub)
}

// ════════════════════════════════════════════════════════════════════════════
// ::CreateDIBSection — Utilities.cpp LoadPng / LoadPngMemory.  DEAD headless;
// returning nullptr makes LoadPng report eUnableToLoadImage were it ever called.
// (In the full build this has a real pixel-buffer impl in wx_gdi_compat.cpp.)
// ════════════════════════════════════════════════════════════════════════════

HBITMAP CreateDIBSection (HDC /*hdc*/, const BITMAPINFO* /*pbmi*/, UINT /*iUsage*/,
                          void** ppvBits, HANDLE /*hSection*/, DWORD /*dwOffset*/)
{
  if (ppvBits)
    *ppvBits = nullptr;
  return nullptr;
}

#endif // !_WIN32 && MUSHCLIENT_FULL_BUILD
