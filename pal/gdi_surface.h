// pal/gdi_surface.h — the native Win32/GDI adapter for pal::ISurface.
//
// The Windows counterpart of platform/wx_surface.h. It draws through the SAME
// MFC CDC/GDI calls that CMiniWindow used directly before the S3 migration, so
// routing a miniwindow op family through pal::ISurface on Windows is
// behaviour-identical to the original code by construction: each seam op below
// emits the exact GDI call the corresponding WindowRectOp/… case emitted.
//
// Migration status (mirrors wx_surface's documented S2/S3 gaps): the ops on a
// migrated live path are implemented exactly; op families still drawing through
// pdc in CMiniWindow are stubbed here and land, matching their original GDI
// setup verbatim, when that family moves onto the seam. As of S3 the live path
// is WindowRectOp → fillRect(solid)/invertRect/drawEdge/floodFill (+ adopt).
//
// Compiled by the MSVC project (MUSHclient.vcxproj) only — the whole body is
// #ifdef _WIN32, so it is inert if ever seen by the CMake port build.

#pragma once
#ifdef _WIN32

#include <map>
#include <cstdint>
#include "pal/isurface.h"

class CDC;

namespace pal {

class GdiSurface : public ISurface {
public:
    GdiSurface() = default;
    ~GdiSurface() override;

    SurfaceId createSurface(Size, Colour background) override;
    void      destroySurface(SurfaceId) override;

    void   readPixels (SurfaceId, Rect, PixelBuffer& out, PixelFormat) override;
    void   writePixels(SurfaceId, Rect, const PixelBuffer&) override;
    Colour getPixel   (SurfaceId, Point) override;

    void fillRect  (SurfaceId, Rect, const BrushSpec&) override;
    void invertRect(SurfaceId, Rect) override;
    void drawEdge  (SurfaceId, Rect, int edge, int flags) override;
    void shape     (SurfaceId, ShapeKind, Rect, Point a, Point b,
                    const PenSpec&, const BrushSpec&) override;
    void line      (SurfaceId, Point, Point, const PenSpec&) override;
    void polyline  (SurfaceId, const Point*, size_t, const PenSpec&) override;
    void polygon   (SurfaceId, const Point*, size_t, const PenSpec&,
                    const BrushSpec&, PolyFill) override;
    void bezier    (SurfaceId, const Point*, size_t, const PenSpec&) override;
    void arc       (SurfaceId, Rect, Point a, Point b, const PenSpec&) override;
    void floodFill (SurfaceId, Point seed, Colour fill, Colour ref, FloodMode) override;
    void gradient  (SurfaceId, Rect, Colour from, Colour to, bool horizontal) override;

    void blit(SurfaceId dst, Rect dstRect, SurfaceId src, Rect srcRect,
              BlitMode, Colour key, BlitQuality, const Matrix2x3*) override;
    void blit(SurfaceId dst, Rect dstRect, const PixelBuffer& src, Rect srcRect,
              BlitMode, Colour key, BlitQuality, const Matrix2x3*) override;

    FontId      createFont(const FontSpec&) override;
    void        destroyFont(FontId) override;
    int         drawText(SurfaceId, FontId, const char* utf8, size_t len, Point,
                         Colour, const Rect* clip) override;
    Size        measureText(FontId, const char* utf8, size_t len) override;
    FontMetrics fontMetrics(FontId) override;
    int         dpi() override;

    // Wrap CMiniWindow's externally-owned offscreen CDC as a surface (the S3
    // migration bridge); the adapter draws into it but does not own/free it.
    SurfaceId adopt(CDC* dc, Size);

private:
    struct Surf {
        CDC* dc      = nullptr;      // the adopted (non-owned) offscreen DC
        Size size;
        bool adopted = false;
    };
    Surf* surf(SurfaceId id);

    std::map<std::uint32_t, Surf> m_surfaces;
    std::uint32_t m_nextSurface = 1;
};

} // namespace pal

// Install the GDI adapter as the S3-migration surface bridge (pal::g_surface +
// pal::g_adoptSurface/...). Call once from CMUSHclientApp::InitInstance.
void InstallGdiSurfaceBridge();

#endif // _WIN32
