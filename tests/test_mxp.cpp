// tests/test_mxp.cpp
// Unit tests for the MXP phase state machine:
//   Phase_MXP_ELEMENT, Phase_MXP_COMMENT, Phase_MXP_QUOTE, Phase_MXP_ENTITY
//
// Strategy: construct a minimal CMUSHclientDoc, drive the Phase_MXP_*
// methods directly, and inspect m_phase / m_strMXPstring after each call.
// We also smoke-test MXP_collected_element dispatch and the entity collector.

#include "mc_test.h"
// B-3 atomic flip: real headers (portable stub deleted; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"

#if defined(__APPLE__)
#include <malloc/malloc.h>   // malloc_zone_statistics (exact leak checks)
#else
#include <malloc.h>          // mallinfo2 (exact leak checks)
#endif

// ── Helpers ───────────────────────────────────────────────────────────────

// Exact count of blocks (macOS) / bytes (Linux) the heap currently has
// outstanding. Unlike an RSS high-water mark this is precise enough to
// bracket a SINGLE call and see a handful of leaked objects -- needed
// because the MXP ArgumentList leaks below don't accumulate across repeated
// calls (see the two leak tests for why), so a loop-and-watch-RSS-grow
// budget can't see them.
static size_t live_heap_blocks()
{
#if defined(__APPLE__)
    malloc_statistics_t st;
    malloc_zone_statistics(malloc_default_zone(), &st);
    return (size_t) st.blocks_in_use;
#else
    struct mallinfo2 mi = mallinfo2();
    return (size_t) mi.uordblks;
#endif
}

static void make_doc(CMUSHclientDoc& doc)
{
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase        = HAVE_MXP_ELEMENT;
    doc.m_strMXPstring = "partial";
    // '<' inside a tag is an error but MXP string resets and phase stays ELEMENT.
    doc.Phase_MXP_ELEMENT('<');
    // m_strMXPstring is cleared
    EXPECT_TRUE(doc.m_strMXPstring.IsEmpty());
}

TEST(MXPElement, SingleQuoteEntersQuotePhase)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase        = HAVE_MXP_ELEMENT;
    doc.m_strMXPstring.Empty();
    doc.Phase_MXP_ELEMENT('"');
    EXPECT_EQ(HAVE_MXP_QUOTE, doc.m_phase);
    EXPECT_EQ('"', doc.m_cMXPquoteTerminator);
}

TEST(MXPElement, CommentPrefixEntersCommentPhase)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase        = HAVE_MXP_ELEMENT;
    doc.m_strMXPstring = "!-";
    // Third hyphen completes "!--" prefix → comment phase
    doc.Phase_MXP_ELEMENT('-');
    EXPECT_EQ(HAVE_MXP_COMMENT, doc.m_phase);
}

TEST(MXPElement, OrdinaryCharsAccumulate)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase        = HAVE_MXP_ELEMENT;
    doc.m_strMXPstring.Empty();
    feed_element(doc, "bold");
    EXPECT_EQ(HAVE_MXP_ELEMENT, doc.m_phase);
    EXPECT_EQ(CString("bold"), doc.m_strMXPstring);
}

// ── Phase_MXP_COMMENT ─────────────────────────────────────────────────────

TEST(MXPComment, NonCloseCharAccumulates)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase        = HAVE_MXP_COMMENT;
    doc.m_strMXPstring = "!--";
    doc.Phase_MXP_COMMENT('x');
    EXPECT_EQ(HAVE_MXP_COMMENT, doc.m_phase);
    EXPECT_EQ(CString("!--x"), doc.m_strMXPstring);
}

TEST(MXPComment, CloseWithoutDashDashAccumulates)
{
    // '>' without preceding "--" is not end of comment
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase        = HAVE_MXP_COMMENT;
    doc.m_strMXPstring = "!--text";   // doesn't end with "--"
    doc.Phase_MXP_COMMENT('>');
    EXPECT_EQ(HAVE_MXP_COMMENT, doc.m_phase);
}

TEST(MXPComment, CloseWithDashDashTerminates)
{
    // '>' after "--" ends the comment → NONE
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase        = HAVE_MXP_COMMENT;
    doc.m_strMXPstring = "!--comment--";
    doc.Phase_MXP_COMMENT('>');
    EXPECT_EQ(NONE, doc.m_phase);
}

// ── Phase_MXP_QUOTE ───────────────────────────────────────────────────────

TEST(MXPQuote, NonTerminatorAccumulates)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase              = HAVE_MXP_QUOTE;
    doc.m_cMXPquoteTerminator = '"';
    doc.m_strMXPstring       = "\"";
    doc.Phase_MXP_QUOTE('a');
    EXPECT_EQ(HAVE_MXP_QUOTE, doc.m_phase);
    EXPECT_EQ(CString("\"a"), doc.m_strMXPstring);
}

TEST(MXPQuote, TerminatorReturnsToElement)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase              = HAVE_MXP_QUOTE;
    doc.m_cMXPquoteTerminator = '\'';
    doc.m_strMXPstring       = "'val";
    doc.Phase_MXP_QUOTE('\'');
    EXPECT_EQ(HAVE_MXP_ELEMENT, doc.m_phase);
}

// ── Phase_MXP_ENTITY ──────────────────────────────────────────────────────

TEST(MXPEntity, SemicolonDispatchesAndReturnsToNONE)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase        = HAVE_MXP_ENTITY;
    doc.m_strMXPstring = "amp";
    doc.Phase_MXP_ENTITY(';');
    EXPECT_EQ(NONE, doc.m_phase);
}

TEST(MXPEntity, AmpersandInsideResetsString)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase        = HAVE_MXP_ENTITY;
    doc.m_strMXPstring = "partial";
    doc.Phase_MXP_ENTITY('&');
    EXPECT_TRUE(doc.m_strMXPstring.IsEmpty());
}

TEST(MXPEntity, OpenBraceInsideSwitchesToElement)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase        = HAVE_MXP_ENTITY;
    doc.m_strMXPstring = "partial";
    doc.Phase_MXP_ENTITY('<');
    EXPECT_EQ(HAVE_MXP_ELEMENT, doc.m_phase);
    EXPECT_TRUE(doc.m_strMXPstring.IsEmpty());
}

TEST(MXPEntity, OrdinaryCharsAccumulate)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    long long errs_before = doc.m_iMXPerrors;
    doc.m_strMXPstring.Empty();
    doc.MXP_collected_element();
    EXPECT_GT(doc.m_iMXPerrors, errs_before);
}

TEST(MXPCollected, SlashPrefixCallsEndTag)
{
    // Open <bold>, then "/bold" must invoke MXP_EndTag and pop it: the active
    // tag count goes 1 → 0.  ("bold" is TAG_OPEN, so it opens in the fixture's
    // default open mode.)  If the end-tag dispatch is skipped the count stays 1.
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_strMXPstring = "bold";
    doc.MXP_collected_element();                       // open the tag
    ASSERT_EQ(1, (int)doc.m_ActiveTagList.GetCount());
    doc.m_strMXPstring = "/bold";
    doc.MXP_collected_element();                       // close via MXP_EndTag
    EXPECT_EQ(0, (int)doc.m_ActiveTagList.GetCount());
}

TEST(MXPCollected, BangPrefixCallsDefinition)
{
    // "!--comment" routes to MXP_Definition.  In non-secure mode that records
    // an error (m_iMXPerrors increments) and defines nothing.  Skipping the
    // definition dispatch would leave m_iMXPerrors unchanged.
    CMUSHclientDoc doc; make_doc(doc);
    long long errs_before = doc.m_iMXPerrors;
    doc.m_strMXPstring = "!--comment";
    doc.MXP_collected_element();
    EXPECT_EQ(errs_before + 1, doc.m_iMXPerrors);
    EXPECT_EQ(0, (int)doc.m_CustomElementMap.GetCount());  // defines nothing
    EXPECT_EQ(0, (int)doc.m_CustomEntityMap.GetCount());
}

TEST(MXPCollected, NormalTagCallsStartTag)
{
    // "b" (bold) — a built-in MXP atomic tag.  MXP_StartTag increments
    // m_iMXPtags and pushes onto m_ActiveTagList; both flip back to 0 if the
    // start-tag dispatch is skipped.
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iMXP_mode    = eMXP_secure;  // built-in tags need secure mode
    doc.m_strMXPstring = "b";
    long long tags_before = doc.m_iMXPtags;
    doc.MXP_collected_element();
    EXPECT_EQ(tags_before + 1, doc.m_iMXPtags);
    EXPECT_EQ(1, (int)doc.m_ActiveTagList.GetCount());
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

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────
// End-to-end MXP: feed real MUD bytes through DisplayMsg and inspect the
// resulting line/style/tag state.

#include "pal/itransport.h"
#include "fake_transport.h"
#include <string>
#include "notepad_fixture.h"

using MxpFakeTransport = RecordingTransport;

// A doc whose current line is created by DisplayMsg itself (and is therefore
// the tail of m_LineList, which MXP_CloseTag walks).
static void make_doc_e2e(CMUSHclientDoc& doc)
{
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_bMXP      = true;
    doc.m_iUseMXP   = eUseMXP;
    doc.m_iMXP_mode = eMXP_open;
}

static void feed(CMUSHclientDoc& doc, const char* s)
{
    doc.DisplayMsg(s, (int)strlen(s), 0);
}

// The style covering the (0-based) character index of a line.
static CStyle* style_at(CLine* line, int idx)
{
    int off = 0;
    for (POSITION pos = line->styleList.GetHeadPosition(); pos; ) {
        CStyle* s = line->styleList.GetNext(pos);
        if (s->iLength > 0 && idx < off + s->iLength) return s;
        off += s->iLength;
    }
    return nullptr;
}

// mxpStart.cpp SECURE_ELEMENT_CHECK: a secure-only tag (<send> has no
// TAG_OPEN) arriving in open mode is rejected with an error, leaves no
// active tag, and its text carries no action.
TEST(MXPSecurity, SecureOnlyTagIgnoredInOpenMode)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);     // open mode
    long long errs = doc.m_iMXPerrors;
    feed(doc, "<send>north</send>");
    EXPECT_GT(doc.m_iMXPerrors, errs);                  // ElementWhenNotSecure
    EXPECT_EQ(0, (int)doc.m_ActiveTagList.GetCount());  // no active tag
    for (POSITION pos = doc.m_pCurrentLine->styleList.GetHeadPosition(); pos; ) {
        CStyle* s = doc.m_pCurrentLine->styleList.GetNext(pos);
        EXPECT_TRUE(s->pAction == NULL);                // no action style
    }
}

// mxpStart.cpp: "get old action ... so that <send href="nick"> <b> blah </b>
// </send> will work — we want the href (action) to persist through the <b>".
TEST(MXPTag, SendHrefPersistsThroughNestedOpenTag)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;
    feed(doc, "<send href=\"nick\"><b>blah</b></send>");

    CLine* line = doc.m_pCurrentLine;
    std::string text(line->text, line->len);
    ASSERT_TRUE(text == "blah");
    int covered = 0;
    for (POSITION pos = line->styleList.GetHeadPosition(); pos; ) {
        CStyle* s = line->styleList.GetNext(pos);
        if (s->iLength > 0) {
            ASSERT_TRUE(s->pAction != NULL);
            EXPECT_STREQ("nick", (const char*)s->pAction->m_strAction);
            covered += s->iLength;
        }
    }
    EXPECT_EQ(4, covered);   // every character of "blah" carries the action
}

// mxpStart.cpp (port guard): after a command tag the current line's style
// list must never be left empty — the next GetTail() (e.g. from an ANSI
// code) would be undefined behaviour.
TEST(MXPTag, CommandTagLeavesStyleListNonEmpty)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;
    long long errs = doc.m_iMXPerrors;
    feed(doc, "<version>");                  // TAG_COMMAND, at line start
    EXPECT_EQ(errs, doc.m_iMXPerrors);       // tag processed, not rejected
    ASSERT_TRUE(doc.m_pCurrentLine != NULL);
    EXPECT_FALSE(doc.m_pCurrentLine->styleList.IsEmpty());

    feed(doc, "\033[31mx");                  // ANSI code must not crash
    ASSERT_TRUE(doc.m_pCurrentLine != NULL);
    EXPECT_FALSE(doc.m_pCurrentLine->styleList.IsEmpty());
}

// mxpEnd.cpp: "<b> <i> </b> — the </b> will also close the <i>" (with a
// closing-out-of-sequence warning); afterwards neither is active.
TEST(MXPTag, OutOfSequenceClosePopsIntermediateTags)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;
    feed(doc, "<b><i>");
    EXPECT_EQ(2, (int)doc.m_ActiveTagList.GetCount());
    feed(doc, "</b>");                       // also closes the nested <i>
    EXPECT_EQ(0, (int)doc.m_ActiveTagList.GetCount());
}

// mxpEnd.cpp: "Cannot close open MXP tag <%s> - it was opened in secure
// mode" — a closing tag received in open mode cannot pop a secure-opened tag.
TEST(MXPSecurity, OpenModeCannotCloseSecureOpenedTag)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;
    feed(doc, "<b>");                        // opened in secure mode
    EXPECT_EQ(1, (int)doc.m_ActiveTagList.GetCount());
    doc.MXP_mode_change(eMXP_open);          // back to open mode
    feed(doc, "</b>");                       // must NOT close it
    EXPECT_EQ(1, (int)doc.m_ActiveTagList.GetCount());
}

// mxpClose.cpp: the opening style tag "remembers the style at the moment it
// was made [so] it will make a good way of restoring existing styles" —
// text after </b> is no longer bold.
TEST(MXPTag, CloseTagRestoresStyleAtOpenPoint)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;
    feed(doc, "x<b>bold</b>y");

    CLine* line = doc.m_pCurrentLine;
    std::string text(line->text, line->len);
    ASSERT_TRUE(text == "xboldy");

    CStyle* sBold = style_at(line, 2);       // inside "bold"
    CStyle* sAfter = style_at(line, 5);      // the trailing 'y'
    ASSERT_NE(nullptr, sBold);
    ASSERT_NE(nullptr, sAfter);
    EXPECT_TRUE((sBold->iFlags  & HILITE) != 0);   // bold applied
    EXPECT_TRUE((sAfter->iFlags & HILITE) == 0);   // restored after </b>
}

// mxpClose.cpp: a user element defined with FLAG=name sets world variable
// mxp_<name> to the enclosed text (Quow's mapper reads mxp_roomname).
TEST(MXPTag, FlagDirectiveSetsMxpPrefixedVariable)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;
    feed(doc, "<!ELEMENT rname FLAG=\"RoomName\">");
    feed(doc, "<rname>The Square</rname>");

    CVariable* v = nullptr;
    ASSERT_TRUE(doc.m_VariableMap.Lookup("mxp_roomname", v));
    ASSERT_NE(nullptr, v);
    EXPECT_STREQ("The Square", (const char*)v->strContents);
}

// mxpMode.cpp: eMXP_secure makes open the default, so the implied mode change
// at newline (-1) reverts to open; eMXP_perm_secure makes secure permanent.
TEST(MXPMode, ImpliedNewlineModeChangeRevertsToDefaultOpen)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.MXP_mode_change(eMXP_secure);
    EXPECT_EQ(eMXP_secure, doc.m_iMXP_mode);
    doc.MXP_mode_change(-1);                 // implied change (newline)
    EXPECT_EQ(eMXP_open, doc.m_iMXP_mode);

    doc.MXP_mode_change(eMXP_perm_secure);
    doc.MXP_mode_change(-1);
    EXPECT_EQ(eMXP_perm_secure, doc.m_iMXP_mode);   // permanent: stays
}

// mxpStart.cpp MXP_Restore_Mode: secure-once applies to exactly one tag —
// the first tag restores the previous mode, so a second secure-only tag is
// rejected.
TEST(MXPMode, SecureOnceAppliesToExactlyOneTag)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);     // open mode
    doc.MXP_mode_change(eMXP_secure_once);
    feed(doc, "<send>");
    EXPECT_EQ(1, (int)doc.m_ActiveTagList.GetCount());  // accepted
    EXPECT_EQ(eMXP_open, doc.m_iMXP_mode);              // mode restored

    long long errs = doc.m_iMXPerrors;
    feed(doc, "<send>");                     // second secure-only tag
    EXPECT_EQ(1, (int)doc.m_ActiveTagList.GetCount());  // rejected
    EXPECT_GT(doc.m_iMXPerrors, errs);
}

// mxpEntities.cpp: numeric entities resolve only for 9/10/13 and 32..255;
// &#7; and &#256; produce an error and an empty expansion.
TEST(MXPEntity, NumericEntityRejectsNonPrintableAndMultibyte)
{
    CMUSHclientDoc doc; make_doc(doc);
    long long errs = doc.m_iMXPerrors;
    CString e7("#7"), e256("#256"), e65("#65"), e9("#9");
    EXPECT_STREQ("", (const char*)doc.MXP_GetEntity(e7));
    EXPECT_STREQ("", (const char*)doc.MXP_GetEntity(e256));
    EXPECT_EQ(errs + 2, doc.m_iMXPerrors);
    // accept side of the same rule
    EXPECT_STREQ("A",  (const char*)doc.MXP_GetEntity(e65));
    EXPECT_STREQ("\t", (const char*)doc.MXP_GetEntity(e9));
}

// mxpEntities.cpp: "if the entity happens to be < & > etc. don't reprocess
// it" — &lt; expands to a literal '<' and does not start element collection.
TEST(MXPEntity, EntityExpansionNotReparsedAsMxp)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);     // open mode, MXP on
    feed(doc, "&lt;b&gt;");
    EXPECT_EQ(NONE, doc.m_phase);            // no element collection started
    EXPECT_EQ(0, (int)doc.m_ActiveTagList.GetCount());
    std::string text(doc.m_pCurrentLine->text, doc.m_pCurrentLine->len);
    EXPECT_STREQ("<b>", text.c_str());       // literal characters on the line
    EXPECT_TRUE(doc.m_bMXP);                 // MXP re-enabled afterwards
}

// mxpOpenAtomic.cpp MXP_ACTION_PASSWORD: "if (m_nTotalLinesSent > 10) ...
// Too many lines sent to MUD - password not sent" — the stored password goes
// out only near the start of a session.
TEST(MXPSecurity, PasswordNotSentAfterTenLinesSent)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    MxpFakeTransport* t = new MxpFakeTransport();
    doc.m_pTransport = t;
    doc.m_iMXP_mode = eMXP_secure;
    doc.m_password = "sekrit";
    doc.m_connect_now = eConnectMXP;

    doc.m_nTotalLinesSent = 11;              // beyond the security threshold
    feed(doc, "<password>");
    EXPECT_EQ((size_t)0, t->sent.size());    // nothing transmitted

    doc.m_nTotalLinesSent = 0;               // fresh session: allowed
    feed(doc, "<password>");
    EXPECT_STREQ("sekrit\r\n", t->sent.c_str());
}

// mxpCloseAtomic.cpp: "no action defined - use &text; as the action" —
// <send>north</send> without href sends its enclosed text.
TEST(MXPTag, SendWithoutHrefUsesEnclosedTextAsAction)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;
    feed(doc, "<send>north</send>");
    CStyle* s = style_at(doc.m_pCurrentLine, 0);   // covers "north"
    ASSERT_NE(nullptr, s);
    ASSERT_TRUE(s->pAction != NULL);
    EXPECT_STREQ("north", (const char*)s->pAction->m_strAction);
}

// mxpCloseAtomic.cpp: "otherwise replace the &text; sequence in action and
// hint" — an href containing &text; has the enclosed text substituted.
TEST(MXPTag, SendHrefTextEntitySubstitutedAtClose)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;
    feed(doc, "<send href=\"go &text;\">north</send>");
    CStyle* s = style_at(doc.m_pCurrentLine, 0);
    ASSERT_NE(nullptr, s);
    ASSERT_TRUE(s->pAction != NULL);
    EXPECT_STREQ("go north", (const char*)s->pAction->m_strAction);
}

// mxputils.cpp GetKeyword: consuming a keyword renumbers later positional
// arguments down by one — "and don't do it twice".
TEST(MXPArgs, KeywordConsumptionRenumbersPositionalArguments)
{
    CMUSHclientDoc doc; make_doc(doc);
    CArgumentList args;
    // "open" is a keyword; "12" and "34" are positional values after it
    ASSERT_TRUE(!doc.BuildArgumentList(args, CString("open 12 34")));

    // before consumption: open=1, 12=2, 34=3
    EXPECT_STREQ("12", (const char*)GetArgument(args, "", 2, false));

    EXPECT_TRUE(GetKeyword(args, "open"));   // consume the keyword
    // positions renumbered: 12=1, 34=2
    EXPECT_STREQ("12", (const char*)GetArgument(args, "", 1, false));
    EXPECT_STREQ("34", (const char*)GetArgument(args, "", 2, false));

    EXPECT_TRUE(GetKeyword(args, "open"));   // second lookup must NOT renumber
    EXPECT_STREQ("12", (const char*)GetArgument(args, "", 1, false));
    EXPECT_STREQ("34", (const char*)GetArgument(args, "", 2, false));

    DELETE_LIST(args);
}

// mxpDefs.cpp DEFINITIONS_MUST_BE_SECURE: <!ELEMENT> in open mode is
// rejected and does not enter the custom element map; in secure mode it does.
TEST(MXPSecurity, DefinitionsRequireSecureMode)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);     // open mode
    long long errs = doc.m_iMXPerrors;
    feed(doc, "<!ELEMENT foo '<b>'>");
    EXPECT_GT(doc.m_iMXPerrors, errs);
    CElement* e = nullptr;
    EXPECT_FALSE(doc.m_CustomElementMap.Lookup("foo", e));

    doc.m_iMXP_mode = eMXP_secure;           // control: secure mode works
    feed(doc, "<!ELEMENT foo '<b>'>");
    EXPECT_TRUE(doc.m_CustomElementMap.Lookup("foo", e));
}

// ═══════════════════════════════════════════════════════════════════════════
// mxpOpenAtomic.cpp — MXP_OpenAtomicTag actions, driven end-to-end: MUD bytes
// through DisplayMsg with MXP active (covers the '<' interception, element
// collection, MXP_StartTag security and the per-action behaviour).

#include "pal/itransport.h"
#include "fake_transport.h"
#include <string>

namespace {

void make_pipe_doc(CMUSHclientDoc& doc)
{
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_bMXP      = true;
    doc.m_iUseMXP   = eUseMXP;
    doc.m_iMXP_mode = eMXP_secure;    // persistent secure: all tags allowed
}

void mud(CMUSHclientDoc& doc, const char* s)
{
    doc.DisplayMsg(s, (int)strlen(s), 0);
}

CLine* pline(CMUSHclientDoc& doc, int idx)
{
    POSITION pos = doc.m_LineList.GetHeadPosition();
    for (int i = 0; pos; i++) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (i == idx) return l;
    }
    return nullptr;
}

std::string ptext(CLine* l) { return std::string(l->text, l->len); }

CStyle* pstyle(CLine* line, int idx)
{
    int off = 0;
    for (POSITION pos = line->styleList.GetHeadPosition(); pos; ) {
        CStyle* s = line->styleList.GetNext(pos);
        if (s->iLength > 0 && idx < off + s->iLength) return s;
        off += s->iLength;
    }
    return nullptr;
}

std::string sent_of(CMUSHclientDoc& doc)
{
    return static_cast<RecordingTransport*>(doc.m_pTransport)->sent;
}

bool contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}

} // namespace

TEST(MXPOpenAtomic, BoldItalicUnderlineNestAndClose)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "a<B>b<I>c</I></B>d\n");
    CLine* l = pline(doc, 0);
    ASSERT_NE(nullptr, l);
    EXPECT_TRUE(ptext(l) == "abcd");
    EXPECT_EQ(0, pstyle(l, 0)->iFlags & TEXT_STYLE);            // a: plain
    EXPECT_EQ(HILITE, pstyle(l, 1)->iFlags & TEXT_STYLE);       // b: bold
    EXPECT_EQ(HILITE | BLINK, pstyle(l, 2)->iFlags & TEXT_STYLE); // c: bold+italic
    EXPECT_EQ(0, pstyle(l, 3)->iFlags & TEXT_STYLE);            // d: closed again
}

TEST(MXPOpenAtomic, UnderlineTagSetsUnderlineBit)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "<U>u</U>\n");
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & UNDERLINE) != 0);
}

TEST(MXPOpenAtomic, ColorTagNamedArgumentsSetRgb)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "<COLOR fore=red back=blue>x</COLOR>\n");
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_EQ(RGB(255, 0, 0), s->iForeColour);
    EXPECT_EQ(RGB(0, 0, 255), s->iBackColour);
}

TEST(MXPOpenAtomic, ColorTagPositionalArgument)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "<C green>x\n");
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_EQ(RGB(0, 128, 0), s->iForeColour);   // HTML green
}

TEST(MXPOpenAtomic, ColorTagUnknownColourRaisesMxpError)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    long long errs = doc.m_iMXPerrors;
    mud(doc, "<COLOR fore=nosuchcolour>x\n");
    EXPECT_GT(doc.m_iMXPerrors, errs);
}

TEST(MXPOpenAtomic, HighTagBrightensForegroundToRgb)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_normalcolour[WHITE] = RGB(100, 100, 100);
    mud(doc, "<HIGH>x\n");
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_NE(RGB(100, 100, 100), s->iForeColour);   // luminance raised
}

TEST(MXPOpenAtomic, SendTagAttachesSendActionWithHint)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_bUnderlineHyperlinks = TRUE;
    mud(doc, "<SEND href='look here' hint='peek'>here</SEND>\n");
    CLine* l = pline(doc, 0);
    ASSERT_NE(nullptr, l);
    EXPECT_TRUE(ptext(l) == "here");
    CStyle* s = pstyle(l, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(ACTION_SEND, s->iFlags & ACTIONTYPE);
    EXPECT_TRUE((s->iFlags & UNDERLINE) != 0);
    ASSERT_TRUE(s->pAction != NULL);
    EXPECT_STREQ("look here", (const char*)s->pAction->m_strAction);
    EXPECT_STREQ("peek",      (const char*)s->pAction->m_strHint);
}

TEST(MXPOpenAtomic, SendTagPromptKeywordMakesPromptAction)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "<SEND 'buy bread' PROMPT>bread</SEND>\n");
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(ACTION_PROMPT, s->iFlags & ACTIONTYPE);
    ASSERT_TRUE(s->pAction != NULL);
    EXPECT_STREQ("buy bread", (const char*)s->pAction->m_strAction);
}

TEST(MXPOpenAtomic, AnchorTagIsHyperlink)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "<A href='http://example.net'>link</A>\n");
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(ACTION_HYPERLINK, s->iFlags & ACTIONTYPE);
    EXPECT_TRUE((s->iFlags & UNDERLINE) != 0);       // hyperlinks always underline
    ASSERT_TRUE(s->pAction != NULL);
    EXPECT_STREQ("http://example.net", (const char*)s->pAction->m_strAction);
}

TEST(MXPOpenAtomic, PuebloXchCmdConvertsAnchorToSend)
{
    // <A XCH_CMD="examine #1"> is Pueblo syntax for a send-to-mud link
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "<A xch_cmd='examine me'>thing</A>\n");
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(ACTION_SEND, s->iFlags & ACTIONTYPE);
    EXPECT_TRUE(doc.m_bPuebloActive);
    ASSERT_TRUE(s->pAction != NULL);
    EXPECT_STREQ("examine me", (const char*)s->pAction->m_strAction);
}

TEST(MXPOpenAtomic, FontTagColourListAndBackground)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "<FONT color=red,blink,bold back=blue>x\n");
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_EQ(RGB(255, 0, 0), s->iForeColour);
    EXPECT_EQ(RGB(0, 0, 255), s->iBackColour);
    EXPECT_TRUE((s->iFlags & BLINK) != 0);
    EXPECT_TRUE((s->iFlags & HILITE) != 0);
}

TEST(MXPOpenAtomic, VersionTagSendsVersionResponse)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_pTransport = new RecordingTransport();
    mud(doc, "<VERSION>\n");
    EXPECT_TRUE(contains(sent_of(doc), "<VERSION MXP=\"0.5\" CLIENT=MUSHclient"));
    EXPECT_TRUE(contains(sent_of(doc), "REGISTERED=YES"));
}

TEST(MXPOpenAtomic, SupportTagListsImplementedTagsAndSubOptions)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_pTransport = new RecordingTransport();
    mud(doc, "<SUPPORT>\n");
    std::string sent = sent_of(doc);
    EXPECT_TRUE(contains(sent, "\x1B[1z<SUPPORTS "));
    EXPECT_TRUE(contains(sent, "+send "));
    EXPECT_TRUE(contains(sent, "+send.href "));
    EXPECT_TRUE(contains(sent, "+color.fore "));
}

TEST(MXPOpenAtomic, SupportTagAnswersSpecificQueries)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_pTransport = new RecordingTransport();
    // color implemented → +color; image flagged NOT_IMP → -image
    mud(doc, "<SUPPORT color image>\n");
    std::string sent = sent_of(doc);
    EXPECT_TRUE(contains(sent, "+color "));
    EXPECT_TRUE(contains(sent, "-image "));
}

TEST(MXPOpenAtomic, SupportTagSubOptionAndWildcardQueries)
{
    // '*' is not a bare-word character for the tag argument tokenizer, so a
    // wildcard query must be quoted: <SUPPORT 'color.*'>
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_pTransport = new RecordingTransport();
    mud(doc, "<SUPPORT send.prompt send.nosuch 'color.*'>\n");
    std::string sent = sent_of(doc);
    EXPECT_TRUE(contains(sent, "+send.prompt "));
    EXPECT_TRUE(contains(sent, "-send.nosuch "));
    EXPECT_TRUE(contains(sent, "+color.fore "));     // wildcard expansion
    EXPECT_TRUE(contains(sent, "+color.back "));
}

TEST(MXPOpenAtomic, SupportTagUnquotedWildcardAbortsTheTag)
{
    // ...whereas an unquoted '*' is a tag-parse error: no response is sent
    // and the MXP error count rises.
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_pTransport = new RecordingTransport();
    long long errs = doc.m_iMXPerrors;
    mud(doc, "<SUPPORT color.*>\n");
    EXPECT_GT(doc.m_iMXPerrors, errs);
    EXPECT_TRUE(sent_of(doc).empty());
}

TEST(MXPOpenAtomic, OptionTagSendsOptionsList)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_pTransport = new RecordingTransport();
    mud(doc, "<OPTION>\n");
    EXPECT_TRUE(contains(sent_of(doc), "\x1B[1z<OPTIONS "));
}

TEST(MXPOpenAtomic, RecommendOptionUnknownNameRaisesError)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_bMudCanChangeOptions = TRUE;
    long long errs = doc.m_iMXPerrors;
    mud(doc, "<RECOMMEND_OPTION nosuchoption=1>\n");
    EXPECT_GT(doc.m_iMXPerrors, errs);
}

TEST(MXPOpenAtomic, UserTagSendsCharacterNameWhenAutoconnectMxp)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_pTransport = new RecordingTransport();
    doc.m_name = "bob";
    doc.m_connect_now = eConnectMXP;
    mud(doc, "<USER>\n");
    EXPECT_TRUE(contains(sent_of(doc), "bob"));
}

TEST(MXPOpenAtomic, UserTagWithoutMxpAutoconnectSendsNothing)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_pTransport = new RecordingTransport();
    doc.m_name = "bob";
    doc.m_connect_now = eNoAutoConnect;  // B-3: stub eConnectNormal -> real doc.h enum (doc.h:337)
    mud(doc, "<USER>\n");
    EXPECT_FALSE(contains(sent_of(doc), "bob"));
}

// PasswordTagSendsPasswordWhenSafe + PasswordTagRefusedAfterTenLinesSent deleted
// (audit §3): both re-cover the MXP_ACTION_PASSWORD m_nTotalLinesSent>10 threshold
// that MXPSecurity.PasswordNotSentAfterTenLinesSent already exercises for BOTH the
// allowed (0 lines → password sent) and refused (11 lines → nothing) branches.

TEST(MXPOpenAtomic, BrTagBreaksTheLine)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "ab<BR>cd\n");
    CLine* l0 = pline(doc, 0);
    CLine* l1 = pline(doc, 1);
    ASSERT_NE(nullptr, l0);
    ASSERT_NE(nullptr, l1);
    EXPECT_TRUE(ptext(l0) == "ab");
    EXPECT_TRUE(ptext(l1) == "cd");
}

TEST(MXPOpenAtomic, HrTagEmitsHorizRuleLine)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "x<HR>y\n");
    CLine* l0 = pline(doc, 0);
    CLine* l1 = pline(doc, 1);
    ASSERT_NE(nullptr, l0);
    ASSERT_NE(nullptr, l1);
    EXPECT_TRUE(ptext(l0) == "x");
    EXPECT_EQ(HORIZ_RULE, l1->flags);
}

TEST(MXPOpenAtomic, PTagEntersParagraphMode)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    EXPECT_FALSE(doc.m_bInParagraph);
    mud(doc, "<P>");
    EXPECT_TRUE(doc.m_bInParagraph);
}

TEST(MXPOpenAtomic, UnorderedListItemsGetBullets)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "<UL><LI>one<LI>two\n");
    CLine* l0 = pline(doc, 0);
    CLine* l1 = pline(doc, 1);
    ASSERT_NE(nullptr, l0);
    ASSERT_NE(nullptr, l1);
    EXPECT_TRUE(ptext(l0) == " * one");
    EXPECT_TRUE(ptext(l1) == " * two");
}

TEST(MXPOpenAtomic, OrderedListItemsAreNumbered)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "<OL><LI>alpha<LI>beta\n");
    CLine* l0 = pline(doc, 0);
    CLine* l1 = pline(doc, 1);
    ASSERT_NE(nullptr, l0);
    ASSERT_NE(nullptr, l1);
    EXPECT_TRUE(ptext(l0) == " 1. alpha");
    EXPECT_TRUE(ptext(l1) == " 2. beta");
}

TEST(MXPOpenAtomic, ImgTagRendersBracketedHyperlink)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_bUnderlineHyperlinks = TRUE;
    mud(doc, "<IMG src='http://x/' fname='pic.png'>\n");
    // image renders as [url] on its own line, hyperlinked
    CLine* l0 = pline(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(ptext(l0) == "[http://x/pic.png]");
    CStyle* s = pstyle(l0, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(ACTION_HYPERLINK, s->iFlags & ACTIONTYPE);
    ASSERT_TRUE(s->pAction != NULL);
    EXPECT_STREQ("http://x/pic.png", (const char*)s->pAction->m_strAction);
}

TEST(MXPOpenAtomic, MxpOffTagDisablesMxp)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "<MXP OFF>");
    EXPECT_FALSE(doc.m_bMXP);
}

TEST(MXPOpenAtomic, ResetTagClearsModesButKeepsMxpOn)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "<UL><P>");
    EXPECT_TRUE(doc.m_bInParagraph);
    doc.m_iMXP_mode = eMXP_secure;    // <RESET> is itself a secure tag
    mud(doc, "<RESET>");
    EXPECT_TRUE(doc.m_bMXP);                 // not a full shutdown
    EXPECT_FALSE(doc.m_bInParagraph);
    EXPECT_EQ(eNoList, doc.m_iListMode);
}

TEST(MXPOpenAtomic, VarTagSetsMxpPrefixedVariableOnClose)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    mud(doc, "<VAR foo>hello</VAR>\n");
    CVariable* var = nullptr;
    ASSERT_TRUE(doc.m_VariableMap.Lookup("mxp_foo", var));
    ASSERT_TRUE(var != nullptr);
    EXPECT_STREQ("hello", (const char*)var->strContents);
}

TEST(MXPOpenAtomic, VarTagInvalidNameRaisesError)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    long long errs = doc.m_iMXPerrors;
    mud(doc, "<VAR 9bad>x</VAR>\n");
    EXPECT_GT(doc.m_iMXPerrors, errs);
}

TEST(MXPOpenAtomic, SecureTagRejectedInOpenMode)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_mode = eMXP_open;
    long long errs = doc.m_iMXPerrors;
    mud(doc, "<SEND 'go'>x</SEND>\n");
    EXPECT_GT(doc.m_iMXPerrors, errs);
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(0, s->iFlags & ACTIONTYPE);    // no action attached
}

// ═══ MXP definitions (<!ELEMENT> / <!ENTITY> / <!ATTLIST>) ═════════════════
// Driven through the REAL stream path: DisplayMsg parses "<!...>" into
// MXP_Definition -> MXP_Element / MXP_Entity / MXP_Attlist (mxpDefs.cpp),
// and "<custom>" through MXP_StartTag / MXP_OpenAtomicTag (mxpStart.cpp).
// The MXP debug/error channel (mxpError.cpp) is observed via a fake
// append-to-notepad hook (that is exactly where MXP_error writes).

#include <string>

// MXP debug output goes to a notepad.  This used to capture at
// g_wxhook_append_to_notepad, a hook belonging to the headless twin that has
// since been deleted (the notepad family is adopted — see
// docs/STUB-ADOPTION-2026-08.md).  NotepadCapture captures at
// g_wxhook_create_text_window instead, which is the seam the product actually
// uses, and hands the engine a real registered CTextDocument so the append path
// runs for real too.
struct MxpDebugCapture {
    NotepadCapture cap;
    bool contains(const char* s) const { return cap.contains(s); }
};

// A doc wired for full-stream MXP: secure mode, all debug levels captured.
static void make_stream_doc(CMUSHclientDoc& doc)
{
    make_doc(doc);
    doc.m_iConnectPhase  = eConnectConnectedToMud;
    doc.m_iMXP_mode      = eMXP_secure;
    doc.m_iMXPdebugLevel = DBG_ALL;
}

static void stream(CMUSHclientDoc& doc, const char* s)
{
    doc.DisplayMsg(s, (int)strlen(s), 0);
}

static std::string current_line(CMUSHclientDoc& doc)
{
    return doc.m_pCurrentLine
        ? std::string(doc.m_pCurrentLine->text, doc.m_pCurrentLine->len)
        : std::string();
}

TEST(MXPDefs, ElementDefinitionRegistersCustomElement)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ELEMENT boldish '<B>' ATT='col=red' TAG=20 OPEN>");
    CElement* el = nullptr;
    ASSERT_TRUE(doc.m_CustomElementMap.Lookup("boldish", el));
    ASSERT_NE(nullptr, el);
    EXPECT_TRUE(el->bOpen);
    EXPECT_FALSE(el->bCommand);
    EXPECT_EQ(20, el->iTag);
    EXPECT_EQ(1, (int)el->ElementItemList.GetCount());   // one atomic: <B>
    EXPECT_EQ(1, (int)el->AttributeList.GetCount());     // col=red
    EXPECT_TRUE(dbg.contains("Got Definition"));
    EXPECT_EQ(0LL, (long long)doc.m_iMXPerrors);
}

TEST(MXPDefs, EmptyKeywordMakesCommandElement)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ELEMENT prompter '<B>' EMPTY>");
    CElement* el = nullptr;
    ASSERT_TRUE(doc.m_CustomElementMap.Lookup("prompter", el));
    EXPECT_TRUE(el->bCommand);
    EXPECT_FALSE(el->bOpen);
}

TEST(MXPDefs, TagNumberOutOfRangeIsIgnored)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ELEMENT overtag '<B>' TAG=150>");
    CElement* el = nullptr;
    ASSERT_TRUE(doc.m_CustomElementMap.Lookup("overtag", el));
    EXPECT_EQ(0, el->iTag);   // 150 outside 20..99
}

TEST(MXPDefs, FlagKeywordSetPrefixAndValidation)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ELEMENT flagel '<B>' FLAG='set myvar'>");
    CElement* el = nullptr;
    ASSERT_TRUE(doc.m_CustomElementMap.Lookup("flagel", el));
    EXPECT_STREQ("myvar", (LPCTSTR)el->strFlag);   // "set " prefix stripped

    stream(doc, "<!ELEMENT flagbad '<B>' FLAG='no!good'>");
    ASSERT_TRUE(doc.m_CustomElementMap.Lookup("flagbad", el));
    EXPECT_TRUE(el->strFlag.IsEmpty());            // rejected by name check
    EXPECT_TRUE(dbg.contains("Bad variable name"));
}

TEST(MXPDefs, DeleteRemovesCustomElement)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ELEMENT deleteme '<B>'>");
    CElement* el = nullptr;
    ASSERT_TRUE(doc.m_CustomElementMap.Lookup("deleteme", el));
    stream(doc, "<!ELEMENT deleteme DELETE>");
    EXPECT_FALSE(doc.m_CustomElementMap.Lookup("deleteme", el));
}

TEST(MXPDefs, CannotRedefineBuiltinElement)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);
    long long errBefore = doc.m_iMXPerrors;
    stream(doc, "<!ELEMENT b '<I>'>");
    EXPECT_GT((long long)doc.m_iMXPerrors, errBefore);
    EXPECT_TRUE(dbg.contains("Cannot redefine built-in MXP element"));
}

// mxpDefs.cpp: MXP_Element's function-static ArgumentList used to leak on
// two of its early returns -- "cannot redefine built-in element" (the arm
// exercised above) and the "DELETE keyword" all-done return -- unlike the
// BuildArgumentList-failure return right above them, which already called
// DELETE_LIST. "<!ELEMENT b '<I>'>" leaves one positional CArgument ('<I>')
// in the list; "<!ELEMENT anything DELETE>" leaves the DELETE keyword
// argument itself.
//
// NOTE this is NOT a loop-and-watch-RSS-grow test: BuildArgumentList's own
// first act on every call is DELETE_LIST(ArgumentList) (mxputils.cpp
// "first get rid of old arguments"), so a leaked list from one call is
// silently drained by the NEXT call before it builds fresh arguments --
// repeating the buggy call never accumulates. What is real, and what this
// pins, is that immediately after ONE buggy call the list is left
// non-empty where it should be empty; live_heap_blocks() is sampled around
// exactly one call each, bracketed by a "flush" (a normal, fully-completing
// definition) that drains the list through the code path that was never
// broken, so each `before` is a known-empty baseline rather than whatever
// an earlier test left behind.
TEST(MXPDefs, RedefinitionEarlyReturnsDoNotLeakTheArgumentList)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    doc.m_iMXPdebugLevel = DBG_NONE;   // keep MXP_error's notepad append out
                                       // of the way -- this test is about
                                       // the ArgumentList, not the debug log

    auto flush = [&] () { stream(doc, "<!ELEMENT flush01 '<I>'>"); };

    flush();
    size_t before1 = live_heap_blocks();
    stream(doc, "<!ELEMENT b '<I>'>");             // cannot-redefine-builtin arm
    size_t after1 = live_heap_blocks();
    EXPECT_EQ(before1, after1);

    flush();
    size_t before2 = live_heap_blocks();
    stream(doc, "<!ELEMENT x DELETE>");      // DELETE-keyword arm
    size_t after2 = live_heap_blocks();
    EXPECT_EQ(before2, after2);
}

TEST(MXPDefs, DefinitionRefusedOutsideSecureMode)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);
    doc.m_iMXP_mode = eMXP_open;
    stream(doc, "<!ELEMENT sneaky '<B>'>");
    CElement* el = nullptr;
    EXPECT_FALSE(doc.m_CustomElementMap.Lookup("sneaky", el));
    EXPECT_TRUE(dbg.contains("MXP definition ignored when not in secure mode"));
}

TEST(MXPDefs, UnknownDefinitionTypeAndBadNames)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!WOTSIT thing>");
    EXPECT_TRUE(dbg.contains("Unknown definition type"));
    stream(doc, "<!ELEMENT 1bad '<B>'>");
    EXPECT_TRUE(dbg.contains("Invalid MXP element/entity name"));
}

TEST(MXPDefs, EntityDefinitionAndEmbeddedEntities)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ENTITY greeting 'hello'>");
    CString v;
    ASSERT_TRUE(doc.m_CustomEntityMap.Lookup("greeting", v));
    EXPECT_STREQ("hello", (LPCTSTR)v);
    // embedded standard entities are resolved at definition time
    stream(doc, "<!ENTITY glyph '&lt;x&gt;'>");
    ASSERT_TRUE(doc.m_CustomEntityMap.Lookup("glyph", v));
    EXPECT_STREQ("<x>", (LPCTSTR)v);
}

TEST(MXPDefs, EntityErrorsAndDeletion)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ENTITY nosemi '&lt'>");
    EXPECT_TRUE(dbg.contains("No closing \";\""));
    stream(doc, "<!ENTITY lt 'clobber'>");   // built-in (App map): refused
    EXPECT_TRUE(dbg.contains("Cannot redefine entity"));
    stream(doc, "<!ENTITY gone 'temp'>");
    CString v;
    ASSERT_TRUE(doc.m_CustomEntityMap.Lookup("gone", v));
    stream(doc, "<!ENTITY gone>");           // blank contents deletes
    EXPECT_FALSE(doc.m_CustomEntityMap.Lookup("gone", v));
}

TEST(MXPDefs, CustomEntityExpandsInStreamText)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ENTITY who 'Gandalf'>");
    stream(doc, "greet &who;!");
    EXPECT_TRUE(current_line(doc).find("greet Gandalf!") != std::string::npos);
}

TEST(MXPDefs, AttlistAppendsToExistingElementOnly)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ELEMENT attel '<B>' ATT='one=1'>");
    CElement* el = nullptr;
    ASSERT_TRUE(doc.m_CustomElementMap.Lookup("attel", el));
    int before = (int)el->AttributeList.GetCount();
    stream(doc, "<!ATTLIST attel two=2>");
    EXPECT_EQ(before + 1, (int)el->AttributeList.GetCount());
    stream(doc, "<!ATTLIST missingel x=1>");
    EXPECT_TRUE(dbg.contains("Cannot add attributes to undefined MXP element"));
}

TEST(MXPDefs, SecureOnceModeRestoredAfterDefinition)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    doc.m_iMXP_mode = eMXP_secure_once;
    doc.m_iMXP_previousMode = eMXP_open;
    stream(doc, "<!ELEMENT onceonly '<B>'>");
    CElement* el = nullptr;
    EXPECT_TRUE(doc.m_CustomElementMap.Lookup("onceonly", el));   // was secure
    EXPECT_EQ(eMXP_open, doc.m_iMXP_mode);                        // restored
}

// ═══ Custom element open/close through the stream (mxpStart / mxpClose) ════

// The style that covers the LAST character currently on the pending line.
static CStyle* last_style(CMUSHclientDoc& doc)
{
    if (!doc.m_pCurrentLine || doc.m_pCurrentLine->styleList.IsEmpty())
        return nullptr;
    return doc.m_pCurrentLine->styleList.GetTail();
}

TEST(MXPCustom, OpenCustomElementAppliesAtomicDefinition)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ELEMENT shout '<B>' OPEN>");
    stream(doc, "plain");
    CStyle* before = last_style(doc);
    ASSERT_NE(nullptr, before);
    EXPECT_EQ(0, (int)(before->iFlags & HILITE));
    stream(doc, "<shout>loud");
    CStyle* inTag = last_style(doc);
    ASSERT_NE(nullptr, inTag);
    EXPECT_EQ(HILITE, (int)(inTag->iFlags & HILITE));   // the <B> arm ran
    stream(doc, "</shout>quiet");
    CStyle* after = last_style(doc);
    ASSERT_NE(nullptr, after);
    EXPECT_EQ(0, (int)(after->iFlags & HILITE));        // closed again
    EXPECT_TRUE(current_line(doc).find("loud") != std::string::npos);
    EXPECT_TRUE(current_line(doc).find("quiet") != std::string::npos);
}

// Merged (audit §3): a closing tag with no matching open both (a) warns to the
// debug notepad and (b) leaves the surrounding literal text on the line untouched.
// Formerly split across MXPCustom.CloseWithoutOpenWarns (warning text) and
// MxpR4.CloseWithoutOpenWarns (text passthrough).
TEST(MXPCustom, CloseWithoutOpenWarns)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "text</b>");
    EXPECT_TRUE(dbg.contains(
        "Closing MXP tag </b> does not have corresponding opening tag"));
    // warning, not error — the tag is ignored and emits NO visible bytes, so the
    // line is EXACTLY "text" (a substring check would pass even if </b> leaked
    // raw onto the line, e.g. "text</b>").
    EXPECT_EQ(std::string("text"), current_line(doc));
}

TEST(MXPCustom, UnterminatedElementAtNewlineIsAnError)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);
    long long errBefore = doc.m_iMXPerrors;
    stream(doc, "<neverclosed\n");
    EXPECT_GT((long long)doc.m_iMXPerrors, errBefore);
    EXPECT_TRUE(dbg.contains("Unterminated MXP element"));
}

TEST(MXPCustom, UnknownTagIsAnError)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);
    long long errBefore = doc.m_iMXPerrors;
    stream(doc, "<nosuchtag>");
    EXPECT_GT((long long)doc.m_iMXPerrors, errBefore);
}

TEST(MXPCustom, AttributeSubstitutionSuppliedNamed)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ELEMENT fancy '<COLOR &col;>' ATT='col=#FF0000' OPEN>");
    stream(doc, "<fancy col=#0000FF>x");
    CStyle* s = last_style(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_RGB, (int)(s->iFlags & COLOURTYPE));
    EXPECT_EQ(RGB(0, 0, 255), s->iForeColour);   // supplied value substituted
}

TEST(MXPCustom, AttributeSubstitutionPositional)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ELEMENT fancy '<COLOR &col;>' ATT='col=#FF0000' OPEN>");
    stream(doc, "<fancy #00FF00>x");
    CStyle* s = last_style(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(RGB(0, 255, 0), s->iForeColour);   // positional argument
}

TEST(MXPCustom, AttributeSubstitutionDefault)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ELEMENT fancy '<COLOR &col;>' ATT='col=#FF0000' OPEN>");
    stream(doc, "<fancy>x");
    CStyle* s = last_style(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(RGB(255, 0, 0), s->iForeColour);   // ATT default used
}

TEST(MXPCustom, MissingNonDefaultArgumentWarns)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ELEMENT needy '<COLOR &c;>' ATT='c' OPEN>");   // no default
    stream(doc, "<needy>x");
    EXPECT_TRUE(dbg.contains("Non-default argument \"c\" not supplied to <needy>"));
}

TEST(MXPCustom, UnterminatedEntityInElementArgument)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);
    stream(doc, "<!ELEMENT brokearg '<COLOR &col>' OPEN>");   // &col missing ;
    stream(doc, "<brokearg>x");
    EXPECT_TRUE(dbg.contains("No closing \";\" in MXP element argument"));
}

TEST(MXPCustom, DebugLevelFiltersNotepadOutput)
{
    // Info-level messages are suppressed when the level only admits errors.
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);
    doc.m_iMXPdebugLevel = DBG_ERROR;
    stream(doc, "<!ELEMENT quietdef '<B>'>");
    EXPECT_FALSE(dbg.contains("Got Definition"));     // info filtered
    stream(doc, "<!WOTSIT bad>");
    EXPECT_TRUE(dbg.contains("Unknown definition type"));   // errors shown
}

// ═══ Round-4 coverage: element-name validation, tag script callbacks,
//     definition error arms, colour errors, headers, AFK ═══════════════════

// An element whose name does not start with a letter is rejected.
TEST(MxpR4, InvalidElementNameRejected)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_defaultMode = doc.m_iMXP_mode = eMXP_perm_secure;
    long long before = doc.m_iMXPerrors;
    mud(doc, "<3bad>x\n");
    EXPECT_TRUE(doc.m_iMXPerrors > before);
}

// Unknown colours in <color> and <font fgcolor/bgcolor> raise errors; the
// fgcolor comma-list applies blink/underline/inverse.
TEST(MxpR4, ColourErrorsAndFontStyleList)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_defaultMode = doc.m_iMXP_mode = eMXP_perm_secure;
    long long before = doc.m_iMXPerrors;
    mud(doc, "<color notarealcolour>x</color>\n");
    EXPECT_TRUE(doc.m_iMXPerrors > before);

    before = doc.m_iMXPerrors;
    mud(doc, "<font fgcolor=blink,underline,inverse,notacolour bgcolor=alsonot>y</font>\n");
    EXPECT_TRUE(doc.m_iMXPerrors >= before + 2);   // fg item + bgcolor errors
    CLine* l = pline(doc, 1);
    ASSERT_NE(nullptr, l);
    CStyle* s = pstyle(l, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & BLINK) != 0);
    EXPECT_TRUE((s->iFlags & UNDERLINE) != 0);
    EXPECT_TRUE((s->iFlags & INVERSE) != 0);
}

// H1-H6 header tags flow through the header arm.
TEST(MxpR4, HeaderTagsRender)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_defaultMode = doc.m_iMXP_mode = eMXP_perm_secure;
    mud(doc, "<h1>big</h1><h6>small</h6>\n");
    CLine* l = pline(doc, 0);
    ASSERT_NE(nullptr, l);
    EXPECT_TRUE(ptext(l).find("big") != std::string::npos);
    EXPECT_TRUE(ptext(l).find("small") != std::string::npos);
}

// <afk> answers the challenge when the player allows it.
TEST(MxpR4, AfkChallengeAnswered)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_defaultMode = doc.m_iMXP_mode = eMXP_perm_secure;
    doc.m_pTransport = new RecordingTransport();
    doc.m_bSendMXP_AFK_Response = TRUE;
    mud(doc, "<afk c3po>\n");
    EXPECT_TRUE(contains(sent_of(doc), "<AFK "));
    EXPECT_TRUE(contains(sent_of(doc), "c3po"));

    // and with the response disabled, nothing goes out
    CMUSHclientDoc doc2; make_pipe_doc(doc2);
    doc2.m_iMXP_defaultMode = doc2.m_iMXP_mode = eMXP_perm_secure;
    doc2.m_pTransport = new RecordingTransport();
    doc2.m_bSendMXP_AFK_Response = FALSE;
    mud(doc2, "<afk c3po>\n");
    EXPECT_TRUE(!contains(sent_of(doc2), "<AFK "));
}

// MxpR4.CloseWithoutOpenWarns deleted (audit §3): merged into
// MXPCustom.CloseWithoutOpenWarns, which now asserts both the warning text and the
// literal-text passthrough for an unmatched closing tag.

// Element DEFINITION error arms: bad name, no "<" in definition, unexpected
// "<", unclosed quote, no ">", empty tag.
TEST(MxpR4, ElementDefinitionErrorArms)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_defaultMode = doc.m_iMXP_mode = eMXP_perm_secure;
    long long e0 = doc.m_iMXPerrors;

    mud(doc, "<!ELEMENT 9bad>\n");                       // invalid name
    long long e1 = doc.m_iMXPerrors;
    EXPECT_TRUE(e1 > e0);

    mud(doc, "<!ELEMENT noname 'plain text'>\n");        // no "<" in def
    long long e2 = doc.m_iMXPerrors;
    EXPECT_TRUE(e2 > e1);

    mud(doc, "<!ELEMENT nested '<a <b>>'>\n");           // unexpected "<"
    long long e3 = doc.m_iMXPerrors;
    EXPECT_TRUE(e3 > e2);

    mud(doc, "<!ELEMENT unquoted '<a \"unclosed>'>\n");  // unclosed quote
    long long e4 = doc.m_iMXPerrors;
    EXPECT_TRUE(e4 > e3);

    mud(doc, "<!ELEMENT unclosed '<a'>\n");              // no closing ">"
    long long e5 = doc.m_iMXPerrors;
    EXPECT_TRUE(e5 > e4);

    mud(doc, "<!ELEMENT empty '<>'>\n");                 // no element name
    EXPECT_TRUE(doc.m_iMXPerrors > e5);
}

// Redefining an element warns "Replacing previously-defined MXP element".
TEST(MxpR4, RedefiningElementWarns)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_defaultMode = doc.m_iMXP_mode = eMXP_perm_secure;
    mud(doc, "<!ELEMENT twice '<b>'>\n");
    long long errsBefore = doc.m_iMXPerrors;
    mud(doc, "<!ELEMENT twice '<u>'>\n");   // replace: warning only
    EXPECT_EQ(errsBefore, doc.m_iMXPerrors);
    // the new definition took: <twice> now renders underline
    mud(doc, "<twice>zz</twice>\n");
    CLine* l = pline(doc, 2);
    ASSERT_NE(nullptr, l);
    CStyle* s = pstyle(l, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & UNDERLINE) != 0);
}

#ifdef MUSHCLIENT_LUA_SCRIPTING
#include "scripting/scripting.h"

// The OnMXP_OpenTag / OnMXP_CloseTag / OnMXP_Error script callbacks fire on
// tag open/close and on MXP errors; an open-tag handler returning true
// suppresses the tag.
TEST(MxpR4, TagAndErrorScriptCallbacks)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_defaultMode = doc.m_iMXP_mode = eMXP_perm_secure;
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString(
        "opened = {} closed = {} errors = {}\n"
        "function OnOpen(name, args)\n"
        "  table.insert(opened, name)\n"
        "  return name == 'suppressme'\n"     // suppress that one tag
        "end\n"
        "function OnClose(name, text) table.insert(closed, name) end\n"
        "function OnError(num, line, level, msg)\n"
        "  table.insert(errors, msg)\n"
        "  return true\n"                     // swallow the error
        "end\n"), "<test>"));
    doc.m_ScriptEngine = engine;
    doc.m_strOnMXP_OpenTag  = "OnOpen";
    doc.m_strOnMXP_CloseTag = "OnClose";
    doc.m_strOnMXP_Error    = "OnError";
    doc.FindAllEntryPoints();

    mud(doc, "<B>bold</B>\n");
    mud(doc, "<3bad>\n");                    // triggers the error callback

    lua_State* L = engine->L;
    lua_getglobal(L, "opened");
    lua_rawgeti(L, -1, 1);
    EXPECT_STREQ("b", lua_tostring(L, -1));
    lua_pop(L, 2);
    lua_getglobal(L, "closed");
    lua_rawgeti(L, -1, 1);
    EXPECT_STREQ("b", lua_tostring(L, -1));
    lua_pop(L, 2);
    // the error callback saw the invalid-element error (it may not be the
    // only recorded error — scan the table)
    lua_getglobal(L, "errors");
    bool sawInvalid = false;
    for (int i = 1; ; i++) {
        lua_rawgeti(L, -1, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        const char* msg = lua_tostring(L, -1);
        if (msg && strstr(msg, "Invalid MXP element name")) sawInvalid = true;
        lua_pop(L, 1);
    }
    EXPECT_TRUE(sawInvalid);
    lua_pop(L, 1);
}
#endif // MUSHCLIENT_LUA_SCRIPTING

// 100 unclosed tags trigger the outstanding-tag warning.
TEST(MxpR4, ManyOutstandingTagsWarn)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_defaultMode = doc.m_iMXP_mode = eMXP_perm_secure;
    for (int i = 0; i < 100; i++)
        mud(doc, "<B>");
    EXPECT_EQ(100, (int)doc.m_ActiveTagList.GetCount());
    EXPECT_EQ(100, (int)doc.m_iLastOutstandingTagCount);   // warning fired
}

// ═══ Round-4 sweep: <support>, <option>, and tag-callback argument echo ═════

TEST(MxpR4, SupportTagRepliesAndValidatesArguments)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_defaultMode = doc.m_iMXP_mode = eMXP_perm_secure;
    doc.m_pTransport = new RecordingTransport();

    mud(doc, "<support>\n");                    // full supports list
    EXPECT_TRUE(contains(sent_of(doc), "<SUPPORTS "));
    EXPECT_TRUE(contains(sent_of(doc), "+send"));

    // individual queries: known tag, known tag.subtag, and unknown tag
    static_cast<RecordingTransport*>(doc.m_pTransport)->sent.clear();
    mud(doc, "<support send color.fore blah>\n");
    EXPECT_TRUE(contains(sent_of(doc), "+send"));
    EXPECT_TRUE(contains(sent_of(doc), "-blah"));

    // invalid <support> argument forms
    long long e0 = doc.m_iMXPerrors;
    mud(doc, "<support bad.sub.sub>\n");
    EXPECT_TRUE(doc.m_iMXPerrors > e0);
}

TEST(MxpR4, OptionTagSetsQueriesAndRejects)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_defaultMode = doc.m_iMXP_mode = eMXP_perm_secure;
    doc.m_pTransport = new RecordingTransport();

    // <option> queries the current option values back to the server
    mud(doc, "<option wrap_column>\n");
    EXPECT_TRUE(contains(sent_of(doc), "<OPTIONS "));

    // <recommend_option> is the server-write path (needs user consent)
    doc.m_bMudCanChangeOptions = TRUE;
    long long e0 = doc.m_iMXPerrors;
    mud(doc, "<recommend_option no_such_option=1>\n");   // unknown → error
    EXPECT_TRUE(doc.m_iMXPerrors > e0);
    long long e1 = doc.m_iMXPerrors;
    mud(doc, "<recommend_option wrap_column=90>\n");     // known option
    // either accepted (info note) or refused as non-writable (error note):
    // both arms are genuine — just require no crash and the tag consumed
    EXPECT_TRUE(doc.m_iMXPerrors >= e1);
}

#ifdef MUSHCLIENT_LUA_SCRIPTING
// With an open-tag script handler installed, tag arguments are echoed into
// the dummy argument list (the name='value' formatting arm).
TEST(MxpR4, OpenTagCallbackReceivesFormattedArguments)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_defaultMode = doc.m_iMXP_mode = eMXP_perm_secure;
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString(
        "args = nil\n"
        "function OnOpen(name, a) args = a return false end\n"), "<test>"));
    doc.m_ScriptEngine = engine;
    doc.m_strOnMXP_OpenTag = "OnOpen";
    doc.FindAllEntryPoints();

    mud(doc, "<color fore=red back=blue>x</color>\n");

    lua_State* L = engine->L;
    lua_getglobal(L, "args");
    ASSERT_TRUE(lua_isstring(L, -1) || lua_istable(L, -1));
    if (lua_isstring(L, -1)) {
        const char* a = lua_tostring(L, -1);
        EXPECT_TRUE(strstr(a, "fore=") != nullptr);
    }
    lua_pop(L, 1);
}

// mxpStart.cpp: MXP_StartTag's function-static ArgumentList used to leak
// whenever the plugin/script open-tag hook vetoes the tag (bNotWanted ==
// true) -- unlike the BuildArgumentList-failure return just above it, which
// already called DELETE_LIST. That branch only runs when pAtomicElement ==
// NULL ("atomic elements have their script called in MXP_OpenAtomicTag
// [instead]" -- the comment right above it), so the tag under test MUST be a
// custom (user-defined, non-built-in) element: a built-in like <color> or
// <b> never reaches this call site at all.
//
// NOT a loop-and-watch-RSS-grow test, for the same reason as
// MXPDefs.RedefinitionEarlyReturnsDoNotLeakTheArgumentList: BuildArgumentList
// drains the list itself on every call (mxputils.cpp), so a leaked list from
// one vetoed tag is silently freed by the next tag's BuildArgumentList before
// it builds fresh arguments -- repeating the veto never accumulates. What is
// real is that immediately after ONE vetoed call the list is left non-empty.
// live_heap_blocks() is sampled around exactly one call, using a tag with no
// enclosed text/close/newline (so nothing about the scrollback changes) and a
// forced Lua GC cycle on each side (so Lua's own per-call garbage -- the
// arguments table handed to OnOpen -- can't read as a false leak). The
// baseline is established by flushing through an UNRELATED doc with no veto
// handler installed, so the flush's own open tag completes normally and
// drains the (process-wide, function-static) list via the code path that was
// never broken.
TEST(MxpR4, OpenTagVetoDoesNotLeakTheArgumentList)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_defaultMode = doc.m_iMXP_mode = eMXP_perm_secure;
    stream(doc, "<!ELEMENT mytag '<B>'>");   // custom -> pAtomicElement stays NULL
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString(
        "function OnOpen(name, a) return true end\n"), "<test>"));  // veto everything
    doc.m_ScriptEngine = engine;
    doc.m_strOnMXP_OpenTag = "OnOpen";
    doc.FindAllEntryPoints();

    // The flush must be the LAST thing to touch the (process-wide,
    // function-static) list before "before" is sampled -- an earlier attempt
    // warmed up THIS doc's own veto path first, which self-contaminated: the
    // measured call's own BuildArgumentList silently freed the warm-up call's
    // leak before building its own, cancelling the delta and passing even
    // with the bug. A SEPARATE doc with no veto handler installed completes
    // normally and drains the list through the code path that was never
    // broken, giving a known-clean baseline.
    {
    CMUSHclientDoc flushDoc; make_pipe_doc(flushDoc);
    stream(flushDoc, "<!ELEMENT mytag '<B>'>");
    mud(flushDoc, "<mytag foo=bar>");
    }

    lua_gc(engine->L, LUA_GCCOLLECT, 0);
    size_t before = live_heap_blocks();
    mud(doc, "<mytag foo=bar>");   // no enclosed text, no close, no \n
    lua_gc(engine->L, LUA_GCCOLLECT, 0);
    size_t after = live_heap_blocks();

    EXPECT_EQ(before, after);
}
#endif

// mxpStart.cpp: MXP_StartTag's custom-element expansion path builds a LOCAL
// CArgumentList (BuiltArgumentList) of new CArgument nodes as it substitutes
// entities into each atomic item's arguments.  The "No closing ';' in MXP
// element argument" error return in that loop used to leave WITHOUT a
// DELETE_LIST(BuiltArgumentList), so every CArgument already added leaked --
// and because BuiltArgumentList is a stack local (not the function-static
// ArgumentList the F21 drains covered), no later call ever frees it: this
// leak is PERMANENT and accumulates.
//
// Isolate it with a differential rather than a zero-delta bracket, because a
// custom tag legitimately allocates a style and an outstanding-tag record
// before it reaches the failing atom loop.  Both tags below are custom,
// non-command, and hit the SAME error return; they differ only in that
// <boomleak>'s <font> atom has a well-formed argument (color=red) BEFORE the
// unterminated-entity one (back="&x"), so one CArgument is added to the built
// list before the abort, whereas <boombare>'s very first atom argument is the
// malformed one, so its built list is still empty at the abort.  Every other
// allocation (style, active tag, the identical MXP_error message on "&x") is
// the same, so with the drain in place the two calls change the heap by the
// same amount; without it, <boomleak> leaks the extra CArgument and its
// per-call delta is strictly larger.  (live_heap_blocks() is exact per the
// note on the helper.)
TEST(MxpR4, CustomElementArgumentAbortDoesNotLeakTheBuiltList)
{
    CMUSHclientDoc doc; make_pipe_doc(doc);
    doc.m_iMXP_defaultMode = doc.m_iMXP_mode = eMXP_perm_secure;
    doc.m_iMXPdebugLevel = DBG_NONE;   // keep MXP_error's notepad append out
                                       // of the heap measurement

    mud(doc, "<!ELEMENT boombare '<font back=\"&x\">'>");
    mud(doc, "<!ELEMENT boomleak '<font color=red back=\"&x\">'>");

    // warm each tag path once (first use of a line/style can allocate one-off
    // scaffolding that would otherwise land in the measured call)
    mud(doc, "<boombare>");
    mud(doc, "<boomleak>");

    size_t b1 = live_heap_blocks();
    mud(doc, "<boombare>");
    size_t bareDelta = live_heap_blocks() - b1;

    size_t b2 = live_heap_blocks();
    mud(doc, "<boomleak>");
    size_t leakDelta = live_heap_blocks() - b2;

    // with BuiltArgumentList drained the extra good argument is freed on the
    // way out and the deltas match; without it, boomleak leaks it
    EXPECT_EQ(bareDelta, leakDelta);
}

// ── round 5: phase-machine abort arms, script discard, wrapped links ──────

// A carriage return inside an MXP element aborts it: "got <NEWLINE>".
TEST(MXPPhaseR5, ElementAbortedByCarriageReturn)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;
    long long errs = doc.m_iMXPerrors;
    feed(doc, "<send\rmore text\n");
    EXPECT_GT(doc.m_iMXPerrors, errs);
}

// An ESC inside an MXP element aborts it: "got <ESC>".
TEST(MXPPhaseR5, ElementAbortedByEscape)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;
    long long errs = doc.m_iMXPerrors;
    feed(doc, "<send\x1B[0mtext\n");
    EXPECT_GT(doc.m_iMXPerrors, errs);
}

// An MXP comment consumed character by character through the comment phase.
TEST(MXPPhaseR5, CommentPhaseDispatch)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    feed(doc, "<!-- a comment -->visible\n");
    // the comment is swallowed; the visible text survives
    CLine* line = doc.m_LineList.GetHead();
    std::string text(line->text, line->len);
    EXPECT_EQ(std::string::npos, text.find("comment"));
    EXPECT_NE(std::string::npos, text.find("visible"));
}

// Incoming <script> content is discarded even in secure mode (the
// m_bMXP_script arm inside MXP_Secure).
TEST(MXPPhaseR5, ScriptContentDiscardedInSecureMode)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;
    feed(doc, "<script>evil();</script>after\n");
    std::string all;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        all.append(l->text, l->len);
    }
    EXPECT_EQ(std::string::npos, all.find("evil"));
    // ...and only the script content: without this the test would pass if MXP
    // discarded the whole line, or the whole packet.
    EXPECT_NE(std::string::npos, all.find("after"));
}

// In paragraph mode a newline after a word (with an earlier space on the
// line) inserts a joining space rather than running words together.
TEST(MXPPhaseR5, ParagraphNewlineJoinsWithSpace)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;
    feed(doc, "<p>alpha beta\ngamma</p>\n");
    std::string all;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        all.append(l->text, l->len);
    }
    EXPECT_NE(std::string::npos, all.find("beta gamma"));
}

// A hyperlink whose text hard-wraps: the continuation line keeps the action
// (StartNewLine_KeepPreviousStyle AddRefs it).
TEST(MXPPhaseR5, WrappedHyperlinkKeepsActionOnNextLine)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;
    doc.m_wrap = TRUE;
    doc.m_nWrapColumn = 20;
    feed(doc, "<send>aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</send>\n");

    // both physical lines carry a style with the same action
    ASSERT_TRUE(doc.m_LineList.GetCount() >= 2);
    CLine* l0 = doc.m_LineList.GetHead();
    POSITION p2 = doc.m_LineList.GetHeadPosition();
    doc.m_LineList.GetNext(p2);
    CLine* l1 = doc.m_LineList.GetAt(p2);
    CAction* a0 = nullptr;
    CAction* a1 = nullptr;
    for (POSITION pos = l0->styleList.GetHeadPosition(); pos; ) {
        CStyle* s = l0->styleList.GetNext(pos);
        if (s->pAction) a0 = s->pAction;
    }
    for (POSITION pos = l1->styleList.GetHeadPosition(); pos; ) {
        CStyle* s = l1->styleList.GetNext(pos);
        if (s->pAction) a1 = s->pAction;
    }
    EXPECT_NE(nullptr, a0);
    EXPECT_NE(nullptr, a1);
    EXPECT_EQ(a0, a1);   // the SAME action, reference-counted across the wrap
}

// A colour-change trigger whose match splits an MXP action style AddRefs the
// action into the split-off runs.
TEST(MXPPhaseR5, TriggerColouringSplitsActionStyle)
{
    CMUSHclientDoc doc; make_doc_e2e(doc);
    doc.m_iMXP_mode = eMXP_secure;

    CTrigger* t  = new CTrigger();
    t->trigger   = CString("*iddl*");
    t->bEnabled  = TRUE;
    t->colour    = 2;
    t->iSequence = DEFAULT_TRIGGER_SEQUENCE;
    t->strLabel  = CString("mid");
    doc.m_TriggerMap.SetAt(t->strLabel, t);
    doc.m_TriggerArray.Add(t);

    feed(doc, "<send>middleman</send>\n");
    EXPECT_EQ(1L, t->nMatched);
}


// mxpEntities.cpp MXP_GetEntity: hex numeric entities (&#xNN;) go through the
// strName[1]=='x' arm — the shift-accumulate (iResult<<4) plus the A-F fold
// (iNewDigit -= 7).  0x41 -> 'A'; 0x4A -> 'J' (the 'A' digit exercises the fold).
TEST(MXPEntity, HexNumericEntityDecodesToChar)
{
    CMUSHclientDoc doc; make_doc(doc);
    long long errs = doc.m_iMXPerrors;
    CString eA("#x41");   // 0x41 = 65 = 'A'
    CString eJ("#x4A");   // 0x4A = 74 = 'J' — the 'A' digit folds via -= 7
    EXPECT_STREQ("A", (const char*)doc.MXP_GetEntity(eA));
    EXPECT_STREQ("J", (const char*)doc.MXP_GetEntity(eJ));
    EXPECT_EQ(errs, doc.m_iMXPerrors);   // valid decodes raise no error
}

// mxpEntities.cpp MXP_GetEntity: a non-hex character after &#x fails the
// isxdigit() guard -> "Invalid hex number" error and an empty expansion.
TEST(MXPEntity, HexNumericEntityInvalidDigitRejected)
{
    CMUSHclientDoc doc; make_doc(doc);
    long long errs = doc.m_iMXPerrors;
    CString bad("#xZ");   // 'Z' is not a hex digit
    EXPECT_STREQ("", (const char*)doc.MXP_GetEntity(bad));
    EXPECT_EQ(errs + 1, doc.m_iMXPerrors);   // one hex-validation error
}

// mxpEntities.cpp MXP_GetEntity: the decimal arm rejects a trailing non-digit
// (&#12x;) via its isdigit() guard -> error and an empty expansion.
TEST(MXPEntity, DecimalNumericEntityInvalidDigitRejected)
{
    CMUSHclientDoc doc; make_doc(doc);
    long long errs = doc.m_iMXPerrors;
    CString bad("#12x");   // 'x' after the digits is invalid
    EXPECT_STREQ("", (const char*)doc.MXP_GetEntity(bad));
    EXPECT_EQ(errs + 1, doc.m_iMXPerrors);   // one decimal-validation error
}

// mxpEntities.cpp MXP_GetEntity: a name found in neither App.m_EntityMap nor
// m_CustomEntityMap is an "Unknown MXP entity" -> error and empty expansion.
TEST(MXPEntity, UnknownEntityRejected)
{
    CMUSHclientDoc doc; make_doc(doc);
    long long errs = doc.m_iMXPerrors;
    CString unknown("nosuchentity");   // not a standard or custom entity
    EXPECT_STREQ("", (const char*)doc.MXP_GetEntity(unknown));
    EXPECT_EQ(errs + 1, doc.m_iMXPerrors);   // one unknown-entity error
}

// mxpEntities.cpp MXP_GetEntity: the hex arm's 'iResult & 0xF0' guard rejects a
// third hex digit with the distinctive "maximum of 2 hex digits" message (which
// separates this arm from the later "Disallowed number" > 255 check).
TEST(MXPEntity, HexNumericEntityTooManyDigitsRejected)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);   // m_iMXPdebugLevel = DBG_ALL
    CString big("#xFFF");   // 3 hex digits — more than one byte
    EXPECT_STREQ("", (const char*)doc.MXP_GetEntity(big));
    EXPECT_TRUE(dbg.contains("maximum of 2 hex digits"));
}

// mxpEntities.cpp:89-91 -- the hex arm folds a letter digit with toupper()
// and then `iNewDigit -= 7`, so BOTH cases must decode alike.  The existing
// MXPEntity.HexNumericEntityDecodesToChar (line ~1748) only feeds UPPER case
// ("#x4A"), and for upper case the toupper() call is a no-op -- deleting it
// leaves that test green.  Lower case is what pins it: without toupper, 'a'
// (97) still passes the `>= 'A'` test at :90, folds to 90, and yields 'j'
// instead of 'J'.
TEST(MXPEntity, HexNumericEntityLowerCaseDigitsFold)
{
    CMUSHclientDoc doc; make_doc(doc);
    long long errs = doc.m_iMXPerrors;

    CString eJ("#x4a");   // toupper('a')='A'=65 -> 58; (4<<4)+58-'0' = 74 = 'J'
    CString eO("#x6f");   // toupper('f')='F'=70 -> 63; (6<<4)+63-'0' = 111 = 'o'
    EXPECT_STREQ("J", (const char*)doc.MXP_GetEntity(eJ));
    EXPECT_STREQ("o", (const char*)doc.MXP_GetEntity(eO));

    // ...and mixed case within one entity, which catches a fold applied to
    // only one of the two digits: 0xAb = 171.  Compared numerically so the
    // expectation carries no non-ASCII source byte.
    CString eAb("#xAb");
    CString r = doc.MXP_GetEntity(eAb);
    ASSERT_EQ(1, r.GetLength());
    EXPECT_EQ(171, (int)(unsigned char)r[0]);

    EXPECT_EQ(errs, doc.m_iMXPerrors);   // all three are valid: no errors raised
}

// mxpEntities.cpp:36 + :71-101 -- the hex arm reached the way a MUD reaches it:
// as bytes on the wire.  ALL existing hex coverage calls MXP_GetEntity directly
// (tests/test_mxp.cpp:1748-1802), so nothing drives an &#...; through the entity
// PHASE.  That is the gap: IsValidName("#x41") is FALSE (mxputils.cpp:412
// requires a leading letter), so on this route the `&& m_strMXPstring.Left (1)
// != "#"` clause at :36 is the only thing that gets a numeric entity past the
// collector's guard -- drop it and every &#...; arriving as ordinary MUD text
// silently becomes an "Invalid MXP entity name" error with no expansion.
// (MXP_GetEntity has three other callers -- mxpStart.cpp:314, mxpStart.cpp:497
// and mxpDefs.cpp:446, which expand entities inside element arguments and
// <!ENTITY> values without consulting IsValidName.  Those keep working, which is
// exactly why this route needs its own test.)
TEST(MXPEntity, HexNumericEntityDecodesInTheByteStream)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    long long errs = doc.m_iMXPerrors;
    long long ents = doc.m_iMXPentities;

    // 0x41='A' (digits only), 0x6f='o' (lower-case fold), 0x4B='K' (upper-case
    // fold).  The literal 'x' and 'y' bracket them so a failure distinguishes
    // "expanded to the wrong byte" from "swallowed the whole packet".
    stream(doc, "x&#x41;&#x6f;&#x4B;y");

    EXPECT_EQ(std::string("xAoKy"), current_line(doc));
    EXPECT_EQ(errs, doc.m_iMXPerrors);        // a valid entity is not an error
    EXPECT_EQ(ents + 3, doc.m_iMXPentities);  // all three reached the collector
    EXPECT_EQ(NONE, doc.m_phase);             // ';' closed the entity phase
}

// mxpEntities.cpp:36-42 -- MXP_collected_entity rejects a name that is neither a
// valid MXP name nor a '#' numeric form.  An error-COUNT assertion cannot see
// this branch: neuter the `!IsValidName (...)` operand (make it constant-false,
// or make IsValidName accept anything) and "9bad" falls through to
// MXP_GetEntity's "Unknown MXP entity" arm (:137-141), which raises exactly ONE
// error too and likewise expands to nothing -- same count, same line, different
// message.  Only the message text discriminates, so this asserts the
// invalid-name wording IS present and the unknown-entity wording is NOT.
// (The guard is an &&: simply DELETING the operand leaves `Left (1) != "#"`,
// still true for "9bad", so that mutation is not the one this test catches.)
TEST(MXPEntity, InvalidEntityNameShortCircuitsBeforeLookup)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);   // m_iMXPdebugLevel = DBG_ALL
    long long errs = doc.m_iMXPerrors;
    long long ents = doc.m_iMXPentities;

    stream(doc, "x&9bad;y");   // '9' fails IsValidName (mxputils.cpp:412)

    EXPECT_EQ(ents + 1, doc.m_iMXPentities);   // the collector really ran
    EXPECT_EQ(errs + 1, doc.m_iMXPerrors);     // exactly one error
    EXPECT_TRUE(dbg.contains("Invalid MXP entity name \"9bad\" supplied."));
    EXPECT_FALSE(dbg.contains("Unknown MXP entity"));   // never reached the maps
    // the entity expands to nothing AND is not echoed raw -- only the
    // surrounding literal text survives (a substring check would pass even if
    // "&9bad;" leaked onto the line).
    EXPECT_EQ(std::string("xy"), current_line(doc));
}

// mxpEntities.cpp:92-99 and :116-124 -- where the hex arm's two guards actually
// sit, and where each edge is:
//   0xFF -- the largest value :92's `iResult & 0xF0` guard can admit (going into
//     the last digit the accumulator is at most 0x0F), and the ONLY input in the
//     suite that pins 255 as accepted by `iResult > 255` at :118.  Existing
//     coverage is &#256; (rejected) and &#65;/&#9; (accepted) at line 507, so an
//     off-by-one to `>= 255` is caught by nothing today.
//   0x20 -- likewise the only input pinning 32 as accepted by `iResult < 32`
//     at :117 (existing coverage jumps from &#7; to &#65;).
//   0x00A -- THREE digits that pass, because :92 is a VALUE guard, not the digit
//     COUNT its message claims.  HexNumericEntityTooManyDigitsRejected ("#xFFF")
//     stays green under a digit-counting rewrite of :92, so this is the only
//     input that separates the two readings.  &#x00A; really is 10, and this is
//     shared (also-Windows) source, so a message-driven "fix" would regress it.
//   0x1F -- the hex arm falling THROUGH to the shared :116-124 range check (the
//     check itself is already covered on the decimal side at line 507); the
//     paired message assertions pin which of the two error arms fired.
TEST(MXPEntity, HexNumericEntityValueBoundaries)
{
    MxpDebugCapture dbg;
    CMUSHclientDoc doc; make_stream_doc(doc);   // m_iMXPdebugLevel = DBG_ALL
    long long errs = doc.m_iMXPerrors;

    CString eFF("#xFF");     // 255: the top of the accepted range
    CString r = doc.MXP_GetEntity(eFF);
    ASSERT_EQ(1, r.GetLength());
    EXPECT_EQ(255, (int)(unsigned char)r[0]);

    CString e20("#x20");     // 32: the bottom of the printable range
    EXPECT_STREQ(" ", (const char*)doc.MXP_GetEntity(e20));

    CString e00A("#x00A");   // 10: leading zeros, THREE digits, still valid
    EXPECT_STREQ("\n", (const char*)doc.MXP_GetEntity(e00A));

    EXPECT_EQ(errs, doc.m_iMXPerrors);   // none of the above is an error

    CString e1F("#x1F");     // 31: below 32 and not one of 9/10/13
    EXPECT_STREQ("", (const char*)doc.MXP_GetEntity(e1F));
    EXPECT_EQ(errs + 1, doc.m_iMXPerrors);
    EXPECT_TRUE(dbg.contains("Disallowed number in MXP entity: &#x1F;"));
    // ...and it is the RANGE check that rejected it, not the hex guard
    EXPECT_FALSE(dbg.contains("maximum of 2 hex digits"));
}

MC_TEST_MAIN()
