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

// A unique colour per pixel of a 4x4 source, so a mis-sampled pixel is
// identifiable rather than merely "some colour landed".  R and G stay in
// 0x20..0xB0, so no tint collides with BLACK/WHITE/RED/GREEN/BLUE/YELLOW.
static Colour tint4(int x, int y)
{
    return (Colour)(0x20 + x * 0x30) | ((Colour)(0x20 + y * 0x30) << 8);
}
static pal::SurfaceId makeTinted4x4(pal::ISurface& ad)
{
    pal::SurfaceId s = ad.createSurface({ 4, 4 }, BLACK);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            ad.setPixel(s, { x, y }, tint4(x, y));
    return s;
}

// Tests drive the INTERFACE, not the concrete adapter — both to exercise the
// contract as the engine will and because default arguments live on ISurface.
struct Fixture {
    pal::CpuRaster impl;
    pal::ISurface& ad = impl;
};

// Audit §4: every test body opened with the identical two lines
//   Fixture fx; pal::ISurface& ad = fx.ad;
// Collapse that boilerplate into one macro that declares both `fx` and `ad`.
#define PAL_FIXTURE() Fixture fx; pal::ISurface& ad = fx.ad

TEST(PalSurface, CreateFillsBackgroundAndBoundsChecks)
{
    PAL_FIXTURE();
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
    PAL_FIXTURE();
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
    PAL_FIXTURE();
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
    PAL_FIXTURE();
    pal::SurfaceId s = ad.createSurface({ 4, 4 }, RED);
    ad.invertRect(s, { 1, 1, 2, 2 });
    EXPECT_TRUE(ad.getPixel(s, { 1, 1 }) == 0xFFFF00);   // ~red = cyan (B,G on)
    EXPECT_TRUE(ad.getPixel(s, { 0, 0 }) == RED);        // outside untouched
    ad.invertRect(s, { 1, 1, 2, 2 });                    // invert twice = identity
    EXPECT_TRUE(ad.getPixel(s, { 1, 1 }) == RED);
}

TEST(PalSurface, LineExcludesLastPixel)
{
    PAL_FIXTURE();
    pal::SurfaceId s = ad.createSurface({ 10, 10 }, BLACK);
    ad.line(s, { 2, 5 }, { 7, 5 }, { pal::PenStyle::Solid, 1, WHITE });
    for (int x = 2; x <= 6; ++x)
        EXPECT_TRUE(ad.getPixel(s, { x, 5 }) == WHITE);
    EXPECT_TRUE(ad.getPixel(s, { 7, 5 }) == BLACK);      // GDI last-pixel rule
    EXPECT_TRUE(ad.getPixel(s, { 1, 5 }) == BLACK);
}

TEST(PalSurface, ShapeRectStrokesAndFills)
{
    PAL_FIXTURE();
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
    PAL_FIXTURE();
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
    PAL_FIXTURE();
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
    PAL_FIXTURE();
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
    PAL_FIXTURE();
    pal::SurfaceId s = ad.createSurface({ 10, 3 }, BLACK);
    ad.gradient(s, { 0, 0, 10, 3 }, BLACK, WHITE, true);
    EXPECT_TRUE(ad.getPixel(s, { 0, 1 }) == BLACK);      // left column = from
    EXPECT_TRUE(ad.getPixel(s, { 9, 1 }) == WHITE);      // right column = to
    const Colour mid = ad.getPixel(s, { 5, 1 });         // monotonic in between
    EXPECT_TRUE((mid & 0xFF) > 0x00 && (mid & 0xFF) < 0xFF);
}

TEST(PalSurface, FloodFillBorderAndSurfaceModes)
{
    PAL_FIXTURE();
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
    PAL_FIXTURE();
    pal::SurfaceId s = ad.createSurface({ 60, 20 }, BLACK);
    pal::FontId f = ad.createFont({ "AnyFace", 10, 400, false, false, false, 0, 0 });

    // Metrics are self-consistent and real for what the adapter draws.
    pal::FontMetrics m = ad.fontMetrics(f);
    EXPECT_TRUE(m.height == 10);
    EXPECT_TRUE(m.ascent + m.descent == m.height);
    EXPECT_TRUE(m.aveCharWidth == 6 && m.maxCharWidth == 6);   // 3/5 of height
    EXPECT_STREQ("CpuRasterTest", m.face.c_str());

    // measure/draw agree; return value is the measured width.
    Size sz = ad.measureText(f, "Dog", 3);
    EXPECT_TRUE(sz.w == 18 && sz.h == 10);
    EXPECT_TRUE(ad.drawText(s, f, "Dog", 3, { 2, 2 }, WHITE, nullptr) == 18);

    // Something was actually drawn inside the first cell ('D' hits (0,0)).
    EXPECT_TRUE(ad.getPixel(s, { 2, 2 }) == WHITE);

    // Clipping truncates the return width (WindowText's min(measured, clip.w))
    // and suppresses pixels outside the clip rect.
    pal::SurfaceId s2 = ad.createSurface({ 60, 20 }, BLACK);
    Rect clip = { 0, 0, 7, 20 };
    EXPECT_TRUE(ad.drawText(s2, f, "Dog", 3, { 0, 0 }, WHITE, &clip) == 7);
    bool outsideClean = true;
    for (int y = 0; y < 20 && outsideClean; ++y)
        for (int x = 7; x < 60; ++x)
            if (ad.getPixel(s2, { x, y }) != BLACK) { outsideClean = false; break; }
    EXPECT_TRUE(outsideClean);

    // UTF-8: multi-byte codepoints count once.
    EXPECT_TRUE(ad.measureText(f, "\xC3\xA9X", 3).w == 12);   // é + X = 2 cells
}

TEST(PalSurface, HatchAndPatternBrushes)
{
    PAL_FIXTURE();
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


TEST(PalSurface, SetPixelRoundTripAndOobNoop)
{
    PAL_FIXTURE();
    pal::SurfaceId s = ad.createSurface({ 8, 6 }, BLACK);
    ad.setPixel(s, { 3, 2 }, RED);                          // WindowSetPixel path
    EXPECT_TRUE(ad.getPixel(s, { 3, 2 }) == RED);           // exact round-trip
    EXPECT_TRUE(ad.getPixel(s, { 2, 2 }) == BLACK);         // neighbour untouched
    // Out-of-bounds writes are rejected (line 114) — no corruption, no crash.
    ad.setPixel(s, { 8, 0 }, WHITE);                        // x == w
    ad.setPixel(s, { 0, 6 }, WHITE);                        // y == h
    ad.setPixel(s, { -1, 2 }, WHITE);                       // x < 0
    EXPECT_TRUE(ad.getPixel(s, { 3, 2 }) == RED);           // still the only set pixel
    EXPECT_TRUE(ad.getPixel(s, { 0, 0 }) == BLACK);
    EXPECT_TRUE(ad.getPixel(s, { 7, 5 }) == BLACK);
}

TEST(PalSurface, HatchVerticalBitFormula)
{
    PAL_FIXTURE();
    pal::SurfaceId s = ad.createSurface({ 16, 16 }, BLACK);
    ad.fillRect(s, { 0, 0, 16, 16 },
                { pal::BrushStyle::Hatch, WHITE, BLUE, pal::HatchKind::Vertical });
    EXPECT_TRUE(ad.getPixel(s, { 0, 3 }) == WHITE);        // x=0  → (x&7)==0 line
    EXPECT_TRUE(ad.getPixel(s, { 8, 3 }) == WHITE);        // x=8  → (x&7)==0 line
    EXPECT_TRUE(ad.getPixel(s, { 3, 3 }) == BLUE);         // x=3  → gap
    EXPECT_TRUE(ad.getPixel(s, { 0, 9 }) == WHITE);        // column, not row: y-agnostic
}

TEST(PalSurface, HatchFDiagonalBitFormula)
{
    PAL_FIXTURE();
    pal::SurfaceId s = ad.createSurface({ 16, 16 }, BLACK);
    ad.fillRect(s, { 0, 0, 16, 16 },
                { pal::BrushStyle::Hatch, WHITE, BLUE, pal::HatchKind::FDiagonal });
    EXPECT_TRUE(ad.getPixel(s, { 0, 0 }) == WHITE);        // x-y=0
    EXPECT_TRUE(ad.getPixel(s, { 5, 5 }) == WHITE);        // x-y=0
    EXPECT_TRUE(ad.getPixel(s, { 8, 0 }) == WHITE);        // x-y=8 → (8&7)==0
    EXPECT_TRUE(ad.getPixel(s, { 6, 5 }) == BLUE);         // x-y=1 → gap
}

TEST(PalSurface, HatchBDiagonalBitFormula)
{
    PAL_FIXTURE();
    pal::SurfaceId s = ad.createSurface({ 16, 16 }, BLACK);
    ad.fillRect(s, { 0, 0, 16, 16 },
                { pal::BrushStyle::Hatch, WHITE, BLUE, pal::HatchKind::BDiagonal });
    EXPECT_TRUE(ad.getPixel(s, { 0, 0 }) == WHITE);        // x+y=0
    EXPECT_TRUE(ad.getPixel(s, { 3, 5 }) == WHITE);        // x+y=8 → (8&7)==0
    EXPECT_TRUE(ad.getPixel(s, { 5, 3 }) == WHITE);        // x+y=8
    EXPECT_TRUE(ad.getPixel(s, { 4, 5 }) == BLUE);         // x+y=9 → gap
}

TEST(PalSurface, HatchCrossBitFormula)
{
    PAL_FIXTURE();
    pal::SurfaceId s = ad.createSurface({ 16, 16 }, BLACK);
    ad.fillRect(s, { 0, 0, 16, 16 },
                { pal::BrushStyle::Hatch, WHITE, BLUE, pal::HatchKind::Cross });
    EXPECT_TRUE(ad.getPixel(s, { 0, 3 }) == WHITE);        // x=0 → column line
    EXPECT_TRUE(ad.getPixel(s, { 3, 0 }) == WHITE);        // y=0 → row line
    EXPECT_TRUE(ad.getPixel(s, { 3, 3 }) == BLUE);         // neither → gap
    EXPECT_TRUE(ad.getPixel(s, { 5, 5 }) == BLUE);         // neither → gap
}

TEST(PalSurface, HatchDiagCrossBitFormula)
{
    PAL_FIXTURE();
    pal::SurfaceId s = ad.createSurface({ 16, 16 }, BLACK);
    ad.fillRect(s, { 0, 0, 16, 16 },
                { pal::BrushStyle::Hatch, WHITE, BLUE, pal::HatchKind::DiagCross });
    EXPECT_TRUE(ad.getPixel(s, { 5, 5 }) == WHITE);        // x-y=0 → '\\' diagonal
    EXPECT_TRUE(ad.getPixel(s, { 3, 5 }) == WHITE);        // x+y=8 → '/' diagonal
    EXPECT_TRUE(ad.getPixel(s, { 1, 0 }) == BLUE);         // x-y=1, x+y=1 → gap
    EXPECT_TRUE(ad.getPixel(s, { 2, 3 }) == BLUE);         // x-y=-1, x+y=5 → gap
}

TEST(PalSurface, GradientVerticalInterpolates)
{
    PAL_FIXTURE();
    pal::SurfaceId s = ad.createSurface({ 3, 10 }, BLACK);
    ad.gradient(s, { 0, 0, 3, 10 }, BLACK, WHITE, false);  // vertical branch
    // Rows are uniform across x; distinguishes from a horizontal gradient,
    // where the left column (x=0) would stay 'from' for every row.
    EXPECT_TRUE(ad.getPixel(s, { 0, 0 }) == BLACK);        // top row = from
    EXPECT_TRUE(ad.getPixel(s, { 2, 0 }) == BLACK);
    EXPECT_TRUE(ad.getPixel(s, { 0, 9 }) == WHITE);        // bottom row = to
    EXPECT_TRUE(ad.getPixel(s, { 2, 9 }) == WHITE);
    // Midrow y=5: t=5/9 → (unsigned char)(255*5/9+0.5)=142 → 0x8E8E8E grey.
    EXPECT_TRUE(ad.getPixel(s, { 1, 5 }) == 0x8E8E8E);
}

TEST(PalSurface, TransformBlitDegenerateDetIsNoop)
{
    PAL_FIXTURE();
    pal::SurfaceId src = ad.createSurface({ 2, 2 }, RED);  // src(0,0) = RED
    pal::SurfaceId dst = ad.createSurface({ 8, 8 }, BLACK);
    // Zero linear part (det = 0*0 - 0*0 = 0) + translate (3,3). Its dest
    // bounding box is the 2x2 block at (2,2)..(3,3); with the det==0 guard the
    // blit must draw NOTHING there. (If the guard were removed the block fills
    // with src(0,0)=RED, since every inverse-map numerator is 0 → sx=sy=0.)
    pal::Matrix2x3 m{ 0, 0, 0, 0, 3, 3 };
    ad.blit(dst, { 0, 0, 2, 2 }, src, { 0, 0, 2, 2 },
            pal::BlitMode::Copy, 0, pal::BlitQuality::Fast, &m);
    EXPECT_TRUE(ad.getPixel(dst, { 2, 2 }) == BLACK);      // bbox pixel, not written
    EXPECT_TRUE(ad.getPixel(dst, { 3, 3 }) == BLACK);
    EXPECT_TRUE(ad.getPixel(dst, { 0, 0 }) == BLACK);
}

TEST(PalSurface, TransformBlitScalesAndSamples)
{
    PAL_FIXTURE();
    pal::SurfaceId src = ad.createSurface({ 2, 2 }, BLACK);
    ad.setPixel(src, { 0, 0 }, RED);
    ad.setPixel(src, { 1, 0 }, GREEN);
    ad.setPixel(src, { 0, 1 }, BLUE);
    ad.setPixel(src, { 1, 1 }, YELLOW);
    pal::SurfaceId dst = ad.createSurface({ 8, 8 }, BLACK);
    // 2x horizontal stretch (x scaled by 2, y unchanged): device = M*src.
    // Inverse map sx=floor((X+0.5)/2), sy=floor(Y+0.5)=Y.
    pal::Matrix2x3 m{ 2, 0, 0, 1, 0, 0 };                  // det = 2*1 - 0*0 = 2
    ad.blit(dst, { 0, 0, 2, 2 }, src, { 0, 0, 2, 2 },
            pal::BlitMode::Copy, 0, pal::BlitQuality::Fast, &m);
    EXPECT_TRUE(ad.getPixel(dst, { 0, 0 }) == RED);        // sx=0,sy=0
    EXPECT_TRUE(ad.getPixel(dst, { 1, 0 }) == RED);        // sx=0,sy=0
    EXPECT_TRUE(ad.getPixel(dst, { 2, 0 }) == GREEN);      // sx=1,sy=0
    EXPECT_TRUE(ad.getPixel(dst, { 3, 0 }) == GREEN);      // sx=1,sy=0
    EXPECT_TRUE(ad.getPixel(dst, { 0, 1 }) == BLUE);       // sx=0,sy=1
    EXPECT_TRUE(ad.getPixel(dst, { 3, 1 }) == YELLOW);     // sx=1,sy=1
    EXPECT_TRUE(ad.getPixel(dst, { 4, 0 }) == BLACK);      // sx=2 → out of source
    EXPECT_TRUE(ad.getPixel(dst, { 0, 2 }) == BLACK);      // sy=2 → out of source
}

// The matrix path (cpu_raster.cpp:315-354) is the reference behind
// world.WindowTransformImage. Two things every other transform test builds on:
// the linear part applies to SOURCE-RELATIVE coordinates, and the translation
// (m->dx/dy) feeds BOTH the destination bounding box (:329-330) and the inverse
// map (:343). dstRect is ignored outright (:316-317), so both blits below hand
// in a dstRect pointing somewhere else entirely -- if the matrix branch were
// ever bypassed, the 1:1 path (:356-370) would honour that dstRect and every
// assertion here would move.
TEST(PalSurface, TransformBlitIdentityAndTranslation)
{
    PAL_FIXTURE();
    pal::SurfaceId src = ad.createSurface({ 2, 2 }, BLACK);
    ad.setPixel(src, { 0, 0 }, RED);    ad.setPixel(src, { 1, 0 }, GREEN);
    ad.setPixel(src, { 0, 1 }, BLUE);   ad.setPixel(src, { 1, 1 }, YELLOW);

    // Identity: det = 1, u = X+0.5, v = Y+0.5 -> sx = X, sy = Y, i.e. a
    // pixel-exact copy at the ORIGIN, not at the dstRect we pass.
    pal::SurfaceId dst = ad.createSurface({ 8, 8 }, BLACK);
    pal::Matrix2x3 identity;                              // {1,0,0,1,0,0}
    ad.blit(dst, { 4, 4, 2, 2 }, src, { 0, 0, 2, 2 },
            pal::BlitMode::Copy, 0, pal::BlitQuality::Fast, &identity);
    EXPECT_TRUE(ad.getPixel(dst, { 0, 0 }) == RED);
    EXPECT_TRUE(ad.getPixel(dst, { 1, 0 }) == GREEN);
    EXPECT_TRUE(ad.getPixel(dst, { 0, 1 }) == BLUE);
    EXPECT_TRUE(ad.getPixel(dst, { 1, 1 }) == YELLOW);
    // The bbox scan is x,y in [0,3), so these two ARE visited and are rejected
    // by the source clamp (:346) at sx/sy == 2 == srcRect.w/h.
    EXPECT_TRUE(ad.getPixel(dst, { 2, 0 }) == BLACK);
    EXPECT_TRUE(ad.getPixel(dst, { 0, 2 }) == BLACK);
    // dstRect really is ignored: bypass the matrix branch and the 1:1 path
    // (sizes match, 2x2 == 2x2) puts the block HERE and nothing at the origin.
    EXPECT_TRUE(ad.getPixel(dst, { 4, 4 }) == BLACK);

    // Pure translation (3,2): sx = floor(X+0.5-3) = X-3, sy = Y-2. The block
    // moves whole -- nothing is left behind at the origin. These four are what
    // catch a sampler that loses `- m->dx` / `- m->dy` (:343): the bbox at
    // :329-330/:337-340 still clamps to X in [2,6), so every sample then maps
    // to sx >= 2 == srcRect.w and the blit draws NOTHING AT ALL.
    pal::SurfaceId dst2 = ad.createSurface({ 8, 8 }, BLACK);
    pal::Matrix2x3 t{ 1, 0, 0, 1, 3, 2 };
    ad.blit(dst2, { 5, 5, 2, 2 }, src, { 0, 0, 2, 2 },
            pal::BlitMode::Copy, 0, pal::BlitQuality::Fast, &t);
    EXPECT_TRUE(ad.getPixel(dst2, { 3, 2 }) == RED);
    EXPECT_TRUE(ad.getPixel(dst2, { 4, 2 }) == GREEN);
    EXPECT_TRUE(ad.getPixel(dst2, { 3, 3 }) == BLUE);
    EXPECT_TRUE(ad.getPixel(dst2, { 4, 3 }) == YELLOW);
    // Here the scan covers x in [2,6), y in [1,5): all four of these are
    // visited and rejected, so they pin the block's exact extent rather than
    // merely sitting outside the loop.
    EXPECT_TRUE(ad.getPixel(dst2, { 2, 2 }) == BLACK);
    EXPECT_TRUE(ad.getPixel(dst2, { 5, 2 }) == BLACK);
    EXPECT_TRUE(ad.getPixel(dst2, { 3, 1 }) == BLACK);
    EXPECT_TRUE(ad.getPixel(dst2, { 3, 4 }) == BLACK);
    // Fires only if the translation is lost from BOTH the bbox (:329-330) and
    // the sampler (:343) -- i.e. the matrix degenerating to its linear part.
    EXPECT_TRUE(ad.getPixel(dst2, { 0, 0 }) == BLACK);
    EXPECT_TRUE(ad.getPixel(dst2, { 5, 5 }) == BLACK);    // dstRect ignored
}

// The ONLY test that can catch an m12/m21 mix-up. Every other transform test
// uses a diagonal matrix, where both off-diagonal coefficients are zero and
// therefore interchangeable. They are NOT interchangeable in the engine:
// miniwindow.cpp:4146-4149 assigns m11=Mxx, m21=Mxy, m12=Myx, m22=Myy -- note
// the CROSS between the script's argument order (Mxx,Mxy,Myx,Myy) and
// Matrix2x3's declaration order (isurface.h:150 -- m11,m12,m21,m22,dx,dy).
//   forward (cpu_raster.cpp:329-330): x' = x*m11 + y*m21 + dx
//                                     y' = x*m12 + y*m22 + dy
// which is exactly the map miniwindow.cpp:4143-4144 documents for the script.
// A +90 degree rotation is Mxx=cos=0, Mxy=sin=1, Myx=-sin=-1, Myy=cos=0 (the
// field comments at miniwindow.cpp:4146-4149 name them cosine/sine/-sine/
// cosine), i.e. Matrix2x3{ m11=0, m12=-1, m21=1, m22=0 }; det = 0*0-1*(-1) = 1.
// Inverse (cpu_raster.cpp:343-345) with dx=1, dy=3:
//   sx = floor(-(Y+0.5-3)) = 2-Y        sy = floor(X+0.5-1) = X-1
// so a 2-wide x 3-tall source lands 3-wide x 2-tall: source COLUMN x becomes
// destination ROW 2-x (column 1 -> row 1, column 0 -> row 2), read
// top-to-bottom -> left-to-right.
TEST(PalSurface, TransformBlitRotate90PinsOffDiagonalRoles)
{
    PAL_FIXTURE();
    const Colour CYAN = 0xFFFF00;                          // 6 distinct source pixels
    pal::SurfaceId src = ad.createSurface({ 2, 3 }, BLACK); // deliberately not square
    ad.setPixel(src, { 0, 0 }, RED);    ad.setPixel(src, { 1, 0 }, GREEN);
    ad.setPixel(src, { 0, 1 }, BLUE);   ad.setPixel(src, { 1, 1 }, YELLOW);
    ad.setPixel(src, { 0, 2 }, WHITE);  ad.setPixel(src, { 1, 2 }, CYAN);
    pal::SurfaceId dst = ad.createSurface({ 8, 8 }, BLACK);

    pal::Matrix2x3 rot90{ 0, -1, 1, 0, 1, 3 };
    // Smooth is what the engine actually passes (miniwindow.cpp:4166); this
    // adapter documents nearest sampling for both qualities (cpu_raster.cpp:291-293)
    // and the PixelBuffer overload ignores the parameter outright (:312).
    ad.blit(dst, { 0, 0, 0, 0 }, src, { 0, 0, 2, 3 },
            pal::BlitMode::Copy, 0, pal::BlitQuality::Smooth, &rot90);

    // Row Y=1 is source column x=1 (sx = 2-Y = 1), read at sy = X-1 = 0,1,2.
    EXPECT_TRUE(ad.getPixel(dst, { 1, 1 }) == GREEN);    // src(1,0)
    EXPECT_TRUE(ad.getPixel(dst, { 2, 1 }) == YELLOW);   // src(1,1)
    EXPECT_TRUE(ad.getPixel(dst, { 3, 1 }) == CYAN);     // src(1,2)
    // Row Y=2 is source column x=0 (sx = 2-Y = 0).
    EXPECT_TRUE(ad.getPixel(dst, { 1, 2 }) == RED);      // src(0,0)
    EXPECT_TRUE(ad.getPixel(dst, { 2, 2 }) == BLUE);     // src(0,1)
    EXPECT_TRUE(ad.getPixel(dst, { 3, 2 }) == WHITE);    // src(0,2)
    // Swap m12/m21 in the inverse map and it becomes sx = Y-3, sy = -X: a
    // single RED pixel at (0,3), so all six above fail.
    // The scan window is x in [0,5), y in [0,4), so all four of these ARE
    // visited -- one per arm of the source clamp at cpu_raster.cpp:346.
    EXPECT_TRUE(ad.getPixel(dst, { 4, 1 }) == BLACK);    // sy =  3 >= srcRect.h
    EXPECT_TRUE(ad.getPixel(dst, { 0, 1 }) == BLACK);    // sy = -1 <  0
    EXPECT_TRUE(ad.getPixel(dst, { 1, 0 }) == BLACK);    // sx =  2 >= srcRect.w
    EXPECT_TRUE(ad.getPixel(dst, { 1, 3 }) == BLACK);    // sx = -1 <  0
}

// The colour-key test INSIDE the transform loop (cpu_raster.cpp:351) is a
// distinct line from the 1:1 path's key test (:367) and is the live path for
// WindowTransformImage Mode 3, where the engine passes the image's own (0,0)
// pixel as the key (miniwindow.cpp:4165).
// NOTE what does NOT prove the matrix branch ran: the dstRect. This is the
// surface overload, which rewrites {0,0,0,0} to {0,0,2,2} at :304-305, so a
// bypassed matrix branch would land an ordinary keyed 2x2 blit at the ORIGIN
// and (3,2) would still be WHITE. The three COLOUR assertions below are what
// catch that.
TEST(PalSurface, TransformBlitTransparentKeySkipsKeyedPixels)
{
    PAL_FIXTURE();
    pal::SurfaceId src = ad.createSurface({ 2, 2 }, BLACK);
    ad.setPixel(src, { 0, 0 }, GREEN);                    // the key -- Mode 3's (0,0) pixel
    ad.setPixel(src, { 1, 0 }, RED);
    ad.setPixel(src, { 0, 1 }, BLUE);
    ad.setPixel(src, { 1, 1 }, YELLOW);
    pal::Matrix2x3 t{ 1, 0, 0, 1, 3, 2 };                 // sx = X-3, sy = Y-2

    // The background is WHITE, not the key colour, so "skipped" is provable as
    // "the destination survived" rather than "black stayed black".
    pal::SurfaceId keyed = ad.createSurface({ 8, 8 }, WHITE);
    ad.blit(keyed, { 0, 0, 0, 0 }, src, { 0, 0, 2, 2 },
            pal::BlitMode::TransparentKey, GREEN, pal::BlitQuality::Fast, &t);
    EXPECT_TRUE(ad.getPixel(keyed, { 3, 2 }) == WHITE);   // keyed pixel skipped
    EXPECT_TRUE(ad.getPixel(keyed, { 4, 2 }) == RED);     // ...and ONLY that one
    EXPECT_TRUE(ad.getPixel(keyed, { 3, 3 }) == BLUE);
    EXPECT_TRUE(ad.getPixel(keyed, { 4, 3 }) == YELLOW);

    // Control: the same blit in Copy mode DOES land the key colour there, so
    // the assertion above cannot pass merely because src(0,0) was never green.
    // It also passes key=GREEN, pinning the `mode == TransparentKey &&` half of
    // the condition -- Copy must consult the key not at all.
    pal::SurfaceId copied = ad.createSurface({ 8, 8 }, WHITE);
    ad.blit(copied, { 0, 0, 0, 0 }, src, { 0, 0, 2, 2 },
            pal::BlitMode::Copy, GREEN, pal::BlitQuality::Fast, &t);
    EXPECT_TRUE(ad.getPixel(copied, { 3, 2 }) == GREEN);
    EXPECT_TRUE(ad.getPixel(copied, { 4, 3 }) == YELLOW);
}

// A transform whose bounding box runs off the destination must draw the
// in-bounds part and nothing else -- no wrap, no overrun. Both directions:
// off the bottom-right (the min(..., dst->size) clamp at cpu_raster.cpp:339-340)
// and off the top-left (the max(..., 0) clamp at :337-338, where the bbox
// corners are negative). The negative case also proves the inverse map keeps
// honouring dx/dy when they are negative: it must sample the source's
// BOTTOM-RIGHT 2x2, not its top-left.
TEST(PalSurface, TransformBlitClipsToDestinationBounds)
{
    PAL_FIXTURE();
    pal::SurfaceId src = makeTinted4x4(ad);

    // Translate (4,4) into a 6x6 destination: sx = X-4, sy = Y-4, so only
    // source columns/rows 0..1 have a destination pixel; 2..3 fall off.
    pal::SurfaceId dst = ad.createSurface({ 6, 6 }, BLACK);
    pal::Matrix2x3 over{ 1, 0, 0, 1, 4, 4 };
    ad.blit(dst, { 0, 0, 0, 0 }, src, { 0, 0, 4, 4 },
            pal::BlitMode::Copy, 0, pal::BlitQuality::Fast, &over);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 6; ++x) {
            const Colour want = (x >= 4 && y >= 4) ? tint4(x - 4, y - 4) : BLACK;
            ASSERT_TRUE(ad.getPixel(dst, { x, y }) == want);
        }

    // Translate (-2,-2): sx = X+2, sy = Y+2 -- the source's bottom-right 2x2
    // lands at the destination origin and the rest is clipped away.
    pal::SurfaceId dst2 = ad.createSurface({ 6, 6 }, BLACK);
    pal::Matrix2x3 under{ 1, 0, 0, 1, -2, -2 };
    ad.blit(dst2, { 0, 0, 0, 0 }, src, { 0, 0, 4, 4 },
            pal::BlitMode::Copy, 0, pal::BlitQuality::Fast, &under);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 6; ++x) {
            const Colour want = (x < 2 && y < 2) ? tint4(x + 2, y + 2) : BLACK;
            ASSERT_TRUE(ad.getPixel(dst2, { x, y }) == want);
        }
}

// srcRect's ORIGIN, not just its size: cpu_raster.cpp:347 rebases the sampled
// coordinate to srcRect.x/y before the buffer bounds check at :348. Only the
// PixelBuffer overload can reach a non-zero origin there -- the surface
// overload clips and lets readPixels rebase to (0,0) first (:302-308) -- so
// this drives BOTH overloads with the same sub-rect and requires them to agree.
TEST(PalSurface, TransformBlitSamplesSrcRectSubRegion)
{
    PAL_FIXTURE();
    pal::SurfaceId src = makeTinted4x4(ad);
    pal::Matrix2x3 t{ 1, 0, 0, 1, 2, 1 };                 // sx = X-2, sy = Y-1

    // PixelBuffer overload: the whole 4x4 image is handed over, but srcRect
    // selects the middle 2x2 (source pixels (1,1)..(2,2)).
    pal::PixelBuffer buf;
    ad.readPixels(src, { 0, 0, 4, 4 }, buf, pal::PixelFormat::BGRA32);
    ASSERT_TRUE(buf.valid());
    pal::SurfaceId dstBuf = ad.createSurface({ 8, 8 }, BLACK);
    ad.blit(dstBuf, { 6, 6, 2, 2 }, buf, { 1, 1, 2, 2 },
            pal::BlitMode::Copy, 0, pal::BlitQuality::Fast, &t);

    // Surface overload with the same sub-rect: it rebases through readPixels,
    // so it must land identically -- same pixels, same place.
    pal::SurfaceId dstSurf = ad.createSurface({ 8, 8 }, BLACK);
    ad.blit(dstSurf, { 6, 6, 2, 2 }, src, { 1, 1, 2, 2 },
            pal::BlitMode::Copy, 0, pal::BlitQuality::Fast, &t);

    for (pal::SurfaceId d : { dstBuf, dstSurf })
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                // Drop the `srcRect.x +` at :347 and the buffer half reads
                // tint4(0,0) here instead of tint4(1,1); ignore the matrix and
                // the block lands at the (6,6) dstRect we deliberately mis-set.
                const Colour want = (x >= 2 && x < 4 && y >= 1 && y < 3)
                                  ? tint4(x - 1, y) : BLACK;
                ASSERT_TRUE(ad.getPixel(d, { x, y }) == want);
            }

    // ...and the line :347 feeds: the BUFFER bounds check at :348. The
    // PixelBuffer overload does no clipping of its own (only the surface
    // overload does, :302-305), so :348 is the sole guard between a
    // caller-supplied srcRect and `src.row(bsy)` -- which for a bottom-up
    // buffer (isurface.h:67) computes a NEGATIVE byte offset once bsy >=
    // size.h. srcRect {3,3,4,4} on a 4x4 buffer admits sx,sy in [0,4) at :346
    // but only bsx == bsy == 3 at :348, so exactly ONE source pixel is legal.
    pal::SurfaceId dstOver = ad.createSurface({ 8, 8 }, BLACK);
    pal::Matrix2x3 t1{ 1, 0, 0, 1, 1, 1 };                // sx = X-1, sy = Y-1
    ad.blit(dstOver, { 0, 0, 4, 4 }, buf, { 3, 3, 4, 4 },
            pal::BlitMode::Copy, 0, pal::BlitQuality::Fast, &t1);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            ASSERT_TRUE(ad.getPixel(dstOver, { x, y })
                        == ((x == 1 && y == 1) ? tint4(3, 3) : BLACK));
}

// The det == 0 guard (cpu_raster.cpp:321-322) with a SINGULAR-BUT-NON-ZERO
// matrix -- the case a naive guard ("m11 == 0 && m22 == 0") would let through,
// and the one a script actually produces by collapsing an image onto a line.
// m{1,1,1,1} maps every source point onto y = x; det = 1*1 - 1*1 = 0.
// It matters because :344-345 divides BY det: on the destination diagonal both
// numerators are exactly zero, so without the guard the sampler evaluates
// 0.0/0.0 and casts the resulting NaN to int. The engine's own pre-check
// (miniwindow.cpp:4136) uses the identical determinant; this pins the
// adapter's second line of defence, which is what the wx adapter
// (wx_surface.cpp:741-742) and any future backend must match.
TEST(PalSurface, TransformBlitSingularNonZeroMatrixDrawsNothing)
{
    PAL_FIXTURE();
    pal::SurfaceId src = ad.createSurface({ 2, 2 }, RED);
    pal::SurfaceId dst = ad.createSurface({ 8, 8 }, BLACK);
    pal::Matrix2x3 squash{ 1, 1, 1, 1, 0, 0 };
    ad.blit(dst, { 0, 0, 2, 2 }, src, { 0, 0, 2, 2 },
            pal::BlitMode::Copy, 0, pal::BlitQuality::Fast, &squash);
    // Its bounding box would be (0,0)..(4,4); sweep the WHOLE surface, since a
    // guard that fired late could still have written part of the diagonal.
    // (Weaken the guard to `det == 0.0 && m->m11 == 0.0` -- which the existing
    // TransformBlitDegenerateDetIsNoop at :400 cannot detect, its m11 being 0 --
    // and RED appears at (0,0),(1,1),(2,2),(3,3),(4,4).)
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            ASSERT_TRUE(ad.getPixel(dst, { x, y }) == BLACK);

    // POSITIVE CONTROL -- without it the sweep above is satisfied by a blit()
    // that does nothing at all, a broken fixture, or an unusable SurfaceId.
    // Same source, same destination, only the matrix made invertible.
    pal::Matrix2x3 ok{ 1, 0, 0, 1, 2, 2 };                // det = 1; sx = X-2, sy = Y-2
    ad.blit(dst, { 0, 0, 2, 2 }, src, { 0, 0, 2, 2 },
            pal::BlitMode::Copy, 0, pal::BlitQuality::Fast, &ok);
    EXPECT_TRUE(ad.getPixel(dst, { 2, 2 }) == RED);
    EXPECT_TRUE(ad.getPixel(dst, { 3, 3 }) == RED);
    EXPECT_TRUE(ad.getPixel(dst, { 1, 1 }) == BLACK);     // scanned (bbox [1,5)) and rejected
}

MC_TEST_MAIN()
