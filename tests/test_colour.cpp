// tests/test_colour.cpp
// Unit tests for the CColor class and COLORREF helper macros.

#include "mc_test.h"
#include "platform/compat.h"
#include "Color.h"

// ── COLORREF macro helpers ────────────────────────────────────────────────

TEST(ColourTest, RGBMacroExtraction) {
    COLORREF c = RGB(255, 128, 64);
    EXPECT_EQ(255, (int)GetRValue(c));
    EXPECT_EQ(128, (int)GetGValue(c));
    EXPECT_EQ(64,  (int)GetBValue(c));
}

TEST(ColourTest, RGBRoundTripAllValues) {
    // Spot-check several combinations
    int vals[] = {0, 1, 127, 128, 254, 255};
    for (int r : vals) {
        for (int g : vals) {
            for (int b : vals) {
                COLORREF c = RGB(r, g, b);
                EXPECT_EQ(r, (int)GetRValue(c));
                EXPECT_EQ(g, (int)GetGValue(c));
                EXPECT_EQ(b, (int)GetBValue(c));
            }
        }
    }
}

TEST(ColourTest, RGBZero) {
    COLORREF c = RGB(0, 0, 0);
    EXPECT_EQ(0u, c);  // Black should be 0x00000000
}

// ── CColor construction ───────────────────────────────────────────────────

TEST(CColorTest, ConstructFromCOLORREF) {
    CColor c(RGB(200, 100, 50));
    EXPECT_EQ(200, c.GetRed());
    EXPECT_EQ(100, c.GetGreen());
    EXPECT_EQ(50,  c.GetBlue());
}

TEST(CColorTest, DefaultConstruct) {
    CColor c;
    EXPECT_EQ(0, c.GetRed());
    EXPECT_EQ(0, c.GetGreen());
    EXPECT_EQ(0, c.GetBlue());
}

TEST(CColorTest, ConvertToCOLORREF) {
    CColor c;
    c.SetRGB(10, 20, 30);
    COLORREF ref = (COLORREF)c;
    EXPECT_EQ(10, (int)GetRValue(ref));
    EXPECT_EQ(20, (int)GetGValue(ref));
    EXPECT_EQ(30, (int)GetBValue(ref));
}

// ── SetRGB / Get channels ─────────────────────────────────────────────────

TEST(CColorTest, SetRGB) {
    CColor c;
    c.SetRGB(255, 128, 0);
    EXPECT_EQ(255, c.GetRed());
    EXPECT_EQ(128, c.GetGreen());
    EXPECT_EQ(0,   c.GetBlue());
}

TEST(CColorTest, SetChannelsIndividually) {
    CColor c;
    c.SetRed(100);
    c.SetGreen(150);
    c.SetBlue(200);
    EXPECT_EQ(100, c.GetRed());
    EXPECT_EQ(150, c.GetGreen());
    EXPECT_EQ(200, c.GetBlue());
}

// ── HLS ───────────────────────────────────────────────────────────────────

TEST(CColorTest, HLSRoundTrip) {
    CColor c(RGB(200, 100, 50));
    double h = c.GetHue();
    double l = c.GetLuminance();
    double s = c.GetSaturation();
    // Re-create from HLS
    CColor c2;
    c2.SetHLS(h, l, s);
    EXPECT_NEAR(200.0, c2.GetRed(),   2.0);
    EXPECT_NEAR(100.0, c2.GetGreen(), 2.0);
    EXPECT_NEAR(50.0,  c2.GetBlue(),  2.0);
}

TEST(CColorTest, PurpleRed) {
    // RGB(255,0,0) = Hue 0 degrees
    CColor c(RGB(255, 0, 0));
    EXPECT_NEAR(0.0, c.GetHue(), 1.0);
    EXPECT_NEAR(0.5, c.GetLuminance(), 0.01);
    EXPECT_NEAR(1.0, c.GetSaturation(), 0.01);
}

TEST(CColorTest, White) {
    CColor c(RGB(255, 255, 255));
    EXPECT_EQ(255, c.GetRed());
    EXPECT_EQ(255, c.GetGreen());
    EXPECT_EQ(255, c.GetBlue());
    EXPECT_NEAR(1.0, c.GetLuminance(), 0.01);
    EXPECT_NEAR(0.0, c.GetSaturation(), 0.01);
}

TEST(CColorTest, Black) {
    CColor c(RGB(0, 0, 0));
    EXPECT_EQ(0, c.GetRed());
    EXPECT_EQ(0, c.GetGreen());
    EXPECT_EQ(0, c.GetBlue());
    EXPECT_NEAR(0.0, c.GetLuminance(), 0.01);
    EXPECT_NEAR(0.0, c.GetSaturation(), 0.01);
}

MC_TEST_MAIN()
