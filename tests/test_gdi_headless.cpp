// tests/test_gdi_headless.cpp — Phase 2 Unit B-2 (docs/PHASE2-RECOVERY.md §5/§6).
//
// Proves the headless GDI shim (platform/gdi_headless.cpp) in ISOLATION.  This is
// the one test target compiled WITH -DMUSHCLIENT_FULL_BUILD, so CDC / CFont /
// CBitmap / CGdiObject take the mfc_ui.h *full-build* shape (declarations-only,
// out-of-line virtual dtors → vtables) — the exact shape the real doc.cpp /
// doc_construct.cpp / Utilities.cpp call these classes through.  It links ONLY the
// shim (no mushcore — which is a DIFFERENT, non-full GDI shape → would ODR-clash),
// so CI proves the shim resolves + behaves before B-3 folds it into the full-build
// mushcore.
//
// It reproduces the two headless-reachable call shapes and asserts the shim's
// degenerate-but-safe contract:
//   1. the font-metric path (doc.cpp ChangeFont/ChangeInputFont, executed headless
//      via world.SetInputFont / world load): CreateCompatibleDC + GetDeviceCaps +
//      CFont::CreateFont + CDC::SelectObject + GetTextMetrics — the metric that
//      lands in m_FontHeight/m_FontWidth must be 0 (matching doc_construct.cpp's
//      ctor), and GetTextMetrics must ZERO the struct (never leave ChangeFont
//      reading uninitialised stack).
//   2. the image path (Utilities.cpp LoadPng/LoadPngMemory, DEAD headless):
//      ::CreateDIBSection must return nullptr and null the pixel pointer, so a
//      would-be LoadPng degrades to eUnableToLoadImage rather than crash.

#include "stdafx.h"          // full-build CDC / CFont / CBitmap / CGdiObject
#include "mc_test.h"

#include <cstring>

// ── 1. Font-metric path (ChangeFont / ChangeInputFont shape) ────────────────
// Mirrors doc.cpp:1140-1192 / 1229-1258 exactly, then asserts the metric that
// ChangeFont would store.
TEST(GdiHeadless, FontMetricPathZeroesMetricsSafely)
{
    CDC dc;
    // ChangeFont: `dc.CreateCompatibleDC(NULL);`  (return is ignored there)
    EXPECT_FALSE(dc.CreateCompatibleDC(NULL));

    // ChangeFont: `-MulDiv(nHeight, dc.GetDeviceCaps(LOGPIXELSY), 72)`
    EXPECT_EQ(0, dc.GetDeviceCaps(LOGPIXELSY));

    CFont* font = new CFont;                 // exercises CFont ctor + vtable
    ASSERT_NE((CFont*)nullptr, font);
    EXPECT_FALSE(font->CreateFont(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                  DEFAULT_CHARSET, 0, 0, 0,
                                  FIXED_PITCH, "Courier"));

    // ChangeFont: `dc.SelectObject(m_font[HILITE]);`
    EXPECT_EQ((CFont*)nullptr, dc.SelectObject(font));

    // ChangeFont: `TEXTMETRIC tm; dc.GetTextMetrics(&tm);`  — poison first so a
    // non-zeroing GetTextMetrics would be caught.
    TEXTMETRIC tm;
    std::memset(&tm, 0x7F, sizeof tm);
    EXPECT_FALSE(dc.GetTextMetrics(&tm));

    // ChangeFont: `m_FontHeight = tm.tmHeight; m_FontWidth = tm.tmAveCharWidth;`
    long m_FontHeight = tm.tmHeight;
    long m_FontWidth  = tm.tmAveCharWidth;
    EXPECT_EQ(0, m_FontHeight);   // matches doc_construct.cpp's ctor (0 headless)
    EXPECT_EQ(0, m_FontWidth);

    delete font;                             // exercises virtual ~CFont path
}

// ── 2. Image path (LoadPng / LoadPngMemory shape) ───────────────────────────
// Mirrors Utilities.cpp:2406-2433: a stack CBitmap + CreateCompatibleDC + the
// free ::CreateDIBSection.  DEAD headless, but must link and degrade cleanly.
TEST(GdiHeadless, ImagePathDegradesToNullBitmap)
{
    CDC B_DC;
    B_DC.CreateCompatibleDC(NULL);
    CBitmap B_bmp;                           // ctor + (later) virtual dtor + vtable

    BITMAPINFO bmi;
    std::memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize     = sizeof bmi;
    bmi.bmiHeader.biWidth    = 4;
    bmi.bmiHeader.biHeight   = 4;
    bmi.bmiHeader.biPlanes   = 1;
    bmi.bmiHeader.biBitCount = 32;

    void*   pBits = (void*)0xDEADBEEF;       // poison — shim must null it
    HBITMAP hbmp  = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);

    EXPECT_EQ((HBITMAP)nullptr, hbmp);       // Utilities.cpp: `if (!hbmp) return eUnableToLoadImage;`
    EXPECT_EQ((void*)nullptr, pBits);
}

// ── 3. GDI object lifetimes (vtable / dtor resolution) ──────────────────────
// Heap + stack construct/destruct of every shim-backed class — a bad vtable or a
// missing dtor here is a link failure or a crash.
TEST(GdiHeadless, ObjectsConstructAndDestructCleanly)
{
    // A freshly-constructed GDI object owns no handle, so the accessors return
    // null.  This is both the no-crash smoke test AND a real assertion on the
    // handle accessors — a stub returning a bogus non-null handle is caught.
    { CDC dc;    EXPECT_EQ((HDC)nullptr,     dc.GetSafeHdc()); }     // ~CDC
    { CFont f;   EXPECT_EQ((HGDIOBJ)nullptr, f.GetSafeHandle()); }  // ~CFont → ~CGdiObject
    { CBitmap b; (void)b; }                                          // ~CBitmap → ~CGdiObject (no GetSafeHandle shim)
    CGdiObject* obj = new CFont;             // delete-through-base → virtual dispatch
    delete obj;
}

MC_TEST_MAIN()
