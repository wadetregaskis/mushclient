// tests/test_lua_scripting.cpp
// Tests for the portable Lua scripting layer:
//   - world library registration and basic calls
//   - variable get/set
//   - trigger/alias/timer CRUD via Lua
//   - GetOptionList / GetDefaultValue (real OptionsTable)
//   - GetStyleInfo
//   - Array subsystem
//   - Hashing functions
//   - String utilities (Replace, EditDistance, Metaphone, TranslateGerman)
//
// Since the Campaign C flip, mushcore compiles the REAL
// scripting/lua_methods.cpp (the same 434-entry world library the Windows
// build and mushgui use); the portable twin (lua_world_api_portable.cpp) is
// gone.  These tests therefore assert the full Windows Lua API + marshalling.
// GUI-only method families resolve to the inert headless bodies in
// platform/doc_gui_method_stubs.cpp (see GuiStubsReturnValues).

#ifdef MUSHCLIENT_LUA_SCRIPTING

#include "mc_test.h"
#include "platform/doc_stub.h"
#include "scripting/scripting.h"   // DOCUMENT_STATE, MakeLuaState, RegisterLuaRoutines

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cstring>
#include <cstdio>

// ── Lua state fixture ─────────────────────────────────────────────────────
// Each test that needs a Lua+Doc pair calls make_lua() and must call
// lua_close(L) at the end.

lua_State* MakeLuaState();
int RegisterLuaRoutines(lua_State*);
extern "C" int luaopen_compress(lua_State*);   // utils.md5/sha256/tohex/... (lua_compress.c)

static lua_State* make_lua(CMUSHclientDoc* pDoc)
{
    lua_State* L = MakeLuaState();
    if (!L) return nullptr;
    luaL_openlibs(L);
    // Bind the doc pointer into the Lua registry (mirrors OpenLuaDelayed)
    lua_pushlightuserdata(L, static_cast<void*>(pDoc));
    lua_setfield(L, LUA_REGISTRYINDEX, DOCUMENT_STATE);
    RegisterLuaRoutines(L);
    luaopen_compress(L);            // mirrors OpenLuaDelayed (lua_scripting.cpp:203)
    RegisterPortableLuaExtras(L);   // port-only world.ExpandVariables
    lua_settop(L, 0);
    return L;
}

// Run a Lua chunk; return true on success.
static bool run(lua_State* L, const char* code)
{
    if (luaL_loadstring(L, code) != 0) {
        fprintf(stderr, "  Lua load error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
        fprintf(stderr, "  Lua runtime error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// Run a Lua chunk that is expected to succeed, leaving results on stack.
// Returns the number of results.
static int run_ret(lua_State* L, const char* code)
{
    int top = lua_gettop(L);
    if (luaL_loadstring(L, code) != 0) {
        fprintf(stderr, "  Lua load error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
        fprintf(stderr, "  Lua runtime error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    return lua_gettop(L) - top;
}

// ── Basic world library ───────────────────────────────────────────────────

TEST(LuaScripting, WorldLibExists)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // 'world' global should be a table
    lua_getglobal(L, "world");
    EXPECT_TRUE(lua_istable(L, -1));
    lua_pop(L, 1);

    lua_close(L);
}

TEST(LuaScripting, WorldVersion)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.Version()");
    EXPECT_EQ(1, n);
    EXPECT_TRUE(lua_isstring(L, -1));
    const char* ver = lua_tostring(L, -1);
    // Version should look like "5.07" or similar
    EXPECT_TRUE(ver && strlen(ver) >= 4);
    lua_settop(L, 0);

    lua_close(L);
}

// Display step 7 regression: the in-progress line is the m_LineList TAIL.
// The old portable DeleteLines/DeleteOutput freed it, leaving m_pCurrentLine
// dangling — the next Note dereferenced freed memory (SIGSEGV).
TEST(LuaScripting, DeleteLinesThenOutputDoesNotUseAfterFree)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    run_ret(L, "world.Note('one') world.Note('two') world.Note('three')");
    run_ret(L, "world.DeleteLines(2)");
    run_ret(L, "world.Note('after delete')");     // crashed pre-fix
    EXPECT_TRUE(doc.m_pCurrentLine != NULL);
    EXPECT_GT((long)doc.m_LineList.GetCount(), 0L);
    EXPECT_TRUE(doc.m_LineList.GetTail() == doc.m_pCurrentLine);

    run_ret(L, "world.DeleteOutput()");
    run_ret(L, "world.Note('after clear')");      // crashed pre-fix
    EXPECT_TRUE(doc.m_pCurrentLine != NULL);
    EXPECT_TRUE(doc.m_LineList.GetTail() == doc.m_pCurrentLine);
    lua_settop(L, 0);

    lua_close(L);
}

// ── Variable get/set ─────────────────────────────────────────────────────

TEST(LuaScripting, SetAndGetVariable)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "world.SetVariable('myvar', 'hello world')"
    ));
    int n = run_ret(L, "return world.GetVariable('myvar')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("hello world", lua_tostring(L, -1));
    lua_settop(L, 0);

    // Overwrite
    EXPECT_TRUE(run(L, "world.SetVariable('myvar', '42')"));
    n = run_ret(L, "return world.GetVariable('myvar')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("42", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, GetVariableMissing)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.GetVariable('does_not_exist')");
    EXPECT_EQ(1, n);
    // Missing variable returns nil or empty string depending on implementation
    // (portable: returns "" for missing)
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, DeleteVariable)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.SetVariable('tmp', 'x')"));
    EXPECT_TRUE(run(L, "world.DeleteVariable('tmp')"));

    // Should be gone now
    int n = run_ret(L, "return world.GetVariable('tmp')");
    EXPECT_EQ(1, n);
    // After delete, variable should not exist — returns "" or nil
    lua_settop(L, 0);

    lua_close(L);
}

// ── Variable list ────────────────────────────────────────────────────────

TEST(LuaScripting, GetVariableList)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "world.SetVariable('alpha', '1')\n"
        "world.SetVariable('beta',  '2')\n"
        "world.SetVariable('gamma', '3')\n"
    ));

    int n = run_ret(L, "return world.GetVariableList()");
    EXPECT_EQ(1, n);
    EXPECT_TRUE(lua_istable(L, -1));

    // Count entries
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        count++;
        lua_pop(L, 1);
    }
    EXPECT_EQ(3, count);
    lua_settop(L, 0);

    lua_close(L);
}

// ── Trigger CRUD via Lua ──────────────────────────────────────────────────

TEST(LuaScripting, AddAndGetTrigger)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // AddTrigger(name, match, response, flags, colour, wildcard, sound[, script])
    // — the Windows shape: L_AddTrigger (lua_methods.cpp:516-530) my_checks
    // args 1-7 (script optional).  The old portable twin accepted 4-5 args —
    // twin-only laxness.
    int n = run_ret(L,
        "return world.AddTrigger('trig1', 'you have died', 'say I died!', 0, 0, 0, '')"
    );
    EXPECT_EQ(1, n);
    EXPECT_EQ(0.0, lua_tonumber(L, -1));   // eOK == 0
    lua_settop(L, 0);

    // Verify the trigger exists
    CTrigger* t = nullptr;
    EXPECT_TRUE(doc.GetTriggerMap().Lookup("trig1", t));
    ASSERT_NE(nullptr, t);
    EXPECT_STREQ("you have died", (const char*)t->trigger);

    // GetTriggerInfo: match text is type 1, response/"send" text is type 2
    // (type 5 is the omit-from-log flag) — matches the canonical numbering.
    n = run_ret(L, "return world.GetTriggerInfo('trig1', 2)");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("say I died!", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, DeleteTrigger)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.AddTrigger('t1', 'pat', 'resp', 0, 0, 0, '')"));
    EXPECT_TRUE(run(L, "world.DeleteTrigger('t1')"));

    CTrigger* t = nullptr;
    EXPECT_FALSE(doc.GetTriggerMap().Lookup("t1", t));

    lua_close(L);
}

TEST(LuaScripting, EnableDisableTrigger)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.AddTrigger('t2', 'match', 'send', 1, 0, 0, '')"));  // flags=1=Enabled

    // Disable it
    EXPECT_TRUE(run(L, "world.EnableTrigger('t2', false)"));
    CTrigger* t = nullptr;
    doc.GetTriggerMap().Lookup("t2", t);
    ASSERT_NE(nullptr, t);
    EXPECT_FALSE(t->bEnabled);

    // Re-enable
    EXPECT_TRUE(run(L, "world.EnableTrigger('t2', true)"));
    EXPECT_TRUE(t->bEnabled);

    lua_close(L);
}

// ── Get/Set*Option round trips (the REAL doc methods via the thin wrappers) ──
// Locks the Windows marshalling semantics: boolean options come back as
// NUMBERS (0/1) exactly as on Windows, read-only options refuse writes with
// ePluginCannotSetOption, unknown options return eUnknownOption, and the
// GetPlugin*Option empty-ID form reads the world's own items.

TEST(LuaScripting, TriggerOptionRoundTrip)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.AddTrigger('t3', 'pat', 'resp', 0, 0, 0, '')"));

    // numeric option round trip
    run_ret(L, "return world.SetTriggerOption('t3', 'sequence', 42)");
    EXPECT_EQ((double)eOK, lua_tonumber(L, -1));
    lua_settop(L, 0);
    run_ret(L, "return world.GetTriggerOption('t3', 'sequence')");
    EXPECT_EQ(42.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // Windows parity: boolean options are numbers, not Lua booleans
    run_ret(L, "return type(world.GetTriggerOption('t3', 'enabled'))");
    EXPECT_STREQ("number", lua_tostring(L, -1));
    lua_settop(L, 0);

    // Lua boolean value coerces via the "1"/"0" convention (get_option_value)
    run_ret(L, "return world.SetTriggerOption('t3', 'enabled', true)");
    EXPECT_EQ((double)eOK, lua_tonumber(L, -1));
    lua_settop(L, 0);
    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("t3", t));
    EXPECT_TRUE(t->bEnabled);

    // alpha option round trip
    run_ret(L, "return world.SetTriggerOption('t3', 'group', 'grp')");
    EXPECT_EQ((double)eOK, lua_tonumber(L, -1));
    lua_settop(L, 0);
    run_ret(L, "return world.GetTriggerOption('t3', 'group')");
    EXPECT_STREQ("grp", lua_tostring(L, -1));
    lua_settop(L, 0);

    // read-only (OPT_CANNOT_WRITE) and unknown options
    run_ret(L, "return world.SetTriggerOption('t3', 'regexp', '1')");
    EXPECT_EQ((double)ePluginCannotSetOption, lua_tonumber(L, -1));
    lua_settop(L, 0);
    run_ret(L, "return world.SetTriggerOption('t3', 'bogus', '1')");
    EXPECT_EQ((double)eUnknownOption, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // missing trigger: get → nil, set → eTriggerNotFound
    run_ret(L, "return world.GetTriggerOption('nosuch', 'enabled')");
    EXPECT_TRUE(lua_isnil(L, -1));
    lua_settop(L, 0);
    run_ret(L, "return world.SetTriggerOption('nosuch', 'enabled', '1')");
    EXPECT_EQ((double)eTriggerNotFound, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // GetPluginTriggerOption with an empty ID reads the world's own trigger
    run_ret(L, "return world.GetPluginTriggerOption('', 't3', 'sequence')");
    EXPECT_EQ(42.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, AliasAndTimerOptionRoundTrip)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.AddAlias('a2', 'shortcut', 'expansion', 1)"));
    run_ret(L, "return world.SetAliasOption('a2', 'send_to', '2')");
    EXPECT_EQ((double)eOK, lua_tonumber(L, -1));
    lua_settop(L, 0);
    run_ret(L, "return world.GetAliasOption('a2', 'send_to')");
    EXPECT_EQ(2.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // empty match text is rejected with the canonical error
    run_ret(L, "return world.SetAliasOption('a2', 'match', '')");
    EXPECT_EQ((double)eAliasCannotBeEmpty, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // timer: the double-valued 'second' option round-trips through VT_R8
    EXPECT_TRUE(run(L, "world.AddTimer('tmr2', 0, 0, 30, 'ping', 1)"));
    run_ret(L, "return world.GetTimerOption('tmr2', 'second')");
    EXPECT_EQ(30.0, lua_tonumber(L, -1));
    lua_settop(L, 0);
    run_ret(L, "return world.SetTimerOption('tmr2', 'second', '12.5')");
    EXPECT_EQ((double)eOK, lua_tonumber(L, -1));
    lua_settop(L, 0);
    run_ret(L, "return world.GetTimerOption('tmr2', 'second')");
    EXPECT_EQ(12.5, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// ── GOLDEN world-file option round trip ──────────────────────────────────────
// The safety net for the option-machinery convergence: set a deliberate mix of
// options through the scripting API (numeric bool/ushort/long, an RGB colour
// that has NO hand-written live-member path, plus single-line / multiline /
// password alphas), save the world, load it into a FRESH doc, and assert every
// value through the scripting API again.  This test must keep passing
// unchanged when the zero-offset portable tables are replaced by the real
// scriptingoptions.cpp machinery.
TEST(LuaScripting, GoldenWorldOptionRoundTrip)
{
    const char* path = "/tmp/mc_golden_option_roundtrip.mcl";
    remove(path);

    {
        CMUSHclientDoc doc;
        lua_State* L = make_lua(&doc);
        ASSERT_NE(nullptr, L);
        EXPECT_TRUE(run(L,
            "assert(world.SetOption('enable_triggers', 0) == 0)\n"
            "assert(world.SetOption('auto_wrap_window_width', 1) == 0)\n"
            "assert(world.SetOption('wrap_column', 100) == 0)\n"
            "assert(world.SetOption('speed_walk_delay', 25) == 0)\n"
            "assert(world.SetOption('max_output_lines', 2000) == 0)\n"
            "assert(world.SetOption('hyperlink_colour', 0x123456) == 0)\n"
            "assert(world.SetAlphaOption('player', 'wizard') == 0)\n"
            "assert(world.SetAlphaOption('notes', 'line one\\nline two') == 0)\n"
            "assert(world.SetAlphaOption('password', 'secret') == 0)\n"));
        EXPECT_TRUE(doc.OnSaveDocument(path));
        lua_close(L);
    }

    {
        CMUSHclientDoc doc2;
        ASSERT_TRUE(doc2.LoadWorldFromFile(path));
        lua_State* L2 = make_lua(&doc2);
        ASSERT_NE(nullptr, L2);
        EXPECT_TRUE(run(L2,
            "assert(world.GetOption('enable_triggers') == 0, 'enable_triggers')\n"
            "assert(world.GetOption('auto_wrap_window_width') == 1, 'auto_wrap')\n"
            "assert(world.GetOption('wrap_column') == 100, 'wrap_column')\n"
            "assert(world.GetOption('speed_walk_delay') == 25, 'speed_walk_delay')\n"
            "assert(world.GetOption('max_output_lines') == 2000, 'max_output_lines')\n"
            "assert(world.GetOption('hyperlink_colour') == 0x123456, 'hyperlink_colour')\n"
            "assert(world.GetAlphaOption('player') == 'wizard', 'player')\n"
            // xmlparse.cpp normalises element content: LF becomes CRLF on
            // load (identical on Windows — same parser TU), so the reloaded
            // multiline value carries \r\n.
            "assert(world.GetAlphaOption('notes') == 'line one\\r\\nline two', 'notes')\n"
            "assert(world.GetAlphaOption('password') == 'secret', 'password')\n"
            // the loaded-value snapshot reflects the file, the default the table
            "assert(world.GetLoadedValue('wrap_column') == 100, 'loaded wrap_column')\n"
            "assert(world.GetDefaultValue('wrap_column') == 80, 'default wrap_column')\n"
            "assert(world.GetCurrentValue('wrap_column') == 100, 'current wrap_column')\n"));
        lua_close(L2);
    }
    remove(path);
}

// ── ExportXML / ImportXML round trip ─────────────────────────────────────────
// world.ExportXML runs the REAL Windows machinery now (methods_xml.cpp over
// the canonical Save_One_Trigger_XML in xml/xml_save_world.cpp); ImportXML
// re-parses the produced XML.  Export a trigger, delete it, reimport it.
TEST(LuaScripting, ExportImportXMLRoundTrip)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "assert(world.AddTrigger('extrig', 'xyzzy plugh', 'say hello', 1, 0, 0, '') == 0)\n"
        "local xml = world.ExportXML(0, 'extrig')\n"
        "assert(type(xml) == 'string' and #xml > 0, 'export came back empty')\n"
        "assert(xml:find('<trigger', 1, true), 'no <trigger element')\n"
        "assert(xml:find('match=\"xyzzy plugh\"', 1, true), 'match attribute missing')\n"
        "assert(world.DeleteTrigger('extrig') == 0)\n"
        "assert(world.IsTrigger('extrig') ~= 0, 'delete did not remove trigger')\n"
        "assert(world.ImportXML(xml) == 1, 'import count ~= 1')\n"
        "assert(world.IsTrigger('extrig') == 0, 'trigger not reimported')\n"
        "assert(world.GetTriggerOption('extrig', 'match') == 'xyzzy plugh', 'match lost')\n"));

    lua_close(L);
}

// ── Alias CRUD via Lua ────────────────────────────────────────────────────

TEST(LuaScripting, AddAndGetAlias)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // AddAlias(name, match, response, flags [, script]).  The optional 5th arg
    // is the script ROUTINE NAME (a string); a non-empty name that doesn't
    // resolve is rejected with eScriptNameNotLocated (as on Windows), so omit it.
    int n = run_ret(L,
        "return world.AddAlias('a1', '^go (.+)$', 'go %1', 128+1024)"
    );
    EXPECT_EQ(1, n);
    EXPECT_EQ(0.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    CAlias* a = nullptr;
    EXPECT_TRUE(doc.GetAliasMap().Lookup("a1", a));
    ASSERT_NE(nullptr, a);
    EXPECT_STREQ("^go (.+)$", (const char*)a->name);

    lua_close(L);
}

// ── Timer CRUD via Lua ────────────────────────────────────────────────────

TEST(LuaScripting, AddAndGetTimer)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // AddTimer(name, hour, minute, second, send, flags [, script]).  The
    // optional 7th arg is the script ROUTINE NAME (a string); omit it rather
    // than pass a number (which would be a non-existent routine name).
    int n = run_ret(L,
        "return world.AddTimer('tmr1', 0, 0, 30, 'ping', 1)"
    );
    EXPECT_EQ(1, n);
    EXPECT_EQ(0.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    CTimer* t = nullptr;
    EXPECT_TRUE(doc.GetTimerMap().Lookup("tmr1", t));
    ASSERT_NE(nullptr, t);

    lua_close(L);
}

// ── GUI stub graceful failure ─────────────────────────────────────────────

TEST(LuaScripting, GuiStubsReturnValues)
{
    // Window*, Chat*, Spell* stubs must return a number (not crash with nil-call).
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // WindowCreate → eNoSuchWindow (30073).  Argument shape is the Windows
    // one — L_WindowCreate (lua_methods.cpp:6060-6075): (Name, Left, Top,
    // Width, Height, Position, Flags, BackgroundColour), all my_check'd.
    int n = run_ret(L, "return world.WindowCreate('w',0,0,100,100,0,0,0)");
    EXPECT_EQ(1, n);
    EXPECT_EQ(30073.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // WindowList with no miniwindows → nil (Windows: the safe array is never
    // created — "cannot create empty dimension" — so Detach hands back VT_EMPTY)
    n = run_ret(L, "return world.WindowList()");
    EXPECT_EQ(1, n);
    EXPECT_TRUE(lua_isnil(L, -1));
    lua_settop(L, 0);

    // ChatCall → eNoChatConnections (30044)
    n = run_ret(L, "return world.ChatCall('host', 4050)");
    EXPECT_EQ(1, n);
    EXPECT_EQ(30044.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // SpellCheck with the spell checker inactive → nil, as on Windows:
    // methods_spellchecker.cpp:30-31 returns an EMPTY VARIANT when
    // App.m_bSpellCheckOK is false, and lua_methods.cpp L_SpellCheck
    // (5675-5680) marshals that through pushVariant's default → nil.
    // (The old portable L_SpellStub returned eSpellCheckNotActive for every
    // spellcheck entry point — twin-only behaviour.)
    n = run_ret(L, "return world.SpellCheck('hello')");
    EXPECT_EQ(1, n);
    EXPECT_TRUE(lua_isnil(L, -1));
    lua_settop(L, 0);

    // AddSpellCheckWord is the entry point that DOES surface the canonical
    // code: eSpellCheckNotActive (30064) — methods_spellchecker.cpp:252-253.
    n = run_ret(L, "return world.AddSpellCheckWord('helo', 'a', 'hello')");
    EXPECT_EQ(1, n);
    EXPECT_EQ(30064.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // SpellCheckCommand with no command window / no spell checker → -1
    // (methods_spellchecker.cpp:110-111).
    n = run_ret(L, "return world.SpellCheckCommand()");
    EXPECT_EQ(1, n);
    EXPECT_EQ(-1.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // ActivateNotepad marshals as a BOOLEAN on Windows — L_ActivateNotepad
    // (lua_methods.cpp:419-424) does lua_pushboolean of the doc method's
    // BOOL, and CMUSHclientDoc::ActivateNotepad (methods_notepad.cpp:210-241)
    // returns false when FindNotepad comes back NULL.  No 'test' notepad
    // exists here → false.  (The old portable twin returned the number
    // 30073 — twin-only marshalling.)
    n = run_ret(L, "return world.ActivateNotepad('test')");
    EXPECT_EQ(1, n);
    EXPECT_TRUE(lua_isboolean(L, -1));
    EXPECT_FALSE(lua_toboolean(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// ── Global constant tables ────────────────────────────────────────────────

TEST(LuaScripting, ErrorCodeTable)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // error_code global table must exist and hold correct values
    int n = run_ret(L, "return type(error_code)");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("table", lua_tostring(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return error_code.eOK");
    EXPECT_EQ(1, n);
    EXPECT_EQ(0.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return error_code.eTriggerNotFound");
    EXPECT_EQ(1, n);
    EXPECT_EQ(30005.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return error_code.eUnknownOption");
    EXPECT_EQ(1, n);
    EXPECT_EQ(30025.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return error_code.eNoSuchWindow");
    EXPECT_EQ(1, n);
    EXPECT_EQ(30073.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, MiniwinTable)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // miniwin global table must exist (even though Window* functions are stubs)
    int n = run_ret(L, "return type(miniwin)");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("table", lua_tostring(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return miniwin.pos_top_left");
    EXPECT_EQ(1, n);
    EXPECT_EQ(4.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return miniwin.create_transparent");
    EXPECT_EQ(1, n);
    EXPECT_EQ(4.0, lua_tonumber(L, -1));   // 0x04
    lua_settop(L, 0);

    n = run_ret(L, "return miniwin.pen_solid");
    EXPECT_EQ(1, n);
    EXPECT_EQ(0.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// ── Option tables ─────────────────────────────────────────────────────────

TEST(LuaScripting, GetOptionListNonEmpty)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.GetOptionList()");
    EXPECT_EQ(1, n);
    ASSERT_TRUE(lua_istable(L, -1));

    // Count entries — should be 174 (the real table)
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        count++;
        lua_pop(L, 1);
    }
    EXPECT_GT(count, 100);   // real table has 174 entries
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, GetAlphaOptionListNonEmpty)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.GetAlphaOptionList()");
    EXPECT_EQ(1, n);
    ASSERT_TRUE(lua_istable(L, -1));

    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        count++;
        lua_pop(L, 1);
    }
    EXPECT_GT(count, 50);   // real table has 70 entries
    lua_settop(L, 0);

    lua_close(L);
}

// ── VT_ARRAY marshalling ──────────────────────────────────────────────────
// GetTimerList / GetOptionList are now thin wrappers over the REAL doc
// methods, whose safearray VARIANTs are marshalled by pushVariant's
// (VT_ARRAY|VT_VARIANT) branch (lua_methods.cpp:178-204).  Lock the shape:
// a 1-based, ipairs-iterable Lua array of names.

TEST(LuaScripting, SafeArrayListMarshalling)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "assert(world.AddTimer('tlist_a', 0, 0, 5, 'x', 0, '') == 0)\n"
        "assert(world.AddTimer('tlist_b', 0, 0, 7, 'y', 0, '') == 0)\n"
    ));

    // Exactly two entries, 1-based, both names present, ipairs-iterable
    EXPECT_TRUE(run(L,
        "local t = world.GetTimerList()\n"
        "assert(type(t) == 'table', 'expected a table, got ' .. type(t))\n"
        "assert(t[0] == nil, 'expected a 1-based array')\n"
        "local n, seen = 0, {}\n"
        "for i, name in ipairs(t) do n = i; seen[name] = true end\n"
        "assert(n == 2, 'expected exactly 2 entries, got ' .. n)\n"
        "assert(seen['tlist_a'], 'missing tlist_a')\n"
        "assert(seen['tlist_b'], 'missing tlist_b')\n"
    ));

    // GetOptionList marshals the real 174-row OptionsTable the same way
    EXPECT_TRUE(run(L,
        "local o = world.GetOptionList()\n"
        "assert(type(o) == 'table', 'expected a table, got ' .. type(o))\n"
        "assert(#o > 100, 'expected >100 options, got ' .. #o)\n"
    ));

    lua_close(L);
}

TEST(LuaScripting, GetDefaultValueNumeric)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.GetDefaultValue('max_output_lines')");
    EXPECT_EQ(1, n);
    EXPECT_EQ(5000.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.GetDefaultValue('port')");
    EXPECT_EQ(1, n);
    EXPECT_EQ(4000.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, GetDefaultValueAlpha)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // "script_language" defaults to "lua"
    int n = run_ret(L, "return world.GetDefaultValue('script_language')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("lua", lua_tostring(L, -1));
    lua_settop(L, 0);

    // "site" defaults to empty string
    n = run_ret(L, "return world.GetDefaultValue('site')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, GetDefaultValueUnknown)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.GetDefaultValue('no_such_option')");
    EXPECT_EQ(1, n);
    EXPECT_TRUE(lua_isnil(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// ── GetOption / SetOption full-table coverage ─────────────────────────────

TEST(LuaScripting, GetOptionLiveMember)
{
    // Verify that GetOption reads the real CMUSHclientDoc member, not just a default.
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // enable_triggers starts true; SetOption should propagate to the real field.
    int n = run_ret(L, "return world.GetOption('enable_triggers')");
    EXPECT_EQ(1, n);
    EXPECT_EQ(1.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    EXPECT_TRUE(run(L, "world.SetOption('enable_triggers', 0)"));
    EXPECT_FALSE(doc.m_enable_triggers);       // real field updated

    n = run_ret(L, "return world.GetOption('enable_triggers')");
    EXPECT_EQ(1, n);
    EXPECT_EQ(0.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, GetOptionRoundTrip)
{
    // max_output_lines round-trips through the REAL OptionsTable machinery
    // (offset write into m_maxlines; the old option cache is gone).
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // Default value applied by the ctor's SetDefaults before any Set.
    int n = run_ret(L, "return world.GetOption('max_output_lines')");
    EXPECT_EQ(1, n);
    EXPECT_EQ(5000.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // Set a custom value and read it back through the real member.
    EXPECT_TRUE(run(L, "world.SetOption('max_output_lines', 2000)"));
    EXPECT_EQ(2000L, (long)doc.m_maxlines);   // offset write hit the member
    n = run_ret(L, "return world.GetOption('max_output_lines')");
    EXPECT_EQ(1, n);
    EXPECT_EQ(2000.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, SetOptionUnknownRejectsError)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.SetOption('no_such_numeric_option', 42)");
    EXPECT_EQ(1, n);
    EXPECT_NE(0.0, lua_tonumber(L, -1));   // non-zero = error code
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, GetAlphaOptionLiveMember)
{
    // GetAlphaOption should read from the real CMUSHclientDoc member.
    CMUSHclientDoc doc;
    doc.m_mush_name = "TestWorld";
    doc.m_server    = "mud.example.com";

    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.GetAlphaOption('name')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("TestWorld", lua_tostring(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.GetAlphaOption('site')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("mud.example.com", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, SetAlphaOptionRoundTrip)
{
    // An alpha option round-trips through the REAL AlphaOptionsTable
    // machinery (offset write into m_strLanguage; the old cache is gone).
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // Default from AlphaOptionsTable (applied by the ctor's SetAlphaDefaults).
    int n = run_ret(L, "return world.GetAlphaOption('script_language')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("lua", lua_tostring(L, -1));
    lua_settop(L, 0);

    // Overwrite via SetAlphaOption — the real member is updated.
    EXPECT_TRUE(run(L, "world.SetAlphaOption('script_language', 'python')"));
    EXPECT_STREQ("python", (const char*)doc.m_strLanguage);

    n = run_ret(L, "return world.GetAlphaOption('script_language')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("python", lua_tostring(L, -1));
    lua_settop(L, 0);

    // Completely unknown alpha option.
    n = run_ret(L, "return world.SetAlphaOption('no_such_alpha_opt', 'x')");
    EXPECT_EQ(1, n);
    EXPECT_NE(0.0, lua_tonumber(L, -1));   // non-zero = error code
    lua_settop(L, 0);

    lua_close(L);
}

// ── Array subsystem ───────────────────────────────────────────────────────

TEST(LuaScripting, ArrayCreateSetGet)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "world.ArrayCreate('myarr')\n"
        "world.ArraySet('myarr', 'key1', 'value1')\n"
        "world.ArraySet('myarr', 'key2', 'value2')\n"
    ));

    EXPECT_EQ(1, run_ret(L, "return world.ArrayExists('myarr')"));
    EXPECT_TRUE(lua_toboolean(L, -1));
    lua_settop(L, 0);

    EXPECT_EQ(1, run_ret(L, "return world.ArrayGet('myarr', 'key1')"));
    EXPECT_STREQ("value1", lua_tostring(L, -1));
    lua_settop(L, 0);

    // ArraySize(name) is the element count of one array; ArrayCount() (no arg)
    // is the number of arrays — use ArraySize here to count myarr's two keys.
    EXPECT_EQ(1, run_ret(L, "return world.ArraySize('myarr')"));
    EXPECT_EQ(2.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, ArrayListAll)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "world.ArrayCreate('arr2')\n"
        "world.ArraySet('arr2', 'a', '1')\n"
        "world.ArraySet('arr2', 'b', '2')\n"
        "world.ArraySet('arr2', 'c', '3')\n"
    ));

    int n = run_ret(L, "return world.ArrayList('arr2')");
    EXPECT_EQ(1, n);
    ASSERT_TRUE(lua_istable(L, -1));

    // Should have keys "a", "b", "c" with values "1", "2", "3"
    lua_getfield(L, -1, "b");
    EXPECT_STREQ("2", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, ArrayDeleteAndExistence)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.ArrayCreate('tmp')"));

    int n = run_ret(L, "return world.ArrayExists('tmp')");
    EXPECT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(L, -1));
    lua_settop(L, 0);

    EXPECT_TRUE(run(L, "world.ArrayDelete('tmp')"));

    n = run_ret(L, "return world.ArrayExists('tmp')");
    EXPECT_EQ(1, n);
    EXPECT_FALSE(lua_toboolean(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// ── String utilities ──────────────────────────────────────────────────────

TEST(LuaScripting, Replace)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.Replace('hello world', 'world', 'Lua')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("hello Lua", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// Locks the methods_info/methods_utilities adoption: the Lua bindings now
// route through the REAL CMUSHclientDoc methods with lua_methods.cpp
// marshalling (GetInfo via pushVariant incl. the VT_DATE→local-SYSTEMTIME→
// CTime seconds round-trip, Windows argument defaults).
TEST(LuaScripting, RealInfoFamilyWiring)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // GetInfo(72) is the real switch (methods_info.cpp:533) — identical to
    // world.Version() (methods_info.cpp:85-90).
    int n = run_ret(L, "return world.GetInfo(72) == world.Version()");
    EXPECT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(L, -1));
    lua_settop(L, 0);

    // Date info types marshal as Unix seconds (Windows pushVariant VT_DATE →
    // COleDateTime → CTime::GetTime), not nil: GetInfo(304) = time now.
    n = run_ret(L, "return world.GetInfo(304)");
    EXPECT_EQ(1, n);
    ASSERT_TRUE(lua_isnumber(L, -1));
    EXPECT_GT(lua_tonumber(L, -1), 1.0e9);
    lua_settop(L, 0);

    // Unassigned info types → VT_NULL → nil.  141 was a twin-only alias for
    // 201; Windows GetInfo(141) hits the default case (methods_info.cpp:1168).
    n = run_ret(L, "return world.GetInfo(141)");
    EXPECT_EQ(1, n);
    EXPECT_TRUE(lua_isnil(L, -1));
    lua_settop(L, 0);

    // GetLineCount is the real method — lines received, m_total_lines
    // (methods_info.cpp:57-60; the ctor seeds line 1).  The old twin
    // returned m_newlines_received (0 here).
    n = run_ret(L, "return world.GetLineCount()");
    EXPECT_EQ(1, n);
    EXPECT_EQ(1.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // Trim through the real method (methods_utilities.cpp:168-175).
    n = run_ret(L, "return world.Trim('  padded  ')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("padded", lua_tostring(L, -1));
    lua_settop(L, 0);

    // Replace's Multiple parameter defaults TRUE as on Windows
    // (lua_methods.cpp L_Replace: optboolean(L, 4, 1)); the old twin
    // defaulted it false and only replaced the first occurrence.
    n = run_ret(L, "return world.Replace('aaa', 'a', 'b')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("bbb", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, EditDistance)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // Identical strings → 0
    int n = run_ret(L, "return world.EditDistance('abc', 'abc')");
    EXPECT_EQ(1, n);
    EXPECT_EQ(0.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // Single insertion
    n = run_ret(L, "return world.EditDistance('abc', 'abcd')");
    EXPECT_EQ(1, n);
    EXPECT_EQ(1.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, TranslateGerman)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // ü (0xFC) → "ue"
    int n = run_ret(L, "return world.TranslateGerman('M\xFCnchen')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("Muenchen", lua_tostring(L, -1));
    lua_settop(L, 0);

    // ß (0xDF) → "ss"
    n = run_ret(L, "return world.TranslateGerman('Stra\xDF" "e')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("Strasse", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// ── ExpandVariables ───────────────────────────────────────────────────────

TEST(LuaScripting, ExpandVariables)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "world.SetVariable('direction', 'north')\n"
        "world.SetVariable('dest', 'forest')\n"
    ));

    int n = run_ret(L, "return world.ExpandVariables('go @direction to @dest')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("go north to forest", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// ── Hashing functions ─────────────────────────────────────────────────────
// world.MD5/world.SHA256 were port-only inventions (never Windows Lua names);
// the canonical surface is utils.md5/utils.sha256 (lua_compress.c, opened by
// OpenLuaDelayed on both platforms).  They return RAW digest bytes
// (lua_pushlstring of the digest); utils.tohex renders uppercase hex
// (lua_compress.c:261 "%02X") — lowercase it to compare against the
// canonical test vectors.

TEST(LuaScripting, MD5)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return utils.tohex(utils.md5('hello')):lower()");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("5d41402abc4b2a76b9719d911017c592", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, SHA256)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return utils.tohex(utils.sha256('hello')):lower()");
    EXPECT_EQ(1, n);
    // SHA-256 of "hello"
    EXPECT_STREQ("2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
                 lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// ── World properties ──────────────────────────────────────────────────────

TEST(LuaScripting, WorldName)
{
    CMUSHclientDoc doc;
    doc.m_mush_name = "TestWorld";
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.WorldName()");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("TestWorld", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, WorldAddress)
{
    CMUSHclientDoc doc;
    doc.m_server = "mud.example.com";
    doc.m_port   = 4242;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.WorldAddress()");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("mud.example.com", lua_tostring(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.WorldPort()");
    EXPECT_EQ(1, n);
    EXPECT_EQ(4242.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// ── Misc API ──────────────────────────────────────────────────────────────

TEST(LuaScripting, GetWorldList)
{
    CMUSHclientDoc doc;
    doc.m_mush_name = "Solo";
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.GetWorldList()");
    EXPECT_EQ(1, n);
    ASSERT_TRUE(lua_istable(L, -1));
    lua_rawgeti(L, -1, 1);
    EXPECT_STREQ("Solo", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(LuaScripting, StripANSI)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.StripANSI('\x1b[31mhello\x1b[0m')");
    EXPECT_EQ(1, n);
    EXPECT_STREQ("hello", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────

// methods_triggers.cpp AddTrigger: "cannot have null match text".
TEST(LuaScripting, AddTriggerEmptyMatchTextRejected)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.AddTrigger('t1', '', 'resp', 0, -1, 0, '', '')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eTriggerCannotBeEmpty, lua_tonumber(L, -1));
    lua_settop(L, 0);
    EXPECT_EQ(0, (int)doc.m_TriggerMap.GetCount());   // nothing added

    lua_close(L);
}

// methods_triggers.cpp AddTriggerEx: "check sequence (0..10000)".
TEST(LuaScripting, AddTriggerExSequenceOutOfRangeRejected)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L,
        "return world.AddTriggerEx('t1','match','resp',0,-1,0,'','',0,10001)");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eTriggerSequenceOutOfRange, lua_tonumber(L, -1));
    lua_settop(L, 0);

    n = run_ret(L,
        "return world.AddTriggerEx('t1','match','resp',0,-1,0,'','',0,-1)");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eTriggerSequenceOutOfRange, lua_tonumber(L, -1));
    lua_settop(L, 0);
    EXPECT_EQ(0, (int)doc.m_TriggerMap.GetCount());

    lua_close(L);
}

// methods_triggers.cpp SetTriggerOption: "for boolean options, accept 'y'
// or 'n'" (same rule as '1'/'0').
TEST(LuaScripting, BooleanTriggerOptionAcceptsYAndN)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.AddTrigger('t','match','',0,-1,0,'','')"));
    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.m_TriggerMap.Lookup("t", t));
    t->bEnabled = FALSE;

    EXPECT_TRUE(run(L, "world.SetTriggerOption('t','enabled','y')"));
    EXPECT_TRUE(t->bEnabled != 0);
    EXPECT_TRUE(run(L, "world.SetTriggerOption('t','enabled','n')"));
    EXPECT_TRUE(t->bEnabled == 0);

    lua_close(L);
}

// lua_methods.cpp get_option_value: "if boolean convert to 0 or 1 ... nil is
// considered false" — SetTriggerOption accepts a Lua boolean or nil.
TEST(LuaScripting, SetTriggerOptionCoercesBooleanAndNilValues)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.AddTrigger('t','match','',0,-1,0,'','')"));
    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.m_TriggerMap.Lookup("t", t));

    EXPECT_TRUE(run(L, "world.SetTriggerOption('t','enabled',true)"));
    EXPECT_TRUE(t->bEnabled != 0);
    EXPECT_TRUE(run(L, "world.SetTriggerOption('t','enabled',false)"));
    EXPECT_TRUE(t->bEnabled == 0);
    EXPECT_TRUE(run(L, "world.SetTriggerOption('t','enabled',true)"));
    EXPECT_TRUE(run(L, "world.SetTriggerOption('t','enabled',nil)"));  // nil = false
    EXPECT_TRUE(t->bEnabled == 0);

    lua_close(L);
}

// doc_shared.cpp:4277: "for custom colours, add 1 when we get it (-1 = no
// change (becomes 0)) — however -1 was really 65535 so adding one made
// 65536, fix that up!" — GetTriggerOption('custom_colour') on a SAMECOLOUR
// trigger reads 0, never 65536.
TEST(LuaScripting, TriggerNoChangeColourReadsAsZeroNot65536)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // colour -1 = "no change" (stored internally as SAMECOLOUR = 65535)
    EXPECT_TRUE(run(L, "world.AddTrigger('t','match','',0,-1,0,'','')"));
    int n = run_ret(L, "return world.GetTriggerOption('t','custom_colour')");
    ASSERT_EQ(1, n);
    EXPECT_EQ(0.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// scriptingoptions.cpp OPT_WORLD_ID: "make sure hex characters" — the id
// option requires exactly 24 hex chars and stores them lowercased.
TEST(LuaScripting, WorldIdOptionValidatesHexAndLowercases)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.SetAlphaOption('id', 'abc')");  // too short
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eOptionOutOfRange, lua_tonumber(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.SetAlphaOption('id', 'ZZZZZZZZZZZZZZZZZZZZZZZZ')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eOptionOutOfRange, lua_tonumber(L, -1));  // non-hex
    lua_settop(L, 0);

    n = run_ret(L, "return world.SetAlphaOption('id', 'ABCDEF0123456789ABCDEF01')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eOK, lua_tonumber(L, -1));
    lua_settop(L, 0);
    EXPECT_STREQ("abcdef0123456789abcdef01", (const char*)doc.m_strWorldID);

    lua_close(L);
}

// doc_shared.cpp:4282: "for booleans - make sure in range 0 to 1" — SetOption
// rejects 2, and GetOption normalises a directly-poked member to 1.
TEST(LuaScripting, BooleanOptionRangeIsZeroToOne)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.SetOption('enable_triggers', 2)");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eOptionOutOfRange, lua_tonumber(L, -1));
    lua_settop(L, 0);

    doc.m_enable_triggers = 5;               // directly poke an out-of-range value
    n = run_ret(L, "return world.GetOption('enable_triggers')");
    ASSERT_EQ(1, n);
    EXPECT_EQ(1.0, lua_tonumber(L, -1));     // normalised to 1
    lua_settop(L, 0);

    lua_close(L);
}

// lua_methods.cpp L_SetOption: "if boolean convert to 0 or 1" — SetOption
// accepts a Lua boolean/nil for the numeric value.
TEST(LuaScripting, SetOptionCoercesBooleanToOneOrZero)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.SetOption('enable_triggers', false)"));
    EXPECT_TRUE(doc.m_enable_triggers == 0);
    EXPECT_TRUE(run(L, "world.SetOption('enable_triggers', true)"));
    EXPECT_TRUE(doc.m_enable_triggers == 1);
    EXPECT_TRUE(run(L, "world.SetOption('enable_triggers', nil)"));   // nil = 0
    EXPECT_TRUE(doc.m_enable_triggers == 0);

    lua_close(L);
}

// doc_shared.cpp SetBaseAlphaOptionItem: "ensure they don't slip multi lines
// into single-line options" — newlines stripped unless OPT_MULTLINE.
TEST(LuaScripting, SingleLineAlphaOptionStripsNewlines)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.SetAlphaOption('player', 'a\\r\\nb')"));
    EXPECT_STREQ("ab", (const char*)doc.m_name);          // stripped

    EXPECT_TRUE(run(L, "world.SetAlphaOption('notes', 'a\\r\\nb')"));
    EXPECT_STREQ("a\r\nb", (const char*)doc.m_notes);     // multiline kept

    lua_close(L);
}

// doc_portable.cpp CheckObjectName: "Trim leading/trailing whitespace ...
// Lowercase the name" — and an invalid label is rejected.
TEST(LuaScripting, ObjectNamesTrimmedAndLowercasedAsMapKeys)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.SetVariable('  MyVar  ', 'vv')"));
    int n = run_ret(L, "return world.GetVariable('myvar')");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("vv", lua_tostring(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.SetVariable('1bad', 'x')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eInvalidObjectLabel, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// methods_variables.cpp GetVariable: "returns EMPTY variable if not there —
// returns NULL variable if a bad name" — two distinct VARIANT states.
TEST(VariableApi, GetVariableReturnsNullForBadNameEmptyForMissing)
{
    CMUSHclientDoc doc;

    VARIANT v = doc.GetVariable("1bad name");
    EXPECT_EQ((unsigned short)VT_NULL, v.vt);    // invalid name
    VariantClear(&v);

    v = doc.GetVariable("validbutmissing");
    EXPECT_EQ((unsigned short)VT_EMPTY, v.vt);   // valid but nonexistent
    VariantClear(&v);
}

// methods_sending.cpp: "cannot change what we are sending in OnPluginSent —
// if (m_bPluginProcessingSent) return eItemInUse" — the whole Send family.
TEST(SendApi, SendReturnsItemInUseWhilePluginProcessingSent)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_bPluginProcessingSent = true;

    EXPECT_EQ((long)eItemInUse, doc.Send("x"));
    EXPECT_EQ((long)eItemInUse, doc.SendImmediate("x"));
    EXPECT_EQ((long)eItemInUse, doc.SendNoEcho("x"));
    EXPECT_EQ((long)eItemInUse, doc.SendPush("x"));
    EXPECT_EQ((long)eItemInUse, doc.SendSpecial("x", FALSE, FALSE, FALSE, FALSE));
}

// utilities_shared.cpp decodeBase64 throw contract: world.Base64Decode wraps
// decode in try/catch and returns nil on invalid input.
TEST(LuaScripting, Base64DecodeInvalidReturnsNil)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.Base64Decode('Zm9vYmFy')");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("foobar", lua_tostring(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.Base64Decode('abc') == nil");   // not mod-4
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.Base64Decode('@@@@') == nil");  // bad chars
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// methods_utilities.cpp OpenBrowser: "don't let them slip in arbitrary OS
// commands" — only http://, https:// and mailto: pass.
TEST(LuaScripting, OpenBrowserRejectsNonWebSchemes)
{
    CMUSHclientDoc doc;

    EXPECT_EQ((long)eBadParameter, doc.OpenBrowser(""));
    EXPECT_EQ((long)eBadParameter, doc.OpenBrowser("file:///etc/passwd"));
    EXPECT_EQ((long)eBadParameter, doc.OpenBrowser("ftp://host/x"));
    EXPECT_EQ((long)eBadParameter, doc.OpenBrowser("rm -rf /"));
}

// methods_commands.cpp GetQueue: "drop echo flag" — queue entries are stored
// with a leading echo-flag byte that GetQueue strips.
TEST(LuaScripting, GetQueueStripsEchoFlagByte)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_iSpeedWalkDelay = 10;              // non-zero: commands are queued
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.Queue('look', true)"));
    EXPECT_TRUE(run(L, "world.Queue('north', false)"));
    int n = run_ret(L, "local q = world.GetQueue() return q[1], q[2]");
    ASSERT_EQ(2, n);
    EXPECT_STREQ("look",  lua_tostring(L, -2));   // no leading flag byte
    EXPECT_STREQ("north", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// methods_info.cpp GetLineInfo: 1-based, "returns EMPTY if line out of
// range" — nil in Lua for 0 and count+1, text for line 1.
TEST(LuaScripting, GetLineInfoOutOfRangeReturnsNil)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    run_ret(L, "world.Note('hello')");

    int n = run_ret(L, "return world.GetLineInfo(0, 1) == nil");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.GetLineInfo(world.GetLinesInBufferCount() + 1, 1) == nil");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.GetLineInfo(1, 1)");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("hello", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// methods_info.cpp GetLinesInBufferCount "returns actual number of lines in
// output buffer (not count of lines received)" — DeleteLines shrinks it
// while GetLineCount is unchanged.
TEST(LuaScripting, LinesInBufferCountExcludesDeletedLines)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    run_ret(L, "world.Note('a') world.Note('b') world.Note('c') world.Note('d')");
    long total_before  = doc.GetLineCount();
    long buffer_before = doc.GetLinesInBufferCount();

    run_ret(L, "world.DeleteLines(2)");
    EXPECT_EQ(buffer_before - 2, doc.GetLinesInBufferCount());
    EXPECT_EQ(total_before, doc.GetLineCount());   // total received unchanged

    lua_close(L);
}

// lua_utils_portable.cpp utils.utf8valid: "valid -> the string's length in
// code points; invalid -> (nil, offset-of-first-bad-byte)".
TEST(LuaScripting, Utf8ValidReturnsCodepointCountOrNilPlusOffset)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return utils.utf8valid('a\\195\\169b')");  // a é b
    ASSERT_EQ(1, n);
    EXPECT_EQ(3.0, lua_tonumber(L, -1));     // three code points
    lua_settop(L, 0);

    n = run_ret(L, "return utils.utf8valid('ab\\195x')");   // bad continuation
    ASSERT_EQ(2, n);
    EXPECT_TRUE(lua_isnil(L, -2));
    EXPECT_EQ(3.0, lua_tonumber(L, -1));     // 1-based offset of the bad byte
    lua_settop(L, 0);

    lua_close(L);
}

// lua_utils_portable.cpp utils.utf8sub: "substring by codepoint indices
// (1-based, negative = from end)" — clamps and never splits a sequence.
TEST(LuaScripting, Utf8SubNegativeIndicesCountFromEnd)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // "aébé" = a, C3 A9, b, C3 A9
    const char* setup = "s = 'a\\195\\169b\\195\\169'";
    EXPECT_TRUE(run(L, setup));

    int n = run_ret(L, "return utils.utf8sub(s, 2, 3)");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("\xC3\xA9""b", lua_tostring(L, -1));   // codepoints 2..3
    lua_settop(L, 0);

    n = run_ret(L, "return utils.utf8sub(s, -2)");      // last two codepoints
    ASSERT_EQ(1, n);
    EXPECT_STREQ("b\xC3\xA9", lua_tostring(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return utils.utf8sub(s, -100, 100)");  // clamped
    ASSERT_EQ(1, n);
    EXPECT_STREQ("a\xC3\xA9""b\xC3\xA9", lua_tostring(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return utils.utf8sub(s, 3, 2)");    // start > stop
    ASSERT_EQ(1, n);
    EXPECT_STREQ("", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// methods_arrays.cpp: "importing is tricky because of the blasted 'escaped
// delimiter'" — export/import round-trips keys and values containing the
// delimiter and backslashes.
TEST(LuaScripting, ArrayExportImportRoundTripPreservesEscapedDelimiters)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "world.ArrayCreate('a')\n"
        "world.ArraySet('a', 'k|ey', 'v|al')\n"
        "world.ArraySet('a', 'back\\\\slash', 'x\\\\y|z')\n"
        "world.ArraySet('a', 'plain', 'value')\n"
        "s = world.ArrayExport('a', '|')\n"
        "world.ArrayCreate('b')\n"
        "rc = world.ArrayImport('b', s, '|')\n"));

    int n = run_ret(L, "return rc, world.ArrayGet('b','k|ey'), "
                       "world.ArrayGet('b','back\\\\slash'), world.ArrayGet('b','plain')");
    ASSERT_EQ(4, n);
    EXPECT_EQ((double)eOK, lua_tonumber(L, -4));
    EXPECT_STREQ("v|al",  lua_tostring(L, -3));
    EXPECT_STREQ("x\\y|z", lua_tostring(L, -2));
    EXPECT_STREQ("value", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// methods_arrays.cpp: "delimiter had better be a single character, other
// than backslash — return eBadDelimiter".
TEST(LuaScripting, ArrayImportRejectsMultiCharOrBackslashDelimiter)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.ArrayCreate('a')"));
    int n = run_ret(L, "return world.ArrayImport('a', 'x', '||')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eBadDelimiter, lua_tonumber(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.ArrayImport('a', 'x', '\\\\')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eBadDelimiter, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // note: ArrayExport checks empty-array BEFORE the delimiter, so the
    // array must be non-empty for the delimiter check to be reached
    EXPECT_TRUE(run(L, "world.ArraySet('a','k','v')"));
    n = run_ret(L, "return world.ArrayExport('a', '||')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eBadDelimiter, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// methods_arrays.cpp ArrayExport: "empty array gives empty string" (whereas
// a missing array returns the numeric error eArrayDoesNotExist).
TEST(LuaScripting, ArrayExportEmptyArrayGivesEmptyStringNotError)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.ArrayCreate('empty')"));
    int n = run_ret(L, "return world.ArrayExport('empty', '|')");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_isstring(L, -1));
    EXPECT_STREQ("", lua_tostring(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.ArrayExport('missing', '|')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eArrayDoesNotExist, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// lua_methods.cpp doc(): "for safety, check that world still exists" — a
// world userdatum whose doc has been destroyed raises "world is no longer
// available" instead of dereferencing the dead pointer.
TEST(LuaScripting, StaleWorldUserdatumRaisesErrorNotCrash)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    CMUSHclientDoc* pOther = new CMUSHclientDoc();
    pOther->m_mush_name = "otherworld";

    int n = run_ret(L, "w = world.GetWorld('otherworld') return w ~= nil");
    ASSERT_EQ(1, n);
    ASSERT_TRUE(lua_toboolean(L, -1));       // found the other world
    lua_settop(L, 0);

    delete pOther;                           // destroy it out from under Lua

    n = run_ret(L, "local ok, err = pcall(function() return w:GetInfo(2) end) "
                   "return ok, err");
    ASSERT_EQ(2, n);
    EXPECT_FALSE(lua_toboolean(L, -2));      // errored, did not crash
    const char* err = lua_tostring(L, -1);
    ASSERT_TRUE(err != nullptr);
    EXPECT_TRUE(strstr(err, "world is no longer available") != nullptr);
    lua_settop(L, 0);

    lua_close(L);
}

// lua_scripting.cpp GetLuaDispid: "Version 3.75+ supports dotted functions
// (eg. string.gsub)" — dispid resolution reaches functions nested in tables.
TEST(LuaScripting, PluginSupportsResolvesDottedFunctionNames)
{
    CMUSHclientDoc doc;
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString(
        "helpers = { run = function() return 1 end }\n"), "<test>"));
    doc.m_ScriptEngine = engine;

    EXPECT_NE(DISPID_UNKNOWN, engine->GetDispid("helpers.run"));
    EXPECT_NE(DISPID_UNKNOWN, engine->GetDispid("string.gsub"));
    EXPECT_EQ(DISPID_UNKNOWN, engine->GetDispid("helpers.missing"));
    EXPECT_EQ(DISPID_UNKNOWN, engine->GetDispid("no.such.table"));
}


// ── Intent-comment tests, tail batch (candidate mining, 2026-07) ──────────

#include <utime.h>

// methods_triggers.cpp AddTriggerEx: "must have a label for 'send to label'
// (eSendToVariable)" — a blank trigger name (auto-assigned '*trigger...'
// internal name) cannot be used with send-to-variable.
TEST(LuaScripting, AddTriggerExSendToVariableRequiresLabel)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // SendTo 9 = eSendToVariable, empty trigger name
    int n = run_ret(L,
        "return world.AddTriggerEx('', 'match', 'resp', 0, -1, 0, '', '', 9, 100)");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eTriggerLabelNotSpecified, lua_tonumber(L, -1));
    lua_settop(L, 0);
    EXPECT_EQ(0, (int)doc.m_TriggerMap.GetCount());

    // control: with a label it works
    n = run_ret(L,
        "return world.AddTriggerEx('lbl', 'match', 'resp', 0, -1, 0, '', '', 9, 100)");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eOK, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// lua_utils_portable.cpp MakeTableItem(COleDateTime): the port renders the
// same instant as a local-time "%m/%d/%Y %I:%M:%S %p" string — the format
// the old portable GetLineInfo "time" locked in.
TEST(LuaScripting, GetLineInfoTimeUsesLockedLocalFormat)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    run_ret(L, "world.Note('hello')");
    int n = run_ret(L,
        "local t = world.GetLineInfo(1)\n"
        "return type(t), tostring(t.timestr)");
    ASSERT_EQ(2, n);
    EXPECT_STREQ("table", lua_tostring(L, -2));
    const char* time_str = lua_tostring(L, -1);
    ASSERT_TRUE(time_str != nullptr);
    lua_pushstring(L, time_str);
    lua_setglobal(L, "captured");
    lua_settop(L, 0);

    n = run_ret(L,
        "return string.match(captured,"
        " '^%d%d/%d%d/%d%d%d%d %d%d:%d%d:%d%d [AP]M$') ~= nil");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// lua_utils_portable.cpp utils.xmlread: "returns a nested Lua table (same
// structure as the Windows utils.xmlread)" — name/content/line plus
// attributes and nodes sub-tables; malformed input → (nil, message, line).
TEST(LuaScripting, XmlreadParsesNestedElementStructure)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L,
        "local t = utils.xmlread('<a b=\"1\"><c>text</c></a>')\n"
        "return t.name, t.nodes[1].name, t.nodes[1].nodes[1].name,\n"
        "       t.nodes[1].nodes[1].content, t.nodes[1].attributes.b");
    ASSERT_EQ(5, n);
    // the root element wraps the document; its first node is <a>
    EXPECT_STREQ("a",    lua_tostring(L, -4));
    EXPECT_STREQ("c",    lua_tostring(L, -3));
    EXPECT_STREQ("text", lua_tostring(L, -2));
    lua_settop(L, 0);

    n = run_ret(L, "local t, msg, line = utils.xmlread('<a><b></a>')\n"
                   "return t == nil, type(msg), type(line)");
    ASSERT_EQ(3, n);
    EXPECT_TRUE(lua_toboolean(L, -3));           // nil on malformed input
    EXPECT_STREQ("string", lua_tostring(L, -2)); // plus a message
    EXPECT_STREQ("number", lua_tostring(L, -1)); // and a line number
    lua_settop(L, 0);

    lua_close(L);
}

// doc_portable.cpp OnScriptFileChanged: "st.st_mtime == m_scriptFileMtime
// -> return, unchanged" (and the new mtime is recorded before Parse so a
// re-entrant call does not loop) --- the reload happens ONLY when the file's
// mtime differs, not when its content does.
TEST(LuaScripting, ScriptFileChangeReloadsOnlyWhenMtimeDiffers)
{
    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_test_script_%d.lua", (int)getpid());
    const time_t t0 = time(nullptr) - 100;

    auto write_script = [&](const char* body) {
        FILE* f = fopen(path, "wb");
        ASSERT_TRUE(f != nullptr);
        fputs(body, f);
        fclose(f);
        struct utimbuf tb; tb.actime = tb.modtime = t0;
        ASSERT_EQ(0, utime(path, &tb));          // pin the mtime
    };
    write_script("function fn1() end\n");

    CMUSHclientDoc doc;
    doc.m_bEnableScripts    = TRUE;
    doc.m_strLanguage       = "lua";
    doc.m_strScriptFilename = path;

    doc.OnScriptFileChanged(false);              // initial load (mtime 0 differs)
    ASSERT_TRUE(doc.m_ScriptEngine != nullptr);
    EXPECT_NE(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn1"));
    EXPECT_EQ(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn2"));

    // new CONTENT but the SAME mtime: must NOT reload (mtime is the trigger)
    write_script("function fn2() end\n");
    doc.OnScriptFileChanged(false);
    ASSERT_TRUE(doc.m_ScriptEngine != nullptr);
    EXPECT_NE(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn1"));  // old script
    EXPECT_EQ(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn2"));

    // bump the mtime: now it reloads and picks up the new content
    struct utimbuf tb; tb.actime = tb.modtime = t0 + 10;
    ASSERT_EQ(0, utime(path, &tb));
    doc.OnScriptFileChanged(false);
    ASSERT_TRUE(doc.m_ScriptEngine != nullptr);
    EXPECT_EQ(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn1"));
    EXPECT_NE(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn2"));

    // and a further call with the same (new) mtime is a no-op, not a loop
    doc.OnScriptFileChanged(false);
    EXPECT_NE(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn2"));
}

// ── world.Metaphone ──────────────────────────────────────────────────────
// CMUSHclientDoc::Metaphone joins primary+","+secondary; the BSTR→Lua
// conversion (pushBstr → CString(char*)) stops at the first NUL, which is
// exactly where SetAt(maxlength,'\0') truncates over-long codes — same
// visible behaviour as Windows (CString-from-BSTR also stops at NUL).

TEST(LuaScripting, MetaphoneJoinsPrimaryAndSecondaryWithComma)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    int n = run_ret(L, "return world.Metaphone('smith', 4)");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("SM0,XMT", lua_tostring(L, -1));
    lua_settop(L, 0);
    lua_close(L);
}

TEST(LuaScripting, MetaphoneOmitsCommaWhenNoAlternate)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    int n = run_ret(L, "return world.Metaphone('thomas', 4)");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("TMS", lua_tostring(L, -1));
    lua_settop(L, 0);
    lua_close(L);
}

TEST(LuaScripting, MetaphoneShortLengthStopsLoopAtLength)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    // catherine → K0RN/KTRN at length 4; at Length 2 the main loop exits as
    // soon as both codes reach 2 chars — no overshoot, so both codes survive.
    int n = run_ret(L, "return world.Metaphone('catherine', 2)");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("K0,KT", lua_tostring(L, -1));
    lua_settop(L, 0);
    lua_close(L);
}

TEST(LuaScripting, MetaphoneOvershootTruncatesAtEmbeddedNul)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    // filipowicz: the final WICZ adds two chars at once (TS/FX), overshooting
    // Length 4 → codes are "FLPTS"/"FLPFX" and SetAt(4,'\0') embeds a NUL.
    // The BSTR→Lua conversion stops at the first NUL, so the ",FLPF" tail is
    // swallowed — longstanding Windows behaviour, preserved by the port.
    int n = run_ret(L, "return world.Metaphone('filipowicz', 4)");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("FLPT", lua_tostring(L, -1));
    lua_settop(L, 0);
    lua_close(L);
}

TEST(LuaScripting, MetaphoneEmptyWordReturnsEmptyString)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    int n = run_ret(L, "return world.Metaphone('', 4)");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("", lua_tostring(L, -1));
    lua_settop(L, 0);
    lua_close(L);
}

// ── Timer/alias/trigger option families ──────────────────────────────────
// Each Get/SetXxxOption pair funnels through the per-class option tables at
// the top of methods_timers/aliases/triggers.cpp.  These tests round-trip
// every documented option and pin the error codes for the rejection arms.

// Run a chunk that ends in `return true`; any embedded assert() failure
// surfaces as a Lua error (run_ret returns -1).
#define EXPECT_LUA_OK(L, code) \
    do { \
        int n_ = run_ret((L), (code)); \
        EXPECT_EQ(1, n_); \
        if (n_ == 1) EXPECT_TRUE(lua_toboolean((L), -1)); \
        lua_settop((L), 0); \
    } while (0)

TEST(LuaScripting, TimerOptionsRoundTripEveryOption)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        assert(world.AddTimer("t1", 0, 0, 10, "hi", 1, "") == error_code.eOK)
        local num = {
          {"hour", 5}, {"minute", 6}, {"second", 7.5},
          {"offset_hour", 1}, {"offset_minute", 2}, {"offset_second", 3},
          {"send_to", 1}, {"user", 42},
          {"enabled", 1}, {"at_time", 1}, {"one_shot", 1},
          {"omit_from_output", 1}, {"omit_from_log", 1},
          {"active_closed", 1}, {"temporary", 1},
        }
        for _, o in ipairs(num) do
          assert(world.SetTimerOption("t1", o[1], o[2]) == error_code.eOK, o[1])
          local got = world.GetTimerOption("t1", o[1])
          assert(got == o[2], o[1] .. " got " .. tostring(got))
        end
        local alpha = { {"group", "grp"}, {"script", "myproc"},
                        {"send", "do this"}, {"variable", "myvar"} }
        for _, o in ipairs(alpha) do
          assert(world.SetTimerOption("t1", o[1], o[2]) == error_code.eOK, o[1])
          assert(world.GetTimerOption("t1", o[1]) == o[2], o[1])
        end
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, TimerOptionsAcceptYAndNForBooleans)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        assert(world.AddTimer("t1", 0, 0, 10, "hi", 0, "") == error_code.eOK)
        assert(world.SetTimerOption("t1", "enabled", "y") == error_code.eOK)
        assert(world.GetTimerOption("t1", "enabled") == 1)
        assert(world.SetTimerOption("t1", "enabled", "n") == error_code.eOK)
        assert(world.GetTimerOption("t1", "enabled") == 0)
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, TimerOptionRejectionArms)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        assert(world.AddTimer("t1", 0, 0, 10, "hi", 1, "") == error_code.eOK)
        assert(world.SetTimerOption("t1", "hour", 24)    == error_code.eOptionOutOfRange)
        -- only "second" is float-parsed; other numeric options are
        -- integer-only (IsNumber has no decimal support), so a fractional
        -- offset_second is rejected even though the field is a double
        assert(world.SetTimerOption("t1", "second", 7.5)         == error_code.eOK)
        assert(world.SetTimerOption("t1", "offset_second", 3.25) == error_code.eOptionOutOfRange)
        assert(world.SetTimerOption("t1", "second", 60)  == error_code.eOptionOutOfRange)
        assert(world.SetTimerOption("t1", "send_to", 99) == error_code.eOptionOutOfRange)
        assert(world.SetTimerOption("t1", "hour", "abc") == error_code.eOptionOutOfRange)
        assert(world.SetTimerOption("t1", "nosuch", 1)   == error_code.eUnknownOption)
        assert(world.SetTimerOption("zz", "hour", 1)     == error_code.eTimerNotFound)
        -- Get: unknown timer yields nil (VT_EMPTY); unknown option nil (VT_NULL)
        assert(world.GetTimerOption("zz", "hour") == nil)
        assert(world.GetTimerOption("t1", "nosuch") == nil)
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, TimerInfoSelectors)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        assert(world.AddTimer("t1", 1, 2, 3, "the send text", 1, "the_proc") == error_code.eOK)
        assert(world.SetTimerOption("t1", "group", "grp") == error_code.eOK)
        assert(world.SetTimerOption("t1", "user", 21)     == error_code.eOK)
        assert(world.SetTimerOption("t1", "send_to", 2)   == error_code.eOK)
        assert(world.GetTimerInfo("t1", 1) == 1)              -- hour (every)
        assert(world.GetTimerInfo("t1", 2) == 2)              -- minute
        assert(world.GetTimerInfo("t1", 3) == 3)              -- second
        assert(world.GetTimerInfo("t1", 4) == "the send text")
        assert(world.GetTimerInfo("t1", 5) == "the_proc")
        assert(world.GetTimerInfo("t1", 6) == true)           -- enabled
        assert(world.GetTimerInfo("t1", 7) == false)          -- one-shot
        assert(world.GetTimerInfo("t1", 8) == false)          -- at-time
        assert(world.GetTimerInfo("t1", 9) == 0)              -- invocations
        assert(world.GetTimerInfo("t1", 10) == 0)             -- times fired
        assert(world.GetTimerInfo("t1", 13) >= 0)             -- seconds until due
        assert(world.GetTimerInfo("t1", 14) == false)         -- temporary
        assert(world.GetTimerInfo("t1", 17) == false)         -- active when closed
        assert(world.GetTimerInfo("t1", 19) == "grp")
        assert(world.GetTimerInfo("t1", 20) == 2)             -- send-to
        assert(world.GetTimerInfo("t1", 21) == 21)            -- user option
        assert(world.GetTimerInfo("t1", 22) == "t1")          -- label
        assert(world.GetTimerInfo("t1", 26) == false)         -- script resolved
        assert(world.GetTimerInfo("t1", 99) == nil)           -- unknown selector
        assert(world.GetTimerInfo("zz", 1) == nil)            -- unknown timer
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, TimerGroupAndLifecycleOps)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        for i = 1, 3 do
          assert(world.AddTimer("g" .. i, 0, 0, 10, "x", 1, "") == error_code.eOK)
          assert(world.SetTimerOption("g" .. i, "group", "grp") == error_code.eOK)
        end
        assert(world.EnableTimerGroup("grp", 0) == 3)     -- 3 affected
        assert(world.GetTimerOption("g2", "enabled") == 0)
        assert(world.EnableTimer("g2", 1) == error_code.eOK)
        assert(world.EnableTimer("zz", 1) == error_code.eTimerNotFound)
        assert(world.ResetTimer("g1") == error_code.eOK)
        assert(world.ResetTimer("zz") == error_code.eTimerNotFound)
        assert(world.IsTimer("g1") == error_code.eOK)
        assert(world.IsTimer("zz") == error_code.eTimerNotFound)
        assert(world.DeleteTimerGroup("grp") == 3)
        assert(world.IsTimer("g1") == error_code.eTimerNotFound)
        -- temporaries
        assert(world.AddTimer("tmp1", 0, 0, 5, "x", 1, "") == error_code.eOK)
        assert(world.SetTimerOption("tmp1", "temporary", 1) == error_code.eOK)
        assert(world.DeleteTemporaryTimers() == 1)
        assert(world.IsTimer("tmp1") == error_code.eTimerNotFound)
        world.ResetTimers()   -- all-timers reset: just must not error
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, DoAfterFamilyCreatesTemporaryTimers)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        assert(world.DoAfter(5, "north") == error_code.eOK)
        assert(world.DoAfterNote(5, "a note") == error_code.eOK)
        assert(world.DoAfterSpeedWalk(5, "4n") == error_code.eOK)
        assert(world.DoAfterSpecial(5, "beep", 2) == error_code.eOK)
        -- all four are one-shot temporary timers
        assert(world.DeleteTemporaryTimers() == 4)
        -- rejection arms: bad sendto, bad seconds
        assert(world.DoAfterSpecial(5, "x", 99) ~= error_code.eOK)
        assert(world.DoAfterSpecial(0, "x", 2) == error_code.eTimeInvalid)
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, AliasOptionsRoundTripEveryOption)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        assert(world.AddAlias("a1", "matchme", "send this", 1, "") == error_code.eOK)
        local num = {
          {"send_to", 1}, {"sequence", 200}, {"user", -5},
          {"enabled", 1}, {"expand_variables", 1}, {"omit_from_log", 1},
          {"omit_from_command_history", 1}, {"omit_from_output", 1},
          {"menu", 1}, {"keep_evaluating", 1}, {"echo_alias", 1},
          {"temporary", 1}, {"one_shot", 1},
        }
        for _, o in ipairs(num) do
          assert(world.SetAliasOption("a1", o[1], o[2]) == error_code.eOK, o[1])
          local got = world.GetAliasOption("a1", o[1])
          assert(got == o[2], o[1] .. " got " .. tostring(got))
        end
        local alpha = { {"group", "grp"}, {"match", "newmatch"},
                        {"script", "proc"}, {"send", "text"}, {"variable", "vv"} }
        for _, o in ipairs(alpha) do
          assert(world.SetAliasOption("a1", o[1], o[2]) == error_code.eOK, o[1])
          assert(world.GetAliasOption("a1", o[1]) == o[2], o[1])
        end
        -- read-only options refuse writes but still read back
        assert(world.SetAliasOption("a1", "regexp", 1) == error_code.ePluginCannotSetOption)
        assert(world.SetAliasOption("a1", "ignore_case", 1) == error_code.ePluginCannotSetOption)
        assert(world.GetAliasOption("a1", "regexp") == 0)
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, AliasInfoSelectorsAndErrors)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        assert(world.AddAlias("a1", "matchme", "send this", 1, "the_proc") == error_code.eOK)
        assert(world.SetAliasOption("a1", "group", "grp") == error_code.eOK)
        assert(world.SetAliasOption("a1", "sequence", 321) == error_code.eOK)
        assert(world.GetAliasInfo("a1", 1) == "matchme")
        assert(world.GetAliasInfo("a1", 2) == "send this")
        assert(world.GetAliasInfo("a1", 3) == "the_proc")
        assert(world.GetAliasInfo("a1", 6) == true)          -- enabled
        assert(world.GetAliasInfo("a1", 7) == false)         -- regexp
        assert(world.GetAliasInfo("a1", 10) == 0)            -- invocation count
        assert(world.GetAliasInfo("a1", 16) == "grp")
        assert(world.GetAliasInfo("a1", 20) == 321)          -- sequence
        assert(world.GetAliasInfo("a1", 99) == nil)
        assert(world.GetAliasInfo("zz", 1) == nil)
        assert(world.IsAlias("a1") == error_code.eOK)
        assert(world.IsAlias("zz") == error_code.eAliasNotFound)
        assert(world.SetAliasOption("zz", "enabled", 1) == error_code.eAliasNotFound)
        assert(world.GetAliasOption("zz", "enabled") == nil)
        assert(world.SetAliasOption("a1", "nosuch", 1) == error_code.eUnknownOption)
        assert(world.SetAliasOption("a1", "sequence", 99999) == error_code.eOptionOutOfRange)
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, AliasGroupAndLifecycleOps)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        for i = 1, 2 do
          assert(world.AddAlias("g" .. i, "m" .. i, "s", 1, "") == error_code.eOK)
          assert(world.SetAliasOption("g" .. i, "group", "grp") == error_code.eOK)
        end
        assert(world.EnableAliasGroup("grp", 0) == 2)
        assert(world.GetAliasOption("g1", "enabled") == 0)
        assert(world.EnableAlias("g1", 1) == error_code.eOK)
        assert(world.EnableAlias("zz", 1) == error_code.eAliasNotFound)
        assert(world.DeleteAliasGroup("grp") == 2)
        assert(world.IsAlias("g1") == error_code.eAliasNotFound)
        assert(world.AddAlias("tmp", "m", "s", 1, "") == error_code.eOK)
        assert(world.SetAliasOption("tmp", "temporary", 1) == error_code.eOK)
        assert(world.DeleteTemporaryAliases() == 1)
        assert(world.DeleteAlias("nosuch") == error_code.eAliasNotFound)
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, TriggerOptionsRoundTripEveryOption)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        assert(world.AddTrigger("t1", "matchtext", "send", 1, -1, 0, "", "") == error_code.eOK)
        local num = {
          {"clipboard_arg", 3}, {"lines_to_match", 5},
          {"match_style", 31}, {"new_style", 3},
          {"other_text_colour", 0x123456}, {"other_back_colour", 0x654321},
          {"send_to", 1}, {"sequence", 200}, {"user", 7},
          {"enabled", 1}, {"expand_variables", 1}, {"keep_evaluating", 1},
          {"multi_line", 1}, {"omit_from_log", 1}, {"omit_from_output", 1},
          {"repeat", 1}, {"sound_if_inactive", 1}, {"lowercase_wildcard", 1},
          {"temporary", 1}, {"one_shot", 1},
        }
        for _, o in ipairs(num) do
          assert(world.SetTriggerOption("t1", o[1], o[2]) == error_code.eOK, o[1])
          local got = world.GetTriggerOption("t1", o[1])
          assert(got == o[2], o[1] .. " got " .. tostring(got))
        end
        local alpha = { {"group", "grp"}, {"match", "newmatch"}, {"script", "proc"},
                        {"sound", "ding.wav"}, {"send", "text"}, {"variable", "vv"} }
        for _, o in ipairs(alpha) do
          assert(world.SetTriggerOption("t1", o[1], o[2]) == error_code.eOK, o[1])
          assert(world.GetTriggerOption("t1", o[1]) == o[2], o[1])
        end
        -- read-only for everyone: regexp, ignore_case
        assert(world.SetTriggerOption("t1", "regexp", 1) == error_code.ePluginCannotSetOption)
        assert(world.SetTriggerOption("t1", "ignore_case", 1) == error_code.ePluginCannotSetOption)
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, TriggerInfoSelectorsAndErrors)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        assert(world.AddTrigger("t1", "matchtext", "send text", 1, -1, 0, "", "the_proc")
               == error_code.eOK)
        assert(world.SetTriggerOption("t1", "group", "grp") == error_code.eOK)
        assert(world.SetTriggerOption("t1", "sequence", 456) == error_code.eOK)
        assert(world.SetTriggerOption("t1", "user", 13) == error_code.eOK)
        assert(world.GetTriggerInfo("t1", 1) == "matchtext")
        assert(world.GetTriggerInfo("t1", 2) == "send text")
        assert(world.GetTriggerInfo("t1", 4) == "the_proc")
        assert(world.GetTriggerInfo("t1", 8) == true)          -- enabled
        assert(world.GetTriggerInfo("t1", 9) == false)         -- regexp
        assert(world.GetTriggerInfo("t1", 16) == 456)          -- sequence
        assert(world.GetTriggerInfo("t1", 20) == 0)            -- invocation count
        assert(world.GetTriggerInfo("t1", 23) == false)        -- temporary
        assert(world.GetTriggerInfo("t1", 26) == "grp")
        assert(world.GetTriggerInfo("t1", 28) == 13)           -- user option
        assert(world.GetTriggerInfo("t1", 99) == nil)
        assert(world.GetTriggerInfo("zz", 1) == nil)
        assert(world.IsTrigger("t1") == error_code.eOK)
        assert(world.IsTrigger("zz") == error_code.eTriggerNotFound)
        assert(world.SetTriggerOption("zz", "enabled", 1) == error_code.eTriggerNotFound)
        assert(world.GetTriggerOption("zz", "enabled") == nil)
        assert(world.SetTriggerOption("t1", "nosuch", 1) == error_code.eUnknownOption)
        assert(world.SetTriggerOption("t1", "sequence", -1) == error_code.eOptionOutOfRange)
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, TriggerGroupAndLifecycleOps)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        for i = 1, 2 do
          assert(world.AddTrigger("g" .. i, "m" .. i, "s", 1, -1, 0, "", "")
                 == error_code.eOK)
          assert(world.SetTriggerOption("g" .. i, "group", "grp") == error_code.eOK)
        end
        assert(world.EnableTriggerGroup("grp", 0) == 2)
        assert(world.GetTriggerOption("g1", "enabled") == 0)
        assert(world.EnableTrigger("g1", 1) == error_code.eOK)
        assert(world.EnableTrigger("zz", 1) == error_code.eTriggerNotFound)
        assert(world.DeleteTriggerGroup("grp") == 2)
        assert(world.IsTrigger("g1") == error_code.eTriggerNotFound)
        assert(world.AddTrigger("tmp", "m", "s", 1, -1, 0, "", "") == error_code.eOK)
        assert(world.SetTriggerOption("tmp", "temporary", 1) == error_code.eOK)
        assert(world.DeleteTemporaryTriggers() == 1)
        assert(world.DeleteTrigger("nosuch") == error_code.eTriggerNotFound)
        return true
    )lua");
    lua_close(L);
}

// ── Colour management methods (methods_colours.cpp) ──────────────────────

TEST(LuaScripting, ColourNameAndRgbConversions)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        assert(world.ColourNameToRGB("red") == 255)          -- COLORREF 0x0000FF
        assert(world.ColourNameToRGB("#102030") == 0x302010) -- byte-swapped to COLORREF
        assert(world.ColourNameToRGB("nosuchcolour") == -1)
        assert(world.RGBColourToName(255) == "red")
        -- an unnamed colour comes back as #RRGGBB (uppercase hex)
        assert(world.RGBColourToName(0x0102A3) == "#A30201")
        -- ANSI() builds the raw escape sequence
        assert(world.ANSI(31) == string.char(27) .. "[31m")
        -- AdjustColour method 1 = invert
        assert(world.AdjustColour(0x000000, 1) == 0xFFFFFF)
        assert(world.AdjustColour(0x123456, 0) == 0x123456)  -- 0 = no change
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, PaletteAccessorsRoundTripAndRejectOutOfRange)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        world.SetNormalColour(3, 0xFF123456)      -- masked to 24 bits
        assert(world.GetNormalColour(3) == 0x123456)
        world.SetBoldColour(8, 0x0000FF)
        assert(world.GetBoldColour(8) == 0x0000FF)
        world.SetCustomColourText(16, 0x111111)
        world.SetCustomColourBackground(16, 0x222222)
        assert(world.GetCustomColourText(16) == 0x111111)
        assert(world.GetCustomColourBackground(16) == 0x222222)
        -- out-of-range indexes read as 0 and ignore writes
        assert(world.GetNormalColour(0) == 0)
        assert(world.GetNormalColour(9) == 0)
        world.SetNormalColour(9, 0x777777)        -- must not corrupt anything
        assert(world.GetCustomColourText(17) == 0)
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, CustomColourNames)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        assert(world.SetCustomColourName(2, "MyColour") == error_code.eOK)
        assert(world.GetCustomColourName(2) == "MyColour")
        assert(world.SetCustomColourName(0, "x")  == error_code.eOptionOutOfRange)
        assert(world.SetCustomColourName(17, "x") == error_code.eOptionOutOfRange)
        assert(world.SetCustomColourName(2, "")   == error_code.eNoNameSpecified)
        assert(world.SetCustomColourName(2, string.rep("x", 31))
               == error_code.eInvalidObjectLabel)
        assert(world.GetCustomColourName(0) == "")   -- out of range reads empty
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, MapColourTranslation)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        -- unmapped colours translate to themselves
        assert(world.GetMapColour(0x123456) == 0x123456)
        world.MapColour(0x123456, 0x654321)
        assert(world.GetMapColour(0x123456) == 0x654321)
        -- the Lua glue returns a keyed map {original = replacement}
        -- (not an array like the COM-dispatch variant)
        local t = world.MapColourList()
        assert(type(t) == "table")
        assert(t[0x123456] == 0x654321)
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, PickColourIsHeadlessNoop)
{
    // no GUI hook installed headless → always -1 (cancelled)
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, "return world.PickColour(255) == -1");
    lua_close(L);
}

// ── Mapper methods (methods_mapping.cpp) ─────────────────────────────────

TEST(LuaScripting, MapperAddListAndDelete)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        assert(world.GetMappingCount() == 0)
        assert(world.AddToMapper("n", "s") == error_code.eOK)
        assert(world.AddToMapper("e", "w") == error_code.eOK)
        assert(world.AddMapperComment("the shop") == error_code.eOK)
        assert(world.GetMappingCount() == 3)
        -- items are stored as dir/reverse and {comment}
        assert(world.GetMappingItem(0) == "n/s")
        assert(world.GetMappingItem(2) == "{the shop}")
        assert(world.GetMappingItem(3) == nil)     -- out of range
        assert(world.GetMappingItem(-1) == nil)
        assert(world.DeleteLastMapItem() == error_code.eOK)
        assert(world.GetMappingCount() == 2)
        assert(world.DeleteAllMapItems() == error_code.eOK)
        assert(world.GetMappingCount() == 0)
        assert(world.DeleteLastMapItem() == error_code.eNoMapItems)
        assert(world.DeleteAllMapItems() == error_code.eNoMapItems)
        return true
    )lua");
    lua_close(L);
}

TEST(LuaScripting, MapperRejectsSpecialCharactersAndEmptyPairs)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        assert(world.AddToMapper("n{", "s")  == error_code.eBadMapItem)
        assert(world.AddToMapper("n", "s\\") == error_code.eBadMapItem)
        assert(world.AddToMapper("", "")     == error_code.eBadMapItem)
        assert(world.AddMapperComment("a/b") == error_code.eBadMapItem)
        assert(world.GetMappingCount() == 0)
        return true
    )lua");
    lua_close(L);
}

// ── Mersenne Twister (world.MtSrand / world.MtRand → mt19937ar.c) ────────

TEST(LuaScripting, MtRandIsSeededAndDeterministic)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_OK(L, R"lua(
        world.MtSrand(12345)
        local a1, a2, a3 = world.MtRand(), world.MtRand(), world.MtRand()
        world.MtSrand(12345)
        local b1, b2, b3 = world.MtRand(), world.MtRand(), world.MtRand()
        assert(a1 == b1 and a2 == b2 and a3 == b3)      -- same seed, same stream
        for _, v in ipairs({a1, a2, a3}) do
          assert(v >= 0 and v < 1)                       -- [0, 1) reals
        end
        world.MtSrand(54321)
        assert(world.MtRand() ~= a1)                     -- different seed diverges
        -- array-seeded form (init_by_array)
        world.MtSrand({1, 2, 3})
        local c1 = world.MtRand()
        world.MtSrand({1, 2, 3})
        assert(world.MtRand() == c1)
        return true
    )lua");
    lua_close(L);
}

MC_TEST_MAIN()

#else  // !MUSHCLIENT_LUA_SCRIPTING

// Without Lua headers the tests can't run — emit a trivial main that passes.
#include <cstdio>
int main() {
    printf("Lua scripting not available — skipping test_lua_scripting\n");
    return 0;
}

#endif // MUSHCLIENT_LUA_SCRIPTING
