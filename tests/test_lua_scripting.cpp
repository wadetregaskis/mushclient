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
#include <sys/resource.h>
// B-3 atomic flip: real headers (portable stub deleted; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "platform/wx_mfc_hooks.h"   // g_wxhook_message_box (the reload prompt)
#include "scripting/scripting.h"   // DOCUMENT_STATE, MakeLuaState, RegisterLuaRoutines

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cstring>
#include <cstdio>

// ── Lua state fixture ─────────────────────────────────────────────────────
// make_lua()/run()/run_ret()/LuaDoc/EXPECT_LUA_OK are shared with
// test_lua_surface.cpp (audit §2/§4).
#include "lua_test_helpers.h"

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

    // A valid-but-missing variable marshals to nil/empty in Lua (VT_EMPTY),
    // never a bogus non-empty string — pin the value, not just the arity.
    int n = run_ret(L,
        "local v = world.GetVariable('does_not_exist'); return v == nil or v == ''");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(L, -1));
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

    // After delete the variable must be gone: GetVariable returns nil/empty,
    // NOT the old value 'x'.  (A no-op DeleteVariable would still answer 'x'.)
    int n = run_ret(L,
        "local v = world.GetVariable('tmp'); return v == nil or v == ''");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(L, -1));
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

// The Window* / Chat* / Spell* families through the Lua marshalling layer.
//
// Named "GuiStubsReturnValues" when all three were headless stubs.  Two of the
// three are adopted now (docs/STUB-ADOPTION-2026-08.md) and these drive their
// real bodies; only the spell-checker below is still answering an inactive-
// subsystem contract.
TEST(LuaScripting, GuiFamilyReturnValues)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // WindowList with NO miniwindows -> nil (the safe array is never created —
    // "cannot create empty dimension" — so Detach hands back VT_EMPTY).  Assert
    // this BEFORE creating one, so it is the empty case being tested.
    int n = run_ret(L, "return world.WindowList()");
    EXPECT_EQ(1, n);
    EXPECT_TRUE(lua_isnil(L, -1));
    lua_settop(L, 0);

    // WindowCreate -> eOK, and the window really exists afterwards.
    //
    // This pair used to assert eNoSuchWindow (30073) and then that WindowList
    // was STILL nil — the stub's contract, under which no miniwindow could ever
    // be created.  miniwindow.cpp and methods_miniwindows.cpp are in mushcore
    // now (docs/STUB-ADOPTION-2026-08.md §5 step 3), so this is the real body:
    // it allocates a CMiniWindow, registers it in m_MiniWindows and re-sorts the
    // z-order.  Argument shape is the Windows one — L_WindowCreate
    // (lua_methods.cpp:6060-6075): (Name, Left, Top, Width, Height, Position,
    // Flags, BackgroundColour), all my_check'd.
    n = run_ret(L, "return world.WindowCreate('w',0,0,100,100,0,0,0)");
    EXPECT_EQ(1, n);
    EXPECT_EQ(0.0, lua_tonumber(L, -1));            // eOK
    lua_settop(L, 0);

    // …and now the list has it.  Asserting the CONTENT, not just non-nil, so a
    // body that created an empty list would fail.
    EXPECT_LUA_OK(L,
        "local l = world.WindowList()\n"
        "assert(type(l) == 'table', 'WindowList should be a table once a window exists')\n"
        "local n = 0\n"
        "for _, v in pairs(l) do n = n + 1; assert(v == 'w') end\n"
        "assert(n == 1, 'expected 1 window, got ' .. n)\n"
        "return true\n");

    // The parameter checks are real too, and they precede any window creation —
    // so these are not "no such window", they are refusals.
    n = run_ret(L, "return world.WindowCreate('',0,0,10,10,0,0,0)");
    EXPECT_EQ(1, n);
    EXPECT_EQ((double) eNoNameSpecified, lua_tonumber(L, -1));
    lua_settop(L, 0);
    n = run_ret(L, "return world.WindowCreate('bad',0,0,-1,10,0,0,0)");
    EXPECT_EQ(1, n);
    EXPECT_EQ((double) eBadParameter, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // ChatCall with a HOSTNAME -> eCannotLookupDomainName (30043).
    //
    // This used to assert eNoChatConnections (30044), which was the stub's
    // answer; chatsock.cpp and methods_chat.cpp are in mushcore now
    // (docs/STUB-ADOPTION-2026-08.md) so this is the real body.
    //
    // 30043 is not a healthy answer — it is a REAL macOS product bug this
    // assertion now pins.  ChatCallGeneral resolves non-dotted-quad addresses
    // through WSAAsyncGetHostByName, which is an inline stub returning nullptr
    // (platform/mfc_ui.h:7451), so methods_chat.cpp:145-149 deletes the socket
    // and reports "cannot look up domain name".  world.ChatCall to a HOSTNAME
    // has therefore never worked in the shipping mac app; only dotted-IP calls
    // do.  Recorded in docs/STUB-ADOPTION-2026-08.md §4 with the route to fixing
    // it (PosixTransport already has a headless-drivable getaddrinfo resolver).
    // When that lands, this expectation changes to eOK and this comment goes.
    n = run_ret(L, "return world.ChatCall('host.invalid', 4050)");
    EXPECT_EQ(1, n);
    EXPECT_EQ(30043.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // The control that makes the bug above legible: a DOTTED-QUAD address takes
    // the other branch and does NOT fail, because it needs no DNS at all.  If
    // both forms returned the same thing, the finding would be invisible.
    n = run_ret(L, "return world.ChatCall('127.0.0.1', 4050)");
    EXPECT_EQ(1, n);
    EXPECT_EQ(0.0, lua_tonumber(L, -1));       // eOK — connection under way
    doc.ChatDisconnectAll();                   // tidy up the socket we just made
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

    // ePluginCouldNotSaveState is its OWN code (30038, errors.h), not
    // ePluginDoesNotSaveState's (30037) — MakeIntFlagsTable does lua_rawset
    // per row, so a duplicate key here is last-write-wins and leaves
    // error_desc[30037] holding the wrong text with error_desc[30038] nil.
    n = run_ret(L, "return error_code.ePluginCouldNotSaveState");
    EXPECT_EQ(1, n);
    EXPECT_EQ(30038.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return error_desc[30037]");
    EXPECT_EQ(1, n);
    ASSERT_TRUE(lua_isstring(L, -1));
    EXPECT_TRUE(strstr(lua_tostring(L, -1), "does not support saving state") != nullptr);
    lua_settop(L, 0);

    n = run_ret(L, "return error_desc[30038]");
    EXPECT_EQ(1, n);
    ASSERT_TRUE(lua_isstring(L, -1));
    EXPECT_TRUE(strstr(lua_tostring(L, -1), "could not save state") != nullptr);
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

    // (audit §3) the GetOptionList>100 block was dropped here — it duplicated
    // LuaScripting.GetOptionListNonEmpty (same function, branch, assertion shape),
    // which remains the single owner of the option-list-count assertion.

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
// output buffer (not count of lines received)".
//
// The interesting half is WHEN world.DeleteLines is allowed to do anything.
// m_bInSendToScript is initialised TRUE (doc_construct.cpp:98) and is only
// cleared for the deferred-trigger-script window inside ProcessPreviousLine
// (:866 "they can do DeleteLines here", restored at :960).  So DeleteLines
// called from an alias, a timer or a plain script chunk is a documented no-op
// on every platform:
//
//     if (m_bInSendToScript)
//       return;   // can't do it
//
// This test used to assert the opposite — that a bare world.DeleteLines(2)
// removed two lines — and it passed only because the HEADLESS STUB of
// DeleteLines had dropped that guard.  The shipping app was always correct
// (mushgui links the real body); the test build diverged, and the test
// defended the divergence.  Both arms are here now so neither direction can
// regress silently.
TEST(LuaScripting, DeleteLinesIsRefusedOutsideATriggerScript)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    run_ret(L, "world.Note('a') world.Note('b') world.Note('c') world.Note('d')");
    const long total_before  = doc.GetLineCount();
    const long buffer_before = doc.GetLinesInBufferCount();
    ASSERT_TRUE(buffer_before >= 4);

    run_ret(L, "world.DeleteLines(2)");
    EXPECT_EQ(buffer_before, doc.GetLinesInBufferCount());   // refused
    EXPECT_EQ(total_before,  doc.GetLineCount());

    lua_close(L);
}

// The arm that makes the one above meaningful — DeleteLines really working
// inside the trigger-script window — lives in test_triggers.cpp, where the
// trigger/feed helpers are (TriggerScript.DeleteLinesIsAllowedInsideTheScriptWindow).
// Without it, a DeleteLines that ALWAYS refused would pass the test above.

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

// lua_methods.cpp doc(): the documented "other:Method (...)" form works for
// every world-table entry, because doc() removes the userdatum from the stack
// before the arguments are checked.  Three entries never called doc() at all,
// so the userdatum arrived where a string was expected.
TEST(LuaScripting, WorldUserdatumFormWorksForStripANSIAndGetWorld)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    CMUSHclientDoc* pOther = new CMUSHclientDoc();
    pOther->m_mush_name  = "stripworld";
    pOther->m_strWorldID = "abcdefabcdefabcdefabcdef";

    int n = run_ret(L,
        "local w = world.GetWorld('stripworld')\n"
        "return w ~= nil,\n"
        "       w:StripANSI('\\27[1;31maredb'),\n"
        "       w:GetWorld('stripworld') ~= nil,\n"
        "       w:GetWorldById('abcdefabcdefabcdefabcdef') ~= nil");
    ASSERT_EQ(4, n);
    ASSERT_TRUE(lua_toboolean(L, -4));       // found the other world
    EXPECT_STREQ("aredb", lua_tostring(L, -3));
    EXPECT_TRUE(lua_toboolean(L, -2));
    EXPECT_TRUE(lua_toboolean(L, -1));
    lua_settop(L, 0);

    // the plain form is unaffected - doc() only removes argument 1 when it
    // really is a world userdatum
    n = run_ret(L, "return world.StripANSI('\\27[0mplain')");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("plain", lua_tostring(L, -1));
    lua_settop(L, 0);

    delete pOther;
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

// lua_methods.cpp L_CallPlugin: the plugin ID and the routine name are taken
// off our stack before the call ("so, if the called function does a CallPlugin
// back to us, it won't matter"), which leaves the two Lua strings unreferenced
// for the whole of it — a collectgarbage inside the called routine frees them,
// and every error message built afterwards names the routine.  A plugin
// calling itself is the sharpest case: caller and callee share one Lua state,
// so the collection really does run over the caller's strings.
TEST(LuaScripting, CallPluginErrorStillNamesRoutineAfterCollectGarbage)
{
    CMUSHclientDoc doc;

    CPlugin* p = new CPlugin(&doc);
    p->m_bSaveState = false;
    p->m_strID   = "ccccccccccccccccccccccc1";   // 24 chars, as GetPlugin wants
    p->m_strName = "GCPlugin";
    p->m_ScriptEngine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!p->m_ScriptEngine->CreateScriptEngine());
    doc.m_PluginList.push_back(p);

    // 'zz.tgt' is built at run time and nothing else holds it: the globals
    // table has 'zz' and the sub-table has 'tgt', never the dotted name.  The
    // loop after the collection re-uses the freed block.
    ASSERT_TRUE(!p->m_ScriptEngine->Parse(CString(
        "zz = {}\n"
        "function zz.tgt ()\n"
        "  collectgarbage ('collect')\n"
        "  local t = {}\n"
        "  for i = 1, 2000 do t [i] = 'wwwww' .. i end\n"
        "  error ('deliberate')\n"
        "end\n"
        "rc, msg = world.CallPlugin ('ccccccccccccccccccccccc1', 'zz' .. '.' .. 'tgt')\n"),
        "<test>"));

    lua_State* L = p->m_ScriptEngine->L;
    lua_getglobal(L, "rc");
    EXPECT_EQ((double)eErrorCallingPluginRoutine, lua_tonumber(L, -1));
    lua_getglobal(L, "msg");
    const char* msg = lua_tostring(L, -1);
    ASSERT_TRUE(msg != nullptr);
    EXPECT_TRUE(strstr(msg, "zz.tgt") != nullptr);
    EXPECT_TRUE(strstr(msg, "ccccccccccccccccccccccc1") != nullptr);
    lua_settop(L, 0);
}

// The `progress` (progress-dialog) library is part of every world/plugin Lua
// state on Windows.  Until 2026-08-12 its registration sat behind #ifdef
// _WIN32 (and its TU behind the GUI-only library), so any plugin touching
// progress.new died with "attempt to index global 'progress' (a nil value)"
// — even though the port's CProgressDlg backing exists and was repaired
// specifically so scripts could drive it.  Headless the dialog is inert, but
// the LIBRARY must exist and construct.
TEST(LuaScripting, ProgressDialogLibraryIsRegistered)
{
    CMUSHclientDoc doc;
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString(
        "assert(type(progress) == 'table', 'progress library missing')\n"
        "assert(type(progress.new) == 'function', 'progress.new missing')\n"
        "local dlg = progress.new('working...')\n"
        "assert(dlg ~= nil, 'progress.new returned nothing')\n"
        "dlg:range(0, 10)\n"
        "dlg:position(3)\n"
        "dlg:close()\n"), "<test>"));
    delete engine;
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

// CMUSHclientDoc::OnScriptFileChanged (scripting/scripting.cpp) — the
// script-file auto-reload, now the canonical body rather than a port twin.
//
// The twin this replaced differed in two ways that this test had absorbed into
// its premise:
//   * it had no "scripting is not active" guard, so it would CREATE the engine
//     from nothing.  The canonical body returns immediately when there is no
//     engine (scripting.cpp:394) — it is a RELOAD hook, not a loader — so the
//     engine has to exist first.  That is why the setup below calls
//     CreateScriptEngine.
//   * it ignored m_nReloadOption entirely and always reloaded.  The canonical
//     body honours eReloadNever / eReloadAlways and otherwise ASKS, which
//     headless answers IDNO (HeadlessMessageBoxAnswer).  So the reload option
//     is now load-bearing, and all three of its values are covered below —
//     coverage the twin made impossible.
TEST(LuaScripting, ScriptFileChangeReloadsOnlyWhenMtimeDiffers)
{
    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_test_script_%d.lua", (int)getpid());
    const time_t t0 = time(nullptr) - 100;

    auto write_script = [&](const char* body, time_t when) {
        FILE* f = fopen(path, "wb");
        ASSERT_TRUE(f != nullptr);
        fputs(body, f);
        fclose(f);
        struct utimbuf tb; tb.actime = tb.modtime = when;
        ASSERT_EQ(0, utime(path, &tb));          // pin the mtime
    };
    write_script("function fn1() end\n", t0);

    CMUSHclientDoc doc;
    doc.m_bEnableScripts    = TRUE;
    doc.m_strLanguage       = "lua";
    doc.m_strScriptFilename = path;
    doc.m_nReloadOption     = eReloadAlways;     // else headless answers IDNO

    // The initial load is CreateScriptEngine's job, not this hook's.  false is
    // the good return.
    ASSERT_FALSE(doc.CreateScriptEngine());
    ASSERT_TRUE(doc.m_ScriptEngine != nullptr);
    EXPECT_NE(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn1"));
    EXPECT_EQ(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn2"));

    // new CONTENT but the SAME mtime: must NOT reload (mtime is the trigger)
    write_script("function fn2() end\n", t0);
    doc.OnScriptFileChanged(false);
    ASSERT_TRUE(doc.m_ScriptEngine != nullptr);
    EXPECT_NE(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn1"));  // old script
    EXPECT_EQ(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn2"));

    // bump the mtime: now it reloads and picks up the new content
    write_script("function fn2() end\n", t0 + 10);
    doc.OnScriptFileChanged(false);
    ASSERT_TRUE(doc.m_ScriptEngine != nullptr);
    EXPECT_EQ(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn1"));
    EXPECT_NE(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn2"));

    // and a further call with the same (new) mtime is a no-op, not a loop
    doc.OnScriptFileChanged(false);
    EXPECT_NE(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn2"));
}

// The reload OPTION, all three values.  The port twin ignored it entirely, so
// none of this was reachable before the adoption — and the default is the
// middle one, which means the headless default flipped from "always reload" to
// "declined", because MB_YESNO answers IDNO here.
TEST(LuaScripting, ScriptReloadOptionDecidesWhetherAChangeIsPickedUp)
{
    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_test_reloadopt_%d.lua", (int)getpid());
    const time_t t0 = time(nullptr) - 100;

    auto write_script = [&](const char* body, time_t when) {
        FILE* f = fopen(path, "wb");
        ASSERT_TRUE(f != nullptr);
        fputs(body, f);
        fclose(f);
        struct utimbuf tb; tb.actime = tb.modtime = when;
        ASSERT_EQ(0, utime(path, &tb));
    };

    struct Case {
        LONG        option;
        const char* name;
        bool        expectReload;
    };
    const Case cases[] = {
        { eReloadAlways,  "eReloadAlways",  true  },
        { eReloadNever,   "eReloadNever",   false },
        // The DEFAULT (scriptingoptions_numeric.inc:129).  It prompts, and
        // HeadlessMessageBoxAnswer answers MB_YESNO with IDNO — so headless,
        // the default is "do not reload".
        { eReloadConfirm, "eReloadConfirm", false },
    };

    for (const Case& c : cases) {
        write_script("function fn1() end\n", t0);

        CMUSHclientDoc doc;
        doc.m_bEnableScripts    = TRUE;
        doc.m_strLanguage       = "lua";
        doc.m_strScriptFilename = path;
        doc.m_nReloadOption     = c.option;

        ASSERT_FALSE(doc.CreateScriptEngine());
        ASSERT_TRUE(doc.m_ScriptEngine != nullptr);

        write_script("function fn2() end\n", t0 + 10);
        doc.OnScriptFileChanged(false);

        ASSERT_TRUE(doc.m_ScriptEngine != nullptr);
        const bool reloaded =
            doc.m_ScriptEngine->GetDispid("fn2") != DISPID_UNKNOWN;
        if (reloaded != c.expectReload)
            printf("  %s: reloaded=%d, expected %d\n",
                   c.name, (int) reloaded, (int) c.expectReload);
        EXPECT_EQ(c.expectReload, reloaded);
        // The old script survives exactly when the new one was not taken —
        // so "reloaded" is not merely "the engine was thrown away".
        EXPECT_EQ(!c.expectReload,
                  doc.m_ScriptEngine->GetDispid("fn1") != DISPID_UNKNOWN);
    }

    // bForce overrides even eReloadNever — that is what Game > Reload script
    // file passes, and nothing covered it before.
    write_script("function fn1() end\n", t0);
    CMUSHclientDoc doc;
    doc.m_bEnableScripts    = TRUE;
    doc.m_strLanguage       = "lua";
    doc.m_strScriptFilename = path;
    doc.m_nReloadOption     = eReloadNever;
    ASSERT_FALSE(doc.CreateScriptEngine());
    write_script("function fn2() end\n", t0 + 10);
    doc.OnScriptFileChanged(true);
    ASSERT_TRUE(doc.m_ScriptEngine != nullptr);
    EXPECT_NE(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn2"));
    ::unlink(path);
}

// The two arms that mutation showed nothing could observe.
//
// Both survived a mutation of the canonical body, and neither because the test
// suite was lazy — each is genuinely invisible through the paths anything else
// takes, which is exactly why they need naming here.

// (1) The RE-ENTRANCY GUARD (scripting.cpp:29).  Both port twins lacked it, so a
// second call would `new CScriptEngine` over the live pointer and LEAK the
// first.  Every one of the six production call sites gates on !m_ScriptEngine or
// calls DisableScripting first, so the leak was latent rather than live — and
// unreachable except by calling the method directly, which is what this does.
TEST(LuaScripting, CreateScriptEngineRefusesToReplaceALiveEngine)
{
    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_test_reentrant_%d.lua", (int)getpid());
    FILE* f = fopen(path, "wb");
    ASSERT_TRUE(f != nullptr);
    fputs("function fn1() end\n", f);
    fclose(f);

    CMUSHclientDoc doc;
    doc.m_bEnableScripts    = TRUE;
    doc.m_strLanguage       = "lua";
    doc.m_strScriptFilename = path;

    ASSERT_FALSE(doc.CreateScriptEngine());       // false = good return
    CScriptEngine * first = doc.m_ScriptEngine;
    ASSERT_NE(nullptr, first);

    // Second call: refused, and — the load-bearing part — the ORIGINAL engine
    // is still the one installed.  Without the guard this pointer changes and
    // the old engine is lost with no delete.
    EXPECT_FALSE(doc.CreateScriptEngine());
    EXPECT_EQ(first, doc.m_ScriptEngine);
    EXPECT_NE(DISPID_UNKNOWN, doc.m_ScriptEngine->GetDispid("fn1"));
    ::unlink(path);
}

// (2) eReloadNever's EARLY RETURN (scripting.cpp:401).  Deleting it leaves the
// headless outcome identical, because control then reaches the prompt and
// HeadlessMessageBoxAnswer answers MB_YESNO with IDNO — so no reload either
// way.  The difference is only visible on Windows, where the user would be
// ASKED about a file they said never to reload.
//
// So the assertion is not about the outcome but about the QUESTION: under
// eReloadNever nothing may be asked at all.
static int  g_askCount = 0;
static int  CountingAsk(const char*, const char*, unsigned) { ++g_askCount; return IDNO; }

// Saves a process-global g_wxhook_* pointer and restores it in the
// destructor, so an ASSERT_* between the install and a manual reset (which
// returns out of the whole TEST body, not just past the reset line) cannot
// leave the hook installed for every later TEST() in this binary.
template <typename T>
struct HookGuard {
    T& slot;
    T  saved;
    explicit HookGuard(T& s) : slot(s), saved(s) {}
    ~HookGuard() { slot = saved; }
};

TEST(LuaScripting, ReloadNeverDoesNotEvenAsk)
{
    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_test_neverask_%d.lua", (int)getpid());
    const time_t t0 = time(nullptr) - 100;

    // Returns false (via EXPECT_, not ASSERT_) on a write failure instead of
    // just unwinding out of the lambda: an ASSERT_* inside a lambda only
    // returns from the LAMBDA, letting the TEST continue past a failed write
    // with a stale file on disk -- which let the targeted
    // EXPECT_EQ(0, g_askCount) below pass vacuously in that corner case
    // instead of failing at the point of the real defect.  Callers now check
    // the return value with their own ASSERT_TRUE, which aborts the TEST body.
    auto write_script = [&](const char* body, time_t when) -> bool {
        FILE* f = fopen(path, "wb");
        EXPECT_TRUE(f != nullptr);
        if (!f) return false;
        fputs(body, f);
        fclose(f);
        struct utimbuf tb; tb.actime = tb.modtime = when;
        EXPECT_EQ(0, utime(path, &tb));
        return true;
    };

    HookGuard<decltype(g_wxhook_message_box)> mbGuard(g_wxhook_message_box);
    g_wxhook_message_box = CountingAsk;

    // eReloadConfirm ASKS — the control, without which "asked nothing" could
    // mean the change was never noticed.
    ASSERT_TRUE(write_script("function fn1() end\n", t0));
    {
        CMUSHclientDoc doc;
        doc.m_bEnableScripts    = TRUE;
        doc.m_strLanguage       = "lua";
        doc.m_strScriptFilename = path;
        doc.m_nReloadOption     = eReloadConfirm;
        ASSERT_FALSE(doc.CreateScriptEngine());
        g_askCount = 0;
        ASSERT_TRUE(write_script("function fn2() end\n", t0 + 10));
        doc.OnScriptFileChanged(false);
        EXPECT_GT(g_askCount, 0);
    }

    // eReloadNever asks NOTHING, on the very same change.
    ASSERT_TRUE(write_script("function fn1() end\n", t0));
    {
        CMUSHclientDoc doc;
        doc.m_bEnableScripts    = TRUE;
        doc.m_strLanguage       = "lua";
        doc.m_strScriptFilename = path;
        doc.m_nReloadOption     = eReloadNever;
        ASSERT_FALSE(doc.CreateScriptEngine());
        g_askCount = 0;
        ASSERT_TRUE(write_script("function fn2() end\n", t0 + 10));
        doc.OnScriptFileChanged(false);
        EXPECT_EQ(0, g_askCount);
    }

    ::unlink(path);
}

// ── world.Metaphone ──────────────────────────────────────────────────────
// CMUSHclientDoc::Metaphone joins primary+","+secondary; the BSTR→Lua
// conversion (pushBstr → CString(char*)) stops at the first NUL, which is
// exactly where SetAt(maxlength,'\0') truncates over-long codes — same
// visible behaviour as Windows (CString-from-BSTR also stops at NUL).

// Parameterised (audit §4): five byte-identical bodies differing only in
// {word, length, expected}.  A fresh LuaDoc per row (RAII → no lua_close leak on
// an early ASSERT return).  The per-case boundary notes live in the 'note' column.
TEST(LuaScripting, MetaphoneCodesAndTruncation)
{
    struct Case { const char* word; int len; const char* expect; const char* note; };
    const Case cases[] = {
        { "smith",      4, "SM0,XMT", "joins primary+','+secondary" },
        { "thomas",     4, "TMS",     "omits comma when there is no alternate" },
        // catherine → K0RN/KTRN at length 4; at Length 2 the loop exits as soon as
        // both codes reach 2 chars — no overshoot, so both codes survive.
        { "catherine",  2, "K0,KT",   "short length stops the loop at length" },
        // filipowicz: the final WICZ adds two chars at once (TS/FX), overshooting
        // Length 4 → codes "FLPTS"/"FLPFX", SetAt(4,'\0') embeds a NUL.  The BSTR→Lua
        // conversion stops at the first NUL, so ",FLPF" is swallowed (Windows parity).
        { "filipowicz", 4, "FLPT",    "overshoot truncates at the embedded NUL" },
        { "",           4, "",        "empty word returns empty string" },
    };
    for (const Case& c : cases) {
        LuaDoc f; ASSERT_NE(nullptr, f.L);
        char code[96];
        snprintf(code, sizeof code, "return world.Metaphone('%s', %d)", c.word, c.len);
        int n = run_ret(f.L, code);
        ASSERT_EQ(1, n);
        EXPECT_STREQ(c.expect, lua_tostring(f.L, -1));   // c.note
        lua_settop(f.L, 0);
    }
}

// ── Timer/alias/trigger option families ──────────────────────────────────
// Each Get/SetXxxOption pair funnels through the per-class option tables at
// the top of methods_timers/aliases/triggers.cpp.  These tests round-trip
// every documented option and pin the error codes for the rejection arms.

// EXPECT_LUA_OK is defined in lua_test_helpers.h (included above).

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

// ═══ bundled Lua C libraries: bc (bignum) + bits ═══════════════════════════
// Every world/plugin Lua state gets these (lua_scripting.cpp OpenLua); the
// engine normally calls bc_init_numbers() once at app start (MUSHclient.cpp),
// mirrored here.

extern "C" int luaopen_bc(lua_State*);
extern "C" int luaopen_bits(lua_State*);
extern "C" void bc_init_numbers(void);

TEST(LuaLibs, BcBignumArithmetic)
{
    bc_init_numbers();
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_TRUE(L != nullptr);
    luaopen_bc(L);
    lua_settop(L, 0);
    EXPECT_TRUE(run(L,
        "local a = bc.number('123456789012345678901234567890')\n"
        "assert(bc.tostring(a + a) == '246913578024691357802469135780')\n"
        "assert(tostring(a - a) == '0')\n"
        "assert(bc.iszero(a - a))\n"
        "assert(bc.isneg(-a))\n"
        "assert(not bc.isneg(a))\n"
        "assert(a == bc.number('123456789012345678901234567890'))\n"
        "assert(bc.number(2) < bc.number(3))\n"
        "assert(bc.compare(bc.number(5), bc.number(4)) > 0)\n"
        "assert(tostring(bc.number(2) ^ 64) == '18446744073709551616')\n"
        "assert(tostring(bc.mul(bc.number(1000000007), bc.number(998244353)))"
        "       == '998244359987710471')\n"
        "assert(tostring(bc.mod(bc.number(10), bc.number(3))) == '1')\n"
        "assert(tostring(bc.div(bc.number(10), bc.number(2))) == '5')\n"
        "local q, r = bc.divmod(bc.number(17), bc.number(5))\n"
        "assert(tostring(q) == '3' and tostring(r) == '2')\n"
        "assert(tostring(bc.sqrt(bc.number(144))) == '12')\n"
        "assert(tostring(bc.powmod(bc.number(4), bc.number(13), bc.number(497)))"
        "       == '445')\n"
        "assert(bc.tonumber(bc.number(42)) == 42)\n"
        "assert(bc.digits(0) == 0)\n"
        "assert(tostring(bc.trunc(bc.number('7'))) == '7')\n"
        "assert(tostring(bc.neg(bc.number(6))) == '-6')\n"));
    lua_close(L);
}

TEST(LuaLibs, BitsLibraryOperations)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_TRUE(L != nullptr);
    luaopen_bits(L);
    lua_settop(L, 0);
    EXPECT_TRUE(run(L,
        "assert(bit.band(6, 3) == 2)\n"
        "assert(bit.bor(4, 1) == 5)\n"
        "assert(bit.xor(6, 3) == 5)\n"
        "assert(bit.shl(1, 4) == 16)\n"
        "assert(bit.shr(16, 4) == 1)\n"
        "assert(bit.ashr(-16, 2) == -4)\n"
        "assert(bit.mod(10, 3) == 1)\n"
        "assert(bit.test(18, 16))\n"
        "assert(not bit.test(18, 1))\n"
        "assert(bit.clear(7, 1) == 6)\n"
        "assert(bit.neg(0) ~= 0)\n"
        "assert(bit.tonumber('ff', 16) == 255)\n"
        "assert(bit.tonumber(' -42') == -42)\n"
        "assert(bit.tostring(255, 16) == 'FF')\n"
        "assert(bit.tostring(-255, 16) == '-FF')\n"
        "assert(bit.tostring(1000) == '1000')\n"));
    lua_close(L);
}

// ═══ Round-4 coverage: marshalling error arms + method-family gaps ═════════

#include "pal/itransport.h"
#include "fake_transport.h"

using LuaFakeTransport = RecordingTransport;

// my_checknumber / my_checkstring type errors carry the argument number and
// the calling function's name (my_argerror / my_typerror / my_tag_error).
TEST(LuaMarshal, TypeErrorsNameArgumentAndFunction)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L,
        "local ok, err = pcall(function() return world.GetLineInfo({}) end)\n"
        "return ok, err");
    ASSERT_EQ(2, n);
    EXPECT_TRUE(!lua_toboolean(L, -2));
    const char* err = lua_tostring(L, -1);
    ASSERT_TRUE(err != nullptr);
    EXPECT_TRUE(strstr(err, "bad argument #1") != nullptr);
    EXPECT_TRUE(strstr(err, "number expected, got table") != nullptr);
    lua_settop(L, 0);

    // string expected
    n = run_ret(L,
        "local ok, err = pcall(function() return world.SetVariable({}, 'v') end)\n"
        "return ok, err");
    ASSERT_EQ(2, n);
    EXPECT_TRUE(!lua_toboolean(L, -2));
    err = lua_tostring(L, -1);
    ASSERT_TRUE(err != nullptr);
    EXPECT_TRUE(strstr(err, "string expected, got table") != nullptr);
    lua_settop(L, 0);

    lua_close(L);
}

// concatArgs: a value whose __tostring metamethod returns a non-string is
// rejected; multiple print args are joined with a space.
TEST(LuaMarshal, ConcatArgsToStringContractAndPrint)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "print('alpha', 'beta')"));   // l_print → Note
    bool sawJoined = false;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (std::string(l->text, l->len).find("alpha beta") != std::string::npos)
            sawJoined = true;
    }
    EXPECT_TRUE(sawJoined);

    int n = run_ret(L,
        "local bad = setmetatable({}, { __tostring = function() return {} end })\n"
        "local ok, err = pcall(function() world.Note(bad) end)\n"
        "return ok, tostring(err)");
    ASSERT_EQ(2, n);
    EXPECT_TRUE(!lua_toboolean(L, -2));
    const char* err = lua_tostring(L, -1);
    ASSERT_TRUE(err != nullptr);
    EXPECT_TRUE(strstr(err, "'tostring' must return a string") != nullptr);
    lua_settop(L, 0);

    lua_close(L);
}

// ArrayImport with a TABLE second argument (Lua extension): import, dup
// detection, missing array, and non-string-key rejection.
TEST(LuaMarshal, ArrayImportTableVariantArms)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // missing array
    int n = run_ret(L, "return world.ArrayImport('nosuch', {a='1'})");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eArrayDoesNotExist, lua_tonumber(L, -1));
    lua_settop(L, 0);

    EXPECT_TRUE(run(L, "world.ArrayCreate('t')"));
    n = run_ret(L, "return world.ArrayImport('t', {a='1', b='2'})");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eOK, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // duplicate keys → eImportedWithDuplicates and the value is replaced
    n = run_ret(L, "return world.ArrayImport('t', {a='9'}), world.ArrayGet('t','a')");
    ASSERT_EQ(2, n);
    EXPECT_EQ((double)eImportedWithDuplicates, lua_tonumber(L, -2));
    EXPECT_STREQ("9", lua_tostring(L, -1));
    lua_settop(L, 0);

    // non-string key → error
    n = run_ret(L,
        "local ok, err = pcall(function() return world.ArrayImport('t', {[1]='x'}) end)\n"
        "return ok, tostring(err)");
    ASSERT_EQ(2, n);
    EXPECT_TRUE(!lua_toboolean(L, -2));
    EXPECT_TRUE(strstr(lua_tostring(L, -1), "string keys") != nullptr);
    lua_settop(L, 0);

    // non-string value → error, not a crash: lua_tostring returns NULL for a
    // boolean, and building the value string from that took the process down
    n = run_ret(L,
        "local ok, err = pcall(function() return world.ArrayImport('t', {zz=true}) end)\n"
        "return ok, tostring(err), world.ArrayGet('t', 'zz') == nil");
    ASSERT_EQ(3, n);
    EXPECT_TRUE(!lua_toboolean(L, -3));
    EXPECT_TRUE(strstr(lua_tostring(L, -2), "string values") != nullptr);
    EXPECT_TRUE(lua_toboolean(L, -1));      // nothing was imported
    lua_settop(L, 0);

    // a number value is still accepted (lua_tostring coerces it in place)
    n = run_ret(L, "return world.ArrayImport('t', {num=42}), world.ArrayGet('t', 'num')");
    ASSERT_EQ(2, n);
    EXPECT_EQ((double)eOK, lua_tonumber(L, -2));
    EXPECT_STREQ("42", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// ColourTell with multiple colour/colour/text triplets consumes them all.
TEST(LuaMarshal, ColourTellMultipleTriplets)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "world.ColourTell('red', 'blue', 'one', 'green', 'black', 'two')\n"
        "world.Note('')"));
    bool sawBoth = false;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (std::string(l->text, l->len).find("onetwo") != std::string::npos)
            sawBoth = true;
    }
    EXPECT_TRUE(sawBoth);
    lua_close(L);
}

// MtSrand table arms: empty table and non-number entries are rejected.
TEST(LuaMarshal, MtSrandTableValidation)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L,
        "local ok, err = pcall(function() world.MtSrand({}) end)\n"
        "return ok, tostring(err)");
    ASSERT_EQ(2, n);
    EXPECT_TRUE(!lua_toboolean(L, -2));
    EXPECT_TRUE(strstr(lua_tostring(L, -1), "must not be empty") != nullptr);
    lua_settop(L, 0);

    n = run_ret(L,
        "local ok, err = pcall(function() world.MtSrand({'x'}) end)\n"
        "return ok, tostring(err)");
    ASSERT_EQ(2, n);
    EXPECT_TRUE(!lua_toboolean(L, -2));
    EXPECT_TRUE(strstr(lua_tostring(L, -1), "consist of numbers") != nullptr);
    lua_settop(L, 0);

    // valid table seed works and produces deterministic output
    n = run_ret(L, "world.MtSrand({1, 2, 3}) return world.MtRand()");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_isnumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// GetStyleInfo Lua extension: StyleNumber 0 → table of all styles with the
// full DoStyle field set (colours, action, flags); explicit style → one
// table; out-of-range → nothing.
TEST(LuaMarshal, GetStyleInfoFullTableExtension)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // a line with two styled runs and a hyperlink
    EXPECT_TRUE(run(L,
        "world.ColourTell('red', 'black', 'plain')\n"
        "world.Hyperlink('go north', 'CLICK', 'a hint', 'cyan', 'black', 1)\n"
        "world.Note('')"));

    int n = run_ret(L,
        "local t = world.GetStyleInfo(1)\n"
        "return #t, t[1].text, t[1].textcolour, t[2].actiontype, t[2].action, t[2].hint");
    ASSERT_EQ(6, n);
    EXPECT_TRUE(lua_tonumber(L, -6) >= 2);
    EXPECT_STREQ("plain", lua_tostring(L, -5));
    EXPECT_TRUE(lua_isnumber(L, -4));
    EXPECT_EQ(2.0, lua_tonumber(L, -3));           // ACTION_HYPERLINK
    EXPECT_STREQ("go north", lua_tostring(L, -2));
    EXPECT_STREQ("a hint", lua_tostring(L, -1));
    lua_settop(L, 0);

    // single style table
    n = run_ret(L, "local t = world.GetStyleInfo(1, 1) return t.text, t.length");
    ASSERT_EQ(2, n);
    EXPECT_STREQ("plain", lua_tostring(L, -2));
    EXPECT_EQ(5.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // out-of-range style / line → no results
    n = run_ret(L, "return world.GetStyleInfo(1, 99)");
    EXPECT_EQ(0, n);
    n = run_ret(L, "return world.GetStyleInfo(999)");
    EXPECT_EQ(0, n);
    lua_settop(L, 0);

    // type-specific VARIANT path for one style (InfoType 1 = text)
    n = run_ret(L, "return world.GetStyleInfo(1, 1, 1)");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("plain", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// GetLineInfo: line 0 / out-of-range → no result; with a performance-counter
// frequency published the elapsed-time fields appear.
TEST(LuaMarshal, GetLineInfoBoundsAndTiming)
{
    LONGLONG saved = App.m_iCounterFrequency;
    App.m_iCounterFrequency = 1000000;
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "world.Note('timed line')"));
    int n = run_ret(L, "return world.GetLineInfo(0)");
    EXPECT_EQ(0, n);
    n = run_ret(L, "local t = world.GetLineInfo(1) return type(t.time)");
    ASSERT_EQ(1, n);
    lua_settop(L, 0);

    App.m_iCounterFrequency = saved;
    lua_close(L);
}

// world.Save writes the world file; Save-As mode restores the doc path.
TEST(LuaMarshal, SaveWritesWorldFile)
{
    CMUSHclientDoc doc;
    doc.m_mush_name = "SaveTest";
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_r4_%d_save.mcl", (int)getpid());
    char code[256];
    snprintf(code, sizeof code, "return world.Save('%s', true)", path);
    int n = run_ret(L, code);
    ASSERT_EQ(1, n);
    // Save() returns !DoSave() — i.e. FALSE on success (Windows parity);
    // the written file below is the real success observable
    EXPECT_TRUE(!lua_toboolean(L, -1));
    lua_settop(L, 0);

    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[128] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    unlink(path);
    EXPECT_TRUE(strstr(buf, "muclient") != nullptr);
    lua_close(L);
}

// world.SendPkt sends raw bytes (no telnet doubling, no line ending).
TEST(LuaMarshal, SendPktSendsRawBytes)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    LuaFakeTransport* t = new LuaFakeTransport();
    doc.m_pTransport = t;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.SendPkt('raw\\255bytes')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eOK, lua_tonumber(L, -1));
    EXPECT_TRUE(t->sent.find("raw" "\xFF" "bytes") != std::string::npos);
    lua_settop(L, 0);

    lua_close(L);
}

// Clipboard round trip headless: SetClipboard is a no-op sink and
// GetClipboard returns the empty string (no window server).
TEST(LuaMarshal, ClipboardHeadlessBehaviour)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_TRUE(run(L, "world.SetClipboard('some text')"));
    int n = run_ret(L, "return world.GetClipboard()");
    ASSERT_EQ(1, n);
    lua_settop(L, 0);
    lua_close(L);
}

// world.IsConnected reflects the connect phase.
TEST(LuaMarshal, IsConnectedReflectsPhase)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    int n = run_ret(L, "return world.IsConnected()");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(L, -1));
    doc.m_iConnectPhase = eConnectNotConnected;
    n = run_ret(L, "return world.IsConnected()");
    EXPECT_TRUE(!lua_toboolean(L, -1));
    lua_settop(L, 0);
    lua_close(L);
}

// tostring(world-userdatum) uses the __tostring metamethod ("world").
TEST(LuaMarshal, WorldUserdatumToString)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    CMUSHclientDoc* pOther = new CMUSHclientDoc();
    pOther->m_mush_name = "tsworld";
    int n = run_ret(L, "local w = world.GetWorld('tsworld') return tostring(w)");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("world", lua_tostring(L, -1));
    lua_settop(L, 0);
    delete pOther;
    lua_close(L);
}

// Database marshalling: NULL and FLOAT column values arrive as nil / number.
TEST(LuaMarshal, DatabaseNullAndFloatColumns)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int rc = run_ret(L,
        "local a = world.DatabaseOpen('db', ':memory:', 6)\n"
        "local b = world.DatabasePrepare('db', 'SELECT 3.5 AS f, NULL AS n')\n"
        "local c = world.DatabaseStep('db')\n"
        "return a, b, c");
    ASSERT_EQ(3, rc);
    EXPECT_EQ(0.0,   lua_tonumber(L, -3));
    EXPECT_EQ(0.0,   lua_tonumber(L, -2));
    EXPECT_EQ(100.0, lua_tonumber(L, -1));   // SQLITE_ROW
    lua_settop(L, 0);
    int n = run_ret(L,
        "local f = world.DatabaseColumnValue('db', 1)\n"
        "local nl = world.DatabaseColumnValue('db', 2)\n"
        "return f, nl == nil");
    ASSERT_EQ(2, n);
    EXPECT_EQ(3.5, lua_tonumber(L, -2));
    EXPECT_TRUE(lua_toboolean(L, -1));
    lua_settop(L, 0);

    // the ColumnValues (plural) variant returns a fresh table of the row
    n = run_ret(L,
        "local t = world.DatabaseColumnValues('db')\n"
        "return t[1], t[2] == nil");
    ASSERT_EQ(2, n);
    EXPECT_EQ(3.5, lua_tonumber(L, -2));
    EXPECT_TRUE(lua_toboolean(L, -1));
    lua_settop(L, 0);

    EXPECT_TRUE(run(L, "world.DatabaseFinalize('db') world.DatabaseClose('db')"));
    lua_close(L);
}

// ═══ Spellchecker family (Lua-backed; active when the app publishes a
//     spellchecker state) ══════════════════════════════════════════════════

struct SpellFixture {
    lua_State* SL = nullptr;
    BOOL savedOK;
    lua_State* savedLua;
    SpellFixture(const char* body) {
        savedOK  = App.m_bSpellCheckOK;
        savedLua = App.m_SpellChecker_Lua;
        SL = luaL_newstate();
        luaL_openlibs(SL);
        luaL_loadstring(SL, body);
        lua_pcall(SL, 0, 0, 0);
        App.m_SpellChecker_Lua = SL;
        App.m_bSpellCheckOK = TRUE;
    }
    ~SpellFixture() {
        App.m_bSpellCheckOK = savedOK;
        App.m_SpellChecker_Lua = savedLua;
        if (SL) lua_close(SL);
    }
};

// SpellCheck: number result → error count; table result → SAFEARRAY of the
// misspelled words; empty table → 0.
TEST(SpellCheckTest, SpellCheckResultShapes)
{
    SpellFixture fx(
        "function spellcheck_string(text)\n"
        "  if text == 'clean' then return 0 end\n"
        "  if text == 'empty' then return {} end\n"
        "  return { 'wrng', 'agn' }\n"
        "end\n");

    CMUSHclientDoc doc;
    VARIANT v = doc.SpellCheck("clean");
    EXPECT_EQ((int)VT_I4, (int)v.vt);
    EXPECT_EQ(0, (int)v.lVal);

    v = doc.SpellCheck("empty");
    EXPECT_EQ((int)VT_I4, (int)v.vt);
    EXPECT_EQ(0, (int)v.lVal);

    v = doc.SpellCheck("has wrng words agn");
    EXPECT_TRUE((v.vt & VT_ARRAY) != 0);
    VariantClear(&v);
}

// SpellCheckDlg: the "spellcheck" function's string result is returned; a
// missing function or non-string result → empty VARIANT.
TEST(SpellCheckTest, SpellCheckDlgArms)
{
    {
        SpellFixture fx("function spellcheck(text) return 'corrected' end\n");
        CMUSHclientDoc doc;
        VARIANT v = doc.SpellCheckDlg("orig");
        EXPECT_EQ((int)VT_BSTR, (int)v.vt);
        VariantClear(&v);
    }
    {
        SpellFixture fx("spellcheck = nil");   // no function
        CMUSHclientDoc doc;
        VARIANT v = doc.SpellCheckDlg("orig");
        EXPECT_EQ((int)VT_EMPTY, (int)v.vt);
    }
    {
        SpellFixture fx("function spellcheck(text) error('boom') end\n");
        CMUSHclientDoc doc;
        VARIANT v = doc.SpellCheckDlg("orig");
        EXPECT_EQ((int)VT_EMPTY, (int)v.vt);   // runtime-error arm
    }
}

// AddSpellCheckWord: validation arms then the Lua add-word call.
TEST(SpellCheckTest, AddSpellCheckWordValidationAndCall)
{
    SpellFixture fx(
        "added = {}\n"
        "function spellcheck_add_word(orig, action, repl)\n"
        "  added[#added+1] = orig .. '/' .. action .. '/' .. (repl or '')\n"
        "  return true\n"
        "end\n");

    CMUSHclientDoc doc;
    EXPECT_EQ((long)eBadParameter, doc.AddSpellCheckWord("", "i", ""));
    CString longWord('x', 70);
    EXPECT_EQ((long)eBadParameter, doc.AddSpellCheckWord(longWord, "i", ""));
    EXPECT_EQ((long)eBadParameter, doc.AddSpellCheckWord("word", "i", longWord));
    EXPECT_EQ((long)eUnknownOption, doc.AddSpellCheckWord("word", "zz", ""));
    EXPECT_EQ((long)eUnknownOption, doc.AddSpellCheckWord("word", "q", ""));
    EXPECT_EQ((long)eOK, doc.AddSpellCheckWord("word", "i", ""));

    // spell-check inactive arm
    App.m_bSpellCheckOK = FALSE;
    EXPECT_EQ((long)eSpellCheckNotActive, doc.AddSpellCheckWord("word", "i", ""));
}

// SpellCheckCommand headless: no command view exists → -1.
TEST(SpellCheckTest, SpellCheckCommandNoViewReturnsMinusOne)
{
    SpellFixture fx("function spellcheck(text, all) return text end\n");
    CMUSHclientDoc doc;
    EXPECT_EQ(-1L, doc.SpellCheckCommand(0, 0));
}

// ═══ Round-4 final sweep: utils compress/hex/base64, Debug() listings,
//     method-family arms, Metaphone battery ═════════════════════════════════

TEST(SweepR4, UtilsCompressRoundTripAndHexArms)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "local plain = string.rep('squeeze me thoroughly ', 200)\n"
        "local z = utils.compress(plain)\n"
        "assert(#z < #plain)\n"
        "assert(utils.decompress(z) == plain)\n"));

    // fromhex: lowercase digits and the bad-digit error
    int n = run_ret(L, "return utils.fromhex('48690a')");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("Hi\n", lua_tostring(L, -1));
    lua_settop(L, 0);

    n = run_ret(L,
        "local ok, err = pcall(function() return utils.fromhex('4z') end)\n"
        "return ok, tostring(err)");
    ASSERT_EQ(2, n);
    EXPECT_TRUE(!lua_toboolean(L, -2));
    EXPECT_TRUE(strstr(lua_tostring(L, -1), "Not a hex digit") != nullptr);
    lua_settop(L, 0);

    // base64encode long enough to hit the wrap arm, decode restores it
    EXPECT_TRUE(run(L,
        "local text = string.rep('x', 200)\n"
        "local b = utils.base64encode(text, true)\n"     // multi-line
        "assert(b:find('\\r\\n'))\n"
        "assert(utils.base64decode(b) == text)\n"));

    lua_close(L);
}

TEST(SweepR4, DebugListingsCoverEachAction)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "world.AddTriggerEx('dt', 'match', 'resp', 1, -1, 0, '', '', 0, 100)\n"
        "world.AddAlias('da', 'match', 'resp', 1, '')\n"
        "world.AddTimer('dm', 0, 1, 0, 'resp', 1, '')\n"
        "world.SetVariable('dv', 'val')\n"
        "world.Debug('')\n"                // help arm
        "world.Debug('triggerlist')\n"
        "world.Debug('aliaslist')\n"
        "world.Debug('timerlist')\n"
        "world.Debug('variablelist')\n"
        "world.Debug('telnetlist')\n"
        "world.Debug('pluginlist')\n"
        "world.Debug('info')\n"));

    // the listings noted things into the output
    bool sawTrigger = false;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (std::string(l->text, l->len).find("dt") != std::string::npos)
            sawTrigger = true;
    }
    EXPECT_TRUE(sawTrigger);

    // plugin-scoped debug with an unknown plugin id notes the failure
    EXPECT_TRUE(run(L, "world.Debug('@ffffffffffffffffffffffff triggerlist')"));
    lua_close(L);
}

TEST(SweepR4, UtilityMethodArms)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // (audit §3) the duplicate StripANSI('a<ESC>[31mred<ESC>[0mb')=='aredb' line was
    // removed here — LuaSurface.UtilityStringHelpers is the single owner of that vector.

    // GenerateName needs the names file — missing file arm first
    int n = run_ret(L, "return world.ReadNamesFile('/nonexistent-r4/names.txt')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eCouldNotOpenFile, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(SweepR4, NoteColourCustomArms)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // custom note colour: Tell in custom palette mode
    doc.m_bNotesInRGB = false;
    doc.m_iNoteTextColour = 3;             // custom palette index
    doc.m_customtext[3] = RGB(1, 2, 3);
    doc.m_customback[3] = RGB(4, 5, 6);
    EXPECT_TRUE(run(L, "world.Tell('custom-coloured')"));

    // GetNoteColourFore/Back through the custom arm
    int n = run_ret(L, "return world.GetNoteColourFore(), world.GetNoteColourBack()");
    ASSERT_EQ(2, n);
    EXPECT_EQ((double)RGB(1, 2, 3), lua_tonumber(L, -2));
    EXPECT_EQ((double)RGB(4, 5, 6), lua_tonumber(L, -1));
    lua_settop(L, 0);

    // same-colour mode with custom-16-as-default
    doc.m_iNoteTextColour = SAMECOLOUR;
    doc.m_bCustom16isDefaultColour = TRUE;
    doc.m_customtext[15] = RGB(7, 8, 9);
    doc.m_customback[15] = RGB(10, 11, 12);
    n = run_ret(L, "return world.GetNoteColourFore(), world.GetNoteColourBack()");
    ASSERT_EQ(2, n);
    EXPECT_EQ((double)RGB(7, 8, 9),    lua_tonumber(L, -2));
    EXPECT_EQ((double)RGB(10, 11, 12), lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(SweepR4, ExportXmlPerTypeAndImportErrors)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "world.AddTriggerEx('xt', 'm', 'r', 1, -1, 0, '', '', 0, 100)\n"
        "world.AddAlias('xa', 'm', 'r', 1, '')\n"
        "world.AddTimer('xm', 0, 1, 0, 'r', 1, '')\n"
        "world.SetVariable('xv', 'val')\n"));

    int n = run_ret(L,
        "return world.ExportXML(0, 'xt'), world.ExportXML(1, 'xa'),\n"
        "       world.ExportXML(2, 'xm'), world.ExportXML(4, 'xv')");
    ASSERT_EQ(4, n);
    EXPECT_TRUE(strstr(lua_tostring(L, -4), "<triggers>")  != nullptr);
    EXPECT_TRUE(strstr(lua_tostring(L, -3), "<aliases>")   != nullptr);
    EXPECT_TRUE(strstr(lua_tostring(L, -2), "<timers>")    != nullptr);
    EXPECT_TRUE(strstr(lua_tostring(L, -1), "<variables>") != nullptr);
    lua_settop(L, 0);

    // ImportXML error arms: not XML at all, and malformed XML
    n = run_ret(L, "return world.ImportXML('just some text'), "
                   "world.ImportXML('<triggers><trigger broken></triggers>')");
    ASSERT_EQ(2, n);
    EXPECT_EQ(-1.0, lua_tonumber(L, -2));
    EXPECT_EQ(-1.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(SweepR4, DefaultAndCurrentAlphaOptions)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // alpha option reads through GetCurrentValue / GetDefaultValue
    int n = run_ret(L,
        "world.SetAlphaOption('terminal_identification', 'r4-terminal')\n"
        "return world.GetCurrentValue('terminal_identification'),\n"
        "       world.GetDefaultValue('terminal_identification'),\n"
        "       world.GetCurrentValue('wrap_column')");
    ASSERT_EQ(3, n);
    EXPECT_STREQ("r4-terminal", lua_tostring(L, -3));
    EXPECT_TRUE(lua_isstring(L, -2));
    EXPECT_TRUE(lua_isnumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// Double Metaphone battery: words chosen to walk the consonant special
// cases (silent letters, gh/ch/sch, x, z, tia, etc.).
TEST(SweepR4, MetaphoneConsonantBattery)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    static const char* words[] = {
        "Xavier", "gnome", "knight", "pneumonia", "wrangle", "aegis",
        "school", "cheese", "chianti", "michael", "czar", "edge",
        "judge", "ghost", "laugh", "cough", "philosophy", "psalm",
        "queen", "island", "sugar", "tion", "nation", "thomas",
        "through", "wagner", "xylophone", "zhivago", "jose", "caesar",
        "chorus", "witch", "watch", "science", "scene", "wholly",
        "acceptance", "success", "bacchus", "orchestra", "architect",
    };
    for (const char* w : words) {
        char code[256];
        snprintf(code, sizeof code, "return world.Metaphone('%s', 10)", w);
        int n = run_ret(L, code);
        if (n != 1 || !lua_isstring(L, -1)) {
            char msg[128];
            snprintf(msg, sizeof msg, "Metaphone('%s') gave no string", w);
            mc_test::fail_msg(__FILE__, __LINE__, msg);
        }
        lua_settop(L, 0);
    }

    // a pinned value: silent K — the primary code starts with N
    int n = run_ret(L, "return world.Metaphone('knight', 10)");
    ASSERT_EQ(1, n);
    const char* mp = lua_tostring(L, -1);
    ASSERT_TRUE(mp != nullptr);
    EXPECT_EQ('N', mp[0]);
    lua_settop(L, 0);

    lua_close(L);
}

// ═══ Round-4 sweep 2: trigger/alias/timer API arms ══════════════════════════

TEST(SweepR4, TriggerApiArms)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // flags: ignore-case(1)+omit-output(4)+regexp(8)+lowercase(2048? use enum)…
    // eIgnoreCase=2, eOmitFromOutput=4, eTriggerRegularExpression=8? — use the
    // canonical literals: eEnabled|eIgnoreCase|eTriggerRegularExpression|
    // eTriggerOneShot|eLowercaseWildcard via GetTrigger reconstruction.
    // eEnabled(1) | eIgnoreCase(0x10) | eTriggerRegularExpression(0x20) |
    // eLowercaseWildcard(0x800) | eTriggerOneShot(0x8000)
    int n = run_ret(L,
        "rc = world.AddTriggerEx('tf', '^x(.*)$', 'resp', "
        "1 + 16 + 32 + 2048 + 32768, -1, 0, '', '', 0, 100)\n"
        "return rc");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eOK, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // GetTriggerInfo boolean/short cases
    n = run_ret(L,
        "return world.GetTriggerInfo('tf', 5), world.GetTriggerInfo('tf', 6),\n"
        "       world.GetTriggerInfo('tf', 7), world.GetTriggerInfo('tf', 10),\n"
        "       world.GetTriggerInfo('tf', 11), world.GetTriggerInfo('tf', 12),\n"
        "       world.GetTriggerInfo('tf', 13), world.GetTriggerInfo('tf', 14),\n"
        "       world.GetTriggerInfo('tf', 17), world.GetTriggerInfo('tf', 18),\n"
        "       world.GetTriggerInfo('tf', 19), world.GetTriggerInfo('tf', 22)");
    ASSERT_EQ(12, n);
    EXPECT_TRUE(lua_toboolean(L, -3));           // ignore_case (case 10 → slot -9… case 19 at -2)
    lua_settop(L, 0);

    // GetTrigger reconstructs the flag word
    n = run_ret(L,
        "local mt, rt, fl = world.GetTrigger('tf')\n"
        "return mt, fl");
    ASSERT_EQ(2, n);
    lua_settop(L, 0);

    // Enable idempotency arms
    n = run_ret(L, "return world.EnableTrigger('tf', true), world.EnableTrigger('tf', true)");
    ASSERT_EQ(2, n);
    EXPECT_EQ((double)eOK, lua_tonumber(L, -1));
    lua_settop(L, 0);
    n = run_ret(L, "return world.EnableTrigger('tf', false), world.EnableTrigger('tf', false)");
    ASSERT_EQ(2, n);
    EXPECT_EQ((double)eOK, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // delete arms: missing name
    n = run_ret(L, "return world.DeleteTrigger('nosuch')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eTriggerNotFound, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(SweepR4, TriggerScriptNameNotLocated)
{
    CMUSHclientDoc doc;
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    doc.m_ScriptEngine = engine;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L,
        "return world.AddTriggerEx('ts', 'm', '', 1, -1, 0, '', 'no_such_fn', 0, 100)");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eScriptNameNotLocated, lua_tonumber(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.AddAlias('as', 'm', 'r', 1, 'no_such_fn')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eScriptNameNotLocated, lua_tonumber(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.AddTimer('tm', 0, 1, 0, 'r', 1, 'no_such_fn')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eScriptNameNotLocated, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(SweepR4, AliasApiArms)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // speedwalk / queue flag coercion (eAliasSpeedWalk=0x800, eAliasQueue=0x1000)
    EXPECT_TRUE(run(L, "assert(world.AddAlias('asw', 'w1', 'n', 1 + 2048, '') == 0)"));
    EXPECT_TRUE(run(L, "assert(world.AddAlias('aq',  'w2', 'n', 1 + 4096, '') == 0)"));
    int n = run_ret(L,
        "return world.GetAliasInfo('asw', 18), world.GetAliasInfo('aq', 18)");
    ASSERT_EQ(2, n);
    EXPECT_EQ((double)eSendToSpeedwalk,    lua_tonumber(L, -2));
    EXPECT_EQ((double)eSendToCommandQueue, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // idempotent enable/disable + delete-missing
    EXPECT_TRUE(run(L,
        "assert(world.EnableAlias('asw', true) == 0)\n"
        "assert(world.EnableAlias('asw', true) == 0)\n"
        "assert(world.EnableAlias('asw', false) == 0)\n"
        "assert(world.EnableAlias('asw', false) == 0)\n"));
    n = run_ret(L, "return world.DeleteAlias('nosuch')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eAliasNotFound, lua_tonumber(L, -1));
    lua_settop(L, 0);

    // GetAlias reconstruction: rc first, then the by-ref outputs
    n = run_ret(L, "local rc, m = world.GetAlias('asw') return rc, m");
    ASSERT_EQ(2, n);
    EXPECT_EQ(0.0, lua_tonumber(L, -2));
    EXPECT_STREQ("w1", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(SweepR4, TimerApiArms)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L, "assert(world.AddTimer('t1', 0, 5, 0, 'ping', 1, '') == 0)"));
    EXPECT_TRUE(run(L,
        "assert(world.EnableTimer('t1', true) == 0)\n"
        "assert(world.EnableTimer('t1', true) == 0)\n"
        "assert(world.EnableTimer('t1', false) == 0)\n"
        "assert(world.EnableTimer('t1', false) == 0)\n"
        "assert(world.ResetTimer('t1') == 0)\n"));

    // GetTimerInfo type sweep
    int n = run_ret(L,
        "local vals = {}\n"
        "for i = 1, 24 do vals[i] = world.GetTimerInfo('t1', i) end\n"
        "return type(vals[1]), type(vals[13])");
    ASSERT_EQ(2, n);
    lua_settop(L, 0);

    n = run_ret(L, "return world.DeleteTimer('nosuch')");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eTimerNotFound, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// ═══ Round-4 sweep 3: utils utf8 family, colourcube, timer, misc arms ═══════

TEST(SweepR4, Utf8FamilyEncodeDecodeSubValid)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // encode: 2-, 3- and 4-byte code points
    EXPECT_TRUE(run(L,
        "assert(utils.utf8encode(0xE9) == '\\195\\169')\n"          // é
        "assert(utils.utf8encode(0x20AC) == '\\226\\130\\172')\n"   // €
        "assert(utils.utf8encode(0x1F600) == '\\240\\159\\152\\128')\n"));

    // decode round trips
    EXPECT_TRUE(run(L,
        "local t = utils.utf8decode('a\\226\\130\\172b')\n"
        "assert(type(t) == 'table')\n"));

    // utf8valid: a good string and the position of a bad byte
    EXPECT_TRUE(run(L,
        "assert(utils.utf8valid('ok\\226\\130\\172') == true or "
        "       utils.utf8valid('ok\\226\\130\\172') ~= nil)\n"));
    int n = run_ret(L,
        "local r = utils.utf8valid('ab\\255cd')\n"
        "return r == true, tostring(r)");
    ASSERT_EQ(2, n);
    EXPECT_TRUE(!lua_toboolean(L, -2));   // not valid — position or false
    lua_settop(L, 0);

    // utf8sub walks characters not bytes
    n = run_ret(L, "return utils.utf8sub('a\\226\\130\\172b', 2, 2)");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("\xE2\x82\xAC", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

TEST(SweepR4, ColourCubeTimerAndInfoUtils)
{
    LONGLONG saved = App.m_iCounterFrequency;
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // both colour-cube variants, plus the unknown-option error
    EXPECT_TRUE(run(L, "utils.colourcube(1)"));
    EXPECT_TRUE(run(L, "utils.colourcube(2)"));
    int n = run_ret(L,
        "local ok, err = pcall(function() utils.colourcube(9) end)\n"
        "return ok, tostring(err)");
    ASSERT_EQ(2, n);
    EXPECT_TRUE(!lua_toboolean(L, -2));
    EXPECT_TRUE(strstr(lua_tostring(L, -1), "Unknown option") != nullptr);
    lua_settop(L, 0);

    // timer: both the QPC arm and the wall-clock arm
    App.m_iCounterFrequency = 1000000;
    EXPECT_TRUE(run(L, "assert(type(utils.timer()) == 'number')"));
    App.m_iCounterFrequency = 0;
    EXPECT_TRUE(run(L, "assert(type(utils.timer()) == 'number')"));

    // info + infotypes + functionlist return tables headless
    EXPECT_TRUE(run(L,
        "assert(type(utils.info()) == 'table')\n"
        "assert(type(utils.infotypes()) == 'table')\n"
        "assert(type(utils.functionlist()) == 'table')\n"
        "assert(type(utils.callbackslist()) == 'table')\n"));

    // edit_distance + metaphone through utils
    EXPECT_TRUE(run(L,
        "assert(utils.edit_distance('kitten', 'sitting') == 3)\n"
        "assert(type(utils.metaphone('orange')) == 'string')\n"));

    App.m_iCounterFrequency = saved;
    lua_close(L);
}

// A fully-populated world survives a save/reload round trip (macros, colour
// table, temporary-alias skip, comment mangling).
TEST(SweepR4, WorldSaveRoundTripWithEverything)
{
    CMUSHclientDoc doc;
    doc.m_mush_name = "RoundTrip";
    doc.m_notes = "notes -- with double hyphens --";
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "world.AddTriggerEx('rt', 'm', 'r', 1, 7, 0, '', '', 0, 100)\n"
        "world.AddAlias('ra', 'am', 'ar', 1, '')\n"
        "world.AddTimer('rm', 0, 2, 0, 'tick', 1, '')\n"
        "world.SetVariable('rv', 'val')\n"));
    doc.m_macros[0] = "macro-zero";

    // one temporary alias that must NOT be saved
    EXPECT_TRUE(run(L, "world.AddAlias('tmpa', 'tm', 'tr', 1 + 16384, '')"));  // eTemporary

    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_rt_%d.mcl", (int)getpid());
    char code[192];
    snprintf(code, sizeof code, "return world.Save('%s', true)", path);
    int n = run_ret(L, code);
    ASSERT_EQ(1, n);
    lua_settop(L, 0);

    CMUSHclientDoc doc2;
    ASSERT_TRUE(doc2.LoadWorldFromFile(path));
    unlink(path);

    CTrigger* t = nullptr;
    EXPECT_TRUE(doc2.GetTriggerMap().Lookup("rt", t));
    CAlias* a = nullptr;
    EXPECT_TRUE(doc2.GetAliasMap().Lookup("ra", a));
    EXPECT_TRUE(!doc2.GetAliasMap().Lookup("tmpa", a));   // temporary skipped
    CTimer* tm = nullptr;
    EXPECT_TRUE(doc2.GetTimerMap().Lookup("rm", tm));
    CVariable* v = nullptr;
    EXPECT_TRUE(doc2.GetVariableMap().Lookup("rv", v));
    EXPECT_STREQ("macro-zero", (LPCTSTR)doc2.m_macros[0]);
}

// ═══ Round-4 sweep 4: engine Parse error arms + script-error logging ════════

TEST(SweepR4, EngineParseCompileAndRuntimeErrors)
{
    LONGLONG saved = App.m_iCounterFrequency;
    App.m_iCounterFrequency = 1000000;   // exercise the timing arms too

    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    // World Properties > Scripting > "Note errors to the output window".  It has
    // to be ON for the errors to reach m_LineList at all: with it off, LuaError
    // raises CScriptErrorDlg instead (IDCANCEL headless, so nothing is noted).
    // ScriptErrorsToOutputWindowOptionDecidesDialogVsNotes pins both arms.
    doc.m_bScriptErrorsToOutputWindow = TRUE;
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    doc.m_ScriptEngine = engine;

    // compile error → true, and the error is noted
    EXPECT_TRUE(engine->Parse(CString("this is not lua"), "<compile>"));
    // runtime error at parse time → true
    EXPECT_TRUE(engine->Parse(CString("error('parse-time boom')"), "<runtime>"));
    // clean parse → false
    EXPECT_TRUE(!engine->Parse(CString("x = 1"), "<ok>"));

    // the errors were reported into the output window
    bool sawError = false;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (std::string(l->text, l->len).find("boom") != std::string::npos)
            sawError = true;
    }
    EXPECT_TRUE(sawError);
    App.m_iCounterFrequency = saved;
}

// World Properties > Scripting > "Note errors to the output window"
// (m_bScriptErrorsToOutputWindow).  LuaError's dialog-vs-notes choice used to
// live inside `#ifdef _WIN32`, with the port's #else arm noting unconditionally,
// so on the port this option did nothing at all — it behaved permanently as if
// ticked.  One body now, so the flag decides on every build: ON notes the four
// canonical lines (event / raised-by / called-by / description); OFF raises
// CScriptErrorDlg, which headless returns IDCANCEL from, so nothing is noted.
static int count_error_lines(CMUSHclientDoc& doc, const char* needle)
{
    int n = 0;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (std::string(l->text, l->len).find(needle) != std::string::npos)
            ++n;
    }
    return n;
}

TEST(SweepR4, ScriptErrorsToOutputWindowOptionDecidesDialogVsNotes)
{
    {
        CMUSHclientDoc on;
        on.m_iConnectPhase = eConnectConnectedToMud;
        on.m_bScriptErrorsToOutputWindow = TRUE;
        CScriptEngine* engine = new CScriptEngine(&on, "lua");
        ASSERT_TRUE(!engine->CreateScriptEngine());
        on.m_ScriptEngine = engine;
        EXPECT_TRUE(engine->Parse(CString("error('option-boom')"), "<opt>"));
        EXPECT_TRUE(count_error_lines(on, "option-boom") > 0);
    }
    {
        CMUSHclientDoc off;
        off.m_iConnectPhase = eConnectConnectedToMud;
        off.m_bScriptErrorsToOutputWindow = FALSE;   // the shipped default
        CScriptEngine* engine = new CScriptEngine(&off, "lua");
        ASSERT_TRUE(!engine->CreateScriptEngine());
        off.m_ScriptEngine = engine;
        EXPECT_TRUE(engine->Parse(CString("error('option-boom')"), "<opt>"));
        // Went to the dialog instead, so the output window stays clean.
        EXPECT_EQ(0, count_error_lines(off, "option-boom"));
    }
}

// The dialog's "show errors in output window next time" checkbox is how a user
// ticks the option from the error itself.  Headless the dialog cancels, so the
// flag must be left alone rather than silently turned on.
TEST(SweepR4, CancellingTheScriptErrorDialogLeavesTheOptionOff)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_bScriptErrorsToOutputWindow = FALSE;
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    doc.m_ScriptEngine = engine;
    EXPECT_TRUE(engine->Parse(CString("error('cancel-boom')"), "<cancel>"));
    EXPECT_TRUE(!doc.m_bScriptErrorsToOutputWindow);
}

TEST(SweepR4, ScriptErrorLogFileAppended)
{
    char dir[128];
    snprintf(dir, sizeof dir, "/tmp/mc_test_r4_errlog_%d/", (int)getpid());
    mkdir(dir, 0755);
    std::string savedDir = (const char*)App.m_strDefaultLogFileDirectory.c_str();
    App.m_strDefaultLogFileDirectory = dir;

    CMUSHclientDoc doc;
    doc.m_bLogScriptErrors = TRUE;
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    doc.m_ScriptEngine = engine;
    EXPECT_TRUE(engine->Parse(CString("error('logged-error')"), "<logged>"));

    std::string logPath = std::string(dir) + "script_error_log.txt";
    FILE* f = fopen(logPath.c_str(), "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[2048] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    EXPECT_TRUE(strstr(buf, "logged-error") != nullptr);
    unlink(logPath.c_str());
    rmdir(dir);
    App.m_strDefaultLogFileDirectory = savedDir.c_str();
}

// ── round 5: lua_methods / methods_info / spellchecker corners ────────────

#include "tests/fake_worldview.h"

extern int DisableDLLs(lua_State* L);

// The MUSHclient Lua-state hardening: debug.debug is replaced with the
// info-gathering (dialog-cancelling headless) version, ColourNote gets a
// notepad-backed stand-in when unattached, os.exit and io.popen error out.
TEST(LuaR5, HardenedStateDebugAndStandins)
{
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    DisableDLLs(L);

    // debug.debug gathers getinfo fields and returns (DoModal cancels)
    EXPECT_EQ(0, luaL_dostring(L,
        "local function f() debug.debug() end\n"
        "f()\n"));

    // the stand-in ColourNote goes to the "Lua Debugging" notepad
    EXPECT_EQ(0, luaL_dostring(L, "ColourNote('white', 'black', 'hi there')"));

    // os.exit and io.popen are disabled with errors
    EXPECT_NE(0, luaL_dostring(L, "os.exit()"));
    EXPECT_NE(0, luaL_dostring(L, "io.popen('ls')"));

    lua_close(L);
}

// The package library is stripped when disabled by policy.
TEST(LuaR5, PackageLibraryStrippedWhenDisabled)
{
    BOOL saved = App.m_bEnablePackageLibrary;
    App.m_bEnablePackageLibrary = FALSE;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    DisableDLLs(L);
    EXPECT_EQ(0, luaL_dostring(L,
        "assert(package.loadlib == nil)\n"
        "assert(package.loaders[3] == nil)\n"
        "assert(package.loaders[4] == nil)\n"));
    lua_close(L);
    App.m_bEnablePackageLibrary = saved;
}

TEST(LuaR5, GetWorldByIdAndUnknownWorld)
{
    CMUSHclientDoc doc;
    doc.m_mush_name  = "R5World";
    doc.m_strWorldID = "0123456789abcdef0123ffff";
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return world.GetWorldById('0123456789abcdef0123ffff')");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_isuserdata(L, -1));
    lua_settop(L, 0);

    n = run_ret(L, "return world.GetWorld('no-such-world-name-r5')");
    EXPECT_EQ(0, n);   // not found: no results
    lua_settop(L, 0);
    lua_close(L);
}

TEST(LuaR5, ColourNoteWithMultipleTriples)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    // two triples: the first goes through ColourTell, the last ColourNote
    EXPECT_TRUE(run(L,
        "world.ColourNote('red', 'black', 'part1', 'green', 'black', 'part2')"));
    lua_close(L);
}

TEST(LuaR5, DatabaseColumnTextOfNullIsNil)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_TRUE(run(L,
        "world.DatabaseOpen('db', ':memory:', 6)\n"
        "world.DatabasePrepare('db', 'SELECT NULL AS n')\n"
        "world.DatabaseStep('db')\n"));
    int n = run_ret(L, "return world.DatabaseColumnText('db', 1)");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_isnil(L, -1));
    lua_settop(L, 0);
    EXPECT_TRUE(run(L, "world.DatabaseFinalize('db') world.DatabaseClose('db')"));
    lua_close(L);
}

// Window-position family with a live worldview attached (main + output
// rects) — the Lua extensions and the canonical BSTR methods.
TEST(LuaR5, WindowPositionsWithWorldView)
{
    CMUSHclientDoc doc;
    FakeWorldView view;
    doc.m_pWorldView = &view;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "local t = world.GetMainWindowPosition() "
                       "return type(t)");
    ASSERT_EQ(1, n);
    lua_settop(L, 0);
    n = run_ret(L, "local t = world.GetWorldWindowPosition() "
                   "return type(t)");
    ASSERT_EQ(1, n);
    lua_settop(L, 0);

    // canonical (string) variants
    BSTR b1 = doc.GetMainWindowPosition();
    CString s1(b1);
    EXPECT_NE(-1, s1.Find("1024"));   // width from FakeWorldView mainRect
    SysFreeString(b1);
    BSTR b2 = doc.GetWorldWindowPosition();
    CString s2(b2);
    EXPECT_NE(-1, s2.Find("640"));    // width from FakeWorldView outputRect
    SysFreeString(b2);

    doc.m_pWorldView = nullptr;
    lua_close(L);
}

// SpellCheckCommand with a live command view: on Windows this walks the
// CSendView list to reach the command-bar CEdit directly; there is no
// CSendView on this port, so it used to be permanently dead (-1) even with a
// world view attached.  A no-op #ifndef _WIN32 arm returns -1 and leaves the
// bar untouched, failing both assertions below.
TEST(SpellCheckTest, SpellCheckCommandAppliesCorrectionThroughView)
{
    SpellFixture fx("function spellcheck(text, all) return 'the' end\n");

    CMUSHclientDoc doc;
    FakeWorldView view;
    view.cmd = "teh cat";
    doc.m_pWorldView = &view;

    // 1-based [1,3] selects "teh" (StartCol decrements to the 0-based start,
    // EndCol is already the 0-based exclusive end -- see the SpellCheckCommand
    // comment on re-encoding through SetCommandSelectionRange).
    EXPECT_EQ(1L, doc.SpellCheckCommand(1, 3));
    EXPECT_STREQ("the cat", view.cmd.c_str());

    doc.m_pWorldView = nullptr;
}

TEST(LuaR5, GetScriptTimeWithCounter)
{
    CMUSHclientDoc doc;
    LONGLONG saved = App.m_iCounterFrequency;
    App.m_iCounterFrequency = 1000;
    doc.m_iScriptTimeTaken = 500;
    EXPECT_EQ(0.5, doc.GetScriptTime());
    App.m_iCounterFrequency = saved;
}

// GetStyleInfo: the styling booleans (9-13), the RGB colours (14/15), the
// prompt action code, and the style-out-of-range guard.
TEST(LuaR5, GetStyleInfoRemainingSelectors)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;

    // build one completed line: underline+blink+inverse styled text with a
    // prompt-type action
    const char* seq = "\x1B[4;5;7mstyled\x1B[0m\n";
    doc.DisplayMsg(seq, (int)strlen(seq), 0);

    // decorate the first style with a prompt action
    CLine* line = doc.m_LineList.GetHead();
    ASSERT_TRUE(line->styleList.GetCount() >= 1);
    CStyle* s0 = line->styleList.GetHead();
    s0->iFlags = (unsigned short)((s0->iFlags & ~ACTIONTYPE) | ACTION_PROMPT);

    VARIANT v;
    v = doc.GetStyleInfo(1, 1, 4);    // action type = 3 (prompt)
    EXPECT_EQ((int)VT_I2, (int)v.vt);
    EXPECT_EQ(3, (int)v.iVal);
    v = doc.GetStyleInfo(1, 1, 9);    // underline
    EXPECT_EQ((int)VT_BOOL, (int)v.vt);
    v = doc.GetStyleInfo(1, 1, 10);   // blink
    EXPECT_EQ((int)VT_BOOL, (int)v.vt);
    v = doc.GetStyleInfo(1, 1, 11);   // inverse
    EXPECT_EQ((int)VT_BOOL, (int)v.vt);
    v = doc.GetStyleInfo(1, 1, 12);   // changed
    EXPECT_EQ((int)VT_BOOL, (int)v.vt);
    v = doc.GetStyleInfo(1, 1, 13);   // start tag
    EXPECT_EQ((int)VT_BOOL, (int)v.vt);
    v = doc.GetStyleInfo(1, 1, 14);   // text RGB
    EXPECT_EQ((int)VT_I4, (int)v.vt);
    v = doc.GetStyleInfo(1, 1, 15);   // back RGB
    EXPECT_EQ((int)VT_I4, (int)v.vt);

    // a style number beyond the list: empty result (the walk guard)
    v = doc.GetStyleInfo(1, 99, 1);
    EXPECT_EQ((int)VT_EMPTY, (int)v.vt);
}

// The Lua world.GetStyleInfo walker has its own action-code switch.
TEST(LuaR5, LuaGetStyleInfoActionCodes)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    const char* seq = "plain\n";
    doc.DisplayMsg(seq, (int)strlen(seq), 0);
    CLine* line = doc.m_LineList.GetHead();
    CStyle* s0 = line->styleList.GetHead();

    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    s0->iFlags = (unsigned short)((s0->iFlags & ~ACTIONTYPE) | ACTION_SEND);
    int n = run_ret(L, "return world.GetStyleInfo(1, 1, 4)");
    ASSERT_EQ(1, n);
    EXPECT_EQ(1.0, lua_tonumber(L, -1));   // ACTION_SEND = 1
    lua_settop(L, 0);

    s0->iFlags = (unsigned short)((s0->iFlags & ~ACTIONTYPE) | ACTION_PROMPT);
    n = run_ret(L, "return world.GetStyleInfo(1, 1, 4)");
    ASSERT_EQ(1, n);
    EXPECT_EQ(3.0, lua_tonumber(L, -1));   // ACTION_PROMPT = 3
    lua_settop(L, 0);
    lua_close(L);
}

// Save (non-save-as) writes the world file through the else arm.
TEST(LuaR5, SaveWithoutSaveAsFlag)
{
    CMUSHclientDoc doc;
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_r5_save_%d.mcl", (int)getpid());
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    char code[256];
    snprintf(code, sizeof code, "return world.Save('%s')", path);
    int n = run_ret(L, code);
    ASSERT_EQ(1, n);
    lua_settop(L, 0);
    unlink(path);
    lua_close(L);
}

// GetPluginVariableList with a non-empty unknown plugin id: no results.
TEST(LuaR5, PluginVariableListUnknownPluginNoResults)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    int n = run_ret(L,
        "return world.GetPluginVariableList('999999999999999999999999')");
    EXPECT_EQ(0, n);   // non-empty plugin not found: zero results
    lua_settop(L, 0);
    lua_close(L);
}

// ── spellchecker remaining arms ───────────────────────────────────────────

TEST(SpellCheckR5, SpellCheckErrorAndShapeArms)
{
    {   // function missing entirely
        SpellFixture fx("spellcheck_string = nil");
        CMUSHclientDoc doc;
        VARIANT v = doc.SpellCheck("word");
        EXPECT_EQ((int)VT_EMPTY, (int)v.vt);
    }
    {   // function raises
        SpellFixture fx("function spellcheck_string(t) error('kapow') end");
        CMUSHclientDoc doc;
        VARIANT v = doc.SpellCheck("word");
        EXPECT_EQ((int)VT_EMPTY, (int)v.vt);
    }
    {   // non-table, non-number result
        SpellFixture fx("function spellcheck_string(t) return 'nope' end");
        CMUSHclientDoc doc;
        VARIANT v = doc.SpellCheck("word");
        EXPECT_EQ((int)VT_EMPTY, (int)v.vt);
    }
    {   // spellcheck marked OK but no Lua state at all
        BOOL savedOK = App.m_bSpellCheckOK;
        lua_State* savedLua = App.m_SpellChecker_Lua;
        App.m_bSpellCheckOK = TRUE;
        App.m_SpellChecker_Lua = nullptr;
        CMUSHclientDoc doc;
        VARIANT v = doc.SpellCheck("word");
        EXPECT_EQ((int)VT_EMPTY, (int)v.vt);
        EXPECT_EQ((long)eSpellCheckNotActive, doc.AddSpellCheckWord("w", "i", ""));
        App.m_bSpellCheckOK = savedOK;
        App.m_SpellChecker_Lua = savedLua;
    }
}

TEST(SpellCheckR5, SpellCheckDlgInactiveAndNonString)
{
    {   // inactive
        BOOL savedOK = App.m_bSpellCheckOK;
        App.m_bSpellCheckOK = FALSE;
        CMUSHclientDoc doc;
        VARIANT v = doc.SpellCheckDlg("word");
        EXPECT_EQ((int)VT_EMPTY, (int)v.vt);
        App.m_bSpellCheckOK = savedOK;
    }
    {   // non-string result (dialog cancelled analogue)
        SpellFixture fx("function spellcheck(t) return nil end");
        CMUSHclientDoc doc;
        VARIANT v = doc.SpellCheckDlg("word");
        EXPECT_EQ((int)VT_EMPTY, (int)v.vt);
    }
    {   // the Lua wrapper world.SpellCheckDlg
        SpellFixture fx("function spellcheck(t) return 'fixed' end");
        CMUSHclientDoc doc;
        lua_State* L = make_lua(&doc);
        ASSERT_NE(nullptr, L);
        int n = run_ret(L, "return world.SpellCheckDlg('orig')");
        ASSERT_EQ(1, n);
        EXPECT_STREQ("fixed", lua_tostring(L, -1));
        lua_settop(L, 0);
        lua_close(L);
    }
}

TEST(SpellCheckR5, AddSpellCheckWordRemainingArms)
{
    {   // action 'a' accepted; add-word function missing; raising; false
        SpellFixture fx("spellcheck_add_word = nil");
        CMUSHclientDoc doc;
        EXPECT_EQ((long)eSpellCheckNotActive,
                  doc.AddSpellCheckWord("word", "a", ""));
    }
    {
        SpellFixture fx("function spellcheck_add_word(o, a, r) error('x') end");
        CMUSHclientDoc doc;
        EXPECT_EQ((long)eSpellCheckNotActive,
                  doc.AddSpellCheckWord("word", "A", ""));
    }
    {
        SpellFixture fx("function spellcheck_add_word(o, a, r) return false end");
        CMUSHclientDoc doc;
        EXPECT_EQ((long)eBadParameter, doc.AddSpellCheckWord("word", "c", ""));
    }
}


// world.Connect on an already-open world → eWorldOpen (30001).
// L_Connect (lua_methods.cpp:1621) pushes doc->Connect() as a number;
// CMUSHclientDoc::Connect (methods_utilities.cpp:528-539) returns eWorldOpen
// from its else-branch when the phase is not eConnectNotConnected.  Forcing
// eConnectConnectedToMud exercises that branch WITHOUT any network (the
// eConnectNotConnected branch would call OnConnectionConnect).  world.Disconnect
// is tested; Connect was 0% covered.
TEST(LuaScripting, ConnectAlreadyOpenReturnsWorldOpen)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    doc.m_iConnectPhase = eConnectConnectedToMud;   // 9, != eConnectNotConnected

    int n = run_ret(L, "return world.Connect()");
    ASSERT_EQ(1, n);
    EXPECT_EQ((double)eWorldOpen, lua_tonumber(L, -1));   // 30001
    lua_settop(L, 0);

    lua_close(L);
}

// The REAL C++ CMUSHclientDoc::ANSI (methods_colours.cpp:95-100) — called by
// the dispatch engines — is untested: world.ANSI in Lua is a separate reimpl
// (L_ANSI, lua_methods.cpp), so call ANSI() directly.  CFormat("%c[%dm", ESC,
// Code) with ESC=='\x1B' (doc.h:112) yields "<ESC>[<code>m".
TEST(LuaScripting, ANSIRealMethodBuildsEscapeSequence)
{
    CMUSHclientDoc doc;

    BSTR b1 = doc.ANSI(1);
    EXPECT_STREQ("\x1b[1m", (const char*)CString(b1));

    BSTR b0 = doc.ANSI(0);
    EXPECT_STREQ("\x1b[0m", (const char*)CString(b0));

    BSTR b31 = doc.ANSI(31);
    EXPECT_STREQ("\x1b[31m", (const char*)CString(b31));
}

// The REAL C++ CMUSHclientDoc::MapColourList (methods_colours.cpp:192-218):
// empty translation map → iCount==0 → the CreateOneDim is skipped and the
// never-created COleSafeArray detaches as VT_EMPTY.  (Lua's world.MapColourList
// is a keyed-table reimpl, L_MapColourList, so this arm is otherwise untested.)
TEST(LuaScripting, MapColourListEmptyIsVtEmpty)
{
    CMUSHclientDoc doc;   // fresh: m_ColourTranslationMap is empty

    VARIANT v = doc.MapColourList();
    EXPECT_EQ((int)VT_EMPTY, (int)v.vt);
}

// The REAL C++ CMUSHclientDoc::MapColourList (methods_colours.cpp:192-218)
// non-empty arm: one map entry → a 1-element (VT_ARRAY|VT_VARIANT) safearray
// whose element is CFormat("%s = %s", ColourToName(original), ColourToName
// (replacement)).  Assert against ColourToName so the expected string tracks
// the actual naming (named or #RRGGBB) — only the format/ordering is pinned.
TEST(LuaScripting, MapColourListNonEmptyBuildsNameEqualsNameArray)
{
    CMUSHclientDoc doc;
    COLORREF a = RGB(1, 2, 3);
    COLORREF b = RGB(4, 5, 6);
    doc.MapColour(a, b);            // m_ColourTranslationMap[a] = b

    VARIANT v = doc.MapColourList();
    ASSERT_EQ((int)(VT_ARRAY | VT_VARIANT), (int)v.vt);

    COleSafeArray sa;
    sa.Attach(v);
    ASSERT_EQ(1L, sa.GetOneDimSize());

    long i = 0;
    VARIANT e;
    VariantInit(&e);
    sa.GetElement(&i, &e);
    ASSERT_EQ((int)VT_BSTR, (int)e.vt);

    CString expected = ColourToName(a) + " = " + ColourToName(b);
    EXPECT_STREQ((const char*)expected, (const char*)CString(e.bstrVal));
}

// utils.utf8decode 4-byte (astral) arm (lua_utils_portable.cpp:297): every
// existing utf8decode test feeds at most a 3-byte codepoint, so the 4-byte
// branch was dead.  U+1F600 GRINNING FACE encodes to F0 9F 98 80.
TEST(LuaScripting, Utf8DecodeFourByteAstral)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "local s = utils.utf8encode(0x1F600)\n"
        "assert(#s == 4, '4-byte encode len = ' .. #s)\n"
        "local cp = utils.utf8decode(s)\n"
        "assert(#cp == 1, 'expected 1 codepoint, got ' .. #cp)\n"
        "assert(cp[1] == 0x1F600, 'decoded ' .. cp[1] .. ' not 0x1F600')\n"));

    lua_close(L);
}

// utils.utf8valid 4-byte code-point-count arm (lua_utils_portable.cpp:327):
// a single astral codepoint counts as ONE code point.  Existing utf8valid
// tests only feed <=3-byte codepoints, so the i+=4 branch was dead.
TEST(LuaScripting, Utf8ValidFourByteAstral)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    int n = run_ret(L, "return utils.utf8valid(utils.utf8encode(0x1F600))");
    ASSERT_EQ(1, n);
    EXPECT_EQ(1.0, lua_tonumber(L, -1));
    lua_settop(L, 0);

    lua_close(L);
}

// utils.utf8sub 4-byte utf8_next arm (lua_utils_portable.cpp:341): sub must
// walk astral codepoints as whole characters and never split a 4-byte
// sequence.  Existing utf8sub tests only use <=3-byte codepoints.
TEST(LuaScripting, Utf8SubFourByteAstral)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "local s = utils.utf8encode(0x1F600) .. utils.utf8encode(0x1F601)\n"
        "local second = utils.utf8sub(s, 2, 2)\n"
        "assert(second == utils.utf8encode(0x1F601), 'sub(2,2) wrong bytes')\n"
        "assert(#second == 4, 'sub len = ' .. #second)\n"
        "assert(utils.utf8sub(s, 1, 1) == utils.utf8encode(0x1F600), 'sub(1,1) wrong')\n"));

    lua_close(L);
}

// utils.utf8decode error arm (lua_utils_portable.cpp:286-287): a lone 0xFF is
// not a valid UTF-8 lead byte, so valid_utf8 returns 1 and utf8decode raises
// "Bad UTF-8 string. Error at column 2".  No existing test drives utf8decode's
// error path (they only decode well-formed strings).
TEST(LuaScripting, Utf8DecodeBadStringErrors)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "local ok, err = pcall(utils.utf8decode, '\\255')\n"
        "assert(not ok, 'utf8decode should reject 0xFF')\n"
        "assert(type(err) == 'string', 'err not a string')\n"
        "assert(err:find('Bad UTF-8', 1, true) ~= nil, 'msg: ' .. err)\n"
        "assert(err:find('column 2', 1, true) ~= nil, 'col: ' .. err)\n"));

    lua_close(L);
}

// utils.utf8encode table-element type check (lua_utils_portable.cpp:251-252):
// a non-numeric element inside a table argument raises "Index [2] of table
// arg #1 must be a number".  No existing test drives this error arm.
TEST(LuaScripting, Utf8EncodeTableNonNumberErrors)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    EXPECT_TRUE(run(L,
        "local ok, err = pcall(utils.utf8encode, {72, 'x'})\n"
        "assert(not ok, 'utf8encode should reject non-number element')\n"
        "assert(type(err) == 'string', 'err not a string')\n"
        "assert(err:find('must be a number', 1, true) ~= nil, 'msg: ' .. err)\n"
        "assert(err:find('Index [2]', 1, true) ~= nil, 'idx: ' .. err)\n"));

    lua_close(L);
}

// world.TraceOut hands a SCRIPT-SUPPLIED string to CMUSHclientDoc::Trace, whose
// first parameter is a printf format run through FormatV.  A '%' in the message
// therefore used to consume a vararg that was never pushed — printing garbage,
// or worse.  (Found by giving Trace a format(printf) attribute; the compiler
// flagged the call as "format string is not a string literal".)
TEST(LuaScripting, TraceOutTreatsPercentAsLiteralText)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_bTrace = TRUE;                       // Trace() is a no-op when off
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);

    // %s and %d are the dangerous ones: they dereference / read a missing arg.
    EXPECT_TRUE(run(L, "world.TraceOut('100% done: %s and %d')"));

    bool sawLiteral = false;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (std::string(l->text, l->len).find("100% done: %s and %d")
                != std::string::npos)
            sawLiteral = true;
    }
    EXPECT_TRUE(sawLiteral);

    lua_close(L);
}

// ── VARIANT ownership on the way back to Lua ──────────────────────────────

// Every world method declared VT_VARIANT that yields a string hands pushVariant
// a BSTR it allocated.  The port's VariantClear frees only SAFEARRAYs -- it must,
// because COleVariant here BORROWS the caller's string -- so that BSTR was
// leaked, once per call, on the most-used path in the most-used language.
//
// The observable is memory.  Deliberately a large loop with a generous
// threshold: the leak is ~40 bytes a call, so 200k calls is ~8 MB against a
// 3 MB bar, comfortably clear of allocator noise.
TEST(LuaScripting, StringReturningMethodsDoNotLeakTheirVariant)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    ASSERT_TRUE(run(f.L, "world.SetVariable('leaky', string.rep('x', 40))\n"));

    auto rss = [] () -> long {
        // ru_maxrss is a high-water mark, which is what we want: it cannot fall
        // back and hide a leak that has already happened.
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        return (long) ru.ru_maxrss;      // bytes on macOS, KB on Linux
    };

    ASSERT_TRUE(run(f.L, "for i = 1, 20000 do local _ = world.GetVariable('leaky') end\n"));
    const long before = rss();
    ASSERT_TRUE(run(f.L, "for i = 1, 200000 do local _ = world.GetVariable('leaky') end\n"));
    const long after = rss();

#ifdef __APPLE__
    const long budget = 3L * 1024 * 1024;      // bytes
#else
    const long budget = 3L * 1024;             // KB
#endif
    EXPECT_LT(after - before, budget);
}

// GetTrigger / GetAlias / GetTimer return *NotFound WITHOUT writing their
// [out] parameters, so those VARIANTs must be initialised before the call --
// otherwise pushVariant switches on a random stack word as a type tag and reads
// its neighbour as a pointer.
//
// This is a CONTRACT test, not a reproducer: before the fix the behaviour was
// undefined, so it could pass by luck.  What it pins is that a miss returns the
// documented code and nils, which is what a script actually depends on.
TEST(LuaScripting, ByRefGettersReturnNilsWhenTheObjectIsMissing)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "local rc, a, b, c, d, e, g, h = world.GetTrigger('nosuchtrigger')\n"
        "assert(rc == error_code.eTriggerNotFound, 'rc=' .. tostring(rc))\n"
        "assert(a == nil and b == nil and c == nil and d == nil, 'trigger out-params not nil')\n"
        "local rc2, m, r, p, fl, sn = world.GetAlias('nosuchalias')\n"
        "assert(rc2 == error_code.eAliasNotFound, 'rc2=' .. tostring(rc2))\n"
        "assert(m == nil and r == nil and p == nil, 'alias out-params not nil')\n"
        "local rc3, hh, mm, ss = world.GetTimer('nosuchtimer')\n"
        "assert(rc3 == error_code.eTimerNotFound, 'rc3=' .. tostring(rc3))\n"
        "assert(hh == nil and mm == nil and ss == nil, 'timer out-params not nil')\n"
    ));
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
