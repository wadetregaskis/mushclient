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

// ── Running one suite, or all of them ──────────────────────────────────────
// `only` non-null runs just the tests whose TEST(suite, ...) matches it.
//
// This exists for the Windows target, where every enabled tests/test_*.cpp is
// linked into ONE executable (tests/win/all_tests.cpp).  That design trades
// link time for isolation, and its own comment says so: "suites are no longer
// isolated from each other's global state ... if a suite ever needs isolation,
// give it its own vcxproj."  Six suites turned out to need it -- each compiled
// and linked, then killed the process partway through a run, always inside a
// DIFFERENT suite from the one just added (docs/WINDOWS-VALIDATION.md section 10t).
//
// A filter is the cheap half of that vcxproj: one binary still, but the runner
// can launch it once per suite, so a suite that corrupts process state takes
// only itself down.  That is exactly the property macOS gets for free by
// building one executable per test file.
inline int run_all(const char* only = nullptr) {
    for (auto& tc : cases()) {
        if (only && strcmp(tc.suite, only) != 0)
            continue;
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

// ── Swapping a fake pal::IWorldView into a document ────────────────────────
// Put the doc's OWN view back at the end of the scope -- never NULL.
//
// On the port m_pWorldView starts null and nothing owns it, so `= &view` then
// `= nullptr` is harmless and that is how the pattern was written.  On Windows
// the doc constructs its own adapter (doc_construct.cpp: pal::CreateWinWorldView)
// and its destructor DELETES it, so the same two lines do one of two wrong
// things: nulling the pointer leaks the adapter, and leaving the fake in place
// runs `delete` over a stack object.  Both were live in this suite the moment
// the Windows target stopped crashing before it reached them.
//
// Templated only so this header need not know the pal types -- it is included
// long before them.  Use it as a scope guard:
//
//     mc_test::ScopedWorldView<CMUSHclientDoc, FakeWorldView> attach (doc, &view);
//
template <class Doc, class View>
struct ScopedWorldView {
    Doc& doc;
    typename std::decay<decltype (std::declval<Doc&> ().m_pWorldView)>::type saved;
    ScopedWorldView (Doc& d, View* v) : doc (d), saved (d.m_pWorldView) { d.m_pWorldView = v; }
    ~ScopedWorldView () { doc.m_pWorldView = saved; }
    ScopedWorldView (const ScopedWorldView&) = delete;
    ScopedWorldView& operator= (const ScopedWorldView&) = delete;
};

// Every distinct TEST() suite name, in registration order, one per line.
// tests/win/run_tests.ps1 reads this to decide what to launch, so the runner
// never has to keep its own copy of the list -- a second list would drift, and
// a suite missing from it would simply not run, silently.
inline void list_suites() {
    std::vector<const char*> seen;
    for (auto& tc : cases()) {
        bool dup = false;
        for (const char* s : seen)
            if (strcmp(s, tc.suite) == 0) { dup = true; break; }
        if (!dup) { seen.push_back(tc.suite); fprintf(stdout, "%s\n", tc.suite); }
    }
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
        char mc_test_buf_[512]; \
        snprintf(mc_test_buf_, sizeof(mc_test_buf_), "Expected " #expected " == " #actual \
                 " (%s vs %s)", mc_test::describe(expected).c_str(), \
                 mc_test::describe(actual).c_str()); \
        mc_test::fail_msg(__FILE__, __LINE__, mc_test_buf_); \
    }} while(0)

#define EXPECT_NE(a, b) \
    do { if (mc_test::cmp_eq((a),(b))) { \
        char mc_test_buf_[512]; \
        snprintf(mc_test_buf_, sizeof(mc_test_buf_), "Expected " #a " != " #b \
                 " (%s vs %s)", mc_test::describe(a).c_str(), \
                 mc_test::describe(b).c_str()); \
        mc_test::fail_msg(__FILE__, __LINE__, mc_test_buf_); \
    }} while(0)

#define EXPECT_GT(a, b) \
    do { if (!((a) > (b))) { \
        char mc_test_buf_[512]; \
        snprintf(mc_test_buf_, sizeof(mc_test_buf_), "Expected " #a " > " #b \
                 " (%s vs %s)", mc_test::describe(a).c_str(), \
                 mc_test::describe(b).c_str()); \
        mc_test::fail_msg(__FILE__, __LINE__, mc_test_buf_); \
    }} while(0)

#define EXPECT_GE(a, b) \
    do { if (!((a) >= (b))) { \
        char mc_test_buf_[512]; \
        snprintf(mc_test_buf_, sizeof(mc_test_buf_), "Expected " #a " >= " #b \
                 " (%s vs %s)", mc_test::describe(a).c_str(), \
                 mc_test::describe(b).c_str()); \
        mc_test::fail_msg(__FILE__, __LINE__, mc_test_buf_); \
    }} while(0)

#define EXPECT_LT(a, b) \
    do { if (!((a) < (b))) { \
        char mc_test_buf_[512]; \
        snprintf(mc_test_buf_, sizeof(mc_test_buf_), "Expected " #a " < " #b \
                 " (%s vs %s)", mc_test::describe(a).c_str(), \
                 mc_test::describe(b).c_str()); \
        mc_test::fail_msg(__FILE__, __LINE__, mc_test_buf_); \
    }} while(0)

#define EXPECT_LE(a, b) \
    do { if (!((a) <= (b))) { \
        char mc_test_buf_[512]; \
        snprintf(mc_test_buf_, sizeof(mc_test_buf_), "Expected " #a " <= " #b \
                 " (%s vs %s)", mc_test::describe(a).c_str(), \
                 mc_test::describe(b).c_str()); \
        mc_test::fail_msg(__FILE__, __LINE__, mc_test_buf_); \
    }} while(0)

#define EXPECT_STREQ(expected, actual) \
    do { if (!mc_test::cmp_eq((const char*)(expected), (const char*)(actual))) { \
        char mc_test_buf_[512]; \
        snprintf(mc_test_buf_, sizeof(mc_test_buf_), "Expected STREQ: \"%s\" vs \"%s\"", \
                 (const char*)(expected), (const char*)(actual)); \
        mc_test::fail_msg(__FILE__, __LINE__, mc_test_buf_); \
    }} while(0)

#define EXPECT_STRNE(a, b) \
    do { if (mc_test::cmp_eq((const char*)(a), (const char*)(b))) \
        mc_test::fail_msg(__FILE__, __LINE__, "Expected STRNE: " #a " != " #b); \
    } while(0)

#define EXPECT_NEAR(expected, actual, tol) \
    do { if (std::fabs((double)(expected) - (double)(actual)) > (double)(tol)) { \
        char mc_test_buf_[256]; \
        snprintf(mc_test_buf_, sizeof(mc_test_buf_), "Expected |" #expected " - " #actual "| <= " #tol \
                 " (diff=%.6g)", std::fabs((double)(expected)-(double)(actual))); \
        mc_test::fail_msg(__FILE__, __LINE__, mc_test_buf_); \
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

// ── the two document templates ────────────────────────────────────────────
// InitInstance builds four of these (MUSHclient.cpp:558-594) and the framework
// then owns every open document through them.  Two of them are load-bearing for
// code that has nothing to do with windows, and this host was crashing on both:
//
//   m_pWorldDocTemplate   CMainFrame::OnUpdateBtnWorlds opens with
//                         App.m_pWorldDocTemplate->GetFirstDocPosition()
//                         (mainfrm.cpp:687), reached from ConnectionClosed --
//                         so closing a connection was an access violation.
//                         AcceleratorTo reads m_hAccelTable off the same
//                         pointer.
//   m_pNormalDocTemplate  AppendToTheNotepad walks it at doc.cpp:3043, reached
//                         by every MXP error and debug message.  Note that the
//                         same walk in notepad_app_shared.cpp:115 DOES check
//                         the pointer for NULL first: two walkers of one
//                         template, one careful and one not.  Neither can be
//                         NULL after InitInstance, so upstream is latent
//                         rather than broken.
//
// Constructing one creates no window: CDocTemplate's constructor stores the
// three runtime classes and loads a string resource.  That load fails and
// reports one assertion per template on stderr -- accepted.
//
// It fails for a reason worth stating exactly, because the obvious guess is
// wrong: the target DOES compile MUSHclient.rc.  That file carries a cursor,
// three icons and version info and nothing else -- confirmed by enumerating the
// built exe's resource types.  The dialogs, menus, string table and
// accelerators live in locale\EN.dll and are loaded at runtime by
// CMUSHclientApp::I18N_Setup (MUSHclient.cpp:1832).  See the note above
// MC_TEST_WIN_MAIN_FRAME for why this host does not do that.
//
// Deliberately NOT AddDocTemplate: that hands the template to the app's
// document manager and makes File>New and the OLE server reachable, which this
// host has no business doing.  The tests need the pointer and an empty
// document list, and that is all this gives them.
#define MC_TEST_WIN_DOC_TEMPLATES() \
    do { \
        if (App.m_pNormalDocTemplate == NULL) \
            App.m_pNormalDocTemplate = new CMultiDocTemplate( \
                IDR_NORMALTYPE, RUNTIME_CLASS(CTextDocument), \
                RUNTIME_CLASS(CTextChildFrame), RUNTIME_CLASS(CTextView)); \
        if (App.m_pWorldDocTemplate == NULL) \
            App.m_pWorldDocTemplate = new CMultiDocTemplate( \
                IDR_MUSHCLTYPE, RUNTIME_CLASS(CMUSHclientDoc), \
                RUNTIME_CLASS(CChildFrame), RUNTIME_CLASS(CMUSHView)); \
        /* Populate m_strDocStrings, m_hMenuShared and m_hAccelTable from the \
           resource id.  The framework does this from AddDocTemplate, which \
           this host does not call; CMultiDocTemplate::LoadTemplate is the same \
           work without handing the template to the document manager.  It needs \
           the resources MC_TEST_WIN_RESOURCE_DLL loads, and it is what makes \
           App.m_pWorldDocTemplate->m_hAccelTable non-NULL -- which \
           AcceleratorTo falls back on, and CopyAcceleratorTable of a NULL \
           table answers 0, after which the body indexes an empty vector. */ \
        App.m_pNormalDocTemplate->LoadTemplate(); \
        App.m_pWorldDocTemplate->LoadTemplate(); \
    } while(0)

// ── the localisation resource DLL ──────────────────────────────────────────
// MUSHclient.rc carries a cursor, three icons and version info and NOTHING
// else -- no dialogs, no menus, no string table, no accelerators.  That is not
// an omission: on Windows they live in locale\EN.dll and are loaded at runtime
// by CMUSHclientApp::I18N_Setup, which ends in AfxSetResourceHandle
// (MUSHclient.cpp:1832).  Confirmed by enumerating the built exe's resource
// types: CURSOR, ICON, GROUP_CURSOR, GROUP_ICON, VERSION, and no others.
//
// Without that step the process has no dialog templates at all, so
// CDialog::Create fails _AfxCheckDialogTemplate, CMultiDocTemplate cannot load
// the title/filter string its constructor asks for, and AfxMessageBox(nID) has
// no text to find.
//
// Six lines add it -- LoadLibrary ("locale\\en.dll") next to the staged runtime
// assets, then AfxSetResourceHandle.  I18N_Setup itself is not callable here
// because its failure arms are plain ::MessageBox, which nothing routes.
//
// The first attempt at this took four suites from green to BLOCKED at the
// runner's clock -- LifecycleArms, LuaScripting, SpellCheckTest, SweepR4 --
// because with templates available CDialog::DoModal stops FAILING and starts
// putting up a window.  That measurement inverted an assumption: this host's
// safety from modal dialogs never came from routing them, it came from there
// being no template to instantiate.
//
// So the resources arrive together with the thing that makes them safe: the
// WH_CBT hook in tests/win/all_tests.cpp, which closes every dialog the process
// creates before it can block.  Load the DLL only if that hook is installed --
// they are one change, not two.
#define MC_TEST_WIN_RESOURCE_DLL() \
    do { \
        HINSTANCE hres_ = ::LoadLibrary("locale\\en.dll"); \
        if (hres_) \
            AfxSetResourceHandle(hres_); \
        else \
            fprintf(stderr, "note: locale\\en.dll not found - no dialog, menu " \
                            "or string resources for this run\n"); \
    } while(0)

// ── a CMainFrame object, with no window in it ──────────────────────────────
// `Frame` is a MACRO -- *((CMainFrame *) App.m_pMainWnd) (stdafx.h:271) -- so
// every use of it is a DEREFERENCE and faults outright when m_pMainWnd is
// NULL.  That single fact is behind four separate exclusions in this target's
// history and three crashing suites, each in a different engine file.
//
// CONSTRUCT one, do NOT Create() it.  The constructor is fourteen assignments
// and a ZeroMemory (mainfrm.cpp:256) -- no window, no toolbars, no timers --
// which leaves every embedded control with m_hWnd == NULL.  That is exactly
// the state MFC's own guards are written for (`if (pWnd->m_hWnd)`,
// `if (Frame.m_wndInfoBar.m_hWnd)`), so the engine's existing checks start
// working instead of faulting one line before they are read.
//
// It is NOT AfxGetApp()->m_pMainWnd in the sense the framework means: nothing
// pumps messages for it and nothing must try.  Anything that needs a CREATED
// frame -- toolbar button state, accelerators -- still cannot work here, and
// those suites stay out with their own reasons.
#define MC_TEST_WIN_MAIN_FRAME() \
    do { \
        if (App.m_pMainWnd == NULL) \
            App.m_pMainWnd = new CMainFrame; \
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
// MUSHCLIENT_VERSION is a CString GLOBAL that CMUSHclientApp::InitInstance
// assigns (MUSHclient.cpp), not a compile-time constant -- so in a host that
// never runs InitInstance it is the empty string, and world.Version() answers
// "".  Set it here for the same reason this macro loads the option table and
// the MXP tables: those are all things InitInstance does that the engine then
// assumes have happened.
// InstallGdiSurfaceBridge is the third InitInstance step this host needs, and
// it is the one with the widest reach: pal::g_surface is NULL until it runs
// (MUSHclient.cpp:254), so without it every CMiniWindow drawing operation --
// the whole world.Window* family, the image ops, text measurement -- fails at
// the seam rather than at anything it is being asked about.  Installing it here
// is what lets the miniwindow tests run against REAL GDI, which is a cross-check
// the port's own pal::CpuRaster has never had.
#define MC_TEST_WIN_APP_STATE() \
    do { \
        MUSHCLIENT_VERSION = VERSION_STRING; \
        MC_TEST_WIN_RESOURCE_DLL(); \
        InstallGdiSurfaceBridge(); \
        MC_TEST_WIN_MAIN_FRAME(); \
        App.LoadGlobalsFromDatabase(); \
        App.MXP_LoadElements(); \
        App.MXP_LoadEntities(); \
        App.MXP_LoadColours(); \
        MC_TEST_WIN_DOC_TEMPLATES(); \
    } while(0)

// Defined in tests/win/all_tests.cpp, which is the only place this macro is
// ever expanded (MC_TEST_COMBINED means every other file's MC_TEST_MAIN()
// produces nothing).  It prints a symbolised stack from the faulting CONTEXT,
// because /EHsc means the runner's catch(...) never sees an access violation
// and the run otherwise dies saying only "CRASHED (no summary)".
void McTestInstallCrashHandler();

// How many dialogs the WH_CBT hook has closed so far.  "A dialog was raised" is
// the only observable a test has for a code path that ends in one, since the
// hook means none of them survives long enough to be inspected.
long McTestDialogsCancelled();

// How many _CRT_ASSERT reports the process has raised so far.  MFC asserts
// where a Win32 handle is required and absent, so a delta across one call is
// "did this reach a state MFC considers illegal" -- a question that otherwise
// only a human reading stderr can answer, and only if they happen to look.
long McTestAssertionsRaised();

#define MC_TEST_WIN_HOST_INIT() \
    do { \
        setvbuf(stdout, NULL, _IONBF, 0); \
        McTestInstallCrashHandler(); \
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

// ── what the tests leaked, as against what the host still owns ─────────────
// The debug CRT dumps every unfreed block at exit, and MFC asks it to
// (_AFX_DEBUG_STATE's destructor).  In this host that dump is 2,684 lines and
// almost none of it is a leak: App outlives the dump and still owns the MXP
// element/entity/colour tables MC_TEST_WIN_APP_STATE loaded, so its ~500
// CObjects and their collection plexes are reported as leaked while being
// perfectly alive.  Measured: adding MC_TEST_WIN_APP_STATE grew the dump from
// 1,618 lines to 2,684 without leaking anything.
//
// A dump that large buries the summary line and cannot be read, which is worse
// than no dump at all -- so take a checkpoint once the host has finished
// setting itself up, diff against it after the last test, and report THAT.
// What survives the diff was allocated by a test and not freed by it, which is
// the only question this binary can usefully answer about memory.
//
// Statistics rather than the object list, because the list is long when it is
// non-empty and this line runs on every green run.  _CrtMemDumpAllObjectsSince
// (&McTestLeakBaseline()) is the one-line escalation when the number moves.
//
// This is NOT a substitute for a sanitizer: it sees blocks that were never
// freed, and says nothing about use-after-free, overruns, or double frees.
// docs/SANITIZERS.md is still the instrument for those, on the macOS side.
inline _CrtMemState& McTestLeakBaseline()
{
    static _CrtMemState s;
    return s;
}

#define MC_TEST_WIN_LEAK_BASELINE()  _CrtMemCheckpoint(&McTestLeakBaseline())

#define MC_TEST_WIN_LEAK_REPORT() \
    do { \
        _CrtMemState after_, diff_; \
        _CrtMemCheckpoint(&after_); \
        if (_CrtMemDifference(&diff_, &McTestLeakBaseline(), &after_)) { \
            fprintf(stderr, "\nstill allocated after the last test " \
                            "(host setup excluded):\n"); \
            _CrtMemDumpStatistics(&diff_); \
        } \
        /* and stop the exit dump reporting App's live tables as leaks */ \
        AfxEnableMemoryLeakDump(FALSE); \
    } while(0)

// One line on stdout naming how many _CRT_ASSERTs the run raised, so a number
// nobody was counting becomes one the runner can aggregate.
//
// MFC asserts wherever a Win32 handle is required and absent, so a suite with a
// non-zero count is a suite reaching a code path that expects a window it has
// not got.  That is the defect family which cost this target three crashing
// suites (docs/WINDOWS-VALIDATION.md §10v) and which §10as's sweep was written
// to catch in one file; the count generalises it to all 186 at no cost.
//
// Printed unconditionally, INCLUDING the zero: a missing line then means an old
// binary rather than a clean run, which is the distinction the runner needs.
#define MC_TEST_WIN_ASSERT_REPORT() \
    printf("MFC assertions: %ld\n", McTestAssertionsRaised())
#else
#define MC_TEST_WIN_HOST_INIT()     ((void)0)
#define MC_TEST_WIN_LEAK_BASELINE() ((void)0)
#define MC_TEST_WIN_LEAK_REPORT()   ((void)0)
#define MC_TEST_WIN_ASSERT_REPORT() ((void)0)
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
// argv is read for two flags, both used only by tests/win/run_tests.ps1:
//   --list-suites   print every TEST() suite name and exit
//   --suite=NAME    run only that suite
// Neither changes what a plain run does, so macOS -- one executable per test
// file, already isolated -- is unaffected.
//
// Both flags are handled AFTER the host is initialised, and that ordering is
// load-bearing rather than tidy.  --list-suites first did its printing and
// returned from main BEFORE MC_TEST_WIN_HOST_INIT, which looks obviously right
// -- listing names needs no MFC, so why pay for it? -- and hung every time:
// MFC's OLE teardown asserts in oleunk.cpp when AfxOleInit was never called,
// and on this host an assertion is an invisible modal dialog (see
// docs/WINDOWS-VALIDATION.md section 10q).  So the process printed nothing,
// exited never, and the runner waited on it forever.
//
// The rule that follows is worth more than the fix: on this host an early
// return from main is a HANG, not a shortcut.  A flag added here must run the
// full host init and leave through the same exit as an ordinary run.
#define MC_TEST_MAIN_ALWAYS() \
    int main(int argc_, char** argv_) { \
        const char* only_ = nullptr; \
        bool list_ = false; \
        for (int i_ = 1; i_ < argc_; ++i_) { \
            if (strcmp(argv_[i_], "--list-suites") == 0) list_ = true; \
            else if (strncmp(argv_[i_], "--suite=", 8) == 0) only_ = argv_[i_] + 8; \
        } \
        MC_TEST_WIN_HOST_INIT(); \
        MC_TEST_IGNORE_SIGPIPE(); \
        Generate256colours(); \
        MC_TEST_WIN_LEAK_BASELINE();  /* host setup done; measure from here */ \
        int rc_ = 0; \
        if (list_) mc_test::list_suites(); \
        else rc_ = mc_test::run_all(only_); \
        MC_TEST_WIN_ASSERT_REPORT(); \
        MC_TEST_WIN_LEAK_REPORT(); \
        return rc_; \
    }

#ifdef MC_TEST_COMBINED
#define MC_TEST_MAIN()   /* tests/win/all_tests.cpp owns main */
#else
#define MC_TEST_MAIN() MC_TEST_MAIN_ALWAYS()
#endif
