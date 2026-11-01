// tests/test_unicode.cpp
// Cell widths and grapheme clusters (src/unicode_width.h), against Unicode's own test data.

#include "mc_test.h"
#include "stdafx.h"
#include "unicode_width.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "unicode_grapheme_cases.inc"

using namespace unicode;

static std::string Utf8 (unsigned int c)
{
    std::string s;
    if (c < 0x80)
        s += (char) c;
    else if (c < 0x800) {
        s += (char) (0xC0 | (c >> 6));
        s += (char) (0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        s += (char) (0xE0 | (c >> 12));
        s += (char) (0x80 | ((c >> 6) & 0x3F));
        s += (char) (0x80 | (c & 0x3F));
    } else {
        s += (char) (0xF0 | (c >> 18));
        s += (char) (0x80 | ((c >> 12) & 0x3F));
        s += (char) (0x80 | ((c >> 6) & 0x3F));
        s += (char) (0x80 | (c & 0x3F));
    }
    return s;
}

static std::string Utf8 (std::initializer_list<unsigned int> list)
{
    std::string s;
    for (unsigned int c : list)
        s += Utf8 (c);
    return s;
}

// Unicode's GraphemeBreakTest.txt, every line of it.
TEST(UnicodeTest, GraphemeBreakTestConformance) {
    int failures = 0;
    for (const char * spec : kGraphemeCases) {
        ClusterTracker tracker;
        const char * p = spec;
        bool first = true;
        bool expectBreak = true;
        while (*p) {
            if (*p == ' ') { expectBreak = false; p++; continue; }
            if (*p == '|') { expectBreak = true; p++; continue; }
            char * end = nullptr;
            const unsigned long c = strtoul (p, &end, 16);
            const bool gotBreak = tracker.Add ((unsigned int) c);
            if (gotBreak != (first || expectBreak)) {
                if (failures < 10)
                    printf ("  at %04lX in: %s\n", c, spec);
                failures++;
            }
            first = false;
            p = end;
        }
    }
    EXPECT_EQ(0, failures);
}

TEST(UnicodeTest, Widths) {
    EXPECT_EQ(1, CodePointWidth('a'));
    EXPECT_EQ(0, CodePointWidth(0x07));
    EXPECT_EQ(0, CodePointWidth(0x85));
    EXPECT_EQ(1, CodePointWidth(0xE9));
    EXPECT_EQ(1, CodePointWidth(0xAD));       // a soft hyphen takes its place
    EXPECT_EQ(0, CodePointWidth(0x0301));     // combining acute
    EXPECT_EQ(0, CodePointWidth(0x200D));     // ZWJ
    EXPECT_EQ(0, CodePointWidth(0xFE0F));     // variation selector
    EXPECT_EQ(2, CodePointWidth(0x4E2D));     // CJK
    EXPECT_EQ(2, CodePointWidth(0xAC00));     // Hangul syllable
    EXPECT_EQ(2, CodePointWidth(0xFF21));     // fullwidth A
    EXPECT_EQ(1, CodePointWidth(0xFF71));     // halfwidth katakana
    EXPECT_EQ(2, CodePointWidth(0x1F600));    // emoji
    EXPECT_EQ(1, CodePointWidth(0x2603));     // snowman: text by default
    EXPECT_EQ(0, CodePointWidth(0x1160));     // conjoining vowel
}

TEST(UnicodeTest, CellsOfStrings) {
    EXPECT_EQ(5, Utf8Cells ("hello", 5));
    EXPECT_EQ(0, Utf8Cells ("", 0));

    std::string s = Utf8 ({ 0x4E2D, 0x6587 });           // two ideographs
    EXPECT_EQ(4, Utf8Cells (s.data (), s.size ()));

    s = Utf8 ({ 'e', 0x0301 });                           // e + combining acute
    EXPECT_EQ(1, Utf8Cells (s.data (), s.size ()));

    s = Utf8 ({ 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467 });   // family: one picture
    EXPECT_EQ(2, Utf8Cells (s.data (), s.size ()));

    s = Utf8 ({ 0x1F1E6, 0x1F1FA, 0x1F1F3, 0x1F1FF });    // two flags
    EXPECT_EQ(4, Utf8Cells (s.data (), s.size ()));

    s = Utf8 ({ 0x2603, 0xFE0F });                        // snowman with emoji presentation
    EXPECT_EQ(2, Utf8Cells (s.data (), s.size ()));

    s = Utf8 ({ 0x1F600, 0xFE0E });                       // emoji asked to be text
    EXPECT_EQ(1, Utf8Cells (s.data (), s.size ()));

    s = Utf8 ({ '1', 0xFE0F, 0x20E3 });                   // keycap
    EXPECT_EQ(2, Utf8Cells (s.data (), s.size ()));

    s = Utf8 ({ 0x1100, 0x1161, 0x11A8 });                // jamo: one syllable
    EXPECT_EQ(2, Utf8Cells (s.data (), s.size ()));

    s = Utf8 ({ 0x0915, 0x093E });                        // Devanagari ka + vowel sign aa
    EXPECT_EQ(2, Utf8Cells (s.data (), s.size ()));

    s = Utf8 ({ 'a', 0x4E2D, 'b', 0x1F600, 'c' });
    EXPECT_EQ(7, Utf8Cells (s.data (), s.size ()));
}

TEST(UnicodeTest, BadUtf8CountsAsOneEach) {
    const char bad [] = "\xC0\x80" "a" "\xE4\xB8";       // overlong, and a truncated one
    EXPECT_EQ(5, Utf8Cells (bad, 5));

    unsigned int c;
    EXPECT_EQ(1, DecodeUtf8 ("\xED\xA0\x80", 3, c));     // a surrogate
    EXPECT_EQ(0xFFFDu, c);
    EXPECT_EQ(1, DecodeUtf8 ("\xF4\x90\x80\x80", 4, c)); // beyond U+10FFFF
    EXPECT_EQ(0xFFFDu, c);
    EXPECT_EQ(4, DecodeUtf8 ("\xF0\x9F\x98\x80", 4, c));
    EXPECT_EQ(0x1F600u, c);
}

TEST(UnicodeTest, ClusterEdges) {
    const std::string s = Utf8 ({ 'a', 'e', 0x0301, 0x1F468, 0x200D, 0x1F469, 'z' });
    // bytes: a=0  e=1 acute=2,3  man=4..7 zwj=8..10 woman=11..14  z=15
    EXPECT_EQ(0u, Utf8ClusterStart (s.data (), s.size (), 0));
    EXPECT_EQ(1u, Utf8ClusterStart (s.data (), s.size (), 2));
    EXPECT_EQ(4u, Utf8ClusterStart (s.data (), s.size (), 9));
    EXPECT_EQ(4u, Utf8ClusterEnd (s.data (), s.size (), 2));
    EXPECT_EQ(1u, Utf8ClusterEnd (s.data (), s.size (), 1));
    EXPECT_EQ(15u, Utf8ClusterEnd (s.data (), s.size (), 5));
    EXPECT_EQ(15u, Utf8ClusterStart (s.data (), s.size (), 15));
    EXPECT_EQ(16u, Utf8ClusterEnd (s.data (), s.size (), 16));
}

TEST(UnicodeTest, NeedsShaping) {
    EXPECT_FALSE(Utf8NeedsShaping ("hello", 5));
    std::string s = Utf8 ({ 0xE9, 0x4E2D, 0xAC00 });      // GDI manages these
    EXPECT_FALSE(Utf8NeedsShaping (s.data (), s.size ()));
    s = Utf8 ({ 0x1F600 });
    EXPECT_TRUE(Utf8NeedsShaping (s.data (), s.size ()));
    s = Utf8 ({ 'e', 0x0301 });
    EXPECT_TRUE(Utf8NeedsShaping (s.data (), s.size ()));
    s = Utf8 ({ 0x0915, 0x094D, 0x0937 });                // a conjunct
    EXPECT_TRUE(Utf8NeedsShaping (s.data (), s.size ()));
    s = Utf8 ({ 0x05D0 });                                // Hebrew
    EXPECT_TRUE(Utf8NeedsShaping (s.data (), s.size ()));
}

TEST(UnicodeTest, EveryTableIsSortedAndClean) {
    // Binary search only works if the generator did its job; spot-check the seams by
    // walking the whole of Unicode and looking for anything impossible.
    int odd = 0;
    for (unsigned int c = 0; c <= 0x10FFFF; c += (c < 0x30000 ? 1 : 0x100)) {
        const int w = CodePointWidth (c);
        if (w < 0 || w > 2)
            odd++;
    }
    EXPECT_EQ(0, odd);
    EXPECT_STREQ("16.0.0", TablesVersion ());
}
