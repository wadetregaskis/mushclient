// tests/test_cstring.cpp
// Unit tests for the macOS CString compatibility layer.
//
// Verifies that our std::string-backed CString behaves like MFC CString
// (MultiByte variant) for all operations used in MUSHclient.

#include "mc_test.h"
#include "platform/mfc_string.h"

// ── Construction ──────────────────────────────────────────────────────────

TEST(CStringTest, DefaultConstruct) {
    CString s;
    EXPECT_TRUE(s.IsEmpty());
    EXPECT_EQ(0, s.GetLength());
}

TEST(CStringTest, ConstructFromCharPtr) {
    CString s("hello");
    EXPECT_FALSE(s.IsEmpty());
    EXPECT_EQ(5, s.GetLength());
    EXPECT_STREQ("hello", (const char*)s);
}

TEST(CStringTest, ConstructFromNullptr) {
    CString s(nullptr);
    EXPECT_TRUE(s.IsEmpty());
}

TEST(CStringTest, ConstructFromStdString) {
    std::string src("world");
    CString s(src);
    EXPECT_EQ(5, s.GetLength());
    EXPECT_STREQ("world", s.StdString().c_str());
}

TEST(CStringTest, CopyConstruct) {
    CString a("copy me");
    CString b(a);
    EXPECT_EQ(a, b);
}

// ── Assignment ────────────────────────────────────────────────────────────

TEST(CStringTest, AssignCharPtr) {
    CString s;
    s = "assigned";
    EXPECT_STREQ("assigned", (const char*)s);
}

TEST(CStringTest, AssignCString) {
    CString a("src");
    CString b;
    b = a;
    EXPECT_EQ(a, b);
}

// ── Concatenation ─────────────────────────────────────────────────────────

TEST(CStringTest, PlusEquals) {
    CString s("hello");
    s += " world";
    EXPECT_STREQ("hello world", (const char*)s);
}

TEST(CStringTest, PlusCStringCString) {
    CString a("foo"), b("bar");
    CString c = a + b;
    EXPECT_STREQ("foobar", (const char*)c);
}

TEST(CStringTest, PlusCStringChar) {
    CString a("abc");
    CString b = a + '!';
    EXPECT_STREQ("abc!", (const char*)b);
}

// ── Comparison ────────────────────────────────────────────────────────────

TEST(CStringTest, EqualityWithCString) {
    CString a("hello"), b("hello");
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(CStringTest, EqualityWithCharPtr) {
    CString a("hello");
    EXPECT_TRUE(a == "hello");
    EXPECT_FALSE(a == "world");
}

TEST(CStringTest, Inequality) {
    CString a("apple"), b("banana");
    EXPECT_TRUE(a != b);
}

TEST(CStringTest, LessThan) {
    CString a("apple"), b("banana");
    EXPECT_TRUE(a < b);
    EXPECT_FALSE(b < a);
}

TEST(CStringTest, GreaterThan) {
    CString a("banana"), b("apple");
    EXPECT_TRUE(a > b);
}

// ── Substrings ────────────────────────────────────────────────────────────

TEST(CStringTest, Left) {
    CString s("hello world");
    EXPECT_EQ(CString("hello"), s.Left(5));
    EXPECT_EQ(CString(""), s.Left(0));
}

TEST(CStringTest, LeftLongerThanString) {
    CString s("hi");
    EXPECT_EQ(CString("hi"), s.Left(100));
}

TEST(CStringTest, Right) {
    CString s("hello world");
    EXPECT_EQ(CString("world"), s.Right(5));
}

TEST(CStringTest, Mid) {
    CString s("hello world");
    EXPECT_EQ(CString("world"), s.Mid(6));
    EXPECT_EQ(CString("lo"), s.Mid(3, 2));
}

// ── Search ────────────────────────────────────────────────────────────────

TEST(CStringTest, FindSubstring) {
    CString s("hello world");
    EXPECT_EQ(6, s.Find("world"));
    EXPECT_EQ(-1, s.Find("xyz"));
}

TEST(CStringTest, FindChar) {
    CString s("hello");
    EXPECT_EQ(1, s.Find('e'));
    EXPECT_EQ(-1, s.Find('z'));
}

TEST(CStringTest, FindWithStart) {
    CString s("abcabc");
    EXPECT_EQ(3, s.Find("abc", 1));
}

TEST(CStringTest, ReverseFind) {
    CString s("hello world");
    EXPECT_EQ(7, s.ReverseFind('o'));   // 'o' in "world"
    EXPECT_EQ(-1, s.ReverseFind('z'));
}

// ── Case conversion ───────────────────────────────────────────────────────

TEST(CStringTest, MakeUpper) {
    CString s("hello");
    s.MakeUpper();
    EXPECT_EQ(CString("HELLO"), s);
}

TEST(CStringTest, MakeLower) {
    CString s("WORLD");
    s.MakeLower();
    EXPECT_EQ(CString("world"), s);
}

// ── Trim ──────────────────────────────────────────────────────────────────

TEST(CStringTest, TrimLeft) {
    CString s("  hello  ");
    s.TrimLeft();
    EXPECT_EQ(CString("hello  "), s);
}

TEST(CStringTest, TrimRight) {
    CString s("  hello  ");
    s.TrimRight();
    EXPECT_EQ(CString("  hello"), s);
}

TEST(CStringTest, Trim) {
    CString s("  hello  ");
    s.Trim();
    EXPECT_EQ(CString("hello"), s);
}

TEST(CStringTest, TrimAlreadyTrimmed) {
    CString s("hello");
    s.Trim();
    EXPECT_EQ(CString("hello"), s);
}

// ── Replace ───────────────────────────────────────────────────────────────

TEST(CStringTest, ReplaceSubstring) {
    CString s("aababab");
    int n = s.Replace("ab", "X");
    EXPECT_EQ(3, n);
    EXPECT_EQ(CString("aXXX"), s);
}

TEST(CStringTest, ReplaceChar) {
    CString s("hello");
    int n = s.Replace('l', 'r');
    EXPECT_EQ(2, n);
    EXPECT_EQ(CString("herro"), s);
}

TEST(CStringTest, ReplaceNotFound) {
    CString s("hello");
    int n = s.Replace("xyz", "abc");
    EXPECT_EQ(0, n);
    EXPECT_EQ(CString("hello"), s);
}

// ── Format (printf-style) ─────────────────────────────────────────────────

TEST(CStringTest, FormatInt) {
    CString s;
    s.Format("Value: %d", 42);
    EXPECT_EQ(CString("Value: 42"), s);
}

TEST(CStringTest, FormatString) {
    CString s;
    s.Format("%s=%s", "key", "value");
    EXPECT_EQ(CString("key=value"), s);
}

TEST(CStringTest, FormatHex) {
    CString s;
    s.Format("0x%08X", 0xDEADBEEF);
    EXPECT_EQ(CString("0xDEADBEEF"), s);
}

// ── GetBuffer / ReleaseBuffer ─────────────────────────────────────────────

TEST(CStringTest, GetBufferReleaseBuffer) {
    CString s("hello");
    char* buf = s.GetBuffer(10);
    buf[0] = 'H';
    s.ReleaseBuffer();
    EXPECT_EQ(CString("Hello"), s);
}

// ── CompareNoCase ─────────────────────────────────────────────────────────

TEST(CStringTest, CompareNoCaseEqual) {
    CString s("Hello");
    EXPECT_EQ(0, s.CompareNoCase("hello"));
    EXPECT_EQ(0, s.CompareNoCase("HELLO"));
}

TEST(CStringTest, CompareNoCaseDifferent) {
    CString s("Hello");
    EXPECT_NE(0, s.CompareNoCase("world"));
}

// ── Delete / Insert ───────────────────────────────────────────────────────

TEST(CStringTest, Delete) {
    CString s("hello world");
    s.Delete(5, 6);
    EXPECT_EQ(CString("hello"), s);
}

TEST(CStringTest, InsertString) {
    CString s("hello");
    s.Insert(5, " world");
    EXPECT_EQ(CString("hello world"), s);
}

TEST(CStringTest, InsertChar) {
    CString s("hllo");
    s.Insert(1, 'e');
    EXPECT_EQ(CString("hello"), s);
}

// ── Character access ──────────────────────────────────────────────────────

TEST(CStringTest, GetAt) {
    CString s("hello");
    EXPECT_EQ('h', s.GetAt(0));
    EXPECT_EQ('o', s.GetAt(4));
}

TEST(CStringTest, IndexOperator) {
    CString s("world");
    EXPECT_EQ('w', s[0]);
    EXPECT_EQ('d', s[4]);
}

// ── FindOneOf ─────────────────────────────────────────────────────────────

TEST(CStringTest, FindOneOf) {
    CString s("hello world");
    EXPECT_EQ(2, s.FindOneOf("lx"));   // first 'l' at index 2
    EXPECT_EQ(-1, s.FindOneOf("xyz1234"));
}

// ── Span operations ───────────────────────────────────────────────────────
// SpanIncluding used to return an INT — the position of the first character not
// in the set — which is a different operation under MFC's name, and answered -1
// for the case where every character matches.

TEST(CStringTest, SpanIncludingReturnsTheLeadingRunThatMatches) {
    EXPECT_EQ(CString("123"), CString("123abc").SpanIncluding("0123456789"));
    EXPECT_EQ(CString("123"), CString("123").SpanIncluding("0123456789"));  // all match
    EXPECT_TRUE(CString("abc").SpanIncluding("0123456789").IsEmpty());
    EXPECT_TRUE(CString("123").SpanIncluding(nullptr).IsEmpty());
    EXPECT_TRUE(CString("").SpanIncluding("0123456789").IsEmpty());
}

TEST(CStringTest, SpanExcludingIsItsMirror) {
    EXPECT_EQ(CString("abc"), CString("abc123").SpanExcluding("0123456789"));
    EXPECT_EQ(CString("abc"), CString("abc").SpanExcluding("0123456789"));
    EXPECT_TRUE(CString("123").SpanExcluding("0123456789").IsEmpty());
}

// ── Empty string edge-cases ───────────────────────────────────────────────

TEST(CStringTest, EmptyStringOps) {
    CString s;
    EXPECT_EQ(-1, s.Find("x"));
    EXPECT_EQ(-1, s.ReverseFind('x'));
    EXPECT_EQ(CString(""), s.Left(5));
    EXPECT_EQ(CString(""), s.Right(5));
    s.Trim();
    EXPECT_TRUE(s.IsEmpty());
}

MC_TEST_MAIN()
