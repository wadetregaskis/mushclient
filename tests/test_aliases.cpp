// tests/test_aliases.cpp
// Tests for alias evaluation: EvaluateCommand → ProcessOneAliasSequence.
//
// Aliases intercept typed commands before they go to the MUD and can
// substitute, suppress, or expand them.  m_nTotalLinesSent is the observable:
// SendMsg increments it for every line actually written to the socket.
// (SendPacket is a no-op when m_pTransport == nullptr, so no transport needed.)

#include "mc_test.h"
#include "platform/doc_stub.h"

// ── helpers ───────────────────────────────────────────────────────────────

static CMUSHclientDoc make_doc()
{
    CMUSHclientDoc doc;
    doc.m_enable_aliases   = TRUE;
    doc.m_enable_triggers  = TRUE;
    doc.m_display_my_input = FALSE;   // don't echo — keeps output clean
    doc.m_iConnectPhase    = eConnectConnectedToMud;  // simulate live connection
    return doc;
}

// Add a plain-text alias to doc.
static CAlias* add_alias(CMUSHclientDoc& doc,
                          const char* match,
                          const char* send  = "",
                          unsigned short sendTo = eSendToWorld)
{
    CAlias* a     = new CAlias();
    a->name       = CString(match);
    a->contents   = CString(send);
    a->bEnabled   = TRUE;
    a->bRegexp    = false;
    a->iSendTo    = sendTo;
    a->iSequence  = DEFAULT_ALIAS_SEQUENCE;
    a->strLabel   = CString(match);   // use pattern as label

    // Compile the regexp (EvaluateCommand needs it for matching)
    CString strRegexp = ConvertToRegularExpression(a->name, true, true);
    a->regexp = regcomp(strRegexp, 0);

    doc.GetAliasMap().SetAt(a->strLabel, a);
    doc.GetAliasArray().Add(a);
    doc.GetAliasRevMap()[a] = std::string(match);
    return a;
}

// Simulate the user typing a command.
static void type_command(CMUSHclientDoc& doc, const char* cmd)
{
    bool bOmit = false;
    doc.EvaluateCommand(CString(cmd), /*bCountThem=*/true, bOmit, /*bTest=*/false);
}

// ── Tests ─────────────────────────────────────────────────────────────────

TEST(AliasTest, NoAliasPassesCommandThrough)
{
    // With no aliases, the raw command is sent to the MUD.
    CMUSHclientDoc doc = make_doc();
    long before = doc.m_nTotalLinesSent;
    type_command(doc, "look");
    EXPECT_EQ(before + 1, doc.m_nTotalLinesSent);
}

TEST(AliasTest, MatchingAliasIncreasesMatchCount)
{
    CMUSHclientDoc doc = make_doc();
    CAlias* a = add_alias(doc, "n", "go north");

    type_command(doc, "n");
    EXPECT_EQ(1L, a->nMatched);
}

TEST(AliasTest, MatchingAliasSendsContents)
{
    // Alias "n" → "go north": typing "n" should result in "go north" being sent.
    CMUSHclientDoc doc = make_doc();
    add_alias(doc, "n", "go north");

    long before = doc.m_nTotalLinesSent;
    type_command(doc, "n");
    // The alias fires and sends its contents — one line out.
    EXPECT_EQ(before + 1, doc.m_nTotalLinesSent);
}

TEST(AliasTest, NonMatchingCommandPassesThrough)
{
    CMUSHclientDoc doc = make_doc();
    CAlias* a = add_alias(doc, "n", "go north");

    long before = doc.m_nTotalLinesSent;
    type_command(doc, "south");
    EXPECT_EQ(0L, a->nMatched);
    // "south" has no alias, so it's sent raw.
    EXPECT_EQ(before + 1, doc.m_nTotalLinesSent);
}

TEST(AliasTest, DisabledAliasDoesNotFire)
{
    CMUSHclientDoc doc = make_doc();
    CAlias* a = add_alias(doc, "n", "go north");
    a->bEnabled = FALSE;

    type_command(doc, "n");
    EXPECT_EQ(0L, a->nMatched);
}

TEST(AliasTest, AliasesDisabledGlobally)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_enable_aliases = FALSE;
    CAlias* a = add_alias(doc, "n", "go north");

    type_command(doc, "n");
    EXPECT_EQ(0L, a->nMatched);
}

TEST(AliasTest, CaseInsensitiveAlias)
{
    CMUSHclientDoc doc = make_doc();
    CAlias* a = add_alias(doc, "NORTH");
    a->bIgnoreCase = TRUE;
    // Recompile with CASELESS flag
    CString strRegexp = ConvertToRegularExpression(a->name, true, true);
    delete a->regexp;
    a->regexp = regcomp(strRegexp, PCRE_CASELESS);

    type_command(doc, "north");
    EXPECT_EQ(1L, a->nMatched);
}

TEST(AliasTest, RegexpAliasWildcard)
{
    // Alias matching "^say (.+)" with regexp=true and send "%1"
    CMUSHclientDoc doc = make_doc();
    CAlias* a = new CAlias();
    a->name      = CString("^say (.+)$");
    a->contents  = CString("%1");
    a->bEnabled  = TRUE;
    a->bRegexp   = TRUE;
    a->iSendTo   = eSendToWorld;
    a->iSequence = DEFAULT_ALIAS_SEQUENCE;
    a->strLabel  = CString("sayalias");
    a->regexp    = regcomp(a->name, 0);

    doc.GetAliasMap().SetAt(a->strLabel, a);
    doc.GetAliasArray().Add(a);
    doc.GetAliasRevMap()[a] = "sayalias";

    type_command(doc, "say hello world");
    EXPECT_EQ(1L, a->nMatched);
}

TEST(AliasTest, MultipleAliasesFirstMatchWins)
{
    // By default bKeepEvaluating=FALSE, so only the first match fires.
    CMUSHclientDoc doc = make_doc();
    CAlias* a1 = add_alias(doc, "n", "go north");
    CAlias* a2 = add_alias(doc, "n", "go south");  // same pattern, later sequence

    a1->iSequence = 100;
    a2->iSequence = 200;

    type_command(doc, "n");
    EXPECT_EQ(1L, a1->nMatched);
    EXPECT_EQ(0L, a2->nMatched);
}

TEST(AliasTest, EmptyCommandIgnored)
{
    CMUSHclientDoc doc = make_doc();
    long before = doc.m_nTotalLinesSent;
    type_command(doc, "");
    // EvaluateCommand returns early for empty input — nothing sent.
    EXPECT_EQ(before, doc.m_nTotalLinesSent);
}

TEST(AliasTest, SendToOutputDoesNotIncreaseSentCount)
{
    // An alias that sends to output (eSendToOutput) should NOT increment
    // m_nTotalLinesSent, since nothing goes to the MUD.
    CMUSHclientDoc doc = make_doc();
    add_alias(doc, "status", "HP: 100", eSendToOutput);

    long before = doc.m_nTotalLinesSent;
    type_command(doc, "status");
    EXPECT_EQ(before, doc.m_nTotalLinesSent);
}

MC_TEST_MAIN()
