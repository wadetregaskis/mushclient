// platform/wx_gdi_compat.cpp
// wx-backed implementations of the MFC GDI compatibility classes.
//
// CDC, CBitmap, CPen, CBrush, CFont — and the global helpers GetDeviceCaps()
// and ExtCreatePen() — are declared in platform/mfc_ui.h (under
// MUSHCLIENT_FULL_BUILD) and implemented here using wxWidgets primitives.
//
// This file is compiled into mushgui only (it needs wxWidgets headers).
// mushapp objects (miniwindow.cpp etc.) reference these symbols as undefined
// externals in libmushapp.a; the linker resolves them at the mushgui link
// step from this translation unit.
//
// Include order: wx headers MUST come before MUSHclient headers.

#define MUSHCLIENT_FULL_BUILD   // mfc_ui.h: use declarations, not inline stubs

// ── wxWidgets ────────────────────────────────────────────────────────────
#include <wx/wx.h>
#include <wx/dcmemory.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/font.h>
#include <wx/pen.h>
#include <wx/brush.h>

// ── MUSHclient platform headers ──────────────────────────────────────────
// stdafx.h pulls in compat.h → mfc_ui.h (with MUSHCLIENT_FULL_BUILD set
// above, so declarations only — no inline stubs).
#include "stdafx.h"

#include <algorithm>
#include <vector>
#include <cstring>
#include <atomic>
#include <unordered_map>

// ── Colour helpers ───────────────────────────────────────────────────────
// COLORREF is stored as 0x00BBGGRR (same as Windows).

static inline wxColour WxCol(COLORREF c) {
    return wxColour(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
}

static inline COLORREF CRef(const wxColour& c) {
    return (COLORREF)c.Red() | ((COLORREF)c.Green() << 8) | ((COLORREF)c.Blue() << 16);
}

// ── CreateDIBSection / DIBSection infrastructure ─────────────────────────
//
// MUSHclient's miniwindow pixel-manipulation functions (BlendImage, Filter,
// MergeImageAlpha, DrawImageAlpha, GetImageAlpha, TransformImage, Write, etc.)
// use CreateDIBSection to get a raw pixel buffer, then blit miniwindow content
// into a CDC that has that DIBSection selected, then manipulate/read the raw
// buffer directly.
//
// The Windows pattern:
//   HBITMAP hbm = CreateDIBSection(..., &pBits, NULL, 0);
//   SelectObject(A_DC.m_hDC, hbm);          // raw Win32, selects into HDC
//   A_DC.BitBlt(0, 0, w, h, &src, ...);     // copies pixels; end up in pBits
//   // pBits now contains the raw pixel data
//   pBits[i] = ...;                          // direct pixel manipulation
//
// On non-Windows, m_hDC is normally nullptr (no real HDC), so
// SelectObject(m_hDC, hbm) is a stub.  To make this work we:
//  1. Give each CDC a unique fake HDC.
//  2. Track "DIBSection selected in which CDC" via g_hdcToSelectedDib.
//  3. In SelectObject(HDC, HGDIOBJ): if the object is a DIBSection, also
//     call CDC::SelectObject to put the DIBSection's wxBitmap into the DC.
//  4. After BitBlt INTO a CDC with a DIBSection: sync wxBitmap→raw buffer.
//  5. Before BitBlt FROM a CDC with a DIBSection: sync raw buffer→wxBitmap.

struct DIBSectionData {
    int width  = 0;
    int height = 0;     // always abs(biHeight)
    bool bottomUp = true; // biHeight > 0 → rows stored bottom-up in buffer
    int stride = 0;     // bytes per row, padded to 4-byte boundary
    int bytesPerPixel = 3; // 3 = 24-bit BGR, 4 = 32-bit BGRA (e.g. PNG w/ alpha)
    std::vector<unsigned char> pixels; // raw BGR(A) pixel data
    wxBitmap* bmp = nullptr;  // wxBitmap for rendering (owned here)

    ~DIBSectionData() { delete bmp; }

    // Copy pixels from wxBitmap into raw buffer (call after BitBlt INTO this DC).
    void SyncBitmapToPixels() {
        if (!bmp || !bmp->IsOk()) return;
        const int bpp = bytesPerPixel;
        wxImage img = bmp->ConvertToImage();
        const bool srcAlpha = (bpp == 4) && img.HasAlpha();
        for (int y = 0; y < height; ++y) {
            // img is always top-down; pixels buffer may be bottom-up
            int imgRow = bottomUp ? (height - 1 - y) : y;
            unsigned char* dst = pixels.data() + y * stride;
            for (int x = 0; x < width; ++x) {
                dst[x*bpp+0] = img.GetBlue (x, imgRow);  // BGR
                dst[x*bpp+1] = img.GetGreen(x, imgRow);
                dst[x*bpp+2] = img.GetRed  (x, imgRow);
                if (bpp == 4)
                    dst[x*bpp+3] = srcAlpha ? img.GetAlpha(x, imgRow) : 255;
            }
        }
    }

    // Copy raw buffer into wxBitmap (call before BitBlt FROM this DC).
    void SyncPixelsToBitmap() {
        if (width <= 0 || height <= 0) return;
        const int bpp = bytesPerPixel;
        // GDI ignores alpha when rendering; wx honours it. A 32bpp buffer
        // whose alpha channel is ALL zero (e.g. a surface readback — GDI-drawn
        // content leaves 0 in the high byte, as does ImageFromWindow) must
        // render opaquely the way BitBlt would, not invisibly — so only carry
        // an alpha channel when some pixel actually has alpha.
        bool anyAlpha = false;
        if (bpp == 4) {
            for (int y = 0; y < height && !anyAlpha; ++y) {
                const unsigned char* src = pixels.data() + y * stride;
                for (int x = 0; x < width; ++x)
                    if (src[x*4+3]) { anyAlpha = true; break; }
            }
        }
        wxImage img(width, height);
        if (bpp == 4 && anyAlpha) img.SetAlpha();
        for (int y = 0; y < height; ++y) {
            int bufRow = bottomUp ? (height - 1 - y) : y;
            const unsigned char* src = pixels.data() + bufRow * stride;
            for (int x = 0; x < width; ++x) {
                const unsigned char* px = src + x*bpp;
                img.SetRGB(x, y, px[2], px[1], px[0]);  // BGR→RGB
                if (bpp == 4 && anyAlpha)
                    img.SetAlpha(x, y, px[3]);
            }
        }
        delete bmp;
        bmp = new wxBitmap(img, 32);
    }
};

// Global maps: HBITMAP→DIBSectionData, HDC→selected HBITMAP, HDC→CDC*
static std::unordered_map<HBITMAP, DIBSectionData*> g_dibSections;
static std::unordered_map<HDC,    HBITMAP>          g_hdcToSelectedDib;
static std::unordered_map<HDC,    CDC*>              g_hdcToCDC;
static std::atomic<uintptr_t> s_hdcCounter{1000};

// Helper: look up any DIBSection selected into the given HDC.
static inline DIBSectionData* GetDIBForHDC(HDC hdc) {
    auto it = g_hdcToSelectedDib.find(hdc);
    if (it == g_hdcToSelectedDib.end()) return nullptr;
    auto it2 = g_dibSections.find(it->second);
    return (it2 != g_dibSections.end()) ? it2->second : nullptr;
}

// Materialize a DIBSection handle (DIBSectionData*) into a standalone wxBitmap,
// freeing the DIBSection.  Used by CBitmap::Attach so that a CBitmap's m_impl is
// ALWAYS a plain wxBitmap* — the rest of the GDI shim (GetBitmap, SelectObject,
// the destructors, …) can then treat m_impl uniformly with no special-casing.
// Returns nullptr if hObject is not a DIBSection handle.
static wxBitmap* MaterializeDIBSection(HGDIOBJ hObject) {
    auto it = g_dibSections.find(reinterpret_cast<HBITMAP>(hObject));
    if (it == g_dibSections.end()) return nullptr;
    DIBSectionData* dib = it->second;
    dib->SyncPixelsToBitmap();   // raw pixel buffer → wxBitmap
    wxBitmap* out = (dib->bmp && dib->bmp->IsOk()) ? new wxBitmap(*dib->bmp)
                                                   : new wxBitmap(1, 1, 32);
    g_dibSections.erase(it);
    delete dib;                  // owns its pixel buffer + working wxBitmap
    return out;
}

// CreateDIBSection — non-Windows portable implementation.
// Allocates a raw pixel buffer, creates a wxBitmap, and ties them together.
// The returned HBITMAP is actually a DIBSectionData* cast to HBITMAP.
HBITMAP CreateDIBSection(HDC /*hdc*/, const BITMAPINFO* pbmi, UINT /*iUsage*/,
                          void** ppvBits, HANDLE /*hSection*/, DWORD /*offset*/)
{
    if (!pbmi || !ppvBits) return nullptr;
    const BITMAPINFOHEADER& h = pbmi->bmiHeader;

    int w  = abs(h.biWidth);
    int he = abs((int)h.biHeight);
    bool bottomUp = ((int)h.biHeight > 0);

    // Only 24-bit DIBs are used by MUSHclient miniwindow code.
    int bpp = (h.biBitCount > 0) ? h.biBitCount : 24;
    int bytesPerPixel = (bpp + 7) / 8;
    int stride = (w * bytesPerPixel + 3) & ~3;

    DIBSectionData* dib = new DIBSectionData();
    dib->width    = w;
    dib->height   = he;
    dib->bottomUp = bottomUp;
    dib->stride   = stride;
    dib->bytesPerPixel = bytesPerPixel;
    dib->pixels.assign(stride * (he > 0 ? he : 1), 0);
    dib->bmp      = new wxBitmap(w > 0 ? w : 1, he > 0 ? he : 1, 32);

    *ppvBits = dib->pixels.data();

    HBITMAP handle = reinterpret_cast<HBITMAP>(dib);
    g_dibSections[handle] = dib;
    return handle;
}

// DeleteObject(HGDIOBJ) — free a CreateDIBSection handle. The global Win32
// DeleteObject is called by miniwindow.cpp on raw CreateDIBSection HBITMAPs
// (BlendImage/Filter/MergeImageAlpha/Gradient temporaries). Without a real
// implementation each call leaked the DIBSectionData + pixel buffer + wxBitmap
// and left a stale g_dibSections entry. (CBitmap objects free themselves via
// the CBitmap::DeleteObject member, so a non-DIBSection handle is a no-op here.)
BOOL DeleteObject(HGDIOBJ hObject) {
    if (!hObject) return FALSE;
    auto it = g_dibSections.find(reinterpret_cast<HBITMAP>(hObject));
    if (it == g_dibSections.end())
        return TRUE;             // not a DIBSection handle — nothing tracked to free
    // Drop any HDC→selected-DIB tracking that still points at this handle so a
    // later GetDIBForHDC can't dereference the freed DIBSectionData. (The DC's own
    // wxMemoryDC keeps a ref-counted copy of the bitmap, so this is just bookkeeping.)
    for (auto s = g_hdcToSelectedDib.begin(); s != g_hdcToSelectedDib.end(); ) {
        if (s->second == it->first) s = g_hdcToSelectedDib.erase(s);
        else ++s;
    }
    delete it->second;           // owns its pixel buffer + working wxBitmap
    g_dibSections.erase(it);
    return TRUE;
}

// SelectObject(HDC, HGDIOBJ) is defined near the END of this file, after the
// CDCImpl struct is fully declared — it needs CDCImpl's members (impl->dc).
// See the definition following WxBitmapFromCBitmap().

// ── CDCImpl — internal state for CDC ────────────────────────────────────

struct CDCImpl {
    wxMemoryDC dc;

    // Currently selected GDI objects (so SelectObject can return the previous)
    CBitmap* currentBitmap = nullptr;
    CPen*    currentPen    = nullptr;
    CBrush*  currentBrush  = nullptr;
    CFont*   currentFont   = nullptr;

    // Attribute state mirrored from DC
    COLORREF textColor = 0x000000;
    COLORREF bkColor   = 0xFFFFFF;
    int      bkMode    = TRANSPARENT;
    UINT     textAlign = TA_LEFT | TA_TOP;
    int      rop2      = R2_COPYPEN;
    int      stretchBltMode = COLORONCOLOR;
    int      polyFillMode   = ALTERNATE;

    // Current drawing position (for LineTo/MoveTo)
    int curX = 0, curY = 0;

    // Save/restore stack entries
    struct Snapshot {
        COLORREF textColor, bkColor;
        int bkMode, rop2, stretchBltMode, polyFillMode;
        UINT textAlign;
        int curX, curY;
        CBitmap* currentBitmap;
        CPen*    currentPen;
        CBrush*  currentBrush;
        CFont*   currentFont;
    };
    std::vector<Snapshot> snapshots;

    // Apply currently selected pen and brush to the underlying DC.
    // Called before every drawing operation.
    void ApplyPen();
    void ApplyBrush();
    void ApplyFont();
    void ApplyBkMode();
};

// ── Apply helpers ────────────────────────────────────────────────────────

void CDCImpl::ApplyPen() {
    if (currentPen && currentPen->m_impl) {
        dc.SetPen(*static_cast<wxPen*>(currentPen->m_impl));
    } else {
        dc.SetPen(*wxBLACK_PEN);
    }
}

void CDCImpl::ApplyBrush() {
    if (currentBrush && currentBrush->m_impl) {
        dc.SetBrush(*static_cast<wxBrush*>(currentBrush->m_impl));
    } else {
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
    }
}

void CDCImpl::ApplyFont() {
    if (currentFont && currentFont->m_impl) {
        dc.SetFont(*static_cast<wxFont*>(currentFont->m_impl));
    }
    // text colour
    dc.SetTextForeground(WxCol(textColor));
    if (bkMode == OPAQUE) {
        dc.SetBackgroundMode(wxSOLID);
        dc.SetTextBackground(WxCol(bkColor));
    } else {
        dc.SetBackgroundMode(wxTRANSPARENT);
    }
}

void CDCImpl::ApplyBkMode() {
    if (bkMode == OPAQUE) {
        dc.SetBackgroundMode(wxSOLID);
        dc.SetTextBackground(WxCol(bkColor));
    } else {
        dc.SetBackgroundMode(wxTRANSPARENT);
    }
}

// ── Pen-style mapping ────────────────────────────────────────────────────

static wxPenStyle MapPenStyle(int ps) {
    switch (ps & PS_STYLE_MASK) {
        case PS_SOLID:       return wxPENSTYLE_SOLID;
        case PS_DASH:        return wxPENSTYLE_SHORT_DASH;
        case PS_DOT:         return wxPENSTYLE_DOT;
        case PS_DASHDOT:     return wxPENSTYLE_DOT_DASH;
        case PS_DASHDOTDOT:  return wxPENSTYLE_DOT_DASH;   // closest match
        case PS_NULL:        return wxPENSTYLE_TRANSPARENT;
        case PS_INSIDEFRAME: return wxPENSTYLE_SOLID;
        default:             return wxPENSTYLE_SOLID;
    }
}

static wxPenCap MapPenCap(int ps) {
    switch (ps & PS_ENDCAP_MASK) {
        case PS_ENDCAP_ROUND:  return wxCAP_ROUND;
        case PS_ENDCAP_SQUARE: return wxCAP_PROJECTING;
        case PS_ENDCAP_FLAT:   return wxCAP_BUTT;
        default:               return wxCAP_ROUND;
    }
}

static wxPenJoin MapPenJoin(int ps) {
    switch (ps & PS_JOIN_MASK) {
        case PS_JOIN_BEVEL: return wxJOIN_BEVEL;
        case PS_JOIN_MITER: return wxJOIN_MITER;
        default:            return wxJOIN_ROUND;
    }
}

// ── Hatch-brush style mapping ────────────────────────────────────────────

static wxBrushStyle MapHatchStyle(int hs) {
    switch (hs) {
        case HS_HORIZONTAL: return wxBRUSHSTYLE_HORIZONTAL_HATCH;
        case HS_VERTICAL:   return wxBRUSHSTYLE_VERTICAL_HATCH;
        case HS_FDIAGONAL:  return wxBRUSHSTYLE_FDIAGONAL_HATCH;
        case HS_BDIAGONAL:  return wxBRUSHSTYLE_BDIAGONAL_HATCH;
        case HS_CROSS:      return wxBRUSHSTYLE_CROSS_HATCH;
        case HS_DIAGCROSS:  return wxBRUSHSTYLE_CROSSDIAG_HATCH;
        default:            return wxBRUSHSTYLE_SOLID;
    }
}

// ── ROP2 mapping ─────────────────────────────────────────────────────────

static wxRasterOperationMode MapRop(DWORD dwRop) {
    switch (dwRop) {
        case SRCCOPY:    return wxCOPY;
        case SRCPAINT:   return wxOR;
        case SRCAND:     return wxAND;
        case SRCINVERT:  return wxXOR;
        case DSTINVERT:  return wxINVERT;
        case BLACKNESS:  return wxCLEAR;
        case WHITENESS:  return wxSET;
        case PATCOPY:    return wxCOPY;
        case PATINVERT:  return wxXOR;
        default:         return wxCOPY;
    }
}

// ╔══════════════════════════════════════════════════════════════════════╗
// ║  CGdiObject                                                          ║
// ╚══════════════════════════════════════════════════════════════════════╝

CGdiObject::~CGdiObject() {
    // Subclass destructors free m_impl; nothing to do here.
}

bool CGdiObject::DeleteObject() {
    // Default: nothing owned at this level.
    m_hObject = nullptr;
    return true;
}

// ╔══════════════════════════════════════════════════════════════════════╗
// ║  CPen                                                                ║
// ╚══════════════════════════════════════════════════════════════════════╝

CPen::CPen(int style, int width, COLORREF crColor) {
    CreatePen(style, width, crColor);
}

CPen::~CPen() {
    if (m_impl) {
        delete static_cast<wxPen*>(m_impl);
        m_impl = nullptr;
    }
}

bool CPen::CreatePen(int nPenStyle, int nWidth, COLORREF crColor) {
    if (m_impl) { delete static_cast<wxPen*>(m_impl); m_impl = nullptr; }
    if ((nPenStyle & PS_STYLE_MASK) == PS_NULL) {
        m_impl = new wxPen(*wxBLACK_PEN);
        static_cast<wxPen*>(m_impl)->SetStyle(wxPENSTYLE_TRANSPARENT);
        return true;
    }
    wxPen* pen = new wxPen(WxCol(crColor), nWidth, MapPenStyle(nPenStyle));
    pen->SetCap(MapPenCap(nPenStyle));
    pen->SetJoin(MapPenJoin(nPenStyle));
    m_impl = pen;
    return true;
}

BOOL CPen::Attach(HGDIOBJ hObject) {
    // hObject is a heap-allocated wxPen* cast to HGDIOBJ by ExtCreatePen.
    if (m_impl) { delete static_cast<wxPen*>(m_impl); m_impl = nullptr; }
    m_hObject = hObject;
    m_impl = hObject;   // wxPen* stored directly
    return TRUE;
}

HGDIOBJ CPen::Detach() {
    HGDIOBJ h = m_hObject;
    m_hObject = nullptr;
    m_impl = nullptr;
    return h;
}

// ╔══════════════════════════════════════════════════════════════════════╗
// ║  CBrush                                                              ║
// ╚══════════════════════════════════════════════════════════════════════╝

CBrush::CBrush(COLORREF c) { CreateSolidBrush(c); }

CBrush::~CBrush() {
    if (m_impl) { delete static_cast<wxBrush*>(m_impl); m_impl = nullptr; }
}

bool CBrush::CreateSolidBrush(COLORREF crColor) {
    if (m_impl) { delete static_cast<wxBrush*>(m_impl); m_impl = nullptr; }
    m_impl = new wxBrush(WxCol(crColor), wxBRUSHSTYLE_SOLID);
    return true;
}

bool CBrush::CreateHatchBrush(int nIndex, COLORREF crColor) {
    if (m_impl) { delete static_cast<wxBrush*>(m_impl); m_impl = nullptr; }
    m_impl = new wxBrush(WxCol(crColor), MapHatchStyle(nIndex));
    return true;
}

bool CBrush::CreatePatternBrush(CBitmap* pBitmap) {
    if (!pBitmap || !pBitmap->m_impl) return false;
    if (m_impl) { delete static_cast<wxBrush*>(m_impl); m_impl = nullptr; }
    m_impl = new wxBrush(*static_cast<wxBitmap*>(pBitmap->m_impl));
    return true;
}

bool CBrush::CreateBrushIndirect(const LOGBRUSH* plb) {
    if (!plb) return false;
    if (m_impl) { delete static_cast<wxBrush*>(m_impl); m_impl = nullptr; }
    switch (plb->lbStyle) {
        case BS_SOLID:
            m_impl = new wxBrush(WxCol(plb->lbColor), wxBRUSHSTYLE_SOLID);
            return true;
        case BS_NULL:   // BS_HOLLOW == BS_NULL, so only one case label needed
            m_impl = new wxBrush(*wxTRANSPARENT_BRUSH);
            return true;
        case BS_HATCHED:
            m_impl = new wxBrush(WxCol(plb->lbColor), MapHatchStyle((int)plb->lbHatch));
            return true;
        default:
            m_impl = new wxBrush(WxCol(plb->lbColor), wxBRUSHSTYLE_SOLID);
            return true;
    }
}

// ╔══════════════════════════════════════════════════════════════════════╗
// ║  CFont                                                               ║
// ╚══════════════════════════════════════════════════════════════════════╝

CFont::~CFont() {
    if (m_impl) { delete static_cast<wxFont*>(m_impl); m_impl = nullptr; }
}

bool CFont::CreateFont(int nHeight, int /*nWidth*/, int /*nEscapement*/,
                       int /*nOrientation*/, int nWeight, BYTE bItalic,
                       BYTE bUnderline, BYTE cStrikeOut, BYTE /*nCharSet*/,
                       BYTE /*nOutPrec*/, BYTE /*nClipPrec*/, BYTE /*nQuality*/,
                       BYTE /*nPitchFamily*/, LPCTSTR lpszFaceName)
{
    if (m_impl) { delete static_cast<wxFont*>(m_impl); m_impl = nullptr; }

    // nHeight < 0: height in logical pixels (common MFC usage)
    // nHeight > 0: cell height; nHeight == 0: default
    int pixH = (nHeight < 0) ? -nHeight : (nHeight > 0 ? nHeight : 12);

    // Every caller derives that height the Win32 way — -MulDiv (points,
    // GetDeviceCaps (LOGPIXELSY), 72) — see ChangeFont (doc.cpp), FixFont
    // (Utilities.cpp) and WindowFont (miniwindow.cpp).  On Windows LOGPIXELSY is
    // 96, so the number stays close to the point size; on the port it is the
    // REAL display PPI, which is 144 on a Retina Mac.  A 9pt request therefore
    // arrives here as 18, and wx measures fonts in logical points on macOS, so
    // handing that to wxFontInfo(wxSize(0, pixH)) yields a ~15pt font.
    // Undo the caller's conversion through the same PPI: the round-trip is
    // exact and every caller gets the point size it actually asked for.
    const int ppi    = wxGetDisplayPPI().y;
    const int points = (ppi > 0) ? MulDiv(pixH, 72, ppi) : pixH;

    wxFontInfo fi(points > 0 ? points : 1);
    if (lpszFaceName && *lpszFaceName)
        fi.FaceName(wxString::FromUTF8(lpszFaceName));
    if (nWeight >= FW_BOLD) fi.Bold();
    if (bItalic)    fi.Italic();
    if (bUnderline) fi.Underlined();
    if (cStrikeOut) fi.Strikethrough();

    wxFont* font = new wxFont(fi);
    if (!font->IsOk()) { delete font; return false; }
    m_impl = font;
    return true;
}

bool CFont::CreateFontIndirect(const LOGFONT* lplf) {
    if (!lplf) return false;
    return CreateFont(lplf->lfHeight, lplf->lfWidth,
                      lplf->lfEscapement, lplf->lfOrientation,
                      lplf->lfWeight, lplf->lfItalic,
                      lplf->lfUnderline, lplf->lfStrikeOut,
                      lplf->lfCharSet, lplf->lfOutPrecision,
                      lplf->lfClipPrecision, lplf->lfQuality,
                      lplf->lfPitchAndFamily, lplf->lfFaceName);
}

bool CFont::CreatePointFont(int nPointSize, LPCTSTR lpszFaceName, void*) {
    // nPointSize is in tenths of a point
    if (m_impl) { delete static_cast<wxFont*>(m_impl); m_impl = nullptr; }
    wxFontInfo fi(nPointSize / 10);
    if (lpszFaceName && *lpszFaceName)
        fi.FaceName(wxString::FromUTF8(lpszFaceName));
    wxFont* font = new wxFont(fi);
    if (!font->IsOk()) { delete font; return false; }
    m_impl = font;
    return true;
}

bool CFont::CreatePointFontIndirect(const LOGFONT* lplf, void*) {
    return lplf ? CreateFontIndirect(lplf) : false;
}

int CFont::GetLogFont(LOGFONT* lf) {
    if (!lf) return 0;
    *lf = {};
    if (!m_impl) return 0;
    wxFont* f = static_cast<wxFont*>(m_impl);
    // Symmetric with CreateFont: report the logical-pixel height a caller would
    // have passed in, i.e. -MulDiv (points, LOGPIXELSY, 72).  Reporting the raw
    // wx pixel size instead would shrink the font on every
    // GetLogFont → CreateFontIndirect round-trip (StatLink.cpp:74 does exactly
    // that to derive its underlined variant).
    const int ppi = wxGetDisplayPPI().y;
    lf->lfHeight = -MulDiv(f->GetPointSize(), (ppi > 0 ? ppi : 72), 72);
    lf->lfWeight = f->GetWeight() == wxFONTWEIGHT_BOLD ? FW_BOLD : FW_NORMAL;
    lf->lfItalic = (f->GetStyle() != wxFONTSTYLE_NORMAL) ? 1 : 0;
    lf->lfUnderline = f->GetUnderlined() ? 1 : 0;
    lf->lfStrikeOut = f->GetStrikethrough() ? 1 : 0;
    wxString face = f->GetFaceName();
    strncpy(lf->lfFaceName, face.ToUTF8().data(),
            sizeof(lf->lfFaceName) - 1);
    return sizeof(LOGFONT);
}

int CFont::GetObject(int /*nCount*/, LOGFONT* lf) const {
    if (!lf || !m_impl) return 0;
    const_cast<CFont*>(this)->GetLogFont(lf);
    return sizeof(LOGFONT);
}

// ╔══════════════════════════════════════════════════════════════════════╗
// ║  CBitmap                                                             ║
// ╚══════════════════════════════════════════════════════════════════════╝

CBitmap::~CBitmap() {
    if (m_impl) { delete static_cast<wxBitmap*>(m_impl); m_impl = nullptr; }
}

bool CBitmap::CreateBitmap(int nWidth, int nHeight, UINT nPlanes,
                           UINT nBitCount, const void* lpBits)
{
    if (m_impl) { delete static_cast<wxBitmap*>(m_impl); m_impl = nullptr; }
    int w = (nWidth  > 0) ? nWidth  : 1;
    int h = (nHeight > 0) ? nHeight : 1;

    if (lpBits && nBitCount == 1) {
        // Monochrome bitmap from raw bit data (used for pattern brushes).
        // Windows DIB scanlines are WORD-aligned, not byte-packed -- each row
        // occupies stride bytes even though only the first ceil(w/8) hold bits.
        // We convert to a wxImage (8x8) then create a wxBitmap.
        wxImage img(w, h);
        img.InitAlpha();
        const unsigned char* src = static_cast<const unsigned char*>(lpBits);
        const int stride = ((w * (int)nBitCount + 15) / 16) * 2;
        for (int y = 0; y < h && y < 8; ++y) {
            unsigned char row = src[y * stride];
            for (int x = 0; x < w && x < 8; ++x) {
                bool set = (row & (0x80u >> x)) != 0;
                unsigned char v = set ? 255 : 0;   // GDI mono: 1=background, 0=text (white/black with the default DC colours)
                img.SetRGB(x, y, v, v, v);
                img.SetAlpha(x, y, 255);
            }
        }
        m_impl = new wxBitmap(img, 32);
    } else {
        // Empty bitmap (most common case — miniwindow canvas)
        m_impl = new wxBitmap(w, h, 32);
    }
    return static_cast<wxBitmap*>(m_impl)->IsOk();
}

// The backing-store scale for drawing canvases: 2 on a Retina panel, 1 on an
// ordinary one.  Derived from the same PPI everything else in this shim uses,
// so a 72 dpi display gets scale 1 and behaves exactly as before.
double wx_canvas_scale()
{
    const int ppi = wxGetDisplayPPI().y;
    if (ppi <= 0) return 1.0;
    const double s = ppi / 72.0;
    return (s < 1.0) ? 1.0 : s;
}

bool CBitmap::CreateCanvasBitmap(int nWidth, int nHeight) {
    if (m_impl) { delete static_cast<wxBitmap*>(m_impl); m_impl = nullptr; }
    const int w = (nWidth  > 0) ? nWidth  : 1;
    const int h = (nHeight > 0) ? nHeight : 1;

    // CreateWithDIPSize allocates w*scale x h*scale physical pixels and records
    // the scale on the bitmap, so a wxMemoryDC over it exposes a w x h LOGICAL
    // coordinate space.  Every existing draw call keeps its coordinates; the
    // difference is that glyphs and lines are rasterised at full device
    // resolution instead of being drawn at 1x and stretched on the way out.
    wxBitmap* bmp = new wxBitmap;
    if (!bmp->CreateWithDIPSize(wxSize(w, h), wx_canvas_scale(), 32)) {
        delete bmp;
        return false;
    }
    m_impl = bmp;
    return true;
}

HBITMAP CBitmap::CreateCompatibleBitmap(void* /*pDC*/, int nWidth, int nHeight) {
    CreateBitmap(nWidth, nHeight, 1, 32, nullptr);
    return static_cast<HBITMAP>(m_impl);   // opaque handle
}

// Bitmap dimensions are reported in LOGICAL units — physical pixels divided by
// the bitmap's scale.  A HiDPI canvas or captured image holds 2x the pixels on a
// Retina panel, but every caller above this shim (CMiniWindow::DrawImage's
// source-rectangle arithmetic, WindowImageInfo, the plugins themselves) works in
// the same pixel coordinates it does on Windows.  Keeping the extra resolution
// invisible here is what lets those callers stay unchanged while the drawing
// underneath happens at full device resolution.
static inline int wx_logical_extent(int physical, double scale)
{
    if (scale <= 1.0) return physical;
    const int s = (int)(scale + 0.5);
    return (s > 0) ? physical / s : physical;
}

BOOL CBitmap::GetBitmap(BITMAP* pBitmap) {
    if (!pBitmap || !m_impl) return FALSE;
    wxBitmap* bmp = static_cast<wxBitmap*>(m_impl);
    *pBitmap = {};
    const double sc = bmp->GetScaleFactor();
    pBitmap->bmWidth   = wx_logical_extent(bmp->GetWidth(),  sc);
    pBitmap->bmHeight  = wx_logical_extent(bmp->GetHeight(), sc);
    pBitmap->bmBitsPixel = bmp->GetDepth();
    pBitmap->bmPlanes  = 1;
    return TRUE;
}

int CBitmap::GetObject(int /*nCount*/, void* lpObject) const {
    if (!lpObject || !m_impl) return 0;
    wxBitmap* bmp = static_cast<wxBitmap*>(m_impl);
    BITMAP* pbm = static_cast<BITMAP*>(lpObject);
    *pbm = {};
    const double sc = bmp->GetScaleFactor();
    pbm->bmWidth      = wx_logical_extent(bmp->GetWidth(),  sc);
    pbm->bmHeight     = wx_logical_extent(bmp->GetHeight(), sc);
    pbm->bmBitsPixel  = bmp->GetDepth();
    pbm->bmPlanes     = 1;
    return sizeof(BITMAP);
}

BOOL CBitmap::Attach(HGDIOBJ hObject) {
    if (m_impl) { delete static_cast<wxBitmap*>(m_impl); m_impl = nullptr; }
    m_hObject = hObject;
    // hObject is normally a wxBitmap*, but the PNG/BMP loaders hand us a
    // DIBSection handle (CreateDIBSection → LoadPng) whose live image lives in a
    // raw pixel buffer.  Materialize that into a standalone wxBitmap so m_impl is
    // always a plain wxBitmap* for the rest of the GDI shim (this is what makes
    // WindowDrawImage of a loaded PNG actually render instead of crashing).
    if (wxBitmap* materialized = MaterializeDIBSection(hObject)) {
        m_impl = materialized;
        // Keep the ownership token NON-NULL.  The shared clear/replace guard
        // reads it through operator HGDIOBJ — methods_output.cpp:372
        // `if ((HBITMAP) m_BackgroundBitmap) m_BackgroundBitmap.DeleteObject()`
        // — and a null here made world.SetBackgroundImage("") skip the delete,
        // so the old image painted forever (the paint reads m_impl, which had
        // survived).  The value is opaque: nothing feeds it back to
        // ::DeleteObject; m_impl doubles as the token, and DeleteObject()
        // above nulls both.
        m_hObject = materialized;
    } else {
        m_impl = hObject;      // already a wxBitmap* (or null)
    }
    return TRUE;
}

HGDIOBJ CBitmap::Detach() {
    HGDIOBJ h = m_hObject;
    m_hObject = nullptr;
    m_impl = nullptr;
    return h;
}

bool CBitmap::DeleteObject() {
    if (m_impl) { delete static_cast<wxBitmap*>(m_impl); m_impl = nullptr; }
    m_hObject = nullptr;
    return true;
}

// ╔══════════════════════════════════════════════════════════════════════╗
// ║  CDC                                                                 ║
// ╚══════════════════════════════════════════════════════════════════════╝

// Helper to access CDCImpl from a CDC*
static inline CDCImpl* Impl(CDC* dc) {
    return static_cast<CDCImpl*>(dc->m_impl);
}
static inline const CDCImpl* Impl(const CDC* dc) {
    return static_cast<const CDCImpl*>(dc->m_impl);
}

CDC::~CDC() {
    if (m_hDC) { g_hdcToCDC.erase(m_hDC); m_hDC = nullptr; }
    if (m_impl) { delete static_cast<CDCImpl*>(m_impl); m_impl = nullptr; }
}

bool CDC::CreateCompatibleDC(CDC* /*pDC*/) {
    // pDC is ignored on non-Windows: we always create a screenless MemoryDC.
    if (m_hDC) { g_hdcToCDC.erase(m_hDC); m_hDC = nullptr; }
    if (m_impl) { delete static_cast<CDCImpl*>(m_impl); m_impl = nullptr; }
    CDCImpl* p = new CDCImpl();
    // Create an initial 1x1 bitmap so the DC is valid immediately.
    wxBitmap* placeholder = new wxBitmap(1, 1, 32);
    p->dc.SelectObject(*placeholder);
    delete placeholder;   // wxMemoryDC copies the bitmap data; safe to delete
    m_impl = p;
    // Assign a unique HDC so raw SelectObject(m_hDC, hbm) calls can find us.
    m_hDC = reinterpret_cast<HDC>(++s_hdcCounter);
    g_hdcToCDC[m_hDC] = this;
    return true;
}

bool CDC::DeleteDC() {
    if (m_hDC) { g_hdcToCDC.erase(m_hDC); m_hDC = nullptr; }
    if (m_impl) { delete static_cast<CDCImpl*>(m_impl); m_impl = nullptr; }
    return true;
}

CDC* CDC::FromHandle(HDC hDC) {
    // Return the CDC that owns this HDC (registered in CreateCompatibleDC), so
    // code like `CDC::FromHandle(lpDrawItemStruct->hDC)->FillSolidRect(...)`
    // (owner-draw DrawItem) actually paints to the intended DC.  Falls back to a
    // static dummy for unknown handles (keeps old no-op behaviour).
    if (hDC) {
        auto it = g_hdcToCDC.find(hDC);
        if (it != g_hdcToCDC.end() && it->second) return it->second;
    }
    static CDC dc;
    return &dc;
}

// ── SelectObject ─────────────────────────────────────────────────────────

CBitmap* CDC::SelectObject(CBitmap* pBitmap) {
    CDCImpl* p = Impl(this);
    if (!p) return nullptr;
    CBitmap* prev = p->currentBitmap;
    p->currentBitmap = pBitmap;
    if (pBitmap && pBitmap->m_impl) {
        p->dc.SelectObject(*static_cast<wxBitmap*>(pBitmap->m_impl));
    } else {
        p->dc.SelectObject(wxNullBitmap);
    }
    return prev;
}

CPen* CDC::SelectObject(CPen* pPen) {
    CDCImpl* p = Impl(this);
    if (!p) return nullptr;
    CPen* prev = p->currentPen;
    p->currentPen = pPen;
    return prev;
}

CBrush* CDC::SelectObject(CBrush* pBrush) {
    CDCImpl* p = Impl(this);
    if (!p) return nullptr;
    CBrush* prev = p->currentBrush;
    p->currentBrush = pBrush;
    return prev;
}

CFont* CDC::SelectObject(CFont* pFont) {
    CDCImpl* p = Impl(this);
    if (!p) return nullptr;
    CFont* prev = p->currentFont;
    p->currentFont = pFont;
    return prev;
}

CGdiObject* CDC::SelectObject(CGdiObject* pObject) {
    if (!pObject) return nullptr;
    // Try each subtype
    if (CPen* pen = dynamic_cast<CPen*>(pObject))
        return SelectObject(pen);
    if (CBrush* br = dynamic_cast<CBrush*>(pObject))
        return SelectObject(br);
    if (CFont* fnt = dynamic_cast<CFont*>(pObject))
        return SelectObject(fnt);
    if (CBitmap* bmp = dynamic_cast<CBitmap*>(pObject))
        return SelectObject(bmp);
    return nullptr;
}

CGdiObject* CDC::SelectStockObject(int /*nIndex*/) { return nullptr; }

// ── Attribute setters / getters ──────────────────────────────────────────

COLORREF CDC::SetTextColor(COLORREF crColor) {
    CDCImpl* p = Impl(this);
    if (!p) return 0;
    COLORREF prev = p->textColor;
    p->textColor = crColor;
    p->dc.SetTextForeground(WxCol(crColor));
    return prev;
}

COLORREF CDC::GetTextColor() const {
    const CDCImpl* p = Impl(this);
    return p ? p->textColor : 0;
}

COLORREF CDC::SetBkColor(COLORREF crColor) {
    CDCImpl* p = Impl(this);
    if (!p) return 0;
    COLORREF prev = p->bkColor;
    p->bkColor = crColor;
    p->dc.SetTextBackground(WxCol(crColor));
    return prev;
}

COLORREF CDC::GetBkColor() const {
    const CDCImpl* p = Impl(this);
    return p ? p->bkColor : 0;
}

int CDC::SetBkMode(int nBkMode) {
    CDCImpl* p = Impl(this);
    if (!p) return 0;
    int prev = p->bkMode;
    p->bkMode = nBkMode;
    p->ApplyBkMode();
    return prev;
}

int CDC::GetBkMode() const {
    const CDCImpl* p = Impl(this);
    return p ? p->bkMode : TRANSPARENT;
}

int CDC::SetROP2(int nDrawMode) {
    CDCImpl* p = Impl(this);
    if (!p) return 0;
    int prev = p->rop2;
    p->rop2 = nDrawMode;
    return prev;
}

int CDC::GetROP2() const {
    const CDCImpl* p = Impl(this);
    return p ? p->rop2 : R2_COPYPEN;
}

UINT CDC::SetTextAlign(UINT nFlags) {
    CDCImpl* p = Impl(this);
    if (!p) return 0;
    UINT prev = p->textAlign;
    p->textAlign = nFlags;
    return prev;
}

UINT CDC::GetTextAlign() const {
    const CDCImpl* p = Impl(this);
    return p ? p->textAlign : (TA_LEFT | TA_TOP);
}

int CDC::SetStretchBltMode(int nStretchMode) {
    CDCImpl* p = Impl(this);
    if (!p) return 0;
    int prev = p->stretchBltMode;
    p->stretchBltMode = nStretchMode;
    return prev;
}

int CDC::GetStretchBltMode() const {
    const CDCImpl* p = Impl(this);
    return p ? p->stretchBltMode : COLORONCOLOR;
}

int CDC::SetPolyFillMode(int nPolyFillMode) {
    CDCImpl* p = Impl(this);
    if (!p) return 0;
    int prev = p->polyFillMode;
    p->polyFillMode = nPolyFillMode;
    return prev;
}

int CDC::GetPolyFillMode() const {
    const CDCImpl* p = Impl(this);
    return p ? p->polyFillMode : ALTERNATE;
}

int CDC::GetDeviceCaps(int nIndex) {
    switch (nIndex) {
        case BITSPIXEL:  return 32;
        case PLANES:     return 1;
        case LOGPIXELSX:
        case LOGPIXELSY: {
            wxSize dpi = wxGetDisplayPPI();
            return (nIndex == LOGPIXELSX) ? dpi.x : dpi.y;
        }
        case HORZRES: {
            wxSize sz = wxGetDisplaySize();
            return sz.GetWidth();
        }
        case VERTRES: {
            wxSize sz = wxGetDisplaySize();
            return sz.GetHeight();
        }
        default: return 0;
    }
}

// ── Text metrics ──────────────────────────────────────────────────────────

bool CDC::GetTextMetrics(TEXTMETRIC* lpMetrics) {
    CDCImpl* p = Impl(this);
    if (!p || !lpMetrics) return false;
    p->ApplyFont();
    wxFontMetrics fm = p->dc.GetFontMetrics();
    *lpMetrics = {};
    lpMetrics->tmHeight           = fm.height;
    lpMetrics->tmAscent           = fm.ascent;
    lpMetrics->tmDescent          = fm.descent;
    lpMetrics->tmInternalLeading  = fm.internalLeading;
    lpMetrics->tmExternalLeading  = fm.externalLeading;
    lpMetrics->tmAveCharWidth     = fm.averageWidth;

    // tmMaxCharWidth used to be `averageWidth * 2` -- invented, because
    // wxFontMetrics has no max-advance field.  It is script-visible as
    // WindowFontInfo(7), so it is MEASURED below: the widest of the printable
    // ASCII range, which is where the maximum sits for nearly every text font.
    lpMetrics->tmMaxCharWidth     = fm.averageWidth;

    bool fixedPitch = false;
    int  family     = FF_DONTCARE;

    if (p->currentFont && p->currentFont->m_impl) {
        wxFont* f = static_cast<wxFont*>(p->currentFont->m_impl);
        lpMetrics->tmWeight   = f->GetWeight() == wxFONTWEIGHT_BOLD ? FW_BOLD : FW_NORMAL;
        lpMetrics->tmItalic   = (f->GetStyle() != wxFONTSTYLE_NORMAL) ? 1 : 0;
        lpMetrics->tmUnderlined = f->GetUnderlined() ? 1 : 0;
        lpMetrics->tmStruckOut  = f->GetStrikethrough() ? 1 : 0;

        fixedPitch = f->IsFixedWidth();
        switch (f->GetFamily()) {
            case wxFONTFAMILY_ROMAN:      family = FF_ROMAN;      break;
            case wxFONTFAMILY_SWISS:      family = FF_SWISS;      break;
            case wxFONTFAMILY_MODERN:
            case wxFONTFAMILY_TELETYPE:   family = FF_MODERN;     break;
            case wxFONTFAMILY_SCRIPT:     family = FF_SCRIPT;     break;
            case wxFONTFAMILY_DECORATIVE: family = FF_DECORATIVE; break;
            default:                      family = FF_DONTCARE;   break;
        }
    }

    // Measured for EVERY font, fixed-pitch included.  Skipping the fixed-pitch
    // case assumed "every advance is the average", and a Windows run disproves
    // it: real GDI reports Courier New 10pt as tmAveCharWidth 8, tmMaxCharWidth
    // 10.  tmAveCharWidth is roughly the width of 'x', while tmMaxCharWidth is
    // the widest glyph CELL -- side bearings and overhang put the maximum above
    // the advance even when every advance is identical.  So max > average is
    // normal, not a contradiction, and the only way to answer WindowFontInfo(7)
    // honestly is to measure rather than assert.
    {
        int widest = fm.averageWidth;
        for (wxChar ch = wxT(' '); ch <= wxT('~'); ++ch) {
            const int w = p->dc.GetTextExtent(wxString(ch)).GetWidth();
            if (w > widest) widest = w;
        }
        lpMetrics->tmMaxCharWidth = widest;
    }

    // In a TEXTMETRIC (unlike a LOGFONT) bit 0 is TMPF_FIXED_PITCH and its
    // meaning is INVERTED: set means the font is VARIABLE pitch.  The old
    // `= FIXED_PITCH` therefore reported every font as variable, and family
    // FF_DONTCARE for all of them.  Both reach scripts as WindowFontInfo(19).
    lpMetrics->tmPitchAndFamily = (BYTE)((fixedPitch ? 0 : 1) | family);
    return true;
}

// FromUTF8 that never silently drops 8-bit text: if the bytes aren't valid UTF-8
// (wxString::FromUTF8 returns empty for non-empty input) fall back to Latin-1 so
// non-UTF-8 content — e.g. a miniwindow WindowText(..., Unicode=false) with high
// bytes — stays visible instead of vanishing.  Pure UTF-8 / ASCII callers (the
// common case for dialogs, notepad, the bridge) are unaffected.
static wxString CdcDecodeText(LPCTSTR lpszString, int nCount)
{
    if (!lpszString) return wxString();
    wxString s = (nCount >= 0) ? wxString::FromUTF8(lpszString, nCount)
                               : wxString::FromUTF8(lpszString);
    const bool nonEmpty = (nCount >= 0) ? (nCount > 0) : (lpszString[0] != '\0');
    if (s.empty() && nonEmpty)
        s = (nCount >= 0) ? wxString::From8BitData(lpszString, nCount)
                          : wxString::From8BitData(lpszString);
    return s;
}

CSize CDC::GetTextExtent(LPCTSTR lpszString, int nCount) {
    CDCImpl* p = Impl(this);
    if (!p) return {};
    p->ApplyFont();
    wxString s = CdcDecodeText(lpszString, nCount);
    wxSize sz = p->dc.GetTextExtent(s);
    return CSize(sz.GetWidth(), sz.GetHeight());
}

CSize CDC::GetTextExtent(const CString& str) {
    return GetTextExtent((LPCTSTR)str, str.GetLength());
}

CSize CDC::GetOutputTextExtent(LPCTSTR s, int n) { return GetTextExtent(s, n); }
CSize CDC::GetOutputTextExtent(const CString& s) { return GetTextExtent(s); }

int CDC::GetTextFace(CString& rString) const {
    const CDCImpl* p = Impl(this);
    rString = "";
    if (!p || !p->currentFont || !p->currentFont->m_impl) return 0;
    wxFont* f = static_cast<wxFont*>(p->currentFont->m_impl);
    wxString face = f->GetFaceName();
    rString = face.ToUTF8().data();
    return rString.GetLength();
}

// ── Viewport/window origin ────────────────────────────────────────────────

CPoint CDC::SetViewportOrg(int x, int y) {
    CDCImpl* p = Impl(this);
    if (!p) return {};
    // Store but don't apply — miniwindow uses SetWindowOrg(0,0) which is identity
    (void)x; (void)y;
    return {};
}

CPoint CDC::SetWindowOrg(int x, int y) {
    (void)x; (void)y;
    return {};
}

// ── Save / Restore DC ────────────────────────────────────────────────────

int CDC::SaveDC() {
    CDCImpl* p = Impl(this);
    if (!p) return 0;
    CDCImpl::Snapshot snap;
    snap.textColor    = p->textColor;
    snap.bkColor      = p->bkColor;
    snap.bkMode       = p->bkMode;
    snap.rop2         = p->rop2;
    snap.stretchBltMode = p->stretchBltMode;
    snap.polyFillMode = p->polyFillMode;
    snap.textAlign    = p->textAlign;
    snap.curX = p->curX; snap.curY = p->curY;
    snap.currentBitmap = p->currentBitmap;
    snap.currentPen    = p->currentPen;
    snap.currentBrush  = p->currentBrush;
    snap.currentFont   = p->currentFont;
    p->snapshots.push_back(snap);
    return (int)p->snapshots.size();   // positive save ID
}

bool CDC::RestoreDC(int nSavedDC) {
    CDCImpl* p = Impl(this);
    if (!p || p->snapshots.empty()) return false;
    int idx = (nSavedDC < 0)
        ? (int)p->snapshots.size() - 1
        : (nSavedDC - 1);
    if (idx < 0 || idx >= (int)p->snapshots.size()) return false;
    const CDCImpl::Snapshot& snap = p->snapshots[idx];
    p->textColor    = snap.textColor;
    p->bkColor      = snap.bkColor;
    p->bkMode       = snap.bkMode;
    p->rop2         = snap.rop2;
    p->stretchBltMode = snap.stretchBltMode;
    p->polyFillMode = snap.polyFillMode;
    p->textAlign    = snap.textAlign;
    p->curX = snap.curX; p->curY = snap.curY;
    p->currentBitmap = snap.currentBitmap;
    p->currentPen    = snap.currentPen;
    p->currentBrush  = snap.currentBrush;
    p->currentFont   = snap.currentFont;
    p->snapshots.resize(idx);
    return true;
}

// ── Accessors for current GDI objects ─────────────────────────────────────

CFont*   CDC::GetCurrentFont()   const { const CDCImpl* p = Impl(this); return p ? p->currentFont   : nullptr; }
CBrush*  CDC::GetCurrentBrush()  const { const CDCImpl* p = Impl(this); return p ? p->currentBrush  : nullptr; }
CPen*    CDC::GetCurrentPen()    const { const CDCImpl* p = Impl(this); return p ? p->currentPen    : nullptr; }
CBitmap* CDC::GetCurrentBitmap() const { const CDCImpl* p = Impl(this); return p ? p->currentBitmap : nullptr; }

// ── Fill / frame ──────────────────────────────────────────────────────────

void CDC::FillSolidRect(int x, int y, int cx, int cy, COLORREF clr) {
    CDCImpl* p = Impl(this);
    if (!p) return;
    p->dc.SetPen(*wxTRANSPARENT_PEN);
    p->dc.SetBrush(wxBrush(WxCol(clr)));
    p->dc.DrawRectangle(x, y, cx, cy);
}

void CDC::FillSolidRect(LPCRECT lpRect, COLORREF clr) {
    if (lpRect)
        FillSolidRect(lpRect->left, lpRect->top,
                      lpRect->right - lpRect->left,
                      lpRect->bottom - lpRect->top, clr);
}

void CDC::FillSolidRect(const CRect* lpRect, COLORREF clr) {
    if (lpRect) FillSolidRect((LPCRECT)(const RECT*)lpRect, clr);
}

void CDC::FillRect(LPCRECT lpRect, CBrush* pBrush) {
    CDCImpl* p = Impl(this);
    if (!p || !lpRect) return;
    p->dc.SetPen(*wxTRANSPARENT_PEN);
    if (pBrush && pBrush->m_impl)
        p->dc.SetBrush(*static_cast<wxBrush*>(pBrush->m_impl));
    else
        p->dc.SetBrush(*wxTRANSPARENT_BRUSH);
    p->dc.DrawRectangle(lpRect->left, lpRect->top,
                        lpRect->right - lpRect->left,
                        lpRect->bottom - lpRect->top);
}

void CDC::FillRect(const CRect* r, CBrush* br) {
    if (r) FillRect((LPCRECT)(const RECT*)r, br);
}

void CDC::FrameRect(LPCRECT lpRect, CBrush* pBrush) {
    CDCImpl* p = Impl(this);
    if (!p || !lpRect || !pBrush || !pBrush->m_impl) return;
    wxBrush* wb = static_cast<wxBrush*>(pBrush->m_impl);
    p->dc.SetPen(wxPen(wb->GetColour(), 1, wxPENSTYLE_SOLID));
    p->dc.SetBrush(*wxTRANSPARENT_BRUSH);
    p->dc.DrawRectangle(lpRect->left, lpRect->top,
                        lpRect->right - lpRect->left,
                        lpRect->bottom - lpRect->top);
}

void CDC::Draw3dRect(LPCRECT lpRect, COLORREF clrTopLeft, COLORREF clrBottomRight) {
    if (!lpRect) return;
    int l = lpRect->left, t = lpRect->top,
        r = lpRect->right - 1, b = lpRect->bottom - 1;
    CDCImpl* p = Impl(this);
    if (!p) return;
    wxPen tlPen(WxCol(clrTopLeft));
    wxPen brPen(WxCol(clrBottomRight));
    p->dc.SetPen(tlPen);
    p->dc.DrawLine(l, t, r, t);   // top
    p->dc.DrawLine(l, t, l, b);   // left
    p->dc.SetPen(brPen);
    p->dc.DrawLine(r, t, r, b);   // right
    p->dc.DrawLine(l, b, r, b);   // bottom
}

void CDC::Draw3dRect(int x,int y,int cx,int cy,COLORREF tl,COLORREF br) {
    RECT r = { x, y, x+cx, y+cy };
    Draw3dRect(&r, tl, br);
}

void CDC::DrawEdge(LPRECT lpRect, UINT nEdge, UINT nFlags) {
    // Win32 DrawEdge draws TWO one-pixel rings.  nEdge is a pair of BDR_ bits:
    // the outer ring (BDR_RAISEDOUTER 0x01 / BDR_SUNKENOUTER 0x02) and the inner
    // (BDR_RAISEDINNER 0x04 / BDR_SUNKENINNER 0x08).  The named EDGE_ values are
    // just combinations -- EDGE_RAISED = 0x05, EDGE_SUNKEN = 0x0A,
    // EDGE_ETCHED = 0x06 (sunken outer + raised inner) and EDGE_BUMP = 0x09.
    //
    // The old body switched on nEdge as if it were an enum, so ETCHED and BUMP
    // fell into `default:` and came out as a flat mid-grey box; it then ignored
    // nFlags entirely and called Draw3dRect, which always strokes all four
    // sides -- directly contradicting its own "Only draw the requested sides"
    // comment.  Both are script-reachable through WindowRectOp action 5.
    if (!lpRect) return;
    CDCImpl* p = Impl(this);
    if (!p) return;

    // The four Win32 3D system colours, in the shim's BGR COLORREF order (these
    // are the standard greys; the port has no system-colour theme to ask).
    const COLORREF kHilight   = 0xFFFFFF;   // COLOR_3DHILIGHT
    const COLORREF kLight     = 0xE0E0E0;   // COLOR_3DLIGHT
    const COLORREF kShadow    = 0x808080;   // COLOR_3DSHADOW
    const COLORREF kDkShadow  = 0x404040;   // COLOR_3DDKSHADOW

    if ((nFlags & BF_RECT) == 0) return;   // no sides named = Win32 draws nothing

    RECT r = *lpRect;

    // Stroke one ring, honouring the BF_ side bits.  tl paints top+left,
    // br paints right+bottom -- the same split Draw3dRect uses.
    auto ring = [&](const RECT& q, COLORREF tl, COLORREF br) {
        const int l = q.left, t = q.top, rr = q.right - 1, b = q.bottom - 1;
        if (rr < l || b < t) return;
        p->dc.SetPen(wxPen(WxCol(tl)));
        if (nFlags & BF_TOP)    p->dc.DrawLine(l, t, rr + 1, t);
        if (nFlags & BF_LEFT)   p->dc.DrawLine(l, t, l, b + 1);
        p->dc.SetPen(wxPen(WxCol(br)));
        if (nFlags & BF_RIGHT)  p->dc.DrawLine(rr, t, rr, b + 1);
        if (nFlags & BF_BOTTOM) p->dc.DrawLine(l, b, rr + 1, b);
    };

    // Outer ring.
    if (nEdge & BDR_RAISEDOUTER)      ring(r, kLight,    kDkShadow);
    else if (nEdge & BDR_SUNKENOUTER) ring(r, kDkShadow, kLight);

    // Inner ring, inset by one pixel on whichever sides the outer ring used.
    RECT ri = r;
    if (nFlags & BF_LEFT)   ri.left++;
    if (nFlags & BF_TOP)    ri.top++;
    if (nFlags & BF_RIGHT)  ri.right--;
    if (nFlags & BF_BOTTOM) ri.bottom--;

    if (nEdge & BDR_RAISEDINNER)      ring(ri, kHilight, kShadow);
    else if (nEdge & BDR_SUNKENINNER) ring(ri, kShadow,  kHilight);

    // BF_ADJUST: report the client rect left inside the border.
    if (nFlags & BF_ADJUST) {
        if (nFlags & BF_LEFT)   lpRect->left   += 2;
        if (nFlags & BF_TOP)    lpRect->top    += 2;
        if (nFlags & BF_RIGHT)  lpRect->right  -= 2;
        if (nFlags & BF_BOTTOM) lpRect->bottom -= 2;
    }
}

// ── Geometry drawing ──────────────────────────────────────────────────────

bool CDC::Rectangle(int x1, int y1, int x2, int y2) {
    CDCImpl* p = Impl(this);
    if (!p) return false;
    p->ApplyPen(); p->ApplyBrush();
    p->dc.DrawRectangle(x1, y1, x2 - x1, y2 - y1);
    return true;
}

bool CDC::Rectangle(LPCRECT r) {
    return r ? Rectangle(r->left, r->top, r->right, r->bottom) : false;
}

bool CDC::Ellipse(int x1, int y1, int x2, int y2) {
    CDCImpl* p = Impl(this);
    if (!p) return false;
    p->ApplyPen(); p->ApplyBrush();
    p->dc.DrawEllipse(x1, y1, x2 - x1, y2 - y1);
    return true;
}

bool CDC::Ellipse(const CRect& r) {
    return Ellipse(r.left, r.top, r.right, r.bottom);
}

bool CDC::RoundRect(int x1, int y1, int x2, int y2, int x3, int y3) {
    CDCImpl* p = Impl(this);
    if (!p) return false;
    // Win32 passes the corner *ellipse* width/height (x3,y3) — radii x3/2, y3/2,
    // which may differ.  wxDC::DrawRoundedRectangle only takes one radius, so
    // trace the outline as a polygon with four quarter-elliptical corners to
    // honour non-square corners.
    if (x2 < x1) std::swap(x1, x2);
    if (y2 < y1) std::swap(y1, y2);
    // Win32 RoundRect is right/bottom-EXCLUSIVE: the figure occupies x1..x2-1.
    // Rectangle and Ellipse above honour that (DrawRectangle(x1, y1, w, h)), and
    // so does the degenerate branch below -- but the polygon branch used to put
    // its extreme points at x2 and y2, one pixel past GDI's last pixel on the
    // right and bottom edges.  Trace over the exclusive extent instead.
    const double xr = x2 - 1.0, yb = y2 - 1.0;
    double rx = std::min(x3 / 2.0, (xr - x1) / 2.0);
    double ry = std::min(y3 / 2.0, (yb - y1) / 2.0);
    p->ApplyPen(); p->ApplyBrush();
    if (rx <= 0.5 || ry <= 0.5) {            // degenerate corners → plain rectangle
        p->dc.DrawRectangle(x1, y1, x2 - x1, y2 - y1);
        return true;
    }
    const int SEG = 8;                       // segments per corner
    struct Corner { double cx, cy, a0; } corners[4] = {
        { xr - rx, y1 + ry, -90.0 },         // top-right    (-90°→  0°)
        { xr - rx, yb - ry,   0.0 },         // bottom-right (  0°→ 90°)
        { x1 + rx, yb - ry,  90.0 },         // bottom-left  ( 90°→180°)
        { x1 + rx, y1 + ry, 180.0 },         // top-left     (180°→270°)
    };
    std::vector<wxPoint> pts;
    for (const Corner& c : corners)
        for (int s = 0; s <= SEG; ++s) {
            double a = wxDegToRad(c.a0 + 90.0 * s / SEG);
            pts.emplace_back(wxRound(c.cx + rx * cos(a)),
                             wxRound(c.cy + ry * sin(a)));
        }
    p->dc.DrawPolygon((int) pts.size(), pts.data());
    return true;
}

bool CDC::RoundRect(const CRect& r, CPoint pt) {
    return RoundRect(r.left, r.top, r.right, r.bottom, pt.x, pt.y);
}

bool CDC::Chord(int x1, int y1, int x2, int y2,
                int x3, int y3, int x4, int y4)
{
    CDCImpl* p = Impl(this);
    if (!p) return false;
    p->ApplyPen(); p->ApplyBrush();
    // wx DrawEllipticArc: start/end in degrees (anticlockwise from 3 o'clock)
    int cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
    int rx = (x2 - x1) / 2, ry = (y2 - y1) / 2;
    if (rx <= 0 || ry <= 0) return false;
    double startDeg = atan2(-(double)(y3 - cy) / ry, (double)(x3 - cx) / rx) * 180.0 / M_PI;
    double endDeg   = atan2(-(double)(y4 - cy) / ry, (double)(x4 - cx) / rx) * 180.0 / M_PI;

    // A CHORD, not a pie.  Win32 bounds the region with the arc plus the
    // straight line joining its two endpoints; wxDC::DrawEllipticArc draws the
    // PIE when the brush is not transparent (wxGCDCImpl moves to the centre and
    // closes back to it), so this used to produce a wedge with two radii and no
    // chord line.  The three-point array beside it was computed and discarded --
    // and went through the centre, so it was not a chord either.
    //
    // Sample the arc and let DrawPolygon close the figure: the closing edge IS
    // the chord.  Same technique as RoundRect's corners above.
    if (endDeg <= startDeg) endDeg += 360.0;      // wx sweeps anticlockwise
    const int SEG = 32;
    std::vector<wxPoint> pts;
    pts.reserve(SEG + 1);
    for (int i = 0; i <= SEG; ++i) {
        const double a = wxDegToRad(startDeg + (endDeg - startDeg) * i / SEG);
        pts.emplace_back(wxRound(cx + rx * cos(a)), wxRound(cy - ry * sin(a)));
    }
    p->dc.DrawPolygon((int) pts.size(), pts.data());
    return true;
}

bool CDC::Chord(const CRect& r, CPoint p1, CPoint p2) {
    return Chord(r.left, r.top, r.right, r.bottom, p1.x, p1.y, p2.x, p2.y);
}

bool CDC::Pie(int x1, int y1, int x2, int y2,
              int xS, int yS, int xE, int yE)
{
    CDCImpl* p = Impl(this);
    if (!p) return false;
    p->ApplyPen(); p->ApplyBrush();
    int cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
    int rx = (x2 - x1) / 2, ry = (y2 - y1) / 2;
    if (rx <= 0 || ry <= 0) return false;
    double startDeg = atan2(-(double)(yS - cy) / ry, (double)(xS - cx) / rx) * 180.0 / M_PI;
    double endDeg   = atan2(-(double)(yE - cy) / ry, (double)(xE - cx) / rx) * 180.0 / M_PI;
    p->dc.DrawEllipticArc(x1, y1, x2 - x1, y2 - y1, startDeg, endDeg);
    // connect arcs to center (pie wedge lines)
    p->dc.SetBrush(*wxTRANSPARENT_BRUSH);
    p->dc.DrawLine(cx, cy, xS, yS);
    p->dc.DrawLine(cx, cy, xE, yE);
    return true;
}

bool CDC::Pie(const CRect& r, CPoint p1, CPoint p2) {
    return Pie(r.left, r.top, r.right, r.bottom, p1.x, p1.y, p2.x, p2.y);
}

bool CDC::Arc(int x1, int y1, int x2, int y2,
              int x3, int y3, int x4, int y4)
{
    CDCImpl* p = Impl(this);
    if (!p) return false;
    p->ApplyPen();
    int cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
    int rx = (x2 - x1) / 2, ry = (y2 - y1) / 2;
    if (rx <= 0 || ry <= 0) return false;
    double startDeg = atan2(-(double)(y3 - cy) / ry, (double)(x3 - cx) / rx) * 180.0 / M_PI;
    double endDeg   = atan2(-(double)(y4 - cy) / ry, (double)(x4 - cx) / rx) * 180.0 / M_PI;
    p->dc.SetBrush(*wxTRANSPARENT_BRUSH);
    p->dc.DrawEllipticArc(x1, y1, x2 - x1, y2 - y1, startDeg, endDeg);
    return true;
}

bool CDC::LineTo(int x, int y) {
    CDCImpl* p = Impl(this);
    if (!p) return false;
    p->ApplyPen();
    p->dc.DrawLine(p->curX, p->curY, x, y);
    p->curX = x; p->curY = y;
    return true;
}

bool CDC::MoveTo(int x, int y, POINT* lpPoint) {
    CDCImpl* p = Impl(this);
    if (!p) return false;
    if (lpPoint) { lpPoint->x = p->curX; lpPoint->y = p->curY; }
    p->curX = x; p->curY = y;
    return true;
}

bool CDC::Polyline(const POINT* lpPoints, int nCount) {
    CDCImpl* p = Impl(this);
    if (!p || nCount < 2) return false;
    p->ApplyPen();
    std::vector<wxPoint> pts(nCount);
    for (int i = 0; i < nCount; ++i) pts[i] = wxPoint(lpPoints[i].x, lpPoints[i].y);
    p->dc.DrawLines(nCount, pts.data());
    return true;
}

bool CDC::Polygon(const POINT* lpPoints, int nCount) {
    CDCImpl* p = Impl(this);
    if (!p || nCount < 2) return false;
    p->ApplyPen(); p->ApplyBrush();
    std::vector<wxPoint> pts(nCount);
    for (int i = 0; i < nCount; ++i) pts[i] = wxPoint(lpPoints[i].x, lpPoints[i].y);
    p->dc.SetLogicalFunction(wxCOPY);
    p->dc.DrawPolygon(nCount, pts.data(), 0, 0,
                      (p->polyFillMode == WINDING) ? wxWINDING_RULE : wxODDEVEN_RULE);
    return true;
}

bool CDC::PolyBezier(const POINT* lpPoints, int nCount) {
    CDCImpl* p = Impl(this);
    if (!p) return false;
    // Win32 PolyBezier: points are [start, c1, c2, end, c1, c2, end, …] — a start
    // point followed by cubic control/control/end triplets.  wxDC has no native
    // Bézier, so flatten each cubic into short segments — a smooth curve rather
    // than a zig-zag through the control points (the old placeholder).
    if (nCount < 4) return Polyline(lpPoints, nCount);
    p->ApplyPen();
    std::vector<wxPoint> pts;
    pts.emplace_back(lpPoints[0].x, lpPoints[0].y);
    const int STEPS = 24;                    // segments per cubic
    for (int i = 1; i + 2 < nCount; i += 3) {
        const double x0 = lpPoints[i-1].x, y0 = lpPoints[i-1].y;
        const double x1 = lpPoints[i  ].x, y1 = lpPoints[i  ].y;
        const double x2 = lpPoints[i+1].x, y2 = lpPoints[i+1].y;
        const double x3 = lpPoints[i+2].x, y3 = lpPoints[i+2].y;
        for (int s = 1; s <= STEPS; ++s) {
            const double t = (double) s / STEPS, u = 1.0 - t;
            const double b0 = u*u*u, b1 = 3*u*u*t, b2 = 3*u*t*t, b3 = t*t*t;
            pts.emplace_back(wxRound(b0*x0 + b1*x1 + b2*x2 + b3*x3),
                             wxRound(b0*y0 + b1*y1 + b2*y2 + b3*y3));
        }
    }
    if (pts.size() < 2) return false;
    p->dc.DrawLines((int) pts.size(), pts.data());
    return true;
}

bool CDC::FloodFill(int x, int y, COLORREF crColor) {
    CDCImpl* p = Impl(this);
    if (!p) return false;
    p->ApplyBrush();
    p->dc.FloodFill(x, y, WxCol(crColor), wxFLOOD_BORDER);
    return true;
}

bool CDC::ExtFloodFill(int x, int y, COLORREF crColor, UINT nFillType) {
    CDCImpl* p = Impl(this);
    if (!p) return false;
    p->ApplyBrush();
    wxFloodFillStyle wxType = (nFillType == FLOODFILLSURFACE) ? wxFLOOD_SURFACE : wxFLOOD_BORDER;
    p->dc.FloodFill(x, y, WxCol(crColor), wxType);
    return true;
}

bool CDC::InvertRect(const RECT* lpRect) {
    CDCImpl* p = Impl(this);
    if (!p || !lpRect) return false;
    wxRect r(lpRect->left, lpRect->top,
             lpRect->right - lpRect->left,
             lpRect->bottom - lpRect->top);
    p->dc.SetLogicalFunction(wxINVERT);
    p->dc.SetPen(*wxTRANSPARENT_PEN);
    p->dc.SetBrush(*wxWHITE_BRUSH);
    p->dc.DrawRectangle(r);
    p->dc.SetLogicalFunction(wxCOPY);
    return true;
}

bool CDC::InvertRect(const CRect* r) {
    return r ? InvertRect((const RECT*)r) : false;
}

BOOL CDC::SetPixelV(int x, int y, COLORREF crColor) {
    CDCImpl* p = Impl(this);
    if (!p) return FALSE;
    p->dc.SetPen(wxPen(WxCol(crColor)));
    p->dc.DrawPoint(x, y);
    return TRUE;
}

COLORREF CDC::GetPixel(int x, int y) const {
    const CDCImpl* p = Impl(this);
    if (!p) return 0;
    wxColour col;
    // wx 3.3 changed GetPixel to take (x, y, &col) and return bool.
    if (!const_cast<wxMemoryDC&>(p->dc).GetPixel(x, y, &col))
        return 0;
    return CRef(col);
}

// Global ::GetPixel(HDC,x,y) bridge — assigned to g_wxhook_get_pixel in
// wx_mfc_impl.cpp's install block.  When a DIBSection is bound to hdc (loaded
// PNG/BMP images), read straight from its raw BGR(A) buffer — that is the
// authoritative source for colour-key transparency (miniwindow.cpp reads pixel
// 0,0 as the transparent colour).  Otherwise fall back to the wxBitmap that the
// owning CDC has selected.
unsigned long wxgdi_get_pixel(void* hdcv, int x, int y) {
    HDC hdc = reinterpret_cast<HDC>(hdcv);
    if (!hdc) return 0;
    if (DIBSectionData* dib = GetDIBForHDC(hdc)) {
        if (x < 0 || y < 0 || x >= dib->width || y >= dib->height)
            return 0;
        const int row = dib->bottomUp ? (dib->height - 1 - y) : y;
        const size_t off = (size_t) row * (size_t) dib->stride
                         + (size_t) x * (size_t) dib->bytesPerPixel;
        if (off + (size_t) dib->bytesPerPixel > dib->pixels.size())
            return 0;
        const unsigned char* px = &dib->pixels[off];
        // buffer is BGR(A); COLORREF is 0x00BBGGRR.
        return (unsigned long) px[2]
             | ((unsigned long) px[1] << 8)
             | ((unsigned long) px[0] << 16);
    }
    CDC* dc = CDC::FromHandle(hdc);
    return dc ? (unsigned long) dc->GetPixel(x, y) : 0;
}

// Raw BGRA read of the bitmap selected in a shim CDC — used by WxSurface's
// alpha-preserving readPixels (pal::PixelFormat::BGRA32_Alpha, i.e. the
// WindowDrawImageAlpha / WindowGetImageAlpha image read). Deliberately does
// NOT go through wxDC::Blit: on wxOSX Blit composites (premultiplies against
// the destination) instead of copying bytes, which corrupts semi-transparent
// pixels — Windows BitBlt into a 32bpp DIB copies the source's BGRA verbatim.
// Reads the DC's DIBSection buffer when one is selected (authoritative bytes),
// else the selected wxBitmap via wxImage (alpha 255 when it has no alpha
// channel; note wxOSX stores premultiplied, so RGB under partial alpha can be
// off by quantization — alpha itself is exact).
// dst is the base of an h-row BOTTOM-UP buffer (DIB order): image row j lives
// at dst + (h-1-j)*strideBytes, 4 bytes/pixel. Pixels outside the bitmap are
// left untouched (callers pre-zero the buffer — BitBlt's leave-as-zero OOB
// behaviour happens engine-side).
bool wxgdi_read_bgra(CDC* dc, int x, int y, int w, int h,
                     unsigned char* dst, int strideBytes)
{
    if (!dc || !dst || w <= 0 || h <= 0) return false;

    if (dc->m_hDC) {
        if (DIBSectionData* dib = GetDIBForHDC(dc->m_hDC)) {
            const int bpp = dib->bytesPerPixel;
            for (int j = 0; j < h; ++j) {
                const int sy = y + j;
                if (sy < 0 || sy >= dib->height) continue;
                const int srow = dib->bottomUp ? (dib->height - 1 - sy) : sy;
                const unsigned char* src = dib->pixels.data()
                                         + (size_t)srow * dib->stride;
                unsigned char* out = dst + (size_t)(h - 1 - j) * strideBytes;
                for (int i = 0; i < w; ++i) {
                    const int sx = x + i;
                    if (sx < 0 || sx >= dib->width) continue;
                    const unsigned char* px = src + (size_t)sx * bpp;
                    unsigned char* o = out + (size_t)i * 4;
                    o[0] = px[0]; o[1] = px[1]; o[2] = px[2];
                    o[3] = (bpp == 4) ? px[3] : 255;
                }
            }
            return true;
        }
    }

    CDCImpl* p = Impl(dc);
    if (!p) return false;
    const wxBitmap& bmp = p->dc.GetSelectedBitmap();
    if (!bmp.IsOk()) return false;

    // The caller addresses the surface in LOGICAL units, but a HiDPI canvas
    // (CBitmap::CreateCanvasBitmap) holds scale x that many physical pixels, and
    // ConvertToImage hands back the physical grid.  Sample the top-left pixel of
    // each logical cell — nearest-neighbour, NOT an average — so a solid fill
    // still reads back its exact colour and WindowGetPixel keeps the exactness
    // plugins rely on.  Scale is 1 on a non-Retina display, where this reduces
    // to the original indexing.
    const double sc  = bmp.GetScaleFactor();
    const int    isc = (sc > 0) ? (int)(sc + 0.5) : 1;
    const int    bw  = bmp.GetWidth(), bh = bmp.GetHeight();   // PHYSICAL pixels
    if (bw <= 0 || bh <= 0) return false;
    // ...and the LOGICAL extent, which is what the caller's coordinates are in
    // and what wxBitmap::GetSubBitmap's rectangle is in (verified: on wxOSX a
    // 1x1 logical sub-rect of a 2x-scaled bitmap yields a 2x2 physical bitmap
    // still tagged scale 2).  Take the smaller of the two derivations so a
    // fractional scale factor can never hand GetSubBitmap an out-of-range rect,
    // which would fail its wxCHECK and return an invalid bitmap.
    const int lwFromScale = bw / isc, lhFromScale = bh / isc;
    const int lw = std::min(lwFromScale, (int) bmp.GetLogicalWidth());
    const int lh = std::min(lhFromScale, (int) bmp.GetLogicalHeight());
    if (lw <= 0 || lh <= 0) return false;

    // Logical bounding box of the samples we are actually going to take, clipped
    // to the bitmap.  Everything outside is left as the caller zeroed it
    // (BitBlt's out-of-bounds behaviour, handled engine-side).
    const int lx0 = std::max(0, x),           ly0 = std::max(0, y);
    const int lx1 = std::min(x + w - 1, lw - 1),
              ly1 = std::min(y + h - 1, lh - 1);              // inclusive
    if (lx1 < lx0 || ly1 < ly0) return true;                  // nothing in range
    const int lwid = lx1 - lx0 + 1, lhgt = ly1 - ly0 + 1;

    // Convert ONLY that box.  ConvertToImage on the whole bitmap is O(surface)
    // per call, and this is the single-pixel path: WindowGetPixel →
    // ISurface::getPixel → readPixels(…, 1, 1) lands here, so a plugin's
    // `for x,y do WindowGetPixel(...)` sweep of a 400x300 Retina miniwindow was
    // 120,000 conversions of an 800x600x4 buffer — minutes, for an operation the
    // user expects to be instant.  The hot BLIT path has the same shape:
    // CMiniWindow's transparent DrawImage reads pixel (0,0) of the SOURCE image
    // for its colour key (miniwindow.cpp:1483 and :4148), i.e. one full-image
    // conversion per icon per redraw.  A 1x1 read now converts 1x1.
    // (Windows never had the problem: GetPixel on a DIB is a pointer add, which
    // is also what the DIBSection fast path above does.)
    wxImage img = (lx0 == 0 && ly0 == 0 && lwid == lw && lhgt == lh)
                  ? bmp.ConvertToImage()          // whole thing: skip the copy
                  : bmp.GetSubBitmap(wxRect(lx0, ly0, lwid, lhgt)).ConvertToImage();
    if (!img.IsOk()) return false;
    const bool hasA = img.HasAlpha();
    const int  iw   = img.GetWidth(), ih = img.GetHeight();

    for (int j = 0; j < h; ++j) {
        const int sy = (y + j - ly0) * isc;
        if (sy < 0 || sy >= ih) continue;
        unsigned char* out = dst + (size_t)(h - 1 - j) * strideBytes;
        for (int i = 0; i < w; ++i) {
            const int sx = (x + i - lx0) * isc;
            if (sx < 0 || sx >= iw) continue;
            unsigned char* o = out + (size_t)i * 4;
            o[0] = img.GetBlue(sx, sy);
            o[1] = img.GetGreen(sx, sy);
            o[2] = img.GetRed(sx, sy);
            o[3] = hasA ? img.GetAlpha(sx, sy) : 255;
        }
    }
    return true;
}

// The write counterpart of wxgdi_read_bgra, and needed for the same reason: the
// BitBlt round-trip in WxSurface::writePixels goes through an UNSCALED 1x DIB,
// so on a HiDPI canvas blitting back SCALES the buffer up and interpolates it --
// a read-modify-write that read exactly still wrote a softened result.  Here
// each LOGICAL pixel is replicated across the scale x scale physical block it
// owns, the exact inverse of the read's top-left sampling, so the round-trip is
// lossless.  Scale is 1 on a non-Retina display, where this reduces to a plain
// per-pixel copy.
bool wxgdi_write_bgra(CDC* dc, int x, int y, int w, int h,
                      const unsigned char* src, int strideBytes, bool hasAlpha)
{
    if (!dc || !src || w <= 0 || h <= 0) return false;

    if (dc->m_hDC) {
        if (DIBSectionData* dib = GetDIBForHDC(dc->m_hDC)) {
            const int bpp = dib->bytesPerPixel;
            for (int j = 0; j < h; ++j) {
                const int dy = y + j;
                if (dy < 0 || dy >= dib->height) continue;
                const int drow = dib->bottomUp ? (dib->height - 1 - dy) : dy;
                unsigned char* out = dib->pixels.data() + (size_t)drow * dib->stride;
                const unsigned char* in = src + (size_t)(h - 1 - j) * strideBytes;
                for (int i = 0; i < w; ++i) {
                    const int dx = x + i;
                    if (dx < 0 || dx >= dib->width) continue;
                    unsigned char* px = out + (size_t)dx * bpp;
                    const unsigned char* s4 = in + (size_t)i * 4;
                    px[0] = s4[0]; px[1] = s4[1]; px[2] = s4[2];
                    if (bpp == 4) px[3] = hasAlpha ? s4[3] : 255;
                }
            }
            return true;
        }
    }

    CDCImpl* p = Impl(dc);
    if (!p) return false;
    wxBitmap bmp = p->dc.GetSelectedBitmap();
    if (!bmp.IsOk()) return false;

    const double sc  = bmp.GetScaleFactor();
    const int    isc = (sc > 0) ? (int)(sc + 0.5) : 1;

    wxImage img = bmp.ConvertToImage();
    if (!img.IsOk()) return false;
    if (hasAlpha && !img.HasAlpha()) img.InitAlpha();
    const int bw = img.GetWidth(), bh = img.GetHeight();

    for (int j = 0; j < h; ++j) {
        const unsigned char* in = src + (size_t)(h - 1 - j) * strideBytes;
        for (int i = 0; i < w; ++i) {
            const unsigned char* s4 = in + (size_t)i * 4;
            for (int by = 0; by < isc; ++by) {
                const int py = (y + j) * isc + by;
                if (py < 0 || py >= bh) continue;
                for (int bx = 0; bx < isc; ++bx) {
                    const int px = (x + i) * isc + bx;
                    if (px < 0 || px >= bw) continue;
                    img.SetRGB(px, py, s4[2], s4[1], s4[0]);
                    if (hasAlpha && img.HasAlpha()) img.SetAlpha(px, py, s4[3]);
                }
            }
        }
    }

    wxBitmap updated(img, 32);
    updated.SetScaleFactor(sc);
    p->dc.SelectObject(wxNullBitmap);
    if (p->currentBitmap && p->currentBitmap->m_impl) {
        // Write the new pixels back through the SELECTED CBitmap, keeping it
        // the single owner.  Selecting a detached `updated` here froze the
        // owner's m_impl at pre-write content, and every consumer that reads
        // the bitmap OBJECT rather than the DC — CMiniWindow::Resize's
        // copy-old-contents blit above all — saw stale pixels: after any
        // pixel-write op, a WindowResize reverted the canvas to what it held
        // before the first such op.
        *static_cast<wxBitmap*>(p->currentBitmap->m_impl) = updated;
        p->dc.SelectObject(*static_cast<wxBitmap*>(p->currentBitmap->m_impl));
    } else {
        // No CBitmap selected (DIB-backed DCs returned earlier): old behaviour.
        p->dc.SelectObject(updated);
    }
    return true;
}


bool CDC::PatBlt(int x, int y, int nWidth, int nHeight, DWORD dwRop) {
    CDCImpl* p = Impl(this);
    if (!p) return false;
    wxRasterOperationMode fn = MapRop(dwRop);
    p->dc.SetLogicalFunction(fn);
    p->ApplyBrush();
    p->dc.SetPen(*wxTRANSPARENT_PEN);
    p->dc.DrawRectangle(x, y, nWidth, nHeight);
    p->dc.SetLogicalFunction(wxCOPY);
    return true;
}

// ── BitBlt / StretchBlt ───────────────────────────────────────────────────

bool CDC::BitBlt(int x, int y, int nWidth, int nHeight,
                 CDC* pSrcDC, int xSrc, int ySrc, DWORD dwRop)
{
    CDCImpl* p = Impl(this);
    CDCImpl* src = pSrcDC ? Impl(pSrcDC) : nullptr;
    if (!p || !src) return false;

    // If the SOURCE has a DIBSection selected, sync raw buffer→wxBitmap first
    // so the caller's pixel manipulations (written to pBits) are visible in the DC.
    if (pSrcDC && pSrcDC->m_hDC) {
        if (DIBSectionData* dib = GetDIBForHDC(pSrcDC->m_hDC)) {
            dib->SyncPixelsToBitmap();
            // SyncPixelsToBitmap allocates a NEW wxBitmap; re-select it into the
            // source DC, else the Blit below reads the stale (pre-edit) bitmap.
            if (dib->bmp) src->dc.SelectObject(*dib->bmp);
        }
    }

    wxRasterOperationMode fn = MapRop(dwRop);
    p->dc.Blit(x, y, nWidth, nHeight, &src->dc, xSrc, ySrc, fn);

    // If the DESTINATION has a DIBSection selected, sync wxBitmap→raw buffer
    // so the caller can read the blitted pixels directly via pBits.
    if (m_hDC) {
        if (DIBSectionData* dib = GetDIBForHDC(m_hDC))
            dib->SyncBitmapToPixels();
    }

    return true;
}

bool CDC::StretchBlt(int x, int y, int nWidth, int nHeight,
                     CDC* pSrcDC, int xSrc, int ySrc,
                     int nSrcWidth, int nSrcHeight, DWORD dwRop)
{
    CDCImpl* p = Impl(this);
    CDCImpl* src = pSrcDC ? Impl(pSrcDC) : nullptr;
    if (!p || !src) return false;

    // Same DIBSection sync as BitBlt.
    if (pSrcDC && pSrcDC->m_hDC) {
        if (DIBSectionData* dib = GetDIBForHDC(pSrcDC->m_hDC)) {
            dib->SyncPixelsToBitmap();
            // SyncPixelsToBitmap allocates a NEW wxBitmap; re-select it into the
            // source DC, else the Blit below reads the stale (pre-edit) bitmap.
            if (dib->bmp) src->dc.SelectObject(*dib->bmp);
        }
    }

    wxRasterOperationMode fn = MapRop(dwRop);
    p->dc.StretchBlit(x, y, nWidth, nHeight,
                      &src->dc, xSrc, ySrc, nSrcWidth, nSrcHeight, fn);

    if (m_hDC) {
        if (DIBSectionData* dib = GetDIBForHDC(m_hDC))
            dib->SyncBitmapToPixels();
    }

    return true;
}

// ── GradientFill ─────────────────────────────────────────────────────────
// Called by CMiniWindow::Gradient() with the raw HDC from pdc->m_hDC.
// Mode 1 = horizontal (GRADIENT_FILL_RECT_H), 2 = vertical (GRADIENT_FILL_RECT_V).
BOOL GradientFill(HDC hdc, TRIVERTEX* pVertex, ULONG nVertex,
                  void* pMesh, ULONG nMesh, ULONG ulMode)
{
    if (!hdc || !pVertex || nVertex < 2 || !pMesh || nMesh < 1) return FALSE;
    auto it = g_hdcToCDC.find(hdc);
    if (it == g_hdcToCDC.end()) return FALSE;
    CDCImpl* p = Impl(it->second);
    if (!p) return FALSE;

    GRADIENT_RECT* r = static_cast<GRADIENT_RECT*>(pMesh);
    if (r->UpperLeft >= nVertex || r->LowerRight >= nVertex) return FALSE;

    TRIVERTEX& v0 = pVertex[r->UpperLeft];
    TRIVERTEX& v1 = pVertex[r->LowerRight];

    int x = (int)v0.x, y = (int)v0.y;
    int w = (int)v1.x - x;
    int h = (int)v1.y - y;
    if (w <= 0 || h <= 0) return FALSE;

    // TRIVERTEX Red/Green/Blue store 16-bit values: high byte = colour, low byte = fraction.
    wxColour startCol((unsigned char)(v0.Red   >> 8),
                      (unsigned char)(v0.Green >> 8),
                      (unsigned char)(v0.Blue  >> 8));
    wxColour endCol  ((unsigned char)(v1.Red   >> 8),
                      (unsigned char)(v1.Green >> 8),
                      (unsigned char)(v1.Blue  >> 8));

    wxDirection dir;
    switch (ulMode) {
        case GRADIENT_FILL_RECT_H: dir = wxRIGHT; break;  // horizontal
        case GRADIENT_FILL_RECT_V: dir = wxDOWN;  break;  // vertical
        default: return FALSE;
    }

    p->dc.GradientFillLinear(wxRect(x, y, w, h), startCol, endCol, dir);
    return TRUE;
}

// ── Text output ───────────────────────────────────────────────────────────

bool CDC::TextOut(int x, int y, LPCTSTR lpszString, int nCount) {
    CDCImpl* p = Impl(this);
    if (!p || !lpszString) return false;
    p->ApplyFont();
    wxString s = CdcDecodeText(lpszString, nCount);

    // Honour TA_BOTTOM (y is baseline, not top)
    if ((p->textAlign & TA_BOTTOM) && p->currentFont && p->currentFont->m_impl) {
        wxFontMetrics fm = p->dc.GetFontMetrics();
        y -= fm.ascent;
    }
    if (p->bkMode == OPAQUE) {
        wxSize ext = p->dc.GetTextExtent(s);
        p->dc.SetPen(*wxTRANSPARENT_PEN);
        p->dc.SetBrush(wxBrush(WxCol(p->bkColor)));
        p->dc.DrawRectangle(x, y, ext.GetWidth(), ext.GetHeight());
    }
    p->dc.SetTextForeground(WxCol(p->textColor));
    p->dc.DrawText(s, x, y);
    return true;
}

bool CDC::TextOut(int x, int y, const CString& str) {
    return TextOut(x, y, (LPCTSTR)str, str.GetLength());
}

bool CDC::ExtTextOut(int x, int y, UINT nOptions, LPCRECT lpRect,
                     LPCTSTR lpszString, int nCount, const int* /*lpDxWidths*/)
{
    CDCImpl* p = Impl(this);
    if (!p) return false;
    p->ApplyFont();

    wxString s = CdcDecodeText(lpszString, nCount);

    // ETO_OPAQUE: fill the clip rectangle with bk colour first
    if ((nOptions & ETO_OPAQUE) && lpRect) {
        p->dc.SetPen(*wxTRANSPARENT_PEN);
        p->dc.SetBrush(wxBrush(WxCol(p->bkColor)));
        p->dc.DrawRectangle(lpRect->left, lpRect->top,
                            lpRect->right  - lpRect->left,
                            lpRect->bottom - lpRect->top);
    }

    int drawY = y;
    if ((p->textAlign & TA_BOTTOM) && p->currentFont && p->currentFont->m_impl) {
        wxFontMetrics fm = p->dc.GetFontMetrics();
        drawY -= fm.ascent;
    }

    if (nOptions & ETO_CLIPPED) {
        // Apply clipping region if requested
        if (lpRect) {
            p->dc.SetClippingRegion(lpRect->left, lpRect->top,
                                    lpRect->right  - lpRect->left,
                                    lpRect->bottom - lpRect->top);
        }
    }

    p->dc.SetTextForeground(WxCol(p->textColor));
    p->dc.DrawText(s, x, drawY);

    if (nOptions & ETO_CLIPPED)
        p->dc.DestroyClippingRegion();

    return true;
}

int CDC::DrawText(LPCTSTR lpszString, int nCount, LPRECT lpRect, UINT nFormat) {
    if (!lpRect) return 0;
    CDCImpl* p = Impl(this);
    if (!p) return 0;
    p->ApplyFont();

    wxString s = CdcDecodeText(lpszString, nCount);

    wxRect r(lpRect->left, lpRect->top,
             lpRect->right - lpRect->left,
             lpRect->bottom - lpRect->top);

    int wx_flags = 0;
    if (nFormat & DT_CENTER)      wx_flags |= wxALIGN_CENTER_HORIZONTAL;
    if (nFormat & DT_RIGHT)       wx_flags |= wxALIGN_RIGHT;
    if (nFormat & DT_VCENTER)     wx_flags |= wxALIGN_CENTER_VERTICAL;
    if (nFormat & DT_BOTTOM)      wx_flags |= wxALIGN_BOTTOM;
    if (nFormat & DT_WORDBREAK)   wx_flags |= wxTE_MULTILINE;
    if (nFormat & DT_SINGLELINE)  {}
    if (nFormat & DT_CALCRECT) {
        // Just measure, don't draw
        wxSize sz = p->dc.GetTextExtent(s);
        lpRect->right  = lpRect->left + sz.GetWidth();
        lpRect->bottom = lpRect->top  + sz.GetHeight();
        return sz.GetHeight();
    }

    p->dc.SetTextForeground(WxCol(p->textColor));
    if (p->bkMode == OPAQUE) {
        p->dc.SetBackgroundMode(wxSOLID);
        p->dc.SetTextBackground(WxCol(p->bkColor));
    } else {
        p->dc.SetBackgroundMode(wxTRANSPARENT);
    }
    p->dc.DrawLabel(s, r, wx_flags);
    return r.GetHeight();
}

int CDC::DrawText(const CString& str, LPRECT lpRect, UINT nFormat) {
    return DrawText((LPCTSTR)str, str.GetLength(), lpRect, nFormat);
}

// ╔══════════════════════════════════════════════════════════════════════╗
// ║  Global GDI functions                                                ║
// ╚══════════════════════════════════════════════════════════════════════╝

int GetDeviceCaps(HDC hDC, int nIndex) {
    // hDC is a CDC* via operator HDC() — but it's opaque here.
    // We return reasonable values independent of the specific DC.
    switch (nIndex) {
        case BITSPIXEL:  return 32;
        case PLANES:     return 1;
        case LOGPIXELSX:
        case LOGPIXELSY: {
            wxSize dpi = wxGetDisplayPPI();
            return (nIndex == LOGPIXELSX) ? dpi.x : dpi.y;
        }
        case HORZRES: return wxGetDisplaySize().GetWidth();
        case VERTRES: return wxGetDisplaySize().GetHeight();
        default:      return 0;
    }
}

HPEN ExtCreatePen(DWORD dwPenStyle, DWORD dwWidth,
                  const LOGBRUSH* lplb, DWORD /*dwStyleCount*/,
                  const DWORD* /*lpStyle*/)
{
    // Allocate a wxPen on the heap.  The caller is expected to Attach() this
    // to a CPen object which then owns and deletes it.
    if (!lplb) return nullptr;
    wxColour col = WxCol(lplb->lbColor);
    wxPen* pen = new wxPen(col, (int)dwWidth, MapPenStyle((int)dwPenStyle));
    pen->SetCap(MapPenCap((int)dwPenStyle));
    pen->SetJoin(MapPenJoin((int)dwPenStyle));
    return reinterpret_cast<HPEN>(pen);
}

// ── wx_gdi_compat.h accessor implementations ─────────────────────────────
// These are declared in platform/wx_gdi_compat.h (guarded by
// MUSHCLIENT_WX_BACKEND) and implemented here in wx_gdi_compat.cpp so that
// wx_world_child.cpp can extract wxDC/wxBitmap pointers from opaque MFC
// impl pointers without knowing the CDCImpl struct layout.

#include "platform/wx_gdi_compat.h"

wxDC* WxDCFromCDC(CDC* dc)
{
    if (!dc || !dc->m_impl) return nullptr;
    return &static_cast<CDCImpl*>(dc->m_impl)->dc;
}

wxBitmap* WxBitmapFromCBitmap(CBitmap* bmp)
{
    return (bmp && bmp->m_impl) ? static_cast<wxBitmap*>(bmp->m_impl) : nullptr;
}

wxBitmap* PaintOwnerDrawButton(CButton* btn, int w, int h, bool disabled)
{
    if (!btn || w <= 0 || h <= 0) return nullptr;

    // Real (wx-backed) CDC — this TU is compiled with MUSHCLIENT_FULL_BUILD.
    CDC dc;
    dc.CreateCompatibleDC(nullptr);
    CBitmap bmp;
    bmp.CreateCompatibleBitmap(&dc, w, h);
    CBitmap* oldBmp = dc.SelectObject(&bmp);

    DRAWITEMSTRUCT dis;
    memset(&dis, 0, sizeof(dis));
    dis.CtlType      = ODT_BUTTON;
    dis.hDC          = dc.GetSafeHdc();
    dis.rcItem.left  = 0; dis.rcItem.top    = 0;
    dis.rcItem.right = w; dis.rcItem.bottom = h;
    dis.itemState    = disabled ? ODS_DISABLED : 0;

    btn->DrawItem(&dis);                 // virtual → CColourButton::DrawItem etc.

    dc.SelectObject(oldBmp);             // deselect so the wxBitmap is readable

    wxBitmap* src = static_cast<wxBitmap*>(bmp.m_impl);
    if (!src || !src->IsOk()) return nullptr;
    // Return an opaque copy (the swatch bitmap is 32-bit; strip any alpha so the
    // painted RGB shows when blitted rather than reading back transparent/black).
    wxImage img = src->ConvertToImage();
    if (!img.IsOk()) return new wxBitmap(*src);
    if (img.HasAlpha()) img.ClearAlpha();
    return new wxBitmap(img, 24);
}

// ── SelectObject(HDC, HGDIOBJ) ─────────────────────────────────────────────
// Intercepts raw Win32 SelectObject calls (e.g. SelectObject(A_DC.m_hDC, hbm))
// so that a DIBSection's wxBitmap is selected into the right CDC's wxMemoryDC.
// Defined here (rather than with the DIBSection infra block above) because it
// needs the full CDCImpl definition.
HGDIOBJ SelectObject(HDC hdc, HGDIOBJ hObject)
{
    if (!hdc) return nullptr;

    HGDIOBJ prev = nullptr;
    auto prevIt = g_hdcToSelectedDib.find(hdc);
    if (prevIt != g_hdcToSelectedDib.end()) prev = (HGDIOBJ)prevIt->second;

    HBITMAP hbm = reinterpret_cast<HBITMAP>(hObject);
    auto dibIt = g_dibSections.find(hbm);
    if (dibIt != g_dibSections.end()) {
        // It's a DIBSection — track the selection and put the wxBitmap into the CDC.
        g_hdcToSelectedDib[hdc] = hbm;

        auto cdcIt = g_hdcToCDC.find(hdc);
        if (cdcIt != g_hdcToCDC.end()) {
            // Select the DIBSection's wxBitmap directly into the wxMemoryDC.
            // (wxMemoryDC::SelectObject auto-deselects any previously selected bitmap.)
            CDCImpl* impl = static_cast<CDCImpl*>(cdcIt->second->m_impl);
            if (impl) {
                impl->dc.SelectObject(*dibIt->second->bmp);
                impl->currentBitmap = nullptr;  // DIBSection managed externally
            }
        }
    } else {
        g_hdcToSelectedDib.erase(hdc);
    }

    return prev;
}
