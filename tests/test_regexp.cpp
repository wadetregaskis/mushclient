// tests/test_regexp.cpp
// Unit tests for the PCRE regex wrapper (regexp.h / regexp.cpp).

#include "mc_test.h"
#include "platform/compat.h"
#include "regexp.h"

// RAII compile+free wrapper (audit §4): compile in the ctor, delete in the dtor,
// so the repeated `t_regexp* r = regcomp(...); ...; delete r;` boilerplate collapses
// and an early ASSERT return no longer leaks the compiled program.  regcomp THROWS on
// an invalid pattern, so r is non-null for every well-formed pattern here.
namespace {
struct Regexp {
    t_regexp* r;
    explicit Regexp(const char* pattern, int flags = 0) : r(regcomp(pattern, flags)) {}
    ~Regexp() { delete r; }
    Regexp(const Regexp&) = delete;
    Regexp& operator=(const Regexp&) = delete;
    t_regexp* get()      const { return r; }
    t_regexp* operator->() const { return r; }
};
} // namespace

// ── Compilation ───────────────────────────────────────────────────────────

TEST(RegexpTest, CompileSimplePattern) {
    Regexp r("hello");
    ASSERT_NE(nullptr, r.get());
}

TEST(RegexpTest, CompilePatternWithCapture) {
    Regexp r("(\\w+)\\s+(\\w+)");
    ASSERT_NE(nullptr, r.get());
}

TEST(RegexpTest, CompileInvalidPatternThrows) {
    // An unmatched bracket is an invalid PCRE pattern.  Windows contract
    // (regexp.cpp): regcomp THROWS a CException* (ThrowErrorException) so
    // callers surface eBadRegularExpression instead of storing a null regexp.
    bool threw = false;
    try {
        regcomp("[unclosed");
    } catch (CException* e) {
        char msg[500] = "";
        e->GetErrorMessage(msg, sizeof msg);
        EXPECT_NE(nullptr, strstr(msg, "Failed"));   // "Failed: <pcre error> at offset N"
        e->Delete();
        threw = true;
    }
    EXPECT_TRUE(threw);
}

// ── Matching ──────────────────────────────────────────────────────────────

// Parameterised (audit §4): the pure {pattern, input, flags, offset, expect-match}
// compile+match/no-match cases (formerly SimpleMatch / NoMatch / CaseInsensitiveFlag /
// AnchoredMatchAtStart / MatchOffset) collapse into one table.  A fresh RAII Regexp
// per row.  regexec > 0 on match, == 0 on no-match.
TEST(RegexpTest, CompileAndMatchTable) {
    struct Case { const char* pattern; const char* input; int flags; int offset; bool match; };
    const Case cases[] = {
        { "hello",   "say hello world", 0,             0, true  },  // SimpleMatch
        { "goodbye", "hello world",     0,             0, false },  // NoMatch
        { "hello",   "HELLO WORLD",     PCRE_CASELESS, 0, true  },  // CaseInsensitiveFlag
        { "^hello",  "hello world",     0,             0, true  },  // AnchoredMatchAtStart (hit)
        { "^hello",  "say hello",       0,             0, false },  // AnchoredMatchAtStart (miss)
        { "world",   "hello world",     0,             0, true  },  // MatchOffset (offset 0)
        { "world",   "hello world",     0,             7, false },  // MatchOffset (past the match)
    };
    for (const Case& c : cases) {
        Regexp r(c.pattern, c.flags);
        ASSERT_NE(nullptr, r.get());
        int result = regexec(r.get(), c.input, c.offset);
        if (c.match) EXPECT_GT(result, 0);
        else         EXPECT_EQ(0, result);
    }
}

TEST(RegexpTest, CaptureGroupExtraction) {
    Regexp r("(\\w+)\\s+(\\w+)");
    ASSERT_NE(nullptr, r.get());
    int result = regexec(r.get(), "foo bar");
    EXPECT_GT(result, 0);
    EXPECT_STREQ("foo", r->GetWildcard(1).c_str());
    EXPECT_STREQ("bar", r->GetWildcard(2).c_str());
}

TEST(RegexpTest, NamedCapture) {
    Regexp r("(?P<first>\\w+)\\s+(?P<second>\\w+)");
    ASSERT_NE(nullptr, r.get());
    int result = regexec(r.get(), "alice bob");
    EXPECT_GT(result, 0);
    EXPECT_STREQ("alice", r->GetWildcard("first").c_str());
    EXPECT_STREQ("bob",   r->GetWildcard("second").c_str());
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
    Regexp r("(a)(b)(c)");
    ASSERT_NE(nullptr, r.get());
    regexec(r.get(), "abc");
    EXPECT_EQ(4, r->m_iCount);
}

TEST(RegexpTest, WildcardCountNoGroups) {
    // No explicit groups: only the whole-match pair (pair 0) → m_iCount == 1.
    Regexp r("abc");
    ASSERT_NE(nullptr, r.get());
    regexec(r.get(), "abc");
    EXPECT_EQ(1, r->m_iCount);
}

TEST(RegexpTest, RuntimeErrorIsNotReportedAsMatch) {
    // A PCRE *runtime* error (count < 0, distinct from PCRE_ERROR_NOMATCH) must
    // NOT be reported as a successful match. "(a+)+$" against a long run of 'a'
    // followed by a non-'a' forces catastrophic backtracking that exhausts the
    // PCRE match limit (PCRE_ERROR_MATCHLIMIT). On Windows this throws; the
    // portable path used to fall through to `return true`, spuriously firing the
    // trigger/alias on a line it never matched.
    Regexp r("(a+)+$");
    ASSERT_NE(nullptr, r.get());
    std::string s(50, 'a');
    s += '!';                       // guarantees the overall match must fail
    int result = regexec(r.get(), s.c_str());
    EXPECT_EQ(0, result);           // errored → NON-match, not a spurious match
    EXPECT_LT(r->m_iExecutionError, 0);  // a real runtime error code was recorded
}

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────

// regexp.cpp GetWildcard: "this code is to ensure that we don't find a match
// (eg. mob = Kobold) and then if duplicates were allowed, replace Kobold
// with false." — with (?J) duplicate names, a later empty/unset duplicate
// must not overwrite an earlier non-empty capture.
TEST(RegexpTest, DuplicateNamedGroupKeepsNonEmptyCapture) {
    Regexp r("(?J)(?P<mob>\\w+) attacks(?: (?P<mob>\\w+))?");
    ASSERT_NE(nullptr, r.get());

    // second <mob> group does not participate: keep the first capture
    EXPECT_GT(regexec(r.get(), "Kobold attacks"), 0);
    EXPECT_STREQ("Kobold", r->GetWildcard("mob").c_str());

    // control: when the later duplicate IS set, it wins
    EXPECT_GT(regexec(r.get(), "Kobold attacks Dragon"), 0);
    EXPECT_STREQ("Dragon", r->GetWildcard("mob").c_str());
}

// regexp.cpp regexec: options = App.m_bRegexpMatchEmpty ? 0 : PCRE_NOTEMPTY.
// B-3 flip: the headless build now reads this global option (real regexp.cpp
// FULL_BUILD arm) instead of the old portable always-PCRE_NOTEMPTY deviation.
// The Windows default is RegexpMatchEmpty=1 (globalregistryoptions.cpp:56), so a
// pattern that CAN match empty fires at offset 0 — matching Windows behaviour.
TEST(RegexpTest, EmptyMatchAllowedByDefault) {
    Regexp r("z*");
    ASSERT_NE(nullptr, r.get());
    EXPECT_GT(regexec(r.get(), "abc"), 0);      // matches "" at offset 0 (RegexpMatchEmpty=1)
    EXPECT_GT(regexec(r.get(), "azzb"), 0);     // control: a real match still works
}

// regexp.cpp: "if, and only if, we match, we will save the matching string,
// the count and offsets" (and on NOMATCH "don't save matching string etc.")
// — a failed match leaves the previous match's state intact.
TEST(RegexpTest, NoMatchPreservesPreviousMatchState) {
    Regexp r("(\\w+)");
    ASSERT_NE(nullptr, r.get());
    EXPECT_GT(regexec(r.get(), "hello"), 0);
    EXPECT_STREQ("hello", r->GetWildcard(1).c_str());
    int savedCount = r->m_iCount;

    EXPECT_EQ(0, regexec(r.get(), "!!!"));   // no word characters: no match
    EXPECT_EQ(savedCount, r->m_iCount);      // state untouched
    EXPECT_STREQ("hello", r->GetWildcard(1).c_str());
    EXPECT_STREQ("hello", r->m_sTarget.c_str());
}

// regexp.cpp: "inspired by a suggestion by Twisol (to remove a hard-coded
// limit on the number of wildcards)" — offsets are sized from
// PCRE_INFO_CAPTURECOUNT, so more than 10 groups extract fine.
TEST(RegexpTest, MoreThanTenCaptureGroupsExtractable) {
    Regexp r("(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)");
    ASSERT_NE(nullptr, r.get());
    EXPECT_GT(regexec(r.get(), "abcdefghijkl"), 0);
    EXPECT_EQ(13, r->m_iCount);           // 12 groups + whole match
    EXPECT_STREQ("k", r->GetWildcard(11).c_str());
    EXPECT_STREQ("l", r->GetWildcard(12).c_str());
}



// regexp.cpp regexec (lines 69-71): "exit if no regexp program to process
// (possibly because of previous error)". This EXTENDS RuntimeErrorIsNotReportedAsMatch:
// once a runtime error frees+nulls m_program, a SECOND regexec on the SAME t_regexp
// must hit the null-program guard and early-out with false -- never crash, and never
// advance past the guard into another pcre_exec attempt.
TEST(RegexpTest, ExecOnErroredProgramEarlyOutsFalse) {
    Regexp r("(a+)+$");
    ASSERT_NE(nullptr, r.get());
    std::string s(50, 'a');
    s += '!';                                    // forces the overall match to fail

    // First exec: catastrophic backtracking -> a PCRE runtime error, which frees
    // and NULLs m_program (regexp.cpp:104-105) and records the error code.
    EXPECT_EQ(0, regexec(r.get(), s.c_str()));
    ASSERT_TRUE(r->m_program == nullptr);        // program was nulled on the error
    EXPECT_LT(r->m_iExecutionError, 0);
    const long attemptsAfterError = r->m_iMatchAttempts;

    // Second exec on the SAME errored program: the null-program guard returns
    // false BEFORE the m_iMatchAttempts++ at regexp.cpp:96. Drop the guard and the
    // return value is still 0 (pcre_exec(NULL) -> PCRE_ERROR_NULL, no crash), but the
    // attempt counter advances -- so the counter, not the return value, catches it.
    EXPECT_EQ(0, regexec(r.get(), "anything"));
    EXPECT_EQ(attemptsAfterError, r->m_iMatchAttempts);
}


MC_TEST_MAIN()
