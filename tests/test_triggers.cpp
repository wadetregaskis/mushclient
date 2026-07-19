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

TEST(TriggerTest, PlainTriggerDoesNotMatchSubstring)
{
    // Windows parity: a non-regexp (wildcard) trigger is anchored to the WHOLE
    // line (^...$). "health" matches ONLY a line that is exactly "health", not
    // a line that merely contains it. (The port previously compiled plain
    // triggers unanchored, so this wrongly fired — the same bug made the
    // automap alias "cc" fire on "accept".)
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "health");

    feed_line(doc, "Your health is low.");

    EXPECT_EQ(0L, t->nMatched);
}

TEST(TriggerTest, WildcardWrappedTriggerMatchesSubstring)
{
    // To match a substring you wrap the pattern in wildcards, exactly as on
    // Windows: "*health*" -> ^(.*?)health(.*?)$.
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "*health*");

    feed_line(doc, "Your health is low.");

    EXPECT_EQ(1L, t->nMatched);
}

TEST(TriggerTest, PlainTriggerMatchesExactLine)
{
    // Counterpart to the above: the plain pattern fires when the line equals it.
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "north");

    feed_line(doc, "go north now");   // contains but != "north"
    EXPECT_EQ(0L, t->nMatched);

    feed_line(doc, "north");          // exact whole line
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
    // "*ping*" (wildcard-wrapped) whole-line-matches a line containing "ping".
    CTrigger* t = add_trigger(doc, "*ping*");

    feed_line(doc, "ping pong ping");   // two occurrences but trigger fires once per line

    // Default bRepeat=false → fires once even if pattern appears multiple times
    EXPECT_EQ(1L, t->nMatched);
}

TEST(TriggerTest, MultipleLinesMultipleMatches)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "*score*");   // wildcard-wrapped: matches lines containing "score"

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

    // Display step 7 (real lifecycle): the tail of m_LineList is the fresh
    // in-progress line; the most recently COMPLETED line sits just before it.
    POSITION pos = doc.m_LineList.GetTailPosition();
    CLine* pTail = doc.m_LineList.GetPrev(pos);   // tail (in-progress)
    ASSERT_NE(nullptr, pTail);
    EXPECT_TRUE(pTail == doc.m_pCurrentLine);
    ASSERT_TRUE(pos != NULL);
    CLine* pLine = doc.m_LineList.GetPrev(pos);   // last completed line
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

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────

// ProcessPreviousLine.cpp: "if a world.note or an input line, do *not*
// process triggers — nb. if you get tempted to you will get a stack overflow
// when a trigger does a world.note".
TEST(TriggerTest, NoteLinesDoNotFireTriggers)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "you have died");

    doc.Note("you have died");            // a note line — must NOT fire
    EXPECT_EQ(0L, t->nMatched);

    doc.DisplayMsg("you have died\n", 14, USER_INPUT);   // user input — ditto
    EXPECT_EQ(0L, t->nMatched);

    feed_line(doc, "you have died");      // real MUD output — fires
    EXPECT_EQ(1L, t->nMatched);
}

// ProcessPreviousLine.cpp: "check for bad UTF8 in the line - otherwise all
// triggers will fail" — a line with an invalid UTF-8 sequence is skipped for
// trigger evaluation entirely and the error counter increments.
TEST(TriggerTest, BadUtf8LineSkipsTriggerEvaluation)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bUTF_8 = TRUE;
    CTrigger* t = add_trigger(doc, "*gold*");

    feed_line(doc, "gold\xC3X");          // 0xC3 with bad continuation
    EXPECT_EQ(0L, t->nMatched);           // evaluation skipped
    EXPECT_GE(doc.m_iUTF8ErrorCount, 1L);

    feed_line(doc, "gold");               // control: clean line fires
    EXPECT_EQ(1L, t->nMatched);
}

// ProcessPreviousLine.cpp: "The text may have matched but do the *colours*?"
// — an iMatch colour constraint requires the first matched character's style
// to carry the wanted foreground.
TEST(TriggerTest, ColourMatchTriggerRequiresMatchingStyle)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "alert");
    // require red (ANSI index 1) foreground: BFS nibbles, F at bits 4-6
    t->iMatch = TRIGGER_MATCH_TEXT | (1 << 4);

    feed_line(doc, "alert");              // white text — text matches, colour no
    EXPECT_EQ(0L, t->nMatched);

    doc.DisplayMsg("\033[31m", 5, 0);     // MUD sets red
    feed_line(doc, "alert");              // red text — fires
    EXPECT_EQ(1L, t->nMatched);
}

// evaluate.cpp: "s += '\n';  // multi-line triggers always end in newlines"
// — the match target is the last iLinesToMatch lines each terminated with
// '\n', so a regexp can span two received lines.
TEST(TriggerTest, MultiLineTriggerMatchesAcrossNewlineJoinedLines)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = new CTrigger();
    t->trigger      = CString("first\\nsecond");
    t->strLabel     = CString("ml");
    t->bEnabled     = TRUE;
    t->bRegexp      = TRUE;
    t->bMultiLine   = true;
    t->iLinesToMatch= 2;
    t->iSendTo      = eSendToOutput;
    t->iSequence    = DEFAULT_TRIGGER_SEQUENCE;
    t->regexp       = regcomp(t->trigger, PCRE_MULTILINE);
    ASSERT_NE(nullptr, t->regexp);
    doc.m_TriggerMap.SetAt(t->strLabel, t);
    doc.m_TriggerArray.Add(t);

    feed_line(doc, "first");
    EXPECT_EQ(0L, t->nMatched);           // needs the second line
    feed_line(doc, "second");
    EXPECT_EQ(1L, t->nMatched);           // matched across "first\nsecond\n"
}

// evaluate.cpp: "trigger match strings can incorporate variables ... if so,
// recompile the regexp with substituted variables" (feature added in 3.18).
TEST(TriggerTest, ExpandVariablesSubstitutesIntoMatchText)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "@target arrives");
    t->bExpandVariables = TRUE;

    CVariable* v = new CVariable;
    v->strLabel = "target";
    v->strContents = "Kobold";
    doc.m_VariableMap.SetAt("target", v);

    feed_line(doc, "Dragon arrives");     // wrong monster
    EXPECT_EQ(0L, t->nMatched);
    feed_line(doc, "Kobold arrives");     // variable's current contents
    EXPECT_EQ(1L, t->nMatched);

    v->strContents = "Dragon";            // recompiled at evaluation time
    feed_line(doc, "Kobold arrives");
    EXPECT_EQ(1L, t->nMatched);           // old value no longer matches
    feed_line(doc, "Dragon arrives");
    EXPECT_EQ(2L, t->nMatched);
}

// evaluate.cpp: "Note, non-existent and empty variables will be silently
// dropped" — an undefined @variable in an expand-variables match text is
// removed without error and the remainder still matches.
TEST(TriggerTest, MissingVariableInMatchTextSilentlyDropped)
{
    CMUSHclientDoc doc = make_doc();
    CTrigger* t = add_trigger(doc, "@missing arrives");
    t->bExpandVariables = TRUE;

    feed_line(doc, " arrives");           // variable dropped: bare remainder
    EXPECT_EQ(1L, t->nMatched);
}

// evaluate.cpp FixWildcard: "escape out backslashes first (ie. \ becomes \\)
// / now turn " to \"" — the order matters, or backslash-quote input would be
// double-escaped.
extern string FixWildcard(const string sWildcard, const bool bMakeLowerCase,
                          const int iSendTo, const CString strLanguage);

TEST(FixWildcardTest, SendToScriptEscapesBackslashesBeforeQuotes)
{
    // input a\"b  →  a\\\"b   (backslash doubled, then quote escaped)
    EXPECT_STREQ("a\\\\\\\"b",
                 FixWildcard("a\\\"b", false, eSendToScript, "Lua").c_str());
    // VBscript instead doubles quotes and leaves backslashes alone
    EXPECT_STREQ("a\\\"\"b",
                 FixWildcard("a\\\"b", false, eSendToScript, "vbscript").c_str());
    // not going to a script: unchanged
    EXPECT_STREQ("a\\\"b",
                 FixWildcard("a\\\"b", false, eSendToWorld, "Lua").c_str());
}

// plugins.h m_iSequence: "negative sequences get evaluated before the main
// world triggers" — observable through send-to-output ordering.
static CTrigger* add_plugin_trigger(CPlugin* p, const char* match,
                                    const char* send)
{
    CTrigger* t  = new CTrigger();
    t->trigger   = CString(match);
    t->contents  = CString(send);
    t->strLabel  = CString(match);
    t->bEnabled  = TRUE;
    t->iSendTo   = eSendToOutput;
    t->iSequence = DEFAULT_TRIGGER_SEQUENCE;
    p->m_TriggerMap.SetAt(t->strLabel, t);
    p->m_TriggerArray.Add(t);
    return t;
}

static int find_line_index(CMUSHclientDoc& doc, const char* text)
{
    int i = 0;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; i++) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (CString(l->text, l->len) == text) return i;
    }
    return -1;
}

TEST(TriggerTest, NegativeSequencePluginTriggersEvaluateBeforeWorld)
{
    {   // negative sequence: plugin's output precedes the world's
        CMUSHclientDoc doc = make_doc();
        CTrigger* wt = add_trigger(doc, "hello", "FROM WORLD", eSendToOutput);
        CPlugin* p = new CPlugin(&doc);
        p->m_bSaveState = false;
        p->m_iSequence  = -100;
        CTrigger* pt = add_plugin_trigger(p, "hello", "FROM PLUGIN");
        doc.m_PluginList.push_back(p);

        feed_line(doc, "hello");
        EXPECT_EQ(1L, wt->nMatched);
        EXPECT_EQ(1L, pt->nMatched);
        int ip = find_line_index(doc, "FROM PLUGIN");
        int iw = find_line_index(doc, "FROM WORLD");
        ASSERT_TRUE(ip >= 0 && iw >= 0);
        EXPECT_LT(ip, iw);                 // plugin evaluated first
    }
    {   // default (positive) sequence: the world's triggers run first
        CMUSHclientDoc doc = make_doc();
        add_trigger(doc, "hello", "FROM WORLD", eSendToOutput);
        CPlugin* p = new CPlugin(&doc);
        p->m_bSaveState = false;
        p->m_iSequence  = 100;             // positive
        add_plugin_trigger(p, "hello", "FROM PLUGIN");
        doc.m_PluginList.push_back(p);

        feed_line(doc, "hello");
        int ip = find_line_index(doc, "FROM PLUGIN");
        int iw = find_line_index(doc, "FROM WORLD");
        ASSERT_TRUE(ip >= 0 && iw >= 0);
        EXPECT_LT(iw, ip);                 // world evaluated first
    }
}

// ── Intent-comment tail (candidate mining, 2026-07) ───────────────────────

#ifdef MUSHCLIENT_LUA_SCRIPTING
// lua_scripting.cpp (wildcards-table construction): "this code is to ensure
// that we don't find a match (eg. mob = Kobold) and then if duplicates were
// allowed, replace Kobold with false." — the wildcards table handed to a
// Lua trigger keeps the non-empty capture for a (?J) duplicate name.
TEST(TriggerScript, DuplicateNamedWildcardKeepsNonEmptyCapture)
{
    CMUSHclientDoc doc = make_doc();

    const char* lua =
        "function OnMob(name, line, wildcards)\n"
        "  world.SetVariable('mob', tostring(wildcards.mob))\n"
        "end\n";
    ASSERT_TRUE(setup_lua(doc, lua));

    CTrigger* t = new CTrigger;
    t->trigger      = CString("(?J)(?P<mob>\\w+) attacks(?: (?P<mob>\\w+))?");
    t->strLabel     = CString("mobtrig");
    t->strProcedure = CString("OnMob");
    t->bEnabled     = TRUE;
    t->bRegexp      = TRUE;
    t->iSendTo      = eSendToWorld;
    t->iSequence    = DEFAULT_TRIGGER_SEQUENCE;
    t->regexp       = regcomp(t->trigger, 0);
    ASSERT_NE(nullptr, t->regexp);
    doc.GetTriggerMap().SetAt(t->strLabel, t);
    doc.GetTriggerArray().Add(t);
    doc.GetTriggerRevMap()[t] = "mobtrig";
    doc.ResolveAllEntryPoints();

    feed_line(doc, "Kobold attacks");        // second <mob> group unset
    CVariable* v = nullptr;
    ASSERT_TRUE(doc.GetVariableMap().Lookup("mob", v));
    ASSERT_NE(nullptr, v);
    EXPECT_STREQ("Kobold", (const char*)v->strContents);   // NOT clobbered

    feed_line(doc, "Kobold attacks Dragon"); // control: set duplicate wins
    ASSERT_TRUE(doc.GetVariableMap().Lookup("mob", v));
    EXPECT_STREQ("Dragon", (const char*)v->strContents);
}
#endif // MUSHCLIENT_LUA_SCRIPTING

// ═══ world.* trigger API (methods_triggers.cpp) ═══════════════════════════

#include "flags.h"   // eEnabled / eReplace / eTemporary alias-flags

static std::string vs(const VARIANT& v)
{
    if (v.vt != VT_BSTR) return "<not-a-string>";
    return std::string((const char*)CString(v.bstrVal));
}

// AddTriggerEx validation: each reject path answers its own error code.
TEST(TriggerApi, AddTriggerExValidatesInput)
{
    CMUSHclientDoc doc = make_doc();

    EXPECT_EQ((long)eOK, doc.AddTriggerEx("t1", "You are hungry", "eat bread",
                                          eEnabled, -1, 0, "", "",
                                          eSendToWorld, 100));
    EXPECT_EQ((long)eOK, doc.IsTrigger("t1"));

    EXPECT_EQ((long)eTriggerAlreadyExists,
              doc.AddTriggerEx("t1", "x", "", eEnabled, -1, 0, "", "",
                               eSendToWorld, 100));
    EXPECT_EQ((long)eOK,
              doc.AddTriggerEx("t1", "replaced", "", eEnabled | eReplace,
                               -1, 0, "", "", eSendToWorld, 100));
    EXPECT_STREQ("replaced", vs(doc.GetTriggerInfo("t1", 1)).c_str());

    EXPECT_EQ((long)eTriggerCannotBeEmpty,
              doc.AddTriggerEx("t2", "", "", eEnabled, -1, 0, "", "",
                               eSendToWorld, 100));
    EXPECT_EQ((long)eInvalidObjectLabel,
              doc.AddTriggerEx("9bad", "m", "", eEnabled, -1, 0, "", "",
                               eSendToWorld, 100));
    EXPECT_EQ((long)eTriggerSequenceOutOfRange,
              doc.AddTriggerEx("t3", "m", "", eEnabled, -1, 0, "", "",
                               eSendToWorld, 10001));
    EXPECT_EQ((long)eTriggerSendToInvalid,
              doc.AddTriggerEx("t4", "m", "", eEnabled, -1, 0, "", "",
                               99, 100));
    // Windows parity: regcomp throws on a malformed pattern -> AddTriggerEx
    // answers eBadRegularExpression and does NOT store the trigger.
    EXPECT_EQ((long)eBadRegularExpression,
              doc.AddTriggerEx("t5", "(unclosed", "",
                               eEnabled | eTriggerRegularExpression,
                               -1, 0, "", "", eSendToWorld, 100));
    EXPECT_EQ(VT_EMPTY, (int)doc.GetTriggerInfo("t5", 1).vt);   // not created
}

// GetTrigger round trip and GetTriggerList enumeration.
TEST(TriggerApi, GetTriggerRoundTripAndList)
{
    CMUSHclientDoc doc = make_doc();
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("rt", "* hits you", "flee",
                                          eEnabled | eKeepEvaluating,
                                          -1, 0, "", "", eSendToWorld, 100));

    VARIANT m, r, f, c, w, snd, scr;
    VariantInit(&m); VariantInit(&r); VariantInit(&f); VariantInit(&c);
    VariantInit(&w); VariantInit(&snd); VariantInit(&scr);
    EXPECT_EQ((long)eOK, doc.GetTrigger("rt", &m, &r, &f, &c, &w, &snd, &scr));
    EXPECT_STREQ("* hits you", vs(m).c_str());
    EXPECT_STREQ("flee",       vs(r).c_str());
    ASSERT_EQ(VT_I2, (int)f.vt);
    EXPECT_TRUE((f.iVal & eEnabled) != 0);
    EXPECT_EQ((long)eTriggerNotFound,
              doc.GetTrigger("nope", &m, &r, &f, &c, &w, &snd, &scr));

    VARIANT v = doc.GetTriggerList();
    ASSERT_EQ((int)(VT_ARRAY | VT_VARIANT), (int)v.vt);
    COleSafeArray sa;
    sa.Attach(v);
    EXPECT_EQ(1L, sa.GetOneDimSize());
    long i0 = 0;
    VARIANT e; VariantInit(&e);
    sa.GetElement(&i0, &e);
    EXPECT_STREQ("rt", vs(e).c_str());
}

// GetTriggerInfo conventions and representative arms.
TEST(TriggerApi, GetTriggerInfoArms)
{
    CMUSHclientDoc doc = make_doc();
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("ti", "the match", "the send",
                                          eEnabled | eTemporary,
                                          -1, 0, "beep.wav", "",
                                          eSendToOutput, 42));

    EXPECT_EQ(VT_EMPTY, (int)doc.GetTriggerInfo("missing", 1).vt);
    EXPECT_EQ(VT_NULL,  (int)doc.GetTriggerInfo("ti", 999).vt);

    EXPECT_STREQ("the match", vs(doc.GetTriggerInfo("ti", 1)).c_str());
    EXPECT_STREQ("the send",  vs(doc.GetTriggerInfo("ti", 2)).c_str());
    EXPECT_STREQ("beep.wav",  vs(doc.GetTriggerInfo("ti", 3)).c_str());
    EXPECT_TRUE(doc.GetTriggerInfo("ti", 8).boolVal);           // enabled
    EXPECT_FALSE(doc.GetTriggerInfo("ti", 9).boolVal);          // not regexp
    EXPECT_EQ((short)eSendToOutput, doc.GetTriggerInfo("ti", 15).iVal);
    EXPECT_EQ((short)42, doc.GetTriggerInfo("ti", 16).iVal);    // sequence
    EXPECT_EQ(0L, doc.GetTriggerInfo("ti", 21).lVal);           // never matched
    EXPECT_TRUE(doc.GetTriggerInfo("ti", 23).boolVal);          // temporary
}

// Trigger options: y/n booleans, numeric validation, and the two error codes.
TEST(TriggerApi, TriggerOptionRoundTripAndErrors)
{
    CMUSHclientDoc doc = make_doc();
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("to", "m", "", eEnabled,
                                          -1, 0, "", "", eSendToWorld, 100));

    EXPECT_EQ((long)eTriggerNotFound, doc.SetTriggerOption("zz", "enabled", "n"));
    EXPECT_EQ((long)eUnknownOption,   doc.SetTriggerOption("to", "no_such", "1"));

    EXPECT_EQ((long)eOK, doc.SetTriggerOption("to", "enabled", "n"));
    EXPECT_EQ(0L, doc.GetTriggerOption("to", "enabled").lVal);
    EXPECT_EQ((long)eOK, doc.SetTriggerOption("to", "enabled", "y"));
    EXPECT_EQ(1L, doc.GetTriggerOption("to", "enabled").lVal);

    EXPECT_EQ((long)eOK, doc.SetTriggerOption("to", "sequence", "77"));
    EXPECT_EQ((short)77, doc.GetTriggerInfo("to", 16).iVal);
    EXPECT_EQ((long)eOptionOutOfRange,
              doc.SetTriggerOption("to", "sequence", "junk"));

    EXPECT_EQ((long)eOK, doc.SetTriggerOption("to", "group", "grp"));
    EXPECT_STREQ("grp", vs(doc.GetTriggerOption("to", "group")).c_str());
}

// Wildcards captured when a MUD line matches an API-added trigger.
TEST(TriggerApi, WildcardsCapturedOnLineMatch)
{
    CMUSHclientDoc doc = make_doc();
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("wc", "* tells you *", "",
                                          eEnabled, -1, 0, "", "",
                                          eSendToWorld, 100));

    feed_line(doc, "Gandalf tells you run");

    EXPECT_STREQ("Gandalf", vs(doc.GetTriggerWildcard("wc", "1")).c_str());
    EXPECT_STREQ("run",     vs(doc.GetTriggerWildcard("wc", "2")).c_str());
    EXPECT_STREQ("Gandalf tells you run",
                 vs(doc.GetTriggerWildcard("wc", "0")).c_str());
    EXPECT_EQ(1L, doc.GetTriggerInfo("wc", 21).lVal);          // matched once
    EXPECT_EQ(VT_EMPTY, (int)doc.GetTriggerWildcard("missing", "1").vt);
}

// Group enable/delete counts only members; temporaries are swept.
TEST(TriggerApi, GroupsAndTemporarySweep)
{
    CMUSHclientDoc doc = make_doc();
    doc.AddTriggerEx("g1", "m1", "", eEnabled, -1, 0, "", "", eSendToWorld, 100);
    doc.AddTriggerEx("g2", "m2", "", eEnabled, -1, 0, "", "", eSendToWorld, 100);
    doc.AddTriggerEx("solo", "m3", "", eEnabled, -1, 0, "", "", eSendToWorld, 100);
    doc.SetTriggerOption("g1", "group", "grp");
    doc.SetTriggerOption("g2", "group", "grp");

    EXPECT_EQ(2L, doc.EnableTriggerGroup("grp", FALSE));
    EXPECT_FALSE(doc.GetTriggerInfo("g1", 8).boolVal);
    EXPECT_TRUE(doc.GetTriggerInfo("solo", 8).boolVal);
    EXPECT_EQ(0L, doc.EnableTriggerGroup("nogroup", TRUE));

    EXPECT_EQ(2L, doc.DeleteTriggerGroup("grp"));
    EXPECT_EQ((long)eTriggerNotFound, doc.IsTrigger("g1"));

    doc.AddTriggerEx("tmp", "mt", "", eEnabled | eTemporary,
                     -1, 0, "", "", eSendToWorld, 100);
    EXPECT_EQ(1L, doc.DeleteTemporaryTriggers());
    EXPECT_EQ((long)eTriggerNotFound, doc.IsTrigger("tmp"));
    EXPECT_EQ((long)eOK, doc.IsTrigger("solo"));

    EXPECT_EQ((long)eOK, doc.DeleteTrigger("solo"));
    EXPECT_EQ((long)eTriggerNotFound, doc.DeleteTrigger("solo"));
}

// EnableTrigger toggling and the missing-name code.
TEST(TriggerApi, EnableTriggerToggleAndMissing)
{
    CMUSHclientDoc doc = make_doc();
    doc.AddTriggerEx("et", "m", "", eEnabled, -1, 0, "", "", eSendToWorld, 100);
    EXPECT_EQ((long)eOK, doc.EnableTrigger("et", FALSE));
    EXPECT_FALSE(doc.GetTriggerInfo("et", 8).boolVal);
    EXPECT_EQ((long)eOK, doc.EnableTrigger("et", TRUE));
    EXPECT_TRUE(doc.GetTriggerInfo("et", 8).boolVal);
    EXPECT_EQ((long)eTriggerNotFound, doc.EnableTrigger("nope", TRUE));
}

MC_TEST_MAIN()
