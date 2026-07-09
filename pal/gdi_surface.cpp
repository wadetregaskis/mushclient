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
    // createSurface(). Fonts and the measuring DC ARE adapter-owned.
    for (auto& kv : m_fonts) {
        kv.second.font->DeleteObject();
        delete kv.second.font;
    }
    m_fonts.clear();
    if (m_measureDc) {
        m_measureDc->DeleteDC();
        delete m_measureDc;
        m_measureDc = nullptr;
    }
    m_surfaces.clear();
}

GdiSurface::Surf* GdiSurface::surf(SurfaceId id) {
    auto it = m_surfaces.find((std::uint32_t)id);
    return it == m_surfaces.end() ? nullptr : &it->second;
}

GdiSurface::Font* GdiSurface::font(FontId id) {
    auto it = m_fonts.find((std::uint32_t)id);
    return it == m_fonts.end() ? nullptr : &it->second;
}

CDC* GdiSurface::measureDc() {
    if (!m_measureDc) {
        m_measureDc = new CDC;
        m_measureDc->CreateCompatibleDC(NULL);   // screen-compatible (LOGPIXELSY = screen)
        m_measureDc->SetTextAlign(TA_LEFT | TA_TOP);   // same recipe as the miniwindow pdc
    }
    return m_measureDc;
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

// ── text / fonts (S3b live path) ─────────────────────────────────────────────
// The seam dropped WindowText's per-call `Unicode` flag (the decided "UTF-8 +
// charset-hint" model), so drawText/measureText recover it by PROBING the bytes.
// This reproduces BOTH original Windows branches exactly:
//   valid UTF-8  -> MultiByteToWideChar(CP_UTF8) + ExtTextOutW  (== old Unicode=true)
//   otherwise    -> ExtTextOutA on the raw bytes               (== old Unicode=false,
//                   where GDI interprets them in the selected font's charset)
// Crucially the ANSI branch keeps LEGACY non-Unicode text correct on Windows
// (e.g. Latin-1 "caf\xE9" still renders "café", not "caf?"), which an
// unconditional CP_UTF8 path would break. The engine already rejects invalid
// UTF-8 when Unicode=true (CalculateUTF8length -> -3), so the only bytes reaching
// the ANSI branch are genuine legacy/ANSI text; the sole divergence from the old
// flag is the unreachable "Unicode=false yet coincidentally-valid-UTF-8 non-ASCII"
// input. The two-call NULL,0 sizing also fixes the old astral-char undersize bug.
//
// Utf8ToWide: valid UTF-8 fills `wide` and returns true; anything else -> false.
static bool Utf8ToWide(const char* utf8, size_t len, std::vector<WCHAR>& wide) {
    if (!utf8 || len == 0) return false;
    int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, (int)len, NULL, 0);
    if (n <= 0) return false;
    wide.resize(n);
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, &wide[0], n);
    return true;
}

FontId GdiSurface::createFont(const FontSpec& spec) {
    Font f;
    f.spec = spec;
    f.font = new CFont;
    // pixelHeight arrives POSITIVE (points converted via dpi() above the seam);
    // CreateFont wants the negative cell-height, exactly the old
    //   -MulDiv (Size, GetDeviceCaps (LOGPIXELSY), 72).
    // weight is already FW_BOLD/FW_NORMAL; the rest map 1:1 to the old CreateFont.
    if (f.font->CreateFont(-spec.pixelHeight, 0, 0, 0,
                           spec.weight,
                           (BYTE)spec.italic, (BYTE)spec.underline, (BYTE)spec.strikeout,
                           (BYTE)spec.charsetHint, 0, 0, 0,
                           (BYTE)spec.pitchAndFamilyHint, spec.face.c_str()) == 0) {
        delete f.font;
        return FontId::None;
    }
    std::uint32_t id = m_nextFont++;
    m_fonts[id] = f;
    return (FontId)id;
}

void GdiSurface::destroyFont(FontId id) {
    Font* f = font(id);
    if (!f) return;
    f->font->DeleteObject();
    delete f->font;
    m_fonts.erase((std::uint32_t)id);
}

int GdiSurface::drawText(SurfaceId sid, FontId fid, const char* utf8, size_t len, Point pt,
                         Colour colour, const Rect* clip) {
    Surf* s = surf(sid);
    Font* f = font(fid);
    if (!s || !s->dc || !f || !utf8 || len == 0) return 0;
    HDC hdc = s->dc->GetSafeHdc();
    CFont* oldFont = s->dc->SelectObject(f->font);
    s->dc->SetTextColor(colour);
    s->dc->SetBkMode(TRANSPARENT);            // WindowText is always transparent
    RECT rc, *prc = NULL;
    UINT eto = 0;
    if (clip) {
        rc.left = clip->x; rc.top = clip->y;
        rc.right = clip->x + clip->w; rc.bottom = clip->y + clip->h;
        prc = &rc; eto = ETO_CLIPPED;         // == the old CRect(...) + ETO_CLIPPED
    }
    int width = 0;
    std::vector<WCHAR> wide;
    if (Utf8ToWide(utf8, len, wide)) {
        ::ExtTextOutW(hdc, pt.x, pt.y, eto, prc, &wide[0], (int)wide.size(), NULL);
        SIZE sz = { 0, 0 };
        ::GetTextExtentPoint32W(hdc, &wide[0], (int)wide.size(), &sz);
        width = sz.cx;
    } else {
        ::ExtTextOutA(hdc, pt.x, pt.y, eto, prc, utf8, (int)len, NULL);
        SIZE sz = { 0, 0 };
        ::GetTextExtentPoint32A(hdc, utf8, (int)len, &sz);
        width = sz.cx;
    }
    s->dc->SelectObject(oldFont);
    return (clip && width > clip->w) ? clip->w : width;
}

Size GdiSurface::measureText(FontId fid, const char* utf8, size_t len) {
    Font* f = font(fid);
    if (!f || !utf8 || len == 0) return Size{};
    CDC* dc = measureDc();
    HDC hdc = dc->GetSafeHdc();
    CFont* oldFont = dc->SelectObject(f->font);
    SIZE sz = { 0, 0 };
    std::vector<WCHAR> wide;
    if (Utf8ToWide(utf8, len, wide))         // same probe as drawText -> same metrics
        ::GetTextExtentPoint32W(hdc, &wide[0], (int)wide.size(), &sz);
    else
        ::GetTextExtentPoint32A(hdc, utf8, (int)len, &sz);
    dc->SelectObject(oldFont);
    return { sz.cx, sz.cy };
}

FontMetrics GdiSurface::fontMetrics(FontId fid) {
    FontMetrics m;
    Font* f = font(fid);
    if (!f) return m;
    CDC* dc = measureDc();
    CFont* oldFont = dc->SelectObject(f->font);
    TEXTMETRIC tm;
    memset(&tm, 0, sizeof tm);
    if (dc->GetTextMetrics(&tm)) {           // == the old FontInfo pdc->GetTextMetrics
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
        m.italic = tm.tmItalic;              // raw bytes (WindowFontInfo returns verbatim)
        m.underlined = tm.tmUnderlined;
        m.struckOut = tm.tmStruckOut;
        m.pitchAndFamily = tm.tmPitchAndFamily;
        m.charset = tm.tmCharSet;
    }
    CString face;
    if (dc->GetTextFace(face) > 0)           // == the old FontInfo pdc->GetTextFace
        m.face = (const char*)face;          // realized face after substitution
    else
        m.face = f->spec.face;
    dc->SelectObject(oldFont);
    return m;
}

// ── lines / arcs (S3c live path) ─────────────────────────────────────────────
// makePen reproduces CMiniWindow::MakeAPen EXACTLY: the legacy CreatePen unless an
// endcap or join is non-default, in which case ExtCreatePen with a PS_GEOMETRIC
// solid pen — so Windows line/arc rendering is unchanged. (The engine already
// decoded the Win32 style bits into PenSpec via PenSpecFromWin32; here we re-encode
// them, and the round-trip is exact for every ValidatePenStyle-accepted input.)
static int psFromPenStyle(PenStyle st) {
    switch (st) {
    case PenStyle::Null:        return PS_NULL;
    case PenStyle::InsideFrame: return PS_INSIDEFRAME;
    case PenStyle::Dash:        return PS_DASH;
    case PenStyle::Dot:         return PS_DOT;
    case PenStyle::DashDot:     return PS_DASHDOT;
    case PenStyle::DashDotDot:  return PS_DASHDOTDOT;
    default:                    return PS_SOLID;
    }
}
static int psFromEndcap(PenEndcap e) {
    switch (e) { case PenEndcap::Square: return PS_ENDCAP_SQUARE;
                 case PenEndcap::Flat:   return PS_ENDCAP_FLAT;
                 default:                return PS_ENDCAP_ROUND; }
}
static int psFromJoin(PenJoin j) {
    switch (j) { case PenJoin::Bevel: return PS_JOIN_BEVEL;
                 case PenJoin::Miter: return PS_JOIN_MITER;
                 default:             return PS_JOIN_ROUND; }
}
static void makePen(CPen& pen, const PenSpec& p) {
    const int cap = psFromEndcap(p.endcap), join = psFromJoin(p.join);
    const int style = psFromPenStyle(p.style);
    if (cap == PS_ENDCAP_ROUND && join == PS_JOIN_ROUND) {
        pen.CreatePen(style, p.width, p.colour);            // legacy path (== MakeAPen)
    } else {
        LOGBRUSH lb; lb.lbStyle = BS_SOLID; lb.lbColor = p.colour; lb.lbHatch = 0;
        pen.Attach(::ExtCreatePen(style | cap | join | PS_GEOMETRIC, p.width, &lb, 0, NULL));
    }
}

void GdiSurface::line(SurfaceId id, Point p1, Point p2, const PenSpec& pen) {
    Surf* s = surf(id);
    if (!s || !s->dc || pen.style == PenStyle::Null) return;
    s->dc->SetBkMode(TRANSPARENT);                          // == old Line() SetBkMode
    CPen cpen; makePen(cpen, pen);
    CPen* old = s->dc->SelectObject(&cpen);
    s->dc->MoveTo(p1.x, p1.y);
    s->dc->LineTo(p2.x, p2.y);                              // == old MoveTo/LineTo
    s->dc->SelectObject(old);
}

void GdiSurface::arc(SurfaceId id, Rect r, Point a, Point b, const PenSpec& pen) {
    Surf* s = surf(id);
    if (!s || !s->dc) return;
    s->dc->SetBkMode(TRANSPARENT);                          // == old Arc() SetBkMode
    CPen cpen; makePen(cpen, pen);
    CPen* old = s->dc->SelectObject(&cpen);
    s->dc->Arc(r.x, r.y, r.x + r.w, r.y + r.h, a.x, a.y, b.x, b.y);   // == old pdc->Arc
    s->dc->SelectObject(old);
}

void GdiSurface::bezier(SurfaceId id, const Point* pts, size_t n, const PenSpec& pen) {
    Surf* s = surf(id);
    if (!s || !s->dc || !pts || n < 4) return;
    s->dc->SetBkMode(TRANSPARENT);                          // == old Bezier() SetBkMode
    CPen cpen; makePen(cpen, pen);
    CPen* old = s->dc->SelectObject(&cpen);
    std::vector<POINT> v(n);
    for (size_t i = 0; i < n; ++i) { v[i].x = pts[i].x; v[i].y = pts[i].y; }
    s->dc->PolyBezier(v.data(), (int)n);                    // == old pdc->PolyBezier
    s->dc->SelectObject(old);
}

// ── op families not yet on the seam — stubbed until their CMiniWindow family
//    migrates (each will map to the exact original GDI call, as above) ─────────
SurfaceId   GdiSurface::createSurface(Size, Colour) { return SurfaceId::None; }
void        GdiSurface::readPixels(SurfaceId, Rect, PixelBuffer& out, PixelFormat) { out = PixelBuffer{}; }
void        GdiSurface::writePixels(SurfaceId, Rect, const PixelBuffer&) {}
Colour      GdiSurface::getPixel(SurfaceId, Point) { return 0xFFFFFFFFu; }   // CLR_INVALID
void        GdiSurface::shape(SurfaceId, ShapeKind, Rect, Point, Point, const PenSpec&, const BrushSpec&) {}
void        GdiSurface::polyline(SurfaceId, const Point*, size_t, const PenSpec&) {}
void        GdiSurface::polygon(SurfaceId, const Point*, size_t, const PenSpec&, const BrushSpec&, PolyFill) {}
void        GdiSurface::gradient(SurfaceId, Rect, Colour, Colour, bool) {}
void        GdiSurface::blit(SurfaceId, Rect, SurfaceId, Rect, BlitMode, Colour, BlitQuality, const Matrix2x3*) {}
void        GdiSurface::blit(SurfaceId, Rect, const PixelBuffer&, Rect, BlitMode, Colour, BlitQuality, const Matrix2x3*) {}

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
