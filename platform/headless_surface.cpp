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

#include <cstring>
#include <map>
#include <memory>
#include <utility>

namespace {

// One adapter hosts every miniwindow's surface, exactly as the wx bridge does
// (platform/wx_surface.cpp:969).
pal::CpuRaster* g_adapter = nullptr;

// What a CBitmap owns.  The geometry is carried alongside the id because
// CBitmap::GetBitmap has to report it and pal::ISurface has no size query —
// and because bmBitsPixel is load-bearing, not decoration: the three alpha ops
// (MergeImageAlpha / GetImageAlpha / DrawImageAlpha) refuse an image outright
// unless it reads 32.
struct BitmapRec {
    pal::SurfaceId id = pal::SurfaceId::None;
    int w = 0, h = 0;
    int bpp = 32;
};

// CBitmap* -> what it owns.  Keyed on the object address rather than on
// anything inside it, so platform/mfc_ui.h's layout is untouched and the wx
// build is unaffected.
std::map<const void*, BitmapRec>& BitmapSurfaces()
{
    static std::map<const void*, BitmapRec> m;
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

pal::SurfaceId SurfaceOfBitmap(const void* key)
{
    auto it = BitmapSurfaces().find(key);
    return it == BitmapSurfaces().end() ? pal::SurfaceId::None : it->second.id;
}

// ── the DIB store ───────────────────────────────────────────────────────────
//
// ::CreateDIBSection is the ONLY way a pixel ever enters this process from
// outside a surface: png_load_shared.cpp's LoadPng / LoadPngMemory decode
// straight into one, and CMiniWindow::ImageFromWindow memcpy's a surface
// readback into one.  In every case the very next thing the caller does is
// CBitmap::Attach, which is where the bytes become a surface.
//
// So a DIB here is a short-lived raw buffer with no drawing behaviour at all —
// deliberately less than the wx version, which additionally has to keep a
// wxBitmap in step because wx CDCs render through one.  Nothing renders through
// a DIB here; CpuRaster owns every drawable pixel.
struct DibSection {
    int  w = 0, h = 0;
    int  bytesPerPixel = 3;
    int  stride = 0;
    bool bottomUp = true;              // biHeight > 0, the DIB default
    std::vector<unsigned char> bytes;
};

std::map<HBITMAP, DibSection*>& Dibs()
{
    static std::map<HBITMAP, DibSection*> m;
    return m;
}

// Hand the DIB over to the caller and forget it, the way wx's
// MaterializeDIBSection does: Attach consumes the handle.
std::unique_ptr<DibSection> TakeDib(HGDIOBJ h)
{
    auto it = Dibs().find((HBITMAP) h);
    if (it == Dibs().end()) return nullptr;
    std::unique_ptr<DibSection> owned(it->second);
    Dibs().erase(it);
    return owned;
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
    return SurfaceOfBitmap(bmp);
}

// Test hook: how many CreateDIBSection handles are outstanding.  Every DIB is
// meant to be consumed by the Attach that follows it a line or two later, so a
// non-zero count after an image op is a leak — which is exactly what the wx
// version had before ::DeleteObject was implemented there (wx_gdi_compat.cpp:202).
size_t HeadlessLiveDibCountForTesting() { return Dibs().size(); }

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
    const int w = nWidth  > 0 ? nWidth  : 1;
    const int h = nHeight > 0 ? nHeight : 1;
    const pal::SurfaceId id = g_adapter->createSurface({ w, h }, 0x000000);
    if (id == pal::SurfaceId::None) return false;
    // 32 matches what the wx build reports for a canvas (wxBitmap::GetDepth on
    // a screen-compatible bitmap) and what CreateCompatibleBitmap gives on a
    // 32-bit display, which is every display MUSHclient runs on now.
    BitmapSurfaces()[this] = { id, w, h, 32 };
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
        dcIt = (dcIt->second == it->second.id) ? DcSurfaces().erase(dcIt) : std::next(dcIt);

    if (g_adapter) g_adapter->destroySurface(it->second.id);
    m.erase(it);
    m_hObject = nullptr;   // the guard token Attach set (see there)
    return true;
}

// The DIB store's other end: turn the raw buffer ::CreateDIBSection handed the
// loader into a real surface, so an image can be blitted, read back and
// measured like any other.
//
// wx materializes into a wxBitmap here (wx_gdi_compat.cpp:697) because its DCs
// draw through one.  The equivalent headless is a CpuRaster surface, which is
// also what makes the image usable as a BLIT SOURCE: DrawImage adopts the
// image's CDC and expects a surface id back, and adopt only looks up.
BOOL CBitmap::Attach(HGDIOBJ hObject)
{
    DeleteObject();                               // replacing whatever we had

    std::unique_ptr<DibSection> dib = TakeDib(hObject);
    if (!dib || !g_adapter) return FALSE;

    // 24bpp and 32bpp are the only two the loaders produce, and the only two
    // pal::PixelBuffer can carry.  Anything else would need a palette or a
    // 16-bit unpack that no caller in this program asks for; failing here is
    // honest, and the loader's own eUnableToLoadImage is what the user sees.
    const bool alpha = dib->bytesPerPixel == 4;
    if (!alpha && dib->bytesPerPixel != 3) return FALSE;

    // _Alpha, not plain BGRA32: the format is what tells an adapter to carry
    // the source's alpha rather than pin it, and a loaded RGBA PNG is precisely
    // the case where it is real.  (Same flag the wx adapter derives from the
    // format when it writes — platform/wx_surface.cpp:266.)
    const pal::PixelFormat fmt = alpha ? pal::PixelFormat::BGRA32_Alpha
                                       : pal::PixelFormat::BGR24;
    pal::PixelBuffer buf;
    buf.format = fmt;
    buf.size   = { dib->w, dib->h };
    buf.stride = dib->stride;

    // A positive biHeight (the DIB default, and what both loaders ask for) is
    // bottom-up — byte-identical to PixelBuffer's own layout, so the buffer
    // moves rather than copies.  A top-down DIB has to be flipped into it.
    if (dib->bottomUp)
        buf.bytes = std::move(dib->bytes);
    else {
        buf.bytes.assign((size_t) dib->stride * dib->h, 0);
        for (int y = 0; y < dib->h; ++y)
            std::memcpy(buf.bytes.data() + (size_t)(dib->h - 1 - y) * dib->stride,
                        dib->bytes.data() + (size_t) y * dib->stride,
                        (size_t) dib->stride);
    }
    if (!buf.valid()) return FALSE;

    const pal::SurfaceId id = g_adapter->createSurface({ dib->w, dib->h }, 0x000000);
    if (id == pal::SurfaceId::None) return FALSE;
    g_adapter->writePixels(id, { 0, 0, dib->w, dib->h }, buf);

    BitmapSurfaces()[this] = { id, dib->w, dib->h, alpha ? 32 : 24 };
    // Non-null ownership token, mirroring the wx build (wx_gdi_compat.cpp
    // CBitmap::Attach): the shared clear/replace guard reads m_hObject via
    // operator HGDIOBJ, so a null here made `if ((HBITMAP) bmp)` skip
    // DeleteObject in this build too.  Opaque — nothing dereferences it; the
    // surface record is keyed on `this`.
    m_hObject = (HGDIOBJ) this;
    return TRUE;
}

// Always zeroes first: the ten call sites in miniwindow.cpp declare `BITMAP bi;`
// uninitialised and several read bi.bmBitsPixel without checking the return, so
// a bare `return FALSE` would let them branch on stack garbage.
BOOL CBitmap::GetBitmap(BITMAP* pBitmap)
{
    if (!pBitmap) return FALSE;
    *pBitmap = {};

    auto it = BitmapSurfaces().find(this);
    if (it == BitmapSurfaces().end()) return FALSE;

    pBitmap->bmWidth      = it->second.w;
    pBitmap->bmHeight     = it->second.h;
    pBitmap->bmBitsPixel  = it->second.bpp;
    pBitmap->bmPlanes     = 1;
    pBitmap->bmWidthBytes = (it->second.w * (it->second.bpp / 8) + 3) & ~3;
    return TRUE;
}

CBitmap* CDC::SelectObject(CBitmap* pBitmap)
{
    // Report the previously selected bitmap the way GDI does, so CMiniWindow's
    // `m_oldBitmap = pdc->SelectObject (m_Bitmap)` / restore pairing works.
    CBitmap* previous = nullptr;
    const pal::SurfaceId had = LookUp(DcSurfaces(), this);
    if (had != pal::SurfaceId::None)
        for (const auto& kv : BitmapSurfaces())
            if (kv.second.id == had) { previous = (CBitmap*) kv.first; break; }

    if (!pBitmap) { DcSurfaces().erase(this); return previous; }

    const pal::SurfaceId id = SurfaceOfBitmap(pBitmap);
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

// ── ::CreateDIBSection ──────────────────────────────────────────────────────
//
// Allocate the raw buffer and hand back both it and a handle.  No surface is
// made here — a DIB is not drawable in this build; CBitmap::Attach is where the
// bytes become one.
//
// The geometry rules are Windows': biHeight's SIGN picks the row order (positive
// = bottom-up), rows pad to a 4-byte boundary, and a zero biBitCount means the
// caller wants the DC's depth, which headless is the 24bpp both loaders use.
HBITMAP CreateDIBSection(HDC /*hdc*/, const BITMAPINFO* pbmi, UINT /*iUsage*/,
                         void** ppvBits, HANDLE /*hSection*/, DWORD /*dwOffset*/)
{
    if (ppvBits) *ppvBits = nullptr;
    if (!pbmi || !ppvBits) return nullptr;

    const BITMAPINFOHEADER& hdr = pbmi->bmiHeader;
    const int w = hdr.biWidth  < 0 ? -(int) hdr.biWidth  : (int) hdr.biWidth;
    const int h = (int) hdr.biHeight < 0 ? -(int) hdr.biHeight : (int) hdr.biHeight;
    if (w <= 0 || h <= 0) return nullptr;

    const int bpp = hdr.biBitCount > 0 ? (int) hdr.biBitCount : 24;
    std::unique_ptr<DibSection> dib(new DibSection);
    dib->w             = w;
    dib->h             = h;
    dib->bottomUp      = (int) hdr.biHeight > 0;
    dib->bytesPerPixel = (bpp + 7) / 8;
    dib->stride        = (w * dib->bytesPerPixel + 3) & ~3;
    dib->bytes.assign((size_t) dib->stride * h, 0);

    *ppvBits = dib->bytes.data();
    HBITMAP handle = (HBITMAP) dib.get();
    Dibs()[handle] = dib.release();
    return handle;
}

// Free a ::CreateDIBSection handle that never reached Attach.  Nothing in
// mushcore calls this today (every DIB is consumed a line or two after it is
// made), but the symbol exists on the wx side for the same reason and leaving
// the headless store with no way to drop a handle would be a trap for whoever
// adds the next image op.
BOOL DeleteObject(HGDIOBJ hObject)
{
    return TakeDib(hObject) ? TRUE : FALSE;
}

#endif // !_WIN32
