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

// ═══ world.* alias API (methods_aliases.cpp) ══════════════════════════════

#include "flags.h"   // eEnabled / eReplace / eTemporary alias-flags

static std::string vs(const VARIANT& v)
{
    if (v.vt != VT_BSTR) return "<not-a-string>";
    return std::string((const char*)CString(v.bstrVal));
}

// AddAlias validation: every reject path answers its own error code, and
// eReplace converts the duplicate error into an overwrite.
TEST(AliasApi, AddAliasValidatesInput)
{
    CMUSHclientDoc doc = make_doc();

    EXPECT_EQ((long)eOK, doc.AddAlias("good", "north", "go north", eEnabled, ""));
    EXPECT_EQ((long)eOK, doc.IsAlias("good"));
    EXPECT_EQ((long)eAliasNotFound, doc.IsAlias("missing"));

    // duplicate label rejected unless eReplace
    EXPECT_EQ((long)eAliasAlreadyExists,
              doc.AddAlias("good", "south", "go south", eEnabled, ""));
    EXPECT_EQ((long)eOK,
              doc.AddAlias("good", "south", "go south", eEnabled | eReplace, ""));
    EXPECT_STREQ("south", vs(doc.GetAliasInfo("good", 1)).c_str());  // replaced

    // empty match text
    EXPECT_EQ((long)eAliasCannotBeEmpty, doc.AddAlias("empty", "", "x", 0, ""));
    // label must be a valid object name
    EXPECT_EQ((long)eInvalidObjectLabel, doc.AddAlias("1bad", "m", "x", 0, ""));
    // Windows parity: regcomp throws on a malformed pattern, so AddAlias
    // answers eBadRegularExpression and does NOT store the alias.
    EXPECT_EQ((long)eBadRegularExpression,
              doc.AddAlias("rx", "(unclosed", "x", eAliasRegularExpression, ""));
    EXPECT_EQ(VT_EMPTY, (int)doc.GetAliasInfo("rx", 1).vt);   // not created

    // a blank label is auto-assigned (alias still created)
    long before = 0;
    { VARIANT v = doc.GetInfo(221); before = v.lVal; }
    EXPECT_EQ((long)eOK, doc.AddAlias("", "autoname", "x", eEnabled, ""));
    { VARIANT v = doc.GetInfo(221); EXPECT_EQ(before + 1, v.lVal); }
}

// GetAlias hands back match/response/flags through VARIANT out-params.
TEST(AliasApi, GetAliasRoundTrip)
{
    CMUSHclientDoc doc = make_doc();
    ASSERT_EQ((long)eOK, doc.AddAlias("rt", "tell * *", "say %1",
                                      eEnabled | eKeepEvaluating, ""));

    VARIANT m, r, p, f, s;
    VariantInit(&m); VariantInit(&r); VariantInit(&p);
    VariantInit(&f); VariantInit(&s);
    EXPECT_EQ((long)eOK, doc.GetAlias("rt", &m, &r, &p, &f, &s));
    EXPECT_STREQ("tell * *", vs(m).c_str());
    EXPECT_STREQ("say %1",   vs(r).c_str());
    // Flags come back as a VT_I2 short...
    ASSERT_EQ(VT_I2, (int)f.vt);
    EXPECT_TRUE((f.iVal & eEnabled) != 0);
    // ...and keep_evaluating is NOT part of the reconstruction (a canonical
    // Windows asymmetry: AddAlias consumes eKeepEvaluating, GetAlias never
    // reports it) — scripts must read it via GetAliasInfo(19) instead.
    EXPECT_FALSE((f.iVal & eKeepEvaluating) != 0);
    EXPECT_TRUE(doc.GetAliasInfo("rt", 19).boolVal);

    EXPECT_EQ((long)eAliasNotFound, doc.GetAlias("nope", &m, &r, &p, &f, &s));
}

// GetAliasList enumerates the labels of every alias.
TEST(AliasApi, GetAliasListEnumeratesLabels)
{
    CMUSHclientDoc doc = make_doc();
    doc.AddAlias("one", "m1", "", eEnabled, "");
    doc.AddAlias("two", "m2", "", eEnabled, "");

    VARIANT v = doc.GetAliasList();
    ASSERT_EQ((int)(VT_ARRAY | VT_VARIANT), (int)v.vt);
    COleSafeArray sa;
    sa.Attach(v);
    EXPECT_EQ(2L, sa.GetOneDimSize());
    bool one = false, two = false;
    for (long i = 0; i < sa.GetOneDimSize(); i++) {
        VARIANT e; VariantInit(&e);
        sa.GetElement(&i, &e);
        if (vs(e) == "one") one = true;
        if (vs(e) == "two") two = true;
    }
    EXPECT_TRUE(one);
    EXPECT_TRUE(two);
}

// GetAliasInfo conventions: missing alias VT_EMPTY, bad code VT_NULL, and
// the representative arms answer live alias state.
TEST(AliasApi, GetAliasInfoArms)
{
    CMUSHclientDoc doc = make_doc();
    ASSERT_EQ((long)eOK, doc.AddAlias("ai", "match*", "send this",
                                      eEnabled | eTemporary, ""));

    EXPECT_EQ(VT_EMPTY, (int)doc.GetAliasInfo("missing", 1).vt);
    EXPECT_EQ(VT_NULL,  (int)doc.GetAliasInfo("ai", 999).vt);

    EXPECT_STREQ("match*",    vs(doc.GetAliasInfo("ai", 1)).c_str());
    EXPECT_STREQ("send this", vs(doc.GetAliasInfo("ai", 2)).c_str());
    EXPECT_TRUE(doc.GetAliasInfo("ai", 6).boolVal);        // enabled
    EXPECT_FALSE(doc.GetAliasInfo("ai", 7).boolVal);       // not regexp
    EXPECT_TRUE(doc.GetAliasInfo("ai", 14).boolVal);       // temporary
    EXPECT_EQ(0L, doc.GetAliasInfo("ai", 11).lVal);        // never matched
    EXPECT_EQ((long)DEFAULT_ALIAS_SEQUENCE,
              doc.GetAliasInfo("ai", 20).lVal);            // default sequence
    EXPECT_FALSE(doc.GetAliasInfo("ai", 29).boolVal);      // not one-shot
}

// Alias options: boolean options accept y/n, numeric options are validated,
// unknown names and unknown aliases each answer their own code.
TEST(AliasApi, AliasOptionRoundTripAndErrors)
{
    CMUSHclientDoc doc = make_doc();
    ASSERT_EQ((long)eOK, doc.AddAlias("oa", "m", "x", eEnabled, ""));

    EXPECT_EQ((long)eAliasNotFound, doc.SetAliasOption("zz", "enabled", "n"));
    EXPECT_EQ((long)eUnknownOption, doc.SetAliasOption("oa", "no_such", "1"));

    // boolean via y/n
    EXPECT_EQ((long)eOK, doc.SetAliasOption("oa", "enabled", "n"));
    EXPECT_EQ(0L, doc.GetAliasOption("oa", "enabled").lVal);
    EXPECT_EQ((long)eOK, doc.SetAliasOption("oa", "enabled", "y"));
    EXPECT_EQ(1L, doc.GetAliasOption("oa", "enabled").lVal);

    // numeric with validation
    EXPECT_EQ((long)eOK, doc.SetAliasOption("oa", "sequence", "50"));
    EXPECT_EQ(50L, doc.GetAliasInfo("oa", 20).lVal);
    EXPECT_EQ((long)eOptionOutOfRange,
              doc.SetAliasOption("oa", "sequence", "not-a-number"));

    // alpha option
    EXPECT_EQ((long)eOK, doc.SetAliasOption("oa", "group", "mygroup"));
    EXPECT_STREQ("mygroup", vs(doc.GetAliasOption("oa", "group")).c_str());

    // unknown option on read is indistinguishable from a missing alias: EMPTY
    EXPECT_EQ(VT_EMPTY, (int)doc.GetAliasOption("oa", "no_such").vt);
    EXPECT_EQ(VT_EMPTY, (int)doc.GetAliasOption("zz", "enabled").vt);
}

// Wildcards captured by the last match are retrievable by number — %0 is
// the whole matching text.
TEST(AliasApi, WildcardsCapturedOnMatch)
{
    CMUSHclientDoc doc = make_doc();
    ASSERT_EQ((long)eOK, doc.AddAlias("wc", "tell * *", "", eEnabled, ""));

    type_command(doc, "tell bob hello");

    EXPECT_STREQ("bob",   vs(doc.GetAliasWildcard("wc", "1")).c_str());
    EXPECT_STREQ("hello", vs(doc.GetAliasWildcard("wc", "2")).c_str());
    EXPECT_STREQ("tell bob hello", vs(doc.GetAliasWildcard("wc", "0")).c_str());
    EXPECT_STREQ("bob",   vs(doc.GetAliasInfo("wc", 101)).c_str());
    EXPECT_EQ(VT_EMPTY, (int)doc.GetAliasWildcard("missing", "1").vt);
}

// Group operations count only members of the named group; temporaries are
// swept by DeleteTemporaryAliases.
TEST(AliasApi, GroupsAndTemporarySweep)
{
    CMUSHclientDoc doc = make_doc();
    doc.AddAlias("g1", "m1", "", eEnabled, "");
    doc.AddAlias("g2", "m2", "", eEnabled, "");
    doc.AddAlias("solo", "m3", "", eEnabled, "");
    doc.SetAliasOption("g1", "group", "grp");
    doc.SetAliasOption("g2", "group", "grp");

    EXPECT_EQ(2L, doc.EnableAliasGroup("grp", FALSE));
    EXPECT_FALSE(doc.GetAliasInfo("g1", 6).boolVal);
    EXPECT_FALSE(doc.GetAliasInfo("g2", 6).boolVal);
    EXPECT_TRUE(doc.GetAliasInfo("solo", 6).boolVal);      // untouched

    EXPECT_EQ(0L, doc.EnableAliasGroup("nogroup", TRUE));  // no members

    EXPECT_EQ(2L, doc.DeleteAliasGroup("grp"));
    EXPECT_EQ((long)eAliasNotFound, doc.IsAlias("g1"));
    EXPECT_EQ((long)eOK, doc.IsAlias("solo"));

    doc.AddAlias("tmp", "mt", "", eEnabled | eTemporary, "");
    EXPECT_EQ(1L, doc.DeleteTemporaryAliases());
    EXPECT_EQ((long)eAliasNotFound, doc.IsAlias("tmp"));
    EXPECT_EQ((long)eOK, doc.IsAlias("solo"));

    EXPECT_EQ((long)eOK, doc.DeleteAlias("solo"));
    EXPECT_EQ((long)eAliasNotFound, doc.DeleteAlias("solo"));
}

// EnableAlias toggles one alias and reports a missing one.
TEST(AliasApi, EnableAliasToggleAndMissing)
{
    CMUSHclientDoc doc = make_doc();
    doc.AddAlias("ea", "m", "", eEnabled, "");
    EXPECT_EQ((long)eOK, doc.EnableAlias("ea", FALSE));
    EXPECT_FALSE(doc.GetAliasInfo("ea", 6).boolVal);
    EXPECT_EQ((long)eOK, doc.EnableAlias("ea", TRUE));
    EXPECT_TRUE(doc.GetAliasInfo("ea", 6).boolVal);
    EXPECT_EQ((long)eAliasNotFound, doc.EnableAlias("nope", TRUE));
}

// ═══ Round-4 coverage: speedwalk prefix, quit macro, alias script dispatch,
//     FixSendText expansion arms, trigger regexp-compile failure arms ═══════

#include "exceptions.h"

// EvaluateCommand speedwalk prefix: "#2n" expands and queues "north" twice.
TEST(EvaluateArms, SpeedwalkPrefixExpandsAndSends)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_enable_speed_walk = TRUE;
    doc.m_speed_walk_prefix = "#";
    type_command(doc, "#2n");
    std::string& sent = static_cast<FakeTransport*>(doc.m_pTransport)->sent;
    // two norths went out (queued with default 0 delay → drained immediately)
    size_t first = sent.find("north");
    ASSERT_TRUE(first != std::string::npos);
    EXPECT_TRUE(sent.find("north", first + 1) != std::string::npos);
}

// EvaluateCommand speedwalk prefix with a malformed walk takes the error arm
// (nothing is sent).
TEST(EvaluateArms, SpeedwalkPrefixErrorSendsNothing)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_enable_speed_walk = TRUE;
    doc.m_speed_walk_prefix = "#";
    type_command(doc, "#3(unclosed");
    EXPECT_TRUE(static_cast<FakeTransport*>(doc.m_pTransport)->sent.empty());
}

// EvaluateCommand speedwalk while disconnected: CheckConnected consumes the
// command.
TEST(EvaluateArms, SpeedwalkWhileDisconnectedConsumed)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_enable_speed_walk = TRUE;
    doc.m_speed_walk_prefix = "#";
    doc.m_iConnectPhase = eConnectNotConnected;
    type_command(doc, "#2n");
    EXPECT_TRUE(static_cast<FakeTransport*>(doc.m_pTransport)->sent.empty());
}

// Typing the QUIT macro sets m_bDisconnectOK (no auto-reconnect on quit).
TEST(EvaluateArms, QuitMacroSetsDisconnectOk)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_macros[MAC_QUIT] = "quit";
    EXPECT_TRUE(!doc.m_bDisconnectOK);
    type_command(doc, "QUIT");             // CompareNoCase arm
    EXPECT_TRUE(doc.m_bDisconnectOK);
}

// Alias echo: with display-my-input on and the alias asking to be echoed,
// the typed line is displayed as user input and logged.
TEST(EvaluateArms, AliasEchoDisplaysAndLogsTypedLine)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_display_my_input = TRUE;
    doc.m_log_input = TRUE;
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_alias_%d.log", (int)getpid());
    doc.OpenLog(path, FALSE);

    CAlias* a = add_alias(doc, "salute", "wave");
    a->bEchoAlias = TRUE;
    type_command(doc, "salute");
    doc.CloseLog();

    // the typed line landed in the output as user input
    bool sawTyped = false;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (std::string(l->text, l->len).find("salute") != std::string::npos &&
            (l->flags & USER_INPUT))
            sawTyped = true;
    }
    EXPECT_TRUE(sawTyped);

    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[512] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    unlink(path);
    EXPECT_TRUE(strstr(buf, "salute") != nullptr);   // command logged
}

// Trace: a matched alias without a label is traced by its match text.
TEST(EvaluateArms, TraceNotesMatchedAliasName)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bTrace = TRUE;
    add_alias(doc, "dance", "boogie");
    type_command(doc, "dance");
    bool sawTrace = false;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (std::string(l->text, l->len).find("Matched alias") != std::string::npos)
            sawTrace = true;
    }
    EXPECT_TRUE(sawTrace);
}

// Alias with bOmitFromCommandHistory sets the doc-wide omit flag.
TEST(EvaluateArms, AliasOmitFromCommandHistoryFlag)
{
    CMUSHclientDoc doc = make_doc();
    CAlias* a = add_alias(doc, "secret", "hush");
    a->bOmitFromCommandHistory = TRUE;
    doc.m_bOmitFromCommandHistory = false;
    type_command(doc, "secret");
    EXPECT_TRUE(doc.m_bOmitFromCommandHistory);
}

#ifdef MUSHCLIENT_LUA_SCRIPTING
#include "scripting/scripting.h"

// Alias script dispatch: the matched alias calls its Lua procedure with
// (name, line, wildcards) — the embedded-callback path.
TEST(EvaluateArms, AliasScriptReceivesNameLineAndWildcards)
{
    CMUSHclientDoc doc = make_doc();
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString(
        "function OnAlias(name, line, wildcards)\n"
        "  world.SetVariable('a_name', name)\n"
        "  world.SetVariable('a_line', line)\n"
        "  world.SetVariable('a_w1', wildcards[1])\n"
        "end\n"), "<test>"));
    doc.m_ScriptEngine = engine;

    CAlias* a = add_alias(doc, "get *", "");
    a->strLabel = "getter";
    a->strProcedure = "OnAlias";
    a->dispid = engine->GetDispid("OnAlias");
    ASSERT_TRUE(a->dispid != DISPID_UNKNOWN);

    type_command(doc, "get sword");

    CVariable* v = nullptr;
    ASSERT_TRUE(doc.GetVariableMap().Lookup("a_name", v));
    EXPECT_STREQ("getter", (LPCTSTR)v->strContents);
    ASSERT_TRUE(doc.GetVariableMap().Lookup("a_line", v));
    EXPECT_STREQ("get sword", (LPCTSTR)v->strContents);
    ASSERT_TRUE(doc.GetVariableMap().Lookup("a_w1", v));
    EXPECT_STREQ("sword", (LPCTSTR)v->strContents);
}

// Unlabelled alias script: the internal name (rev-map arm) is passed instead.
TEST(EvaluateArms, UnlabelledAliasScriptGetsInternalName)
{
    CMUSHclientDoc doc = make_doc();
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString(
        "function OnAlias2(name, line, wildcards)\n"
        "  world.SetVariable('a2_name', name)\n"
        "end\n"), "<test>"));
    doc.m_ScriptEngine = engine;

    CAlias* a = add_alias(doc, "jump", "");
    a->strLabel = "";                       // unlabelled → rev-map lookup
    a->strProcedure = "OnAlias2";
    a->dispid = engine->GetDispid("OnAlias2");

    type_command(doc, "jump");
    CVariable* v = nullptr;
    ASSERT_TRUE(doc.GetVariableMap().Lookup("a2_name", v));
    EXPECT_STREQ("jump", (LPCTSTR)v->strContents);   // rev-map key
}

// Trigger script dispatch: the matched trigger calls its Lua procedure with
// (name, line, wildcards) — the ExecuteTriggerScript Lua path.
TEST(EvaluateArms, TriggerScriptReceivesNameLineAndWildcards)
{
    CMUSHclientDoc doc = make_doc();
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString(
        "function OnTrig(name, line, wildcards)\n"
        "  world.SetVariable('t_name', name)\n"
        "  world.SetVariable('t_line', line)\n"
        "  world.SetVariable('t_w1', wildcards[1])\n"
        "end\n"), "<test>"));
    doc.m_ScriptEngine = engine;

    ASSERT_EQ((long)eOK, doc.AddTriggerEx("attacked", "* attacks you", "",
                                          1 /*eEnabled*/, -1, 0, "", "OnTrig",
                                          eSendToWorld, 100));

    doc.DisplayMsg("A goblin attacks you\r\n", 22, 0);

    CVariable* v = nullptr;
    ASSERT_TRUE(doc.GetVariableMap().Lookup("t_name", v));
    EXPECT_STREQ("attacked", (LPCTSTR)v->strContents);
    ASSERT_TRUE(doc.GetVariableMap().Lookup("t_line", v));
    EXPECT_STREQ("A goblin attacks you", (LPCTSTR)v->strContents);
    ASSERT_TRUE(doc.GetVariableMap().Lookup("t_w1", v));
    EXPECT_STREQ("A goblin", (LPCTSTR)v->strContents);
}
#endif // MUSHCLIENT_LUA_SCRIPTING

// ═══ FixSendText — the %/@ expansion engine ════════════════════════════════

TEST(FixSendTextArms, DoubledEscapesAndDisabledExpansion)
{
    CMUSHclientDoc doc = make_doc();
    doc.SetVariable("who", "Bob");

    // @@ → literal @; %% → literal %
    CString r = doc.FixSendText("mail @@home %%50", eSendToWorld, nullptr, "lua",
                                false, true, true, false, false, false, "nm");
    EXPECT_STREQ("mail @home %50", (LPCTSTR)r);

    // expansion disabled: @who and %1 pass through untouched
    r = doc.FixSendText("tell @who %1", eSendToWorld, nullptr, "lua",
                        false, false, false, false, false, false, "nm");
    EXPECT_STREQ("tell @who %1", (LPCTSTR)r);
    delete doc.m_pTransport; doc.m_pTransport = nullptr;
}

TEST(FixSendTextArms, VariableExpansionAndEscapingArms)
{
    CMUSHclientDoc doc = make_doc();
    doc.SetVariable("target", "a[b]*c");

    // regexp context (bFixRegexps + bIsRegexp): specials are escaped
    CString r = doc.FixSendText("@target", eSendToWorld, nullptr, "lua",
                                false, true, true, true, true, false, "nm");
    EXPECT_STREQ("a\\[b\\]\\*c", (LPCTSTR)r);

    // non-regexp escape context: asterisks are dropped
    r = doc.FixSendText("@target", eSendToWorld, nullptr, "lua",
                        false, true, true, true, false, false, "nm");
    EXPECT_STREQ("a[b]c", (LPCTSTR)r);

    // @! defeats the escaping entirely
    r = doc.FixSendText("@!target", eSendToWorld, nullptr, "lua",
                        false, true, true, true, true, false, "nm");
    EXPECT_STREQ("a[b]*c", (LPCTSTR)r);
    delete doc.m_pTransport; doc.m_pTransport = nullptr;
}

TEST(FixSendTextArms, VariableToScriptQuotingPerLanguage)
{
    CMUSHclientDoc doc = make_doc();
    doc.SetVariable("v", "say \"hi\\there\" $5");

    // lua/script destination: backslashes then quotes escaped
    CString r = doc.FixSendText("@v", eSendToScript, nullptr, "lua",
                                false, true, true, false, false, false, "nm");
    EXPECT_STREQ("say \\\"hi\\\\there\\\" $5", (LPCTSTR)r);

    // perlscript also escapes $
    r = doc.FixSendText("@v", eSendToScript, nullptr, "perlscript",
                        false, true, true, false, false, false, "nm");
    EXPECT_STREQ("say \\\"hi\\\\there\\\" \\$5", (LPCTSTR)r);

    // vbscript doubles quotes instead
    r = doc.FixSendText("@v", eSendToScript, nullptr, "vbscript",
                        false, true, true, false, false, false, "nm");
    EXPECT_STREQ("say \"\"hi\\there\"\" $5", (LPCTSTR)r);
    delete doc.m_pTransport; doc.m_pTransport = nullptr;
}

TEST(FixSendTextArms, VariableToHtmlLogFixed)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bLogHTML = TRUE;
    doc.SetVariable("h", "a<b>&c");
    CString r = doc.FixSendText("@h", eSendToLogFile, nullptr, "lua",
                                false, true, true, false, false, false, "nm");
    EXPECT_STREQ("a&lt;b&gt;&amp;c", (LPCTSTR)r);
    delete doc.m_pTransport; doc.m_pTransport = nullptr;
}

TEST(FixSendTextArms, ExceptionArmsForBadVariables)
{
    CMUSHclientDoc doc = make_doc();

    // "@" with no name throws when exceptions are enabled
    bool threw = false;
    try {
        doc.FixSendText("hello @ there", eSendToWorld, nullptr, "lua",
                        false, true, true, false, false, true, "nm");
    } catch (CException* e) { e->Delete(); threw = true; }
    EXPECT_TRUE(threw);

    // undefined variable throws too
    threw = false;
    try {
        doc.FixSendText("@nosuchvar", eSendToWorld, nullptr, "lua",
                        false, true, true, false, false, true, "nm");
    } catch (CException* e) { e->Delete(); threw = true; }
    EXPECT_TRUE(threw);

    // without exceptions both silently expand to nothing
    CString r = doc.FixSendText("@nosuchvar", eSendToWorld, nullptr, "lua",
                                false, true, true, false, false, false, "nm");
    EXPECT_STREQ("", (LPCTSTR)r);
    delete doc.m_pTransport; doc.m_pTransport = nullptr;
}

TEST(FixSendTextArms, WildcardDigitNamedAndUnknownPercent)
{
    CMUSHclientDoc doc = make_doc();
    t_regexp* re = regcomp("get (\\w+) from (\\w+)", 0);
    ASSERT_TRUE(regexec(re, "get sword from chest"));

    CString r = doc.FixSendText("take %1 out of %2", eSendToWorld, re, "lua",
                                false, true, true, false, false, false, "nm");
    EXPECT_STREQ("take sword out of chest", (LPCTSTR)r);

    // unknown %x sequence is copied through
    r = doc.FixSendText("100%x", eSendToWorld, re, "lua",
                        false, true, true, false, false, false, "nm");
    EXPECT_STREQ("100%x", (LPCTSTR)r);

    // %N expands the item name
    r = doc.FixSendText("by %N", eSendToWorld, re, "lua",
                        false, true, true, false, false, false, "myname");
    EXPECT_STREQ("by myname", (LPCTSTR)r);
    delete re;
    delete doc.m_pTransport; doc.m_pTransport = nullptr;
}

TEST(FixSendTextArms, NamedWildcardExpansion)
{
    CMUSHclientDoc doc = make_doc();
    t_regexp* re = regcomp("get (?<item>\\w+)", 0);
    ASSERT_TRUE(regexec(re, "get lantern"));
    CString r = doc.FixSendText("drop %<item> now", eSendToWorld, re, "lua",
                                false, true, true, false, false, false, "nm");
    EXPECT_STREQ("drop lantern now", (LPCTSTR)r);
    delete re;
    delete doc.m_pTransport; doc.m_pTransport = nullptr;
}

// ═══ Trigger regexp-compile failure arms in EvaluateTrigger ════════════════

// Variable-expanding triggers recompile their regexp on every evaluation:
// an invalid recompiled pattern takes the report-and-skip catch arm, while a
// valid one matches; a trigger with NO regexp at all is skipped outright.
TEST(EvaluateArms, VariableExpandingTriggerRecompileArms)
{
    CMUSHclientDoc doc = make_doc();

    // recompiles to "(unclosed" → regcomp throws → catch arm → skipped
    CTrigger* bad = new CTrigger();
    bad->trigger = "(unclosed@novar";
    bad->bRegexp = TRUE;
    bad->bExpandVariables = TRUE;
    bad->bEnabled = TRUE;
    bad->iSendTo = eSendToVariable;
    bad->strVariable = "badv";
    bad->contents = "no";
    bad->regexp = nullptr;
    bad->iSequence = 1;
    doc.GetTriggerMap().SetAt("bad", bad);
    doc.GetTriggerRevMap()[bad] = "bad";
    doc.GetTriggerArray().Add(bad);

    // recompiles to plain "hello" (undefined @novar drops silently) → fires
    CTrigger* good = new CTrigger();
    good->trigger = "hello@novar";
    good->bRegexp = FALSE;
    good->bExpandVariables = TRUE;
    good->bEnabled = TRUE;
    good->iSendTo = eSendToVariable;
    good->strVariable = "goodv";
    good->contents = "yes";
    good->regexp = nullptr;
    good->iSequence = 2;
    doc.GetTriggerMap().SetAt("good", good);
    doc.GetTriggerRevMap()[good] = "good";
    doc.GetTriggerArray().Add(good);

    // no regexp and no variable expansion → the no-regexp skip arm
    CTrigger* inert = new CTrigger();
    inert->trigger = "hello";
    inert->bRegexp = FALSE;
    inert->bExpandVariables = FALSE;
    inert->bEnabled = TRUE;
    inert->iSendTo = eSendToVariable;
    inert->strVariable = "inertv";
    inert->contents = "nope";
    inert->regexp = nullptr;
    inert->iSequence = 3;
    doc.GetTriggerMap().SetAt("inert", inert);
    doc.GetTriggerRevMap()[inert] = "inert";
    doc.GetTriggerArray().Add(inert);

    doc.DisplayMsg("hello\r\n", 7, 0);

    CVariable* v = nullptr;
    EXPECT_TRUE(!doc.GetVariableMap().Lookup("badv", v));
    EXPECT_TRUE(!doc.GetVariableMap().Lookup("inertv", v));
    ASSERT_TRUE(doc.GetVariableMap().Lookup("goodv", v));
    EXPECT_STREQ("yes", (LPCTSTR)v->strContents);
    // the bad trigger still has no compiled regexp
    EXPECT_TRUE(bad->regexp == nullptr);
}

// ── round 5 ───────────────────────────────────────────────────────────────

// A send-to-world alias while the world is disconnected aborts the alias
// sequence (the CheckConnected arm).
TEST(AliasR5, SendToWorldWhileDisconnectedAborts)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_iConnectPhase = eConnectNotConnected;
    CAlias* a = add_alias(doc, "n", "go north", eSendToWorld);
    bool omit = false;
    doc.EvaluateCommand(CString("n"), true, omit, false);
    EXPECT_EQ(1L, a->nMatched);   // matched, but the send was refused
}

MC_TEST_MAIN()
