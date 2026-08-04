// platform/gui_bridge_selftest.cpp — in-app conformance test for the wx bridge
// layers the headless mushcore tests cannot reach (--gui-bridge-selftest).
//
// mushcore links neither wx nor the wx adapters, so the window-geometry
// persistence, the dialog-template → wx control mapping and the world-creation
// hooks have no home in tests/.  This harness runs inside the real app binary
// (real wx, real recovered resources, real CMUSHclientDoc) before the GUI boots,
// prints one line per assertion and exits — the same shape as the surface /
// transport / miniwindow selftests, and registered with ctest alongside them.
//
// It writes only to a throwaway config file in the system temp dir: the geometry
// code takes its wxConfigBase by reference precisely so this never touches the
// user's real preferences.

#ifndef _WIN32

// ── wxWidgets (first — same include-order rule as wx_mfc_impl.cpp) ─────────
#include <wx/wx.h>
#include <wx/mdi.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/listbox.h>
#include <memory>
#include <cstdio>

// ── MUSHclient headers ────────────────────────────────────────────────────
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "resource.h"                   // IDD_SEND_TO_ALL, IDC_WORLD_LIST
#include "platform/wx_world_child.h"    // wx_save_child_geometry
#include "platform/wx_dialog_bridge.h"  // MfcDialogResources, BuildWxDialogFromTemplate

// Defined in wx_mfc_impl.cpp: the geometry writer the quit hook and the MDI
// parent's close handler share, taking the config it writes into.
void SaveWindowGeometryTo(wxConfigBase& cfg, wxTopLevelWindow* frame);

// Defined in wx_mfc_impl.cpp: the pre-load defaults File ▸ New World and Open
// World File both apply to a freshly constructed document.
void wx_seed_new_world_defaults(CMUSHclientDoc* doc);

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

// A wxFileConfig on a scratch file — never the user's real preferences.
wxString ScratchConfigPath()
{
    wxFileName fn(wxStandardPaths::Get().GetTempDir(),
                  wxT("mushclient-selftest-geometry.ini"));
    if (fn.FileExists()) wxRemoveFile(fn.GetFullPath());
    return fn.GetFullPath();
}

// ── 1. Window geometry survives a quit ────────────────────────────────────
// The quit hook (hook_query_quit_save) calls SaveWindowGeometryTo with the MDI
// parent.  On macOS the window the user sees and resizes is the active MDI
// child, so that is what gets measured — but the child sizes itself at
// construction from /geometry/child_w + /geometry/child_h, and the quit path
// used to write only the main_* keys, which are read back onto the MDI parent
// wxOSX never shows.  Result: ⌘Q lost the size, ⇧⌘W kept it.
void TestGeometry()
{
    printf("Window geometry (quit path):\n");

    const wxString path = ScratchConfigPath();

    wxMDIParentFrame* parent = new wxMDIParentFrame(
        nullptr, wxID_ANY, wxT("selftest-parent"),
        wxDefaultPosition, wxSize(1000, 700));
    wxMDIChildFrame* child = new wxMDIChildFrame(
        parent, wxID_ANY, wxT("selftest-child"),
        wxDefaultPosition, wxSize(640, 480));
    child->Activate();                       // make it the frame's active child
    child->SetSize(640, 480);

    const bool haveChild = (parent->GetActiveChild() == child);
    check(haveChild, "the MDI parent reports the child we just created");

    {
        wxFileConfig cfg(wxEmptyString, wxEmptyString, path, wxEmptyString,
                         wxCONFIG_USE_LOCAL_FILE);
        SaveWindowGeometryTo(cfg, parent);

        long w = 0, h = 0;
        const bool hadW = cfg.Read(wxT("/geometry/child_w"), &w);
        const bool hadH = cfg.Read(wxT("/geometry/child_h"), &h);
        check(hadW && hadH,
              "the quit path writes the keys the world window reads back "
              "(/geometry/child_w + child_h)");
        // The child's own frame size, not the parent's 1000x700.
        check(w == 640 && h == 480,
              "...and they carry the VISIBLE window's size, not the MDI parent's");

        long mw = 0, mh = 0;
        cfg.Read(wxT("/geometry/main_w"), &mw);
        cfg.Read(wxT("/geometry/main_h"), &mh);
        check(mw == 640 && mh == 480,
              "the main_* keys still get the same measurement (unchanged)");
    }

    // A plain top-level window (no MDI child in play) must NOT claim to be the
    // world window: writing child_* there would let, say, a notepad's size
    // become every future world window's size.
    {
        wxRemoveFile(path);
        wxFrame* plain = new wxFrame(nullptr, wxID_ANY, wxT("selftest-plain"),
                                     wxDefaultPosition, wxSize(300, 200));
        wxFileConfig cfg(wxEmptyString, wxEmptyString, path, wxEmptyString,
                         wxCONFIG_USE_LOCAL_FILE);
        SaveWindowGeometryTo(cfg, plain);
        long w = 0;
        check(!cfg.Read(wxT("/geometry/child_w"), &w),
              "a non-MDI top-level window does not write the world-window keys");
        plain->Destroy();
    }

    // The shared helper refuses a degenerate size, so a torn-down or
    // never-laid-out window cannot wipe a good saved size.
    {
        wxRemoveFile(path);
        wxFileConfig cfg(wxEmptyString, wxEmptyString, path, wxEmptyString,
                         wxCONFIG_USE_LOCAL_FILE);
        wx_save_child_geometry(cfg, wxSize(0, 0));
        long w = 0;
        check(!cfg.Read(wxT("/geometry/child_w"), &w),
              "a zero-size window is not persisted");
    }

    child->Destroy();
    parent->Destroy();
    wxRemoveFile(path);
}

// ── 2. Dialog bridge: list-box selection model ────────────────────────────
// LBS_MULTIPLESEL (0x8) and LBS_EXTENDEDSEL (0x800) are different models.  Both
// used to map to wxLB_EXTENDED, where a plain click REPLACES the selection —
// so in Game ▸ Send to All Worlds (the one LBS_MULTIPLESEL list in the
// recovered resources) each world the user clicked deselected the previous one.
void TestListBoxStyles()
{
    printf("Dialog bridge (list-box selection model):\n");

    const RcDialog* t = MfcDialogResources().FindDialog(IDD_SEND_TO_ALL);
    check(t != nullptr, "IDD_SEND_TO_ALL is in the recovered resources");
    if (t)
    {
        wxDialog* dlg = BuildWxDialogFromTemplate(nullptr, *t);
        wxListBox* lb = wxDynamicCast(dlg->FindWindow(IDC_WORLD_LIST), wxListBox);
        check(lb != nullptr, "its world list is built as a wxListBox");
        if (lb)
        {
            const long st = lb->GetWindowStyleFlag();
            check((st & wxLB_MULTIPLE) != 0,
                  "LBS_MULTIPLESEL builds wxLB_MULTIPLE (a click toggles one item)");
            check((st & wxLB_EXTENDED) == 0,
                  "...and NOT wxLB_EXTENDED (where a click clears the others)");
            // Multi-selection itself still works through the CListBox shim.
            lb->Append(wxT("one")); lb->Append(wxT("two")); lb->Append(wxT("three"));
            lb->SetSelection(0);
            lb->SetSelection(2);
            wxArrayInt sel;
            check(lb->GetSelections(sel) == 2,
                  "two programmatic selections coexist (CListBox::SetSel path)");
        }
        dlg->Destroy();
    }

    // A template that asks for the Shift/Ctrl model still gets it, and a plain
    // list box stays single-select.  No recovered template uses LBS_EXTENDEDSEL,
    // so synthesise both through the same production builder.
    {
        RcDialog synth;
        synth.id = 1; synth.cx = 200; synth.cy = 100;
        synth.caption = "selftest";
        RcControl ext;
        ext.className = "ListBox"; ext.id = 1000;
        ext.style = 0x50A10801;     // WS_* | LBS_NOTIFY | LBS_EXTENDEDSEL(0x800)
        ext.x = 5; ext.y = 5; ext.cx = 100; ext.cy = 40;
        RcControl single;
        single.className = "ListBox"; single.id = 1001;
        single.style = 0x50B10101;   // the ordinary single-select list style
        single.x = 5; single.y = 50; single.cx = 100; single.cy = 40;
        synth.controls.push_back(ext);
        synth.controls.push_back(single);

        wxDialog* dlg = BuildWxDialogFromTemplate(nullptr, synth);
        wxListBox* e = wxDynamicCast(dlg->FindWindow(1000), wxListBox);
        wxListBox* s = wxDynamicCast(dlg->FindWindow(1001), wxListBox);
        check(e && (e->GetWindowStyleFlag() & wxLB_EXTENDED) != 0,
              "LBS_EXTENDEDSEL still builds wxLB_EXTENDED");
        check(e && (e->GetWindowStyleFlag() & wxLB_MULTIPLE) == 0,
              "...and not the click-toggles model");
        check(s && (s->GetWindowStyleFlag() &
                    (wxLB_MULTIPLE | wxLB_EXTENDED)) == 0,
              "a list box with neither style is single-select");
        dlg->Destroy();
    }
}

// ── 3. A new world keeps Gammon's custom-colour palette ───────────────────
// Both world-creation hooks used to re-seed m_customtext[] before the document
// was loaded (from m_normalcolour[i % 8], black background), and File ▸ New
// World then painted all sixteen slots light grey.  The constructor already
// installed the canonical palette (SetDefaultCustomColours), which is what
// Windows shows and what the loader keeps when a world file has no <custom>
// section — so the port made custom colour 1 black-on-black on open and
// sixteen identical greys on new.
void TestNewWorldDefaults()
{
    printf("New/Open world colour defaults:\n");

    std::unique_ptr<CMUSHclientDoc> doc(new CMUSHclientDoc());
    wx_seed_new_world_defaults(doc.get());

    check(doc->m_customtext[0] == RGB(255, 128, 128),
          "custom text 1 is the default salmon, not black-on-black");
    check(doc->m_customtext[4] == RGB(0, 128, 255),
          "custom text 5 is the readable blue notes default to (m_iNoteTextColour=4)");
    check(doc->m_customtext[15] == RGB(0, 0, 255),
          "custom text 16 is the default blue");
    check(doc->m_customtext[0] != doc->m_customtext[1] &&
          doc->m_customtext[1] != doc->m_customtext[2],
          "the sixteen custom colours are still sixteen DIFFERENT colours");
    check(doc->m_customback[0] == RGB(0, 0, 0),
          "custom backgrounds stay black");
    check(doc->m_normalcolour[7] == RGB(192, 192, 192) &&
          doc->m_boldcolour[7]   == RGB(255, 255, 255),
          "the ANSI tables keep SetDefaultAnsiColours' values");
    check(doc->m_nWrapColumn == 80 && doc->m_bUTF_8 == 1 && doc->m_maxlines == 5000,
          "the port-only display defaults are still seeded");
}

} // namespace

int RunGuiBridgeSelfTest()
{
    printf("GUI-BRIDGE-SELFTEST: geometry, dialog controls, world defaults\n");

    TestGeometry();
    TestListBoxStyles();
    TestNewWorldDefaults();

    printf("GUI-BRIDGE-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#endif // !_WIN32
