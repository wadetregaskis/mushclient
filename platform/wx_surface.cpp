// platform/wx_surface.cpp — wx/mac adapter for pal::ISurface (docs/PAL.md S2).
// See wx_surface.h for the design notes, deviations and S2 gaps.
//
// Compiled into mushgui only. Include order: wx headers MUST come before
// MUSHclient headers (same rule as wx_gdi_compat.cpp).

#ifndef _WIN32

#define MUSHCLIENT_FULL_BUILD   // mfc_ui.h: use declarations, not inline stubs

// ── wxWidgets ────────────────────────────────────────────────────────────
#include <wx/wx.h>
#include <wx/strconv.h>
#include <wx/fontmap.h>

// ── MUSHclient platform headers ──────────────────────────────────────────
#include "stdafx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "platform/wx_surface.h"
#include "pal/surface_bridge.h"

// Windows-charset → wxFontEncoding mapper shared with the output pane
// (defined in wx_outputpane.cpp; the decided legacy-charset fix reuses it).
wxFontEncoding WinCharsetToEncoding(int cs);

// Raw BGRA read of a CDC's selected bitmap, bypassing wxDC::Blit (which
// composites on wxOSX). Defined in wx_gdi_compat.cpp; see the note there.
bool wxgdi_read_bgra(CDC* dc, int x, int y, int w, int h,
                     unsigned char* dst, int strideBytes);
bool wxgdi_write_bgra(CDC* dc, int x, int y, int w, int h,
                      const unsigned char* src, int strideBytes, bool hasAlpha);
double wx_canvas_scale();   // wx_gdi_compat.cpp: the display scale canvases are made at

namespace pal {

// ── helpers ─────────────────────────────────────────────────────────────────
static int BytesPerLine(int w, int bpp) { return ((w * bpp + 31) & ~31) / 8; }

static inline unsigned char cB(Colour c) { return (unsigned char)((c >> 16) & 0xFF); }
static inline unsigned char cG(Colour c) { return (unsigned char)((c >> 8) & 0xFF); }
static inline unsigned char cR(Colour c) { return (unsigned char)(c & 0xFF); }

// Hatch bit at surface-origin phase — identical to CpuRaster::hatchBit so the
// two adapters agree (the Windows adapter is the exact GDI-phase parity bearer).
static bool hatchBit(HatchKind k, int x, int y) {
    switch (k) {
    case HatchKind::Horizontal: return (y & 7) == 0;
    case HatchKind::Vertical:   return (x & 7) == 0;
    case HatchKind::FDiagonal:  return ((x - y) & 7) == 0;
    case HatchKind::BDiagonal:  return ((x + y) & 7) == 0;
    case HatchKind::Cross:      return (y & 7) == 0 || (x & 7) == 0;
    case HatchKind::DiagCross:  return ((x - y) & 7) == 0 || ((x + y) & 7) == 0;
    }
    return false;
}

static bool clipToBounds(Rect& r, Size bounds) {
    const long long rx1 = (long long)r.x + r.w, ry1 = (long long)r.y + r.h;
    int x0 = r.x > 0 ? r.x : 0, y0 = r.y > 0 ? r.y : 0;
    int x1 = (int)(rx1 < bounds.w ? rx1 : bounds.w);
    int y1 = (int)(ry1 < bounds.h ? ry1 : bounds.h);
    if (x1 <= x0 || y1 <= y0) return false;
    r = { x0, y0, x1 - x0, y1 - y0 };
    return true;
}

WxSurface::~WxSurface() {
    while (!m_surfaces.empty())
        destroySurface((SurfaceId)m_surfaces.begin()->first);
    while (!m_fonts.empty())
        destroyFont((FontId)m_fonts.begin()->first);
    delete m_measureDc;
}

WxSurface::Surf* WxSurface::surf(SurfaceId id) {
    auto it = m_surfaces.find((std::uint32_t)id);
    return it == m_surfaces.end() ? nullptr : &it->second;
}

// surf(), plus the record that these pixels are about to change.  See the
// header for why every mutator goes through here rather than surf().
WxSurface::Surf* WxSurface::mutate(SurfaceId id) {
    Surf* s = surf(id);
    if (s) s->gen = ++m_genCounter;
    return s;
}

unsigned long long WxSurface::contentGeneration(SurfaceId id) {
    Surf* s = surf(id);
    return s ? s->gen : 0;    // 0 is the interface's "no caching" sentinel
}

WxSurface::Font* WxSurface::font(FontId id) {
    auto it = m_fonts.find((std::uint32_t)id);
    return it == m_fonts.end() ? nullptr : &it->second;
}

CDC* WxSurface::measureDc() {
    if (!m_measureDc) {
        m_measureDc = new CDC;
        m_measureDc->CreateCompatibleDC(NULL);
        m_measureDc->SetTextAlign(TA_LEFT | TA_TOP);
    }
    return m_measureDc;
}

// ── lifecycle (the exact CMiniWindow::Create dance) ─────────────────────────
SurfaceId WxSurface::createSurface(Size sz, Colour background) {
    Surf s;
    s.size = { sz.w > 0 ? sz.w : 1, sz.h > 0 ? sz.h : 1 };
    s.dc = new CDC;
    s.dc->CreateCompatibleDC(NULL);
    s.dc->SetTextAlign(TA_LEFT | TA_TOP);
    s.bmp = new CBitmap;
    s.bmp->CreateBitmap(s.size.w, s.size.h, 1, GetDeviceCaps((*s.dc), BITSPIXEL), NULL);
    s.oldBmp = s.dc->SelectObject(s.bmp);
    s.dc->SetWindowOrg(0, 0);
    s.dc->FillSolidRect(0, 0, s.size.w, s.size.h, background);
    s.gen = ++m_genCounter;      // fresh content-state, never reused
    std::uint32_t id = m_nextSurface++;
    m_surfaces[id] = s;
    return (SurfaceId)id;
}

void WxSurface::destroySurface(SurfaceId id) {
    Surf* s = surf(id);
    if (!s) return;
    if (!s->adopted) {                      // adopted DCs are owned by the caller
        s->dc->SelectObject(s->oldBmp);
        s->bmp->DeleteObject();
        delete s->bmp;
        delete s->dc;
    }
    m_surfaces.erase((std::uint32_t)id);
}

// Wrap an existing CDC (the CMiniWindow's pdc) as a non-owning surface.
SurfaceId WxSurface::adopt(CDC* dc, Size sz) {
    if (!dc) return SurfaceId::None;
    Surf s;
    s.dc      = dc;
    s.size    = { sz.w > 0 ? sz.w : 1, sz.h > 0 ? sz.h : 1 };
    s.adopted = true;
    s.gen     = ++m_genCounter;  // ditto: an adopted DC starts a new state
    std::uint32_t id = m_nextSurface++;
    m_surfaces[id] = s;
    return (SurfaceId)id;
}

// ── region pixel I/O — the engine's temp-DIB BitBlt round-trip, verbatim ────
// The DIB the shim builds has EXACTLY the PixelBuffer layout (BGR24/BGRA32,
// bottom-up, 4-byte-aligned rows), so the copy out/in is row-wise memcpy.
void WxSurface::readPixels(SurfaceId id, Rect r, PixelBuffer& out, PixelFormat fmt) {
    Surf* s = surf(id);
    out = PixelBuffer{};
    if (!s || !clipToBounds(r, s->size)) return;

    if (fmt == PixelFormat::BGRA32_Alpha) {
        // Alpha-preserving read — straight off the DC's selected bitmap
        // (wxgdi_read_bgra). The BitBlt round-trip below is unusable here:
        // wxDC::Blit composites on wxOSX instead of copying bytes, which
        // would corrupt semi-transparent pixels. Windows BitBlt into a
        // 32bpp DIB copies the source's BGRA verbatim; this reproduces that.
        out.format = fmt;
        out.size   = { r.w, r.h };
        out.stride = PixelBuffer::strideFor(r.w, fmt);
        out.bytes.assign((size_t)out.stride * r.h, 0);
        if (!wxgdi_read_bgra(s->dc, r.x, r.y, r.w, r.h,
                             out.bytes.data(), out.stride))
            out = PixelBuffer{};
        return;
    }

    if (fmt == PixelFormat::BGR24) {
        // Route through the alpha path and drop alpha, rather than the
        // temp-DIB BitBlt below.  That BitBlt targets an UNSCALED 1x bitmap
        // (wx_gdi_compat.cpp CreateDIBSection), so on a HiDPI canvas -- and
        // miniwindow canvases are created at scale (CreateCanvasBitmap) -- it
        // resamples each 2x2 block of physical pixels down to one.  Every
        // read-modify-write op therefore softened the image it was editing, and
        // an 8x8 stipple could collapse to a single colour.  getPixel already
        // took this route and said why; every other BGR24 caller
        // (CMiniWindow::Filter, WindowGetImageAlpha and friends) did not.
        PixelBuffer rgba;
        readPixels(id, r, rgba, PixelFormat::BGRA32_Alpha);
        if (!rgba.valid()) return;

        out.format = fmt;
        out.size   = { r.w, r.h };
        out.stride = PixelBuffer::strideFor(r.w, fmt);
        out.bytes.assign((size_t)out.stride * r.h, 0);
        for (int y = 0; y < r.h; ++y) {
            const unsigned char* src = rgba.bytes.data() + (size_t)y * rgba.stride;
            unsigned char*       dst = out.bytes.data()  + (size_t)y * out.stride;
            for (int x = 0; x < r.w; ++x, src += 4, dst += 3) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
            }
        }
        return;
    }

    const int bpp = 32;
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize        = sizeof bmi;
    bmi.bmiHeader.biWidth       = r.w;
    bmi.bmiHeader.biHeight      = r.h;          // positive = bottom-up
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = bpp;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage   = r.h * BytesPerLine(r.w, bpp);

    unsigned char* bits = NULL;
    HBITMAP hbm = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    if (!hbm || !bits) return;
    CDC tmp;
    tmp.CreateCompatibleDC(s->dc);
    HBITMAP hOld = (HBITMAP)SelectObject(tmp.m_hDC, hbm);
    tmp.BitBlt(0, 0, r.w, r.h, s->dc, r.x, r.y, SRCCOPY);

    out.format = fmt;
    out.size   = { r.w, r.h };
    out.stride = PixelBuffer::strideFor(r.w, fmt);
    out.bytes.assign(bits, bits + bmi.bmiHeader.biSizeImage);
    if (fmt == PixelFormat::BGRA32)             // surface alpha pinned 0
        for (size_t i = 3; i < out.bytes.size(); i += 4) out.bytes[i] = 0;

    SelectObject(tmp.m_hDC, hOld);
    DeleteObject(hbm);
}

void WxSurface::writePixels(SurfaceId id, Rect r, const PixelBuffer& in) {
    Surf* s = mutate(id);
    if (!s || !in.valid()) return;
    Rect clipped = { r.x, r.y, r.w < in.size.w ? r.w : in.size.w,
                               r.h < in.size.h ? r.h : in.size.h };
    if (!clipToBounds(clipped, s->size)) return;

    // Write through the backing bitmap directly, the exact inverse of the read
    // path above.  The temp-DIB BitBlt below targets an UNSCALED 1x bitmap, so
    // on a HiDPI canvas blitting it back SCALES it up and interpolates: a
    // read-modify-write that now reads exactly would still have written a
    // softened result.  wxgdi_write_bgra replicates each logical pixel across
    // the scale x scale physical block it owns, so the round-trip is lossless.
    {
        const int srcBpp = in.bytesPerPixel();
        const int stride4 = clipped.w * 4;
        std::vector<unsigned char> rgba((size_t)stride4 * clipped.h, 0);
        for (int y = 0; y < clipped.h; ++y) {
            // in.row() is top-down over the logical buffer; wxgdi_write_bgra
            // wants bottom-up rows, matching wxgdi_read_bgra's output.
            const unsigned char* srow = in.row(y + (clipped.y - r.y))
                                      + (size_t)(clipped.x - r.x) * srcBpp;
            unsigned char* drow = rgba.data() + (size_t)(clipped.h - 1 - y) * stride4;
            for (int x = 0; x < clipped.w; ++x, srow += srcBpp, drow += 4) {
                drow[0] = srow[0]; drow[1] = srow[1]; drow[2] = srow[2];
                drow[3] = (srcBpp == 4) ? srow[3] : 0xFF;
            }
        }
        if (wxgdi_write_bgra(s->dc, clipped.x, clipped.y, clipped.w, clipped.h,
                             rgba.data(), stride4,
                             in.format == PixelFormat::BGRA32_Alpha))
            return;
        // else fall through to the legacy BitBlt path
    }

    const int bpp = (in.format == PixelFormat::BGR24) ? 24 : 32;
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize        = sizeof bmi;
    bmi.bmiHeader.biWidth       = in.size.w;
    bmi.bmiHeader.biHeight      = in.size.h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = bpp;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage   = in.size.h * BytesPerLine(in.size.w, bpp);

    unsigned char* bits = NULL;
    HBITMAP hbm = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    if (!hbm || !bits) return;
    const int dibStride = BytesPerLine(in.size.w, bpp);
    for (int y = 0; y < in.size.h; ++y)          // row copy (strides may differ)
        memcpy(bits + (size_t)(in.size.h - 1 - y) * dibStride, in.row(y),
               (size_t)in.size.w * in.bytesPerPixel());
    if (bpp == 32) {
        // Force alpha opaque: the surface has no alpha semantics, and the
        // shim's 32bpp DIB→wxBitmap sync honours buffer alpha — 0 would make
        // the whole blit invisibly transparent on wxOSX.
        for (size_t i = 3; i < bmi.bmiHeader.biSizeImage; i += 4) bits[i] = 0xFF;
    }

    CDC tmp;
    tmp.CreateCompatibleDC(s->dc);
    HBITMAP hOld = (HBITMAP)SelectObject(tmp.m_hDC, hbm);
    s->dc->BitBlt(clipped.x, clipped.y, clipped.w, clipped.h,
                  &tmp, clipped.x - r.x, clipped.y - r.y, SRCCOPY);
    SelectObject(tmp.m_hDC, hOld);
    DeleteObject(hbm);
    (void) dibStride;
}

Colour WxSurface::getPixel(SurfaceId id, Point p) {
    Surf* s = surf(id);
    if (!s || p.x < 0 || p.y < 0 || p.x >= s->size.w || p.y >= s->size.h)
        return 0xFFFFFFFFu;                     // CLR_INVALID
    // Answer from canonical bytes: the shim's CDC::GetPixel sits on
    // wxMemoryDC::GetPixel, which is unimplemented on wxOSX (returns false →
    // black).
    //
    // Read through the ALPHA path, not the BGR24 one.  BGR24 goes via a BitBlt
    // into a temporary 1x DIB, and on a HiDPI canvas that resamples a 2x2 block
    // of physical pixels down to one — so a probe on an antialiased edge, or
    // one pixel in from a fill, comes back as a blend rather than the colour
    // that was drawn.  The alpha path reads the backing bitmap directly and
    // samples the logical cell's top-left pixel, which keeps WindowGetPixel
    // exact.  Byte order matches: both give B,G,R in the first three bytes.
    PixelBuffer px;
    readPixels(id, { p.x, p.y, 1, 1 }, px, PixelFormat::BGRA32_Alpha);
    if (px.bytes.size() < 3) return 0xFFFFFFFFu;
    const unsigned char* b = px.row(0);
    return (Colour)b[2] | ((Colour)b[1] << 8) | ((Colour)b[0] << 16);
}

void WxSurface::setPixel(SurfaceId id, Point p, Colour c) {
    Surf* s = mutate(id);
    if (!s || p.x < 0 || p.y < 0 || p.x >= s->size.w || p.y >= s->size.h) return;
    // CPU-exact 1x1 write via the DIB round-trip — consistent with getPixel and
    // exact (wxOSX DrawPoint can antialias / subpixel-position, so SetPixel then
    // GetPixel would not round-trip). BGR24: b[0]=B, b[1]=G, b[2]=R.
    PixelBuffer px;
    px.format = PixelFormat::BGR24;
    px.size   = { 1, 1 };
    px.stride = PixelBuffer::strideFor(1, PixelFormat::BGR24);
    px.bytes.assign((size_t)px.stride, 0);
    unsigned char* b = px.row(0);
    b[0] = (unsigned char)((c >> 16) & 0xFF);
    b[1] = (unsigned char)((c >> 8) & 0xFF);
    b[2] = (unsigned char)(c & 0xFF);
    writePixels(id, { p.x, p.y, 1, 1 }, px);
}

// ── brushes / pens ──────────────────────────────────────────────────────────
static int hatchToHs(HatchKind k) {
    switch (k) {
    case HatchKind::Horizontal: return HS_HORIZONTAL;
    case HatchKind::Vertical:   return HS_VERTICAL;
    case HatchKind::FDiagonal:  return HS_FDIAGONAL;
    case HatchKind::BDiagonal:  return HS_BDIAGONAL;
    case HatchKind::Cross:      return HS_CROSS;
    case HatchKind::DiagCross:  return HS_DIAGCROSS;
    }
    return HS_HORIZONTAL;
}

static int penToPs(PenStyle st) {
    switch (st) {
    case PenStyle::Solid:       return PS_SOLID;
    case PenStyle::Null:        return PS_NULL;
    case PenStyle::InsideFrame: return PS_INSIDEFRAME;
    case PenStyle::Dash:        return PS_DASH;
    case PenStyle::Dot:         return PS_DOT;
    case PenStyle::DashDot:     return PS_DASHDOT;
    case PenStyle::DashDotDot:  return PS_DASHDOTDOT;
    }
    return PS_SOLID;
}

// Build a CBrush for the spec; returns false for a malformed spec. Sets up
// the DC hatch-gap state the way the engine does (bg colour with OPAQUE gaps,
// or TRANSPARENT gaps for bg == -1).
// patternBmp is caller-owned so a mono pattern brush's backing bitmap outlives
// the shape draw (the wxBrush stipple references it).
static bool makeBrush(CBrush& br, CDC* dc, const BrushSpec& spec, CBitmap& patternBmp) {
    switch (spec.style) {
    case BrushStyle::Solid:
        return br.CreateSolidBrush(spec.fg) != 0;
    case BrushStyle::Null: {
        LOGBRUSH lb; memset(&lb, 0, sizeof lb);
        lb.lbStyle = BS_NULL;
        return br.CreateBrushIndirect(&lb) != 0;
    }
    case BrushStyle::Hatch: {
        LOGBRUSH lb; memset(&lb, 0, sizeof lb);
        lb.lbStyle = BS_HATCHED;
        lb.lbColor = spec.fg;
        lb.lbHatch = hatchToHs(spec.hatch);
        if (spec.bg == (Colour)-1) dc->SetBkMode(TRANSPARENT);
        else { dc->SetBkMode(OPAQUE); dc->SetBkColor(spec.bg); }
        dc->SetTextColor(spec.fg);
        return br.CreateBrushIndirect(&lb) != 0;
    }
    case BrushStyle::Pattern: {
        if (spec.pattern) {
            // Arbitrary-image pattern (WindowImageOp): materialize the engine's
            // BGR24 buffer as a bitmap and tile it. GDI ignores the text/bk
            // colours for COLOUR pattern brushes; the wx stipple does too.
            const PixelBuffer* pat = spec.pattern;
            if (!pat->valid()) return false;
            BITMAPINFO bmi;
            memset(&bmi, 0, sizeof bmi);
            bmi.bmiHeader.biSize        = sizeof bmi;
            bmi.bmiHeader.biWidth       = pat->size.w;
            bmi.bmiHeader.biHeight      = pat->size.h;   // bottom-up, as the buffer
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 24;
            bmi.bmiHeader.biCompression = BI_RGB;
            bmi.bmiHeader.biSizeImage   = pat->size.h * BytesPerLine(pat->size.w, 24);
            if ((size_t)bmi.bmiHeader.biSizeImage > pat->bytes.size()) return false;
            unsigned char* bits = NULL;
            HBITMAP hbm = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS,
                                           (void**)&bits, NULL, 0);
            if (!hbm || !bits) return false;
            memcpy(bits, pat->bytes.data(), bmi.bmiHeader.biSizeImage);
            patternBmp.Attach((HGDIOBJ)hbm);     // materializes + frees the DIB
            return br.CreatePatternBrush(&patternBmp) != 0;
        }
        if (!spec.monoPattern) return false;
        // The 5 built-in mono patterns (WindowCircleOp/WindowPolygon 8-12).
        //
        // GDI colours a 1-bpp pattern brush FROM THE DC: 0 bits take the text
        // colour, 1 bits the background colour.  wx does not -- the shim
        // materialises the mono bits as a literal black/white image and
        // CreatePatternBrush ignores SetTextColor/SetBkColor entirely -- so
        // styles 8-12 used to come out black-and-white whatever colours the
        // plugin asked for.  Build the 8x8 tile ALREADY COLOURED instead, which
        // is cheap because it is only 64 pixels.
        //
        // Transparent gaps (bg == -1) are still not expressible this way: a
        // stipple has no mask here, so those fall back to painting the gap in
        // the foreground colour rather than leaving the destination alone.
        // Recorded in docs/WINDOWS-VALIDATION.md.
        {
            const Colour fg = spec.fg;
            const Colour bg = (spec.bg == (Colour)-1) ? spec.fg : spec.bg;

            // Build the tile at PHYSICAL resolution.  An 8x8 logical stipple on
            // a 2x canvas is scaled up by wx and its alternating pixels average
            // away -- kFine (0xAA/0x55) is a checkerboard and came out a single
            // flat colour.  Replicating each pattern pixel across the scale x
            // scale block, and telling the bitmap its logical size is still 8x8,
            // makes wx lay it down 1:1 on the physical grid.
            const double sc = wx_canvas_scale();
            int isc = (sc > 0) ? (int)(sc + 0.5) : 1;
            if (isc < 1) isc = 1;
            const int dim = 8 * isc;

            BITMAPINFO bmi;
            memset(&bmi, 0, sizeof bmi);
            bmi.bmiHeader.biSize        = sizeof bmi;
            bmi.bmiHeader.biWidth       = dim;
            bmi.bmiHeader.biHeight      = dim;           // bottom-up
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 24;
            bmi.bmiHeader.biCompression = BI_RGB;
            const int stride = (int) BytesPerLine(dim, 24);
            bmi.bmiHeader.biSizeImage   = dim * stride;

            unsigned char* bits = NULL;
            HBITMAP hbm = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS,
                                           (void**)&bits, NULL, 0);
            if (!hbm || !bits) return false;
            memset(bits, 0, bmi.bmiHeader.biSizeImage);

            for (int row = 0; row < 8; ++row) {
                const unsigned char bitsRow = spec.monoPattern[row];
                for (int ry = 0; ry < isc; ++ry) {
                    // The DIB is bottom-up; the pattern's first byte is its top row.
                    const int py = dim - 1 - (row * isc + ry);
                    unsigned char* out = bits + (size_t)py * stride;
                    for (int col = 0; col < 8; ++col) {
                        const bool set = (bitsRow & (0x80u >> col)) != 0;  // MSB = left
                        const Colour c = set ? bg : fg;                    // GDI: 1 -> bk
                        for (int rx = 0; rx < isc; ++rx) {
                            *out++ = (unsigned char)((c >> 16) & 0xFF);    // B
                            *out++ = (unsigned char)((c >>  8) & 0xFF);    // G
                            *out++ = (unsigned char)( c        & 0xFF);    // R
                        }
                    }
                }
            }
            patternBmp.Attach((HGDIOBJ)hbm);   // materializes + frees the DIB
            if (isc > 1) {
                if (wxBitmap* wb = static_cast<wxBitmap*>(patternBmp.m_impl))
                    if (wb->IsOk()) wb->SetScaleFactor((double) isc);
            }
            return br.CreatePatternBrush(&patternBmp) != 0;
        }
    }
    }
    return false;
}

void WxSurface::fillRect(SurfaceId id, Rect r, const BrushSpec& spec) {
    Surf* s = mutate(id);
    if (!s || spec.style == BrushStyle::Null) return;
    if (spec.style == BrushStyle::Pattern) return;       // S2 gap (with ImageOp family)
    if (spec.style == BrushStyle::Solid) {
        s->dc->FillSolidRect(r.x, r.y, r.w, r.h, spec.fg);
        return;
    }
    // Hatch: CPU-exact over the DIB round-trip — wxOSX hatch brushes antialias
    // the lines and drop the gap colour, so this rasterizes GDI's semantics
    // (fg on the hatch bits; bg in the gaps, or nothing when bg == -1).
    Rect c = r;
    if (!clipToBounds(c, s->size)) return;
    PixelBuffer buf;
    readPixels(id, c, buf, PixelFormat::BGR24);
    if (buf.bytes.empty()) return;
    for (int y = 0; y < c.h; ++y) {
        unsigned char* row = buf.row(y);
        for (int x = 0; x < c.w; ++x) {
            unsigned char* p = row + (size_t)x * 3;
            if (hatchBit(spec.hatch, c.x + x, c.y + y)) {
                p[0] = cB(spec.fg); p[1] = cG(spec.fg); p[2] = cR(spec.fg);
            } else if (spec.bg != (Colour)-1) {
                p[0] = cB(spec.bg); p[1] = cG(spec.bg); p[2] = cR(spec.bg);
            }   // else: transparent gap — leave the pixel
        }
    }
    writePixels(id, c, buf);
}

// CPU-exact destination-NOT (wx logical functions are unreliable on wxOSX).
void WxSurface::invertRect(SurfaceId id, Rect r) {
    Surf* s = mutate(id);
    if (!s || !clipToBounds(r, s->size)) return;
    PixelBuffer buf;
    readPixels(id, r, buf, PixelFormat::BGR24);
    if (buf.bytes.empty()) return;
    for (int y = 0; y < buf.size.h; ++y) {
        unsigned char* p = buf.row(y);
        for (int x = 0; x < buf.size.w * 3; ++x) p[x] = (unsigned char)~p[x];
    }
    writePixels(id, r, buf);
}

void WxSurface::drawEdge(SurfaceId id, Rect r, int edge, int flags) {
    Surf* s = mutate(id);
    if (!s) return;
    CRect rc(r.x, r.y, r.x + r.w, r.y + r.h);
    s->dc->DrawEdge(&rc, edge, flags);           // shim's approximation, as today
}

void WxSurface::shape(SurfaceId id, ShapeKind kind, Rect r, Point a, Point b,
                      const PenSpec& pen, const BrushSpec& brSpec) {
    Surf* s = mutate(id);
    if (!s) return;
    CPen cpen;
    cpen.CreatePen(penToPs(pen.style), pen.width, pen.colour);
    CBrush cbr;
    CBitmap patternBmp;
    if (!makeBrush(cbr, s->dc, brSpec, patternBmp)) {
        BrushSpec nullBr; nullBr.style = BrushStyle::Null;
        makeBrush(cbr, s->dc, nullBr, patternBmp);
    }
    CPen*   oldPen = s->dc->SelectObject(&cpen);
    CBrush* oldBr  = s->dc->SelectObject(&cbr);
    CRect rc(r.x, r.y, r.x + r.w, r.y + r.h);
    switch (kind) {
    case ShapeKind::Rect:      s->dc->Rectangle(rc); break;
    case ShapeKind::RoundRect: s->dc->RoundRect(rc, CPoint(a.x, a.y)); break;
    case ShapeKind::Ellipse:   s->dc->Ellipse(rc); break;
    case ShapeKind::Chord:     s->dc->Chord(rc, CPoint(a.x, a.y), CPoint(b.x, b.y)); break;
    case ShapeKind::Pie:       s->dc->Pie(rc, CPoint(a.x, a.y), CPoint(b.x, b.y)); break;
    }
    s->dc->SelectObject(oldPen);
    s->dc->SelectObject(oldBr);
}

void WxSurface::line(SurfaceId id, Point p1, Point p2, const PenSpec& pen) {
    Surf* s = mutate(id);
    if (!s || pen.style == PenStyle::Null) return;
    s->dc->SetBkMode(TRANSPARENT);               // == the old Line() SetBkMode (dashed gaps)
    CPen cpen;
    cpen.CreatePen(penToPs(pen.style), pen.width, pen.colour);
    CPen* old = s->dc->SelectObject(&cpen);
    s->dc->MoveTo(p1.x, p1.y);
    s->dc->LineTo(p2.x, p2.y);   // shim draws INCLUSIVE (documented deviation)
    s->dc->SelectObject(old);
}

void WxSurface::polyline(SurfaceId id, const Point* pts, size_t n, const PenSpec& pen) {
    Surf* s = mutate(id);
    if (!s || !pts || n < 2) return;
    CPen cpen;
    cpen.CreatePen(penToPs(pen.style), pen.width, pen.colour);
    CPen* old = s->dc->SelectObject(&cpen);
    std::vector<POINT> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = { pts[i].x, pts[i].y };
    s->dc->Polyline(v.data(), (int)n);
    s->dc->SelectObject(old);
}

void WxSurface::polygon(SurfaceId id, const Point* pts, size_t n, const PenSpec& pen,
                        const BrushSpec& brSpec, PolyFill fill) {
    Surf* s = mutate(id);
    if (!s || !pts || n < 2) return;
    CPen cpen;
    cpen.CreatePen(penToPs(pen.style), pen.width, pen.colour);
    CBrush cbr;
    CBitmap patternBmp;
    if (!makeBrush(cbr, s->dc, brSpec, patternBmp)) {
        BrushSpec nullBr; nullBr.style = BrushStyle::Null;
        makeBrush(cbr, s->dc, nullBr, patternBmp);
    }
    CPen*   oldPen = s->dc->SelectObject(&cpen);
    CBrush* oldBr  = s->dc->SelectObject(&cbr);
    s->dc->SetPolyFillMode(fill == PolyFill::Winding ? WINDING : ALTERNATE);
    std::vector<POINT> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = { pts[i].x, pts[i].y };
    s->dc->Polygon(v.data(), (int)n);
    s->dc->SelectObject(oldPen);
    s->dc->SelectObject(oldBr);
}

void WxSurface::bezier(SurfaceId id, const Point* pts, size_t n, const PenSpec& pen) {
    Surf* s = mutate(id);
    if (!s || !pts || n < 4) return;
    CPen cpen;
    cpen.CreatePen(penToPs(pen.style), pen.width, pen.colour);
    CPen* old = s->dc->SelectObject(&cpen);
    std::vector<POINT> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = { pts[i].x, pts[i].y };
    s->dc->PolyBezier(v.data(), (int)n);
    s->dc->SelectObject(old);
}

void WxSurface::arc(SurfaceId id, Rect r, Point a, Point b, const PenSpec& pen) {
    Surf* s = mutate(id);
    if (!s) return;
    s->dc->SetBkMode(TRANSPARENT);               // == the old Arc() SetBkMode
    CPen cpen;
    cpen.CreatePen(penToPs(pen.style), pen.width, pen.colour);
    CPen* old = s->dc->SelectObject(&cpen);
    s->dc->Arc(r.x, r.y, r.x + r.w, r.y + r.h, a.x, a.y, b.x, b.y);
    s->dc->SelectObject(old);
}

// CPU-exact flood over the whole-surface DIB round-trip: wxDC::FloodFill is an
// unimplemented no-op on wxOSX (same class as GetPixel), so — like invertRect
// and the colour-key blit — this reimplements the GDI semantics against
// canonical bytes. Matches CpuRaster::floodFill exactly.
void WxSurface::floodFill(SurfaceId id, Point seed, Colour fill, Colour ref, FloodMode mode) {
    Surf* s = mutate(id);
    if (!s || seed.x < 0 || seed.y < 0 || seed.x >= s->size.w || seed.y >= s->size.h)
        return;
    PixelBuffer buf;
    readPixels(id, { 0, 0, s->size.w, s->size.h }, buf, PixelFormat::BGR24);
    if (buf.bytes.empty()) return;
    const int w = buf.size.w, h = buf.size.h;
    auto at = [&](int x, int y) -> Colour {
        const unsigned char* p = buf.row(y) + (size_t)x * 3;
        return (Colour)p[2] | ((Colour)p[1] << 8) | ((Colour)p[0] << 16);
    };
    const Colour atSeed = at(seed.x, seed.y);
    if (mode == FloodMode::Border  && atSeed == ref) return;   // seeded on the boundary
    if (mode == FloodMode::Surface && atSeed != ref) return;   // seeded off the region
    std::vector<unsigned char> visited((size_t)w * h, 0);
    std::vector<Point> stack{ seed };
    while (!stack.empty()) {
        Point p = stack.back(); stack.pop_back();
        if (p.x < 0 || p.y < 0 || p.x >= w || p.y >= h) continue;
        unsigned char& v = visited[(size_t)p.y * w + p.x];
        if (v) continue;
        v = 1;
        const Colour c = at(p.x, p.y);
        if (mode == FloodMode::Border ? (c == ref) : (c != ref)) continue;
        unsigned char* q = buf.row(p.y) + (size_t)p.x * 3;
        q[0] = (unsigned char)((fill >> 16) & 0xFF);   // B
        q[1] = (unsigned char)((fill >> 8) & 0xFF);    // G
        q[2] = (unsigned char)(fill & 0xFF);           // R
        stack.push_back({ p.x + 1, p.y }); stack.push_back({ p.x - 1, p.y });
        stack.push_back({ p.x, p.y + 1 }); stack.push_back({ p.x, p.y - 1 });
    }
    writePixels(id, { 0, 0, w, h }, buf);
}

// The engine's own per-strip fallback (exact endpoints, matches the reference).
void WxSurface::gradient(SurfaceId id, Rect r, Colour from, Colour to, bool horizontal) {
    Surf* s = mutate(id);
    Rect c = r;
    if (!s || !clipToBounds(c, s->size)) return;
    const int n = horizontal ? r.w : r.h;
    for (int i = 0; i < (horizontal ? c.w : c.h); ++i) {
        const int j = (horizontal ? c.x - r.x : c.y - r.y) + i;   // offset in the FULL rect
        const double t = n <= 1 ? 0.0 : (double)j / (n - 1);
        auto ch = [&](int f, int tt) { return (int)(f + (tt - f) * t + 0.5); };
        const COLORREF col = RGB(ch(GetRValue(from), GetRValue(to)),
                                 ch(GetGValue(from), GetGValue(to)),
                                 ch(GetBValue(from), GetBValue(to)));
        if (horizontal) s->dc->FillSolidRect(c.x + i, c.y, 1, c.h, col);
        else            s->dc->FillSolidRect(c.x, c.y + i, c.w, 1, col);
    }
}

// ── blits ───────────────────────────────────────────────────────────────────
void WxSurface::blit(SurfaceId dstId, Rect dstRect, SurfaceId srcId, Rect srcRect,
                     BlitMode mode, Colour key, BlitQuality q, const Matrix2x3* m) {
    Surf* dst = mutate(dstId);
    Surf* src = surf(srcId);
    if (!dst || !src) return;
    if (m) {
        // Matrix blit (WindowTransformImage): read the source and inverse-map
        // it CPU-side — wxDC has no world transform. Nearest sampling (the
        // GDI HALFTONE resample is the Windows adapter's; documented deviation).
        Rect clipped = srcRect;
        if (!clipToBounds(clipped, src->size)) return;
        PixelBuffer sp;
        readPixels(srcId, clipped, sp, PixelFormat::BGR24);
        if (!sp.valid()) return;
        blit(dstId, dstRect, sp, { 0, 0, sp.size.w, sp.size.h }, mode, key, q, m);
        return;
    }
    const bool stretch = (dstRect.w != srcRect.w || dstRect.h != srcRect.h);
    // Clip srcRect to the source; the removed margin shifts the destination
    // 1:1 (BitBlt clipping). A stretched blit whose source was clipped needs a
    // scaled shift — an S2 gap (rare; WindowDrawImage stretched paths are
    // fully in-bounds), so bail rather than mis-scale.
    Rect clipped = srcRect;
    if (!clipToBounds(clipped, src->size)) return;
    const bool clippedAway = (clipped.w != srcRect.w || clipped.h != srcRect.h);
    if (clippedAway && stretch) return;                              // S2 gap
    const Rect dr = clippedAway
        ? Rect{ dstRect.x + (clipped.x - srcRect.x),
                dstRect.y + (clipped.y - srcRect.y), clipped.w, clipped.h }
        : dstRect;
    if (mode == BlitMode::TransparentKey) {
        if (stretch) return;                                         // S2 gap: keyed+stretch
        PixelBuffer sp;
        readPixels(srcId, clipped, sp, PixelFormat::BGR24);
        blit(dstId, dr, sp, { 0, 0, sp.size.w, sp.size.h }, mode, key, q, nullptr);
        return;
    }
    if (!stretch) {
        dst->dc->BitBlt(dr.x, dr.y, dr.w, dr.h, src->dc, clipped.x, clipped.y, SRCCOPY);
    } else {
        dst->dc->SetStretchBltMode(q == BlitQuality::Smooth ? HALFTONE : COLORONCOLOR);
        dst->dc->StretchBlt(dstRect.x, dstRect.y, dstRect.w, dstRect.h,
                            src->dc, clipped.x, clipped.y, clipped.w, clipped.h, SRCCOPY);
    }
}

void WxSurface::blit(SurfaceId dstId, Rect dstRect, const PixelBuffer& src, Rect srcRect,
                     BlitMode mode, Colour key, BlitQuality /*q*/, const Matrix2x3* m) {
    Surf* dst = mutate(dstId);
    if (!dst || !src.valid()) return;
    if (m) {
        // Matrix blit: device = M·(srcRect-relative) + t; dstRect ignored
        // (contract). Inverse-map, pixel-centre sampling, nearest neighbour.
        const double det = m->m11 * m->m22 - m->m21 * m->m12;
        if (det == 0.0) return;
        // destination bounding box from the four transformed source corners
        const double cw = srcRect.w, ch = srcRect.h;
        const double cxs[4] = { 0, cw, 0, cw }, cys[4] = { 0, 0, ch, ch };
        double minX = m->dx, maxX = m->dx, minY = m->dy, maxY = m->dy;
        for (int i = 0; i < 4; ++i) {
            const double X = cxs[i] * m->m11 + cys[i] * m->m21 + m->dx;
            const double Y = cxs[i] * m->m12 + cys[i] * m->m22 + m->dy;
            if (X < minX) minX = X; if (X > maxX) maxX = X;
            if (Y < minY) minY = Y; if (Y > maxY) maxY = Y;
        }
        Rect box = { (int)floor(minX) - 1, (int)floor(minY) - 1,
                     (int)ceil(maxX) - (int)floor(minX) + 2,
                     (int)ceil(maxY) - (int)floor(minY) + 2 };
        if (!clipToBounds(box, dst->size)) return;
        PixelBuffer db;
        readPixels(dstId, box, db, PixelFormat::BGR24);
        if (!db.valid()) return;
        const int sbpp = src.bytesPerPixel();
        for (int y = 0; y < box.h; ++y) {
            unsigned char* drow = db.row(y);
            for (int x = 0; x < box.w; ++x) {
                const double u = (box.x + x + 0.5) - m->dx;
                const double v = (box.y + y + 0.5) - m->dy;
                const int sx = (int)floor(( u * m->m22 - v * m->m21) / det);
                const int sy = (int)floor((-u * m->m12 + v * m->m11) / det);
                if (sx < 0 || sy < 0 || sx >= srcRect.w || sy >= srcRect.h) continue;
                const int bsx = srcRect.x + sx, bsy = srcRect.y + sy;
                if (bsx < 0 || bsy < 0 || bsx >= src.size.w || bsy >= src.size.h) continue;
                const unsigned char* p = src.row(bsy) + (size_t)bsx * sbpp;
                const Colour c = (Colour)p[2] | ((Colour)p[1] << 8) | ((Colour)p[0] << 16);
                if (mode == BlitMode::TransparentKey && c == key) continue;
                unsigned char* d = drow + (size_t)x * 3;
                d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
            }
        }
        writePixels(dstId, box, db);
        return;
    }
    if (dstRect.w != srcRect.w || dstRect.h != srcRect.h) return;   // S2 gap: stretch
    if (mode == BlitMode::Copy) {
        // Fast path: land the buffer via the DIB round-trip.
        // writePixels writes from the buffer ORIGIN, so carve the sub-rect out
        // when srcRect isn't the whole buffer.
        if (srcRect.x == 0 && srcRect.y == 0 &&
            srcRect.w == src.size.w && srcRect.h == src.size.h) {
            writePixels(dstId, dstRect, src);
            return;
        }
    }
    // Exact per-pixel path (colour-key, or sub-rect copy): read the destination
    // region, merge, write back.
    Rect dc = dstRect;
    if (!clipToBounds(dc, dst->size)) return;
    PixelBuffer db;
    readPixels(dstId, dc, db, PixelFormat::BGR24);
    if (db.bytes.empty()) return;
    const int sbpp = src.bytesPerPixel();
    for (int y = 0; y < db.size.h; ++y) {
        const int sy = srcRect.y + (dc.y - dstRect.y) + y;
        if (sy < 0 || sy >= src.size.h) continue;
        const unsigned char* srow = src.row(sy);
        unsigned char*       drow = db.row(y);
        for (int x = 0; x < db.size.w; ++x) {
            const int sx = srcRect.x + (dc.x - dstRect.x) + x;
            if (sx < 0 || sx >= src.size.w) continue;
            const unsigned char* p = srow + (size_t)sx * sbpp;
            const Colour c = (Colour)p[2] | ((Colour)p[1] << 8) | ((Colour)p[0] << 16);
            if (mode == BlitMode::TransparentKey && c == key) continue;
            unsigned char* d = drow + (size_t)x * 3;
            d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
        }
    }
    writePixels(dstId, dc, db);
}

// ── text ────────────────────────────────────────────────────────────────────
FontId WxSurface::createFont(const FontSpec& spec) {
    Font f;
    f.spec = spec;
    f.font = new CFont;
    // The engine passes a NEGATIVE pixel height into CreateFont (points were
    // converted with dpi() above the seam).
    const int h = spec.pixelHeight > 0 ? spec.pixelHeight : 10;
    if (f.font->CreateFont(-h, 0, 0, 0,
                           spec.weight >= 700 ? FW_BOLD : FW_NORMAL,
                           spec.italic, spec.underline, spec.strikeout,
                           spec.charsetHint, 0, 0, 0,
                           spec.pitchAndFamilyHint, spec.face.c_str()) == 0) {
        delete f.font;
        return (FontId)0;
    }
    std::uint32_t id = m_nextFont++;
    m_fonts[id] = f;
    return (FontId)id;
}

void WxSurface::destroyFont(FontId id) {
    Font* f = font(id);
    if (!f) return;
    f->font->DeleteObject();
    delete f->font;
    m_fonts.erase((std::uint32_t)id);
}

// Decode text bytes to a UTF-8 wxString-safe form: valid UTF-8 passes through;
// otherwise decode via the font's charset hint (the decided legacy-charset fix,
// mirroring GDI's "the font's charset interprets the bytes"), else Latin-1 —
// the shim's existing fallback.
static std::string DecodeSeamText(const char* utf8, size_t len, int charsetHint) {
    if (!utf8 || len == 0) return std::string();
    wxString probe = wxString::FromUTF8(utf8, len);
    if (!probe.empty())
        return std::string(utf8, len);
    if (charsetHint > 2) {
        wxCSConv conv(WinCharsetToEncoding(charsetHint));
        wxString decoded(utf8, conv, len);
        if (!decoded.empty()) {
            const wxScopedCharBuffer b = decoded.utf8_str();
            return std::string(b.data(), b.length());
        }
    }
    wxString latin1(utf8, wxConvISO8859_1, len);
    const wxScopedCharBuffer b = latin1.utf8_str();
    return std::string(b.data(), b.length());
}

int WxSurface::drawText(SurfaceId sid, FontId fid, const char* utf8, size_t len, Point pt,
                        Colour colour, const Rect* clip) {
    Surf* s = mutate(sid);
    Font* f = font(fid);
    if (!s || !f) return 0;
    const std::string text = DecodeSeamText(utf8, len, f->spec.charsetHint);
    CFont* oldFont = s->dc->SelectObject(f->font);
    s->dc->SetTextColor(colour);
    s->dc->SetBkMode(TRANSPARENT);
    if (clip) {
        s->dc->ExtTextOut(pt.x, pt.y, ETO_CLIPPED,
                          CRect(clip->x, clip->y, clip->x + clip->w, clip->y + clip->h),
                          text.c_str(), (int)text.size(), NULL);
    } else {
        s->dc->ExtTextOut(pt.x, pt.y, 0, CRect(0, 0, 0, 0),
                          text.c_str(), (int)text.size(), NULL);
    }
    CSize sz = s->dc->GetTextExtent(text.c_str(), (int)text.size());
    s->dc->SelectObject(oldFont);
    const int measured = sz.cx;
    return clip ? (measured < clip->w ? measured : clip->w) : measured;
}

Size WxSurface::measureText(FontId fid, const char* utf8, size_t len) {
    Font* f = font(fid);
    if (!f) return {};
    const std::string text = DecodeSeamText(utf8, len, f->spec.charsetHint);
    CDC* dc = measureDc();
    CFont* old = dc->SelectObject(f->font);
    CSize sz = dc->GetTextExtent(text.c_str(), (int)text.size());
    dc->SelectObject(old);
    return { sz.cx, sz.cy };
}

FontMetrics WxSurface::fontMetrics(FontId fid) {
    FontMetrics m;
    Font* f = font(fid);
    if (!f) return m;
    CDC* dc = measureDc();
    CFont* old = dc->SelectObject(f->font);
    TEXTMETRIC tm;
    memset(&tm, 0, sizeof tm);
    if (dc->GetTextMetrics(&tm)) {
        m.height = tm.tmHeight;
        m.ascent = tm.tmAscent;
        m.descent = tm.tmDescent;
        m.internalLeading = tm.tmInternalLeading;
        m.externalLeading = tm.tmExternalLeading;
        m.aveCharWidth = tm.tmAveCharWidth;
        m.maxCharWidth = tm.tmMaxCharWidth;
        m.weight = tm.tmWeight;
        m.overhang = tm.tmOverhang;
        m.digitizedAspectX = tm.tmDigitizedAspectX;
        m.digitizedAspectY = tm.tmDigitizedAspectY;
        m.firstChar = tm.tmFirstChar;
        m.lastChar = tm.tmLastChar;
        m.defaultChar = tm.tmDefaultChar;
        m.breakChar = tm.tmBreakChar;
        m.italic = tm.tmItalic;              // raw TEXTMETRIC bytes (WindowFontInfo
        m.underlined = tm.tmUnderlined;      // 16/17/18 returns them verbatim, as
        m.struckOut = tm.tmStruckOut;        // the pre-seam pdc->GetTextMetrics did)
        m.pitchAndFamily = tm.tmPitchAndFamily;
        m.charset = tm.tmCharSet;
    }
    CString face;
    if (dc->GetTextFace(face) > 0)
        m.face = (const char*)face;              // realized face after substitution
    else
        m.face = f->spec.face;
    dc->SelectObject(old);
    return m;
}

int WxSurface::dpi() {
    // The real display PPI — 144 on a Retina Mac, NOT the ~72 this comment used
    // to claim (the PAL surface selftest prints it).  Callers convert points to
    // a pixel height with MulDiv(points, dpi(), 72); CFont::CreateFont converts
    // straight back through the same PPI, so the round-trip is exact.
    return GetDeviceCaps((*measureDc()), LOGPIXELSY);
}

} // namespace pal

// ── S3 migration bridge install ──────────────────────────────────────────────
// One adapter instance hosts every miniwindow's adopted surface.
static pal::WxSurface* s_bridgeAdapter = nullptr;

static pal::SurfaceId wxAdoptSurface(void* nativeCtx, int w, int h) {
    if (!s_bridgeAdapter || !nativeCtx) return pal::SurfaceId::None;
    return s_bridgeAdapter->adopt(reinterpret_cast<CDC*>(nativeCtx), { w, h });
}
static void wxReleaseAdoptedSurface(pal::SurfaceId id) {
    if (s_bridgeAdapter && id != pal::SurfaceId::None)
        s_bridgeAdapter->destroySurface(id);
}

void InstallWxSurfaceBridge() {
    if (s_bridgeAdapter) return;                 // idempotent
    s_bridgeAdapter               = new pal::WxSurface();
    pal::g_surface                = s_bridgeAdapter;
    pal::g_adoptSurface           = &wxAdoptSurface;
    pal::g_releaseAdoptedSurface  = &wxReleaseAdoptedSurface;
}

// ── conformance selftest (hidden --pal-surface-selftest flag) ────────────────
static int g_stPass = 0, g_stFail = 0;
static void stCheck(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_stPass : g_stFail)++;
}

int RunPalSurfaceSelfTest() {
    using namespace pal;
    printf("PAL ISurface conformance selftest (wx adapter)\n");
    WxSurface impl;
    ISurface& ad = impl;

    const Colour kRed = 0x0000FF, kGreen = 0x00FF00, kBlue = 0xFF0000,
                 kWhite = 0xFFFFFF, kBlack = 0x000000;

    // 1. lifecycle + background + bounds
    SurfaceId s = ad.createSurface({ 8, 6 }, kBlue);
    stCheck(ad.getPixel(s, { 0, 0 }) == kBlue &&
            ad.getPixel(s, { 7, 5 }) == kBlue, "createSurface fills background");
    stCheck(ad.getPixel(s, { 8, 0 }) == 0xFFFFFFFFu &&
            ad.getPixel(s, { 0, -1 }) == 0xFFFFFFFFu, "getPixel OOB = CLR_INVALID");
    // setPixel round-trips (WindowSetPixel/WindowGetPixel); OOB set is a no-op.
    ad.setPixel(s, { 3, 2 }, kRed);
    ad.setPixel(s, { 99, 99 }, kGreen);       // OOB — must not corrupt/crash
    stCheck(ad.getPixel(s, { 3, 2 }) == kRed && ad.getPixel(s, { 4, 2 }) == kBlue,
            "setPixel writes one pixel (round-trips via getPixel)");

    // 2. THE critical check: readPixels DIB layout (BGR, bottom-up, padded)
    SurfaceId q = ad.createSurface({ 2, 2 }, kBlack);
    ad.fillRect(q, { 0, 0, 1, 1 }, { BrushStyle::Solid, kRed });
    ad.fillRect(q, { 1, 1, 1, 1 }, { BrushStyle::Solid, kGreen });
    PixelBuffer out;
    ad.readPixels(q, { 0, 0, 2, 2 }, out, PixelFormat::BGR24);
    bool layoutOk = out.stride == 8 && out.bytes.size() == 16;
    if (layoutOk) {
        const unsigned char* b = out.bytes.data();
        layoutOk = b[3] == 0x00 && b[4] == 0xFF && b[5] == 0x00 &&   // row0=bottom: (1,1) green
                   b[8] == 0x00 && b[9] == 0x00 && b[10] == 0xFF;    // row1=top:   (0,0) red
    }
    stCheck(layoutOk, "readPixels emits exact DIB bytes (BGR24, bottom-up, padded)");

    // 3. write/read round-trip (BGRA32)
    PixelBuffer buf32;
    ad.readPixels(q, { 0, 0, 2, 2 }, buf32, PixelFormat::BGRA32);
    SurfaceId q2 = ad.createSurface({ 2, 2 }, kWhite);
    ad.writePixels(q2, { 0, 0, 2, 2 }, buf32);
    stCheck(ad.getPixel(q2, { 0, 0 }) == kRed && ad.getPixel(q2, { 1, 1 }) == kGreen &&
            ad.getPixel(q2, { 1, 0 }) == kBlack, "writePixels round-trips BGRA32");

    // 4. invertRect exact NOT, twice = identity
    SurfaceId iv = ad.createSurface({ 4, 4 }, kRed);
    ad.invertRect(iv, { 1, 1, 2, 2 });
    bool invOk = ad.getPixel(iv, { 1, 1 }) == 0xFFFF00 && ad.getPixel(iv, { 0, 0 }) == kRed;
    ad.invertRect(iv, { 1, 1, 2, 2 });
    stCheck(invOk && ad.getPixel(iv, { 1, 1 }) == kRed, "invertRect is exact dest-NOT");

    // 5. blit copy + exact colour-key
    SurfaceId src = ad.createSurface({ 4, 4 }, kGreen);
    ad.fillRect(src, { 1, 1, 2, 2 }, { BrushStyle::Solid, kRed });
    SurfaceId dst = ad.createSurface({ 8, 8 }, kBlack);
    ad.blit(dst, { 2, 2, 4, 4 }, src, { 0, 0, 4, 4 }, BlitMode::Copy);
    bool blitOk = ad.getPixel(dst, { 2, 2 }) == kGreen && ad.getPixel(dst, { 3, 3 }) == kRed &&
                  ad.getPixel(dst, { 1, 1 }) == kBlack;
    stCheck(blitOk, "blit copy lands at destination");
    SurfaceId dst2 = ad.createSurface({ 8, 8 }, kBlue);
    ad.blit(dst2, { 0, 0, 4, 4 }, src, { 0, 0, 4, 4 }, BlitMode::TransparentKey, kGreen);
    stCheck(ad.getPixel(dst2, { 0, 0 }) == kBlue && ad.getPixel(dst2, { 1, 1 }) == kRed,
            "colour-key blit skips exactly the key");

    // 6. gradient endpoints exact
    SurfaceId g = ad.createSurface({ 10, 3 }, kBlack);
    ad.gradient(g, { 0, 0, 10, 3 }, kBlack, kWhite, true);
    stCheck(ad.getPixel(g, { 0, 1 }) == kBlack && ad.getPixel(g, { 9, 1 }) == kWhite,
            "gradient endpoints are exact");

    // 7. text: draw/measure agreement + metrics sanity + pixels landed
    FontId f = ad.createFont({ "", 14, 400, false, false, false, 0, 0 });
    Size msz = ad.measureText(f, "Dog", 3);
    SurfaceId ts = ad.createSurface({ 100, 30 }, kBlack);
    int drawn = ad.drawText(ts, f, "Dog", 3, { 2, 2 }, kWhite, nullptr);
    stCheck(msz.w > 0 && drawn == msz.w, "drawText returns measureText width");
    FontMetrics fm = ad.fontMetrics(f);
    stCheck(fm.height > 0 && fm.ascent > 0 && fm.ascent <= fm.height &&
            fm.aveCharWidth > 0 && !fm.face.empty(), "fontMetrics are sane + realized face");
    bool inked = false;
    for (int y = 0; y < 30 && !inked; ++y)
        for (int x = 0; x < 100; ++x)
            if (ad.getPixel(ts, { x, y }) != kBlack) { inked = true; break; }
    stCheck(inked, "drawText actually inks pixels");

    // 7b. point-size round-trip.  Callers (WindowFont, ChangeFont, FixFont) turn
    // a point size into pixelHeight with MulDiv(points, dpi(), 72); CFont::
    // CreateFont must undo that through the same PPI.  If it does not, a 10pt
    // request realizes ~1.67x too large on a 144-PPI Retina display and every
    // miniwindow's text overflows the box the plugin sized for it.  A correctly
    // realized 10pt font has a line height around 12-13 logical units; the
    // unconverted bug gives ~20+.
    {
        const int kPoints = 10;
        FontId pf = ad.createFont({ "", MulDiv(kPoints, ad.dpi(), 72), 400,
                                    false, false, false, 0, 0 });
        FontMetrics pfm = ad.fontMetrics(pf);
        stCheck(pfm.height >= kPoints - 2 && pfm.height <= kPoints + 6,
                "createFont round-trips a point size through dpi()");
    }

    // 8. line interior (endpoint inclusivity is the shim's documented deviation)
    SurfaceId ln = ad.createSurface({ 10, 10 }, kBlack);
    ad.line(ln, { 2, 5 }, { 7, 5 }, { PenStyle::Solid, 1, kWhite });
    stCheck(ad.getPixel(ln, { 3, 5 }) == kWhite && ad.getPixel(ln, { 6, 5 }) == kWhite &&
            ad.getPixel(ln, { 1, 5 }) == kBlack, "line draws its interior pixels");

    // 8b. arc inks pixels inside its bounding box (the WindowArc S3c path)
    SurfaceId ac = ad.createSurface({ 20, 20 }, kBlack);
    ad.arc(ac, { 0, 0, 20, 20 }, { 20, 0 }, { 0, 20 }, { PenStyle::Solid, 1, kWhite });
    bool arcInked = false;
    for (int y = 0; y < 20 && !arcInked; ++y)
        for (int x = 0; x < 20; ++x)
            if (ad.getPixel(ac, { x, y }) != kBlack) { arcInked = true; break; }
    stCheck(arcInked, "arc inks pixels (WindowArc path)");

    // 8c. bezier inks pixels (the WindowBezier S3c path) — 4 control points
    SurfaceId bz = ad.createSurface({ 20, 20 }, kBlack);
    const Point bzp[4] = { { 1, 10 }, { 6, 1 }, { 13, 19 }, { 18, 10 } };
    ad.bezier(bz, bzp, 4, { PenStyle::Solid, 1, kWhite });
    bool bzInked = false;
    for (int y = 0; y < 20 && !bzInked; ++y)
        for (int x = 0; x < 20; ++x)
            if (ad.getPixel(bz, { x, y }) != kBlack) { bzInked = true; break; }
    stCheck(bzInked, "bezier inks pixels (WindowBezier path)");

    // 8d. mono pattern-brush shape fill tiles (WindowCircleOp/Polygon styles 8-12).
    // On wxOSX the shim renders the mono stipple black/white (Windows/GDI applies
    // the real fg/bg) — so just assert the interior is TILED: both colours present.
    SurfaceId pf = ad.createSurface({ 16, 16 }, kBlack);
    const unsigned char kFine[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
    BrushSpec patBr; patBr.style = BrushStyle::Pattern;
    patBr.monoPattern = kFine; patBr.fg = kWhite; patBr.bg = kBlack;
    ad.shape(pf, ShapeKind::Rect, { 1, 1, 14, 14 }, {}, {}, { PenStyle::Null }, patBr);
    int nWhite = 0, nBlack = 0;
    for (int y = 3; y < 13; ++y)
        for (int x = 3; x < 13; ++x) {
            const Colour c = ad.getPixel(pf, { x, y });
            if (c == kWhite) nWhite++; else if (c == kBlack) nBlack++;
        }
    stCheck(nWhite > 0 && nBlack > 0, "mono pattern brush tiles a shape fill");

    // 9. shape(Rect): border + interior
    SurfaceId sh = ad.createSurface({ 10, 10 }, kBlack);
    ad.shape(sh, ShapeKind::Rect, { 2, 2, 6, 5 }, {}, {},
             { PenStyle::Solid, 1, kWhite }, { BrushStyle::Solid, kGreen });
    const Colour shBorder = ad.getPixel(sh, { 2, 2 }),
                 shFill   = ad.getPixel(sh, { 4, 4 }),
                 shOut    = ad.getPixel(sh, { 8, 2 });
    // Pen-stroked shapes render through wx/CoreGraphics — the shipped product
    // pipeline — whose readback can be off by ±2/channel (e.g. white borders
    // read fe,ff,fe). Fills / blits / keys / pixel I/O stay byte-exact; stroke
    // colour gets a tolerance, matching what the product has always drawn.
    auto near = [](Colour a, Colour b) {
        auto d = [](int x, int y) { int v = x - y; return v < 0 ? -v : v; };
        return d(a & 0xFF, b & 0xFF) <= 2 &&
               d((a >> 8) & 0xFF, (b >> 8) & 0xFF) <= 2 &&
               d((a >> 16) & 0xFF, (b >> 16) & 0xFF) <= 2;
    };
    if (!near(shBorder, kWhite) || shFill != kGreen || shOut != kBlack)
        printf("    (border=%06x fill=%06x outside=%06x)\n",
               (unsigned)shBorder, (unsigned)shFill, (unsigned)shOut);
    stCheck(near(shBorder, kWhite) && shFill == kGreen && shOut == kBlack,
            "shape(Rect) strokes and fills (stroke +/-2/channel)");

    // 10. floodFill border + surface modes (review claim: wxDC::FloodFill is a
    // no-op on wxOSX — this check is the empirical arbiter).
    SurfaceId ff = ad.createSurface({ 10, 10 }, kBlack);
    ad.shape(ff, ShapeKind::Rect, { 2, 2, 6, 6 }, {}, {},
             { PenStyle::Solid, 1, kWhite }, { BrushStyle::Null });
    ad.floodFill(ff, { 4, 4 }, kRed, kWhite, FloodMode::Border);
    stCheck(ad.getPixel(ff, { 4, 4 }) == kRed && ad.getPixel(ff, { 0, 0 }) == kBlack,
            "floodFill border mode fills inside the boundary");
    ad.floodFill(ff, { 0, 0 }, kGreen, kBlack, FloodMode::Surface);
    stCheck(ad.getPixel(ff, { 0, 0 }) == kGreen && ad.getPixel(ff, { 9, 9 }) == kGreen &&
            ad.getPixel(ff, { 4, 4 }) == kRed, "floodFill surface mode replaces the region");

    // 11. hatch brush: fg lines + bg gaps; bg == -1 leaves gaps untouched
    // (review claim: wxOSX hatches ignore the gap colour and grey the lines).
    SurfaceId hb = ad.createSurface({ 16, 16 }, kBlack);
    ad.fillRect(hb, { 0, 0, 16, 16 },
                { BrushStyle::Hatch, kWhite, kBlue, HatchKind::Horizontal });
    bool sawFg = false, sawBg = false, sawOther = false;
    for (int y = 0; y < 16; ++y) {
        const Colour c = ad.getPixel(hb, { 3, y });
        if (c == kWhite) sawFg = true;
        else if (c == kBlue) sawBg = true;
        else sawOther = true;
    }
    if (!(sawFg && sawBg && !sawOther))
        printf("    (hatch column scan: fg=%d bg=%d other=%d)\n", sawFg, sawBg, sawOther);
    stCheck(sawFg && sawBg && !sawOther, "hatch brush: exact fg lines + bg gaps");
    SurfaceId hb2 = ad.createSurface({ 16, 16 }, kGreen);
    ad.fillRect(hb2, { 0, 0, 16, 16 },
                { BrushStyle::Hatch, kWhite, (Colour)-1, HatchKind::Horizontal });
    bool gapsUntouched = true;
    for (int y = 0; y < 16 && gapsUntouched; ++y) {
        const Colour c = ad.getPixel(hb2, { 3, y });
        if (c != kWhite && c != kGreen) gapsUntouched = false;
    }
    stCheck(gapsUntouched, "hatch brush: bg == -1 gaps stay untouched");

    // 12. adopt an externally-owned CDC (the CMiniWindow S3 bridge) and draw the
    // RectOp family through it — into the SAME context the engine still owns.
    {
        CDC dc; dc.CreateCompatibleDC(NULL); dc.SetTextAlign(TA_LEFT | TA_TOP);
        CBitmap bmp; bmp.CreateBitmap(24, 24, 1, GetDeviceCaps(dc, BITSPIXEL), NULL);
        CBitmap* oldb = dc.SelectObject(&bmp);
        dc.SetWindowOrg(0, 0);
        dc.FillSolidRect(0, 0, 24, 24, kBlack);
        SurfaceId as = impl.adopt(&dc, { 24, 24 });
        stCheck(as != SurfaceId::None, "adopt returns a surface for an external CDC");
        ad.fillRect(as, { 2, 2, 10, 10 }, { BrushStyle::Solid, kBlue });   // RectOp 2
        // RectOp 1 frame + RectOp 6 flood inside it (the wxOSX-broken op)
        ad.fillRect(as, { 12, 12, 1, 10 }, { BrushStyle::Solid, kWhite });
        ad.fillRect(as, { 21, 12, 1, 10 }, { BrushStyle::Solid, kWhite });
        ad.fillRect(as, { 12, 12, 10, 1 }, { BrushStyle::Solid, kWhite });
        ad.fillRect(as, { 12, 21, 10, 1 }, { BrushStyle::Solid, kWhite });
        ad.floodFill(as, { 16, 16 }, kRed, kWhite, FloodMode::Border);
        stCheck(ad.getPixel(as, { 3, 3 }) == kBlue &&
                ad.getPixel(as, { 16, 16 }) == kRed, "adopt: RectOp fill + flood draw into the CDC");
        // Releasing an adopted surface must NOT free the caller's CDC/bitmap.
        ad.destroySurface(as);
        dc.FillSolidRect(0, 0, 24, 24, kGreen);        // would crash on a freed CDC
        SurfaceId as2 = impl.adopt(&dc, { 24, 24 });
        stCheck(ad.getPixel(as2, { 5, 5 }) == kGreen, "adopt: release leaves the CDC usable");
        ad.destroySurface(as2);
        dc.SelectObject(oldb); bmp.DeleteObject();
    }

    // 13. text into an ADOPTED CDC — the exact CMiniWindow::Text/TextWidth (S3b)
    // live path: create a font, draw + measure through the adopted surface.
    {
        CDC dc; dc.CreateCompatibleDC(NULL); dc.SetTextAlign(TA_LEFT | TA_TOP);
        CBitmap bmp; bmp.CreateBitmap(80, 20, 1, GetDeviceCaps(dc, BITSPIXEL), NULL);
        CBitmap* oldb = dc.SelectObject(&bmp);
        dc.FillSolidRect(0, 0, 80, 20, kBlack);
        SurfaceId as = impl.adopt(&dc, { 80, 20 });
        FontId tf = ad.createFont({ "", 14, 400, false, false, false, 0, 0 });
        const Rect clip { 0, 0, 80, 20 };
        int w = ad.drawText(as, tf, "Text", 4, { 1, 1 }, kWhite, &clip);
        Size m = ad.measureText(tf, "Text", 4);
        bool inked = false;
        for (int y = 0; y < 20 && !inked; ++y)
            for (int x = 0; x < 80; ++x)
                if (ad.getPixel(as, { x, y }) != kBlack) { inked = true; break; }
        stCheck(w > 0 && w == m.w && inked, "adopt: drawText into the CDC (WindowText path)");
        ad.destroyFont(tf);
        ad.destroySurface(as);
        dc.SelectObject(oldb); bmp.DeleteObject();
    }

    // 14. alpha-preserving image read (BGRA32_Alpha) — the exact
    // WindowDrawImageAlpha path: a 32bpp DIBSection with alpha (as LoadPng
    // makes) Attach-materialized into a CBitmap, selected into a CDC, adopted,
    // then read back. Alpha must be exact; RGB is asserted at A=255 only
    // (wxOSX stores premultiplied, so RGB under partial alpha may quantize).
    {
        BITMAPINFO bmi; memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi;
        bmi.bmiHeader.biWidth = 4; bmi.bmiHeader.biHeight = 2;   // bottom-up
        bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        bmi.bmiHeader.biSizeImage = 2 * 16;
        unsigned char* bits = NULL;
        HBITMAP hbm = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS,
                                       (void**)&bits, NULL, 0);
        auto put = [&](int x, int y, unsigned char B, unsigned char G,
                       unsigned char R, unsigned char A) {
            unsigned char* p = bits + (size_t)(1 - y) * 16 + (size_t)x * 4;
            p[0] = B; p[1] = G; p[2] = R; p[3] = A;
        };
        put(0, 0, 10, 20, 30, 255);  put(1, 0, 40, 50, 60, 128);
        put(2, 0, 70, 80, 90, 37);   put(3, 0, 200, 150, 100, 0);
        put(0, 1, 1, 2, 3, 255);
        CBitmap img; img.Attach((HGDIOBJ)hbm);   // materializes + frees the DIB
        CDC idc; idc.CreateCompatibleDC(NULL);
        CBitmap* oldi = idc.SelectObject(&img);
        SurfaceId is = impl.adopt(&idc, { 4, 2 });

        PixelBuffer pa;
        ad.readPixels(is, { 0, 0, 4, 2 }, pa, PixelFormat::BGRA32_Alpha);
        bool aOk = pa.valid() && pa.size.w == 4 && pa.size.h == 2;
        if (aOk) {
            const unsigned char* r0 = pa.row(0);
            aOk = r0[3] == 255 && r0[7] == 128 && r0[11] == 37 && r0[15] == 0
               && r0[0] == 10 && r0[1] == 20 && r0[2] == 30
               && pa.row(1)[0] == 1 && pa.row(1)[3] == 255;
        }
        stCheck(aOk, "readPixels BGRA32_Alpha: real image alpha (+RGB at A=255)");

        PixelBuffer pc;
        ad.readPixels(is, { 2, 0, 4, 2 }, pc, PixelFormat::BGRA32_Alpha);
        stCheck(pc.valid() && pc.size.w == 2 && pc.size.h == 2 &&
                pc.row(0)[3] == 37, "BGRA32_Alpha read clips to image bounds");

        PixelBuffer pp;
        ad.readPixels(is, { 0, 0, 4, 2 }, pp, PixelFormat::BGRA32);
        bool pinOk = pp.valid();
        if (pinOk)
            for (size_t k = 3; k < pp.bytes.size(); k += 4)
                if (pp.bytes[k]) { pinOk = false; break; }
        stCheck(pinOk, "readPixels BGRA32 still pins surface alpha to 0");

        ad.destroySurface(is);
        idc.SelectObject(oldi);
    }

    // 15. arbitrary-image pattern brush (WindowImageOp): a 2x2 red/green
    // checker buffer tiled through shape(). Tiling anchor may differ per
    // backend, so assert both colours appear inside the fill, not positions.
    {
        SurfaceId ps = ad.createSurface({ 16, 16 }, kBlack);
        PixelBuffer pb;
        pb.format = PixelFormat::BGR24;
        pb.size   = { 2, 2 };
        pb.stride = PixelBuffer::strideFor(2, PixelFormat::BGR24);
        pb.bytes.assign((size_t)pb.stride * 2, 0);
        unsigned char* r0 = pb.row(0);        // top row: red, green
        r0[0] = 0x00; r0[1] = 0x00; r0[2] = 0xFF;
        r0[3] = 0x00; r0[4] = 0xFF; r0[5] = 0x00;
        unsigned char* r1 = pb.row(1);        // bottom row: green, red
        r1[0] = 0x00; r1[1] = 0xFF; r1[2] = 0x00;
        r1[3] = 0x00; r1[4] = 0x00; r1[5] = 0xFF;
        BrushSpec pbr; pbr.style = BrushStyle::Pattern; pbr.pattern = &pb;
        PenSpec ppen; ppen.colour = kWhite;
        ad.shape(ps, ShapeKind::Rect, { 1, 1, 14, 14 }, { 0, 0 }, { 0, 0 }, ppen, pbr);
        int nRed = 0, nGreen = 0;
        for (int y = 3; y < 13; ++y)
            for (int x = 3; x < 13; ++x) {
                Colour c = ad.getPixel(ps, { x, y });
                if (c == kRed) ++nRed; else if (c == kGreen) ++nGreen;
            }
        stCheck(nRed > 0 && nGreen > 0, "image pattern brush tiles a shape fill");
        ad.destroySurface(ps);
    }

    // 16. matrix blit (WindowTransformImage): a translate-only matrix is
    // fully deterministic under the inverse-map — pixels land 1:1 at (dx,dy).
    // A keyed matrix blit must skip the key colour.
    {
        SurfaceId msrc = ad.createSurface({ 2, 2 }, kRed);
        ad.setPixel(msrc, { 1, 1 }, kGreen);
        SurfaceId mdst = ad.createSurface({ 12, 12 }, kBlack);
        Matrix2x3 mt;                       // identity + translate (5,3)
        mt.dx = 5; mt.dy = 3;
        ad.blit(mdst, { 0, 0, 0, 0 }, msrc, { 0, 0, 2, 2 },
                BlitMode::Copy, 0, BlitQuality::Smooth, &mt);
        stCheck(ad.getPixel(mdst, { 5, 3 }) == kRed &&
                ad.getPixel(mdst, { 6, 4 }) == kGreen &&
                ad.getPixel(mdst, { 4, 3 }) == kBlack &&
                ad.getPixel(mdst, { 7, 5 }) == kBlack,
                "matrix blit: translate lands pixels exactly");
        ad.blit(mdst, { 0, 0, 0, 0 }, msrc, { 0, 0, 2, 2 },
                BlitMode::TransparentKey, kRed, BlitQuality::Smooth, &mt);
        stCheck(ad.getPixel(mdst, { 6, 3 }) == kRed &&   // key skipped: prior copy shows
                ad.getPixel(mdst, { 6, 4 }) == kGreen,
                "matrix blit: transparent key skips the key colour");
        ad.destroySurface(msrc);
        ad.destroySurface(mdst);
    }

    // ── content generation: EVERY mutator moves it, no reader does ──────────
    // The compositor caches the transparency mask a transparent miniwindow
    // needs, keyed by this number.  A mutator that forgets to bump it does not
    // fail loudly -- it leaves the miniwindow frozen at its previous contents,
    // which looks exactly like a plugin bug and gets diagnosed as one.  So
    // every mutating entry point is exercised here, one per check, and every
    // read is checked NOT to move it.
    //
    // This is what makes the mask cache safe to have: the ~50 Window* script
    // methods all funnel into these adapter ops, so "did we cover them all?" is
    // a question about this list rather than about the whole scripting surface.
    {
        SurfaceId g = ad.createSurface({ 8, 8 }, kBlack);
        SurfaceId gsrc = ad.createSurface({ 4, 4 }, kGreen);
        FontSpec gfs; gfs.face = "Monaco"; gfs.pixelHeight = 12;
        FontId gf = ad.createFont(gfs);

        stCheck(ad.contentGeneration(g) != 0,
                "gen: a live surface never reports 0 (the no-caching sentinel)");
        stCheck(ad.contentGeneration(SurfaceId::None) == 0,
                "gen: an unknown surface reports 0");

        unsigned long long last = ad.contentGeneration(g);
        auto moved = [&](const char* what) {
            const unsigned long long now = ad.contentGeneration(g);
            stCheck(now != last, what);
            last = now;
        };
        auto still = [&](const char* what) {
            const unsigned long long now = ad.contentGeneration(g);
            stCheck(now == last, what);
            last = now;
        };

        const Point pts[3] = { { 0, 0 }, { 3, 3 }, { 6, 0 } };
        PixelBuffer pb;
        ad.readPixels(g, { 0, 0, 2, 2 }, pb, PixelFormat::BGRA32);

        ad.setPixel(g, { 1, 1 }, kRed);                          moved("gen: setPixel");
        ad.writePixels(g, { 0, 0, 2, 2 }, pb);                   moved("gen: writePixels");
        ad.fillRect(g, { 0, 0, 4, 4 }, { BrushStyle::Solid, kRed });
                                                                 moved("gen: fillRect");
        ad.invertRect(g, { 0, 0, 2, 2 });                        moved("gen: invertRect");
        ad.drawEdge(g, { 0, 0, 4, 4 }, 4 /*BDR_RAISEDINNER*/, 15 /*BF_RECT*/);
                                                                 moved("gen: drawEdge");
        ad.shape(g, ShapeKind::Rect, { 1, 1, 3, 3 }, {}, {},
                 { PenStyle::Solid, 1, kBlue }, { BrushStyle::Solid, kGreen });
                                                                 moved("gen: shape");
        ad.line(g, { 0, 0 }, { 7, 7 }, { PenStyle::Solid, 1, kBlue });
                                                                 moved("gen: line");
        ad.polyline(g, pts, 3, { PenStyle::Solid, 1, kBlue });    moved("gen: polyline");
        ad.polygon(g, pts, 3, { PenStyle::Solid, 1, kBlue },
                   { BrushStyle::Solid, kGreen }, PolyFill::Winding);
                                                                 moved("gen: polygon");
        ad.bezier(g, pts, 3, { PenStyle::Solid, 1, kBlue });      moved("gen: bezier");
        ad.arc(g, { 0, 0, 8, 8 }, { 0, 0 }, { 7, 7 }, { PenStyle::Solid, 1, kBlue });
                                                                 moved("gen: arc");
        ad.floodFill(g, { 4, 4 }, kWhite, kBlack, FloodMode::Surface);
                                                                 moved("gen: floodFill");
        ad.gradient(g, { 0, 0, 8, 8 }, kRed, kBlue, true);        moved("gen: gradient");
        ad.blit(g, { 0, 0, 4, 4 }, gsrc, { 0, 0, 4, 4 }, BlitMode::Copy);
                                                                 moved("gen: blit (surface source)");
        ad.blit(g, { 0, 0, 2, 2 }, pb, { 0, 0, 2, 2 }, BlitMode::Copy);
                                                                 moved("gen: blit (pixel-buffer source)");
        ad.drawText(g, gf, "x", 1, { 0, 0 }, kWhite, nullptr);    moved("gen: drawText");

        // ...and the readers must NOT move it, or the cache is rebuilt on every
        // paint and the whole exercise buys nothing.
        ad.getPixel(g, { 1, 1 });                                still("gen: getPixel does not move it");
        ad.readPixels(g, { 0, 0, 2, 2 }, pb, PixelFormat::BGR24);
                                                                 still("gen: readPixels does not move it");
        ad.measureText(gf, "x", 1);                              still("gen: measureText does not move it");
        // A blit whose SOURCE is this surface changes the destination, not it.
        SurfaceId gdst = ad.createSurface({ 8, 8 }, kBlack);
        ad.blit(gdst, { 0, 0, 4, 4 }, g, { 0, 0, 4, 4 }, BlitMode::Copy);
        still("gen: being a blit SOURCE does not move it");
        stCheck(ad.contentGeneration(gdst) != ad.contentGeneration(g),
                "gen: ...but the blit DESTINATION moved");

        // Globally unique, not per-surface: two surfaces never share a
        // generation, so a cache keyed by it cannot be handed an entry
        // belonging to a destroyed surface whose id was recycled.
        SurfaceId ga = ad.createSurface({ 2, 2 }, kBlack);
        SurfaceId gb = ad.createSurface({ 2, 2 }, kBlack);
        stCheck(ad.contentGeneration(ga) != ad.contentGeneration(gb),
                "gen: two identical fresh surfaces do NOT share a generation");
        const unsigned long long genA = ad.contentGeneration(ga);
        ad.destroySurface(ga);
        SurfaceId gc = ad.createSurface({ 2, 2 }, kBlack);
        stCheck(ad.contentGeneration(gc) != genA,
                "gen: ...and a recycled id starts a new one");
        ad.destroySurface(gb);
        ad.destroySurface(gc);

        ad.destroyFont(gf);
        ad.destroySurface(gdst);
        ad.destroySurface(gsrc);
        ad.destroySurface(g);
    }

    printf("  dpi() reports: %d\n", ad.dpi());
    printf("PAL-SURFACE-SELFTEST: %d passed, %d failed\n", g_stPass, g_stFail);
    return g_stFail;
}

#endif // !_WIN32
