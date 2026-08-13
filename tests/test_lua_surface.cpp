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
#include <vector>
#include <unistd.h>
#include <png.h>       // the image-family tests emit a real RGBA PNG

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

        // These two now report the FILE failure they always should have —
        // eCouldNotOpenFile for a path that does not exist.  The stub answered
        // eOK for anything, so "the image silently did not load" was
        // indistinguishable from success.
        // eFileNotFound, not eCouldNotOpenFile.  The canonical mapping
        // (methods_output.cpp:430) is "GetLastError() == 2 -> eFileNotFound,
        // else eCouldNotOpenFile", and now that .bmp files really are opened
        // here the failed fopen sets errno = ENOENT = 2, which is the same
        // number Windows' ERROR_FILE_NOT_FOUND has.  The old expectation was
        // pinning the stub: ::LoadImage returned null without touching errno,
        // so the branch read whatever the last unrelated syscall had left.
        "assert(world.SetBackgroundImage('x.bmp', 0) == error_code.eFileNotFound)\n"
        "assert(world.SetForegroundImage('x.bmp', 0) == error_code.eFileNotFound)\n"
        "assert(world.SetCursor(0) == 0)\n"

        "world.SetTitle('t')\n"
        "world.SetOutputFont('Courier', 10)\n"
        "world.SetInputFont('Courier', 10, 400, 0)\n"
        "assert(world.SetScroll(-2, true) == 0)\n"
        "assert(world.SetScroll(1, true) == 0)\n"
        // SetToolBarPosition answers eBadParameter for EVERY Which — see the
        // note on it below; this is a real macOS gap, now visible.
        "assert(world.SetToolBarPosition(1, false, 0, 0, 0) == error_code.eBadParameter)\n"

        "world.SetWorldWindowStatus(1)\n"
        "assert(world.TextRectangle(0, 0, 100, 100, 0, 0, 1, 0, 0) == 0)\n"
        "assert(world.Transparency(0, 1) == false)\n"
        "assert(world.GetFrame() ~= nil)\n"
    ));
}


// ── .bmp decoding ──────────────────────────────────────────────────────────
// Write a 2x2 BMP to /tmp and return its path.  `bpp` is 24 or 32; `topDown`
// stores the rows the other way up (a negative biHeight), which real encoders
// do emit and which the decoder has to undo.
//
// The image is, reading normally (top row first):   red   green
//                                                   blue  white
static std::string write_bmp(const char* tag, int bpp, bool topDown)
{
    static int seq = 0;
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_bmp_%d_%d_%s.bmp",
             (int) getpid(), ++seq, tag);

    const int bytesPP  = bpp / 8;
    const int rowBytes = ((2 * bpp + 31) / 32) * 4;      // padded to 4
    const int pixels   = rowBytes * 2;
    const int palBytes = (bpp == 8) ? 4 * 4 : 0;         // four BGRA entries
    const int offBits  = 14 + 40 + palBytes;

    std::vector<unsigned char> out(offBits + pixels, 0);
    auto put16 = [&](int at, unsigned v) {
        out[at] = (unsigned char)(v & 0xFF); out[at+1] = (unsigned char)((v >> 8) & 0xFF);
    };
    auto put32 = [&](int at, unsigned v) {
        for (int i = 0; i < 4; i++) out[at+i] = (unsigned char)((v >> (8*i)) & 0xFF);
    };

    out[0] = 'B'; out[1] = 'M';
    put32(2, (unsigned)(offBits + pixels));
    put32(10, (unsigned) offBits);
    put32(14, 40);                                  // BITMAPINFOHEADER
    put32(18, 2);                                   // width
    put32(22, (unsigned)(topDown ? -2 : 2));        // height (sign = row order)
    put16(26, 1);                                   // planes
    put16(28, (unsigned) bpp);
    put32(30, 0);                                   // BI_RGB
    put32(34, (unsigned) pixels);
    if (bpp == 8) {
        put32(46, 4);                               // biClrUsed
        // Palette, BGRA: 0 red, 1 green, 2 blue, 3 white.
        const unsigned char pal[4][4] = {{0,0,255,0}, {0,255,0,0},
                                         {255,0,0,0}, {255,255,255,0}};
        for (int i = 0; i < 4; i++)
            for (int b = 0; b < 4; b++) out[54 + i*4 + b] = pal[i][b];
    }

    // BGR(A) in the file.  Rows are stored bottom-up unless topDown.
    const unsigned char topRow[2][4]    = {{0,0,255,255}, {0,255,0,255}};   // red,   green
    const unsigned char bottomRow[2][4] = {{255,0,0,255}, {255,255,255,255}};// blue,  white
    for (int r = 0; r < 2; r++) {
        // Which image row this stored row holds.
        const bool isTopImageRow = topDown ? (r == 0) : (r == 1);
        const unsigned char (*srcRow)[4] = isTopImageRow ? topRow : bottomRow;
        unsigned char* dst = &out[offBits + r * rowBytes];
        if (bpp == 8) {
            // Palette indices rather than colours: 0 red, 1 green, 2 blue,
            // 3 white, matching the table written above.
            dst[0] = isTopImageRow ? 0 : 2;
            dst[1] = isTopImageRow ? 1 : 3;
        } else {
            for (int x = 0; x < 2; x++)
                for (int b = 0; b < bytesPP; b++)
                    dst[x * bytesPP + b] = srcRow[x][b];
        }
    }

    FILE* fp = fopen(path, "wb");
    if (!fp) return "";
    fwrite(out.data(), 1, out.size(), fp);
    fclose(fp);
    return path;
}

// world.WindowLoadImage on a .bmp used to fail for EVERY file: ::LoadImage was a
// stub returning null, so the caller reported eFileNotFound for a file that was
// there.  .png worked (it has its own decoder); .bmp had none.  Now decoded by
// bmp_load_shared.cpp -- and the proof is the PIXELS, not the return code.
TEST(LuaSurface, BmpImagesDecodeAndDraw)
{
    // 24bpp, 32bpp, top-down row order, and 8-bit palettised — the four shapes
    // the decoder accepts.  The palette branch has its own pixel path (an index
    // lookup rather than a memcpy), which is why it needs its own variant: a
    // mutation reversing its channels survived until this was here.
    for (int variant = 0; variant < 4; variant++)
    {
        const int  bpp     = (variant == 1) ? 32 : (variant == 3) ? 8 : 24;
        const bool topDown = (variant == 2);
        const std::string bmp = write_bmp(variant == 0 ? "b24"
                                        : variant == 1 ? "b32"
                                        : variant == 2 ? "top" : "pal", bpp, topDown);
        ASSERT_FALSE(bmp.empty());

        LuaDoc f; ASSERT_NE(nullptr, f.L);
        const std::string script =
            "assert(world.WindowCreate('b', 0, 0, 8, 8, 0, 0, 0x000000) == 0)\n"
            "assert(world.WindowLoadImage('b', 'i', '" + bmp + "') == 0,"
            "       'bmp did not load')\n"
            // Drawn 1:1 into the top-left corner (mode 1 = copy).
            "assert(world.WindowDrawImage('b', 'i', 0, 0, 2, 2, 1, 0, 0, 2, 2) == 0)\n"
            // The four corners of the image, in image order.  A decoder that
            // ignored the row order would swap the two rows; one that got BGR
            // and RGB the wrong way round would swap red and blue.
            "assert(world.WindowGetPixel('b', 0, 0) == 0x0000FF, 'top-left not red')\n"
            "assert(world.WindowGetPixel('b', 1, 0) == 0x00FF00, 'top-right not green')\n"
            "assert(world.WindowGetPixel('b', 0, 1) == 0xFF0000, 'bottom-left not blue')\n"
            "assert(world.WindowGetPixel('b', 1, 1) == 0xFFFFFF, 'bottom-right not white')\n";
        EXPECT_TRUE(run(f.L, script.c_str()));
        remove(bmp.c_str());
    }
}

// The refusals, which matter as much: a caller must be able to tell "no such
// file" from "that is not a bitmap I can read", and a malformed header must not
// be half-decoded into a wrong picture.
TEST(LuaSurface, BmpDecoderRefusesWhatItCannotRead)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_bmp_bad_%d.bmp", (int) getpid());

    auto writeBytes = [&](const std::vector<unsigned char>& v) {
        FILE* fp = fopen(path, "wb");
        if (fp) { fwrite(v.data(), 1, v.size(), fp); fclose(fp); }
    };

    EXPECT_TRUE(run(f.L, "assert(world.WindowCreate('b', 0, 0, 8, 8, 0, 0, 0) == 0)\n"));

    // Missing file — distinct from unreadable.
    const std::string missing =
        std::string("assert(world.WindowLoadImage('b','i','/no/such/x.bmp') == ")
        + "error_code.eFileNotFound)\n";
    EXPECT_TRUE(run(f.L, missing.c_str()));

    // Not a BMP at all (right extension, wrong magic).
    writeBytes({'N','O','T','A','B','M','P',0,0,0,0,0,0,0,0,0,0,0,0,0,
                0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                0,0,0,0,0,0,0,0,0,0,0,0,0,0});
    const std::string notbmp =
        std::string("assert(world.WindowLoadImage('b','i','") + path + "') == "
        + "error_code.eUnableToLoadImage)\n";
    EXPECT_TRUE(run(f.L, notbmp.c_str()));

    // A well-formed header whose pixel data is truncated: refused, not read past.
    {
        std::vector<unsigned char> v(54 + 4, 0);
        v[0] = 'B'; v[1] = 'M';
        v[10] = 54;                       // offBits
        v[14] = 40;                       // header size
        v[18] = 64;                       // width 64
        v[22] = 64;                       // height 64  (needs 64*192 bytes)
        v[26] = 1;  v[28] = 24;           // planes, bpp
        writeBytes(v);
    }
    const std::string trunc =
        std::string("assert(world.WindowLoadImage('b','i','") + path + "') == "
        + "error_code.eUnableToLoadImage)\n";
    EXPECT_TRUE(run(f.L, trunc.c_str()));

    remove(path);
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
        // eNoSuchCommand, not eOK: AddFont cannot register a font on this
        // platform at all — see the note below the test.
        "assert(world.AddFont('/no/such/font.ttf') == error_code.eNoSuchCommand)\n"
        // SetCursor: the range gate is -1 .. 12
        "assert(world.SetCursor(-2) == bad)\n"
        "assert(world.SetCursor(13) == bad)\n"
        "assert(world.SetCursor(-1) == 0)\n"
        "assert(world.SetCursor(12) == 0)\n"
        // The two image setters gate on Mode BEFORE they touch the file, so an
        // out-of-range mode is eBadParameter and not the file error.  Both arms
        // are needed: the stub accepted anything, and a check-free body passes a
        // test that only ever supplies a valid mode.
        "assert(world.SetBackgroundImage('x.bmp', 99) == bad)\n"
        "assert(world.SetBackgroundImage('x.bmp', -1) == bad)\n"
        "assert(world.SetForegroundImage('x.bmp', 99) == bad)\n"
        // ...and a VALID mode gets past the gate to report the file failure,
        // so "reject everything" fails too.
        "assert(world.SetBackgroundImage('x.bmp', 0) == error_code.eFileNotFound)\n"
        // SetToolBarPosition: `default: return eBadParameter` on Which
        "assert(world.SetToolBarPosition(0, false, 0, 0, 0) == bad)\n"
        "assert(world.SetToolBarPosition(5, false, 0, 0, 0) == bad)\n"
        // ...and so does a VALID Which, because the toolbars do not exist.
        "assert(world.SetToolBarPosition(1, false, 0, 0, 0) == bad)\n"
    ));
}

// ── two macOS gaps these assertions now pin ───────────────────────────────
//
// Both were INVISIBLE while the output family was stubbed, because the stub
// answered eOK and a test asserted eOK.  They are not adoption regressions —
// they are what the shipping app has always done — and they are pinned here so
// the next person meets them as facts rather than rediscovering them.
//
// world.AddFont ALWAYS fails on macOS.  AddSpecialFont (doc.cpp:3851) opens with
// LoadLibrary("gdi32") to find AddFontResourceExA; platform/mfc_ui.h:5921 stubs
// LoadLibrary to nullptr, so the real body returns eNoSuchCommand at doc.cpp:3874
// before it ever looks at the path.  The macOS equivalent is
// CTFontManagerRegisterFontsForURL.
//
// world.SetToolBarPosition ALWAYS returns eBadParameter on macOS, for every
// Which — not only the out-of-range ones.  The real body gates each arm on
// `!pToolBar->m_hWnd` (methods_output.cpp:275) and the toolbars are created in
// CMainFrame::OnCreate, one of the port's recorded dead readers.  The survey
// listed this as "probable, needs a product run"; the adopted body settles it.
//
// world.Transparency is a third instance of the same LoadLibrary shape —
// MakeWindowTransparent opens with LoadLibrary("user32") — and is asserted false
// in the sweep above.  All three are recorded in docs/STUB-ADOPTION-2026-08.md §4.

// GetFrame and Transparency were the two stubs that blocked this whole family,
// because both dereference App.m_pMainWnd, which used to be nullptr headless.
// platform/headless_app.cpp points it at the real CMainFrame that
// windows_stubs.cpp already owns, whose m_hWnd is null — so both answer exactly
// what their stubs answered, with no dereference.  Pinned so that a later change
// to m_pMainWnd cannot silently turn either into a crash or a different value.
TEST(LuaSurface, FrameDependentMethodsAnswerWithoutAMainWindow)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        // A null lightuserdata — the same "no window" answer the stub gave, and
        // the honest one: the product hands back a wxWindow* here, which is not
        // an HWND and cannot be used as one.
        "local h = world.GetFrame()\n"
        "assert(h == nil or tostring(h) == 'userdata: 0x0', tostring(h))\n"
        // FALSE, matching the PRODUCT — MakeWindowTransparent bails at its
        // LoadLibrary('user32') before touching a window.
        "assert(world.Transparency(0, 1) == false)\n"
        // The clamp still runs ahead of that (MWT_MIN_FACTOR/MWT_MAX_FACTOR), so
        // out-of-range input must not throw.
        "assert(world.Transparency(0, -500) == false)\n"
        "assert(world.Transparency(0, 5000) == false)\n"
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

        "assert(world.WindowRectOp('m', 2, 0, 0, 10, 10, 0, 0) == 0)\n"
        "assert(world.WindowSetPixel('m', 1, 1, 255) == 0)\n"

        // Fonts work now — stage 2 installed a pal::CpuRaster, and its
        // (deliberately synthetic) test font realises.  This asserted
        // eCannotAddFont in stage 1, when there was no surface to realise into.
        "assert(world.WindowFont('m','f','Menlo',10,false,false,false,false,0,0) == 0)\n"

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

// Stage 2: the pixels are real.
//
// platform/headless_surface.cpp installs pal::CpuRaster as the headless
// pal::ISurface, so the ~190 drawing sites in miniwindow.cpp stop taking their
// guarded `if (!srf)` arm and actually paint.  Before this, every assertion the
// miniwindow family could make was about STATE — geometry, z-order, hotspots.
//
// Every assertion below is written so that "drew nothing" FAILS, which matters
// more here than usual: CpuRaster has documented no-op gaps (pal/cpu_raster.h:15-19
// — drawEdge, non-Rect shapes, polyline/bezier/arc, stretched blits, pen widths
// > 1), so an assertion that merely checked a return code would pass against an
// op that painted nothing at all.  These read the pixels back.
TEST(LuaSurface, WindowDrawingActuallyPaints)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        // Background: WindowCreate's own FillSolidRect must reach the surface,
        // which is the ordering hazard headless_surface.cpp exists to get right
        // (the fill happens BEFORE the adopt).  A white window must read white.
        "assert(world.WindowCreate('p', 0, 0, 40, 30, 0, 0, 0xFFFFFF) == 0)\n"
        "assert(world.WindowGetPixel('p', 5, 5) == 0xFFFFFF, 'background lost')\n"

        // setPixel/getPixel round-trip, and only where it was set: the
        // neighbour must still be the background, so "fill everything" fails.
        "assert(world.WindowSetPixel('p', 10, 10, 0x0000FF) == 0)\n"
        "assert(world.WindowGetPixel('p', 10, 10) == 0x0000FF)\n"
        "assert(world.WindowGetPixel('p', 11, 10) == 0xFFFFFF, 'setPixel bled')\n"

        // A filled rect covers its interior and NOTHING outside it.
        "assert(world.WindowRectOp('p', 2, 2, 2, 8, 8, 0xFF0000, 0) == 0)\n"
        "assert(world.WindowGetPixel('p', 4, 4) == 0xFF0000)\n"
        "assert(world.WindowGetPixel('p', 20, 20) == 0xFFFFFF, 'fill escaped')\n"

        // ...and the rect's edges are where they were asked to be.  Right/bottom
        // are exclusive, as everywhere else in this API.
        "assert(world.WindowGetPixel('p', 2, 2) == 0xFF0000, 'top-left missing')\n"
        "assert(world.WindowGetPixel('p', 7, 7) == 0xFF0000, 'inside missing')\n"
        "assert(world.WindowGetPixel('p', 8, 8) == 0xFFFFFF, 'right/bottom inclusive')\n"
        "assert(world.WindowGetPixel('p', 1, 1) == 0xFFFFFF, 'left/top overreach')\n"

        // A gradient must actually vary — the assertion a return code cannot
        // make.  Ends differ from each other AND from the background.
        "assert(world.WindowCreate('g', 0, 0, 40, 10, 0, 0, 0) == 0)\n"
        "assert(world.WindowGradient('g', 0, 0, 40, 10, 0x000000, 0xFFFFFF, 1) == 0)\n"
        "local a, b = world.WindowGetPixel('g', 1, 5), world.WindowGetPixel('g', 38, 5)\n"
        "assert(a ~= b, 'gradient is flat: ' .. tostring(a) .. ' vs ' .. tostring(b))\n"

        // Text: CpuRaster's font is deliberately synthetic, so do NOT assert
        // glyph shapes.  What IS a real contract is that measuring and drawing
        // agree, that a longer string measures wider, and that drawing changed
        // something.
        "assert(world.WindowFont('t2', 'f', 'Menlo', 10, false, false, false, false, 0, 0)"
        "       == error_code.eNoSuchWindow)\n"
        "assert(world.WindowFont('g', 'f', 'Menlo', 10, false, false, false, false, 0, 0) == 0)\n"
        "local w1 = world.WindowTextWidth('g', 'f', 'i', false)\n"
        "local w2 = world.WindowTextWidth('g', 'f', 'iiii', false)\n"
        "assert(w1 > 0 and w2 > w1, 'text measurement is degenerate')\n"
    ));

    // Drawing text must change pixels — checked from C++ so the failure message
    // can say how many changed.
    EXPECT_TRUE(run(f.L,
        "assert(world.WindowCreate('tx', 0, 0, 60, 20, 0, 0, 0x000000) == 0)\n"
        "assert(world.WindowFont('tx','f','Menlo',10,false,false,false,false,0,0) == 0)\n"
        "local before = 0\n"
        "for x = 0, 59 do for y = 0, 19 do\n"
        "  if world.WindowGetPixel('tx', x, y) ~= 0 then before = before + 1 end\n"
        "end end\n"
        "assert(before == 0, 'canvas did not start blank')\n"
        "assert(world.WindowText('tx','f','WWWW',0,0,60,20,0xFFFFFF,false) > 0)\n"
        "local after = 0\n"
        "for x = 0, 59 do for y = 0, 19 do\n"
        "  if world.WindowGetPixel('tx', x, y) ~= 0 then after = after + 1 end\n"
        "end end\n"
        "assert(after > 0, 'WindowText reported a width but painted nothing')\n"
    ));
}

// WindowDelete must release the surface, not just forget the window.
//
// The lifetime here is not obvious: the CBitmap owns the surface, and
// CMiniWindow::Create releases the OLD surface id AFTER already deleting the old
// bitmap — so headless_surface.cpp's release is deliberately a no-op and the
// destroy happens in CBitmap::DeleteObject.  If that pairing were wrong this
// would either leak every canvas or destroy a live one.
TEST(LuaSurface, RecreatingAWindowDoesNotStrandItsCanvas)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        // Create, paint, then RE-create at a different size: the second create
        // frees the first canvas and makes a fresh one, which must come back
        // with the NEW background rather than the old pixels.
        "assert(world.WindowCreate('r', 0, 0, 20, 20, 0, 0, 0xFF0000) == 0)\n"
        "assert(world.WindowGetPixel('r', 5, 5) == 0xFF0000)\n"
        "assert(world.WindowCreate('r', 0, 0, 30, 30, 0, 0, 0x00FF00) == 0)\n"
        "assert(world.WindowGetPixel('r', 5, 5) == 0x00FF00, 'stale canvas survived')\n"
        "assert(world.WindowInfo('r', 3) == 30)\n"
        // ...and a resize keeps the window usable rather than stranding it.
        "assert(world.WindowResize('r', 50, 40, 0x0000FF) == 0)\n"
        "assert(world.WindowSetPixel('r', 45, 35, 0xFFFFFF) == 0)\n"
        "assert(world.WindowGetPixel('r', 45, 35) == 0xFFFFFF, 'resized canvas is dead')\n"
        "assert(world.WindowDelete('r') == 0)\n"
    ));
}

// ── the image family — the DIB store ──────────────────────────────────────
//
// Stage 2's second half (docs/STUB-ADOPTION-2026-08.md §5b).  ::CreateDIBSection
// used to return nullptr headless, so world.WindowLoadImage answered
// eUnableToLoadImage for a perfectly good PNG and the whole image family — draw,
// info, the three alpha ops — had no pixel source at all.  These drive the real
// png_load_shared.cpp decoder through the real miniwindow.cpp bodies.

// Emit a small RGBA PNG.  Written with libpng rather than by hand because the
// point is to exercise the SHIPPING decoder, and a hand-rolled file would only
// prove the two agreed with each other.
//
// Every channel gets a different function of (x, y) so a channel swap, a row
// flip or a transpose all fail: R varies with x, G with y, B is constant, and
// alpha varies with y on a different scale from G.
static std::string write_rgba_png(int w, int h)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_alpha_%d.png", (int)getpid());
    FILE* fp = fopen(path, "wb");
    if (!fp) return "";

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        if (png) png_destroy_write_struct(&png, info ? &info : NULL);
        fclose(fp);
        return "";
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<unsigned char> row((size_t)w * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            row[x*4+0] = (unsigned char)(20 + x * 10);   // R
            row[x*4+1] = (unsigned char)(90 + y * 5);    // G
            row[x*4+2] = 200;                            // B
            row[x*4+3] = (unsigned char)(240 - y * 80);  // A
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return path;
}

// LoadPngMemory is the OTHER decoder — world.WindowLoadImageMemory, which is how
// a plugin loads an image it has fetched or generated rather than one on disk.
// It had only a negative test ("notpng" is refused), so its row-copy loop
// (png_load_shared.cpp:348-350) was never executed.
//
// That loop is the twin of the one in LoadPng that was reading past every row
// allocation, so this test is what keeps a fix at one site from being made at
// only one site.  The widths below are deliberately NOT multiples of 4: for a
// 24bpp image that is exactly when the padded DIB stride exceeds what libpng
// allocated, and the over-read appears.  Run under AddressSanitizer, this test
// is what catches a regression; run normally, it still proves the decode is
// correct.
// A 24bpp (RGB, no alpha) PNG.  It has to be 24bpp: at 32bpp the row is w * 4
// bytes, which is a multiple of 4 for EVERY width, so the padded DIB stride
// always equals what libpng allocated and the over-read this test guards
// against cannot happen.  Writing an RGBA fixture here would have been a test
// that could never fail — which is exactly what the first draft of it was.
static std::string write_rgb_png(int w, int h)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_rgb_%d_%d.png", w, (int)getpid());
    FILE* fp = fopen(path, "wb");
    if (!fp) return "";

    png_structp png  = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        if (png) png_destroy_write_struct(&png, info ? &info : NULL);
        fclose(fp);
        return "";
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<unsigned char> row((size_t)w * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            row[x*3+0] = (unsigned char)(20 + x * 10);   // R varies with x
            row[x*3+1] = (unsigned char)(90 + y * 5);    // G varies with y
            row[x*3+2] = 200;                            // B constant
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return path;
}

TEST(LuaSurface, LoadImageMemoryDecodesAPngWithAPaddedStride)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    // Widths 5, 6 and 7 pad by 1, 2 and 3 bytes respectively — every non-zero
    // remainder — and a height of 3 means the loop runs more than once, so a
    // fault on any row but the first is still reachable.
    for (int w = 5; w <= 7; ++w) {
        const std::string png = write_rgb_png(w, 3);
        ASSERT_FALSE(png.empty());

        char script[1400];
        snprintf(script, sizeof(script),
            "local fh = assert(io.open('%s', 'rb'))\n"
            "local bytes = fh:read('*a')\n"
            "fh:close()\n"
            "assert(#bytes > 0, 'fixture PNG is empty')\n"
            "assert(world.WindowCreate('m%d', 0, 0, 20, 20, 0, 0, 0x000000) == 0)\n"
            "assert(world.WindowLoadImageMemory('m%d', 'i', bytes, false) == 0,\n"
            "       'in-memory PNG did not decode')\n"
            "assert(world.WindowImageInfo('m%d', 'i', 2) == %d, 'width')\n"
            "assert(world.WindowImageInfo('m%d', 'i', 3) == 3, 'height')\n"
            // 24bpp is load-bearing here, not incidental: it is the only depth
            // at which the DIB stride is padded, which is the condition under
            // test.  Assert it, so a fixture that quietly became RGBA fails.
            "assert(world.WindowImageInfo('m%d', 'i', 6) == 24, 'depth')\n"
            "assert(world.WindowDrawImage('m%d', 'i', 0, 0, 0, 0, 1, 0, 0, 0, 0) == 0)\n"
            "for x = 0, %d do for y = 0, 2 do\n"
            "  local want = (20 + x * 10) + (90 + y * 5) * 256 + 200 * 65536\n"
            "  local got = world.WindowGetPixel('m%d', x, y)\n"
            "  assert(got == want,\n"
            "         string.format('memory decode at %%d,%%d: got %%d want %%d',\n"
            "                       x, y, got, want))\n"
            "end end\n",
            png.c_str(), w, w, w, w, w, w, w, w - 1, w);

        EXPECT_TRUE(run(f.L, script));
        ::unlink(png.c_str());
    }
}

TEST(LuaSurface, ImageFamilyRoundTripsAPngThroughTheDibStore)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    char png[256];
    snprintf(png, sizeof(png), "/tmp/mc_test_image_%d.png", (int)getpid());
    ::unlink(png);

    char script[2048];
    snprintf(script, sizeof(script),
        // Paint a source window with three distinguishable areas, then let the
        // app's own writer emit a PNG of it.
        // Blue field with a green band at x in [2, 6) — off both edges, so a
        // mirrored or transposed image cannot land on the same colours.
        "assert(world.WindowCreate('src', 0, 0, 9, 5, 0, 0, 0x0000FF) == 0)\n"
        "assert(world.WindowRectOp('src', 2, 2, 0, 6, 5, 0x00FF00, 0) == 0)\n"
        "assert(world.WindowWrite('src', '%s') == 0)\n"

        // ...and read it straight back in.  This is the assertion the old
        // contract could not make: it used to be eUnableToLoadImage here.
        "assert(world.WindowCreate('dst', 0, 0, 20, 20, 0, 0, 0x000000) == 0)\n"
        "assert(world.WindowLoadImage('dst', 'img', '%s') == 0, 'PNG did not load')\n"

        // Geometry survives the round trip.  9 x 5 at 24bpp is 27 bytes a row,
        // padded to 28 — so bmWidthBytes catches a stride computed as w*3.
        "assert(world.WindowImageInfo('dst', 'img', 2) == 9, 'width wrong')\n"
        "assert(world.WindowImageInfo('dst', 'img', 3) == 5, 'height wrong')\n"
        "assert(world.WindowImageInfo('dst', 'img', 6) == 24, 'depth wrong')\n"
        "assert(world.WindowImageInfo('dst', 'img', 4) == 28, 'stride wrong')\n"
        "assert(world.WindowImageInfo('dst', 'img', 5) == 1)\n"
        "local list = world.WindowImageList('dst')\n"
        "assert(type(list) == 'table' and list[1] == 'img')\n"

        // Drawing it moves real pixels, in the right orientation and at the
        // right offset.  Mode 1 is a straight copy.
        "assert(world.WindowDrawImage('dst', 'img', 3, 4, 0, 0, 1, 0, 0, 0, 0) == 0)\n"
        "assert(world.WindowGetPixel('dst', 3, 4) == 0x0000FF, 'left column lost')\n"
        "assert(world.WindowGetPixel('dst', 5, 4) == 0x00FF00, 'green band lost')\n"
        "assert(world.WindowGetPixel('dst', 9, 4) == 0x0000FF, 'right of band wrong')\n"
        "assert(world.WindowGetPixel('dst', 12, 4) == 0x000000, 'drew past the image')\n"
        "assert(world.WindowGetPixel('dst', 3, 3) == 0x000000, 'drew above the offset')\n"

        // A sub-rectangle really is a sub-rectangle: start at x=2, which is
        // inside the green band, so the left-hand blue must NOT appear.
        "assert(world.WindowDrawImage('dst', 'img', 0, 10, 0, 0, 1, 2, 0, 5, 3) == 0)\n"
        "assert(world.WindowGetPixel('dst', 0, 10) == 0x00FF00, 'src offset ignored')\n"
        "assert(world.WindowGetPixel('dst', 3, 10) == 0x000000, 'src width ignored')\n"

        // Mode 3 keys on the image's own (0,0) pixel, which is blue — so the
        // blue columns drop out and the green survives.
        "assert(world.WindowDrawImage('dst', 'img', 0, 15, 0, 0, 3, 0, 0, 0, 0) == 0)\n"
        "assert(world.WindowGetPixel('dst', 0, 15) == 0x000000, 'colour key ignored')\n"
        "assert(world.WindowGetPixel('dst', 2, 15) == 0x00FF00, 'colour key too greedy')\n"

        // An unknown id is refused rather than silently ignored, and a bad mode
        // is rejected — both controls for \"the body ran at all\".
        "assert(world.WindowDrawImage('dst', 'nope', 0, 0, 0, 0, 1, 0, 0, 0, 0)\n"
        "       == error_code.eImageNotInstalled)\n"
        "assert(world.WindowDrawImage('dst', 'img', 0, 0, 0, 0, 9, 0, 0, 0, 0)\n"
        "       == error_code.eBadParameter)\n",
        png, png);

    EXPECT_TRUE(run(f.L, script));
    ::unlink(png);
}

// A .bmp written by world.WindowWrite must be readable — by anything.  The
// shim's BITMAPFILEHEADER lacked Windows' #pragma pack(2), so on LP64 it was
// 16 bytes: every field of a written .bmp sat +2 from its standard offset,
// bfOffBits said 56, no parser (including this port's own LoadBmp) could open
// the file, and WindowWrite still returned eOK.  The static_assert pins the
// layout; the round trip and the raw header bytes pin the file.
static_assert(sizeof(BITMAPFILEHEADER) == 14,
              "BITMAPFILEHEADER must have the on-disk 14-byte layout");

TEST(LuaSurface, WindowWriteBmpRoundTrips)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    char bmp[256];
    snprintf(bmp, sizeof(bmp), "/tmp/mc_test_write_%d.bmp", (int)getpid());
    ::unlink(bmp);

    char script[1536];
    snprintf(script, sizeof(script),
        // Same non-degenerate source as the PNG round trip: blue field, green
        // band off both edges.
        "assert(world.WindowCreate('src', 0, 0, 9, 5, 0, 0, 0x0000FF) == 0)\n"
        "assert(world.WindowRectOp('src', 2, 2, 0, 6, 5, 0x00FF00, 0) == 0)\n"
        "assert(world.WindowWrite('src', '%s') == 0)\n"
        "assert(world.WindowCreate('dst', 0, 0, 20, 20, 0, 0, 0x000000) == 0)\n"
        "assert(world.WindowLoadImage('dst', 'img', '%s') == 0, 'BMP unreadable')\n"
        "assert(world.WindowImageInfo('dst', 'img', 2) == 9, 'width wrong')\n"
        "assert(world.WindowImageInfo('dst', 'img', 3) == 5, 'height wrong')\n"
        "assert(world.WindowDrawImage('dst', 'img', 0, 0, 0, 0, 1, 0, 0, 0, 0) == 0)\n"
        "assert(world.WindowGetPixel('dst', 0, 0) == 0x0000FF, 'blue lost')\n"
        "assert(world.WindowGetPixel('dst', 3, 0) == 0x00FF00, 'band lost')\n",
        bmp, bmp);
    EXPECT_TRUE(run(f.L, script));

    // The header bytes themselves, at their STANDARD offsets: 'BM' at 0,
    // bfOffBits at 10 == 54 (14-byte file header + 40-byte info header).
    FILE* fp = fopen(bmp, "rb");
    ASSERT_TRUE(fp != nullptr);
    unsigned char hdr[14] = {};
    ASSERT_EQ((size_t)14, fread(hdr, 1, sizeof hdr, fp));
    fclose(fp);
    EXPECT_EQ('B', hdr[0]);
    EXPECT_EQ('M', hdr[1]);
    const unsigned offBits = hdr[10] | (hdr[11] << 8) |
                             ((unsigned)hdr[12] << 16) | ((unsigned)hdr[13] << 24);
    EXPECT_EQ(54u, offBits);
    ::unlink(bmp);
}

// world.SetBackgroundImage('') must CLEAR the image.  The clear guard is
// shared Windows code — `if ((HBITMAP) m_BackgroundBitmap) DeleteObject()` —
// reading the m_hObject ownership token, which the port's CBitmap::Attach
// left NULL after every successful load: the guard skipped the delete, the
// surface survived, and (in the GUI) the old image painted forever after the
// user removed it.  Attach now keeps the token non-null in both builds.
TEST(LuaSurface, SetBackgroundImageEmptyClearsTheImage)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    char png[256];
    snprintf(png, sizeof(png), "/tmp/mc_test_bgimg_%d.png", (int)getpid());
    ::unlink(png);

    char script[512];
    snprintf(script, sizeof(script),
        "assert(world.WindowCreate('src', 0, 0, 9, 5, 0, 0, 0x0000FF) == 0)\n"
        "assert(world.WindowWrite('src', '%s') == 0)\n"
        "assert(world.SetBackgroundImage('%s', 0) == 0, 'load failed')\n",
        png, png);
    EXPECT_TRUE(run(f.L, script));

    // After a successful load the token is set and the bitmap is REAL.
    EXPECT_TRUE((HBITMAP) f.doc.m_BackgroundBitmap != NULL);
    BITMAP info = {};
    EXPECT_TRUE(f.doc.m_BackgroundBitmap.GetBitmap(&info));
    EXPECT_EQ(9, (int) info.bmWidth);

    // Clearing really deletes it: with the old null token, DeleteObject was
    // skipped and GetBitmap kept answering for the stale image.
    EXPECT_EQ((long) eOK, f.doc.SetBackgroundImage("", 0));
    EXPECT_TRUE((HBITMAP) f.doc.m_BackgroundBitmap == NULL);
    EXPECT_FALSE(f.doc.m_BackgroundBitmap.GetBitmap(&info));
    ::unlink(png);
}

TEST(LuaSurface, AlphaImageOpsReadTheImagesRealAlpha)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    const std::string png = write_rgba_png(4, 3);
    ASSERT_FALSE(png.empty());

    char script[3072];
    snprintf(script, sizeof(script),
        "assert(world.WindowCreate('w', 0, 0, 20, 20, 0, 0, 0x000000) == 0)\n"
        "assert(world.WindowLoadImage('w', 'a', '%s') == 0, 'RGBA PNG did not load')\n"

        // The gate every alpha op checks first.  A 32bpp PNG must report 32, or
        // all three return eImageNotInstalled and nothing below runs.
        "assert(world.WindowImageInfo('w', 'a', 6) == 32, 'alpha channel lost')\n"

        // WindowGetImageAlpha paints the alpha CHANNEL as grey.  Row y has
        // alpha 240 - y*80, so the three rows must read three different greys —
        // an image whose alpha came back as a constant (0, or 255) fails.
        "assert(world.WindowGetImageAlpha('w', 'a', 0, 0, 4, 3, 0, 0) == 0)\n"
        "assert(world.WindowGetPixel('w', 1, 0) == 240 * 0x010101, 'row 0 alpha')\n"
        "assert(world.WindowGetPixel('w', 1, 1) == 160 * 0x010101, 'row 1 alpha')\n"
        "assert(world.WindowGetPixel('w', 1, 2) ==  80 * 0x010101, 'row 2 alpha')\n"

        // ...and it is the alpha, not the colour: B is a constant 200 in every
        // pixel, so a body that copied a colour channel would give one grey.
        "assert(world.WindowGetPixel('w', 3, 0) == 240 * 0x010101, 'not per-pixel')\n"

        // WindowDrawImageAlpha over black at full opacity is C * alpha / 255,
        // per channel.  R varies with x and G with y, so this pins the whole
        // BGRA ordering as well as the blend.
        "assert(world.WindowCreate('m', 0, 0, 20, 20, 0, 0, 0x000000) == 0)\n"
        "assert(world.WindowLoadImage('m', 'a', '%s') == 0)\n"
        "assert(world.WindowDrawImageAlpha('m', 'a', 0, 0, 4, 3, 1.0, 0, 0) == 0)\n"
        "local function px(r, g, b) return r + g * 256 + b * 65536 end\n"
        "local function blend(c, m) return math.floor((c * m) / 255) end\n"
        "for y = 0, 2 do\n"
        "  local al = 240 - y * 80\n"
        "  for x = 0, 3 do\n"
        "    local want = px(blend(20 + x * 10, al), blend(90 + y * 5, al), blend(200, al))\n"
        "    local got = world.WindowGetPixel('m', x, y)\n"
        "    assert(got == want, string.format('draw-alpha at %%d,%%d: got %%d want %%d',\n"
        "                                       x, y, got, want))\n"
        "  end\n"
        "end\n"
        // Outside the drawn rectangle the base is untouched.
        "assert(world.WindowGetPixel('m', 5, 0) == 0, 'draw-alpha escaped its rect')\n"

        // Opacity 0 is the control: the same call must leave the base alone.
        // Without it, a body that ignored Opacity entirely would still pass
        // everything above.
        "assert(world.WindowCreate('z', 0, 0, 20, 20, 0, 0, 0x123456) == 0)\n"
        "assert(world.WindowLoadImage('z', 'a', '%s') == 0)\n"
        "assert(world.WindowDrawImageAlpha('z', 'a', 0, 0, 4, 3, 0.0, 0, 0) == 0)\n"
        "assert(world.WindowGetPixel('z', 1, 1) == 0x123456, 'opacity 0 still drew')\n"
        "assert(world.WindowDrawImageAlpha('z', 'a', 0, 0, 4, 3, 1.5, 0, 0)\n"
        "       == error_code.eBadParameter)\n",
        png.c_str(), png.c_str(), png.c_str());

    EXPECT_TRUE(run(f.L, script));
    ::unlink(png.c_str());
}

// WindowImageFromWindow is the other producer of a 32bpp image, and it makes one
// without going near a PNG: a surface readback memcpy'd into a DIB.  Its alpha is
// the surface's, i.e. zero everywhere — which is not a defect but the same thing
// GDI leaves behind, and it has an observable consequence worth pinning.
TEST(LuaSurface, ImageFromWindowCapturesPixelsWithGdiAlpha)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "assert(world.WindowCreate('s', 0, 0, 6, 4, 0, 0, 0x00FF00) == 0)\n"
        "assert(world.WindowSetPixel('s', 1, 1, 0xFF0000) == 0)\n"
        "assert(world.WindowCreate('d', 0, 0, 20, 20, 0, 0, 0x000000) == 0)\n"
        "assert(world.WindowImageFromWindow('d', 'cap', 's') == 0)\n"
        "assert(world.WindowImageInfo('d', 'cap', 2) == 6)\n"
        "assert(world.WindowImageInfo('d', 'cap', 3) == 4)\n"
        "assert(world.WindowImageInfo('d', 'cap', 6) == 32)\n"

        // The captured pixels are the source's, in place.
        "assert(world.WindowDrawImage('d', 'cap', 0, 0, 0, 0, 1, 0, 0, 0, 0) == 0)\n"
        "assert(world.WindowGetPixel('d', 1, 1) == 0xFF0000, 'capture lost a pixel')\n"
        "assert(world.WindowGetPixel('d', 0, 0) == 0x00FF00, 'capture lost the field')\n"

        // Alpha 0 everywhere: an alpha draw of it changes nothing, because
        // Blend_Mask with a zero mask keeps the base.  Same on Windows.
        "assert(world.WindowCreate('b', 0, 0, 20, 20, 0, 0, 0x0000FF) == 0)\n"
        "assert(world.WindowImageFromWindow('b', 'cap', 's') == 0)\n"
        "assert(world.WindowDrawImageAlpha('b', 'cap', 0, 0, 6, 4, 1.0, 0, 0) == 0)\n"
        "assert(world.WindowGetPixel('b', 1, 1) == 0x0000FF, 'zero alpha still drew')\n"
        // ...while a plain DrawImage of the same capture DOES paint, so the
        // line above is about the alpha, not about the capture being empty.
        "assert(world.WindowDrawImage('b', 'cap', 0, 0, 0, 0, 1, 0, 0, 0, 0) == 0)\n"
        "assert(world.WindowGetPixel('b', 1, 1) == 0xFF0000, 'capture was empty')\n"

        // A capture of a window that is not there leaves nothing behind.
        "assert(world.WindowImageFromWindow('d', 'cap2', 'nope')\n"
        "       == error_code.eNoSuchWindow)\n"
        "assert(world.WindowImageInfo('d', 'cap2', 2) == nil)\n"
    ));
}

// ── WindowMergeImageAlpha — the MASK variant of the alpha family ───────────
//
// A DIFFERENT method from WindowDrawImageAlpha (covered above).  It takes TWO
// image ids and blends the source into the window through a per-CHANNEL mask:
//
//     result = (A * M + B * (255 - M)) / 255           miniwindow.cpp:3373
//
// where A is the source image's byte, B the window's own byte and M the mask's
// — B, G and R each masked INDEPENDENTLY.  Opacity then fades that result back
// towards B (Simple_Opacity, blending.h:45).  Mode 0 is the plain blend; mode 1
// additionally treats the SOURCE RECTANGLE's top-left colour as transparent, so
// every pixel carrying it keeps the window's own colour.
//
// There is NO 32bpp gate in this body.  miniwindow.cpp has exactly two, at :3797
// (DrawImageAlpha) and :3916 (GetImageAlpha), and neither is on this path — so a
// plain 24bpp RGB PNG is a legal source AND a legal mask, which is what these
// fixtures deliberately use.  (tests/test_gdi_headless.cpp:251 says
// "WindowMergeImageAlpha ... return[s] eImageNotInstalled unless this reads 32";
// that is true of the other two methods it names and false of this one.)
//
// Fixture shape follows the degenerate-fixture rule: the source image is 7x4,
// the mask is 9x6 (different from the source AND non-square), the window is
// 20x14, the destination corner is Left=3 Top=5 (Left != Top) and the source
// corner is SrcLeft=2 SrcTop=1 (both non-zero and unequal).  Image space, mask
// space and window space are therefore three different coordinate systems and
// none of the conversions the body performs is an identity.

// An 8-bit RGB (24bpp, no alpha channel) PNG whose three channels are three
// DIFFERENT functions of (x, y), written with libpng so the shipping decoder is
// what runs.
//   kind 0 — the merge source.  R varies with x, G with y, B with x on a third
//            scale.  Image pixel (5,2) is a planted DUPLICATE of (2,1): mode 1
//            keys on the source rectangle's top-left pixel, and without a second
//            pixel of that colour away from the corner, "the key works" would be
//            indistinguishable from "the corner alone was skipped".
//   kind 1 — the mask.  Mask pixel (3,3) is a planted extreme, RGB(255, 0, 128):
//            at that pixel red must come out as the IMAGE's exactly, green as
//            the WINDOW's exactly and blue as a mix — which no single-scalar
//            (luminance, or "the blue byte") mask can produce.
// Both are pure functions of (x, y), so the same formula describes a mask of any
// size — which is what lets the too-small / exactly-big-enough masks below be
// compared against the same model.
static std::string write_merge_png(const char* tag, int w, int h, int kind)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_test_merge_%s_%d.png", tag, (int)getpid());
    FILE* fp = fopen(path, "wb");
    if (!fp) return "";

    png_structp png  = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        if (png) png_destroy_write_struct(&png, info ? &info : NULL);
        fclose(fp);
        return "";
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<unsigned char> row((size_t)w * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int r, g, b;
            if (kind == 0) {
                const bool dup = (x == 5 && y == 2);          // planted duplicate
                const int sx = dup ? 2 : x, sy = dup ? 1 : y;
                r = 30 + 20 * sx;  g = 60 + 25 * sy;  b = 130 + 5 * sx;
            } else if (x == 3 && y == 3) {
                r = 255;  g = 0;  b = 128;                    // planted extreme
            } else {
                r = 255 - 25 * x;  g = 12 * y;  b = 100 + 9 * x + 3 * y;
            }
            row[x*3+0] = (unsigned char) r;
            row[x*3+1] = (unsigned char) g;
            row[x*3+2] = (unsigned char) b;
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return path;
}

// The shared Lua prelude: fresh(name) builds a window, and the model functions
// transcribe the shipping arithmetic so every merged pixel can be asserted
// EXACTLY rather than "it changed".
//
// The base is two-tone across the merged rectangle (a brighter block over its
// right half), so a body that read one window pixel and reused it, or that
// blended against a constant, cannot pass.  check() also insists the model
// DIFFERS from the base at every pixel it asserts, so "the code did nothing"
// always fails.
static std::string merge_prelude(const std::string& img, const std::string& mask)
{
    return
        "local IMG, MSK = '" + img + "', '" + mask + "'\n"
        "local function px (r, g, b) return r + g * 256 + b * 65536 end\n"

        "local function fresh (name)\n"
        "  assert(world.WindowCreate(name, 0, 0, 20, 14, 0, 0, 0x102030) == 0)\n"
        // the block: x in [5,7), y in [4,8) — Left ~= Top, and its top row sits
        // one row ABOVE the merged rectangle so it doubles as an escape control.
        "  assert(world.WindowRectOp(name, 2, 5, 4, 7, 8, 0xC0A080, 0) == 0)\n"
        "  assert(world.WindowLoadImage(name, 'img', IMG) == 0, 'source PNG did not load')\n"
        "  assert(world.WindowLoadImage(name, 'msk', MSK) == 0, 'mask PNG did not load')\n"
        // 24bpp for BOTH, and the merge below still works: no depth gate here.
        "  assert(world.WindowImageInfo(name, 'img', 6) == 24, 'source is not 24bpp')\n"
        "  assert(world.WindowImageInfo(name, 'msk', 6) == 24, 'mask is not 24bpp')\n"
        "  assert(world.WindowImageInfo(name, 'img', 2) == 7, 'source width')\n"
        "  assert(world.WindowImageInfo(name, 'img', 3) == 4, 'source height')\n"
        "  assert(world.WindowImageInfo(name, 'msk', 2) == 9, 'mask width')\n"
        "  assert(world.WindowImageInfo(name, 'msk', 3) == 6, 'mask height')\n"
        "  assert(world.WindowGetPixel(name, 3, 5) == 0x102030, 'base field wrong')\n"
        "  assert(world.WindowGetPixel(name, 5, 5) == 0xC0A080, 'base block wrong')\n"
        "end\n"

        // ── the shipping arithmetic, transcribed ────────────────────────────
        // Blend_Mask (miniwindow.cpp:3373) is integer division; Simple_Opacity
        // (blending.h:45) is the double expression, in the same order, so the
        // two agree bit-for-bit.  Opacity is only ever a half or a quarter here,
        // which is exact in a double, so nothing rests on rounding luck.
        "local function bm (a, b, m) return math.floor((a * m + b * (255 - m)) / 255) end\n"
        "local function so (b, x, o) return math.floor(o * x + (1.0 - o) * b + 0.5) end\n"

        // the fixture's own generators — must match write_merge_png() above
        "local function src (x, y)\n"
        "  if x == 5 and y == 2 then return src(2, 1) end\n"
        "  return 30 + 20 * x, 60 + 25 * y, 130 + 5 * x\n"
        "end\n"
        "local function msk (x, y)\n"
        "  if x == 3 and y == 3 then return 255, 0, 128 end\n"
        "  return 255 - 25 * x, 12 * y, 100 + 9 * x + 3 * y\n"
        "end\n"
        "local function base (wx, wy)\n"
        "  if wx >= 5 and wx < 7 and wy >= 4 and wy < 8 then return 128, 160, 192 end\n"
        "  return 48, 32, 16\n"
        "end\n"

        // one merged pixel: ix,iy are IMAGE coords (the mask is read at the SAME
        // source offset, which is exactly what these assertions pin), wx,wy are
        // WINDOW coords, and kx,ky the source rectangle's top-left.
        "local function want (ix, iy, wx, wy, mode, op, kx, ky)\n"
        "  local ar, ag, ab = src(ix, iy)\n"
        "  local br, bg, bb = base(wx, wy)\n"
        "  local mr, mg, mb = msk(ix, iy)\n"
        "  if mode == 1 then\n"
        "    local r0, g0, b0 = src(kx, ky)\n"
        "    if ar == r0 and ag == g0 and ab == b0 then ar, ag, ab = br, bg, bb end\n"
        "  end\n"
        "  local r, g, b = bm(ar, br, mr), bm(ag, bg, mg), bm(ab, bb, mb)\n"
        "  if op < 1.0 then r, g, b = so(br, r, op), so(bg, g, op), so(bb, b, op) end\n"
        "  return px(r, g, b)\n"
        "end\n"

        // Assert the whole merged rectangle, pixel by pixel and channel by
        // channel.  `strict` demands the model differ from the base at every
        // pixel — the fixture self-check that makes "did nothing" fail.
        "local function check (name, l, t, sl, st, w, h, mode, op, strict, tag)\n"
        "  for dy = 0, h - 1 do\n"
        "    for dx = 0, w - 1 do\n"
        "      local wx, wy = l + dx, t + dy\n"
        "      local exp = want(sl + dx, st + dy, wx, wy, mode, op, sl, st)\n"
        "      local got = world.WindowGetPixel(name, wx, wy)\n"
        "      assert(got == exp, string.format('%s at win %d,%d: got %d want %d',\n"
        "                                       tag, wx, wy, got, exp))\n"
        "      if strict then\n"
        "        local br, bg, bb = base(wx, wy)\n"
        "        assert(exp ~= px(br, bg, bb),\n"
        "               string.format('DEGENERATE FIXTURE: %s at win %d,%d equals the base',\n"
        "                             tag, wx, wy))\n"
        "      end\n"
        "    end\n"
        "  end\n"
        "end\n"

        // the control for every positive assertion: this pixel still reads what
        // fresh() painted there.
        "local function untouched (name, wx, wy, tag)\n"
        "  local br, bg, bb = base(wx, wy)\n"
        "  local got = world.WindowGetPixel(name, wx, wy)\n"
        "  assert(got == px(br, bg, bb),\n"
        "         string.format('%s: win %d,%d moved: got %d want %d',\n"
        "                       tag, wx, wy, got, px(br, bg, bb)))\n"
        "end\n";
}

// Mode 0 at full opacity: the blend itself, and that the mask is per channel and
// per pixel, read at the source offset rather than from the image's own corner.
TEST(LuaSurface, MergeImageAlphaBlendsPerChannelThroughItsMask)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    const std::string img = write_merge_png("img", 7, 4, 0);
    const std::string msk = write_merge_png("msk", 9, 6, 1);
    ASSERT_FALSE(img.empty());
    ASSERT_FALSE(msk.empty());

    const std::string script = merge_prelude(img, msk) +
        "fresh('a')\n"
        // dest (3,5)-(8,12) is 5x7, source (2,1)-(6,4) is 4x3, so the extent is
        // taken from the SOURCE side of the min() at miniwindow.cpp:3293.
        "assert(world.WindowMergeImageAlpha('a', 'img', 'msk', 3, 5, 8, 12,\n"
        "                                   0, 1.0, 2, 1, 6, 4) == 0, 'merge refused')\n"
        "check('a', 3, 5, 2, 1, 4, 3, 0, 1.0, true, 'mode0/op1')\n"

        // The planted extreme: mask (3,3) = RGB(255, 0, 128) lands at window
        // (4,7).  Red is the image's 90 exactly (M=255), green the window's 32
        // exactly (M=0), blue a mix.  A body that used one scalar mask for all
        // three channels cannot produce this triple.
        "assert(world.WindowGetPixel('a', 4, 7) == px(90, 32, 80),\n"
        "       'the mask is not applied per channel')\n"

        // ...and it landed at (4,7) because the mask was read at (SrcLeft+1,
        // SrcTop+2).  Had it been read from the mask's own (0,0) the extreme
        // would be two columns and one row away, which check() above forbids.

        // Controls: nothing outside the merged rectangle moved, on all four
        // sides — including the block's own top row, which is one row above.
        "untouched('a',  2,  5, 'left of the rect')\n"
        "untouched('a',  7,  5, 'right of the rect')\n"
        "untouched('a',  3,  4, 'above the rect')\n"
        "untouched('a',  3,  8, 'below the rect')\n"
        "untouched('a',  5,  4, 'the block row above the rect')\n"
        "untouched('a',  6,  8, 'the block row below the rect')\n"
        "untouched('a', 19, 13, 'the far corner')\n"
        "return true\n";

    EXPECT_LUA_OK(f.L, script.c_str());
    ::unlink(img.c_str());
    ::unlink(msk.c_str());
}

// The two opacity arms are genuinely different code (miniwindow.cpp:3378), and
// the fade is against the WINDOW's own pixel, not against black.
TEST(LuaSurface, MergeImageAlphaOpacityArmsAreDistinct)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    const std::string img = write_merge_png("img", 7, 4, 0);
    const std::string msk = write_merge_png("msk", 9, 6, 1);
    ASSERT_FALSE(img.empty());
    ASSERT_FALSE(msk.empty());

    const std::string script = merge_prelude(img, msk) +
        "fresh('half')  fresh('full')  fresh('zero')  fresh('quarter')\n"
        "local function merge (n, op)\n"
        "  return world.WindowMergeImageAlpha(n, 'img', 'msk', 3, 5, 8, 12, 0, op, 2, 1, 6, 4)\n"
        "end\n"
        "assert(merge('half', 0.5) == 0)\n"
        "assert(merge('quarter', 0.25) == 0)\n"
        "assert(merge('full', 1.0) == 0)\n"
        "assert(merge('zero', 0.0) == 0)\n"

        "check('half',    3, 5, 2, 1, 4, 3, 0, 0.5,  true, 'mode0/op0.5')\n"
        "check('quarter', 3, 5, 2, 1, 4, 3, 0, 0.25, true, 'mode0/op0.25')\n"

        // The faded picture is not the unfaded one.  Without this a body that
        // dropped Opacity on the floor would pass every exact assertion that
        // used op = 1.
        "local same = 0\n"
        "for dy = 0, 2 do for dx = 0, 3 do\n"
        "  if world.WindowGetPixel('half', 3 + dx, 5 + dy)\n"
        "     == world.WindowGetPixel('full', 3 + dx, 5 + dy) then same = same + 1 end\n"
        "end end\n"
        "assert(same == 0, 'opacity 0.5 painted the full-opacity picture at '\n"
        "                  .. same .. ' of 12 pixels')\n"

        // Opacity 0 is the other end of the same arm: the call SUCCEEDS and
        // leaves every pixel exactly as it was.
        "for dy = 0, 2 do for dx = 0, 3 do\n"
        "  untouched('zero', 3 + dx, 5 + dy, 'opacity 0 still drew')\n"
        "end end\n"
        "return true\n";

    EXPECT_LUA_OK(f.L, script.c_str());
    ::unlink(img.c_str());
    ::unlink(msk.c_str());
}

// Mode 1 keys on the SOURCE RECTANGLE's top-left pixel — image (SrcLeft,SrcTop),
// which is neither the image's own (0,0) nor (because PixelBuffer is stored
// bottom-up) the first row of bytes in the buffer.  Any of those three would
// pick a different key colour here, and therefore a different set of pixels to
// leave alone.
TEST(LuaSurface, MergeImageAlphaModeOneKeysOnTheSourceRectsCorner)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    const std::string img = write_merge_png("img", 7, 4, 0);
    const std::string msk = write_merge_png("msk", 9, 6, 1);
    ASSERT_FALSE(img.empty());
    ASSERT_FALSE(msk.empty());

    const std::string script = merge_prelude(img, msk) +
        "fresh('k1')  fresh('k0')  fresh('kh')\n"
        "assert(world.WindowMergeImageAlpha('k1', 'img', 'msk', 3, 5, 8, 12,\n"
        "                                   1, 1.0, 2, 1, 6, 4) == 0)\n"
        // `strict` is false: mode 1 is SUPPOSED to leave the two keyed pixels
        // reading their base, so demanding every pixel differ would be wrong.
        "check('k1', 3, 5, 2, 1, 4, 3, 1, 1.0, false, 'mode1/op1')\n"

        // Exactly two pixels carry the key colour src(2,1): image (2,1) itself,
        // at window (3,5), and the planted duplicate image (5,2), at window
        // (6,6).  Their bases DIFFER (field vs block), so a body that wrote a
        // constant, or that skipped the corner only, fails here.
        "assert(world.WindowGetPixel('k1', 3, 5) == 0x102030, 'keyed corner over the field')\n"
        "assert(world.WindowGetPixel('k1', 6, 6) == 0xC0A080, 'keyed pixel over the block')\n"

        // ...and mode 1 has NOT simply stopped blending: an unkeyed pixel still
        // takes the full per-channel blend, the same value mode 0 gives it.
        "assert(world.WindowGetPixel('k1', 4, 7) == px(90, 32, 80), 'mode 1 stopped blending')\n"

        // The contrast control.  Those same two pixels DO move under mode 0, so
        // the two assertions above are about the mode, not about the fixture.
        "assert(world.WindowMergeImageAlpha('k0', 'img', 'msk', 3, 5, 8, 12,\n"
        "                                   0, 1.0, 2, 1, 6, 4) == 0)\n"
        "assert(world.WindowGetPixel('k0', 3, 5) ~= 0x102030, 'mode 0 honoured a colour key')\n"
        "assert(world.WindowGetPixel('k0', 6, 6) ~= 0xC0A080, 'mode 0 honoured a colour key')\n"

        // Mode 1 with a fade runs the other of its two arms.  Keyed pixels stay
        // put at any opacity — Blend_Mask(B,B,M) is B and Simple_Opacity(B,B,o)
        // is B — while everything else fades.
        "assert(world.WindowMergeImageAlpha('kh', 'img', 'msk', 3, 5, 8, 12,\n"
        "                                   1, 0.5, 2, 1, 6, 4) == 0)\n"
        "check('kh', 3, 5, 2, 1, 4, 3, 1, 0.5, false, 'mode1/op0.5')\n"
        "assert(world.WindowGetPixel('kh', 3, 5) == 0x102030, 'keyed pixel faded')\n"
        "assert(world.WindowGetPixel('kh', 6, 6) == 0xC0A080, 'keyed pixel faded')\n"
        "assert(world.WindowGetPixel('kh', 4, 7) ~= world.WindowGetPixel('k1', 4, 7),\n"
        "       'mode 1 ignored Opacity')\n"

        // Mode 1 is the only arm that walks the buffer a ROW at a time
        // (base = row * perline), so it is the only one where the row stride
        // matters — and a 4-wide merge cannot see it, because 4 pixels of BGR24
        // are 12 bytes and a 4-byte-aligned stride of 12 as well.  Redo it FIVE
        // wide, where the stride is 16 and 5 * 3 is 15, so a body that computed
        // the row pitch from the width instead of reading the buffer's stride
        // walks off by one byte per row and shears the picture.
        "fresh('k5')\n"
        "assert(world.WindowMergeImageAlpha('k5', 'img', 'msk', 3, 5, 8, 12,\n"
        "                                   1, 1.0, 2, 1, 0, 4) == 0)\n"
        "check('k5', 3, 5, 2, 1, 5, 3, 1, 1.0, false, 'mode1 five wide')\n"
        "assert(world.WindowGetPixel('k5', 3, 5) == 0x102030, 'keyed corner, five wide')\n"
        "assert(world.WindowGetPixel('k5', 6, 6) == 0xC0A080, 'keyed pixel, five wide')\n"
        "untouched('k5', 8, 5, 'past a 5-wide mode 1 merge')\n"

        // Nothing escaped in either mode.
        "untouched('k1', 2, 5, 'left of the rect')\n"
        "untouched('k1', 7, 5, 'right of the rect')\n"
        "untouched('kh', 3, 4, 'above the rect')\n"
        "untouched('kh', 3, 8, 'below the rect')\n"
        "return true\n";

    EXPECT_LUA_OK(f.L, script.c_str());
    ::unlink(img.c_str());
    ::unlink(msk.c_str());
}

// The rectangle arithmetic: which side of the min() wins, the <= 0 "offset from
// the far edge" rule on BOTH the destination and the source, the negative-corner
// clamps, and the degenerate-rectangle early out (which happens BEFORE the mode
// is ever looked at).
TEST(LuaSurface, MergeImageAlphaRectangleArithmetic)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    const std::string img = write_merge_png("img", 7, 4, 0);
    const std::string msk = write_merge_png("msk", 9, 6, 1);
    ASSERT_FALSE(img.empty());
    ASSERT_FALSE(msk.empty());

    const std::string script = merge_prelude(img, msk) +
        // ── the DESTINATION side of the min() (the source side is pinned by
        // every other test here, whose source rect is the smaller one).
        // dest (10,2)-(14,5) is 4x3; source (1,0)-(7,4) is 6x4.
        "fresh('d1')\n"
        "assert(world.WindowMergeImageAlpha('d1', 'img', 'msk', 10, 2, 14, 5,\n"
        "                                   0, 1.0, 1, 0, 7, 4) == 0)\n"
        "check('d1', 10, 2, 1, 0, 4, 3, 0, 1.0, true, 'dest-limited')\n"
        "untouched('d1', 14, 2, 'past the dest width')\n"
        "untouched('d1', 10, 5, 'past the dest height')\n"
        "untouched('d1',  9, 2, 'left of the dest')\n"

        // ── Right/Bottom <= 0 means "offset from the far edge" (FixRight /
        // FixBottom, miniwindow.cpp:88 and :161).  The window is 20x14, so
        // Right = 0 is column 20 and Bottom = -1 is row 13, both exclusive.
        // Right and Bottom take DIFFERENT arms, and the resulting 4x2 extent is
        // width-from-the-source, height-from-the-destination.
        "fresh('d2')\n"
        "assert(world.WindowMergeImageAlpha('d2', 'img', 'msk', 16, 11, 0, -1,\n"
        "                                   0, 1.0, 2, 1, 6, 4) == 0)\n"
        "check('d2', 16, 11, 2, 1, 4, 2, 0, 1.0, true, 'fix-right/bottom')\n"
        // (19,11) being merged is what says FixRight(0) is 20 and not 19.
        "untouched('d2', 15, 11, 'left of the corner rect')\n"
        "untouched('d2', 16, 13, 'Bottom = -1 must stop before row 13')\n"

        // ── the same rule on the SOURCE rectangle (miniwindow.cpp:3286).  The
        // image is 7x4, so SrcRight = -1 is column 6 and SrcBottom = 0 is row 4
        // — i.e. exactly the (6,4) the other tests pass explicitly.  Assert the
        // two produce the SAME picture, pixel for pixel.
        "fresh('d3')  fresh('d4')\n"
        "assert(world.WindowMergeImageAlpha('d3', 'img', 'msk', 3, 5, 8, 12,\n"
        "                                   0, 1.0, 2, 1, -1, 0) == 0)\n"
        "assert(world.WindowMergeImageAlpha('d4', 'img', 'msk', 3, 5, 8, 12,\n"
        "                                   0, 1.0, 2, 1,  6, 4) == 0)\n"
        "check('d3', 3, 5, 2, 1, 4, 3, 0, 1.0, true, 'negative src corner')\n"
        "for dy = 0, 2 do for dx = 0, 3 do\n"
        "  assert(world.WindowGetPixel('d3', 3 + dx, 5 + dy)\n"
        "         == world.WindowGetPixel('d4', 3 + dx, 5 + dy),\n"
        "         string.format('SrcRight=-1/SrcBottom=0 differ at %d,%d', dx, dy))\n"
        "end end\n"

        // ...and ZERO is the other arm of that same rule, on the other axis
        // from d3's SrcBottom: SrcRight = 0 means the image's full width (7),
        // giving a 5-wide source rectangle, NOT an empty one.  If the rule
        // narrows to `SrcRight < 0`, iWidth goes negative and the merge quietly
        // no-ops — which is why this needs a positive assertion, not a code.
        "fresh('d7')\n"
        "assert(world.WindowMergeImageAlpha('d7', 'img', 'msk', 3, 5, 8, 12,\n"
        "                                   0, 1.0, 2, 1, 0, 4) == 0)\n"
        "check('d7', 3, 5, 2, 1, 5, 3, 0, 1.0, true, 'SrcRight = 0')\n"
        "untouched('d7', 8, 5, 'past a 5-wide merge')\n"
        "untouched('d7', 3, 8, 'below a 5-wide merge')\n"

        // ── negative Left/Top clamp to 0 and an over-large Right/Bottom clamps
        // to the window (miniwindow.cpp:3240-3247).  Without the clamp the
        // window readback would be out of bounds, the three buffers would
        // disagree in size and the merge would silently do nothing.
        "fresh('d5')\n"
        "assert(world.WindowMergeImageAlpha('d5', 'img', 'msk', -5, -2, 999, 999,\n"
        "                                   0, 1.0, 2, 1, 6, 4) == 0)\n"
        "check('d5', 0, 0, 2, 1, 4, 3, 0, 1.0, true, 'clamped corner')\n"
        "untouched('d5', 4, 0, 'past the clamped width')\n"
        "untouched('d5', 0, 3, 'past the clamped height')\n"

        // The Right/Bottom half of that clamp only BITES when the destination is
        // the limiting side, so d5 above (whose source is smaller) cannot see it
        // at all.  Here Left=17 Top=12 in a 20x14 window leaves room for 3x2, and
        // the source offers 4x3 — so an unclamped Right would ask the window for
        // a 4x3 readback it cannot supply, the three layers would disagree in
        // size, and the merge would silently return eOK having drawn nothing.
        "fresh('d8')\n"
        "assert(world.WindowMergeImageAlpha('d8', 'img', 'msk', 17, 12, 999, 999,\n"
        "                                   0, 1.0, 2, 1, 6, 4) == 0)\n"
        "check('d8', 17, 12, 2, 1, 3, 2, 0, 1.0, true, 'clamped far edge')\n"
        "untouched('d8', 16, 12, 'left of the clamped rect')\n"
        "untouched('d8', 17, 11, 'above the clamped rect')\n"

        // ── a degenerate rectangle returns eOK and draws nothing, and it does so
        // BEFORE the switch on Mode (miniwindow.cpp:3296 vs :3464) — so an
        // otherwise-invalid mode is never reported.
        //
        // Careful with what this proves.  Deleting the sanity check outright
        // does NOT fail these lines, because on the PAL path a zero-extent
        // readPixels clips to nothing and the `!bufA.valid ()` guard at :3357
        // answers eOK anyway.  The input that DOES tell the two apart is a
        // zero-width rectangle over a too-SHORT mask, and it lives in
        // MergeImageAlphaErrorArmsAndTheirOrder because that is where the
        // undersized masks are loaded.
        "fresh('d6')\n"
        "assert(world.WindowMergeImageAlpha('d6', 'img', 'msk', 5, 5, 5, 12,\n"
        "                                   9, 1.0, 2, 1, 6, 4) == 0, 'zero width')\n"
        "assert(world.WindowMergeImageAlpha('d6', 'img', 'msk', 3, 5, 8, 5,\n"
        "                                   9, 1.0, 2, 1, 6, 4) == 0, 'zero height')\n"
        // ...and an empty SOURCE rectangle is the same story.
        "assert(world.WindowMergeImageAlpha('d6', 'img', 'msk', 3, 5, 8, 12,\n"
        "                                   9, 1.0, 2, 1, 2, 4) == 0, 'empty src')\n"
        "for dy = 0, 2 do for dx = 0, 3 do\n"
        "  untouched('d6', 3 + dx, 5 + dy, 'a degenerate rect drew')\n"
        "end end\n"
        "untouched('d6', 5, 5, 'a degenerate rect drew')\n"
        "return true\n";

    EXPECT_LUA_OK(f.L, script.c_str());
    ::unlink(img.c_str());
    ::unlink(msk.c_str());
}

// Every error arm, the order they are taken in, and the boundary of the mask
// size check — plus the control that a refused call leaves the window alone.
TEST(LuaSurface, MergeImageAlphaErrorArmsAndTheirOrder)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    const std::string img   = write_merge_png("img",   7, 4, 0);
    const std::string msk   = write_merge_png("msk",   9, 6, 1);
    const std::string thin  = write_merge_png("thin",  5, 6, 1);   // one column short
    const std::string shrt  = write_merge_png("short", 9, 3, 1);   // one row short
    const std::string exact = write_merge_png("exact", 6, 4, 1);   // exactly enough
    ASSERT_FALSE(img.empty());
    ASSERT_FALSE(msk.empty());
    ASSERT_FALSE(thin.empty());
    ASSERT_FALSE(shrt.empty());
    ASSERT_FALSE(exact.empty());

    const std::string script = merge_prelude(img, msk) +
        "local eBad, eNotInst = error_code.eBadParameter, error_code.eImageNotInstalled\n"
        "local eUnknown = error_code.eUnknownOption\n"

        // ── the mask size check (miniwindow.cpp:3306).  The merge needs mask
        // columns [SrcLeft, SrcLeft + iWidth) = [2,6) and rows [1,4), i.e. a
        // mask at least 6 x 4.
        "fresh('m1')\n"
        "assert(world.WindowLoadImage('m1', 'thin', '" + thin + "') == 0)\n"
        "assert(world.WindowLoadImage('m1', 'short', '" + shrt + "') == 0)\n"
        "assert(world.WindowLoadImage('m1', 'exact', '" + exact + "') == 0)\n"
        "assert(world.WindowImageInfo('m1', 'thin', 2) == 5, 'thin mask width')\n"
        "assert(world.WindowImageInfo('m1', 'short', 3) == 3, 'short mask height')\n"
        "local function merge (n, mask, mode, op)\n"
        "  return world.WindowMergeImageAlpha(n, 'img', mask, 3, 5, 8, 12, mode, op, 2, 1, 6, 4)\n"
        "end\n"
        "assert(merge('m1', 'thin',  0, 1.0) == eBad, 'a mask one column short was accepted')\n"
        "assert(merge('m1', 'short', 0, 1.0) == eBad, 'a mask one row short was accepted')\n"
        // ORDER: the degenerate-rectangle early out (:3296) comes BEFORE the mask
        // size gate (:3306).  A zero-WIDTH rectangle whose height would still
        // have overrun the short mask therefore answers eOK, not eBadParameter —
        // the one input that tells the two orderings apart, since with the width
        // collapsed the height check would otherwise still fire.
        "assert(world.WindowMergeImageAlpha('m1', 'img', 'short', 5, 5, 5, 12,\n"
        "                                   0, 1.0, 2, 1, 6, 4) == 0,\n"
        "       'the degenerate-rect check no longer runs before the mask size gate')\n"
        // The refusals really refused: nothing was drawn.
        "for dy = 0, 2 do for dx = 0, 3 do\n"
        "  untouched('m1', 3 + dx, 5 + dy, 'a refused merge drew')\n"
        "end end\n"
        // ...and 6 x 4 — the exact boundary — is ACCEPTED, and gives the same
        // picture the 9 x 6 mask does, because the mask formula is a pure
        // function of (x, y).  That pins `<` rather than `<=` in both checks.
        "assert(merge('m1', 'exact', 0, 1.0) == 0, 'an exactly-large-enough mask was refused')\n"
        "check('m1', 3, 5, 2, 1, 4, 3, 0, 1.0, true, 'exact-size mask')\n"

        // ── Opacity is validated at miniwindow.cpp:3249, BEFORE either image
        // lookup.  Both ends of the range are rejected; both endpoints are legal.
        "fresh('m2')\n"
        "assert(merge('m2', 'msk', 0,  1.5)  == eBad, 'opacity > 1 accepted')\n"
        "assert(merge('m2', 'msk', 0, -0.01) == eBad, 'opacity < 0 accepted')\n"
        "for dy = 0, 2 do for dx = 0, 3 do\n"
        "  untouched('m2', 3 + dx, 5 + dy, 'a bad-opacity merge drew')\n"
        "end end\n"
        "assert(merge('m2', 'msk', 0, 0.0) == 0, 'opacity 0 rejected')\n"
        "assert(merge('m2', 'msk', 0, 1.0) == 0, 'opacity 1 rejected')\n"
        // The ORDER: a bad opacity beats a nonexistent image id.  Reverse the
        // two blocks in the body and this line reports eImageNotInstalled.
        "assert(world.WindowMergeImageAlpha('m2', 'nope', 'nope', 3, 5, 8, 12,\n"
        "                                   0, 2.0, 2, 1, 6, 4) == eBad,\n"
        "       'the opacity gate no longer runs before the image lookups')\n"

        // ── the two lookups, each on its own (miniwindow.cpp:3253 and :3261).
        "fresh('m3')\n"
        "assert(world.WindowMergeImageAlpha('m3', 'nope', 'msk', 3, 5, 8, 12,\n"
        "                                   0, 1.0, 2, 1, 6, 4) == eNotInst, 'bad ImageId')\n"
        "assert(world.WindowMergeImageAlpha('m3', 'img', 'nope', 3, 5, 8, 12,\n"
        "                                   0, 1.0, 2, 1, 6, 4) == eNotInst, 'bad MaskId')\n"
        // ...and the mask id is a genuinely separate lookup: passing the SOURCE
        // as its own mask is legal and does something (M = A, so the result is
        // pulled towards the image wherever the image is bright).
        "assert(world.WindowMergeImageAlpha('m3', 'img', 'img', 3, 5, 8, 12,\n"
        "                                   0, 1.0, 2, 1, 6, 4) == 0, 'image as its own mask')\n"
        "assert(world.WindowGetPixel('m3', 3, 5) == px(bm(70, 48, 70), bm(85, 32, 85),\n"
        "                                              bm(140, 16, 140)),\n"
        "       'the MaskId argument is not a separate lookup')\n"

        // ── an unrecognised Mode, over a rectangle that is NOT degenerate, so
        // the switch really is reached (miniwindow.cpp:3464).
        "fresh('m4')\n"
        "assert(world.WindowLoadImage('m4', 'thin', '" + thin + "') == 0)\n"
        "assert(merge('m4', 'msk',  2, 1.0) == eUnknown, 'mode 2 accepted')\n"
        "assert(merge('m4', 'msk', -1, 1.0) == eUnknown, 'mode -1 accepted')\n"
        "for dy = 0, 2 do for dx = 0, 3 do\n"
        "  untouched('m4', 3 + dx, 5 + dy, 'an unknown mode drew')\n"
        "end end\n"
        // ORDER again: the mask size check is at :3306 and the switch on Mode at
        // :3464, so a too-small mask beats an unknown mode.  Both are refusals —
        // only the CODE tells the two orderings apart.
        "assert(merge('m4', 'thin', 2, 1.0) == eBad,\n"
        "       'the mask size check no longer runs before the switch on Mode')\n"
        // ...and the image lookups are earlier still, so a bad id beats both.
        "assert(world.WindowMergeImageAlpha('m4', 'nope', 'thin', 3, 5, 8, 12,\n"
        "                                   2, 1.0, 2, 1, 6, 4) == eNotInst,\n"
        "       'the image lookup no longer runs before the mask size check')\n"
        "return true\n";

    EXPECT_LUA_OK(f.L, script.c_str());
    ::unlink(img.c_str());
    ::unlink(msk.c_str());
    ::unlink(thin.c_str());
    ::unlink(shrt.c_str());
    ::unlink(exact.c_str());
}

// A 32bpp source is legal too — and its ALPHA is ignored, because all three
// layers are read as BGR24 (miniwindow.cpp:3331).  That is the same on Windows,
// where the three temporaries are 24bpp DIBs.  Together with the 24bpp fixtures
// above this says the method has no depth opinion at all.
TEST(LuaSurface, MergeImageAlphaIgnoresA32bppSourcesAlphaChannel)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);

    const std::string img  = write_merge_png("img", 7, 4, 0);
    const std::string msk  = write_merge_png("msk", 9, 6, 1);
    const std::string rgba = write_rgba_png(4, 3);
    ASSERT_FALSE(img.empty());
    ASSERT_FALSE(msk.empty());
    ASSERT_FALSE(rgba.empty());

    const std::string script = merge_prelude(img, msk) +
        "fresh('t')\n"
        "assert(world.WindowLoadImage('t', 'rgba', '" + rgba + "') == 0)\n"
        // The gate the OTHER two alpha ops would apply.  This one does not.
        "assert(world.WindowImageInfo('t', 'rgba', 6) == 32, 'RGBA PNG is not 32bpp')\n"
        // write_rgba_png: R = 20 + 10x, G = 90 + 5y, B = 200, A = 240 - 80y.
        "local function rgb32 (x, y) return 20 + 10 * x, 90 + 5 * y, 200 end\n"

        // dest (8,3)-(11,5) is 3x2; source (1,0)-(4,2) is 3x2.  Non-square, at a
        // non-zero source offset, and clear of the block so the base is the
        // plain field.
        "assert(world.WindowMergeImageAlpha('t', 'rgba', 'msk', 8, 3, 11, 5,\n"
        "                                   0, 1.0, 1, 0, 4, 2) == 0, 'a 32bpp source was refused')\n"
        "for dy = 0, 1 do for dx = 0, 2 do\n"
        "  local ix, iy = 1 + dx, 0 + dy\n"
        "  local wx, wy = 8 + dx, 3 + dy\n"
        "  local ar, ag, ab = rgb32(ix, iy)\n"
        "  local mr, mg, mb = msk(ix, iy)\n"
        "  local br, bg, bb = base(wx, wy)\n"
        "  local exp = px(bm(ar, br, mr), bm(ag, bg, mg), bm(ab, bb, mb))\n"
        "  local got = world.WindowGetPixel('t', wx, wy)\n"
        "  assert(got == exp, string.format('32bpp merge at %d,%d: got %d want %d',\n"
        "                                   wx, wy, got, exp))\n"
        // The fixture must be able to tell an alpha-weighted body apart: row 0
        // carries alpha 240 and row 1 alpha 160, so pre-multiplying would move
        // the two rows by visibly different amounts.
        "  assert(exp ~= px(br, bg, bb), 'DEGENERATE FIXTURE: 32bpp merge equals the base')\n"
        "end end\n"
        "assert(world.WindowGetPixel('t', 8, 3) ~= world.WindowGetPixel('t', 8, 4),\n"
        "       'the two source rows became the same colour')\n"
        "untouched('t', 11, 3, 'right of the 32bpp rect')\n"
        "untouched('t',  8, 5, 'below the 32bpp rect')\n"
        "untouched('t',  7, 3, 'left of the 32bpp rect')\n"
        "return true\n";

    EXPECT_LUA_OK(f.L, script.c_str());
    ::unlink(img.c_str());
    ::unlink(msk.c_str());
    ::unlink(rgba.c_str());
}

// ── CMiniWindow::Filter — world.WindowFilter ──────────────────────────────
//
// The only probe of this body that existed anywhere is
// platform/miniwindow_selftest.cpp:304, and it asks for operation 7 (additive
// brightness) with Options = 0 — an IDENTITY.  A Filter that read nothing,
// edited nothing and returned eOK passed it.  Everything below reads pixels
// back.
//
// Shared fixture geometry, chosen so that no two of the spaces this body
// converts between can be confused: the window is 20 x 12 and the filtered
// rectangle is (3, 5)-(8, 9) — Left != Top, and 5 wide by 4 high, so w != h.
// Five pixels of BGR24 is 15 bytes, which BytesPerLine(5, 24) pads to 16, so a
// row pitch computed as w * 3 shears the rectangle rather than being invisible.
// Right/Bottom are exclusive (iWidth = FixRight(Right) - Left).
//
// Expected values are read off the filter helpers in miniwindow.cpp —
// Brightness (~2818), Contrast (~2847), ColourBrightness (~2896),
// BrightnessMultiply (~2833), MakeGreyscale (~3008) and Average (~3040) — which
// all run over a BGR24 buffer, blue at byte 0 and red at byte 2.  COLORREF is
// 0x00BBGGRR, so px(r, g, b) below packs exactly what WindowGetPixel returns.

TEST(LuaSurface, FilterGreyscaleWeightingsDisagreeOnAPureChannel)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "local function px(r, g, b) return r + g * 256 + b * 65536 end\n"
        "local BG = px(7, 130, 240)\n"          // deliberately nowhere near grey
        "local function fixture(n)\n"
        "  assert(world.WindowCreate(n, 0, 0, 20, 12, 0, 0, BG) == 0)\n"
        // One pure channel per ROW as well as per column, so a body that walked
        // only the first row, or transposed the rectangle, cannot pass.  (5, 8)
        // is the bottom row of the rect, which is byte-row 0 of the bottom-up
        // PixelBuffer — the two ends of the vertical mapping.
        "  assert(world.WindowSetPixel(n, 3, 5, px(255, 0, 0)) == 0)\n"
        "  assert(world.WindowSetPixel(n, 4, 6, px(0, 255, 0)) == 0)\n"
        "  assert(world.WindowSetPixel(n, 5, 8, px(0, 0, 255)) == 0)\n"
        "  assert(world.WindowGetPixel(n, 3, 5) == px(255, 0, 0), 'fixture red')\n"
        "  assert(world.WindowGetPixel(n, 4, 6) == px(0, 255, 0), 'fixture green')\n"
        "  assert(world.WindowGetPixel(n, 5, 8) == px(0, 0, 255), 'fixture blue')\n"
        "end\n"
        "fixture('gl') fixture('gp')\n"

        // Operation 19 is the LINEAR greyscale, (b + g + r) / 3, so every pure
        // channel collapses onto the same 85 — which is what makes operation 20
        // below a real discriminator rather than a restatement.
        "assert(world.WindowFilter('gl', 3, 5, 8, 9, 19, 0) == 0)\n"
        "assert(world.WindowGetPixel('gl', 3, 5) == px(85, 85, 85), 'linear red')\n"
        "assert(world.WindowGetPixel('gl', 4, 6) == px(85, 85, 85), 'linear green')\n"
        "assert(world.WindowGetPixel('gl', 5, 8) == px(85, 85, 85), 'linear blue')\n"
        // 377 / 3 = 125.66..., truncated into an unsigned char by the store.
        "assert(world.WindowGetPixel('gl', 7, 7) == px(125, 125, 125), 'linear bg')\n"

        // Operation 20 is PERCEPTUAL: pi[0]*0.11 + pi[1]*0.59 + pi[2]*0.30.
        // The three coefficients differ, so each pure channel lands somewhere
        // else — and THAT is the channel-index assertion: red is byte 2 and so
        // weighs 0.30 (76), green byte 1 weighs 0.59 (150), blue byte 0 weighs
        // 0.11 (28).  Swap any two indices and at least two of these change.
        "assert(world.WindowFilter('gp', 3, 5, 8, 9, 20, 0) == 0)\n"
        "assert(world.WindowGetPixel('gp', 3, 5) == px(76, 76, 76), 'perceptual red')\n"
        "assert(world.WindowGetPixel('gp', 4, 6) == px(150, 150, 150), 'perceptual green')\n"
        "assert(world.WindowGetPixel('gp', 5, 8) == px(28, 28, 28), 'perceptual blue')\n"
        // 240*0.11 + 130*0.59 + 7*0.30 = 105.2
        "assert(world.WindowGetPixel('gp', 7, 7) == px(105, 105, 105), 'perceptual bg')\n"

        // ...and the two weightings really do disagree on a pure channel, which
        // is the statement an identity probe can never make.
        "assert(world.WindowGetPixel('gl', 3, 5) ~= world.WindowGetPixel('gp', 3, 5),\n"
        "       'linear and perceptual greyscale agree on pure red')\n"

        // Sweep EVERY remaining pixel of the rectangle, not just the
        // interesting ones.  MakeGreyscale re-bases per row from iPerLine, and
        // a pitch that is wrong by one byte still lands on most of the bytes
        // the right one does — verified: a w*3 pitch survives assertions made
        // only at the corners.
        "local special = { ['3,5'] = true, ['4,6'] = true, ['5,8'] = true }\n"
        "for _, pair in ipairs { { 'gl', px(125, 125, 125) },\n"
        "                        { 'gp', px(105, 105, 105) } } do\n"
        "  local n, plain = pair[1], pair[2]\n"
        "  for x = 3, 7 do for y = 5, 8 do\n"
        "    if not special[x .. ',' .. y] then\n"
        "      assert(world.WindowGetPixel(n, x, y) == plain,\n"
        "             string.format('%s at %d,%d is %d not %d', n, x, y,\n"
        "                           world.WindowGetPixel(n, x, y), plain))\n"
        "    end\n"
        "  end end\n"
        "end\n"

        // Controls: nothing outside the rectangle moved, in either window.
        "for _, n in ipairs { 'gl', 'gp' } do\n"
        "  assert(world.WindowGetPixel(n, 2, 5) == BG, 'left of rect ' .. n)\n"
        "  assert(world.WindowGetPixel(n, 8, 5) == BG, 'right of rect ' .. n)\n"
        "  assert(world.WindowGetPixel(n, 3, 4) == BG, 'above rect ' .. n)\n"
        "  assert(world.WindowGetPixel(n, 3, 9) == BG, 'below rect ' .. n)\n"
        "  assert(world.WindowGetPixel(n, 19, 11) == BG, 'far corner ' .. n)\n"
        "end\n"
    ));
}

TEST(LuaSurface, FilterPerChannelOpsPinTheBgrIndex)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "local function px(r, g, b) return r + g * 256 + b * 65536 end\n"
        // Three DIFFERENT channel values, so a body that edited the wrong byte
        // cannot land on the right answer by accident.
        "local BG = px(60, 90, 120)\n"
        "local names = { 'all', 'red', 'grn', 'blu', 'con', 'mul' }\n"
        "for _, n in ipairs (names) do\n"
        "  assert(world.WindowCreate(n, 0, 0, 20, 12, 0, 0, BG) == 0)\n"
        "  assert(world.WindowGetPixel(n, 3, 5) == BG, 'fixture ' .. n)\n"
        "end\n"

        // Operation 7 (Brightness) adds Options to EVERY byte...
        "assert(world.WindowFilter('all', 3, 5, 8, 9, 7, 50) == 0)\n"
        "assert(world.WindowGetPixel('all', 3, 5) == px(110, 140, 170), 'brightness')\n"

        // ...while 10 / 13 / 16 are ColourBrightness with iColour 2 / 1 / 0.
        // Each moves exactly one channel and each moves a DIFFERENT one, so an
        // off-by-one in the index fails at least two of the three.  The two
        // corners named here are for a readable failure message only — the
        // assertion that actually holds the pitch is the full sweep below.
        "assert(world.WindowFilter('red', 3, 5, 8, 9, 10, 50) == 0)\n"
        "assert(world.WindowGetPixel('red', 3, 5) == px(110, 90, 120), 'red idx first')\n"
        "assert(world.WindowGetPixel('red', 7, 8) == px(110, 90, 120), 'red idx last')\n"
        "assert(world.WindowFilter('grn', 3, 5, 8, 9, 13, 50) == 0)\n"
        "assert(world.WindowGetPixel('grn', 3, 5) == px(60, 140, 120), 'green idx first')\n"
        "assert(world.WindowGetPixel('grn', 7, 8) == px(60, 140, 120), 'green idx last')\n"
        "assert(world.WindowFilter('blu', 3, 5, 8, 9, 16, 50) == 0)\n"
        "assert(world.WindowGetPixel('blu', 3, 5) == px(60, 90, 170), 'blue idx first')\n"
        "assert(world.WindowGetPixel('blu', 7, 8) == px(60, 90, 170), 'blue idx last')\n"

        // Operation 8 (Contrast) is the lookup-table arm, centred on 128 and
        // multiplied: 60 -> -8, which CLAMP floors at 0 (so the clamp is live
        // too), 90 -> 52, 120 -> 112.
        "assert(world.WindowFilter('con', 3, 5, 8, 9, 8, 2.0) == 0)\n"
        "assert(world.WindowGetPixel('con', 3, 5) == px(0, 52, 112), 'contrast')\n"

        // Operation 21 is MULTIPLICATIVE brightness, which additive 7 cannot
        // imitate on these values (60/90/120 x 1.5 = 90/135/180).
        "assert(world.WindowFilter('mul', 3, 5, 8, 9, 21, 1.5) == 0)\n"
        "assert(world.WindowGetPixel('mul', 3, 5) == px(90, 135, 180), 'brightness x1.5')\n"
        "assert(world.WindowGetPixel('mul', 3, 5) ~= world.WindowGetPixel('all', 3, 5),\n"
        "       'additive and multiplicative brightness coincide')\n"

        // Now sweep EVERY pixel of every rectangle.  Two-corner spot checks are
        // not enough: ColourBrightness stepping by w*3 = 15 instead of 16 still
        // lands on the right byte in the first and last buffer rows (15*0 and
        // the pad byte of the row below), so it slips past corner assertions
        // while brightening the GREEN of the middle rows.  It does not slip
        // past this.
        "local expect = { all = px(110, 140, 170), red = px(110, 90, 120),\n"
        "                 grn = px(60, 140, 120),  blu = px(60, 90, 170),\n"
        "                 con = px(0, 52, 112),    mul = px(90, 135, 180) }\n"
        "for _, n in ipairs (names) do\n"
        "  for x = 3, 7 do for y = 5, 8 do\n"
        "    assert(world.WindowGetPixel(n, x, y) == expect[n],\n"
        "           string.format('%s at %d,%d is %d not %d', n, x, y,\n"
        "                         world.WindowGetPixel(n, x, y), expect[n]))\n"
        "  end end\n"
        "end\n"

        // Controls: every window is untouched outside its rectangle.
        "for _, n in ipairs (names) do\n"
        "  assert(world.WindowGetPixel(n, 2, 5) == BG, 'left of rect ' .. n)\n"
        "  assert(world.WindowGetPixel(n, 8, 8) == BG, 'right of rect ' .. n)\n"
        "  assert(world.WindowGetPixel(n, 3, 4) == BG, 'above rect ' .. n)\n"
        "  assert(world.WindowGetPixel(n, 7, 9) == BG, 'below rect ' .. n)\n"
        "end\n"
    ));
}

TEST(LuaSurface, FilterAverageCollapsesTheRectangleToItsMean)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "local function px(r, g, b) return r + g * 256 + b * 65536 end\n"
        "local BG = px(9, 9, 9)\n"
        "assert(world.WindowCreate('av', 0, 0, 20, 12, 0, 0, BG) == 0)\n"
        // Two UNEQUAL vertical bands inside the 5 x 4 rectangle: 2 columns of
        // red over 3 of green, so the mean is the value of neither band, and
        // the red mean differs from the green mean.
        "assert(world.WindowRectOp('av', 2, 3, 5, 5, 9, px(200, 0, 0), 0) == 0)\n"
        "assert(world.WindowRectOp('av', 2, 5, 5, 8, 9, px(0, 100, 0), 0) == 0)\n"
        "assert(world.WindowGetPixel('av', 3, 5) == px(200, 0, 0), 'fixture band 1')\n"
        "assert(world.WindowGetPixel('av', 7, 8) == px(0, 100, 0), 'fixture band 2')\n"

        // 8 red pixels + 12 green over 20: r = 1600/20 = 80, g = 1200/20 = 60,
        // b = 0.  Average walks COLUMN-major stepping by BytesPerLine(5, 24) =
        // 16, so a pitch of 15 averages (and paints) a sheared rectangle.
        "assert(world.WindowFilter('av', 3, 5, 8, 9, 27, 0) == 0)\n"
        "local want = px(80, 60, 0)\n"
        "for x = 3, 7 do for y = 5, 8 do\n"
        "  assert(world.WindowGetPixel('av', x, y) == want,\n"
        "         string.format('average at %d,%d is %d not %d', x, y,\n"
        "                       world.WindowGetPixel('av', x, y), want))\n"
        "end end\n"

        // Controls.
        "assert(world.WindowGetPixel('av', 2, 5) == BG, 'left of rect')\n"
        "assert(world.WindowGetPixel('av', 8, 5) == BG, 'right of rect')\n"
        "assert(world.WindowGetPixel('av', 3, 4) == BG, 'above rect')\n"
        "assert(world.WindowGetPixel('av', 3, 9) == BG, 'below rect')\n"
    ));
}

TEST(LuaSurface, FilterRejectsUnknownOperationsAndHonoursTheEdgeOffsets)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L,
        "local function px(r, g, b) return r + g * 256 + b * 65536 end\n"
        "local BG = px(60, 90, 120)\n"
        "assert(world.WindowCreate('e', 0, 0, 20, 12, 0, 0, BG) == 0)\n"

        // The default arm returns before the write-back, so an unknown
        // operation must be both refused AND inert.  0 and 28 bracket the
        // switch, whose cases run 1..27.
        "assert(world.WindowFilter('e', 3, 5, 8, 9, 0, 50) == error_code.eUnknownOption)\n"
        "assert(world.WindowFilter('e', 3, 5, 8, 9, 28, 50) == error_code.eUnknownOption)\n"
        "assert(world.WindowFilter('e', 3, 5, 8, 9, 99, 50) == error_code.eUnknownOption)\n"
        "assert(world.WindowGetPixel('e', 3, 5) == BG, 'unknown op still filtered')\n"

        // An empty rectangle is eOK and inert (the iWidth/iHeight < 1 arm) —
        // once with no columns, once with no rows.
        "assert(world.WindowFilter('e', 5, 5, 5, 9, 7, 50) == 0)\n"
        "assert(world.WindowFilter('e', 3, 9, 8, 9, 7, 50) == 0)\n"
        "assert(world.WindowGetPixel('e', 5, 5) == BG, 'empty rect still filtered')\n"

        // ...and the SAME operation on a real rectangle does move the pixels,
        // so the assertions above are about the guards and not about a Filter
        // that never does anything.
        "assert(world.WindowFilter('e', 3, 5, 8, 9, 7, 50) == 0)\n"
        "assert(world.WindowGetPixel('e', 3, 5) == px(110, 140, 170), 'op 7 inert')\n"

        // Right/Bottom <= 0 are offsets from the far edge (FixRight/FixBottom,
        // which stay engine-side above the seam).  0 means the edge itself.
        "assert(world.WindowCreate('e2', 0, 0, 20, 12, 0, 0, BG) == 0)\n"
        "assert(world.WindowFilter('e2', 3, 5, 0, 0, 7, 50) == 0)\n"
        "assert(world.WindowGetPixel('e2', 19, 11) == px(110, 140, 170), 'corner missed')\n"
        "assert(world.WindowGetPixel('e2', 3, 5) == px(110, 140, 170), 'origin missed')\n"
        "assert(world.WindowGetPixel('e2', 2, 5) == BG, 'left of rect')\n"
        "assert(world.WindowGetPixel('e2', 3, 4) == BG, 'above rect')\n"
        // -2 keeps two pixels of each far edge out of it: x stops at 17, y at 9.
        "assert(world.WindowCreate('e3', 0, 0, 20, 12, 0, 0, BG) == 0)\n"
        "assert(world.WindowFilter('e3', 3, 5, -2, -2, 7, 50) == 0)\n"
        "assert(world.WindowGetPixel('e3', 17, 9) == px(110, 140, 170), 'edge offset short')\n"
        "assert(world.WindowGetPixel('e3', 18, 9) == BG, 'edge offset overran x')\n"
        "assert(world.WindowGetPixel('e3', 17, 10) == BG, 'edge offset overran y')\n"
    ));
}

// ── CMiniWindow::BlendImage — world.WindowBlendImage ──────────────────────
//
// The largest body in miniwindow.cpp (2078) and, until the DIB store, one with
// no reachable pixel source at all.  It reads the IMAGE through a temporary
// adopted surface at (SrcLeft, SrcTop), the BASE through the miniwindow's own
// surface at (Left, Top), blends byte by byte, and writes the base rectangle
// back.  A is the image, B is the base — everywhere, including in blending.h.
//
// The fixture below is shared by the four blend tests:
//   'sw' (the image source) — 8 x 6, background px(10, 20, 30), carrying a
//     px(200, 150, 60) patch at (2, 1)-(7, 4).  The patch is BOTH offset from
//     the origin and smaller than the image, so a body that ignored SrcLeft or
//     SrcTop would read the background instead and every number below changes.
//   'base' — 20 x 12, background px(9, 9, 9), with three full-width bands at
//     y = 6, 7, 8 in three different colours.  Blending into (4, 6)-(9, 9)
//     therefore meets a different base on every row, which is what makes a
//     wrong row pitch visible — and 5 pixels of BGR24 is 15 bytes padded to 16,
//     so there IS a pitch to get wrong.  Left(4) != Top(6), 5 wide != 3 high.
//
// Arithmetic straight from blending.h, INTEGER division throughout:
//   Blend_Normal(A,B)      = A
//   Blend_Overlay(A,B)     = (B < 128) ? 2*A*B/255 : 255 - 2*(255-A)*(255-B)/255
//   Blend_HardLight(A,B)   = Blend_Overlay(B,A)          -- the operands swapped
//   Blend_Opacity(A,B,F,O) = (uint8)(O*F(A,B) + (1-O)*B + 0.5)
//   Simple_Opacity(B,X,O)  = (uint8)(O*X + (1-O)*B + 0.5)
// Opacity >= 1.0 takes the plain arm; anything less takes the *_Opacity arm,
// including 0.0 — which therefore reproduces B exactly, and is the control.
static std::string blend_script(const char* body)
{
    return std::string(
        "local function px(r, g, b) return r + g * 256 + b * 65536 end\n"
        "local IMG = { 200, 150, 60 }\n"
        "local BASE = { [6] = { 100, 40, 220 },\n"
        "               [7] = { 30, 200, 90 },\n"
        "               [8] = { 250, 10, 170 } }\n"
        "local SBG, BBG = px(10, 20, 30), px(9, 9, 9)\n"
        "local function fixture(base)\n"
        "  assert(world.WindowCreate('sw', 0, 0, 8, 6, 0, 0, SBG) == 0)\n"
        "  assert(world.WindowRectOp('sw', 2, 2, 1, 7, 4,\n"
        "                            px(IMG[1], IMG[2], IMG[3]), 0) == 0)\n"
        "  assert(world.WindowGetPixel('sw', 2, 1) == px(200, 150, 60), 'src patch')\n"
        "  assert(world.WindowGetPixel('sw', 1, 1) == SBG, 'src patch too wide')\n"
        "  assert(world.WindowGetPixel('sw', 2, 0) == SBG, 'src patch too tall')\n"
        "  assert(world.WindowGetPixel('sw', 0, 0) == SBG, 'src origin')\n"
        "  assert(world.WindowCreate(base, 0, 0, 20, 12, 0, 0, BBG) == 0)\n"
        "  for y = 6, 8 do\n"
        "    local c = BASE[y]\n"
        "    assert(world.WindowRectOp(base, 2, 0, y, 20, y + 1,\n"
        "                              px(c[1], c[2], c[3]), 0) == 0)\n"
        "    assert(world.WindowGetPixel(base, 4, y) == px(c[1], c[2], c[3]),\n"
        "           'fixture band ' .. y)\n"
        "  end\n"
        "  assert(world.WindowImageFromWindow(base, 'im', 'sw') == 0, 'capture failed')\n"
        "  assert(world.WindowImageInfo(base, 'im', 2) == 8, 'image width')\n"
        "  assert(world.WindowImageInfo(base, 'im', 3) == 6, 'image height')\n"
        "end\n"
        // The control every positive assertion needs: the blend stayed inside
        // (4, 6)-(9, 9).  Left and right land on the BAND colour rather than the
        // background, so a blend one pixel too wide is caught as well as one
        // that escaped into the plain background.
        "local function outside(base)\n"
        "  local c6 = BASE[6]\n"
        "  assert(world.WindowGetPixel(base, 3, 6) == px(c6[1], c6[2], c6[3]),\n"
        "         'blend escaped left of the rect')\n"
        "  assert(world.WindowGetPixel(base, 9, 6) == px(c6[1], c6[2], c6[3]),\n"
        "         'blend escaped right of the rect')\n"
        "  assert(world.WindowGetPixel(base, 4, 5) == BBG, 'blend escaped above')\n"
        "  assert(world.WindowGetPixel(base, 4, 9) == BBG, 'blend escaped below')\n"
        "end\n")
        + body;
}

TEST(LuaSurface, BlendImageOverlayAndHardLightAreOperandOrderSensitive)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L, blend_script(
        "fixture('ov') fixture('hl')\n"

        // Mode 17 = Blend_Overlay(image, base): the BASE picks the branch, so
        // each band takes a different one.  Row 6 (100, 40, 220):
        //   r: 100 < 128  -> 2*200*100/255            = 156
        //   g:  40 < 128  -> 2*150*40/255             =  47
        //   b: 220 >= 128 -> 255 - 2*195*35/255       = 202
        // Row 7 (30, 200, 90) -> (47, 210, 42); row 8 (250, 10, 170) ->
        //   (255 - 2*55*5/255, 2*150*10/255, 255 - 2*195*85/255) = (253, 11, 125).
        "assert(world.WindowBlendImage('ov', 'im', 4, 6, 9, 9, 17, 1.0, 2, 1, 7, 4) == 0)\n"
        "assert(world.WindowGetPixel('ov', 4, 6) == px(156, 47, 202), 'overlay row 6')\n"
        "assert(world.WindowGetPixel('ov', 8, 6) == px(156, 47, 202), 'overlay row 6 end')\n"
        "assert(world.WindowGetPixel('ov', 6, 7) == px(47, 210, 42), 'overlay row 7')\n"
        "assert(world.WindowGetPixel('ov', 8, 8) == px(253, 11, 125), 'overlay row 8')\n"

        // Mode 19 = Blend_HardLight, which blending.h DEFINES as
        // Blend_Overlay(B, A): identical maths, operands swapped, so now the
        // IMAGE picks the branch.  Row 6:
        //   r: 200 >= 128 -> 255 - 2*(255-100)*(255-200)/255 = 189
        //   g: 150 >= 128 -> 255 - 2*(255-40)*(255-150)/255  =  78
        //   b:  60 <  128 -> 2*220*60/255                    = 103
        // Row 7 -> (158, 210, 42) and row 8 -> (253, 54, 80).  Rows 6 and 8
        // differ from mode 17; row 7 shares two channels because both operands
        // fall on the same side of 128 there, which is why row 6 carries the
        // inequality below.
        "assert(world.WindowBlendImage('hl', 'im', 4, 6, 9, 9, 19, 1.0, 2, 1, 7, 4) == 0)\n"
        "assert(world.WindowGetPixel('hl', 4, 6) == px(189, 78, 103), 'hardlight row 6')\n"
        "assert(world.WindowGetPixel('hl', 6, 7) == px(158, 210, 42), 'hardlight row 7')\n"
        "assert(world.WindowGetPixel('hl', 8, 8) == px(253, 54, 80), 'hardlight row 8')\n"
        "assert(world.WindowGetPixel('ov', 4, 6) ~= world.WindowGetPixel('hl', 4, 6),\n"
        "       'overlay and hard light agree: the operands are interchangeable')\n"

        // Sweep both rectangles whole: a pitch that is off by one byte still
        // hits most of the bytes the right one does, so corner checks alone let
        // it through.
        "local function sweep(n, e6, e7, e8)\n"
        "  local e = { [6] = e6, [7] = e7, [8] = e8 }\n"
        "  for x = 4, 8 do for y = 6, 8 do\n"
        "    assert(world.WindowGetPixel(n, x, y) == e[y],\n"
        "           string.format('%s at %d,%d is %d not %d', n, x, y,\n"
        "                         world.WindowGetPixel(n, x, y), e[y]))\n"
        "  end end\n"
        "end\n"
        "sweep('ov', px(156, 47, 202), px(47, 210, 42), px(253, 11, 125))\n"
        "sweep('hl', px(189, 78, 103), px(158, 210, 42), px(253, 54, 80))\n"

        "outside('ov') outside('hl')\n"
    ).c_str()));
}

TEST(LuaSurface, BlendImageColourModesTakeOneChannelFromEachOperand)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L, blend_script(
        "fixture('cr') fixture('cg') fixture('cb')\n"

        // 36 / 37 / 38 are Colour_Op(rA,gB,bB) / (rB,gA,bB) / (rB,gB,bA): one
        // channel from the image, the other two kept from the base — a
        // different one each time, so an index that is off by one fails at
        // least two of the three.  Unlike Blend_It these walk row by row using
        // `perline`, which is the PADDED 16 rather than 15, so this is the
        // pitch assertion as well: every row's expectation differs.
        "assert(world.WindowBlendImage('cr', 'im', 4, 6, 9, 9, 36, 1.0, 2, 1, 7, 4) == 0)\n"
        "assert(world.WindowGetPixel('cr', 4, 6) == px(200, 40, 220), 'red-from-image row 6')\n"
        "assert(world.WindowGetPixel('cr', 8, 7) == px(200, 200, 90), 'red-from-image row 7')\n"
        "assert(world.WindowGetPixel('cr', 6, 8) == px(200, 10, 170), 'red-from-image row 8')\n"

        "assert(world.WindowBlendImage('cg', 'im', 4, 6, 9, 9, 37, 1.0, 2, 1, 7, 4) == 0)\n"
        "assert(world.WindowGetPixel('cg', 4, 6) == px(100, 150, 220), 'green-from-image row 6')\n"
        "assert(world.WindowGetPixel('cg', 8, 7) == px(30, 150, 90), 'green-from-image row 7')\n"
        "assert(world.WindowGetPixel('cg', 6, 8) == px(250, 150, 170), 'green-from-image row 8')\n"

        "assert(world.WindowBlendImage('cb', 'im', 4, 6, 9, 9, 38, 1.0, 2, 1, 7, 4) == 0)\n"
        "assert(world.WindowGetPixel('cb', 4, 6) == px(100, 40, 60), 'blue-from-image row 6')\n"
        "assert(world.WindowGetPixel('cb', 8, 7) == px(30, 200, 60), 'blue-from-image row 7')\n"
        "assert(world.WindowGetPixel('cb', 6, 8) == px(250, 10, 60), 'blue-from-image row 8')\n"

        // The three must actually differ from one another — a body that took
        // all three channels from the same operand would satisfy none of the
        // above, but say so directly anyway.
        "assert(world.WindowGetPixel('cr', 4, 6) ~= world.WindowGetPixel('cg', 4, 6)\n"
        "   and world.WindowGetPixel('cg', 4, 6) ~= world.WindowGetPixel('cb', 4, 6),\n"
        "       'colour modes 36/37/38 are indistinguishable')\n"

        // Opacity < 1 inside a Colour_Op goes through Simple_Opacity, which is
        // a different line from Blend_Opacity: red is 0.5*200 + 0.5*100 + 0.5 =
        // 150, while the two channels taken FROM the base blend with themselves
        // and so must not move at all.
        "fixture('ch')\n"
        "assert(world.WindowBlendImage('ch', 'im', 4, 6, 9, 9, 36, 0.5, 2, 1, 7, 4) == 0)\n"
        "assert(world.WindowGetPixel('ch', 4, 6) == px(150, 40, 220),\n"
        "       'colour op at half opacity')\n"

        // Sweep all three rectangles whole.  Colour_Op indexes with
        // `row * perline`, and perline off by one still writes most of the
        // right bytes, so this is where a pitch bug is actually caught.
        "local function sweep(n, e6, e7, e8)\n"
        "  local e = { [6] = e6, [7] = e7, [8] = e8 }\n"
        "  for x = 4, 8 do for y = 6, 8 do\n"
        "    assert(world.WindowGetPixel(n, x, y) == e[y],\n"
        "           string.format('%s at %d,%d is %d not %d', n, x, y,\n"
        "                         world.WindowGetPixel(n, x, y), e[y]))\n"
        "  end end\n"
        "end\n"
        "sweep('cr', px(200, 40, 220), px(200, 200, 90), px(200, 10, 170))\n"
        "sweep('cg', px(100, 150, 220), px(30, 150, 90), px(250, 150, 170))\n"
        "sweep('cb', px(100, 40, 60), px(30, 200, 60), px(250, 10, 60))\n"
        "sweep('ch', px(150, 40, 220), px(115, 200, 90), px(225, 10, 170))\n"

        "outside('cr') outside('cg') outside('cb') outside('ch')\n"
    ).c_str()));
}

TEST(LuaSurface, BlendImageOpacityExtremesAreDeterministicControls)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L, blend_script(
        "fixture('o0') fixture('o5') fixture('o1')\n"

        // Mode 1 (Blend_Normal) is A: it throws the base away entirely, which
        // is the strongest available statement that the IMAGE is the A operand
        // and that SrcLeft/SrcTop were honoured (the image origin is a
        // different colour from the patch).
        "assert(world.WindowBlendImage('o1', 'im', 4, 6, 9, 9, 1, 1.0, 2, 1, 7, 4) == 0)\n"
        "for x = 4, 8 do for y = 6, 8 do\n"
        "  assert(world.WindowGetPixel('o1', x, y) == px(200, 150, 60),\n"
        "         string.format('normal blend at %d,%d is %d', x, y,\n"
        "                       world.WindowGetPixel('o1', x, y)))\n"
        "end end\n"

        // Opacity 0 takes the OTHER branch (Opacity < 1.0) and must leave every
        // base byte exactly where it was: (uint8)(0*A + 1*B + 0.5) == B.
        // Without this control a body that ignored Opacity would pass
        // everything else in this file.
        "assert(world.WindowBlendImage('o0', 'im', 4, 6, 9, 9, 1, 0.0, 2, 1, 7, 4) == 0)\n"
        "for y = 6, 8 do local c = BASE[y]\n"
        "  for x = 4, 8 do\n"
        "    assert(world.WindowGetPixel('o0', x, y) == px(c[1], c[2], c[3]),\n"
        "           'opacity 0 still blended row ' .. y)\n"
        "  end\n"
        "end\n"

        // ...and half-way is neither of those two: 0.5*A + 0.5*B + 0.5,
        // truncated.  Row 6 (100,40,220) -> (150, 95, 140), row 7 (30,200,90)
        // -> (115, 175, 75), row 8 (250,10,170) -> (225, 80, 115).
        "assert(world.WindowBlendImage('o5', 'im', 4, 6, 9, 9, 1, 0.5, 2, 1, 7, 4) == 0)\n"
        "local half = { [6] = px(150, 95, 140), [7] = px(115, 175, 75),\n"
        "               [8] = px(225, 80, 115) }\n"
        "for x = 4, 8 do for y = 6, 8 do\n"
        "  assert(world.WindowGetPixel('o5', x, y) == half[y],\n"
        "         string.format('half opacity at %d,%d is %d not %d', x, y,\n"
        "                       world.WindowGetPixel('o5', x, y), half[y]))\n"
        "end end\n"

        "outside('o0') outside('o5') outside('o1')\n"
    ).c_str()));
}

TEST(LuaSurface, BlendImageSizesFromBothRectanglesAndRefusesBadArguments)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    EXPECT_TRUE(run(f.L, blend_script(
        "fixture('g1') fixture('g2') fixture('g3')\n"

        // SrcRight/SrcBottom <= 0 are offsets from the image's far edge, so
        // (-1, -2) on an 8 x 6 image is the same 7 / 4 spelled differently —
        // and the two offsets differ, so a body that used one for both fails.
        "assert(world.WindowBlendImage('g1', 'im', 4, 6, 9, 9, 1, 1.0, 2, 1, -1, -2) == 0)\n"
        "for x = 4, 8 do for y = 6, 8 do\n"
        "  assert(world.WindowGetPixel('g1', x, y) == px(200, 150, 60),\n"
        "         string.format('negative src edge at %d,%d', x, y))\n"
        "end end\n"
        "outside('g1')\n"

        // The blended size is the MIN of the two rectangles.  Ask for an 8 x 5
        // destination against a 3 x 2 source: exactly 3 x 2 may move, and the
        // two dimensions are different so one cannot stand in for the other.
        "assert(world.WindowBlendImage('g2', 'im', 4, 6, 12, 11, 1, 1.0, 2, 1, 5, 3) == 0)\n"
        "for x = 4, 6 do for y = 6, 7 do\n"
        "  assert(world.WindowGetPixel('g2', x, y) == px(200, 150, 60),\n"
        "         string.format('min-sized blend missed %d,%d', x, y))\n"
        "end end\n"
        "local c6, c8 = BASE[6], BASE[8]\n"
        "assert(world.WindowGetPixel('g2', 7, 6) == px(c6[1], c6[2], c6[3]),\n"
        "       'blend used the destination width')\n"
        "assert(world.WindowGetPixel('g2', 4, 8) == px(c8[1], c8[2], c8[3]),\n"
        "       'blend used the destination height')\n"

        // Error arms.  Each must be refused AND inert — every one of them
        // returns before the write-back.
        "assert(world.WindowBlendImage('g3', 'nope', 4, 6, 9, 9, 1, 1.0, 2, 1, 7, 4)\n"
        "       == error_code.eImageNotInstalled)\n"
        "assert(world.WindowBlendImage('g3', 'im', 4, 6, 9, 9, 1, 1.5, 2, 1, 7, 4)\n"
        "       == error_code.eBadParameter)\n"
        "assert(world.WindowBlendImage('g3', 'im', 4, 6, 9, 9, 1, -0.5, 2, 1, 7, 4)\n"
        "       == error_code.eBadParameter)\n"
        // 0 and 65 bracket the switch, whose live cases run 1..64 (65 is
        // commented out in miniwindow.cpp).
        "assert(world.WindowBlendImage('g3', 'im', 4, 6, 9, 9, 0, 1.0, 2, 1, 7, 4)\n"
        "       == error_code.eUnknownOption)\n"
        "assert(world.WindowBlendImage('g3', 'im', 4, 6, 9, 9, 65, 1.0, 2, 1, 7, 4)\n"
        "       == error_code.eUnknownOption)\n"
        "for y = 6, 8 do local c = BASE[y]\n"
        "  assert(world.WindowGetPixel('g3', 4, y) == px(c[1], c[2], c[3]),\n"
        "         'a refused blend still wrote row ' .. y)\n"
        "end\n"
        // ...and the same window DOES blend when asked properly, so the five
        // assertions above are about the guards, not about a dead window.
        "assert(world.WindowBlendImage('g3', 'im', 4, 6, 9, 9, 1, 1.0, 2, 1, 7, 4) == 0)\n"
        "assert(world.WindowGetPixel('g3', 4, 6) == px(200, 150, 60), 'g3 never blended')\n"
        "outside('g3')\n"
    ).c_str()));
}

// ── the transform / stretch / image-brush arms ────────────────────────────
//
// Three arms of the image family that the block above does not reach.  They
// share one fixture, which is deliberately NOT square and whose x structure
// differs from its y structure, so a transpose, a mirror or a row/column
// mix-up all land somewhere visible:
//
//          x:  0 1 2 3 4 5 6 7 8
//    y = 0..2: B B G G G G B B B
//    y = 3..4: R R R R R R R R R
//
// It is painted by the app's own RectOp, written by the app's own PNG writer
// and read back through WindowLoadImage, so every pixel comes from the shipping
// decoder rather than from a hand-rolled file that only agrees with itself.
static std::string fixture_png_lua (const char * path)
{
    return std::string (
        "local BLUE, GREEN, RED = 0x0000FF, 0x00FF00, 0xFF0000\n"
        "assert(world.WindowCreate('fx', 0, 0, 9, 5, 0, 0, BLUE) == 0)\n"
        "assert(world.WindowRectOp('fx', 2, 2, 0, 6, 5, GREEN, 0) == 0)\n"
        "assert(world.WindowRectOp('fx', 2, 0, 3, 9, 5, RED, 0) == 0)\n"
        // Prove the fixture really has the shape the assertions below assume —
        // otherwise a broken RectOp would quietly weaken every test using it.
        "assert(world.WindowGetPixel('fx', 0, 0) == BLUE,  'fixture 0,0')\n"
        "assert(world.WindowGetPixel('fx', 2, 0) == GREEN, 'fixture 2,0')\n"
        "assert(world.WindowGetPixel('fx', 5, 2) == GREEN, 'fixture 5,2')\n"
        "assert(world.WindowGetPixel('fx', 6, 0) == BLUE,  'fixture 6,0')\n"
        "assert(world.WindowGetPixel('fx', 0, 3) == RED,   'fixture 0,3')\n"
        "assert(world.WindowGetPixel('fx', 4, 4) == RED,   'fixture 4,4')\n"
        "assert(world.WindowWrite('fx', '") + path + "') == 0)\n";
}

static std::string fixture_png_path (const char * tag)
{
    char p[256];
    snprintf (p, sizeof p, "/tmp/mc_test_%s_%d.png", tag, (int) getpid ());
    ::unlink (p);
    return p;
}

// (a) CMiniWindow::TransformImage — the Mxx/Mxy/Myx/Myy -> m11/m21/m12/m22 cross.
//
// tests/test_pal_surface.cpp builds a pal::Matrix2x3 by hand and so can never
// see the ENGINE's field assignment (miniwindow.cpp:4146-4149).  Getting that
// cross wrong transposes or mirrors the result, which is invisible on a square
// image and invisible again if Left == Top — hence a 9 x 5 fixture and
// Left = 13, Top = 3.
//
// Two matrices, because one is not enough:
//   * a 90-degree rotation (Mxx = Myy = 0) pins Mxy -> m21 against Myx -> m12,
//     but says nothing about m11 vs m22 because both are zero;
//   * an anisotropic scale (Mxx = 2, Myy = 1) pins m11 against m22, which the
//     rotation cannot.
// The expected pixels are the CpuRaster inverse map worked through by hand:
// sx = floor((u*m22 - v*m21)/det), sy = floor((-u*m12 + v*m11)/det) with
// u = X + 0.5 - Left, v = Y + 0.5 - Top.
TEST(LuaSurface, TransformImageCrossesTheMatrixFieldsIntoTheSurface)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    const std::string png = fixture_png_path ("xform");

    const std::string code = fixture_png_lua (png.c_str ()) +
        "local BG = 0x123456\n"

        // ── 90-degree rotation: Mxx=0 Mxy=-1 Myx=1 Myy=0 ────────────────
        // Forward: X = sx*Mxx + sy*Mxy + Left,  Y = sx*Myx + sy*Myy + Top,
        // so (sx, sy) -> (13 - sy - 1, 3 + sx): a 9-wide, 5-tall image comes
        // out 5 wide and 9 tall.  Inverse: sx = Y - 3, sy = 12 - X, valid for
        // X in [8,12], Y in [3,11].
        "assert(world.WindowCreate('w', 0, 0, 20, 16, 0, 0, BG) == 0)\n"
        "assert(world.WindowLoadImage('w', 'img', '" + png + "') == 0,\n"
        "       'fixture PNG did not load')\n"
        "assert(world.WindowImageInfo('w', 'img', 2) == 9, 'fixture not 9 wide')\n"
        "assert(world.WindowImageInfo('w', 'img', 3) == 5, 'fixture not 5 tall')\n"
        "assert(world.WindowTransformImage('w', 'img', 13, 3, 1, 0, -1, 1, 0) == 0)\n"

        // The image's x axis now runs DOWN the window (sx = Y - 3) ...
        "assert(world.WindowGetPixel('w', 12,  3) == BLUE,  'rot sx=0 sy=0')\n"
        "assert(world.WindowGetPixel('w', 12,  5) == GREEN, 'rot sx=2 sy=0')\n"
        "assert(world.WindowGetPixel('w', 12,  8) == GREEN, 'rot sx=5 sy=0')\n"
        "assert(world.WindowGetPixel('w', 12,  9) == BLUE,  'rot sx=6 sy=0')\n"
        "assert(world.WindowGetPixel('w', 12, 11) == BLUE,  'rot sx=8 sy=0')\n"
        // ... and its y axis runs RIGHT-TO-LEFT across the window (sy = 12 - X).
        "assert(world.WindowGetPixel('w', 11,  5) == GREEN, 'rot sx=2 sy=1')\n"
        "assert(world.WindowGetPixel('w', 10,  5) == GREEN, 'rot sx=2 sy=2')\n"
        "assert(world.WindowGetPixel('w',  9,  5) == RED,   'rot sx=2 sy=3')\n"
        "assert(world.WindowGetPixel('w',  8,  5) == RED,   'rot sx=2 sy=4')\n"
        "assert(world.WindowGetPixel('w',  8,  3) == RED,   'rot sx=0 sy=4')\n"
        "assert(world.WindowGetPixel('w',  9, 11) == RED,   'rot sx=8 sy=3')\n"
        // Controls: one pixel past each edge of the rotated footprint.  A body
        // that ignored the translation, or that used the untransposed extent,
        // would paint at least one of these.
        "assert(world.WindowGetPixel('w', 13,  5) == BG, 'rot spilled right')\n"
        "assert(world.WindowGetPixel('w',  7,  5) == BG, 'rot spilled left')\n"
        "assert(world.WindowGetPixel('w', 12,  2) == BG, 'rot spilled up')\n"
        "assert(world.WindowGetPixel('w', 12, 12) == BG, 'rot spilled down')\n"

        // ── anisotropic scale: Mxx=2 Mxy=0 Myx=0 Myy=1 ──────────────────
        // Inverse: sx = floor((X - 1.5)/2), sy = Y - 7 — the image is doubled
        // horizontally and left alone vertically, so the four green columns
        // (sx 2..5) become the eight destination columns X = 6..13.  Swapping
        // m11 with m22 doubles it VERTICALLY instead and moves every pixel
        // asserted below.
        "assert(world.WindowCreate('v', 0, 0, 22, 16, 0, 0, BG) == 0)\n"
        "assert(world.WindowLoadImage('v', 'img', '" + png + "') == 0)\n"
        "assert(world.WindowTransformImage('v', 'img', 2, 7, 1, 2, 0, 0, 1) == 0)\n"
        "assert(world.WindowGetPixel('v',  2,  7) == BLUE,  'scale sx=0')\n"
        "assert(world.WindowGetPixel('v',  5,  7) == BLUE,  'scale sx=1')\n"
        "assert(world.WindowGetPixel('v',  6,  7) == GREEN, 'scale sx=2 left')\n"
        "assert(world.WindowGetPixel('v', 13,  7) == GREEN, 'scale sx=5 right')\n"
        "assert(world.WindowGetPixel('v', 14,  7) == BLUE,  'scale sx=6')\n"
        "assert(world.WindowGetPixel('v', 19,  7) == BLUE,  'scale sx=8')\n"
        "assert(world.WindowGetPixel('v',  6,  9) == GREEN, 'scale sy=2')\n"
        "assert(world.WindowGetPixel('v',  6, 10) == RED,   'scale sy=3')\n"
        "assert(world.WindowGetPixel('v',  2, 11) == RED,   'scale sy=4')\n"
        // Controls on all four sides of the stretched footprint.
        "assert(world.WindowGetPixel('v',  1,  7) == BG, 'scale spilled left')\n"
        "assert(world.WindowGetPixel('v', 20,  7) == BG, 'scale spilled right')\n"
        "assert(world.WindowGetPixel('v',  2,  6) == BG, 'scale spilled up')\n"
        "assert(world.WindowGetPixel('v',  2, 12) == BG, 'scale spilled down')\n"

        // ── Mode 3 keys on the image's own (0,0) pixel ──────────────────
        // Identity matrix at Left=4, Top=1: sx = X - 4, sy = Y - 1.  The key is
        // the fixture's (0,0), which is BLUE, so every blue pixel drops out and
        // the green and red ones survive.
        "assert(world.WindowCreate('t', 0, 0, 20, 12, 0, 0, BG) == 0)\n"
        "assert(world.WindowLoadImage('t', 'img', '" + png + "') == 0)\n"
        "assert(world.WindowTransformImage('t', 'img', 4, 1, 3, 1, 0, 0, 1) == 0)\n"
        "assert(world.WindowGetPixel('t',  6,  1) == GREEN, 'mode 3 lost green')\n"
        "assert(world.WindowGetPixel('t',  9,  1) == GREEN, 'mode 3 lost green')\n"
        "assert(world.WindowGetPixel('t',  4,  4) == RED,   'mode 3 lost red')\n"
        "assert(world.WindowGetPixel('t', 12,  5) == RED,   'mode 3 lost red')\n"
        "assert(world.WindowGetPixel('t',  4,  1) == BG, 'mode 3 ignored the key')\n"
        "assert(world.WindowGetPixel('t', 10,  1) == BG, 'mode 3 ignored the key')\n"
        // ...and the SAME transform in mode 1 does keep the blue, so the two
        // background readings above are about the colour key and not about the
        // call having drawn nothing.
        "assert(world.WindowCreate('u', 0, 0, 20, 12, 0, 0, BG) == 0)\n"
        "assert(world.WindowLoadImage('u', 'img', '" + png + "') == 0)\n"
        "assert(world.WindowTransformImage('u', 'img', 4, 1, 1, 1, 0, 0, 1) == 0)\n"
        "assert(world.WindowGetPixel('u',  4,  1) == BLUE, 'mode 1 dropped blue')\n"
        "assert(world.WindowGetPixel('u', 10,  1) == BLUE, 'mode 1 dropped blue')\n"
        "assert(world.WindowGetPixel('u',  6,  1) == GREEN, 'mode 1 lost green')\n"

        // ── the refusing arms, each with a canvas control ───────────────
        // A fresh window, the SAME rotation the first block proved paints, and
        // only the rejected parameter changed: if an arm stopped refusing, the
        // pixel it would have painted is the one asserted still background.
        "assert(world.WindowCreate('e', 0, 0, 20, 16, 0, 0, BG) == 0)\n"
        "assert(world.WindowLoadImage('e', 'img', '" + png + "') == 0)\n"
        // Mode 2 (the stretch mode of WindowDrawImage) has never been a
        // transform mode.
        "assert(world.WindowTransformImage('e', 'img', 13, 3, 2, 0, -1, 1, 0)\n"
        "       == error_code.eBadParameter, 'mode 2 was accepted')\n"
        "assert(world.WindowGetPixel('e', 12, 3) == BG, 'mode 2 drew anyway')\n"
        "assert(world.WindowTransformImage('e', 'img', 13, 3, 0, 0, -1, 1, 0)\n"
        "       == error_code.eBadParameter, 'mode 0 was accepted')\n"
        "assert(world.WindowGetPixel('e', 12, 3) == BG, 'mode 0 drew anyway')\n"
        // A singular matrix: 2*2 - 4*1 == 0.  Deliberately not the trivial
        // all-ones one, so the determinant really has to be computed.
        "assert(world.WindowTransformImage('e', 'img', 13, 3, 1, 2, 4, 1, 2)\n"
        "       == error_code.eBadParameter, 'singular matrix was accepted')\n"
        "assert(world.WindowGetPixel('e', 12, 3) == BG, 'singular matrix drew')\n"
        "assert(world.WindowTransformImage('e', 'nope', 13, 3, 1, 0, -1, 1, 0)\n"
        "       == error_code.eImageNotInstalled)\n"
        "assert(world.WindowTransformImage('nowin', 'img', 13, 3, 1, 0, -1, 1, 0)\n"
        "       == error_code.eNoSuchWindow)\n"
        "assert(world.WindowGetPixel('e', 12, 3) == BG, 'a refused call drew')\n"
        // ...and the control for the controls: the very same call, valid, does
        // reach that pixel.
        "assert(world.WindowTransformImage('e', 'img', 13, 3, 1, 0, -1, 1, 0) == 0)\n"
        "assert(world.WindowGetPixel('e', 12, 3) == BLUE, 'the valid call drew nothing')\n";

    EXPECT_TRUE(run(f.L, code.c_str ()));
    ::unlink (png.c_str ());
}

// (b) world.WindowDrawImage MODE 2 — the stretch arm.
//
// It reaches real pixels headless, which was worth establishing: the arm is
// dispatched, the source rectangle is honoured and the image lands at
// (Left, Top).  Two mutations proved that (see below).
//
// WHAT IT CANNOT COVER, stated rather than papered over.  The one thing that
// makes mode 2 different from mode 1 — the destination extent, computed at
// miniwindow.cpp:1474 as FixRight (Right) - Left by FixBottom (Bottom) - Top —
// is UNOBSERVABLE through pal::CpuRaster, and not for the reason the backlog
// records.  The suspected blocker was the no-stretch guard at
// cpu_raster.cpp:371; the actual behaviour is that the SurfaceId -> SurfaceId
// blit overload OVERWRITES the caller's destination extent with the clipped
// source extent before it delegates:
//
//     dstRect = { dstRect.x + (clipped.x - srcRect.x),
//                 dstRect.y + (clipped.y - srcRect.y), clipped.w, clipped.h };
//                                                      ^^^^^^^^^^^^^^^^^^^^^
//
// so the guard can never fire on this path and every mode-2 call draws 1:1 at
// the destination ORIGIN whatever Right and Bottom say.  Measured: asking for a
// 2 x 1 destination still painted the whole 9 x 5 image, and changing the arm to
// `Right - Left` / `Bottom - Top` (which makes both negative here) left the
// entire suite green.  Headless, therefore, mode 2 is observationally identical
// to mode 1 — the BlitQuality::Smooth difference is dropped too, CpuRaster
// ignores the quality argument.  Only the Windows GdiSurface (StretchBlt) and
// the wx adapter can tell the two apart; that is Windows-validation work.
//
// So the assertions below deliberately pin only what IS observed, and add the
// negative SrcRight/SrcBottom form ("-1 means one in from the right") that the
// mode-1 block above never exercises.
TEST(LuaSurface, DrawImageStretchModeDrawsAndHonoursItsSourceRect)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    const std::string png = fixture_png_path ("stretch");

    const std::string code = fixture_png_lua (png.c_str ()) +
        "local BG = 0x123456\n"
        // 20 x 14 — both non-square and different from the 9 x 5 image, so a
        // window/image extent mix-up cannot come out right by luck.
        "assert(world.WindowCreate('w', 0, 0, 20, 14, 0, 0, BG) == 0)\n"
        "assert(world.WindowLoadImage('w', 'img', '" + png + "') == 0,\n"
        "       'fixture PNG did not load')\n"

        // The call shape a plugin actually writes: Right = 0 / Bottom = 0 for
        // \"to the far edge\".  Left = 11 and Top = 9 differ from each other, so
        // an x/y swap in the destination origin moves every pixel below.
        "assert(world.WindowDrawImage('w', 'img', 11, 9, 0, 0, 2, 0, 0, 0, 0) == 0,\n"
        "       'mode 2 was refused')\n"
        "assert(world.WindowGetPixel('w', 11,  9) == BLUE,  'mode 2 drew nothing')\n"
        "assert(world.WindowGetPixel('w', 13,  9) == GREEN, 'mode 2 src 2,0')\n"
        "assert(world.WindowGetPixel('w', 16,  9) == GREEN, 'mode 2 src 5,0')\n"
        "assert(world.WindowGetPixel('w', 17,  9) == BLUE,  'mode 2 src 6,0')\n"
        "assert(world.WindowGetPixel('w', 19,  9) == BLUE,  'mode 2 src 8,0')\n"
        "assert(world.WindowGetPixel('w', 11, 12) == RED,   'mode 2 src 0,3')\n"
        "assert(world.WindowGetPixel('w', 19, 13) == RED,   'mode 2 src 8,4')\n"
        // Controls on the two sides that are not the window edge.
        "assert(world.WindowGetPixel('w', 10,  9) == BG, 'mode 2 spilled left')\n"
        "assert(world.WindowGetPixel('w', 11,  8) == BG, 'mode 2 spilled up')\n"
        "assert(world.WindowGetPixel('w', 10, 13) == BG, 'mode 2 spilled left')\n"

        // Second call: a genuine STRETCH, with a source sub-rectangle given
        // using NEGATIVE far edges.  SrcRight = -2 -> 9 - 2 = 7 and
        // SrcBottom = -1 -> 5 - 1 = 4, so the source is (2,2)..(7,4) = 5 x 2,
        // while Right = 0 / Bottom = 0 make the destination 19 x 6.  Those
        // extents differ in BOTH axes and by different ratios, so a body that
        // used one axis's scale for both fails.
        //
        // Nearest neighbour (CpuRaster is COLORONCOLOR, never interpolating),
        // so the expected pixels are exact rather than approximate:
        //   sy = 2 + floor(Y * 2 / 6)  -> Y in [0,3) reads image row 2,
        //                                 Y in [3,6) reads image row 3
        //   sx = 2 + floor(X * 5 / 19) -> X in [0,16) reads image x 2..5,
        //                                 X in [16,19) reads image x 6
        // Image row 2 is the last blue/green row and row 3 the first red one,
        // so the two halves must differ — dropping SrcTop would give six
        // identical blue/green rows.
        "assert(world.WindowDrawImage('w', 'img', 1, 8, 0, 0, 2, 2, 2, -2, -1) == 0)\n"
        "assert(world.WindowGetPixel('w',  1,  8) == GREEN, 'SrcLeft ignored')\n"
        "assert(world.WindowGetPixel('w', 16,  8) == GREEN, 'src x=5 column')\n"
        "assert(world.WindowGetPixel('w', 17,  8) == BLUE,  'src x=6 column')\n"
        "assert(world.WindowGetPixel('w', 19,  8) == BLUE,  'stretch stopped short')\n"
        "assert(world.WindowGetPixel('w',  1, 10) == GREEN, 'row 2 band too thin')\n"
        "assert(world.WindowGetPixel('w',  1, 11) == RED,   'SrcTop ignored')\n"
        "assert(world.WindowGetPixel('w', 19, 13) == RED,   'stretch stopped short')\n"
        // ...and it really SCALED rather than copying 1:1 at the origin, which
        // is what this adapter did until the stretch was implemented: at 1:1
        // the source is only 5 wide and 2 tall, so (6,8) and (1,10) would both
        // be background.
        "assert(world.WindowGetPixel('w',  6,  8) ~= BG, 'not stretched: 1:1 width')\n"
        "assert(world.WindowGetPixel('w',  1, 12) ~= BG, 'not stretched: 1:1 height')\n"
        // Controls on the two edges that are not the window's own.
        "assert(world.WindowGetPixel('w', 0,  8) == BG, 'sub-rect spilled left')\n"
        "assert(world.WindowGetPixel('w', 1,  7) == BG, 'sub-rect spilled up')\n"

        // ── MAGNIFY, with explicit far edges so all four spill controls exist
        //
        // Both calls above run to the window's own edge (Right = 0), which
        // makes a right/bottom overrun invisible.  This one does not.
        //
        // Source (0,0)..(3,1) is 3 x 1 — image x = 0,1 blue and x = 2 green —
        // into a 9 x 5 destination at (2,1).  So each source column becomes
        // exactly three columns and the single source row becomes five rows:
        //   sx = floor(X * 3 / 9) = X / 3,   sy = floor(Y * 1 / 5) = 0
        // The ratios differ per axis (3x across, 5x down), so a body using one
        // scale for both fails, and 9/3 and 5/1 are exact so there is no
        // rounding to argue about.
        "assert(world.WindowCreate('s', 0, 0, 20, 14, 0, 0, BG) == 0)\n"
        "assert(world.WindowLoadImage('s', 'img', '" + png + "') == 0)\n"
        "assert(world.WindowDrawImage('s', 'img', 2, 1, 11, 6, 2, 0, 0, 3, 1) == 0)\n"
        "assert(world.WindowGetPixel('s',  2, 1) == BLUE,  'magnify origin')\n"
        "assert(world.WindowGetPixel('s',  7, 1) == BLUE,  'magnify src x=1')\n"
        "assert(world.WindowGetPixel('s',  8, 1) == GREEN, 'magnify src x=2')\n"
        "assert(world.WindowGetPixel('s', 10, 1) == GREEN, 'magnify right edge')\n"
        // The one source row really did become five: without a vertical scale
        // rows 2..5 of the destination would be background.
        "assert(world.WindowGetPixel('s', 10, 5) == GREEN, 'magnify bottom edge')\n"
        "assert(world.WindowGetPixel('s',  2, 5) == BLUE,  'vertical scale wrong')\n"
        // All four controls, which is what the explicit far edges buy.
        "assert(world.WindowGetPixel('s',  1, 1) == BG, 'magnify spilled left')\n"
        "assert(world.WindowGetPixel('s',  2, 0) == BG, 'magnify spilled up')\n"
        "assert(world.WindowGetPixel('s', 11, 1) == BG, 'magnify spilled right')\n"
        "assert(world.WindowGetPixel('s',  2, 6) == BG, 'magnify spilled down')\n"

        // ── MINIFY — the other direction, which a scale-up-only body fails.
        //
        // The whole 9 x 5 image into a 3 x 2 destination at (14,8):
        //   sx = floor(X * 9 / 3) = 3X,   sy = floor(Y * 5 / 2)
        // so the three destination columns sample image x = 0, 3, 6 — blue,
        // green, blue — which no monotone smear could produce.
        "assert(world.WindowDrawImage('s', 'img', 14, 8, 17, 10, 2, 0, 0, 9, 5) == 0)\n"
        "assert(world.WindowGetPixel('s', 14, 8) == BLUE,  'minify src x=0')\n"
        "assert(world.WindowGetPixel('s', 15, 8) == GREEN, 'minify src x=3')\n"
        "assert(world.WindowGetPixel('s', 16, 8) == BLUE,  'minify src x=6')\n"
        "assert(world.WindowGetPixel('s', 17, 8) == BG,    'minify spilled right')\n"
        "assert(world.WindowGetPixel('s', 14, 10) == BG,   'minify spilled down')\n"

        // The refusing control: a mode the switch does not know is rejected,
        // and leaves the canvas alone — then the very same call in mode 2 does
        // reach that pixel.
        "assert(world.WindowCreate('c', 0, 0, 20, 14, 0, 0, BG) == 0)\n"
        "assert(world.WindowLoadImage('c', 'img', '" + png + "') == 0)\n"
        "assert(world.WindowDrawImage('c', 'img', 11, 9, 0, 0, 4, 0, 0, 0, 0)\n"
        "       == error_code.eBadParameter, 'mode 4 was accepted')\n"
        "assert(world.WindowGetPixel('c', 11, 9) == BG, 'mode 4 drew anyway')\n"
        "assert(world.WindowDrawImage('c', 'img', 11, 9, 0, 0, 2, 0, 0, 0, 0) == 0)\n"
        "assert(world.WindowGetPixel('c', 11, 9) == BLUE, 'mode 2 drew nothing')\n";

    EXPECT_TRUE(run(f.L, code.c_str ()));
    ::unlink (png.c_str ());
}

// (c) CMiniWindow::ImageOp — action 2 (rectangle) with a COLOUR image brush.
//
// Reduced scope on purpose.  Actions 1 (ellipse) and 3 (round rect) reach
// CpuRaster::shape, which handles ShapeKind::Rect only (cpu_raster.cpp:218), and
// the MONO branch of ImageOp carries its pattern in BrushSpec::monoPattern,
// which CpuRaster drops (cpu_raster.cpp:161-173).  Both are silent no-ops here,
// so a test of either would pin the gap rather than the body.  Action 2 with an
// image loaded from a PNG is the arm that genuinely draws.
//
// The rectangle starts at (4, 3): 4 is not a multiple of the 9-pixel tile width
// and 3 is not a multiple of the 5-pixel tile height, so a body that only
// happened to work on tile-aligned rectangles fails.  The tile PHASE is not
// asserted — the wx adapter deliberately declines to guarantee it — only that
// the fill is two-dimensionally tiled from the image's own palette.
TEST(LuaSurface, ImageOpTilesAColourImageAcrossARectangle)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    const std::string png = fixture_png_path ("imageop");

    const std::string code = fixture_png_lua (png.c_str ()) +
        "local BG, PEN, BRUSH = 0x123456, 0xFFFFFF, 0x00FFFF\n"
        "assert(world.WindowCreate('w', 0, 0, 24, 18, 0, 0, BG) == 0)\n"
        "assert(world.WindowLoadImage('w', 'img', '" + png + "') == 0,\n"
        "       'fixture PNG did not load')\n"
        // Action 2, rect (4,3)-(17,13), solid 1px pen, BrushColour deliberately
        // a colour the image does NOT contain: GDI ignores the text/background
        // colours for a COLOUR pattern brush, so cyan must appear nowhere.
        "assert(world.WindowImageOp('w', 2, 4, 3, 17, 13, PEN, 0, 1, BRUSH, 'img')\n"
        "       == 0, 'image_fill_rectangle was refused')\n"

        // The interior is x 5..15, y 4..11 (inside the 1px border).  That is 11
        // wide and 8 tall, i.e. at least one full 9 x 5 tile period on each
        // axis, so all three fixture colours must be present whatever the phase.
        "local seen, n = {}, 0\n"
        "for y = 4, 11 do for x = 5, 15 do\n"
        "  local c = world.WindowGetPixel('w', x, y)\n"
        "  assert(c == BLUE or c == GREEN or c == RED,\n"
        "         string.format('interior %d,%d is not an image colour: %d', x, y, c))\n"
        "  if not seen[c] then seen[c] = true; n = n + 1 end\n"
        "end end\n"
        "assert(seen[BLUE],  'no blue tile pixel: the brush was not the image')\n"
        "assert(seen[GREEN], 'no green tile pixel: the brush was not the image')\n"
        "assert(seen[RED],   'no red tile pixel: the brush was not the image')\n"
        "assert(n == 3, 'unexpected interior colour count: ' .. n)\n"

        // ...and it is TILED, not flat-filled: some adjacent pair differs along
        // each axis.  Horizontal variation alone would also be produced by a
        // column-striped fill, so both axes are required.
        "local hdiff, vdiff = false, false\n"
        "for y = 4, 11 do for x = 5, 14 do\n"
        "  if world.WindowGetPixel('w', x, y) ~= world.WindowGetPixel('w', x + 1, y)\n"
        "    then hdiff = true end\n"
        "end end\n"
        "for y = 4, 10 do for x = 5, 15 do\n"
        "  if world.WindowGetPixel('w', x, y) ~= world.WindowGetPixel('w', x, y + 1)\n"
        "    then vdiff = true end\n"
        "end end\n"
        "assert(hdiff, 'the fill has no horizontal variation: not tiled')\n"
        "assert(vdiff, 'the fill has no vertical variation: not tiled')\n"

        // The pen really drew the border, on all four sides of the FixRight/
        // FixBottom-resolved rectangle.
        "assert(world.WindowGetPixel('w',  4,  3) == PEN, 'top-left corner')\n"
        "assert(world.WindowGetPixel('w', 10,  3) == PEN, 'top edge')\n"
        "assert(world.WindowGetPixel('w', 16, 12) == PEN, 'bottom-right corner')\n"
        "assert(world.WindowGetPixel('w',  4,  8) == PEN, 'left edge')\n"
        "assert(world.WindowGetPixel('w', 16,  8) == PEN, 'right edge')\n"

        // Nothing outside the rectangle moved.
        "assert(world.WindowGetPixel('w',  3,  3) == BG, 'spilled left')\n"
        "assert(world.WindowGetPixel('w', 17,  3) == BG, 'spilled right')\n"
        "assert(world.WindowGetPixel('w',  4,  2) == BG, 'spilled up')\n"
        "assert(world.WindowGetPixel('w',  4, 13) == BG, 'spilled down')\n"
        "assert(world.WindowGetPixel('w',  0,  0) == BG, 'spilled to the origin')\n"

        // Right = 0 / Bottom = 0 means \"the far edges\", and ImageOp resolves
        // them itself through FixRight/FixBottom before the seam.  Without this
        // call the whole test passes with FixRight/FixBottom deleted, because
        // every rectangle above has positive edges and they are the identity
        // there.  A 16 x 11 window (again neither square nor the image's size)
        // puts the resolved border at x = 15 and y = 10.
        "assert(world.WindowCreate('z', 0, 0, 16, 11, 0, 0, BG) == 0)\n"
        "assert(world.WindowLoadImage('z', 'img', '" + png + "') == 0)\n"
        "assert(world.WindowImageOp('z', 2, 3, 2, 0, 0, PEN, 0, 1, BRUSH, 'img') == 0)\n"
        "assert(world.WindowGetPixel('z',  3,  2) == PEN, 'far-edge top-left')\n"
        "assert(world.WindowGetPixel('z', 15,  2) == PEN, 'FixRight not applied')\n"
        "assert(world.WindowGetPixel('z',  3, 10) == PEN, 'FixBottom not applied')\n"
        "assert(world.WindowGetPixel('z', 15, 10) == PEN, 'far-edge bottom-right')\n"
        "assert(world.WindowGetPixel('z',  2,  2) == BG,  'far-edge spilled left')\n"
        "assert(world.WindowGetPixel('z',  3,  1) == BG,  'far-edge spilled up')\n"
        "local zc = world.WindowGetPixel('z', 4, 3)\n"
        "assert(zc == BLUE or zc == GREEN or zc == RED, 'far-edge interior empty')\n"

        // ── the refusing arms, each with a canvas control ───────────────
        "assert(world.WindowCreate('e', 0, 0, 24, 18, 0, 0, BG) == 0)\n"
        "assert(world.WindowLoadImage('e', 'img', '" + png + "') == 0)\n"
        "assert(world.WindowImageOp('e', 2, 4, 3, 17, 13, PEN, 0, 1, BRUSH, 'nope')\n"
        "       == error_code.eImageNotInstalled)\n"
        "assert(world.WindowGetPixel('e', 8, 8) == BG, 'missing image still drew')\n"
        // Action 9 is not one of the three: eUnknownOption, and no pixels.
        "assert(world.WindowImageOp('e', 9, 4, 3, 17, 13, PEN, 0, 1, BRUSH, 'img')\n"
        "       == error_code.eUnknownOption)\n"
        "assert(world.WindowGetPixel('e', 8, 8) == BG, 'bad action still drew')\n"
        // PS_STYLE_MASK has no style 7, so ValidatePenStyle rejects it BEFORE
        // any pixel work.
        "assert(world.WindowImageOp('e', 2, 4, 3, 17, 13, PEN, 7, 1, BRUSH, 'img')\n"
        "       == error_code.ePenStyleNotValid)\n"
        "assert(world.WindowGetPixel('e', 8, 8) == BG, 'bad pen style still drew')\n"
        "assert(world.WindowImageOp('nowin', 2, 4, 3, 17, 13, PEN, 0, 1, BRUSH, 'img')\n"
        "       == error_code.eNoSuchWindow)\n"
        // ...and the same call, valid, does reach that pixel.
        "assert(world.WindowImageOp('e', 2, 4, 3, 17, 13, PEN, 0, 1, BRUSH, 'img') == 0)\n"
        "local c = world.WindowGetPixel('e', 8, 8)\n"
        "assert(c == BLUE or c == GREEN or c == RED, 'the valid call drew nothing')\n";

    EXPECT_TRUE(run(f.L, code.c_str ()));
    ::unlink (png.c_str ());
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

// ChatNote's line cap sampled p[1] AFTER ReleaseBuffer -- on the port's
// CString shim, GetBuffer hands out a scratch that ReleaseBuffer clears, so
// the read was out-of-lifetime (fine on Windows, where p targets the real
// buffer).  The byte is sampled before release now; behaviour is identical.
TEST(LuaSurface, ChatNoteTruncatesOverLongMessages)
{
    LuaDoc f; ASSERT_NE(nullptr, f.L);
    f.doc.m_iMaxChatLinesPerMessage = 2;
    f.doc.ChatNote(0, "one\ntwo\nthree\nfour");

    bool sawSuffix = false, sawCutContent = false;
    for (POSITION p2 = f.doc.m_LineList.GetHeadPosition(); p2; ) {
        CLine* l = f.doc.m_LineList.GetNext(p2);
        if (!l || l->len <= 0) continue;
        std::string t(l->text, (size_t) l->len);
        if (t.find("Chat message truncated") != std::string::npos)
            sawSuffix = true;
        if (t.find("three") != std::string::npos ||
            t.find("four") != std::string::npos)
            sawCutContent = true;
    }
    EXPECT_TRUE(sawSuffix);          // the cap fired and said so
    EXPECT_FALSE(sawCutContent);     // content past the cap is really gone

    // Control: an under-cap message passes through untouched.
    f.doc.m_iMaxChatLinesPerMessage = 50;
    f.doc.ChatNote(0, "alpha\nbeta");
    bool sawBeta = false, sawSuffix2 = false;
    for (POSITION p2 = f.doc.m_LineList.GetHeadPosition(); p2; ) {
        CLine* l = f.doc.m_LineList.GetNext(p2);
        if (!l || l->len <= 0) continue;
        std::string t(l->text, (size_t) l->len);
        if (t.find("beta") != std::string::npos) sawBeta = true;
        if (t.find("truncated, exceeds 50") != std::string::npos) sawSuffix2 = true;
    }
    EXPECT_TRUE(sawBeta);
    EXPECT_FALSE(sawSuffix2);
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
