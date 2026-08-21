// tests/mc_test.h
// Minimal self-contained unit-test framework for MUSHclient tests.
//
// Provides a subset of the GoogleTest API so tests can be migrated to
// GTest later without changes.  No external dependencies.
//
// Usage:
//   #include "mc_test.h"
//   TEST(SuiteName, TestName) { EXPECT_EQ(1, 1); }
//   MC_TEST_MAIN()   // put in exactly one translation unit

#pragma once

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <sstream>
#include <type_traits>
#include <utility>
#include <functional>
#include <exception>   // std::exception (run_all catches an escaping throw)

namespace mc_test {

// ── Result accumulator ───────────────────────────────────────────────────

struct Result {
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    bool current_failed = false;
    const char* current_skip = nullptr;   // reason, or null if not skipped
    const char* current_suite = "";
    const char* current_name  = "";
};
inline Result& g() { static Result r; return r; }

// ── Registration ─────────────────────────────────────────────────────────

struct TestCase {
    const char* suite;
    const char* name;
    std::function<void()> fn;
};
inline std::vector<TestCase>& cases() {
    static std::vector<TestCase> v;
    return v;
}
struct Registrar {
    Registrar(const char* s, const char* n, std::function<void()> f) {
        cases().push_back({s, n, f});
    }
};

// ── Runner ────────────────────────────────────────────────────────────────

inline int run_all() {
    for (auto& tc : cases()) {
        g().current_suite  = tc.suite;
        g().current_name   = tc.name;
        g().current_failed = false;
        g().current_skip   = nullptr;
        // An exception that escapes a test is a FAILED TEST, not a dead run.
        // Without this the whole process dies at the first one and every suite
        // after it is silently not run -- which is exactly what happened on
        // Windows, where the engine throws MFC exceptions by design (a NULL
        // POSITION into CPtrList::GetAt asserts and then throws
        // CInvalidArgException*).  One such walk was costing this target ~220
        // tests a run, silently: the summary line prints after the loop, so a
        // dead run has no summary to notice the absence in.
        //
        // catch(...) rather than catch(CException*): mc_test.h is included
        // BEFORE stdafx.h by every test file, so the MFC exception classes are
        // incomplete here and a handler may not name a pointer to an incomplete
        // type.  The name of the test is the diagnosis anyway.
        try {
            tc.fn();
        } catch (const std::exception& e) {
            g().current_failed = true;
            fprintf(stderr, "  uncaught std::exception: %s\n", e.what());
        } catch (...) {
            g().current_failed = true;
            fprintf(stderr, "  uncaught exception escaped the test body\n");
        }
        if (g().current_failed) {
            ++g().failed;
            fprintf(stderr, "[FAIL] %s.%s\n", tc.suite, tc.name);
        } else if (g().current_skip) {
            ++g().skipped;
            fprintf(stdout, "[SKIP] %s.%s (%s)\n", tc.suite, tc.name, g().current_skip);
        } else {
            ++g().passed;
            fprintf(stdout, "[PASS] %s.%s\n", tc.suite, tc.name);
        }
    }
    if (g().skipped)
        fprintf(stdout, "\n%d passed, %d failed, %d skipped\n",
                g().passed, g().failed, g().skipped);
    else
        fprintf(stdout, "\n%d passed, %d failed\n", g().passed, g().failed);
    return g().failed ? 1 : 0;
}

// ── Running under a sanitizer? ────────────────────────────────────────────
//
// A few tests in this suite measure the ALLOCATOR rather than the code: live
// block counts (malloc_zone_statistics / mallinfo2), RSS growth budgets, or
// whether a just-freed address is handed straight back by the next allocation.
// A sanitizer replaces the allocator wholesale -- redzones around every block,
// and a quarantine that deliberately does NOT reuse freed addresses -- so those
// measurements stop meaning what the test needs them to mean.  The code under
// test is fine; the instrument is gone.
//
// Such a test says so with MC_SKIP_UNDER_SANITIZER, which prints a visible
// [SKIP] line and counts it, rather than passing vacuously.  Nothing else is
// skipped: finding the memory errors those tests could only approximate is the
// whole reason to run a sanitizer.  See docs/SANITIZERS.md.
#if defined(__has_feature)
# if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) \
  || __has_feature(memory_sanitizer)
#  define MC_TEST_SANITIZED 1
# endif
#endif
#if !defined(MC_TEST_SANITIZED) \
 && (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__))
# define MC_TEST_SANITIZED 1
#endif
#ifndef MC_TEST_SANITIZED
# define MC_TEST_SANITIZED 0
#endif

inline void skip_current(const char* reason) { g().current_skip = reason; }

// Skip the enclosing test when a sanitizer owns the allocator.  `reason` must
// say what the test measures, not merely that it is skipped.
#define MC_SKIP_UNDER_SANITIZER(reason)                                       \
    do { if (MC_TEST_SANITIZED) { mc_test::skip_current(reason); return; } }  \
    while (0)

// ── Assertion helpers ─────────────────────────────────────────────────────

inline void fail_msg(const char* file, int line, const char* msg) {
    fprintf(stderr, "  %s:%d: %s\n", file, line, msg);
    g().current_failed = true;
}

template<typename A, typename B>
inline bool cmp_eq(const A& a, const B& b) { return a == b; }
inline bool cmp_eq(const char* a, const char* b) {
    return a == b || (a && b && strcmp(a, b) == 0);
}

// ── Value rendering for failure messages ─────────────────────────────────
//
// Best-effort human-readable rendering of a compared value, so a failure
// inside a table- or loop-driven assertion shows WHICH input produced it,
// not just the source text of the expression.  Falls back to a placeholder
// for types with no operator<< (EXPECT_EQ etc. are used with every kind of
// value in the suite, including plain structs), rather than failing to
// compile.
template<typename T, typename = void>
struct has_ostream_op : std::false_type {};
template<typename T>
struct has_ostream_op<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template<typename T>
inline std::string describe(const T& v) {
    if constexpr (has_ostream_op<T>::value) {
        std::ostringstream oss;
        oss << v;
        return oss.str();
    } else {
        (void)v;
        return "<?>";
    }
}

} // namespace mc_test

// ── Public macros ─────────────────────────────────────────────────────────

// Register a test
#define TEST(suite, name) \
    static void mc_test_fn_##suite##_##name(); \
    static mc_test::Registrar mc_test_reg_##suite##_##name( \
        #suite, #name, mc_test_fn_##suite##_##name); \
    static void mc_test_fn_##suite##_##name()

// Assertions
#define EXPECT_TRUE(expr) \
    do { if (!(expr)) \
        mc_test::fail_msg(__FILE__, __LINE__, "Expected true: " #expr); \
    } while(0)

#define EXPECT_FALSE(expr) \
    do { if (!!(expr)) \
        mc_test::fail_msg(__FILE__, __LINE__, "Expected false: " #expr); \
    } while(0)

#define EXPECT_EQ(expected, actual) \
    do { if (!mc_test::cmp_eq((expected), (actual))) { \
        char buf[512]; \
        snprintf(buf, sizeof(buf), "Expected " #expected " == " #actual \
                 " (%s vs %s)", mc_test::describe(expected).c_str(), \
                 mc_test::describe(actual).c_str()); \
        mc_test::fail_msg(__FILE__, __LINE__, buf); \
    }} while(0)

#define EXPECT_NE(a, b) \
    do { if (mc_test::cmp_eq((a),(b))) { \
        char buf[512]; \
        snprintf(buf, sizeof(buf), "Expected " #a " != " #b \
                 " (%s vs %s)", mc_test::describe(a).c_str(), \
                 mc_test::describe(b).c_str()); \
        mc_test::fail_msg(__FILE__, __LINE__, buf); \
    }} while(0)

#define EXPECT_GT(a, b) \
    do { if (!((a) > (b))) { \
        char buf[512]; \
        snprintf(buf, sizeof(buf), "Expected " #a " > " #b \
                 " (%s vs %s)", mc_test::describe(a).c_str(), \
                 mc_test::describe(b).c_str()); \
        mc_test::fail_msg(__FILE__, __LINE__, buf); \
    }} while(0)

#define EXPECT_GE(a, b) \
    do { if (!((a) >= (b))) { \
        char buf[512]; \
        snprintf(buf, sizeof(buf), "Expected " #a " >= " #b \
                 " (%s vs %s)", mc_test::describe(a).c_str(), \
                 mc_test::describe(b).c_str()); \
        mc_test::fail_msg(__FILE__, __LINE__, buf); \
    }} while(0)

#define EXPECT_LT(a, b) \
    do { if (!((a) < (b))) { \
        char buf[512]; \
        snprintf(buf, sizeof(buf), "Expected " #a " < " #b \
                 " (%s vs %s)", mc_test::describe(a).c_str(), \
                 mc_test::describe(b).c_str()); \
        mc_test::fail_msg(__FILE__, __LINE__, buf); \
    }} while(0)

#define EXPECT_LE(a, b) \
    do { if (!((a) <= (b))) { \
        char buf[512]; \
        snprintf(buf, sizeof(buf), "Expected " #a " <= " #b \
                 " (%s vs %s)", mc_test::describe(a).c_str(), \
                 mc_test::describe(b).c_str()); \
        mc_test::fail_msg(__FILE__, __LINE__, buf); \
    }} while(0)

#define EXPECT_STREQ(expected, actual) \
    do { if (!mc_test::cmp_eq((const char*)(expected), (const char*)(actual))) { \
        char buf[512]; \
        snprintf(buf, sizeof(buf), "Expected STREQ: \"%s\" vs \"%s\"", \
                 (const char*)(expected), (const char*)(actual)); \
        mc_test::fail_msg(__FILE__, __LINE__, buf); \
    }} while(0)

#define EXPECT_STRNE(a, b) \
    do { if (mc_test::cmp_eq((const char*)(a), (const char*)(b))) \
        mc_test::fail_msg(__FILE__, __LINE__, "Expected STRNE: " #a " != " #b); \
    } while(0)

#define EXPECT_NEAR(expected, actual, tol) \
    do { if (std::fabs((double)(expected) - (double)(actual)) > (double)(tol)) { \
        char buf[256]; \
        snprintf(buf, sizeof(buf), "Expected |" #expected " - " #actual "| <= " #tol \
                 " (diff=%.6g)", std::fabs((double)(expected)-(double)(actual))); \
        mc_test::fail_msg(__FILE__, __LINE__, buf); \
    }} while(0)

#define ASSERT_NE(a, b) \
    do { if (mc_test::cmp_eq((a),(b))) { \
        mc_test::fail_msg(__FILE__, __LINE__, "Assert failed: " #a " != " #b); \
        return; \
    }} while(0)

#define ASSERT_EQ(expected, actual) \
    do { if (!mc_test::cmp_eq((expected),(actual))) { \
        mc_test::fail_msg(__FILE__, __LINE__, "Assert failed: " #expected " == " #actual); \
        return; \
    }} while(0)

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) { \
        mc_test::fail_msg(__FILE__, __LINE__, "Assert failed: " #expr); \
        return; \
    }} while(0)

#define ASSERT_FALSE(expr) \
    do { if (expr) { \
        mc_test::fail_msg(__FILE__, __LINE__, "Assert failed: !(" #expr ")"); \
        return; \
    }} while(0)

// The ordering ASSERTs, for the same reason as ASSERT_EQ/ASSERT_NE: when a
// later assertion would dereference or index using the value, carrying on past
// a failure turns a clear message into a crash.
#define ASSERT_GT(a, b) \
    do { if (!((a) > (b))) { \
        mc_test::fail_msg(__FILE__, __LINE__, "Assert failed: " #a " > " #b); \
        return; \
    }} while(0)

#define ASSERT_GE(a, b) \
    do { if (!((a) >= (b))) { \
        mc_test::fail_msg(__FILE__, __LINE__, "Assert failed: " #a " >= " #b); \
        return; \
    }} while(0)

#define ASSERT_LT(a, b) \
    do { if (!((a) < (b))) { \
        mc_test::fail_msg(__FILE__, __LINE__, "Assert failed: " #a " < " #b); \
        return; \
    }} while(0)

#define ASSERT_LE(a, b) \
    do { if (!((a) <= (b))) { \
        mc_test::fail_msg(__FILE__, __LINE__, "Assert failed: " #a " <= " #b); \
        return; \
    }} while(0)

// Saves a process-global g_wxhook_* pointer and restores it in the
// destructor, so an ASSERT_* between the install and a manual reset (which
// returns out of the whole TEST body, not just past the reset line) cannot
// leave the hook installed for every later TEST() in this binary.
template <typename T>
struct HookGuard {
    T& slot;
    T  saved;
    explicit HookGuard(T& s) : slot(s), saved(s) {}
    ~HookGuard() { slot = saved; }
};

// Entry point — put in exactly one .cpp in each test executable.
//
// Ignoring SIGPIPE mirrors what the product does at startup
// (platform/wx_app.cpp): a write to a peer-closed socket must report EPIPE,
// not kill the process.  Tests that drive raw sockets of their own build them
// with plain ::socket/::accept and so get none of CAsyncSocket's per-socket
// SO_NOSIGPIPE; without this, one of those writes losing a timing race takes
// the whole runner down.  That was a real intermittent ctest failure
// (test_pal_transport, SIGPIPE — roughly 1 run in 25 under `ctest -j8`, and
// never reproducible when the binary was run on its own).
#ifndef _WIN32
#  include <csignal>
#  define MC_TEST_IGNORE_SIGPIPE() ::signal(SIGPIPE, SIG_IGN)
#else
#  define MC_TEST_IGNORE_SIGPIPE() ((void)0)
#endif

// The real app fills the xterm-256 palette in CMUSHclientApp::InitInstance
// (MUSHclient.cpp), but the headless InitInstance is a stub, so under test
// xterm_256_colours[] was all zeros.  Assertions of the shape
// EXPECT_EQ(xterm_256_colours[196], style->iBackColour) were therefore comparing
// 0 with 0 and passed no matter what the ANSI parser did.  Fill it once here so
// every suite starts from the same state the product does.
void Generate256colours(void);

#ifdef _WIN32
// ── The Windows host ─────────────────────────────────────────────────────
// On Windows the engine compiles against REAL MFC — stdafx.h takes the
// afxwin.h arm, not platform/compat.h — so a test binary is an "MFC in a
// console application" host and has to do what WinMain would have done:
//
//   AfxWinInit    without it the first AfxGetApp() asserts in afxwin1.inl
//   AfxOleInit    CMUSHclientDoc is a CCmdTarget with EnableAutomation, and
//                 ~CCmdTarget calls CoDisconnectObject, which asserts in
//                 oleunk.cpp when COM is not initialised on the thread
//   CoInitializeEx  ...and AfxOleInit is NOT enough here.  Measured: after a
//                 successful AfxOleInit, CoInitializeEx returns S_OK -- not
//                 S_FALSE -- so COM was not in fact initialised on this
//                 thread, and both CoDisconnectObject and a plain
//                 CoCreateInstance answered CO_E_NOTINITIALIZED.  That was 69
//                 oleunk assertions per run, one per document destroyed, and
//                 they went to zero the moment this line was added.  Written
//                 as a hard failure rather than a hopeful extra call: if a
//                 future MFC does initialise COM here, this returns S_FALSE
//                 (which is not FAILED) and nothing changes.
//   AfxSocketInit CAsyncSocket, for the chat and transport suites
//
// And it must not pop dialogs.  A failed ASSERT in a test process is a test
// result, not something to wait for a human on: routed to stderr a run that
// trips one keeps going and says so, where the default modal box blocks the
// whole suite until somebody clicks it.  That is not hypothetical — it is how
// the first Windows run of this harness was found to be wedged.
#include <crtdbg.h>
// ── /tmp on Windows ────────────────────────────────────────────────────────
// 15 test files build scratch paths as "/tmp/mc_test_<pid>..." -- 120 of them.
// That is POSIX scaffolding around the assertions, never the thing under test,
// and on Windows a leading slash means "root of the current drive", so they all
// resolve to <drive>:\tmp\... and fail to open if that directory is absent.
// OpenLog then returns eCouldNotOpenFile and a test asserting on log CONTENT
// fails for a reason that has nothing to do with logging.
//
// Make the directory rather than rewrite 120 call sites in 15 shared files:
// this is a property of the host, and every one of those edits would be a
// change to a file macOS also compiles, for scaffolding.  Announce a failure --
// a run where every file-based test fails wants the reason at the top, not
// inferred from the wreckage.
#define MC_TEST_WIN_MAKE_TMP() \
    do { \
        if (!::CreateDirectoryA("/tmp", NULL) && \
            ::GetLastError() != ERROR_ALREADY_EXISTS) \
            fprintf(stderr, "WARNING: cannot create /tmp (error %lu); " \
                            "tests that write scratch files will fail\n", \
                    (unsigned long) ::GetLastError()); \
    } while(0)

// ── the InitInstance steps a headless host still needs ─────────────────────
// CMUSHclientApp's constructor does almost nothing; InitInstance is where the
// Windows app builds its global state, and InitInstance does not run here.
// The macOS side answers this in platform/headless_app.cpp's ctor -- these are
// the same two answers, in the same order, for the same reason.
//
//   * LoadGlobalsFromDatabase over NO open prefs database.  db_simple_query
//     returns SQLITE_CANTOPEN and the caller's default when App.db is null
//     (MUSHclient.cpp:1947), so every one of the ~77 global options takes its
//     table default from globalregistryoptions.cpp -- the real function and the
//     real table, not a hand-copied set of constants that would drift.  It also
//     means the run CANNOT read the developer's installed prefs, so it is
//     reproducible on any machine.  Without it App is merely zero-filled, and
//     an option whose Windows default is 1 reads as 0: RegexpMatchEmpty is one,
//     and it silently changed what every regexp in the suite matches.
//   * The three MXP tables (MUSHclient.cpp:750-752).  The XML parser resolves
//     &amp; and friends through App.m_EntityMap, so without this the parser
//     rejects a document containing any predefined entity -- and a world file's
//     custom entity would SHADOW a predefined one rather than lose to it.
#define MC_TEST_WIN_APP_STATE() \
    do { \
        App.LoadGlobalsFromDatabase(); \
        App.MXP_LoadElements(); \
        App.MXP_LoadEntities(); \
        App.MXP_LoadColours(); \
    } while(0)

#define MC_TEST_WIN_HOST_INIT() \
    do { \
        setvbuf(stdout, NULL, _IONBF, 0); \
        for (int rt_ = 0; rt_ < 3; rt_++) { \
            _CrtSetReportMode(rt_, _CRTDBG_MODE_FILE); \
            _CrtSetReportFile(rt_, _CRTDBG_FILE_STDERR); \
        } \
        if (!AfxWinInit(::GetModuleHandle(NULL), NULL, ::GetCommandLine(), 0)) \
            { fprintf(stderr, "FATAL: AfxWinInit failed\n"); return 2; } \
        if (!AfxOleInit()) \
            { fprintf(stderr, "FATAL: AfxOleInit failed\n"); return 2; } \
        if (FAILED(::CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) \
            { fprintf(stderr, "FATAL: CoInitializeEx failed\n"); return 2; } \
        AfxSocketInit(); \
        MC_TEST_WIN_MAKE_TMP(); \
        MC_TEST_WIN_APP_STATE(); \
    } while(0)
#else
#define MC_TEST_WIN_HOST_INIT() ((void)0)
#endif

// MC_TEST_COMBINED links many suites into ONE executable, which is what the
// Windows target does: an MFC test binary costs ~9 MB and a full link, so 40 of
// them is minutes per build where one is seconds.  Every test .cpp still writes
// MC_TEST_MAIN() and needs no edit — the macro simply stops producing a main,
// and tests/win/all_tests.cpp provides the single one.  TEST() registers into a
// global registry either way, so nothing else changes.
// The real thing.  It has to stay a MACRO rather than becoming an inline
// function, because MC_TEST_WIN_HOST_INIT calls AfxWinInit: on Windows the test
// files include mc_test.h BEFORE stdafx.h, so afxwin.h is not in scope here and
// is in scope at the MC_TEST_MAIN() call site at the foot of the file.
#define MC_TEST_MAIN_ALWAYS() \
    int main(int, char**) { \
        MC_TEST_WIN_HOST_INIT(); \
        MC_TEST_IGNORE_SIGPIPE(); \
        Generate256colours(); \
        return mc_test::run_all(); \
    }

#ifdef MC_TEST_COMBINED
#define MC_TEST_MAIN()   /* tests/win/all_tests.cpp owns main */
#else
#define MC_TEST_MAIN() MC_TEST_MAIN_ALWAYS()
#endif
