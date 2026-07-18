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

TEST(RegexpTest, RuntimeErrorIsNotReportedAsMatch) {
    // A PCRE *runtime* error (count < 0, distinct from PCRE_ERROR_NOMATCH) must
    // NOT be reported as a successful match. "(a+)+$" against a long run of 'a'
    // followed by a non-'a' forces catastrophic backtracking that exhausts the
    // PCRE match limit (PCRE_ERROR_MATCHLIMIT). On Windows this throws; the
    // portable path used to fall through to `return true`, spuriously firing the
    // trigger/alias on a line it never matched.
    t_regexp* r = regcomp("(a+)+$");
    ASSERT_NE(nullptr, r);
    std::string s(50, 'a');
    s += '!';                       // guarantees the overall match must fail
    int result = regexec(r, s.c_str());
    EXPECT_EQ(0, result);           // errored → NON-match, not a spurious match
    EXPECT_LT(r->m_iExecutionError, 0);  // a real runtime error code was recorded
    delete r;
}

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────

// regexp.cpp GetWildcard: "this code is to ensure that we don't find a match
// (eg. mob = Kobold) and then if duplicates were allowed, replace Kobold
// with false." — with (?J) duplicate names, a later empty/unset duplicate
// must not overwrite an earlier non-empty capture.
TEST(RegexpTest, DuplicateNamedGroupKeepsNonEmptyCapture) {
    t_regexp* r = regcomp("(?J)(?P<mob>\\w+) attacks(?: (?P<mob>\\w+))?");
    ASSERT_NE(nullptr, r);

    // second <mob> group does not participate: keep the first capture
    EXPECT_GT(regexec(r, "Kobold attacks"), 0);
    EXPECT_STREQ("Kobold", r->GetWildcard("mob").c_str());

    // control: when the later duplicate IS set, it wins
    EXPECT_GT(regexec(r, "Kobold attacks Dragon"), 0);
    EXPECT_STREQ("Dragon", r->GetWildcard("mob").c_str());
    delete r;
}

// regexp.cpp regexec: "int options = PCRE_NOTEMPTY; // don't match on an
// empty string" — a pattern that can only match empty is reported as a
// non-match, so triggers/aliases can never fire on zero-length matches.
TEST(RegexpTest, EmptyOnlyMatchSuppressedByNotEmpty) {
    t_regexp* r = regcomp("z*");
    ASSERT_NE(nullptr, r);
    EXPECT_EQ(0, regexec(r, "abc"));      // would match "" at offset 0
    EXPECT_GT(regexec(r, "azzb"), 0);     // control: a real match still works
    delete r;
}

// regexp.cpp: "if, and only if, we match, we will save the matching string,
// the count and offsets" (and on NOMATCH "don't save matching string etc.")
// — a failed match leaves the previous match's state intact.
TEST(RegexpTest, NoMatchPreservesPreviousMatchState) {
    t_regexp* r = regcomp("(\\w+)");
    ASSERT_NE(nullptr, r);
    EXPECT_GT(regexec(r, "hello"), 0);
    EXPECT_STREQ("hello", r->GetWildcard(1).c_str());
    int savedCount = r->m_iCount;

    EXPECT_EQ(0, regexec(r, "!!!"));      // no word characters: no match
    EXPECT_EQ(savedCount, r->m_iCount);   // state untouched
    EXPECT_STREQ("hello", r->GetWildcard(1).c_str());
    EXPECT_STREQ("hello", r->m_sTarget.c_str());
    delete r;
}

// regexp.cpp: "inspired by a suggestion by Twisol (to remove a hard-coded
// limit on the number of wildcards)" — offsets are sized from
// PCRE_INFO_CAPTURECOUNT, so more than 10 groups extract fine.
TEST(RegexpTest, MoreThanTenCaptureGroupsExtractable) {
    t_regexp* r = regcomp("(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)");
    ASSERT_NE(nullptr, r);
    EXPECT_GT(regexec(r, "abcdefghijkl"), 0);
    EXPECT_EQ(13, r->m_iCount);           // 12 groups + whole match
    EXPECT_STREQ("k", r->GetWildcard(11).c_str());
    EXPECT_STREQ("l", r->GetWildcard(12).c_str());
    delete r;
}

MC_TEST_MAIN()
