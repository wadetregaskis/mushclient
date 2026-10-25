// pal/cpu_raster.h — the headless CPU reference adapter for pal::ISurface.
//
// Pure, deterministic CPU rasterization: no toolkit, no OS calls, main-thread.
// Purpose (docs/PAL.md, S1): prove the contract is implementable headless,
// serve as the golden-test double for the world.Window* migration, and remain
// the seed for a future mushcore miniwindow implementation (Phase 4).
//
// S1 implements the contract's core with exact seam semantics:
//   lifecycle · readPixels/writePixels/getPixel (both DIB formats, bottom-up,
//   padded rows) · fillRect (solid/null/hatch/pattern) · invertRect ·
//   line (solid width 1, last pixel excluded) · shape(Rect) · 1:1 blit
//   (copy + exact colour-key; surface and PixelBuffer sources) · gradient ·
//   floodFill (border + surface) · text/fonts/dpi.
//
// Documented S1 gaps (deliberate no-ops until the S3 op families need them):
//   drawEdge; shape kinds other than Rect; polyline/polygon/bezier/arc;
//   pen widths > 1 and dash styles; and
//   BrushSpec::monoPattern, which fillSpan drops (it consults only
//   br.pattern) — so WindowImageOp over a WindowCreateImage image, and
//   WindowCircleOp / WindowPolygon / WindowRectOp brush styles 8-12, fill
//   nothing here.
//   Hatch phase is this adapter's own 8x8 grid (the Windows adapter is the
//   parity bearer for exact GDI hatch phase).
//
//   NOT gaps, despite what this list said until 2026-08-07: the MATRIX blit is
//   implemented (blit's `if (m)` arm), and so is the STRETCHED blit — nearest
//   neighbour, GDI's COLORONCOLOR rule.  Two stretch COMBINATIONS are still
//   declined, both matching the wx adapter's own S2 gaps so the two agree: a
//   stretch whose source rect was clipped, and a colour-KEYED stretch.
//
//   The stretch resamples but does not interpolate, so it will not reproduce
//   the wx adapter's HALFTONE (BlitQuality::Smooth) colours — deliberately.
//   A test over a stretch must assert PLACEMENT and EXTENT, never an
//   interpolated colour value; the shipping rasteriser is the parity bearer
//   for the latter.
//
// Because the gaps above are SILENT no-ops, never write a test whose passing
// condition is "nothing was drawn" — it pins the gap instead of the body.
//
// TEXT IS A TEST FONT: glyphs are a deterministic procedural pattern in a
// fixed cell (width = 3/5 of height) — NOT real typography. It exists so the
// text contract (draw/measure/metrics consistency, clipping, return width) is
// testable headlessly; every metric it reports is exact for what it draws.

#pragma once

#include <map>
#include "pal/isurface.h"

namespace pal {

class CpuRaster : public ISurface {
public:
    SurfaceId createSurface(Size, Colour background) override;
    void      destroySurface(SurfaceId) override;

    void   readPixels (SurfaceId, Rect, PixelBuffer& out, PixelFormat) override;
    void   writePixels(SurfaceId, Rect, const PixelBuffer&) override;
    Colour getPixel   (SurfaceId, Point) override;
    void   setPixel   (SurfaceId, Point, Colour) override;

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
    int         dpi() override { return 96; }   // reference adapter = Windows parity

private:
    struct Surf {
        Size size;
        // Internal store: BGRA32, bottom-up, padded rows, A pinned to 0 —
        // identical layout to PixelBuffer{BGRA32}, so 32-bit readback is a
        // straight row copy.
        std::vector<unsigned char> px;
        int stride = 0;
        unsigned char*       row(int y)       { return px.data() + (size_t)(size.h - 1 - y) * stride; }
        const unsigned char* row(int y) const { return px.data() + (size_t)(size.h - 1 - y) * stride; }
    };
    struct Font {
        FontSpec spec;
        int cellW = 0, cellH = 0;   // fixed monospace cell of the test font
    };

    Surf* surf(SurfaceId id);
    void  setPx(Surf&, int x, int y, Colour);           // bounds-checked
    Colour pxAt(const Surf&, int x, int y) const;       // no bounds check
    void  fillSpan(Surf&, int x0, int x1, int y, const BrushSpec&); // [x0,x1)

    std::map<std::uint32_t, Surf> m_surfaces;
    std::map<std::uint32_t, Font> m_fonts;
    std::uint32_t m_nextSurface = 1, m_nextFont = 1;
};

} // namespace pal
