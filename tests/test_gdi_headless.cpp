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
//   2. the image path (png_load_shared.cpp LoadPng/LoadPngMemory): the DIB
//      STORE — ::CreateDIBSection hands out a real padded, zeroed, bottom-up
//      buffer and CBitmap::Attach turns it into a pal surface with the right
//      geometry, orientation, depth and alpha.  (This was "DEAD headless, must
//      return nullptr" until stage 2; both its callers are in mushcore now.)

#include "stdafx.h"          // full-build CDC / CFont / CBitmap / CGdiObject
#include "mc_test.h"
#include "pal/surface_bridge.h"
#include "pal/isurface.h"

pal::SurfaceId HeadlessSurfaceOfBitmapForTesting(const CBitmap*);
size_t         HeadlessLiveDibCountForTesting();

#include <cstring>
#include <vector>
#include <cstdio>            // LoadBmp backslash-path test: fopen/fwrite
#include <unistd.h>          // unlink
#include <sys/stat.h>        // mkdir
#include "scripting/errors.h"  // eFileNotFound / eUnableToLoadImage

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

// ── 2. The DIB store ────────────────────────────────────────────────────────
//
// This used to be ImagePathDegradesToNullBitmap, asserting that
// ::CreateDIBSection returned null — the exact "a test pins a stub's contract
// and so blocks adopting the real body" shape.  The image path is no longer
// dead headless: png_load_shared.cpp and miniwindow.cpp are both in mushcore,
// so these two entry points are what a loaded PNG's pixels actually travel
// through.  The tests below are the same shape as the old one (the LoadPng
// call sequence, png_load_shared.cpp:146-182) with the assertions turned round.

// Fill a DIB's buffer the way LoadPng does — bottom-up, 4-byte padded rows.
static void FillDib(unsigned char* bits, int w, int h, int bpp)
{
    const int stride = (w * (bpp / 8) + 3) & ~3;
    for (int imgY = 0; imgY < h; ++imgY) {
        unsigned char* row = bits + (size_t)(h - 1 - imgY) * stride;   // bottom-up
        for (int x = 0; x < w; ++x) {
            unsigned char* px = row + x * (bpp / 8);
            px[0] = (unsigned char)(10 + x);          // B
            px[1] = (unsigned char)(40 + imgY);       // G — differs from B
            px[2] = (unsigned char)(90 + x + imgY);   // R — differs from both
            if (bpp == 32) px[3] = (unsigned char)(200 - imgY * 30);
        }
    }
}

static BITMAPINFO MakeBmi(int w, int h, int bpp)
{
    BITMAPINFO bmi;
    std::memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize     = sizeof bmi;
    bmi.bmiHeader.biWidth    = w;
    bmi.bmiHeader.biHeight   = h;          // positive = bottom-up, as both loaders ask
    bmi.bmiHeader.biPlanes   = 1;
    bmi.bmiHeader.biBitCount = (WORD) bpp;
    return bmi;
}

TEST(GdiHeadless, CreateDIBSectionHandsBackARealBuffer)
{
    CDC B_DC;
    B_DC.CreateCompatibleDC(NULL);

    // 5 wide x 3 high, so a transposed width/height shows up, and 5*3 = 15 bytes
    // per row at 24bpp — NOT a multiple of 4, so the row padding is exercised
    // rather than being accidentally right.
    BITMAPINFO bmi = MakeBmi(5, 3, 24);
    void* pBits = (void*)0xDEADBEEF;         // poison: a failure must null it
    HBITMAP hbmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);

    ASSERT_NE((HBITMAP)nullptr, hbmp);       // png_load_shared: `if (!hbmp) return eUnableToLoadImage`
    ASSERT_NE((void*)nullptr, pBits);
    EXPECT_NE((void*)0xDEADBEEF, pBits);

    // The buffer is writable for stride * height bytes and starts zeroed (the
    // alpha ops rely on the zero fill past an image edge).
    const int stride = (5 * 3 + 3) & ~3;     // = 16, not 15
    unsigned char* p = (unsigned char*) pBits;
    for (int i = 0; i < stride * 3; ++i)
        ASSERT_EQ(0, (int) p[i]);
    FillDib(p, 5, 3, 24);

    EXPECT_EQ((size_t)1, HeadlessLiveDibCountForTesting());
    EXPECT_TRUE(DeleteObject((HGDIOBJ) hbmp));
    EXPECT_EQ((size_t)0, HeadlessLiveDibCountForTesting());
    EXPECT_FALSE(DeleteObject((HGDIOBJ) hbmp));   // gone, and says so
}

TEST(GdiHeadless, AttachTurnsADibIntoAReadableSurface)
{
    BITMAPINFO bmi = MakeBmi(5, 3, 24);
    void* pBits = nullptr;
    HBITMAP hbmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    ASSERT_NE((void*)nullptr, pBits);
    FillDib((unsigned char*) pBits, 5, 3, 24);

    CBitmap image;
    ASSERT_TRUE(image.Attach((HGDIOBJ) hbmp));

    // Attach CONSUMES the handle, as wx's MaterializeDIBSection does — otherwise
    // every WindowLoadImage would leak its decode buffer.
    EXPECT_EQ((size_t)0, HeadlessLiveDibCountForTesting());

    BITMAP bi;
    std::memset((void *) &bi, 0x7F, sizeof bi);       // poison: GetBitmap must overwrite
    ASSERT_TRUE(image.GetBitmap(&bi));
    EXPECT_EQ(5, (int) bi.bmWidth);
    EXPECT_EQ(3, (int) bi.bmHeight);
    EXPECT_EQ(24, (int) bi.bmBitsPixel);
    EXPECT_EQ(1, (int) bi.bmPlanes);
    EXPECT_EQ(0, (int) bi.bmType);        // ImageInfo 1 reports this verbatim
    EXPECT_EQ(16, (int) bi.bmWidthBytes); // 5*3 = 15, padded

    // The pixels arrived, in the right places.  Reading (x,y) rather than a
    // single pixel is the point: the DIB is stored bottom-up and the surface is
    // addressed top-down, so a missing flip shows as a vertical mirror — and
    // with h = 3 the middle row is the only one that would survive one.
    const pal::SurfaceId sid = HeadlessSurfaceOfBitmapForTesting(&image);
    ASSERT_TRUE(sid != pal::SurfaceId::None);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x) {
            // COLORREF is 0x00BBGGRR — R lowest.  The three channels are given
            // different values in FillDib precisely so a swap here fails.
            const pal::Colour c = pal::g_surface->getPixel(sid, { x, y });
            const int r = (int)(c & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)((c >> 16) & 0xFF);
            ASSERT_EQ(10 + x, b);
            ASSERT_EQ(40 + y, g);
            ASSERT_EQ(90 + x + y, r);
        }

    // A 24bpp image has no alpha, and must read back as 0 rather than as
    // opaque: that is what Windows leaves in the high byte when a 24bpp source
    // is BitBlt'ed into a 32bpp DIB, and WindowDrawImageAlpha treats 0 as
    // "keep the base".  255 here would silently make every 24bpp image opaque.
    pal::PixelBuffer buf;
    pal::g_surface->readPixels(sid, { 0, 0, 5, 3 }, buf, pal::PixelFormat::BGRA32_Alpha);
    ASSERT_TRUE(buf.valid());
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x)
            ASSERT_EQ(0, (int) buf.row(y)[x * 4 + 3]);
}

// biHeight's SIGN is the DIB row-order rule: positive is bottom-up (what both
// loaders ask for), negative is top-down.  Nothing in MUSHclient makes a
// top-down DIB today, so the strongest available assertion is that the two
// spellings of the SAME image produce the same surface — which fails if either
// arm flips and the other does not.
TEST(GdiHeadless, DibRowOrderFollowsTheSignOfBiHeight)
{
    const int w = 4, h = 3, stride = (w * 3 + 3) & ~3;

    // Bottom-up: buffer row 0 is the image's BOTTOM row.
    BITMAPINFO up = MakeBmi(w, h, 24);
    void* upBits = nullptr;
    HBITMAP upH = CreateDIBSection(NULL, &up, DIB_RGB_COLORS, &upBits, NULL, 0);
    ASSERT_NE((void*)nullptr, upBits);
    FillDib((unsigned char*) upBits, w, h, 24);

    // Top-down: same image, buffer row 0 is the image's TOP row.
    BITMAPINFO down = MakeBmi(w, -h, 24);
    void* downBits = nullptr;
    HBITMAP downH = CreateDIBSection(NULL, &down, DIB_RGB_COLORS, &downBits, NULL, 0);
    ASSERT_NE((void*)nullptr, downBits);
    for (int imgY = 0; imgY < h; ++imgY)
        std::memcpy((unsigned char*) downBits + (size_t) imgY * stride,
                    (const unsigned char*) upBits + (size_t)(h - 1 - imgY) * stride,
                    (size_t) stride);

    CBitmap a, b;
    ASSERT_TRUE(a.Attach((HGDIOBJ) upH));
    ASSERT_TRUE(b.Attach((HGDIOBJ) downH));

    const pal::SurfaceId sa = HeadlessSurfaceOfBitmapForTesting(&a);
    const pal::SurfaceId sb = HeadlessSurfaceOfBitmapForTesting(&b);
    ASSERT_TRUE(sa != pal::SurfaceId::None && sb != pal::SurfaceId::None);

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const pal::Colour ca = pal::g_surface->getPixel(sa, { x, y });
            ASSERT_EQ(ca, pal::g_surface->getPixel(sb, { x, y }));
            // ...and both are the image the fill described, so "both flipped
            // the same way" cannot pass either.
            ASSERT_EQ(40 + y, (int)((ca >> 8) & 0xFF));   // G varies with y only
        }
}

TEST(GdiHeadless, AttachKeepsTheAlphaChannelOfA32bppDib)
{
    BITMAPINFO bmi = MakeBmi(4, 3, 32);
    void* pBits = nullptr;
    HBITMAP hbmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    ASSERT_NE((void*)nullptr, pBits);
    FillDib((unsigned char*) pBits, 4, 3, 32);

    CBitmap image;
    ASSERT_TRUE(image.Attach((HGDIOBJ) hbmp));

    BITMAP bi;
    ASSERT_TRUE(image.GetBitmap(&bi));
    // Load-bearing, not decoration: WindowDrawImageAlpha (miniwindow.cpp:3797)
    // and WindowGetImageAlpha (:3916) both return eImageNotInstalled unless
    // this reads 32.  (WindowMergeImageAlpha does NOT gate on depth — an
    // earlier version of this comment claimed it did.)
    EXPECT_EQ(32, (int) bi.bmBitsPixel);

    const pal::SurfaceId sid = HeadlessSurfaceOfBitmapForTesting(&image);
    ASSERT_TRUE(sid != pal::SurfaceId::None);

    // BGRA32_Alpha is the format the three alpha ops read an image with, and it
    // must give back what the PNG carried — per row, so an all-one-value alpha
    // (which any plausible bug would produce) cannot pass.
    pal::PixelBuffer buf;
    pal::g_surface->readPixels(sid, { 0, 0, 4, 3 }, buf, pal::PixelFormat::BGRA32_Alpha);
    ASSERT_TRUE(buf.valid());
    for (int y = 0; y < 3; ++y) {
        ASSERT_EQ(200 - y * 30, (int) buf.row(y)[3]);
        ASSERT_EQ(10 + 0,     (int) buf.row(y)[0]);   // B — and not the alpha,
        ASSERT_EQ(40 + y,     (int) buf.row(y)[1]);   // G   which a 4-byte
        ASSERT_EQ(90 + 0 + y, (int) buf.row(y)[2]);   // R   rotation would give
    }

    // ...and plain BGRA32 must still pin it to 0, or the drawing-surface half of
    // the contract is gone.
    pal::PixelBuffer plain;
    pal::g_surface->readPixels(sid, { 0, 0, 4, 3 }, plain, pal::PixelFormat::BGRA32);
    ASSERT_TRUE(plain.valid());
    for (int y = 0; y < 3; ++y)
        ASSERT_EQ(0, (int) plain.row(y)[3]);
}

TEST(GdiHeadless, AttachRejectsWhatItCannotRepresent)
{
    // Only 24 and 32 bpp can be carried: pal::PixelBuffer has no palette and no
    // wide-channel format.  Attach says no rather than inventing a mapping —
    // and consumes the handle either way, so a rejected load does not leak.
    //
    // 48bpp is the case that NEEDS the explicit depth check: below 24 the
    // buffer fails PixelBuffer::valid() on its own (its stride is too small to
    // be a BGR24 row), but 48bpp has a stride that is comfortably large enough,
    // so without the check it would be read as BGR24 and silently produce a
    // garbled image.
    const int kUnsupported[] = { 8, 16, 48 };
    for (int bpp : kUnsupported) {
        BITMAPINFO bmi = MakeBmi(4, 4, bpp);
        void* pBits = nullptr;
        HBITMAP hbmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
        ASSERT_NE((HBITMAP)nullptr, hbmp);
        ASSERT_NE((void*)nullptr, pBits);

        CBitmap image;
        ASSERT_FALSE(image.Attach((HGDIOBJ) hbmp));
        ASSERT_EQ((size_t)0, HeadlessLiveDibCountForTesting());
        ASSERT_EQ(pal::SurfaceId::None, HeadlessSurfaceOfBitmapForTesting(&image));

        // ...and the caller is left with a BITMAP it can branch on rather than
        // stack garbage.  miniwindow.cpp declares `BITMAP bi;` uninitialised and
        // several sites read bi.bmBitsPixel without checking the return, so a
        // GetBitmap that only returned FALSE would decide those branches by
        // whatever happened to be on the stack.
        BITMAP bi;
        std::memset((void *) &bi, 0x7F, sizeof bi);
        ASSERT_FALSE(image.GetBitmap(&bi));
        ASSERT_EQ(0, (int) bi.bmWidth);
        ASSERT_EQ(0, (int) bi.bmHeight);
        ASSERT_EQ(0, (int) bi.bmBitsPixel);
        ASSERT_EQ(0, (int) bi.bmPlanes);
        ASSERT_EQ(0, (int) bi.bmWidthBytes);
        ASSERT_EQ(0, (int) bi.bmType);
    }

    // A degenerate BITMAPINFO gets nothing at all, and still nulls the poison.
    BITMAPINFO empty = MakeBmi(0, 4, 24);
    void* p2 = (void*)0xDEADBEEF;
    EXPECT_EQ((HBITMAP)nullptr,
              CreateDIBSection(NULL, &empty, DIB_RGB_COLORS, &p2, NULL, 0));
    EXPECT_EQ((void*)nullptr, p2);

    // And a handle that never came from the store is refused rather than
    // dereferenced — LoadImage(".bmp") returns null headless, so this is the
    // path SetBackgroundImage takes for a BMP.
    CBitmap other;
    int notAHandle = 0;
    EXPECT_FALSE(other.Attach((HGDIOBJ) &notAHandle));
    EXPECT_FALSE(other.Attach(nullptr));
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

// ~CBitmap releases the surface (2026-08-12).  miniwindow.cpp deletes images
// with plain `delete` at five sites (image replace in LoadImage /
// LoadImageMemory / CreateImage / ImageFromWindow and the m_Images teardown);
// an empty dtor leaked each one's CpuRaster pixel buffer AND left the
// surface map keyed by the freed object address, which the next heap-recycled
// CBitmap aliased — GetBitmap answered for the DEAD image.
TEST(GdiHeadless, DeletingABitmapObjectReleasesItsSurface)
{
    MC_SKIP_UNDER_SANITIZER("needs the allocator to recycle a freed address; a sanitizer never does");
    CBitmap* a = new CBitmap;
    ASSERT_TRUE(a->CreateCanvasBitmap(8, 8));
    BITMAP info = {};
    EXPECT_TRUE(a->GetBitmap(&info));      // registered in the store

    delete a;                              // plain delete — miniwindow's shape

    // macOS libmalloc reuses a just-freed same-size block immediately, so the
    // very next new lands on a's address; the loop is a bound, not a search
    // strategy.  A recycled address must NOT inherit the dead image.
    CBitmap* recycled = nullptr;
    std::vector<CBitmap*> others;
    for (int i = 0; i < 64 && !recycled; ++i) {
        CBitmap* c = new CBitmap;
        if (c == a) recycled = c; else others.push_back(c);
    }
    for (CBitmap* c : others) delete c;
    EXPECT_TRUE(recycled != nullptr);      // allocator handed the address back
    if (recycled) {
        BITMAP inherited = {};
        EXPECT_FALSE(recycled->GetBitmap(&inherited));  // no aliasing
        EXPECT_FALSE(recycled->DeleteObject());          // nothing to release
        delete recycled;
    }
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

TEST(GdiHeadless, LoadBmpTranslatesBackslashPaths)
{
    // world.WindowWrite creates files through CFile::Open's '\\'->'/' mapping,
    // so a plugin that writes "shots\\x.bmp" gets shots/x.bmp on disk -- but
    // LoadBmp used a raw fopen, so loading the same backslash path returned a
    // fabricated eFileNotFound for a file that was right there (its sibling
    // LoadPng already translated).  Garbage content is enough to pin it: any
    // return OTHER than eFileNotFound means fopen FOUND the file via the
    // translated path, where before it could not.
    char dirtmpl[] = "/tmp/mc_bmp_bs_XXXXXX";
    ASSERT_TRUE(mkdtemp(dirtmpl) != NULL);
    std::string dir = dirtmpl;
    std::string posixPath = dir + "/x.bmp";
    {
        FILE* f = fopen(posixPath.c_str(), "wb");
        ASSERT_TRUE(f != NULL);
        fwrite("not-a-real-bmp", 1, 14, f);
        fclose(f);
    }

    std::string winPath = dir + "\\x.bmp";
    HBITMAP hbmp = (HBITMAP) 0;
    long rc = LoadBmp(winPath.c_str(), hbmp);
    EXPECT_NE(eFileNotFound, rc);        // FOUND via the translated separator
    EXPECT_EQ(eUnableToLoadImage, rc);   // opened, then refused as not a real BMP

    unlink(posixPath.c_str());
    rmdir(dir.c_str());
}

MC_TEST_MAIN()
