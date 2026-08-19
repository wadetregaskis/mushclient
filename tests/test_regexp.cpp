// tests/test_regexp.cpp
// Unit tests for the PCRE regex wrapper (regexp.h / regexp.cpp).

#include "mc_test.h"
#include "platform/compat.h"
#include "regexp.h"

// Real doc.h layout (matches mushcore / mushcore_testsupport -- see the
// WIN32/_NOSOUND/MUSHCLIENT_FULL_BUILD comment in tests/CMakeLists.txt) is
// needed for the RecallText leak test below.
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

#include <sys/resource.h>   // getrusage (RSS leak check)

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

// ── GetCallbackWildcards ──────────────────────────────────────────────────
//
// The positional wildcard list a script callback is handed.  Shape is fixed by
// what the six engines already do: [0] = whole match, [1..capture-count] = the
// numbered captures, length = capture-count + 1 (NOT padded to MAX_WILDCARDS).

TEST(RegexpWildcardsTest, WholeMatchFirstThenNumberedCaptures) {
    Regexp r("(\\w+)\\s+(\\w+)");
    ASSERT_NE(nullptr, r.get());
    ASSERT_NE(0, regexec(r.get(), "Hello Big World"));

    std::vector<std::string> wc;
    r->GetCallbackWildcards(false, wc);

    ASSERT_EQ(3u, wc.size());              // 2 captures + the whole match
    EXPECT_EQ("Hello Big", wc[0]);         // [0] is the whole match, not %1
    EXPECT_EQ("Hello",     wc[1]);
    EXPECT_EQ("Big",       wc[2]);
}

// The list is sized from the pattern's capture COUNT, not from how many
// captures happened to participate in the match -- so an unmatched optional
// group still occupies its slot, as an empty string.
TEST(RegexpWildcardsTest, UnparticipatingCaptureKeepsItsSlot) {
    Regexp r("(a)(b)?(c)");
    ASSERT_NE(nullptr, r.get());
    ASSERT_NE(0, regexec(r.get(), "ac"));

    std::vector<std::string> wc;
    r->GetCallbackWildcards(false, wc);

    ASSERT_EQ(4u, wc.size());
    EXPECT_EQ("ac", wc[0]);
    EXPECT_EQ("a",  wc[1]);
    EXPECT_EQ("",   wc[2]);                // (b)? did not participate
    EXPECT_EQ("c",  wc[3]);
}

// The trigger option "convert wildcards to lower case"
// (CTrigger::bLowercaseWildcard).  FixWildcard applies it when it fills
// trigger_item->wildcards[], which is what a Windows ActiveScript callback and
// GetTriggerInfo(101..110) receive; the embedded engines stand in for those
// hosts, so they ask for it here.
TEST(RegexpWildcardsTest, LowerCasesEveryEntryIncludingTheWholeMatch) {
    Regexp r("(\\w+)\\s+(\\w+)");
    ASSERT_NE(nullptr, r.get());
    ASSERT_NE(0, regexec(r.get(), "Hello BIG World"));

    std::vector<std::string> wc;
    r->GetCallbackWildcards(true, wc);

    ASSERT_EQ(3u, wc.size());
    EXPECT_EQ("hello big", wc[0]);         // the whole match is folded too...
    EXPECT_EQ("hello",     wc[1]);         // ...and so is every capture
    EXPECT_EQ("big",       wc[2]);
}

// The flag must be genuinely optional: with it clear, case survives untouched.
// (Assert the negative as well as the positive, or a helper that always folds
// would pass the test above.)
TEST(RegexpWildcardsTest, PreservesCaseWhenNotAsked) {
    Regexp r("(\\w+)");
    ASSERT_NE(nullptr, r.get());
    ASSERT_NE(0, regexec(r.get(), "MiXeD"));

    std::vector<std::string> wc;
    r->GetCallbackWildcards(false, wc);

    ASSERT_EQ(2u, wc.size());
    EXPECT_EQ("MiXeD", wc[0]);
    EXPECT_EQ("MiXeD", wc[1]);
}

// A pattern with no captures still yields one entry: the whole match.
TEST(RegexpWildcardsTest, NoCapturesStillYieldsTheWholeMatch) {
    Regexp r("wor\\w+");
    ASSERT_NE(nullptr, r.get());
    ASSERT_NE(0, regexec(r.get(), "hello world"));

    std::vector<std::string> wc;
    r->GetCallbackWildcards(false, wc);

    ASSERT_EQ(1u, wc.size());
    EXPECT_EQ("world", wc[0]);
}

// The out-parameter is cleared, not appended to -- a caller reusing one vector
// across matches must not accumulate.
TEST(RegexpWildcardsTest, ClearsTheOutputVectorFirst) {
    Regexp r("(\\w+)");
    ASSERT_NE(nullptr, r.get());
    ASSERT_NE(0, regexec(r.get(), "second"));

    std::vector<std::string> wc;
    wc.push_back("stale");
    wc.push_back("also stale");
    r->GetCallbackWildcards(false, wc);

    ASSERT_EQ(2u, wc.size());
    EXPECT_EQ("second", wc[0]);
}

// A regexp whose program pcre cannot be interrogated (catastrophic backtracking
// NULLs m_program -- see ExecOnErroredProgramEarlyOutsFalse) must degrade to
// "the whole match only", never loop over an uninitialised capture count.
TEST(RegexpWildcardsTest, ErroredProgramDegradesToASingleEmptyEntry) {
    Regexp r("(a+)+$");
    ASSERT_NE(nullptr, r.get());
    std::string s(50, 'a');
    s += '!';
    EXPECT_EQ(0, regexec(r.get(), s.c_str()));
    ASSERT_TRUE(r->m_program == nullptr);

    std::vector<std::string> wc;
    r->GetCallbackWildcards(false, wc);

    ASSERT_EQ(1u, wc.size());
    EXPECT_EQ("", wc[0]);
}

// ── CMUSHclientDoc::RecallText ─────────────────────────────────────────────
//
// doc.cpp: RecallText compiles a fresh t_regexp (regcomp) on every call when
// bRegexp is set, but the compiled program was never deleted on the way out
// -- neither the normal fall-through nor the catch(CException*) exit freed
// it, so every regexp recall (Edit > Find with "regular expression" ticked)
// leaked one compiled PCRE program. The observable is memory, same shape as
// LuaScripting.StringReturningMethodsDoNotLeakTheirVariant: a large loop with
// a generous threshold well clear of allocator noise.
TEST(RegexpTest, RecallTextDoesNotLeakItsCompiledRegexp) {
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;

    // Populate the output buffer with enough lines for RecallText to walk
    // (and find) on every call.
    for (int i = 0; i < 50; i++) {
        CString line;
        line.Format("haystack line %d\n", i);
        doc.DisplayMsg((LPCTSTR) line, line.GetLength(), 0);
    }

    auto rss = [] () -> long {
        // ru_maxrss is a high-water mark, which is what we want: it cannot
        // fall back and hide a leak that has already happened.
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        return (long) ru.ru_maxrss;      // bytes on macOS, KB on Linux
    };

    auto recall = [&] () {
        doc.RecallText("haystack", false, true, false, true, false, 0, "");
    };

    for (int i = 0; i < 2000; i++)       // warm up the allocator
        recall();

    const long before = rss();
    for (int i = 0; i < 20000; i++)
        recall();
    const long after = rss();

#ifdef __APPLE__
    const long budget = 2L * 1024 * 1024;      // bytes
#else
    const long budget = 2L * 1024;             // KB
#endif
    EXPECT_LT(after - before, budget);
}


MC_TEST_MAIN()
