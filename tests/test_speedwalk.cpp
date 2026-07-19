// tests/test_speedwalk.cpp
// Tests for CMUSHclientDoc::DoEvaluateSpeedwalk (doc_portable.cpp), checking
// Windows parity (scripting/methods/methods_speedwalks.cpp): direction
// expansion, the C/O/L/K action prefixes (close/open/lock/unlock), and the
// per-digit repeat-count cap (no integer overflow).
//
// Every build shares the doc_shared.cpp wrappers now: the terminator is
// ENDLINE ("\r\n"), exactly what Windows has always produced (P1b de-drift).

#include "mc_test.h"
#include "platform/doc_stub.h"

// ── Basic direction expansion ─────────────────────────────────────────────

TEST(Speedwalk, SingleDirectionExpands)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("north\r\n", (const char*)doc.DoEvaluateSpeedwalk("n"));
}

TEST(Speedwalk, RepeatCountExpands)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("north\r\nnorth\r\nnorth\r\n", (const char*)doc.DoEvaluateSpeedwalk("3n"));
}

// ── C/O/L/K action codes (Windows parity — were missing on the port) ───────

TEST(Speedwalk, OpenActionPrefixesDirection)
{
    // "on" -> open the door to the north, sent as one line "open north".
    CMUSHclientDoc doc;
    EXPECT_STREQ("open north\r\n", (const char*)doc.DoEvaluateSpeedwalk("on"));
}

TEST(Speedwalk, CloseLockUnlockActions)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("close south\r\n", (const char*)doc.DoEvaluateSpeedwalk("cs"));
    EXPECT_STREQ("lock east\r\n",   (const char*)doc.DoEvaluateSpeedwalk("le"));
    EXPECT_STREQ("unlock west\r\n", (const char*)doc.DoEvaluateSpeedwalk("kw"));
}

TEST(Speedwalk, ActionCodeThenMovement)
{
    // "onn": open the north door, then walk north.
    CMUSHclientDoc doc;
    EXPECT_STREQ("open north\r\nnorth\r\n", (const char*)doc.DoEvaluateSpeedwalk("onn"));
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

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────

// speedwalk.cpp: "int iSlash = str.Find ("/");   // only use up to the slash"
// — expanding "(ne/sw)" emits only the part before the slash; the part after
// it is reserved for the reverse walk.
TEST(Speedwalk, ParenPairExpandsUsingTextBeforeSlash)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("ne\r\n", (const char*)doc.DoEvaluateSpeedwalk("(ne/sw)"));
}

// speedwalk.cpp: "str = strRightPart + "/" + strLeftPart;   // swap parts"
// — reversing "(ne/sw)" swaps the halves so the reverse walk uses the
// second direction.
TEST(Speedwalk, ReverseParenSlashPairSwapsSides)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("(sw/ne)", (const char*)doc.DoReverseSpeedwalk("(ne/sw)"));
}

// speedwalk.cpp: "Error strings all start with '*' (callers detect an error
// by testing [0])" — and the invalid-direction message itself begins with
// '*' via TFormat, so that particular error carries a doubled asterisk.
TEST(Speedwalk, InvalidDirectionYieldsStarErrorString)
{
    CMUSHclientDoc doc;
    CString r = doc.DoEvaluateSpeedwalk("x");
    ASSERT_TRUE(r.GetLength() >= 2);
    EXPECT_EQ('*', ((const char*)r)[0]);
    EXPECT_EQ('*', ((const char*)r)[1]);          // the doubled asterisk
    EXPECT_TRUE(r.Find("Invalid direction") != -1);
}

// speedwalk.cpp: "a '{' must be closed before end of input".
TEST(Speedwalk, UnterminatedCommentIsError)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("*Comment code of '{' not terminated by a '}'",
                 (const char*)doc.DoEvaluateSpeedwalk("{oops"));
    // a terminated comment is simply skipped
    EXPECT_STREQ("north\r\n", (const char*)doc.DoEvaluateSpeedwalk("{oops}n"));
}

// speedwalk.cpp: "Speed walk counter may not be followed by a comment"
// (and a counter at end of string: "not followed by an action").
TEST(Speedwalk, CounterFollowedByCommentIsError)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("*Speed walk counter may not be followed by a comment",
                 (const char*)doc.DoEvaluateSpeedwalk("3{note}n"));
    EXPECT_STREQ("*Speed walk counter not followed by an action",
                 (const char*)doc.DoEvaluateSpeedwalk("3"));
}

// speedwalk.cpp ReverseSpeedwalkString: "preserve comments" (result built by
// prepending, so overall order reverses) and "'\r': break; // discard
// carriage returns".
TEST(Speedwalk, ReversePreservesCommentBlocks)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("3w{note}2s", (const char*)doc.DoReverseSpeedwalk("2n{note}3e"));
    // carriage returns in the input are discarded
    EXPECT_STREQ("ns", (const char*)doc.DoReverseSpeedwalk("n\rs"));
}

// speedwalk.cpp: "action prefix C/O/L/K (remembered, re-emitted before the
// reversed direction)" — reverse of "on" (open north) is "os" (open south).
TEST(Speedwalk, ReverseKeepsActionPrefixOnReversedDirection)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("os", (const char*)doc.DoReverseSpeedwalk("on"));
    EXPECT_STREQ("ku", (const char*)doc.DoReverseSpeedwalk("kd"));  // unlock
}

// ── Mapper: typed-direction auto-mapping (Mapping.cpp AddToMap, single-sourced
// into doc_shared.cpp so both builds share one body).  The old headless stub was
// `void AddToMap(CString){}`, silently dropping every tracked move; these assert
// the real m_strMapList accumulation through the same MapDirectionsMap the GUI
// uses.  Falsification: all three FAIL against the no-op stub (list stays empty).
TEST(Mapper, TypedDirectionIsRecorded)
{
    CMUSHclientDoc doc;
    doc.AddToMap("north\r\n");                       // "north" -> logged as "n"
    ASSERT_EQ(1, doc.m_strMapList.GetCount());
    EXPECT_STREQ("n", (const char*)doc.m_strMapList.GetHead());
}

TEST(Mapper, MultipleDirectionsAccumulate)
{
    CMUSHclientDoc doc;
    doc.AddToMap("north\r\n");
    doc.AddToMap("east\r\n");
    ASSERT_EQ(2, doc.m_strMapList.GetCount());
    EXPECT_STREQ("n", (const char*)doc.m_strMapList.GetHead());
    EXPECT_STREQ("e", (const char*)doc.m_strMapList.GetTail());
}

// m_bRemoveMapReverses: N then S is a net no-move, so the earlier "n" is removed
// rather than "s" appended (Mapping.cpp:162-163).
TEST(Mapper, RemoveReversesCancelsBacktrack)
{
    CMUSHclientDoc doc;
    doc.m_bRemoveMapReverses = TRUE;
    doc.AddToMap("north\r\n");                       // -> ["n"]
    doc.AddToMap("south\r\n");                       // reverse of north cancels -> []
    EXPECT_EQ(0, doc.m_strMapList.GetCount());
}

MC_TEST_MAIN()
