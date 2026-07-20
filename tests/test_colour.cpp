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

// Parameterised (audit §4): PurpleRed/White/Black share the construct-from-RGB then
// check luminance/saturation (+ hue for the chromatic case, + RGB readback) shape.
// checkHue is false for the two greyscale rows (hue is undefined at saturation 0).
TEST(CColorTest, ReferenceColoursHlsRoundTrip) {
    struct Case {
        COLORREF rgb; int r, g, b; double hue, lum, sat; bool checkHue;
    };
    const Case cases[] = {
        { RGB(255,   0,   0), 255,   0,   0, 0.0, 0.5, 1.0, true  },  // PurpleRed
        { RGB(255, 255, 255), 255, 255, 255, 0.0, 1.0, 0.0, false },  // White
        { RGB(  0,   0,   0),   0,   0,   0, 0.0, 0.0, 0.0, false },  // Black
    };
    for (const Case& t : cases) {
        CColor c(t.rgb);
        EXPECT_EQ(t.r, c.GetRed());
        EXPECT_EQ(t.g, c.GetGreen());
        EXPECT_EQ(t.b, c.GetBlue());
        EXPECT_NEAR(t.lum, c.GetLuminance(),  0.01);
        EXPECT_NEAR(t.sat, c.GetSaturation(), 0.01);
        if (t.checkHue)
            EXPECT_NEAR(t.hue, c.GetHue(), 1.0);
    }
}


// ToHLS G==maxcolor arm (Color.cpp:189-190) — never hit by the existing red-dominant
// cases. RGB(50,200,100): G is max, so min=50/255 max=200/255 diff=150/255,
// L=125/255≈0.4902 (<0.5), S=diff/(max+min)=150/250=0.6,
// hue = 60*(B-R)/diff + 120 = 60*(50/150)+120 = 140.
TEST(CColorTest, HlsGreenDominantHue) {
    CColor c(RGB(50, 200, 100));
    double hue = c.GetHue();          // forces ToHLS via the G==maxcolor branch
    double lum = c.GetLuminance();
    double sat = c.GetSaturation();
    EXPECT_NEAR(140.0, hue, 0.5);
    EXPECT_NEAR(0.490196, lum, 0.001);
    EXPECT_NEAR(0.6, sat, 0.001);
    // round-trip back through ToRGB
    EXPECT_EQ(50,  c.GetRed());
    EXPECT_EQ(200, c.GetGreen());
    EXPECT_EQ(100, c.GetBlue());
}

// ToHLS B==maxcolor arm (Color.cpp:191-192) — the else branch, never reached before.
// RGB(50,100,200): B is max, min=50/255 max=200/255 diff=150/255, L=125/255≈0.4902,
// S=0.6, hue = 60*(R-G)/diff + 240 = 60*(-50/150)+240 = 220.
TEST(CColorTest, HlsBlueDominantHue) {
    CColor c(RGB(50, 100, 200));
    double hue = c.GetHue();          // forces ToHLS via the B==maxcolor (else) branch
    double lum = c.GetLuminance();
    double sat = c.GetSaturation();
    EXPECT_NEAR(220.0, hue, 0.5);
    EXPECT_NEAR(0.490196, lum, 0.001);
    EXPECT_NEAR(0.6, sat, 0.001);
    // round-trip back through ToRGB
    EXPECT_EQ(50,  c.GetRed());
    EXPECT_EQ(100, c.GetGreen());
    EXPECT_EQ(200, c.GetBlue());
}

// ToHLS negative-hue wrap (Color.cpp:194-195): when R==maxcolor and B>G the raw hue is
// negative, so `if (m_hue < 0.0) m_hue += 360.0` fires. RGB(200,50,100): R is max,
// hue = 60*(G-B)/diff + 0 = 60*(-50/150) = -20 -> +360 = 340.
TEST(CColorTest, HlsHueWrapNegative) {
    CColor c(RGB(200, 50, 100));
    double hue = c.GetHue();          // R==maxcolor, raw hue -20 -> wrapped to 340
    double lum = c.GetLuminance();
    double sat = c.GetSaturation();
    EXPECT_NEAR(340.0, hue, 0.5);
    EXPECT_NEAR(0.490196, lum, 0.001);
    EXPECT_NEAR(0.6, sat, 0.001);
    // round-trip back through ToRGB
    EXPECT_EQ(200, c.GetRed());
    EXPECT_EQ(50,  c.GetGreen());
    EXPECT_EQ(100, c.GetBlue());
}

// ToRGB high-luminance q-formula (Color.cpp:228-229): the else branch
// q = (L + S) - (L*S) runs only when luminance >= 0.5 (all prior tests are <0.5).
// RGB(150,200,220): min=150/255 max=220/255 diff=70/255, L=185/255≈0.7255 (>0.5),
// S=diff/(2-(max+min))=70/140=0.5, hue = 60*(R-G)/diff + 240 ≈ 197.14; q=0.862745.
// Reading RGB back exercises the L>=0.5 q-formula and round-trips exactly.
TEST(CColorTest, ToRgbHighLuminanceQ) {
    CColor c(RGB(150, 200, 220));
    double lum = c.GetLuminance();    // forces ToHLS; L>0.5 selects the high-lum q branch in ToRGB
    double sat = c.GetSaturation();
    double hue = c.GetHue();
    EXPECT_NEAR(0.725490, lum, 0.001);
    EXPECT_TRUE(lum > 0.5);           // documents that ToRGB will take the else (high-lum) q branch
    EXPECT_NEAR(0.5, sat, 0.001);
    EXPECT_NEAR(197.143, hue, 0.5);
    // read RGB back -> ToRGB() runs the L>=0.5 q-formula (line 229)
    EXPECT_EQ(150, c.GetRed());
    EXPECT_EQ(200, c.GetGreen());
    EXPECT_EQ(220, c.GetBlue());
}

MC_TEST_MAIN()
