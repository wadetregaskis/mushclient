// tests/test_menu_rules.cpp — the port's main-menu enable/check rules.
//
// On Windows MFC greys a menu item when nothing in the command-routing chain
// handles it, so with no world open every command whose only ON_COMMAND lives
// on CMUSHView / CSendView / CMUSHclientDoc is disabled.  wxWidgets does the
// opposite (an item nobody speaks for stays enabled), which left a dozen
// world-only commands live and silently doing nothing, and left the two
// per-world diagnostics toggles showing a stale tick from another world.
//
// platform/menu_ui_rules.cpp restates MFC's answer; this pins it.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "platform/menu_ui_rules.h"
#include "fake_worldview.h"

// ── world-only commands ────────────────────────────────────────────────────

// Every id here has its canonical handler ONLY on the view or the document, so
// Windows greys it with no world open.  Listed literally rather than looped out
// of the implementation's own table, or the test would agree with any table.
static const int kWorldOnly[] = {
    32248,  // ID_DISPLAY_FIND
    32249,  // ID_DISPLAY_FINDAGAIN
    32201,  // ID_DISPLAY_FREEZEOUTPUT
    32963,  // ID_DISPLAY_GOTOLINE
    32250,  // ID_FILE_PRINT_WORLD
    32275,  // ID_DISPLAY_GOTOURL
    32276,  // ID_DISPLAY_SENDMAILTO
    33036,  // ID_DISPLAY_HIGHLIGHTPHRASE
    33044,  // ID_DISPLAY_MULTILINETRIGGER
    32331,  // ID_DISPLAY_TEXTATTRIBUTES
    32318,  // ID_DISPLAY_BOOKMARKSELECTION
    32319,  // ID_DISPLAY_GOTOBOOKMARK
    32320,  // ID_INPUT_GLOBALCHANGE
    32323,  // ID_EDIT_SPELLCHECK
    32326,  // ID_EDIT_GENERATECHARACTERNAME
    32272,  // ID_REPEAT_LAST_COMMAND
    32152,  // ID_GAME_COMMANDHISTORY
    32946,  // ID_EDIT_GOTOMATCHINGBRACE
    32947,  // ID_EDIT_SELECTTOMATCHINGBRACE
    33046,  // ID_GAME_FUNCTIONSLIST
    32953,  // ID_EDIT_FLIPTONOTEPAD
    32983,  // ID_FILE_PLUGINS
    32971,  // ID_EDIT_DEBUGINCOMINGPACKETS
    32334,  // ID_GAME_TRACE
};

TEST(MenuRules, WorldOnlyCommandsAreDisabledWithNoWorld) {
    for (int id : kWorldOnly) {
        const MenuCmdUiState st = MenuCommandUiState(id, nullptr);
        if (!st.handled)
            printf("      (id %d is not covered by the rules)\n", id);
        EXPECT_TRUE(st.handled);
        EXPECT_FALSE(st.enabled);
    }
}

TEST(MenuRules, WorldOnlyCommandsAreEnabledWithAWorld) {
    CMUSHclientDoc doc;
    for (int id : kWorldOnly) {
        const MenuCmdUiState st = MenuCommandUiState(id, &doc);
        EXPECT_TRUE(st.handled);
        EXPECT_TRUE(st.enabled);
    }
}

// Commands that work with no world open must NOT be swept into the rules —
// a table that greyed everything would pass the two tests above.
TEST(MenuRules, GlobalCommandsAreLeftAlone) {
    static const int kGlobal[] = {
        32954,  // ID_FILE_OPENWORLDSINSTARTUPLIST  (CMainFrame)
        32327,  // ID_EDIT_RELOADNAMESFILE          (CMainFrame)
        32203,  // ID_DISPLAY_ACTIVITYLIST          (CMainFrame, with its own update-UI)
        32325,  // ID_GAME_SENDTOALLWORLDS          (CMainFrame)
        32234,  // ID_GAME_MINIMISEPROGRAM          (the app)
        0xE140, // ID_APP_ABOUT                     (the app)
    };
    for (int id : kGlobal) {
        const MenuCmdUiState st = MenuCommandUiState(id, nullptr);
        EXPECT_FALSE(st.handled);
    }
}

// ── the per-world check marks ──────────────────────────────────────────────
// Trace and Debug Incoming Packets are per-DOCUMENT flags.  The port used to
// tick them once, at click time, so the tick described whichever world was
// active then and went stale as soon as the user switched worlds — clicking a
// ticked item then turned tracing ON in the new world.

TEST(MenuRules, TraceAndDebugChecksFollowTheActiveWorld) {
    CMUSHclientDoc a, b;
    a.m_bTrace = true;
    a.m_bDebugIncomingPackets = false;
    b.m_bTrace = false;
    b.m_bDebugIncomingPackets = true;

    EXPECT_EQ(1, MenuCommandUiState(32334 /*Trace*/, &a).checked);
    EXPECT_EQ(0, MenuCommandUiState(32334, &b).checked);
    EXPECT_EQ(0, MenuCommandUiState(32971 /*Debug packets*/, &a).checked);
    EXPECT_EQ(1, MenuCommandUiState(32971, &b).checked);

    // Toggling the flag flips the tick on the next menu draw.
    a.m_bTrace = false;
    EXPECT_EQ(0, MenuCommandUiState(32334, &a).checked);

    // With no world there is nothing to tick, and the item is dead anyway.
    const MenuCmdUiState none = MenuCommandUiState(32334, nullptr);
    EXPECT_FALSE(none.enabled);
    EXPECT_EQ(0, none.checked);
}

// Freeze Output is view state (CMUSHView::m_freeze on Windows, the output
// pane's autoscroll pause here), pulled back through pal::IWorldView.
TEST(MenuRules, FreezeOutputChecksFollowTheOutputPane) {
    CMUSHclientDoc doc;
    FakeWorldView view;
    doc.m_pWorldView = &view;

    EXPECT_EQ(0, MenuCommandUiState(32201, &doc).checked);
    view.setPaused(true);
    EXPECT_EQ(1, MenuCommandUiState(32201, &doc).checked);
    view.setPaused(false);
    EXPECT_EQ(0, MenuCommandUiState(32201, &doc).checked);

    doc.m_pWorldView = nullptr;   // no view attached: never frozen, never crashes
    EXPECT_EQ(0, MenuCommandUiState(32201, &doc).checked);
}

// Only the three toggles are check items; the plain commands must report -1 or
// the frame would draw a permanent empty tick box beside every one of them.
TEST(MenuRules, PlainCommandsAreNotCheckItems) {
    CMUSHclientDoc doc;
    EXPECT_EQ(-1, MenuCommandUiState(32248 /*Find*/, &doc).checked);
    EXPECT_EQ(-1, MenuCommandUiState(32320 /*Global Change*/, &doc).checked);
    EXPECT_EQ(-1, MenuCommandUiState(32250 /*Print World*/, &doc).checked);
}

// ── "Quit From This World" needs a live session, not just a world ──────────
// sendvw.cpp routes ID_ACTIONS_QUIT's update-UI to CSendView::OnUpdateSessionOpen.
TEST(MenuRules, QuitFromThisWorldNeedsAConnectedSession) {
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectNotConnected;
    EXPECT_TRUE(MenuCommandUiState(32217, &doc).handled);
    EXPECT_FALSE(MenuCommandUiState(32217, &doc).enabled);

    doc.m_iConnectPhase = eConnectConnectedToMud;
    EXPECT_TRUE(MenuCommandUiState(32217, &doc).enabled);

    EXPECT_FALSE(MenuCommandUiState(32217, nullptr).enabled);
}

// ── canonical ids world.DoCommand posts ────────────────────────────────────
// StringToCommandID hands back the canonical MFC id; the port's menu bar builds
// these particular commands on wx's own ids, so the frame has to recognise both
// or the DoCommand is a silent no-op that still returns eOK.

TEST(MenuRules, CanonicalIdsMapToThePortsOwnCommands) {
    EXPECT_EQ(kPortMenuCloseWorld,   PortMenuActionForCommandId(0xE102)); // ID_FILE_CLOSE
    EXPECT_EQ(kPortMenuCut,          PortMenuActionForCommandId(0xE123)); // ID_EDIT_CUT
    EXPECT_EQ(kPortMenuPaste,        PortMenuActionForCommandId(0xE125)); // ID_EDIT_PASTE
    EXPECT_EQ(kPortMenuSelectAll,    PortMenuActionForCommandId(0xE12A)); // ID_EDIT_SELECT_ALL
    EXPECT_EQ(kPortMenuUndo,         PortMenuActionForCommandId(0xE12B)); // ID_EDIT_UNDO
    EXPECT_EQ(kPortMenuAbout,        PortMenuActionForCommandId(0xE140)); // ID_APP_ABOUT
    EXPECT_EQ(kPortMenuArrangeIcons, PortMenuActionForCommandId(0xE131)); // ID_WINDOW_ARRANGE
    EXPECT_EQ(kPortMenuCascade,      PortMenuActionForCommandId(0xE132)); // ID_WINDOW_CASCADE
    EXPECT_EQ(kPortMenuTileHorz,     PortMenuActionForCommandId(0xE133)); // ID_WINDOW_TILE_HORZ
    EXPECT_EQ(kPortMenuTileVert,     PortMenuActionForCommandId(0xE134)); // ID_WINDOW_TILE_VERT
}

// The ids come from the SAME table world.DoCommand uses, so a rename there
// cannot leave the frame matching an id nothing posts any more.
TEST(MenuRules, DoCommandNamesResolveToTheMappedIds) {
    EXPECT_EQ(kPortMenuCloseWorld, PortMenuActionForCommandId(StringToCommandID("Close")));
    EXPECT_EQ(kPortMenuCut,        PortMenuActionForCommandId(StringToCommandID("Cut")));
    EXPECT_EQ(kPortMenuPaste,      PortMenuActionForCommandId(StringToCommandID("Paste")));
    EXPECT_EQ(kPortMenuSelectAll,  PortMenuActionForCommandId(StringToCommandID("SelectAll")));
    EXPECT_EQ(kPortMenuUndo,       PortMenuActionForCommandId(StringToCommandID("Undo")));
    EXPECT_EQ(kPortMenuAbout,      PortMenuActionForCommandId(StringToCommandID("About")));
    EXPECT_EQ(kPortMenuCascade,    PortMenuActionForCommandId(StringToCommandID("CascadeWindows")));
    EXPECT_EQ(kPortMenuTileHorz,   PortMenuActionForCommandId(StringToCommandID("TileWindows")));
    EXPECT_EQ(kPortMenuTileVert,
              PortMenuActionForCommandId(StringToCommandID("TileWindowsVertically")));
    EXPECT_EQ(kPortMenuArrangeIcons,
              PortMenuActionForCommandId(StringToCommandID("ArrangeIcons")));
}

// Commands the frame already dispatches under their canonical id must NOT be
// translated — Copy in particular deliberately lives on ID_EDIT_COPY here (a
// non-standard id, so AppKit does not hijack it).
TEST(MenuRules, CommandsAlreadyOnCanonicalIdsAreNotTranslated) {
    EXPECT_EQ(kPortMenuNone, PortMenuActionForCommandId(StringToCommandID("Copy")));
    EXPECT_EQ(kPortMenuNone, PortMenuActionForCommandId(StringToCommandID("Connect")));
    EXPECT_EQ(kPortMenuNone, PortMenuActionForCommandId(StringToCommandID("ClearOutput")));
    EXPECT_EQ(kPortMenuNone, PortMenuActionForCommandId(StringToCommandID("ExitClient")));
    EXPECT_EQ(kPortMenuNone, PortMenuActionForCommandId(0));
}

MC_TEST_MAIN()
