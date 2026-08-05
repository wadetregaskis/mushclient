// platform/wx_surface.h — the wx/mac adapter for pal::ISurface (docs/PAL.md S2).
//
// Wraps the SAME shim machinery (CDC / CBitmap / CFont over wx, from
// wx_gdi_compat.cpp) that miniwindow.cpp drives directly today, so rendering
// through this adapter is behaviour-identical to the shipping product; S3 can
// then reroute CMiniWindow op-family by op-family onto ISurface without
// changing what appears on screen.
//
// Deliberate S2 choices (documented deviations / gaps):
//   - invertRect, getPixel, the colour-key blit, floodFill and hatch fills are
//     CPU-exact (read-merge-write over the DIB round-trip). Several wxDC ops
//     are unreliable or unimplemented on wxOSX — GetPixel returns black,
//     FloodFill is a no-op, hatch brushes antialias and drop the gap colour, and
//     the SRCINVERT/SRCAND ROP dance is shaky — so these reimplement the exact
//     GDI semantics against canonical bytes and match the CPU reference.
//   - gradient uses the engine's own per-strip fallback algorithm (exact
//     endpoints, matches the CPU reference) rather than wxDC::GradientFillLinear.
//   - line renders INCLUSIVE of the last pixel — the shim's already-shipped,
//     documented deviation from the GDI-normative exclusive rule.
//   - S2 gaps (no-ops / bails, to land with their S3 op families): pattern
//     brushes; pen endcap/join; matrix blits; keyed+stretched blits; a
//     buffer-source stretched blit; a stretched blit whose source rect was
//     clipped (needs a scaled shift). Hatch/pattern INSIDE a stroked shape()
//     still routes through the (imperfect on wxOSX) GDI brush — as the product
//     does today; fillRect hatch is the CPU-exact path.
//   - Non-UTF-8 text decodes via the font's charset hint (the decided legacy-
//     charset fix), falling back to Latin-1 exactly like the shim otherwise.
//   - Pen-STROKED shape output may vary by ±2/channel on readback (wxOSX /
//     CoreGraphics rendering — the pipeline the product has always drawn
//     with). Fills, blits, colour-keys and pixel I/O are byte-exact.
//   - dpi() returns the shim's GetDeviceCaps(LOGPIXELSY) — the same value the
//     product uses today (wxGetDisplayPPI: ~72 on a 1x display, 144 on a 2x
//     Retina Mac). Mac-native sizing, unchanged from the product; see PAL.md.
//
// Compiled into mushgui only (needs wx + the MUSHCLIENT_FULL_BUILD shim).

#pragma once
#ifndef _WIN32

#include <map>
#include "pal/isurface.h"

class CDC;
class CBitmap;
class CFont;

namespace pal {

class WxSurface : public ISurface {
public:
    WxSurface() = default;
    ~WxSurface() override;

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
    int         dpi() override;   // shim LOGPIXELSY (== product; Mac-native)

    unsigned long long contentGeneration(SurfaceId) override;

    // Wrap an externally-owned CDC as a surface (S3 migration bridge); the
    // adapter draws into it but does not own/free it.
    SurfaceId adopt(CDC* dc, Size);

private:
    struct Surf {
        CDC*     dc      = nullptr;  // CreateCompatibleDC(NULL), TA_LEFT|TA_TOP
        CBitmap* bmp     = nullptr;  // device-depth bitmap selected into dc
        CBitmap* oldBmp  = nullptr;  // swap-back on destroy
        Size     size;
        bool     adopted = false;    // wraps an externally-owned CDC (S3 bridge)
        // The value of the adapter-wide counter when these pixels last
        // changed.  Never 0 for a live surface, because 0 is the interface's
        // "not tracked" sentinel.
        unsigned long long gen = 0;
    };
    struct Font {
        CFont*   font = nullptr;
        FontSpec spec;
    };

    Surf* surf(SurfaceId id);
    // surf(), plus "this call is about to change the pixels".  Every mutating
    // op goes through THIS rather than surf(), which is what makes the counter
    // trustworthy: the ~50 Window* script methods all funnel into these few
    // adapter entry points, so there is one place to get right instead of one
    // per method -- and per-op coverage in the surface selftest catches a
    // mutator that forgets, which is the failure that would show up as a
    // miniwindow frozen at its previous contents.
    Surf* mutate(SurfaceId id);
    Font* font(FontId id);
    CDC*  measureDc();               // lazily-created 1x1 DC for measuring

    std::map<std::uint32_t, Surf> m_surfaces;

    // ONE counter for the whole adapter, not one per surface, so a generation
    // number identifies a content-state globally.  Per-surface counters would
    // both start at 1, and SurfaceIds are recycled -- so a cache keyed by
    // (surface, generation) could hit an entry belonging to a destroyed
    // surface that happened to have been mutated the same number of times.
    // That is a stale miniwindow, which looks like a plugin bug.
    unsigned long long m_genCounter = 0;
    std::map<std::uint32_t, Font> m_fonts;
    std::uint32_t m_nextSurface = 1, m_nextFont = 1;
    CDC* m_measureDc = nullptr;
};

} // namespace pal

// Install the wx adapter as the S3-migration surface bridge (pal::g_surface +
// pal::g_adoptSurface/...). Call once at frontend startup. Idempotent.
void InstallWxSurfaceBridge();

// Conformance selftest: runs the adapter through the contract goldens (the
// adapter-invariant subset of tests/test_pal_surface.cpp) inside a live wx
// app. Invoked via the hidden `--pal-surface-selftest` command-line flag
// (see wx_app.cpp); prints one line per check and a summary to stdout.
// Returns the number of failures.
int RunPalSurfaceSelfTest();

#endif // !_WIN32
