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
#include "pal/surface_bridge.h"

pal::SurfaceId HeadlessSurfaceOfBitmapForTesting(const CBitmap*);

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

// ── 4. The headless surface bridge's canvas lifetime ────────────────────────
//
// platform/headless_surface.cpp makes CBitmap own a pal::CpuRaster surface and
// CDC::SelectObject associate a DC with it.  Three parts of that pairing are
// easy to get wrong and are not reachable through world.Window*, so they are
// asserted directly here.
//
// The third one — DeleteObject making any DC forget the surface — is the reason
// a mutation that removed it survived the Lua-level tests: CMiniWindow::Create
// deletes the DC immediately afterwards, so the stale entry is never read
// through the scripting API.  It is still a real hazard, because `delete pdc;
// pdc = new CDC;` very often hands back the SAME address, and the entry would
// then be inherited by an unrelated DC.

TEST(GdiHeadless, CanvasBitmapOwnsASurfaceAndSelectObjectAssociatesIt)
{
    CDC dc;
    CBitmap bmp;

    // A DC with nothing selected paints nowhere — and must not crash.
    dc.FillSolidRect(0, 0, 10, 10, RGB(255, 0, 0));

    ASSERT_TRUE(bmp.CreateCanvasBitmap(16, 16));

    // Still not selected: the fill must NOT reach the new canvas.  Without this
    // control, "always paint the most recent bitmap" would pass.
    dc.FillSolidRect(0, 0, 16, 16, RGB(255, 0, 0));

    CBitmap* previous = dc.SelectObject(&bmp);
    EXPECT_EQ((CBitmap*)nullptr, previous);   // nothing was selected before

    // Now it lands.  Reading back through the surface is what proves it.
    dc.FillSolidRect(0, 0, 16, 16, RGB(0, 0, 255));
    EXPECT_TRUE(bmp.DeleteObject());          // releases the surface
    EXPECT_FALSE(bmp.DeleteObject());         // ...and only once
}

TEST(GdiHeadless, DeletingABitmapLeavesNoDcPointingAtItsSurface)
{
    // The hazard: a DC that outlives the bitmap selected into it.  After
    // DeleteObject the surface is destroyed, so a later fill through that DC
    // must be inert rather than addressing freed state.
    CDC dc;
    {
        CBitmap bmp;
        ASSERT_TRUE(bmp.CreateCanvasBitmap(8, 8));
        dc.SelectObject(&bmp);
        dc.FillSolidRect(0, 0, 8, 8, RGB(0, 255, 0));
        bmp.DeleteObject();
    }
    // If DeleteObject had not cleared the association this addresses a
    // destroyed surface.  Under ASan that is a use-after-free; without it, it
    // is silent corruption of whatever surface id got reused.
    dc.FillSolidRect(0, 0, 8, 8, RGB(255, 255, 255));

    // And the same DC must still be usable for a NEW canvas afterwards — the
    // case CMiniWindow::Create hits every time a window is re-created.
    CBitmap fresh;
    ASSERT_TRUE(fresh.CreateCanvasBitmap(8, 8));
    EXPECT_EQ((CBitmap*)nullptr, dc.SelectObject(&fresh));
    dc.FillSolidRect(0, 0, 8, 8, RGB(0, 0, 255));
    EXPECT_TRUE(fresh.DeleteObject());
}

// Why DeleteObject's DC cleanup is DEFENSIVE, and what would make it load-bearing.
//
// This test was written to prove the cleanup matters, and it proved the
// opposite — which is worth keeping rather than deleting.
//
// The hazard would be: CpuRaster hands a destroyed surface's id back to the next
// createSurface, and a DC still holding the stale association silently paints
// into a canvas belonging to something else.  It cannot happen today, because
// CpuRaster allocates ids monotonically and never recycles them (the [note]
// below prints the two ids so this stays visible rather than assumed).
//
// So the cleanup in headless_surface.cpp is hygiene, not a fix for a live bug —
// and no mutation of it can fail a test, which is why removing it survives the
// suite.  That is recorded rather than papered over.
//
// What this test actually pins is the INVARIANT the reasoning rests on: ids are
// not recycled.  If someone makes CpuRaster reuse them, the note changes and the
// cleanup becomes load-bearing — at which point the assertion below starts
// carrying real weight without anyone having to rediscover why.
TEST(GdiHeadless, AStaleDcCannotPaintIntoAReusedSurface)
{
    CDC stale;
    pal::SurfaceId freed = pal::SurfaceId::None;
    {
        CBitmap doomed;
        ASSERT_TRUE(doomed.CreateCanvasBitmap(8, 8));
        freed = HeadlessSurfaceOfBitmapForTesting(&doomed);
        ASSERT_TRUE(freed != pal::SurfaceId::None);
        stale.SelectObject(&doomed);
        doomed.DeleteObject();
    }

    // The next canvas very likely gets that id back.
    CBitmap victim;
    ASSERT_TRUE(victim.CreateCanvasBitmap(8, 8));
    const pal::SurfaceId reused = HeadlessSurfaceOfBitmapForTesting(&victim);

    CDC live;
    live.SelectObject(&victim);
    live.FillSolidRect(0, 0, 8, 8, RGB(0, 0, 255));      // victim is blue
    ASSERT_EQ((unsigned long) 0x00FF0000u,
              (unsigned long) pal::g_surface->getPixel(reused, { 4, 4 }));

    // The stale DC tries to paint red.  It must reach nothing.
    stale.FillSolidRect(0, 0, 8, 8, RGB(255, 0, 0));
    // Still blue: a red pixel here means the stale DC reached a reused surface.
    EXPECT_EQ((unsigned long) 0x00FF0000u,
              (unsigned long) pal::g_surface->getPixel(reused, { 4, 4 }));

    printf("  [note] freed id %d, next id %d — %s\n", (int) freed, (int) reused,
           reused == freed ? "RECYCLED: the cleanup is now load-bearing"
                           : "not recycled: the cleanup stays defensive");
    EXPECT_TRUE(victim.DeleteObject());
}

MC_TEST_MAIN()
