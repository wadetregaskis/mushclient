// tests/test_utilities.cpp
//
// Unit tests for the single-sourced pure-logic utilities.  These live in
// utilities_shared.cpp (myAtoF, encodeBase64, EditDistance) and
// utilities_portable.cpp (decodeBase64) — both compiled into mushcore, which is
// the SAME utilities_shared.cpp the shipping product links.  So unlike the old
// portable twins, these exercise the real shipping code, not a reimplementation.

#include "mc_test.h"
#include "platform/doc_stub.h"   // CString + prototypes: myAtoF, EditDistance,
                                 // encodeBase64, decodeBase64
#include <cstring>

// ── encodeBase64 / decodeBase64 ─────────────────────────────────────────────

// RFC 4648 §10 test vectors — the canonical base64 encodings.
TEST(Utilities, EncodeBase64_Rfc4648Vectors)
{
    EXPECT_STREQ("",         (const char*)encodeBase64("", false));
    EXPECT_STREQ("Zg==",     (const char*)encodeBase64("f", false));
    EXPECT_STREQ("Zm8=",     (const char*)encodeBase64("fo", false));
    EXPECT_STREQ("Zm9v",     (const char*)encodeBase64("foo", false));
    EXPECT_STREQ("Zm9vYg==", (const char*)encodeBase64("foob", false));
    EXPECT_STREQ("Zm9vYmE=", (const char*)encodeBase64("fooba", false));
    EXPECT_STREQ("Zm9vYmFy", (const char*)encodeBase64("foobar", false));
}

TEST(Utilities, DecodeBase64_Rfc4648Vectors)
{
    EXPECT_STREQ("f",      (const char*)decodeBase64("Zg=="));
    EXPECT_STREQ("fo",     (const char*)decodeBase64("Zm8="));
    EXPECT_STREQ("foo",    (const char*)decodeBase64("Zm9v"));
    EXPECT_STREQ("foob",   (const char*)decodeBase64("Zm9vYg=="));
    EXPECT_STREQ("fooba",  (const char*)decodeBase64("Zm9vYmE="));
    EXPECT_STREQ("foobar", (const char*)decodeBase64("Zm9vYmFy"));
}

TEST(Utilities, DecodeBase64_IgnoresWhitespace)
{
    // decodeBase64 strips spaces/tabs/newlines before decoding.
    EXPECT_STREQ("foobar", (const char*)decodeBase64("Zm9v\r\nYmFy"));
    EXPECT_STREQ("foobar", (const char*)decodeBase64("Zm9v YmFy"));
}

TEST(Utilities, Base64RoundTrip)
{
    const char* samples[] = {
        "", "A", "AB", "ABC", "ABCD", "Hello, world!",
        "The quick brown fox jumps over the lazy dog."
    };
    for (const char* s : samples)
    {
        CString enc = encodeBase64(s, false);
        EXPECT_STREQ(s, (const char*)decodeBase64((const char*)enc));
    }
}

TEST(Utilities, EncodeBase64_MultiLineWrapsAt76)
{
    // 60 input bytes → 80 base64 chars; multi-line inserts a CRLF after the
    // first 76 output chars (i.e. after 57 input bytes).
    CString enc = encodeBase64(
        "0123456789012345678901234567890123456789012345678901234567890123456789", true);
    const char* p = (const char*)enc;
    EXPECT_TRUE(strstr(p, "\r\n") != NULL);
}

// ── myAtoF ──────────────────────────────────────────────────────────────────

static double atof_val(const char* s) { double d = -999.0; myAtoF(s, &d); return d; }

TEST(Utilities, MyAtoF_Values)
{
    EXPECT_NEAR(0.0,     atof_val("0"),       1e-12);
    EXPECT_NEAR(42.0,    atof_val("42"),      1e-12);
    EXPECT_NEAR(-42.0,   atof_val("-42"),     1e-12);
    EXPECT_NEAR(0.5,     atof_val("0.5"),     1e-12);
    EXPECT_NEAR(0.25,    atof_val("0.25"),    1e-12);
    EXPECT_NEAR(-0.25,   atof_val("-0.25"),   1e-12);
    EXPECT_NEAR(3.14,    atof_val("3.14"),    1e-9);
    EXPECT_NEAR(1000.0,  atof_val("1e3"),     1e-9);
    EXPECT_NEAR(0.001,   atof_val("1e-3"),    1e-12);
    EXPECT_NEAR(12.5,    atof_val("  12.5"),  1e-12);  // leading whitespace skipped
}

TEST(Utilities, MyAtoF_ReturnsCharsConsumed)
{
    double d;
    EXPECT_EQ(2, myAtoF("42abc", &d));   // stops at 'a'
    EXPECT_EQ(4, myAtoF("3.14", &d));
    EXPECT_EQ(0, myAtoF("abc", &d));     // consumes nothing
}

// ── EditDistance ────────────────────────────────────────────────────────────

TEST(Utilities, EditDistance_Basics)
{
    EXPECT_EQ(0, EditDistance("", ""));
    EXPECT_EQ(3, EditDistance("", "abc"));
    EXPECT_EQ(3, EditDistance("abc", ""));
    EXPECT_EQ(0, EditDistance("kitten", "kitten"));
    EXPECT_EQ(3, EditDistance("kitten", "sitting"));   // classic Levenshtein = 3
    EXPECT_EQ(2, EditDistance("book", "back"));
}

TEST(Utilities, EditDistance_DamerauTransposition)
{
    // The algorithm's step 6A credits an adjacent transposition as a single
    // edit (for positions >2): "abcd" → "abdc" is 1, not 2.
    EXPECT_EQ(1, EditDistance("abcd", "abdc"));
}

TEST(Utilities, EditDistance_CapsAt20)
{
    // Each side is clamped to 20 chars, so distance can never exceed 20.
    std::string a(50, 'a');
    std::string b(50, 'b');
    EXPECT_EQ(20, EditDistance(a, b));
}

MC_TEST_MAIN()
