// statusitem_selftest.cpp — in-app conformance test for the macOS menu-bar
// ("system tray") item (--statusitem-selftest).
//
// Nothing headless can see any of this.  An NSStatusItem is AppKit, the Obj-C++
// TU that owns it is compiled into the executable alone, and the preference
// that drives it (App.m_iIconPlacement) previously reached a function -- the
// canonical CMainFrame::AddTrayIcon -- that could not possibly do anything.
// So the whole point of this harness is to make "the code did nothing" FAIL:
//
//   * every visibility assertion is read back OUT of AppKit
//     (wxOSXStatusItemVisible / ...ImageWidth), never out of a flag this file
//     set.  A no-op ApplyTrayItemFromPrefs leaves the item absent and the ON
//     cases fail;
//   * the item must carry an image with a non-zero size, so an item that
//     occupies menu-bar space and shows nothing -- the failure mode a missing
//     artwork file would produce -- is a FAILURE, not a pass;
//   * the OFF cases assert the item is GONE.  Without them a version that
//     ignored the preference and always showed the item would pass everything
//     else, which is the over-broad mutation;
//   * the menu is rebuilt THROUGH the NSMenu delegate and read back item by
//     item, so a delegate that never fires fails rather than silently showing
//     an empty menu.
//
// NOT covered, deliberately: the click itself.  A real click on an NSStatusItem
// cannot be synthesised from a process that exits before its event loop starts,
// and mc_statusitem_menu_invoke ends in either CMainFrame::OnTrayMenu (which
// raises a world window) or a wxEVT_MENU on the main frame (About opens a
// modal, Exit quits) -- neither of which a harness can complete.  What IS
// pinned instead is the routing DECISION: section 7 asserts that every id the
// menu emits lands on the correct side of the one predicate that function
// splits on, and that the ids on the far side are ones the frame's dispatcher
// actually handles.  Also unpinned: MDI child activation is asynchronous in a
// harness (see world_frame_selftest.cpp:347), so asserting "the right world
// came forward" here would be a flake, not a test.
//
// Fixture note (degenerate-fixture trap): the worlds are deliberately named so
// that alphabetical order is the REVERSE of view-number order, because the menu
// sorts by name while the command id is built from the view number.  With
// worlds named in view order, "sorted position" and "view number - 1" would be
// the same integer and the conversion under test would be an identity that
// nothing could discriminate.

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/mdi.h>

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "mainfrm.h"          // TRAY_FIRST_MENU
#include "childfrm.h"
#include "mushview.h"
#include "resource.h"

#include "platform/wx_statusitem.h"
#include "platform/wx_osx_statusitem.h"
#include "platform/wx_dialog_bridge.h"   // LoadDialogArt — the bundled tray artwork
#include "platform/menu_ui_rules.h"      // PortMenuActionForCommandId — the frame's own routing

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

void checkf(bool ok, const std::string& what)
{
    check(ok, what.c_str());
}

// The model as the delegate would pull it, flattened for readability in the log.
std::string describe(const std::vector<TrayMenuEntry>& m)
{
    std::string s;
    for (size_t i = 0; i < m.size(); ++i)
    {
        char buf[128];
        if (m[i].command == 0)
            snprintf(buf, sizeof buf, "%s<sep>", i ? " | " : "");
        else
            snprintf(buf, sizeof buf, "%s%s#%d%s", i ? " | " : "",
                     m[i].label.c_str(), m[i].command, m[i].enabled ? "" : "(dim)");
        s += buf;
    }
    return s;
}

// Read one NSMenuItem back out of the built menu.
struct BuiltItem {
    std::string title;
    int  tag       = 0;
    bool enabled   = false;
    bool separator = false;
    bool present   = false;
};

BuiltItem builtItem(int index)
{
    BuiltItem b;
    char title[256] = { 0 };
    int tag = 0, enabled = 0, separator = 0;
    b.present   = wxOSXStatusItemMenuItemAt(index, title, (int) sizeof title,
                                            &tag, &enabled, &separator);
    b.title     = title;
    b.tag       = tag;
    b.enabled   = enabled != 0;
    b.separator = separator != 0;
    return b;
}

CMUSHclientDoc* makeWorld(const char* name, int viewNumber, int newLines)
{
    CMUSHclientDoc* doc = new CMUSHclientDoc();
    doc->m_mush_name   = name;
    doc->m_view_number = viewNumber;
    doc->m_new_lines   = newLines;
    // In the GUI build the document does NOT register itself: doc_construct.cpp's
    // AddDocument is inside #if MUSHCLIENT_HEADLESS_APP, and mushgui registers
    // from the CWorldChildFrame constructor instead (wx_world_child.cpp:1872).
    // This harness builds no frames, so it must register them itself -- which is
    // what world_frame_selftest.cpp:2868 and engines_selftest.cpp:1195 also do.
    App.m_pWorldDocTemplate->AddDocument(doc);
    return doc;
}

} // namespace

int RunStatusItemSelfTest()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("STATUSITEM-SELFTEST: the macOS menu-bar item and its menu\n");

    // The bundled artwork must be reachable, or every icon assertion below
    // would be testing the fallback instead of the real path.
    check(LoadDialogArt(129).IsOk(),
          "the default tray artwork (IDR_MUSHCLTYPE / doc.ico) is bundled");

    // ── 1. the preference decides, in both directions ─────────────────────
    // Read back from AppKit each time -- see the header comment.

    App.m_iIconPlacement = ICON_PLACEMENT_TASKBAR;
    ApplyTrayItemFromPrefs();
    check(!wxOSXStatusItemVisible(),
          "Icon Placement = Task bar: no menu-bar item");

    App.m_iIconPlacement = ICON_PLACEMENT_TRAY;
    App.m_iTrayIcon      = 0;
    ApplyTrayItemFromPrefs();
    const bool shown = wxOSXStatusItemVisible();
    check(shown, "Icon Placement = System tray: the menu-bar item exists");
    if (!shown)
    {
        // Nothing below can mean anything without an item; say so once, loudly,
        // rather than emitting a dozen derived failures.
        printf("STATUSITEM-SELFTEST: no NSStatusItem could be created in this "
               "process -- the remaining assertions are not meaningful.\n");
        printf("STATUSITEM-SELFTEST: %d passed, %d FAILED\n", g_pass, g_fail);
        return g_fail;
    }

    // A blank item would occupy menu-bar space and show nothing: the exact
    // failure a missing artwork file produces, and the reason the .mm has a
    // drawn placeholder.  Assert there is a real image with a real size.
    checkf(wxOSXStatusItemImageWidth()  > 0 &&
           wxOSXStatusItemImageHeight() > 0,
           "the item shows an image with a non-zero size ("
           + std::to_string(wxOSXStatusItemImageWidth()) + "x"
           + std::to_string(wxOSXStatusItemImageHeight()) + ")");

    App.m_iIconPlacement = ICON_PLACEMENT_BOTH;
    ApplyTrayItemFromPrefs();
    check(wxOSXStatusItemVisible(),
          "Icon Placement = Both: the menu-bar item exists");

    // The direction that only exists because of the port: nothing canonical
    // removes a tray icon, so without this the preferences dialog could turn
    // the item on but never off.
    App.m_iIconPlacement = ICON_PLACEMENT_TASKBAR;
    ApplyTrayItemFromPrefs();
    check(!wxOSXStatusItemVisible(),
          "switching back to Task bar REMOVES the menu-bar item");

    // ── 2. a custom icon still yields a real image ────────────────────────
    App.m_iIconPlacement = ICON_PLACEMENT_TRAY;
    App.m_iTrayIcon      = 4;                 // IDI_ICON4 (Earth.ico)
    ApplyTrayItemFromPrefs();
    check(wxOSXStatusItemVisible() && wxOSXStatusItemImageWidth() > 0,
          "Tray Icon = 4 (Earth) also produces a sized image");

    // An index with no artwork must fall back, NOT go blank.  10 means "the
    // file named by TrayIconFileName", and an unset/bad name is the case that
    // would otherwise leave nothing to draw.
    App.m_iTrayIcon           = 10;
    App.m_strTrayIconFileName = "/nonexistent/definitely-not-here.ico";
    ApplyTrayItemFromPrefs();
    check(wxOSXStatusItemVisible() && wxOSXStatusItemImageWidth() > 0,
          "an unloadable custom icon file falls back to a visible image");
    App.m_iTrayIcon           = 0;
    App.m_strTrayIconFileName = "";
    ApplyTrayItemFromPrefs();

    // ── 3. the menu, with no worlds open ──────────────────────────────────
    // InitInstance never runs in a selftest, so there is no doc template yet:
    // BuildTrayMenu must survive that (it did not have to be told -- assert it).
    {
        std::vector<TrayMenuEntry> m = BuildTrayMenu();
        printf("  menu (no template): %s\n", describe(m).c_str());
        check(!m.empty(), "the menu is built even before InitInstance");
        check(!m.empty() && m[0].label == "(no worlds open)" && !m[0].enabled,
              "with no worlds it opens with a disabled \"(no worlds open)\"");
    }

    // The IDR_RIGHT_TRAY_MENU tail, read out of the recovered resource rather
    // than hard-coded here -- but its two canonical ids ARE pinned, because
    // they are what the frame's dispatcher matches on.
    {
        std::vector<TrayMenuEntry> m = BuildTrayMenu();
        bool sawSeparator = false, sawAbout = false, sawExit = false;
        for (size_t i = 0; i < m.size(); ++i)
        {
            if (m[i].command == 0)                sawSeparator = true;
            if (m[i].command == 0xE140 /*ID_APP_ABOUT*/) sawAbout = true;
            if (m[i].command == 0xE141 /*ID_APP_EXIT*/)  sawExit  = true;
        }
        check(sawSeparator, "the worlds are separated from the app commands");
        check(sawAbout, "IDR_RIGHT_TRAY_MENU's About item is in the menu (ID_APP_ABOUT)");
        check(sawExit,  "IDR_RIGHT_TRAY_MENU's Exit item is in the menu (ID_APP_EXIT)");
    }

    // ── 4. the menu, with worlds open ─────────────────────────────────────
    // Non-degenerate on purpose: alphabetical order is the reverse of view
    // order, so "position in the menu" and "view number - 1" cannot be
    // confused, and only one world has unread lines.
    if (App.m_pWorldDocTemplate == NULL)
        App.m_pWorldDocTemplate = new CMultiDocTemplate(
            IDR_MUSHCLTYPE,
            RUNTIME_CLASS(CMUSHclientDoc),
            RUNTIME_CLASS(CChildFrame),
            RUNTIME_CLASS(CMUSHView));

    CMUSHclientDoc* zeta  = makeWorld("Zeta",  1, 0);   // view 1 -> id 11000
    CMUSHclientDoc* mid   = makeWorld("Mid",   2, 7);   // view 2 -> id 11001, 7 unread
    CMUSHclientDoc* alpha = makeWorld("Alpha", 3, 0);   // view 3 -> id 11002

    {
        std::vector<TrayMenuEntry> m = BuildTrayMenu();
        printf("  menu (3 worlds): %s\n", describe(m).c_str());

        // EXACT, not >=: three world rows + the separator + IDR_RIGHT_TRAY_MENU's
        // two items.  ">= 3" would have passed on the four-row no-worlds menu,
        // which is precisely the state a broken world walk leaves behind.
        const size_t tail = 1 /*separator*/ + 2 /*About, Exit*/;
        checkf(m.size() == 3 + tail,
               "three worlds produce exactly " + std::to_string(3 + tail) +
                   " rows (got " + std::to_string(m.size()) + ")");
        if (m.size() >= 3)
        {
            // Sorted by displayed NAME: Alpha, Mid (7), Zeta.
            check(m[0].label == "Alpha", "sorted by name: Alpha first");
            check(m[1].label == "Mid (7)",
                  "a world with unread lines is labelled \"Mid (7)\"");
            check(m[2].label == "Zeta", "sorted by name: Zeta last");

            // ...but the COMMAND is built from the view number, which here runs
            // the other way.  This is the assertion the fixture is shaped for.
            checkf(m[0].command == TRAY_FIRST_MENU + 2,
                   "Alpha (view 3) carries id " +
                       std::to_string(TRAY_FIRST_MENU + 2) + ", not its menu position");
            checkf(m[1].command == TRAY_FIRST_MENU + 1,
                   "Mid (view 2) carries id " + std::to_string(TRAY_FIRST_MENU + 1));
            checkf(m[2].command == TRAY_FIRST_MENU + 0,
                   "Zeta (view 1) carries id " + std::to_string(TRAY_FIRST_MENU + 0));

            check(m[0].enabled && m[1].enabled && m[2].enabled,
                  "the world rows are enabled");
        }

        bool sawPlaceholder = false;
        for (size_t i = 0; i < m.size(); ++i)
            if (m[i].label == "(no worlds open)")
                sawPlaceholder = true;
        check(!sawPlaceholder,
              "the \"(no worlds open)\" placeholder is gone once worlds exist");
    }

    // ── 5. the NSMenu delegate actually builds all of that ────────────────
    // Everything above tests the model.  This drives the real delegate --
    // the path a click takes -- and reads AppKit's own NSMenuItems back.
    {
        std::vector<TrayMenuEntry> model = BuildTrayMenu();
        const int built = wxOSXStatusItemRebuildMenu();
        checkf(built == (int) model.size(),
               "the NSMenu delegate built all " + std::to_string(model.size()) +
                   " rows (got " + std::to_string(built) + ")");

        BuiltItem first = builtItem(0);
        check(first.present, "the built menu has a first item");
        check(first.present && first.title == "Alpha",
              "the first NSMenuItem is titled \"Alpha\"");
        checkf(first.present && first.tag == TRAY_FIRST_MENU + 2,
               "and carries tag " + std::to_string(TRAY_FIRST_MENU + 2) +
                   " (got " + std::to_string(first.tag) + ")");
        check(first.present && first.enabled && !first.separator,
              "and is an enabled, non-separator item");

        // The separator has to be a real NSMenuItem separator, not an item
        // titled "" -- an item would be clickable and would look like a bug.
        int sepIndex = -1;
        for (size_t i = 0; i < model.size(); ++i)
            if (model[i].command == 0) { sepIndex = (int) i; break; }
        checkf(sepIndex >= 0, "the model has a separator row");
        if (sepIndex >= 0)
        {
            BuiltItem sep = builtItem(sepIndex);
            check(sep.present && sep.separator,
                  "and AppKit built it as a real separatorItem");
        }

        // The last row is the last of IDR_RIGHT_TRAY_MENU's items; assert the
        // tag survived the C boundary, since that tag is the whole dispatch.
        BuiltItem last = builtItem((int) model.size() - 1);
        checkf(last.present && last.tag == model.back().command,
               "the last NSMenuItem carries command id " +
                   std::to_string(model.back().command));
        check(last.present && !last.title.empty(),
              "and has a non-empty title from the recovered resource");
    }

    // ── 6. a disabled row stays disabled in AppKit ────────────────────────
    // NSMenu autoenabling would silently re-enable "(no worlds open)".  Close
    // the worlds and confirm the built item is really dim.
    // Unregister rather than delete: ~CMUSHclientDoc tears down the line list
    // and the plugin/script state, none of which these three stubs ever set up,
    // and the menu only ever sees the template's registry anyway.  What is
    // under test is "no worlds are registered", which this produces exactly.
    App.m_pWorldDocTemplate->RemoveDocument(alpha);
    App.m_pWorldDocTemplate->RemoveDocument(mid);
    App.m_pWorldDocTemplate->RemoveDocument(zeta);
    {
        std::vector<TrayMenuEntry> m = BuildTrayMenu();
        check(!m.empty() && m[0].label == "(no worlds open)",
              "closing every world restores the placeholder");
        wxOSXStatusItemRebuildMenu();
        BuiltItem b = builtItem(0);
        check(b.present && !b.enabled,
              "and AppKit shows it disabled (autoenabling does not override us)");
    }

    // ── 7. every id the menu emits is one the dispatcher can route ────────
    // mc_statusitem_menu_invoke splits on exactly one predicate: is the command
    // inside [TRAY_FIRST_MENU, TRAY_FIRST_MENU + TRAY_MENU_COUNT)?  Inside goes
    // to CMainFrame::OnTrayMenu, outside goes to the main frame's menu
    // dispatcher.  A real click cannot be synthesised onto an NSStatusItem from
    // here, so the CHOICE is what gets pinned: each side of the split must send
    // its ids to a handler that exists.
    {
        // Re-open the worlds so both sides of the split are populated.
        App.m_pWorldDocTemplate->AddDocument(zeta);
        App.m_pWorldDocTemplate->AddDocument(mid);
        App.m_pWorldDocTemplate->AddDocument(alpha);
        std::vector<TrayMenuEntry> m = BuildTrayMenu();

        int inRange = 0, outOfRange = 0;
        bool everyOutsideIsRoutable = true;
        for (size_t i = 0; i < m.size(); ++i)
        {
            const int c = m[i].command;
            if (c == 0)
                continue;                          // separator
            if (c >= TRAY_FIRST_MENU && c < TRAY_FIRST_MENU + TRAY_MENU_COUNT)
                { ++inRange; continue; }
            ++outOfRange;
            // ID_APP_ABOUT is translated by the frame's dispatcher; ID_APP_EXIT
            // is matched literally there (wx_mfc_impl.cpp:2047).  Anything else
            // would fall through to evt.Skip() and do nothing.
            if (PortMenuActionForCommandId(c) == kPortMenuNone &&
                c != 0xE141 /*ID_APP_EXIT*/)
                everyOutsideIsRoutable = false;
        }
        checkf(inRange == 3,
               "the three world rows take the OnTrayMenu branch (got " +
                   std::to_string(inRange) + ")");
        checkf(outOfRange == 2,
               "the two app rows take the main-frame branch (got " +
                   std::to_string(outOfRange) + ")");
        check(everyOutsideIsRoutable,
              "...and every one of those ids is one the frame dispatcher handles");

        App.m_pWorldDocTemplate->RemoveDocument(alpha);
        App.m_pWorldDocTemplate->RemoveDocument(mid);
        App.m_pWorldDocTemplate->RemoveDocument(zeta);
    }

    // Leave nothing behind in the user's menu bar.
    App.m_iIconPlacement = ICON_PLACEMENT_TASKBAR;
    ApplyTrayItemFromPrefs();
    check(!wxOSXStatusItemVisible(), "the harness removes its own menu-bar item");

    printf("STATUSITEM-SELFTEST: %d passed, %d FAILED\n", g_pass, g_fail);
    return g_fail;
}

#endif // !_WIN32
