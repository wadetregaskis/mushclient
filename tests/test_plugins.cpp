// tests/test_plugins.cpp
// Intent-comment tests for the plugin subsystem: callback dispatch semantics
// (plugins.cpp), plugin lookup/lifecycle (methods_plugins.cpp), CallPlugin
// marshalling (lua_methods.cpp), state saving (CPlugin::SaveState) and the
// per-plugin Lua sandbox (lua_scripting.cpp).
//
// Real plugins are loaded through the REAL InternalLoadPlugin from temp XML
// files, exactly as the product does.

#include "mc_test.h"
#include "platform/doc_stub.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// ── helpers ───────────────────────────────────────────────────────────────

static CMUSHclientDoc make_doc()
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    return doc;
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
TEST(PluginCallback, PluginLookupRequiresExact24CharID)
{
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
    ASSERT_TRUE(load_plugin(doc, write_plugin("zero", IDA, "ZeroPlugin",
        "function OnPluginSend(text) return 0 end\n")));

    EXPECT_FALSE(doc.SendToAllPluginCallbacks(ON_PLUGIN_SEND, "x", true));
}

// plugins.cpp SendToAllPluginCallbacks(bStopOnFalse): "we only return true
// if each plugin returned true" — any veto blackballs the action.
TEST(PluginCallback, AnyPluginFalseVetoesSendCallback)
{
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc2 = make_doc();
    ASSERT_TRUE(load_plugin(doc2, write_plugin("first_none", IDA, "NoTrace",
        "-- nothing\n")));
    EXPECT_FALSE(doc2.SendToFirstPluginCallbacks(ON_PLUGIN_TRACE, "msg"));
}

// plugins.h m_strCallingPluginID: "during a CallPlugin - the ID of the
// calling plugin" — visible to the callee as GetPluginInfo(id, 23), and
// reverted once the call returns.
TEST(PluginCallback, CallPluginExposesCallerIDViaInfoType23)
{
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
    ASSERT_TRUE(load_plugin(doc, write_plugin("rtn_keep", IDA, "RtnKeep",
        "function OnPluginCommandEntered(t) world.SetVariable('saw', t) end\n")));
    CString cmd("keepme");
    doc.SendToAllPluginCallbacksRtn(ON_PLUGIN_COMMAND_ENTERED, cmd);
    EXPECT_STREQ("keepme", (const char*)cmd);    // nil return: unchanged
    EXPECT_STREQ("keepme",
                 (const char*)plugin_var(doc.GetPlugin(IDA), "saw"));

    CMUSHclientDoc doc2 = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc2 = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc2 = make_doc();
    ASSERT_TRUE(load_plugin(doc2, write_plugin("tr_no", IDA, "ReqFalse",
        "function OnPluginTelnetRequest(t, s) return false end\n")));
    EXPECT_FALSE(doc2.SendToAllPluginCallbacks(ON_PLUGIN_TELNET_REQUEST,
                                               (long)1, string("x"),
                                               false, true));

    // no handler anywhere: tail inversion (same rule as 3-argument form)
    CMUSHclientDoc doc3 = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
    CPlugin* p = add_bare_plugin(doc, IDA, "NoEngine");
    EXPECT_EQ(DISPID_UNKNOWN, p->GetPluginDispid("OnPluginSend"));
}

// plugins.cpp SaveState: "need a directory" — with no state directory
// configured the save is skipped entirely: no OnPluginSaveState callback,
// reported as 'not needed' (true).
TEST(PluginState, SaveStateSkippedWhenNoStateDirConfigured)
{
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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

#endif // MUSHCLIENT_LUA_SCRIPTING

MC_TEST_MAIN()
