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
// B-3 atomic flip: real headers (portable stub deleted; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "pal/itransport.h"
#include "fake_transport.h"

// ── helpers ───────────────────────────────────────────────────────────────

using FakeTransport = RecordingTransport;

static void make_doc(CMUSHclientDoc& doc)
{
    doc.m_enable_aliases   = TRUE;
    doc.m_enable_triggers  = TRUE;
    doc.m_display_my_input = FALSE;   // don't echo — keeps output clean
    doc.m_iConnectPhase    = eConnectConnectedToMud;  // simulate live connection
    // the doc destructor tears the transport down (TeardownTransport)
    doc.m_pTransport = new FakeTransport();
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

// NoAliasPassesCommandThrough deleted (audit §3): its no-aliases-registered nuance
// is folded into NonMatchingCommandPassesThrough below as a second segment; the
// stronger test (which also asserts nMatched==0) subsumes the +1-line assertion.

TEST(AliasTest, MatchingAliasFiresAndSendsContents)
{
    // Alias "n" → "go north": typing "n" both increments the match count AND
    // sends the *substituted* contents (not the raw "n") to the world.  The
    // substituted-text check is what distinguishes a fired alias from a raw
    // passthrough (which would also send exactly one line).
    CMUSHclientDoc doc; make_doc(doc);
    CAlias* a = add_alias(doc, "n", "go north");

    long before = doc.m_nTotalLinesSent;
    type_command(doc, "n");
    EXPECT_EQ(1L, a->nMatched);
    EXPECT_EQ(before + 1, doc.m_nTotalLinesSent);
    EXPECT_STREQ("go north\r\n",
                 static_cast<FakeTransport*>(doc.m_pTransport)->sent.c_str());
}

TEST(AliasTest, NonMatchingCommandPassesThrough)
{
    CMUSHclientDoc doc; make_doc(doc);
    CAlias* a = add_alias(doc, "n", "go north");

    long before = doc.m_nTotalLinesSent;
    type_command(doc, "south");
    EXPECT_EQ(0L, a->nMatched);
    // "south" has no alias, so it's sent raw.
    EXPECT_EQ(before + 1, doc.m_nTotalLinesSent);

    // Folded from the deleted NoAliasPassesCommandThrough: with NO aliases
    // registered at all, a raw command still passes through (+1 line).
    CMUSHclientDoc doc2; make_doc(doc2);
    long before2 = doc2.m_nTotalLinesSent;
    type_command(doc2, "look");
    EXPECT_EQ(before2 + 1, doc2.m_nTotalLinesSent);
}

TEST(AliasTest, DisabledAliasDoesNotFire)
{
    CMUSHclientDoc doc; make_doc(doc);
    CAlias* a = add_alias(doc, "n", "go north");
    a->bEnabled = FALSE;

    type_command(doc, "n");
    EXPECT_EQ(0L, a->nMatched);
}

TEST(AliasTest, AliasesDisabledGlobally)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_enable_aliases = FALSE;
    CAlias* a = add_alias(doc, "n", "go north");

    type_command(doc, "n");
    EXPECT_EQ(0L, a->nMatched);
}

TEST(AliasTest, CaseInsensitiveAlias)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    long before = doc.m_nTotalLinesSent;
    type_command(doc, "");
    // EvaluateCommand returns early for empty input — nothing sent.
    EXPECT_EQ(before, doc.m_nTotalLinesSent);
}

TEST(AliasTest, SendToOutputDoesNotIncreaseSentCount)
{
    // An alias that sends to output (eSendToOutput) should NOT increment
    // m_nTotalLinesSent, since nothing goes to the MUD.
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
        CMUSHclientDoc doc; make_doc(doc);
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
        CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_enable_command_stack = TRUE;
    doc.m_strCommandStackCharacter = ";";

    doc.Execute("say a;;b");
    EXPECT_STREQ("say a;b\r\n", wire(doc)->sent.c_str());
}

// methods_commands.cpp: "do command stacking first, so that other things
// (like aliases) work within command-stack".
TEST(CommandTest, AliasesMatchWithinCommandStack)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_enable_command_stack = TRUE;
    doc.m_strCommandStackCharacter = ";";
    CAlias* a = add_alias(doc, "n", "go north");

    doc.Execute("look;n");                   // 'n' is the second segment
    EXPECT_EQ(1L, a->nMatched);              // alias fired on its segment
    EXPECT_STREQ("look\r\ngo north\r\n", wire(doc)->sent.c_str());
}

// A multi-line command is sent as one command PER LINE, independently of
// command stacking.  This is the path a pasted block takes, and the path a
// Shift+Return composition takes: the command bar hands Execute the whole
// thing in one go (wx_world_child.cpp OnInputEnter) and the split happens
// here, so nothing above the engine has to know about it.
//
// Pinned after driving the real client against a live MUD (2026-08-11):
// pasting "north/south/east" into the command bar and pressing Enter walked
// three rooms in order.  Stacking is deliberately left OFF, because these are
// different mechanisms and a test that enabled it could not tell which one
// did the splitting.
TEST(CommandTest, MultiLineCommandSendsOneCommandPerLine)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_FALSE(doc.m_enable_command_stack);   // the control: stacking is off

    doc.Execute("north\nsouth\neast");
    EXPECT_STREQ("north\r\nsouth\r\neast\r\n", wire(doc)->sent.c_str());
    EXPECT_EQ(3L, doc.m_nTotalLinesSent);
}

// ...and the same when the lines arrive CRLF-separated, which is what a
// Windows clipboard carries and what the multiline edit control holds there.
TEST(CommandTest, MultiLineCommandAcceptsCRLFSeparators)
{
    CMUSHclientDoc doc; make_doc(doc);

    doc.Execute("north\r\nsouth");
    EXPECT_STREQ("north\r\nsouth\r\n", wire(doc)->sent.c_str());
    EXPECT_EQ(2L, doc.m_nTotalLinesSent);
}

// methods_commands.cpp: "if list is empty, make sure we send at least one
// empty line" — Execute("") sends exactly one blank line.
TEST(CommandTest, EmptyCommandSendsOneBlankLine)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);

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
    doc.FindAllEntryPoints();

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

// vs(const VARIANT&) is shared with test_triggers/test_timers (audit §2/§4).
#include "variant_test_helpers.h"

// AddAlias validation: every reject path answers its own error code, and
// eReplace converts the duplicate error into an overwrite.
TEST(AliasApi, AddAliasValidatesInput)
{
    CMUSHclientDoc doc; make_doc(doc);

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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_enable_speed_walk = TRUE;
    doc.m_speed_walk_prefix = "#";
    type_command(doc, "#3(unclosed");
    EXPECT_TRUE(static_cast<FakeTransport*>(doc.m_pTransport)->sent.empty());
}

// EvaluateCommand speedwalk while disconnected: CheckConnected consumes the
// command.
TEST(EvaluateArms, SpeedwalkWhileDisconnectedConsumed)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_enable_speed_walk = TRUE;
    doc.m_speed_walk_prefix = "#";
    doc.m_iConnectPhase = eConnectNotConnected;
    type_command(doc, "#2n");
    EXPECT_TRUE(static_cast<FakeTransport*>(doc.m_pTransport)->sent.empty());
}

// Typing the QUIT macro sets m_bDisconnectOK (no auto-reconnect on quit).
TEST(EvaluateArms, QuitMacroSetsDisconnectOk)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_macros[MAC_QUIT] = "quit";
    // B-3: the real ctor (doc_construct.cpp:394) starts m_bDisconnectOK true;
    // establish the precondition so the QUIT-sets-it toggle is still observable.
    doc.m_bDisconnectOK = false;
    EXPECT_TRUE(!doc.m_bDisconnectOK);
    type_command(doc, "QUIT");             // CompareNoCase arm
    EXPECT_TRUE(doc.m_bDisconnectOK);
}

// Alias echo: with display-my-input on and the alias asking to be echoed,
// the typed line is displayed as user input and logged.
TEST(EvaluateArms, AliasEchoDisplaysAndLogsTypedLine)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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

// Trace + scripted alias: ExecuteLua's "Executing %s script" trace line
// reaches the output headless.  Until Phase 2 Stage 1 this was a live parity
// bug (docs/PHASE2-PLAN.md Stage 1 item 2): a stub-only Trace(const CString&){}
// no-op overload won resolution over the real Trace(LPCTSTR,...)
// (methods_tracing.cpp) for lua_scripting.cpp:564/795 + scriptengine.cpp:79,
// which pass a TFormat CString — silently dropping the line.  Deleting the
// overload lets CString->LPCTSTR bind the real body, as on Windows.
TEST(EvaluateArms, TraceNotesExecutingAliasScript)
{
    CMUSHclientDoc doc; make_doc(doc);
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString("function OnAlias() end\n"), "<test>"));
    doc.m_ScriptEngine = engine;
    doc.m_bTrace = TRUE;

    CAlias* a = add_alias(doc, "jig", "");
    a->strProcedure = "OnAlias";
    a->dispid = engine->GetDispid("OnAlias");
    ASSERT_TRUE(a->dispid != DISPID_UNKNOWN);

    type_command(doc, "jig");

    // evaluate.cpp ExecuteAliasScript passes strType "alias", so the real
    // Trace body emits: TRACE: Executing alias script "OnAlias"
    bool sawExec = false;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (std::string(l->text, l->len).find("Executing alias script") != std::string::npos)
            sawExec = true;
    }
    EXPECT_TRUE(sawExec);
}

// Unlabelled alias script: the internal name (rev-map arm) is passed instead.
TEST(EvaluateArms, UnlabelledAliasScriptGetsInternalName)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bLogHTML = TRUE;
    doc.SetVariable("h", "a<b>&c");
    CString r = doc.FixSendText("@h", eSendToLogFile, nullptr, "lua",
                                false, true, true, false, false, false, "nm");
    EXPECT_STREQ("a&lt;b&gt;&amp;c", (LPCTSTR)r);
    delete doc.m_pTransport; doc.m_pTransport = nullptr;
}

TEST(FixSendTextArms, ExceptionArmsForBadVariables)
{
    CMUSHclientDoc doc; make_doc(doc);

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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);

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
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iConnectPhase = eConnectNotConnected;
    CAlias* a = add_alias(doc, "n", "go north", eSendToWorld);
    bool omit = false;
    doc.EvaluateCommand(CString("n"), true, omit, false);
    EXPECT_EQ(1L, a->nMatched);   // matched, but the send was refused
}

// ── the Ctrl+click alias menu trio (doc_aliasmenu_shared.cpp, 2026-08-13) ───
// Hoisted from CMUSHView::AliasMenu / OnMXPMenu's ACTION_ALIAS arm, which the
// port never constructed: aliases flagged "menu" gained nothing and
// GetInfo(86) was always "".

TEST(AliasMenu, BuildsFilteredSortedListWithPluginTags)
{
    CMUSHclientDoc doc;
    const long eEnabledF = 1, eMenuF = eAliasMenu /*0x2000*/;
    // Two IN (inserted in reverse order to make the sort observable), two OUT.
    ASSERT_EQ(0L, doc.AddAlias("zebra_walk", "zw", "walk", eEnabledF | eMenuF, ""));
    ASSERT_EQ(0L, doc.AddAlias("apple_eat",  "ae", "eat",  eEnabledF | eMenuF, ""));
    ASSERT_EQ(0L, doc.AddAlias("dead_menu",  "dm", "x",    eMenuF,             ""));
    ASSERT_EQ(0L, doc.AddAlias("plain",      "pl", "y",    eEnabledF,          ""));

    // ...and one in an enabled plugin, which gains the ":pluginid" tag.
    CPlugin* p = new CPlugin(&doc);
    p->m_bSaveState = false;
    p->m_strID   = "bbbbbbbbbbbbbbbbbbbbbbbb";
    p->m_strName = "menu_fixture";
    p->m_bEnabled = TRUE;
    doc.m_PluginList.push_back(p);
    CAlias* pa = new CAlias();
    pa->name = "pm"; pa->strLabel = "middle_item";
    pa->bMenu = TRUE; pa->bEnabled = TRUE;
    p->m_AliasMap.SetAt("middle_item", pa);

    CString items[100];
    const int n = doc.BuildAliasMenuItems(items, 100);
    ASSERT_EQ(3, n);
    EXPECT_STREQ("apple_eat",  (LPCTSTR) items[0]);   // sorted by label...
    EXPECT_STREQ("middle_item:bbbbbbbbbbbbbbbbbbbbbbbb",
                 (LPCTSTR) items[1]);                 // ...ignoring the tag
    EXPECT_STREQ("zebra_walk", (LPCTSTR) items[2]);
    EXPECT_TRUE(doc.m_CurrentPlugin == NULL);         // context restored
}

TEST(AliasMenu, WordUnderMenuFollowsTheDelimiterSet)
{
    CMUSHclientDoc doc;
    doc.DisplayMsg("say foo.bar baz\n", 16, 0);
    CLine* pLine = doc.m_LineList.GetAt(doc.GetLinePosition(0));
    ASSERT_TRUE(pLine != NULL && pLine->len >= 11);

    // Byte col 5 sits inside "foo".  With '.' a delimiter the word is "foo";
    // without it, "foo.bar" — the delimiter-sensitivity is what makes this
    // non-degenerate (a scan ignoring App.m_strWordDelimitersDblClick could
    // not produce both answers).
    const CString saved = App.m_strWordDelimitersDblClick;
    App.m_strWordDelimitersDblClick = ".,";
    doc.SetWordUnderMenu(pLine, 5);
    EXPECT_STREQ("foo", (LPCTSTR) doc.m_strWordUnderMenu);
    App.m_strWordDelimitersDblClick = ",";
    doc.SetWordUnderMenu(pLine, 5);
    EXPECT_STREQ("foo.bar", (LPCTSTR) doc.m_strWordUnderMenu);
    App.m_strWordDelimitersDblClick = saved;

    // ...and GetInfo(86) is how scripts read it.
    VARIANT v = doc.GetInfo(86);
    EXPECT_EQ(std::string("foo.bar"), vs(v));
    VariantClear(&v);
}

TEST(AliasMenu, WordUnderMenuClampsAColumnPastTheEndOfTheLine)
{
    CMUSHclientDoc doc;
    doc.DisplayMsg("say foo.bar baz\n", 16, 0);
    CLine* pLine = doc.m_LineList.GetAt(doc.GetLinePosition(0));
    ASSERT_TRUE(pLine != NULL);

    // A completed line is resized to exactly len bytes with no terminator
    // (doc_shared.cpp), so text [len] is already one past the buffer.  The hit
    // test hands us that column whenever the click lands to the right of the
    // text, so the scan has to clamp before its first read -- the fixture is
    // only meaningful while len is the real byte count.
    ASSERT_EQ(15, pLine->len);

    const CString saved = App.m_strWordDelimitersDblClick;
    App.m_strWordDelimitersDblClick = ".,";

    // One past the end, and well past it: both name the last word instead of
    // scanning from a byte that is not ours.
    doc.SetWordUnderMenu(pLine, pLine->len);
    EXPECT_STREQ("baz", (LPCTSTR) doc.m_strWordUnderMenu);
    doc.SetWordUnderMenu(pLine, pLine->len + 50);
    EXPECT_STREQ("baz", (LPCTSTR) doc.m_strWordUnderMenu);

    // ...while an in-range column still answers for itself, so the clamp is
    // not simply pinning every column to the last word.
    doc.SetWordUnderMenu(pLine, 5);
    EXPECT_STREQ("foo", (LPCTSTR) doc.m_strWordUnderMenu);

    App.m_strWordDelimitersDblClick = saved;
}

TEST(AliasMenu, ExecuteRunsTheChosenAliasInItsOwnSpace)
{
    CMUSHclientDoc doc;
    const long eEnabledF = 1, eMenuF = eAliasMenu;

    // World alias: send-to-variable, with a %% substitution to prove the
    // replacement pass ran.
    ASSERT_EQ(0L, doc.AddAlias("do_it", "di", "done 100%%", eEnabledF | eMenuF, ""));
    CAlias* wa = nullptr;
    ASSERT_TRUE(doc.GetAliasMap().Lookup("do_it", wa));
    wa->iSendTo = eSendToVariable;
    wa->strVariable = "menu_out";

    doc.ExecuteAliasMenuItem(CString("do_it"));
    CVariable* v = nullptr;
    ASSERT_TRUE(doc.GetVariableMap().Lookup("menu_out", v));
    EXPECT_STREQ("done 100%", (LPCTSTR) v->strContents);
    EXPECT_EQ(1, (int) wa->nMatched);            // counted as a match

    // Plugin item: "label:id" executes in the PLUGIN's space — its variable
    // map, not the world's — and restores m_CurrentPlugin.
    CPlugin* p = new CPlugin(&doc);
    p->m_bSaveState = false;
    p->m_strID   = "cccccccccccccccccccccccc";
    p->m_strName = "exec_fixture";
    p->m_bEnabled = TRUE;
    doc.m_PluginList.push_back(p);
    CAlias* pa = new CAlias();
    pa->name = "px"; pa->strLabel = "plug_act";
    pa->bMenu = TRUE; pa->bEnabled = TRUE;
    pa->contents = "from plugin";
    pa->iSendTo = eSendToVariable;
    pa->strVariable = "plug_out";
    p->m_AliasMap.SetAt("plug_act", pa);

    doc.ExecuteAliasMenuItem(CString("plug_act:cccccccccccccccccccccccc"));
    CVariable* pv = nullptr;
    ASSERT_TRUE(p->m_VariableMap.Lookup("plug_out", pv));
    EXPECT_STREQ("from plugin", (LPCTSTR) pv->strContents);
    CVariable* worldV = nullptr;
    EXPECT_FALSE(doc.m_VariableMap.Lookup("plug_out", worldV));  // not the world's
    EXPECT_TRUE(doc.m_CurrentPlugin == NULL);
}

// BUG-1 (GUI drive run 2026-08-12/13, docs/UPSTREAM-BUGS.md #14): the
// ACTION_ALIAS %-substitution loop computed the %<digit> subscript and then
// never used it, so a menu alias's own send text always lost its %1 -- the
// server received "echo greeting " with the word silently dropped, even
// though the menu itself was correctly keyed off the clicked word (proving
// SetWordUnderMenu / word-matching worked; only the substitution was dead).
// This drives the whole chain the real gesture does: scan a real output line
// for the word under the click, then execute the menu item and check what
// actually reached the (fake) world.
TEST(AliasMenu, ExecuteSubstitutesWordUnderMenuIntoPercentOne)
{
    CMUSHclientDoc doc; make_doc(doc);
    const long eEnabledF = 1, eMenuF = eAliasMenu;

    doc.DisplayMsg("echo: samwise\n", 14, 0);
    CLine* pLine = doc.m_LineList.GetAt(doc.GetLinePosition(0));
    ASSERT_TRUE(pLine != NULL);
    doc.SetWordUnderMenu(pLine, 8);   // column inside "samwise"
    ASSERT_TRUE(doc.m_strWordUnderMenu == "samwise");

    ASSERT_EQ(0L, doc.AddAlias("greet_this", "greet *", "echo greeting %1",
                               eEnabledF | eMenuF, ""));

    doc.ExecuteAliasMenuItem(CString("greet_this"));
    EXPECT_STREQ("echo greeting samwise\r\n",
                 static_cast<FakeTransport*>(doc.m_pTransport)->sent.c_str());

    // %0 and %2-%9 have no analogue for a menu click (there is only ever one
    // captured word) and stay empty -- not a regression, just pinned so a
    // future "substitute the same word everywhere" change is a visible diff.
    ASSERT_EQ((long) eOK, doc.SetAliasOption("greet_this", "send",
                                             "echo %0 [%1] %2"));
    static_cast<FakeTransport*>(doc.m_pTransport)->sent.clear();
    doc.ExecuteAliasMenuItem(CString("greet_this"));
    EXPECT_STREQ("echo  [samwise] \r\n",
                 static_cast<FakeTransport*>(doc.m_pTransport)->sent.c_str());
}

MC_TEST_MAIN()

// The alias mirror of the trigger match recompile (methods_aliases.cpp
// :732-764) -- and the more dangerous of the two, because aliases have no
// safety net.  ProcessOneAliasSequence calls regexec(alias_item->regexp, ...)
// directly (evaluate.cpp:873) with no null check and NO lazy recompile (the
// trigger loop has one at evaluate.cpp:332-357; the alias loop has none), and
// regexec dereferences its argument on its first executable line
// (regexp.cpp:70, `if (prog->m_program == NULL)`).  So if a rejected pattern
// left the alias holding a NULL or freed regexp, the very next typed command
// would take the process down.
//
// It does not, because the setter compiles the NEW pattern first (:749 --
// regcomp allocates the new t_regexp at regexp.cpp:47 while the old one is
// still live) and only frees the old one afterwards (:761), and both reject
// arms return before either.  The pointer assertions pin that ordering; the
// type_command() after them is the crash canary.
TEST(AliasApi, SetAliasOptionMatchRejectionKeepsTheOldCompiledRegexp)
{
    CMUSHclientDoc doc; make_doc(doc);
    // a regexp alias, so "(unclosed" reaches regcomp un-escaped (a wildcard
    // alias would be ConvertToRegularExpression'd and the '(' escaped away)
    ASSERT_EQ((long)eOK,
              doc.AddAlias("am", "^north$", "go north",
                           eEnabled | eAliasRegularExpression, ""));
    CAlias* a = nullptr;
    ASSERT_TRUE(doc.GetAliasMap().Lookup("am", a));
    t_regexp* compiledAtAdd = a->regexp;
    ASSERT_NE(nullptr, compiledAtAdd);

    // empty match -> eAliasCannotBeEmpty (:734-735), before anything is touched
    EXPECT_EQ((long)eAliasCannotBeEmpty, doc.SetAliasOption("am", "match", ""));
    EXPECT_EQ(compiledAtAdd, a->regexp);
    // malformed pattern -> regcomp throws -> eBadRegularExpression (:755-759)
    EXPECT_EQ((long)eBadRegularExpression,
              doc.SetAliasOption("am", "match", "(unclosed"));
    EXPECT_EQ(compiledAtAdd, a->regexp);
    EXPECT_STREQ("^north$", vs(doc.GetAliasInfo("am", 1)).c_str());

    // the surviving program still matches -- and this line is the crash canary
    // for a NULL regexp (evaluate.cpp:873 -> regexp.cpp:70)
    type_command(doc, "north");
    EXPECT_EQ(1L, a->nMatched);

    // a good pattern DOES swap the program: regcomp (:749) runs before the
    // delete (:761), so the new allocation cannot reuse the freed address
    ASSERT_EQ((long)eOK, doc.SetAliasOption("am", "match", "^south$"));
    EXPECT_NE(compiledAtAdd, a->regexp);
    type_command(doc, "north");
    EXPECT_EQ(1L, a->nMatched);          // the old pattern is really gone
    type_command(doc, "south");
    EXPECT_EQ(2L, a->nMatched);

    // "go north" from the two alias hits, with the now-unaliased "north"
    // passing through raw in between (evaluate.cpp:146 -> DoSendMsg, which
    // appends ENDLINE at doc_shared.cpp:2471-2472)
    EXPECT_STREQ("go north\r\nnorth\r\ngo north\r\n",
                 static_cast<FakeTransport*>(doc.m_pTransport)->sent.c_str());
}

// The wildcard branch of the alias match recompile (methods_aliases.cpp:744):
// for a non-regexp alias the new text is put through
// ConvertToRegularExpression with its DEFAULT arguments -- bWholeLine=true,
// bMakeAsterisksWildcards=true (stdafx.h:437-439) -- exactly as AddAlias does
// at :165.
//
// The LINE already runs (test_lua_scripting.cpp:2146-2151 sets "match" on a
// non-regexp alias) but that test only round-trips the stored text and never
// types a command, so neither default is pinned.  Both matter, and neither is
// reached by SetAliasOptionMatchRecompilePaths, which uses a regexp alias and
// so takes the :742 branch.
TEST(AliasApi, SetAliasOptionMatchOnWildcardAliasReconvertsAndAnchors)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddAlias("wm", "nothing", "ok", eEnabled, ""));
    CAlias* a = nullptr;
    ASSERT_TRUE(doc.GetAliasMap().Lookup("wm", a));
    EXPECT_FALSE(doc.GetAliasInfo("wm", 7).boolVal);   // not a regexp alias

    // '*' has to survive as a WILDCARD -- handed to PCRE raw it is a
    // quantifier with nothing to repeat and the call would fail outright
    ASSERT_EQ((long)eOK, doc.SetAliasOption("wm", "match", "tell * *"));
    type_command(doc, "tell bob hello");
    EXPECT_EQ(1L, a->nMatched);
    EXPECT_STREQ("bob", vs(doc.GetAliasWildcard("wm", "1")).c_str());

    // ... and the conversion anchors ^...$, so a short alias does NOT fire on
    // a command that merely contains it -- this is the "cc" matching "accept"
    // regression, reached through the option setter instead of the loader.
    ASSERT_EQ((long)eOK, doc.SetAliasOption("wm", "match", "cc"));
    type_command(doc, "accept");
    EXPECT_EQ(1L, a->nMatched);          // unchanged
    type_command(doc, "cc");
    EXPECT_EQ(2L, a->nMatched);
}

// AliasOptionsTable flags ignore_case (methods_aliases.cpp:46) and regexp
// (:50) OPT_CANNOT_WRITE, so SetAliasOption bounces them at :726-727 --
// unconditionally, which is why a plain world script gets the plugin-shaped
// ePluginCannotSetOption with no plugin anywhere in sight.
//
// The return codes are already pinned (test_lua_scripting.cpp:2153-2154), but
// only in the 0 -> 1 direction and only for an alias whose flags were already
// 0.  This is the 1 -> 0 direction: an alias created case-insensitive stays
// case-insensitive, and its compiled program is not touched.
//
// Deliberately NOT asserted here: that the alias still matches "NORTH".  That
// would pass even if the flag check were deleted -- the alias setter has no
// recompile block (:691-694 goes straight to SetBaseOptionItem), so clearing
// bIgnoreCase would leave PCRE_CASELESS in the already-compiled program and
// the match would succeed anyway.  The read-back is the real discriminator.
//
// Worth pinning because "I set ignore_case and nothing happened" looks exactly
// like the dead-reader settings bugs this port has spent a lot of time on, and
// it is not one: upstream deliberately refuses the write, because case
// sensitivity is baked into the COMPILED program (PCRE_CASELESS at :170) and
// the alias setter -- unlike the trigger one -- has nothing to rebuild it with.
TEST(AliasApi, SetAliasOptionIgnoreCaseAndRegexpAreReadOnly)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK,
              doc.AddAlias("ro", "^north$", "go north",
                           eEnabled | eAliasRegularExpression | eIgnoreAliasCase,
                           ""));
    CAlias* a = nullptr;
    ASSERT_TRUE(doc.GetAliasMap().Lookup("ro", a));
    t_regexp* compiledAtAdd = a->regexp;
    ASSERT_NE(nullptr, compiledAtAdd);

    EXPECT_EQ((long)ePluginCannotSetOption,
              doc.SetAliasOption("ro", "ignore_case", "0"));
    EXPECT_EQ((long)ePluginCannotSetOption,
              doc.SetAliasOption("ro", "regexp", "0"));

    EXPECT_EQ(1L, doc.GetAliasOption("ro", "ignore_case").lVal);
    EXPECT_EQ(1L, doc.GetAliasOption("ro", "regexp").lVal);
    EXPECT_EQ(compiledAtAdd, a->regexp);
}

// SetAliasOption("script", ...) -- methods_aliases.cpp:765-779, the alias twin
// of the trigger path.  The option is already round-tripped
// (test_lua_scripting.cpp:2146-2151) but with no CScriptEngine on that doc, so
// :770 is false and the resolve block has never run.
//
// Three arms, in the order that makes :777 falsifiable:
//   * an alias with no script name calls nothing (ExecuteAliasScript is
//     entered for every matched alias at evaluate.cpp:176, but
//     CheckScriptingAvailable returns "OK" for an empty name at doc.cpp:555-556
//     and the dispid gate at evaluate.cpp:1029 then declines);
//   * setting a RESOLVABLE name must write the dispid at :777 -- this is the
//     only arm where that write is observable, because we start from
//     DISPID_UNKNOWN and CheckScriptingAvailable (doc.cpp:570-580) refuses to
//     call on it.  Once the dispid is 1 it stays 1 (GetLuaDispid,
//     lua_scripting.cpp:355-359, answers 1 for every resolvable function), so
//     the later OnAlpha->OnBeta swap is evidence about the NAME, not the
//     dispid (ExecuteLua resolves by name at lua_scripting.cpp:656);
//   * an unresolvable name returns eScriptNameNotLocated at :776, BEFORE
//     SetBaseAlphaOptionItem (:784), so the old name and old dispid survive
//     and the alias keeps calling what it called.
//
// The fourth arm -- clearing the name with "" -- is deliberately NOT asserted:
// it leaves dispid stale and ExecuteAliasScript gates on the DISPID, not the
// name (evaluate.cpp:1029), while CheckScriptingAvailable waves an empty name
// through (doc.cpp:555-556).  So the alias goes on trying to call something
// after its script was cleared.  See the note in the gap report; do not pin
// that behaviour until it is decided.
//
// The handlers append rather than assign so "kept running the old one" is
// visible as a character in the log rather than an indistinguishable rewrite.
TEST(AliasApi, SetAliasOptionScriptResolvesAndSwapsTheCallback)
{
    CMUSHclientDoc doc; make_doc(doc);
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString(
        "function OnAlpha()\n"
        "  world.SetVariable('log', (world.GetVariable('log') or '') .. 'A')\n"
        "end\n"
        "function OnBeta()\n"
        "  world.SetVariable('log', (world.GetVariable('log') or '') .. 'B')\n"
        "end\n"), "<test>"));
    doc.m_ScriptEngine = engine;

    // created with NO script: dispid starts unresolved, which is what makes
    // the :777 write below falsifiable
    ASSERT_EQ((long)eOK, doc.AddAlias("as", "ping", "", eEnabled, ""));
    CAlias* a = nullptr;
    ASSERT_TRUE(doc.GetAliasMap().Lookup("as", a));
    EXPECT_EQ((long)DISPID_UNKNOWN, (long)a->dispid);

    CVariable* v = nullptr;
    type_command(doc, "ping");
    EXPECT_EQ(1L, a->nMatched);
    EXPECT_FALSE(doc.GetVariableMap().Lookup("log", v));   // nothing called

    // arm 1: a resolvable name -- :777 writes the dispid, so the call happens.
    // Drop :777 and CheckScriptingAvailable bails on DISPID_UNKNOWN: no 'A'.
    EXPECT_EQ((long)eOK, doc.SetAliasOption("as", "script", "OnAlpha"));
    EXPECT_STREQ("OnAlpha", vs(doc.GetAliasOption("as", "script")).c_str());
    EXPECT_NE((long)DISPID_UNKNOWN, (long)a->dispid);
    type_command(doc, "ping");
    ASSERT_TRUE(doc.GetVariableMap().Lookup("log", v));
    EXPECT_STREQ("A", (const char*)v->strContents);

    // arm 2: unresolvable name -- rejected, and nothing was written
    EXPECT_EQ((long)eScriptNameNotLocated,
              doc.SetAliasOption("as", "script", "NoSuchFunction"));
    EXPECT_STREQ("OnAlpha", vs(doc.GetAliasOption("as", "script")).c_str());
    type_command(doc, "ping");
    ASSERT_TRUE(doc.GetVariableMap().Lookup("log", v));
    EXPECT_STREQ("AA", (const char*)v->strContents);   // still OnAlpha

    // arm 3: a different resolvable name -- the stored NAME re-points the call
    EXPECT_EQ((long)eOK, doc.SetAliasOption("as", "script", "OnBeta"));
    EXPECT_STREQ("OnBeta", vs(doc.GetAliasOption("as", "script")).c_str());
    type_command(doc, "ping");
    ASSERT_TRUE(doc.GetVariableMap().Lookup("log", v));
    EXPECT_STREQ("AAB", (const char*)v->strContents);
    EXPECT_EQ(4L, a->nMatched);
}
