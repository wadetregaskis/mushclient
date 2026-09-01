// tests/test_world_load.cpp
// Tests for CMUSHclientDoc::LoadWorldFromFile — the portable XML world loader.
//
// Each test writes a minimal .mcl-format XML string to a temp file, calls
// LoadWorldFromFile, then inspects the resulting trigger/alias/variable/timer
// maps.
//
// The loader under test is the REAL Windows loader (xml/xml_load_world.cpp,
// adopted into mushcore); LoadWorldFromFile (doc_portable.cpp) drives it the
// way the Windows Serialize loading branch does (serialize.cpp:44-110).

#include "mc_test.h"
// B-3 atomic flip: real headers (portable stub deleted; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "platform/wx_mfc_hooks.h"
// The notepad fixture is #ifndef _WIN32 (see its own header): a notepad on
// Windows is a real CEditView from the MFC doc/view framework, with no
// callback seam to substitute and no doc template in this host.  The few cases
// below that use it are guarded individually so the rest of the file still
// runs there.
#include "notepad_fixture.h"   // NotepadCapture (XML-import notes seam)

#include <cstdio>
#include <cstring>
#include <string>
#include "posix_compat.h"
#include <sys/stat.h> // mkdir (plugin state dir)

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

// Load a plugin through the REAL (void, throwing) InternalLoadPlugin
// (xml/xml_load_world.cpp:2657) and report success as a bool, wrapping the
// exceptions exactly like the product caller (methods_plugins.cpp
// LoadPlugin: catch CFileException*/CArchiveException*).
static bool TryLoadPlugin(CMUSHclientDoc& doc, const char* path)
{
    try { doc.InternalLoadPlugin(CString(path)); return true; }
    catch (CException* e) { e->Delete(); return false; }
    catch (...)           { return false; }
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

TEST(WorldLoad, AbsentAlphaOptionsFollowWindowsSemantics)
{
    // Windows semantics (xml_load_world.cpp Load_World_Single_Line_Alpha_
    // Options_XML): an alpha option absent from the <world> tag is BLANKED —
    // the canonical saver (xml_save_world.cpp, adopted into mushcore) writes
    // every non-empty value, so absent means empty.  Hand-written minimal
    // world files keep the table defaults by saying defaults="y", as on
    // Windows (the ctor's SetAlphaDefaults seeds them; the loader then leaves
    // absent options alone).  (This replaces the twin-saver-era test that
    // asserted absent alphas keep their defaults unconditionally — that
    // matched the deleted omitting saver, not Windows.)
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";

    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

        // command_stack_character: the canonical setter refuses an empty
        // value (OPT_COMMAND_STACK, scriptingoptions.cpp:363-373 — returns
        // eOptionOutOfRange and forces command stacking OFF), so the ctor's
        // ";" survives the blanking attempt — identical on Windows.
        EXPECT_STREQ(";", (const char*)doc.m_strCommandStackCharacter);
        EXPECT_FALSE(doc.m_enable_command_stack);
        EXPECT_STREQ("", (const char*)doc.m_speed_walk_prefix);
        EXPECT_STREQ("", (const char*)doc.m_strSpamMessage);
        EXPECT_STREQ("", (const char*)doc.m_strAutoSayString);
        // OPT_WORLD_ID is the exception: absent → a fresh 24-hex unique ID,
        // never blank (xml_load_world.cpp:1128-1129).
        EXPECT_EQ(24, doc.m_strWorldID.GetLength());
    }

    const char* xmlDefaults = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000" defaults="y">
</world>
</muclient>)";

    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xmlDefaults).c_str()));

        EXPECT_STREQ(";",     (const char*)doc.m_strCommandStackCharacter);
        EXPECT_STREQ("#",     (const char*)doc.m_speed_walk_prefix);
        EXPECT_STREQ("look",  (const char*)doc.m_strSpamMessage);
        EXPECT_STREQ("say ",  (const char*)doc.m_strAutoSayString);
    }
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

    // Feed a matching line through the full pipeline. A plain (non-regexp)
    // trigger is whole-line-anchored (^ping$, Windows parity), so the line must
    // be exactly "ping" — not merely contain it.
    std::string s = "ping\n";
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

// ── Unlimited scrollback (max_output_lines = 0) ────────────────────────────
//
// 0 is the OPT_ZERO_UNLIMITED sentinel: legal below the option's 200-line
// minimum, and it means "never evict".  It has to survive a save/reload, or a
// user who sets it loses it the next time the world file is written — and the
// failure would be silent, since the value would just come back as the 5,000
// default.
TEST(WorldLoad, UnlimitedScrollbackSentinelRoundTrips)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="mud.example.com" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_unlim_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
        ASSERT_EQ(5000L, (long)doc.m_maxlines);          // the default
        doc.m_maxlines = 0;
        ASSERT_TRUE(doc.OnSaveDocument(path));
    }

    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    std::string content; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) content.append(buf, n);
    fclose(f);
    EXPECT_NE(content.find("max_output_lines=\"0\""), std::string::npos);

    CMUSHclientDoc reloaded;
    ASSERT_TRUE(reloaded.LoadWorldFromFile(path));
    EXPECT_EQ(0L, (long)reloaded.m_maxlines);
    remove(path);
}



// ── A preamble's trailing space is the whole point of a preamble ───────────
//
// `Get_XML_string` trims leading and trailing whitespace unless the option row
// carries `OPT_KEEP_SPACES`, and `doc.cpp` concatenates a Send-to-world line
// with no separator at all:
//
//     full_line  = dlg.m_strLinePreamble;
//     full_line += str;
//     full_line += dlg.m_strLinePostamble;
//
// so the space in `"say "` is the only thing between the verb and the text.
// `send_to_world_line_preamble` and `…_postamble` were the only two
// non-multiline `*amble`/prefix rows in the whole table without the flag —
// the six log-line ones, both paste-line ones, the recall one, the chat prefix,
// the speedwalk prefix and all three timestamps have it. An omission, not a
// judgement. `docs/UPSTREAM-BUGS.md` 104.
//
// The paste twins are the control: they were always right, so a test that only
// checked the send pair could pass on a build where the trim had been removed
// wholesale.
TEST(WorldLoad, ASendPreamblesTrailingSpaceSurvivesTheLoad)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="mud.example.com" port="4000"
       send_to_world_line_preamble="say "
       send_to_world_line_postamble=" ."
       paste_line_preamble="tell bob "
       paste_line_postamble=" !">
</world>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    EXPECT_STREQ("say ", (const char*)doc.m_line_preamble);
    EXPECT_STREQ(" .",   (const char*)doc.m_line_postamble);

    // the control pair, which never lost theirs
    EXPECT_STREQ("tell bob ", (const char*)doc.m_pasteline_preamble);
    EXPECT_STREQ(" !",        (const char*)doc.m_pasteline_postamble);
}
// ── A meaningful zero on an option whose DEFAULT is not zero ───────────────
//
// The sibling of the test above, and the case its own comment did not expect to
// exist. `Save_XML_number` omits an attribute whose value is 0, and the loader
// keeps the member as constructed — i.e. the table default — when an attribute
// is absent. Those two rules only compose correctly when the default IS zero.
//
// `max_output_lines` was the one row anybody had noticed, and it was given
// `OPT_ZERO_UNLIMITED` to force the write. But four more rows accept a
// meaningful 0 against a non-zero default and had no such flag:
//
//     pixel_offset                  default 1      range 0-20
//     fade_output_opacity_percent   default 20     range 0-100
//     tool_tip_visible_time         default 5000   range 0-120000
//     tool_tip_start_time           default 400    range 0-120000
//
// Set any of them to 0, save, reload, and the value is silently back to its
// default. `docs/UPSTREAM-BUGS.md` 101.
//
// The saver now writes a 0 whenever the option's default is non-zero, which is
// exactly the condition under which omitting it loses information. A row whose
// default is 0 is written exactly as before.
TEST(WorldLoad, AZeroSurvivesOnEveryOptionWhoseDefaultIsNotZero)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="mud.example.com" port="4000">
</world>
</muclient>)";

    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_zero_%d.mcl", (int)getpid());

    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));

        // The defaults, so a later 0 cannot be mistaken for "it was already 0".
        ASSERT_EQ(1L,    (long)doc.m_iPixelOffset);
        ASSERT_EQ(20L,   (long)doc.m_FadeOutputOpacityPercent);
        ASSERT_EQ(5000L, (long)doc.m_iToolTipVisibleTime);
        ASSERT_EQ(400L,  (long)doc.m_iToolTipStartTime);

        EXPECT_EQ(eOK, doc.SetOption("pixel_offset", 0));
        EXPECT_EQ(eOK, doc.SetOption("fade_output_opacity_percent", 0));
        EXPECT_EQ(eOK, doc.SetOption("tool_tip_visible_time", 0));
        EXPECT_EQ(eOK, doc.SetOption("tool_tip_start_time", 0));

        ASSERT_TRUE(doc.OnSaveDocument(path));
    }

    // The zeros must be ON DISK.  Checking only the reload would pass on a
    // build where the loader happened to zero an absent attribute, which is the
    // behaviour the boolean and alpha arms have and the numeric one does not.
    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    std::string content; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) content.append(buf, n);
    fclose(f);
    EXPECT_NE(content.find("pixel_offset=\"0\""),                std::string::npos);
    EXPECT_NE(content.find("fade_output_opacity_percent=\"0\""), std::string::npos);
    EXPECT_NE(content.find("tool_tip_visible_time=\"0\""),       std::string::npos);
    EXPECT_NE(content.find("tool_tip_start_time=\"0\""),         std::string::npos);

    CMUSHclientDoc reloaded;
    ASSERT_TRUE(reloaded.LoadWorldFromFile(path));
    EXPECT_EQ(0L, (long)reloaded.m_iPixelOffset);
    EXPECT_EQ(0L, (long)reloaded.m_FadeOutputOpacityPercent);
    EXPECT_EQ(0L, (long)reloaded.m_iToolTipVisibleTime);
    EXPECT_EQ(0L, (long)reloaded.m_iToolTipStartTime);

    remove(path);
}
TEST(WorldLoad, MaxOutputLinesRangeAcceptsZeroButNotOneToOneNinetyNine)
{
    // The range is "200 lines at least, OR none at all", which a plain min/max
    // cannot express — hence OPT_ZERO_UNLIMITED.  Both halves matter: allowing
    // 0 without keeping the floor would let a script set a 5-line buffer.
    CMUSHclientDoc doc;

    EXPECT_EQ(eOK,               doc.SetOption("max_output_lines", 0));
    EXPECT_EQ(0L,                (long)doc.m_maxlines);
    EXPECT_EQ(eOptionOutOfRange, doc.SetOption("max_output_lines", 5));
    EXPECT_EQ(eOptionOutOfRange, doc.SetOption("max_output_lines", 199));
    EXPECT_EQ(eOK,               doc.SetOption("max_output_lines", 200));
    // ...and the ceiling is raised well past the old 500,000.
    EXPECT_EQ(eOK,               doc.SetOption("max_output_lines", 5000000));
    EXPECT_EQ(eOptionOutOfRange, doc.SetOption("max_output_lines", 200000000));
}

TEST(WorldLoad, PasswordSaveEncodesBase64)
{
    // A password set in the doc must be written base64-encoded with a
    // <name>_base64="y" flag (so it round-trips with Windows, and the
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

// NOTE: worlds script via an external script FILE (script_filename,
// scriptingoptions_alpha.inc:59) — the REAL loader has no world-level inline
// <script> support (Load_Scripts_XML runs only for plugins,
// xml_load_world.cpp:457-462).  enable_scripts="y" is required because the
// Windows absent-boolean rule zeroes it for a minimal <world> tag, and
// engine creation is gated on m_bEnableScripts (doc.cpp:475).
TEST(WorldLoad, ScriptFileCreatesEngine)
{
    // A world with a script file should end up with a live Lua engine
    // after LoadWorldFromFile.
    const char* lua = R"(
-- trivial Lua so the parser has something to chew on
function OnWorldOpen()
  return true
end
)";
    std::string luaPath = write_temp_named("engine.lua", lua);
    char xml[1024];
    snprintf(xml, sizeof(xml), R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000" script_language="lua" enable_scripts="y"
       script_filename="%s">
</world>
</muclient>)", luaPath.c_str());

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    ASSERT_NE(nullptr, doc.m_ScriptEngine);
    EXPECT_TRUE(doc.m_ScriptEngine->IsLua());
}

TEST(WorldLoad, ScriptFileDispidResolvable)
{
    // After loading, GetDispid should find a function defined in the script.
    const char* lua = R"(
function MyHandler()
  return 42
end
)";
    std::string luaPath = write_temp_named("dispid.lua", lua);
    char xml[1024];
    snprintf(xml, sizeof(xml), R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world script_language="lua" enable_scripts="y" script_filename="%s">
</world>
</muclient>)", luaPath.c_str());

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    ASSERT_NE(nullptr, doc.m_ScriptEngine);

    DISPID id = doc.m_ScriptEngine->GetLuaDispid("MyHandler");
    EXPECT_NE(DISPID_UNKNOWN, id);
}

TEST(WorldLoad, ScriptLanguageDefaultsToLua)
{
    // A world with a script file but no explicit script_language should
    // still initialise a Lua engine (the table default is "lua",
    // scriptingoptions_alpha.inc:60; defaults="y" keeps table defaults for
    // this minimal hand-written <world> tag, as on Windows).
    const char* lua = R"(
function Ping() return "pong" end
)";
    std::string luaPath = write_temp_named("deflang.lua", lua);
    char xml[1024];
    snprintf(xml, sizeof(xml), R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world script_filename="%s" defaults="y">
</world>
</muclient>)", luaPath.c_str());

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    ASSERT_NE(nullptr, doc.m_ScriptEngine);
    EXPECT_TRUE(doc.m_ScriptEngine->IsLua());
}

// Scripting ENABLED with no script file still gets an engine.
//
// This test used to assert the opposite, and the assertion was the whole of the
// "deliberate headless deviation" recorded in
// docs/STUB-ADOPTION-2026-08.md §7: the port's hand-written CreateScriptEngine
// twin bailed on an empty m_strScriptFilename BEFORE constructing the engine,
// so engine-presence meant script-file-presence. Windows has never worked that
// way — scripting/scripting.cpp:37 creates the engine and only then, at :48,
// returns early for the empty filename — and the deviation dated from when Lua
// was the only language the port could run.
//
// It matters because the engine is what makes immediate mode and world.Execute
// work: a world with scripting on and no script file is exactly the setup
// somebody uses to type Lua at the command line.
TEST(WorldLoad, ScriptingEnabledWithNoScriptFileStillGetsAnEngine)
{
    // No <world> tag, so the ctor's default m_bEnableScripts = TRUE survives
    // (the absent-boolean rule only applies to attributes of a tag that IS
    // present).
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="ping" name="t1" enabled="y"><send>pong</send></trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    ASSERT_TRUE(doc.m_bEnableScripts);            // the premise, stated
    ASSERT_NE(nullptr, doc.m_ScriptEngine);
    EXPECT_TRUE(doc.m_ScriptEngine->IsLua());
    EXPECT_TRUE(doc.m_strScript.IsEmpty());       // ...and it ran no script

    // The engine is USABLE, which is the whole point of having it — this is
    // the path CImmediateDlg takes when you type Lua at the immediate window
    // (scripting/scripting.cpp's OnGameImmediate).  Parse returns false on
    // success, MFC-style.
    EXPECT_FALSE(doc.m_ScriptEngine->Parse("world.SetVariable('v', 'typed')",
                                           "Immediate"));
    CVariable * pVar = NULL;
    EXPECT_TRUE(doc.GetVariableMap().Lookup("v", pVar));
    ASSERT_NE(nullptr, pVar);
    EXPECT_EQ(CString("typed"), pVar->strContents);
}

// The control, and the thing that genuinely means "no engine": scripting off.
// Without this the change above would be indistinguishable from "always make an
// engine", which is not what the canonical body does — doc.cpp:467 gates on
// m_bEnableScripts.
TEST(WorldLoad, ScriptingDisabledGetsNoEngine)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    // A <world> tag with no enable_scripts attribute zeroes it, by the Windows
    // absent-boolean rule.
    ASSERT_FALSE(doc.m_bEnableScripts);
    EXPECT_EQ(nullptr, doc.m_ScriptEngine);
}

TEST(WorldLoad, TriggerWithScriptProcedure)
{
    // A trigger with a script= attribute should have its dispid resolved
    // after the world is loaded (GetLuaDispid on the named function).
    const char* lua = R"(
function OnHealth(name, line, wildcards)
  -- would update HP display
end
)";
    std::string luaPath = write_temp_named("onhealth.lua", lua);
    char xml[1024];
    snprintf(xml, sizeof(xml), R"xml(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world script_language="lua" enable_scripts="y" script_filename="%s">
</world>
<triggers>
  <trigger match="^health: (\d+)" name="hp_trigger" regexp="y"
           enabled="y" script="OnHealth">
    <send></send>
  </trigger>
</triggers>
</muclient>)xml", luaPath.c_str());

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
    // Regexp trigger ^fooba matches "fooba" (cols 0-4) within the single-style
    // line "foobar" (cols 0-5, iStyleEndCol=6 exclusive) — exactly the boundary
    // case (iEndCol=5, iStyleEndCol-1=5) that the buggy condition mishandled.
    // (Uses regexp="y" with an unterminated ^fooba so the match ends BEFORE the
    // line end; a plain non-regexp trigger is whole-line-anchored and would
    // match all of "foobar", leaving no trailing char to split.)
    // (defaults="y" keeps enable_triggers at its table default for this
    // hand-written minimal <world> tag — Windows semantics zero an absent
    // boolean otherwise, since the saver omits false booleans.)
    const char* lua = R"(
function Noop() end
)";
    std::string luaPath = write_temp_named("noop.lua", lua);
    char xml[1024];
    snprintf(xml, sizeof(xml), R"xml(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world script_language="lua" defaults="y" script_filename="%s">
</world>
<triggers>
  <trigger match="^fooba" name="t1" regexp="y" enabled="y" custom_colour="1" send_to="14">
    <send></send>
  </trigger>
</triggers>
</muclient>)xml", luaPath.c_str());

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
<plugin name="TestPlugin" author="TestAuthor" id="aaaaaaaaaaaaaaaaaaaa0001"
        version="2.5" />
</muclient>)";

// Plugin XML that also defines a trigger.
static const char* PLUGIN_WITH_TRIGGER = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<plugin name="TrigPlugin" author="A" id="bbbbbbbbbbbbbbbbbbbb0001" version="1.0" />
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
<plugin name="VarPlugin" author="A" id="cccccccccccccccccccc0001" version="1.0" />
<variables>
  <variable name="plugin_var">plugin_value</variable>
</variables>
</muclient>)";

TEST(PluginLoad, BasicMetadataStored)
{
    std::string path = write_temp_named("basic.xml", PLUGIN_BASIC);
    ASSERT_TRUE(!path.empty());

    CMUSHclientDoc doc;
    EXPECT_TRUE(TryLoadPlugin(doc, path.c_str()));

    ASSERT_EQ(1u, doc.m_PluginList.size());
    CPlugin* p = doc.m_PluginList.front();
    EXPECT_STREQ("TestPlugin",        (const char*)p->m_strName);
    EXPECT_STREQ("TestAuthor",        (const char*)p->m_strAuthor);
    EXPECT_STREQ("aaaaaaaaaaaaaaaaaaaa0001",  (const char*)p->m_strID);
    EXPECT_NEAR(2.5, p->m_dVersion, 0.001);
}

TEST(PluginLoad, SourcePathRecorded)
{
    std::string path = write_temp_named("srcpath.xml", PLUGIN_BASIC);
    CMUSHclientDoc doc;
    ASSERT_TRUE(TryLoadPlugin(doc, path.c_str()));

    CPlugin* p = doc.m_PluginList.front();
    // m_strSource is the path the loader OPENED, not the string it was handed:
    // it comes from CFile::GetFilePath, which fully qualifies.  On the port
    // "/tmp/x.xml" already IS the full path so the two are equal; on Windows
    // the same file is "C:\tmp\x.xml".  Assert what is true of both -- the
    // file is named, and the recorded path is one that opens.
    CString src = p->m_strSource;
    EXPECT_TRUE(src.Find("srcpath.xml") != -1);
    FILE* reopened = fopen((const char*)src, "rb");
    EXPECT_TRUE(reopened != nullptr);
    if (reopened) fclose(reopened);
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
    ASSERT_TRUE(TryLoadPlugin(doc, path.c_str()));

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
    ASSERT_TRUE(TryLoadPlugin(doc, path.c_str()));

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
    EXPECT_FALSE(TryLoadPlugin(doc, "/nonexistent/plugin.xml"));
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
<plugin name="SortPlugin" author="A" id="dddddddddddddddddddd0001" version="1.0" />
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
    ASSERT_TRUE(TryLoadPlugin(doc, path.c_str()));

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
<plugin author="A" id="eeeeeeeeeeeeeeeeeeee0001" version="1.0" />
</muclient>)";

    std::string path = write_temp_named("noname.xml", xml);
    CMUSHclientDoc doc;
    EXPECT_FALSE(TryLoadPlugin(doc, path.c_str()));
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
    EXPECT_FALSE(TryLoadPlugin(doc, path.c_str()));
    EXPECT_EQ(0u, doc.m_PluginList.size());
}

TEST(PluginLoad, DuplicateIDRejected)
{
    std::string path = write_temp_named("dup.xml", PLUGIN_BASIC);
    CMUSHclientDoc doc;

    EXPECT_TRUE(TryLoadPlugin(doc, path.c_str()));
    EXPECT_EQ(1u, doc.m_PluginList.size());

    // Second load of same ID should be silently rejected
    EXPECT_FALSE(TryLoadPlugin(doc, path.c_str()));
    EXPECT_EQ(1u, doc.m_PluginList.size());   // still only one
}

TEST(PluginLoad, MultipleDistinctPlugins)
{
    std::string p1 = write_temp_named("multi1.xml", PLUGIN_WITH_TRIGGER);
    std::string p2 = write_temp_named("multi2.xml", PLUGIN_WITH_VARIABLE);
    ASSERT_TRUE(!p1.empty()); ASSERT_TRUE(!p2.empty());

    CMUSHclientDoc doc;
    EXPECT_TRUE(TryLoadPlugin(doc, p1.c_str()));
    EXPECT_TRUE(TryLoadPlugin(doc, p2.c_str()));
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
    EXPECT_FALSE(TryLoadPlugin(doc, path.c_str()));
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

TEST(WorldLoad, AbsentUtf8FollowsTheOptionTableDefault)
{
    // utf_8 is off by default — scriptingoptions_numeric.inc:172,
    //   {"utf_8",  false,  O(m_bUTF_8)}
    // — and a world file that does not mention it must come up in that state,
    // exactly as on Windows.  Two arms, because the loader treats them
    // differently (xml_load_world.cpp Load_World_Numeric_Options_XML, boolean
    // branch):
    //
    //   no defaults="y"  → the "take zero" arm forces the member to 0.
    //   defaults="y"     → Get_XML_boolean returns false and NOTHING is
    //                      written, so whatever the document carried INTO the
    //                      load survives it.
    //
    // The second arm is why this matters on the port.  wx_seed_new_world_defaults
    // (wx_mfc_impl.cpp) runs on the Open World File path as well as File ▸ New
    // World, and it used to set m_bUTF_8 = 1 before the load — against the table.
    // A hand-written defaults="y" world therefore opened in UTF-8 on macOS and
    // in ANSI on Windows, and the flip was saved back into the user's file.
    // Arm three below pins the "survives the load" semantics that made the seed
    // load-bearing, so a future re-seed of a table-contradicting value is not
    // silently invisible again.  (The seed itself is GUI-side, so the assertion
    // ON it lives in platform/gui_bridge_selftest.cpp, TestNewWorldDefaults.)

    // The ctor's SetDefaults(false) walks the same table: a brand-new document
    // — the "new world" starting point — is already ANSI.
    {
        CMUSHclientDoc fresh;
        EXPECT_EQ(0, (int)fresh.m_bUTF_8);
    }

    const char* plain = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";

    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(plain).c_str()));
        EXPECT_EQ(0, (int)doc.m_bUTF_8);
    }

    const char* withDefaults = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000" defaults="y">
</world>
</muclient>)";

    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(withDefaults).c_str()));
        EXPECT_EQ(0, (int)doc.m_bUTF_8);
    }

    // ...and the load really is a no-op for this attribute under defaults="y" —
    // a value planted before the load (what the port's pre-load seed did) comes
    // straight back out.  Without this arm the two assertions above would also
    // pass if the loader happened to zero the member for its own reasons, and
    // the seeding bug would look harmless.
    {
        CMUSHclientDoc doc;
        doc.m_bUTF_8 = 1;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(withDefaults).c_str()));
        // defaults="y" + absent utf_8 must leave the member untouched — which is
        // precisely why nothing may seed it against the option table pre-load.
        EXPECT_EQ(1, (int)doc.m_bUTF_8);
    }

    // The explicit attribute still wins over both, in both directions.
    {
        const char* explicitOn = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000" defaults="y" utf_8="y">
</world>
</muclient>)";
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(explicitOn).c_str()));
        EXPECT_EQ(1, (int)doc.m_bUTF_8);
    }
}

TEST(WorldLoad, FreshDocumentUtf8FollowsOptionsTableDefault)
{
    // doc_construct.cpp used to set m_bUTF_8 = false explicitly, right after
    // SetDefaults(false) had already set it from OptionsTable's "utf_8" row
    // two lines earlier -- a redundant second write.  The two agreed, so
    // nothing looked wrong, but it meant OptionsTable was NOT actually the
    // sole authority for a fresh document's m_bUTF_8: mutating the table's
    // default alone would not have moved a fresh document at all (the "last
    // writer wins hides a mutation" shape this project has been bitten by
    // before). The redundant write is gone now; this test proves the table
    // really is authoritative by mutating its default and checking that a
    // freshly constructed document follows it, in both directions, then
    // restores the default so other tests (e.g.
    // AbsentUtf8FollowsTheOptionTableDefault above) are unaffected.
    extern tConfigurationNumericOption OptionsTable[];

    int utf8Index = -1;
    for (int i = 0; OptionsTable[i].pName; i++)
        if (strcmp(OptionsTable[i].pName, "utf_8") == 0)
            { utf8Index = i; break; }
    // "utf_8" row not found in OptionsTable
    ASSERT_NE(-1, utf8Index);

    const double originalDefault = OptionsTable[utf8Index].iDefault;
    // test assumes utf_8 defaults to off
    ASSERT_EQ(0.0, originalDefault);

    // Sanity: with the table at its normal default, a fresh document is off.
    {
        CMUSHclientDoc doc;
        EXPECT_EQ(0, (int)doc.m_bUTF_8);
    }

    // Flip the table default and construct a NEW document. If the
    // constructor still hardcoded m_bUTF_8 anywhere (the bug this guards
    // against), this fresh document would come up 0 regardless of the table.
    // Everything from here to the restore below uses EXPECT, not ASSERT, so
    // the restore always runs even if an assertion fails.
    OptionsTable[utf8Index].iDefault = 1;

    {
        // If this fails: a fresh document did not follow OptionsTable's
        // "utf_8" default -- something besides the table is writing
        // m_bUTF_8 in the constructor.
        CMUSHclientDoc doc;
        EXPECT_EQ(1, (int)doc.m_bUTF_8);
    }

    OptionsTable[utf8Index].iDefault = originalDefault;

    // Mutate both directions: confirm the restore is also honoured, not just
    // the flip to 1.
    {
        CMUSHclientDoc doc;
        EXPECT_EQ(0, (int)doc.m_bUTF_8);
    }
}

TEST(WorldLoad, WorldSettingsVisibleViaOptionAPI)
{
    // Values loaded from XML must be visible through the real option API
    // (GetOption / GetAlphaOptionItem over the OptionsTable machinery —
    // the old hand-mirrored option caches are gone).
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

    // Numeric — the loader's SetOptionItem wrote the real member
    EXPECT_EQ(100L, doc.GetOption("wrap_column"));
    EXPECT_EQ(9999L, doc.GetOption("port"));

    // Alpha — same through the alpha table
    long idx = doc.GetAlphaOptionIndex("proxy_server");
    ASSERT_NE(-1L, idx);
    EXPECT_STREQ("myproxy", (const char*)doc.GetAlphaOptionItem(idx));
}

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────

#include <clocale>

// Read a whole file into a std::string.
static std::string slurp(const char* path)
{
    std::string content;
    FILE* f = fopen(path, "rb");
    if (!f) return content;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) content.append(buf, n);
    fclose(f);
    return content;
}

// xml_load_world.cpp: "even if they say 'don't take default', the absence
// of a colour gives the default behaviour of 'no change'" — and colour 17
// (MAX_CUSTOM + 1) is the "other" slot, stored zero-relative as 16.
TEST(WorldLoad, AbsentTriggerCustomColourMeansNoChange)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="a" name="nocolour" enabled="y"><send>x</send></trigger>
  <trigger match="b" name="zerocolour" enabled="y" custom_colour="0"><send>x</send></trigger>
  <trigger match="c" name="othercolour" enabled="y" custom_colour="17"><send>x</send></trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("nocolour", t));
    EXPECT_EQ((unsigned short)SAMECOLOUR, t->colour);   // absent → no change
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("zerocolour", t));
    EXPECT_EQ((unsigned short)SAMECOLOUR, t->colour);   // "0" → no change
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("othercolour", t));
    EXPECT_EQ(16, (int)t->colour);                      // 17 → "other" slot
}

// xml_load_world.cpp: "Timer for 'every' time cannot have zero interval" —
// such a timer draws a load warning and is not added to the map.
TEST(WorldLoad, EveryTimerZeroIntervalRejected)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<timers>
  <timer name="tz" enabled="y"><send>x</send></timer>
  <timer name="tok" enabled="y" second="5"><send>x</send></timer>
</timers>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CTimer* t = nullptr;
    EXPECT_FALSE(doc.GetTimerMap().Lookup("tz", t));    // zero interval: rejected
    EXPECT_TRUE(doc.GetTimerMap().Lookup("tok", t));    // control: added
}

// xml_load_world.cpp: "check if this exact trigger already exists (suggested
// by Fiendish 12 Dec 2011) — version: 4.81" — an identical unnamed trigger
// is deleted and not added again.
TEST(WorldLoad, UnnamedDuplicateTriggerNotAddedTwice)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="dupe me" enabled="y" sequence="100"><send>hi</send></trigger>
  <trigger match="dupe me" enabled="y" sequence="100"><send>hi</send></trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    EXPECT_EQ(1, (int)doc.GetTriggerArray().GetSize());   // only one copy
}

// xmlparse.cpp Get_XML_Entity: "we will accept tabs, newlines and
// carriage-returns ;) — don't allow nonprintable characters; don't allow
// characters more than 1 byte" — &#7; / &#256; fail the whole load.
TEST(WorldLoad, NumericEntityOutsideByteRangeRejected)
{
    const char* bad7 = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="a&#7;b" name="t1" enabled="y"><send>x</send></trigger>
</triggers>
</muclient>)";
    CMUSHclientDoc doc;
    EXPECT_FALSE(doc.LoadWorldFromFile(write_temp(bad7).c_str()));

    const char* bad256 = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="a&#256;b" name="t1" enabled="y"><send>x</send></trigger>
</triggers>
</muclient>)";
    CMUSHclientDoc doc2;
    EXPECT_FALSE(doc2.LoadWorldFromFile(write_temp(bad256).c_str()));

    // control: &#65; ('A') is accepted
    const char* ok = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="a&#65;b" name="t1" enabled="y"><send>x</send></trigger>
</triggers>
</muclient>)";
    CMUSHclientDoc doc3;
    ASSERT_TRUE(doc3.LoadWorldFromFile(write_temp(ok).c_str()));
    CTrigger* t = nullptr;
    ASSERT_TRUE(doc3.GetTriggerMap().Lookup("t1", t));
    EXPECT_STREQ("aAb", (const char*)t->trigger);
}

// xmlparse.cpp: "special treatment for ampersands, so they don't get
// converted into entities" — CDATA content is copied literally.
TEST(WorldLoad, CdataKeepsAmpersandTextLiteral)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="x" name="t1" enabled="y"><send><![CDATA[&lt; & stays]]></send></trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("t1", t));
    // "&lt;" is five^H^H four literal characters — NOT expanded to '<'
    EXPECT_STREQ("&lt; & stays", (const char*)t->contents);
}

// xml_save_world.cpp Save_XML_double: "don't want any blasted commas!" — a
// timer's fractional second is saved with '.' even under a comma-decimal
// locale (the setlocale approach DIDN'T WORK — forum 7953/8077).
TEST(WorldLoad, TimerSecondSavedWithDotUnderCommaLocale)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";

    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_locale_%d.mcl", (int)getpid());

    const char* prev = setlocale(LC_NUMERIC, NULL);
    std::string saved(prev ? prev : "C");
    if (!setlocale(LC_NUMERIC, "de_DE.UTF-8") && !setlocale(LC_NUMERIC, "de_DE")) {
        fprintf(stderr, "  (no comma-decimal locale available; skipping)\n");
        return;
    }
    ASSERT_TRUE(localeconv()->decimal_point[0] == ',');   // locale is live

    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
        CTimer* t = new CTimer();
        t->strLabel = CString("frac");
        t->bEnabled = TRUE;
        t->iType = CTimer::eInterval;
        t->fEverySecond = 7.5;
        doc.GetTimerMap().SetAt(t->strLabel, t);
        ASSERT_TRUE(doc.OnSaveDocument(path));
    }
    setlocale(LC_NUMERIC, saved.c_str());

    std::string content = slurp(path);
    EXPECT_NE(content.find("second=\"7.50\""), std::string::npos);  // "%.2f"
    EXPECT_EQ(content.find("7,5"), std::string::npos);    // no blasted commas

    // and it reloads correctly
    CMUSHclientDoc doc2;
    ASSERT_TRUE(doc2.LoadWorldFromFile(path));
    CTimer* t2 = nullptr;
    ASSERT_TRUE(doc2.GetTimerMap().Lookup("frac", t2));
    EXPECT_NEAR(7.5, t2->fEverySecond, 1e-9);
}

// xml_save_world.cpp: "don't write 65536 to world file (colour !=
// SAMECOLOUR guard); + 1, to match custom colours" — SAMECOLOUR omits the
// attribute; a real colour c is written as c+1 and round-trips.
TEST(WorldLoad, TriggerSameColourOmittedFromSavedFile)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_colour_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
        CTrigger* t1 = new CTrigger();
        t1->trigger = CString("same"); t1->strLabel = CString("tsame");
        t1->bEnabled = TRUE; t1->colour = SAMECOLOUR;
        doc.GetTriggerMap().SetAt(t1->strLabel, t1);
        CTrigger* t2 = new CTrigger();
        t2->trigger = CString("real"); t2->strLabel = CString("treal");
        t2->bEnabled = TRUE; t2->colour = 3;
        doc.GetTriggerMap().SetAt(t2->strLabel, t2);
        ASSERT_TRUE(doc.OnSaveDocument(path));
    }
    std::string content = slurp(path);
    EXPECT_EQ(content.find("custom_colour=\"65536\""), std::string::npos);
    EXPECT_NE(content.find("custom_colour=\"4\""), std::string::npos);  // 3+1
    // exactly one custom_colour attribute in the file (tsame omitted it)
    size_t first = content.find("custom_colour=");
    ASSERT_TRUE(first != std::string::npos);
    EXPECT_EQ(content.find("custom_colour=", first + 1), std::string::npos);

    // round-trip: colour 3 comes back as 3, SAMECOLOUR as SAMECOLOUR
    CMUSHclientDoc doc2;
    ASSERT_TRUE(doc2.LoadWorldFromFile(path));
    CTrigger* t = nullptr;
    ASSERT_TRUE(doc2.GetTriggerMap().Lookup("treal", t));
    EXPECT_EQ(3, (int)t->colour);
    ASSERT_TRUE(doc2.GetTriggerMap().Lookup("tsame", t));
    EXPECT_EQ((unsigned short)SAMECOLOUR, t->colour);
}

// xml_save_world.cpp: "don't save temporary triggers (bTemporary ||
// bIncluded skipped)" — a temporary trigger is absent from the saved file.
TEST(WorldLoad, TemporaryTriggerOmittedFromSave)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_temp_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
        CTrigger* t1 = new CTrigger();
        t1->trigger = CString("temporary_trigger_match");
        t1->strLabel = CString("ttemp");
        t1->bEnabled = TRUE; t1->bTemporary = true;
        doc.GetTriggerMap().SetAt(t1->strLabel, t1);
        CTrigger* t2 = new CTrigger();
        t2->trigger = CString("permanent_trigger_match");
        t2->strLabel = CString("tperm");
        t2->bEnabled = TRUE;
        doc.GetTriggerMap().SetAt(t2->strLabel, t2);
        ASSERT_TRUE(doc.OnSaveDocument(path));
    }
    std::string content = slurp(path);
    EXPECT_EQ(content.find("temporary_trigger_match"), std::string::npos);
    EXPECT_NE(content.find("permanent_trigger_match"), std::string::npos);
}

// xml_serialize.cpp: "m_CurrentPlugin = NULL; // make sure we save main
// triggers etc." — a save issued while a plugin is current still writes the
// WORLD's items and restores m_CurrentPlugin afterwards.
TEST(WorldLoad, SaveWithCurrentPluginStillSavesWorldTriggers)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_curpl_%d.mcl", (int)getpid());

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
    CTrigger* t = new CTrigger();
    t->trigger = CString("world_trigger_match");
    t->strLabel = CString("wt");
    t->bEnabled = TRUE;
    doc.GetTriggerMap().SetAt(t->strLabel, t);

    CPlugin* p = new CPlugin(&doc);
    p->m_bSaveState = false;
    p->m_strID = "eeeeeeeeeeeeeeeeeeeeeee5";
    doc.m_PluginList.push_back(p);
    doc.m_CurrentPlugin = p;                   // save happens "inside" a plugin

    ASSERT_TRUE(doc.OnSaveDocument(path));
    EXPECT_TRUE(doc.m_CurrentPlugin == p);     // restored afterwards
    doc.m_CurrentPlugin = NULL;

    std::string content = slurp(path);
    EXPECT_NE(content.find("world_trigger_match"), std::string::npos);
}

// xml_serialize.cpp: "ensure world has an ID (if m_strWorldID empty,
// GetUniqueID() before Save_World_XML)".
TEST(WorldLoad, SaveGeneratesWorldIdWhenMissing)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_id_%d.mcl", (int)getpid());

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
    doc.m_strWorldID.Empty();                  // wipe the ID
    ASSERT_TRUE(doc.OnSaveDocument(path));

    ASSERT_EQ(24, doc.m_strWorldID.GetLength());   // fresh 24-hex ID in doc
    std::string content = slurp(path);
    std::string expect = std::string("id=\"") +
                         (const char*)doc.m_strWorldID + "\"";
    EXPECT_NE(content.find(expect), std::string::npos);   // and in the file
}

// The <aliases>/<variables> SAVE loops (Save_Aliases_XML / Save_One_Alias_XML /
// Save_Variables_XML, and the SortAliases/SortVariables ordering pass they run)
// had ZERO coverage: no save test ever contained an alias or a variable.  Add
// three aliases and two variables, save, and pin that every one is written with
// its fields and round-trips on reload.  (Save order is by (iSequence, match),
// which depends on allocation for equal keys, so it is deliberately not asserted
// — presence + round-trip is the robust, non-flaky observable.)
TEST(WorldLoad, AliasesAndVariablesSaveAndRoundTrip)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_av_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
        ASSERT_EQ((long)eOK, doc.AddAlias("la", "aaa", "go a", 0x01 /*eEnabled*/, ""));
        ASSERT_EQ((long)eOK, doc.AddAlias("lz", "zzz", "go z", 0x01, ""));
        ASSERT_EQ((long)eOK, doc.AddAlias("lm", "mmm", "go m", 0x01, ""));
        ASSERT_EQ((long)eOK, doc.SetVariable("vzebra", "vz-val"));
        ASSERT_EQ((long)eOK, doc.SetVariable("valpha", "va-val"));
        ASSERT_TRUE(doc.OnSaveDocument(path));
    }

    std::string c = slurp(path);
    // all three aliases (with name label + match text + send) and both variables
    // (with value) were written.
    EXPECT_NE(c.find("name=\"la\""),      std::string::npos);
    EXPECT_NE(c.find("match=\"aaa\""),    std::string::npos);
    EXPECT_NE(c.find("match=\"mmm\""),    std::string::npos);
    EXPECT_NE(c.find("match=\"zzz\""),    std::string::npos);
    EXPECT_NE(c.find("go a"),             std::string::npos);   // alias contents present
    EXPECT_NE(c.find("name=\"valpha\">va-val</variable>"), std::string::npos);
    EXPECT_NE(c.find("name=\"vzebra\">vz-val</variable>"), std::string::npos);

    // everything reloads with its data intact
    CMUSHclientDoc doc2;
    ASSERT_TRUE(doc2.LoadWorldFromFile(path));
    CAlias* a = nullptr;
    ASSERT_TRUE(doc2.GetAliasMap().Lookup("lm", a));
    EXPECT_EQ(std::string("mmm"), std::string((const char*)a->name));
    EXPECT_EQ(std::string("go m"), std::string((const char*)a->contents));
    VARIANT vv = doc2.GetVariable("valpha");
    ASSERT_EQ((unsigned short)VT_BSTR, vv.vt);
    VariantClear(&vv);
    EXPECT_EQ(3, (int)doc2.GetAliasMap().GetCount());
}

// ── Variables are written in sorted order ──────────────────────────────────
//
// AliasesAndVariablesSaveAndRoundTrip (below) covers that the sections are
// written and reload intact.  What it cannot see is the ORDER: SortVariables /
// CompareVariable (xml_serialize.cpp) qsort the map into a stable alphabetical
// sequence before the write loop, and a comparator that returned 0 for
// everything would still produce a perfectly loadable file.  The only place
// that shows is the byte order in the saved XML.
//
// Insertion order here is deliberately reverse-alphabetical, so "unsorted"
// and "sorted" cannot coincide.
TEST(WorldLoad, VariablesAreSavedInSortedOrder)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="mud.example.com" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_vsort_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
        ASSERT_EQ((long)eOK, doc.SetVariable("zeta",  "z"));
        ASSERT_EQ((long)eOK, doc.SetVariable("mid",   "m"));
        ASSERT_EQ((long)eOK, doc.SetVariable("alpha", "a"));
        ASSERT_TRUE(doc.OnSaveDocument(path));
    }

    const std::string c = slurp(path);
    const size_t vA = c.find("name=\"alpha\"");
    const size_t vM = c.find("name=\"mid\"");
    const size_t vZ = c.find("name=\"zeta\"");
    ASSERT_NE(vA, std::string::npos);
    ASSERT_NE(vM, std::string::npos);
    ASSERT_NE(vZ, std::string::npos);
    EXPECT_TRUE(vA < vM);
    EXPECT_TRUE(vM < vZ);
    remove(path);
}

// scriptingoptions.cpp OPT_COMMAND_STACK validation: only the empty-value arm was
// covered.  A value longer than one char, and a non-printable/whitespace char,
// are also rejected with eOptionOutOfRange — and the latter forces command
// stacking off (the length arm returns before touching the flag).
TEST(WorldLoad, CommandStackCharacterValidation)
{
    CMUSHclientDoc doc;

    // length > 1 → out of range, WITHOUT disabling command stacking
    doc.m_enable_command_stack = TRUE;
    EXPECT_EQ((long)eOptionOutOfRange,
              doc.SetAlphaOption("command_stack_character", "ab"));
    EXPECT_TRUE(doc.m_enable_command_stack);

    // a single whitespace/non-printable char → out of range AND stacking forced off
    doc.m_enable_command_stack = TRUE;
    EXPECT_EQ((long)eOptionOutOfRange,
              doc.SetAlphaOption("command_stack_character", "\t"));
    EXPECT_FALSE(doc.m_enable_command_stack);

    // a valid single printable char is accepted
    EXPECT_EQ((long)eOK, doc.SetAlphaOption("command_stack_character", ";"));
}

// xmlparse.cpp Get_XML_boolean: "be nice and let them slip in Y or y ...
// '1'/'-1'/yes/y/true/t are true — anything else throws 'unrecognised
// value'" — the failing element draws a warning and is dropped.
TEST(WorldLoad, BooleanAttributeLexicon)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="a" name="tminus" enabled="-1"><send>x</send></trigger>
  <trigger match="b" name="tfalse" enabled="F"><send>x</send></trigger>
  <trigger match="c" name="tbad" enabled="maybe"><send>x</send></trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("tminus", t));
    EXPECT_TRUE(t->bEnabled != 0);                        // "-1" is true
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("tfalse", t));
    EXPECT_TRUE(t->bEnabled == 0);                        // "F" is false
    EXPECT_FALSE(doc.GetTriggerMap().Lookup("tbad", t));  // "maybe": rejected
}

// xmlparse.cpp ProcessEntity: "entities might have entities in them, so
// replace them" — a DTD entity referencing an earlier entity expands
// transitively.
TEST(WorldLoad, CustomEntityExpandsNestedEntity)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient [
  <!ENTITY a "x">
  <!ENTITY b "&a;y">
]>
<muclient>
<triggers>
  <trigger match="&b;z" name="t1" enabled="y"><send>go</send></trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("t1", t));
    EXPECT_STREQ("xyz", (const char*)t->trigger);   // &b; → "&a;y" → "xy"
}

// xmlparse.cpp: "convert tabs to spaces, we don't want tabs in our data
// (whole XML buffer, before parsing — a literal tab can only be embedded
// via &#9;)".
TEST(WorldLoad, LiteralTabsBecomeSpacesOnLoad)
{
    const char* xml = "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE muclient>\n"
        "<muclient>\n"
        "<triggers>\n"
        "  <trigger match=\"a&#9;b\" name=\"t1\" enabled=\"y\">\n"
        "    <send>x\ty</send>\n"                    // literal TAB
        "  </trigger>\n"
        "</triggers>\n"
        "</muclient>\n";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("t1", t));
    EXPECT_STREQ("a\tb", (const char*)t->trigger);    // &#9; → real tab
    EXPECT_STREQ("x y",  (const char*)t->contents);   // literal tab → space
}

// xml_load_world.cpp: "note - in 3.35 onwards these are incorporated into
// iSendTo" — legacy speed_walk="y" maps to eSendToSpeedwalk; combining it
// with a conflicting explicit send_to throws and drops the alias.
TEST(WorldLoad, LegacySpeedWalkFlagMapsToSendTo)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<aliases>
  <alias match="sw" name="aok" enabled="y" speed_walk="y"><send>3n</send></alias>
  <alias match="bad" name="abad" enabled="y" speed_walk="y" send_to="2"><send>x</send></alias>
</aliases>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CAlias* a = nullptr;
    ASSERT_TRUE(doc.GetAliasMap().Lookup("aok", a));
    EXPECT_EQ((int)eSendToSpeedwalk, (int)a->iSendTo);
    EXPECT_FALSE(doc.GetAliasMap().Lookup("abad", a));    // conflict: rejected
}

// xml_load_world.cpp: "conversion from older versions - make variable name
// same as label if necessary" — send-to-variable with no variable copies
// the label; with neither it is rejected.
TEST(WorldLoad, SendToVariableFallsBackToLabel)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="a" name="lbl" enabled="y" send_to="9"><send>x</send></trigger>
  <trigger match="b" enabled="y" send_to="9"><send>x</send></trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("lbl", t));
    EXPECT_STREQ("lbl", (const char*)t->strVariable);   // label copied in
    EXPECT_EQ(1, (int)doc.GetTriggerArray().GetSize()); // unnamed one rejected
}

// xml_load_world.cpp: "don't include it twice (m_strCurrentIncludeFileList
// membership → 'has already been included')" — the second include of the
// same file warns; the world still loads and the item exists exactly once.
TEST(WorldLoad, DuplicateIncludeRejectedOnce)
{
    const char* incXml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="from include" name="tinc" enabled="y"><send>x</send></trigger>
</triggers>
</muclient>)";
    std::string incPath = write_temp_named("dup_include.xml", incXml);
    ASSERT_TRUE(!incPath.empty());

    char main_xml[1024];
    snprintf(main_xml, sizeof main_xml, R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<include name="%s" plugin="n" />
<include name="%s" plugin="n" />
<world site="test.mud" port="4000">
</world>
</muclient>)", incPath.c_str(), incPath.c_str());

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(main_xml).c_str()));
    CTrigger* t = nullptr;
    EXPECT_TRUE(doc.GetTriggerMap().Lookup("tinc", t)); // included once
    EXPECT_EQ(1, (int)doc.GetTriggerArray().GetSize()); // not twice
}

// doc_portable.cpp Load_Set: "TRUE = cancelled, matching the GUI cancel
// path ... FALSE = OK (matches Windows convention)" — the inverted return.
TEST(WorldLoad, LoadSetEmptyNameReturnsCancelledTrue)
{
    CMUSHclientDoc doc;
    CTrigger* t = new CTrigger();
    t->trigger = CString("keepme"); t->strLabel = CString("keepme");
    t->bEnabled = TRUE;
    doc.GetTriggerMap().SetAt(t->strLabel, t);

    EXPECT_TRUE(doc.Load_Set(TRIGGER, CString(""), NULL));   // cancelled
    CTrigger* still = nullptr;
    EXPECT_TRUE(doc.GetTriggerMap().Lookup("keepme", still)); // untouched
}

// doc_portable.cpp Load_Set: "replace=TRUE: with no prompt possible we take
// the dialogs' default answer — case TRIGGER: if (replace)
// DELETE_MAP(m_TriggerMap, CTrigger)" — the set REPLACES existing triggers.
TEST(WorldLoad, LoadSetTriggersReplaceExistingMap)
{
    const char* setXml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="from set" name="tset" enabled="y"><send>x</send></trigger>
</triggers>
</muclient>)";
    std::string setPath = write_temp_named("trigger_set.xml", setXml);
    ASSERT_TRUE(!setPath.empty());

    CMUSHclientDoc doc;
    CTrigger* old = new CTrigger();
    old->trigger = CString("old"); old->strLabel = CString("oldtrig");
    old->bEnabled = TRUE;
    doc.GetTriggerMap().SetAt(old->strLabel, old);

    EXPECT_FALSE(doc.Load_Set(TRIGGER, CString(setPath.c_str()), NULL)); // OK
    CTrigger* t = nullptr;
    EXPECT_FALSE(doc.GetTriggerMap().Lookup("oldtrig", t));  // replaced...
    EXPECT_TRUE(doc.GetTriggerMap().Lookup("tset", t));      // ...by the set
}

// ── XML parser error paths (xml/xmlparse.cpp ThrowErrorException arms) ────
// Each malformation targets a specific parser rejection; the loader turns
// the thrown CException into a false return.

static bool loads(const char* xml)
{
    CMUSHclientDoc doc;
    return doc.LoadWorldFromFile(write_temp(xml).c_str());
}

// Collapsed (audit §3): the fine-grained XmlParseErrors arm-by-arm tests were
// strictly weaker duplicates of test_xmlparse's XmlParse.StructuralErrorsDiagnosed /
// AttributeErrorsDiagnosed / EntityErrorsDiagnosed / NonPrintableCharactersRejected,
// which drive CXMLparser::BuildStructure directly and assert the EXACT thrown message
// and the resulting tree. Here we keep just a couple of representative INTEGRATION
// cases proving the loader converts a thrown parser CException into a `false` return
// (structural, attribute, and entity malformations) while a well-formed control loads.
TEST(XmlParseErrors, MalformedXmlConvertedToFalseLoad)
{
    // structural: elements terminated out of sequence
    EXPECT_FALSE(loads("<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n"
                       "<muclient>\n<triggers>\n</muclient>\n</triggers>"));
    // attribute: unquoted attribute value
    EXPECT_FALSE(loads("<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n"
                       "<muclient>\n<world use_mxp=1 />\n</muclient>"));
    // entity: unknown entity in content
    EXPECT_FALSE(loads("<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n"
                       "<muclient>\n<variables>\n"
                       "<variable name=\"v\">&nosuchentity;</variable>\n"
                       "</variables>\n</muclient>"));
    // control: a well-formed document loads (underscore is a legal first char)
    EXPECT_TRUE(loads("<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n"
                      "<muclient>\n<_odd />\n</muclient>"));
}

// Kept (NOT in the §3 cluster): the MAX_XML_NAME_LENGTH (255) limit is not exercised
// by any test_xmlparse case, so this remains the sole owner of that boundary.
TEST(XmlParseErrors, ElementNameTooLong)
{
    std::string name(256, 'x');           // MAX_XML_NAME_LENGTH is 255
    std::string xml = "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n<"
                      + name + " />\n</muclient>";
    EXPECT_FALSE(loads(xml.c_str()));
}

// XmlParseEdge.* deleted (audit §3): the positive edge paths (standard+numeric entity
// expansion, CDATA markup preservation, UTF-8 BOM skip, tab→space) are fully covered
// by test_xmlparse's EntityReplacementInContent / NumericEntityWhitespaceAndHighBytes /
// CdataPreservesMarkupCharacters / Utf8BomIsSkipped / TabsBecomeSpaces, which assert the
// exact parsed content. WorldLoad.LiteralTabsBecomeSpacesOnLoad (above) is kept because
// it additionally verifies &#9; → a real tab, a distinct behaviour.

// ═══ duplicate suppression (CTrigger/CAlias/CTimer operator==) ═════════════
// xml_load_world.cpp (Fiendish, 4.81): an UNLABELLED item identical in every
// field to one already loaded is silently dropped.

TEST(WorldLoad, DuplicateUnnamedTriggerDropped)
{
    const char* xml =
        "<?xml version=\"1.0\"?><!DOCTYPE muclient><muclient>\n"
        "<triggers>\n"
        "  <trigger enabled=\"y\" match=\"twin\" send_to=\"0\" sequence=\"100\">"
        "<send>hi</send></trigger>\n"
        "  <trigger enabled=\"y\" match=\"twin\" send_to=\"0\" sequence=\"100\">"
        "<send>hi</send></trigger>\n"
        "  <trigger enabled=\"y\" match=\"other\" send_to=\"0\" sequence=\"100\">"
        "<send>hi</send></trigger>\n"
        "</triggers>\n"
        "</muclient>\n";
    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    EXPECT_EQ(2, (int)doc.GetTriggerMap().GetCount());   // twin loaded once
}

TEST(WorldLoad, DuplicateUnnamedAliasDropped)
{
    const char* xml =
        "<?xml version=\"1.0\"?><!DOCTYPE muclient><muclient>\n"
        "<aliases>\n"
        "  <alias enabled=\"y\" match=\"^dupe$\" send_to=\"0\" sequence=\"100\">"
        "<send>x</send></alias>\n"
        "  <alias enabled=\"y\" match=\"^dupe$\" send_to=\"0\" sequence=\"100\">"
        "<send>x</send></alias>\n"
        "</aliases>\n"
        "</muclient>\n";
    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    EXPECT_EQ(1, (int)doc.GetAliasMap().GetCount());
}

TEST(WorldLoad, DuplicateUnnamedTimerDropped)
{
    const char* xml =
        "<?xml version=\"1.0\"?><!DOCTYPE muclient><muclient>\n"
        "<timers>\n"
        "  <timer enabled=\"y\" second=\"30\" send_to=\"0\">"
        "<send>tick</send></timer>\n"
        "  <timer enabled=\"y\" second=\"30\" send_to=\"0\">"
        "<send>tick</send></timer>\n"
        "</timers>\n"
        "</muclient>\n";
    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    EXPECT_EQ(1, (int)doc.GetTimerMap().GetCount());
}

// ═══ macros ════════════════════════════════════════════════════════════════

extern CString strMacroDescriptions[MACRO_COUNT];

static int macro_index(const char* name)
{
    for (int i = 0; i < MACRO_COUNT; i++)
        if (strMacroDescriptions[i] == name) return i;
    return -1;
}

TEST(WorldLoad, MacrosLoadWithEachSendType)
{
    const char* xml =
        "<?xml version=\"1.0\"?><!DOCTYPE muclient><muclient>\n"
        "<macros>\n"
        "  <macro name=\"F2\" type=\"send_now\"><send>north</send></macro>\n"
        "  <macro name=\"F3\" type=\"replace\"><send>south</send></macro>\n"
        "  <macro name=\"F4\" type=\"insert\"><send>east</send></macro>\n"
        "</macros>\n"
        "</muclient>\n";
    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    int f2 = macro_index("F2"), f3 = macro_index("F3"), f4 = macro_index("F4");
    ASSERT_TRUE(f2 >= 0 && f3 >= 0 && f4 >= 0);
    EXPECT_STREQ("north", (const char*)doc.m_macros[f2]);
    EXPECT_EQ((int)SEND_NOW, (int)doc.m_macro_type[f2]);
    EXPECT_STREQ("south", (const char*)doc.m_macros[f3]);
    EXPECT_EQ((int)REPLACE_COMMAND, (int)doc.m_macro_type[f3]);
    EXPECT_STREQ("east", (const char*)doc.m_macros[f4]);
    EXPECT_EQ((int)ADD_TO_COMMAND, (int)doc.m_macro_type[f4]);
}

TEST(WorldLoad, MacroErrorsAreTolerated)
{
    // Unknown macro name / missing type / bad type each throw inside the
    // loader, are caught per-item ("macro not loaded"), and the world still
    // loads with the valid macro applied.
    const char* xml =
        "<?xml version=\"1.0\"?><!DOCTYPE muclient><muclient>\n"
        "<macros>\n"
        "  <macro name=\"NotAMacro\" type=\"send_now\"><send>x</send></macro>\n"
        "  <macro name=\"F5\"><send>x</send></macro>\n"
        "  <macro name=\"F7\" type=\"warp\"><send>x</send></macro>\n"
        "  <macro name=\"F8\" type=\"send_now\"><send>ok</send></macro>\n"
        "</macros>\n"
        "</muclient>\n";
    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    int f8 = macro_index("F8");
    ASSERT_TRUE(f8 >= 0);
    EXPECT_STREQ("ok", (const char*)doc.m_macros[f8]);
    int f5 = macro_index("F5");
    EXPECT_TRUE(doc.m_macros[f5].IsEmpty());   // failed item left untouched
}

// ═══ printing styles (<printing><ansi>) ════════════════════════════════════

// Values pinned from genprint.h (not included here: it drags the print block).
#ifndef FONT_ITALIC
#  define FONT_ITALIC    0x01
#  define FONT_UNDERLINE 0x02
#  define FONT_BOLD      0x04
#endif

TEST(WorldLoad, PrintingAnsiStylesLoad)
{
    const char* xml =
        "<?xml version=\"1.0\"?><!DOCTYPE muclient><muclient>\n"
        "<printing>\n"
        "  <ansi>\n"
        "    <normal>\n"
        "      <style seq=\"1\" bold=\"y\" italic=\"n\" underline=\"y\" />\n"
        "    </normal>\n"
        "    <bold>\n"
        "      <style seq=\"2\" italic=\"y\" />\n"
        "    </bold>\n"
        "  </ansi>\n"
        "</printing>\n"
        "</muclient>\n";
    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    EXPECT_EQ(FONT_BOLD | FONT_UNDERLINE, doc.m_nNormalPrintStyle[0]);
    EXPECT_EQ(FONT_ITALIC, doc.m_nBoldPrintStyle[1]);
}

// ═══ Round-4 coverage: loader error/edge arms ═══════════════════════════════

// World "id" validation: wrong length and non-hex characters both throw.
TEST(WorldLoadR4, WorldIdValidationArms)
{
    // per-option errors are warn-and-continue (HandleLoadException): the
    // load succeeds but the bad id is NOT adopted
    {
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world name="W" id="tooshort" />
</muclient>)";
        std::string path = write_temp(xml);
        CMUSHclientDoc doc;
        EXPECT_TRUE(doc.LoadWorldFromFile(path.c_str()));
        EXPECT_TRUE(doc.m_strWorldID != "tooshort");
    }
    {
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world name="W" id="zzzzzzzzzzzzzzzzzzzzzzzz" />
</muclient>)";
        std::string path = write_temp(xml);
        CMUSHclientDoc doc;
        EXPECT_TRUE(doc.LoadWorldFromFile(path.c_str()));
        EXPECT_TRUE(doc.m_strWorldID != "zzzzzzzzzzzzzzzzzzzzzzzz");
    }
}

// Loading a world whose ID collides with an already-open world generates a
// fresh unique ID (and marks the document modified).
TEST(WorldLoadR4, DuplicateWorldIdRegenerated)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world name="W" id="0123456789abcdef01234567" />
</muclient>)";

    CMUSHclientDoc* other = new CMUSHclientDoc();
    other->m_strWorldID = "0123456789abcdef01234567";

    std::string path = write_temp(xml);
    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(path.c_str()));
    EXPECT_TRUE(doc.m_strWorldID != other->m_strWorldID);
    EXPECT_EQ(24, doc.m_strWorldID.GetLength());
    delete other;
}

// Trigger validation arms: invalid variable name, invalid label, duplicate
// labels (rejected without XML_OVERWRITE), and a bad regexp.  Each bad
// trigger is reported and skipped (HandleLoadException) — the load succeeds
// but the offender is NOT added.
TEST(WorldLoadR4, TriggerValidationArms)
{
    {   // invalid send-to-variable name
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><triggers>
  <trigger match="x" variable="9bad" send_to="9" sequence="100"><send>y</send></trigger>
</triggers></muclient>)";
        std::string path = write_temp(xml);
        CMUSHclientDoc doc;
        EXPECT_TRUE(doc.LoadWorldFromFile(path.c_str()));
        EXPECT_EQ(0, (int)doc.GetTriggerMap().GetCount());
    }
    {   // invalid label
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><triggers>
  <trigger match="x" name="9label" sequence="100"><send>y</send></trigger>
</triggers></muclient>)";
        std::string path = write_temp(xml);
        CMUSHclientDoc doc;
        EXPECT_TRUE(doc.LoadWorldFromFile(path.c_str()));
        EXPECT_EQ(0, (int)doc.GetTriggerMap().GetCount());
    }
    {   // duplicate label: the second one is refused
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><triggers>
  <trigger match="x" name="dup" sequence="100"><send>y</send></trigger>
  <trigger match="z" name="dup" sequence="100"><send>y</send></trigger>
</triggers></muclient>)";
        std::string path = write_temp(xml);
        CMUSHclientDoc doc;
        EXPECT_TRUE(doc.LoadWorldFromFile(path.c_str()));
        EXPECT_EQ(1, (int)doc.GetTriggerMap().GetCount());
        CTrigger* t = nullptr;
        ASSERT_TRUE(doc.GetTriggerMap().Lookup("dup", t));
        // world loads carry XML_OVERWRITE: the SECOND definition replaces
        // the first (the delete-old arm)
        EXPECT_STREQ("z", (LPCTSTR)t->trigger);
    }
    {   // the same duplicate via Load_Set (no XML_OVERWRITE): the second
        // definition is refused instead
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><triggers>
  <trigger match="x" name="dup" sequence="100"><send>y</send></trigger>
  <trigger match="z" name="dup" sequence="100"><send>y</send></trigger>
</triggers></muclient>)";
        std::string path = write_temp_named("dupset.xml", xml);
        CMUSHclientDoc doc;
        doc.Load_Set(TRIGGER, CString(path.c_str()), nullptr);
        EXPECT_EQ(1, (int)doc.GetTriggerMap().GetCount());
        CTrigger* t = nullptr;
        ASSERT_TRUE(doc.GetTriggerMap().Lookup("dup", t));
        EXPECT_STREQ("x", (LPCTSTR)t->trigger);   // the FIRST one won
    }
    {   // regexp that does not compile
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><triggers>
  <trigger match="(unclosed" regexp="y" sequence="100"><send>y</send></trigger>
</triggers></muclient>)";
        std::string path = write_temp(xml);
        CMUSHclientDoc doc;
        EXPECT_TRUE(doc.LoadWorldFromFile(path.c_str()));
        EXPECT_EQ(0, (int)doc.GetTriggerMap().GetCount());
    }
}

// Alias 'set_variable' interplay with send_to: incompatible destination
// throws; compatible one coerces to send-to-variable.
TEST(WorldLoadR4, AliasSetVariableArms)
{
    {
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><aliases>
  <alias match="a" set_variable="y" variable="v" send_to="2" sequence="100"><send>y</send></alias>
</aliases></muclient>)";
        std::string path = write_temp(xml);
        CMUSHclientDoc doc;
        EXPECT_TRUE(doc.LoadWorldFromFile(path.c_str()));   // warn-and-skip
        EXPECT_EQ(0, (int)doc.GetAliasMap().GetCount());
    }
    {
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><aliases>
  <alias match="a" name="al" set_variable="y" variable="v" send_to="0" sequence="100"><send>y</send></alias>
</aliases></muclient>)";
        std::string path = write_temp(xml);
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(path.c_str()));
        CAlias* a = nullptr;
        ASSERT_TRUE(doc.GetAliasMap().Lookup("al", a));
        EXPECT_EQ((int)eSendToVariable, (int)a->iSendTo);
    }
}

// Include files: no name → error; clipboard pseudo-name → error; a real
// include is loaded relative to the world-file directory.
TEST(WorldLoadR4, IncludeFileArms)
{
    {
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><include /></muclient>)";
        std::string path = write_temp(xml);
        CMUSHclientDoc doc;
        EXPECT_TRUE(doc.LoadWorldFromFile(path.c_str()));   // warn-and-skip
        EXPECT_EQ(0, (int)doc.GetVariableMap().GetCount());
    }
    {
        // a real include: the included file defines a variable
        std::string inc = write_temp_named("included.xml",
            R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><variables>
  <variable name="from_include">yes</variable>
</variables></muclient>)");

        char xml[512];
        snprintf(xml, sizeof xml,
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE muclient>\n"
            "<muclient><include name=\"%s\" /></muclient>", inc.c_str());
        std::string path = write_temp(xml);
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(path.c_str()));
        CVariable* v = nullptr;
        ASSERT_TRUE(doc.GetVariableMap().Lookup("from_include", v));
        EXPECT_STREQ("yes", (LPCTSTR)v->strContents);
    }
}

#ifdef MUSHCLIENT_LUA_SCRIPTING
// A PLUGIN whose script fails to parse aborts the plugin load (and the
// half-built plugin is thrown away).
TEST(WorldLoadR4, PluginScriptParseErrorAborts)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<plugin name="Broken" author="x" id="0123456789abcdef0123456e" language="Lua" version="1.0" />
<script><![CDATA[
this is not valid lua !!!
]]></script>
</muclient>)";
    std::string path = write_temp_named("brokenscript.xml", xml);
    CMUSHclientDoc doc;
    EXPECT_TRUE(!TryLoadPlugin(doc, path.c_str()));
    EXPECT_TRUE(doc.m_PluginList.empty());
}
#endif

// ═══ Round-4: Load_Set family arms + the '!' handler prefix ═════════════════

TEST(WorldLoadR4, LoadSetPerTypeArms)
{
    // aliases
    {
        std::string p = write_temp_named("set_alias.xml",
            R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><aliases>
  <alias match="sa" name="sa" send_to="0" sequence="100"><send>x</send></alias>
</aliases></muclient>)");
        CMUSHclientDoc doc;
        doc.Load_Set(ALIAS, CString(p.c_str()), nullptr);
        CAlias* a = nullptr;
        EXPECT_TRUE(doc.GetAliasMap().Lookup("sa", a));
    }
    // timers
    {
        std::string p = write_temp_named("set_timer.xml",
            R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><timers>
  <timer name="st" minute="5" enabled="y" send_to="0"><send>x</send></timer>
</timers></muclient>)");
        CMUSHclientDoc doc;
        doc.Load_Set(TIMER, CString(p.c_str()), nullptr);
        CTimer* t = nullptr;
        EXPECT_TRUE(doc.GetTimerMap().Lookup("st", t));
    }
    // macros
    {
        std::string p = write_temp_named("set_macro.xml",
            R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><macros>
  <macro name="F2" send="macro-two" type="0" />
</macros></muclient>)");
        CMUSHclientDoc doc;
        doc.Load_Set(MACRO, CString(p.c_str()), nullptr);
    }
    // colours
    {
        std::string p = write_temp_named("set_colour.xml",
            R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><colours>
  <ansi>
    <colour name="NormalColour1" colour="123456" />
  </ansi>
</colours></muclient>)");
        CMUSHclientDoc doc;
        doc.Load_Set(COLOUR, CString(p.c_str()), nullptr);
    }
    // not-XML input takes the bad-signature arm (throws internally, caught)
    {
        std::string p = write_temp_named("set_bad.txt", "definitely not xml at all here");
        CMUSHclientDoc doc;
        doc.Load_Set(TRIGGER, CString(p.c_str()), nullptr);
        EXPECT_EQ(0, (int)doc.GetTriggerMap().GetCount());
    }
    // empty name = the GUI cancel path
    {
        CMUSHclientDoc doc;
        EXPECT_TRUE(doc.Load_Set(TRIGGER, CString(""), nullptr));
    }
}

TEST(WorldLoadR4, BangHandlerExecutesInsteadOfScript)
{
    CMUSHclientDoc doc;
    // '!' prefix: the rest is Executed as a command and the script is vetoed
    EXPECT_TRUE(!doc.SeeIfHandlerCanExecute(CString("!look")));
    // plain names allow the normal dispid dispatch
    EXPECT_TRUE(doc.SeeIfHandlerCanExecute(CString("OnConnect")));
    EXPECT_TRUE(doc.SeeIfHandlerCanExecute(CString("")));
}

// A UTF-16LE world file (FF FE BOM) must be RECOGNISED as XML by the
// IsArchiveXML sniff (xml/xml_serialize.cpp — its FF FE arm used to cast the
// byte stream to LPCWSTR, which mis-reads on 4-byte-wchar_t platforms, so a
// Unicode world file was rejected as "not XML") and then load end-to-end
// through BuildStructure's round-4 UTF-16 conversion arm (xml/xmlparse.cpp).
TEST(WorldLoadR5, Utf16FileRecognisedAsXmlAndLoads)
{
    const char* xml8 =
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE muclient>\n"
        "<muclient>\n"
        "<variables>\n"
        "  <variable name=\"greeting\">hello unicode</variable>\n"
        "</variables>\n"
        "</muclient>\n";

    // encode as UTF-16LE with BOM (ASCII content: one unit per byte)
    std::string doc16 = "\xFF\xFE";
    for (const char* c = xml8; *c; ++c) { doc16 += *c; doc16 += '\0'; }

    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_u16_%d.mcl", (int)getpid());
    FILE* f = fopen(path, "wb");
    ASSERT_TRUE(f != nullptr);
    fwrite(doc16.data(), 1, doc16.size(), f);
    fclose(f);

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(path));

    CVariable* v = nullptr;
    ASSERT_TRUE(doc.GetVariableMap().Lookup("greeting", v));
    EXPECT_STREQ("hello unicode", (const char*)v->strContents);
    unlink(path);
}

// ── round 5: loader validation and tolerated-error arms ───────────────────

// A world load swallows per-item errors (HandleLoadException) and still
// succeeds; the bad item is simply not loaded.
TEST(WorldLoadR5, BadItemsAreToleratedAndSkipped)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<variables>
  <variable name="9bad">nope</variable>
  <variable name="good">yes</variable>
</variables>
<colours>
  <ansi>
    <normal>
      <colour seq="99" rgb="#000000"/>
      <colour seq="1" rgb="#010203"/>
    </normal>
  </ansi>
</colours>
<keypad>
  <key name="NoSuchKey">north</key>
  <key name="Numpad0">look</key>
</keypad>
<printing>
  <ansi>
    <normal>
      <style seq="99" bold="y"/>
      <style seq="1" bold="y"/>
    </normal>
  </ansi>
</printing>
</muclient>)";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CVariable* v = nullptr;
    EXPECT_FALSE(doc.GetVariableMap().Lookup("9bad", v));   // invalid name arm
    EXPECT_TRUE(doc.GetVariableMap().Lookup("good", v));
    EXPECT_EQ((COLORREF)RGB(1, 2, 3), doc.m_normalcolour[0]);
}

// Reloading a variable with different contents: the overwrite warning arm
// (no XML_OVERWRITE on a plain load) fires, and the old variable is replaced.
TEST(WorldLoadR5, VariableOverwriteWarnsAndReplaces)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<variables>
  <variable name="v">first</variable>
  <variable name="v">second</variable>
</variables>
</muclient>)";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    CVariable* v = nullptr;
    ASSERT_TRUE(doc.GetVariableMap().Lookup("v", v));
    EXPECT_STREQ("second", (const char*)v->strContents);
}

// Alias validation error arms: on a world load each bad alias is tolerated
// (HandleLoadException) and skipped; the duplicate-label THROW arm needs a
// no-overwrite Load_Set.
TEST(WorldLoadR5, AliasValidationArms)
{
    struct { const char* aliasXml; bool loads; } cases[] = {
        // legacy 'queue' flag maps onto send-to-command-queue
        { "<alias match=\"q\" name=\"qf\" queue=\"y\" send_to=\"0\"><send>x</send></alias>", true },
        // 'queue' + incompatible send_to: skipped
        { "<alias match=\"q\" name=\"qb\" queue=\"y\" send_to=\"12\"><send>x</send></alias>", false },
        // send-to-variable without a variable name: skipped
        { "<alias match=\"q\" name=\"nv\" send_to=\"9\"><send>x</send></alias>", false },
        // invalid variable name: skipped
        { "<alias match=\"q\" name=\"bv\" send_to=\"9\" variable=\"9bad\"><send>x</send></alias>", false },
        // invalid label: skipped
        { "<alias match=\"q\" name=\"9bad\"><send>x</send></alias>", false },
        // no match text at all: skipped
        { "<alias name=\"nm\"><send>x</send></alias>", false },
        // regexp that does not compile: skipped
        { "<alias match=\"(unclosed\" name=\"br\" regexp=\"y\"/>", false },
    };

    for (auto& c : cases) {
        std::string xml =
            "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
            "<aliases>\n" + std::string(c.aliasXml) + "\n</aliases>\n</muclient>\n";
        CMUSHclientDoc doc;
        EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml.c_str()).c_str()));
        EXPECT_EQ(c.loads ? 1 : 0, (int)doc.GetAliasMap().GetCount());
    }

    {   // duplicate label via Load_Set (no XML_OVERWRITE): second refused
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><aliases>
  <alias match="a" name="same"/>
  <alias match="b" name="same"/>
</aliases></muclient>)";
        std::string path = write_temp_named("aliasdupset.xml", xml);
        CMUSHclientDoc doc;
        doc.Load_Set(ALIAS, CString(path.c_str()), nullptr);
        EXPECT_EQ(1, (int)doc.GetAliasMap().GetCount());
        CAlias* a = nullptr;
        ASSERT_TRUE(doc.GetAliasMap().Lookup("same", a));
        EXPECT_STREQ("a", (LPCTSTR)a->name);   // the FIRST one won
    }
}

// Timer arms: at_time timers read hour/minute/second into the At fields;
// legacy speed_walk/note flags map onto send_to; validation errors throw.
TEST(WorldLoadR5, TimerAtTimeAndLegacyFlagArms)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<timers>
  <timer name="att" at_time="y" hour="3" minute="4" second="5" enabled="y">
    <send>tick</send>
  </timer>
  <timer name="sw" speed_walk="y" hour="0" minute="1" second="0"><send>4n</send></timer>
  <timer name="nt" note="y" hour="0" minute="1" second="0"><send>hi</send></timer>
</timers>
</muclient>)";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    CTimer* t = nullptr;
    ASSERT_TRUE(doc.GetTimerMap().Lookup("att", t));
    EXPECT_EQ((int)CTimer::eAtTime, (int)t->iType);
    EXPECT_EQ(3, t->iAtHour);
    EXPECT_EQ(4, t->iAtMinute);

    ASSERT_TRUE(doc.GetTimerMap().Lookup("sw", t));
    EXPECT_EQ((int)eSendToSpeedwalk, (int)t->iSendTo);
    ASSERT_TRUE(doc.GetTimerMap().Lookup("nt", t));
    EXPECT_EQ((int)eSendToOutput, (int)t->iSendTo);
}

TEST(WorldLoadR5, TimerValidationArms)
{
    struct { const char* timerXml; } cases[] = {
        // speed_walk + incompatible send_to: skipped
        { "<timer name=\"t1\" speed_walk=\"y\" send_to=\"12\" minute=\"1\"><send>x</send></timer>" },
        // note + incompatible send_to: skipped
        { "<timer name=\"t2\" note=\"y\" send_to=\"12\" minute=\"1\"><send>x</send></timer>" },
        // send-to-variable without variable: skipped
        { "<timer name=\"t3\" send_to=\"9\" minute=\"1\"><send>x</send></timer>" },
        // invalid variable name: skipped
        { "<timer name=\"t4\" send_to=\"9\" variable=\"9bad\" minute=\"1\"><send>x</send></timer>" },
        // invalid label: skipped
        { "<timer name=\"9bad\" minute=\"1\"><send>x</send></timer>" },
    };

    for (auto& c : cases) {
        std::string xml =
            "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
            "<timers>\n" + std::string(c.timerXml) + "\n</timers>\n</muclient>\n";
        CMUSHclientDoc doc;
        EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml.c_str()).c_str()));
        EXPECT_EQ(0, (int)doc.GetTimerMap().GetCount());
    }

    {   // duplicate label via Load_Set (no XML_OVERWRITE): second refused
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><timers>
  <timer name="same" minute="1"/>
  <timer name="same" minute="2"/>
</timers></muclient>)";
        std::string path = write_temp_named("timerdupset.xml", xml);
        CMUSHclientDoc doc;
        doc.Load_Set(TIMER, CString(path.c_str()), nullptr);
        EXPECT_EQ(1, (int)doc.GetTimerMap().GetCount());
        CTimer* t = nullptr;
        ASSERT_TRUE(doc.GetTimerMap().Lookup("same", t));
        EXPECT_EQ(1, t->iEveryMinute);         // the FIRST one won
    }
}

// Structural tags that are world-only: while a plugin is current (as with a
// plugin script calling ImportXML), each throws "... cannot be used inside a
// plugin".  Driven through Load_World_XML directly with a full section mask.
TEST(WorldLoadR5, WorldOnlyTagsRejectedInsidePlugin)
{
    // a real plugin so m_CurrentPlugin is a live CPlugin
    const char* pxml =
        "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
        "<plugin name=\"P\" id=\"0123456789abcdef012345cc\" language=\"lua\">\n"
        "</plugin>\n</muclient>\n";
    CMUSHclientDoc doc;
    ASSERT_TRUE(TryLoadPlugin(doc, write_temp_named("worldonly.xml", pxml).c_str()));
    doc.m_CurrentPlugin = doc.m_PluginList.back();

    const char* tags[] = {
        "<world site=\"x\"/>",
        "<macros><macro name=\"F2\" send_to=\"0\"/></macros>",
        "<colours><ansi/></colours>",
        "<keypad><key name=\"Numpad0\">x</key></keypad>",
        "<printing><ansi/></printing>",
    };
    for (const char* tag : tags) {
        std::string xml =
            "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n" +
            std::string(tag) + "\n</muclient>\n";
        CMemFile f((unsigned char*)xml.data(), xml.size());
        CArchive ar(&f, CArchive::load);
        bool threw = false;
        try {
            doc.Load_World_XML(ar,
                (unsigned long)~(XML_PLUGINS | XML_NO_PLUGINS |
                                 XML_PASTE_DUPLICATE | XML_IMPORT_MAIN_FILE_ONLY),
                0);
        }
        catch (CException* e) { e->Delete(); threw = true; }
        EXPECT_TRUE(threw);
    }
    doc.m_CurrentPlugin = nullptr;
}

// Plugin header validation arms, one throw each.
//
// The two `language=` cases are platform-dependent and deliberately kept rather
// than deleted.  stdafx.h defines USE_PYTHON / USE_TCL / USE_PHP / USE_RUBY
// unconditionally inside its `#ifdef _WIN32` block, and the whitelist in
// xml_load_world.cpp:2538 also lets perlscript and jscript through on `_WIN32`
// alone -- so on Windows all of them are ACCEPTED, and the port accepts only
// the engines it actually built.  Asserting the accept is worth as much as
// asserting the refusal: it is the same fork read from the other side.
TEST(WorldLoadR5, PluginHeaderValidationArms)
{
    // The languages Windows whitelists unconditionally and this port build does
    // not: the expectation flips rather than the case disappearing.
#ifdef _WIN32
    const bool kExtraLanguagesLoad = true;
#else
    const bool kExtraLanguagesLoad = false;
#endif

    struct { const char* name; const char* header; } cases[] = {
        { "p_req.xml",     "name=\"P\" id=\"0123456789abcdef012345dd\" requires=\"99.99\"" },
        { "p_badname.xml", "name=\"9bad\" id=\"0123456789abcdef012345dd\"" },
        { "p_longname.xml",
          "name=\"a123456789012345678901234567890123\" id=\"0123456789abcdef012345dd\"" },
        { "p_author.xml",
          "name=\"P\" author=\"a123456789012345678901234567890123\" id=\"0123456789abcdef012345dd\"" },
        { "p_purpose.xml",
          "name=\"P\" purpose=\"a123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789x\" id=\"0123456789abcdef012345dd\"" },
        { "p_shortid.xml", "name=\"P\" id=\"abc\"" },
        { "p_hexid.xml",   "name=\"P\" id=\"zzzzzzzzzzzzzzzzzzzzzzzz\"" },
    };

    for (auto& c : cases) {
        std::string xml =
            "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
            "<plugin " + std::string(c.header) + ">\n</plugin>\n</muclient>\n";
        CMUSHclientDoc doc;
        bool loaded = TryLoadPlugin(doc, write_temp_named(c.name, xml.c_str()).c_str());
        // Name the case: they share one source line, so a bare EXPECT_FALSE
        // says only that one of seven did the wrong thing.
        if (loaded)
            fprintf(stderr, "  header case %s LOADED and should not have\n", c.name);
        EXPECT_FALSE(loaded);
    }

    struct { const char* name; const char* header; } languages[] = {
        { "p_lang.xml",    "name=\"P\" id=\"0123456789abcdef012345dd\" language=\"rubyscript\"" },
        { "p_lang_perl.xml",
          "name=\"P\" id=\"0123456789abcdef012345de\" language=\"perlscript\"" },
    };

    for (auto& c : languages) {
        std::string xml =
            "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
            "<plugin " + std::string(c.header) + ">\n</plugin>\n</muclient>\n";
        CMUSHclientDoc doc;
        bool loaded = TryLoadPlugin(doc, write_temp_named(c.name, xml.c_str()).c_str());
        if (loaded != kExtraLanguagesLoad)
            fprintf(stderr, "  language case %s %s and should not have\n",
                    c.name, loaded ? "LOADED" : "was REFUSED");
        EXPECT_EQ(kExtraLanguagesLoad, loaded);
    }
}

// A whitelisted-but-unbuildable language ("python" is accepted off-Windows)
// reaches CreateScriptEngine, which fails headless: "Could not initialise
// script engine".
TEST(WorldLoadR5, PluginNonLuaEngineFailsToInitialise)
{
    const char* xml =
        "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
        "<plugin name=\"P\" id=\"0123456789abcdef012345ee\" language=\"python\">\n"
        "</plugin>\n"
        "<script>print('hi')</script>\n"
        "</muclient>\n";
    CMUSHclientDoc doc;
    EXPECT_FALSE(TryLoadPlugin(doc, write_temp_named("p_python.xml", xml).c_str()));
}

// A <script> tag with no language: the throw is tolerated per-item
// (HandleLoadException) — the plugin loads, but the script text is dropped.
TEST(WorldLoadR5, PluginScriptWithoutLanguageDropped)
{
    const char* xml =
        "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
        "<plugin name=\"P\" id=\"0123456789abcdef012345ef\">\n</plugin>\n"
        "<script>whatever</script>\n"
        "</muclient>\n";
    CMUSHclientDoc doc;
    EXPECT_TRUE(TryLoadPlugin(doc, write_temp_named("p_nolang.xml", xml).c_str()));
    EXPECT_TRUE(doc.m_PluginList.back()->m_strScript.IsEmpty());
}

// Loading the same plugin id twice: "The plugin '%s' is already loaded."
TEST(WorldLoadR5, PluginDuplicateIdRejected)
{
    const char* xml =
        "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
        "<plugin name=\"P\" id=\"0123456789abcdef012345f0\" language=\"lua\">\n"
        "</plugin>\n</muclient>\n";
    std::string path = write_temp_named("p_dup.xml", xml);
    CMUSHclientDoc doc;
    EXPECT_TRUE(TryLoadPlugin(doc, path.c_str()));
    EXPECT_FALSE(TryLoadPlugin(doc, path.c_str()));
}

// Plugin state files: an XML state file's variables are merged in (with
// XML_OVERWRITE), a state file containing a <plugin> tag aborts the load,
// and a non-XML non-empty state file is rejected.
TEST(WorldLoadR5, PluginStateFileArms)
{
    char dir[128];
    snprintf(dir, sizeof dir, "/tmp/mc_state_%d/", (int)getpid());
    mkdir(dir, 0755);

    CString savedState = App.m_strDefaultStateFilesDirectory;
    App.m_strDefaultStateFilesDirectory = dir;

    const char* pid = "0123456789abcdef012345f1";
    std::string plugin =
        "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
        "<plugin name=\"P\" id=\"" + std::string(pid) + "\" language=\"lua\" save_state=\"y\">\n"
        "</plugin>\n</muclient>\n";
    std::string ppath = write_temp_named("p_state.xml", plugin.c_str());

    // 1. good state: a variable, loaded with overwrite semantics
    {
        CMUSHclientDoc doc;
        doc.m_strWorldID = "0123456789abcdef01234567";
        std::string state = std::string(dir) + (const char*)doc.m_strWorldID +
                            "-" + pid + "-state.xml";
        FILE* f = fopen(state.c_str(), "wb");
        ASSERT_NE(nullptr, f);
        fputs("<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
              "<variables>\n<variable name=\"sv\">persisted</variable>\n"
              "</variables>\n</muclient>\n", f);
        fclose(f);

        ASSERT_TRUE(TryLoadPlugin(doc, ppath.c_str()));
        CPlugin* p = doc.m_PluginList.back();
        CVariable* v = nullptr;
        ASSERT_TRUE(p->m_VariableMap.Lookup("sv", v));
        EXPECT_STREQ("persisted", (const char*)v->strContents);
        unlink(state.c_str());
    }

    // 2. state file containing a plugin tag: "Plugin not expected here"
    {
        CMUSHclientDoc doc;
        doc.m_strWorldID = "0123456789abcdef01234567";
        std::string state = std::string(dir) + (const char*)doc.m_strWorldID +
                            "-" + pid + "-state.xml";
        FILE* f = fopen(state.c_str(), "wb");
        ASSERT_NE(nullptr, f);
        fputs("<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
              "<plugin name=\"Q\" id=\"0123456789abcdef012345f2\"/>\n"
              "</muclient>\n", f);
        fclose(f);
        EXPECT_FALSE(TryLoadPlugin(doc, ppath.c_str()));
        unlink(state.c_str());
    }

    // 3. non-XML, non-empty state file: "is not an XML file"
    {
        CMUSHclientDoc doc;
        doc.m_strWorldID = "0123456789abcdef01234567";
        std::string state = std::string(dir) + (const char*)doc.m_strWorldID +
                            "-" + pid + "-state.xml";
        FILE* f = fopen(state.c_str(), "wb");
        ASSERT_NE(nullptr, f);
        fputs("this is not xml at all", f);
        fclose(f);
        EXPECT_FALSE(TryLoadPlugin(doc, ppath.c_str()));
        unlink(state.c_str());
    }

    App.m_strDefaultStateFilesDirectory = savedState;
}

// <comment> content lands on the "XML import notes" notepad.
// Port-only observation seam.  The notepad fixture drives CTextDocument's
// m_fn_* callbacks and hooks g_wxhook_create_text_window, and both are
// #ifndef _WIN32: on Windows a notepad IS a real CEditView created through the
// MFC doc/view framework, so there is no function pointer to substitute and no
// window for this host to make.  Re-entering it means giving the test host a
// document template and a frame -- the same thing test_accelerators.cpp needs
// (docs/WINDOWS-VALIDATION.md 10k) -- not deleting the guard.
#ifndef _WIN32
TEST(WorldLoadR5, CommentsGoToTheNotepad)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<comment>Imported from somewhere.</comment>
</muclient>)";

    // Capture at g_wxhook_create_text_window, the seam the product uses: no
    // notepad by this title exists yet, so the real AppendToTheNotepad
    // (methods_notepad.cpp:103, adopted into mushcore) falls through to
    // CreateTextWindow, which is what makes the window in the first place.
    // This used to capture at g_wxhook_append_to_notepad, a hook on the headless
    // twin that has since been deleted.
    NotepadCapture cap;

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));

    // the <comment> body lands on the "XML import notes ..." notepad
    ASSERT_EQ(1u, (unsigned) cap.count());
    EXPECT_NE(std::string::npos, cap.text().find("Imported from somewhere."));
    EXPECT_NE(std::string::npos,
              std::string((const char*) cap.pads[0]->doc->m_strTitle)
                  .find("XML import notes"));
}
#endif  // !_WIN32

// An include file whose name is the reserved clipboard pseudo-name is refused
// BEFORE it is recorded (the check at xml_load_world.cpp:836 throws before the
// AddTail at :884), so m_strIncludeFileList stays empty — while a plain missing
// include IS recorded first and only then fails to open.
TEST(WorldLoadR5, IncludeFileReservedNameRejected)
{
    // CLIPBOARD_NAME is "Clipboard" (CompareNoCase), so the reserved name must
    // be exactly that to trip the guard.
    const char* reserved =
        "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
        "<include name=\"Clipboard\"/>\n"
        "</muclient>\n";
    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(reserved).c_str()));
    EXPECT_TRUE(doc.m_strIncludeFileList.IsEmpty());   // never recorded

    // Control: a non-reserved include that merely fails to open IS recorded
    // (AddTail runs before the open throws) — proving a reserved include would
    // show up in the list too if the guard didn't short-circuit it first.
    const char* missing =
        "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
        "<include name=\"nonexistent_include_file.xml\"/>\n"
        "</muclient>\n";
    CMUSHclientDoc doc2;
    EXPECT_TRUE(doc2.LoadWorldFromFile(write_temp(missing).c_str()));
    EXPECT_EQ(1, (int)doc2.m_strIncludeFileList.GetCount());
}

// <send trim="y"> strips leading/trailing whitespace from the send text.
TEST(WorldLoadR5, SendTextTrimAttribute)
{
    const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<triggers>
  <trigger match="x" name="tr" send_to="0">
    <send trim="y">
       say hi
    </send>
  </trigger>
</triggers>
</muclient>)";

    CMUSHclientDoc doc;
    EXPECT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("tr", t));
    EXPECT_STREQ("say hi", (const char*)t->contents);
}



// xml_save_world.cpp Save_One_Macro_XML (~607-633): the macro send-type maps
// to a string via a switch -- ADD_TO_COMMAND -> "insert", and any value that
// is not REPLACE_COMMAND/SEND_NOW/ADD_TO_COMMAND falls to default -> "unknown".
// Macros live in the fixed m_macros[]/m_macro_type[] arrays (function-key
// sends); the saver writes name= (from strMacroDescriptions[i]) then type= on
// the <macro> line, skipping entries whose send text is empty.
TEST(WorldLoad, MacroTypeSavedInsertAndUnknown)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_macro_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
        // strMacroDescriptions[0]=="up", [1]=="down" (windows_stubs.cpp table).
        doc.m_macros[0]     = "into the command line";
        doc.m_macro_type[0] = ADD_TO_COMMAND;          // -> "insert"
        doc.m_macros[1]     = "bogus send-type macro";
        doc.m_macro_type[1] = 99;                      // out of range -> "unknown"
        ASSERT_TRUE(doc.OnSaveDocument(path));
    }
    std::string c = slurp(path);
    // Save_One_Macro_XML writes name= then type= on the same <macro> line.
    EXPECT_NE(c.find("name=\"up\" type=\"insert\""),    std::string::npos);
    EXPECT_NE(c.find("name=\"down\" type=\"unknown\""), std::string::npos);
}


// xml_save_world.cpp Save_Includes_XML (~764-776): every entry in the doc's
// m_strIncludeFileList is written as <include name="..." /> (part of the
// XML_INCLUDES mask, which OnSaveDocument's ~0 save includes).  No plugins are
// loaded, so this is the only <include element in the file.
TEST(WorldLoad, IncludeFileListWritten)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_incsave_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
        doc.m_strIncludeFileList.AddTail("my_included_file.xml");
        ASSERT_TRUE(doc.OnSaveDocument(path));
    }
    std::string c = slurp(path);
    EXPECT_NE(c.find("<include name=\"my_included_file.xml\""), std::string::npos);
}


// xml_save_world.cpp Save_Printing_XML (~723-737): a normal-print style entry
// that is not FONT_NORMAL is written as <style seq="N" bold=.. italic=..
// underline=.. />, seq being 1-relative (i+1).  Seed exactly FONT_BOLD (0x04)
// at index 0 so only the bold attribute is emitted.
TEST(WorldLoad, NormalPrintStyleBoldWritten)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_pstyle_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
        doc.m_nNormalPrintStyle[0] = 0x04;   // FONT_BOLD (genprint.h) -> bold="y"
        ASSERT_TRUE(doc.OnSaveDocument(path));
    }
    std::string c = slurp(path);
    // Only the bold bit is set, so italic/underline are omitted; seq is i+1 = 1.
    EXPECT_NE(c.find("<style seq=\"1\" bold=\"y\""), std::string::npos);
}


// xml_save_world.cpp Save_One_Trigger_XML (~433-438): a trigger's "other"
// custom-colour slot (iOtherForeground/iOtherBackground) is written as
// other_text_colour / other_back_colour whenever non-zero (black is the omitted
// default).  ColourToName renders an RGB with no named match as #RRGGBB, and
// RGB(1,2,3)/RGB(4,5,6) are not named MXP colours, so they serialise to
// #010203 / #040506 and read back exactly.
TEST(WorldLoad, TriggerOtherCustomColourWrittenAndRoundTrips)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_othercol_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
        CTrigger* t = new CTrigger();
        t->trigger  = CString("other colour match");
        t->strLabel = CString("otherc");
        t->bEnabled = TRUE;
        t->colour   = OTHER_CUSTOM;                 // the "other" custom slot
        t->iOtherForeground = RGB(1, 2, 3);
        t->iOtherBackground = RGB(4, 5, 6);
        doc.GetTriggerMap().SetAt(t->strLabel, t);
        ASSERT_TRUE(doc.OnSaveDocument(path));
    }
    std::string c = slurp(path);
    EXPECT_NE(c.find("other_text_colour=\"#010203\""), std::string::npos);
    EXPECT_NE(c.find("other_back_colour=\"#040506\""), std::string::npos);

    // round-trip: the "other" colours are read back onto the reloaded trigger.
    CMUSHclientDoc doc2;
    ASSERT_TRUE(doc2.LoadWorldFromFile(path));
    CTrigger* r = nullptr;
    ASSERT_TRUE(doc2.GetTriggerMap().Lookup("otherc", r));
    ASSERT_NE(nullptr, r);
    EXPECT_EQ((int)RGB(1, 2, 3), (int)r->iOtherForeground);
    EXPECT_EQ((int)RGB(4, 5, 6), (int)r->iOtherBackground);
}


// xml_save_world.cpp Save_World_XML (~66-70): an XML comment cannot contain
// "--", so a supplied save-comment has every "--" collapsed to "-" before it is
// written as <!-- ... -->.  OnSaveDocument passes an empty comment, so drive the
// writer directly with a CArchive (the same public entry the plugin save-state
// path uses, plugins.cpp:855).
TEST(WorldLoad, SavedCommentDoubleHyphenCollapsed)
{
    const char* base = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient>
<world site="test.mud" port="4000">
</world>
</muclient>)";
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_comment_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(base).c_str()));
        CFile f;
        ASSERT_TRUE(f.Open(path,
            CFile::modeWrite | CFile::modeCreate | CFile::shareDenyWrite));
        {
            CArchive ar(&f, CArchive::store);
            doc.Save_World_XML(ar, (unsigned long)~0, CString("before--after"));
            ar.Close();
        }
        f.Flush();
        f.Close();
    }
    std::string c = slurp(path);
    EXPECT_NE(c.find("<!-- before-after -->"), std::string::npos);   // collapsed
    EXPECT_EQ(c.find("before--after"),         std::string::npos);   // no "--"
}


// The other half of NumericEntityAllowsLineFeedAndCarriageReturn (:163): the
// reader was widened (f96a15c) to accept &#10;/&#13;, but the WRITER still
// emitted a lone CR/LF raw -- Save_XML_string collapsed only the exact CRLF
// pair, and FixHTMLString escaped only < > & " -- so the port loaded such a
// file and then destroyed it on the next save: the parser refuses a raw
// control character inside a quoted value and the whole world became
// unopenable.  Save_XML_string now escapes a residual CR/LF/tab as the
// matching numeric entity (FixXMLAttributeString), making writer and reader
// symmetric.  docs/UPSTREAM-DIVERGENCES.md §22.
TEST(WorldLoad, NumericEntitySurvivesSaveAndReload)
{
    const char* xml = "<?xml version=\"1.0\"?>\n"
"<!DOCTYPE muclient>\n"
"<muclient>\n"
"<triggers>\n"
"  <trigger match=\"abc&#10;def&#13;ghi\" name=\"t1\" enabled=\"y\">\n"
"    <send></send>\n"
"  </trigger>\n"
"</triggers>\n"
"</muclient>\n";

    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_entity_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
        CTrigger* t = nullptr;
        ASSERT_TRUE(doc.GetTriggerMap().Lookup("t1", t));
        EXPECT_STREQ("abc\ndef\rghi", (const char*)t->trigger);

        CFile f;
        ASSERT_TRUE(f.Open(path,
            CFile::modeWrite | CFile::modeCreate | CFile::shareDenyWrite));
        {
            CArchive ar(&f, CArchive::store);
            doc.Save_World_XML(ar, (unsigned long)~0, CString(""));
            ar.Close();
        }
        f.Flush();
        f.Close();
    }

    // The written attribute carries the entities, not raw control characters.
    std::string c = slurp(path);
    // The saved match attribute must re-emit the numeric entities.
    EXPECT_NE(c.find("match=\"abc&#10;def&#13;ghi\""), std::string::npos);
    // No raw LF/CR anywhere inside a match attribute value.
    size_t m = c.find("match=\"");
    ASSERT_NE(m, std::string::npos);
    size_t close = c.find('"', m + 7);
    ASSERT_NE(close, std::string::npos);
    // A raw LF inside the quoted value would make the file unopenable.
    EXPECT_FALSE(c.find('\n', m + 7) < close);

    // The discriminating leg: the saved file must LOAD, byte-identically.
    // At the pre-fix HEAD this failed with the parser's "not terminated
    // (no closing quote)" -- the file the app itself wrote was refused by
    // the app's own loader.
    CMUSHclientDoc doc2;
    ASSERT_TRUE(doc2.LoadWorldFromFile(path));
    CTrigger* t2 = nullptr;
    ASSERT_TRUE(doc2.GetTriggerMap().Lookup("t1", t2));
    EXPECT_STREQ("abc\ndef\rghi", (const char*)t2->trigger);
}

// The same hole without any hand-authored XML, and on BOTH platforms: the
// script API stores match text unfiltered (methods_triggers.cpp), so
// world.AddTrigger("t", "a\nb", ...) put a raw LF in the trigger and the next
// save produced an unloadable file -- upstream latent bug, closed by the same
// writer escape.  A tab exercises the pre-existing upstream half (Get_XML_Entity
// always accepted &#9;, but FixHTMLString could never re-emit one).
TEST(WorldLoad, AddTriggerEmbeddedControlCharsSurviveSaveAndReload)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_world_addtrig_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_EQ((long)eOK, doc.AddTrigger("t_lf", "a\nb", "resp",
                                            1 /*eEnabled*/, -1, 0, "", ""));
        ASSERT_EQ((long)eOK, doc.AddTrigger("t_tab", "a\tb", "resp",
                                            1 /*eEnabled*/, -1, 0, "", ""));
        CFile f;
        ASSERT_TRUE(f.Open(path,
            CFile::modeWrite | CFile::modeCreate | CFile::shareDenyWrite));
        {
            CArchive ar(&f, CArchive::store);
            doc.Save_World_XML(ar, (unsigned long)~0, CString(""));
            ar.Close();
        }
        f.Flush();
        f.Close();
    }

    std::string c = slurp(path);
    EXPECT_NE(c.find("a&#10;b"), std::string::npos);   // LF escaped in the attribute
    EXPECT_NE(c.find("a&#9;b"),  std::string::npos);   // tab escaped in the attribute

    CMUSHclientDoc doc2;
    ASSERT_TRUE(doc2.LoadWorldFromFile(path));
    CTrigger* t = nullptr;
    ASSERT_TRUE(doc2.GetTriggerMap().Lookup("t_lf", t));
    EXPECT_STREQ("a\nb", (const char*)t->trigger);
    t = nullptr;
    ASSERT_TRUE(doc2.GetTriggerMap().Lookup("t_tab", t));
    EXPECT_STREQ("a\tb", (const char*)t->trigger);
}

// Negative control: the escape above is load-bearing BECAUSE the parser
// rejects the raw form.  If it ever silently started accepting a raw LF
// inside a quoted value, the escape would stop being what makes the round
// trip work and this family of tests would prove nothing -- so pin the
// rejection itself.
TEST(WorldLoad, RawLinefeedInAttributeValueIsRejected)
{
    const char* xml = "<?xml version=\"1.0\"?>\n"
"<!DOCTYPE muclient>\n"
"<muclient>\n"
"<triggers>\n"
"  <trigger match=\"abc\ndef\" name=\"t1\" enabled=\"y\">\n"
"    <send></send>\n"
"  </trigger>\n"
"</triggers>\n"
"</muclient>\n";

    CMUSHclientDoc doc;
    EXPECT_FALSE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
}

// xml_load_world.cpp: world loads carry XML_OVERWRITE, so a second alias
// (alias_check, ~1577-1584) or timer (timer_check, ~1817-1824) that shares an
// explicit name deletes the first and the SECOND definition wins -- mirroring
// the duplicate-label trigger arm proven in WorldLoadR4.TriggerValidationArms.
TEST(WorldLoadR5, DuplicateExplicitLabelReplacedOnLoad)
{
    {   // two aliases, same explicit name="dup": second replaces first
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><aliases>
  <alias match="firstmatch"  name="dup" enabled="y" sequence="100"><send>x</send></alias>
  <alias match="secondmatch" name="dup" enabled="y" sequence="100"><send>y</send></alias>
</aliases></muclient>)";
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
        EXPECT_EQ(1, (int)doc.GetAliasMap().GetCount());
        CAlias* a = nullptr;
        ASSERT_TRUE(doc.GetAliasMap().Lookup("dup", a));
        EXPECT_STREQ("secondmatch", (const char*)a->name);   // second wins
    }
    {   // two timers, same explicit name="dup" (non-zero interval each)
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><timers>
  <timer name="dup" enabled="y" second="5"><send>one</send></timer>
  <timer name="dup" enabled="y" second="5"><send>two</send></timer>
</timers></muclient>)";
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
        EXPECT_EQ(1, (int)doc.GetTimerMap().GetCount());
        CTimer* t = nullptr;
        ASSERT_TRUE(doc.GetTimerMap().Lookup("dup", t));
        EXPECT_STREQ("two", (const char*)t->strContents);   // second wins
    }
}

// A duplicate label whose second definition then FAILS validation must not
// leave the freed original in the map.  The overwrite arm deletes the old
// object, and the regexp compile a few lines later throws for a bad pattern;
// Load_Triggers_XML reports that and carries on, so anything still keyed to
// the deleted object (SortTriggers, the destructor's DELETE_MAP) would be
// working on freed memory.  "keep" is there to give SortTriggers two entries,
// which is what makes it compare sequences and dereference them.
TEST(WorldLoadR5, DuplicateLabelWhoseReplacementFailsLeavesNoFreedEntry)
{
    {   // triggers
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><triggers>
  <trigger match="a" name="keep" sequence="100"><send>y</send></trigger>
  <trigger match="b" name="dup"  sequence="100"><send>y</send></trigger>
  <trigger match="(unclosed" regexp="y" name="dup" sequence="100"><send>y</send></trigger>
</triggers></muclient>)";
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
        // the replacement was refused, and the original went with it
        EXPECT_EQ(1, (int)doc.GetTriggerMap().GetCount());
        CTrigger* t = nullptr;
        EXPECT_FALSE(doc.GetTriggerMap().Lookup("dup", t));
        ASSERT_TRUE(doc.GetTriggerMap().Lookup("keep", t));
        EXPECT_STREQ("a", (const char*)t->trigger);
        doc.SortTriggers();   // must not walk a freed entry
    }
    {   // aliases - identical shape
        const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE muclient>
<muclient><aliases>
  <alias match="a" name="keep" sequence="100"><send>y</send></alias>
  <alias match="b" name="dup"  sequence="100"><send>y</send></alias>
  <alias match="(unclosed" regexp="y" name="dup" sequence="100"><send>y</send></alias>
</aliases></muclient>)";
        CMUSHclientDoc doc;
        ASSERT_TRUE(doc.LoadWorldFromFile(write_temp(xml).c_str()));
        EXPECT_EQ(1, (int)doc.GetAliasMap().GetCount());
        CAlias* a = nullptr;
        EXPECT_FALSE(doc.GetAliasMap().Lookup("dup", a));
        ASSERT_TRUE(doc.GetAliasMap().Lookup("keep", a));
        EXPECT_STREQ("a", (const char*)a->name);
        doc.SortAliases();    // must not walk a freed entry
    }
}

MC_TEST_MAIN()
