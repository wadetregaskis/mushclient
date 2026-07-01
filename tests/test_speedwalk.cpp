// tests/test_speedwalk.cpp
// Tests for CMUSHclientDoc::DoEvaluateSpeedwalk (doc_portable.cpp), checking
// Windows parity (scripting/methods/methods_speedwalks.cpp): direction
// expansion, the C/O/L/K action prefixes (close/open/lock/unlock), and the
// per-digit repeat-count cap (no integer overflow).
//
// The portable build uses ENDLINE "\n".

#include "mc_test.h"
#include "platform/doc_stub.h"

// ── Basic direction expansion ─────────────────────────────────────────────

TEST(Speedwalk, SingleDirectionExpands)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("north\n", (const char*)doc.DoEvaluateSpeedwalk("n"));
}

TEST(Speedwalk, RepeatCountExpands)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("north\nnorth\nnorth\n", (const char*)doc.DoEvaluateSpeedwalk("3n"));
}

// ── C/O/L/K action codes (Windows parity — were missing on the port) ───────

TEST(Speedwalk, OpenActionPrefixesDirection)
{
    // "on" -> open the door to the north, sent as one line "open north".
    CMUSHclientDoc doc;
    EXPECT_STREQ("open north\n", (const char*)doc.DoEvaluateSpeedwalk("on"));
}

TEST(Speedwalk, CloseLockUnlockActions)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("close south\n", (const char*)doc.DoEvaluateSpeedwalk("cs"));
    EXPECT_STREQ("lock east\n",   (const char*)doc.DoEvaluateSpeedwalk("le"));
    EXPECT_STREQ("unlock west\n", (const char*)doc.DoEvaluateSpeedwalk("kw"));
}

TEST(Speedwalk, ActionCodeThenMovement)
{
    // "onn": open the north door, then walk north.
    CMUSHclientDoc doc;
    EXPECT_STREQ("open north\nnorth\n", (const char*)doc.DoEvaluateSpeedwalk("onn"));
}

TEST(Speedwalk, ActionCodeWithCountIsError)
{
    // A count may not precede an action code.
    CMUSHclientDoc doc;
    CString r = doc.DoEvaluateSpeedwalk("2on");
    ASSERT_TRUE(r.GetLength() > 0);
    EXPECT_EQ('*', ((const char*)r)[0]);
}

TEST(Speedwalk, ActionCodeWithoutDirectionIsError)
{
    // An action code must be followed by a direction.
    CMUSHclientDoc doc;
    CString r = doc.DoEvaluateSpeedwalk("o");
    ASSERT_TRUE(r.GetLength() > 0);
    EXPECT_EQ('*', ((const char*)r)[0]);
}

// ── Repeat-count cap: no integer overflow past >99 (per-digit check) ───────

TEST(Speedwalk, CountOverNinetyNineIsError)
{
    // Error strings use the Windows MakeSpeedWalkErrorString format: "*" + msg,
    // with NO trailing newline.
    CMUSHclientDoc doc;
    EXPECT_STREQ("*Speed walk counter exceeds 99",
                 (const char*)doc.DoEvaluateSpeedwalk("100n"));
}

TEST(Speedwalk, HugeCountDoesNotOverflowPastCap)
{
    // A digit run long enough to overflow a signed int must still be rejected
    // by the >99 cap (checked per-digit), not wrap to a small value and expand.
    // Pre-fix the count was accumulated across ALL digits then checked once, so
    // "4294967297" wrapped to 1 and expanded to a single "north".
    CMUSHclientDoc doc;
    EXPECT_STREQ("*Speed walk counter exceeds 99",
                 (const char*)doc.DoEvaluateSpeedwalk("4294967297n"));
}

// ── Reverse speedwalk (also shared logic) ─────────────────────────────────

TEST(Speedwalk, ReverseSimplePath)
{
    // Reverse of "3n2e" is "2w3s": inverse steps, in reverse order.
    CMUSHclientDoc doc;
    EXPECT_STREQ("2w3s", (const char*)doc.DoReverseSpeedwalk("3n2e"));
}

TEST(Speedwalk, ReverseSingleStep)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("s", (const char*)doc.DoReverseSpeedwalk("n"));
    EXPECT_STREQ("d", (const char*)doc.DoReverseSpeedwalk("u"));
}

TEST(Speedwalk, ReverseCustomParenWord)
{
    // A built-in diagonal word reverses via the reverse-word table: (ne) -> (sw).
    CMUSHclientDoc doc;
    EXPECT_STREQ("(sw)", (const char*)doc.DoReverseSpeedwalk("(ne)"));
}

MC_TEST_MAIN()
