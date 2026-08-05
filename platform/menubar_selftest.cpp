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

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "childfrm.h"
#include "MUSHview.h"
#include "resource.h"
#include "platform/wx_world_child.h"
#include "platform/wx_mfc_hooks.h"

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

    frame->Destroy();
    wxYield();

    printf("MENUBAR-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

#endif // !_WIN32
