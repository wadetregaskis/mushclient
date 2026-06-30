// tests/test_world_load.cpp
// Tests for CMUSHclientDoc::LoadWorldFromFile — the portable XML world loader.
//
// Each test writes a minimal .mcl-format XML string to a temp file, calls
// LoadWorldFromFile, then inspects the resulting trigger/alias/variable/timer
// maps.
//
// KNOWN PORTABLE-SUBSET FAILURES (deferred — low priority):
// PluginLoad.MultipleDistinctPlugins and PluginLoad.ViaIncludeInWorldFile
// exercise plugin loading/flattening that the portable mushcore loader only
// partially implements; the product, mushgui, loads plugins through the full
// engine.  Closing this gap is deferred — the GUI is the product.

#include "mc_test.h"
#include "platform/doc_stub.h"

#include <cstdio>
#include <cstring>

#ifdef MUSHCLIENT_LUA_SCRIPTING
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
#endif

// ── helpers ───────────────────────────────────────────────────────────────

// Write text to a temporary file and return the path.  The file is in the
// system temp directory and survives until the test process exits (good enough
// for unit tests — no cleanup needed).
static std::string write_temp(const char* xml)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_%d.mcl", (int)getpid());
    FILE* f = fopen(path, "wb");
    if (!f) { perror("fopen temp"); return ""; }
    fwrite(xml, 1, strlen(xml), f);
    fclose(f);
    return path;
}

// Write text to a temp file with an explicit suffix/name, e.g. for
// plugin files that must sit alongside their world files.
static std::string write_temp_named(const char* name, const char* xml)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_%d_%s", (int)getpid(), name);
    FILE* f = fopen(path, "wb");
    if (!f) { perror("fopen temp"); return ""; }
    fwrite(xml, 1, strlen(xml), f);
    fclose(f);
    return path;
}

// ── Trigger loading ───────────────────────────────────────────────────────

TEST(WorldLoad, LoadOneTrigger)
{
    const char* xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="you have died" name="death" enabled="y" send_to="0" sequence="100">
    <send>say I died!</send>
  </trigger>
</triggers>
</muclient>)";

    std::string path = write_temp(xml);
    ASSERT_TRUE(!path.empty());

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(path.c_str()));

    CTrigger* t = nullptr;
    EXPECT_TRUE(doc.GetTriggerMap().Lookup("death", t));
    ASSERT_NE(nullptr, t);
    EXPECT_STREQ("you have died", (const char*)t->trigger);
    EXPECT_STREQ("say I died!", (const char*)t->contents);
    EXPECT_NE(nullptr, t->regexp);   // regexp should have been compiled
    EXPECT_TRUE(t->bEnabled);
    EXPECT_EQ(1,  (int)doc.GetTriggerArray().GetSize());
}

TEST(WorldLoad, NumericEntityAllowsLineFeedAndCarriageReturn)
{
    // XML attribute-value normalization (XML 1.0 section 3.3.3) replaces any
    // literal CR in the source with a space, so &#10; / &#13; numeric character
    // references are the ONLY way to embed a literal LF/CR inside an attribute
    // value -- a normal, spec-legal XML construct. CXMLparser::Get_XML_Entity
    // only exempted &#9; (tab) from its "disallowed control character" check,
    // rejecting &#10; and &#13; with "Disallowed number in XML entity" and
    // failing the whole world load -- inconsistent with the sibling MXP entity
    // decoder (mxpEntities.cpp), which correctly exempts 9, 10, and 13.
    const char* xml = "<?xml version=\"1.0\"?>\n"
"<!DOCTYPE muclient>\n"
"<muclient>\n"
"<triggers>\n"
"  <trigger match=\"abc&#10;def&#13;ghi\" name=\"t1\" enabled=\"y\">\n"
"    <send></send>\n"
"  </trigger>\n"
"</triggers>\n"
"</muclient>\n";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("t1", t));
    ASSERT_NE(nullptr, t);
    EXPECT_STREQ("abc\ndef\rghi", (const char*)t->trigger);
}

TEST(WorldLoad, TriggerRegexpFlag)
{
    const char* xml = R"xml(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="^You have (\d+) gold" name="gold" regexp="y" enabled="y">
    <send>say got %1 gold</send>
  </trigger>
</triggers>
</muclient>)xml";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CTrigger* t = nullptr;
    EXPECT_TRUE(doc.GetTriggerMap().Lookup("gold", t));
    ASSERT_NE(nullptr, t);
    EXPECT_TRUE(t->bRegexp);
    EXPECT_NE(nullptr, t->regexp);
}

TEST(WorldLoad, TriggerIgnoreCase)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="HELLO" name="hi" ignore_case="y" enabled="y">
    <send></send>
  </trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CTrigger* t = nullptr;
    EXPECT_TRUE(doc.GetTriggerMap().Lookup("hi", t));
    ASSERT_NE(nullptr, t);
    EXPECT_TRUE(t->ignore_case);
}

TEST(WorldLoad, MultipleTriggers)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="health" name="hp" enabled="y"><send></send></trigger>
  <trigger match="mana"   name="mp" enabled="y"><send></send></trigger>
  <trigger match="score"  name="sc" enabled="y"><send></send></trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    EXPECT_EQ(3, (int)doc.GetTriggerArray().GetSize());
}

TEST(WorldLoad, TriggerLoadedAndFires)
{
    // Round-trip: load a trigger from XML, then feed a matching line and
    // confirm the trigger fires via EvaluateTrigger.
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="ping" name="pinger" enabled="y" sequence="100">
    <send>pong</send>
  </trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_iFlags        = COLOUR_ANSI;
    doc.m_iForeColour   = WHITE;
    doc.m_iBackColour   = BLACK;
    doc.m_pCurrentLine  = new CLine();
    doc.AddStyle(COLOUR_ANSI, WHITE, BLACK, 0, static_cast<CAction*>(nullptr));

    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    // Feed a matching line through the full pipeline
    std::string s = "ping pong\n";
    doc.DisplayMsg(s.c_str(), (int)s.size(), 0);

    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("pinger", t));
    EXPECT_EQ(1L, t->nMatched);
}

// ── Alias loading ─────────────────────────────────────────────────────────

TEST(WorldLoad, LoadOneAlias)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<aliases>
  <alias match="n" name="north" enabled="y" sequence="100">
    <send>go north</send>
  </alias>
</aliases>
</muclient>)";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CAlias* a = nullptr;
    EXPECT_TRUE(doc.GetAliasMap().Lookup("north", a));
    ASSERT_NE(nullptr, a);
    EXPECT_STREQ("n", (const char*)a->name);
    EXPECT_STREQ("go north", (const char*)a->contents);
    EXPECT_NE(nullptr, a->regexp);
    EXPECT_TRUE(a->bEnabled);
}

TEST(WorldLoad, AliasRegexp)
{
    const char* xml = R"xml(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<aliases>
  <alias match="^go (\w+)" name="godir" regexp="y" enabled="y">
    <send>move %1</send>
  </alias>
</aliases>
</muclient>)xml";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CAlias* a = nullptr;
    EXPECT_TRUE(doc.GetAliasMap().Lookup("godir", a));
    ASSERT_NE(nullptr, a);
    EXPECT_TRUE(a->bRegexp);
}

// ── Variable loading ──────────────────────────────────────────────────────

TEST(WorldLoad, LoadVariables)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<variables>
  <variable name="hp">100</variable>
  <variable name="maxhp">200</variable>
  <variable name="greeting">hello world</variable>
</variables>
</muclient>)";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CVariable* v = nullptr;
    ASSERT_TRUE(doc.GetVariableMap().Lookup("hp", v));
    EXPECT_STREQ("100", (const char*)v->strContents);

    ASSERT_TRUE(doc.GetVariableMap().Lookup("maxhp", v));
    EXPECT_STREQ("200", (const char*)v->strContents);

    ASSERT_TRUE(doc.GetVariableMap().Lookup("greeting", v));
    EXPECT_STREQ("hello world", (const char*)v->strContents);
}

TEST(WorldLoad, VariableTrim)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<variables>
  <variable name="trimmed" trim="y">
    hello
  </variable>
</variables>
</muclient>)";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CVariable* v = nullptr;
    ASSERT_TRUE(doc.GetVariableMap().Lookup("trimmed", v));
    EXPECT_STREQ("hello", (const char*)v->strContents);
}

// ── Timer loading ─────────────────────────────────────────────────────────

TEST(WorldLoad, LoadIntervalTimer)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<timers>
  <timer name="heartbeat" enabled="y" hour="0" minute="0" second="5.0" type="0">
    <send>look</send>
  </timer>
</timers>
</muclient>)";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CTimer* t = nullptr;
    ASSERT_TRUE(doc.GetTimerMap().Lookup("heartbeat", t));
    EXPECT_EQ(CTimer::eInterval, t->iType);
    EXPECT_EQ(0,   t->iEveryHour);
    EXPECT_EQ(0,   t->iEveryMinute);
    EXPECT_NEAR(5.0, t->fEverySecond, 0.001);
    EXPECT_STREQ("look", (const char*)t->strContents);
}

// ── World settings ────────────────────────────────────────────────────────

TEST(WorldLoad, WorldSettings)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world name="My MUD" site="mud.example.com" port="4000">
</world>
</muclient>)";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    // site = host (m_server, canonical); name = world display name (m_mush_name).
    EXPECT_STREQ("mud.example.com", (const char*)doc.m_server);
    EXPECT_STREQ("My MUD",          (const char*)doc.m_mush_name);
    EXPECT_EQ(4000, (int)doc.m_port);
}

TEST(WorldLoad, PasswordBase64Load)
{
    // Windows stores passwords base64-encoded with a <name>_base64="y" flag.
    // "secret" -> "c2VjcmV0", "proxypw" -> "cHJveHlwdw==".
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="mud.example.com" port="4000"
       password="c2VjcmV0" password_base64="y"
       proxy_password="cHJveHlwdw==" proxy_password_base64="y">
</world>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    EXPECT_STREQ("secret",  (const char*)doc.m_password);
    EXPECT_STREQ("proxypw", (const char*)doc.m_strProxyPassword);
}

TEST(WorldLoad, PasswordSaveEncodesBase64)
{
    // A password set in the doc must be written base64-encoded with a
    // <name>_base64="y" flag (so it round-trips with Windows / mushcli, and the
    // plaintext never hits disk). "secret" -> "c2VjcmV0".
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="mud.example.com" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_pw_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
        doc.m_password = "secret";
        ASSERT_TRUE(doc.OnSaveDocument(path));
    }

    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    std::string content; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) content.append(buf, n);
    fclose(f);

    EXPECT_NE(content.find("password=\"c2VjcmV0\""), std::string::npos);  // base64
    EXPECT_NE(content.find("password_base64=\"y\""), std::string::npos);  // flag
    EXPECT_EQ(content.find("\"secret\""),            std::string::npos);  // not plaintext
}

TEST(WorldLoad, DefaultWorldSaveUseMxpInRange)
{
    // Regression for the broken portable iUseMXP enum: m_iUseMXP defaulted to
    // eNoMXP, which under the old enum was 4 — out of use_mxp's 0-3 range — and
    // the saver wrote it out, making the world unloadable. With the corrected
    // enum the default is eOnCommandMXP (0 == OptionsTable default), so use_mxp
    // is omitted; in any case it must never be written out of range.
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="mud.example.com" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_rt_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
        ASSERT_TRUE(doc.OnSaveDocument(path));
    }
    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    std::string content; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) content.append(buf, n);
    fclose(f);

    EXPECT_EQ(content.find("use_mxp=\"4\""), std::string::npos);  // never out of range
    EXPECT_EQ(content.find("use_mxp="),      std::string::npos);  // default → omitted
}

// ── Error handling ────────────────────────────────────────────────────────

TEST(WorldLoad, MissingFileReturnsFalse)
{
    CMUSHclientDoc doc;
    EXPECT_FALSE(doc.LoadWorldFromFile("/nonexistent/path/world.mcl"));
}

TEST(WorldLoad, EmptyXMLReturnsFalse)
{
    std::string path = write_temp("");
    CMUSHclientDoc doc;
    // Zero-length file: parser should report error gracefully
    EXPECT_FALSE(doc.LoadWorldFromFile(path.c_str()));
}

TEST(WorldLoad, TriggerWithNoMatchSkipped)
{
    // A trigger with no match text should be silently skipped
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="" name="empty" enabled="y"><send></send></trigger>
  <trigger match="ok" name="good" enabled="y"><send></send></trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    // Only the valid trigger should appear
    EXPECT_EQ(1, (int)doc.GetTriggerArray().GetSize());
    CTrigger* t = nullptr;
    EXPECT_TRUE(doc.GetTriggerMap().Lookup("good", t));
}

// ── Script loading ────────────────────────────────────────────────────────

#ifdef MUSHCLIENT_LUA_SCRIPTING
#include "scripting/scripting.h"

TEST(WorldLoad, InlineScriptCreatesEngine)
{
    // A world with an inline <script> block should end up with a live
    // Lua engine after LoadWorldFromFile.
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000" script_language="lua">
</world>
<script>
-- trivial Lua so the parser has something to chew on
function OnWorldOpen()
  return true
end
</script>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    ASSERT_NE(nullptr, doc.m_ScriptEngine);
    EXPECT_TRUE(doc.m_ScriptEngine->IsLua());
}

TEST(WorldLoad, InlineScriptDispidResolvable)
{
    // After loading, GetDispid should find a function defined in the script.
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world script_language="lua">
</world>
<script>
function MyHandler()
  return 42
end
</script>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    ASSERT_NE(nullptr, doc.m_ScriptEngine);

    DISPID id = doc.m_ScriptEngine->GetLuaDispid("MyHandler");
    EXPECT_NE(DISPID_UNKNOWN, id);
}

TEST(WorldLoad, ScriptLanguageDefaultsToLua)
{
    // A world with a <script> block but no explicit script_language
    // should still initialise a Lua engine (default language is Lua).
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<script>
function Ping() return "pong" end
</script>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    ASSERT_NE(nullptr, doc.m_ScriptEngine);
    EXPECT_TRUE(doc.m_ScriptEngine->IsLua());
}

TEST(WorldLoad, NoScriptNoEngine)
{
    // A world with no <script> block should have no script engine.
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="ping" name="t1" enabled="y"><send>pong</send></trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    EXPECT_EQ(nullptr, doc.m_ScriptEngine);
}

TEST(WorldLoad, TriggerWithScriptProcedure)
{
    // A trigger with a script= attribute should have its dispid resolved
    // after the world is loaded (GetLuaDispid on the named function).
    const char* xml = R"xml(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world script_language="lua">
</world>
<triggers>
  <trigger match="^health: (\d+)" name="hp_trigger" regexp="y"
           enabled="y" script="OnHealth">
    <send></send>
  </trigger>
</triggers>
<script>
function OnHealth(name, line, wildcards)
  -- would update HP display
end
</script>
</muclient>)xml";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    ASSERT_NE(nullptr, doc.m_ScriptEngine);

    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("hp_trigger", t));
    ASSERT_NE(nullptr, t);
    EXPECT_STREQ("OnHealth", (const char*)t->strProcedure);
}

TEST(WorldLoad, TriggerColourSplitDoesNotEatTrailingChar)
{
    // Regression test for an off-by-one in ProcessPreviousLine.cpp's trigger
    // style-splitting logic (the code that colours only the matched portion of
    // a line, used both on-screen and for the TriggerStyleRuns table passed to
    // "send to script after omit" triggers). PCRE match offsets are [start,end)
    // — end is EXCLUSIVE, one past the last matched char — matching the style
    // run's own iStyleEndCol convention. The split-after-match condition was
    // `iEndCol < (iStyleEndCol - 1)` instead of `iEndCol < iStyleEndCol`, so
    // whenever the match ended exactly one character before the style's end
    // (e.g. a whole-line style run with one trailing unmatched char), the
    // split was skipped and the trailing character was wrongly absorbed into
    // the coloured/matched style run.
    //
    // Trigger matches "fooba" (cols 0-4) within the single-style line "foobar"
    // (cols 0-5, iStyleEndCol=6 exclusive) — exactly the boundary case
    // (iEndCol=5, iStyleEndCol-1=5) that the buggy condition mishandled.
    const char* xml = R"xml(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world script_language="lua">
</world>
<triggers>
  <trigger match="fooba" name="t1" enabled="y" custom_colour="1" send_to="14">
    <send></send>
  </trigger>
</triggers>
<script>
function Noop() end
</script>
</muclient>)xml";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    ASSERT_NE(nullptr, doc.m_ScriptEngine);
    ASSERT_TRUE(doc.m_ScriptEngine->IsLua());

    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_iFlags        = COLOUR_ANSI;
    doc.m_iForeColour   = WHITE;
    doc.m_iBackColour   = BLACK;
    doc.m_pCurrentLine  = new CLine();
    doc.AddStyle(COLOUR_ANSI, WHITE, BLACK, 0, static_cast<CAction*>(nullptr));

    const char seq[] = "foobar\n";
    doc.DisplayMsg(seq, (int)strlen(seq), 0);

    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("t1", t));
    EXPECT_EQ(1L, t->nMatched);

    // The trigger's deferred "send to script" runs synchronously inside
    // DisplayMsg with the TriggerStyleRuns global set from the (possibly
    // mis-split) style runs — inspect it directly.
    lua_State* L = doc.m_ScriptEngine->L;
    ASSERT_NE(nullptr, L);
    lua_getglobal(L, "TriggerStyleRuns");
    ASSERT_TRUE(lua_istable(L, -1));

    lua_rawgeti(L, -1, 1);
    ASSERT_TRUE(lua_istable(L, -1));
    lua_getfield(L, -1, "text");
    std::string run1 = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 2);   // pop text, run1 table

    lua_rawgeti(L, -1, 2);
    bool hasRun2 = lua_istable(L, -1);
    std::string run2;
    if (hasRun2) {
        lua_getfield(L, -1, "text");
        run2 = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
    }
    lua_pop(L, 1);   // pop run2 (table or nil)
    lua_pop(L, 1);   // pop TriggerStyleRuns

    EXPECT_STREQ("fooba", run1.c_str());      // only the matched text is coloured
    ASSERT_TRUE(hasRun2);                      // the trailing "r" must be a separate run
    EXPECT_STREQ("r", run2.c_str());           // and must not include the matched text
}

#endif // MUSHCLIENT_LUA_SCRIPTING

// ── Timer reset after load ─────────────────────────────────────────────────

TEST(WorldLoad, TimersResetAfterLoad)
{
    // After LoadWorldFromFile, timers should have a tFireTime in the future.
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<timers>
  <timer name="tick" enabled="y" hour="0" minute="0" second="5.0" type="0">
    <send>look</send>
  </timer>
</timers>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CTimer* t = nullptr;
    ASSERT_TRUE(doc.GetTimerMap().Lookup("tick", t));
    // tFireTime should be strictly after the load time (i.e. in the future)
    EXPECT_TRUE(t->tFireTime > CmcDateTime::GetTimeNow() - CmcDateTimeSpan(0, 0, 0, 1.0));
}

// ── Plugin loading ────────────────────────────────────────────────────────

// Minimal valid plugin XML — supplies required name and id attributes.
static const char* PLUGIN_BASIC = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<plugin name="TestPlugin" author="TestAuthor" id="test-plugin-0001"
        version="2.5" />
</muclient>)";

// Plugin XML that also defines a trigger.
static const char* PLUGIN_WITH_TRIGGER = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<plugin name="TrigPlugin" author="A" id="trig-plugin-0001" version="1.0" />
<triggers>
  <trigger match="plugin fires" name="ptrig" enabled="y" sequence="100">
    <send>yes it does</send>
  </trigger>
</triggers>
</muclient>)";

// Plugin XML that defines a variable.
static const char* PLUGIN_WITH_VARIABLE = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<plugin name="VarPlugin" author="A" id="var-plugin-0001" version="1.0" />
<variables>
  <variable name="plugin_var">plugin_value</variable>
</variables>
</muclient>)";

TEST(PluginLoad, BasicMetadataStored)
{
    std::string path = write_temp_named("basic.xml", PLUGIN_BASIC);
    ASSERT_TRUE(!path.empty());

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.InternalLoadPlugin(CString(path.c_str())));

    ASSERT_EQ(1u, doc.m_PluginList.size());
    CPlugin* p = doc.m_PluginList.front();
    EXPECT_STREQ("TestPlugin",        (const char*)p->m_strName);
    EXPECT_STREQ("TestAuthor",        (const char*)p->m_strAuthor);
    EXPECT_STREQ("test-plugin-0001",  (const char*)p->m_strID);
    EXPECT_NEAR(2.5, p->m_dVersion, 0.001);
}

TEST(PluginLoad, SourcePathRecorded)
{
    std::string path = write_temp_named("srcpath.xml", PLUGIN_BASIC);
    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.InternalLoadPlugin(CString(path.c_str())));

    CPlugin* p = doc.m_PluginList.front();
    // m_strSource should equal the path we passed in
    EXPECT_STREQ(path.c_str(), (const char*)p->m_strSource);
}

TEST(PluginLoad, TriggersLandInPluginMap)
{
    // Triggers defined inside a plugin XML must be stored in the plugin's own
    // m_TriggerMap, not the doc's map.  This mirrors the Windows behaviour:
    // GetTriggerMap() returns plugin->m_TriggerMap when m_CurrentPlugin is set,
    // so InternalLoadPlugin (which sets m_CurrentPlugin = plugin before loading)
    // routes items into the plugin's collections.
    std::string path = write_temp_named("trig.xml", PLUGIN_WITH_TRIGGER);
    ASSERT_TRUE(!path.empty());

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.InternalLoadPlugin(CString(path.c_str())));

    ASSERT_EQ(1u, doc.m_PluginList.size());
    CPlugin* p = doc.m_PluginList.front();

    // Item must be in the plugin's map, NOT in the doc's map.
    CTrigger* t = nullptr;
    EXPECT_TRUE(p->m_TriggerMap.Lookup("ptrig", t));
    ASSERT_NE(nullptr, t);
    EXPECT_STREQ("plugin fires",  (const char*)t->trigger);
    EXPECT_STREQ("yes it does",   (const char*)t->contents);

    // Doc map must NOT contain the plugin's trigger.
    CTrigger* dt = nullptr;
    EXPECT_FALSE(doc.m_TriggerMap.Lookup("ptrig", dt));
}

TEST(PluginLoad, VariablesLandInPluginMap)
{
    // Variables defined inside a plugin XML must be stored in the plugin's
    // own m_VariableMap, mirroring the Windows behaviour.
    std::string path = write_temp_named("var.xml", PLUGIN_WITH_VARIABLE);
    ASSERT_TRUE(!path.empty());

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.InternalLoadPlugin(CString(path.c_str())));

    ASSERT_EQ(1u, doc.m_PluginList.size());
    CPlugin* p = doc.m_PluginList.front();

    CVariable* v = nullptr;
    EXPECT_TRUE(p->m_VariableMap.Lookup("plugin_var", v));
    ASSERT_NE(nullptr, v);
    EXPECT_STREQ("plugin_value", (const char*)v->strContents);

    // Doc map must NOT contain the plugin's variable.
    CVariable* dv = nullptr;
    EXPECT_FALSE(doc.m_VariableMap.Lookup("plugin_var", dv));
}

TEST(PluginLoad, MissingFileReturnsFalse)
{
    CMUSHclientDoc doc;
    EXPECT_FALSE(doc.InternalLoadPlugin(CString("/nonexistent/plugin.xml")));
    EXPECT_EQ(0u, doc.m_PluginList.size());
}

// Verify that triggers loaded from a world file are sorted by sequence number
// in the TriggerArray, regardless of their order in the XML.  This ensures
// ProcessPreviousLine evaluates them in the correct priority order.
TEST(WorldLoad, TriggerArraySortedBySequence)
{
    // Write the high-sequence trigger FIRST in XML — load must still sort it last.
    static const char* xml = R"(<?xml version="1.0"?>
<muclient>
<triggers>
  <trigger match="high priority" name="hi"  enabled="y" sequence="200">
    <send>hi fires</send>
  </trigger>
  <trigger match="low priority"  name="lo"  enabled="y" sequence="100">
    <send>lo fires</send>
  </trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    // Array must have both triggers, with sequence-100 at index 0.
    ASSERT_EQ(2, doc.m_TriggerArray.GetSize());
    CTrigger* first  = doc.m_TriggerArray[0];
    CTrigger* second = doc.m_TriggerArray[1];
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    EXPECT_EQ(100, first->iSequence);
    EXPECT_EQ(200, second->iSequence);
}

// Same check for a plugin's trigger array.
TEST(PluginLoad, TriggerArraySortedBySequence)
{
    static const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<plugin name="SortPlugin" author="A" id="sort-plugin-0001" version="1.0" />
<triggers>
  <trigger match="high" name="hi" enabled="y" sequence="200">
    <send>hi</send>
  </trigger>
  <trigger match="low"  name="lo" enabled="y" sequence="100">
    <send>lo</send>
  </trigger>
</triggers>
</muclient>)";

    std::string path = write_temp_named("sort.xml", xml);
    ASSERT_TRUE(!path.empty());

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.InternalLoadPlugin(CString(path.c_str())));

    ASSERT_EQ(1u, doc.m_PluginList.size());
    CPlugin* p = doc.m_PluginList.front();

    // Plugin's TriggerArray must be sorted: sequence 100 before 200.
    ASSERT_EQ(2, p->m_TriggerArray.GetSize());
    CTrigger* first  = p->m_TriggerArray[0];
    CTrigger* second = p->m_TriggerArray[1];
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    EXPECT_EQ(100, first->iSequence);
    EXPECT_EQ(200, second->iSequence);

    // Doc's own array must be empty (triggers are plugin-owned).
    EXPECT_EQ(0, doc.m_TriggerArray.GetSize());
}

TEST(PluginLoad, NoNameRejected)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<plugin author="A" id="no-name-0001" version="1.0" />
</muclient>)";

    std::string path = write_temp_named("noname.xml", xml);
    CMUSHclientDoc doc;
    EXPECT_FALSE(doc.InternalLoadPlugin(CString(path.c_str())));
    EXPECT_EQ(0u, doc.m_PluginList.size());
}

TEST(PluginLoad, NoIDRejected)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<plugin name="NoID" author="A" version="1.0" />
</muclient>)";

    std::string path = write_temp_named("noid.xml", xml);
    CMUSHclientDoc doc;
    EXPECT_FALSE(doc.InternalLoadPlugin(CString(path.c_str())));
    EXPECT_EQ(0u, doc.m_PluginList.size());
}

TEST(PluginLoad, DuplicateIDRejected)
{
    std::string path = write_temp_named("dup.xml", PLUGIN_BASIC);
    CMUSHclientDoc doc;

    EXPECT_TRUE(doc.InternalLoadPlugin(CString(path.c_str())));
    EXPECT_EQ(1u, doc.m_PluginList.size());

    // Second load of same ID should be silently rejected
    EXPECT_FALSE(doc.InternalLoadPlugin(CString(path.c_str())));
    EXPECT_EQ(1u, doc.m_PluginList.size());   // still only one
}

TEST(PluginLoad, MultipleDistinctPlugins)
{
    std::string p1 = write_temp_named("multi1.xml", PLUGIN_WITH_TRIGGER);
    std::string p2 = write_temp_named("multi2.xml", PLUGIN_WITH_VARIABLE);
    ASSERT_TRUE(!p1.empty()); ASSERT_TRUE(!p2.empty());

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.InternalLoadPlugin(CString(p1.c_str())));
    EXPECT_TRUE(doc.InternalLoadPlugin(CString(p2.c_str())));
    EXPECT_EQ(2u, doc.m_PluginList.size());

    // Each plugin's items live in that plugin's own maps (plugin isolation),
    // not the doc's — search the loaded plugins (cf. SingleTriggerPlugin above).
    CTrigger* t = nullptr;
    CVariable* v = nullptr;
    bool haveTrig = false, haveVar = false;
    for (CPlugin* p : doc.m_PluginList) {
        if (p->m_TriggerMap.Lookup("ptrig", t))       haveTrig = true;
        if (p->m_VariableMap.Lookup("plugin_var", v))  haveVar  = true;
    }
    EXPECT_TRUE(haveTrig);
    EXPECT_TRUE(haveVar);
}

TEST(PluginLoad, ViaIncludeInWorldFile)
{
    // Write the plugin file, then a world file that references it via <include>.
    std::string pluginPath = write_temp_named("inc_plugin.xml", PLUGIN_WITH_TRIGGER);
    ASSERT_TRUE(!pluginPath.empty());

    // Build a world XML that includes the plugin by absolute path
    char worldXml[1024];
    snprintf(worldXml, sizeof(worldXml), R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
<include name="%s" plugin="y" />
</muclient>)", pluginPath.c_str());

    std::string worldPath = write_temp_named("inc_world.mcl", worldXml);
    ASSERT_TRUE(!worldPath.empty());

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(worldPath.c_str()));

    // Plugin should have been loaded
    EXPECT_EQ(1u, doc.m_PluginList.size());
    EXPECT_STREQ("TrigPlugin", (const char*)doc.m_PluginList.front()->m_strName);

    // Plugin's trigger lives in the plugin's own map (isolation), not the doc's.
    CTrigger* t = nullptr;
    EXPECT_TRUE(doc.m_PluginList.front()->m_TriggerMap.Lookup("ptrig", t));
}

TEST(PluginLoad, NoPluginElementReturnsFalse)
{
    // Valid XML but no <plugin> metadata element
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="test" name="t1" enabled="y"><send>ok</send></trigger>
</triggers>
</muclient>)";

    std::string path = write_temp_named("nopluginelem.xml", xml);
    CMUSHclientDoc doc;
    EXPECT_FALSE(doc.InternalLoadPlugin(CString(path.c_str())));
}

// ── World settings — OptionsTable-driven loading ─────────────────────────

TEST(WorldLoad, WorldSettingsNumericOptions)
{
    // Verify OptionsTable-driven loading: boolean, range, and alpha options
    // all survive a round-trip through LoadWorldFromFile.
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000"
  enable_triggers="n"
  enable_aliases="n"
  enable_timers="n"
  wrap_column="120"
  max_output_lines="5000"
  use_ssl="y"
  proxy_type="2"
  proxy_port="1080"
  send_keep_alives="y"
  save_world_automatically="y"
  utf_8="y"
  connect_method="1"
  player="wizard"
  script_prefix="#"
  proxy_server="proxy.example.com"
  proxy_username="user1"
>
</world>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    // Numeric/boolean live members
    EXPECT_EQ(0,    (int)doc.m_enable_triggers);
    EXPECT_EQ(0,    (int)doc.m_enable_aliases);
    EXPECT_EQ(0,    (int)doc.m_bEnableTimers);
    EXPECT_EQ(120,  (int)doc.m_nWrapColumn);
    EXPECT_EQ(5000, (long)doc.m_maxlines);
    EXPECT_EQ(1,    (int)doc.m_bUseSSL);
    EXPECT_EQ(2,    (int)doc.m_iSocksProcessing);   // eProxyServerSocks5
    EXPECT_EQ(1080, (int)doc.m_iProxyServerPort);
    EXPECT_EQ(1,    (int)doc.m_bSendKeepAlives);
    EXPECT_EQ(1,    (int)doc.m_bSaveWorldAutomatically);
    EXPECT_EQ(1,    (int)doc.m_bUTF_8);
    EXPECT_EQ(1,    (int)doc.m_connect_now);         // connect_method

    // Alpha live members
    EXPECT_STREQ("wizard",            (const char*)doc.m_name);
    EXPECT_STREQ("#",                 (const char*)doc.m_strScriptPrefix);
    EXPECT_STREQ("proxy.example.com", (const char*)doc.m_strProxyServerName);
    EXPECT_STREQ("user1",             (const char*)doc.m_strProxyUserName);
}

TEST(WorldLoad, WorldSettingsInOptionCache)
{
    // Values loaded from XML must also be visible via the option caches so
    // that GetOption / GetAlphaOption return correct values without a live
    // struct member for every option.
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="cache.mud" port="9999"
  wrap_column="100"
  proxy_server="myproxy"
>
</world>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    // Numeric cache
    auto nit = doc.m_numericOptionCache.find("wrap_column");
    ASSERT_NE(doc.m_numericOptionCache.end(), nit);
    EXPECT_EQ(100L, nit->second);

    // Alpha cache
    auto ait = doc.m_alphaOptionCache.find("proxy_server");
    ASSERT_NE(doc.m_alphaOptionCache.end(), ait);
    EXPECT_EQ("myproxy", ait->second);
}

MC_TEST_MAIN()
