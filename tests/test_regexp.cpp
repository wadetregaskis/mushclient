// tests/test_regexp.cpp
// Unit tests for the PCRE regex wrapper (regexp.h / regexp.cpp).

#include "mc_test.h"
#include "platform/compat.h"
#include "regexp.h"

// ── Compilation ───────────────────────────────────────────────────────────

TEST(RegexpTest, CompileSimplePattern) {
    t_regexp* r = regcomp("hello");
    ASSERT_NE(nullptr, r);
    delete r;
}

TEST(RegexpTest, CompilePatternWithCapture) {
    t_regexp* r = regcomp("(\\w+)\\s+(\\w+)");
    ASSERT_NE(nullptr, r);
    delete r;
}

TEST(RegexpTest, CompileInvalidPatternReturnsNull) {
    // An unmatched bracket is an invalid PCRE pattern
    t_regexp* r = regcomp("[unclosed");
    EXPECT_EQ(nullptr, r);
}

// ── Matching ──────────────────────────────────────────────────────────────

TEST(RegexpTest, SimpleMatch) {
    t_regexp* r = regcomp("hello");
    ASSERT_NE(nullptr, r);
    int result = regexec(r, "say hello world");
    EXPECT_GT(result, 0);
    delete r;
}

TEST(RegexpTest, NoMatch) {
    t_regexp* r = regcomp("goodbye");
    ASSERT_NE(nullptr, r);
    int result = regexec(r, "hello world");
    EXPECT_EQ(0, result);
    delete r;
}

TEST(RegexpTest, CaptureGroupExtraction) {
    t_regexp* r = regcomp("(\\w+)\\s+(\\w+)");
    ASSERT_NE(nullptr, r);
    int result = regexec(r, "foo bar");
    EXPECT_GT(result, 0);
    EXPECT_STREQ("foo", r->GetWildcard(1).c_str());
    EXPECT_STREQ("bar", r->GetWildcard(2).c_str());
    delete r;
}

TEST(RegexpTest, NamedCapture) {
    t_regexp* r = regcomp("(?P<first>\\w+)\\s+(?P<second>\\w+)");
    ASSERT_NE(nullptr, r);
    int result = regexec(r, "alice bob");
    EXPECT_GT(result, 0);
    EXPECT_STREQ("alice", r->GetWildcard("first").c_str());
    EXPECT_STREQ("bob",   r->GetWildcard("second").c_str());
    delete r;
}

TEST(RegexpTest, CaseInsensitiveFlag) {
    t_regexp* r = regcomp("hello", PCRE_CASELESS);
    ASSERT_NE(nullptr, r);
    int result = regexec(r, "HELLO WORLD");
    EXPECT_GT(result, 0);
    delete r;
}

TEST(RegexpTest, AnchoredMatchAtStart) {
    t_regexp* r = regcomp("^hello");
    ASSERT_NE(nullptr, r);
    EXPECT_GT(regexec(r, "hello world"), 0);
    EXPECT_EQ(0, regexec(r, "say hello"));
    delete r;
}

TEST(RegexpTest, MatchOffset) {
    t_regexp* r = regcomp("world");
    ASSERT_NE(nullptr, r);
    EXPECT_GT(regexec(r, "hello world", 0), 0);
    // Start offset past the match
    EXPECT_EQ(0, regexec(r, "hello world", 7));
    delete r;
}

// ── CheckRegularExpression ────────────────────────────────────────────────

TEST(RegexpTest, CheckValidExpression) {
    EXPECT_TRUE(CheckRegularExpression("(\\w+)", 0));
}

TEST(RegexpTest, CheckInvalidExpression) {
    EXPECT_FALSE(CheckRegularExpression("[unclosed", 0));
}

// ── Wildcard count ────────────────────────────────────────────────────────

TEST(RegexpTest, WildcardCountSimple) {
    // pcre_exec returns total captured pairs including the whole match (pair 0),
    // so m_iCount == num_groups + 1.  Three explicit groups → m_iCount == 4.
    t_regexp* r = regcomp("(a)(b)(c)");
    ASSERT_NE(nullptr, r);
    regexec(r, "abc");
    EXPECT_EQ(4, r->m_iCount);
    delete r;
}

TEST(RegexpTest, WildcardCountNoGroups) {
    // No explicit groups: only the whole-match pair (pair 0) → m_iCount == 1.
    t_regexp* r = regcomp("abc");
    ASSERT_NE(nullptr, r);
    regexec(r, "abc");
    EXPECT_EQ(1, r->m_iCount);
    delete r;
}

MC_TEST_MAIN()
