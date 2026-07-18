// tests/test_aliases.cpp
// Tests for alias evaluation: EvaluateCommand → ProcessOneAliasSequence.
//
// Aliases intercept typed commands before they go to the MUD and can
// substitute, suppress, or expand them.  m_nTotalLinesSent is the observable:
// DoSendMsg increments it for every line actually written to the socket.
// The real (single-sourced) DoSendMsg gates on ConnectionOpen(), i.e.
// m_pTransport != nullptr — so each test doc gets a FakeTransport that just
// swallows the bytes (same shape as tests/test_pal_transport.cpp).

#include "mc_test.h"
#include "platform/doc_stub.h"
#include "pal/itransport.h"

// ── helpers ───────────────────────────────────────────────────────────────

namespace {
struct FakeTransport : pal::ITransport {
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

static CMUSHclientDoc make_doc()
{
    CMUSHclientDoc doc;
    doc.m_enable_aliases   = TRUE;
    doc.m_enable_triggers  = TRUE;
    doc.m_display_my_input = FALSE;   // don't echo — keeps output clean
    doc.m_iConnectPhase    = eConnectConnectedToMud;  // simulate live connection
    // the doc destructor tears the transport down (TeardownTransport)
    doc.m_pTransport = new FakeTransport();
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

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────

// evaluate.cpp: "now that we have run all scripts etc., delete one-shot
// aliases" — a one-shot alias fires exactly once and is then removed; the
// same command afterwards passes through unmatched.
TEST(AliasTest, OneShotAliasDeletedAfterFirstMatch)
{
    CMUSHclientDoc doc = make_doc();
    CAlias* a = add_alias(doc, "once", "hi there");
    a->bOneShot = TRUE;
    a->strInternalName = CString("once");   // the key one-shot deletion uses

    long before = doc.m_nTotalLinesSent;
    type_command(doc, "once");
    EXPECT_EQ(before + 1, doc.m_nTotalLinesSent);   // alias contents sent

    // the alias is gone from the map (do NOT touch `a` — it was freed)
    CAlias* gone = nullptr;
    EXPECT_FALSE(doc.GetAliasMap().Lookup("once", gone));

    before = doc.m_nTotalLinesSent;
    type_command(doc, "once");                      // now unmatched: raw send
    EXPECT_EQ(before + 1, doc.m_nTotalLinesSent);
}

// evaluate.cpp EvaluateCommand: "get rid of any carriage returns" — CRs are
// stripped from the typed command before alias matching.
TEST(AliasTest, CarriageReturnsStrippedBeforeAliasMatch)
{
    CMUSHclientDoc doc = make_doc();
    CAlias* a = add_alias(doc, "north", "go north");

    type_command(doc, "north\r");
    EXPECT_EQ(1L, a->nMatched);       // "\r" did not defeat the match
}

// evaluate.cpp FixSendText: "If bThrowExceptions is set certain conditions
// (such as variable not existing) cause an exception to be thrown" — an
// alias send referencing an undefined @variable aborts the send: the
// exception is caught, the command consumed, nothing written to the MUD.
TEST(AliasTest, UndefinedVariableInAliasSendAbortsSend)
{
    CMUSHclientDoc doc = make_doc();
    CAlias* a = add_alias(doc, "n", "kill @undefined_variable");
    a->bExpandVariables = TRUE;

    long before = doc.m_nTotalLinesSent;
    type_command(doc, "n");
    EXPECT_EQ(1L, a->nMatched);                    // it did match...
    EXPECT_EQ(before, doc.m_nTotalLinesSent);      // ...but nothing was sent
    EXPECT_TRUE(static_cast<FakeTransport*>(doc.m_pTransport)->sent.empty());
}

// evaluate.cpp: "Do only negative sequence number plugins at this point.
// Suggested by Fiendish. Added in version 4.97." — a negative-sequence
// plugin's aliases are evaluated before the world's, so its response is
// sent first (matching aliases in both scopes each fire; order is the
// observable).
static CAlias* add_plugin_alias(CPlugin* p, const char* match, const char* send)
{
    CAlias* pa    = new CAlias();
    pa->name      = CString(match);
    pa->contents  = CString(send);
    pa->bEnabled  = TRUE;
    pa->iSendTo   = eSendToWorld;
    pa->iSequence = DEFAULT_ALIAS_SEQUENCE;
    pa->strLabel  = CString(match);
    pa->strInternalName = CString(match);
    pa->regexp    = regcomp(ConvertToRegularExpression(pa->name, true, true), 0);
    p->m_AliasMap.SetAt(pa->strLabel, pa);
    p->m_AliasArray.Add(pa);
    return pa;
}

TEST(AliasTest, NegativeSequencePluginAliasEvaluatedBeforeWorld)
{
    {   // negative sequence: plugin alias evaluated (and sent) first
        CMUSHclientDoc doc = make_doc();
        CAlias* wa = add_alias(doc, "n", "world north");
        CPlugin* p = new CPlugin(&doc);
        p->m_bSaveState = false;
        p->m_iSequence  = -100;
        CAlias* pa = add_plugin_alias(p, "n", "plugin north");
        doc.m_PluginList.push_back(p);

        type_command(doc, "n");
        EXPECT_EQ(1L, pa->nMatched);
        EXPECT_EQ(1L, wa->nMatched);
        EXPECT_STREQ("plugin north\r\nworld north\r\n",
                     static_cast<FakeTransport*>(doc.m_pTransport)->sent.c_str());
    }
    {   // positive sequence: the world's alias runs (and sends) first
        CMUSHclientDoc doc = make_doc();
        add_alias(doc, "n", "world north");
        CPlugin* p = new CPlugin(&doc);
        p->m_bSaveState = false;
        p->m_iSequence  = 100;
        add_plugin_alias(p, "n", "plugin north");
        doc.m_PluginList.push_back(p);

        type_command(doc, "n");
        EXPECT_STREQ("world north\r\nplugin north\r\n",
                     static_cast<FakeTransport*>(doc.m_pTransport)->sent.c_str());
    }
}

// ── Command stacking / execution depth (methods_commands.cpp) ─────────────

static FakeTransport* wire(CMUSHclientDoc& doc)
{
    return static_cast<FakeTransport*>(doc.m_pTransport);
}

// methods_commands.cpp: "new in version 3.74 - command stack character at
// start of line disables command stacking (the leading stack character
// itself is deleted)".
TEST(CommandTest, LeadingStackCharDisablesStacking)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_enable_command_stack = TRUE;
    doc.m_strCommandStackCharacter = ";";

    doc.Execute(";a;b");                     // leading ';' disables stacking
    EXPECT_STREQ("a;b\r\n", wire(doc)->sent.c_str());

    wire(doc)->sent.clear();
    doc.Execute("a;b");                      // normal stacking: two lines
    EXPECT_STREQ("a\r\nb\r\n", wire(doc)->sent.c_str());
}

// methods_commands.cpp: "convert two command stacks in a row to 0x01 ...
// replace any 0x01 with one command stack character" — ';;' escapes to a
// literal ';'.
TEST(CommandTest, DoubledStackCharSendsLiteralStackChar)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_enable_command_stack = TRUE;
    doc.m_strCommandStackCharacter = ";";

    doc.Execute("say a;;b");
    EXPECT_STREQ("say a;b\r\n", wire(doc)->sent.c_str());
}

// methods_commands.cpp: "do command stacking first, so that other things
// (like aliases) work within command-stack".
TEST(CommandTest, AliasesMatchWithinCommandStack)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_enable_command_stack = TRUE;
    doc.m_strCommandStackCharacter = ";";
    CAlias* a = add_alias(doc, "n", "go north");

    doc.Execute("look;n");                   // 'n' is the second segment
    EXPECT_EQ(1L, a->nMatched);              // alias fired on its segment
    EXPECT_STREQ("look\r\ngo north\r\n", wire(doc)->sent.c_str());
}

// methods_commands.cpp: "if list is empty, make sure we send at least one
// empty line" — Execute("") sends exactly one blank line.
TEST(CommandTest, EmptyCommandSendsOneBlankLine)
{
    CMUSHclientDoc doc = make_doc();
    doc.Execute("");
    EXPECT_STREQ("\r\n", wire(doc)->sent.c_str());
    EXPECT_EQ(1L, doc.m_nTotalLinesSent);
}

// methods_commands.cpp: "stop an alias from recalling itself indefinitely:
// if (++m_iExecutionDepth > MAX_EXECUTION_DEPTH) return
// eCommandsNestedTooDeeply" — a self-recursing send-to-Execute alias
// terminates instead of looping forever.
TEST(CommandTest, ExecutionDepthCapStopsRecursiveAliases)
{
    CMUSHclientDoc doc = make_doc();
    CAlias* a = add_alias(doc, "loop", "loop", eSendToExecute);

    doc.Execute("loop");                     // must terminate, not recurse forever

    EXPECT_GT(a->nMatched, 0L);                        // it did fire...
    EXPECT_LE(a->nMatched, (long)MAX_EXECUTION_DEPTH); // ...boundedly
    EXPECT_EQ(0, doc.m_iExecutionDepth);               // depth fully unwound
}

#ifdef MUSHCLIENT_LUA_SCRIPTING
#include "scripting/scripting.h"

// methods_aliases.cpp DeleteAlias: "can't if executing a script (returns
// eItemInUse)" — an alias deleting itself from its own script survives.
TEST(AliasScript, DeleteAliasFromOwnScriptReturnsItemInUse)
{
    CMUSHclientDoc doc = make_doc();

    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString(
        "function OnAlias(name, line, wildcards)\n"
        "  world.SetVariable('rc', tostring(world.DeleteAlias('myalias')))\n"
        "end\n"), "<test>"));
    doc.m_ScriptEngine = engine;

    // an alias labelled "myalias" matching the command "n"
    CAlias* a = add_alias(doc, "n", "");
    doc.GetAliasMap().RemoveKey("n");
    a->strLabel        = CString("myalias");
    a->strInternalName = CString("myalias");
    a->strProcedure    = CString("OnAlias");
    doc.GetAliasMap().SetAt("myalias", a);
    doc.ResolveAllEntryPoints();

    type_command(doc, "n");

    CVariable* v = nullptr;
    ASSERT_TRUE(doc.m_VariableMap.Lookup("rc", v));
    ASSERT_NE(nullptr, v);
    EXPECT_STREQ((const char*)CFormat("%d", (int)eItemInUse),
                 (const char*)v->strContents);
    // no use-after-free: the alias is still in the map
    CAlias* still = nullptr;
    EXPECT_TRUE(doc.GetAliasMap().Lookup("myalias", still));
    EXPECT_TRUE(still == a);
}
#endif // MUSHCLIENT_LUA_SCRIPTING

MC_TEST_MAIN()
