// tests/test_triggers.cpp
// End-to-end tests for trigger matching via DisplayMsg → StartNewLine →
// ProcessPreviousLine → ProcessOneTriggerSequence.
//
// These are the first tests that exercise the full received-line pipeline:
// characters accumulate in m_pCurrentLine, \n finalises the line and fires
// ProcessPreviousLine(), which walks m_TriggerArray and calls SendTo/Note
// for any match.

#include "mc_test.h"
#include "platform/doc_stub.h"

// ── helpers ──────────────────────────────────────────────────────────────────

// Build a minimal doc ready for trigger testing.
static CMUSHclientDoc make_doc()
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_iFlags        = COLOUR_ANSI;
    doc.m_iForeColour   = WHITE;
    doc.m_iBackColour   = BLACK;
    doc.m_pCurrentLine  = new CLine();
    doc.AddStyle(COLOUR_ANSI, WHITE, BLACK, 0,
                 static_cast<CAction*>(nullptr));
    return doc;
}

// Add a simple plain-text trigger (no regex) to doc.
// Returns a pointer to the trigger (owned by doc's m_TriggerArray).
static CTrigger* add_trigger(CMUSHclientDoc& doc,
                              const char* match,
                              const char* send = "",
                              unsigned short sendTo = eSendToOutput)
{
    CTrigger* t  = new CTrigger();
    t->trigger   = CString(match);
    t->contents  = CString(send);
    t->bEnabled  = TRUE;
    t->bRegexp   = false;
    t->iSendTo   = sendTo;
    t->iSequence = DEFAULT_TRIGGER_SEQUENCE;

    // Register in both map (by label) and array (for iteration)
    t->strLabel = CString(match);   // use pattern as label for test simplicity
    doc.m_TriggerMap.SetAt(t->strLabel, t);
    doc.m_TriggerArray.Add(t);
    return t;
}

// Feed a line of plain text to the doc as if it arrived from the MUD.
// Appends \n so DisplayMsg finalises the line and calls ProcessPreviousLine.
static void feed_line(CMUSHclientDoc& doc, const char* text)
{
    std::string s(text);
    s += '\n';
    doc.DisplayMsg(s.c_str(), (int)s.size(), 0);
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST(TriggerTest, NonMatchingLineDoesNotFire)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "you have died");

    feed_line(doc, "Welcome to the MUD!");

    EXPECT_EQ(0L, t->nMatched);
}

TEST(TriggerTest, ExactMatchFires)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "you have died");

    feed_line(doc, "you have died");

    EXPECT_EQ(1L, t->nMatched);
}

TEST(TriggerTest, SubstringMatchFires)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "health");

    feed_line(doc, "Your health is low.");

    EXPECT_EQ(1L, t->nMatched);
}

TEST(TriggerTest, CaseInsensitiveMatch)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "YOU HAVE DIED");
    t->ignore_case = TRUE;

    feed_line(doc, "you have died");

    EXPECT_EQ(1L, t->nMatched);
}

TEST(TriggerTest, CaseSensitiveNoMatch)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "YOU HAVE DIED");
    t->ignore_case = FALSE;

    feed_line(doc, "you have died");

    EXPECT_EQ(0L, t->nMatched);
}

TEST(TriggerTest, RegexpWildcardMatch)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "^You have (\\d+) gold");
    t->bRegexp = true;

    feed_line(doc, "You have 42 gold");

    EXPECT_EQ(1L, t->nMatched);
    // Wildcard 1 should be "42"
    EXPECT_EQ(std::string("42"), t->wildcards[1]);
}

TEST(TriggerTest, RegexpNoMatchOnWrongLine)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "^You have (\\d+) gold");
    t->bRegexp = true;

    feed_line(doc, "He has some gold");

    EXPECT_EQ(0L, t->nMatched);
}

TEST(TriggerTest, FiresOncePerLine)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "ping");

    feed_line(doc, "ping pong ping");   // two occurrences but trigger fires once per line

    // Default bRepeat=false → fires once even if pattern appears multiple times
    EXPECT_EQ(1L, t->nMatched);
}

TEST(TriggerTest, MultipleLinesMultipleMatches)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "score");

    feed_line(doc, "Your score is 100.");
    feed_line(doc, "Total score: 100.");
    feed_line(doc, "Nothing here.");

    EXPECT_EQ(2L, t->nMatched);
}

TEST(TriggerTest, DisabledTriggerDoesNotFire)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "you have died");
    t->bEnabled = FALSE;

    feed_line(doc, "you have died");

    EXPECT_EQ(0L, t->nMatched);
}

TEST(TriggerTest, LineAccumulatedCorrectly)
{
    // Verify that m_LineList contains the line after processing.
    CMUSHclientDoc doc = make_doc();
    add_trigger(doc, "hello");

    feed_line(doc, "hello world");

    // After the \n, StartNewLine moved the line to m_LineList and started a new one.
    EXPECT_GT((long)doc.m_LineList.GetCount(), 0L);

    // The most recently completed line is the tail of m_LineList.
    CLine* pLine = doc.m_LineList.GetTail();
    ASSERT_NE(nullptr, pLine);
    CString text(pLine->text, pLine->len);
    EXPECT_STREQ("hello world", (const char*)text);
}

// ── Lua script dispatch ───────────────────────────────────────────────────
// These tests require a live Lua engine and are only compiled when Lua is
// present (MUSHCLIENT_LUA_SCRIPTING is set by tests/CMakeLists.txt).

#ifdef MUSHCLIENT_LUA_SCRIPTING
#include "scripting/scripting.h"

// Feed a complete Lua engine into doc: create engine, parse script, resolve.
static bool setup_lua(CMUSHclientDoc& doc, const char* lua_src)
{
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    if (engine->CreateScriptEngine()) { delete engine; return false; }
    if (engine->Parse(CString(lua_src), "<test>")) { delete engine; return false; }
    doc.m_ScriptEngine = engine;
    doc.ResolveAllEntryPoints();
    return true;
}

TEST(TriggerScript, LuaCallbackFiresOnMatch)
{
    // A trigger with script="OnPing" should call the Lua function when
    // a matching line arrives.  The function sets a variable we can check.
    CMUSHclientDoc doc = make_doc();

    // The Lua function stores its argument in a world variable
    const char* lua =
        "function OnPing(name, line, wildcards)\n"
        "  world.SetVariable('fired', line)\n"
        "end\n";
    ASSERT_TRUE(setup_lua(doc, lua));

    // Add a trigger with a script callback
    CTrigger* t = add_trigger(doc, "ping");
    t->strProcedure = CString("OnPing");
    doc.ResolveAllEntryPoints();   // re-resolve now that trigger exists

    feed_line(doc, "ping");

    EXPECT_EQ(1L, t->nMatched);

    // Check the variable was set by the Lua function
    CVariable* v = nullptr;
    EXPECT_TRUE(doc.GetVariableMap().Lookup("fired", v));
    ASSERT_NE(nullptr, v);
    EXPECT_STREQ("ping", (const char*)v->strContents);
}

TEST(TriggerScript, LuaCallbackReceivesWildcard)
{
    // A regexp trigger with a wildcard: the Lua function receives wildcards
    // as a table parameter.
    CMUSHclientDoc doc = make_doc();

    const char* lua =
        "function OnHP(name, line, wildcards)\n"
        "  world.SetVariable('hp', wildcards[1])\n"
        "end\n";
    ASSERT_TRUE(setup_lua(doc, lua));

    // Regexp trigger: match "HP: <number>"
    CTrigger* t = new CTrigger;
    t->trigger     = CString("^HP: (\\d+)");
    t->strLabel    = CString("hp_trigger");
    t->strProcedure= CString("OnHP");
    t->bEnabled    = TRUE;
    t->bRegexp     = TRUE;
    t->iSendTo     = eSendToWorld;
    t->iSequence   = DEFAULT_TRIGGER_SEQUENCE;
    t->regexp      = regcomp(t->trigger, 0);
    ASSERT_NE(nullptr, t->regexp);

    doc.GetTriggerMap().SetAt(t->strLabel, t);
    doc.GetTriggerArray().Add(t);
    doc.GetTriggerRevMap()[t] = "hp_trigger";
    doc.ResolveAllEntryPoints();

    feed_line(doc, "HP: 42");

    EXPECT_EQ(1L, t->nMatched);

    CVariable* v = nullptr;
    EXPECT_TRUE(doc.GetVariableMap().Lookup("hp", v));
    ASSERT_NE(nullptr, v);
    EXPECT_STREQ("42", (const char*)v->strContents);
}

TEST(TriggerScript, MissingLuaFunctionDoesNotCrash)
{
    // A trigger whose Lua function does not exist should fail gracefully —
    // no crash, and nMatched is still incremented (the trigger matched the
    // line; only the script call failed).
    CMUSHclientDoc doc = make_doc();

    const char* lua = "-- intentionally empty script\n";
    ASSERT_TRUE(setup_lua(doc, lua));

    CTrigger* t = add_trigger(doc, "hello");
    t->strProcedure = CString("NonExistentFunction");
    doc.ResolveAllEntryPoints();
    // After resolve, dispid should still be DISPID_UNKNOWN for a missing fn
    EXPECT_EQ(DISPID_UNKNOWN, t->dispid);

    feed_line(doc, "hello");
    EXPECT_EQ(1L, t->nMatched);  // matched — script failure doesn't suppress this
}

#endif // MUSHCLIENT_LUA_SCRIPTING

MC_TEST_MAIN()
