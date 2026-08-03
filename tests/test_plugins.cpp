// tests/test_plugins.cpp
// Intent-comment tests for the plugin subsystem: callback dispatch semantics
// (plugins.cpp), plugin lookup/lifecycle (methods_plugins.cpp), CallPlugin
// marshalling (lua_methods.cpp), state saving (CPlugin::SaveState) and the
// per-plugin Lua sandbox (lua_scripting.cpp).
//
// Real plugins are loaded through the REAL InternalLoadPlugin from temp XML
// files, exactly as the product does.

#include "mc_test.h"
// B-3 atomic flip: real headers (portable stub deleted; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "pal/itransport.h"
#include "scripting/scripting.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// ── helpers ───────────────────────────────────────────────────────────────

static void make_doc(CMUSHclientDoc& doc)
{
    doc.m_iConnectPhase = eConnectConnectedToMud;
}

// A bare (engine-less) plugin registered with the doc.
static CPlugin* add_bare_plugin(CMUSHclientDoc& doc, const char* id,
                                const char* name)
{
    CPlugin* p = new CPlugin(&doc);
    p->m_bSaveState = false;
    p->m_strID   = id;
    p->m_strName = name;
    doc.m_PluginList.push_back(p);
    return p;
}

// methods_plugins.cpp GetPlugin: "return if id wrong length" — only IDs of
// exactly PLUGIN_UNIQUE_ID_LENGTH (24) characters match.
// OnFilePluginwizard is a protected afx_msg handler; reach it via a derived
// using-declaration (legal member-pointer access, no doc.h change).
static void call_OnFilePluginwizard(CMUSHclientDoc& d)
{
    struct Access : CMUSHclientDoc { using CMUSHclientDoc::OnFilePluginwizard; };
    (d.*(&Access::OnFilePluginwizard))();
}

TEST(PluginCallback, PluginLookupRequiresExact24CharID)
{
    CMUSHclientDoc doc; make_doc(doc);
    add_bare_plugin(doc, "abcdefabcdefabcdefabcdef", "P");   // 24 chars

    EXPECT_TRUE(doc.IsPluginInstalled("abcdefabcdefabcdefabcdef"));
    EXPECT_FALSE(doc.IsPluginInstalled("abcdefabcdefabcdefabcde"));   // 23
    EXPECT_FALSE(doc.IsPluginInstalled("abcdefabcdefabcdefabcdef0")); // 25
    EXPECT_FALSE(doc.IsPluginInstalled(""));
}

// plugins.cpp ExecutePluginScript: "return true; assume they OK'd it" — a
// plugin that does not define the queried callback counts as approving.
TEST(PluginCallback, MissingSendCallbackCountsAsApproval)
{
    CMUSHclientDoc doc; make_doc(doc);
    add_bare_plugin(doc, "abcdefabcdefabcdefabcdef", "NoCallbacks");

    EXPECT_TRUE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, "a line", true));
}

#ifdef MUSHCLIENT_LUA_SCRIPTING

// Write a plugin XML file to /tmp and return its path.
static std::string write_plugin(const char* tag, const char* id,
                                const char* name, const char* script_body)
{
    static int s_seq = 0;
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_plugin_%d_%d_%s.xml",
             (int)getpid(), ++s_seq, tag);
    FILE* f = fopen(path, "wb");
    if (!f) return "";
    fprintf(f,
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE muclient>\n"
        "<muclient>\n"
        "<plugin name=\"%s\" author=\"T\" id=\"%s\" language=\"Lua\" "
        "version=\"1.0\" />\n"
        "<script><![CDATA[\n%s\n]]></script>\n"
        "</muclient>\n", name, id, script_body);
    fclose(f);
    return path;
}

static bool load_plugin(CMUSHclientDoc& doc, const std::string& path)
{
    try { doc.InternalLoadPlugin(CString(path.c_str())); return true; }
    catch (CException* e) { e->Delete(); return false; }
    catch (...)           { return false; }
}

static CString plugin_var(CPlugin* p, const char* name)
{
    CVariable* v = nullptr;
    if (!p || !p->m_VariableMap.Lookup(name, v) || !v)
        return CString("<unset>");
    return v->strContents;
}

static bool output_contains(CMUSHclientDoc& doc, const char* text)
{
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (CString(l->text, l->len).Find(text) != -1) return true;
    }
    return false;
}

#define IDA "aaaaaaaaaaaaaaaaaaaaaaa1"
#define IDB "bbbbbbbbbbbbbbbbbbbbbbb2"
#define IDC "ccccccccccccccccccccccc3"

// plugins.cpp: "this crazy code is to get 0x00 bytes into telnet
// subnegotiations ;)" — the (long, string) callback overload passes
// embedded NULs intact to OnPluginTelnetSubnegotiation.
TEST(PluginCallback, TelnetSubnegotiationStringPreservesEmbeddedNuls)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("nul", IDA, "NulPlugin",
        "function OnPluginTelnetSubnegotiation(msg, data)\n"
        "  world.SetVariable('len', tostring(#data))\n"
        "  local b = {}\n"
        "  for i = 1, #data do b[#b+1] = string.byte(data, i) end\n"
        "  world.SetVariable('bytes', table.concat(b, ','))\n"
        "end\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    doc.m_subnegotiation_type = 201;                       // e.g. GMCP
    doc.m_IAC_subnegotiation_data = std::string("a\0b\0c", 5);
    doc.m_phase = HAVE_SUBNEGOTIATION_IAC;
    doc.Phase_SUBNEGOTIATION_IAC(SE);

    EXPECT_STREQ("5", (const char*)plugin_var(p, "len"));
    EXPECT_STREQ("97,0,98,0,99", (const char*)plugin_var(p, "bytes"));
}

// lua_scripting.cpp: "I use number rather than boolean because 0 is
// considered true in Lua" — a callback returning the number 0 vetoes.
TEST(PluginCallback, LuaZeroNumberResultVetoesLikeFalse)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("zero", IDA, "ZeroPlugin",
        "function OnPluginSend(text) return 0 end\n")));

    EXPECT_FALSE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, "x", true));
}

// plugins.cpp SendToAllPluginCallbacks(bStopOnFalse): "we only return true
// if each plugin returned true" — any veto blackballs the action.
TEST(PluginCallback, AnyPluginFalseVetoesSendCallback)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("ok", IDA, "OkPlugin",
        "function OnPluginSend(text) return true end\n")));
    EXPECT_TRUE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, "x", true));

    ASSERT_TRUE(load_plugin(doc, write_plugin("veto", IDB, "VetoPlugin",
        "function OnPluginSend(text) return false end\n")));
    EXPECT_FALSE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, "x", true));
}

// lua_scripting.cpp: "if the function later has an error we set the DISPID
// to DISPID_UNKNOWN as a flag to not call it continuously" — after one
// run-time error the callback is never re-invoked.
TEST(PluginCallback, RuntimeErrorLatchesCallbackOff)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("err", IDA, "ErrPlugin",
        "count = 0\n"
        "function OnPluginSend(text)\n"
        "  count = count + 1\n"
        "  world.SetVariable('count', tostring(count))\n"
        "  error('deliberate failure')\n"
        "end\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, "x", true);   // errors once
    EXPECT_STREQ("1", (const char*)plugin_var(p, "count"));
    EXPECT_FALSE(p->m_PluginCallbacks[ON_PLUGIN_SEND].isvalid());  // latched

    doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, "x", true);   // skipped
    EXPECT_STREQ("1", (const char*)plugin_var(p, "count"));    // not re-run
}

// lua_scripting.cpp: "bugfix thanks to Twisol (previous version didn't
// handle bad debug table properly)" — an error is still handled cleanly
// when the script has clobbered the global debug table.
TEST(PluginCallback, LuaErrorHandledWhenDebugTableClobbered)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("dbg", IDA, "DbgPlugin",
        "debug = 42\n"                       // clobber the debug table
        "count = 0\n"
        "function OnPluginSend(text)\n"
        "  count = count + 1\n"
        "  world.SetVariable('count', tostring(count))\n"
        "  error('boom')\n"
        "end\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, "x", true);   // must not crash
    EXPECT_STREQ("1", (const char*)plugin_var(p, "count"));
    EXPECT_FALSE(p->m_PluginCallbacks[ON_PLUGIN_SEND].isvalid());
}

// methods_plugins.cpp BroadcastPlugin: "don't broadcast to ourselves -
// could cause indefinite loop" — and the return counts only OTHER plugins
// with a valid OnPluginBroadcast.
TEST(PluginCallback, BroadcastPluginSkipsSender)
{
    CMUSHclientDoc doc; make_doc(doc);
    // B: receives broadcasts
    ASSERT_TRUE(load_plugin(doc, write_plugin("bcastB", IDB, "BPlugin",
        "function OnPluginBroadcast(msg, id, name, text)\n"
        "  world.SetVariable('got', text .. '/' .. tostring(msg))\n"
        "end\n")));
    // A: broadcasts from its own script body (and has its own handler,
    // which must NOT be invoked)
    ASSERT_TRUE(load_plugin(doc, write_plugin("bcastA", IDA, "APlugin",
        "function OnPluginBroadcast(msg, id, name, text)\n"
        "  world.SetVariable('self', 'WRONGLY INVOKED')\n"
        "end\n"
        "world.SetVariable('rc', tostring(world.BroadcastPlugin(7, 'hello')))\n")));

    CPlugin* pa = doc.GetPlugin(IDA);
    CPlugin* pb = doc.GetPlugin(IDB);
    ASSERT_NE(nullptr, pa);
    ASSERT_NE(nullptr, pb);
    EXPECT_STREQ("hello/7", (const char*)plugin_var(pb, "got"));  // B received
    EXPECT_STREQ("<unset>", (const char*)plugin_var(pa, "self")); // A skipped
    EXPECT_STREQ("1", (const char*)plugin_var(pa, "rc"));         // only B counted
}

// plugins.cpp SendToFirstPluginCallbacks: "this is for when we want the
// first available plugin to handle something" — stops at the first plugin
// whose callback dispid is valid.
TEST(PluginCallback, SendToFirstStopsAtFirstPluginWithCallback)
{
    CMUSHclientDoc doc; make_doc(doc);
    // A has no OnPluginTrace; B and C both do
    ASSERT_TRUE(load_plugin(doc, write_plugin("first_a", IDA, "FirstA",
        "-- no trace handler\n")));
    ASSERT_TRUE(load_plugin(doc, write_plugin("first_b", IDB, "FirstB",
        "function OnPluginTrace(msg) world.SetVariable('traced', msg) end\n")));
    ASSERT_TRUE(load_plugin(doc, write_plugin("first_c", IDC, "FirstC",
        "function OnPluginTrace(msg) world.SetVariable('traced', msg) end\n")));

    EXPECT_TRUE(doc.SendToFirstPluginCallbacks(ON_PLUGIN_TRACE, "the message"));
    EXPECT_STREQ("the message", (const char*)plugin_var(doc.GetPlugin(IDB), "traced"));
    EXPECT_STREQ("<unset>",     (const char*)plugin_var(doc.GetPlugin(IDC), "traced"));

    // and false when NO plugin defines the callback
    CMUSHclientDoc doc2; make_doc(doc2);
    ASSERT_TRUE(load_plugin(doc2, write_plugin("first_none", IDA, "NoTrace",
        "-- nothing\n")));
    EXPECT_FALSE(doc2.SendToFirstPluginCallbacks(ON_PLUGIN_TRACE, "msg"));
}

// plugins.h m_strCallingPluginID: "during a CallPlugin - the ID of the
// calling plugin" — visible to the callee as GetPluginInfo(id, 23), and
// reverted once the call returns.
TEST(PluginCallback, CallPluginExposesCallerIDViaInfoType23)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("calleeB", IDB, "CalleeB",
        "function fn()\n"
        "  world.SetVariable('caller', tostring(\n"
        "      world.GetPluginInfo(world.GetPluginID(), 23)))\n"
        "end\n")));
    ASSERT_TRUE(load_plugin(doc, write_plugin("callerA", IDA, "CallerA",
        "world.SetVariable('rc', tostring(world.CallPlugin('" IDB "', 'fn')))\n")));

    CPlugin* pa = doc.GetPlugin(IDA);
    CPlugin* pb = doc.GetPlugin(IDB);
    ASSERT_NE(nullptr, pa);
    ASSERT_NE(nullptr, pb);
    EXPECT_STREQ("0", (const char*)plugin_var(pa, "rc"));       // eOK
    EXPECT_STREQ(IDA, (const char*)plugin_var(pb, "caller"));   // A's ID seen
    EXPECT_STREQ("", (const char*)pb->m_strCallingPluginID);    // reverted
}

// lua_methods.cpp L_CallPlugin: "new in 4.55: Lua to Lua calls can handle
// multiple arguments and multiple return values" — eOK first, then every
// value the routine returned.
TEST(LuaScripting, CallPluginReturnsOkThenAllRoutineValues)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("multiB", IDB, "MultiB",
        "function fn(x, y)\n"
        "  return 'r1', 2, x .. '!', y\n"
        "end\n")));
    ASSERT_TRUE(load_plugin(doc, write_plugin("multiA", IDA, "MultiA",
        "local rc, a, b, c, d = world.CallPlugin('" IDB "', 'fn', 'x', true)\n"
        "world.SetVariable('rc', tostring(rc))\n"
        "world.SetVariable('a', tostring(a))\n"
        "world.SetVariable('b', tostring(b))\n"
        "world.SetVariable('c', tostring(c))\n"
        "world.SetVariable('d', tostring(d))\n")));

    CPlugin* pa = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, pa);
    EXPECT_STREQ("0",    (const char*)plugin_var(pa, "rc"));   // eOK
    EXPECT_STREQ("r1",   (const char*)plugin_var(pa, "a"));
    EXPECT_STREQ("2",    (const char*)plugin_var(pa, "b"));
    EXPECT_STREQ("x!",   (const char*)plugin_var(pa, "c"));
    EXPECT_STREQ("true", (const char*)plugin_var(pa, "d"));
}

// lua_methods.cpp L_CallPlugin: "we can handle: nil, boolean, number,
// string but NOT: table ... i + 2, add two because we deleted plugin ID and
// function name" — the reported argument number matches the caller's
// original position, and the target's stack is cleared for the next call.
TEST(LuaScripting, CallPluginTableArgErrorReportsOriginalArgNumber)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("tblB", IDB, "TblB",
        "function fn(x) return x end\n")));
    ASSERT_TRUE(load_plugin(doc, write_plugin("tblA", IDA, "TblA",
        "local rc, msg = world.CallPlugin('" IDB "', 'fn', 'ok', {})\n"
        "world.SetVariable('rc', tostring(rc))\n"
        "world.SetVariable('msg', tostring(msg))\n"
        "local rc2, v = world.CallPlugin('" IDB "', 'fn', 'y')\n"
        "world.SetVariable('rc2', tostring(rc2))\n"
        "world.SetVariable('v', tostring(v))\n")));

    CPlugin* pa = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, pa);
    CString rc = plugin_var(pa, "rc");
    EXPECT_STREQ((const char*)CFormat("%d", (int)eBadParameter), (const char*)rc);
    CString msg = plugin_var(pa, "msg");
    EXPECT_TRUE(msg.Find("#4") != -1);          // caller's original position
    EXPECT_TRUE(msg.Find("table") != -1);
    // the target plugin's stack was cleared: the next call works normally
    EXPECT_STREQ("0", (const char*)plugin_var(pa, "rc2"));
    EXPECT_STREQ("y", (const char*)plugin_var(pa, "v"));
}

// methods_plugins.cpp GetPlugin: "cater for obscure case where plugin isn't
// in list (eg. we are closing it)" — during OnPluginClose the plugin can
// still find itself by ID via m_CurrentPlugin.
TEST(PluginCallback, ClosingPluginStillFindableDuringOnPluginClose)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("close", IDA, "ClosingPlugin",
        "function OnPluginClose()\n"
        "  world.Note('close:' .. tostring(\n"
        "      world.GetPluginInfo(world.GetPluginID(), 1)))\n"
        "end\n")));

    EXPECT_EQ((long)eOK, doc.UnloadPlugin(IDA));
    // the close callback resolved its own info while already out of the list
    EXPECT_TRUE(output_contains(doc, "close:ClosingPlugin"));
}

// methods_plugins.cpp UnloadPlugin: "cannot delete ourselves" — a plugin
// unloading its own ID gets eBadParameter and stays installed.
TEST(PluginCallback, PluginCannotUnloadItself)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("self", IDA, "SelfPlugin",
        "world.SetVariable('rc', tostring(world.UnloadPlugin(world.GetPluginID())))\n")));

    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);                       // still installed
    EXPECT_STREQ((const char*)CFormat("%d", (int)eBadParameter),
                 (const char*)plugin_var(p, "rc"));
}

// methods_plugins.cpp UnloadPlugin: "if not found, try to find by name" —
// a case-insensitive NAME lookup when the argument is not a known ID.
TEST(LuaScripting, UnloadPluginAcceptsNameWhenIDNotFound)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("named", IDA, "NamedPlugin",
        "-- nothing\n")));
    ASSERT_TRUE(doc.GetPlugin(IDA) != NULL);

    EXPECT_EQ((long)eOK, doc.UnloadPlugin("namedplugin"));   // by name
    EXPECT_TRUE(doc.GetPlugin(IDA) == NULL);                 // gone
}

// methods_plugins.cpp LoadPlugin: "m_CurrentPlugin = NULL; otherwise plugin
// won't load if done from another one" — a plugin can install another
// plugin at world scope.
TEST(PluginLoad, PluginCanLoadAnotherPlugin)
{
    CMUSHclientDoc doc; make_doc(doc);
    std::string pathB = write_plugin("loadB", IDB, "LoadedB", "-- nothing\n");
    std::string body = "world.SetVariable('rc', tostring(world.LoadPlugin('" +
                       pathB + "')))\n";
    ASSERT_TRUE(load_plugin(doc, write_plugin("loadA", IDA, "LoaderA",
                                              body.c_str())));

    CPlugin* pa = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, pa);
    EXPECT_STREQ("0", (const char*)plugin_var(pa, "rc"));    // eOK
    EXPECT_TRUE(doc.GetPlugin(IDB) != NULL);                 // installed
    // and the loader is still the world (not nested inside A)
    CPlugin* pb = doc.GetPlugin(IDB);
    EXPECT_TRUE(pb->m_pDoc == &doc);
}

// lua_scripting.cpp OpenLuaDelayed: "unless they explicitly enable it,
// remove ability to load DLLs" — with App.m_bEnablePackageLibrary off,
// package.loadlib is nil (and the C-library loaders stripped) in a fresh
// plugin script state; with it on, loadlib survives.  (NB: the candidate's
// "by default" is inaccurate — the global AllowLoadingDlls option defaults
// to 1/on in globalregistryoptions.cpp, on Windows too.)
TEST(PluginLoad, ScriptStateHasLoadlibRemovedWhenPackageLibraryDisabled)
{
    unsigned int saved = App.m_bEnablePackageLibrary;

    App.m_bEnablePackageLibrary = 0;             // sandboxed
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("sandbox", IDA, "SandboxPlugin",
        "world.SetVariable('ll', tostring(package.loadlib))\n"
        "world.SetVariable('ld3', tostring(package.loaders[3]))\n"
        "world.SetVariable('ld4', tostring(package.loaders[4]))\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);
    EXPECT_STREQ("nil", (const char*)plugin_var(p, "ll"));
    EXPECT_STREQ("nil", (const char*)plugin_var(p, "ld3"));  // DLL loader
    EXPECT_STREQ("nil", (const char*)plugin_var(p, "ld4"));  // all-in-one

    App.m_bEnablePackageLibrary = 1;             // explicitly enabled
    ASSERT_TRUE(load_plugin(doc, write_plugin("sandbox2", IDB, "Sandbox2",
        "world.SetVariable('ll', tostring(package.loadlib))\n")));
    CPlugin* p2 = doc.GetPlugin(IDB);
    ASSERT_NE(nullptr, p2);
    EXPECT_TRUE(plugin_var(p2, "ll").Find("function") != -1);

    App.m_bEnablePackageLibrary = saved;
}

// methods_plugins.cpp SaveState: "if we are already saving the state, don't
// do it again" — world.SaveState() from inside OnPluginSaveState returns
// ePluginCouldNotSaveState instead of recursing.
TEST(PluginState, ReentrantSaveStateRejected)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_strWorldID = "0123456789abcdef01234567";
    char dir[256];
    snprintf(dir, sizeof dir, "/tmp/mc_test_state_%d_reent/", (int)getpid());
    App.m_strDefaultStateFilesDirectory = dir;

    ASSERT_TRUE(load_plugin(doc, write_plugin("reent", IDA, "ReentPlugin",
        "function OnPluginSaveState()\n"
        "  world.SetVariable('rc', tostring(world.SaveState()))\n"
        "end\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    EXPECT_FALSE(p->SaveState(true));            // scripted save succeeds
                                                 // (returns bError: false = OK)
    EXPECT_STREQ((const char*)CFormat("%d", (int)ePluginCouldNotSaveState),
                 (const char*)plugin_var(p, "rc"));   // inner call rejected

    App.m_strDefaultStateFilesDirectory.Empty();
}

// plugins.cpp: "Auto-create the plugin-state folder (mkdir -p) rather than
// blocking with a modal on a fresh setup" — and the state file is named
// exactly <worldID>-<pluginID>-state.xml (the ecosystem contract).
TEST(PluginState, SaveStateAutoCreatesFolderAndNamesFile)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_strWorldID = "0123456789abcdef01234567";
    char dir[256];
    snprintf(dir, sizeof dir, "/tmp/mc_test_state_%d/nested/deeper/", (int)getpid());
    App.m_strDefaultStateFilesDirectory = dir;

    ASSERT_TRUE(load_plugin(doc, write_plugin("state", IDA, "StatePlugin",
        "-- nothing\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    EXPECT_FALSE(p->SaveState(true));            // creates the missing folder

    char expected[512];
    snprintf(expected, sizeof expected, "%s%s-%s-state.xml",
             dir, "0123456789abcdef01234567", IDA);
    struct stat st;
    EXPECT_EQ(0, stat(expected, &st));           // file exists, exact name
    EXPECT_GT((long)st.st_size, 0L);

    App.m_strDefaultStateFilesDirectory.Empty();
}

// plugins.cpp SendToAll/SendToFirst dispatchers: "ignore disabled plugins" —
// every dispatch variant must skip a disabled plugin, and run it again once
// it is re-enabled.
TEST(PluginCallback, DisabledPluginSkippedByAllDispatchVariants)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("dis", IDA, "DisPlugin",
        "function OnPluginWorldSave() world.SetVariable('ws', 'ran') end\n"
        "function OnPluginTrace(m)   world.SetVariable('tr', m)     end\n"
        "function OnPluginSend(t)    world.SetVariable('sd', t)     return true end\n"
        "function OnPluginCommandEntered(t) return t .. '!' end\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    p->m_bEnabled = false;
    CString cmd("go");
    doc.SendToAllPluginCallbacks(ON_PLUGIN_WORLD_SAVE);              // no-arg
    EXPECT_FALSE(doc.SendToFirstPluginCallbacks(ON_PLUGIN_TRACE, "t"));
    EXPECT_TRUE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, "x", true));
    doc.SendToAllPluginCallbacksRtn(ON_PLUGIN_COMMAND_ENTERED, cmd); // string rtn
    EXPECT_STREQ("<unset>", (const char*)plugin_var(p, "ws"));
    EXPECT_STREQ("<unset>", (const char*)plugin_var(p, "tr"));
    EXPECT_STREQ("<unset>", (const char*)plugin_var(p, "sd"));
    EXPECT_STREQ("go", (const char*)cmd);                            // untouched

    p->m_bEnabled = true;
    doc.SendToAllPluginCallbacks(ON_PLUGIN_WORLD_SAVE);
    EXPECT_STREQ("ran", (const char*)plugin_var(p, "ws"));
}

// plugins.cpp SendToAllPluginCallbacksRtn: "this sends a string to all
// plugins and allows them to modify it" — each plugin in load order sees the
// previous plugin's rewrite (a pipeline, not fan-out of the original).
TEST(PluginCallback, RtnCallbackChainPipesRewrittenString)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("rtn_a", IDA, "RtnA",
        "function OnPluginCommandEntered(t) return t .. '+A' end\n")));
    ASSERT_TRUE(load_plugin(doc, write_plugin("rtn_b", IDB, "RtnB",
        "function OnPluginCommandEntered(t) return t .. '+B' end\n")));

    CString cmd("cmd");
    doc.SendToAllPluginCallbacksRtn(ON_PLUGIN_COMMAND_ENTERED, cmd);
    EXPECT_STREQ("cmd+A+B", (const char*)cmd);   // B saw A's output
}

// lua_scripting.cpp ExecuteLua (string in/out): the result is only replaced
// when the function returns a string — no return leaves the text untouched,
// while a returned number is coerced to its decimal string (lua_isstring
// accepts numbers).
TEST(PluginCallback, RtnCallbackNoReturnKeepsTextNumberIsCoerced)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("rtn_keep", IDA, "RtnKeep",
        "function OnPluginCommandEntered(t) world.SetVariable('saw', t) end\n")));
    CString cmd("keepme");
    doc.SendToAllPluginCallbacksRtn(ON_PLUGIN_COMMAND_ENTERED, cmd);
    EXPECT_STREQ("keepme", (const char*)cmd);    // nil return: unchanged
    EXPECT_STREQ("keepme",
                 (const char*)plugin_var(doc.GetPlugin(IDA), "saw"));

    CMUSHclientDoc doc2; make_doc(doc2);
    ASSERT_TRUE(load_plugin(doc2, write_plugin("rtn_num", IDA, "RtnNum",
        "function OnPluginCommandEntered(t) return 42 end\n")));
    CString cmd2("original");
    doc2.SendToAllPluginCallbacksRtn(ON_PLUGIN_COMMAND_ENTERED, cmd2);
    EXPECT_STREQ("42", (const char*)cmd2);       // number coerced to string
}

// plugins.cpp ExecutePluginScript(long, long, string): both numbers and the
// string reach the Lua callback in declaration order (numbers first).
TEST(PluginCallback, TwoNumbersOneStringCallbackReceivesAllArgs)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("sd", IDA, "ScreendrawPlugin",
        "function OnPluginScreendraw(t, l, s)\n"
        "  world.SetVariable('args',\n"
        "      tostring(t) .. '|' .. tostring(l) .. '|' .. s)\n"
        "end\n")));

    EXPECT_TRUE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SCREENDRAW,
                                             (long)5, (long)1, "drawn",
                                             false, false));
    EXPECT_STREQ("5|1|drawn",
                 (const char*)plugin_var(doc.GetPlugin(IDA), "args"));
}

// plugins.cpp 3-argument SendToAllPluginCallbacks: bStopOnTrue returns as
// soon as a plugin with the callback answers true (later plugins unasked);
// with NO handler at all the tail inverts: "if they wanted to stop on true,
// assume false and vice-versa".
TEST(PluginCallback, TwoNumbersOneStringStopOnTrueSemantics)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("sd_yes", IDA, "SaysTrue",
        "function OnPluginScreendraw(t, l, s) return true end\n")));
    ASSERT_TRUE(load_plugin(doc, write_plugin("sd_after", IDB, "AfterTrue",
        "function OnPluginScreendraw(t, l, s)\n"
        "  world.SetVariable('ran', 'yes')\n"
        "end\n")));

    EXPECT_TRUE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SCREENDRAW,
                                             (long)1, (long)2, "x",
                                             true, false));   // stop on true
    EXPECT_STREQ("<unset>",
                 (const char*)plugin_var(doc.GetPlugin(IDB), "ran"));

    // no plugin defines the callback: stop-on-true assumes false...
    CMUSHclientDoc doc2; make_doc(doc2);
    ASSERT_TRUE(load_plugin(doc2, write_plugin("sd_none", IDA, "NoHandler",
        "-- nothing\n")));
    EXPECT_FALSE(doc2.SendToAllPluginCallbacks(ON_PLUGIN_SCREENDRAW,
                                               (long)1, (long)2, "x",
                                               true, false));
    // ...and stop-on-false assumes true
    EXPECT_TRUE(doc2.SendToAllPluginCallbacks(ON_PLUGIN_SCREENDRAW,
                                              (long)1, (long)2, "x",
                                              false, true));
}

// plugins.cpp 3-argument SendToAllPluginCallbacks: bStopOnFalse returns
// false at the first handler that vetoes.
TEST(PluginCallback, TwoNumbersOneStringStopOnFalseVetoes)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("sd_no", IDA, "SaysFalse",
        "function OnPluginScreendraw(t, l, s) return false end\n")));

    EXPECT_FALSE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SCREENDRAW,
                                              (long)1, (long)2, "x",
                                              false, true));
}

// plugins.cpp 2-argument (number + string) SendToAllPluginCallbacks: the
// same early-stop arms as the 3-argument form.
TEST(PluginCallback, OneNumberOneStringStopSemantics)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("tr_yes", IDA, "ReqTrue",
        "function OnPluginTelnetRequest(t, s)\n"
        "  world.SetVariable('args', tostring(t) .. '|' .. s)\n"
        "  return true\n"
        "end\n")));
    ASSERT_TRUE(load_plugin(doc, write_plugin("tr_after", IDB, "ReqAfter",
        "function OnPluginTelnetRequest(t, s)\n"
        "  world.SetVariable('ran', 'yes')\n"
        "end\n")));

    // stop-on-true: first true wins, B never asked
    EXPECT_TRUE(doc.SendToAllPluginCallbacks(ON_PLUGIN_TELNET_REQUEST,
                                             (long)201, string("do"),
                                             true, false));
    EXPECT_STREQ("201|do",
                 (const char*)plugin_var(doc.GetPlugin(IDA), "args"));
    EXPECT_STREQ("<unset>",
                 (const char*)plugin_var(doc.GetPlugin(IDB), "ran"));

    // stop-on-false with a vetoing handler
    CMUSHclientDoc doc2; make_doc(doc2);
    ASSERT_TRUE(load_plugin(doc2, write_plugin("tr_no", IDA, "ReqFalse",
        "function OnPluginTelnetRequest(t, s) return false end\n")));
    EXPECT_FALSE(doc2.SendToAllPluginCallbacks(ON_PLUGIN_TELNET_REQUEST,
                                               (long)1, string("x"),
                                               false, true));

    // no handler anywhere: tail inversion (same rule as 3-argument form)
    CMUSHclientDoc doc3; make_doc(doc3);
    ASSERT_TRUE(load_plugin(doc3, write_plugin("tr_none", IDA, "ReqNone",
        "-- nothing\n")));
    EXPECT_FALSE(doc3.SendToAllPluginCallbacks(ON_PLUGIN_TELNET_REQUEST,
                                               (long)1, string("x"),
                                               true, false));
    EXPECT_TRUE(doc3.SendToAllPluginCallbacks(ON_PLUGIN_TELNET_REQUEST,
                                              (long)1, string("x"),
                                              false, true));
}

// plugins.cpp CString convenience overloads "forward to the const char*
// implementation" — including the 4-argument form, whose bStopOnTrue flag
// is documented as ignored.
TEST(PluginCallback, CStringOverloadsForwardToCharImplementation)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("cs", IDA, "CsPlugin",
        "function OnPluginSend(t)\n"
        "  world.SetVariable('saw', t)\n"
        "  return false\n"
        "end\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    // 2-arg CString form: bStopOnFalse defaults off, veto still aggregates
    EXPECT_FALSE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, CString("one")));
    EXPECT_STREQ("one", (const char*)plugin_var(p, "saw"));

    // 3-arg CString form forwards the stop flag
    EXPECT_FALSE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, CString("two"),
                                              true));
    EXPECT_STREQ("two", (const char*)plugin_var(p, "saw"));

    // 4-arg CString form: bStopOnTrue ignored, behaves as the 2-arg form
    EXPECT_FALSE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, CString("three"),
                                              true, true));
    EXPECT_STREQ("three", (const char*)plugin_var(p, "saw"));
}

// plugins.cpp GetPluginDispid: "else return DISPID_UNKNOWN" — a plugin with
// no script engine can never resolve a callback.
TEST(PluginCallback, EnginelessPluginDispidIsUnknown)
{
    CMUSHclientDoc doc; make_doc(doc);
    CPlugin* p = add_bare_plugin(doc, IDA, "NoEngine");
    EXPECT_EQ(DISPID_UNKNOWN, p->GetPluginDispid("OnPluginSend"));
}

// plugins.cpp SaveState: "need a directory" — with no state directory
// configured the save is skipped entirely: no OnPluginSaveState callback,
// reported as 'not needed' (true).
TEST(PluginState, SaveStateSkippedWhenNoStateDirConfigured)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_strWorldID = "0123456789abcdef01234567";
    App.m_strDefaultStateFilesDirectory.Empty();

    ASSERT_TRUE(load_plugin(doc, write_plugin("nodir", IDA, "NoDirPlugin",
        "function OnPluginSaveState() world.SetVariable('cb', 'ran') end\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    EXPECT_TRUE(p->SaveState(true));   // skipped, not an error
    EXPECT_STREQ("<unset>", (const char*)plugin_var(p, "cb"));
}

// plugins.cpp SaveState: "need a world id" — an unsaved world (empty
// m_strWorldID) cannot name a state file, so the save is skipped before the
// OnPluginSaveState callback.
TEST(PluginState, SaveStateSkippedWithoutWorldID)
{
    CMUSHclientDoc doc; make_doc(doc);
    // (the ctor seeds a world ID — plugin LOADING requires one, so clear it
    // only after the load)
    ASSERT_TRUE(load_plugin(doc, write_plugin("noworld", IDA, "NoWorldPlugin",
        "function OnPluginSaveState() world.SetVariable('cb', 'ran') end\n")));
    doc.m_strWorldID.Empty();

    char dir[256];
    snprintf(dir, sizeof dir, "/tmp/mc_test_noworld_%d/", (int)getpid());
    App.m_strDefaultStateFilesDirectory = dir;
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    EXPECT_TRUE(p->SaveState(true));
    EXPECT_STREQ("<unset>", (const char*)plugin_var(p, "cb"));

    App.m_strDefaultStateFilesDirectory.Empty();
}

// plugins.cpp SaveState: OnPluginSaveState runs BEFORE the variables are
// serialized — a variable set inside the callback appears in the state file.
TEST(PluginState, OnPluginSaveStateRunsBeforeSerialization)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_strWorldID = "0123456789abcdef01234567";
    char dir[256];
    snprintf(dir, sizeof dir, "/tmp/mc_test_cborder_%d/", (int)getpid());
    App.m_strDefaultStateFilesDirectory = dir;

    ASSERT_TRUE(load_plugin(doc, write_plugin("cborder", IDA, "CbOrderPlugin",
        "function OnPluginSaveState()\n"
        "  world.SetVariable('set_inside_callback', 'sentinel_value_xyzzy')\n"
        "end\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    EXPECT_FALSE(p->SaveState(true));   // false == saved OK (returns bError)

    char statefile[512];
    snprintf(statefile, sizeof statefile, "%s%s-%s-state.xml",
             dir, "0123456789abcdef01234567", IDA);
    FILE* f = fopen(statefile, "rb");
    ASSERT_NE(nullptr, (void*)f);
    std::string contents;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        contents.append(buf, n);
    fclose(f);
    EXPECT_NE(std::string::npos, contents.find("set_inside_callback"));
    EXPECT_NE(std::string::npos, contents.find("sentinel_value_xyzzy"));

    App.m_strDefaultStateFilesDirectory.Empty();
}

// plugins.cpp SaveState: a state "folder" that exists but is NOT a
// directory raises the error path; headless there is no directory picker,
// so the save proceeds with the unchanged (file) prefix — the state file
// lands beside it with the concatenated name.
TEST(PluginState, SaveStateFolderNotADirectoryStillSavesBesideIt)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_strWorldID = "0123456789abcdef01234567";

    char base[256], blocker[256];
    snprintf(base, sizeof base, "/tmp/mc_test_notdir_%d", (int)getpid());
    ::mkdir(base, 0755);
    snprintf(blocker, sizeof blocker, "%s/blocker", base);
    FILE* bf = fopen(blocker, "wb");
    ASSERT_NE(nullptr, (void*)bf);
    fputs("x", bf);
    fclose(bf);
    App.m_strDefaultStateFilesDirectory = blocker;    // a FILE, not a folder

    ASSERT_TRUE(load_plugin(doc, write_plugin("notdir", IDA, "NotDirPlugin",
        "-- nothing\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    EXPECT_FALSE(p->SaveState(true));   // still "succeeds"

    char statefile[512];
    snprintf(statefile, sizeof statefile, "%s%s-%s-state.xml",
             blocker, "0123456789abcdef01234567", IDA);
    struct stat st;
    EXPECT_EQ(0, stat(statefile, &st)); // beside the blocker, glued name

    App.m_strDefaultStateFilesDirectory.Empty();
}

// plugins.cpp ~CPlugin: "delete our arrays" — an array created inside a
// plugin lives in that plugin's own m_Arrays map (not the world's), and is
// reclaimed when the plugin closes.
TEST(PluginLifecycle, PluginArrayLivesInPluginScope)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("arr", IDA, "ArrPlugin",
        "world.SetVariable('rc', tostring(world.ArrayCreate('a1')))\n"
        "world.ArraySet('a1', 'k', 'v')\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);
    EXPECT_STREQ("0", (const char*)plugin_var(p, "rc"));   // eOK
    EXPECT_EQ((size_t)1, p->m_Arrays.size());              // plugin-scoped
    EXPECT_EQ((size_t)0, doc.m_Arrays.size());             // not world-scoped
    // doc teardown at end of test runs ~CPlugin, freeing the array map
}

// ═══ ExecutePluginScript argument-marshalling variants ═════════════════════
// The non-embedded (dispatch) arms: a plugin whose engine is a COM-style
// language (here "vbscript", which has no embedded engine on this platform)
// routes through CScriptEngine::Execute.  Headless, Execute's non-embedded
// arm consumes the dispid (sets DISPID_UNKNOWN, returns true) — so these pin
// the argument-packing paths and the "assume they OK'd it" default.

static CPlugin* add_dispatch_plugin(CMUSHclientDoc& doc, const char* id)
{
    CPlugin* p = new CPlugin(&doc);
    p->m_bSaveState = false;
    p->m_strID = id;
    p->m_strName = "DispatchPlugin";
    p->m_ScriptEngine = new CScriptEngine(&doc, "vbscript");
    doc.m_PluginList.push_back(p);
    return p;
}

TEST(PluginDispatch, NoArgVariantConsumesDispid)
{
    CMUSHclientDoc doc; make_doc(doc);
    CPlugin* p = add_dispatch_plugin(doc, IDA);
    CScriptDispatchID& d = p->m_PluginCallbacks[ON_PLUGIN_TICK];
    d._dispid = 1;   // pretend the routine resolved
    CScriptCallInfo ci(ON_PLUGIN_TICK, d);
    p->ExecutePluginScript(ci);
    EXPECT_EQ((DISPID)DISPID_UNKNOWN, d._dispid);   // headless Execute consumed it
    EXPECT_EQ(1, (int)d._count);
}

TEST(PluginDispatch, TextVariantDefaultsToApproval)
{
    CMUSHclientDoc doc; make_doc(doc);
    CPlugin* p = add_dispatch_plugin(doc, IDA);
    CScriptDispatchID& d = p->m_PluginCallbacks[ON_PLUGIN_SEND];
    d._dispid = 1;
    CScriptCallInfo ci(ON_PLUGIN_SEND, d);
    EXPECT_TRUE(p->ExecutePluginScript(ci, "the line"));
    EXPECT_EQ(1, (int)d._count);
}

TEST(PluginDispatch, NumberTextVariantPacksEmbeddedNuls)
{
    CMUSHclientDoc doc; make_doc(doc);
    CPlugin* p = add_dispatch_plugin(doc, IDA);
    CScriptDispatchID& d = p->m_PluginCallbacks[ON_PLUGIN_TELNET_SUBNEGOTIATION];
    d._dispid = 1;
    CScriptCallInfo ci(ON_PLUGIN_TELNET_SUBNEGOTIATION, d);
    // the "crazy code" path: a std::string payload with embedded NULs
    EXPECT_TRUE(p->ExecutePluginScript(ci, 201L, std::string("a\0b", 3)));
    EXPECT_EQ(1, (int)d._count);
}

TEST(PluginDispatch, TwoNumbersTextVariant)
{
    CMUSHclientDoc doc; make_doc(doc);
    CPlugin* p = add_dispatch_plugin(doc, IDA);
    CScriptDispatchID& d = p->m_PluginCallbacks[ON_PLUGIN_MXP_SETENTITY];
    d._dispid = 1;
    CScriptCallInfo ci(ON_PLUGIN_MXP_SETENTITY, d);
    EXPECT_TRUE(p->ExecutePluginScript(ci, 3L, 4L, std::string("txt")));
    EXPECT_EQ(1, (int)d._count);
}

TEST(PluginDispatch, NumberAndThreeStringsVariant)
{
    CMUSHclientDoc doc; make_doc(doc);
    CPlugin* p = add_dispatch_plugin(doc, IDA);
    CScriptDispatchID& d = p->m_PluginCallbacks[ON_PLUGIN_CHAT_ACCEPT];
    d._dispid = 1;
    CScriptCallInfo ci(ON_PLUGIN_CHAT_ACCEPT, d);
    EXPECT_TRUE(p->ExecutePluginScript(ci, 7L, "one", "two", "three"));
    EXPECT_EQ(1, (int)d._count);
}

TEST(PluginDispatch, RtnVariantLeavesTextWhenNoResult)
{
    CMUSHclientDoc doc; make_doc(doc);
    CPlugin* p = add_dispatch_plugin(doc, IDA);
    CScriptDispatchID& d = p->m_PluginCallbacks[ON_PLUGIN_PARTIAL_LINE];
    d._dispid = 1;
    CScriptCallInfo ci(ON_PLUGIN_PARTIAL_LINE, d);
    CString text = "unchanged";
    p->ExecutePluginScriptRtn(ci, text);
    EXPECT_STREQ("unchanged", (const char*)text);   // no VT_BSTR result headless
    EXPECT_EQ(1, (int)d._count);
}

TEST(PluginDispatch, UnknownDispidShortCircuitsAsApproval)
{
    CMUSHclientDoc doc; make_doc(doc);
    CPlugin* p = add_dispatch_plugin(doc, IDA);
    CScriptDispatchID& d = p->m_PluginCallbacks[ON_PLUGIN_SEND];
    ASSERT_EQ((DISPID)DISPID_UNKNOWN, d._dispid);   // never resolved
    CScriptCallInfo ci(ON_PLUGIN_SEND, d);
    EXPECT_TRUE(p->ExecutePluginScript(ci, "line"));
    EXPECT_EQ(0, (int)d._count);                    // not invoked at all
}

// ═══ Lua arms of the argument variants ═════════════════════════════════════

// Bind `fn` (already parsed in the plugin's Lua engine) as callback `name`.
static CScriptDispatchID& bind_callback(CPlugin* p, const char* fn)
{
    CScriptDispatchID& d = p->m_PluginCallbacks[fn];
    d._dispid = p->m_ScriptEngine->GetDispid(fn);
    return d;
}

TEST(PluginLuaArgs, ArgVariantsReachLuaWithArguments)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("args", IDA, "ArgsPlugin",
        "function fNumText(n, text)\n"
        "  world.SetVariable('numtext', tostring(n) .. ':' .. text)\n"
        "  return true\n"
        "end\n"
        "function fTwoNums(a, b, text)\n"
        "  world.SetVariable('twonums', tostring(a+b) .. ':' .. text)\n"
        "  return true\n"
        "end\n"
        "function fStrings(n, s1, s2, s3)\n"
        "  world.SetVariable('strings', tostring(n) .. s1 .. s2 .. s3)\n"
        "  return true\n"
        "end\n"
        "function fRtn(text)\n"
        "  return text .. '-modified'\n"
        "end\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);
    CPlugin* was = doc.m_CurrentPlugin;
    doc.m_CurrentPlugin = p;   // variables land in the plugin's own scope

    CScriptDispatchID& d1 = bind_callback(p, "fNumText");
    ASSERT_NE((DISPID)DISPID_UNKNOWN, d1._dispid);
    CScriptCallInfo c1("fNumText", d1);
    EXPECT_TRUE(p->ExecutePluginScript(c1, 42L, std::string("payload")));
    EXPECT_STREQ("42:payload", (const char*)plugin_var(p, "numtext"));

    CScriptDispatchID& d2 = bind_callback(p, "fTwoNums");
    CScriptCallInfo c2("fTwoNums", d2);
    EXPECT_TRUE(p->ExecutePluginScript(c2, 30L, 12L, std::string("t")));
    EXPECT_STREQ("42:t", (const char*)plugin_var(p, "twonums"));

    CScriptDispatchID& d3 = bind_callback(p, "fStrings");
    CScriptCallInfo c3("fStrings", d3);
    EXPECT_TRUE(p->ExecutePluginScript(c3, 9L, "a", "b", "c"));
    EXPECT_STREQ("9abc", (const char*)plugin_var(p, "strings"));

    CScriptDispatchID& d4 = bind_callback(p, "fRtn");
    CScriptCallInfo c4("fRtn", d4);
    CString text = "base";
    p->ExecutePluginScriptRtn(c4, text);
    EXPECT_STREQ("base-modified", (const char*)text);

    doc.m_CurrentPlugin = was;
}

// ═══ MXP plugin callbacks (mxpStart / mxpClose / mxpDefs / mxpError) ═══════

TEST(PluginCallback, MxpTagAndEntityCallbacksReachPlugins)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bMXP = true;
    doc.m_iUseMXP = eUseMXP;
    doc.m_iMXP_mode = eMXP_secure;
    ASSERT_TRUE(load_plugin(doc, write_plugin("mxpcb", IDA, "MxpCb",
        "function OnPluginMXPopenTag(text) world.SetVariable('open', text) end\n"
        "function OnPluginMXPcloseTag(text) world.SetVariable('close', text) end\n"
        "function OnPluginMXPsetEntity(text) world.SetVariable('ent', text) end\n"
        "function OnPluginMXPerror(text) world.SetVariable('err', text) end\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    // ASSERT, do not assign.  These four flags are what MXP consults before it
    // will call a plugin at all (mxpStart.cpp:159, mxpOpenAtomic.cpp:46), and
    // deriving them from the plugin's callbacks is InternalLoadPlugin's job
    // (xml/xml_load_world.cpp).  This test used to SET them by hand, which meant
    // it kept passing when that derivation was disabled -- and it was, silently,
    // for a while: every plugin's MXP callbacks were dead and nothing noticed.
    EXPECT_TRUE(doc.m_bPluginProcessesOpenTag);
    EXPECT_TRUE(doc.m_bPluginProcessesCloseTag);
    EXPECT_TRUE(doc.m_bPluginProcessesSetEntity);
    EXPECT_TRUE(doc.m_bPluginProcessesError);

    const char* text1 = "<b>bold</b>";
    doc.DisplayMsg(text1, (int)strlen(text1), 0);
    EXPECT_STREQ("b,", (const char*)plugin_var(p, "open"));
    EXPECT_TRUE(CString(plugin_var(p, "close")).Find("b,") == 0);

    const char* text2 = "<!ENTITY pver '4'>";
    doc.DisplayMsg(text2, (int)strlen(text2), 0);
    EXPECT_STREQ("pver=4", (const char*)plugin_var(p, "ent"));

    const char* text3 = "<nosuchtag>";
    doc.DisplayMsg(text3, (int)strlen(text3), 0);
    EXPECT_TRUE(CString(plugin_var(p, "err")).Find("E,") == 0);
}

// The MXP open-tag CALL SITE, end to end: real plugin, real MUD text, real
// mxpStart.cpp.  Everything else in the scripting suite calls CScriptEngine
// directly, which leaves the code that BUILDS the callback's arguments
// untested -- and that is exactly where the damage was: all four MXP hooks
// gated on IsLua(), so the other five languages took a COM path whose SAFEARRAY
// the port cannot marshal and received no attributes at all.  Lua is the
// language this suite can link, so it is the one that guards the call site; the
// five embedded engines' handling of the same table is covered by the
// per-language conformance suite (platform/engines_selftest.cpp).
// The MXP open-tag CALL SITE for the WORLD's own script handler, end to end.
//
// Two different notifications leave mxpStart.cpp and they carry different
// things.  Plugins get SendToAllPluginCallbacks(ON_PLUGIN_MXP_OPENTAG, ...) --
// one combined "tag,args" string, on both platforms.  The world's configured
// handler (World Properties > MXP) gets the RICH callback: the tag name, the
// argument text, and a TABLE of the parsed arguments.
//
// That rich arm gated on IsLua(), so a world scripted in Python, Perl,
// JavaScript, Tcl or Ruby fell through to the COM arm, which packs the
// arguments into a SAFEARRAY that the port's CScriptEngine::Execute cannot
// marshal (its switch ends "default: break;") -- those five got no arguments at
// all.  Lua is the language this suite can link, so it guards the call site;
// every language's handling of the table itself is covered by the per-language
// conformance suite (platform/engines_selftest.cpp).
TEST(MxpWorldScript, OpenTagHandlerGetsTagArgumentsAndTable)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bMXP = true;
    doc.m_iUseMXP = eUseMXP;
    doc.m_iMXP_mode = eMXP_secure;

    doc.m_ScriptEngine = new CScriptEngine(&doc, "Lua");
    ASSERT_FALSE(doc.m_ScriptEngine->CreateScriptEngine());
    ASSERT_FALSE(doc.m_ScriptEngine->Parse(CString(
        "function OnMXPopenTag(name, args, t)\n"
        "  world.SetVariable('tag',   tostring(name))\n"
        "  world.SetVariable('args',  tostring(args))\n"
        "  world.SetVariable('ttype', type(t))\n"
        "end\n"), "<test>"));
    doc.m_strOnMXP_OpenTag   = "OnMXPopenTag";
    doc.m_dispidOnMXP_OpenTag = doc.m_ScriptEngine->GetDispid("OnMXPopenTag");
    ASSERT_NE(DISPID_UNKNOWN, doc.m_dispidOnMXP_OpenTag);

    const char* text = "<font color=red>hi</font>";
    doc.DisplayMsg(text, (int)strlen(text), 0);

    VARIANT v;
    v = doc.GetVariable("tag");
    EXPECT_TRUE(CString(v.bstrVal ? v.bstrVal : "").Find("font") == 0);
    v = doc.GetVariable("args");
    EXPECT_TRUE(CString(v.bstrVal ? v.bstrVal : "").Find("color") >= 0);
    // The third argument is the tell: the COM arm cannot deliver it.
    v = doc.GetVariable("ttype");
    EXPECT_STREQ("table", v.bstrVal ? v.bstrVal : "");
}

// The other half: a plugin with no MXP callbacks must leave the flags alone.
// Without this, the derivation could satisfy the test above by setting all four
// for every plugin, which would make MXP call into plugins that implement
// nothing on every tag.
TEST(PluginMxp, FlagsStayFalseForAPluginWithNoMxpCallbacks)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iUseMXP = eUseMXP;
    ASSERT_TRUE(load_plugin(doc, write_plugin("nomxp", IDB, "NoMxp",
        "function OnPluginConnect() end\n")));
    ASSERT_NE(nullptr, doc.GetPlugin(IDB));

    EXPECT_FALSE(doc.m_bPluginProcessesOpenTag);
    EXPECT_FALSE(doc.m_bPluginProcessesCloseTag);
    EXPECT_FALSE(doc.m_bPluginProcessesSetVariable);
    EXPECT_FALSE(doc.m_bPluginProcessesSetEntity);
    EXPECT_FALSE(doc.m_bPluginProcessesError);
}

// ═══ CPlugin::SaveState ════════════════════════════════════════════════════

TEST(PluginState, SaveStateWritesVariablesToStateFile)
{
    char dir[128];
    snprintf(dir, sizeof dir, "/tmp/mc_test_state_%d", (int)getpid());
    mkdir(dir, 0755);
    CString savedDir = App.m_strDefaultStateFilesDirectory;
    App.m_strDefaultStateFilesDirectory = CString(dir) + "/";

    {
        CMUSHclientDoc doc; make_doc(doc);
        doc.m_strWorldID = "worldid0123456789abcdef0";
        ASSERT_TRUE(load_plugin(doc, write_plugin("save", IDA, "SavePlugin",
            "world.SetVariable('persisted', 'yes')\n"
            "function OnPluginSaveState()\n"
            "  world.SetVariable('saved_from_callback', 'indeed')\n"
            "end\n")));
        CPlugin* p = doc.GetPlugin(IDA);
        ASSERT_NE(nullptr, p);
        p->m_bSaveState = true;
        p->SaveState(true);

        char path[256];
        snprintf(path, sizeof path, "%s/%s-%s-state.xml", dir,
                 "worldid0123456789abcdef0", IDA);
        FILE* f = fopen(path, "rb");
        ASSERT_NE(nullptr, f);
        char buf[8192] = {};
        fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        std::string xml = buf;
        EXPECT_TRUE(xml.find("persisted") != std::string::npos);
        EXPECT_TRUE(xml.find("yes") != std::string::npos);
        // the ON_PLUGIN_SAVE_STATE callback ran BEFORE the write
        EXPECT_TRUE(xml.find("saved_from_callback") != std::string::npos);
        unlink(path);
    }
    App.m_strDefaultStateFilesDirectory = savedDir;
    rmdir(dir);
}

TEST(PluginState, SaveStateSkippedWithoutStateFolderOrWorldId)
{
    CString savedDir = App.m_strDefaultStateFilesDirectory;
    CMUSHclientDoc doc; make_doc(doc);
    CPlugin* p = add_bare_plugin(doc, IDA, "NoState");
    p->m_bSaveState = true;

    App.m_strDefaultStateFilesDirectory = "";
    EXPECT_TRUE(p->SaveState(true));    // no folder configured -> "not needed"

    App.m_strDefaultStateFilesDirectory = "/tmp/";
    doc.m_strWorldID = "";
    EXPECT_TRUE(p->SaveState(true));    // no world id -> nothing to key on

    p->m_bSaveState = false;
    EXPECT_TRUE(p->SaveState(false));   // not scripted, not wanted
    App.m_strDefaultStateFilesDirectory = savedDir;
}

static char* s_pickedDir;

TEST(PluginState, MissingStateFolderFallsBackToDirectoryPicker)
{
    char dir[128];
    snprintf(dir, sizeof dir, "/tmp/mc_test_state_pick_%d", (int)getpid());
    mkdir(dir, 0755);
    s_pickedDir = dir;
    CString savedDir = App.m_strDefaultStateFilesDirectory;
    // A folder that cannot be auto-created (mkdir at "/" fails headless)
    App.m_strDefaultStateFilesDirectory = "/mc-no-such-root-xyz/state/";
    g_wxhook_directorypicker = [](const char* title, const char*) -> char* {
        EXPECT_TRUE(strstr(title, "state files") != nullptr);
        return strdup(s_pickedDir);
    };
    {
        CMUSHclientDoc doc; make_doc(doc);
        doc.m_strWorldID = "worldid0123456789abcdef0";
        CPlugin* p = add_bare_plugin(doc, IDA, "PickState");
        p->m_bSaveState = true;
        p->SaveState(true);
        // picker result adopted (with a trailing slash appended)
        CString expect = CString(dir) + "/";
        EXPECT_STREQ((const char*)expect,
                     (const char*)App.m_strDefaultStateFilesDirectory);
        char path[256];
        snprintf(path, sizeof path, "%s/%s-%s-state.xml", dir,
                 "worldid0123456789abcdef0", IDA);
        FILE* f = fopen(path, "rb");
        EXPECT_NE(nullptr, f);   // state written into the picked folder
        if (f) { fclose(f); unlink(path); }
    }
    g_wxhook_directorypicker = nullptr;
    App.m_strDefaultStateFilesDirectory = savedDir;
    rmdir(dir);
}

// ═══ Round-4 coverage: CallPlugin marshalling arms, disabled-plugin skip
//     arms, SaveState folder errors, the wizard's headless prefix ═══════════

// L_CallPlugin argument arms: missing routine name; calling a routine in a
// plugin that has no script engine.
TEST(CallPluginArms, MissingRoutineAndNonScriptingTarget)
{
    CMUSHclientDoc doc;

    // a scriptless plugin (no <script> block, no language engine)
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_plugin_%d_noscript.xml", (int)getpid());
    FILE* f = fopen(path, "wb");
    ASSERT_TRUE(f != nullptr);
    fprintf(f,
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE muclient>\n"
        "<muclient>\n"
        "<plugin name=\"NoScript\" author=\"T\" id=\"" IDB "\" language=\"Lua\" "
        "version=\"1.0\" />\n"
        "</muclient>\n");
    fclose(f);
    ASSERT_TRUE(load_plugin(doc, path));
    unlink(path);

    // caller plugin runs CallPlugin at load time and records the results
    std::string body =
        "rc1, err1 = world.CallPlugin('" IDB "', '')\n"           // no routine
        "world.SetVariable('rc1', tostring(rc1))\n"
        "world.SetVariable('err1', tostring(err1))\n"
        "rc2, err2 = world.CallPlugin('" IDB "', 'anyfn')\n"      // no engine
        "world.SetVariable('rc2', tostring(rc2))\n"
        "world.SetVariable('err2', tostring(err2))\n";
    std::string caller = write_plugin("caller1", IDA, "Caller1", body.c_str());
    ASSERT_TRUE(load_plugin(doc, caller));
    unlink(caller.c_str());

    CPlugin* pA = doc.GetPlugin(IDA);
    ASSERT_TRUE(pA != nullptr);
    EXPECT_STREQ("30036", (LPCTSTR)plugin_var(pA, "rc1"));   // eNoSuchRoutine
    EXPECT_TRUE(plugin_var(pA, "err1").Find("No function name") != -1);
    EXPECT_STREQ("30036", (LPCTSTR)plugin_var(pA, "rc2"));
    EXPECT_TRUE(plugin_var(pA, "err2").Find("Scripting not enabled") != -1);
}

// CallPlugin marshalling: arguments cross into the target state, multiple
// return values come back with eOK first; a runtime error in the target
// yields the three-value error shape; unsupported return types are refused.
TEST(CallPluginArms, ReturnValuesRuntimeErrorAndBadReturnType)
{
    CMUSHclientDoc doc;

    std::string target =
        "function greet(a, b, flag)\n"
        "  return a .. '/' .. b, 42, flag == nil and 'nilflag' or 'flag'\n"
        "end\n"
        "function boom() error('kapow') end\n"
        "function badret() return function() end end\n";
    std::string tpath = write_plugin("target2", IDB, "Target2", target.c_str());
    ASSERT_TRUE(load_plugin(doc, tpath));
    unlink(tpath.c_str());

    std::string body =
        "local rc, s, n, f = world.CallPlugin('" IDB "', 'greet', 'x', 'y', nil)\n"
        "world.SetVariable('rc', tostring(rc))\n"
        "world.SetVariable('s', tostring(s))\n"
        "world.SetVariable('n', tostring(n))\n"
        "world.SetVariable('f', tostring(f))\n"
        "local erc, emsg, elua = world.CallPlugin('" IDB "', 'boom')\n"
        "world.SetVariable('erc', tostring(erc))\n"
        "world.SetVariable('emsg', tostring(emsg))\n"
        "world.SetVariable('elua', tostring(elua))\n"
        "local brc, bmsg = world.CallPlugin('" IDB "', 'badret')\n"
        "world.SetVariable('brc', tostring(brc))\n"
        "world.SetVariable('bmsg', tostring(bmsg))\n";
    std::string caller = write_plugin("caller2", IDA, "Caller2", body.c_str());
    ASSERT_TRUE(load_plugin(doc, caller));
    unlink(caller.c_str());

    CPlugin* pA = doc.GetPlugin(IDA);
    ASSERT_TRUE(pA != nullptr);
    EXPECT_STREQ("0",       (LPCTSTR)plugin_var(pA, "rc"));   // eOK
    EXPECT_STREQ("x/y",     (LPCTSTR)plugin_var(pA, "s"));
    EXPECT_STREQ("42",      (LPCTSTR)plugin_var(pA, "n"));
    EXPECT_STREQ("nilflag", (LPCTSTR)plugin_var(pA, "f"));    // nil crossed over

    EXPECT_STREQ("30040", (LPCTSTR)plugin_var(pA, "erc"));    // eErrorCallingPluginRoutine
    EXPECT_TRUE(plugin_var(pA, "emsg").Find("Runtime error") != -1);
    EXPECT_TRUE(plugin_var(pA, "elua").Find("kapow") != -1);

    EXPECT_STREQ("30040", (LPCTSTR)plugin_var(pA, "brc"));
    EXPECT_TRUE(plugin_var(pA, "bmsg").Find("Cannot handle return value") != -1);
}

// The deprecated 3-argument CallPlugin form (string argument) still works.
TEST(CallPluginArms, LegacyThreeArgumentForm)
{
    CMUSHclientDoc doc;
    std::string target =
        "function legacy(arg)\n"
        "  world.SetVariable('got', tostring(arg))\n"
        "  return 0\n"
        "end\n";
    std::string tpath = write_plugin("target3", IDB, "Target3", target.c_str());
    ASSERT_TRUE(load_plugin(doc, tpath));
    unlink(tpath.c_str());

    std::string body =
        "world.SetVariable('rc', tostring(world.CallPlugin('" IDB "', 'legacy', 'payload')))\n";
    std::string caller = write_plugin("caller3", IDA, "Caller3", body.c_str());
    ASSERT_TRUE(load_plugin(doc, caller));
    unlink(caller.c_str());

    CPlugin* pB = doc.GetPlugin(IDB);
    ASSERT_TRUE(pB != nullptr);
    EXPECT_STREQ("payload", (LPCTSTR)plugin_var(pB, "got"));
}

// Disabled plugins are skipped by the 2- and 3-numeric-argument callback
// broadcast overloads.
TEST(CallPluginArms, DisabledPluginSkippedByNumericCallbacks)
{
    CMUSHclientDoc doc;
    std::string body =
        "count = 0\n"
        "function OnPluginMXPSetEntity(a) count = count + 1 end\n";
    std::string p = write_plugin("dis", IDA, "Disabled", body.c_str());
    ASSERT_TRUE(load_plugin(doc, p));
    unlink(p.c_str());
    CPlugin* pA = doc.GetPlugin(IDA);
    ASSERT_TRUE(pA != nullptr);
    pA->m_bEnabled = false;

    // 2-arg (long, string) and 3-arg (long, long, string) overloads; with
    // stop-on-true and no enabled handler the broadcast reports false
    EXPECT_TRUE(!doc.SendToAllPluginCallbacks(ON_PLUGIN_MXP_SETENTITY, 1L,
                                              std::string("x"), true, false));
    doc.SendToAllPluginCallbacks(ON_PLUGIN_TELNET_SUBNEGOTIATION, 1L, 2L,
                                 std::string("x"), false, false);
}

// CPlugin::SaveState folder checks: a state "directory" that is actually a
// file trips the is-not-a-directory arm; save fails.
TEST(PluginState, StateFolderIsAFileFails)
{
    CMUSHclientDoc doc;
    doc.m_strWorldID = "0123456789abcdef0123456";

    char filePath[256];
    snprintf(filePath, sizeof filePath, "/tmp/mc_test_plugstate_%d", (int)getpid());
    FILE* f = fopen(filePath, "wb");
    ASSERT_TRUE(f != nullptr);
    fputs("not a dir", f);
    fclose(f);

    CString savedDir = App.m_strDefaultStateFilesDirectory;
    App.m_strDefaultStateFilesDirectory = filePath;

    std::string p = write_plugin("state", IDA, "Stateful",
                                 "world.SetVariable('v', '1')\n");
    ASSERT_TRUE(load_plugin(doc, p));
    unlink(p.c_str());
    CPlugin* pA = doc.GetPlugin(IDA);
    ASSERT_TRUE(pA != nullptr);
    pA->m_bSaveState = true;

    EXPECT_TRUE(!pA->SaveState(true));

    App.m_strDefaultStateFilesDirectory = savedDir;
    unlink(filePath);
}

// The plugin wizard headless: DoModal cancels, but the preparation runs —
// everything gets pre-selected and the script file is loaded.
TEST(WizardTest, HeadlessWizardSelectsEverythingThenCancels)
{
    CMUSHclientDoc doc;

    ASSERT_EQ((long)eOK, doc.AddTrigger("wt", "match", "resp", 1, -1, 0, "", ""));
    ASSERT_EQ((long)eOK, doc.AddAlias("wa", "match", "resp", 1, ""));
    ASSERT_EQ((long)eOK, doc.AddTimer("wm", 0, 1, 0, "resp", 1, ""));
    doc.SetVariable("wv", "val");

    char spath[256];
    snprintf(spath, sizeof spath, "/tmp/mc_test_wiz_%d.lua", (int)getpid());
    FILE* f = fopen(spath, "wb");
    ASSERT_TRUE(f != nullptr);
    fputs("-- wizard script\n", f);
    fclose(f);
    doc.m_strScriptFilename = spath;

    call_OnFilePluginwizard(doc);     // headless: prepares, then cancels
    unlink(spath);

    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("wt", t));
    EXPECT_TRUE(t->bSelected);
    CAlias* a = nullptr;
    ASSERT_TRUE(doc.GetAliasMap().Lookup("wa", a));
    EXPECT_TRUE(a->bSelected);
    CTimer* tm = nullptr;
    ASSERT_TRUE(doc.GetTimerMap().Lookup("wm", tm));
    EXPECT_TRUE(tm->bSelected);
    CVariable* v = nullptr;
    ASSERT_TRUE(doc.GetVariableMap().Lookup("wv", v));
    EXPECT_TRUE(v->bSelected);
}

// ═══ Round-4 sweep: GetPluginInfo cases, load/reload error arms, the
//     doc-level CallPlugin ═══════════════════════════════════════════════════

TEST(PluginInfoR4, AllInfoCasesAnswerTheirTypes)
{
    CMUSHclientDoc doc;
    std::string p = write_plugin("info", IDA, "InfoPlugin",
        "world.SetVariable('x', '1')\n");
    ASSERT_TRUE(load_plugin(doc, p));
    unlink(p.c_str());

    // string cases
    static const int strCases[] = { 1, 2, 3, 4, 5, 6, 7, 8, 20, 23 };
    for (int c : strCases) {
        VARIANT v = doc.GetPluginInfo(IDA, c);
        if ((int)v.vt != VT_BSTR) {
            char msg[64];
            snprintf(msg, sizeof msg, "GetPluginInfo(%d) vt=%d != BSTR", c, (int)v.vt);
            mc_test::fail_msg(__FILE__, __LINE__, msg);
        }
        VariantClear(&v);
    }
    EXPECT_EQ((int)VT_BSTR, (int)doc.GetPluginInfo(IDA, 7).vt);

    // numeric counts
    EXPECT_EQ((int)VT_I4, (int)doc.GetPluginInfo(IDA, 9).vt);
    EXPECT_EQ((int)VT_I4, (int)doc.GetPluginInfo(IDA, 10).vt);
    EXPECT_EQ((int)VT_I4, (int)doc.GetPluginInfo(IDA, 11).vt);
    VARIANT v = doc.GetPluginInfo(IDA, 12);
    EXPECT_EQ((int)VT_I4, (int)v.vt);
    EXPECT_EQ(1, (int)v.lVal);                       // the one variable we set

    // booleans / doubles / dates / order / sequence
    EXPECT_EQ((int)VT_BOOL, (int)doc.GetPluginInfo(IDA, 15).vt);
    EXPECT_EQ((int)VT_BOOL, (int)doc.GetPluginInfo(IDA, 16).vt);
    EXPECT_EQ((int)VT_BOOL, (int)doc.GetPluginInfo(IDA, 17).vt);
    EXPECT_EQ((int)VT_R8,   (int)doc.GetPluginInfo(IDA, 18).vt);
    EXPECT_EQ((int)VT_R8,   (int)doc.GetPluginInfo(IDA, 19).vt);
    EXPECT_EQ((int)VT_I4,   (int)doc.GetPluginInfo(IDA, 21).vt);   // load order
    EXPECT_EQ((int)VT_DATE, (int)doc.GetPluginInfo(IDA, 22).vt);
    EXPECT_EQ((int)VT_R8,   (int)doc.GetPluginInfo(IDA, 24).vt);   // script time
    EXPECT_EQ((int)VT_I2,   (int)doc.GetPluginInfo(IDA, 25).vt);   // sequence

    // 13/14: date-written / date-modified — either DATE (stamped) or EMPTY
    // (zero time); both sides of the guard are legal, just probe the arms
    {
        // unset dates leave the NULL default in place
        VARIANT d13 = doc.GetPluginInfo(IDA, 13);
        EXPECT_TRUE((int)d13.vt == VT_DATE || (int)d13.vt == VT_NULL);
        VARIANT d14 = doc.GetPluginInfo(IDA, 14);
        EXPECT_TRUE((int)d14.vt == VT_DATE || (int)d14.vt == VT_NULL);
    }

    // unknown case answers VT_NULL; unknown plugin answers VT_NULL too
    EXPECT_EQ((int)VT_NULL, (int)doc.GetPluginInfo(IDA, 999).vt);
    EXPECT_EQ((int)VT_NULL, (int)doc.GetPluginInfo("ffffffffffffffffffffffff", 1).vt);
}

TEST(PluginInfoR4, LoadAndReloadErrorArms)
{
    CMUSHclientDoc doc;
    // Windows parity (round 5): the shim CFile constructor now throws
    // CFileException* on open failure, so a missing plugin file surfaces as
    // ePluginFileNotFound exactly as on Windows (previously the round-3/4
    // documented divergence answered the generic eProblemsLoadingPlugin).
    EXPECT_EQ((long)ePluginFileNotFound, doc.LoadPlugin("/nonexistent-r4/p.xml"));

    // a file that exists but is not valid plugin XML
    char bad[128];
    snprintf(bad, sizeof bad, "/tmp/mc_test_badplug_%d.xml", (int)getpid());
    FILE* f = fopen(bad, "wb");
    ASSERT_TRUE(f != nullptr);
    fputs("<?xml version=\"1.0\"?><!DOCTYPE muclient><muclient><plugin/></muclient>", f);
    fclose(f);
    EXPECT_EQ((long)eProblemsLoadingPlugin, doc.LoadPlugin(bad));
    unlink(bad);

    // reload arms
    EXPECT_EQ((long)eNoSuchPlugin, doc.ReloadPlugin("ffffffffffffffffffffffff"));

    std::string p = write_plugin("rl", IDA, "Reloadable",
        "world.SetVariable('n', '1')\n");
    ASSERT_TRUE(load_plugin(doc, p));
    EXPECT_EQ((long)eOK, doc.ReloadPlugin(IDA));
    EXPECT_TRUE(doc.GetPlugin(IDA) != nullptr);

    // reload after the source file vanished: ePluginFileNotFound, and (as on
    // Windows) the plugin was already deleted before the reload was attempted
    unlink(p.c_str());
    EXPECT_EQ((long)ePluginFileNotFound, doc.ReloadPlugin(IDA));
    EXPECT_TRUE(doc.GetPlugin(IDA) == nullptr);
}

// CPlugin::SaveState error arm (plugins.cpp catch CFileException*): the state
// file cannot be created -> the catch reports it and SaveState answers "not
// saved" (true), which world.SaveState maps to ePluginCouldNotSaveState.
// Dead until round 5 (the old shim CFile ctor could not throw).
TEST(PluginInfoR5, SaveStateFileErrorArm)
{
    CMUSHclientDoc doc;
    doc.m_strWorldID = "b1b1b1b1b1b1b1b1b1b1b1b1";

    std::string p = write_plugin("ss", IDA, "Saver",
        "world.SetVariable('keep', '1')\n");
    ASSERT_TRUE(load_plugin(doc, p));
    CPlugin* pl = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, pl);

    char dir[128];
    snprintf(dir, sizeof dir, "/tmp/mc_test_state_%d", (int)getpid());
    mkdir(dir, 0755);
    App.m_strDefaultStateFilesDirectory = (std::string(dir) + "/").c_str();

    // success arm: the state file is created
    EXPECT_FALSE(pl->SaveState(true));
    std::string state = std::string(dir) + "/" +
        (const char*)doc.m_strWorldID + "-" + IDA + "-state.xml";
    struct stat st;
    ASSERT_EQ(0, stat(state.c_str(), &st));

    // failure arm: an unwritable state file -> CFile ctor throws
    // CFileException* -> the catch arm answers "not saved"
    chmod(state.c_str(), 0444);
    EXPECT_TRUE(pl->SaveState(true));

    chmod(state.c_str(), 0644);
    unlink(state.c_str());
    rmdir(dir);
    unlink(p.c_str());
    App.m_strDefaultStateFilesDirectory.Empty();
}

TEST(PluginInfoR4, DocLevelCallPluginArms)
{
    CMUSHclientDoc doc;
    std::string p = write_plugin("dc", IDA, "DocCall",
        "function hello(arg) world.SetVariable('arg', tostring(arg)) end\n");
    ASSERT_TRUE(load_plugin(doc, p));
    unlink(p.c_str());

    EXPECT_EQ((long)eNoSuchPlugin,  doc.CallPlugin("ffffffffffffffffffffffff", "hello", "x"));
    EXPECT_EQ((long)eNoSuchRoutine, doc.CallPlugin(IDA, "", "x"));
    EXPECT_EQ((long)eNoSuchRoutine, doc.CallPlugin(IDA, "nosuch", "x"));

    CPlugin* pA = doc.GetPlugin(IDA);
    ASSERT_TRUE(pA != nullptr);
    pA->m_bEnabled = false;
    EXPECT_EQ((long)ePluginDisabled, doc.CallPlugin(IDA, "hello", "x"));
    pA->m_bEnabled = true;

    EXPECT_EQ((long)eOK, doc.CallPlugin(IDA, "hello", "payload!"));
    EXPECT_STREQ("payload!", (LPCTSTR)plugin_var(pA, "arg"));
}

TEST(PluginInfoR4, PluginListGetters)
{
    CMUSHclientDoc doc;
    std::string p = write_plugin("lst", IDA, "Lister",
        "world.SetVariable('v1', 'a')\nworld.SetVariable('v2', 'b')\n");
    ASSERT_TRUE(load_plugin(doc, p));
    unlink(p.c_str());

    VARIANT v = doc.GetPluginVariableList(IDA);
    EXPECT_TRUE((v.vt & VT_ARRAY) != 0);
    VariantClear(&v);

    // and the current-plugin name from inside a callback
    std::string p2 = write_plugin("nm", IDB, "Namer",
        "world.SetVariable('myname', world.GetPluginName())\n");
    ASSERT_TRUE(load_plugin(doc, p2));
    unlink(p2.c_str());
    CPlugin* pB = doc.GetPlugin(IDB);
    EXPECT_STREQ("Namer", (LPCTSTR)plugin_var(pB, "myname"));
}

// A plugin's OnPluginLineReceived returning false omits the line from output.
TEST(PluginInfoR4, LineReceivedFalseOmitsLineFromOutput)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    std::string p = write_plugin("omit", IDA, "Omitter",
        "function OnPluginLineReceived(line)\n"
        "  return line:find('secret') == nil\n"
        "end\n");
    ASSERT_TRUE(load_plugin(doc, p));
    unlink(p.c_str());

    doc.DisplayMsg("public line\r\n", 13, 0);
    doc.DisplayMsg("secret line\r\n", 13, 0);
    doc.DisplayMsg("another public\r\n", 16, 0);

    EXPECT_TRUE(output_contains(doc, "public line"));
    EXPECT_TRUE(output_contains(doc, "another public"));
    EXPECT_TRUE(!output_contains(doc, "secret line"));   // omitted
}

// The non-Windows CScriptEngine::Execute used to declare its DISPPARAMS
// parameter WITHOUT A NAME and hand ExecuteLua two empty argument lists, so
// every argument a caller packed was dropped: world.CallPlugin's argument for a
// non-Lua target (methods_plugins.cpp:435) and every MXP callback argument
// (mxpStart / mxpClose / mxpError) arrived as nil.  Windows passes rgvarg
// straight to m_pDispatch->Invoke and never lost them.
TEST(ScriptEngineExecute, DispparamsReachTheCallee)
{
    CMUSHclientDoc doc; make_doc(doc);
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());       // false == created OK
    ASSERT_TRUE(!engine->Parse(CString(
        "got = 'NOTCALLED'\n"
        "function f(a) got = tostring(a) end\n"), "<test>"));
    doc.m_ScriptEngine = engine;

    DISPID d = engine->GetDispid("f");
    ASSERT_TRUE(d != DISPID_UNKNOWN);

    COleVariant args[1];
    args[0] = "hello";
    DISPPARAMS params = { args, NULL, 1, 0 };
    long n = 0;
    engine->Execute(d, "f", eDontChangeAction, "test", "probe", params, n, NULL);

    // Read the global back out of the same Lua state.
    lua_getglobal(engine->L, "got");
    const char* got = lua_tostring(engine->L, -1);
    EXPECT_STREQ("hello", got ? got : "(nil)");
    lua_pop(engine->L, 1);

    doc.m_ScriptEngine = nullptr;
    delete engine;
}

#endif // MUSHCLIENT_LUA_SCRIPTING


// ── round 5: plugin-driven engine arms ────────────────────────────────────

// A plugin approving an unknown telnet WILL option makes the client send
// IAC DO and the SENT_DO notification (telnet_phases default arms).
TEST(PluginR5, TelnetRequestApprovalSendsDoAndWill)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("tr_ok", IDA, "ReqOk",
        "function OnPluginTelnetRequest(t, s)\n"
        "  world.SetVariable('last', tostring(t) .. '|' .. s)\n"
        "  return true\n"
        "end\n")));

    doc.Phase_WILL(201);   // unknown option, plugin approves → IAC DO
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[201]);
    doc.Phase_DO(202);     // unknown option, plugin approves → IAC WILL
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WILL[202]);
}

// OnPluginSend returning false vetoes the outgoing command entirely
// (the DoSendMsg decline arm needs a live transport to get that far).
namespace {
struct NullTransport : pal::ITransport {
    void resolve(const char*, pal::ResolveToken) override {}
    void cancelResolve() override {}
    void connect(unsigned int, unsigned short, const pal::TransportOptions&) override {}
    void send(const char*, size_t) override {}
    void startTls(const pal::TlsParams&) override {}
    void close() override {}
    std::string lastErrorString() const override { return "null"; }
};
} // namespace

TEST(PluginR5, SendCallbackCanVetoOutgoingCommand)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pTransport = new NullTransport();   // doc dtor tears it down
    ASSERT_TRUE(load_plugin(doc, write_plugin("send_no", IDA, "SendVeto",
        "function OnPluginSend(s) return false end\n")));
    long before = doc.m_nTotalLinesSent;
    doc.DoSendMsg("blocked command", false, false);
    EXPECT_EQ(before, doc.m_nTotalLinesSent);   // vetoed: nothing went out
}

// OnPluginPacketReceived returning an empty string discards the packet.
TEST(PluginR5, PacketReceivedCallbackCanDiscardPacket)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("pk_zap", IDA, "PacketZap",
        "function OnPluginPacketReceived(s)\n"
        "  return ''\n"
        "end\n")));

    int linesBefore = (int)doc.m_LineList.GetCount();
    doc.DisplayMsg("swallowed\n", 10, 0);
    // nothing was added: the plugin ate the whole packet
    EXPECT_EQ(linesBefore, (int)doc.m_LineList.GetCount());
}

// A scripted trigger living in a PLUGIN dispatches through the plugin loop
// of the deferred trigger-script pass.
TEST(PluginR5, PluginScriptedTriggerExecutes)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("trig", IDA, "TrigPlugin",
        "function got_line(name, line, wildcards)\n"
        "  world.SetVariable('plugin_saw', line)\n"
        "end\n"
        "world.AddTrigger('ptrig', '*needle*', '', 1, -1, 0, '',"
        " 'got_line')\n")));   // flags: 1 = enabled

    doc.DisplayMsg("the needle line\n", 16, 0);

    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);
    EXPECT_STREQ("the needle line",
                 (const char*)plugin_var(p, "plugin_saw"));
}

// PluginListChanged from inside ON_PLUGIN_LIST_CHANGED does not recurse
// (the bInPluginListChanged guard): the callback loads ANOTHER plugin,
// whose own PluginListChanged is swallowed by the guard.
TEST(PluginR5, ListChangedCallbackDoesNotRecurse)
{
    CMUSHclientDoc doc; make_doc(doc);
    std::string third = write_plugin("lc3", "ccccccccccccccccccccccc3",
                                     "Third", "-- nothing\n");
    std::string body =
        "function OnPluginListChanged()\n"
        "  if not loaded_once then\n"
        "    loaded_once = true\n"
        "    world.SetVariable('rc', tostring(world.LoadPlugin('" + third + "')))\n"
        "  end\n"
        "end\n";
    ASSERT_TRUE(load_plugin(doc, write_plugin("lc1", IDA, "ListWatcher",
                                              body.c_str())));
    // InternalLoadPlugin does not fire the event; the doc-level LoadPlugin does
    std::string other = write_plugin("lc2", IDB, "Other", "-- nothing\n");
    EXPECT_EQ((long)eOK, doc.LoadPlugin(CString(other.c_str())));

    CPlugin* watcher = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, watcher);
    EXPECT_STREQ("0", (const char*)plugin_var(watcher, "rc"));  // eOK
    EXPECT_NE(nullptr, doc.GetPlugin("ccccccccccccccccccccccc3"));
}


// methods_plugins.cpp SaveState (world scope): "if (!m_CurrentPlugin) return
// eNotAPlugin" — world.SaveState() called with no plugin scope (a bare doc,
// no current plugin) is rejected: only a plugin may save state.
TEST(PluginState, DocSaveStateNoCurrentPluginReturnsNotAPlugin)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(doc.m_CurrentPlugin == NULL);        // no plugin scope
    EXPECT_EQ((long)eNotAPlugin, doc.SaveState());   // "only a plugin can do this"
}

// methods_plugins.cpp SaveState (world scope) success arm (line 514-517): with
// a current plugin whose CPlugin::SaveState(true) succeeds (returns false), the
// doc-level SaveState returns eOK and the state file is written.
TEST(PluginState, DocSaveStateWithCurrentPluginReturnsOk)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_strWorldID = "0123456789abcdef01234567";
    char dir[256];
    snprintf(dir, sizeof dir, "/tmp/mc_test_state_%d_docok/", (int)getpid());
    App.m_strDefaultStateFilesDirectory = dir;

    ASSERT_TRUE(load_plugin(doc, write_plugin("docok", IDA, "DocOkPlugin",
        "-- nothing\n")));
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    doc.m_CurrentPlugin = p;                        // "we are this plugin"
    EXPECT_EQ((long)eOK, doc.SaveState());          // success arm
    doc.m_CurrentPlugin = NULL;

    char expected[512];
    snprintf(expected, sizeof expected, "%s%s-%s-state.xml",
             dir, "0123456789abcdef01234567", IDA);
    struct stat st;
    EXPECT_EQ(0, stat(expected, &st));              // state file was written

    App.m_strDefaultStateFilesDirectory.Empty();
}

// methods_plugins.cpp GetPluginInfo cases 13/14: the SetUpVariantDate arm runs
// only when m_tDateWritten / m_tDateModified is non-zero (line 121/125); a
// stamped date yields VT_DATE.  AllInfoCasesAnswerTheirTypes only probes the
// zero side (VT_NULL); this pins the stamped (VT_DATE) side.
TEST(PluginInfoR4, DateWrittenAndModifiedReturnDateWhenStamped)
{
    CMUSHclientDoc doc; make_doc(doc);
    std::string path = write_plugin("dates", IDA, "DatesPlugin", "-- nothing\n");
    ASSERT_TRUE(load_plugin(doc, path));
    unlink(path.c_str());
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    // the loaded XML carried no date attributes → both stamps default to 0;
    // stamp them non-zero so the guard passes.
    p->m_tDateWritten  = CTime(2020, 1, 15, 10, 30, 0);
    p->m_tDateModified = CTime(2021, 6, 20,  8, 15, 0);

    VARIANT d13 = doc.GetPluginInfo(IDA, 13);
    EXPECT_EQ((int)VT_DATE, (int)d13.vt);          // case 13 stamped → DATE
    EXPECT_TRUE(d13.date > 0.0);
    VariantClear(&d13);

    VARIANT d14 = doc.GetPluginInfo(IDA, 14);
    EXPECT_EQ((int)VT_DATE, (int)d14.vt);          // case 14 stamped → DATE
    EXPECT_TRUE(d14.date > 0.0);
    VariantClear(&d14);
}

// methods_plugins.cpp ReloadPlugin: "if not found, try to find by name" (line
// 218-226) — a reload keyed on the plugin's case-insensitive NAME, not its
// 24-char ID.  UnloadPlugin-by-name is tested; ReloadPlugin-by-name was not.
TEST(PluginInfoR4, ReloadPluginByNameLookup)
{
    CMUSHclientDoc doc; make_doc(doc);
    std::string path = write_plugin("rlname", IDA, "ReloadByName",
        "world.SetVariable('n', '1')\n");
    ASSERT_TRUE(load_plugin(doc, path));
    ASSERT_TRUE(doc.GetPlugin(IDA) != NULL);

    // "reloadbyname" is 12 chars → GetPlugin() rejects it on length; only the
    // by-name arm can resolve it.  (Source file kept present so reload succeeds.)
    EXPECT_EQ((long)eOK, doc.ReloadPlugin("reloadbyname"));
    EXPECT_TRUE(doc.GetPlugin(IDA) != NULL);       // reinstalled

    unlink(path.c_str());
}

// methods_plugins.cpp ReloadPlugin: "cannot reload ourselves" (line 232-233) —
// a plugin reloading its own ID (m_CurrentPlugin == the target) gets
// eBadParameter and is NOT deleted.
TEST(PluginInfoR4, ReloadPluginCannotReloadItself)
{
    CMUSHclientDoc doc; make_doc(doc);
    std::string path = write_plugin("rlself", IDA, "ReloadSelf", "-- nothing\n");
    ASSERT_TRUE(load_plugin(doc, path));
    unlink(path.c_str());                          // a real reload would now fail
    CPlugin* p = doc.GetPlugin(IDA);
    ASSERT_NE(nullptr, p);

    doc.m_CurrentPlugin = p;                        // "we are this plugin"
    EXPECT_EQ((long)eBadParameter, doc.ReloadPlugin(IDA));
    doc.m_CurrentPlugin = NULL;
    EXPECT_TRUE(doc.GetPlugin(IDA) != NULL);        // guarded before any delete
}

// methods_plugins.cpp BroadcastPlugin: the self-skip "don't broadcast to
// ourselves" (line 596-597) and the disabled-skip "ignore disabled plugins"
// (line 592-593).  Issued while m_CurrentPlugin is a plugin whose
// ON_PLUGIN_BROADCAST is RESOLVED (so the sender WOULD fire without the skip),
// with a disabled receiver present.  (The old BroadcastPluginSkipsSender
// broadcasts from A's load body, when A's own dispid is not yet resolved, so it
// never really exercised the self-skip.)
TEST(PluginCallback, BroadcastSkipsSenderAndDisabledWhenCallbacksResolved)
{
    CMUSHclientDoc doc; make_doc(doc);
    // A = sender; its handler must NOT run despite a resolved dispid
    ASSERT_TRUE(load_plugin(doc, write_plugin("bsA", IDA, "SenderA",
        "function OnPluginBroadcast(msg, id, name, text)\n"
        "  world.SetVariable('self', 'WRONGLY INVOKED')\n"
        "end\n")));
    // B = enabled receiver
    ASSERT_TRUE(load_plugin(doc, write_plugin("bsB", IDB, "RecvB",
        "function OnPluginBroadcast(msg, id, name, text)\n"
        "  world.SetVariable('got', text .. '/' .. tostring(msg))\n"
        "end\n")));
    // D = disabled receiver (valid handler, but disabled)
    ASSERT_TRUE(load_plugin(doc, write_plugin("bsD", IDC, "RecvD",
        "function OnPluginBroadcast(msg, id, name, text)\n"
        "  world.SetVariable('dis', 'WRONGLY INVOKED')\n"
        "end\n")));

    CPlugin* pa = doc.GetPlugin(IDA);
    CPlugin* pb = doc.GetPlugin(IDB);
    CPlugin* pd = doc.GetPlugin(IDC);
    ASSERT_NE(nullptr, pa);
    ASSERT_NE(nullptr, pb);
    ASSERT_NE(nullptr, pd);
    // the sender's callback really IS resolved post-load (what the old test
    // could not establish)
    ASSERT_TRUE(pa->m_PluginCallbacks[ON_PLUGIN_BROADCAST].isvalid());
    pd->m_bEnabled = false;                         // disabled receiver

    doc.m_CurrentPlugin = pa;                        // we ARE the sender
    long rc = doc.BroadcastPlugin(7, "hello");
    doc.m_CurrentPlugin = NULL;

    EXPECT_EQ(1L, rc);                                           // only B counted
    EXPECT_STREQ("hello/7", (const char*)plugin_var(pb, "got")); // B received
    EXPECT_STREQ("<unset>", (const char*)plugin_var(pa, "self"));// A self-skipped
    EXPECT_STREQ("<unset>", (const char*)plugin_var(pd, "dis")); // D disabled-skipped
}

// ── the plugin veto ───────────────────────────────────────────────────────

// SendToAllPluginCallbacks asks every plugin "is this OK?" and the answer
// decides whether a line is sent, displayed or suppressed.  The rich Execute*
// overloads return early WITHOUT writing *result when the callback is not
// implemented -- and CPlugin::ExecutePluginScript passed that value straight
// back from an uninitialised `bool`.  So a plugin that simply does not
// implement OnPluginSend voted at random.
//
// A CONTRACT test, not a reproducer: an uninitialised read is undefined, so it
// could pass by luck before.  What it pins is that not implementing a callback
// is not a veto -- which is what every plugin that implements only one or two
// callbacks depends on.
TEST(PluginVeto, APluginThatDoesNotImplementTheCallbackDoesNotVeto)
{
    CMUSHclientDoc doc; make_doc(doc);
    // A plugin with a real engine, but no OnPluginSend anywhere in it.
    ASSERT_TRUE(load_plugin(doc, write_plugin("noveto", IDA, "NoVeto",
        "function OnPluginConnect() end\n")));
    ASSERT_NE(nullptr, doc.GetPlugin(IDA));

    for (int i = 0; i < 50; i++)
        EXPECT_TRUE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, "a line", true));
}

// ...and one that DOES implement it is still obeyed, both ways -- otherwise the
// assertion above would pass for an engine that always answers true.
TEST(PluginVeto, APluginThatImplementsTheCallbackIsObeyed)
{
    {
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("yes", IDB, "SaysYes",
        "function OnPluginSend(text) return true end\n")));
    EXPECT_TRUE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, "a line", true));
    }
    {
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_TRUE(load_plugin(doc, write_plugin("no", IDC, "SaysNo",
        "function OnPluginSend(text) return false end\n")));
    EXPECT_FALSE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, "a line", true));
    }
}

MC_TEST_MAIN()
