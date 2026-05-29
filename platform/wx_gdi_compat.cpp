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
    std::vector<unsigned char> pixels; // raw 24-bit BGR pixel data
    wxBitmap* bmp = nullptr;  // wxBitmap for rendering (owned here)

    ~DIBSectionData() { delete bmp; }

    // Copy pixels from wxBitmap into raw buffer (call after BitBlt INTO this DC).
    void SyncBitmapToPixels() {
        if (!bmp || !bmp->IsOk()) return;
        wxImage img = bmp->ConvertToImage();
        for (int y = 0; y < height; ++y) {
            // img is always top-down; pixels buffer may be bottom-up
            int imgRow = bottomUp ? (height - 1 - y) : y;
            unsigned char* dst = pixels.data() + y * stride;
            for (int x = 0; x < width; ++x) {
                dst[x*3+0] = img.GetBlue (x, imgRow);  // BGR
                dst[x*3+1] = img.GetGreen(x, imgRow);
                dst[x*3+2] = img.GetRed  (x, imgRow);
            }
        }
    }

    // Copy raw buffer into wxBitmap (call before BitBlt FROM this DC).
    void SyncPixelsToBitmap() {
        if (!bmp) return;
        wxImage img(width > 0 ? width : 1, height > 0 ? height : 1);
        for (int y = 0; y < height; ++y) {
            int bufRow = bottomUp ? (height - 1 - y) : y;
            const unsigned char* src = pixels.data() + bufRow * stride;
            for (int x = 0; x < width; ++x) {
                img.SetRGB(x, y, src[x*3+2], src[x*3+1], src[x*3+0]);  // BGR→RGB
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
    dib->pixels.assign(stride * (he > 0 ? he : 1), 0);
    dib->bmp      = new wxBitmap(w > 0 ? w : 1, he > 0 ? he : 1, 32);

    *ppvBits = dib->pixels.data();

    HBITMAP handle = reinterpret_cast<HBITMAP>(dib);
    g_dibSections[handle] = dib;
    return handle;
}

// SelectObject(HDC, HGDIOBJ) — non-Windows implementation.
// Intercepts raw Win32 SelectObject calls (e.g. SelectObject(A_DC.m_hDC, hbm))
// so that DIBSection bitmaps are actually selected into the right CDC.
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
            // Delegate to the CDC's member SelectObject so the wxMemoryDC is updated.
            cdcIt->second->SelectObject(nullptr);  // deselect previous
            // Directly select the DIBSection's wxBitmap into the wxMemoryDC.
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

    wxFontInfo fi(wxSize(0, pixH));
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
    lf->lfHeight = -f->GetPixelSize().GetHeight();
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
        // Windows stores 8 bytes, one byte per row, MSB = leftmost pixel.
        // We convert to a wxImage (8x8) then create a wxBitmap.
        wxImage img(w, h);
        img.InitAlpha();
        const unsigned char* src = static_cast<const unsigned char*>(lpBits);
        for (int y = 0; y < h && y < 8; ++y) {
            unsigned char row = src[y];
            for (int x = 0; x < w && x < 8; ++x) {
                bool set = (row & (0x80u >> x)) != 0;
                unsigned char v = set ? 0 : 255;   // Windows mono: 1=black,0=white
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

HBITMAP CBitmap::CreateCompatibleBitmap(void* /*pDC*/, int nWidth, int nHeight) {
    CreateBitmap(nWidth, nHeight, 1, 32, nullptr);
    return static_cast<HBITMAP>(m_impl);   // opaque handle
}

BOOL CBitmap::GetBitmap(BITMAP* pBitmap) {
    if (!pBitmap || !m_impl) return FALSE;
    wxBitmap* bmp = static_cast<wxBitmap*>(m_impl);
    *pBitmap = {};
    pBitmap->bmWidth   = bmp->GetWidth();
    pBitmap->bmHeight  = bmp->GetHeight();
    pBitmap->bmBitsPixel = bmp->GetDepth();
    pBitmap->bmPlanes  = 1;
    return TRUE;
}

int CBitmap::GetObject(int /*nCount*/, void* lpObject) const {
    if (!lpObject || !m_impl) return 0;
    wxBitmap* bmp = static_cast<wxBitmap*>(m_impl);
    BITMAP* pbm = static_cast<BITMAP*>(lpObject);
    *pbm = {};
    pbm->bmWidth      = bmp->GetWidth();
    pbm->bmHeight     = bmp->GetHeight();
    pbm->bmBitsPixel  = bmp->GetDepth();
    pbm->bmPlanes     = 1;
    return sizeof(BITMAP);
}

BOOL CBitmap::Attach(HGDIOBJ hObject) {
    if (m_impl) { delete static_cast<wxBitmap*>(m_impl); m_impl = nullptr; }
    m_hObject = hObject;
    // hObject is expected to be a wxBitmap* (e.g. from CreateDIBSection stub)
    // For now just store it; DrawBitmap won't work without a proper wxBitmap
    m_impl = hObject;
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

CDC* CDC::FromHandle(HDC /*hDC*/) {
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
    lpMetrics->tmMaxCharWidth     = fm.averageWidth * 2;
    if (p->currentFont && p->currentFont->m_impl) {
        wxFont* f = static_cast<wxFont*>(p->currentFont->m_impl);
        lpMetrics->tmWeight   = f->GetWeight() == wxFONTWEIGHT_BOLD ? FW_BOLD : FW_NORMAL;
        lpMetrics->tmItalic   = (f->GetStyle() != wxFONTSTYLE_NORMAL) ? 1 : 0;
        lpMetrics->tmUnderlined = f->GetUnderlined() ? 1 : 0;
        lpMetrics->tmStruckOut  = f->GetStrikethrough() ? 1 : 0;
    }
    lpMetrics->tmPitchAndFamily = FIXED_PITCH;
    return true;
}

CSize CDC::GetTextExtent(LPCTSTR lpszString, int nCount) {
    CDCImpl* p = Impl(this);
    if (!p) return {};
    p->ApplyFont();
    wxString s = (nCount >= 0)
        ? wxString::FromUTF8(lpszString, nCount)
        : wxString::FromUTF8(lpszString);
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
    // Simple approximation: draw a 3D border using the edge type
    if (!lpRect) return;
    COLORREF light = 0xFFFFFF, shadow = 0x808080, dark = 0x404040;
    COLORREF tl, br;
    switch (nEdge) {
        case EDGE_RAISED: case BDR_RAISEDOUTER: case BDR_RAISEDINNER:
            tl = light; br = dark; break;
        case EDGE_SUNKEN: case BDR_SUNKENOUTER: case BDR_SUNKENINNER:
            tl = dark; br = light; break;
        default:
            tl = shadow; br = shadow; break;
    }
    // Only draw the requested sides
    RECT r2 = *lpRect;
    Draw3dRect(&r2, tl, br);
    (void)nFlags;
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
    p->ApplyPen(); p->ApplyBrush();
    p->dc.DrawRoundedRectangle(x1, y1, x2 - x1, y2 - y1,
                               (double)std::min(x3, y3) / 2.0);
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
    // Approximate with polygon
    wxPoint pts[3] = { wxPoint(x3, y3), wxPoint(cx, cy), wxPoint(x4, y4) };
    p->dc.DrawEllipticArc(x1, y1, x2 - x1, y2 - y1, startDeg, endDeg);
    (void)pts;
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
    // Approximate with polyline for now
    return Polyline(lpPoints, nCount);
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
        if (DIBSectionData* dib = GetDIBForHDC(pSrcDC->m_hDC))
            dib->SyncPixelsToBitmap();
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
        if (DIBSectionData* dib = GetDIBForHDC(pSrcDC->m_hDC))
            dib->SyncPixelsToBitmap();
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

// ── Text output ───────────────────────────────────────────────────────────

bool CDC::TextOut(int x, int y, LPCTSTR lpszString, int nCount) {
    CDCImpl* p = Impl(this);
    if (!p || !lpszString) return false;
    p->ApplyFont();
    wxString s = (nCount >= 0)
        ? wxString::FromUTF8(lpszString, nCount)
        : wxString::FromUTF8(lpszString);

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

    wxString s = (nCount >= 0)
        ? wxString::FromUTF8(lpszString, nCount)
        : wxString::FromUTF8(lpszString);

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

    wxString s = (nCount >= 0)
        ? wxString::FromUTF8(lpszString, nCount)
        : wxString::FromUTF8(lpszString);

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
