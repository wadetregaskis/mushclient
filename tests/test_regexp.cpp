// tests/test_regexp.cpp
// Unit tests for the PCRE regex wrapper (regexp.h / regexp.cpp).

#include "mc_test.h"

// Real doc.h layout (matches mushcore / mushcore_testsupport -- see the
// WIN32/_NOSOUND/MUSHCLIENT_FULL_BUILD comment in tests/CMakeLists.txt) is
// needed for the RecallText leak test below.
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "regexp.h"   // after the MFC headers: it uses CString and ASSERT
#include "mc_pcre.h"

#include "posix_compat.h"

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

// regexec, with the ONE platform difference in its error arm normalised.
//
// A PCRE *runtime* error -- catastrophic backtracking hitting the match limit,
// as opposed to a plain no-match -- frees and NULLs the compiled program on
// both platforms, and then regexp.cpp:109 THROWS on Windows where :119 returns
// false off it.  That divergence is deliberate and commented at the site: on
// Windows ThrowErrorException is how the client reports it, and the portable
// arm's "an errored attempt is a NON-match" exists so control cannot fall
// through to `return true` and fire a trigger on a line that never matched.
//
// Both mean the same thing to the tests below, which are about what happens
// AFTER the error -- the program is null, the error code is recorded, a second
// exec early-outs, the wildcard list degrades to one empty entry -- and all of
// that is identical on the two platforms.  So normalise the report and let the
// assertions stay shared.  A spurious MATCH still fails: it is returned, not
// thrown, and the callers assert 0.
static int regexec_erroring(t_regexp* re, const char* s)
{
#ifdef _WIN32
    try { return regexec(re, s); }
    catch (CException* e) { e->Delete(); return 0; }
#else
    return regexec(re, s);
#endif
}

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
    int result = regexec_erroring(r.get(), s.c_str());
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
    EXPECT_EQ(0, regexec_erroring(r.get(), s.c_str()));
    ASSERT_TRUE(r->m_program == nullptr);        // program was nulled on the error
    EXPECT_LT(r->m_iExecutionError, 0);
    const long attemptsAfterError = r->m_iMatchAttempts;

    // Second exec on the SAME errored program: the null-program guard returns
    // false BEFORE the m_iMatchAttempts++ at regexp.cpp:96. Drop the guard and the
    // return value is still 0 (pcre_exec(NULL) -> PCRE_ERROR_NULL, no crash), but the
    // attempt counter advances -- so the counter, not the return value, catches it.
    EXPECT_EQ(0, regexec_erroring(r.get(), "anything"));
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
    EXPECT_EQ(0, regexec_erroring(r.get(), s.c_str()));
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
    MC_SKIP_UNDER_SANITIZER("measures RSS growth; a sanitizer's redzones blow any budget");
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
#ifdef _WIN32
        // The debug CRT's live bytes, where a leaked t_regexp and its PCRE
        // program would sit.  Not the working set: under the x64 and x86
        // emulators on ARM64 Windows, each freshly JIT-compiled pattern is
        // translated anew and the translations are cached, so the working set
        // climbs ~0.4 KB per compile with nothing leaked (the CRT heap stays
        // flat, and so does the working set of a native ARM64 build).
        _CrtMemState st;
        _CrtMemCheckpoint(&st);
        return (long) ((st.lSizes[_NORMAL_BLOCK] + st.lSizes[_CLIENT_BLOCK]) / 1024);   // KB
#else
        // ru_maxrss is a high-water mark, which is what we want: it cannot
        // fall back and hide a leak that has already happened.
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        return (long) ru.ru_maxrss;      // bytes on macOS, KB on Linux
#endif
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

// ── The JIT (mc_pcre.c: on every platform, if it can get the memory) ──────

// regcomp asks for the JIT; where it can run, every trigger and alias pattern
// runs compiled.  Elsewhere the same call leaves the pattern interpreted and
// changes nothing else.
TEST(RegexpTest, PatternsAreJitCompiledWhereTheJitCanRun) {
    Regexp r("^You (?:hit|miss) (\\w+)");
    size_t jit = 0;
    ASSERT_EQ(0, pcre2_pattern_info(r->m_program, PCRE2_INFO_JITSIZE, &jit));
#ifdef _WIN32
    EXPECT_GT(jit, (size_t) 0);
#endif
    EXPECT_TRUE(regexec(r.get(), "You hit the troll"));
    EXPECT_EQ(std::string("the"), r->GetWildcard(1));
}

// ── Deep matches ──────────────────────────────────────────────────────────

// "ab...abc", n letters before the c: against ^(?:a|b)*c$ every letter is a
// repeat of the group, and so a frame of PCRE's interpreter.
static std::string DeepLine(int n)
{
    std::string s;
    for (int i = 0; i < n; i++)
        s += (i & 1) ? 'b' : 'a';
    return s + 'c';
}

// PCRE 8's interpreter recursed on the machine stack, so a long enough line
// took the client down: 1,000 letters in a Debug build, 20,000 in Release.
// PCRE2's keeps its frames on the heap.
TEST(RegexpTest, TheInterpreterTakesALineThatOverflowedTheStackInPcre8) {
    const char* error;
    int erroroffset;
    pcre2_code* code = mc_pcre_compile("^(?:a|b)*c$", 0, NULL, 0, &error, &erroroffset);
    ASSERT_NE(nullptr, code);
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(code, NULL);
    const std::string deep = DeepLine(200000);
    EXPECT_EQ(1, mc_pcre_match(code, &md, deep.c_str(), (int) deep.size(), 0, 0, NULL, NULL));
    int offsets[2];
    mc_pcre_get_offsets(md, offsets, 1);
    EXPECT_EQ((int) deep.size(), offsets[1]);
    // ... and it does not keep the frames for good
    EXPECT_GT(pcre2_get_match_data_heapframes_size(md), (PCRE2_SIZE) 1024 * 1024);
    const std::string shallow = DeepLine(20);
    EXPECT_EQ(1, mc_pcre_match(code, &md, shallow.c_str(), (int) shallow.size(), 0, 0, NULL, NULL));
    EXPECT_LT(pcre2_get_match_data_heapframes_size(md), (PCRE2_SIZE) 1024 * 1024);
    pcre2_match_data_free(md);
    pcre2_code_free(code);
}

// A trigger's view of such lines: the JIT takes the first on its own stack.  The
// second outgrows even 8 MB (PCRE2_ERROR_JIT_STACKLIMIT), so it is rerun in the
// interpreter, whose limit is the heap.
TEST(RegexpTest, ATriggerOnAVeryDeepLineMatches) {
    Regexp r("^(?:a|b)*c$");
    const std::string s = DeepLine(200000);
    EXPECT_TRUE(regexec(r.get(), s.c_str()));
    EXPECT_EQ(0, r->m_iExecutionError);
    EXPECT_EQ(s, r->GetWildcard(0));
#if defined(_WIN64) || defined(__LP64__)    // the 32-bit heap limit is a quarter of that
    const std::string t = DeepLine(1000000);
    EXPECT_TRUE(regexec(r.get(), t.c_str()));
    EXPECT_EQ(0, r->m_iExecutionError);
    EXPECT_EQ(t, r->GetWildcard(0));
    EXPECT_NE(nullptr, r->m_program);
#endif
}

// A match too deep for the JIT's stack is rerun.  The smallest stack PCRE makes,
// so a modest line outgrows it.
TEST(RegexpTest, AMatchTooDeepForTheJitStackIsRerunInTheInterpreter) {
    pcre2_match_context* context = mc_pcre_match_context();
    ASSERT_NE(nullptr, context);
    pcre2_jit_stack* tiny = pcre2_jit_stack_create(8 * 1024, 8 * 1024, NULL);
    ASSERT_NE(nullptr, tiny);
    pcre2_jit_stack_assign(context, NULL, tiny);
    const std::string s = DeepLine(100000);
    {
        Regexp r("^(?:a|b)*c$");
        EXPECT_TRUE(regexec(r.get(), s.c_str()));
        EXPECT_EQ(0, r->m_iExecutionError);
    }
    // A match too deep for that too is a runtime error like the match limit:
    // reported, the program dropped, no match -- never a crash.
    uint32_t limit = 0;
    pcre2_config(PCRE2_CONFIG_HEAPLIMIT, &limit);
    pcre2_set_heap_limit(context, 64);    // KB
    {
        Regexp r("^(?:a|b)*c$");
        EXPECT_EQ(0, regexec_erroring(r.get(), s.c_str()));
        EXPECT_EQ(PCRE_ERROR_RECURSIONLIMIT, r->m_iExecutionError);
        EXPECT_EQ(nullptr, r->m_program);
        EXPECT_EQ(std::string("Recursion Limit"),
                  std::string((LPCTSTR) Convert_PCRE_Runtime_Error(PCRE_ERROR_RECURSIONLIMIT)));
    }
    pcre2_set_heap_limit(context, limit);
    pcre2_jit_stack_assign(context, NULL, pcre2_jit_stack_create(32 * 1024, 8 * 1024 * 1024, NULL));
    pcre2_jit_stack_free(tiny);
}

// ── What scripts can see stays PCRE 8's ──────────────────────────────────

// These reach scripts: GetTriggerInfo (t, 28), GetAliasInfo, execution_error.  In
// PCRE2 the match limit is -47 and bad UTF-8 is -3 to -23.
TEST(RegexpTest, RuntimeErrorCodesAreStillPcre8s) {
    EXPECT_EQ(-8, PCRE_ERROR_MATCHLIMIT);
    EXPECT_EQ(-10, PCRE_ERROR_BADUTF8);
    EXPECT_EQ(-21, PCRE_ERROR_RECURSIONLIMIT);
    EXPECT_EQ(-27, PCRE_ERROR_JIT_STACKLIMIT);
    {
        Regexp r("(*LIMIT_MATCH=10)(a+)+$");
        std::string s(50, 'a');
        s += '!';
        EXPECT_EQ(0, regexec_erroring(r.get(), s.c_str()));
        EXPECT_EQ(-8, r->m_iExecutionError);
    }
    {
        Regexp r("a", PCRE_UTF8);
        EXPECT_EQ(0, regexec_erroring(r.get(), "a\xff"));
        EXPECT_EQ(-10, r->m_iExecutionError);
    }
    {
        Regexp r("(a+)+$");
        std::string s(50, 'a');
        s += '!';
        EXPECT_EQ(0, regexec_erroring(r.get(), s.c_str()));
        EXPECT_EQ(-8, r->m_iExecutionError);
    }
}

TEST(RegexpTest, OptionBitsAreStillPcre8s) {
    EXPECT_EQ(1, PCRE_CASELESS);
    EXPECT_EQ(2, PCRE_MULTILINE);
    EXPECT_EQ(0x800, PCRE_UTF8);
    {
        Regexp r("hello", PCRE_CASELESS);
        EXPECT_TRUE(regexec(r.get(), "say HeLLo"));
    }
    {
        Regexp r("^b$", PCRE_MULTILINE);
        EXPECT_TRUE(regexec(r.get(), "a\nb\nc"));
    }
    {
        Regexp r("^.$", PCRE_UTF8);
        EXPECT_TRUE(regexec(r.get(), "\xc3\xa9"));
    }
    {
        Regexp r("^.$");    // raw bytes, in a world that isn't UTF-8
        EXPECT_FALSE(regexec(r.get(), "\xc3\xa9"));
        EXPECT_TRUE(regexec(r.get(), "\xe9"));
    }
    // pcre_compile refused what isn't a compile option
    t_regexp* notcompiled = NULL;
    try { notcompiled = regcomp("a", PCRE_NOTBOL); }
    catch (CException* e) { e->Delete(); }
    EXPECT_EQ(nullptr, notcompiled);
    delete notcompiled;
}

// ── Patterns PCRE 8 took, that PCRE2 refuses or reads differently ─────────

// (pattern, subject, what it must match)
static void expect_matches(const char* pattern, const char* subject, const char* expected, int options = 0) {
    Regexp r(pattern, options);
    EXPECT_TRUE(regexec(r.get(), subject));
    EXPECT_EQ(std::string(expected), r->GetWildcard(0));
}

static bool compiles(const char* pattern) {
    t_regexp* r = NULL;
    try { r = regcomp(pattern); }
    catch (CException* e) { e->Delete(); }
    const bool ok = r != NULL;
    delete r;
    return ok;
}

TEST(RegexpCompat, ALetterWithNoMeaningIsThatLetter) {
    expect_matches("a\\jb", "xajby", "ajb");
    expect_matches("\\y+", "ayyy", "yyy");
    expect_matches("[\\j]", "xj", "j");
    expect_matches("[a\\qz]+", "xaqz", "aqz");
    expect_matches("\\i\\I\\J\\m\\M\\O\\T\\Y", "iIJmMOTY", "iIJmMOTY");
    // ... and in a class, these too
    expect_matches("[\\A\\B\\C\\G\\K\\R\\X\\Z\\z]+", "ABCGKRXZz", "ABCGKRXZz");
    // while PCRE 8 refused these, and so must we
    EXPECT_FALSE(compiles("\\u0041"));
    EXPECT_FALSE(compiles("\\U00000041"));
    EXPECT_FALSE(compiles("\\L"));
    EXPECT_FALSE(compiles("\\p{Foo}"));
    EXPECT_FALSE(compiles("[z-a]"));
    EXPECT_FALSE(compiles("(a"));
    EXPECT_FALSE(compiles("[a-\\d]"));
}

TEST(RegexpCompat, AHyphenAfterASetIsAHyphen) {
    expect_matches("[\\w-_]+", "!a-_b!", "a-_b");
    expect_matches("[\\d-a]+", "x1-ax", "1-a");
    expect_matches("[[:alpha:]-z]+", "1ab-z1", "ab-z");
    expect_matches("[\\s-x]+", "a x-a", " x-");
    expect_matches("[^\\d-a]+", "1-a!b1", "!b");
    // what was always right stays as it was
    expect_matches("[a-c]+", "xabcx", "abc");
    expect_matches("[a-c-e]+", "xa-ex", "a-e");
    expect_matches("[+--]+", "a+,-a", "+,-");
    expect_matches("[\\Qa-c\\E]+", "b-a", "-a");
}

TEST(RegexpCompat, ABackslashEightOrNineWithNoSuchGroupIsTheDigit) {
    expect_matches("a\\8", "xa8", "a8");
    expect_matches("a\\9b", "xa9b", "a9b");
    expect_matches("\\89", "x89", "89");
    expect_matches("(a)(b)(c)(d)(e)(f)(g)(h)\\8", "abcdefghh", "abcdefghh");
    expect_matches("(a)\\1", "aa", "aa");
}

TEST(RegexpCompat, BracesPcre8ReadAsTextAreText) {
    expect_matches("x{,3}", "xxx x{,3}", "x{,3}");
    expect_matches("x{ 2 }", "xx x{ 2 }", "x{ 2 }");
    expect_matches("x{2, 3}", "xxx x{2, 3}", "x{2, 3}");
    expect_matches("x{2 ,3}", "xxx x{2 ,3}", "x{2 ,3}");
    expect_matches("(x){,3}y", "xxxy (x){,3}y x{,3}y", "x{,3}y");
    // real quantifiers stay quantifiers
    expect_matches("x{2}", "xxx", "xx");
    expect_matches("x{2,}", "xxx", "xxx");
    expect_matches("x{1,2}", "xxx", "xx");
    // not touched in a class, an escape, or a comment
    expect_matches("[{,3}]+", "x{,3}", "{,3}");
    expect_matches("\\x{41}{,2}", "A{,2}", "A{,2}");
    expect_matches("\\Qx{,3}\\E", "x{,3}", "x{,3}");
    expect_matches("(?#{,3})a{,2}", "a{,2}", "a{,2}");
    expect_matches("(?x) a # {,3} [\n {,2}", "a{,2}", "a{,2}");
    expect_matches("(?x: a # [ \n b) c#", "ab c#", "ab c#");
    expect_matches("(?x: a # [ \n \\j) c{,2}", "aj c{,2}", "aj c{,2}");
}

TEST(RegexpCompat, KInALookaround) {
    expect_matches("foo\\Kbar", "foobar", "bar");
    {
        Regexp r("foo(?=bar\\K)");
        EXPECT_TRUE(regexec(r.get(), "foobar"));
        EXPECT_EQ(std::string(""), r->GetWildcard(0));   // the end is before the start
    }
}

TEST(RegexpCompat, NewlyAcceptedIsHarmless) {
    expect_matches("(?<=a{1,3})b", "aab", "b");
    expect_matches("(?<aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa>x)", "x", "x");
}

// A pattern PCRE2 takes as it stands is never touched; the suspect ones are the
// smallest change that does.
TEST(RegexpCompat, TheRewrite) {
    struct { const char* in; const char* out; } cases[] = {
        { "abc",            NULL },
        { "[a-z]+\\d{2}",   NULL },
        { "\\j",            "j" },
        { "[\\w-_]",        "[\\w\\-_]" },
        { "x{,3}",          "x\\{,3}" },
        { "x{ 2 }",         "x\\{ 2 }" },
        { "x{2}",           NULL },
        { "\\8",            "8" },
        { "(?X)a",          "(?)a" },
        { "\\x",            "\\x00" },
    };
    for (auto& c : cases) {
        char* got = mc_pcre_rewrite(c.in, 0);
        if (c.out == NULL)
            EXPECT_EQ(nullptr, got);
        else {
            ASSERT_NE(nullptr, got);
            EXPECT_EQ(std::string(c.out), std::string(got));
        }
        free(got);
    }
    // PCRE_EXTRA is PCRE 8's strict mode: \j stays an error
    EXPECT_EQ(nullptr, mc_pcre_rewrite("\\j", PCRE_EXTRA));
}

// The complaint is about what is actually wrong, at the right place, whatever the
// rewrite did on the way.
TEST(RegexpCompat, AFailedCompileReportsThePatternAsWritten) {
    const char* error = NULL;
    int offset = -1;
    EXPECT_EQ(nullptr, mc_pcre_compile("\\j[a", 0, NULL, 0, &error, &offset));
    ASSERT_NE(nullptr, error);
    EXPECT_NE(std::string::npos, std::string(error).find("missing terminating ]"));
    EXPECT_EQ(4, offset);
}

// ── The required-literal prefilter (mc_pcre_required_literal, regexec) ────
//
// A literal is only ever claimed when every match must contain it: a wrong one
// would make a trigger miss lines, silently.  tools/trigger_bench checks the rule
// against every pattern in the tree and Quow's run-time triggers; these pin the
// shape of it.

static std::string RequiredLiteral(const char* pattern, int options = 0) {
    char out[32];
    const int n = mc_pcre_required_literal(pattern, options, out, sizeof out);
    return std::string(out, n);
}

TEST(RegexpPrefilter, TheLiteralIsALongestRunEveryMatchHas) {
    const int I = PCRE_CASELESS, U = PCRE_UTF8;
    const struct { const char* pattern; int options; const char* literal; } cases[] = {
        { "^You have (\\d+) items$",                     0, "You have " },
        { "^(?:> )?(?P<who>(?:\\w+\\b\\W*?){1,8}) has no arcane(?: or divine)? protection\\.$",
                                                         0, " has no arcane" },
        { "abc|def",                                     0, "" },     // either will do
        { "(?:abc|xyz)def",                              0, "def" },  // not inside the alternation
        { "(?:abc)?defg",                                0, "defg" }, // not inside an optional group
        { "(?:abcd)+ef",                                 0, "ef" },   // nor a repeated one
        { "(abcd)ef",                                    0, "abcd" }, // a required group counts
        { "(?<name>abcd)ef",                             0, "abcd" },
        { "(?>abcd)ef",                                  0, "abcd" },
        { "(?=abcd)ef",                                  0, "ef" },   // a lookahead does not
        { "(?<!abcd)ef",                                 0, "ef" },
        { "xab+cde",                                     0, "cde" },  // b+ ends the run "xa"
        { "abcx?de",                                     0, "abc" },
        { "abcx{2}de",                                   0, "abc" },
        { "abcx{2,}de",                                  0, "abc" },
        { "a.bc[xyz]def\\dgh",                           0, "def" },
        { "[]abc]xy",                                    0, "xy" },   // ] first in a class
        { "[^]a]xy",                                     0, "xy" },
        { "[[:alpha:]]xy",                               0, "xy" },
        { "a\\.b\\(c",                                   0, "a.b(c" },
        { "\\x41xyz",                                    0, "xyz" },  // (\x41BC would lose BC: safe)
        { "\\x{41}BC",                                   0, "BC" },
        { "(?<n>x)\\k<n>abcd",                           0, "abcd" },
        { "(x)\\g{-1}abcd",                              0, "abcd" },
        { "(?P<n>ab)(?P=n)cdef",                         0, "cdef" },
        { "(?P<n>ab)(?P>n)?cdef",                        0, "cdef" },
        { "\\p{Lu}abc",                                  0, "abc" },
        { "\\babc\\b",                                   0, "abc" },
        { "ABC",                                         I, "abc" },  // caseless: lower case
        // anything not understood: nothing at all
        { "(?i)abc",                                     0, "" },
        { "a(?i:b)cd",                                   0, "" },
        { "(*UTF)abc",                                   0, "" },
        { "abc(*ACCEPT)def",                             0, "" },
        { "\\Qabc\\E",                                   0, "" },
        { "abc{,3}de",                                   0, "" },     // {,3}: text to PCRE 8
        { "abc{ 3 }de",                                  0, "" },
        { "abc(?#note)",                                 0, "" },
        { "(?(1)a|b)cd",                                 0, "" },
        { "abc(?R)?",                                    0, "" },
        { "\\yabc",                                      0, "" },     // a letter with no meaning
        { "abcd",                         PCRE_EXTENDED,    "" },
        // UTF-8: a character outside ASCII ends a run, quantified or not
        { "caf\xC3\xA9+ au lait",                        U, " au lait" },
        { "caf\xC3\xA9 au lait",                         0, "caf\xC3\xA9 au lait" },
        // caseless UTF-8: K and S also match the Kelvin sign and the long s
        { "Kelvins",                                 I | U, "elvin" },
        { "Kelvins",                                     I, "kelvins" },
        // non-ASCII is not taken caseless
        { "caf\xE9s",                                    I, "caf" },
        // a long run is kept in pieces
        { "There is a sudden white flash\\.  Your magical shield has broken",
                                                         0, "There is a sudden white flash.  " },
    };
    for (auto& c : cases)
        EXPECT_EQ(std::string(c.pattern) + " -> " + c.literal,
                  std::string(c.pattern) + " -> " + RequiredLiteral(c.pattern, c.options));
}

// An alternation whose every branch has a literal gives the set of them, any one
// of which a match must contain.
static std::string RequiredLiterals(const char* pattern, int options = 0) {
    char out[16 * 32];
    int lengths[16];
    const int n = mc_pcre_required_literals(pattern, options, out, sizeof out, lengths, 16);
    std::string r;
    for (int i = 0, at = 0; i < n; at += lengths[i++])
        r += (i ? "|" : "") + std::string(out + at, lengths[i]);
    return r;
}

TEST(RegexpPrefilter, AnAlternationGivesASetOfLiterals) {
    const struct { const char* pattern; const char* literals; } cases[] = {
        { "abc|def",                                      "abc|def" },
        { "^(?:> )?(?>Exploiting your confusion, |Catching you by surprise, )x",
                                                          "Exploiting your confusion, |Catching you by surprise, " },
        { "(?:ab|cd)ef",                                  "ef" },        // a single literal is cheaper
        { "(?:abcd|efgh)x",                               "abcd|efgh" }, // longer than x
        { "abcde(?:fg|hi)",                               "abcde" },     // four or more: alone
        { "abc|d.e",                                      "abc|d" },     // each branch its best
        { "abc|\\d+",                                     "" },          // a branch with none
        { "(?:abc|xyz)?def",                              "def" },       // an optional group is no use
        { "(?=abc|xyz)def",                               "def" },
        { "(a(?:bc|de)f|gh)i",                            "bc|de|gh" },  // nested: a branch's best
        { "a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q",           "" },          // more than sixteen
    };
    for (auto& c : cases)
        EXPECT_EQ(std::string(c.pattern) + " -> " + c.literals,
                  std::string(c.pattern) + " -> " + RequiredLiterals(c.pattern));
}

// A pattern prefiltered on a set: no match unless one of them is there.
TEST(RegexpPrefilter, ASetSkipsOnlyWhenNoneIsThere) {
    Regexp r("^(?:> )?(?>Exploiting your confusion|Catching you by surprise), (.+)$");
    ASSERT_EQ(2u, r->m_vRequired.size());
    EXPECT_EQ(0, regexec(r.get(), "The rat bites you."));
    EXPECT_EQ(1, regexec(r.get(), "> Catching you by surprise, the rat bites."));
    EXPECT_EQ(1, regexec(r.get(), "Exploiting your confusion, the rat bites."));
    EXPECT_EQ(3, r->m_iMatchAttempts);
}

TEST(RegexpPrefilter, TheSearchFindsTheLiteralWhereverItIs) {
    const char* lit = "arcane";
    for (int anchor = 0; anchor < 6; anchor++) {
        EXPECT_TRUE(mc_pcre_has_literal("arcane", 6, lit, 6, 0, anchor));
        EXPECT_TRUE(mc_pcre_has_literal("has no arcane protection", 24, lit, 6, 0, anchor));
        EXPECT_TRUE(mc_pcre_has_literal("xxarcane", 8, lit, 6, 0, anchor));
        EXPECT_FALSE(mc_pcre_has_literal("arcan", 5, lit, 6, 0, anchor));
        EXPECT_FALSE(mc_pcre_has_literal("arcanX arcan", 12, lit, 6, 0, anchor));
        EXPECT_FALSE(mc_pcre_has_literal("ARCANE", 6, lit, 6, 0, anchor));
        EXPECT_TRUE(mc_pcre_has_literal("ARCANE", 6, lit, 6, 1, anchor));
        EXPECT_TRUE(mc_pcre_has_literal("no ArCaNe!", 10, lit, 6, 1, anchor));
        EXPECT_FALSE(mc_pcre_has_literal("no ArCaN!", 9, lit, 6, 1, anchor));
    }
    // a byte that only folds to a letter by |0x20 is not that letter
    EXPECT_FALSE(mc_pcre_has_literal("[", 1, "{", 1, 1, 0));
    EXPECT_FALSE(mc_pcre_has_literal("A", 1, "a", 1, 0, 0));
    // the rarest byte is looked for first
    EXPECT_EQ(1, mc_pcre_literal_anchor(" has no", 7, 0));     // the h
    EXPECT_EQ(2, mc_pcre_literal_anchor(" slithers", 9, 0));   // the l, not the space
}

// What a plugin can see of a prefiltered attempt is what it saw before: no
// match, the attempt counted, the last match's text kept.
TEST(RegexpPrefilter, ASkippedAttemptIsCountedAsANoMatch) {
    Regexp r("^You have (\\d+) items$");
    ASSERT_FALSE(r->m_vRequired.empty());
    EXPECT_EQ(1, regexec(r.get(), "You have 3 items"));
    EXPECT_EQ(std::string("3"), r->GetWildcard(1));
    const long attempts = r->m_iMatchAttempts;
    for (int i = 0; i < 100; i++)
        EXPECT_EQ(0, regexec(r.get(), "The rat bites you."));
    EXPECT_EQ(attempts + 100, r->m_iMatchAttempts);
    EXPECT_EQ(std::string("You have 3 items"), r->m_sTarget);
    EXPECT_EQ(std::string("3"), r->GetWildcard(1));
    EXPECT_GE(r->iTimeTaken, 0);
}

// In UTF-8 mode, a subject that isn't valid UTF-8 is still an error, as it was:
// the prefilter stands aside for anything not plain ASCII.
TEST(RegexpPrefilter, BadUtf8IsStillAnErrorInUtf8Mode) {
    Regexp r("abc", PCRE_UTF8);
    ASSERT_FALSE(r->m_vRequired.empty());
    EXPECT_EQ(0, regexec(r.get(), "plain ascii"));      // skipped: no error possible
    EXPECT_EQ(0, r->m_iExecutionError);
    EXPECT_EQ(0, regexec_erroring(r.get(), "bad \xFF byte"));
    EXPECT_EQ(PCRE_ERROR_BADUTF8, r->m_iExecutionError);
}


MC_TEST_MAIN()
