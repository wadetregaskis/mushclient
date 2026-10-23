// platform/menubar_selftest.cpp — in-app conformance test for the REAL menu
// bar (--menubar-selftest).
//
// Everything else in the suite tests the commands; nothing tested the menu.
// That gap matters more than it sounds, because the two ways a menu item fails
// are both silent:
//
//   * an accelerator string wxWidgets cannot parse produces an item with NO
//     shortcut and no error, so the key does nothing and the menu still looks
//     right; and
//   * two items sharing an accelerator produce one item that never fires,
//     with no warning from wx or from AppKit.
//
// Neither is visible without pressing the key, and neither is reachable from
// the 37 headless executables — the menu bar is built inside the MFC→wx bridge
// hook, which only the app links.  So build it for real, here, and read it
// back.
//
// The hook does the whole job: window, geometry restore, menu bar, status bar,
// timers.  Nothing it does writes to the user's config (the geometry SAVE is
// on close, which we never reach), and we exit before InitInstance, so no
// world is opened.

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/mdi.h>
#include <wx/menu.h>
#include <wx/accel.h>
#include <wx/filename.h>   // wxFileName — the Open Recent labels are basenames
// Before stdafx.h, like every other wx header here: compat.h defines Windows
// macros that otherwise clobber wx's own.  app_config.h below needs it.
#include <wx/config.h>

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "childfrm.h"
#include "mainfrm.h"          // CMainFrame — m_bActive / m_bFlashingWindow
#include "mushview.h"
#include "resource.h"
#include "platform/wx_world_child.h"
#include "platform/wx_outputpane.h"   // BuildContextMenu — the second of the two
                                      // logging entry points
#include "dialogs/logdlg.h"           // CLogDlg — answered by a hook below
#include "platform/wx_mfc_hooks.h"
#include "platform/wx_mfc_logic.h"   // RecentWorlds* — the MRU behind File ▸ Open Recent
#include "platform/wx_toolbar.h"     // CWorldToolBars — the Connect/Disconnect toggle
#include "platform/app_config.h"     // OpenAppConfig — redirected around the toolbar blocks
#include "platform/menu_ui_rules.h"  // kPortMenuIdDisconnect — the id both must agree on
#include "platform/wx_activity.h"    // ActivityWindowIsOpen — the Activity List tick
#include "dialogs/world_prefs/TreePropertySheet.h"   // m_iWantedPage — which page a Configure command asked for

// Defined in wx_toolbar.cpp: the world in activity slot 1..10.  The menu/
// startup ID_WORLDS_WORLD* commands resolve their world through this same
// lookup, which is the point of asserting on it here.
CMUSHclientDoc* wx_world_in_slot(int slot);

// Defined in wx_mfc_impl.cpp: drop a world from the doc→frame registry.
// Closing a world normally does this from CWorldChildFrame::OnClose, via
// RunCloseTeardown -- but this harness tears its worlds down with Destroy(),
// which never runs OnClose.  The entry would then outlive the frame, and the
// next command that activates a world by slot would dereference freed memory
// (seen: an intermittent SIGSEGV in the world-switch block below).
void wx_unregister_world_frame(CMUSHclientDoc* doc);

#include <cstdio>
#include <map>
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

// One menu item, flattened out of the tree.
struct Item {
    int         id;
    std::string path;      // "Display > Clear to Start"
    std::string label;     // accelerator stripped
    std::string accel;     // "" when the item has none
    int         accelFlags = 0;
    int         accelKey   = 0;
};

void flatten(const wxMenu* menu, const std::string& prefix, std::vector<Item>& out)
{
    for (const wxMenuItem* mi : menu->GetMenuItems()) {
        if (mi->IsSeparator()) continue;
        const wxString raw = mi->GetItemLabel();
        Item it;
        it.id    = mi->GetId();
        it.label = std::string(mi->GetItemLabelText().utf8_str());
        it.path  = prefix + it.label;

        const int tab = raw.Find('\t');
        if (tab != wxNOT_FOUND) {
            const wxString a = raw.Mid(tab + 1);
            it.accel = std::string(a.utf8_str());
            // Parse the way wx itself does when it builds the item, so this
            // reports exactly what the app ended up with.
            wxAcceleratorEntry e;
            if (e.FromString(raw.Mid(tab))) {
                it.accelFlags = e.GetFlags();
                it.accelKey   = e.GetKeyCode();
            }
        }
        out.push_back(it);

        if (mi->IsSubMenu() && mi->GetSubMenu())
            flatten(mi->GetSubMenu(), it.path + " > ", out);
    }
}

const Item* find_by_id(const std::vector<Item>& items, int id)
{
    for (const Item& it : items)
        if (it.id == id) return &it;
    return nullptr;
}

// First item whose label CONTAINS `needle` — for the handful of commands the
// port builds on wxID_HIGHEST-relative ids, which are file-static and so have
// no name out here.  The label is what the user picks anyway.
const Item* find_by_label(const std::vector<Item>& items, const char* needle)
{
    for (const Item& it : items)
        if (it.label.find(needle) != std::string::npos) return &it;
    return nullptr;
}

// ── what the frame answers for one menu command ────────────────────────────
// The frame decides greying, ticking and (for one item) the caption in a
// wxEVT_UPDATE_UI handler.  Nothing ever sent it a wxUpdateUIEvent, so none of
// that had ever run outside the app proper.  It needs no event loop: build the
// event and process it.
struct UiAnswer {
    bool        claimed    = false;   // the handler took it rather than Skip()ing
    bool        setEnabled = false;
    bool        enabled    = true;
    bool        setChecked = false;
    bool        checked    = false;
    bool        setText    = false;
    std::string text;
};

// Fire a menu command at the frame exactly as a menu click or a posted
// WM_COMMAND does.  Returns whether any handler claimed it.
bool fireCmd(wxWindow* frame, int id)
{
    wxCommandEvent evt(wxEVT_MENU, id);
    evt.SetEventObject(frame);
    return frame->GetEventHandler()->ProcessEvent(evt);
}

// The command id a toolbar button forwarded to the main frame.  A plain
// function rather than a lambda so the binding can be undone again: the
// recorder must not outlive the toolbar assertions below.
int g_forwardedCmd = -1;
void recordForwardedCmd(wxCommandEvent& evt) { g_forwardedCmd = evt.GetId(); }

// What File ▸ Save / Save As handed to the single save flow.  Globals rather
// than captures because g_wxhook_save_world takes a plain function pointer.
int   g_saveCalls  = 0;
void* g_saveDoc    = NULL;
bool  g_saveAs     = false;
void* g_saveParent = NULL;


// The wxToolBar inside a CWorldToolBars strip that carries `id`.  The bars are
// private to CWorldToolBars and are built with wxID_ANY, so FindWindowById
// cannot reach them; this walk can, and it is what lets the harness read a
// button's PRESSED state, which is the whole of the defect below.
wxToolBar* findToolBarWith(wxWindow* root, int id)
{
    if (wxToolBar* tb = dynamic_cast<wxToolBar*>(root))
        if (tb->FindById(id)) return tb;
    for (wxWindow* c : root->GetChildren())
        if (wxToolBar* r = findToolBarWith(c, id)) return r;
    return NULL;
}

// Click a toolbar button.  wxEVT_TOOL is delivered to the bar's FRAME, which is
// where CWorldToolBars binds its handler.
void pressTool(wxWindow* barFrame, int id)
{
    wxCommandEvent evt(wxEVT_TOOL, id);
    evt.SetEventObject(barFrame);
    barFrame->GetEventHandler()->ProcessEvent(evt);
}

UiAnswer probeUi(wxWindow* frame, int id)
{
    wxUpdateUIEvent evt(id);
    evt.SetEventObject(frame);

    UiAnswer a;
    a.claimed    = frame->GetEventHandler()->ProcessEvent(evt);
    a.setEnabled = evt.GetSetEnabled();
    a.enabled    = evt.GetEnabled();
    a.setChecked = evt.GetSetChecked();
    a.checked    = evt.GetChecked();
    a.setText    = evt.GetSetText();
    a.text       = std::string(evt.GetText().utf8_str());
    return a;
}

} // namespace

int RunMenuBarSelfTest()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("MENUBAR-SELFTEST: building the real MUSHclient menu bar\n");

    // ── preferences are REDIRECTED for the whole harness ────────────────────
    // This harness exits from wxApp::OnInit, so OpenAppConfig is still pointing
    // at ~/Library/Preferences/mushclient Preferences -- the real file belonging
    // to the copy of MUSHclient the person is actually using.  It builds real
    // CWorldChildFrames, and every one of those builds the three toolbars,
    // which READ the saved arrangement on construction and WRITE it back
    // whenever it changes.  Rearranging someone's toolbars from a test run is
    // exactly the accident platform/app_config.h exists to prevent.
    //
    // It has to be here rather than around the toolbar blocks below: the first
    // world frame is built hundreds of lines earlier, so a redirect down there
    // would be too late to stop the read AND would make the toolbar assertions
    // depend on whatever arrangement the user happens to have saved.  (Found
    // exactly that way -- the block passed until a real drag was saved.)
    //
    // Asserted rather than assumed, because nothing downstream would notice if
    // it had silently failed.
    const wxString savedCfgPath = AppConfigPathForTesting();
    SetAppConfigPathForTesting(
        wxString::Format(wxT("/tmp/mc_selftest_menubar_%d.cfg"), (int) getpid()));
    check(!AppConfigPathForTesting().empty(),
          "the harness is not writing the user's real preferences");

    // Seed a recent-worlds list into the scratch config, before anything reads
    // it: the MRU is loaded during the frame build below, and File > Open
    // Recent is checked against whatever that produced.  Those checks used to
    // run against the tester's own config -- which the block's own comment
    // calls out as a hazard, since a machine with no recent worlds takes the
    // empty-MRU branch and the populated one is never exercised.  Redirecting
    // the config made every machine that machine, so seed it instead and the
    // populated branch runs everywhere, the same way, every time.
    {
        const std::unique_ptr<wxConfigBase> cfg = OpenAppConfig();
        cfg->Write(wxT("/recent/0"), wxT("/tmp/mc_selftest_recent_one.mcl"));
        cfg->Write(wxT("/recent/1"), wxT("/tmp/mc_selftest_recent_two.mcl"));
        cfg->Write(wxT("/recent/2"), wxT("/tmp/mc_selftest_recent_three.mcl"));
        cfg->Flush();
    }

    // The bridge walks the world registry while building window furniture.
    if (App.m_pWorldDocTemplate == NULL)
        App.m_pWorldDocTemplate = new CMultiDocTemplate(
            IDR_MUSHCLTYPE,
            RUNTIME_CLASS(CMUSHclientDoc),
            RUNTIME_CLASS(CChildFrame),
            RUNTIME_CLASS(CMUSHView));

    check(g_wxhook_load_mdi_frame != NULL, "the MDI-frame bridge hook is installed");
    if (!g_wxhook_load_mdi_frame) {
        printf("MENUBAR-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail;
    }

    // The hook stores the wxMDIParentFrame it created in the CWnd's m_hWnd,
    // which is how the rest of the bridge finds it.
    CWnd host;
    const int built = g_wxhook_load_mdi_frame(&host, IDR_MAINFRAME, 0, NULL);
    check(built != 0, "the main frame was created");

    wxMDIParentFrame* frame = static_cast<wxMDIParentFrame*>(host.m_hWnd);
    check(frame != NULL, "...and handed back through m_hWnd");
    if (!frame) {
        printf("MENUBAR-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail;
    }

    wxMenuBar* mb = frame->GetMenuBar();
    check(mb != NULL && mb->GetMenuCount() > 0, "...carrying a populated menu bar");
    if (!mb) {
        frame->Destroy();
        printf("MENUBAR-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail;
    }

    std::vector<Item> items;
    for (size_t m = 0; m < mb->GetMenuCount(); ++m)
        flatten(mb->GetMenu(m),
                std::string(mb->GetMenuLabelText(m).utf8_str()) + " > ", items);
    check(items.size() > 50, "the menu bar has its full complement of commands");

    // ── Help items that launch a browser must say so ─────────────────────────
    // BUG-6 (2026-08-12 GUI drive): Help > Function List... opened Safari with
    // no visual warning, unlike its "(web)" siblings.  The table label was
    // already right for that one item; what this pins is the CONVENTION, so a
    // future item cannot go stale the same way silently.  Each id below is one
    // whose handler unconditionally calls ShellExecute on this platform (read
    // from the source, not guessed from the label):
    //   ID_HELP_FORUM/_FUNCTIONSWEBPAGE/_DOCUMENTATIONWEBPAGE/
    //   _REGULAREXPRESSIONSWEBPAGE/_MUDLISTS/_PLUGINSLIST and
    //   ID_HELP_BUGREPORTSUGGESTION all do so in mainfrm.cpp, on every
    //   platform.  ID_HELP_GETTINGSTARTED does so ONLY on this port --
    //   MUSHclient.cpp's OnHelpGettingstarted has a real #ifdef _WIN32 arm
    //   (local .chm help) and a non-Windows #else that opens the online docs
    //   instead, because no .chm ships here -- so the port's OWN menu table
    //   must reflect the port's OWN behaviour, not Windows'.
    //
    // ID_HELP_TIPOFTHEDAY is the control: CMainFrame::OnHelpTipoftheday opens
    // CTipDlg, a real local dialog, so its label must NOT carry the tag --
    // proving this loop can fail, not just pass by construction.
    {
        const struct { int id; const char* name; } kWebBound[] = {
            { ID_HELP_FORUM,                     "Forum"                     },
            { ID_HELP_FUNCTIONSWEBPAGE,          "Function List"             },
            { ID_HELP_DOCUMENTATIONWEBPAGE,      "Documentation"             },
            { ID_HELP_REGULAREXPRESSIONSWEBPAGE, "Regular Expressions"       },
            { ID_HELP_MUDLISTS,                  "MUD Lists"                 },
            { ID_HELP_PLUGINSLIST,               "Plugins List"              },
            { ID_HELP_BUGREPORTSUGGESTION,       "Bug Reports / Suggestions" },
            { ID_HELP_GETTINGSTARTED,            "Getting Started"           },
        };
        for (const auto& w : kWebBound) {
            const Item* it = find_by_id(items, w.id);
            const std::string what =
                std::string("Help > ") + w.name + " is labelled (web)";
            checkf(it != NULL, what + " -- item exists");
            if (it)
                checkf(it->label.find("(web)") != std::string::npos, what);
        }

        const Item* tip = find_by_id(items, ID_HELP_TIPOFTHEDAY);
        check(tip != NULL, "Help > Tip Of The Day is in the menu bar");
        check(tip && tip->label.find("(web)") == std::string::npos,
              "...and, being genuinely local, carries no (web) tag");
    }

    // ── every accelerator that is present must PARSE ────────────────────────
    // A rejected string leaves the item with no shortcut at all.  Covers every
    // menu in the app, not just the new ones.
    //
    // Weaker than it looks, and worth knowing why: wx does not REJECT a string
    // containing a modifier it does not recognise, it silently DROPS that
    // modifier.  Mutating "RawCtrl+Ctrl+L" to "RawCtl+Ctrl+L" still parses --
    // as plain Ctrl+L.  What catches that is the per-item modifier assertion
    // further down, and the duplicate lint, which saw the mangled item collide
    // with the real Ctrl+L.  Keep all three.
    {
        int withAccel = 0, unparsed = 0;
        std::string firstBad;
        for (const Item& it : items) {
            if (it.accel.empty()) continue;
            withAccel++;
            if (it.accelKey == 0) {
                unparsed++;
                if (firstBad.empty())
                    firstBad = it.path + " (\"" + it.accel + "\")";
            }
        }
        printf("    (%d items, %d with an accelerator)\n",
               (int) items.size(), withAccel);
        check(withAccel > 20, "plenty of items carry an accelerator");
        checkf(unparsed == 0,
               unparsed == 0 ? std::string("every accelerator in the menu bar parses")
                             : "an accelerator wx cannot parse: " + firstBad);
    }

    // ── no two commands may claim the same accelerator ──────────────────────
    // wx and AppKit both accept a duplicate silently and then deliver the key
    // to exactly one of them, so the other menu item's shortcut is dead.
    {
        std::map<std::pair<int,int>, const Item*> seen;
        int clashes = 0;
        std::string firstClash;
        for (const Item& it : items) {
            if (it.accelKey == 0) continue;
            const auto key = std::make_pair(it.accelFlags, it.accelKey);
            auto found = seen.find(key);
            if (found == seen.end()) { seen[key] = &it; continue; }
            if (found->second->id == it.id) continue;   // same command, two menus
            clashes++;
            printf("    (clash) %s: %s and %s\n",
                   it.accel.c_str(), found->second->path.c_str(), it.path.c_str());
            if (firstClash.empty())
                firstClash = it.accel + ": " + found->second->path + " and " + it.path;
        }
        checkf(clashes == 0,
               clashes == 0 ? std::string("no two commands share an accelerator")
                            : "duplicate accelerator -- " + firstClash);
    }

    // ── the four clears are in the Display menu, with the right keys ─────────
    {
        struct Expect { const char* label; const char* accel; int flags; int key; };
        const Expect kExpect[] = {
            { "Clear to Start",          CWorldChildFrame::kAccelClearToStart,
              wxACCEL_CTRL,                   'K' },
            { "Clear to Previous Mark",  CWorldChildFrame::kAccelClearToMark,
              wxACCEL_CTRL,                   'L' },
            { "Clear Scrollback",        CWorldChildFrame::kAccelClearScrollback,
              wxACCEL_ALT | wxACCEL_CTRL,     'K' },
            { "Clear Screen",            CWorldChildFrame::kAccelClearScreen,
              wxACCEL_RAW_CTRL | wxACCEL_CTRL, 'L' },
        };

        for (const Expect& e : kExpect) {
            const Item* found = NULL;
            for (const Item& it : items)
                if (it.label == e.label) { found = &it; break; }

            const std::string what = std::string("Display > ") + e.label;
            checkf(found != NULL, what + " is in the menu bar");
            if (!found) continue;
            checkf(found->path == std::string("Display > ") + e.label,
                   what + " is under Display, not somewhere else");
            checkf(found->accel == e.accel,
                   what + " shows " + e.accel);
            checkf(found->accelFlags == e.flags && found->accelKey == e.key,
                   what + " binds the modifiers it advertises");

            // ...and the id is one the frame's command dispatcher answers.  An
            // item wired to nothing is the third silent failure: it draws,
            // it highlights, and clicking it does nothing.  With no world open
            // the handler returns early, but it must still CLAIM the event.
            wxCommandEvent cmd(wxEVT_MENU, found->id);
            cmd.SetEventObject(frame);
            checkf(frame->GetEventHandler()->ProcessEvent(cmd),
                   what + " is bound to a handler");
        }
    }

    // ── the six scroll commands Windows' Display menu opens with ────────────
    // mushclient_dialogs.rc:3013-3018.  The port carried NONE of them: four
    // were reachable only as hard-coded keys in the command bar, and Line Up /
    // Line Down existed nowhere at all.  The ids are the canonical ID_TEST_*
    // ones, so world.DoCommand and any accelerator that names them reach the
    // same handler.
    {
        struct Expect { const char* label; int id;
                        const char* accel; int flags; int key; };
        const Expect kScroll[] = {
            { "Start",     ID_TEST_START,
              CWorldChildFrame::kAccelScrollStart,
              wxACCEL_RAW_CTRL, WXK_HOME },
            { "Page Up",   ID_TEST_PAGEUP,    "", 0, 0 },
            { "Page Down", ID_TEST_PAGEDOWN,  "", 0, 0 },
            { "End",       ID_TEST_END,
              CWorldChildFrame::kAccelScrollEnd,
              wxACCEL_RAW_CTRL, WXK_END },
            { "Line Up",   ID_TEST_LINEUP,
              CWorldChildFrame::kAccelScrollLineUp,
              wxACCEL_RAW_CTRL, WXK_UP },
            { "Line Down", ID_TEST_LINEDOWN,
              CWorldChildFrame::kAccelScrollLineDown,
              wxACCEL_RAW_CTRL, WXK_DOWN },
        };

        for (const Expect& e : kScroll) {
            const Item* found = NULL;
            for (const Item& it : items)
                if (it.path == std::string("Display > ") + e.label)
                    { found = &it; break; }

            const std::string what = std::string("Display > ") + e.label;
            checkf(found != NULL, what + " is in the menu bar");
            if (!found) continue;
            checkf(found->id == e.id,
                   what + " carries the canonical ID_TEST_* id");
            checkf(found->accel == e.accel,
                   what + std::string(" shows ") +
                   (e.accel[0] ? e.accel : "no shortcut"));
            checkf(found->accelFlags == e.flags && found->accelKey == e.key,
                   what + " binds the modifiers it advertises");

            // ...and the id reaches a handler.  With no world open the
            // dispatcher returns early, but it must still CLAIM the event --
            // an item wired to nothing draws and highlights and does nothing.
            wxCommandEvent cmd(wxEVT_MENU, found->id);
            cmd.SetEventObject(frame);
            checkf(frame->GetEventHandler()->ProcessEvent(cmd),
                   what + " is bound to a handler");
        }

        // Page Up / Page Down deliberately have no key equivalent: theirs carry
        // no modifier on Windows, and an unmodified menu accelerator on macOS
        // is claimed app-wide -- it would scroll the world's output when the
        // user pressed Page Up in a notepad or a dialog.  Asserted, because
        // "no accelerator" is exactly what an accidental one would look like
        // in the loop above only if someone also updated the table.
        int bareParagraphKeys = 0;
        for (const Item& it : items)
            if (it.accelFlags == 0 &&
                (it.accelKey == WXK_PAGEUP || it.accelKey == WXK_PAGEDOWN))
                bareParagraphKeys++;
        check(bareParagraphKeys == 0,
              "no menu item claims a BARE Page Up / Page Down (it would fire "
              "app-wide, including in notepads and dialogs)");
    }

    // ── Edit ▸ Select All must not be back on the standard id (BUG-3) ───────
    // wxID_SELECTALL is claimed natively by AppKit for whatever text control
    // has REAL wx focus, which is always the command bar (nothing ever calls
    // SetFocus() on the output pane) -- so a menu click or Cmd+A on the pane
    // never even reached this frame's own EVT_MENU handler, let alone
    // CWorldChildFrame::EditSelectAll's pane-vs-command-bar decision.
    // ID_EDIT_SELECT_ALL is the fix; this pins the id so a revert to the
    // standard one fails loudly here instead of silently going dead again.
    {
        const Item* found = NULL;
        for (const Item& it : items)
            if (it.path == "Edit > Select All") { found = &it; break; }

        checkf(found != NULL, "Edit > Select All is in the menu bar");
        if (found) {
            checkf(found->id != wxID_SELECTALL,
                   "Edit > Select All is not on the standard id (AppKit would "
                   "claim its click/enable-state and never reach our dispatcher)");

            wxCommandEvent cmd(wxEVT_MENU, found->id);
            cmd.SetEventObject(frame);
            checkf(frame->GetEventHandler()->ProcessEvent(cmd),
                   "Edit > Select All is bound to a handler");
        }
    }

    // ── Edit ▸ Undo has to EXIST (2026-08-18) ───────────────────────────────
    // The canonical Edit menu opens with Undo, and the port's menu simply had
    // no such item: Ctrl/Cmd+Z was registered nowhere, so neither the command
    // bar nor any bridged dialog field could be undone from the keyboard, and
    // world.DoCommand("Undo") was the only way in.  The dispatcher arm for it
    // was already there -- only the item was missing.
    {
        const Item* found = NULL;
        for (const Item& it : items)
            if (it.path == "Edit > Undo") { found = &it; break; }

        checkf(found != NULL, "Edit > Undo is in the menu bar");
        if (found) {
            checkf(found->accelKey == 'Z' && found->accelFlags == wxACCEL_CTRL,
                   "...bound to Ctrl/Cmd+Z");
            // Unlike Copy and Select All this one KEEPS the standard id on
            // purpose: its target is always the focused field, which is
            // exactly what AppKit's undo: routing does.
            checkf(found->id == wxID_UNDO,
                   "...on the standard id, so AppKit routes it to the focused field");
        }
    }

    // ── File ▸ Save Selection exists at all (2026-08-18) ────────────────────
    // ID_FILE_SAVESELECTION is a canonical command whose only handler is
    // CMUSHView::OnFileSaveselection, and CMUSHView is never constructed on
    // the port -- so the command had no item and no implementation here.
    {
        const Item* found = find_by_id(items, 32255 /*ID_FILE_SAVESELECTION*/);
        check(found != NULL, "File > Save Selection is in the menu bar");
        if (found) {
            // Prefix compare: the label carries an ellipsis, and a narrow
            // literal may not (ascii-literals lint).
            checkf(found->path.find("File > Save Selection") == 0,
                   "...under File, beside Print Selection");
            // It follows Copy's rule: no world, no selection, no command.  The
            // handler must SAY so rather than leave the item alone, which is
            // what an unhandled id looks like.
            const UiAnswer a = probeUi(frame, found->id);
            check(a.setEnabled && !a.enabled,
                  "...and is explicitly greyed with no world open");
        }
    }

    // ── chords that already mean something in MUSHclient still mean it ──────
    // Stronger than the duplicate lint, which only catches two commands
    // claiming one key WITHIN this menu bar.  These chords are bound in the
    // canonical resource, so binding them to a different command here is just
    // as wrong even though nothing on the port collides: a MUSHclient user's
    // fingers land on the wrong dialog and no test would have said a word.
    // Each entry cites the resource line it comes from.
    {
        struct Canon { const char* accel; const char* label; const char* rc; };
        const Canon kCanon[] = {
            { "Shift+Ctrl+V", "Paste To World",  "rc:2955" },
            { "Shift+Ctrl+7", "Variables",       "rc:3077" },
            { "Shift+Ctrl+9", "Aliases",         "rc:3081" },
            { "Shift+Ctrl+0", "Timers",          "rc:3079" },
            { "Shift+Ctrl+I", "Info",            "rc:3083" },
            { "Shift+Ctrl+A", "Auto Say",        "rc:3014" },
        };
        for (const Canon& c : kCanon) {
            wxAcceleratorEntry want;
            if (!want.FromString(wxString(wxT("\t")) + wxString::FromUTF8(c.accel))) {
                checkf(false, std::string("could not parse ") + c.accel);
                continue;
            }
            const Item* holder = NULL;
            for (const Item& it : items)
                if (it.accelKey == want.GetKeyCode() &&
                    it.accelFlags == want.GetFlags()) { holder = &it; break; }

            const std::string what =
                std::string(c.accel) + " is " + c.label + " (" + c.rc + ")";
            checkf(holder != NULL, what + " -- bound at all");
            if (!holder) continue;
            // Label match is a prefix test: the port drops the "..." and the
            // resource's "&" mnemonics land in different places.
            checkf(holder->label.find(c.label) != std::string::npos, what);
        }
    }

    // Also pin that Clear Output Buffer -- the canonical Windows command these
    // sit beside -- is still there and still distinct from all four.  It is the
    // one that erases everything, and confusing it with the others is the
    // mistake this whole family exists to make harder.
    {
        const Item* canon = find_by_id(items, 32291 /*ID_DISPLAY_CLEAR_OUTPUT*/);
        check(canon != NULL, "Clear Output Buffer is still in the Display menu");
        if (canon)
            check(canon->accelKey == 0,
                  "...and takes no accelerator here (Windows keeps Shift+Ctrl+C)");
    }

    // ── the Save-As default filename ────────────────────────────────────────
    // The scrub itself is covered headlessly (Utilities.DefaultWorldFileName*).
    // What only exists on this side is the wx conversion, and the fallback that
    // has to come AFTER it: FromUTF8 is all-or-nothing, so a world name that is
    // not valid UTF-8 -- an old world file carrying Latin-1 bytes -- converts to
    // an EMPTY string, and an empty filename in the Save dialog is worse than a
    // generic one.  Before the scrub was hoisted the conversion came first and
    // this fell out of the existing IsEmpty test.
    {
        check(DefaultWorldFileNameForName("Dune") == wxT("Dune.mcl"),
              "the Save-As default is the world name plus .mcl");
        check(DefaultWorldFileNameForName("a/b") == wxT("ab.mcl"),
              "...with the path separator scrubbed out");
        check(DefaultWorldFileNameForName("") == wxT("world.mcl"),
              "...falling back for an unnamed world");
        // The regression this exists to catch.  "Caf\xE9" is Latin-1, not UTF-8.
        check(DefaultWorldFileNameForName("Caf\xE9") == wxT("world.mcl"),
              "...and for a name that is not valid UTF-8, rather than empty");
        check(!DefaultWorldFileNameForName("Caf\xE9").IsEmpty(),
              "...which is the part that matters: never an empty filename");
    }

    // ── File ▸ Open Recent ──────────────────────────────────────────────────
    // The MRU load and the menu rebuild both ran during the frame build above;
    // nothing had ever looked at the result.  Two halves, because they fail
    // differently: the LIVE menu is checked against the LIVE list (so the two
    // cannot drift), and the builder is then driven with lists of our own (so
    // the check is not at the mercy of whatever is in the tester's config —
    // on a machine with no recent worlds every list assertion would otherwise
    // be vacuously true).
    {
        const int idBase = RecentWorldsMenuIdBase();
        const std::vector<std::string>& live = RecentWorldsList();

        wxMenu* recent = NULL;
        for (size_t m = 0; m < mb->GetMenuCount() && !recent; ++m)
            for (const wxMenuItem* mi : mb->GetMenu(m)->GetMenuItems())
                if (mi->IsSubMenu() && mi->GetItemLabelText() == wxT("Open Recent"))
                    { recent = mi->GetSubMenu(); break; }

        check(recent != NULL, "File > Open Recent is in the menu bar");
        printf("    (%d recent worlds in this config)\n", (int) live.size());

        // Whatever recent_worlds_load produced from the real config has to obey
        // the MRU's own invariants, or the menu it feeds is already wrong.
        check((int) live.size() <= RecentWorldsMaxEntries(),
              "the loaded MRU respects the cap");
        {
            bool clean = true;
            for (size_t i = 0; i < live.size(); ++i) {
                if (live[i].empty()) clean = false;      // an empty entry ends the list
                for (size_t j = i + 1; j < live.size(); ++j)
                    if (live[i] == live[j]) clean = false;
            }
            check(clean, "the loaded MRU has no empty and no duplicate entries");
        }

        // A full MRU is the only case where the mnemonic digit wraps: entries
        // 1..9 get &1..&9 and the tenth gets &0, because "&10" is not a
        // mnemonic.  Driven with our own list so it does not depend on the
        // tester's config having ten worlds in it.
        {
            std::vector<std::string> full;
            for (int i = 1; i <= RecentWorldsMaxEntries(); ++i)
                full.push_back("/tmp/w" + std::to_string(i) + ".mcl");
            wxMenu probe;
            RecentWorldsBuildMenu(&probe, full, idBase);
            check((int) probe.GetMenuItemCount() == RecentWorldsMaxEntries(),
                  "a full MRU gives one menu item per entry");
            const wxMenuItem* first = probe.FindItemByPosition(0);
            const wxMenuItem* last  =
                probe.FindItemByPosition(RecentWorldsMaxEntries() - 1);
            check(first && first->GetItemLabel().StartsWith(wxT("&1")),
                  "...the first is mnemonic &1");
            check(last && last->GetItemLabel().StartsWith(wxT("&0")),
                  "...and the tenth wraps to &0, not &10");
        }

        if (recent) {
            check(recent->GetMenuItemCount() ==
                      (live.empty() ? 1u : (unsigned) live.size()),
                  "the menu has exactly one item per recent world");
            if (live.empty()) {
                const wxMenuItem* mi = recent->FindItemByPosition(0);
                check(mi && !mi->IsEnabled(),
                      "an empty MRU shows a DISABLED placeholder");
                check(mi && mi->GetId() == idBase,
                      "...on the first id of the reserved block");
            } else {
                bool idsOk = true, labelsOk = true, enabledOk = true;
                for (size_t i = 0; i < live.size(); ++i) {
                    const wxMenuItem* mi = recent->FindItemByPosition(i);
                    if (!mi) { idsOk = labelsOk = false; break; }
                    if (mi->GetId() != idBase + (int) i) idsOk = false;
                    if (!mi->IsEnabled()) enabledOk = false;
                    // The label carries only the basename; the command handler
                    // indexes the LIST by (id - base), so this correspondence is
                    // what makes the right world open.
                    const wxString want =
                        wxFileName(wxString::FromUTF8(live[i].c_str())).GetFullName();
                    if (mi->GetItemLabelText().Find(want) == wxNOT_FOUND)
                        labelsOk = false;
                }
                check(idsOk,     "each entry sits at its own id, in list order");
                check(labelsOk,  "each entry is labelled with its world's filename");
                check(enabledOk, "every real entry is enabled");
            }
        }

        // ── the builder, driven directly ────────────────────────────────────
        // Rebuild a scratch menu from lists we choose, so both branches are
        // exercised on every machine.  (It is the same function the frame just
        // ran; the live menu is left alone.)
        {
            wxMenu scratch;
            const std::vector<std::string> three = {
                "/worlds/Discworld.mcl", "/elsewhere/Aardwolf.mcl", "/w/Discworld.mcl" };
            RecentWorldsBuildMenu(&scratch, three, idBase);
            check(scratch.GetMenuItemCount() == 3, "builder: one item per world");
            bool ok = true;
            for (int i = 0; i < 3; ++i) {
                const wxMenuItem* mi = scratch.FindItemByPosition(i);
                if (!mi || mi->GetId() != idBase + i || !mi->IsEnabled()) ok = false;
            }
            check(ok, "builder: consecutive ids from the base, all enabled");
            // Two worlds sharing a basename must still be two items — the menu
            // shows only the filename, but they are different files.
            check(scratch.FindItemByPosition(0)->GetItemLabelText()
                      .Find(wxT("Discworld.mcl")) != wxNOT_FOUND &&
                  scratch.FindItemByPosition(2)->GetItemLabelText()
                      .Find(wxT("Discworld.mcl")) != wxNOT_FOUND,
                  "builder: same-basename worlds each get their own entry");

            // Rebuilding must REPLACE, not append: recent_worlds_add calls this
            // on every world opened, so a menu that only grew would soon list
            // the same world ten times.
            const std::vector<std::string> one = { "/worlds/Only.mcl" };
            RecentWorldsBuildMenu(&scratch, one, idBase);
            check(scratch.GetMenuItemCount() == 1,
                  "builder: a rebuild replaces the old items, it does not append");

            RecentWorldsBuildMenu(&scratch, std::vector<std::string>(), idBase);
            check(scratch.GetMenuItemCount() == 1,
                  "builder: an empty list still leaves something to look at");
            const wxMenuItem* ph = scratch.FindItemByPosition(0);
            check(ph && !ph->IsEnabled(),
                  "builder: ...and that placeholder is disabled, so it cannot be picked");
        }
    }

    // ── update-UI: what the menu bar greys, ticks and renames ───────────────
    // The frame's wxEVT_UPDATE_UI handler is the fourth silent failure mode: an
    // item that should be greyed but is not looks completely normal, and only
    // does nothing when picked.  Nothing had ever fired one of these events, so
    // every arm of that handler was dead code as far as the suite went.  The
    // rules themselves are pinned headlessly (tests/test_menu_rules.cpp); what
    // is only checkable HERE is that the frame is actually wired to them, and
    // the arms that must stay wx-side.
    {
        const Item* lua = find_by_label(items, "Execute Lua");
        const Item* log = find_by_label(items, "Logging");
        check(lua != NULL, "Game > Execute Lua is in the menu bar");
        check(log != NULL, "Connection > Start Logging is in the menu bar");
        check(find_by_id(items, 32239) != NULL,
              "Connection > Connect is on ID_CONNECTION_CONNECT");
        check(find_by_id(items, 0xE200) != NULL,
              "Connection > Disconnect is on the port's own id");
        check(find_by_id(items, 0xE201) != NULL,
              "Connection > Reconnect is on the port's own id");

        // ── with no world open ──────────────────────────────────────────────
        {
            const UiAnswer connect    = probeUi(frame, 32239);
            const UiAnswer disconnect = probeUi(frame, 0xE200);
            const UiAnswer reconnect  = probeUi(frame, 0xE201);
            check(connect.claimed && disconnect.claimed && reconnect.claimed,
                  "the frame answers for all three Connection commands");
            check(connect.setEnabled && !connect.enabled &&
                  disconnect.setEnabled && !disconnect.enabled &&
                  reconnect.setEnabled && !reconnect.enabled,
                  "...greying every one of them with no world open");

            // The canonical-command table's three outcomes, which are easy to
            // conflate.  A 'D'-target command with no active world never
            // reaches a handler at all: it is forced OFF because there is no
            // document to route to.  A 'F'-target command always reaches one
            // (CMainFrame is always there), so its answer is whatever that
            // handler says.  Only a target with NO update-UI handler falls back
            // to the row's needsDoc flag.
            check(!probeUi(frame, 32980 /*ID_FILE_IMPORT, 'D'*/).enabled,
                  "a world-targeted table command is dead with no world");

            check(!probeUi(frame, ID_DISPLAY_CLEAR_COMMAND_HISTORY).enabled,
                  "Display > Clear Command History is dead with no world too");

            // 'F', driven BOTH ways: CMainFrame::OnUpdateFileOpenworldsinstartuplist
            // enables Open Worlds In Startup List only when that list is not
            // empty.  An arm that answered "true" out of the initialiser rather
            // than out of the canonical handler would pass one half and fail
            // the other.
            {
                const CString saved = App.m_strWorldList;
                App.m_strWorldList = "";
                const bool deadWhenEmpty = !probeUi(frame, 32954).enabled;
                App.m_strWorldList = "example.mcl";
                const bool liveWhenSet = probeUi(frame, 32954).enabled;
                App.m_strWorldList = saved;
                check(deadWhenEmpty && liveWhenSet,
                      "...while a frame-targeted one follows CMainFrame's own rule");
            }

            // 'A' is the one arm that reaches the needsDoc fallback: the app's
            // message map carries no ON_UPDATE_COMMAND_UI whatsoever, and this
            // row does not need a world, so it stays live.
            check(probeUi(frame, 32234 /*ID_GAME_MINIMISEPROGRAM, 'A'*/).enabled,
                  "...and an app-targeted one falls back to its needs-a-world flag");

            check(!probeUi(frame, lua ? lua->id : 0).enabled,
                  "Execute Lua is dead with no world");
            const UiAnswer logAns = probeUi(frame, log ? log->id : 0);
            check(!logAns.enabled, "Start Logging is dead with no world");
            check(!logAns.setText,
                  "...and its caption is left alone rather than reset");


            // ── the three View toggles answer their CANONICAL ids ───────────
            // world.DoCommand("AlwaysOnTop") / ("FullScreenMode") /
            // ("ViewStatusbar") post the canonical id through
            // Frame.PostMessage -> hook_post_message -> wxEVT_MENU.  Two of the
            // three were moved onto their canonical ids for exactly that reason
            // (038dc88); Always On Top was left on wxID_HIGHEST + 430, so the
            // command matched no arm, was skipped, and DoCommand still returned
            // eOK.  Firing 32322 is the half that fails on the private id.
            {
                // The harness exits from wxApp::OnInit, so InitInstance never
                // ran and the sqlite preferences database is not open: the
                // db_write_int inside the arm is a no-op.  Assert that rather
                // than trust it -- this is the one arm here that persists.
                check(App.db == NULL,
                      "the preferences database is closed, so the toggle cannot "
                      "write the tester's real prefs");

                const unsigned int savedOnTop = App.m_bAlwaysOnTop;
                App.m_bAlwaysOnTop = 0;

                const bool claimed1 = fireCmd(frame, 32322 /*ID_VIEW_ALWAYSONTOP*/);
                const bool onAfter1 = App.m_bAlwaysOnTop != 0;
                const UiAnswer uiOn = probeUi(frame, 32322);

                const bool claimed2 = fireCmd(frame, 32322);
                const bool onAfter2 = App.m_bAlwaysOnTop != 0;
                const UiAnswer uiOff = probeUi(frame, 32322);

                check(claimed1 && claimed2,
                      "DoCommand(\"AlwaysOnTop\") is claimed by a handler");
                // Both directions, so an arm that merely SET the flag fails.
                check(onAfter1 && !onAfter2,
                      "...and toggles the preference, then toggles it back");
                check(uiOn.setChecked && uiOn.checked,
                      "...with the menu tick following it on");
                check(uiOff.setChecked && !uiOff.checked,
                      "...and off again");

                App.m_bAlwaysOnTop = savedOnTop;
            }


            // ── Game ▸ Activity List ticks while the window is open ─────────
            // CMainFrame::OnUpdateDisplayActivitylist (mainfrm.cpp:1164) is
            // Enable(TRUE) + SetCheck(App.m_pActivityView != NULL).  The port
            // built the item with Append (so wx discards a Check() outright)
            // and had no update-UI arm for the id at all, so the tick — which
            // on Windows is the ONLY sign the window is already open, since
            // choosing the item again merely raises it — was never shown.
            {
                wxMenuItem* activity = mb->FindItem(32203);
                check(activity != NULL,
                      "Game > Activity List is in the menu bar");
                check(activity && activity->IsCheckable(),
                      "...as a check item, or wx drops the answer");

                check(!ActivityWindowIsOpen(),
                      "the activity window is closed to begin with");
                const UiAnswer shut = probeUi(frame, 32203);
                check(shut.claimed && shut.setEnabled && shut.enabled,
                      "...and the item is live regardless, as on Windows");
                check(shut.setChecked && !shut.checked,
                      "...and unticked");

                // Open it through the command id, which is the route a user
                // takes; that also proves the command arm and the update arm
                // agree about which id they are talking about.
                fireCmd(frame, 32203);
                check(ActivityWindowIsOpen(),
                      "choosing Activity List opens the activity window");
                const UiAnswer open = probeUi(frame, 32203);
                check(open.setChecked && open.checked,
                      "...and the menu item is now ticked");

                // Both directions: an arm that ticked unconditionally passes
                // the assertion above and fails this one.
                if (wxWindow* w = wxWindow::FindWindowByLabel(wxT("Activity")))
                    w->Close(true);
                check(!ActivityWindowIsOpen(),
                      "closing it clears the singleton again");
                const UiAnswer reshut = probeUi(frame, 32203);
                check(reshut.setChecked && !reshut.checked,
                      "...and the tick goes away with it");
            }

            // A View item that deliberately stays live with nothing open --
            // proof the probe is reading a real answer and not just "false".
            const Item* statusBar = find_by_label(items, "Status Bar");
            check(statusBar != NULL, "View > Status Bar is in the menu bar");
            check(statusBar && probeUi(frame, statusBar->id).enabled,
                  "...and is live with no world open, as it is on Windows");

            // ID_GAME_FUNCTIONSLIST is the command the Windows round moved out
            // of the world-only list (docs/WINDOWS-VALIDATION.md 10am): besides
            // CSendView and CTextView, CMainFrame::OnGameFunctionslist
            // (scripting/functionlist.cpp:602) opens the same dialog with no
            // context at all, so it must stay live with nothing open.  The rules
            // TABLE is pinned by tests/test_menu_rules.cpp; this is the other
            // half of that fix -- the real wx menu bar, on the real frame, which
            // is where the item was actually greyed.
            // ID_GAME_FUNCTIONSLIST is the command the Windows round moved out
            // of the world-only list (docs/WINDOWS-VALIDATION.md 10am): besides
            // CSendView and CTextView, CMainFrame::OnGameFunctionslist
            // (scripting/functionlist.cpp:602) opens the same dialog with no
            // context at all, so Windows leaves it live with nothing open.
            //
            // Read as "nobody greyed it", NOT as `.enabled`.  wxUpdateUIEvent
            // constructs with m_enabled FALSE and m_setEnabled false, and wx
            // only applies the answer when setEnabled is true -- so an id no arm
            // claims reports enabled==false here while the real menu item stays
            // exactly as built, i.e. live.  Asserting `.enabled` fails for a
            // correctly-live item, which is a trap this check walked into once.
            // The discriminator is real: while 33046 sat in kNeedsWorld,
            // MenuCommandUiState claimed it and called Enable(false), so
            // setEnabled was 1 and enabled 0 -- exactly what this rejects.  The
            // ID_FILE_IMPORT check above is the live negative control.
            const UiAnswer funcs = probeUi(frame, 33046 /*ID_GAME_FUNCTIONSLIST*/);
            check(!(funcs.setEnabled && !funcs.enabled),
                  "no arm greys Game > Functions List with no world open");

            // Stop Sound Playing survives with no world because CMainFrame
            // handles it too ('M' falls back to the frame); its two halves are
            // document commands and correctly grey out.
            check(probeUi(frame, 32292).enabled,
                  "Stop Sound Playing is live with no world open");
            check(!probeUi(frame, 33054).enabled && !probeUi(frame, 33055).enabled,
                  "...while its two halves, being document commands, are not");
        }

        // ── with a world open ───────────────────────────────────────────────
        // Everything above is one half of each rule; without a document the
        // suite could not tell a correct rule from `Enable(false)`.
        {
            CMUSHclientDoc* doc = new CMUSHclientDoc();
            doc->m_mush_name   = "MenuBarSelfTest";
            doc->m_nWrapColumn = 80;
            doc->m_maxlines    = 5000;
            doc->m_server      = "mud.example.com";
            doc->SetUpOutputWindow();

            CWorldChildFrame* child = new CWorldChildFrame(frame, doc);
            child->Show();
            frame->Show();
            child->Activate();
            wxYield();

            check(frame->GetActiveChild() == child,
                  "the world window is the frame's active child");

            // The other half of the 'D' path above: with a document to route
            // to, the same command comes alive.
            check(probeUi(frame, 32980 /*ID_FILE_IMPORT, 'D'*/).enabled,
                  "a world-targeted table command comes alive with one open");

            // Connect: disconnected AND the world has an address to go to.
            doc->m_iConnectPhase = eConnectNotConnected;
            check(probeUi(frame, 32239).enabled,
                  "Connect is live on a disconnected world");
            doc->m_server = "0.0.0.0";
            check(!probeUi(frame, 32239).enabled,
                  "...but not on a world still at the placeholder address");
            check(probeUi(frame, 0xE201).enabled,
                  "...which does NOT gate Reconnect, whose rule is its own");
            doc->m_server = "mud.example.com";

            // Disconnect: live on EVERY non-zero phase, so a connect that hangs
            // can still be cancelled.  This is the rule that has shipped broken.
            check(!probeUi(frame, 0xE200).enabled,
                  "Disconnect is dead while the world is disconnected");
            bool liveThroughout = true, connectDead = true, reconnectDead = true;
            // World Properties is greyed for the whole of a connect in flight —
            // "no changing of address whilst connecting"
            // (CMUSHclientDoc::OnUpdateGamePreferences, doc.cpp:1988).  The menu
            // bar used to answer `doc != nullptr` for it from a hard-coded list,
            // while the output pane's context menu and the World toolbar both
            // reached that handler: three surfaces, two answers.
            bool prefsDeadMidConnect = true;
            for (int phase = eConnectMudNameLookup; phase <= eConnectDisconnecting;
                 ++phase) {
                doc->m_iConnectPhase = phase;
                if (!probeUi(frame, 0xE200).enabled) {
                    liveThroughout = false;
                    printf("    (Disconnect greyed in connect phase %d)\n", phase);
                }
                if (probeUi(frame, 32239).enabled) connectDead   = false;
                if (probeUi(frame, 0xE201).enabled) reconnectDead = false;
                // Phase 9 is eConnectConnectedToMud, which the rule allows —
                // it is inside this loop only because Disconnect's rule spans
                // the whole range.
                if (phase != eConnectConnectedToMud) {
                    const UiAnswer prefs =
                        probeUi(frame, 32158 /*ID_GAME_PREFERENCES*/);
                    if (!prefs.setEnabled || prefs.enabled) {
                        prefsDeadMidConnect = false;
                        printf("    (World Properties live in connect phase %d)\n",
                               phase);
                    }
                }
            }
            check(liveThroughout,
                  "Disconnect stays live through every phase of a hanging connect");
            check(connectDead && reconnectDead,
                  "...while Connect and Reconnect are dead in all of them");
            check(prefsDeadMidConnect,
                  "World Properties is greyed while a connect is in flight");

            doc->m_iConnectPhase = eConnectNotConnected;
            // ...and the other direction, at both ends of the rule, so an arm
            // that simply greyed the item could not pass either.
            check(probeUi(frame, 32158).enabled,
                  "...and live again on a disconnected world");
            doc->m_iConnectPhase = eConnectConnectedToMud;
            check(probeUi(frame, 32158).enabled,
                  "...and on a fully connected one");
            doc->m_iConnectPhase = eConnectNotConnected;

            // Scripting and the log caption, the two port-only rules.
            doc->m_bEnableScripts = 0;
            check(!probeUi(frame, lua ? lua->id : 0).enabled,
                  "Execute Lua stays dead in a world with scripting off");
            doc->m_bEnableScripts = 1;
            check(probeUi(frame, lua ? lua->id : 0).enabled,
                  "...and comes alive when scripting is on");
            check(probeUi(frame, 32290 /*ID_GAME_RELOAD_SCRIPT_FILE*/).enabled,
                  "Reload Script follows the same switch");

            const UiAnswer logAns = probeUi(frame, log ? log->id : 0);
            check(logAns.enabled, "Start Logging is live with a world open");
            check(logAns.setText && logAns.text.find("Start") != std::string::npos,
                  "...and captions itself Start while the world is not logging");

            // ── ONE session log, whichever menu opens it ───────────────────
            // The defect this pins: "Start Logging" in the output pane's
            // context menu used to drive CWorldChildFrame's own
            // fopen(path, "a") -- its own banner, its own line format, and no
            // idea the world had m_bLogOutput / m_log_input / m_bLogNotes, a
            // preamble or a postamble -- while "Start Logging" in the menu bar
            // ran the canonical CMUSHclientDoc::OnFileLogsession.  Two
            // identically-named commands, two different files, two formats.
            //
            // Both entry points are driven for real here, through the frame's
            // own dispatcher, and the resulting files are compared BYTE FOR
            // BYTE.  This is the only harness that can: the equality is a
            // property of the dispatch, and the dispatcher only exists on the
            // frame the MDI bridge hook builds.
            //
            // The two modals the canonical handler puts up are answered by
            // hooks (the established pattern -- world_frame_selftest.cpp:2240):
            // CLogDlg accepts whatever OnFileLogsession seeded it with, which
            // is exactly the world's own logging options, and the Save panel
            // answers with the path this run wants.
            {
                CWorldChildFrame* const savedChild = child;
                COutputPane* const pane = savedChild->OutputPane();
                check(pane != NULL, "log unification: the world has an output pane");

                intptr_t (*const savedRunDlg)(void*, unsigned) = g_run_mfc_dialog;
                char* (*const savedFileDlg)(bool, const char*, const char*,
                                            const char*, const char*, bool, bool)
                    = g_wxhook_file_dialog;
                const unsigned savedConfirm = App.m_bConfirmLogFileClose;
                wxWindow* const savedTop =
                    wxTheApp ? wxTheApp->GetTopWindow() : NULL;

                // Accept the seeded options, and DO NOT dump the scrollback:
                // the buffer differs between runs (each run logs into it), and
                // this block is about what live logging writes.
                g_run_mfc_dialog = [](void* self, unsigned idd) -> intptr_t {
                    if (idd != IDD_LINE_COUNT) return IDCANCEL;
                    ((CLogDlg*) self)->m_lines = 0;
                    return IDOK;
                };
                // Where the Save panel "puts" the log.  One slot, rewritten
                // before each run.
                static std::string s_logPath;
                g_wxhook_file_dialog = [](bool, const char*, const char*,
                                          const char*, const char*, bool,
                                          bool) -> char* {
                    return strdup(s_logPath.c_str());
                };
                App.m_bConfirmLogFileClose = 0;   // no modal on the close half
                if (wxTheApp) wxTheApp->SetTopWindow(frame);

                // Fixed, %-free markers: FormatTime leaves a string with no
                // strftime code alone, so every byte below is deterministic and
                // each logged CATEGORY is identifiable in the output.
                doc->m_strLogFilePreamble    = "PREAMBLE";
                doc->m_strLogFilePostamble   = "POSTAMBLE";
                doc->m_strLogLinePreambleOutput = "OUT:";
                doc->m_strLogLinePreambleInput  = "IN:";
                doc->m_strLogLinePreambleNotes  = "NOTE:";
                doc->m_strLogLinePostambleOutput = "";
                doc->m_strLogLinePostambleInput  = "";
                doc->m_strLogLinePostambleNotes  = "";
                doc->m_bWriteWorldNameToLog  = 0;   // it embeds the wall clock
                doc->m_bLogHTML              = 0;
                doc->m_bLogRaw               = FALSE;
                doc->m_strAutoLogFileName    = "";

                auto slurp = [](const std::string& p) -> std::string {
                    std::string out;
                    if (FILE* f = fopen(p.c_str(), "rb")) {
                        char buf[4096];
                        size_t n;
                        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
                            out.append(buf, n);
                        fclose(f);
                    }
                    return out;
                };

                // Open through the given entry point, log one of each kind of
                // line, close through the same entry point, return the bytes.
                enum EntryPoint { kMenuBar, kContextMenu };
                auto runSession = [&](EntryPoint via, const char* tag) -> std::string {
                    char path[256];
                    snprintf(path, sizeof path, "/tmp/mc_selftest_log_%s_%d.txt",
                             tag, (int) getpid());
                    remove(path);
                    s_logPath = path;

                    auto toggle = [&]() {
                        if (via == kMenuBar) {
                            // Connection ▸ Start/Stop Logging -- the item found
                            // by label above, so its own port id and the arm
                            // that forwards it to 32117 are both in the chain,
                            // not just the canonical id.
                            fireCmd(frame, log ? log->id : 0);
                        } else {
                            // The output pane's right-click item, chosen the
                            // way a click chooses it.
                            wxMenu menu;
                            pane->BuildContextMenu(menu, 0,
                                                   pane->LineHeight() / 2);
                            wxCommandEvent pick(wxEVT_MENU, ID_FILE_LOGSESSION);
                            menu.ProcessEvent(pick);
                        }
                    };

                    toggle();
                    if (doc->m_logfile != NULL) {
                        doc->Note("a note");
                        doc->LogCommand("a command\r\n");   // ENDLINE
                        doc->DisplayMsg("some output\n", 12, 0);
                        doc->UpdateAllViews(NULL);
                        wxYield();
                    }
                    toggle();

                    const std::string bytes = slurp(path);
                    remove(path);
                    return bytes;
                };

                // ── all three categories on ────────────────────────────────
                doc->m_bLogOutput = 1;
                doc->m_log_input  = 1;
                doc->m_bLogNotes  = 1;
                const std::string barAll = runSession(kMenuBar,     "bar_all");
                const std::string ctxAll = runSession(kContextMenu, "ctx_all");

                check(!barAll.empty(),
                      "log unification: the menu bar's entry point wrote a log");
                check(!ctxAll.empty(),
                      "log unification: so did the context menu's");

                // THE assertion.  Same world, same settings, same content --
                // so the same bytes, or the two commands are not the same
                // command.
                check(barAll == ctxAll,
                      "LOG PARITY: both entry points produce a byte-identical "
                      "log file for the same settings");

                check(barAll.find("PREAMBLE") != std::string::npos &&
                      barAll.find("POSTAMBLE") != std::string::npos,
                      "log unification: the preamble and postamble are both "
                      "written (the port's own log had neither)");
                check(barAll.find("OUT:")  != std::string::npos &&
                      barAll.find("IN:")   != std::string::npos &&
                      barAll.find("NOTE:") != std::string::npos,
                      "log unification: output, input and notes are all logged "
                      "when all three options are on");

                // ── notes only: the options really are consulted ───────────
                // Without this the equality above would also hold for two
                // implementations that BOTH ignored the world's options.
                doc->m_bLogOutput = 0;
                doc->m_log_input  = 0;
                doc->m_bLogNotes  = 1;
                const std::string barNotes = runSession(kMenuBar,     "bar_notes");
                const std::string ctxNotes = runSession(kContextMenu, "ctx_notes");

                check(barNotes == ctxNotes,
                      "LOG PARITY: ...and again with a different set of logging "
                      "options, so both read the SAME options");
                check(barNotes != barAll,
                      "log unification: turning two options off really changed "
                      "the file (so the equality above is not vacuous)");
                check(barNotes.find("NOTE:") != std::string::npos,
                      "log unification: notes still logged with log_notes on");
                check(barNotes.find("OUT:") == std::string::npos &&
                      barNotes.find("IN:")  == std::string::npos,
                      "...while output and input are honoured as OFF, from "
                      "both entry points");

                // ── the TICK, which is how Windows says "logging" ──────────
                // OnUpdateFileLogsession does SetCheck (m_logfile != NULL)
                // (doc.cpp:1008).  The port's kCanonCmds row declared 32117
                // uncheckable, and wx_canonical_menu_ui_state forces `checked`
                // back to -1 for an uncheckable row -- so the tick never
                // reached the File menu's item nor the output pane's copy of
                // it, and nothing said a log was open.  Probed with a log
                // really open and really closed, so "never ticks" and "always
                // ticks" both fail.
                {
                    doc->m_bLogOutput = 1;
                    doc->m_log_input  = 1;
                    doc->m_bLogNotes  = 1;
                    char path[256];
                    snprintf(path, sizeof path, "/tmp/mc_selftest_log_tick_%d.txt",
                             (int) getpid());
                    remove(path);

                    const UiAnswer shut = probeUi(frame, ID_FILE_LOGSESSION);
                    check(shut.setChecked && !shut.checked,
                          "log tick: Log Session is a check item, unticked with "
                          "no log open");

                    doc->OpenLog(path, FALSE);
                    check(doc->m_logfile != NULL,
                          "log tick: a log opened for the ticked half");
                    const UiAnswer open = probeUi(frame, ID_FILE_LOGSESSION);
                    check(open.setChecked && open.checked,
                          "log tick: ...and TICKED while one is (was: never, "
                          "the table said uncheckable)");
                    doc->CloseLog();
                    remove(path);
                }

                // Whichever entry point opened it, closing a world writes the
                // postamble and lets go of the handle -- the frame's teardown
                // hook is the document's CloseLog now, not a second one.
                doc->m_bLogOutput = 1;
                doc->m_log_input  = 1;
                doc->m_bLogNotes  = 1;
                {
                    char path[256];
                    snprintf(path, sizeof path, "/tmp/mc_selftest_log_close_%d.txt",
                             (int) getpid());
                    remove(path);
                    s_logPath = path;
                    wxMenu menu;
                    pane->BuildContextMenu(menu, 0, pane->LineHeight() / 2);
                    wxCommandEvent pick(wxEVT_MENU, ID_FILE_LOGSESSION);
                    menu.ProcessEvent(pick);
                    check(doc->m_logfile != NULL,
                          "log teardown: the context menu opened a log");
                    savedChild->PersistBeforeTeardown();
                    check(doc->m_logfile == NULL,
                          "log teardown: closing the world closed THAT log "
                          "(one implementation, so one thing to close)");
                    check(slurp(path).find("POSTAMBLE") != std::string::npos,
                          "log teardown: ...after writing the postamble");
                    remove(path);
                }

                if (wxTheApp) wxTheApp->SetTopWindow(savedTop);
                App.m_bConfirmLogFileClose = savedConfirm;
                g_wxhook_file_dialog = savedFileDlg;
                g_run_mfc_dialog     = savedRunDlg;
                doc->m_strLogFilePreamble  = "";
                doc->m_strLogFilePostamble = "";
                doc->m_strLogLinePreambleOutput = "";
                doc->m_strLogLinePreambleInput  = "";
                doc->m_strLogLinePreambleNotes  = "";
            }

            // ── File > Export ... pre-fills the world name PLUS the set suffix ──
            // twin-drift#1: Set_Up_Set_Strings (platform/mushapp_stubs.cpp) used
            // to leave suggested_name untouched, so every export's save panel
            // defaulted to the bare world name instead of e.g.
            // "MenuBarSelfTest triggers" -- the suffix evaluate.cpp's Windows
            // version appends (suggested_name += " triggers", etc.), which is
            // what the save dialog pre-fills.  Save_Set is mushgui_adapters-only
            // (never linked into a headless test executable), so an in-app
            // selftest is the only way to pin it -- same reasoning, same hook,
            // as the Log-Session block just above.
            {
                char* (*const savedFileDlg)(bool, const char*, const char*,
                                            const char*, const char*, bool, bool)
                    = g_wxhook_file_dialog;
                static std::string s_defaultFile;
                g_wxhook_file_dialog = [](bool, const char*, const char*,
                                          const char* defaultFile, const char*,
                                          bool, bool) -> char* {
                    s_defaultFile = defaultFile ? defaultFile : "";
                    return nullptr;   // cancel -- nothing should be written
                };

                struct { int set_type; const char* suffix; } kCases[] = {
                    { TRIGGER, "triggers" },
                    { ALIAS,   "aliases" },
                    { COLOUR,  "colours" },
                    { MACRO,   "macros" },
                    { TIMER,   "timers" },
                };
                bool allMatched = true;
                for (const auto& c : kCases) {
                    s_defaultFile.clear();
                    doc->Save_Set(c.set_type, nullptr);
                    char want[128];
                    snprintf(want, sizeof want, "%s %s",
                             (LPCTSTR) doc->m_mush_name, c.suffix);
                    if (s_defaultFile != want) allMatched = false;
                }
                check(allMatched,
                      "export set: the save panel is pre-filled with the world "
                      "name plus the per-set suffix (was: the bare world name, "
                      "every export losing the \" triggers\"/\" aliases\"/... "
                      "Windows appends)");

                g_wxhook_file_dialog = savedFileDlg;
            }

            // ── Display ▸ Stop Sound Playing and its two new halves ─────────
            // Stop Sound Playing is handled by BOTH doc.cpp and mainfrm.cpp, and
            // MFC routes it to the DOCUMENT while one is active — which is why
            // its table row is 'M' (document first, else the frame) and not the
            // 'F' it used to be.  The two halves are plain document commands.
            {
                const Item* both    = find_by_id(items, 32292);
                const Item* trigger = find_by_id(items, 33054);
                const Item* script  = find_by_id(items, 33055);
                check(both && trigger && script,
                      "Display has Stop Sound Playing and both of its halves");
                check(trigger && trigger->label.find("Trigger") != std::string::npos,
                      "...the trigger-sound one is named for what it stops");
                check(script && script->label.find("Script") != std::string::npos,
                      "...and so is the script-sound one");
                // Enable rules: the combined one works with no world (it falls
                // through to the frame); the halves are document-only.
                check(probeUi(frame, 32292).enabled,
                      "Stop Sound Playing is live with a world open");
                check(probeUi(frame, 33054).enabled && probeUi(frame, 33055).enabled,
                      "...as are both halves");

                // Enablement cannot tell 'M' from 'F' — the frame's update
                // handler enables it either way — so the routing is pinned by
                // WHICH handler runs.  Swap in recording sound hooks and fire
                // the command for real: the document's CancelSound stops the
                // app channel AND this world's buffers; the frame's stops only
                // the app channel.
                static int s_appStops = 0, s_bufStops = 0;
                static const void* s_bufOwner = nullptr;
                void (*savedStop)()                     = g_wxhook_stop_sound;
                void (*savedStopBuf)(const void*, int)  = g_wxhook_sound_stop_buffer;
                g_wxhook_stop_sound        = []() { ++s_appStops; };
                g_wxhook_sound_stop_buffer = [](const void* owner, int buffer) {
                    if (buffer == 0) { ++s_bufStops; s_bufOwner = owner; }
                };

                s_appStops = 0; s_bufStops = 0; s_bufOwner = nullptr;
                fireCmd(frame, 32292);
                wxYield();
                check(s_appStops == 1,
                      "Stop Sound Playing silences the world's own voice...");
                check(s_bufStops == 1 && s_bufOwner == (const void*) doc,
                      "...and this world's script buffers, because MFC routes it "
                      "to the document");

                s_appStops = 0; s_bufStops = 0; s_bufOwner = nullptr;
                fireCmd(frame, 33054);
                wxYield();
                check(s_appStops == 1 && s_bufStops == 0,
                      "Stop Trigger Sounds takes only the first half");

                s_appStops = 0; s_bufStops = 0; s_bufOwner = nullptr;
                fireCmd(frame, 33055);
                wxYield();
                check(s_appStops == 0 && s_bufStops == 1 &&
                      s_bufOwner == (const void*) doc,
                      "Stop Script Sounds takes only the second");

                g_wxhook_stop_sound        = savedStop;
                g_wxhook_sound_stop_buffer = savedStopBuf;
            }

            // ── Display ▸ Clear Command History (ID_DISPLAY_CLEAR_COMMAND_HISTORY,
            // 32343) ──────────────────────────────────────────────────────────
            // Before this there was no UI route to it at all: the id existed
            // only in resource.h / accelerators.cpp / the recovered .rc, never
            // in the port's hand-built Display menu.
            {
                const Item* clearHist =
                    find_by_id(items, ID_DISPLAY_CLEAR_COMMAND_HISTORY);
                check(clearHist != NULL,
                      "Display > Clear Command History is in the menu bar");
                check(probeUi(frame, ID_DISPLAY_CLEAR_COMMAND_HISTORY).enabled,
                      "...and enabled with a world open");

                // The item itself is right, but a menu entry with no wiring
                // behind it is the exact failure mode this test file exists
                // to catch (see the file header) -- so fire it for real and
                // check the effect, not just that the row exists.
                int (*savedMB)(const char*, const char*, unsigned) =
                    g_wxhook_message_box;
                g_wxhook_message_box = [](const char*, const char*, unsigned) {
                    return IDYES;
                };
                child->addToCommandHistory("menubar_f8_probe");
                check(!child->commandList(0).empty(),
                      "...the history has an entry to clear before firing it");
                check(fireCmd(frame, ID_DISPLAY_CLEAR_COMMAND_HISTORY),
                      "...firing the command is claimed by a handler");
                check(child->commandList(0).empty(),
                      "...and it really does reach ClearCommandHistory");
                g_wxhook_message_box = savedMB;
            }

            // ── macro command ids reach the macro, not evt.Skip() ──────────
            // Pressing F1 always worked (the key handler maps the key straight
            // to a slot).  The COMMAND-ID way in did not: on Windows those ids
            // are CSendView's, and CSendView is never constructed here, so
            // world.DoCommand("MacroF1") -- and any menu item, toolbar button or
            // accelerator bound to the command name -- fell through to
            // evt.Skip() while DoCommand returned eOK.
            {
                wxTextCtrl* bar = child->ActiveInputPublic();
                check(bar != NULL, "the world window has a command bar to fire into");
                if (bar) {
                    // REPLACE_COMMAND puts the macro text in the bar, which is
                    // the observable that needs no connection.
                    doc->m_macros[MAC_F1]      = "west";
                    doc->m_macro_type[MAC_F1]  = REPLACE_COMMAND;
                    doc->m_macros[MAC_ALT_Z]   = "look";
                    doc->m_macro_type[MAC_ALT_Z] = REPLACE_COMMAND;

                    bar->SetValue(wxT("untouched"));
                    fireCmd(frame, 33026 /*ID_MACRO_F1*/);
                    wxYield();
                    check(bar->GetValue() == wxT("west"),
                          "DoCommand(\"MacroF1\") fires the F1 macro");

                    // A second, different slot: a handler that fired some fixed
                    // macro would pass the first check and fail this one.
                    bar->SetValue(wxT("untouched"));
                    fireCmd(frame, 33014 /*ID_ALT_Z*/);
                    wxYield();
                    check(bar->GetValue() == wxT("look"),
                          "...and DoCommand(\"AltZ\") fires the Alt+Z one");

                    // ID_ALT_V is published with no slot behind it (upstream);
                    // it must leave the bar alone rather than fire macro 0.
                    bar->SetValue(wxT("untouched"));
                    fireCmd(frame, 33012 /*ID_ALT_V*/);
                    wxYield();
                    check(bar->GetValue() == wxT("untouched"),
                          "...while AltV, which has no macro slot, does nothing");

                    doc->m_macros[MAC_F1] = "";
                    doc->m_macros[MAC_ALT_Z] = "";
                    bar->SetValue(wxEmptyString);
                }
            }


            // ── keypad command ids reach the keypad, not evt.Skip() ─────────
            // The same gap one table over.  All 30 keypad slots work from a
            // real numpad (OnInputKeyDown maps WXK_NUMPAD* straight to a slot),
            // but the COMMAND-ID way in -- world.DoCommand("Keypad1"), which
            // accelerators.cpp publishes and GetInternalCommandsList lists --
            // arrived at CSendView::OnKeypadCommand on Windows and at nothing
            // at all here, so it was skipped while DoCommand returned eOK.
            {
                wxTextCtrl* bar = child->ActiveInputPublic();
                check(bar != NULL, "the world window has a command bar (keypad)");

                // "0.0.0.0" is CheckConnected's own escape hatch (doc.cpp:2785)
                // -- it returns "not connected" without a message box, so the
                // send half is silent and nothing modal can appear in this
                // loop-free harness.  The alias fires before that check.
                const CString savedServer = doc->m_server;
                doc->m_server = "0.0.0.0";
                const unsigned short savedAliases = doc->m_enable_aliases;
                const unsigned short savedKeypad  = doc->m_keypad_enable;
                doc->m_enable_aliases = TRUE;
                doc->m_keypad_enable  = TRUE;

                auto addAlias = [&](const char* text) -> CAlias* {
                    CAlias* a    = new CAlias();
                    a->name      = CString(text);
                    a->contents  = "";
                    a->bEnabled  = TRUE;
                    a->bRegexp   = false;
                    a->iSendTo   = eSendToVariable;
                    a->strVariable = "zz_keypad_probe";
                    a->iSequence = DEFAULT_ALIAS_SEQUENCE;
                    a->strLabel  = CString(text);
                    a->regexp    = regcomp(
                        ConvertToRegularExpression(a->name, true, true), 0);
                    doc->GetAliasMap().SetAt(a->strLabel, a);
                    doc->GetAliasArray().Add(a);
                    doc->GetAliasRevMap()[a] = std::string(text);
                    return a;
                };

                CAlias* aPlain = addAlias("zz_kp_plain");
                CAlias* aCtrl  = addAlias("zz_kp_ctrl");
                doc->m_keypad[eKeypad_1]      = "zz_kp_plain";
                doc->m_keypad[eCtrl_Keypad_1] = "zz_kp_ctrl";

                // Two DIFFERENT slots, one from each half of the table, so a
                // handler wired to one fixed slot passes the first and fails
                // the second.
                check(fireCmd(frame, ID_KEYPAD_1),
                      "DoCommand(\"Keypad1\") is claimed by a handler");
                check(aPlain->nMatched == 1 && aCtrl->nMatched == 0,
                      "...and sends the keypad 1 slot, not some other one");
                check(fireCmd(frame, ID_CTRL_KEYPAD_1),
                      "DoCommand(\"CtrlKeypad1\") is claimed too");
                check(aCtrl->nMatched == 1,
                      "...and sends the CTRL keypad 1 slot, a different one");

                // The declining arm.  A command id has no keystroke to fall
                // through to, so where the KEY path lets an unbound numpad key
                // type its digit, the id path must type the digit itself --
                // literally CSendView's else-arm,
                // GetEditCtrl().ReplaceSel(sValues[iIndex]).
                doc->m_keypad_enable = FALSE;
                if (bar) {
                    bar->SetValue(wxT("ab"));
                    bar->SetInsertionPoint(1);
                    const bool claimed = fireCmd(frame, ID_KEYPAD_5);
                    check(claimed,
                          "with the keypad off the id is still claimed, not skipped");
                    check(bar->GetValue() == wxT("a5b"),
                          "...and types its digit at the caret, as Windows does");
                    bar->SetValue(wxEmptyString);
                }
                doc->m_keypad_enable = TRUE;

                // The arm matches thirty ids, not a range: an id no message
                // map knows must still be passed on, and must fire no slot.
                // (The adjacent ID_GAME_CONFIGURE_KEYPAD would be the sharper
                // control, but it opens the modal prefs sheet -- there is no
                // event loop here to close one.  tests/test_utilities.cpp
                // pins that id against KeypadCommandIdToSlot instead.)
                const long plainBefore = aPlain->nMatched;
                check(!fireCmd(frame, 30054),
                      "...while an id the arm does not own is still skipped");
                check(aPlain->nMatched == plainBefore,
                      "...and fires no keypad slot");

                doc->m_keypad[eKeypad_1]      = "";
                doc->m_keypad[eCtrl_Keypad_1] = "";
                doc->m_enable_aliases = savedAliases;
                doc->m_keypad_enable  = savedKeypad;
                doc->m_server         = savedServer;
            }


            // ── the scroll / caret / window command ids ─────────────────────
            // Their canonical handlers are CMUSHView::OnTest* and
            // CSendView::OnCommandHome/OnKeysTab/OnProcessEscape/OnWindow*, on
            // views the port never constructs.  The KEY that does each of these
            // was implemented in OnInputKeyDown, which is exactly what hid the
            // gap: the ids themselves matched no arm, so world.DoCommand
            // ("PageUp") / ("CommandEnd") / ("Minimize") posted, were skipped,
            // and still returned eOK.
            {
                COutputPane* pane = child->OutputPane();
                wxTextCtrl*  bar  = child->ActiveInputPublic();
                check(pane != NULL && bar != NULL,
                      "the world window has an output pane and a command bar");

                // More than a screenful, or every scroll command is a no-op and
                // "did nothing" would pass.
                for (int i = 0; i < 400; ++i)
                    doc->Note(CFormat("scroll probe line %d", i));
                doc->UpdateAllViews(nullptr);

                if (pane && bar) {
                    auto viewY = [pane]() -> int {
                        int x = 0, y = 0;
                        pane->GetViewStart(&x, &y);
                        return y;
                    };

                    check(fireCmd(frame, 32107 /*ID_TEST_END*/),
                          "DoCommand(\"End\") is claimed by a handler");
                    const int bottom = viewY();
                    check(bottom > 0,
                          "...and the fixture really is longer than the window");

                    check(fireCmd(frame, 32111 /*ID_TEST_PAGEUP*/),
                          "DoCommand(\"PageUp\") is claimed too");
                    const int afterPage = viewY();
                    check(afterPage < bottom, "...and scrolls the pane UP");

                    fireCmd(frame, 32107 /*ID_TEST_END*/);
                    fireCmd(frame, 32115 /*ID_TEST_LINEUP*/);
                    const int afterLine = viewY();
                    check(afterLine < bottom, "DoCommand(\"LineUp\") scrolls up too");
                    // The pair is what tells the two apart: a handler wired to
                    // the wrong action passes one of these and fails this.
                    check(bottom - afterLine < bottom - afterPage,
                          "...by LESS than a page, so the two are not the same command");

                    check(fireCmd(frame, 32114 /*ID_TEST_START*/),
                          "DoCommand(\"Start\") is claimed");
                    check(viewY() == 0, "...and goes to the very top");

                    fireCmd(frame, 32116 /*ID_TEST_LINEDOWN*/);
                    const int downOne = viewY();
                    check(downOne > 0, "DoCommand(\"LineDown\") comes back down");
                    fireCmd(frame, 32112 /*ID_TEST_PAGEDOWN*/);
                    check(viewY() > downOne,
                          "...and DoCommand(\"PageDown\") goes further, a page at a time");

                    // End re-engages auto-scroll, which LineDown alone does not:
                    // that is the difference between "at the end" and "one row
                    // further on".
                    fireCmd(frame, 32114 /*ID_TEST_START*/);
                    check(pane->IsPaused(),
                          "scrolled to the top, the pane is paused");
                    fireCmd(frame, 32107 /*ID_TEST_END*/);
                    check(!pane->IsPaused(),
                          "...and End re-engages auto-scroll");

                    // ── the caret commands ────────────────────────────────
                    bar->SetValue(wxT("hello"));
                    bar->SetInsertionPoint(2);
                    check(fireCmd(frame, 32978 /*ID_COMMAND_HOME*/),
                          "DoCommand(\"CommandHome\") is claimed");
                    check(bar->GetInsertionPoint() == 0,
                          "...and puts the caret at the start of the command bar");
                    check(fireCmd(frame, 32979 /*ID_COMMAND_END*/),
                          "DoCommand(\"CommandEnd\") is claimed");
                    check(bar->GetInsertionPoint() == 5,
                          "...and at the end - a different place, so one arm "
                          "cannot serve both");

                    // ── Escape ────────────────────────────────────────────
                    {
                        const unsigned short savedDel  = doc->m_bEscapeDeletesInput;
                        const unsigned short savedSave = doc->m_bSaveDeletedCommand;
                        doc->m_bEscapeDeletesInput = TRUE;
                        doc->m_bSaveDeletedCommand = TRUE;
                        bar->SetValue(wxT("zz_escape_probe"));
                        check(fireCmd(frame, 32317 /*ID_KEYS_ESCAPE*/),
                              "DoCommand(\"KeysEscape\") is claimed");
                        check(bar->GetValue().IsEmpty(),
                              "...and clears the command bar (escape_deletes_input)");
                        // The half that separates "handled" from "swallowed":
                        // what Escape throws away is kept, so it can be recalled.
                        const std::vector<std::string> hist = child->commandList(0);
                        bool kept = false;
                        for (const std::string& h : hist)
                            if (h == "zz_escape_probe") kept = true;
                        check(kept, "...and what it discarded reached the history");
                        doc->m_bEscapeDeletesInput = savedDel;
                        doc->m_bSaveDeletedCommand = savedSave;
                    }

                    // ── Tab completion ────────────────────────────────────
                    {
                        doc->Note("zz_tabcompletion_target");
                        doc->UpdateAllViews(nullptr);
                        bar->SetValue(wxT("zz_tabcompl"));
                        bar->SetInsertionPointEnd();
                        check(fireCmd(frame, 32288 /*ID_KEYS_TAB*/),
                              "DoCommand(\"KeysTab\") is claimed");
                        // The completed word, not merely "changed".
                        check(bar->GetValue().StartsWith(wxT("zz_tabcompletion_target")),
                              "...and completes the word from the output buffer");
                        bar->SetValue(wxEmptyString);
                    }

                    // ── Shift+Tab function completion ─────────────────────
                    // With nothing to offer it must take the early-out rather
                    // than open the modal chooser, which no loop-free harness
                    // could answer.  Claiming the event is the observable.
                    {
                        const unsigned short savedFns = doc->m_bTabCompleteFunctions;
                        doc->m_bTabCompleteFunctions = FALSE;
                        doc->m_ExtraShiftTabCompleteItems.clear();
                        bar->SetValue(wxT("zz_untouched"));
                        check(fireCmd(frame, 33048 /*ID_COMPLETE_FUNCTION*/),
                              "DoCommand(\"CompleteFunction\") is claimed");
                        check(bar->GetValue() == wxT("zz_untouched"),
                              "...and offers nothing when there is nothing to offer");
                        doc->m_bTabCompleteFunctions = savedFns;
                        bar->SetValue(wxEmptyString);
                    }

                    // ── activate the input area ───────────────────────────
                    // Read as "an arm claimed it": wxWindow::FindFocus is null
                    // in a process that was never activated (the edit-commands
                    // work measured this), so the focus itself is not
                    // observable here.
                    check(fireCmd(frame, 32142 /*ID_KEYS_ACTIVATECOMMANDVIEW*/),
                          "DoCommand(\"ActivateInputArea\") is claimed");

                    // ── the window commands ───────────────────────────────
                    // The target is the world's own frame, not the MDI parent:
                    // on macOS the active child IS the visible window (see the
                    // MDI-geometry note), so minimising the parent minimises
                    // nothing the user can see.  Windows' OnWindowMinimize
                    // TOGGLES (mushview.cpp:3400), which is why this fires it
                    // twice and expects to come back.
                    {
                        const wxSize savedSize = child->GetSize();

                        // Make the window SMALLER than the client area first.
                        // Without this the fixture is degenerate: a freshly
                        // built child already fills the parent, so "maximise"
                        // and "do nothing" are the same size and the assertion
                        // below could not tell them apart.
                        child->SetSize(wxSize(320, 240));
                        const wxRect want = frame->GetScreenRect();
                        check(fireCmd(frame, 33050 /*ID_WINDOW_MAXIMIZE*/),
                              "DoCommand(\"Maximize\") is claimed by a handler");
                        check(child->GetSize().x > 320 && child->GetSize().y > 240,
                              "...and grows the WORLD window, not the hidden parent");
                        check(child->GetSize() == want.GetSize(),
                              "...to exactly the client area, not the whole screen");
                        check(!child->IsFullScreen(),
                              "...and without zooming it full-screen");

                        // Minimize and Restore can only be read as "an arm
                        // claimed it" here.  wxOSX's Iconize is
                        // -[NSWindow miniaturize:], which the window server
                        // completes on the run loop this harness exits before:
                        // measured, IsIconized() is still false immediately
                        // after.  Same limit the ID_WORLDS_WORLD block records
                        // for Activate().
                        check(fireCmd(frame, 32235 /*ID_WINDOW_MINIMIZE*/),
                              "DoCommand(\"Minimize\") is claimed");
                        check(fireCmd(frame, 32235),
                              "...and again, since Windows' handler TOGGLES");
                        check(fireCmd(frame, 33051 /*ID_WINDOW_RESTORE*/),
                              "DoCommand(\"Restore\") is claimed");
                        child->Restore();
                        child->SetSize(savedSize);
                    }
                }
            }

            // ── a canonical command with no menu item of its own still runs ──
            // The port's Configure menu items carry PRIVATE ids, so the
            // canonical ID_GAME_CONFIGURE_* ids only ever arrive from
            // world.DoCommand(name) or an accelerator.  They used to reach the
            // dispatcher's final else and evt.Skip() — DoCommand posted the id,
            // nothing consumed it, and it still returned eOK.  MFC would have
            // routed them to the active DOCUMENT, whose message map has had the
            // handlers all along.
            {
                static int s_page = -999, s_sheets = 0;
                intptr_t (*const savedSheet)(void*) = g_run_mfc_property_sheet;
                g_run_mfc_property_sheet = [](void* self) -> intptr_t {
                    s_page = ((CTreePropertySheet*) self)->m_iWantedPage;
                    ++s_sheets;
                    return IDCANCEL;      // decline: no SavePrefs* runs
                };

                // Four ids, four DIFFERENT pages, so a handler that opened the
                // sheet at some fixed page could not pass.  The last two are the
                // ids furthest from the others (32344 / 33025), which is where an
                // id-range shortcut would fail.
                const struct { int id; int page; const char* name; } kCases[] = {
                    { 32301, ePageANSIColours, "ConfigureColours"          },
                    { 32306, ePageKeypad,      "ConfigureKeypad"           },
                    { 32344, ePageConnecting,  "ConfigureNameAndPassword"  },
                    { 33025, ePageChat,        "ConfigureChat"             },
                };
                for (const auto& c : kCases) {
                    s_page = -999; s_sheets = 0;
                    const bool claimed = fireCmd(frame, c.id);
                    wxYield();
                    checkf(s_sheets == 1,
                           std::string("DoCommand(\"") + c.name + "\") reaches the document");
                    checkf(s_page == c.page,
                           std::string("...and opens World Properties at its own page"));
                    checkf(claimed, std::string("...and the event is consumed, not skipped"));
                }

                // The control: routing to the document must not become a
                // catch-all.  30054 is in no message map anywhere, so it must
                // still be SKIPPED — and skipping is the half that matters,
                // because "no sheet opened" is equally true of a dispatcher
                // that swallowed the event.  ProcessEvent answers false only
                // when the handler skipped and nobody else claimed it.
                s_page = -999; s_sheets = 0;
                const bool strayClaimed = fireCmd(frame, 30054);
                wxYield();
                check(s_sheets == 0,
                      "an id no message map knows opens nothing");
                check(!strayClaimed,
                      "...and is passed on rather than swallowed");

                g_run_mfc_property_sheet = savedSheet;
            }

            // ── File ▸ Save / Save As go through the ONE save flow ──────────
            // There used to be two hand-copied versions of the whole save/
            // save-as sequence: this menu path had its own, and the close
            // prompt called g_wxhook_save_world (wx_world_child.cpp:6038).
            // They had drifted -- the menu copy's failure message box was
            // parented to nothing, so a save error appeared as a detached
            // app-modal alert instead of belonging to the world that failed.
            // Asserting the PARENT is what pins the merge: a revert stops
            // calling the hook at all and every line here fails.
            {
                bool (*savedSaveWorld)(void*, bool, void*) = g_wxhook_save_world;
                g_wxhook_save_world = [](void* d, bool as, void* p) -> bool {
                    ++g_saveCalls;
                    g_saveDoc    = d;
                    g_saveAs     = as;
                    g_saveParent = p;
                    return true;      // no disk, no Save As dialog
                };

                g_saveCalls = 0; g_saveDoc = NULL; g_saveParent = NULL;
                fireCmd(frame, 0xE103 /*ID_FILE_SAVE*/);
                check(g_saveCalls == 1, "File > Save reaches the save flow");
                check(g_saveDoc == doc, "...with the active world's document");
                check(!g_saveAs, "...as a plain Save, not a Save As");
                check(g_saveParent == child,
                      "...parented to the world's own child frame");

                g_saveCalls = 0; g_saveDoc = NULL; g_saveParent = NULL;
                fireCmd(frame, 0xE104 /*ID_FILE_SAVE_AS*/);
                check(g_saveCalls == 1, "File > Save As reaches the same flow");
                check(g_saveAs, "...asking for the Save As arm");
                check(g_saveParent == child,
                      "...and parented to the child frame too");

                g_wxhook_save_world = savedSaveWorld;
            }

            doc->m_pWorldView = NULL;
            wx_unregister_world_frame(doc);   // Destroy() skips OnClose — see above
            child->Destroy();
            wxYield();
        }
    }

    // ── ID_WORLDS_WORLD1..0 switch worlds (2026-08-18) ──────────────────
    // InitInstance ends by posting ID_WORLDS_WORLD1 (MUSHclient.cpp:950) so
    // that with "Open activity window at startup" on, the Activity frame does
    // not keep the focus.  On the port that was a silent no-op: the canonical
    // handler is CMainFrame::OnWorldSwitch, an ON_COMMAND_EX entry, and the
    // port's message maps never expand ON_COMMAND_EX -- so no amount of
    // AfxDispatchCommand could have reached it, and the id fell through to
    // evt.Skip().
    //
    // What CANNOT be asserted here is the end of the chain.  Window activation
    // needs the event loop this harness exits before: measured 2026-08-18,
    // wxMDIChildFrame::Activate() moves neither GetActiveChild() nor a single
    // wxEVT_ACTIVATE, whichever child it is called on.  So the two halves that
    // ARE observable get asserted instead -- the frame now CLAIMS the command
    // (it used to skip), and the slot it resolves comes from the one lookup
    // the activity toolbar also uses.
    {
        CWnd* const savedMainWnd = App.m_pMainWnd;
        App.m_pMainWnd = &host;

        auto makeWorld = [&](const char* name) -> CWorldChildFrame* {
            CMUSHclientDoc* d = new CMUSHclientDoc();
            d->m_mush_name   = name;
            d->m_nWrapColumn = 80;
            d->m_maxlines    = 5000;
            d->SetUpOutputWindow();
            return new CWorldChildFrame(frame, d, wxString::FromUTF8(name));
        };

        CWorldChildFrame* w1 = makeWorld("zz_selftest_switch1");
        CWorldChildFrame* w2 = makeWorld("zz_selftest_switch2");
        w1->Show();
        w2->Show();
        wxYield();

        // Earlier blocks left their documents behind, so take the slots as
        // assigned rather than assuming 1 and 2 -- what matters is that two
        // DIFFERENT worlds hold two DIFFERENT slots.
        App.UpdateWorldCounts();
        const int s1 = w1->GetDoc()->m_view_number;
        const int s2 = w2->GetDoc()->m_view_number;
        check(s1 >= 1 && s2 >= 1 && s1 != s2,
              "two open worlds hold two distinct slots");
        check(wx_world_in_slot(s1) == w1->GetDoc() &&
              wx_world_in_slot(s2) == w2->GetDoc(),
              "...and the shared slot lookup maps each id to its own world");

        // The chain InitInstance actually uses: CWnd::SendMessage(WM_COMMAND)
        // -> the send-message hook -> resolveTarget(App.m_pMainWnd) -> this
        // frame's dispatcher.  Observed with a handler that passes the event
        // on, so the real arm still runs behind it.
        static int seen = -1;
        frame->Bind(wxEVT_MENU, [](wxCommandEvent& e) { seen = e.GetId(); e.Skip(); });
        seen = -1;
        // One of THIS block's own slots, not a bare ID_WORLDS_WORLD1: earlier
        // blocks leave their documents behind, so slot 1 can belong to a world
        // whose frame is already gone.
        const int wantId = ID_WORLDS_WORLD1 + s1 - 1;
        Frame.SendMessage(WM_COMMAND, wantId, 0);
        check(seen == wantId,
              "Frame.SendMessage(WM_COMMAND, ID_WORLDS_WORLD*) reaches the frame");

        check(fireCmd(frame, ID_WORLDS_WORLD1 + s1 - 1),
              "...and a world-switch id is consumed, not skipped (was: skipped)");
        check(fireCmd(frame, ID_WORLDS_WORLD1 + s2 - 1),
              "...for the second world's slot too");
        // Both ends of the block, since it is a RANGE test and an off-by-one
        // at either end is the mistake to fear.
        check(fireCmd(frame, ID_WORLDS_WORLD0),
              "...and the last slot (the button labelled 0, i.e. world 10)");

        // The control: the arm must not have become a catch-all.  32214 is
        // ID_GAME_TESTCOMMAND, one past the block, and no message map here
        // knows it -- so it must still be passed on.
        check(!fireCmd(frame, 32214 /*ID_GAME_TESTCOMMAND*/),
              "...while the id one past the block is still skipped");

        w1->GetDoc()->m_pWorldView = NULL;
        w2->GetDoc()->m_pWorldView = NULL;
        wx_unregister_world_frame(w1->GetDoc());   // Destroy() skips OnClose
        wx_unregister_world_frame(w2->GetDoc());
        w1->Destroy();
        w2->Destroy();
        wxYield();
        App.m_pMainWnd = savedMainWnd;
    }

    // ── the toolbar's Connect/Disconnect toggle sends the SAME id the menu
    //    does (2026-08-18) ────────────────────────────────────────────────
    // Windows has one toggle button; the port splits it into two commands and
    // picks by connection phase.  The Disconnect half used to carry a bare
    // 0xE200 while menu_ui_rules.h named the same number — the second copy of
    // an id this exact command has already come adrift on once.  Nothing had
    // ever pressed the button, so a drift would have been silent.
    //
    // Both branches are asserted: an implementation stuck on either one is a
    // plausible mistake, and only the pair rules it out.  Deliberately runs
    // after the world child is gone — the MDI parent offers its events to the
    // active child first, whose own toolbar would answer instead of us.
    {
        CMUSHclientDoc* tdoc = new CMUSHclientDoc();
        tdoc->m_mush_name = "ToolBarSelfTest";
        tdoc->m_server    = "mud.example.com";

        wxFrame*     tbHost  = new wxFrame(frame, wxID_ANY, wxT("toolbar-host"));
        wxBoxSizer*  tbSizer = new wxBoxSizer(wxVERTICAL);
        tbHost->SetSizer(tbSizer);
        // Heap, and deleted below rather than leaked: a live CWorldToolBars
        // stays on the list every other strip pushes a rearrangement to, so one
        // left behind with a destroyed host is a crash waiting for the next
        // drag.  Deleting it is safe now that the click lambda bound on the
        // frame carries an alive token (wx_toolbar.cpp).
        CWorldToolBars* bars = new CWorldToolBars(tbHost, tbSizer);

        check(wxTheApp->GetTopWindow() != tbHost,
              "the toolbar's forwarding target is not its own frame");

        frame->Bind(wxEVT_MENU, &recordForwardedCmd);

        tdoc->m_iConnectPhase = eConnectConnectedToMud;
        bars->UpdateState(tdoc);         // the only way in to the active doc
        g_forwardedCmd = -1;
        pressTool(tbHost, ID_CONNECT_DISCONNECT);
        check(g_forwardedCmd == kPortMenuIdDisconnect,
              "a connected world's toggle forwards kPortMenuIdDisconnect");

        tdoc->m_iConnectPhase = eConnectNotConnected;
        bars->UpdateState(tdoc);
        g_forwardedCmd = -1;
        pressTool(tbHost, ID_CONNECT_DISCONNECT);
        check(g_forwardedCmd == 32239 /*ID_CONNECTION_CONNECT*/,
              "...and a disconnected one forwards Connect (the other branch)");

        frame->Unbind(wxEVT_MENU, &recordForwardedCmd);
        delete bars;              // UnInits its wxAuiManager -- before the host goes
        tbHost->Destroy();
        wxYield();
    }


    // ── the World bar's check buttons follow the truth (2026-09-01) ─────────
    // CWorldToolBars::UpdateState ran the canonical ON_UPDATE_COMMAND_UI for
    // each button against the DOCUMENT alone, where Windows' MFC routes a
    // toolbar update view -> document -> frame -> app.  Freeze Output's
    // canonical handler is CMUSHView::OnUpdateDisplayFreezeoutput
    // (mushview.cpp:3240) on the never-constructed view, and the document's map
    // ends in OnFixMenus, which returns before capturing anything -- so
    // `checked` stayed -1 and ToggleTool was never called for that button at
    // all.  Its pressed state was therefore whatever wxToolBarTool::Action's
    // auto-toggle last wrote, which inverts as soon as the freeze is changed
    // from any of the other four doors (the Display menu, the output pane's
    // context item, the status-bar pane, scrolling back).
    //
    // Driven through a real COutputPane, because the port's answer for this
    // command is `doc->m_pWorldView->isPaused()` and the world frame IS that
    // view.  The bar is a standalone strip fed the real document, which is the
    // only way in to UpdateState from out here.
    {
        CMUSHclientDoc* fdoc = new CMUSHclientDoc();
        fdoc->m_mush_name   = "zz_selftest_toolbarcheck";
        fdoc->m_nWrapColumn = 80;
        fdoc->m_maxlines    = 5000;
        fdoc->SetUpOutputWindow();
        CWorldChildFrame* wf =
            new CWorldChildFrame(frame, fdoc, wxT("zz_selftest_toolbarcheck"));
        wf->Show();
        wxYield();

        wxFrame*    tbHost  = new wxFrame(frame, wxID_ANY, wxT("toolbar-checks"));
        wxBoxSizer* tbSizer = new wxBoxSizer(wxVERTICAL);
        tbHost->SetSizer(tbSizer);
        CWorldToolBars* bars = new CWorldToolBars(tbHost, tbSizer);

        wxToolBar* wb = findToolBarWith(tbHost, ID_DISPLAY_FREEZEOUTPUT);
        check(wb != NULL, "the World toolbar carries a Freeze Output button");
        check(wf->OutputPane() != NULL, "...and the world window has an output pane");

        if (wb && wf->OutputPane()) {
            // FREEZE through one of the other doors -- not the button.  This is
            // the assertion that fails at HEAD: nothing ever ticked it.
            wf->OutputPane()->SetPaused(true);
            bars->UpdateState(fdoc);
            check(wb->GetToolState(ID_DISPLAY_FREEZEOUTPUT),
                  "freezing the pane presses the toolbar's Freeze button");

            // Both polarities, so an implementation stuck on `true` fails here.
            wf->OutputPane()->SetPaused(false);
            bars->UpdateState(fdoc);
            check(!wb->GetToolState(ID_DISPLAY_FREEZEOUTPUT),
                  "...and un-freezing releases it again");

            // The inversion itself.  wxToolBarTool::Action presses a
            // wxITEM_CHECK tool BEFORE dispatching the click, so this is
            // exactly the state a click leaves behind; with the pane running,
            // the next update must correct it rather than leave the button
            // reading the opposite of the truth.
            wb->ToggleTool(ID_DISPLAY_FREEZEOUTPUT, true);
            bars->UpdateState(fdoc);
            check(!wb->GetToolState(ID_DISPLAY_FREEZEOUTPUT),
                  "...and a click's own auto-toggle is corrected, not kept");

            // Log Session: the document HAS always answered this one
            // (OnUpdateFileLogsession, doc.cpp:1008) -- the button was simply
            // built wxITEM_NORMAL, so the answer had nowhere to go.
            check(!wb->GetToolState(ID_FILE_LOGSESSION),
                  "the Log Session button is up on a world that is not logging");
            FILE* const saved = fdoc->m_logfile;
            fdoc->m_logfile = fopen("/dev/null", "w");
            bars->UpdateState(fdoc);
            check(fdoc->m_logfile == NULL ||
                  wb->GetToolState(ID_FILE_LOGSESSION),
                  "...and pressed while the session IS being logged");
            if (fdoc->m_logfile) fclose(fdoc->m_logfile);
            fdoc->m_logfile = saved;
            bars->UpdateState(fdoc);
            check(!wb->GetToolState(ID_FILE_LOGSESSION),
                  "...and up again when the log closes");
        }

        delete bars;              // UnInits its wxAuiManager -- before the host
        tbHost->Destroy();
        fdoc->m_pWorldView = NULL;
        wx_unregister_world_frame(fdoc);
        wf->Destroy();
        wxYield();
    }

    // ── the three toolbars can be REARRANGED, and the strip follows (2026-08-23) ──
    // Windows creates all three CBRS_GRIPPER and gives them
    // EnableDocking (CBRS_ALIGN_ANY) (mainfrm.cpp:318-409), and tips.txt opens
    // with "You can drag the "game" toolbar alongside the "file" toolbar, to
    // save room at the top of the screen".  The port had them nailed to one
    // sizer row each.  They are now wxAuiManager panes in a strip.
    //
    // The drag GESTURE is wxAuiManager's and there is no way to inject the
    // mouse events it reads, so it is not what is pinned here.  Everything the
    // gesture ENDS in is this port's code and is: the relayout, the strip's
    // height, the save, and the push to the other world windows.
    {
        typedef CWorldToolBars::Placement Place;
        const CWorldToolBars::Which kStd = CWorldToolBars::eStandard;
        const CWorldToolBars::Which kWld = CWorldToolBars::eWorld;
        const CWorldToolBars::Which kAct = CWorldToolBars::eActivity;

        wxFrame*    tbHost  = new wxFrame(frame, wxID_ANY, wxT("toolbar-arrange"));
        wxBoxSizer* tbSizer = new wxBoxSizer(wxVERTICAL);
        tbHost->SetSizer(tbSizer);
        CWorldToolBars* bars = new CWorldToolBars(tbHost, tbSizer);

        check(bars->GetPlacement(kStd).row == 0 &&
              bars->GetPlacement(kWld).row == 1 &&
              bars->GetPlacement(kAct).row == 2,
              "the three toolbars start one per row, in canonical order");

        const int threeRows = bars->StripHeight();
        // Tall enough for three rows of the 16x15 toolbar art.  "> 0" would
        // have been satisfied by the strip's default 20 px -- i.e. by a frame
        // that was never laid out at all, which is one of the ways this can
        // break.
        checkf(threeRows >= 3 * 15,
               std::string("the toolbar strip is three rows tall (")
                   + std::to_string(threeRows) + " px)");

        // A pure REORDER: still one bar per row, different order.  The height
        // must NOT move.  This is the pair that tells a reorder from a merge --
        // an implementation that quietly ignored row numbers would satisfy the
        // height half and fail the placement half, and one that rebuilt the
        // strip from scratch every time would do the reverse.
        {
            Place p[CWorldToolBars::eBarCount];
            p[kStd] = { 1, 0 };
            p[kWld] = { 2, 0 };
            p[kAct] = { 0, 0 };
            bars->SetArrangement(p);
        }
        check(bars->GetPlacement(kAct).row == 0 &&
              bars->GetPlacement(kStd).row == 1 &&
              bars->GetPlacement(kWld).row == 2,
              "the rows can be reordered");
        check(bars->StripHeight() == threeRows,
              "...and reordering three rows leaves the strip the same height");

        // The tip's own case: the World bar dragged up alongside the Standard
        // one.  Two rows need less height than three, so if the strip did not
        // really re-lay out, this is the assertion that fails.
        {
            Place p[CWorldToolBars::eBarCount];
            p[kStd] = { 0, 0 };
            p[kWld] = { 0, 1 };
            p[kAct] = { 1, 0 };
            bars->SetArrangement(p);
        }
        check(bars->GetPlacement(kStd).row   == 0 &&
              bars->GetPlacement(kWld).row   == 0 &&
              bars->GetPlacement(kStd).order == 0 &&
              bars->GetPlacement(kWld).order == 1,
              "two bars can share one row, in a known left-to-right order");
        const int twoRows = bars->StripHeight();
        checkf(twoRows < threeRows,
               std::string("...and the strip gives back the row they saved (")
                   + std::to_string(threeRows) + " -> " + std::to_string(twoRows) + ")");

        // It has to reach the preferences FILE, not just the record in memory --
        // the arrangement is meant to survive a restart.  The key names are
        // written out here on purpose: renaming one would silently discard
        // everybody's saved arrangement, and nothing else would notice.
        {
            const std::unique_ptr<wxConfigBase> cfg = OpenAppConfig();
            int row = -1, order = -1;
            cfg->Read(wxT("/toolbars/row1"),   &row,   -1);
            cfg->Read(wxT("/toolbars/order1"), &order, -1);
            checkf(row == 0 && order == 1,
                   std::string("the arrangement is saved to preferences (row=")
                       + std::to_string(row) + " order=" + std::to_string(order) + ")");
        }

        // A second strip, standing in for a second world window.  Windows has
        // ONE set of bars on its single main frame, so rearranging them is
        // global; here every world window owns a set and the change has to be
        // pushed -- the same rule wx_mfc_impl.cpp follows for a visibility
        // toggle.  First half: a window opened LATER adopts the arrangement.
        wxFrame*    tbHost2  = new wxFrame(frame, wxID_ANY, wxT("toolbar-arrange-2"));
        wxBoxSizer* tbSizer2 = new wxBoxSizer(wxVERTICAL);
        tbHost2->SetSizer(tbSizer2);
        CWorldToolBars* bars2 = new CWorldToolBars(tbHost2, tbSizer2);

        check(bars2->GetPlacement(kWld).row   == 0 &&
              bars2->GetPlacement(kWld).order == 1,
              "a world window opened later starts with the arrangement in use");

        // Second half, and the one that fails if nothing is pushed: rearrange
        // in the SECOND window and watch the FIRST follow.
        {
            Place p[CWorldToolBars::eBarCount];
            p[kStd] = { 0, 0 };
            p[kWld] = { 1, 0 };
            p[kAct] = { 1, 1 };
            bars2->SetArrangement(p);
        }
        check(bars->GetPlacement(kAct).row   == 1 &&
              bars->GetPlacement(kAct).order == 1 &&
              bars->GetPlacement(kWld).row   == 1,
              "rearranging one world window rearranges the others too");

        // View ▸ Reset Toolbar Locations, in every window at once.
        bars->ResetLocations();
        check(bars->GetPlacement(kStd).row == 0 &&
              bars->GetPlacement(kWld).row == 1 &&
              bars->GetPlacement(kAct).row == 2 &&
              bars->StripHeight() == threeRows,
              "Reset Toolbar Locations puts them back one per row");
        check(bars2->GetPlacement(kWld).row == 1 &&
              bars2->GetPlacement(kAct).row == 2,
              "...and that reaches the other world windows as well");

        // Hiding a bar takes its row with it -- View > Toolbars and the drag
        // arrangement share one strip, and this is where they could disagree.
        bars->SetBarVisible(kAct, false);
        checkf(bars->StripHeight() < threeRows,
               std::string("hiding a bar gives up its row (")
                   + std::to_string(threeRows) + " -> "
                   + std::to_string(bars->StripHeight()) + ")");
        bars->SetBarVisible(kAct, true);
        check(bars->StripHeight() == threeRows,
              "...and showing it again takes the row back");

        // Regression: hiding a bar that is NOT last renumbers the visible bars
        // into lower rows, and the hidden bar used to keep its old ABSOLUTE row
        // -- so Standard (row 0) and World (renumbered 1 -> 0) both read
        // (row 0, order 0), and re-showing Standard docked it on top of World.
        // Every bar must have a DISTINCT (row, order) even while one is hidden.
        bars->SetBarVisible(kStd, false);
        {
            const Place a = bars->GetPlacement(kStd);
            const Place b = bars->GetPlacement(kWld);
            const Place c = bars->GetPlacement(kAct);
            auto same = [](const Place& x, const Place& y) {
                return x.row == y.row && x.order == y.order;
            };
            check(!same(a, b) && !same(a, c) && !same(b, c),
                  "a hidden bar does not collide with a renumbered visible bar");
        }
        bars->SetBarVisible(kStd, true);   // restore before teardown

        delete bars2;             // both before their hosts: see the block above
        delete bars;
        tbHost2->Destroy();
        tbHost->Destroy();
    }

    // The user's real preferences again.
    SetAppConfigPathForTesting(savedCfgPath);
    {
        char scratch[256];
        snprintf(scratch, sizeof scratch, "/tmp/mc_selftest_menubar_%d.cfg",
                 (int) getpid());
        remove(scratch);
    }

    // ── app activation reaches CMainFrame::m_bActive (2026-08-18) ───────────
    // Windows maintains it in CMainFrame::OnActivate; the port never did, so
    // the `if (bFlashIcon && !m_bActive)` guard at mainfrm.cpp:811 -- the one
    // that stops a background world asking for attention while MUSHclient is
    // already the frontmost app -- was vacuously true, and m_bFlashingWindow
    // was set once and never cleared (its only clearer is that same handler).
    {
        const bool savedActive = Frame.m_bActive;
        const bool savedFlash  = Frame.m_bFlashingWindow;

        Frame.m_bActive = false;
        {
            wxActivateEvent act(wxEVT_ACTIVATE_APP, true);
            wxTheApp->ProcessEvent(act);
        }
        check(Frame.m_bActive, "activating the app sets CMainFrame::m_bActive");

        {
            wxActivateEvent act(wxEVT_ACTIVATE_APP, false);
            wxTheApp->ProcessEvent(act);
        }
        check(!Frame.m_bActive, "...and deactivating clears it again");

        // The half the flash actually depends on: a window flashed while in
        // the background must stop being marked as flashing when the user
        // comes back to it, or it can never be flashed again.
        Frame.m_bFlashingWindow = true;
        {
            wxActivateEvent act(wxEVT_ACTIVATE_APP, true);
            wxTheApp->ProcessEvent(act);
        }
        check(!Frame.m_bFlashingWindow,
              "coming back to the app clears m_bFlashingWindow");

        // The control: deactivating must NOT clear it, or the flag would look
        // right for the wrong reason.
        Frame.m_bFlashingWindow = true;
        {
            wxActivateEvent act(wxEVT_ACTIVATE_APP, false);
            wxTheApp->ProcessEvent(act);
        }
        check(Frame.m_bFlashingWindow,
              "...while leaving it does not (only activation stops a flash)");

        Frame.m_bActive         = savedActive;
        Frame.m_bFlashingWindow = savedFlash;
    }

    frame->Destroy();
    wxYield();

    printf("MENUBAR-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

#endif // !_WIN32
