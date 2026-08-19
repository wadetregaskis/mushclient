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
// ─── WHERE THIS TU LIVES POST-B-3 ──────────────────────────────────────────────
// mushcore itself is built WITH MUSHCLIENT_FULL_BUILD (the B-3 flip landed), and
// this TU is one of MUSHCORE_TESTSUPPORT_SOURCES (CMakeLists.txt) — the headless
// fakes linked into all 40 test executables, never into mushgui (wx_gdi_compat.cpp
// owns these symbols there instead; the two never co-link).  `test_gdi_headless`
// still exists as one of those 40 executables, but this is not a standalone
// target — every other test binary that touches CDC/CFont/CBitmap gets its
// definitions from here too.
//
// ─── WHAT THESE BODIES DO (and why there is no wx / no cpu_raster) ────────────
// On mushgui these same symbols are the wx-backed impls in wx_gdi_compat.cpp
// (real font metrics via wxFontMetrics, real DIBSection pixel buffers).  Headless
// there is no display and no wx, and — crucially — the reachable callers do not
// NEED real pixels or real metrics:
//
//   * The image path was DEAD headless when this was written, on the reasoning
//     that LoadPng's only callers (methods_output.cpp SetBackground/Foreground-
//     Image, miniwindow.cpp) were mushapp-only.  BOTH are now in mushcore, so
//     that premise is gone: ::CreateDIBSection and CBitmap::Attach/GetBitmap are
//     real, in platform/headless_surface.cpp, and a PNG really does load, blit
//     and read back here.  Only CBitmap's dtor/vtable remain this file's business.
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
// Compiled into mushcore_testsupport, linked into every one of the 40 test
// executables (never into mushgui — wx_gdi_compat.cpp owns these symbols there,
// via mushgui_adapters; the two adapter sets never co-link).  Under
// MUSHCLIENT_FULL_BUILD the mfc_ui.h GDI members are declarations only, so these
// are the sole definitions in the test link; there is no other headless provider
// of CDC/CFont/CBitmap/CGdiObject out-of-line symbols (windows_stubs.cpp /
// doc_headless_stubs.cpp / doc_flip_gui_stubs.cpp provide none — verified by grep;
// doc_gui_method_stubs.cpp no longer exists, adopted 2026-08).

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

// Release the surface (2026-08-12): miniwindow.cpp deletes images with plain
// `delete` at five sites (image replace + the m_Images map teardown), and an
// empty dtor leaked each one's CpuRaster pixel buffer AND left a stale
// BitmapSurfaces() entry keyed by the freed address — which a heap-recycled
// CBitmap then aliased.  DeleteObject is a cheap map lookup and a no-op for
// never-registered bitmaps (LoadPng's stack B_bmp).
CBitmap::~CBitmap () { DeleteObject (); }

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

// ::CreateDIBSection and CBitmap::Attach / GetBitmap are the DIB STORE, and
// they live in platform/headless_surface.cpp with the rest of the surface
// bridge — they are the image family's pixel source, not part of this inert
// shim.  (They were one-liners here until the DIB store landed.)

// ════════════════════════════════════════════════════════════════════════════
// B-3: the FULL remainder of the GDI shim.  Under MUSHCLIENT_FULL_BUILD the
// mfc_ui.h GDI classes are declaration-only; test_mfc_ui.cpp (and, transitively,
// the adopted dialog/view TUs) reference the whole no-op drawing surface.  These
// out-of-line bodies mirror the mfc_ui.h non-full inline returns EXACTLY (so the
// existing GDI assertions in test_mfc_ui stay green) — the B-2 handoff note #2
// "re-run nm -u for GDI symbols beyond the 13 and add one-liners" realised.
// ════════════════════════════════════════════════════════════════════════════

// ── CBitmap ─────────────────────────────────────────────────────────────────
HBITMAP CBitmap::CreateCompatibleBitmap (void*, int, int)       { return nullptr; }
bool    CBitmap::CreateBitmap (int, int, UINT, UINT, const void*) { return false; }

// CreateCanvasBitmap / Attach / GetBitmap / DeleteObject and ::CreateDIBSection
// are NOT here: stage 2 gave them real bodies over a pal::CpuRaster surface, in
// platform/headless_surface.cpp.  They own pixels, which makes them the surface
// bridge rather than part of this inert shim.
//
// The two left above stay inert.  An earlier version of this comment said
// "nothing in mushcore calls either (CreateCompatibleBitmap is a mushview.cpp
// path, CreateBitmap a dialog one)".  BOTH halves were wrong, and the mistake
// is worth leaving recorded because it is the sort a reader would otherwise
// inherit:
//
//   * CreateBitmap has SIX unguarded callers in miniwindow.cpp, which is in
//     mushcore — the five built-in 8x8 mono brush patterns in ValidateBrushStyle
//     (:498-538) and CMiniWindow::CreateImage (:2000).  Only the canvas call at
//     :239 is inside `#ifdef _WIN32`; the port takes CreateCanvasBitmap there.
//   * CreateCompatibleBitmap is called from CMiniWindow::Resize (:4039).
//
// So the `false` is contract, but for a different reason than claimed: the port
// deliberately keeps the mono pattern in a PARALLEL portable store
// (m_MonoImages, miniwindow.cpp:2005-2011) so ImageOp can rebuild a real mono
// brush through pal::BrushSpec, and never reads the CBitmap.  The CBitmap
// failing is therefore by design, not a gap — the gap is one layer down, where
// pal::CpuRaster drops BrushSpec::monoPattern.
//
// Every drawing site in miniwindow.cpp is guarded `if (!srf) return …`, so a
// bitmap that realises no surface degrades to "the window exists, the pixels do
// not" rather than to an error.  Do NOT "improve" these into failure returns —
// those guarded arms answer eOK by design on both shipping platforms.
// tests/test_mfc_ui.cpp:2559-2560 pins both returns.

// ── CBrush (dtor is the vtable key function) ────────────────────────────────
CBrush::~CBrush () {}
bool CBrush::CreateSolidBrush (COLORREF)        { return false; }
bool CBrush::CreateHatchBrush (int, COLORREF)   { return false; }
bool CBrush::CreatePatternBrush (CBitmap*)      { return false; }
bool CBrush::CreateBrushIndirect (const LOGBRUSH*) { return false; }

// ── CPen (dtor is the vtable key function) ──────────────────────────────────
CPen::~CPen () {}
bool    CPen::CreatePen (int, int, COLORREF)    { return false; }
BOOL    CPen::Attach (HGDIOBJ)                  { return FALSE; }
HGDIOBJ CPen::Detach ()                         { return nullptr; }

// ── CFont (~CFont / CreateFont already above) ───────────────────────────────
bool CFont::CreateFontIndirect (const LOGFONT*)          { return false; }
bool CFont::CreatePointFont (int, LPCTSTR, void*)        { return false; }
int  CFont::GetLogFont (LOGFONT* lf)          { if (lf) *lf = {}; return 0; }
int  CFont::GetObject (int, LOGFONT* lf) const { if (lf) *lf = {}; return 0; }

// ── CDC drawing surface (all no-ops; the ~CDC / CreateCompatibleDC /
//    GetDeviceCaps(int) / SelectObject(CFont*) / GetTextMetrics are above) ────
CDC* CDC::FromHandle (HDC)                       { static CDC dc; return &dc; }

// SelectObject(CBitmap*) moved to platform/headless_surface.cpp — it is what
// associates a DC with the surface its bitmap owns.
CBrush*  CDC::SelectObject (CBrush*)             { return nullptr; }
CPen*    CDC::SelectObject (CPen*)               { return nullptr; }
CGdiObject* CDC::SelectStockObject (int)         { return nullptr; }

bool CDC::Rectangle (int, int, int, int)         { return false; }
bool CDC::Ellipse (int, int, int, int)           { return false; }
bool CDC::Ellipse (const CRect& r)               { return Ellipse (r.left, r.top, r.right, r.bottom); }
bool CDC::RoundRect (const CRect&, CPoint)       { return false; }
bool CDC::LineTo (int, int)                      { return false; }
bool CDC::MoveTo (int, int, POINT*)              { return false; }
bool CDC::Polyline (const POINT*, int)           { return false; }
bool CDC::Polygon (const POINT*, int)            { return false; }
bool CDC::Chord (const CRect&, CPoint, CPoint)   { return false; }
bool CDC::Pie (int, int, int, int, int, int, int, int) { return false; }
bool CDC::Pie (const CRect&, CPoint, CPoint)     { return false; }
bool CDC::FloodFill (int, int, COLORREF)         { return false; }
bool CDC::ExtFloodFill (int, int, COLORREF, UINT) { return false; }
bool CDC::InvertRect (const RECT*)               { return false; }
bool CDC::InvertRect (const CRect*)              { return false; }
// FillSolidRect moved to platform/headless_surface.cpp — it paints the
// miniwindow background through the selected bitmap's surface.
void CDC::Draw3dRect (int, int, int, int, COLORREF, COLORREF) {}
bool CDC::PatBlt (int, int, int, int, DWORD)     { return false; }
bool CDC::BitBlt (int, int, int, int, CDC*, int, int, DWORD) { return false; }
bool CDC::StretchBlt (int, int, int, int, CDC*, int, int, int, int, DWORD) { return false; }
BOOL CDC::SetPixelV (int, int, COLORREF)         { return FALSE; }
COLORREF CDC::GetPixel (int, int) const          { return 0; }

bool CDC::TextOut (int, int, LPCTSTR, int)       { return false; }
bool CDC::TextOut (int, int, const CString&)     { return false; }
bool CDC::ExtTextOut (int, int, UINT, LPCRECT, LPCTSTR, int, const int*) { return false; }
int  CDC::DrawText (LPCTSTR, int, LPRECT, UINT)  { return 0; }

COLORREF CDC::SetTextColor (COLORREF)            { return 0; }
COLORREF CDC::GetTextColor () const              { return 0; }
COLORREF CDC::SetBkColor (COLORREF)              { return 0; }
COLORREF CDC::GetBkColor () const                { return 0; }
int      CDC::SetBkMode (int)                    { return 0; }
int      CDC::GetBkMode () const                 { return 0; }
int      CDC::SetROP2 (int)                      { return 0; }
int      CDC::GetROP2 () const                   { return 0; }
UINT     CDC::SetTextAlign (UINT)                { return 0; }
UINT     CDC::GetTextAlign () const              { return 0; }
int      CDC::SetTextCharacterExtra (int)        { return 0; }
int      CDC::SetPolyFillMode (int)              { return 0; }
int      CDC::GetPolyFillMode () const           { return 0; }
int      CDC::SetStretchBltMode (int)            { return 0; }
int      CDC::GetStretchBltMode () const         { return 0; }
int      CDC::GetTextFace (CString& s) const     { s = ""; return 0; }

CSize  CDC::GetTextExtent (LPCTSTR, int)         { return {}; }
CPoint CDC::SetViewportOrg (int, int)            { return {}; }
CPoint CDC::SetWindowOrg (int, int)              { return {}; }

bool CDC::DeleteDC ()                            { return true; }
int  CDC::SaveDC ()                              { return 0; }
bool CDC::RestoreDC (int)                        { return false; }

CFont*   CDC::GetCurrentFont ()   const          { return nullptr; }
CBrush*  CDC::GetCurrentBrush ()  const          { return nullptr; }
CPen*    CDC::GetCurrentPen ()    const          { return nullptr; }
CBitmap* CDC::GetCurrentBitmap () const          { return nullptr; }

// ── Global GDI functions (full-build declares them; wx impls are mushgui) ────
HGDIOBJ SelectObject (HDC, HGDIOBJ)              { return nullptr; }
int     GetDeviceCaps (HDC, int)                 { return 0; }
// ::DeleteObject is in platform/headless_surface.cpp — it frees a DIB store
// handle, so it belongs with ::CreateDIBSection rather than here.
HPEN    ExtCreatePen (DWORD, DWORD, const LOGBRUSH*, DWORD, const DWORD*) { return nullptr; }

#endif // !_WIN32 && MUSHCLIENT_FULL_BUILD
