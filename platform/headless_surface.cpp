// platform/headless_surface.cpp — the headless pal::ISurface bridge.
//
// Stage 2 of the miniwindow adoption (docs/STUB-ADOPTION-2026-08.md §5).
//
// Stage 1 put miniwindow.cpp into mushcore and left the surface null, so every
// drawing site took its guarded `if (!srf)` arm: windows existed, their geometry
// and hotspots were real, and nothing was ever painted.  This installs a real
// adapter so the pixels are real too.
//
// Nothing here is new rasterisation.  pal::CpuRaster already implements the
// contract and is already conformance-tested (tests/test_pal_surface.cpp); all
// that was missing was an INSTALLER and an `adopt` that CMiniWindow's
// CDC-shaped canvas can be looked up through.
//
// ── where the pixels live, and why that ordering matters ────────────────────
//
// CMiniWindow::Create does this, in this order (miniwindow.cpp:234-259):
//
//     pdc = new CDC; pdc->CreateCompatibleDC (NULL);
//     m_Bitmap = new CBitmap;
//     m_Bitmap->CreateCanvasBitmap (w, h);
//     m_oldBitmap = pdc->SelectObject (m_Bitmap);
//     pdc->FillSolidRect (0, 0, w, h, backgroundColour);
//     m_surfaceId = pal::g_adoptSurface (pdc, w, h);
//
// The background fill happens BEFORE the adopt.  On wx that is fine — the fill
// lands in a real bitmap and `adopt` wraps that same bitmap — so a headless
// `adopt` that CREATED a surface would silently discard the background colour
// of every miniwindow.
//
// So the ownership here follows GDI's own model rather than the adopt call:
// the BITMAP owns the pixels, the DC merely selects it, and drawing goes to
// whatever bitmap is currently selected.  CreateCanvasBitmap makes the surface,
// SelectObject associates it with the DC, FillSolidRect paints through that
// association, and `adopt` only LOOKS UP what is already there.
//
// That also makes the lifetime unambiguous: the CBitmap owns the surface, so
// g_releaseAdoptedSurface is a no-op.  It has to be — CMiniWindow::Create
// releases the OLD surface id after already having DeleteObject'd the old
// bitmap, so destroying on release too would be a double free.

#ifndef _WIN32

#include "stdafx.h"
#include "MUSHclient.h"

#include "pal/cpu_raster.h"
#include "pal/surface_bridge.h"

#include <map>

namespace {

// One adapter hosts every miniwindow's surface, exactly as the wx bridge does
// (platform/wx_surface.cpp:969).
pal::CpuRaster* g_adapter = nullptr;

// CBitmap* -> the surface it owns.  Keyed on the object address rather than on
// anything inside it, so platform/mfc_ui.h's layout is untouched and the wx
// build is unaffected.
std::map<const void*, pal::SurfaceId>& BitmapSurfaces()
{
    static std::map<const void*, pal::SurfaceId> m;
    return m;
}

// CDC* -> the surface of the bitmap currently selected into it.
std::map<const void*, pal::SurfaceId>& DcSurfaces()
{
    static std::map<const void*, pal::SurfaceId> m;
    return m;
}

pal::SurfaceId LookUp(const std::map<const void*, pal::SurfaceId>& m, const void* key)
{
    auto it = m.find(key);
    return it == m.end() ? pal::SurfaceId::None : it->second;
}

// `adopt` does not create — see the header comment.  It answers with whatever
// surface the DC already has selected, so the background fill that ran a line
// earlier is preserved.
pal::SurfaceId HeadlessAdoptSurface(void* nativeCtx, int /*w*/, int /*h*/)
{
    if (!g_adapter || !nativeCtx) return pal::SurfaceId::None;
    return LookUp(DcSurfaces(), nativeCtx);
}

// Deliberately inert: the CBitmap owns the surface.  CMiniWindow::Create
// releases the old id AFTER deleting the old bitmap, so destroying here would
// be a double free.
void HeadlessReleaseAdoptedSurface(pal::SurfaceId) {}

// Installed at load.  Headless has no InitInstance to hang this off, and every
// platform is supposed to install an adapter at startup — the wx product does it
// from InitInstance, Windows from its own.  A file-scope constructor is the
// closest headless equivalent, and it is safe: the three globals it writes are
// constant-initialised to nullptr in the zero-init phase (surface_bridge.cpp),
// and nothing reads them until a test runs.
struct Installer {
    Installer()
    {
        if (g_adapter) return;                    // idempotent
        g_adapter                    = new pal::CpuRaster();
        pal::g_surface               = g_adapter;
        pal::g_adoptSurface          = &HeadlessAdoptSurface;
        pal::g_releaseAdoptedSurface = &HeadlessReleaseAdoptedSurface;
    }
};
Installer g_installer;

} // namespace

// Test hook.  The maps above are file-static on purpose — nothing in the engine
// should reach them — but one hazard is invisible without them: after a surface
// is destroyed, CpuRaster may hand the SAME id back to the next createSurface,
// at which point a DC still holding the stale association would silently paint
// into somebody else's canvas.  tests/test_gdi_headless.cpp drives exactly that.
pal::SurfaceId HeadlessSurfaceOfBitmapForTesting(const CBitmap* bmp)
{
    return LookUp(BitmapSurfaces(), bmp);
}

// ── the GDI entry points that own and select the pixels ─────────────────────
//
// These are declared in platform/mfc_ui.h under MUSHCLIENT_FULL_BUILD and were
// inert one-liners in platform/gdi_headless.cpp until now.  They live here
// rather than there because they are the surface bridge, not the GDI shim: the
// rest of gdi_headless.cpp stays honestly inert.

bool CBitmap::CreateCanvasBitmap(int nWidth, int nHeight)
{
    if (!g_adapter) return false;
    DeleteObject();                               // replacing an existing canvas

    // Black, not the miniwindow's background: the caller paints that itself
    // through FillSolidRect on the very next line, and inventing a colour here
    // would mask a FillSolidRect that stopped working.
    const pal::SurfaceId id = g_adapter->createSurface(
        { nWidth > 0 ? nWidth : 1, nHeight > 0 ? nHeight : 1 }, 0x000000);
    if (id == pal::SurfaceId::None) return false;
    BitmapSurfaces()[this] = id;
    return true;
}

bool CBitmap::DeleteObject()
{
    auto& m = BitmapSurfaces();
    auto it = m.find(this);
    if (it == m.end()) return false;

    // Any DC still pointing at this surface forgets it.
    //
    // DEFENSIVE, not a fix for a live bug, and the distinction is measured
    // rather than assumed: the hazard would need CpuRaster to recycle a
    // destroyed id, and it allocates monotonically.  So no mutation of these two
    // lines can fail a test — removing them survives the suite, which is exactly
    // what a reader should expect.  tests/test_gdi_headless.cpp pins the
    // invariant that makes it defensive (ids are not recycled) and says what
    // changes if that ever stops being true.
    for (auto dcIt = DcSurfaces().begin(); dcIt != DcSurfaces().end(); )
        dcIt = (dcIt->second == it->second) ? DcSurfaces().erase(dcIt) : std::next(dcIt);

    if (g_adapter) g_adapter->destroySurface(it->second);
    m.erase(it);
    return true;
}

BOOL CBitmap::Attach(HGDIOBJ)
{
    // Attaching a foreign handle has no meaning here: the headless canvas is a
    // CpuRaster surface, not an HBITMAP.  Only the image family reaches this,
    // and its pixels come through ::CreateDIBSection, which is still inert (the
    // DIB store is the remaining half of stage 2).
    return FALSE;
}

CBitmap* CDC::SelectObject(CBitmap* pBitmap)
{
    // Report the previously selected bitmap the way GDI does, so CMiniWindow's
    // `m_oldBitmap = pdc->SelectObject (m_Bitmap)` / restore pairing works.
    CBitmap* previous = nullptr;
    const pal::SurfaceId had = LookUp(DcSurfaces(), this);
    if (had != pal::SurfaceId::None)
        for (const auto& kv : BitmapSurfaces())
            if (kv.second == had) { previous = (CBitmap*) kv.first; break; }

    if (!pBitmap) { DcSurfaces().erase(this); return previous; }

    const pal::SurfaceId id = LookUp(BitmapSurfaces(), pBitmap);
    if (id == pal::SurfaceId::None) DcSurfaces().erase(this);
    else                           DcSurfaces()[this] = id;
    return previous;
}

void CDC::FillSolidRect(int x, int y, int cx, int cy, COLORREF clr)
{
    const pal::SurfaceId id = LookUp(DcSurfaces(), this);
    if (!g_adapter || id == pal::SurfaceId::None) return;
    g_adapter->fillRect(id, { x, y, cx, cy },
                        { pal::BrushStyle::Solid, (pal::Colour) clr });
}

void CDC::FillSolidRect(LPCRECT lpRect, COLORREF clr)
{
    if (lpRect)
        FillSolidRect(lpRect->left, lpRect->top,
                      lpRect->right - lpRect->left,
                      lpRect->bottom - lpRect->top, clr);
}

void CDC::FillSolidRect(const CRect* lpRect, COLORREF clr)
{
    if (lpRect)
        FillSolidRect(lpRect->left, lpRect->top,
                      lpRect->Width(), lpRect->Height(), clr);
}

#endif // !_WIN32
