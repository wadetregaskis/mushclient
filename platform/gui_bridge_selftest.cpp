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
#include <wx/listctrl.h>                // wxListCtrl (SysListView32 view mode)
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
#include "platform/wx_mfc_logic.h"      // the decisions lifted out of the wx entry points
#include "platform/selftest_support.h"  // plugin-loading fixture (DebugHelper context)
#include "dialogs/ProgDlg.h"            // CProgressDlg
#include "dialogs/world_prefs/prefspropertypages.h"  // CPrefsP17 — ScriptBrowser
#include "dialogs/world_prefs/triggdlg.h"             // CTriggerDlg
#include "dialogs/global_prefs/GlobalPrefs.h"         // CGlobalPrefsP4 — printer font
#include "dialogs/DebugWorldInputDlg.h"                // CDebugWorldInputDlg — fixed-pitch font
#include "dialogs/plugins/PluginsDlg.h"                // CPluginsDlg — description liveness guard

#include <cctype>                       // isxdigit/islower — the GUID shape check

#include <vector>

// Defined in wx_mfc_impl.cpp: the geometry writer the quit hook and the MDI
// parent's close handler share, taking the config it writes into.
void SaveWindowGeometryTo(wxConfigBase& cfg, wxTopLevelWindow* frame);

// Defined in wx_mfc_impl.cpp: enter/leave full screen on a window, stashing
// (or discarding) its pre-full-screen rect -- the same bookkeeping the real
// View ▸ Full Screen menu handler does, exposed so this harness can drive it
// without a fully wired MDI parent + wxEVT_MENU dispatch.
void wx_toggle_full_screen(wxTopLevelWindow* target);

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
// construction from /geometry/child_w + /geometry/child_h (+ child_x +
// child_y), and the quit path used to write only the main_* keys, which are
// read back onto the MDI parent wxOSX never shows.  Result: ⌘Q lost the
// size, ⇧⌘W kept it.  The position half of the same bug (main_x/main_y
// landing on the same invisible parent, with no child_x/child_y at all)
// survived that fix and is what this now also guards.
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
    // A position distinct from (0,0) and from the parent's own origin: a fix
    // that quietly wrote the parent's position (or just left the field zero)
    // would still pass a test anchored at the origin.
    child->SetSize(123, 45, 640, 480);

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

        long x = 0, y = 0;
        const bool hadX = cfg.Read(wxT("/geometry/child_x"), &x);
        const bool hadY = cfg.Read(wxT("/geometry/child_y"), &y);
        check(hadX && hadY,
              "...and the position too (/geometry/child_x + child_y) -- "
              "the port bug this guards: only the parent-scoped main_x/main_y "
              "used to carry it, and macOS never shows the parent");
        check(x == 123 && y == 45,
              "...carrying the VISIBLE window's position, not the origin");

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
    // never-laid-out window cannot wipe a good saved size — or position.
    {
        wxRemoveFile(path);
        wxFileConfig cfg(wxEmptyString, wxEmptyString, path, wxEmptyString,
                         wxCONFIG_USE_LOCAL_FILE);
        wx_save_child_geometry(cfg, wxRect(123, 45, 0, 0));
        long w = 0, x = 0;
        check(!cfg.Read(wxT("/geometry/child_w"), &w),
              "a zero-size window is not persisted");
        check(!cfg.Read(wxT("/geometry/child_x"), &x),
              "...nor is its position, even though it was well-formed");
    }

    // R2-04-F5: wx_geometry_rect_for_save is the ONE choice both the quit
    // path (above) and CWorldChildFrame::OnClose (Cmd+W) go through, so the
    // two paths cannot disagree about a full-screen window's persisted rect.
    // A helper that just forwards GetRect() -- what OnClose used to do --
    // fails this: while full screen, GetRect() reports the display-sized
    // bounds, not the size the window had before going full screen.
    {
        wxFrame* fsFrame = new wxFrame(nullptr, wxID_ANY, wxT("selftest-fullscreen"),
                                       wxPoint(11, 22), wxSize(555, 333));
        fsFrame->Show();
        wx_toggle_full_screen(fsFrame);   // stashes 555x333 @ (11,22); enters full screen
        check(fsFrame->IsFullScreen(), "the frame reports full screen after toggling");

        const wxRect saved = wx_geometry_rect_for_save(fsFrame);
        check(saved.width == 555 && saved.height == 333,
              "wx_geometry_rect_for_save returns the PRE-full-screen size while "
              "still full screen -- a bare GetRect() would return the display "
              "size instead");
        check(saved.x == 11 && saved.y == 22,
              "...and the pre-full-screen position too");

        wxRemoveFile(path);
        wxFileConfig cfg(wxEmptyString, wxEmptyString, path, wxEmptyString,
                         wxCONFIG_USE_LOCAL_FILE);
        // The exact call OnClose now makes (Cmd+W path).
        wx_save_child_geometry(cfg, wx_geometry_rect_for_save(fsFrame));
        long w = 0, x = 0;
        cfg.Read(wxT("/geometry/child_w"), &w);
        cfg.Read(wxT("/geometry/child_x"), &x);
        check(w == 555 && x == 11,
              "...so the value that reaches OnClose's persisted keys is the "
              "pre-full-screen rect, not the full-screen one");

        wx_toggle_full_screen(fsFrame);   // leaves full screen; the stash is erased
        check(!fsFrame->IsFullScreen(), "the frame reports leaving full screen");
        fsFrame->Destroy();
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
//   StepIt / OffsetPos  the relative movers.  Lua's progress library offers
//                       both to scripts, and Find/Global Change drives them —
//                       so those bars sat at zero.  (NOT plugin loading: that
//                       block is inside #if IT_GET_GOATS_BLOOD_OUT, a macro
//                       defined nowhere, so it is dead on Windows too.)
//   SetWindowText       every caller sets the caption right after Create()
//                       ("Sending...", "Recalling...") and every one of them
//                       was dropped.
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

// ── 5. ::MessageBox MB_* decode ───────────────────────────────────────────
// hook_message_box gates every destructive confirmation in the app and the
// whole of scripted utils.msgbox, and until now had NO coverage at all: three
// selftests replace the hook with a stub of their own, and the real one ends in
// ShowModal, which cannot complete without an event loop.  Its own comments
// record three bugs that shipped in these lines — MB_OKCANCEL read as Yes/No,
// the two borrowed button sets collapsing so utils.msgbox could only ever
// return IDOK, and a dropped MB_DEFBUTTON2 that pointed "Delete the selected N
// items?" at Yes.  Table-test the decode, which is where all three lived.
void TestMessageBoxDecode()
{
    printf("MessageBox MB_* decode:\n");

    // ── the button set: the LOW NIBBLE, an enumeration, not bit flags ──────
    struct BtnCase {
        const char* what;
        unsigned    flags;
        long        wantOn;     // bits that must be set
        long        wantOff;    // bits that must NOT be set
        MbRelabel   wantRelabel;
    };
    static const BtnCase kBtn[] = {
        { "MB_OK is just OK",
          MB_OK,               wxOK,              wxCANCEL | wxYES_NO, eNoRelabel },
        // The first shipped bug: 1 & 0x0003 is non-zero, so a bitwise test read
        // MB_OKCANCEL as Yes/No and threw the Cancel button away.
        { "MB_OKCANCEL is OK+Cancel, NOT Yes/No",
          MB_OKCANCEL,         wxOK | wxCANCEL,   wxYES_NO,            eNoRelabel },
        { "MB_ABORTRETRYIGNORE borrows Yes/No/Cancel and relabels it",
          MB_ABORTRETRYIGNORE, wxYES_NO | wxCANCEL, 0,                 eAbortRetryIgnore },
        { "MB_YESNOCANCEL is Yes/No/Cancel, unrelabelled",
          MB_YESNOCANCEL,      wxYES_NO | wxCANCEL, 0,                 eNoRelabel },
        { "MB_YESNO has no Cancel",
          MB_YESNO,            wxYES_NO,          wxCANCEL,            eNoRelabel },
        { "MB_RETRYCANCEL borrows OK/Cancel and relabels it",
          MB_RETRYCANCEL,      wxOK | wxCANCEL,   wxYES_NO,            eRetryCancel },
        // 6 and 7 complete the nibble; Win32 has no such set, and a bitwise
        // decode would have found "Yes/No" in both.
        { "an unknown button set falls back to plain OK (6)",
          6u,                  wxOK,              wxCANCEL | wxYES_NO, eNoRelabel },
        { "an unknown button set falls back to plain OK (7)",
          7u,                  wxOK,              wxCANCEL | wxYES_NO, eNoRelabel },
    };
    for (const BtnCase& c : kBtn) {
        MbRelabel relabel = eRetryCancel;          // poisoned: it must be written
        const long style = MessageBoxStyleForMbFlags(c.flags, &relabel);
        check((style & c.wantOn) == c.wantOn && (style & c.wantOff) == 0, c.what);
        check(relabel == c.wantRelabel,
              (std::string(c.what) + " (relabel)").c_str());
    }

    // ── the icon: the 0xF0 nibble, decoded independently of the buttons ────
    struct IconCase { const char* what; unsigned flags; long want; };
    static const IconCase kIcon[] = {
        { "no icon requested, none set",   0u,                  0 },
        { "MB_ICONERROR",       (unsigned) MB_ICONERROR,        wxICON_ERROR },
        { "MB_ICONQUESTION",    (unsigned) MB_ICONQUESTION,     wxICON_QUESTION },
        { "MB_ICONEXCLAMATION", (unsigned) MB_ICONEXCLAMATION,  wxICON_WARNING },
        { "MB_ICONINFORMATION", (unsigned) MB_ICONINFORMATION,  wxICON_INFORMATION },
        { "an unknown icon nibble asks for no icon", 0x0050u,   0 },
    };
    const long kAllIcons = wxICON_ERROR | wxICON_QUESTION |
                           wxICON_WARNING | wxICON_INFORMATION;
    for (const IconCase& c : kIcon) {
        MbRelabel relabel = eNoRelabel;
        const long style = MessageBoxStyleForMbFlags(c.flags, &relabel);
        check((style & kAllIcons) == c.want,
              (std::string("icon: ") + c.what).c_str());
    }

    // ── the default button: a THIRD field, and the one that was dropped ────
    // Win32 numbers buttons in the order the SET declares them, so "the second
    // button" is a different button per set — for Yes/No/Cancel it is No, not
    // Cancel.  wx can only express the two negative defaults it names.
    struct DefCase { const char* what; unsigned flags; bool wantNo; bool wantCancel; };
    static const DefCase kDef[] = {
        { "MB_DEFBUTTON1 asks for nothing (wx already defaults to the first)",
          (unsigned)(MB_YESNO | MB_DEFBUTTON1),             false, false },
        { "no default-button field at all",
          (unsigned) MB_YESNO,                              false, false },
        // The third shipped bug, and the reason it mattered: this exact
        // combination is the six delete confirmations.
        { "MB_YESNO + DEFBUTTON2 defaults to No (the delete confirmations)",
          (unsigned)(MB_YESNO | MB_DEFBUTTON2),             true,  false },
        { "MB_YESNOCANCEL + DEFBUTTON2 defaults to No, NOT Cancel",
          (unsigned)(MB_YESNOCANCEL | MB_DEFBUTTON2),       true,  false },
        { "MB_YESNOCANCEL + DEFBUTTON3 defaults to Cancel",
          (unsigned)(MB_YESNOCANCEL | MB_DEFBUTTON3),       false, true  },
        { "MB_OKCANCEL + DEFBUTTON2 defaults to Cancel",
          (unsigned)(MB_OKCANCEL | MB_DEFBUTTON2),          false, true  },
        // Sets with no such button: Win32 falls back to the first, which is
        // already wx's default, so nothing may be added.
        { "MB_YESNO + DEFBUTTON3 has no third button to default to",
          (unsigned)(MB_YESNO | MB_DEFBUTTON3),             false, false },
        { "MB_OKCANCEL + DEFBUTTON3 has no third button either",
          (unsigned)(MB_OKCANCEL | MB_DEFBUTTON3),          false, false },
        { "MB_OK + DEFBUTTON2 has only one button",
          (unsigned)(MB_OK | MB_DEFBUTTON2),                false, false },
        // MB_DEFBUTTON4 is 0x0300 — the two DEFBUTTON bits TOGETHER, because
        // this is a field and not a set of flags.  No MB_* set has a fourth
        // button, so Win32 falls back to the first.  A `defBtn & 0x0100` test
        // would read it as DEFBUTTON2 and quietly point these at the wrong
        // button; only a value that sets both bits can tell the two apart.
        { "MB_DEFBUTTON4 (0x0300) names a button no set has",
          (unsigned)(MB_YESNOCANCEL | 0x0300u),             false, false },
        { "MB_DEFBUTTON4 on OK/Cancel likewise",
          (unsigned)(MB_OKCANCEL | 0x0300u),                false, false },
    };
    for (const DefCase& c : kDef) {
        MbRelabel relabel = eNoRelabel;
        const long style = MessageBoxStyleForMbFlags(c.flags, &relabel);
        check(((style & wxNO_DEFAULT) != 0)     == c.wantNo &&
              ((style & wxCANCEL_DEFAULT) != 0) == c.wantCancel,
              (std::string("default button: ") + c.what).c_str());
    }

    // ── the three fields are independent ──────────────────────────────────
    // The whole point of decoding them as fields: setting one must not disturb
    // the others.  "Delete the selected N items?" is exactly this combination.
    {
        MbRelabel relabel = eAbortRetryIgnore;
        const long style = MessageBoxStyleForMbFlags(
            (unsigned)(MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2), &relabel);
        check((style & wxYES_NO) == wxYES_NO &&
              (style & wxCANCEL) == 0 &&
              (style & kAllIcons) == wxICON_QUESTION &&
              (style & wxNO_DEFAULT) != 0 &&
              relabel == eNoRelabel,
              "buttons, icon and default button decode independently");
    }

    // ── the result remap, in both directions ──────────────────────────────
    check(MbResultForWxId(eNoRelabel, wxID_OK)     == IDOK,     "plain: wxID_OK -> IDOK");
    check(MbResultForWxId(eNoRelabel, wxID_CANCEL) == IDCANCEL, "plain: wxID_CANCEL -> IDCANCEL");
    check(MbResultForWxId(eNoRelabel, wxID_YES)    == IDYES,    "plain: wxID_YES -> IDYES");
    check(MbResultForWxId(eNoRelabel, wxID_NO)     == IDNO,     "plain: wxID_NO -> IDNO");

    check(MbResultForWxId(eAbortRetryIgnore, wxID_YES)    == IDABORT,
          "abort/retry/ignore: the first button is Abort");
    check(MbResultForWxId(eAbortRetryIgnore, wxID_NO)     == IDRETRY,
          "abort/retry/ignore: the second is Retry");
    check(MbResultForWxId(eAbortRetryIgnore, wxID_CANCEL) == IDIGNORE,
          "abort/retry/ignore: the third is Ignore");
    // A dismissed dialog has to pick something; Ignore is the no-op arm.
    check(MbResultForWxId(eAbortRetryIgnore, wxID_NONE)   == IDIGNORE,
          "abort/retry/ignore: dismissing means Ignore");

    check(MbResultForWxId(eRetryCancel, wxID_OK)     == IDRETRY,
          "retry/cancel: the borrowed OK is Retry");
    check(MbResultForWxId(eRetryCancel, wxID_CANCEL) == IDCANCEL,
          "retry/cancel: Cancel is Cancel");
    check(MbResultForWxId(eRetryCancel, wxID_NONE)   == IDCANCEL,
          "retry/cancel: dismissing means Cancel");

    // The bug that made a script's abort/retry/ignore branch unreachable was
    // precisely that all three answers collapsed onto ONE code.  Assert the
    // distinctness, not just the individual rows, so no future remap can
    // reintroduce it row-by-row.
    {
        const int a = MbResultForWxId(eAbortRetryIgnore, wxID_YES);
        const int r = MbResultForWxId(eAbortRetryIgnore, wxID_NO);
        const int i = MbResultForWxId(eAbortRetryIgnore, wxID_CANCEL);
        check(a != r && r != i && a != i,
              "abort, retry and ignore are three DISTINCT return codes");
        check(MbResultForWxId(eRetryCancel, wxID_OK) !=
              MbResultForWxId(eRetryCancel, wxID_CANCEL),
              "retry and cancel are two distinct return codes");
        // ...and the relabelled sets must not answer with the plain codes,
        // which is what a caller would see if the relabel were forgotten.
        check(a != IDOK && r != IDOK,
              "a relabelled set never answers IDOK (it did when the relabel was lost)");
    }
}

// ── 6. dialog CHAR_HOOK key translation ───────────────────────────────────
// Six canonical PreTranslateMessage overrides hang off this: the tab-inserts-tab
// preference in CEditMultiLine, the <enter> swallow in the two Lua input boxes,
// and Alt+Enter opening the multi-line "send" editor in the trigger, alias and
// timer dialogs.  The hook itself is delivered by the event loop, which no
// selftest has, so test the translation.
void TestDialogKeyTranslation()
{
    printf("Dialog CHAR_HOOK key translation:\n");

    MSG msg{};
    msg.message = 0xDEAD; msg.wParam = 0xBEEF;

    check(DialogKeyToMsg(WXK_TAB, false, &msg) &&
          msg.message == WM_KEYDOWN && msg.wParam == VK_TAB,
          "TAB becomes WM_KEYDOWN/VK_TAB");

    msg = MSG{};
    check(DialogKeyToMsg(WXK_RETURN, false, &msg) &&
          msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN,
          "RETURN becomes WM_KEYDOWN/VK_RETURN");

    msg = MSG{};
    check(DialogKeyToMsg(WXK_NUMPAD_ENTER, false, &msg) &&
          msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN,
          "the keypad's Enter is the same key to MFC");

    // The distinguishing case: MFC sees Alt+Return as WM_SYSKEYDOWN, and that
    // is the ONLY thing separating "open the multi-line editor" from "press the
    // default button" in CTriggerDlg / CAliasDlg / CTimerDlg.
    msg = MSG{};
    check(DialogKeyToMsg(WXK_RETURN, true, &msg) &&
          msg.message == WM_SYSKEYDOWN && msg.wParam == VK_RETURN,
          "Alt+RETURN becomes WM_SYSKEYDOWN (the multi-line editor chord)");
    msg = MSG{};
    check(DialogKeyToMsg(WXK_NUMPAD_ENTER, true, &msg) &&
          msg.message == WM_SYSKEYDOWN,
          "...and so does Alt+keypad-Enter");

    // Alt does NOT promote Tab: nothing overrides Alt+Tab, and the window
    // manager owns it anyway.
    msg = MSG{};
    check(DialogKeyToMsg(WXK_TAB, true, &msg) && msg.message == WM_KEYDOWN,
          "Alt+TAB is still a plain WM_KEYDOWN");

    // Everything else must be refused, so the caller Skip()s it and wx's own
    // Esc→Cancel and Enter→default-button keep working in every dialog.  A
    // blanket hook here would break all of them.
    const int kPassThrough[] = {
        WXK_ESCAPE, WXK_SPACE, WXK_BACK, WXK_DELETE, WXK_LEFT, WXK_UP,
        WXK_F1, WXK_HOME, WXK_PAGEUP, WXK_NUMPAD_ADD, 'a', 'Z', '0', '\'',
    };
    bool anyTranslated = false;
    for (int k : kPassThrough) {
        MSG m{};
        m.message = 0x1234;
        if (DialogKeyToMsg(k, false, &m) || DialogKeyToMsg(k, true, &m))
            anyTranslated = true;
        if (m.message != 0x1234) anyTranslated = true;   // and it left *msg alone
    }
    check(!anyTranslated,
          "every other key is refused untouched (Esc, arrows, letters, F-keys)");

    check(!DialogKeyToMsg(WXK_TAB, false, nullptr),
          "a null MSG is refused rather than written through");
}

// ── 7. CMenu -> wxMenu (world.WindowMenu) ─────────────────────────────────
// The translation behind CMiniWindow::Menu.  Showing the popup needs an event
// loop; BUILDING it does not, so build one and read it back.
void TestPopupMenuBuild()
{
    printf("CMenu -> wxMenu (world.WindowMenu):\n");

    CMenu::ResetArena();

    // A menu shaped like the ones plugins actually produce (miniwindow.cpp
    // :3630-3675): plain items, a separator, a ticked item, a greyed
    // placeholder with NO command id, a labelled submenu — plus the unlabelled
    // submenu the port has to invent a rule for.
    CMenu top;
    top.CreatePopupMenu();
    top.AppendMenu(MF_STRING, 5001, "Look");
    top.AppendMenu(MF_SEPARATOR, 0, "");
    top.AppendMenu(MF_STRING | MF_CHECKED, 5002, "Sound on");
    top.AppendMenu(MF_STRING | MF_GRAYED,  0,    "-- not available --");

    CMenu* named = CMenu::NewPopup();
    named->AppendMenu(MF_STRING, 5010, "North");
    named->AppendMenu(MF_STRING, 5011, "South");
    top.AppendMenu(MF_POPUP | MF_ENABLED, (UINT_PTR) named->m_hMenu, "Go");

    // An UNLABELLED popup — what CMenu::GetSubMenu materialises, because the
    // port has no menu-resource loader to supply the .rc title.  Its items get
    // inlined behind a separator rather than shown as a nameless submenu.
    CMenu* unnamed = CMenu::NewPopup();
    unnamed->AppendMenu(MF_STRING, 5020, "Inlined A");
    unnamed->AppendMenu(MF_STRING, 5021, "Inlined B");
    top.AppendMenu(MF_POPUP, (UINT_PTR) unnamed->m_hMenu, "");

    std::unique_ptr<wxMenu> m(wx_build_popup_menu(&top));
    check(m != nullptr, "the CMenu tree renders as a wxMenu");
    if (!m) return;

    // 4 top-level entries + the "Go" submenu + a separator + 2 inlined items.
    check(m->GetMenuItemCount() == 8,
          "every item is carried over (with the unlabelled popup inlined)");

    const wxMenuItem* look = m->FindItem(5001);
    check(look != nullptr && look->GetItemLabelText() == wxT("Look"),
          "a plain item keeps its id and its label");

    const wxMenuItem* sound = m->FindItem(5002);
    check(sound != nullptr && sound->IsCheckable() && sound->IsChecked(),
          "MF_CHECKED produces a checkable item that IS checked");
    check(look != nullptr && !look->IsCheckable(),
          "...and an unticked item is not even checkable");

    // THE guard worth having: a greyed placeholder has arg == 0, and 0 is what
    // TrackPopupMenu returns for "the user cancelled".  Give it id 0 and a
    // caller cannot tell the two apart.
    {
        const wxMenuItem* grey = nullptr;
        for (const wxMenuItem* mi : m->GetMenuItems())
            if (mi->GetItemLabelText() == wxT("-- not available --")) grey = mi;
        check(grey != nullptr, "the greyed placeholder is present");
        if (grey) {
            check(!grey->IsEnabled(), "...and is disabled");
            check(grey->GetId() != 0,
                  "...and does NOT get id 0, which means \"cancelled\"");
        }
    }

    {
        const wxMenuItem* go = nullptr;
        for (const wxMenuItem* mi : m->GetMenuItems())
            if (mi->GetItemLabelText() == wxT("Go")) go = mi;
        check(go != nullptr && go->IsSubMenu(), "a labelled popup stays a submenu");
        if (go && go->GetSubMenu()) {
            check(go->GetSubMenu()->GetMenuItemCount() == 2,
                  "...carrying its children");
            check(go->GetSubMenu()->FindItem(5011) != nullptr,
                  "...with their ids intact");
        }
    }

    // The unlabelled popup's items are inlined into the PARENT, not hidden
    // behind a nameless submenu the user cannot read.
    check(m->FindItem(5020) != nullptr && m->FindItem(5021) != nullptr,
          "an unlabelled popup's items are inlined into the parent menu");
    {
        int separators = 0;
        for (const wxMenuItem* mi : m->GetMenuItems())
            if (mi->IsSeparator()) separators++;
        check(separators == 2,
              "...behind a separator of their own, plus the explicit one");
    }

    CMenu::ResetArena();
}

// ── 8. recent-worlds MRU ──────────────────────────────────────────────────
// File ▸ Open Recent.  The list transform is pure; the persistence is tested
// against a scratch config file, never the user's real preferences.
void TestRecentWorlds()
{
    printf("Recent-worlds MRU:\n");

    // ── the pure transform ────────────────────────────────────────────────
    {
        std::vector<std::string> l;
        RecentWorldsPromote(l, "/w/a.mcl", 3);
        RecentWorldsPromote(l, "/w/b.mcl", 3);
        check(l.size() == 2 && l[0] == "/w/b.mcl" && l[1] == "/w/a.mcl",
              "the newest world goes to the front");

        RecentWorldsPromote(l, "/w/a.mcl", 3);
        check(l.size() == 2 && l[0] == "/w/a.mcl" && l[1] == "/w/b.mcl",
              "re-opening a world moves it to the front, it does not duplicate");

        RecentWorldsPromote(l, "/w/c.mcl", 3);
        RecentWorldsPromote(l, "/w/d.mcl", 3);
        check(l.size() == 3, "the list is capped");
        check(l[0] == "/w/d.mcl" && l[1] == "/w/c.mcl" && l[2] == "/w/a.mcl",
              "...and it is the OLDEST entry that falls off the end");

        RecentWorldsPromote(l, "", 3);
        check(l.size() == 3 && l[0] == "/w/d.mcl",
              "an empty path is ignored rather than pushed");

        // Two worlds of the same basename in different directories are two
        // different worlds; the menu shows only the basename, so a dedup on
        // the wrong key would silently lose one.
        std::vector<std::string> two;
        RecentWorldsPromote(two, "/one/w.mcl", 5);
        RecentWorldsPromote(two, "/two/w.mcl", 5);
        check(two.size() == 2, "paths are deduped whole, not by basename");
    }

    // ── the config round-trip, on a scratch file ──────────────────────────
    {
        wxFileName fn(wxStandardPaths::Get().GetTempDir(),
                      wxT("mushclient-selftest-recent.ini"));
        if (fn.FileExists()) wxRemoveFile(fn.GetFullPath());
        wxFileConfig cfg(wxEmptyString, wxEmptyString, fn.GetFullPath(),
                         wxEmptyString, wxCONFIG_USE_LOCAL_FILE);

        const std::vector<std::string> five = {
            "/w/1.mcl", "/w/2.mcl", "/w/3.mcl", "/w/4.mcl", "/w/5.mcl" };
        RecentWorldsSaveTo(cfg, five, 10);

        std::vector<std::string> back;
        RecentWorldsLoadFrom(cfg, back, 10);
        check(back == five, "the list round-trips through the config in order");

        // THE invariant: the load stops at the first gap, so a shorter list
        // written over a longer one MUST erase the tail.  Without the
        // DeleteEntry the next load resurrects worlds the user removed.
        const std::vector<std::string> two = { "/w/9.mcl", "/w/8.mcl" };
        RecentWorldsSaveTo(cfg, two, 10);
        RecentWorldsLoadFrom(cfg, back, 10);
        check(back == two,
              "a shorter list erases the old tail (the load stops at a gap)");

        // The load is also capped, so a config hand-edited past the cap cannot
        // grow the menu beyond the id block reserved for it.
        for (int i = 0; i < 12; ++i)
            cfg.Write(wxString::Format(wxT("/recent/%d"), i),
                      wxString::Format(wxT("/w/%d.mcl"), i));
        RecentWorldsLoadFrom(cfg, back, 10);
        check((int) back.size() == 10, "the load honours the cap");

        // An empty value is a gap too — that is what makes DeleteEntry and
        // "never store an empty path" the same invariant.
        cfg.Write(wxT("/recent/3"), wxT(""));
        RecentWorldsLoadFrom(cfg, back, 10);
        check(back.size() == 3, "an empty entry ends the list");

        wxRemoveFile(fn.GetFullPath());
    }
}

// ── 9. the system clipboard round-trips ───────────────────────────────────
// putontoclipboard / GetClipboardContents are the seam behind world.GetClipboard,
// world.SetClipboard, Edit ▸ Convert Clipboard Forum Codes, the
// colour-from-clipboard picker and IsClipboardXML.  Reading was broken for all
// of them: hook_get_clipboard_text built its UTF-8 buffer from the TEMPORARY
// wxString returned by wxTextDataObject::GetText(), and wxString::ToUTF8() can
// return a buffer pointing into that string's own storage, so the bytes were
// freed at the end of the statement while the buffer's length field survived.
// Every read came back empty.  The forum-codes command then wrote that empty
// string BACK, silently destroying whatever the user had copied.
//
// This has to go through the REAL clipboard: the defect is entirely in the wx
// interaction, so a test over the string handling alone would have stayed green.
// It therefore saves and restores whatever the user had there — a test is not
// entitled to take someone's clipboard, and this one would otherwise take it
// every ctest run.
void TestClipboardRoundTrip()
{
    printf("System clipboard round-trip:\n");

    CString saved;
    const bool hadSaved = GetClipboardContents(saved, false, false) ? true : false;

    // Saving and restoring only covers what can be READ as text.  An empty read
    // does not mean an empty clipboard: a copied image or file reference reads
    // as nothing here, and putontoclipboard replaces the whole pasteboard, so
    // running the round trip would destroy it with no way to put it back.  A
    // test is not entitled to do that to whoever typed `ctest`.
    if ((!hadSaved || saved.IsEmpty()) &&
        g_wxhook_clipboard_has_nontext && g_wxhook_clipboard_has_nontext())
    {
        printf("  [SKIP] clipboard holds non-text content - not overwriting it\n");
        return;
    }

    // Deliberately contains the characters the forum-codes path counts, so a
    // silent truncation shows up as a wrong length rather than a lucky match.
    const char* const kProbe = "HELLO[42]WORLD\\end";

    putontoclipboard(CString(kProbe), false);

    CString got;
    const BOOL ok = GetClipboardContents(got, false, false);
    check(ok != FALSE, "GetClipboardContents reports success after a write");
    check(got.GetLength() == (int) strlen(kProbe),
          "...and returns the full length, not a truncated or empty string");
    check(strcmp((const char*) got, kProbe) == 0,
          "...and the bytes survive the round trip exactly");

    // The control: a DIFFERENT value must read back differently, so the test
    // cannot be satisfied by a stale read of whatever was there before.
    putontoclipboard(CString("second value"), false);
    CString got2;
    GetClipboardContents(got2, false, false);
    check(strcmp((const char*) got2, "second value") == 0,
          "...and a second write is what the next read sees");

    if (hadSaved && !saved.IsEmpty())
        putontoclipboard(saved, false);
    else
        putontoclipboard(CString(""), false);
}

// ── 10. Game ▸ Functions List: the view mode, and reading a row back ───────
// Two defects, one dialog, both of which made it useless.
//
// The bridge hardcoded wxLC_REPORT for every SysListView32.  Fourteen of the
// fifteen recovered list controls do ask for LVS_REPORT — but IDC_FUNCTIONS_LIST
// asks for LVS_LIST, which has no columns and whose owner never calls
// InsertColumn.  In report mode with zero columns every InsertItem still
// succeeded and returned a row index, and not one of ~400 function names was
// visible because there was nowhere to draw them.
//
// And CListCtrl::GetItemText was the one accessor in the shim with no hook
// behind it, hardcoded `return {}`.  Its only caller is this dialog's Copy
// button, so Copy put an empty string on the clipboard, over whatever the user
// had there.
void TestFunctionListView()
{
    printf("Functions list (view mode + item text):\n");

    const RcDialog* t = MfcDialogResources().FindDialog(IDD_FUNCTION_LIST);
    check(t != nullptr, "IDD_FUNCTION_LIST is in the recovered resources");
    if (t)
    {
        wxDialog* dlg = BuildWxDialogFromTemplate(nullptr, *t);
        wxListCtrl* lv = wxDynamicCast(dlg->FindWindow(IDC_FUNCTIONS_LIST), wxListCtrl);
        check(lv != nullptr, "its function list is built as a wxListCtrl");
        if (lv)
        {
            const long st = lv->GetWindowStyleFlag();
            check((st & wxLC_LIST) != 0,
                  "LVS_LIST builds wxLC_LIST (a column-less list)");
            check((st & wxLC_REPORT) == 0,
                  "...and NOT wxLC_REPORT, which here would have no columns at all");

            // End to end: a row inserted the way ReloadList does it is readable
            // back through the CListCtrl shim — which is the Copy button's source.
            lv->InsertItem(0, wxT("world.Note"));
            CListCtrl lc;
            lc.m_hWnd = (HWND) lv;
            check(lc.GetItemCount() == 1, "an inserted row is counted");
            check(lc.GetItemText(0, 0) == CString("world.Note"),
                  "...and GetItemText reads it back (what Copy puts on the clipboard)");
            lc.m_hWnd = nullptr;
        }
        dlg->Destroy();
    }

    // The control, and the reason this is safe: a template that asks for
    // LVS_REPORT still gets report mode, so honouring the view bits did not
    // quietly change the fourteen list controls that DO want columns.
    {
        RcDialog synth;
        synth.id = 1; synth.cx = 200; synth.cy = 100;
        synth.caption = "selftest";
        RcControl rep;
        rep.className = "SysListView32"; rep.id = 1000;
        rep.style = 0x50810009;    // the style every entity-manager list uses
        rep.x = 5; rep.y = 5; rep.cx = 150; rep.cy = 60;
        synth.controls.push_back(rep);

        wxDialog* d2 = BuildWxDialogFromTemplate(nullptr, synth);
        wxListCtrl* r = wxDynamicCast(d2->FindWindow(1000), wxListCtrl);
        check(r && (r->GetWindowStyleFlag() & wxLC_REPORT) != 0,
              "LVS_REPORT still builds wxLC_REPORT (the entity managers)");
        check(r && (r->GetWindowStyleFlag() & wxLC_LIST) == 0,
              "...and does not pick up list mode");
        d2->Destroy();
    }
}

} // namespace

// ── 11. DDX text hooks keep legacy-charset bytes (2026-08-12) ──────────────
// hook_set_window_text decoded with bare wxString::FromUTF8, which is
// all-or-nothing: a doc CString holding CP1252 bytes ("Caf\xE9") rendered as
// an EMPTY edit field in every bridged dialog, and DDX's save arm then wrote
// the blank back over the member on OK — silent data destruction for any
// unvalidated free-text field.  The trigger/alias list rows blanked the same
// way through the listctrl hooks.  Now DecodeTextLenient falls back to
// Latin-1, so the text stays visible and round-trips (re-encoded as UTF-8).
void TestDdxCharsetRoundTrip()
{
    printf("DDX charset round-trip (legacy bytes):\n");

    wxFrame* host = new wxFrame(nullptr, wxID_ANY, wxT("ddx-host"));
    wxTextCtrl* tc = new wxTextCtrl(host, wxID_ANY);

    // The exact seam DDX load uses: CWnd::SetWindowText → this hook.
    check(g_wxhook_set_window_text != nullptr, "set_window_text hook installed");
    g_wxhook_set_window_text((void*) tc, "Caf\xE9 monde");  // é in Latin-1: invalid UTF-8
    const wxString shown = tc->GetValue();
    check(!shown.empty(),
          "a legacy-charset value still renders (was: blank field)");
    check(shown.Contains(wxT("Caf")) && shown.Contains(wxT("monde")),
          "...with its ASCII text intact");
    check(shown.length() == 10,
          "...one character per byte (the Latin-1 reading)");

    // The save arm reads it back — the data-destruction half of the bug.
    char* back = g_wxhook_get_window_text((void*) tc);
    check(back && *back,
          "reading the field back is non-empty (was: member erased on OK)");
    check(back && strcmp(back, "Caf\xC3\xA9 monde") == 0,
          "...as UTF-8 re-encoded text (the e-acute survives as a character)");
    free(back);

    // The common case must stay exact: valid UTF-8 round-trips untouched.
    g_wxhook_set_window_text((void*) tc, "Caf\xC3\xA9");
    char* back2 = g_wxhook_get_window_text((void*) tc);
    check(back2 && strcmp(back2, "Caf\xC3\xA9") == 0,
          "valid UTF-8 round-trips byte-identical");
    free(back2);

    // And the list rows.
    wxListCtrl* lv = new wxListCtrl(host, wxID_ANY, wxDefaultPosition,
                                    wxDefaultSize, wxLC_REPORT);
    lv->InsertColumn(0, wxT("match"));
    const int row = g_wxhook_listctrl_insert_item((void*) lv, 0, "Vous \xEAtes");
    check(row == 0, "listctrl insert accepts the row");
    check(!lv->GetItemText(0).empty(),
          "a legacy-charset list row shows its text (was: blank row)");

    host->Destroy();
}

// ── 11b. CEdit offsets are BYTES, not characters (2026-08-18) ──────────────
// The CEdit hooks answered in wxWidgets CHARACTER positions, but every
// canonical caller uses the same numbers to index the UTF-8 CString it got
// from GetWindowText: FindMatchingBrace (Utilities.cpp:579) slices strBuf with
// them, EditMultiLine's Go-To-Line splices at LineIndex, and the Notes page's
// Find (prefspropertypages.cpp:5351) scans from GetSel.  One non-ASCII
// character before the caret shifted every offset after it, so brace matching
// selected the wrong span and Go-To-Line jumped into the middle of a character.
//
// The fixture has to make the two spaces DIFFER — a monospace/ASCII string
// would make the conversion an identity and nothing could discriminate.
void TestEditOffsetsAreBytes()
{
    printf("CEdit offsets (UTF-8 bytes vs wx characters):\n");

    wxFrame* host = new wxFrame(nullptr, wxID_ANY, wxT("edit-host"));

    // "héllo wörld": 11 characters, 13 bytes.  "wörld" is characters 6..11,
    // which is bytes 7..13 — every number below differs between the two.
    {
        wxTextCtrl* tc = new wxTextCtrl(host, wxID_ANY);
        tc->SetValue(wxString::FromUTF8("h\xC3\xA9llo w\xC3\xB6rld"));

        CEdit ed;
        ed.m_hWnd = (HWND) tc;

        check(ed.GetWindowTextLength() == 13,
              "GetWindowTextLength counts bytes, not characters (13, not 11)");

        // The caller's own text, in the space it indexes.
        char* raw = g_wxhook_get_window_text((void*) tc);
        CString text(raw ? raw : "");
        free(raw);
        check(text.GetLength() == ed.GetWindowTextLength(),
              "...which is exactly the length of the CString GetWindowText returns");

        tc->SetSelection(6, 11);              // wx ground truth: "wörld"
        int s = 0, e = 0;
        ed.GetSel(s, e);
        check(s == 7 && e == 13,
              "GetSel reports the byte span of the selection (7..13, not 6..11)");
        check(text.Mid(s, e - s) == CString("w\xC3\xB6rld"),
              "...so slicing the CString with them yields exactly the selected word");

        // And back the other way: byte arguments must land on the same word.
        ed.SetSel(7, 13);
        long from = 0, to = 0;
        tc->GetSelection(&from, &to);
        check(from == 6 && to == 11,
              "SetSel maps byte arguments back onto the wx character range");
    }

    // The line accessors carry the same numbers.  Three lines of "àà": each is
    // 2 characters but 4 bytes, so the line starts diverge immediately.
    // Characters: 0-1 àà, 2 \n, 3-4 àà, 5 \n, 6-7 àà   (last position 8)
    // Bytes:      0-3 àà, 4 \n, 5-8 àà, 9 \n, 10-13 àà (length 14)
    {
        wxTextCtrl* tc = new wxTextCtrl(host, wxID_ANY, wxEmptyString,
                                        wxDefaultPosition, wxDefaultSize,
                                        wxTE_MULTILINE | wxTE_DONTWRAP);
        tc->SetValue(wxString::FromUTF8(
            "\xC3\xA0\xC3\xA0\n\xC3\xA0\xC3\xA0\n\xC3\xA0\xC3\xA0"));

        CEdit ed;
        ed.m_hWnd = (HWND) tc;

        check(ed.GetLineCount() == 3, "the line count is unaffected (the control)");
        check(ed.GetWindowTextLength() == 14, "the total length is 14 bytes, not 8");
        check(ed.LineIndex(1) == 5,  "LineIndex(1) is the line's BYTE start (5, not 3)");
        check(ed.LineIndex(2) == 10, "LineIndex(2) likewise (10, not 6)");
        check(ed.LineLength(5) == 4,
              "LineLength takes a byte offset and answers in bytes (4, not 2)");
        // Byte 8 is the second à of line 1; read as a character it would be the
        // end of the text, i.e. line 2 — so this one crosses a line boundary.
        check(ed.LineFromChar(8) == 1,
              "LineFromChar reads its argument as a byte offset (line 1, not 2)");
    }

    host->Destroy();
}

// ── 12. Scripting page: Browse speaks the port's languages (2026-08-12) ────
// CPrefsP17::ScriptBrowser's Python/Tcl/Ruby cases were behind USE_PYTHON/
// USE_TCL/USE_RUBY, which only stdafx.h's _WIN32 arm defines — so with the
// world's language set to any of the three (fully supported here), the
// Browse and New-script buttons fell into `default: return FALSE`: a silent
// no-op.  The gates now admit the port; this drives the real ScriptBrowser
// with the file-dialog hook stubbed to capture the filter it offers.
void TestScriptingPageBrowse()
{
    printf("Scripting page Browse (port languages):\n");

    static std::string s_filter;
    static int s_calls = 0;
    char* (*saved)(bool, const char*, const char*, const char*, const char*,
                   bool) = g_wxhook_file_dialog;
    g_wxhook_file_dialog = [](bool, const char*, const char*, const char*,
                              const char* filter, bool) -> char* {
        s_filter = filter ? filter : "";
        ++s_calls;
        return nullptr;                     // cancel — no file touched
    };

    wxFrame* host = new wxFrame(nullptr, wxID_ANY, wxT("p17-host"));
    wxComboBox* cb = new wxComboBox(host, wxID_ANY);
    static const char* const kLangs[] = { "VBscript", "JScript", "PerlScript",
        "Python", "TclScript", "Lua", "PhpScript", "RubyScript" };
    for (const char* l : kLangs) cb->Append(wxString(l));

    struct Probe : CPrefsP17 { using CPrefsP17::ScriptBrowser; };
    Probe page;
    page.m_ctlLanguage.m_hWnd = cb;         // the real hook route: toWx(m_hWnd)

    struct Case { int idx; const char* pat; const char* what; };
    static const Case kCases[] = {
        { 3, "*.pys", "Python browse offers *.pys (was: silent no-op)" },
        { 4, "*.tcl", "Tcl browse offers *.tcl (was: silent no-op)" },
        { 7, "*.rb",  "Ruby browse offers *.rb (was: silent no-op)" },
        { 5, "*.lua", "Lua browse still offers *.lua (control)" },
    };
    for (const Case& c : kCases) {
        cb->SetSelection(c.idx);
        s_calls = 0; s_filter.clear();
        const BOOL ran = page.ScriptBrowser(TRUE);
        check(!ran && s_calls == 1 && s_filter.find(c.pat) != std::string::npos,
              c.what);
    }
    // PHP stays refused on the port (unsupported language, by design).
    cb->SetSelection(6);
    s_calls = 0;
    check(page.ScriptBrowser(TRUE) == FALSE && s_calls == 0,
          "PHP browse stays a refusal (unsupported on the port)");

    page.m_ctlLanguage.m_hWnd = nullptr;    // not ours to destroy
    g_wxhook_file_dialog = saved;
    host->Destroy();
}

// ── 13. CListBox mutators really mutate (2026-08-12) ───────────────────────
// DeleteString / InsertString / FindStringExact were hard LB_ERR stubs with
// no wx hook — so Global Prefs' Remove button did nothing, Move Up/Down only
// CLEARED the selection (InsertString's LB_ERR fed SetCurSel(-1)), and the
// add-time duplicate check always answered "not found".  With that page the
// ONLY writer of the startup world list, there was no UI way to stop a world
// auto-opening at launch.
void TestListBoxMutators()
{
    printf("CListBox mutators (Global Prefs Remove / Move / dedupe):\n");

    wxFrame* host = new wxFrame(nullptr, wxID_ANY, wxT("lbx-host"));
    wxListBox* lb = new wxListBox(host, wxID_ANY);
    CListBox box;
    box.m_hWnd = lb;

    box.AddString("alpha");
    box.AddString("bravo");
    box.AddString("charlie");

    // Delete: the COUNT decrease is what makes "did nothing" fail.
    check(box.DeleteString(1) != LB_ERR, "DeleteString returns success");
    check(box.GetCount() == 2, "...and the list really shrank");
    CString at1; box.GetText(1, at1);
    check(at1 == "charlie", "...with the right survivor at index 1");

    // Insert at 0 (the Move Up shape): lands at 0 and returns 0.
    check(box.InsertString(0, "bravo") == 0, "InsertString returns the index");
    CString at0; box.GetText(0, at0);
    check(at0 == "bravo" && box.GetCount() == 3,
          "...and the item is really there");

    // Find: exact, case-insensitive, absent -> LB_ERR (the dedupe check).
    check(box.FindStringExact(-1, "alpha") != LB_ERR, "FindStringExact finds");
    check(box.FindStringExact(-1, "ALPHA") != LB_ERR, "...case-insensitively");
    check(box.FindStringExact(-1, "zz") == LB_ERR, "...and misses honestly");

    // The whole Move Up sequence, as GlobalPrefs.cpp runs it: must end with
    // the item one row up AND STILL SELECTED (it used to end deselected).
    box.SetCurSel(2);   // "charlie"
    {
        int nIndex = box.GetCurSel();
        CString strItem; box.GetText(nIndex, strItem);
        box.DeleteString(nIndex);
        nIndex = box.InsertString(nIndex - 1, strItem);
        box.SetCurSel(nIndex);
    }
    CString moved; box.GetText(1, moved);
    check(moved == "charlie" && box.GetCurSel() == 1,
          "the Move Up sequence moves and keeps the selection");

    box.m_hWnd = nullptr;
    host->Destroy();
}

// ── 14. utils.directorypicker shows a dialog, or answers nil (2026-08-18) ──
// The app's Lua "utils" grab-bag comes from scripting/lua_utils.cpp, whose
// directorypicker body hung entirely off ::SHGetMalloc — off Windows the shim
// stub that always returns E_FAIL (platform/mfc_ui.h).  So the body never ran,
// nothing was pushed, and the function's `return 1` handed back whatever
// argument was still on top of the Lua stack: a plugin asking the user for a
// folder got back the DEFAULT PATH it had passed in, indistinguishable from the
// user having chosen exactly that, or the dialog TITLE when it passed no
// default.  No dialog was ever shown.
//
// It has to be tested here.  mushcore links the portable twin
// (scripting/lua_utils_portable.cpp), which always had the hook arm, so the
// headless suite exercises the other body entirely.
extern const struct luaL_Reg * ptr_xmllib;      // scripting/lua_utils.cpp

static std::string g_dirpick_title, g_dirpick_default;

void TestLuaDirectoryPicker()
{
    printf("utils.directorypicker:\n");

    lua_State* L = luaL_newstate();
    if (!L) { check(false, "a Lua state to register utils into"); return; }
    luaL_openlibs(L);
    luaL_register(L, "utils", ptr_xmllib);
    lua_settop(L, 0);

    char* (*const savedHook)(const char*, const char*) = g_wxhook_directorypicker;

    g_dirpick_title.clear();
    g_dirpick_default.clear();
    g_wxhook_directorypicker = [](const char* title, const char* defPath) -> char* {
        g_dirpick_title   = title   ? title   : "";
        g_dirpick_default = defPath ? defPath : "";
        return strdup("/fake");
    };

    check(luaL_dostring(L, "picked = utils.directorypicker ('Pick one', '/tmp')") == 0,
          "utils.directorypicker is registered and runs");
    lua_getglobal(L, "picked");
    check(lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "/fake") == 0,
          "...and returns the folder the picker actually chose");
    lua_pop(L, 1);
    check(g_dirpick_title == "Pick one" && g_dirpick_default == "/tmp",
          "...having handed the title and the default directory to the dialog");

    // No picker installed is the shape of the shipped defect: nothing can be
    // chosen, so the answer must be nil.  Returning the default path here is
    // the bug — the caller cannot tell it from a genuine choice.
    g_wxhook_directorypicker = nullptr;
    check(luaL_dostring(L, "picked = utils.directorypicker ('Pick one', '/tmp')") == 0,
          "a call with no picker available still returns cleanly");
    lua_getglobal(L, "picked");
    check(lua_isnil(L, -1), "...and answers nil, not the default directory");
    lua_pop(L, 1);

    check(luaL_dostring(L, "picked = utils.directorypicker ('Pick one')") == 0,
          "...as does one that passed no default directory");
    lua_getglobal(L, "picked");
    check(lua_isnil(L, -1), "...answering nil rather than the dialog title");
    lua_pop(L, 1);

    g_wxhook_directorypicker = savedHook;
    lua_close(L);
}


// ── 15. World-properties change detection and page-to-page state (2026-08-18) ──
// Five defects that show up only when a page is left and re-entered, or when OK
// is pressed with nothing altered.  None of them is reachable from mushcore:
// configuration.cpp, prefspropertypages.cpp and GlobalPrefs.cpp are all
// mushgui_adapters TUs.

// CPrefsP8::CheckIfChanged decides whether editing a trigger dirties the world.
// It compared every field of CTrigger except bMultiLine, so un-ticking a
// trigger's "multi-line" box and changing nothing else was silently discarded.
void TestTriggerMultiLineIsAChange()
{
    printf("Trigger dialog change detection:\n");

    CMUSHclientDoc doc;
    CPrefsP14 page14;                    // LoadDialog reads its three show-* flags
    CPrefsP8  page8;
    page8.m_doc      = &doc;
    page8.m_prefsp3  = NULL;
    page8.m_prefsp14 = &page14;

    CTrigger trig;
    trig.trigger       = "^You see (.*)$";
    trig.contents      = "look $1";
    trig.bMultiLine    = true;
    trig.iLinesToMatch = 3;

    CTriggerDlg dlg;
    page8.LoadDialog(&dlg, &trig);

    // The control.  Without it a gutted `return true` would pass everything
    // below.
    check(!page8.CheckIfChanged(&dlg, &trig),
          "an untouched trigger dialog reports NO change");

    dlg.m_bMultiLine = FALSE;
    check(page8.CheckIfChanged(&dlg, &trig),
          "un-ticking multi-line IS a change (was: silently thrown away)");

    // The sibling field was already compared; assert it still is, so a fix that
    // replaced a term rather than adding one would show up.
    dlg.m_bMultiLine = TRUE;
    dlg.m_iLinesToMatch = 4;
    check(page8.CheckIfChanged(&dlg, &trig),
          "...and changing the number of lines to match still is too");
}

// ChangedPrefsP9 / ChangedPrefsP17 decide whether pressing OK on World
// Properties marks the world modified.  Both undid LoadPrefs*'s SAMECOLOUR
// encoding by subtracting 1, giving -1 — which the unsigned short 65535 can
// never equal, so a world using the default "same as normal text" colour was
// reported changed every single time.
void TestDefaultColoursAreNotASpuriousChange()
{
    printf("World-properties change detection (SAMECOLOUR):\n");

    CMUSHclientDoc doc;

    CPrefsP9 page9;
    doc.m_echo_colour = SAMECOLOUR;          // the default: same as normal text
    doc.LoadPrefsP9(page9);
    check(!doc.ChangedPrefsP9(page9),
          "the default echo colour is not reported as a change");
    page9.m_echo_colour = 1;                 // the user picked custom colour 1
    check(doc.ChangedPrefsP9(page9),
          "...but picking one IS (so this is not a blanket 'nothing changed')");

    // A world that already uses a custom colour has to keep working too.
    doc.m_echo_colour = 4;
    doc.LoadPrefsP9(page9);
    check(!doc.ChangedPrefsP9(page9),
          "a custom echo colour round-trips as unchanged as well");

    CPrefsP17 page17;
    doc.m_iNoteTextColour = SAMECOLOUR;
    doc.LoadPrefsP17(page17);
    check(!doc.ChangedPrefsP17(page17),
          "the default note colour is not reported as a change");
    page17.m_iNoteTextColour = 5;
    check(doc.ChangedPrefsP17(page17),
          "...but picking one IS");

    doc.m_iNoteTextColour = 4;               // the shipped default note colour
    doc.LoadPrefsP17(page17);
    check(!doc.ChangedPrefsP17(page17),
          "a custom note colour round-trips as unchanged as well");
}

// CPrefsP9::OnSetActive re-seeds both command-window swatches from
// m_input_text_colour / m_input_background_colour, which only OnOK ever wrote.
// So a colour picked on the Commands page was discarded by visiting any other
// page and coming back.  CPrefsP3 and CPrefsP5 already saved theirs in
// OnKillActive; P9 now does the same.
void TestCommandColoursSurviveAPageChange()
{
    printf("Commands page colour swatches:\n");

    CPrefsP9 page;
    page.m_input_text_colour       = RGB(1, 2, 3);
    page.m_input_background_colour = RGB(4, 5, 6);
    page.m_ctlTextSwatch.m_colour       = RGB(9, 9, 9);   // what the user just picked
    page.m_ctlBackgroundSwatch.m_colour = RGB(8, 8, 8);

    page.OnKillActive();                     // leaving the page for another one

    check(page.m_input_text_colour == RGB(9, 9, 9) &&
          page.m_input_background_colour == RGB(8, 8, 8),
          "leaving the page saves both swatches (was: only OnOK ever did)");

    // ...so the two lines OnSetActive re-seeds with restore the user's pick
    // rather than reverting it.  (OnSetActive itself needs a live property sheet
    // as its parent, so its body is spelled out here.)
    page.m_ctlTextSwatch.m_colour       = 0;
    page.m_ctlBackgroundSwatch.m_colour = 0;
    page.m_ctlTextSwatch.m_colour       = page.m_input_text_colour;
    page.m_ctlBackgroundSwatch.m_colour = page.m_input_background_colour;
    check(page.m_ctlTextSwatch.m_colour == RGB(9, 9, 9) &&
          page.m_ctlBackgroundSwatch.m_colour == RGB(8, 8, 8),
          "...so coming back shows it, not the colour from before the sheet opened");
}

// CGlobalPrefsP4::OnFont seeded the font picker's face from App.m_strPrinterFont
// — the value from before the sheet opened — instead of the page's own
// in-progress one.  Reopening the picker to change only the size therefore
// silently reverted the face chosen a moment earlier.  CGlobalPrefsP10::OnFont
// already did it the right way round.
void TestPrinterFontPickerSeedsFromThePage()
{
    printf("Global prefs printer font picker:\n");

    static std::string s_seed;
    static int s_calls = 0;
    int (*const savedFont)(void*, WxFontChoice*)              = g_wxhook_font_dialog;
    int (*const savedBox)(const char*, const char*, unsigned) = g_wxhook_message_box;

    g_wxhook_font_dialog = [](void*, WxFontChoice* io) -> int {
        s_seed = io ? io->faceName : "";
        ++s_calls;
        return 0;                            // cancel — nothing is chosen
    };
    // The page has no window here.  DDX now leaves a member alone when the
    // control is missing (it used to zero it, which then tripped the
    // lines-per-page DDV), but keep the swallow: the assertion below is about
    // which string seeded the picker, not about the margins.
    g_wxhook_message_box = [](const char*, const char*, unsigned) -> int {
        return IDOK;
    };

    struct Probe : CGlobalPrefsP4 { using CGlobalPrefsP4::OnFont; };
    Probe page;
    App.m_strPrinterFont    = "Courier";     // what it was when the sheet opened
    page.m_strPrinterFont   = "Menlo";       // what the user has picked since
    page.m_nPrinterFontSize = 10;

    s_seed.clear(); s_calls = 0;
    page.OnFont();

    check(s_calls == 1, "the font picker was actually opened");
    check(s_seed == "Menlo",
          "...seeded with the page's own font (was: the one from before the sheet)");

    g_wxhook_font_dialog = savedFont;
    g_wxhook_message_box = savedBox;
}

// CPrefsP16::CompareObjects is the Timers list's sort.  Its switch had no
// eColumnContents case, so it fell through to `default: iResult = 0` and
// clicking the Contents header did nothing at all.
void TestTimerListSortsByContents()
{
    printf("Timers list column sort:\n");

    CTimer a, b;
    // Everything the other cases key on is deliberately EQUAL, so only the
    // Contents comparison can discriminate.
    a.strContents = "aaa";
    b.strContents = "zzz";

    check(CPrefsP16::CompareObjects(CPrefsP16::eColumnContents, &a, &b) < 0,
          "the Contents column sorts (was: always 0, so the header did nothing)");
    check(CPrefsP16::CompareObjects(CPrefsP16::eColumnContents, &b, &a) > 0,
          "...in both directions");
    check(CPrefsP16::CompareObjects(CPrefsP16::eColumnContents, &a, &a) == 0,
          "...and calls equal contents equal");
    // Case-insensitively, like every other string column on these pages.
    b.strContents = "AAA";
    check(CPrefsP16::CompareObjects(CPrefsP16::eColumnContents, &a, &b) == 0,
          "...ignoring case, as the alias and trigger pages do");
    // The control: a column that has always sorted still does.
    a.strLabel = "aaa"; b.strLabel = "zzz";
    check(CPrefsP16::CompareObjects(CPrefsP16::eColumnLabel, &a, &b) < 0,
          "the Label column still sorts too");
}

// Three dialogs put their fixed-pitch font on their edit control with a raw
// WM_SETFONT ::SendMessage, and the port's send-message hook handles only
// WM_COMMAND and the two LVM_*EXTENDEDLISTVIEWSTYLE messages — so the font was
// simply dropped and the World ▸ Debug input box, the Credits box and Send To
// All showed the proportional default.  They now go through CWnd::SetFont, as
// Utilities.cpp's FixFont already did (with the WM_SETFONT form commented out).
void TestFixedPitchFontReachesTheDialogs()
{
    printf("Fixed-pitch font on the raw-SendMessage dialogs:\n");

    static std::string s_face;
    static int s_calls = 0;
    void (*const savedFont)(void*, void*) = g_wxhook_set_font;
    g_wxhook_set_font = [](void*, void* font) {
        ++s_calls;
        wxFont* f = (wxFont*) font;
        s_face = f ? std::string(f->GetFaceName().utf8_str()) : std::string();
    };

    App.m_strFixedPitchFont  = "Menlo";
    App.m_iFixedPitchFontSize = 9;

    s_calls = 0; s_face.clear();
    struct Probe : CDebugWorldInputDlg { using CDebugWorldInputDlg::OnInitDialog; };
    Probe dlg;
    dlg.OnInitDialog();

    check(s_calls >= 1, "World > Debug input box sets a font at all (was: 0 calls)");
    check(s_face == "Menlo", "...and it is the fixed-pitch font from the preferences");

    g_wxhook_set_font = savedFont;
}

// GUID::Data1 was declared `unsigned long`, which is 64 bits here and 32 on
// Windows, while GetGUID's format was "%08lX" — a MINIMUM width.  So the first
// group came out sixteen hex digits and world.CreateGUID returned a malformed
// 44-character GUID.  Only the GUI build compiles Utilities.cpp (mushcore has
// its own twin in doc_port_glue.cpp), so this cannot be caught headlessly.
void TestCreateGuidShape()
{
    printf("GetGUID (world.CreateGUID):\n");

    const CString guid = GetGUID ();
    const std::string s ((LPCTSTR) guid);

    check(s.size() == 36, "the GUID is 36 characters (was: 44 on any LP64 host)");

    // 8-4-4-4-12 hex with dashes, checked positionally so a wrong group WIDTH
    // fails rather than merely a wrong total length.
    static const int groups[5] = { 8, 4, 4, 4, 12 };
    bool shape = s.size() == 36;
    size_t at = 0;
    for (int g = 0; g < 5 && shape; g++) {
        if (g > 0) shape = shape && at < s.size() && s[at++] == '-';
        for (int i = 0; i < groups[g] && shape; i++, at++)
            shape = at < s.size() && isxdigit((unsigned char) s[at]) != 0
                                  && !islower((unsigned char) s[at]);
    }
    check(shape && at == s.size(), "...in 8-4-4-4-12 uppercase hex groups");

    // The control: it is not a constant.
    check(GetGUID () != guid, "consecutive calls differ");
}

// metaphone() clamps its OUTPUT length to the size of its 61-byte stack buffer
// and then fills that buffer from the input with no bound at all, so any word of
// more than 60 letters smashes the stack frame (upstream bug 30).  It has no
// caller in the tree, and only the GUI build compiles Utilities.cpp, so this is
// the only place the shipping body can be driven.
void TestMetaphoneBoundsItsInput()
{
    printf("metaphone() input bound:\n");

    char out[64];

    // The control first: an ordinary word still comes back with something.
    out[0] = '\0';
    metaphone ("Thompson", out, (int) sizeof (out) - 1);
    check(out[0] != '\0', "an ordinary word produces a result");

    std::string huge;
    while (huge.size() < 200) huge += "Thompson";
    out[0] = '\0';
    metaphone (huge.c_str (), out, (int) sizeof (out) - 1);
    check(out[0] != '\0', "a 200-letter word produces one too (was: stack smash)");
    check(std::string (out).size () < sizeof (out),
          "...inside the caller's buffer");
}

// The "Special characters" help popup (CRecallSearchDlg / CPrefsP4's
// IDC_SUBSITUTION_HELP button) opens a CCreditsDlg on IDR_SUBSTITUTION, which
// loads its body via g_wxhook_load_text_resource the same way the About box's
// licence/credits text does (id 252/177) -- this exercises that third id.
void TestSubstitutionTextResource()
{
    printf("Substitution help text resource (IDR_SUBSTITUTION):\n");

    unsigned size = 0;
    const char* text = g_wxhook_load_text_resource(IDR_SUBSTITUTION, &size);

    check(text != nullptr, "IDR_SUBSTITUTION resolves to bundled text (was: nullptr, \"Could not load text\")");
    check(size > 0, "...with non-zero size");
}

// The Debug "summary" page is a wall of hyperlinks whose action is
// "!!<DEBUG_PLUGIN_ID>:<command>(<arg>)"; HyperlinkClicked routes those to
// DebugHelper, which is the ONLY caller.  Two defects lived there, both
// upstream (no platform ifdef) and both reachable only through such a link —
// never from MUD output, MXP, or any link the client itself emits.
void TestDebugHelperPluginContext()
{
    printf("Debug hyperlink helper (plugin context):\n");

    SelfTestEnablePluginLoading();

    CMUSHclientDoc doc;
    doc.m_mush_name   = "zz_selftest_debughelper";
    doc.m_nWrapColumn = 80;
    doc.m_maxlines    = 5000;
    doc.SetUpOutputWindow();       // else Note has nowhere to put a line

    // True if some line in the output buffer contains `want`.
    auto sawLine = [&](const char* want) -> bool {
        for (POSITION p = doc.m_LineList.GetHeadPosition(); p; ) {
            const CLine* l = doc.m_LineList.GetNext(p);
            if (l && l->len > 0 &&
                std::string(l->text, l->len).find(want) != std::string::npos)
                return true;
        }
        return false;
    };

    // ── callbacklist with no plugin in context ─────────────────────────────
    // The branch walked m_CurrentPlugin->m_PluginCallbacks with no null guard,
    // unlike the editplugin branch beside it, so a link naming no plugin took
    // the whole app down.  Reaching this at all used to crash the selftest.
    doc.m_CurrentPlugin = NULL;
    doc.HyperlinkClicked("!!" DEBUG_PLUGIN_ID ":callbacklist()",
                         ACTION_SEND, true);
    check(sawLine("No plugin selected."),
          "callbacklist with no plugin says so (was: dereferenced NULL)");

    // ── an early return still puts the plugin context back ─────────────────
    // Five branches returned before the restore at the end of DebugHelper, so
    // clicking e.g. a trigger link that named something gone left the document
    // stuck in that plugin's context: every later map lookup, and every script
    // call, saw the plugin's tables instead of the world's.
    {
        const char* kPluginID = "0123456789abcdef01234567";
        const std::string pluginPath = SelfTestWritePlugin(
            "debughelper", kPluginID, "DebugCtx", "");
        check(!pluginPath.empty(), "restore: wrote a fixture plugin file");

        if (!pluginPath.empty()) {
            const long rc = doc.LoadPlugin(CString(pluginPath.c_str()));
            check(rc == 0 && doc.m_PluginList.size() == 1,
                  "restore: the fixture plugin loaded");

            doc.m_CurrentPlugin = NULL;
            doc.HyperlinkClicked("!!" DEBUG_PLUGIN_ID
                                 ":showtrigger(_0123456789abcdef01234567_nosuch)",
                                 ACTION_SEND, true);

            // The two controls: without them "m_CurrentPlugin is NULL" would
            // also pass for a helper that never entered the plugin at all, or
            // never reached the early return under test.
            check(sawLine("(For plugin: DebugCtx)"),
                  "restore: the link really did switch into the plugin");
            check(sawLine("Trigger nosuch does not exist."),
                  "restore: ...and took the missing-trigger early return");

            check(doc.m_CurrentPlugin == NULL,
                  "restore: the plugin context is put back on that return");

            remove(pluginPath.c_str());
        }
    }
}

// CPluginsDlg::OnUpdateNeedDescription read `p->m_strDescription` straight off
// the selected row's item data with no find()-in-m_PluginList liveness check,
// unlike every sibling handler (OnShowDescription, OnEdit, OnEnable,
// OnDisable, OnRdblclkPluginsList).  A plugin destroyed while the dialog is
// open without LoadList() rebuilding the rows — world.UnloadPlugin /
// ReloadPlugin from a trigger or timer firing during the dialog's modal loop
// — leaves that row's item data pointing at a CPlugin the idle-driven
// update-UI pump then reads.  This drives the real ON_UPDATE_COMMAND_UI
// handler through AfxDispatchUpdateUI over a real wxListCtrl-backed
// CListCtrl, the same path idle processing uses; the erased plugin is kept
// ALIVE (just removed from m_PluginList) so the test itself never touches
// freed memory — only the liveness check under test does.
void TestPluginsDlgDescriptionLiveness()
{
    printf("Plugins dialog: description liveness guard:\n");

    SelfTestEnablePluginLoading();

    CMUSHclientDoc doc;
    doc.m_mush_name   = "zz_selftest_pluginsdlg";
    doc.m_nWrapColumn = 80;
    doc.m_maxlines    = 5000;
    doc.SetUpOutputWindow();

    const std::string livePath = SelfTestWritePlugin(
        "pluginsdlg_live", "aaaaaaaaaaaaaaaaaaaaaaaa", "AAALive", "");
    const std::string erasedPath = SelfTestWritePlugin(
        "pluginsdlg_erased", "bbbbbbbbbbbbbbbbbbbbbbbb", "ZZZErased", "");
    check(!livePath.empty() && !erasedPath.empty(),
          "wrote both fixture plugin files");

    if (livePath.empty() || erasedPath.empty()) return;

    check(doc.LoadPlugin(CString(livePath.c_str())) == 0 &&
          doc.LoadPlugin(CString(erasedPath.c_str())) == 0 &&
          doc.m_PluginList.size() == 2,
          "both fixture plugins loaded");

    remove(livePath.c_str());
    remove(erasedPath.c_str());

    auto findPluginById = [&](const char* id) -> CPlugin* {
        for (CPlugin* pl : doc.m_PluginList)
            if (pl->m_strID == id) return pl;
        return nullptr;
    };

    CPlugin* live   = findPluginById("aaaaaaaaaaaaaaaaaaaaaaaa");
    CPlugin* erased = findPluginById("bbbbbbbbbbbbbbbbbbbbbbbb");
    check(live && erased, "both plugin objects are reachable by ID");
    if (!live || !erased) return;

    live->m_strDescription   = "The live plugin's long description.";
    erased->m_strDescription = "The erased plugin's long description.";

    const RcDialog* t = MfcDialogResources().FindDialog(IDD_PLUGINS);
    check(t != nullptr, "IDD_PLUGINS is in the recovered resources");
    if (!t) return;

    wxDialog* wxdlg = BuildWxDialogFromTemplate(nullptr, *t);
    wxListCtrl* lv = wxDynamicCast(wxdlg->FindWindow(IDC_PLUGINS_LIST), wxListCtrl);
    check(lv != nullptr, "its plugin list is built as a wxListCtrl");

    if (lv)
    {
        CPluginsDlg dlg;
        dlg.m_pDoc = &doc;
        dlg.m_ctlPluginList.m_hWnd = (HWND) lv;

        // LoadList() only fills rows — the columns a report-mode list needs to
        // hold them are normally inserted by OnInitDialog, which this test
        // skips (it also touches the user's real column-width preferences).
        dlg.m_ctlPluginList.InsertColumn(eColumnName,     "Name",     LVCFMT_LEFT,  130);
        dlg.m_ctlPluginList.InsertColumn(eColumnPurpose,  "Purpose",  LVCFMT_LEFT,  130);
        dlg.m_ctlPluginList.InsertColumn(eColumnAuthor,   "Author",   LVCFMT_LEFT,   55);
        dlg.m_ctlPluginList.InsertColumn(eColumnLanguage, "Language", LVCFMT_LEFT,   55);
        dlg.m_ctlPluginList.InsertColumn(eColumnFile,     "File",     LVCFMT_LEFT,   80);
        dlg.m_ctlPluginList.InsertColumn(eColumnEnabled,  "Enabled",  LVCFMT_LEFT,   30);
        dlg.m_ctlPluginList.InsertColumn(eColumnVersion,  "Ver",      LVCFMT_RIGHT,  20);

        dlg.LoadList();   // sorts by name ascending: row 0 = AAALive, row 1 = ZZZErased
        check(lv->GetItemCount() == 2, "both plugins became list rows");

        // Simulate the hazard: the plugin is removed from the doc's list
        // without LoadList() rebuilding the rows, so row 1's item data still
        // points at `erased`.
        PluginListIterator pit = find (doc.m_PluginList.begin (),
                                       doc.m_PluginList.end (),
                                       erased);
        check(pit != doc.m_PluginList.end (), "erased plugin starts in m_PluginList");
        if (pit != doc.m_PluginList.end ())
            doc.m_PluginList.erase (pit);

        auto selectOnly = [&](int item) {
            dlg.m_ctlPluginList.SetItemState (-1, 0, LVIS_SELECTED);
            dlg.m_ctlPluginList.SetItemState (item, LVIS_SELECTED, LVIS_SELECTED);
        };

        selectOnly(0);   // AAALive — still in m_PluginList
        check(dlg.m_ctlPluginList.GetSelectedCount () == 1,
              "selection landed on exactly the live plugin's row");

        bool enabled = false;
        CCmdUI cmdui;
        cmdui.m_nID            = IDC_SHOW_DESCRIPTION;
        cmdui.m_pCaptureEnable = &enabled;
        check(AfxDispatchUpdateUI (&dlg, &cmdui) != FALSE,
              "OnUpdateNeedDescription answered for the live-plugin row");
        check(enabled,
              "...and enables Show Info for a live plugin with a description");

        selectOnly(1);   // ZZZErased — erased from m_PluginList, item data stale

        // The over-broad-mutation control: a handler that unconditionally
        // disabled Show Info would also pass the assertion below, so first
        // confirm this row really is the erased plugin's stale item data.
        check((CPlugin*) dlg.m_ctlPluginList.GetItemData (1) == erased,
              "row 1's item data is still the erased plugin's stale pointer");
        check(dlg.m_ctlPluginList.GetSelectedCount () == 1,
              "selection landed on exactly the erased plugin's row");

        enabled = true;   // the opposite of the expected answer below
        cmdui = CCmdUI();
        cmdui.m_nID            = IDC_SHOW_DESCRIPTION;
        cmdui.m_pCaptureEnable = &enabled;
        check(AfxDispatchUpdateUI (&dlg, &cmdui) != FALSE,
              "OnUpdateNeedDescription answered for the erased-plugin row");
        check(!enabled,
              "...and disables Show Info once the plugin has left m_PluginList "
              "(was: read the stale item data unchecked)");

        dlg.m_ctlPluginList.m_hWnd = nullptr;   // don't outlive the wxDialog below
    }

    wxdlg->Destroy();

    delete erased;   // no longer owned by doc.m_PluginList after the erase() above
}

int RunGuiBridgeSelfTest()
{
    printf("GUI-BRIDGE-SELFTEST: geometry, dialog controls, world defaults\n");

    TestGeometry();
    TestListBoxStyles();
    TestNewWorldDefaults();
    TestProgressDialog();
    TestMessageBoxDecode();
    TestDialogKeyTranslation();
    TestPopupMenuBuild();
    TestRecentWorlds();
    TestClipboardRoundTrip();
    TestFunctionListView();
    TestDdxCharsetRoundTrip();
    TestEditOffsetsAreBytes();
    TestScriptingPageBrowse();
    TestListBoxMutators();
    TestLuaDirectoryPicker();
    TestTriggerMultiLineIsAChange();
    TestDefaultColoursAreNotASpuriousChange();
    TestCommandColoursSurviveAPageChange();
    TestPrinterFontPickerSeedsFromThePage();
    TestTimerListSortsByContents();
    TestFixedPitchFontReachesTheDialogs();
    TestCreateGuidShape();
    TestMetaphoneBoundsItsInput();
    TestSubstitutionTextResource();
    TestDebugHelperPluginContext();
    TestPluginsDlgDescriptionLiveness();

    printf("GUI-BRIDGE-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#endif // !_WIN32
