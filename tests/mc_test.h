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
#include <functional>

namespace mc_test {

// ── Result accumulator ───────────────────────────────────────────────────

struct Result {
    int passed = 0;
    int failed = 0;
    bool current_failed = false;
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
        tc.fn();
        if (g().current_failed) {
            ++g().failed;
            fprintf(stderr, "[FAIL] %s.%s\n", tc.suite, tc.name);
        } else {
            ++g().passed;
            fprintf(stdout, "[PASS] %s.%s\n", tc.suite, tc.name);
        }
    }
    fprintf(stdout, "\n%d passed, %d failed\n", g().passed, g().failed);
    return g().failed ? 1 : 0;
}

// ── Assertion helpers ─────────────────────────────────────────────────────

inline void fail_msg(const char* file, int line, const char* msg) {
    fprintf(stderr, "  %s:%d: %s\n", file, line, msg);
    g().current_failed = true;
}

template<typename A, typename B>
inline bool cmp_eq(const A& a, const B& b) { return a == b; }
inline bool cmp_eq(const char* a, const char* b) {
    return a && b && strcmp(a, b) == 0;
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
        snprintf(buf, sizeof(buf), "Expected " #expected " == " #actual); \
        mc_test::fail_msg(__FILE__, __LINE__, buf); \
    }} while(0)

#define EXPECT_NE(a, b) \
    do { if (mc_test::cmp_eq((a),(b))) \
        mc_test::fail_msg(__FILE__, __LINE__, "Expected " #a " != " #b); \
    } while(0)

#define EXPECT_GT(a, b) \
    do { if (!((a) > (b))) \
        mc_test::fail_msg(__FILE__, __LINE__, "Expected " #a " > " #b); \
    } while(0)

#define EXPECT_GE(a, b) \
    do { if (!((a) >= (b))) \
        mc_test::fail_msg(__FILE__, __LINE__, "Expected " #a " >= " #b); \
    } while(0)

#define EXPECT_LT(a, b) \
    do { if (!((a) < (b))) \
        mc_test::fail_msg(__FILE__, __LINE__, "Expected " #a " < " #b); \
    } while(0)

#define EXPECT_LE(a, b) \
    do { if (!((a) <= (b))) \
        mc_test::fail_msg(__FILE__, __LINE__, "Expected " #a " <= " #b); \
    } while(0)

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

#define MC_TEST_MAIN() \
    int main(int, char**) { \
        MC_TEST_IGNORE_SIGPIPE(); \
        Generate256colours(); \
        return mc_test::run_all(); \
    }
