// tests/test_mc_test_framework.cpp
// Self-tests for the mc_test.h assertion framework itself.  A regression here
// is invisible to every OTHER suite -- they'd just silently mis-report
// pass/fail -- so it needs its own coverage.

#include "mc_test.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

// X04-F16: cmp_eq's const char* overload used to compare two NULL pointers
// via strcmp(a, b) with the `a && b` guard failing first, so it always
// returned false for two nulls -- EXPECT_EQ/EXPECT_STREQ over two null C
// strings could never pass, and EXPECT_NE over two null C strings could
// never fail.
TEST(McTestFramework, CmpEqBothNullCharPointersAreEqual) {
    const char* a = nullptr;
    const char* b = nullptr;
    EXPECT_TRUE(mc_test::cmp_eq(a, b));
}

TEST(McTestFramework, CmpEqOneNullOneNotAreStillUnequal) {
    const char* a = nullptr;
    const char* b = "x";
    EXPECT_FALSE(mc_test::cmp_eq(a, b));
    EXPECT_FALSE(mc_test::cmp_eq(b, a));
}

// X04-F15: EXPECT_EQ (and NE/GT/GE/LT/LE) used to print only the stringified
// EXPRESSIONS, never the runtime VALUES, so a failure inside a table- or
// loop-driven assertion identified the line but not which input produced it.
// Use named variables (not literal constants) so the stringified source text
// itself can't accidentally already contain the compared digits -- that's
// what would let this test pass even before the fix.
TEST(McTestFramework, FailureMessageIncludesBothComparedValues) {
    int expected_val = 424242;
    int actual_val   = 987654;

    char tmpl[] = "/tmp/mc_test_framework_stderr_XXXXXX";
    int tmp_fd = mkstemp(tmpl);
    ASSERT_TRUE(tmp_fd >= 0);

    fflush(stderr);
    int saved_stderr = dup(fileno(stderr));
    ASSERT_TRUE(saved_stderr >= 0);
    dup2(tmp_fd, fileno(stderr));
    close(tmp_fd);

    // Deliberately mismatched -- it's this assertion's MESSAGE we're
    // inspecting, not a real failure of this meta-test, so undo its effect
    // on the pass/fail tally immediately afterwards.
    EXPECT_EQ(expected_val, actual_val);
    mc_test::g().current_failed = false;

    fflush(stderr);
    dup2(saved_stderr, fileno(stderr));
    close(saved_stderr);

    FILE* f = fopen(tmpl, "r");
    ASSERT_TRUE(f != nullptr);
    char buf[1024] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    unlink(tmpl);

    std::string captured(buf);
    EXPECT_NE(std::string::npos, captured.find("424242"));
    EXPECT_NE(std::string::npos, captured.find("987654"));
}

MC_TEST_MAIN()
