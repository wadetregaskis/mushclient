// tests/test_pal_surface.cpp
// Golden raster tests for pal::ISurface via the CPU reference adapter
// (pal/cpu_raster.cpp) — S1 of the ISurface migration (docs/PAL.md).
//
// These pin the CONTRACT, most critically the PixelBuffer DIB layout
// (BGR24/BGRA32, bottom-up rows, 4-byte-aligned stride) that the engine's
// existing blend/filter loops depend on byte-for-byte.

#include "mc_test.h"
#include "pal/cpu_raster.h"

using pal::Colour;
using pal::Point;
using pal::Rect;
using pal::Size;

static constexpr Colour RED    = 0x0000FF;   // COLORREF 0x00BBGGRR
static constexpr Colour GREEN  = 0x00FF00;
static constexpr Colour BLUE   = 0xFF0000;
static constexpr Colour YELLOW = 0x00FFFF;
static constexpr Colour WHITE  = 0xFFFFFF;
static constexpr Colour BLACK  = 0x000000;
static constexpr Colour INVALID = 0xFFFFFFFFu;

// Tests drive the INTERFACE, not the concrete adapter — both to exercise the
// contract as the engine will and because default arguments live on ISurface.
struct Fixture {
    pal::CpuRaster impl;
    pal::ISurface& ad = impl;
};

TEST(PalSurface, CreateFillsBackgroundAndBoundsChecks)
{
    Fixture fx;
    pal::ISurface& ad = fx.ad;
    pal::SurfaceId s = ad.createSurface({ 8, 6 }, BLUE);
    EXPECT_TRUE(ad.getPixel(s, { 0, 0 }) == BLUE);
    EXPECT_TRUE(ad.getPixel(s, { 7, 5 }) == BLUE);
    EXPECT_TRUE(ad.getPixel(s, { 8, 0 }) == INVALID);    // out of bounds → CLR_INVALID
    EXPECT_TRUE(ad.getPixel(s, { 0, -1 }) == INVALID);
    ad.destroySurface(s);
    EXPECT_TRUE(ad.getPixel(s, { 0, 0 }) == INVALID);    // destroyed surface
}

// THE critical contract test: readPixels emits exact Windows-DIB bytes —
// B,G,R order, bottom-up rows, stride padded to 4 bytes.
TEST(PalSurface, ReadPixelsIsExactDibLayout)
{
    Fixture fx;
    pal::ISurface& ad = fx.ad;
    pal::SurfaceId s = ad.createSurface({ 2, 2 }, BLACK);
    ad.fillRect(s, { 0, 0, 1, 1 }, { pal::BrushStyle::Solid, RED });     // TL
    ad.fillRect(s, { 1, 0, 1, 1 }, { pal::BrushStyle::Solid, GREEN });   // TR
    ad.fillRect(s, { 0, 1, 1, 1 }, { pal::BrushStyle::Solid, BLUE });    // BL
    ad.fillRect(s, { 1, 1, 1, 1 }, { pal::BrushStyle::Solid, YELLOW });  // BR

    pal::PixelBuffer out;
    ad.readPixels(s, { 0, 0, 2, 2 }, out, pal::PixelFormat::BGR24);
    ASSERT_EQ(8, out.stride);                    // (2*3+3) & ~3 — 2 pad bytes per row
    ASSERT_EQ(16u, (unsigned)out.bytes.size());
    const unsigned char* b = out.bytes.data();
    // Buffer row 0 = image BOTTOM row (bottom-up): BL blue, BR yellow.
    EXPECT_TRUE(b[0] == 0xFF && b[1] == 0x00 && b[2] == 0x00);   // blue  = B,G,R
    EXPECT_TRUE(b[3] == 0x00 && b[4] == 0xFF && b[5] == 0xFF);   // yellow
    // Buffer row 1 = image TOP row: TL red, TR green.
    EXPECT_TRUE(b[8]  == 0x00 && b[9]  == 0x00 && b[10] == 0xFF); // red
    EXPECT_TRUE(b[11] == 0x00 && b[12] == 0xFF && b[13] == 0x00); // green
    // row() helper agrees with the layout.
    EXPECT_TRUE(out.row(0) == b + 8 && out.row(1) == b + 0);
}

TEST(PalSurface, WritePixelsRoundTripsBothFormats)
{
    Fixture fx;
    pal::ISurface& ad = fx.ad;
    pal::SurfaceId a = ad.createSurface({ 5, 4 }, BLACK);
    ad.gradient(a, { 0, 0, 5, 4 }, RED, BLUE, true);

    for (auto fmt : { pal::PixelFormat::BGR24, pal::PixelFormat::BGRA32 }) {
        pal::PixelBuffer buf;
        ad.readPixels(a, { 0, 0, 5, 4 }, buf, fmt);
        pal::SurfaceId c = ad.createSurface({ 5, 4 }, WHITE);
        ad.writePixels(c, { 0, 0, 5, 4 }, buf);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 5; ++x)
                ASSERT_TRUE(ad.getPixel(c, { x, y }) == ad.getPixel(a, { x, y }));
        ad.destroySurface(c);
    }
}

TEST(PalSurface, InvertRectIsExactNot)
{
    Fixture fx;
    pal::ISurface& ad = fx.ad;
    pal::SurfaceId s = ad.createSurface({ 4, 4 }, RED);
    ad.invertRect(s, { 1, 1, 2, 2 });
    EXPECT_TRUE(ad.getPixel(s, { 1, 1 }) == 0xFFFF00);   // ~red = cyan (B,G on)
    EXPECT_TRUE(ad.getPixel(s, { 0, 0 }) == RED);        // outside untouched
    ad.invertRect(s, { 1, 1, 2, 2 });                    // invert twice = identity
    EXPECT_TRUE(ad.getPixel(s, { 1, 1 }) == RED);
}

TEST(PalSurface, LineExcludesLastPixel)
{
    Fixture fx;
    pal::ISurface& ad = fx.ad;
    pal::SurfaceId s = ad.createSurface({ 10, 10 }, BLACK);
    ad.line(s, { 2, 5 }, { 7, 5 }, { pal::PenStyle::Solid, 1, WHITE });
    for (int x = 2; x <= 6; ++x)
        EXPECT_TRUE(ad.getPixel(s, { x, 5 }) == WHITE);
    EXPECT_TRUE(ad.getPixel(s, { 7, 5 }) == BLACK);      // GDI last-pixel rule
    EXPECT_TRUE(ad.getPixel(s, { 1, 5 }) == BLACK);
}

TEST(PalSurface, ShapeRectStrokesAndFills)
{
    Fixture fx;
    pal::ISurface& ad = fx.ad;
    pal::SurfaceId s = ad.createSurface({ 10, 10 }, BLACK);
    ad.shape(s, pal::ShapeKind::Rect, { 2, 2, 6, 5 }, {}, {},
             { pal::PenStyle::Solid, 1, WHITE }, { pal::BrushStyle::Solid, GREEN });
    EXPECT_TRUE(ad.getPixel(s, { 2, 2 }) == WHITE);      // border corners (exclusive r/b)
    EXPECT_TRUE(ad.getPixel(s, { 7, 6 }) == WHITE);
    EXPECT_TRUE(ad.getPixel(s, { 8, 2 }) == BLACK);      // just outside
    EXPECT_TRUE(ad.getPixel(s, { 4, 4 }) == GREEN);      // interior
    // Null pen: GDI Rectangle() draws the figure one pixel smaller on the
    // right/bottom (the documented null-pen shrink) — (w-1) x (h-1) top-left.
    ad.shape(s, pal::ShapeKind::Rect, { 0, 8, 3, 2 }, {}, {},
             { pal::PenStyle::Null }, { pal::BrushStyle::Solid, RED });
    EXPECT_TRUE(ad.getPixel(s, { 0, 8 }) == RED);
    EXPECT_TRUE(ad.getPixel(s, { 1, 8 }) == RED);
    EXPECT_TRUE(ad.getPixel(s, { 2, 8 }) == BLACK);      // shrunk right column
    EXPECT_TRUE(ad.getPixel(s, { 0, 9 }) == BLACK);      // shrunk bottom row
    EXPECT_TRUE(ad.getPixel(s, { 2, 9 }) == BLACK);
}

TEST(PalSurface, BlitCopyAndExactColourKey)
{
    Fixture fx;
    pal::ISurface& ad = fx.ad;
    pal::SurfaceId src = ad.createSurface({ 4, 4 }, GREEN);
    ad.fillRect(src, { 1, 1, 2, 2 }, { pal::BrushStyle::Solid, RED });
    pal::SurfaceId dst = ad.createSurface({ 8, 8 }, BLACK);

    // Plain copy of the whole source into (2,2).
    ad.blit(dst, { 2, 2, 4, 4 }, src, { 0, 0, 4, 4 }, pal::BlitMode::Copy);
    EXPECT_TRUE(ad.getPixel(dst, { 2, 2 }) == GREEN);
    EXPECT_TRUE(ad.getPixel(dst, { 3, 3 }) == RED);
    EXPECT_TRUE(ad.getPixel(dst, { 1, 1 }) == BLACK);    // outside dstRect untouched

    // Colour-key: GREEN is transparent — only the red block lands.
    pal::SurfaceId dst2 = ad.createSurface({ 8, 8 }, BLUE);
    ad.blit(dst2, { 0, 0, 4, 4 }, src, { 0, 0, 4, 4 },
            pal::BlitMode::TransparentKey, GREEN);
    EXPECT_TRUE(ad.getPixel(dst2, { 0, 0 }) == BLUE);    // keyed pixel skipped
    EXPECT_TRUE(ad.getPixel(dst2, { 1, 1 }) == RED);     // opaque pixel copied
}

// A PixelBuffer whose declared geometry is not backed by its bytes must be
// rejected, not read out of bounds (writePixels / blit / pattern brush).
TEST(PalSurface, HostilePixelBufferRejected)
{
    Fixture fx;
    pal::ISurface& ad = fx.ad;
    pal::SurfaceId s = ad.createSurface({ 4, 4 }, GREEN);

    pal::PixelBuffer evil;
    evil.format = pal::PixelFormat::BGR24;
    evil.size   = { 100, 100 };
    evil.stride = 400;
    evil.bytes.assign(4, 0x7F);                          // 4 bytes backing "100x100"
    ad.writePixels(s, { 0, 0, 4, 4 }, evil);
    ad.blit(s, { 0, 0, 4, 4 }, evil, { 0, 0, 4, 4 }, pal::BlitMode::Copy);
    pal::BrushSpec pb; pb.style = pal::BrushStyle::Pattern; pb.pattern = &evil;
    ad.fillRect(s, { 0, 0, 4, 4 }, pb);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            ASSERT_TRUE(ad.getPixel(s, { x, y }) == GREEN);   // untouched

    // Understated stride is equally invalid (row 0 would overrun the end).
    evil.size = { 2, 2 }; evil.stride = 2; evil.bytes.assign(4, 0x7F);
    ad.writePixels(s, { 0, 0, 2, 2 }, evil);
    EXPECT_TRUE(ad.getPixel(s, { 0, 0 }) == GREEN);
}

// A srcRect partially outside the source surface blits the in-bounds
// intersection at the correspondingly shifted destination (BitBlt-style),
// identically for surface and PixelBuffer sources.
TEST(PalSurface, PartialOverlapBlitDrawsIntersection)
{
    Fixture fx;
    pal::ISurface& ad = fx.ad;
    pal::SurfaceId src = ad.createSurface({ 4, 4 }, RED);
    pal::SurfaceId dst = ad.createSurface({ 8, 8 }, BLACK);

    // srcRect {2,2,4,4} only overlaps the 2x2 bottom-right of src; those pixels
    // must land at dstRect origin (no shift needed: clip starts at src offset 0).
    ad.blit(dst, { 0, 0, 4, 4 }, src, { 2, 2, 4, 4 }, pal::BlitMode::Copy);
    EXPECT_TRUE(ad.getPixel(dst, { 0, 0 }) == RED);      // intersection drawn
    EXPECT_TRUE(ad.getPixel(dst, { 1, 1 }) == RED);
    EXPECT_TRUE(ad.getPixel(dst, { 2, 2 }) == BLACK);    // beyond the source
    EXPECT_TRUE(ad.getPixel(dst, { 3, 3 }) == BLACK);

    // Negative-origin srcRect: the clipped-away margin shifts the destination.
    pal::SurfaceId dst2 = ad.createSurface({ 8, 8 }, BLACK);
    ad.blit(dst2, { 0, 0, 4, 4 }, src, { -2, -2, 4, 4 }, pal::BlitMode::Copy);
    EXPECT_TRUE(ad.getPixel(dst2, { 0, 0 }) == BLACK);   // clipped margin
    EXPECT_TRUE(ad.getPixel(dst2, { 1, 1 }) == BLACK);
    EXPECT_TRUE(ad.getPixel(dst2, { 2, 2 }) == RED);     // shifted intersection
    EXPECT_TRUE(ad.getPixel(dst2, { 3, 3 }) == RED);
}

TEST(PalSurface, GradientEndpointsExact)
{
    Fixture fx;
    pal::ISurface& ad = fx.ad;
    pal::SurfaceId s = ad.createSurface({ 10, 3 }, BLACK);
    ad.gradient(s, { 0, 0, 10, 3 }, BLACK, WHITE, true);
    EXPECT_TRUE(ad.getPixel(s, { 0, 1 }) == BLACK);      // left column = from
    EXPECT_TRUE(ad.getPixel(s, { 9, 1 }) == WHITE);      // right column = to
    const Colour mid = ad.getPixel(s, { 5, 1 });         // monotonic in between
    EXPECT_TRUE((mid & 0xFF) > 0x00 && (mid & 0xFF) < 0xFF);
}

TEST(PalSurface, FloodFillBorderAndSurfaceModes)
{
    Fixture fx;
    pal::ISurface& ad = fx.ad;
    pal::SurfaceId s = ad.createSurface({ 10, 10 }, BLACK);
    // A white 6x6 border box; border mode fills its inside only.
    ad.shape(s, pal::ShapeKind::Rect, { 2, 2, 6, 6 }, {}, {},
             { pal::PenStyle::Solid, 1, WHITE }, { pal::BrushStyle::Null });
    ad.floodFill(s, { 4, 4 }, RED, WHITE, pal::FloodMode::Border);
    EXPECT_TRUE(ad.getPixel(s, { 4, 4 }) == RED);        // inside filled
    EXPECT_TRUE(ad.getPixel(s, { 3, 3 }) == RED);
    EXPECT_TRUE(ad.getPixel(s, { 2, 2 }) == WHITE);      // border intact
    EXPECT_TRUE(ad.getPixel(s, { 0, 0 }) == BLACK);      // outside not reached
    // Surface mode: replace the connected BLACK region outside the box.
    ad.floodFill(s, { 0, 0 }, GREEN, BLACK, pal::FloodMode::Surface);
    EXPECT_TRUE(ad.getPixel(s, { 0, 0 }) == GREEN);
    EXPECT_TRUE(ad.getPixel(s, { 9, 9 }) == GREEN);
    EXPECT_TRUE(ad.getPixel(s, { 4, 4 }) == RED);        // inside box untouched
}

TEST(PalSurface, TextContractRoundTrips)
{
    Fixture fx;
    pal::ISurface& ad = fx.ad;
    pal::SurfaceId s = ad.createSurface({ 60, 20 }, BLACK);
    pal::FontId f = ad.createFont({ "AnyFace", 10, 400, false, false, false, 0, 0 });

    // Metrics are self-consistent and real for what the adapter draws.
    pal::FontMetrics m = ad.fontMetrics(f);
    EXPECT_TRUE(m.height == 10);
    EXPECT_TRUE(m.ascent + m.descent == m.height);
    EXPECT_TRUE(m.aveCharWidth == 6 && m.maxCharWidth == 6);   // 3/5 of height
    EXPECT_STREQ("CpuRasterTest", m.face.c_str());

    // measure/draw agree; return value is the measured width.
    Size sz = ad.measureText(f, "Dog");
    EXPECT_TRUE(sz.w == 18 && sz.h == 10);
    EXPECT_TRUE(ad.drawText(s, f, "Dog", { 2, 2 }, WHITE, nullptr) == 18);

    // Something was actually drawn inside the first cell ('D' hits (0,0)).
    EXPECT_TRUE(ad.getPixel(s, { 2, 2 }) == WHITE);

    // Clipping truncates the return width (WindowText's min(measured, clip.w))
    // and suppresses pixels outside the clip rect.
    pal::SurfaceId s2 = ad.createSurface({ 60, 20 }, BLACK);
    Rect clip = { 0, 0, 7, 20 };
    EXPECT_TRUE(ad.drawText(s2, f, "Dog", { 0, 0 }, WHITE, &clip) == 7);
    bool outsideClean = true;
    for (int y = 0; y < 20 && outsideClean; ++y)
        for (int x = 7; x < 60; ++x)
            if (ad.getPixel(s2, { x, y }) != BLACK) { outsideClean = false; break; }
    EXPECT_TRUE(outsideClean);

    // UTF-8: multi-byte codepoints count once.
    EXPECT_TRUE(ad.measureText(f, "\xC3\xA9X").w == 12);   // é + X = 2 cells
}

TEST(PalSurface, HatchAndPatternBrushes)
{
    Fixture fx;
    pal::ISurface& ad = fx.ad;
    pal::SurfaceId s = ad.createSurface({ 16, 16 }, BLACK);
    // Horizontal hatch: line rows get fg, gap rows get bg.
    ad.fillRect(s, { 0, 0, 16, 16 },
                { pal::BrushStyle::Hatch, WHITE, BLUE, pal::HatchKind::Horizontal });
    EXPECT_TRUE(ad.getPixel(s, { 3, 0 }) == WHITE);      // y%8==0 → hatch line
    EXPECT_TRUE(ad.getPixel(s, { 3, 8 }) == WHITE);
    EXPECT_TRUE(ad.getPixel(s, { 3, 4 }) == BLUE);       // gap
    // Transparent gaps: bg == -1 leaves the underlying pixels alone.
    ad.fillRect(s, { 0, 0, 16, 16 }, { pal::BrushStyle::Solid, GREEN });
    ad.fillRect(s, { 0, 0, 16, 16 },
                { pal::BrushStyle::Hatch, WHITE, (Colour)-1, pal::HatchKind::Horizontal });
    EXPECT_TRUE(ad.getPixel(s, { 3, 0 }) == WHITE);
    EXPECT_TRUE(ad.getPixel(s, { 3, 4 }) == GREEN);      // gap untouched

    // Pattern brush: 2x1 red/yellow tile, aligned to the SURFACE origin.
    pal::PixelBuffer pat;
    pat.format = pal::PixelFormat::BGR24;
    pat.size   = { 2, 1 };
    pat.stride = pal::PixelBuffer::strideFor(2, pat.format);
    pat.bytes.assign((size_t)pat.stride, 0);
    unsigned char* r0 = pat.row(0);
    r0[0] = 0x00; r0[1] = 0x00; r0[2] = 0xFF;            // (0,0) red  (B,G,R)
    r0[3] = 0x00; r0[4] = 0xFF; r0[5] = 0xFF;            // (1,0) yellow
    pal::BrushSpec pb; pb.style = pal::BrushStyle::Pattern; pb.pattern = &pat;
    ad.fillRect(s, { 1, 1, 4, 2 }, pb);                  // offset rect: origin-aligned tiling
    EXPECT_TRUE(ad.getPixel(s, { 2, 1 }) == RED);        // even x → red
    EXPECT_TRUE(ad.getPixel(s, { 3, 1 }) == YELLOW);     // odd x → yellow
    EXPECT_TRUE(ad.getPixel(s, { 1, 1 }) == YELLOW);     // x=1 odd → yellow
}

MC_TEST_MAIN()
