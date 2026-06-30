// tests/test_mxp.cpp
// Unit tests for the MXP phase state machine:
//   Phase_MXP_ELEMENT, Phase_MXP_COMMENT, Phase_MXP_QUOTE, Phase_MXP_ENTITY
//
// Strategy: construct a minimal CMUSHclientDoc, drive the Phase_MXP_*
// methods directly, and inspect m_phase / m_strMXPstring after each call.
// We also smoke-test MXP_collected_element dispatch and the entity collector.

#include "mc_test.h"
#include "platform/doc_stub.h"

// ── Helpers ───────────────────────────────────────────────────────────────

static CMUSHclientDoc make_doc()
{
    CMUSHclientDoc doc;
    doc.m_pCurrentLine = new CLine();
    doc.m_iFlags       = COLOUR_ANSI;
    doc.m_iForeColour  = WHITE;
    doc.m_iBackColour  = BLACK;
    doc.AddStyle(COLOUR_ANSI, WHITE, BLACK, 0,
                 static_cast<CAction*>(nullptr));
    // Turn on MXP so the protocol methods are active
    doc.m_bMXP      = true;
    doc.m_iUseMXP   = eUseMXP;   // always on
    doc.m_iMXP_mode = eMXP_open;
    return doc;
}

// Feed a NUL-terminated string of bytes through Phase_MXP_ELEMENT one char
// at a time (stopping before '>' so the caller can inspect mid-stream state).
static void feed_element(CMUSHclientDoc& doc, const char* s)
{
    for (const char* p = s; *p; ++p)
        doc.Phase_MXP_ELEMENT((unsigned char)*p);
}

// ── Phase_MXP_ELEMENT ─────────────────────────────────────────────────────

TEST(MXPElement, ClosingBraceDispatchesAndReturnsToNONE)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase        = HAVE_MXP_ELEMENT;
    doc.m_strMXPstring.Empty();
    // Feed a minimal tag name then close it — MXP_collected_element fires.
    // We rely on the error path (empty tag) rather than full element dispatch.
    doc.Phase_MXP_ELEMENT('>');
    // After '>', phase must be NONE regardless of outcome.
    EXPECT_EQ(NONE, doc.m_phase);
}

TEST(MXPElement, OpeningBraceInsideTagResetsAndStaysInElement)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase        = HAVE_MXP_ELEMENT;
    doc.m_strMXPstring = "partial";
    // '<' inside a tag is an error but MXP string resets and phase stays ELEMENT.
    doc.Phase_MXP_ELEMENT('<');
    // m_strMXPstring is cleared
    EXPECT_TRUE(doc.m_strMXPstring.IsEmpty());
}

TEST(MXPElement, SingleQuoteEntersQuotePhase)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase        = HAVE_MXP_ELEMENT;
    doc.m_strMXPstring.Empty();
    doc.Phase_MXP_ELEMENT('\'');
    EXPECT_EQ(HAVE_MXP_QUOTE, doc.m_phase);
    // The quote character itself is collected
    EXPECT_EQ(CString("'"), doc.m_strMXPstring);
    EXPECT_EQ('\'', doc.m_cMXPquoteTerminator);
}

TEST(MXPElement, DoubleQuoteEntersQuotePhase)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase        = HAVE_MXP_ELEMENT;
    doc.m_strMXPstring.Empty();
    doc.Phase_MXP_ELEMENT('"');
    EXPECT_EQ(HAVE_MXP_QUOTE, doc.m_phase);
    EXPECT_EQ('"', doc.m_cMXPquoteTerminator);
}

TEST(MXPElement, CommentPrefixEntersCommentPhase)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase        = HAVE_MXP_ELEMENT;
    doc.m_strMXPstring = "!-";
    // Third hyphen completes "!--" prefix → comment phase
    doc.Phase_MXP_ELEMENT('-');
    EXPECT_EQ(HAVE_MXP_COMMENT, doc.m_phase);
}

TEST(MXPElement, OrdinaryCharsAccumulate)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase        = HAVE_MXP_ELEMENT;
    doc.m_strMXPstring.Empty();
    feed_element(doc, "bold");
    EXPECT_EQ(HAVE_MXP_ELEMENT, doc.m_phase);
    EXPECT_EQ(CString("bold"), doc.m_strMXPstring);
}

// ── Phase_MXP_COMMENT ─────────────────────────────────────────────────────

TEST(MXPComment, NonCloseCharAccumulates)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase        = HAVE_MXP_COMMENT;
    doc.m_strMXPstring = "!--";
    doc.Phase_MXP_COMMENT('x');
    EXPECT_EQ(HAVE_MXP_COMMENT, doc.m_phase);
    EXPECT_EQ(CString("!--x"), doc.m_strMXPstring);
}

TEST(MXPComment, CloseWithoutDashDashAccumulates)
{
    // '>' without preceding "--" is not end of comment
    CMUSHclientDoc doc = make_doc();
    doc.m_phase        = HAVE_MXP_COMMENT;
    doc.m_strMXPstring = "!--text";   // doesn't end with "--"
    doc.Phase_MXP_COMMENT('>');
    EXPECT_EQ(HAVE_MXP_COMMENT, doc.m_phase);
}

TEST(MXPComment, CloseWithDashDashTerminates)
{
    // '>' after "--" ends the comment → NONE
    CMUSHclientDoc doc = make_doc();
    doc.m_phase        = HAVE_MXP_COMMENT;
    doc.m_strMXPstring = "!--comment--";
    doc.Phase_MXP_COMMENT('>');
    EXPECT_EQ(NONE, doc.m_phase);
}

// ── Phase_MXP_QUOTE ───────────────────────────────────────────────────────

TEST(MXPQuote, NonTerminatorAccumulates)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase              = HAVE_MXP_QUOTE;
    doc.m_cMXPquoteTerminator = '"';
    doc.m_strMXPstring       = "\"";
    doc.Phase_MXP_QUOTE('a');
    EXPECT_EQ(HAVE_MXP_QUOTE, doc.m_phase);
    EXPECT_EQ(CString("\"a"), doc.m_strMXPstring);
}

TEST(MXPQuote, TerminatorReturnsToElement)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase              = HAVE_MXP_QUOTE;
    doc.m_cMXPquoteTerminator = '"';
    doc.m_strMXPstring       = "\"hello";
    doc.Phase_MXP_QUOTE('"');
    EXPECT_EQ(HAVE_MXP_ELEMENT, doc.m_phase);
    // Terminator char itself is still collected
    EXPECT_EQ(CString("\"hello\""), doc.m_strMXPstring);
}

TEST(MXPQuote, SingleQuoteTerminates)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase              = HAVE_MXP_QUOTE;
    doc.m_cMXPquoteTerminator = '\'';
    doc.m_strMXPstring       = "'val";
    doc.Phase_MXP_QUOTE('\'');
    EXPECT_EQ(HAVE_MXP_ELEMENT, doc.m_phase);
}

// ── Phase_MXP_ENTITY ──────────────────────────────────────────────────────

TEST(MXPEntity, SemicolonDispatchesAndReturnsToNONE)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase        = HAVE_MXP_ENTITY;
    doc.m_strMXPstring = "amp";
    doc.Phase_MXP_ENTITY(';');
    EXPECT_EQ(NONE, doc.m_phase);
}

TEST(MXPEntity, AmpersandInsideResetsString)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase        = HAVE_MXP_ENTITY;
    doc.m_strMXPstring = "partial";
    doc.Phase_MXP_ENTITY('&');
    EXPECT_TRUE(doc.m_strMXPstring.IsEmpty());
}

TEST(MXPEntity, OpenBraceInsideSwitchesToElement)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase        = HAVE_MXP_ENTITY;
    doc.m_strMXPstring = "partial";
    doc.Phase_MXP_ENTITY('<');
    EXPECT_EQ(HAVE_MXP_ELEMENT, doc.m_phase);
    EXPECT_TRUE(doc.m_strMXPstring.IsEmpty());
}

TEST(MXPEntity, OrdinaryCharsAccumulate)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase        = HAVE_MXP_ENTITY;
    doc.m_strMXPstring.Empty();
    doc.Phase_MXP_ENTITY('l');
    doc.Phase_MXP_ENTITY('t');
    EXPECT_EQ(CString("lt"), doc.m_strMXPstring);
    EXPECT_EQ(HAVE_MXP_ENTITY, doc.m_phase);
}

// ── End-to-end: MXP_collected_element dispatch ────────────────────────────

TEST(MXPCollected, EmptyStringLogsError)
{
    // Empty element string should record an error (m_iMXPerrors increments)
    // and not crash.
    CMUSHclientDoc doc = make_doc();
    long long errs_before = doc.m_iMXPerrors;
    doc.m_strMXPstring.Empty();
    doc.MXP_collected_element();
    EXPECT_GT(doc.m_iMXPerrors, errs_before);
}

TEST(MXPCollected, SlashPrefixCallsEndTag)
{
    // "/bold" should invoke MXP_EndTag without crashing.
    CMUSHclientDoc doc = make_doc();
    doc.m_strMXPstring = "/bold";
    doc.MXP_collected_element();   // must not throw/assert
}

TEST(MXPCollected, BangPrefixCallsDefinition)
{
    // "!--comment" is a definition; should not crash.
    CMUSHclientDoc doc = make_doc();
    doc.m_strMXPstring = "!--comment";
    doc.MXP_collected_element();
}

TEST(MXPCollected, NormalTagCallsStartTag)
{
    // "b" (bold) — a built-in MXP atomic tag.
    CMUSHclientDoc doc = make_doc();
    doc.m_iMXP_mode    = eMXP_secure;  // built-in tags need secure mode
    doc.m_strMXPstring = "b";
    doc.MXP_collected_element();   // must not crash
}

// ── GetWord (mxp/mxputils.cpp) ────────────────────────────────────────────
// Regression test for an out-of-bounds read: on an UNCLOSED quote, GetWord's
// scan loop correctly stops at the string's NUL terminator (*p == 0, still
// in-bounds), but then did `str = ++p` unconditionally — advancing one byte
// PAST that NUL and dereferencing it via CString's assignment, a one-past-
// the-end read. Verify the post-fix behaviour: an unclosed quote consumes
// the rest of the input and leaves nothing to parse, rather than reading
// past the buffer.

TEST(GetWordTest, ClosedQuoteReturnsRemainder)
{
    CString result, remainder = "'hello' world";
    bool bEnd = GetWord(result, remainder);
    EXPECT_FALSE(bEnd);
    EXPECT_STREQ("hello", (const char*)result);
    EXPECT_STREQ(" world", (const char*)remainder);   // leading space not stripped here
}

TEST(GetWordTest, UnclosedQuoteLeavesEmptyRemainder)
{
    CString result, remainder = "'unclosed";
    bool bEnd = GetWord(result, remainder);
    EXPECT_FALSE(bEnd);
    EXPECT_STREQ("unclosed", (const char*)result);
    EXPECT_STREQ("", (const char*)remainder);   // nothing left — not garbage
}

TEST(GetWordTest, UnclosedQuoteEmptyContentLeavesEmptyRemainder)
{
    // Boundary case: the quote is the very last character (zero-length
    // unterminated content) — *p hits NUL immediately after the opening quote.
    CString result, remainder = "'";
    bool bEnd = GetWord(result, remainder);
    EXPECT_FALSE(bEnd);
    EXPECT_STREQ("", (const char*)result);
    EXPECT_STREQ("", (const char*)remainder);
}

MC_TEST_MAIN()
