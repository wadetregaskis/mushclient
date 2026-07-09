// pal/gdi_surface.cpp — native Win32/GDI adapter for pal::ISurface (see the
// header for the migration-status contract). Compiled by MUSHclient.vcxproj
// only; the whole file is #ifdef _WIN32.
//
// Each live seam op emits the exact MFC/GDI call the pre-migration WindowRectOp
// case emitted, so behaviour on Windows is identical by construction:
//   fillRect(solid) : CDC::FillSolidRect  — same pixels as the old FillRect /
//                     FrameRect strips / Draw3dRect strips (all solid fills)
//   invertRect      : CDC::InvertRect      — old RectOp case 3
//   drawEdge        : CDC::DrawEdge        — old RectOp case 5
//   floodFill       : select solid brush + CDC::FloodFill / ExtFloodFill —
//                     old RectOp cases 6/7
// Op families still drawing through pdc in CMiniWindow are stubbed and land,
// matching their original GDI setup, when they move onto the seam.

#ifdef _WIN32

#include "stdafx.h"          // MFC (CDC/CBrush/CRect/…) — the project prefix header
#include "pal/gdi_surface.h"
#include "pal/surface_bridge.h"

namespace pal {

GdiSurface::~GdiSurface() {
    // Every held surface is adopted (wraps CMiniWindow's own CDC); nothing to
    // free — the engine owns the DC. Owned-surface teardown lands with
    // createSurface().
    m_surfaces.clear();
}

GdiSurface::Surf* GdiSurface::surf(SurfaceId id) {
    auto it = m_surfaces.find((std::uint32_t)id);
    return it == m_surfaces.end() ? nullptr : &it->second;
}

SurfaceId GdiSurface::adopt(CDC* dc, Size sz) {
    if (!dc) return SurfaceId::None;
    Surf s;
    s.dc      = dc;
    s.size    = { sz.w > 0 ? sz.w : 1, sz.h > 0 ? sz.h : 1 };
    s.adopted = true;
    std::uint32_t id = m_nextSurface++;
    m_surfaces[id] = s;
    return (SurfaceId)id;
}

void GdiSurface::destroySurface(SurfaceId id) {
    auto it = m_surfaces.find((std::uint32_t)id);
    if (it == m_surfaces.end()) return;
    // adopted surfaces wrap an externally-owned CDC — never free it.
    m_surfaces.erase(it);
}

// ── drawing (S3 live path) ───────────────────────────────────────────────────
void GdiSurface::fillRect(SurfaceId id, Rect r, const BrushSpec& b) {
    Surf* s = surf(id);
    if (!s || !s->dc || b.style == BrushStyle::Null) return;
    CRect rc(r.x, r.y, r.x + r.w, r.y + r.h);
    if (b.style == BrushStyle::Solid) {
        s->dc->FillSolidRect(rc, b.fg);   // == old FillRect / FrameRect+Draw3dRect strips
        return;
    }
    // Hatch/Pattern brushes are used only by op families not yet migrated to the
    // seam (shape()/WindowImageOp); they land, matching the original GDI brush
    // setup exactly, with those families.
}

void GdiSurface::invertRect(SurfaceId id, Rect r) {
    Surf* s = surf(id);
    if (!s || !s->dc) return;
    s->dc->InvertRect(CRect(r.x, r.y, r.x + r.w, r.y + r.h));   // == old RectOp case 3
}

void GdiSurface::drawEdge(SurfaceId id, Rect r, int edge, int flags) {
    Surf* s = surf(id);
    if (!s || !s->dc) return;
    s->dc->DrawEdge(CRect(r.x, r.y, r.x + r.w, r.y + r.h),
                    (UINT)edge, (UINT)flags);                   // == old RectOp case 5
}

void GdiSurface::floodFill(SurfaceId id, Point seed, Colour fill, Colour ref, FloodMode mode) {
    Surf* s = surf(id);
    if (!s || !s->dc) return;
    CBrush br;
    br.CreateSolidBrush(fill);
    CBrush* oldBrush = s->dc->SelectObject(&br);
    if (mode == FloodMode::Border)
        s->dc->FloodFill(seed.x, seed.y, ref);                          // == old case 6
    else
        s->dc->ExtFloodFill(seed.x, seed.y, ref, FLOODFILLSURFACE);     // == old case 7
    s->dc->SelectObject(oldBrush);
}

int GdiSurface::dpi() {
    HDC hdc = ::GetDC(NULL);
    int d = hdc ? ::GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc) ::ReleaseDC(NULL, hdc);
    return d;
}

// ── op families not yet on the seam — stubbed until their CMiniWindow family
//    migrates (each will map to the exact original GDI call, as above) ─────────
SurfaceId   GdiSurface::createSurface(Size, Colour) { return SurfaceId::None; }
void        GdiSurface::readPixels(SurfaceId, Rect, PixelBuffer& out, PixelFormat) { out = PixelBuffer{}; }
void        GdiSurface::writePixels(SurfaceId, Rect, const PixelBuffer&) {}
Colour      GdiSurface::getPixel(SurfaceId, Point) { return 0xFFFFFFFFu; }   // CLR_INVALID
void        GdiSurface::shape(SurfaceId, ShapeKind, Rect, Point, Point, const PenSpec&, const BrushSpec&) {}
void        GdiSurface::line(SurfaceId, Point, Point, const PenSpec&) {}
void        GdiSurface::polyline(SurfaceId, const Point*, size_t, const PenSpec&) {}
void        GdiSurface::polygon(SurfaceId, const Point*, size_t, const PenSpec&, const BrushSpec&, PolyFill) {}
void        GdiSurface::bezier(SurfaceId, const Point*, size_t, const PenSpec&) {}
void        GdiSurface::arc(SurfaceId, Rect, Point, Point, const PenSpec&) {}
void        GdiSurface::gradient(SurfaceId, Rect, Colour, Colour, bool) {}
void        GdiSurface::blit(SurfaceId, Rect, SurfaceId, Rect, BlitMode, Colour, BlitQuality, const Matrix2x3*) {}
void        GdiSurface::blit(SurfaceId, Rect, const PixelBuffer&, Rect, BlitMode, Colour, BlitQuality, const Matrix2x3*) {}
FontId      GdiSurface::createFont(const FontSpec&) { return FontId::None; }
void        GdiSurface::destroyFont(FontId) {}
int         GdiSurface::drawText(SurfaceId, FontId, const char*, size_t, Point, Colour, const Rect*) { return 0; }
Size        GdiSurface::measureText(FontId, const char*, size_t) { return Size{}; }
FontMetrics GdiSurface::fontMetrics(FontId) { return FontMetrics{}; }

} // namespace pal

// ── S3 migration bridge install ──────────────────────────────────────────────
// One adapter instance hosts every miniwindow's adopted surface.
static pal::GdiSurface* s_bridgeAdapter = nullptr;

static pal::SurfaceId gdiAdoptSurface(void* nativeCtx, int w, int h) {
    if (!s_bridgeAdapter || !nativeCtx) return pal::SurfaceId::None;
    return s_bridgeAdapter->adopt(reinterpret_cast<CDC*>(nativeCtx), { w, h });
}
static void gdiReleaseAdoptedSurface(pal::SurfaceId id) {
    if (s_bridgeAdapter && id != pal::SurfaceId::None)
        s_bridgeAdapter->destroySurface(id);
}

void InstallGdiSurfaceBridge() {
    if (s_bridgeAdapter) return;                 // idempotent
    s_bridgeAdapter               = new pal::GdiSurface();
    pal::g_surface                = s_bridgeAdapter;
    pal::g_adoptSurface           = &gdiAdoptSurface;
    pal::g_releaseAdoptedSurface  = &gdiReleaseAdoptedSurface;
}

#endif // _WIN32
