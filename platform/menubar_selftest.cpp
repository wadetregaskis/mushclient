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

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "childfrm.h"
#include "MUSHview.h"
#include "resource.h"
#include "platform/wx_world_child.h"
#include "platform/wx_mfc_hooks.h"
#include "platform/wx_mfc_logic.h"   // RecentWorlds* — the MRU behind File ▸ Open Recent
#include "dialogs/world_prefs/TreePropertySheet.h"   // m_iWantedPage — which page a Configure command asked for

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

            // A View item that deliberately stays live with nothing open --
            // proof the probe is reading a real answer and not just "false".
            const Item* statusBar = find_by_label(items, "Status Bar");
            check(statusBar != NULL, "View > Status Bar is in the menu bar");
            check(statusBar && probeUi(frame, statusBar->id).enabled,
                  "...and is live with no world open, as it is on Windows");

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
            for (int phase = eConnectMudNameLookup; phase <= eConnectDisconnecting;
                 ++phase) {
                doc->m_iConnectPhase = phase;
                if (!probeUi(frame, 0xE200).enabled) {
                    liveThroughout = false;
                    printf("    (Disconnect greyed in connect phase %d)\n", phase);
                }
                if (probeUi(frame, 32239).enabled) connectDead   = false;
                if (probeUi(frame, 0xE201).enabled) reconnectDead = false;
            }
            check(liveThroughout,
                  "Disconnect stays live through every phase of a hanging connect");
            check(connectDead && reconnectDead,
                  "...while Connect and Reconnect are dead in all of them");

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

            doc->m_pWorldView = NULL;
            child->Destroy();
            wxYield();
        }
    }

    frame->Destroy();
    wxYield();

    printf("MENUBAR-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

#endif // !_WIN32
