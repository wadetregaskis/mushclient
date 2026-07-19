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

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────
// End-to-end MXP: feed real MUD bytes through DisplayMsg and inspect the
// resulting line/style/tag state.

#include "pal/itransport.h"
#include <string>

namespace {
struct MxpFakeTransport : pal::ITransport {
    std::string sent;
    void resolve(const char*, pal::ResolveToken) override {}
    void cancelResolve() override {}
    void connect(unsigned int, unsigned short, const pal::TransportOptions&) override {}
    void send(const char* data, size_t n) override { sent.append(data, n); }
    void startTls(const pal::TlsParams&) override {}
    void close() override {}
    std::string lastErrorString() const override { return "fake"; }
};
} // namespace

// A doc whose current line is created by DisplayMsg itself (and is therefore
// the tail of m_LineList, which MXP_CloseTag walks).
static CMUSHclientDoc make_doc_e2e()
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_bMXP      = true;
    doc.m_iUseMXP   = eUseMXP;
    doc.m_iMXP_mode = eMXP_open;
    return doc;
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
    CMUSHclientDoc doc = make_doc_e2e();     // open mode
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
    CMUSHclientDoc doc = make_doc_e2e();
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
    CMUSHclientDoc doc = make_doc_e2e();
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
    CMUSHclientDoc doc = make_doc_e2e();
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
    CMUSHclientDoc doc = make_doc_e2e();
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
    CMUSHclientDoc doc = make_doc_e2e();
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
    CMUSHclientDoc doc = make_doc_e2e();
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
    CMUSHclientDoc doc = make_doc_e2e();
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
    CMUSHclientDoc doc = make_doc_e2e();     // open mode
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc_e2e();     // open mode, MXP on
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
    CMUSHclientDoc doc = make_doc_e2e();
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
    CMUSHclientDoc doc = make_doc_e2e();
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
    CMUSHclientDoc doc = make_doc_e2e();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc_e2e();     // open mode
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
#include <string>

namespace {

struct RecordingTransport : pal::ITransport {
    std::string sent;
    void resolve(const char*, pal::ResolveToken) override {}
    void cancelResolve() override {}
    void connect(unsigned int, unsigned short, const pal::TransportOptions&) override {}
    void send(const char* data, size_t n) override { sent.append(data, n); }
    void startTls(const pal::TlsParams&) override {}
    void close() override {}
    std::string lastErrorString() const override { return "recording"; }
};

CMUSHclientDoc make_pipe_doc()
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_bMXP      = true;
    doc.m_iUseMXP   = eUseMXP;
    doc.m_iMXP_mode = eMXP_secure;    // persistent secure: all tags allowed
    return doc;
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
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
    mud(doc, "<U>u</U>\n");
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & UNDERLINE) != 0);
}

TEST(MXPOpenAtomic, ColorTagNamedArgumentsSetRgb)
{
    CMUSHclientDoc doc = make_pipe_doc();
    mud(doc, "<COLOR fore=red back=blue>x</COLOR>\n");
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_EQ(RGB(255, 0, 0), s->iForeColour);
    EXPECT_EQ(RGB(0, 0, 255), s->iBackColour);
}

TEST(MXPOpenAtomic, ColorTagPositionalArgument)
{
    CMUSHclientDoc doc = make_pipe_doc();
    mud(doc, "<C green>x\n");
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_EQ(RGB(0, 128, 0), s->iForeColour);   // HTML green
}

TEST(MXPOpenAtomic, ColorTagUnknownColourRaisesMxpError)
{
    CMUSHclientDoc doc = make_pipe_doc();
    long long errs = doc.m_iMXPerrors;
    mud(doc, "<COLOR fore=nosuchcolour>x\n");
    EXPECT_GT(doc.m_iMXPerrors, errs);
}

TEST(MXPOpenAtomic, HighTagBrightensForegroundToRgb)
{
    CMUSHclientDoc doc = make_pipe_doc();
    doc.m_normalcolour[WHITE] = RGB(100, 100, 100);
    mud(doc, "<HIGH>x\n");
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_NE(RGB(100, 100, 100), s->iForeColour);   // luminance raised
}

TEST(MXPOpenAtomic, SendTagAttachesSendActionWithHint)
{
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
    mud(doc, "<SEND 'buy bread' PROMPT>bread</SEND>\n");
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(ACTION_PROMPT, s->iFlags & ACTIONTYPE);
    ASSERT_TRUE(s->pAction != NULL);
    EXPECT_STREQ("buy bread", (const char*)s->pAction->m_strAction);
}

TEST(MXPOpenAtomic, AnchorTagIsHyperlink)
{
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
    doc.m_pTransport = new RecordingTransport();
    mud(doc, "<VERSION>\n");
    EXPECT_TRUE(contains(sent_of(doc), "<VERSION MXP=\"0.5\" CLIENT=MUSHclient"));
    EXPECT_TRUE(contains(sent_of(doc), "REGISTERED=YES"));
}

TEST(MXPOpenAtomic, SupportTagListsImplementedTagsAndSubOptions)
{
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
    doc.m_pTransport = new RecordingTransport();
    long long errs = doc.m_iMXPerrors;
    mud(doc, "<SUPPORT color.*>\n");
    EXPECT_GT(doc.m_iMXPerrors, errs);
    EXPECT_TRUE(sent_of(doc).empty());
}

TEST(MXPOpenAtomic, OptionTagSendsOptionsList)
{
    CMUSHclientDoc doc = make_pipe_doc();
    doc.m_pTransport = new RecordingTransport();
    mud(doc, "<OPTION>\n");
    EXPECT_TRUE(contains(sent_of(doc), "\x1B[1z<OPTIONS "));
}

TEST(MXPOpenAtomic, RecommendOptionUnknownNameRaisesError)
{
    CMUSHclientDoc doc = make_pipe_doc();
    doc.m_bMudCanChangeOptions = TRUE;
    long long errs = doc.m_iMXPerrors;
    mud(doc, "<RECOMMEND_OPTION nosuchoption=1>\n");
    EXPECT_GT(doc.m_iMXPerrors, errs);
}

TEST(MXPOpenAtomic, UserTagSendsCharacterNameWhenAutoconnectMxp)
{
    CMUSHclientDoc doc = make_pipe_doc();
    doc.m_pTransport = new RecordingTransport();
    doc.m_name = "bob";
    doc.m_connect_now = eConnectMXP;
    mud(doc, "<USER>\n");
    EXPECT_TRUE(contains(sent_of(doc), "bob"));
}

TEST(MXPOpenAtomic, UserTagWithoutMxpAutoconnectSendsNothing)
{
    CMUSHclientDoc doc = make_pipe_doc();
    doc.m_pTransport = new RecordingTransport();
    doc.m_name = "bob";
    doc.m_connect_now = eConnectNormal;
    mud(doc, "<USER>\n");
    EXPECT_FALSE(contains(sent_of(doc), "bob"));
}

TEST(MXPOpenAtomic, PasswordTagSendsPasswordWhenSafe)
{
    CMUSHclientDoc doc = make_pipe_doc();
    doc.m_pTransport = new RecordingTransport();
    doc.m_password = "sesame";
    doc.m_connect_now = eConnectMXP;
    doc.m_nTotalLinesSent = 0;
    mud(doc, "<PASSWORD>\n");
    EXPECT_TRUE(contains(sent_of(doc), "sesame"));
}

TEST(MXPOpenAtomic, PasswordTagRefusedAfterTenLinesSent)
{
    // security check: a late <PASSWORD> could be a spoof — refuse it
    CMUSHclientDoc doc = make_pipe_doc();
    doc.m_pTransport = new RecordingTransport();
    doc.m_password = "sesame";
    doc.m_connect_now = eConnectMXP;
    doc.m_nTotalLinesSent = 11;
    mud(doc, "<PASSWORD>\n");
    EXPECT_FALSE(contains(sent_of(doc), "sesame"));
}

TEST(MXPOpenAtomic, BrTagBreaksTheLine)
{
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
    EXPECT_FALSE(doc.m_bInParagraph);
    mud(doc, "<P>");
    EXPECT_TRUE(doc.m_bInParagraph);
}

TEST(MXPOpenAtomic, UnorderedListItemsGetBullets)
{
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
    mud(doc, "<MXP OFF>");
    EXPECT_FALSE(doc.m_bMXP);
}

TEST(MXPOpenAtomic, ResetTagClearsModesButKeepsMxpOn)
{
    CMUSHclientDoc doc = make_pipe_doc();
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
    CMUSHclientDoc doc = make_pipe_doc();
    mud(doc, "<VAR foo>hello</VAR>\n");
    CVariable* var = nullptr;
    ASSERT_TRUE(doc.m_VariableMap.Lookup("mxp_foo", var));
    ASSERT_TRUE(var != nullptr);
    EXPECT_STREQ("hello", (const char*)var->strContents);
}

TEST(MXPOpenAtomic, VarTagInvalidNameRaisesError)
{
    CMUSHclientDoc doc = make_pipe_doc();
    long long errs = doc.m_iMXPerrors;
    mud(doc, "<VAR 9bad>x</VAR>\n");
    EXPECT_GT(doc.m_iMXPerrors, errs);
}

TEST(MXPOpenAtomic, SecureTagRejectedInOpenMode)
{
    CMUSHclientDoc doc = make_pipe_doc();
    doc.m_iMXP_mode = eMXP_open;
    long long errs = doc.m_iMXPerrors;
    mud(doc, "<SEND 'go'>x</SEND>\n");
    EXPECT_GT(doc.m_iMXPerrors, errs);
    CStyle* s = pstyle(pline(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(0, s->iFlags & ACTIONTYPE);    // no action attached
}

MC_TEST_MAIN()
