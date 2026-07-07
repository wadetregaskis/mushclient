// pal/cpu_raster.cpp — headless CPU reference adapter for pal::ISurface.
// See cpu_raster.h for scope and the documented S1 gaps.

#include "pal/cpu_raster.h"

#include <algorithm>
#include <cstring>

namespace pal {

// ── colour helpers (COLORREF 0x00BBGGRR ↔ DIB byte order B,G,R[,A]) ─────────
static inline unsigned char colR(Colour c) { return (unsigned char)(c & 0xFF); }
static inline unsigned char colG(Colour c) { return (unsigned char)((c >> 8) & 0xFF); }
static inline unsigned char colB(Colour c) { return (unsigned char)((c >> 16) & 0xFF); }
static inline Colour makeCol(unsigned char r, unsigned char g, unsigned char b) {
    return (Colour)r | ((Colour)g << 8) | ((Colour)b << 16);
}
static const Colour kInvalidColour = 0xFFFFFFFFu;   // CLR_INVALID

// Clip rect to a surface's bounds; returns false if nothing remains.
// Sums in 64-bit: rects ultimately carry script-supplied coordinates, and
// x + w must not be signed-overflow UB.
static bool clipRect(Rect& r, Size bounds) {
    const long long rx1 = (long long)r.x + r.w, ry1 = (long long)r.y + r.h;
    int x0 = std::max(r.x, 0), y0 = std::max(r.y, 0);
    int x1 = (int)std::min<long long>(rx1, bounds.w);
    int y1 = (int)std::min<long long>(ry1, bounds.h);
    if (x1 <= x0 || y1 <= y0) return false;
    r = { x0, y0, x1 - x0, y1 - y0 };
    return true;
}

// ── surface bookkeeping ─────────────────────────────────────────────────────
CpuRaster::Surf* CpuRaster::surf(SurfaceId id) {
    auto it = m_surfaces.find((std::uint32_t)id);
    return it == m_surfaces.end() ? nullptr : &it->second;
}

Colour CpuRaster::pxAt(const Surf& s, int x, int y) const {
    const unsigned char* p = s.row(y) + (size_t)x * 4;
    return makeCol(p[2], p[1], p[0]);
}

void CpuRaster::setPx(Surf& s, int x, int y, Colour c) {
    if (x < 0 || y < 0 || x >= s.size.w || y >= s.size.h) return;
    unsigned char* p = s.row(y) + (size_t)x * 4;
    p[0] = colB(c); p[1] = colG(c); p[2] = colR(c); p[3] = 0;
}

SurfaceId CpuRaster::createSurface(Size sz, Colour background) {
    Surf s;
    s.size = { std::max(sz.w, 1), std::max(sz.h, 1) };   // zero dims → 1x1 (CreateBitmap rule)
    s.stride = PixelBuffer::strideFor(s.size.w, PixelFormat::BGRA32);
    s.px.assign((size_t)s.stride * s.size.h, 0);
    std::uint32_t id = m_nextSurface++;
    auto& stored = m_surfaces[id] = std::move(s);
    fillRect((SurfaceId)id, { 0, 0, stored.size.w, stored.size.h },
             BrushSpec{ BrushStyle::Solid, background });
    return (SurfaceId)id;
}

void CpuRaster::destroySurface(SurfaceId id) { m_surfaces.erase((std::uint32_t)id); }

// ── region pixel I/O ────────────────────────────────────────────────────────
void CpuRaster::readPixels(SurfaceId id, Rect r, PixelBuffer& out, PixelFormat fmt) {
    Surf* s = surf(id);
    out = PixelBuffer{};
    if (!s || !clipRect(r, s->size)) return;
    out.format = fmt;
    out.size   = { r.w, r.h };
    out.stride = PixelBuffer::strideFor(r.w, fmt);
    out.bytes.assign((size_t)out.stride * r.h, 0);
    const int bpp = out.bytesPerPixel();
    for (int y = 0; y < r.h; ++y) {
        const unsigned char* src = s->row(r.y + y) + (size_t)r.x * 4;
        unsigned char*       dst = out.row(y);
        for (int x = 0; x < r.w; ++x, src += 4, dst += bpp) {
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
            if (bpp == 4) dst[3] = 0;                     // surface alpha pinned 0
        }
    }
}

void CpuRaster::writePixels(SurfaceId id, Rect r, const PixelBuffer& in) {
    Surf* s = surf(id);
    if (!s || !in.valid()) return;   // declared geometry must be byte-backed
    // Only as much as both the rect and the buffer provide, clipped to bounds.
    Rect clipped = { r.x, r.y, std::min(r.w, in.size.w), std::min(r.h, in.size.h) };
    if (!clipRect(clipped, s->size)) return;
    const int bpp = in.bytesPerPixel();
    for (int y = 0; y < clipped.h; ++y) {
        const unsigned char* src = in.row((clipped.y - r.y) + y)
                                   + (size_t)(clipped.x - r.x) * bpp;
        unsigned char* dst = s->row(clipped.y + y) + (size_t)clipped.x * 4;
        for (int x = 0; x < clipped.w; ++x, src += bpp, dst += 4) {
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 0;
        }
    }
}

Colour CpuRaster::getPixel(SurfaceId id, Point p) {
    Surf* s = surf(id);
    if (!s || p.x < 0 || p.y < 0 || p.x >= s->size.w || p.y >= s->size.h)
        return kInvalidColour;
    return pxAt(*s, p.x, p.y);
}

// ── brushes ─────────────────────────────────────────────────────────────────
static bool hatchBit(HatchKind k, int x, int y) {
    // This adapter's own 8x8 grid; GDI phase parity is the Windows adapter's job.
    switch (k) {
    case HatchKind::Horizontal: return (y & 7) == 0;
    case HatchKind::Vertical:   return (x & 7) == 0;
    case HatchKind::FDiagonal:  return ((x - y) & 7) == 0;   // "\"
    case HatchKind::BDiagonal:  return ((x + y) & 7) == 0;   // "/"
    case HatchKind::Cross:      return (y & 7) == 0 || (x & 7) == 0;
    case HatchKind::DiagCross:  return ((x - y) & 7) == 0 || ((x + y) & 7) == 0;
    }
    return false;
}

void CpuRaster::fillSpan(Surf& s, int x0, int x1, int y, const BrushSpec& br) {
    switch (br.style) {
    case BrushStyle::Null:
        return;
    case BrushStyle::Solid:
        for (int x = x0; x < x1; ++x) setPx(s, x, y, br.fg);
        return;
    case BrushStyle::Hatch:
        for (int x = x0; x < x1; ++x) {
            if (hatchBit(br.hatch, x, y))    setPx(s, x, y, br.fg);
            else if (br.bg != (Colour)-1)    setPx(s, x, y, br.bg);
        }
        return;
    case BrushStyle::Pattern: {
        const PixelBuffer* pat = br.pattern;
        if (!pat || !pat->valid()) return;   // declared geometry must be byte-backed
        const int bpp = pat->bytesPerPixel();
        // Tiles from the SURFACE origin (GDI pattern-brush alignment).
        const unsigned char* prow = pat->row(y % pat->size.h);
        for (int x = x0; x < x1; ++x) {
            const unsigned char* p = prow + (size_t)(x % pat->size.w) * bpp;
            if (bpp == 4 && p[3] == 0) continue;          // alpha 0 = transparent gap
            setPx(s, x, y, makeCol(p[2], p[1], p[0]));
        }
        return;
    }
    }
}

void CpuRaster::fillRect(SurfaceId id, Rect r, const BrushSpec& br) {
    Surf* s = surf(id);
    if (!s || br.style == BrushStyle::Null || !clipRect(r, s->size)) return;
    for (int y = r.y; y < r.y + r.h; ++y)
        fillSpan(*s, r.x, r.x + r.w, y, br);
}

void CpuRaster::invertRect(SurfaceId id, Rect r) {
    Surf* s = surf(id);
    if (!s || !clipRect(r, s->size)) return;
    for (int y = r.y; y < r.y + r.h; ++y) {
        unsigned char* p = s->row(y) + (size_t)r.x * 4;
        for (int x = 0; x < r.w; ++x, p += 4) {
            p[0] = (unsigned char)~p[0]; p[1] = (unsigned char)~p[1];
            p[2] = (unsigned char)~p[2];                  // alpha untouched
        }
    }
}

// ── lines & shapes ──────────────────────────────────────────────────────────
void CpuRaster::line(SurfaceId id, Point a, Point b, const PenSpec& pen) {
    Surf* s = surf(id);
    if (!s || pen.style == PenStyle::Null) return;
    // S1: solid, width 1 (dash styles / widths > 1 are documented gaps).
    // Bresenham, EXCLUDING the final pixel (normative GDI LineTo rule).
    int x = a.x, y = a.y;
    const int dx = std::abs(b.x - a.x), sx = a.x < b.x ? 1 : -1;
    const int dy = -std::abs(b.y - a.y), sy = a.y < b.y ? 1 : -1;
    int err = dx + dy;
    while (x != b.x || y != b.y) {
        setPx(*s, x, y, pen.colour);
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}

void CpuRaster::shape(SurfaceId id, ShapeKind kind, Rect r, Point /*a*/, Point /*b*/,
                      const PenSpec& pen, const BrushSpec& br) {
    Surf* s = surf(id);
    if (!s) return;
    if (kind != ShapeKind::Rect) return;   // S1 gap: roundRect/ellipse/chord/pie
    Rect c = r;
    if (!clipRect(c, s->size)) return;
    const bool stroked = pen.style != PenStyle::Null;
    // Fill extent per GDI Rectangle(): inside the 1px border when stroked;
    // with NULL_PEN the whole figure is drawn one pixel smaller on the
    // right/bottom (the documented GDI null-pen shrink), i.e. (w-1) x (h-1)
    // anchored top-left. Loop ranges clamped to the surface (script-supplied
    // rects can be huge).
    // Both cases fill up to (exclusive) x+w-1 / y+h-1: when stroked that last
    // pixel is the border; with a null pen it is GDI's shrink.
    const int inset = stroked ? 1 : 0;
    const int fx0 = std::max(r.x + inset, 0), fx1 = std::min(r.x + r.w - 1, s->size.w);
    const int fy0 = std::max(r.y + inset, 0), fy1 = std::min(r.y + r.h - 1, s->size.h);
    for (int y = fy0; y < fy1; ++y)
        fillSpan(*s, fx0, fx1, y, br);
    if (stroked) {
        const int x0 = std::max(r.x, 0), x1 = std::min(r.x + r.w, s->size.w);
        const int y0 = std::max(r.y, 0), y1 = std::min(r.y + r.h, s->size.h);
        for (int x = x0; x < x1; ++x) {                   // top + bottom edges
            setPx(*s, x, r.y, pen.colour);
            setPx(*s, x, r.y + r.h - 1, pen.colour);
        }
        for (int y = y0; y < y1; ++y) {                   // left + right edges
            setPx(*s, r.x, y, pen.colour);
            setPx(*s, r.x + r.w - 1, y, pen.colour);
        }
    }
}

// S1 documented gaps — deliberate no-ops until their S3 op families land.
void CpuRaster::drawEdge(SurfaceId, Rect, int, int) {}
void CpuRaster::polyline(SurfaceId, const Point*, size_t, const PenSpec&) {}
void CpuRaster::polygon (SurfaceId, const Point*, size_t, const PenSpec&,
                         const BrushSpec&, PolyFill) {}
void CpuRaster::bezier  (SurfaceId, const Point*, size_t, const PenSpec&) {}
void CpuRaster::arc     (SurfaceId, Rect, Point, Point, const PenSpec&) {}

void CpuRaster::floodFill(SurfaceId id, Point seed, Colour fill, Colour ref, FloodMode mode) {
    Surf* s = surf(id);
    if (!s || seed.x < 0 || seed.y < 0 || seed.x >= s->size.w || seed.y >= s->size.h)
        return;
    const Colour at = pxAt(*s, seed.x, seed.y);
    if (mode == FloodMode::Border  && at == ref) return;  // seeded on the boundary
    if (mode == FloodMode::Surface && at != ref) return;  // seeded off the surface colour
    std::vector<unsigned char> visited((size_t)s->size.w * s->size.h, 0);
    std::vector<Point> stack{ seed };
    while (!stack.empty()) {
        Point p = stack.back(); stack.pop_back();
        if (p.x < 0 || p.y < 0 || p.x >= s->size.w || p.y >= s->size.h) continue;
        unsigned char& v = visited[(size_t)p.y * s->size.w + p.x];
        if (v) continue;
        v = 1;
        const Colour c = pxAt(*s, p.x, p.y);
        if (mode == FloodMode::Border  ? (c == ref) : (c != ref)) continue;
        setPx(*s, p.x, p.y, fill);
        stack.push_back({ p.x + 1, p.y }); stack.push_back({ p.x - 1, p.y });
        stack.push_back({ p.x, p.y + 1 }); stack.push_back({ p.x, p.y - 1 });
    }
}

void CpuRaster::gradient(SurfaceId id, Rect r, Colour from, Colour to, bool horizontal) {
    Surf* s = surf(id);
    Rect c = r;
    if (!s || !clipRect(c, s->size)) return;
    const int n = horizontal ? r.w : r.h;                 // interpolate over the FULL rect
    auto lerpAt = [&](int i) -> Colour {
        const double t = n <= 1 ? 0.0 : (double)i / (n - 1);
        auto ch = [&](unsigned char f, unsigned char tt) {
            return (unsigned char)(f + (tt - f) * t + 0.5);
        };
        return makeCol(ch(colR(from), colR(to)), ch(colG(from), colG(to)),
                       ch(colB(from), colB(to)));
    };
    if (horizontal) {
        for (int x = c.x; x < c.x + c.w; ++x) {
            const Colour col = lerpAt(x - r.x);
            for (int y = c.y; y < c.y + c.h; ++y) setPx(*s, x, y, col);
        }
    } else {
        for (int y = c.y; y < c.y + c.h; ++y) {
            const Colour col = lerpAt(y - r.y);
            for (int x = c.x; x < c.x + c.w; ++x) setPx(*s, x, y, col);
        }
    }
}

// ── blits ───────────────────────────────────────────────────────────────────
// S1: 1:1 only (srcRect and dstRect the same size), Fast quality, no matrix.
void CpuRaster::blit(SurfaceId dstId, Rect dstRect, SurfaceId srcId, Rect srcRect,
                     BlitMode mode, Colour key, BlitQuality q, const Matrix2x3* m) {
    Surf* src = surf(srcId);
    if (!src) return;
    // Clip srcRect to the source ourselves so the destination placement moves
    // with it (readPixels rebases to 0,0) — a partially-outside srcRect draws
    // the in-bounds intersection at the right spot, exactly as the PixelBuffer
    // overload's per-pixel clipping does.
    Rect clipped = srcRect;
    if (!clipRect(clipped, src->size)) return;
    dstRect = { dstRect.x + (clipped.x - srcRect.x),
                dstRect.y + (clipped.y - srcRect.y), clipped.w, clipped.h };
    PixelBuffer buf;
    readPixels(srcId, clipped, buf, PixelFormat::BGRA32);
    blit(dstId, dstRect, buf, { 0, 0, buf.size.w, buf.size.h }, mode, key, q, m);
}

void CpuRaster::blit(SurfaceId dstId, Rect dstRect, const PixelBuffer& src, Rect srcRect,
                     BlitMode mode, Colour key, BlitQuality /*q*/, const Matrix2x3* m) {
    Surf* dst = surf(dstId);
    if (!dst || !src.valid() || m != nullptr) return;             // S1: no matrix
    if (dstRect.w != srcRect.w || dstRect.h != srcRect.h) return; // S1: no stretch
    const int bpp = src.bytesPerPixel();
    for (int y = 0; y < srcRect.h; ++y) {
        const int sy = srcRect.y + y;
        if (sy < 0 || sy >= src.size.h) continue;
        const unsigned char* sp = src.row(sy);
        for (int x = 0; x < srcRect.w; ++x) {
            const int sx = srcRect.x + x;
            if (sx < 0 || sx >= src.size.w) continue;
            const unsigned char* p = sp + (size_t)sx * bpp;
            const Colour c = makeCol(p[2], p[1], p[0]);
            if (mode == BlitMode::TransparentKey && c == key) continue;  // EXACT match
            setPx(*dst, dstRect.x + x, dstRect.y + y, c);
        }
    }
}

// ── text (the deterministic test font — see the header warning) ─────────────
static size_t decodeUtf8(std::string_view s, size_t& i) {
    // Returns the codepoint at byte i, advancing i. Invalid bytes → the byte value.
    const unsigned char b0 = (unsigned char)s[i];
    size_t cp = b0, extra = 0;
    if      ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; extra = 1; }
    else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; extra = 2; }
    else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; extra = 3; }
    ++i;
    for (; extra && i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80; --extra, ++i)
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    return cp;
}

FontId CpuRaster::createFont(const FontSpec& spec) {
    Font f;
    f.spec  = spec;
    f.cellH = spec.pixelHeight > 0 ? spec.pixelHeight : 10;
    f.cellW = std::max(1, f.cellH * 3 / 5);
    std::uint32_t id = m_nextFont++;
    m_fonts[id] = std::move(f);
    return (FontId)id;
}

void CpuRaster::destroyFont(FontId id) { m_fonts.erase((std::uint32_t)id); }

Size CpuRaster::measureText(FontId id, std::string_view utf8) {
    auto it = m_fonts.find((std::uint32_t)id);
    if (it == m_fonts.end()) return {};
    int n = 0;
    for (size_t i = 0; i < utf8.size();) { decodeUtf8(utf8, i); ++n; }
    return { n * it->second.cellW, it->second.cellH };
}

int CpuRaster::drawText(SurfaceId sid, FontId fid, std::string_view utf8, Point pt,
                        Colour colour, const Rect* clip) {
    Surf* s = surf(sid);
    auto it = m_fonts.find((std::uint32_t)fid);
    if (!s || it == m_fonts.end()) return 0;
    const Font& f = it->second;
    int cx = pt.x, n = 0;
    for (size_t i = 0; i < utf8.size();) {
        const size_t cp = decodeUtf8(utf8, i);
        ++n;
        // Deterministic procedural glyph: a cell-filling pattern seeded by the
        // codepoint. NOT typography — exists to make the contract testable.
        for (int gy = 0; gy < f.cellH; ++gy)
            for (int gx = 0; gx < f.cellW; ++gx) {
                if ((cp * 31 + (size_t)gx * 7 + (size_t)gy * 13) % 4) continue;
                const int x = cx + gx, y = pt.y + gy;
                if (clip && (x < clip->x || x >= clip->x + clip->w ||
                             y < clip->y || y >= clip->y + clip->h)) continue;
                setPx(*s, x, y, colour);
            }
        cx += f.cellW;
    }
    const int measured = n * f.cellW;
    return clip ? std::min(measured, clip->w) : measured;   // WindowText's rule
}

FontMetrics CpuRaster::fontMetrics(FontId id) {
    FontMetrics m;
    auto it = m_fonts.find((std::uint32_t)id);
    if (it == m_fonts.end()) return m;
    const Font& f = it->second;
    m.height  = f.cellH;
    m.descent = f.cellH / 5;
    m.ascent  = f.cellH - m.descent;
    m.aveCharWidth = m.maxCharWidth = f.cellW;
    m.weight  = f.spec.weight;
    m.italic  = f.spec.italic; m.underlined = f.spec.underline;
    m.struckOut = f.spec.strikeout;
    m.charset = f.spec.charsetHint;
    m.firstChar = 32; m.lastChar = 126; m.defaultChar = '?'; m.breakChar = ' ';
    m.face = "CpuRasterTest";
    return m;
}

} // namespace pal
