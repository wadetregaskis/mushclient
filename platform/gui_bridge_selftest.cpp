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
#include <string>

// ── MUSHclient headers ────────────────────────────────────────────────────
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "resource.h"                   // IDD_SEND_TO_ALL, IDC_WORLD_LIST
#include "platform/wx_world_child.h"    // wx_save_child_geometry
#include "platform/wx_dialog_bridge.h"  // MfcDialogResources, BuildWxDialogFromTemplate
#include "platform/wx_mfc_hooks.h"      // g_wxhook_progress_* — the progress seam
#include "dialogs/ProgDlg.h"            // CProgressDlg

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

// ── 4. CProgressDlg drives the wx progress dialog ─────────────────────────
// CProgressDlg is the app's ONLY modeless dialog, and the port backs it with a
// wxProgressDialog rather than a bridged window (CDialog::Create is a `return
// FALSE` stub).  Three of its methods had no port arm at all and drove the
// inert CProgressCtrl / PumpMessages instead:
//
//   StepIt / OffsetPos  the relative movers.  xml_load_world advances the
//                       plugin-loading bar with SetStep(1) + StepIt(), and
//                       Lua's progress library offers both to scripts — so the
//                       bar sat at zero for the whole load.
//   SetWindowText       every caller sets the caption right after Create()
//                       ("Loading plugins ...", "Sending...", "Recalling...")
//                       and every one of them was dropped.
//
// Drive the real class against stub hooks and watch what it asks for.
void TestProgressDialog()
{
    printf("CProgressDlg -> wxProgressDialog:\n");

    static int  s_pos = -1;
    static int  s_max = -1;
    static std::string s_title, s_status;
    static bool s_open = false;
    static bool s_cancel = false;

    void* (*savedOpen)(const char*, const char*, int)  = g_wxhook_progress_open;
    void  (*savedRange)(void*, int)                    = g_wxhook_progress_set_range;
    bool  (*savedUpd)(void*, int, const char*)         = g_wxhook_progress_update;
    void  (*savedClose)(void*)                         = g_wxhook_progress_close;
    void  (*savedTitle)(void*, const char*)            = g_wxhook_progress_set_title;

    g_wxhook_progress_open = [](const char*, const char* status, int maximum) -> void* {
        s_open = true; s_max = maximum; s_status = status ? status : "";
        return (void*) 0x1;                       // an opaque non-null handle
    };
    g_wxhook_progress_set_range = [](void*, int maximum) { s_max = maximum; };
    g_wxhook_progress_update = [](void*, int pos, const char* status) -> bool {
        s_pos = pos; s_status = status ? status : "";
        return !s_cancel;
    };
    g_wxhook_progress_close  = [](void*) { s_open = false; };
    g_wxhook_progress_set_title = [](void*, const char* t) { s_title = t ? t : ""; };

    {
        CProgressDlg dlg;
        check(dlg.Create() != FALSE, "Create opens the wx progress dialog");
        check(s_open, "...and the open hook really ran");

        dlg.SetRange(0, 40);
        check(s_max == 40, "SetRange resizes it to the real amount of work");

        dlg.SetWindowText("Loading plugins ...");
        check(s_title == "Loading plugins ...",
              "SetWindowText reaches the dialog's caption");

        dlg.SetStatus("working");
        dlg.SetStep(1);
        dlg.StepIt();
        check(s_pos == 1, "StepIt advances by the step (it used to do nothing)");
        check(s_status == "working", "...carrying the status text with it");
        dlg.StepIt();
        dlg.StepIt();
        check(s_pos == 3, "...and keeps advancing");

        dlg.OffsetPos(10);
        check(s_pos == 13, "OffsetPos advances by its argument");

        dlg.SetPos(20);
        check(s_pos == 20, "SetPos is still absolute");
        dlg.StepIt();
        check(s_pos == 21, "...and StepIt continues from wherever SetPos left it");

        // Cancel: the hook says stop, and CheckCancelButton must report it.
        s_cancel = true;
        dlg.StepIt();
        check(dlg.CheckCancelButton() != FALSE,
              "a cancelled wx dialog is reported through CheckCancelButton");
        s_cancel = false;
    }
    check(!s_open, "the dialog is closed when CProgressDlg goes out of scope");

    g_wxhook_progress_open      = savedOpen;
    g_wxhook_progress_set_range = savedRange;
    g_wxhook_progress_update    = savedUpd;
    g_wxhook_progress_close     = savedClose;
    g_wxhook_progress_set_title = savedTitle;
}

} // namespace

int RunGuiBridgeSelfTest()
{
    printf("GUI-BRIDGE-SELFTEST: geometry, dialog controls, world defaults\n");

    TestGeometry();
    TestListBoxStyles();
    TestNewWorldDefaults();
    TestProgressDialog();

    printf("GUI-BRIDGE-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#endif // !_WIN32
