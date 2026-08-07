// pal/isurface.h — the raster-surface seam of the Platform Abstraction Layer.
//
// An ISurface adapter owns per-miniwindow offscreen raster surfaces and fonts —
// nothing else. The design of record (scope, rationale, semantics, migration)
// is docs/PAL.md § "ISurface — design"; this header transcribes that contract.
// Highlights:
//   - ~22 stateless ops: pens/brushes/fonts are per-call arguments, never
//     retained adapter state.
//   - Region pixel I/O (readPixels/writePixels) preserves the EXACT Windows
//     DIB layout — BGR24/BGRA32, rows 4-byte aligned, bottom-up — so the
//     existing engine blend/filter loops consume seam buffers unmodified.
//   - Images do NOT cross the seam as handles: a loaded image is an
//     engine-owned PixelBuffer, passed as a blit source.
//   - Output-pane text is NOT behind this interface (styled lines ship via
//     IWorldView; each view rasterizes them itself).
//   - Coordinates: top-left origin, y down; Rect is {x, y, w, h} (right/bottom
//     exclusive, i.e. pixels x..x+w-1). Colour is COLORREF 0x00BBGGRR.
//
// Implemented by: the CPU reference adapter (pal/cpu_raster.h — headless,
// deterministic, used by tests and eventually mushcore), the wx adapter
// (mac/Linux, S2), and the native GDI adapter (Windows, S5).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "pal/pal_types.h"

namespace pal {

// ── pixel buffers ───────────────────────────────────────────────────────────
// EXACTLY the Windows DIB contract (deliberate — see docs/PAL.md):
//   BGR24:  3 bytes/pixel, [B,G,R]
//   BGRA32: 4 bytes/pixel, [B,G,R,A]  (surface readback pins A = 0; alpha is
//           meaningful only in engine-owned image buffers and brush patterns)
//   BGRA32_Alpha: as BGRA32 but the 4th byte is carried instead of pinned, in
//           BOTH directions. On readPixels that is Windows
//           BitBlt-into-32bpp-DIB semantics: real alpha when the source is a
//           32bpp image bitmap, whatever GDI left there for an ordinary
//           drawing surface — used by the alpha image ops
//           (WindowDrawImageAlpha / WindowGetImageAlpha) on temporarily-
//           adopted image sources. On writePixels it means the buffer IS an
//           image whose alpha is meaningful, which is how a loaded 32bpp PNG
//           becomes a surface (CBitmap::Attach). Plain BGRA32 pins either way.
//   rows padded to 4-byte boundaries; stored BOTTOM-UP (image row y lives at
//   buffer row h-1-y).
enum class PixelFormat : std::uint8_t { BGR24, BGRA32, BGRA32_Alpha };

struct PixelBuffer {
    std::vector<unsigned char> bytes;
    int         stride = 0;                       // bytes per row (4-byte aligned)
    Size        size;
    PixelFormat format = PixelFormat::BGR24;

    int bytesPerPixel() const { return format == PixelFormat::BGR24 ? 3 : 4; }
    static int strideFor(int w, PixelFormat f) {
        return (w * (f == PixelFormat::BGR24 ? 3 : 4) + 3) & ~3;
    }
    // The declared geometry is actually backed by bytes. Adapters MUST reject
    // buffers that fail this before trusting row() — the fields are plain data
    // and may arrive inconsistent (heap-overread otherwise).
    bool valid() const {
        return size.w > 0 && size.h > 0 &&
               stride >= size.w * bytesPerPixel() &&
               bytes.size() >= (size_t)stride * size.h;
    }
    // Pointer to IMAGE row y (0 = top). Bottom-up storage.
    unsigned char*       row(int y)       { return bytes.data() + (size_t)(size.h - 1 - y) * stride; }
    const unsigned char* row(int y) const { return bytes.data() + (size_t)(size.h - 1 - y) * stride; }
};

// ── pens / brushes ──────────────────────────────────────────────────────────
enum class PenStyle : std::uint8_t {
    Solid, Null, InsideFrame,          // any width
    Dash, Dot, DashDot, DashDotDot,    // width <= 1 only (validated engine-side)
};
enum class PenEndcap : std::uint8_t { Round, Square, Flat };
enum class PenJoin   : std::uint8_t { Round, Bevel, Miter };

struct PenSpec {
    PenStyle  style  = PenStyle::Solid;
    int       width  = 1;
    Colour    colour = 0;
    PenEndcap endcap = PenEndcap::Round;
    PenJoin   join   = PenJoin::Round;
};

enum class BrushStyle : std::uint8_t {
    Solid, Null,
    Hatch,       // one of the 6 GDI hatches (kind below)
    Pattern,     // tiled from surface origin. Two carriers:
                 //   • monoPattern != null → one of MUSHclient's 5 built-in 8x8
                 //     MONO patterns (WindowCircleOp/WindowPolygon brush styles
                 //     8-12): the adapter builds a mono CreatePatternBrush and
                 //     colours it via fg/bg with GDI mono-brush semantics
                 //     (SetTextColor(fg) / SetBkColor(bg), TRANSPARENT gaps when
                 //     fg == CLR (0xFFFFFFFF)). This is byte-exact to the original
                 //     on Windows — the adapter, not the engine, applies the 0/1
                 //     -> text/bk mapping, so no mapping is guessed off-Windows.
                 //   • pattern (PixelBuffer) != null → an arbitrary loaded image
                 //     tiled as a brush (WindowImageOp). The engine passes BGR24;
                 //     tiling is opaque, as GDI CreatePatternBrush (which ignores
                 //     text/bk colours — and alpha — for colour bitmaps).
};
enum class HatchKind : std::uint8_t {  // HS_* order
    Horizontal, Vertical, FDiagonal, BDiagonal, Cross, DiagCross,
};

struct BrushSpec {
    BrushStyle           style = BrushStyle::Solid;
    Colour               fg    = 0;   // hatch line colour; mono-pattern text colour
    Colour               bg    = 0;   // hatch gap colour; mono-pattern bk colour
    HatchKind            hatch = HatchKind::Horizontal;
    const unsigned char* monoPattern = nullptr;  // 8 bytes (8x8, MSB=left) or null
    const PixelBuffer*   pattern     = nullptr;   // non-owning; valid for the call only
};

// ── fonts / text ────────────────────────────────────────────────────────────
struct FontSpec {
    std::string face;
    int         pixelHeight = 0;       // already converted from points via dpi()
    int         weight      = 400;     // 700+ = bold
    bool        italic = false, underline = false, strikeout = false;
    int         charsetHint = 0;       // pass-through on Windows; others may ignore
    int         pitchAndFamilyHint = 0;
};

// The TEXTMETRIC-ish fields WindowFontInfo exposes, plus the realized face.
// Fidelity tiers (docs/PAL.md): height/ascent/descent/leadings/avg+max width
// MUST be real and consistent with rendering; the rest are best-effort.
struct FontMetrics {
    int height = 0, ascent = 0, descent = 0;
    int internalLeading = 0, externalLeading = 0;
    int aveCharWidth = 0, maxCharWidth = 0;
    int weight = 0, overhang = 0;
    int digitizedAspectX = 0, digitizedAspectY = 0;
    int firstChar = 0, lastChar = 0, defaultChar = 0, breakChar = 0;
    int italic = 0, underlined = 0, struckOut = 0;   // raw TEXTMETRIC bytes: GDI
                 // may set 255 (not 1); WindowFontInfo 16/17/18 returns them verbatim
    int pitchAndFamily = 0, charset = 0;
    std::string face;                  // realized face after substitution (FontInfo 21)
};

// ── op parameter enums ──────────────────────────────────────────────────────
enum class ShapeKind  : std::uint8_t { Rect, RoundRect, Ellipse, Chord, Pie };
enum class FloodMode  : std::uint8_t { Border, Surface };
enum class BlitMode   : std::uint8_t { Copy, TransparentKey };  // key: EXACT match
enum class BlitQuality: std::uint8_t { Fast, Smooth };          // Smooth = HALFTONE-class
enum class PolyFill   : std::uint8_t { Alternate, Winding };    // WindowPolygon's Winding flag

struct Matrix2x3 { double m11 = 1, m12 = 0, m21 = 0, m22 = 1, dx = 0, dy = 0; };

// ── the interface ───────────────────────────────────────────────────────────
class ISurface {
public:
    // ── content generation ──────────────────────────────────────────────────
    // A counter that changes whenever a surface's PIXELS change, so a consumer
    // can cache something expensive derived from them (the compositor's
    // transparency mask) and know when to throw it away.
    //
    // Returning 0 means "this adapter does not track it" and is the default, so
    // an adapter that has not implemented it forces callers to recompute every
    // time -- slow, never stale.  Callers MUST treat 0 as "no caching", not as
    // a generation number, or the first adapter without support silently
    // freezes every cache at whatever it saw first.
    virtual unsigned long long contentGeneration(SurfaceId) { return 0; }

    virtual ~ISurface() = default;

    // lifecycle
    virtual SurfaceId createSurface(Size, Colour background) = 0;
    virtual void      destroySurface(SurfaceId) = 0;

    // region pixel I/O — the direct-access path (never per-pixel virtuals)
    virtual void   readPixels (SurfaceId, Rect, PixelBuffer& out,
                               PixelFormat = PixelFormat::BGR24) = 0;
    virtual void   writePixels(SurfaceId, Rect, const PixelBuffer&) = 0;
    virtual Colour getPixel   (SurfaceId, Point) = 0;   // 0xFFFFFFFF (CLR_INVALID) out of bounds
    virtual void   setPixel   (SurfaceId, Point, Colour) = 0;   // single-pixel write (WindowSetPixel)

    // drawing — stateless; stroke AND fill in one call where GDI does
    virtual void fillRect  (SurfaceId, Rect, const BrushSpec&) = 0;
    virtual void invertRect(SurfaceId, Rect) = 0;                 // dest-NOT; the only ROP kept
    virtual void drawEdge  (SurfaceId, Rect, int edge, int flags) = 0;  // EDGE_*/BF_* system
                 // bevels only (Draw3dRect's two arbitrary colours decompose core-side)
    // chord/pie (and arc): a and b define rays from the ellipse centre; the arc
    // runs counter-clockwise from the a-ray to the b-ray (GDI convention);
    // degenerate rays follow GDI behaviour.
    virtual void shape   (SurfaceId, ShapeKind, Rect, Point a, Point b,
                          const PenSpec&, const BrushSpec&) = 0;
    virtual void line    (SurfaceId, Point, Point, const PenSpec&) = 0;
                 // normative: GDI last-pixel-exclusive (documented wx deviation accepted)
    virtual void polyline(SurfaceId, const Point*, size_t, const PenSpec&) = 0;
    virtual void polygon (SurfaceId, const Point*, size_t, const PenSpec&,
                          const BrushSpec&, PolyFill = PolyFill::Alternate) = 0;
    virtual void bezier  (SurfaceId, const Point*, size_t, const PenSpec&) = 0;
    virtual void arc     (SurfaceId, Rect, Point a, Point b, const PenSpec&) = 0;
    // border:  fill until hitting ref-coloured boundary (GDI FloodFill)
    // surface: fill while on ref colour (ExtFloodFill FLOODFILLSURFACE)
    virtual void floodFill(SurfaceId, Point seed, Colour fill, Colour ref, FloodMode) = 0;
    virtual void gradient (SurfaceId, Rect, Colour from, Colour to, bool horizontal) = 0;

    // blit — source is another surface or an engine-owned buffer.
    // matrix non-null => dstRect ignored: device coords come entirely from
    // M·src (GDI GM_ADVANCED convention); mode/quality still apply.
    virtual void blit(SurfaceId dst, Rect dstRect, SurfaceId src, Rect srcRect,
                      BlitMode, Colour key = 0, BlitQuality = BlitQuality::Fast,
                      const Matrix2x3* = nullptr) = 0;
    virtual void blit(SurfaceId dst, Rect dstRect, const PixelBuffer& src, Rect srcRect,
                      BlitMode, Colour key = 0, BlitQuality = BlitQuality::Fast,
                      const Matrix2x3* = nullptr) = 0;

    // text — UTF-8 bytes + length; draw and measure MUST use identical metrics.
    // Background is always transparent (WindowText hard-sets TRANSPARENT).
    // (const char*+len, not string_view, so this header stays C++11 — it is
    //  compiled by every platform's build, incl. the C++14 MSVC project.)
    virtual FontId      createFont(const FontSpec&) = 0;
    virtual void        destroyFont(FontId) = 0;
    virtual int         drawText(SurfaceId, FontId, const char* utf8, size_t len,
                                 Point, Colour, const Rect* clip) = 0;   // → width drawn
    virtual Size        measureText(FontId, const char* utf8, size_t len) = 0;
    virtual FontMetrics fontMetrics(FontId) = 0;
    virtual int         dpi() = 0;     // for points→pixels (constant per adapter)
};

} // namespace pal
