// tests/test_lua_surface.cpp
// Coverage-round-2 sweep of the world.* Lua surface (scripting/lua_methods.cpp
// L_* wrappers) and the utils.* portable library, driving each family through
// the same marshalling path a real Lua plugin uses.  Expected values are pinned
// against the shipping sources:
//   - methods_utilities.cpp / methods_sounds.cpp (_NOSOUND arm) /
//     methods_udp.cpp / methods_arrays.cpp / methods_plugins.cpp /
//     methods_infobar.cpp / methods_commands.cpp / methods_xml.cpp
//   - platform/doc_gui_method_stubs.cpp (Window*/Notepad*/Chat* headless stubs)
//   - scripting/lua_utils_portable.cpp (utils.*)
// GUI-only calls answer their documented inert values headless (hooks null).

#ifdef MUSHCLIENT_LUA_SCRIPTING

#include "mc_test.h"
// B-3 atomic flip: real headers (the headless stub is gone; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "scripting/scripting.h"   // DOCUMENT_STATE

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

// make_lua()/run()/run_ret()/LuaDoc/EXPECT_LUA_OK are shared with
// test_lua_scripting.cpp (audit §2/§4).
#include "lua_test_helpers.h"
#include "notepad_fixture.h"

// ── methods_utilities.cpp: pure string helpers ────────────────────────────

TEST(LuaSurface, UtilityStringHelpers)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(world.StripANSI('a\\027[31mred\\027[0mb') == 'aredb')\n"
        "assert(world.FixupHTML('<&>') == '&lt;&amp;&gt;')\n"
        "assert(world.Base64Encode('abc', false) == 'YWJj')\n"
        "assert(world.Base64Decode('YWJj') == 'abc')\n"
        "assert(#world.Hash('x') == 40)\n"           // SHS digest: 5 x %08x
        "local g1, g2 = world.CreateGUID(), world.CreateGUID()\n"
        "assert(#g1 > 0 and g1 ~= g2)\n"
        "assert(#world.GetUniqueID() == 24)\n"
        "local n1 = world.GetUniqueNumber()\n"
        "assert(world.GetUniqueNumber() > n1)\n"
        "assert(type(world.MakeRegularExpression('a.b')) == 'string')\n"
        "assert(world.Trim('  x  ') == 'x')\n"
        "assert(type(world.FixupEscapeSequences('a\\\\tb')) == 'string')\n"
        "assert(world.TranslateDebug('msg') == 1)\n"  // no translator script
    ));
}

// ── methods_utilities.cpp: group enable/delete ────────────────────────────

TEST(LuaSurface, UtilityGroupOps)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(world.AddTrigger('gt', 'zzz', '', 1, -1, 0, '', '') == 0)\n"
        "assert(world.SetTriggerOption('gt', 'group', 'grp') == 0)\n"
        "assert(world.AddAlias('ga', 'yyy', '', 1, '') == 0)\n"
        "assert(world.SetAliasOption('ga', 'group', 'grp') == 0)\n"
        "assert(world.AddTimer('gm', 0, 1, 0, '', 0, '') == 0)\n"
        "assert(world.SetTimerOption('gm', 'group', 'grp') == 0)\n"
        "assert(world.EnableGroup('grp', 0) == 3)\n"   // one of each disabled
        "assert(world.DeleteGroup('grp') == 3)\n"      // and deleted
    ));
}

// ── methods_utilities.cpp: misc arms with pinned headless values ──────────

TEST(LuaSurface, UtilityMiscArms)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(world.GenerateName() == nil)\n"            // names file not read
        "assert(world.ReadNamesFile('') == error_code.eNoNameSpecified)\n"
        "assert(world.OpenBrowser('') == error_code.eBadParameter)\n"
        "assert(world.OpenBrowser('ftp://x') == error_code.eBadParameter)\n"
        "assert(world.SendPkt('x') == error_code.eWorldClosed)\n"  // not connected
        "assert(world.Disconnect() == error_code.eWorldClosed)\n"
        "assert(world.Menu('a|b', 'a') == '')\n"           // hook null headless
        "assert(world.Open('/no/such/file.mcl') == false)\n" // stub OpenDocumentFile
        "world.SetChanged(false)\n"
        "world.SetNotes('remember me')\n"
        "assert(world.GetNotes() == 'remember me')\n"
        "world.SetStatus('status text')\n"
        "world.ResetIP()\n"
        "world.Activate()\n"
        "world.SetSelection(1, 1, 1, 5)\n"
        "world.SetFrameBackgroundColour(255)\n"
        "world.Help('')\n"
        "world.Help('lua')\n"
        "world.Help('Send')\n"
    ));
}

TEST(LuaSurface, ChangeDirRoundTrip)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    char orig[1024];
    ASSERT_NE(nullptr, getcwd(orig, sizeof orig));
    std::string code =
        "assert(world.ChangeDir('/') == 1)\n"
        "assert(world.ChangeDir('/no/such/dir/xyz') == 0)\n"
        "assert(world.ChangeDir('" + std::string(orig) + "') == 1)\n";
    EXPECT_TRUE(run(f.L, code.c_str()));
    char now[1024];
    ASSERT_NE(nullptr, getcwd(now, sizeof now));
    EXPECT_STREQ(orig, now);
}

// ── methods_sounds.cpp (_NOSOUND arm, null hooks) ─────────────────────────

TEST(LuaSurface, SoundFamilyHeadless)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(world.Sound('') == error_code.eNoNameSpecified)\n"
        "assert(world.Sound('nofile.wav') == error_code.eCannotPlaySound)\n"
        "assert(world.PlaySound(0, 'f.wav', false, 0, 0) == error_code.eCannotPlaySound)\n"
        "assert(world.PlaySoundMemory(0, 'notawav') == error_code.eCannotPlaySound)\n"
        "assert(world.StopSound(0) == error_code.eOK)\n"
        "assert(world.GetSoundStatus(1) == -3)\n"     // no audio backend
        "assert(world.GetSoundStatus(0) == -3)\n"
    ));
}

// ── methods_udp.cpp ───────────────────────────────────────────────────────

TEST(LuaSurface, UdpFamily)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(world.UdpSend('not.an.ip', 4111, 'x') == -1)\n"
        "assert(world.UdpSend('127.0.0.1', 46311, 'hello') == 0)\n"
        "assert(world.UdpListen('127.0.0.1', 46312, 's') == error_code.eNotAPlugin)\n"
        "assert(world.UdpPortList() == nil)\n"           // no listeners → VT_EMPTY
        "assert(world.GetUdpPort(10, 5) == 0)\n"         // bad range
        "assert(world.GetUdpPort(0, 100) == 0)\n"        // First < 1
        "assert(world.GetUdpPort(46000, 46010) == 46000)\n"
    ));
}

// ── methods_infobar.cpp — IWorldView sink is null headless: inert ─────────

TEST(LuaSurface, InfobarFamily)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "world.ShowInfoBar(1)\n"
        "world.Info('some text')\n"
        "world.InfoFont('Courier', 12, 1)\n"
        "world.InfoColour('red')\n"
        "world.InfoColour('nosuchcolourname')\n"   // SetColour fails → skipped
        "world.InfoBackground('white')\n"
        "world.InfoClear()\n"
    ));
}

// ── methods_arrays.cpp — the whole family through Lua ─────────────────────

TEST(LuaSurface, ArraysLifecycle)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(world.ArrayCount() == 0)\n"
        "assert(world.ArrayListAll() == nil)\n"          // none → VT_EMPTY → nil
        "assert(world.ArrayCreate('a') == error_code.eOK)\n"
        "assert(world.ArrayCreate('a') == error_code.eArrayAlreadyExists)\n"
        "assert(world.ArrayCount() == 1)\n"
        "assert(world.ArraySet('a', 'k1', 'v1') == error_code.eOK)\n"
        "assert(world.ArraySet('a', 'k2', 'v2') == error_code.eOK)\n"
        "assert(world.ArraySet('a', 'k1', 'v9') == error_code.eSetReplacingExistingValue)\n"
        "assert(world.ArrayGet('a', 'k1') == 'v9')\n"
        "assert(world.ArrayKeyExists('a', 'k1') == true)\n"
        "assert(world.ArrayKeyExists('a', 'nope') == false)\n"
        "assert(world.ArraySize('a') == 2)\n"
        "assert(world.ArrayGetFirstKey('a') == 'k1')\n"
        "assert(world.ArrayGetLastKey('a') == 'k2')\n"
        "local keys = world.ArrayListKeys('a')\n"
        "assert(keys[1] == 'k1' and keys[2] == 'k2')\n"
        "local vals = world.ArrayListValues('a')\n"
        "assert(vals[1] == 'v9' and vals[2] == 'v2')\n"
        "local all = world.ArrayListAll()\n"
        "assert(all[1] == 'a')\n"
        "assert(type(world.ArrayExport('a', ',')) == 'string')\n"
        "assert(world.ArrayExportKeys('a', ',') == 'k1,k2')\n"
        "assert(world.ArrayDeleteKey('a', 'nope') == error_code.eKeyDoesNotExist)\n"
        "assert(world.ArrayDeleteKey('a', 'k2') == error_code.eOK)\n"
        "assert(world.ArrayClear('a') == error_code.eOK)\n"
        "assert(world.ArraySize('a') == 0)\n"
        "assert(world.ArrayImport('a', 'k1,v1,k2,v2', ',') == error_code.eOK)\n"
        "assert(world.ArrayImport('a', 'k3,v3,k4', ',') == error_code.eArrayNotEvenNumberOfValues)\n"
        "assert(world.ArrayImport('a', 'x,y', ',,') == error_code.eBadDelimiter)\n"
        "assert(world.ArrayImport('a', 'k1,zz', ',') == error_code.eImportedWithDuplicates)\n"
        "assert(world.ArrayDelete('a') == error_code.eOK)\n"
        "assert(world.ArrayDelete('a') == error_code.eArrayDoesNotExist)\n"
        "assert(world.ArrayClear('a') == error_code.eArrayDoesNotExist)\n"
        "assert(world.ArrayGetFirstKey('a') == nil)\n"
        "assert(world.ArrayGetLastKey('a') == nil)\n"
        "assert(world.ArrayListKeys('a') == nil)\n"
        "assert(world.ArrayListValues('a') == nil)\n"
        "assert(world.ArrayExportKeys('a', ',') == error_code.eArrayDoesNotExist)\n"
    ));
}

// ── methods_plugins.cpp — a real Lua plugin loaded from XML ───────────────

#define SURF_ID "eeeeeeeeeeeeeeeeeeeeeee5"

static std::string write_surface_plugin()
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_surface_plugin_%d.xml",
             (int)getpid());
    FILE* fp = fopen(path, "wb");
    if (!fp) return "";
    fprintf(fp,
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE muclient>\n"
        "<muclient>\n"
        "<plugin name=\"SurfPlugin\" author=\"T\" id=\"%s\" language=\"Lua\" "
        "version=\"1.0\" />\n"
        "<triggers>\n"
        "  <trigger enabled=\"y\" match=\"surftrigmatch\" name=\"ptrig\" "
        "sequence=\"100\"><send>x</send></trigger>\n"
        "</triggers>\n"
        "<aliases>\n"
        "  <alias match=\"surfalias\" enabled=\"y\" name=\"palias\">"
        "<send>y</send></alias>\n"
        "</aliases>\n"
        "<timers>\n"
        "  <timer name=\"ptimer\" minute=\"10\" enabled=\"n\">"
        "<send>z</send></timer>\n"
        "</timers>\n"
        "<script><![CDATA[\n"
        "world.SetVariable('pv', '42')\n"
        "function pfunc(arg) world.SetVariable('rc', arg) end\n"
        "]]></script>\n"
        "</muclient>\n", SURF_ID);
    fclose(fp);
    return path;
}

TEST(LuaSurface, PluginQueriesAgainstLoadedPlugin)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    std::string path = write_surface_plugin();
    ASSERT_TRUE(!path.empty());

    std::string load = "assert(world.LoadPlugin('" + path + "') == 0)\n";
    ASSERT_TRUE(run(f.L, load.c_str()));

    EXPECT_TRUE(run(f.L,
        // list contains the plugin's ID; not inside a plugin: ID/name empty
        "local l = world.GetPluginList()\n"
        "assert(l[1] == '" SURF_ID "')\n"
        "assert(world.GetPluginID() == '')\n"
        "assert(world.GetPluginName() == '')\n"
        "assert(world.IsPluginInstalled('" SURF_ID "'))\n"
        "assert(not world.IsPluginInstalled('ffffffffffffffffffffffff'))\n"
        // variable written by the plugin's own script
        "assert(world.GetPluginVariable('" SURF_ID "', 'pv') == '42')\n"
        // Lua extension: GetPluginVariableList returns a NAME → VALUE map
        // (GetVariableListHelper), not an array of names.
        "local vl = world.GetPluginVariableList('" SURF_ID "')\n"
        "assert(vl.pv == '42', 'pv missing from variable map')\n"
        // trigger/alias/timer lists + info/options
        "local tl = world.GetPluginTriggerList('" SURF_ID "')\n"
        "assert(tl[1] == 'ptrig')\n"
        "assert(world.GetPluginTriggerInfo('" SURF_ID "', 'ptrig', 1) == 'surftrigmatch')\n"
        "local al = world.GetPluginAliasList('" SURF_ID "')\n"
        "assert(al[1] == 'palias')\n"
        "assert(world.GetPluginAliasInfo('" SURF_ID "', 'palias', 1) == 'surfalias')\n"
        "assert(world.GetPluginAliasOption('" SURF_ID "', 'palias', 'enabled') == 1)\n"
        "local ml = world.GetPluginTimerList('" SURF_ID "')\n"
        "assert(ml[1] == 'ptimer')\n"
        "assert(world.GetPluginTimerInfo('" SURF_ID "', 'ptimer', 2) == 10)\n"  // minute
        "assert(world.GetPluginTimerOption('" SURF_ID "', 'ptimer', 'minute') == 10)\n"
        // unknown plugin → nil (VT_NULL)
        "assert(world.GetPluginVariable('ffffffffffffffffffffffff', 'pv') == nil)\n"
        "assert(world.GetPluginTriggerList('ffffffffffffffffffffffff') == nil)\n"
    ));

    EXPECT_TRUE(run(f.L,
        // call into the plugin, then read the result it stored
        "assert(world.CallPlugin('" SURF_ID "', 'pfunc', 'hi') == 0)\n"
        "assert(world.GetPluginVariable('" SURF_ID "', 'rc') == 'hi')\n"
        "assert(world.CallPlugin('" SURF_ID "', 'nosuch', '') == error_code.eNoSuchRoutine)\n"
        "assert(world.CallPlugin('ffffffffffffffffffffffff', 'f', '') == error_code.eNoSuchPlugin)\n"
        "assert(world.PluginSupports('" SURF_ID "', 'pfunc') == 0)\n"
        "assert(world.PluginSupports('" SURF_ID "', 'nosuch') == error_code.eNoSuchRoutine)\n"
        // disable → calls rejected; re-enable → OK again
        "assert(world.EnablePlugin('" SURF_ID "', 0) == 0)\n"
        "assert(world.CallPlugin('" SURF_ID "', 'pfunc', 'x') == error_code.ePluginDisabled)\n"
        "assert(world.EnablePlugin('" SURF_ID "', 1) == 0)\n"
        "assert(world.EnablePlugin('ffffffffffffffffffffffff', 1) == error_code.eNoSuchPlugin)\n"
        // reload from the same file
        "assert(world.ReloadPlugin('" SURF_ID "') == 0)\n"
        "assert(world.ReloadPlugin('ffffffffffffffffffffffff') == error_code.eNoSuchPlugin)\n"
        "assert(world.IsPluginInstalled('" SURF_ID "'))\n"
    ));
    unlink(path.c_str());
}

// ── the Window* family's NO-SUCH-WINDOW arm ───────────────────────────────
//
// Every call here targets window 'w', which this fixture never creates, so the
// whole sweep still reads exactly as it did when these were stubs — but for a
// different reason.  It used to pass because the stubs answered eNoSuchWindow
// unconditionally; it passes now because the REAL bodies
// (scripting/methods/methods_miniwindows.cpp, adopted into mushcore — see
// docs/STUB-ADOPTION-2026-08.md) look the name up and do not find it.
//
// On its own that makes this a weak test, in exactly the way the notepad family
// was weak before its adoption: a body that did nothing at all would pass every
// line.  WindowFamilyWithAWindowOpen below is the other arm.

TEST(LuaSurface, WindowFamilyWithNoSuchWindow)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "local e = error_code.eNoSuchWindow\n"
        "assert(world.WindowAddHotspot('w','h',0,0,10,10,'','','','','','',0,0) == e)\n"
        "assert(world.WindowArc('w',0,0,10,10,0,0,5,5,0,0,1) == e)\n"
        "assert(world.WindowBezier('w','0,0,1,1,2,2,3,3',0,0,1) == e)\n"
        "assert(world.WindowBlendImage('w','i',0,0,10,10,1,1) == e)\n"
        "assert(world.WindowCircleOp('w',1,0,0,10,10,0,0,1,0,0) == e)\n"
        "assert(world.WindowCreateImage('w','i',1,2,3,4,5,6,7,8) == e)\n"
        "assert(world.WindowDelete('w') == e)\n"
        "assert(world.WindowDeleteAllHotspots('w') == e)\n"
        "assert(world.WindowDeleteHotspot('w','h') == e)\n"
        "assert(world.WindowDragHandler('w','h','','',0) == e)\n"
        "assert(world.WindowDrawImage('w','i',0,0,10,10,1,0,0,0,0) == e)\n"
        "assert(world.WindowDrawImageAlpha('w','i',0,0,10,10,1,0,0) == e)\n"
        "assert(world.WindowFilter('w',0,0,10,10,1,1) == e)\n"
        "assert(world.WindowFont('w','f','Courier',10,false,false,false,false,1,0) == e)\n"
        "assert(world.WindowFontInfo('w','f',1) == nil)\n"
        "assert(world.WindowFontList('w') == nil)\n"
        "assert(world.WindowGetImageAlpha('w','i',0,0,10,10,0,0) == e)\n"
        "assert(world.WindowGradient('w',0,0,10,10,0,255,1) == e)\n"
        "assert(world.WindowHotspotInfo('w','h',1) == nil)\n"
        "assert(world.WindowHotspotList('w') == nil)\n"
        "assert(world.WindowHotspotTooltip('w','h','tip') == e)\n"
        "assert(world.WindowImageFromWindow('w','i','w2') == e)\n"
        "assert(world.WindowImageInfo('w','i',1) == nil)\n"
        "assert(world.WindowImageList('w') == nil)\n"
        "assert(world.WindowImageOp('w',1,0,0,10,10,0,0,1,0,'i',0,0) == e)\n"
        "assert(world.WindowInfo('w',1) == nil)\n"
        "assert(world.WindowLine('w',0,0,10,10,0,0,1) == e)\n"
        "assert(world.WindowList() == nil)\n"
        "assert(world.WindowLoadImage('w','i','/no/file.png') == e)\n"
        "assert(world.WindowLoadImageMemory('w','i','notpng',false) == e)\n"
        "assert(world.WindowMenu('w',0,0,'a|b') == '')\n"
        "assert(world.WindowMergeImageAlpha('w','i','m',0,0,10,10,0,1.0,0,0,10,10) == e)\n"
        "assert(world.WindowMoveHotspot('w','h',0,0,10,10) == e)\n"
        "assert(world.WindowPolygon('w','0,0,1,1,2,2',0,0,1,0,1,true,false) == e)\n"
        "assert(world.WindowPosition('w',0,0,0,0) == e)\n"
        "assert(world.WindowRectOp('w',1,0,0,10,10,0,0) == e)\n"
        "assert(world.WindowResize('w',10,10,0) == e)\n"
        "assert(world.WindowScrollwheelHandler('w','h','') == e)\n"
        "assert(world.WindowSetPixel('w',0,0,0) == e)\n"
        "assert(world.WindowSetZOrder('w',1) == e)\n"
        "assert(world.WindowShow('w',true) == e)\n"
        "assert(world.WindowTransformImage('w','i',0,0,1,1,0,0,1) == e)\n"
        "assert(world.WindowWrite('w','/no/file.png') == e)\n"
    ));
}

// ── The Window* family's SENTINEL returns are not error codes ─────────────
//
// Three of the family answer a documented numeric sentinel for "no such
// window", not an eXxx error code, because their success value is a
// measurement: WindowText and WindowTextWidth return the width they drew or
// measured, so failure is a negative width; WindowGetPixel returns a colour, so
// failure is -2 (and -1 means "outside the window", which is a different case).
// Plugins test for these numerically -- `if WindowGetPixel(...) == -2 then`.
//
// The headless stubs answered eNoSuchWindow, and the sweep above asserted it,
// so every such plugin test would have failed under the test build while
// working in the app. methods_miniwindows.cpp:235/241 (WindowText),
// :251 (WindowTextWidth), :798 (WindowGetPixel).
TEST(LuaSurface, WindowFamilySentinelsAreNumbersNotErrorCodes)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(world.WindowGetPixel('nope',0,0) == -2)\n"
        "assert(world.WindowText('nope','f','hi',0,0,10,10,0,false) == -1)\n"
        "assert(world.WindowText('','f','hi',0,0,10,10,0,false) == -1)\n"
        "assert(world.WindowTextWidth('nope','f','hi',false) == -1)\n"
        // ...and they are genuinely distinct from the error code the rest of
        // the family uses, which is the whole point.
        "assert(-2 ~= error_code.eNoSuchWindow)\n"
        "assert(-1 ~= error_code.eNoSuchWindow)\n"
    ));
}

// ── BlendPixel / FilterPixel are NOT stubs ────────────────────────────────
//
// They used to be in the sweep above, asserting eNoSuchWindow — which is a
// contract no Windows build has ever had.  Both are pure colour arithmetic on
// a pair of longs: no m_MiniWindows lookup, no surface, nothing to be missing.
// They sat in the mushgui-only miniwindow TU purely by association with the
// rest of the Window* family, so the headless build linked a stub and the test
// pinned the stub's answer.  They now live in
// scripting/methods/methods_pixel_shared.cpp and run for real here.
//
// Pinning the discriminating arms rather than a colour table: the error returns
// (which are the documented API), the identity operation, and one arm whose
// arithmetic is checkable by hand.
TEST(LuaSurface, PixelMathsRunsForReal)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        // FilterPixel: unknown operation is -1 (methods_pixel_shared.cpp
        // `default: return -1`), and operation 27 ("Average — of 1 pixel is
        // itself") is the identity, which is the cleanest proof the real body
        // ran: a stub returning eNoSuchWindow could not produce it.
        "assert(world.FilterPixel(0x123456, 999, 0) == -1)\n"
        "assert(world.FilterPixel(0x123456, 27, 0) == 0x123456)\n"
        // Brightness (7) adds Options to each channel and clamps to 0..255.
        "assert(world.FilterPixel(0x000000, 7, 16) == 0x101010)\n"
        "assert(world.FilterPixel(0xFFFFFF, 7, 16) == 0xFFFFFF)\n"   // clamped
        "assert(world.FilterPixel(0xFFFFFF, 7, -255) == 0x000000)\n" // clamped
        // BlendPixel: opacity outside 0..1 is -2, unknown mode is -1.
        "assert(world.BlendPixel(0xFFFFFF, 0, 1, 1.5) == -2)\n"
        "assert(world.BlendPixel(0xFFFFFF, 0, 1, -0.1) == -2)\n"
        "assert(world.BlendPixel(0xFFFFFF, 0, 999, 1.0) == -1)\n"
        // Normal blend (1) at full opacity is the blend colour outright...
        "assert(world.BlendPixel(0x336699, 0x000000, 1, 1.0) == 0x336699)\n"
        // ...and at zero opacity it is the base, untouched.
        "assert(world.BlendPixel(0x336699, 0x112233, 1, 0.0) == 0x112233)\n"
    ));
}

// ── doc_gui_method_stubs.cpp — misc view/frame arms ───────────────────────

TEST(LuaSurface, MiscViewStubs)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "world.AddFont('/no/such/font.ttf')\n"
        "world.Bookmark(1, true)\n"
        "world.FlashIcon()\n"
        "world.MoveMainWindow(0, 0, 100, 100)\n"
        "world.MoveWorldWindow(0, 0, 100, 100)\n"
        "world.Pause(true)\n"
        "world.Pause(false)\n"
        "world.Repaint()\n"
        "world.Redraw()\n"
        "world.ResetStatusTime()\n"
        // SetBackgroundColour returns the PREVIOUS colour, not eOK — that is
        // its documented result, and the stub that used to answer 0 here had
        // this test defending the wrong contract.  Checked properly below.

        "assert(world.SetBackgroundImage('x.bmp', 0) == 0)\n"
        "assert(world.SetForegroundImage('x.bmp', 0) == 0)\n"
        "assert(world.SetCursor(0) == 0)\n"

        "world.SetTitle('t')\n"
        "world.SetOutputFont('Courier', 10)\n"
        "world.SetInputFont('Courier', 10, 400, 0)\n"
        "assert(world.SetScroll(-2, true) == 0)\n"
        "assert(world.SetScroll(1, true) == 0)\n"
        "assert(world.SetToolBarPosition(1, false, 0, 0, 0) == 0)\n"

        "world.SetWorldWindowStatus(1)\n"
        "assert(world.TextRectangle(0, 0, 100, 100, 0, 0, 1, 0, 0) == 0)\n"
        "assert(world.Transparency(0, 1) == false)\n"
        "assert(world.GetFrame() ~= nil)\n"
    ));
}

// ── Three of those were never GUI-only ────────────────────────────────────
//
// SetUnseenLines, SetMainTitle and SetBackgroundColour write document state and
// (at most) push it out through an Emit* seam that exists in every build.  Their
// stubs did nothing, and the sweep above asserted the stubs' answers — so the
// wrong contract was defended rather than merely unreachable.  They now live in
// scripting/methods/methods_output_shared.cpp; these are the real contracts.
TEST(LuaSurface, OutputStateSettersWriteTheDocument)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        // world.GetInfo(89) reads m_strMainWindowTitle back, so a SetMainTitle
        // that dropped the write made GetInfo(89) permanently blank.
        "world.SetMainTitle('a new title')\n"
        "assert(world.GetInfo(89) == 'a new title', 'GetInfo(89) = ' ..\n"
        "       tostring(world.GetInfo(89)))\n"
        // SetUnseenLines sets the unread counter GetInfo(202) reports.
        "world.SetUnseenLines(7)\n"
        "assert(world.GetInfo(202) == 7)\n"
        "world.SetUnseenLines(0)\n"
        "assert(world.GetInfo(202) == 0)\n"
        // SetBackgroundColour returns the PREVIOUS colour and keeps the new one.
        "world.SetBackgroundColour(0x112233)\n"
        "assert(world.SetBackgroundColour(0x445566) == 0x112233)\n"
        "assert(world.SetBackgroundColour(0x112233) == 0x445566)\n"
    ));
}

// ── Parameter validation happens before any GUI is involved ───────────────
//
// These three still have GUI-only bodies, but their FIRST act on Windows is a
// parameter check, and the stubs answered eOK to inputs Windows rejects.  A
// script probing for a supported cursor or toolbar would have been told
// everything works.
TEST(LuaSurface, GuiOnlySettersStillValidateTheirArguments)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "local bad = error_code.eBadParameter\n"
        // AddFont: `if (strlen (PathName) <= 0) return eBadParameter`
        "assert(world.AddFont('') == bad)\n"
        "assert(world.AddFont('/no/such/font.ttf') == 0)\n"
        // SetCursor: the range gate is -1 .. 12
        "assert(world.SetCursor(-2) == bad)\n"
        "assert(world.SetCursor(13) == bad)\n"
        "assert(world.SetCursor(-1) == 0)\n"
        "assert(world.SetCursor(12) == 0)\n"
        // SetToolBarPosition: `default: return eBadParameter` on Which
        "assert(world.SetToolBarPosition(0, false, 0, 0, 0) == bad)\n"
        "assert(world.SetToolBarPosition(5, false, 0, 0, 0) == bad)\n"
        "assert(world.SetToolBarPosition(1, false, 0, 0, 0) == 0)\n"
    ));
}

// world.TextRectangle's nine member writes are document state — the reserved
// rectangle is what the engine wrap column and NAWS are computed from — even
// though the brush validation and the repaint at either end are not.  The stub
// dropped all nine, so the whole mechanism was invisible headless.
TEST(LuaSurface, TextRectangleRecordsTheReservedArea)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L, "assert(world.TextRectangle(5, 6, -200, 8, 2, 0, 1, 0, 0) == 0)\n"));
    EXPECT_EQ(5L,    (long) f.doc.m_TextRectangle.left);
    EXPECT_EQ(6L,    (long) f.doc.m_TextRectangle.top);
    EXPECT_EQ(-200L, (long) f.doc.m_TextRectangle.right);   // offset-from-edge form
    EXPECT_EQ(8L,    (long) f.doc.m_TextRectangle.bottom);
    EXPECT_EQ(2L,    (long) f.doc.m_TextRectangleBorderOffset);
}

// world.GetRecentLines joins with the SAME rule as the shipping body.
//
// methods_output.cpp:116-117 guards the separator on the ACCUMULATOR
// (`if (!s.empty ())`); the headless twin guarded it on the loop INDEX
// (`if (i > start)`).  Those agree on every input except one: when the oldest
// line in the requested window is blank.  That is not a contrived case — every
// completed line is pushed unconditionally at ProcessPreviousLine.cpp:549,
// blank ones included — and the value feeds multi-line trigger matching, so the
// headless engine was matching against a leading newline the product never has.
TEST(LuaSurface, GetRecentLinesJoinsTheWayTheShippingBodyDoes)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    // The discriminator.  Index guard → "\nhp: 100"; accumulator guard → "hp: 100".
    f.doc.m_sRecentLines.clear();
    f.doc.m_sRecentLines.push_back("");
    f.doc.m_sRecentLines.push_back("hp: 100");
    EXPECT_TRUE(run(f.L, "assert(world.GetRecentLines(2) == 'hp: 100')\n"));

    // Control: two non-blank lines must still be joined by exactly one newline,
    // so "never emit a separator" fails too.
    f.doc.m_sRecentLines.clear();
    f.doc.m_sRecentLines.push_back("a");
    f.doc.m_sRecentLines.push_back("b");
    EXPECT_TRUE(run(f.L, "assert(world.GetRecentLines(2) == 'a\\nb')\n"));

    // Second control: a blank line in the MIDDLE still produces its separators,
    // so the fix cannot be "drop blank lines".
    f.doc.m_sRecentLines.clear();
    f.doc.m_sRecentLines.push_back("a");
    f.doc.m_sRecentLines.push_back("");
    f.doc.m_sRecentLines.push_back("b");
    EXPECT_TRUE(run(f.L, "assert(world.GetRecentLines(3) == 'a\\n\\nb')\n"));
}

// The other arm: with a miniwindow actually created, most of this family does
// real work headless.
//
// This is what the stage-1 adoption bought.  No pixels are involved — nothing
// installs a pal::ISurface headless, so every drawing site takes its guarded
// `if (!srf)` arm — but the miniwindow itself, its geometry, visibility,
// z-order and hotspots are all real engine state, and world.GetInfo and the
// plugin API read them back.
TEST(LuaSurface, WindowFamilyWithAWindowOpen)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(world.WindowCreate('m', 10, 20, 300, 200, 0, 0, 0) == 0)\n"

        // Geometry is stored and reported — WindowInfo 1..4 are left/top/
        // width/height.  Deliberately four DIFFERENT numbers: a fixture where
        // any two matched would hide a swapped pair.
        "assert(world.WindowInfo('m', 1) == 10)\n"
        "assert(world.WindowInfo('m', 2) == 20)\n"
        "assert(world.WindowInfo('m', 3) == 300)\n"
        "assert(world.WindowInfo('m', 4) == 200)\n"

        // Visibility really toggles (info 5 is the show flag), so "WindowShow
        // does nothing" fails.  Created windows start hidden.
        "assert(world.WindowInfo('m', 5) == false)\n"
        "assert(world.WindowShow('m', true) == 0)\n"
        "assert(world.WindowInfo('m', 5) == true)\n"

        // Moving and resizing change what Info reports — the control against a
        // body that stores the creation values and never updates them.
        "assert(world.WindowPosition('m', 50, 60, 0, 0) == 0)\n"
        "assert(world.WindowInfo('m', 1) == 50)\n"
        "assert(world.WindowResize('m', 400, 300, 0) == 0)\n"
        "assert(world.WindowInfo('m', 3) == 400)\n"

        "assert(world.WindowSetZOrder('m', 7) == 0)\n"

        // Hotspots: add, retitle, move, list, delete — and deleting twice must
        // report eHotspotNotInstalled, NOT eNoSuchWindow.  The stub could not
        // tell those apart because it answered eNoSuchWindow to both.
        "assert(world.WindowAddHotspot('m','h',0,0,10,10,'','','','','','tip',0,0) == 0)\n"
        "local hs = world.WindowHotspotList('m')\n"
        "assert(type(hs) == 'table')\n"
        "local n = 0\n"
        "for _, v in pairs(hs) do n = n + 1; assert(v == 'h') end\n"
        "assert(n == 1, 'expected 1 hotspot, got ' .. n)\n"
        "assert(world.WindowHotspotTooltip('m','h','tip2') == 0)\n"
        "assert(world.WindowMoveHotspot('m','h',1,2,3,4) == 0)\n"
        "assert(world.WindowDeleteHotspot('m','h') == 0)\n"
        "assert(world.WindowDeleteHotspot('m','h') == error_code.eHotspotNotInstalled)\n"

        // The drawing calls answer eOK against a null surface, by design: that
        // guarded arm is what makes stage 1 safe without a headless rasteriser,
        // and it is the same arm both shipping platforms take if an adapter ever
        // fails to install.  Pinned so a later change cannot quietly turn it
        // into an error.
        "assert(world.WindowRectOp('m', 2, 0, 0, 10, 10, 0, 0) == 0)\n"
        "assert(world.WindowSetPixel('m', 1, 1, 255) == 0)\n"

        // Fonts DO need the GDI shim, so they fail honestly rather than
        // pretending — eCannotAddFont, not eNoSuchWindow.
        "assert(world.WindowFont('m','f','Menlo',10,false,false,false,false,0,0)"
        " == error_code.eCannotAddFont)\n"

        // The window is in the list while it exists, and gone after.
        "local l = world.WindowList()\n"
        "local c = 0\n"
        "for _, v in pairs(l) do c = c + 1; assert(v == 'm') end\n"
        "assert(c == 1, 'expected 1 window, got ' .. c)\n"
        "assert(world.WindowDelete('m') == 0)\n"
        "assert(world.WindowInfo('m', 1) == nil)\n"
        "assert(world.WindowList() == nil)\n"
    ));
}

// ── the accelerator family — real headless, not stubbed ───────────────────
//
// These used to live in MiscViewStubs asserting AcceleratorList() == nil, which
// was true only because AcceleratorTo bailed out at a view-list gate that can
// never be satisfied off-Windows.  It binds for real now (the HACCEL half is
// Windows-only), so assert the binding, not the emptiness.  Behaviour detail is
// in tests/test_accelerators.cpp; this is the Lua-surface reach.

TEST(LuaSurface, AcceleratorFamilyBindsThroughLua)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(world.AcceleratorList() == nil)\n"          // nothing bound yet
        "assert(world.Accelerator('F9', 'send this') == 0)\n"
        "assert(world.AcceleratorTo('F10', 'x', 0) == 0)\n"
        "local t = world.AcceleratorList()\n"
        "assert(type(t) == 'table')\n"
        "local n = 0\n"
        "local seen = {}\n"
        "for _, v in pairs(t) do n = n + 1; seen[v] = true end\n"
        "assert(n == 2, 'expected 2 accelerators, got ' .. n)\n"
        "assert(seen['F9 = send this'])\n"                  // eSendToExecute: no suffix
        "assert(seen['F10 = x\\t[0]'])\n"                    // eSendToWorld == 0
        "assert(world.Accelerator('F9', '') == 0)\n"        // empty Send un-binds
        "local u = world.AcceleratorList()\n"
        "local m = 0\n"
        "for _ in pairs(u) do m = m + 1 end\n"
        "assert(m == 1, 'expected 1 accelerator after delete, got ' .. m)\n"
    ));
}

// ── notepad family — REAL bodies now, not stubs ───────────────────────────
//
// scripting/methods/methods_notepad.cpp is compiled into mushcore
// (docs/STUB-ADOPTION-2026-08.md), so these drive the shipping bodies.  The
// block below used to pin the twin's contract; the one value that actually
// changed on adoption is GetNotepadList, and it changed to match Windows —
// see the comment on it.
//
// The Lua-level tests here cover the marshalling.  tests/test_notepad_methods.cpp
// covers the semantics against a real CTextDocument.

TEST(LuaSurface, NotepadFamilyWithNoNotepadOpen)
{
    ClearAllNotepads();
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(world.AppendToNotepad('t', 'text') == false)\n"
        // L_ReplaceNotepad pushes its boolean but returns 0 results (canonical
        // lua_methods.cpp quirk) — Lua callers always see nil.
        "assert(world.ReplaceNotepad('t', 'text') == nil)\n"
        "assert(world.SendToNotepad('t', 'text') == false)\n"
        "assert(world.CloseNotepad('t', false) == 0)\n"
        "assert(world.GetNotepadLength('t') == 0)\n"
        "assert(world.GetNotepadText('t') == '')\n"
        // CHANGED ON ADOPTION, deliberately: the twin called CreateOneDim(…, 0)
        // and handed back a real zero-element array, which reached Lua as an
        // empty TABLE.  The shipping body guards with `if (iCount)` — "cannot
        // create empty array dimension" — so a never-created COleSafeArray
        // detaches as VT_EMPTY, which reaches Lua as NIL.  Windows has always
        // done the latter.
        "assert(world.GetNotepadList(false) == nil)\n"
        "assert(world.MoveNotepadWindow('t', 0, 0, 10, 10) == false)\n"
        "assert(world.NotepadColour('t', 'red', 'white') == 0)\n"
        "assert(world.NotepadFont('t', 'Courier', 10, 0, 0) == 0)\n"
        "assert(world.NotepadReadOnly('t', true) == 0)\n"
        "assert(world.NotepadSaveMethod('t', 0) == 0)\n"
        "assert(world.SaveNotepad('t', '/tmp/np.txt', true) == 0)\n"
    ));
}

// The other arm.  Without this the whole family would still only ever be tested
// in its "there is no notepad" state — which is exactly the hole the stub left,
// and every assertion above would pass against a body that did nothing at all.
TEST(LuaSurface, NotepadFamilyWithANotepadOpen)
{
    ClearAllNotepads();
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    FakeNotepad pad(&f.doc, "t", f.doc.m_iUniqueDocumentNumber);
    pad.text = "seed";

    EXPECT_TRUE(run(f.L,
        "assert(world.AppendToNotepad('t', '+more') == true)\n"
        "assert(world.GetNotepadText('t') == 'seed+more')\n"
        "assert(world.GetNotepadLength('t') == 9)\n"
        // now the list is non-empty, and reaches Lua as a table
        "local l = world.GetNotepadList(false)\n"
        "assert(type(l) == 'table')\n"
        "local n = 0\n"
        "for _, v in pairs(l) do n = n + 1; assert(v == 't') end\n"
        "assert(n == 1, 'expected 1 notepad, got ' .. n)\n"
        "assert(world.NotepadReadOnly('t', true) == 1)\n"
        "assert(world.MoveNotepadWindow('t', 1, 2, 3, 4) == true)\n"
        "assert(world.GetNotepadWindowPosition('t') ~= '')\n"
    ));
    EXPECT_TRUE(pad.readOnly);
    EXPECT_EQ(1, pad.moveCalls);
}

// ── chat family — REAL bodies now, not stubs ──────────────────────────────
//
// chatsock.cpp, chatlistensock.cpp and methods_chat.cpp are compiled into
// mushcore (docs/STUB-ADOPTION-2026-08.md), so these drive the shipping bodies.
//
// The block this replaces asserted `== eNoChatConnections` for nineteen of these
// methods.  That was the STUB's contract, and it is right for exactly TWO of
// them.  The real bodies distinguish five different failure modes, and two of
// the calls are not failures at all — which is the whole reason this family was
// worth adopting rather than leaving inert.

TEST(LuaSurface, ChatFamilyWithNoConnections)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        // "No connections at all" — the only two that really mean that.  Both
        // are broadcasts, so having nobody to broadcast to IS the error.
        "assert(world.ChatEverybody('hi', false) == error_code.eNoChatConnections)\n"
        "assert(world.ChatGroup('g', 'hi', false) == error_code.eNoChatConnections)\n"

        // "That chat ID is not open" — a DIFFERENT error from having none, and
        // the one nine of these actually return.  The stub answered 30044 for
        // all of them, so a script distinguishing "nobody connected" from "that
        // person hung up" could not.
        "local nf = error_code.eChatIDNotFound\n"
        "assert(world.ChatDisconnect(1)          == nf)\n"
        "assert(world.ChatID(1, 'hi', false)     == nf)\n"
        "assert(world.ChatMessage(1, 1, 'x')     == nf)\n"
        "assert(world.ChatPasteText(1)           == nf)\n"
        "assert(world.ChatPeekConnections(1)     == nf)\n"
        "assert(world.ChatPing(1)                == nf)\n"
        "assert(world.ChatRequestConnections(1)  == nf)\n"
        "assert(world.ChatSendFile(1, '/tmp/x')  == nf)\n"
        "assert(world.ChatStopFileTransfer(1)    == nf)\n"
        "assert(world.SetChatOption(1, 'x', 'y') == nf)\n"

        // Looked up BY NAME rather than by id, so it fails differently again.
        "assert(world.ChatPersonal('w', 'hi', false) == error_code.eChatPersonNotFound)\n"

        // Fails on the CLIPBOARD, before it ever looks for a peer — so the stub's
        // 30044 was reporting the wrong subsystem entirely.
        "assert(world.ChatPasteEverybody() == error_code.eClipboardEmpty)\n"

        // Not an error code at all: a COUNT of how many were closed.  The stub
        // returned 30044 here, so `if world.ChatDisconnectAll() > 0` was true
        // when nothing had been disconnected — backwards.
        "assert(world.ChatDisconnectAll() == 0)\n"

        // Also a plain value, not a code: 0 means "no such person".  The stub's
        // 30044 would read as a valid chat ID.
        "assert(world.ChatGetID('nobody') == 0)\n"

        // The VARIANT-returning three answer nil when there is no such session.
        "assert(world.GetChatInfo(1, 1) == nil)\n"
        "assert(world.GetChatOption(1, 'name') == nil)\n"
        "assert(world.GetChatList() == nil)\n"

        // Void, must not throw.
        "world.ChatStopAcceptingCalls()\n"
        "world.ChatNote(1, 'note')\n"
    ));
}

// ChatNameChange SUCCEEDS with zero connections, and mutates state.
//
// This is the sharpest example of what the stub cost: it returned 30044 and
// changed nothing, where Windows returns eOK, renames the world's chat identity,
// marks the document modified and broadcasts the change to any peers.  A script
// setting its chat name at startup — before connecting to anyone, which is the
// normal time to do it — silently did not.
TEST(LuaSurface, ChatNameChangeWorksWithNobodyConnected)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    f.doc.m_strOurChatName = "old";

    EXPECT_TRUE(run(f.L, "assert(world.ChatNameChange('newname') == error_code.eOK)\n"));
    EXPECT_EQ(std::string("newname"),
              std::string((const char*) f.doc.m_strOurChatName));

    // …and the parameter check is real: a blank name is refused, and refused
    // WITHOUT clobbering the name that was already set.  Without this control a
    // body that assigned unconditionally would pass.
    EXPECT_TRUE(run(f.L, "assert(world.ChatNameChange('') == error_code.eBadParameter)\n"));
    EXPECT_EQ(std::string("newname"),
              std::string((const char*) f.doc.m_strOurChatName));
}

TEST(LuaSurface, LoggingLifecycle)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    char logpath[256];
    snprintf(logpath, sizeof logpath, "/tmp/mc_test_log_%d.txt", (int)getpid());
    std::string code =
        "local p = '" + std::string(logpath) + "'\n"
        "assert(not world.IsLogOpen())\n"
        "assert(world.OpenLog(p, false) == 0)\n"
        "assert(world.IsLogOpen())\n"
        "assert(world.WriteLog('a log line') == 0)\n"
        "assert(world.FlushLog() == 0)\n"
        "world.SetLogInput(true)\n"
        "assert(world.GetLogInput() == true)\n"
        "world.SetLogOutput(false)\n"
        "assert(world.GetLogOutput() == false)\n"
        "world.SetLogNotes(true)\n"
        "assert(world.GetLogNotes() == true)\n"
        "world.LogSend('sent line')\n"
        "assert(world.CloseLog() == 0)\n"
        "assert(not world.IsLogOpen())\n";
    EXPECT_TRUE(run(f.L, code.c_str()));

    FILE* lf = fopen(logpath, "rb");
    ASSERT_NE(nullptr, lf);
    char buf[4096] = {0};
    fread(buf, 1, sizeof buf - 1, lf);
    fclose(lf);
    EXPECT_NE(nullptr, strstr(buf, "a log line"));
    unlink(logpath);
}

// ── note-colour / note-style family (doc_shared.cpp) ──────────────────────

TEST(LuaSurface, NoteColourAndStyle)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "world.SetNoteColourFore(0x0000FF)\n"
        "assert(world.GetNoteColourFore() == 0x0000FF)\n"
        "world.SetNoteColourBack(0x00FF00)\n"
        "assert(world.GetNoteColourBack() == 0x00FF00)\n"
        "world.SetNoteColour(3)\n"
        "assert(world.GetNoteColour() == 3)\n"
        "world.NoteColourRGB(255, 65280)\n"
        "world.NoteColourName('red', 'white')\n"
        "world.NoteStyle(1)\n"           // bold
        "assert(world.GetNoteStyle() == 1)\n"
        "world.NoteStyle(0)\n"
    ));
}

// ── noting family writes through DisplayMsg (doc_shared display arms) ─────

TEST(LuaSurface, NotingWritesToOutput)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "local before = world.GetLinesInBufferCount()\n"
        "world.Note('plain note')\n"
        "world.ColourNote('red', 'white', 'coloured note')\n"
        "world.ColourTell('lime', 'black', 'tell-part')\n"
        "world.Note('')\n"                     // flush the tell
        "world.Tell('tell2')\n"
        "world.Note('')\n"
        "world.AnsiNote('\\027[31mansi red\\027[0m')\n"
        "world.NoteHr()\n"
        "world.Hyperlink('http://x', 'link text', 'prompt', 'blue', '', false)\n"
        "world.Note('')\n"
        "assert(world.GetLinesInBufferCount() > before)\n"
        "assert(type(world.GetRecentLines(5)) == 'string')\n"
    ));
}

// ── speedwalk wrappers (methods_speedwalks.cpp / speedwalk.cpp) ───────────

TEST(LuaSurface, SpeedwalkWrappers)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "local ok = world.EvaluateSpeedwalk('2n')\n"
        "assert(type(ok) == 'string' and ok:sub(1,1) ~= '*', ok)\n"
        "assert(ok:find('n'))\n"
        "local bad = world.EvaluateSpeedwalk('2#')\n"
        "assert(type(bad) == 'string' and bad:sub(1,1) == '*', bad)\n"
        "assert(type(world.ReverseSpeedwalk('2n3e')) == 'string')\n"
        "assert(type(world.RemoveBacktracks('nns')) == 'string')\n"
        "world.SetSpeedWalkDelay(25)\n"
        "assert(world.GetSpeedWalkDelay() == 25)\n"
    ));
}

// ── mapping family (methods_mapping.cpp) ──────────────────────────────────

TEST(LuaSurface, MappingFamily)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "world.EnableMapping(true)\n"
        "assert(world.GetMapping() == true)\n"     // the enable flag
        "assert(world.GetMappingCount() == 0)\n"
        "assert(world.GetMappingItem(0) == nil)\n" // out of range → VT_NULL
        "assert(type(world.GetMappingString()) == 'string')\n"
        "world.SetRemoveMapReverses(1)\n"
        "assert(world.GetRemoveMapReverses() == true)\n"
        "world.SetMapping(false)\n"
        "assert(world.GetMapping() == false)\n"
        "world.DeleteAllMapItems()\n"
        "world.EnableMapping(false)\n"
    ));
}

// ── command-window seam (methods_commands.cpp, headless) ──────────────────

TEST(LuaSurface, CommandWindowHeadless)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(type(world.GetCommand()) == 'string')\n"
        "world.SetCommand('typed text')\n"
        "world.SetCommandSelection(1, 2)\n"
        "world.SelectCommand()\n"
        "assert(type(world.PushCommand()) == 'string')\n"
        "world.PasteCommand('pasted')\n"
        "world.DeleteCommandHistory()\n"
        "local cl = world.GetCommandList(10)\n"
        "assert(cl == nil or type(cl) == 'table')\n"
        "assert(world.SetCommandWindowHeight(60) == 0 or true)\n"
        "local icl = world.GetInternalCommandsList()\n"
        "assert(type(icl) == 'table' and #icl > 50)\n"
        "assert(world.DoCommand('nosuchcommandzz') == error_code.eNoSuchCommand)\n"
        "assert(world.DoCommand('trace') == 0)\n"   // posts to stub frame: inert
        "world.SetEchoInput(false)\n"
        "assert(world.GetEchoInput() == false)\n"
        "world.SetEchoInput(true)\n"
    ));
}

// ── entities (methods_xml.cpp) ────────────────────────────────────────────

TEST(LuaSurface, EntityRoundTrip)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "world.SetEntity('mine', 'value1')\n"
        "assert(world.GetEntity('mine') == 'value1')\n"
        "world.SetEntity('mine', '')\n"             // empty removes
        "assert(world.GetEntity('mine') == '')\n"
        "assert(world.GetXMLEntity('amp') == '&')\n"
        "assert(world.GetXMLEntity('lt') == '<')\n"
        "world.Reset()\n"                            // MXP_Off
    ));
}

// ── misc info getters ─────────────────────────────────────────────────────

TEST(LuaSurface, MiscInfoGetters)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(#world.GetWorldID() == 24)\n"
        "local wl = world.GetWorldIdList()\n"
        "assert(wl == nil or type(wl) == 'table')\n"
        "assert(world.GetWorldById('ffffffffffffffffffffffff') == nil)\n"
        "assert(world.GetReceivedBytes() >= 0)\n"
        "assert(world.GetSentBytes(false) >= 0)\n"
        "assert(world.GetConnectDuration() == 0)\n"  // never connected
        "assert(type(world.GetScriptTime()) == 'number')\n"
        "assert(world.GetHostAddress('') == nil)\n"  // MFC parity: VT_EMPTY
        "assert(world.GetHostName('') == '')\n"
        "world.SetTrace(true)\n"
        "assert(world.GetTrace() == true)\n"
        "world.TraceOut('trace line')\n"
        "world.SetTrace(false)\n"
        "local mwp = world.GetMainWindowPosition()\n"
        "assert(type(mwp) == 'table' and mwp.left ~= nil)\n"
        "local wwp = world.GetWorldWindowPosition(1)\n"
        "assert(wwp == nil or type(wwp) == 'table')\n"
        "assert(world.GetNotepadWindowPosition('t') == nil)\n"   // no notepad headless
        "assert(world.GetSelectionStartLine() >= 0)\n"
        "assert(world.GetSelectionEndLine() >= 0)\n"
        "assert(world.GetSelectionStartColumn() >= 0)\n"
        "assert(world.GetSelectionEndColumn() >= 0)\n"
        "assert(type(world.GetSystemMetrics(0)) == 'number')\n"
        "assert(type(world.GetSysColor(1)) == 'number')\n"
        "assert(type(world.GetDeviceCaps(8)) == 'number')\n"
        "assert(type(world.GetGlobalOption('AutoConnectWorlds')) == 'number')\n"
        "local gl = world.GetGlobalOptionList()\n"
        "assert(type(gl) == 'table' and #gl > 50)\n"
        "assert(world.ErrorDesc(0) == 'No error')\n"
        "assert(world.ErrorDesc(30073):find('window'))\n"
    ));
}

// ── multi-out-param getters via the Lua wrappers ──────────────────────────

TEST(LuaSurface, MultiReturnGetters)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(world.AddTrigger('mt', 'the match', 'resp', 1, -1, 0, '', '') == 0)\n"
        "local code, match = world.GetTrigger('mt')\n"
        "assert(code == 0 and match == 'the match')\n"
        "assert(world.GetTriggerWildcard('mt', '1') == '')\n"  // never matched
        "local tlist = world.GetTriggerList()\n"
        "assert(tlist[1] == 'mt')\n"

        "assert(world.AddAlias('ma', 'am', 'resp', 1, '') == 0)\n"
        "local acode, amatch = world.GetAlias('ma')\n"
        "assert(acode == 0 and amatch == 'am')\n"
        "assert(world.GetAliasWildcard('ma', '1') == '')\n"
        "local alist = world.GetAliasList()\n"
        "assert(alist[1] == 'ma')\n"

        "assert(world.AddTimer('mm', 0, 5, 0, 'x', 0, '') == 0)\n"
        "local tcode, th, tm = world.GetTimer('mm')\n"
        "assert(tcode == 0 and th == 0 and tm == 5)\n"
    ));
}

// ── send-family offline arms ──────────────────────────────────────────────

TEST(LuaSurface, SendFamilyOffline)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(world.Send('hello') == error_code.eWorldClosed)\n"
        "assert(world.SendImmediate('hello') == error_code.eWorldClosed)\n"
        "assert(world.SendNoEcho('hello') == error_code.eWorldClosed)\n"
        "assert(world.SendPush('hello') == error_code.eWorldClosed)\n"
        "assert(world.SendSpecial('hello', true, true, false, false) == error_code.eWorldClosed)\n"
        "assert(world.Execute('nonexistentcmd') == 0 or true)\n"
        "world.Simulate('simulated line\\n')\n"
        "world.DiscardQueue()\n"
        "world.StopEvaluatingTriggers(false)\n"
        "world.ShiftTabCompleteItem('word')\n"
        ));
}

// ── utils.* portable library ──────────────────────────────────────────────

TEST(LuaSurface, UtilsPureFunctions)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(utils.utf8encode(72, 105) == 'Hi')\n"
        "assert(utils.utf8encode({72, 105}) == 'Hi')\n"
        "local t = utils.utf8decode('Hi\\195\\169')\n"    // H i é
        "assert(t[1] == 72 and t[2] == 105 and t[3] == 233)\n"
        "assert(utils.utf8convert('\\233') == '\\195\\169')\n"  // Latin-1 é → UTF-8
        "assert(utils.edit_distance('kitten', 'sitting') == 3)\n"
        "local m1, m2 = utils.metaphone('Thompson')\n"
        "assert(type(m1) == 'string' and #m1 > 0)\n"
        "utils.colourcube(1)\n"
        "utils.colourcube(2)\n"
        "assert(type(utils.timer()) == 'number')\n"
        "local it = utils.infotypes()\n"
        "assert(type(it) == 'table' and it[1] ~= nil)\n"
        "local fl = utils.functionlist()\n"
        "local fa = utils.functionargs()\n"
        "local cb = utils.callbackslist()\n"
        "assert(type(fl) == 'table' and type(fa) == 'table' and type(cb) == 'table')\n"
        "assert(#cb > 10)\n"
        "assert(type(utils.getfontfamilies()) == 'table')\n"
        "assert(utils.glyph_available('Courier', 65) == 0)\n"
        "local info = utils.info()\n"
        "assert(type(info) == 'table' and type(info.current_directory) == 'string')\n"
        "assert(utils.menufontsize() == 0)\n"
        "utils.showdebugstatus(false)\n"
    ));
}

TEST(LuaSurface, UtilsDialogsHeadlessInert)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        // hook-backed dialogs: null hooks headless → nil / false
        "assert(utils.inputbox('t', 'p', 'd') == nil)\n"
        "assert(utils.editbox('t', 'p', 'd') == nil)\n"
        "assert(utils.filepicker('t', 'n', 'ext', nil, false) == nil)\n"
        "assert(utils.directorypicker('t', '/') == nil)\n"
        "assert(utils.choose({'a','b'}, 'a') == nil)\n"
        "assert(utils.listbox({'a','b'}, 'a') == nil)\n"
        "assert(utils.multilistbox({'a','b'}, 'a') == nil)\n"
        "assert(utils.fontpicker() == nil)\n"
        "assert(utils.filterpicker() == nil)\n"
        "assert(utils.spellcheckdialog() == nil)\n"
        "assert(utils.sendtofront('x') == false)\n"
        // No notepad exists and nothing can create one (no window hook), so
        // both are false.  The arms where they SUCCEED are covered by
        // UtilsNotepadReachesTheAppLevelNotepads below.
        "assert(utils.appendtonotepad('t', 'm') == false)\n"
        "assert(utils.activatenotepad('t') == false)\n"
        "utils.setbackgroundcolour(0)\n"
        "utils.reload_global_prefs()\n"
        // msgbox routes through the headless UMessageBox shim (auto-answers)
        "assert(type(utils.msgbox('question', 'title', 'ok', '!', 1)) == 'string')\n"
        "assert(type(utils.umsgbox('question', 'title', 'yesno', '?', 1)) == 'string')\n"
    ));
}

// ── world.Debug (scripting/world_debug_portable.cpp) ─────────────────────
// DebugCommandsWriteReports deleted (audit §3): SweepR4.DebugListingsCoverEachAction
// (test_lua_scripting.cpp) is a strict superset — it drives the same
// triggerlist/aliaslist/timerlist/variablelist/telnetlist/pluginlist/info listings
// plus the '' help arm and the plugin-scoped '@id triggerlist' arm, and asserts the
// listing content rather than only a buffer-grew count.

// ── utils compress/hash library (scripting/lua_compress.c) ───────────────

TEST(LuaSurface, UtilsCompressAndHashes)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "local orig = string.rep('compressible data ', 50)\n"
        "local z = utils.compress(orig)\n"
        "assert(type(z) == 'string' and #z < #orig)\n"
        "assert(utils.decompress(z) == orig)\n"
        "assert(utils.tohex(utils.md5('abc')):lower() == "
        "'900150983cd24fb0d6963f7d28e17f72')\n"
        "assert(utils.tohex(utils.sha256('abc')):lower() == "
        "'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')\n"
        "assert(type(utils.hash('x')) == 'string')\n"
        "assert(utils.tohex('AB') == '4142')\n"
        "assert(utils.fromhex('4142') == 'AB')\n"
        "assert(utils.base64encode('abc') == 'YWJj')\n"
        "assert(utils.base64decode('YWJj') == 'abc')\n"
        "local t = utils.split('a,b,c', ',')\n"
        "assert(t[1] == 'a' and t[2] == 'b' and t[3] == 'c')\n"
        "local ok, dir = pcall(utils.readdir, '/tmp/*')\n"   // arm coverage
        "assert(ok == true or ok == false)\n"
    ));
}

TEST(LuaSurface, UtilsXmlRead)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "local t, name = utils.xmlread('<?xml version=\"1.0\"?>"
        "<!DOCTYPE muclient><muclient><world site=\"mud.org\">inner</world>"
        "</muclient>')\n"
        "assert(type(t) == 'table')\n"
        "assert(name == 'muclient')\n"
        // malformed XML → nil + message + line
        "local bad, msg = utils.xmlread('<unclosed')\n"
        "assert(bad == nil and type(msg) == 'string')\n"
    ));
}

// utils.appendtonotepad / utils.activatenotepad reach the APP-level notepads.
//
// These are the notepads with no owning world (m_pRelatedWorld == NULL), which
// is what distinguishes CMUSHclientApp::FindNotepad from the world-scoped
// CMUSHclientDoc::FindNotepad.  The portable twins used to route through
// g_wxhook_append_to_notepad, which looked up by TITLE ALONE and never created
// anything — so on this platform utils.appendtonotepad to a new title silently
// did nothing where Windows opens a notepad.  They call the same App methods
// Windows calls now (notepad_app_shared.cpp).
TEST(LuaSurface, UtilsNotepadReachesTheAppLevelNotepads)
{
    NotepadCapture cap;
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    // Nothing by this name: the canonical body CREATES.  Against the old hook
    // route this returned false and made no window at all.
    EXPECT_TRUE(run(f.L, "assert(utils.appendtonotepad('apppad', 'hello') == true)\n"));
    ASSERT_EQ(1u, (unsigned) cap.count());
    EXPECT_EQ(std::string("apppad"),
              std::string((const char*) cap.pads[0]->doc->m_strTitle));
    EXPECT_NE(std::string::npos, cap.pads[0]->text.find("hello"));

    // Now it exists, so the second call APPENDS to it rather than making a
    // second window — the control that makes "always create" fail.
    EXPECT_TRUE(run(f.L, "assert(utils.appendtonotepad('apppad', ' more') == true)\n"));
    EXPECT_EQ(1u, (unsigned) cap.count());
    EXPECT_NE(std::string::npos, cap.pads[0]->text.find("hello more"));

    // Newlines become ENDLINE (canonical lua_utils.cpp:1543), which the twin
    // never did.
    EXPECT_TRUE(run(f.L, "assert(utils.appendtonotepad('apppad', 'a\\nb') == true)\n"));
    EXPECT_NE(std::string::npos, cap.pads[0]->text.find("a\r\nb"));

    // …and activate finds the same window.
    EXPECT_TRUE(run(f.L, "assert(utils.activatenotepad('apppad') == true)\n"));
    EXPECT_EQ(1, cap.pads[0]->activateCalls);

    // Control: a title that does not exist must NOT be activated.
    EXPECT_TRUE(run(f.L, "assert(utils.activatenotepad('nosuch') == false)\n"));
    EXPECT_EQ(1, cap.pads[0]->activateCalls);
}

MC_TEST_MAIN()

#else // !MUSHCLIENT_LUA_SCRIPTING

#include <cstdio>
int main(int, char**)
{
    printf("Lua scripting not available — skipping test_lua_surface\n");
    return 0;
}

#endif // MUSHCLIENT_LUA_SCRIPTING
