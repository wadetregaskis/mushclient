// tests/test_appearance.cpp
// The arithmetic behind light and dark chrome (src/appearance.h): what the preference
// comes to, and what becomes of toolbar pixels.  The painting can only be judged by
// looking at it.

#include "mc_test.h"
#include "stdafx.h"
#include "appearance.h"

#include <vector>

TEST(AppearanceTest, PreferenceResolves) {
    // System follows the system
    EXPECT_FALSE(Appearance_ResolveDark(APPEARANCE_SYSTEM, false, true));
    EXPECT_TRUE(Appearance_ResolveDark(APPEARANCE_SYSTEM, true, true));
    // forcing ignores it
    EXPECT_FALSE(Appearance_ResolveDark(APPEARANCE_LIGHT, true, true));
    EXPECT_TRUE(Appearance_ResolveDark(APPEARANCE_DARK, false, true));
    // a bad value out of the database acts like the default
    EXPECT_TRUE(Appearance_ResolveDark(99, true, true));
    EXPECT_FALSE(Appearance_ResolveDark(-1, false, true));
}

TEST(AppearanceTest, UnsupportedStaysLightWhateverIsAsked) {
    EXPECT_FALSE(Appearance_ResolveDark(APPEARANCE_DARK, true, false));
    EXPECT_FALSE(Appearance_ResolveDark(APPEARANCE_SYSTEM, true, false));
}

TEST(AppearanceTest, WindowsBuildGates) {
    EXPECT_FALSE(Appearance_WindowsBuildSupportsDark(0));        // not Windows 10 at all
    EXPECT_FALSE(Appearance_WindowsBuildSupportsDark(17134));    // 1803
    EXPECT_TRUE(Appearance_WindowsBuildSupportsDark(17763));     // 1809
    EXPECT_TRUE(Appearance_WindowsBuildSupportsDark(26300));
    EXPECT_EQ(19, Appearance_ImmersiveDarkAttribute(18363));     // 1909
    EXPECT_EQ(20, Appearance_ImmersiveDarkAttribute(19041));     // 2004
}

static int Luma(unsigned long c) {
    return (int)(((c & 0xFF) * 30 + ((c >> 8) & 0xFF) * 59 + ((c >> 16) & 0xFF) * 11) / 100);
}

TEST(AppearanceTest, EveryToolbarColourShowsUpOnADarkFace) {
    // the sixteen colours of the bitmaps, and a sample besides
    const unsigned long colours[] = {
        0x000000, 0x000080, 0x008000, 0x008080, 0x800000, 0x800080, 0x808000, 0x808080,
        0xC0C0C0, 0x0000FF, 0x00FF00, 0x00FFFF, 0xFF0000, 0xFF00FF, 0xFFFF00, 0xFFFFFF,
        0x202020, 0x101040, 0x400040,
    };
    for (unsigned long c : colours) {
        const unsigned long out = Appearance_ToolbarPixelForDark(c);
        EXPECT_EQ(0ul, out >> 24);
        // kFace is luma 32.  Not much of a promise for blue, whose luma is 11% of
        // anything; which is why the lift raises all the channels.
        EXPECT_GT(Luma(out), 75);
    }
}

TEST(AppearanceTest, BlackAndWhiteStayDistinct) {
    // a white page with a black outline must stay a page and an outline
    const int black = Luma(Appearance_ToolbarPixelForDark(0x000000));
    const int white = Luma(Appearance_ToolbarPixelForDark(0xFFFFFF));
    const int grey = Luma(Appearance_ToolbarPixelForDark(0x808080));
    EXPECT_GT(black, 200);
    EXPECT_GT(white, 200);
    EXPECT_LT(grey, black);
}

TEST(AppearanceTest, BrightColoursAreLeftAlone) {
    EXPECT_EQ(0x0000FFul, Appearance_ToolbarPixelForDark(0x0000FF));
    EXPECT_EQ(0x00FFFFul, Appearance_ToolbarPixelForDark(0x00FFFF));
}

TEST(AppearanceTest, LiftingKeepsTheHue) {
    const unsigned long out = Appearance_ToolbarPixelForDark(0x800000);     // navy: blue channel high
    EXPECT_GT((int)((out >> 16) & 0xFF), (int)(out & 0xFF));
    EXPECT_GT((int)((out >> 16) & 0xFF), (int)((out >> 8) & 0xFF));
}

TEST(AppearanceTest, DisabledPixelIsGreyAndFaded) {
    const unsigned int out = Appearance_DisabledPixel(0xFFFF0000, true);
    EXPECT_LT(out >> 24, 0xFFu);
    EXPECT_EQ((out >> 16) & 0xFF, (out >> 8) & 0xFF);
    EXPECT_EQ((out >> 8) & 0xFF, out & 0xFF);
    // and what was transparent stays that way
    EXPECT_EQ(0u, Appearance_DisabledPixel(0, true) >> 24);
}

TEST(AppearanceScaleTest, IdentityIsACopy) {
    const unsigned int src[4] = { 0xFF112233, 0xFF445566, 0x00000000, 0x80778899 };
    unsigned int dst[4] = {};
    Appearance_ScaleImage(src, 2, 2, dst, 2, 2);
    for (int i = 0; i < 4; i++)
        EXPECT_EQ(src[i], dst[i]);
}

TEST(AppearanceScaleTest, DoubleIsBlocks) {
    const unsigned int src[2] = { 0xFF000000, 0xFFFFFFFF };
    unsigned int dst[8] = {};
    Appearance_ScaleImage(src, 2, 1, dst, 4, 2);
    EXPECT_EQ(0xFF000000u, dst[0]);
    EXPECT_EQ(0xFF000000u, dst[1]);
    EXPECT_EQ(0xFFFFFFFFu, dst[2]);
    EXPECT_EQ(0xFFFFFFFFu, dst[3]);
    EXPECT_EQ(0xFF000000u, dst[4]);
    EXPECT_EQ(0xFFFFFFFFu, dst[7]);
}

TEST(AppearanceScaleTest, OneAndAHalfAverages) {
    // 2 pixels to 3: the middle one is half of each
    const unsigned int src[2] = { 0xFF000000, 0xFFC8C8C8 };
    unsigned int dst[3] = {};
    Appearance_ScaleImage(src, 2, 1, dst, 3, 1);
    EXPECT_EQ(0xFF000000u, dst[0]);
    EXPECT_EQ(0xFF646464u, dst[1]);
    EXPECT_EQ(0xFFC8C8C8u, dst[2]);
}

TEST(AppearanceScaleTest, TransparentNeighbourDoesNotDarkenTheEdge) {
    // Half a white pixel and half nothing is half-transparent WHITE, not grey.
    const unsigned int src[2] = { 0xFFFFFFFF, 0x00000000 };
    unsigned int dst[3] = {};
    Appearance_ScaleImage(src, 2, 1, dst, 3, 1);
    EXPECT_EQ(0xFFFFFFFFu, dst[0]);
    EXPECT_EQ(0x80u, dst[1] >> 24);
    EXPECT_EQ(0xFFFFFFu, dst[1] & 0xFFFFFF);
    EXPECT_EQ(0u, dst[2] >> 24);
}

TEST(AppearanceScaleTest, ToolbarSizedIconAtEveryCommonScale) {
    std::vector<unsigned int> src(16 * 15, 0xFF102030);
    for (int percent : { 100, 125, 150, 175, 200, 225, 250, 300 }) {
        const int w = 16 * percent / 100, h = 15 * percent / 100;
        std::vector<unsigned int> dst(w * h, 0);
        Appearance_ScaleImage(&src[0], 16, 15, &dst[0], w, h);
        for (unsigned int p : dst)
            EXPECT_EQ(0xFF102030u, p);
    }
}
