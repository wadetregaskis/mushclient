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
// KNOWN PORTABLE-SUBSET FAILURES (deferred — low priority):
// Several tests below (AddAndGetTrigger, DeleteTrigger, EnableDisableTrigger,
// AddAndGetAlias, AddAndGetTimer, GuiStubsReturnValues,
// SetAlphaOptionRoundTrip, ArrayCreateSetGet, MD5, SHA256, WorldAddress)
// assert the FULL world API.  lua_world_api_portable.cpp (used by mushcore /
// these tests) implements only a subset; the product, mushgui, uses
// the complete scripting/lua_methods.cpp where these work.  Bringing the
// portable layer to parity is deferred — the GUI is the product.

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

static lua_State* make_lua(CMUSHclientDoc* pDoc)
{
    lua_State* L = MakeLuaState();
    if (!L) return nullptr;
    luaL_openlibs(L);
    // Bind the doc pointer into the Lua registry (mirrors OpenLuaDelayed)
    lua_pushlightuserdata(L, static_cast<void*>(pDoc));
    lua_setfield(L, LUA_REGISTRYINDEX, DOCUMENT_STATE);
    RegisterLuaRoutines(L);
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

    // AddTrigger(name, match, send, flags, sendto, group, sounds, script, sequence)
    int n = run_ret(L,
        "return world.AddTrigger('trig1', 'you have died', 'say I died!', 0, 0)"
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

    EXPECT_TRUE(run(L, "world.AddTrigger('t1', 'pat', 'resp', 0, 0)"));
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

    EXPECT_TRUE(run(L, "world.AddTrigger('t2', 'match', 'send', 1, 0)"));  // flags=1=Enabled

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

    EXPECT_TRUE(run(L, "world.AddTrigger('t3', 'pat', 'resp', 0, 0)"));

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

    // WindowCreate → eNoSuchWindow (30073)
    int n = run_ret(L, "return world.WindowCreate('w','',0,0,100,100,0,0)");
    EXPECT_EQ(1, n);
    EXPECT_EQ(30073.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // WindowList → empty table (not nil)
    n = run_ret(L, "return world.WindowList()");
    EXPECT_EQ(1, n);
    EXPECT_TRUE(lua_istable(L, -1));
    lua_settop(L, 0);

    // ChatCall → eNoChatConnections (30044)
    n = run_ret(L, "return world.ChatCall('host', 4050)");
    EXPECT_EQ(1, n);
    EXPECT_EQ(30044.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // SpellCheck → eSpellCheckNotActive (30064)
    n = run_ret(L, "return world.SpellCheck('hello')");
    EXPECT_EQ(1, n);
    EXPECT_EQ(30064.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // ActivateNotepad on a title that doesn't exist → eNoSuchWindow (30073),
    // exactly as on Windows (no 'test' notepad was created here).
    n = run_ret(L, "return world.ActivateNotepad('test')");
    EXPECT_EQ(1, n);
    EXPECT_EQ(30073.0, lua_tonumber(L, -1));
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

TEST(LuaScripting, MD5)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.MD5('hello')");
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

    int n = run_ret(L, "return world.SHA256('hello')");
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

MC_TEST_MAIN()

#else  // !MUSHCLIENT_LUA_SCRIPTING

// Without Lua headers the tests can't run — emit a trivial main that passes.
#include <cstdio>
int main() {
    printf("Lua scripting not available — skipping test_lua_scripting\n");
    return 0;
}

#endif // MUSHCLIENT_LUA_SCRIPTING
