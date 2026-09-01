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
#include <wx/odcombo.h>                 // wxOwnerDrawnComboBox (owner-draw combos)
#include <wx/valtext.h>                 // wxTextValidator (ES_NUMBER digit filter)
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
#include "dialogs/MapDlg.h"                            // CMapDlg — the mapper's "make alias"
#include "dialogs/world_prefs/aliasdlg.h"              // CAliasDlg — what it runs
#include "dialogs/ColourComboBox.h"                    // CColourComboBox — owner-draw combo

#include <cctype>                       // isxdigit/islower — the GUID shape check
#include <cstring>                      // strcmp — the OptionsTable row lookup

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

// The world option table (scriptingoptions.cpp, rows in
// scriptingoptions_numeric.inc) — the single source of truth for every
// numeric/boolean world default, which the seed above must agree with.
extern tConfigurationNumericOption OptionsTable [];
// Defined in wx_mfc_impl.cpp: render one row of an owner-draw combo by the same
// call the popup's paint makes.  Selftests exit before the event loop, so no
// real paint ever happens — this is the engine-shaped function to call instead.
// Returns an invalid bitmap when the control is not an owner-draw combo.
wxBitmap wx_render_owner_draw_combo_item(wxWindow* combo, int item, int flags,
                                         int w, int h);

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

// ── 2b. LBS_SORT / CBS_SORT keep Windows' alphabetical display order ──────
// Windows sorts as AddString inserts and returns the sorted index; consumers
// map rows via that index + item data, so dropping the styles was harmless to
// correctness but showed Choose Notepad, Complete Word, Send to All Worlds
// and utils.choose in insertion order where Windows users see them sorted.
// The behavioural half (Append lands "Apple" ABOVE the earlier "Zebra") is
// what proves the sort takes effect; the flag check alone could pass with a
// wx build that ignores it.
void TestSortedListStyles()
{
    printf("Dialog bridge (LBS_SORT / CBS_SORT):\n");


    struct { int idd; int ctlId; const char* what; } sorted[] = {
        { IDD_SEND_TO_ALL,    IDC_WORLD_LIST,   "Send to All Worlds' world list sorts" },
        { IDD_CHOOSE_NOTEPAD, IDC_NOTEPAD_LIST, "Choose Notepad's list sorts" },
        { IDD_COMPLETE_WORD,  IDC_WORD_LIST,    "Complete Word's candidate list sorts" },
    };
    for (const auto& r : sorted)
    {
        const RcDialog* t = MfcDialogResources().FindDialog(r.idd);
        check(t != nullptr, "sorted-list template is in the recovered resources");
        if (!t) continue;
        wxDialog* dlg = BuildWxDialogFromTemplate(nullptr, *t);
        wxListBox* lb = wxDynamicCast(dlg->FindWindow(r.ctlId), wxListBox);
        check(lb != nullptr, "its list is built as a wxListBox");
        if (lb)
        {
            check((lb->GetWindowStyleFlag() & wxLB_SORT) != 0,
                  "LBS_SORT builds wxLB_SORT");
            lb->Append(wxT("Zebra"));
            lb->Append(wxT("Apple"));
            check(lb->GetString(0) == wxT("Apple"), r.what);
        }
        dlg->Destroy();
    }

    // The read-only combo arm: utils.choose (CLuaChooseBox) declares CBS_SORT
    // on a CBS_DROPDOWNLIST, which builds a wxChoice.
    {
        const RcDialog* t = MfcDialogResources().FindDialog(IDD_LUA_CHOOSE);
        check(t != nullptr, "IDD_LUA_CHOOSE is in the recovered resources");
        if (t)
        {
            wxDialog* dlg = BuildWxDialogFromTemplate(nullptr, *t);
            wxChoice* ch = wxDynamicCast(dlg->FindWindow(IDC_CHOOSE_COMBO), wxChoice);
            check(ch != nullptr, "utils.choose's combo is built as a wxChoice");
            if (ch)
            {
                check((ch->GetWindowStyleFlag() & wxCB_SORT) != 0,
                      "CBS_SORT builds wxCB_SORT");

                // The whole CLuaChooseBox pattern, through the CComboBox shim:
                // AddString's RETURNED index is where the item landed (the
                // CB_ADDSTRING contract), and item data attached through it
                // stays with its string.  Raw wxChoice::Append does NOT keep
                // that contract on wxOSX (it inserts sorted but returns the
                // append-order count), so a fix that merely sets wxCB_SORT
                // without correcting hook_combo_add_string fails here -- the
                // data would land on the wrong row and utils.choose would
                // answer the wrong value.
                CComboBox combo;
                combo.m_hWnd = (HWND) ch;
                const int iZebra = combo.AddString("Zebra");
                combo.SetItemData(iZebra, 100);
                const int iApple = combo.AddString("Apple");
                combo.SetItemData(iApple, 200);
                check(iApple == 0,
                      "AddString returns the SORTED index (CB_ADDSTRING contract)");
                check(ch->GetString(0) == wxT("Apple"),
                      "utils.choose drop-downs list alphabetically");
                check(combo.GetItemData(0) == 200 && combo.GetItemData(1) == 100,
                      "item data stays with its string (utils.choose's value mapping)");
            }
            dlg->Destroy();
        }
    }

    // Negative control: a list WITHOUT LBS_SORT must stay in insertion order,
    // so a fix that blanket-sorts every list fails here.  Same synthetic
    // no-sort style TestListBoxStyles uses (no recovered template needed).
    {
        RcDialog synth;
        synth.id = 1; synth.cx = 200; synth.cy = 100;
        synth.caption = "sort-selftest";
        RcControl plain;
        plain.className = "ListBox"; plain.id = 1000;
        plain.style = 0x50B10101;    // ordinary single-select, NO LBS_SORT
        plain.x = 5; plain.y = 5; plain.cx = 100; plain.cy = 40;
        synth.controls.push_back(plain);

        wxDialog* dlg = BuildWxDialogFromTemplate(nullptr, synth);
        wxListBox* lb = wxDynamicCast(dlg->FindWindow(1000), wxListBox);
        check(lb != nullptr, "the unsorted control list box was built");
        if (lb)
        {
            check((lb->GetWindowStyleFlag() & wxLB_SORT) == 0,
                  "no LBS_SORT, no wxLB_SORT");
            lb->Append(wxT("Zebra"));
            lb->Append(wxT("Apple"));
            check(lb->GetString(0) == wxT("Zebra"),
                  "...and items stay in insertion order");
        }
        dlg->Destroy();
    }
}

// ── 2d. BS_DEFPUSHBUTTON becomes the dialog's default item ────────────────
// Windows' dialog manager presses the template's DEFPUSHBUTTON on Enter from
// any non-button control.  The bridge dropped the style, and wx's own
// machinery could not compensate: wxTextCtrl::OnChar presses the dialog's
// default ITEM, which nothing set.  The last three fixtures are the
// non-degenerate ones -- their default is NOT the IDOK button, so a fix that
// hardcodes "the button whose id is IDOK" passes the first row and fails
// them; the negative control fails a fix that defaults the first button.
void TestDefaultButtons()
{
    printf("Dialog bridge (BS_DEFPUSHBUTTON -> wx default item):\n");

    struct { int idd; int ctlId; const char* what; } rows[] = {
        { IDD_GLOBAL_CHANGE,  IDOK,              "Global Change defaults to Change All (IDOK)" },
        { IDD_CHOOSE_NOTEPAD, IDC_OPEN_EXISTING, "Choose Notepad defaults to Open existing" },
        { IDD_LUA_DEBUG,      ID_DO_COMMAND,     "the Lua debugger defaults to Do Command" },
        { IDD_IMPORT_XML,     IDC_PLUGINS_LIST,  "Import XML defaults to Load/View Plugins" },
    };
    for (const auto& r : rows)
    {
        const RcDialog* t = MfcDialogResources().FindDialog(r.idd);
        check(t != nullptr, "default-button template is in the recovered resources");
        if (!t) continue;
        wxDialog* dlg = BuildWxDialogFromTemplate(nullptr, *t);
        wxWindow* def = dlg->GetDefaultItem();
        check(def != nullptr && def == dlg->FindWindow(r.ctlId), r.what);
        dlg->Destroy();
    }

    // Negative control: no BS_DEFPUSHBUTTON, no default item.
    {
        RcDialog synth;
        synth.id = 3; synth.cx = 200; synth.cy = 100;
        synth.caption = "default-selftest";
        RcControl plain;
        plain.className = "Button"; plain.id = 1000;
        plain.style = 0x50010000;    // WS_* | BS_PUSHBUTTON -- not the default style
        plain.x = 5; plain.y = 5; plain.cx = 50; plain.cy = 14;
        plain.text = "Plain";
        synth.controls.push_back(plain);

        wxDialog* dlg = BuildWxDialogFromTemplate(nullptr, synth);
        check(dlg->GetDefaultItem() == nullptr,
              "a template with no BS_DEFPUSHBUTTON gets no default item");
        dlg->Destroy();
    }
}

// ── 2c. ES_NUMBER / ES_UPPERCASE reach the bridged edit controls ──────────
// ES_NUMBER makes non-digit keystrokes (the minus sign included) untypeable on
// Windows, which is the only guard on fields with no DDV -- Recall's line
// count would otherwise hand a negative straight to RecallText.  ES_UPPERCASE
// folds typed text to upper case (the Global Prefs locale field).
void TestEditStyleValidators()
{
    printf("Dialog bridge (ES_NUMBER / ES_UPPERCASE):\n");

    // ES_NUMBER: Recall's line count carries it; the search-text edit in the
    // SAME template does not -- the non-degenerate pair.
    {
        const RcDialog* t = MfcDialogResources().FindDialog(IDD_RECALL);
        check(t != nullptr, "IDD_RECALL is in the recovered resources");
        if (t)
        {
            wxDialog* dlg = BuildWxDialogFromTemplate(nullptr, *t);
            wxTextCtrl* num = wxDynamicCast(dlg->FindWindow(IDC_LINE_COUNT), wxTextCtrl);
            wxTextCtrl* txt = wxDynamicCast(dlg->FindWindow(IDC_EDIT1), wxTextCtrl);
            check(num && txt, "both Recall edits were built");
            if (num && txt)
            {
                wxTextValidator* v =
                    wxDynamicCast(num->GetValidator(), wxTextValidator);
                check(v != nullptr, "ES_NUMBER attaches a wxTextValidator");
                if (v)
                {
                    check(v->HasFlag(wxFILTER_DIGITS),
                          "...with the digits-only filter");
                    check(v->IsValid(wxT("100")).empty(),
                          "digits pass the filter");
                    check(!v->IsValid(wxT("-100")).empty(),
                          "a minus sign is rejected (ES_NUMBER blocks it on Windows)");
                }
                check(txt->GetValidator() == nullptr ||
                      wxDynamicCast(txt->GetValidator(), wxTextValidator) == nullptr,
                      "an edit WITHOUT ES_NUMBER gets no digit filter");
            }
            dlg->Destroy();
        }
    }

    // ES_UPPERCASE: the Global Prefs locale field.  ForceUpper converts on the
    // text event SetValue raises synchronously, so no loop is needed.
    {
        const RcDialog* t = MfcDialogResources().FindDialog(IDD_GLOBAL_PREFSP2);
        check(t != nullptr, "IDD_GLOBAL_PREFSP2 is in the recovered resources");
        if (t)
        {
            wxDialog* dlg = BuildWxDialogFromTemplate(nullptr, *t);
            wxTextCtrl* loc = wxDynamicCast(dlg->FindWindow(IDC_LOCALE), wxTextCtrl);
            check(loc != nullptr, "the locale edit was built");
            if (loc)
            {
                loc->SetValue(wxT("en"));
                check(loc->GetValue() == wxT("EN"),
                      "ES_UPPERCASE folds entered text to upper case");
            }
            dlg->Destroy();
        }
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
    check(doc->m_nWrapColumn == 80 && doc->m_maxlines == 5000,
          "the port-only display defaults are still seeded");

    // ── the seed must not DISAGREE with the option table ──────────────────
    // This hook runs before the load on the Open World File path as well as
    // File ▸ New World, and the loader leaves a member alone when its
    // attribute is absent and the <world> tag says defaults="y"
    // (xml_load_world.cpp Load_World_Numeric_Options_XML, boolean arm).  So
    // any value seeded here that differs from OptionsTable becomes the
    // effective setting for every such world file — and gets written back
    // into the user's file on save.  utf_8 was seeded to 1 against a table
    // default of false, which opened hand-written defaults="y" worlds in
    // UTF-8 here and in ANSI on Windows.
    //
    // Asserted against the live table rather than a literal, so the rule
    // survives a future change to any of the three defaults.
    {
        auto tableDefault = [](const char* name) -> double {
            for (int i = 0; OptionsTable[i].pName; i++)
                if (strcmp(OptionsTable[i].pName, name) == 0)
                    return OptionsTable[i].iDefault;
            return -1;   // no such row → the check below fails loudly
        };
        check(doc->m_bUTF_8 == (unsigned short) tableDefault("utf_8") &&
              tableDefault("utf_8") == 0,
              "utf_8 is left at the OptionsTable default (ANSI), as on Windows");
        check(doc->m_nWrapColumn == (unsigned int) tableDefault("wrap_column"),
              "wrap_column agrees with the OptionsTable default");
        check(doc->m_maxlines == (long) tableDefault("max_output_lines"),
              "max_output_lines agrees with the OptionsTable default");
    }
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

    // Esc: wx's own Esc→Cancel looks the Cancel button up by wxID_CANCEL
    // (5101), which no bridged control carries (template ids are Win32's,
    // IDCANCEL == 2), so the bridge translates Esc and dispatches IDCANCEL
    // itself -- after PreTranslateMessage has had first refusal, which is how
    // CDebugLuaDlg's "Continue Execution (Esc)" keeps winning.
    msg = MSG{};
    check(DialogKeyToMsg(WXK_ESCAPE, false, &msg) &&
          msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE,
          "ESCAPE becomes WM_KEYDOWN/VK_ESCAPE (the bridge's Esc->Cancel arm)");

    // Everything else must be refused, so the caller Skip()s it and the
    // focused control's own key handling keeps working in every dialog.  A
    // blanket hook here would break all of it.
    const int kPassThrough[] = {
        WXK_SPACE, WXK_BACK, WXK_DELETE, WXK_LEFT, WXK_UP,
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
          "every other key is refused untouched (arrows, letters, F-keys)");

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
// Latin-1, so the text stays visible.
//
// 2026-08-19: the SAVE arm was still bare utf8_str(), so the two arms were not
// inverses and this test PINNED the wrong contract -- the member came back
// RE-ENCODED (E9 -> C3 A9) on any dialog OK, mutating stored bytes the user
// never touched.  It reaches disk: xml/xmlparse.cpp never acts on the encoding=
// declaration xml_save_world.cpp:40 writes (it handles only a UTF-16/UTF-8 BOM),
// so a world file is byte-transparent end to end and this seam was the only
// thing changing the bytes.  The decode is not injective -- one wxString, two
// possible byte strings -- so the save arm cannot invert it by inspection; it
// remembers the bytes the load arm was handed and hands those back.
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
    check(back && strcmp(back, "Caf\xE9 monde") == 0,
          "...as the VERY BYTES the load arm was handed (was: re-encoded to "
          "UTF-8, mutating a member the user never touched)");
    free(back);

    // The common case must stay exact: valid UTF-8 round-trips untouched.
    g_wxhook_set_window_text((void*) tc, "Caf\xC3\xA9");
    char* back2 = g_wxhook_get_window_text((void*) tc);
    check(back2 && strcmp(back2, "Caf\xC3\xA9") == 0,
          "valid UTF-8 round-trips byte-identical");
    free(back2);

    // ── the whole seam, as a dialog runs it ────────────────────────────────
    // The two hooks above are DDX's two arms; this drives the arms themselves,
    // through the real DDX_Text(CString) over a real CDataExchange, which is
    // what every bridged dialog's DoDataExchange calls.  "Open the dialog,
    // change nothing, press OK" must leave the member bit-for-bit as it was.
    {
        const int kId = 4711;
        wxTextCtrl* field = new wxTextCtrl(host, kId);

        CWnd dlgWnd;  dlgWnd.m_hWnd = (HWND) host;
        CDataExchange dx;  dx.m_pDlgWnd = &dlgWnd;

        // A world-file member as it actually sits on disk: CP1252/Latin-1 text
        // written by MUSHclient on Windows, which is not valid UTF-8.
        const CString kOnDisk("Vous \xEAtes ici \x97 \x93sud\x94");
        CString member = kOnDisk;

        dx.m_bSaveAndValidate = FALSE;       // load: member -> control
        DDX_Text(&dx, kId, member);
        check(!field->GetValue().empty(),
              "DDX load puts a legacy-charset member on screen");

        dx.m_bSaveAndValidate = TRUE;        // OK: control -> member
        DDX_Text(&dx, kId, member);
        check(member.GetLength() == kOnDisk.GetLength() &&
              memcmp((LPCTSTR) member, (LPCTSTR) kOnDisk,
                     kOnDisk.GetLength()) == 0,
              "DDX load -> OK -> save leaves the member BYTE-IDENTICAL "
              "(was: silently re-encoded to UTF-8 on every dialog OK)");

        // A second OK must not drift either -- the memo has to survive the
        // save, or the first OK would be exact and the next one would not.
        DDX_Text(&dx, kId, member);
        check(member == kOnDisk,
              "...and a second OK does not drift it further");

        // ── the control ────────────────────────────────────────────────────
        // Everything above would also pass a save arm that ignored the control
        // and always answered with the remembered bytes.  Typing must win.
        field->ChangeValue(wxString::FromUTF8("Vous \xC3\xAAtes l\xC3\xA0"));
        DDX_Text(&dx, kId, member);
        check(member == CString("Vous \xC3\xAAtes l\xC3\xA0"),
              "...but an EDITED field saves what the user typed, as UTF-8, "
              "not the bytes the load arm remembered");

        // And a control the load arm never touched is unaffected by any of it.
        {
            const int kId2 = 4712;
            wxTextCtrl* fresh = new wxTextCtrl(host, kId2);
            fresh->ChangeValue(wxString::FromUTF8("caf\xC3\xA9"));
            CString never;
            DDX_Text(&dx, kId2, never);
            check(never == CString("caf\xC3\xA9"),
                  "...and a control the load arm never wrote still saves as UTF-8");
        }
    }

    // ── multi-line fields: the CRLF seam (2026-09-01) ──────────────────────
    // Windows' CEdit holds a member's CRLF bytes verbatim, so DDX there is
    // byte-exact.  wxOSX EXPANDS each CRLF to TWO line breaks on the way in and
    // can only ever hand back LF — so a CRLF member (the normal form: the world
    // loader's AssembleContent makes every multi-line option CRLF) displayed
    // double-spaced, and a change-nothing OK wrote the LF-only doubled form
    // back, blank lines compounding once per dialog visit + save + reload.
    // The hooks now translate at the seam: CRLF/CR -> LF into the control,
    // LF -> CRLF back out, with the byte-fidelity memo compared against the
    // load-arm-NORMALISED form so the change-nothing case stays byte-identical.
    {
        const int kIdMulti = 4713;
        wxTextCtrl* multi = new wxTextCtrl(host, kIdMulti, wxEmptyString,
                                           wxDefaultPosition, wxDefaultSize,
                                           wxTE_MULTILINE);

        // (a) The display half: ONE break shown per CRLF, not two.
        g_wxhook_set_window_text((void*) multi, "a\r\nb");
        check(multi->GetValue() == wxT("a\nb"),
              "a CRLF member displays as ONE line break (was: doubled to two)");

        // (b) Change-nothing OK: the member must come back byte-identical.
        char* mlBack = g_wxhook_get_window_text((void*) multi);
        check(mlBack && strcmp(mlBack, "a\r\nb") == 0,
              "...and reads back as the very CRLF bytes (was: LF-only, doubled)");
        free(mlBack);

        // Through the real DDX arms, as a dialog runs them.
        CWnd dlgWnd2;  dlgWnd2.m_hWnd = (HWND) host;
        CDataExchange dx2;  dx2.m_pDlgWnd = &dlgWnd2;

        const CString kNotes("line one\r\nline two");
        CString member = kNotes;
        dx2.m_bSaveAndValidate = FALSE;      // load: member -> control
        DDX_Text(&dx2, kIdMulti, member);
        check(multi->GetValue() == wxT("line one\nline two"),
              "DDX load of a CRLF member shows two lines, not three");
        dx2.m_bSaveAndValidate = TRUE;       // OK: control -> member
        DDX_Text(&dx2, kIdMulti, member);
        check(member == kNotes,
              "DDX load -> change-nothing OK leaves a CRLF member BYTE-IDENTICAL "
              "(was: an LF-only doubled string, blank lines compounding per visit)");

        // (c) An EDITED field expands back to CRLF — pins the expansion, not
        // just the memo (a save arm that always answered from the memo passes
        // (b) but fails here).
        multi->ChangeValue(wxT("x\ny"));
        DDX_Text(&dx2, kIdMulti, member);
        check(member == CString("x\r\ny"),
              "an EDITED multi-line field saves LF back as CRLF (the CEdit form)");

        // (d) Legacy-charset bytes WITH a CRLF: the memo guard must compare the
        // normalised forms, or 672817c's byte fidelity dies for every
        // multi-line legacy member and this comes back re-encoded.
        const CString kLegacy("Caf\xE9\r\nmonde");    // é in Latin-1: invalid UTF-8
        member = kLegacy;
        dx2.m_bSaveAndValidate = FALSE;
        DDX_Text(&dx2, kIdMulti, member);
        dx2.m_bSaveAndValidate = TRUE;
        DDX_Text(&dx2, kIdMulti, member);
        check(member.GetLength() == kLegacy.GetLength() &&
              memcmp((LPCTSTR) member, (LPCTSTR) kLegacy,
                     kLegacy.GetLength()) == 0,
              "a legacy-charset member with a CRLF round-trips byte-identical "
              "(the memo guard normalises before comparing)");

        // Single-line control stays untouched by all of it: the seam is gated
        // on IsMultiLine(), so the plain fields above already prove the
        // negative — but pin the boundary explicitly with a lone CR member,
        // which a single-line control passes through as-is.
        const int kIdSingle = 4714;
        wxTextCtrl* single = new wxTextCtrl(host, kIdSingle);
        g_wxhook_set_window_text((void*) single, "plain text");
        char* slBack = g_wxhook_get_window_text((void*) single);
        check(slBack && strcmp(slBack, "plain text") == 0,
              "a single-line field is untouched by the CRLF seam (control)");
        free(slBack);
    }

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

// ── 11a. The Multi-Line Trigger conversion idiom is not a no-op (2026-09-01) ─
// CWorldChildFrame::MakeMultiLineTrigger (the port twin of
// CMUSHView::OnDisplayMultilinetrigger) converts the dialog's text with
// strText.Replace(ENDLINE, "\\n") — keyed on CRLF, because that is what a
// Windows CEdit hands DDX.  Before the CRLF seam fix, the bridge returned
// LF-only text, the Replace matched NOTHING, and the trigger was stored with
// raw LFs — which Save_XML_string wrote unescaped into the match attribute,
// and the parser then refused the whole world file ("not terminated").  This
// drives the exact seam the conversion depends on and asserts the replacement
// COUNT, so "the Replace did nothing" fails.
void TestMultiLineTriggerSeam()
{
    printf("Multi-Line Trigger conversion seam (ENDLINE is CRLF):\n");

    wxFrame* host = new wxFrame(nullptr, wxID_ANY, wxT("mlt-host"));
    wxTextCtrl* tc = new wxTextCtrl(host, wxID_ANY, wxEmptyString,
                                    wxDefaultPosition, wxDefaultSize,
                                    wxTE_MULTILINE);

    // The dialog seeds CRLF (dlg.m_strTriggerText.Replace("\\n", "\r\n"))
    // and DDX pushes it in; an untouched OK reads it straight back out.
    g_wxhook_set_window_text((void*) tc, "line1\r\nline2");
    char* raw = g_wxhook_get_window_text((void*) tc);
    CString strText(raw ? raw : "");
    free(raw);

    // iLinesToMatch counts '\n' — one per break, as on Windows, not two.
    int iCount = 1;
    for (const char* q = strText; *q; q++)
        if (*q == '\n') iCount++;
    check(iCount == 2, "two selected lines count as 2 lines to match (was: 3)");

    check(strText.Replace(ENDLINE, "\\n") == 1,
          "Replace(ENDLINE, ...) replaces ONE break (was: matched nothing)");
    check(strText.Find('\n') == -1 && strText.Find('\r') == -1,
          "...and no raw LF/CR survives into the trigger's match text");
    check(strText == CString("line1\\nline2"),
          "...the stored pattern is the two-character escape Windows produces");

    // The EDITED path must come back CRLF too — the memo answering the seeded
    // bytes would make the untouched case exact while an edit regressed.
    tc->ChangeValue(wxT("alpha\nbeta\ngamma"));
    raw = g_wxhook_get_window_text((void*) tc);
    CString strEdited(raw ? raw : "");
    free(raw);
    check(strEdited.Replace(ENDLINE, "\\n") == 2,
          "an EDITED selection converts both breaks (the expansion, not the memo)");
    check(strEdited == CString("alpha\\nbeta\\ngamma"),
          "...into the canonical escaped pattern");

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
    // 2 characters but 4 bytes — and for a MULTI-LINE control the byte space is
    // the CRLF representation GetWindowText answers with (Windows' CEdit), so
    // every break is TWO bytes where the control counts one character.
    // Characters: 0-1 àà, 2 \n, 3-4 àà, 5 \n, 6-7 àà      (last position 8)
    // CRLF bytes: 0-3 àà, 4-5 \r\n, 6-9 àà, 10-11 \r\n, 12-15 àà (length 16)
    {
        wxTextCtrl* tc = new wxTextCtrl(host, wxID_ANY, wxEmptyString,
                                        wxDefaultPosition, wxDefaultSize,
                                        wxTE_MULTILINE | wxTE_DONTWRAP);
        tc->SetValue(wxString::FromUTF8(
            "\xC3\xA0\xC3\xA0\n\xC3\xA0\xC3\xA0\n\xC3\xA0\xC3\xA0"));

        CEdit ed;
        ed.m_hWnd = (HWND) tc;

        // The caller's own text, in the space it indexes: GetWindowText on a
        // multi-line edit is CRLF, exactly as on Windows.
        char* raw = g_wxhook_get_window_text((void*) tc);
        CString crlfText(raw ? raw : "");
        free(raw);
        check(crlfText ==
              CString("\xC3\xA0\xC3\xA0\r\n\xC3\xA0\xC3\xA0\r\n\xC3\xA0\xC3\xA0"),
              "GetWindowText on a multi-line edit answers CRLF (the CEdit form)");

        check(ed.GetLineCount() == 3, "the line count is unaffected (the control)");
        check(ed.GetWindowTextLength() == 16,
              "the total length is 16 CRLF bytes, not 8 characters or 14 LF bytes");
        check(ed.LineIndex(1) == 6,  "LineIndex(1) is the line's CRLF-BYTE start (6, not 3 or 5)");
        check(ed.LineIndex(2) == 12, "LineIndex(2) likewise (12, not 6 or 10)");
        check(ed.LineLength(6) == 4,
              "LineLength takes a byte offset and answers in bytes (4, not 2)");
        // Byte 8 is inside line 1's àà; read as a character it would be the
        // end of the text, i.e. line 2 — so this one crosses a line boundary.
        check(ed.LineFromChar(8) == 1,
              "LineFromChar reads its argument as a byte offset (line 1, not 2)");
        // And a byte span crossing the break selects the same characters the
        // CRLF CString says it does: bytes 2..8 are "à\r\nà" = chars 1..4.
        ed.SetSel(2, 8);
        long from = 0, to = 0;
        tc->GetSelection(&from, &to);
        check(from == 1 && to == 4,
              "SetSel across a line break counts the break as TWO bytes, one char");
    }

    host->Destroy();
}

// ── 11c. The Complete Word popup's caret anchor (2026-08-31) ──────────────
// FunctionMenu (scripting/functionlist.cpp:663) positions the popup with
//   dlg.m_pt = editctrl.PosFromChar (n);  editctrl.ClientToScreen (&dlg.m_pt);
// and CCompleteWordDlg::OnInitDialog applies it with SetWindowPos(SWP_NOSIZE).
// All three shim members were inert, so the anchor was computed and thrown away
// three times over and the popup opened wherever the bridge put it.
//
// Only a live wx control exercises the first step, so it belongs here rather
// than in tests/test_mfc_ui.cpp (which pins the plumbing with a fake hook).
// No wxYield anywhere: SetSize/GetRect are synchronous on this side of the
// window server, and this harness exits from wxApp::OnInit.
void TestCaretAnchorChain()
{
    printf("Complete Word caret anchor (PosFromChar -> ClientToScreen -> SetWindowPos):\n");

    wxFrame* host = new wxFrame(nullptr, wxID_ANY, wxT("anchor-host"),
                                wxPoint(60, 70), wxSize(500, 300));

    wxTextCtrl* tc = new wxTextCtrl(host, wxID_ANY, wxEmptyString,
                                    wxPoint(0, 0), wxSize(400, 200),
                                    wxTE_MULTILINE | wxTE_DONTWRAP);
    tc->SetValue(wxT("first line\nsecond line\nthird line"));

    CEdit ed;
    ed.m_hWnd = (HWND) tc;
    // Byte offsets (the CEdit seam): for a MULTI-LINE control the seam counts
    // CRLF line breaks, matching the CRLF CString a canonical caller holds --
    // line starts are 0, 12, 25 for this ASCII text.  Two characters on the
    // same line must differ in x, and a character two lines down must be
    // exactly two rows lower -- a shim answering a constant (the old CPoint())
    // fails all of these.
    const int lineH = tc->GetCharHeight();
    check(lineH > 0, "the control reports a character height to build rows from");

    const CPoint a = ed.PosFromChar(3);    // line 0, column 3
    const CPoint b = ed.PosFromChar(7);    // line 0, column 7
    const CPoint c = ed.PosFromChar(28);   // line 2, column 3
    const CPoint home = ed.PosFromChar(25);  // line 2, column 0

    check(a.x > 0, "PosFromChar answers a real column offset, not 0");
    check(b.x > a.x && b.y == a.y,
          "...further along the same line is further right, on the same row");
    check(a.y == 0 && c.y == 2 * lineH,
          "...and the row is the line number times the character height");
    check(home.x == 0 && home.y == c.y,
          "...while the first character of a line sits at the left edge of it");

    // The byte/character seam: "wörld" starts at byte 7 but character 6, so a
    // shim that handed the byte offset to wx would measure one character too
    // much.  Same fixture shape as TestEditOffsetsAreBytes, and it is the whole
    // reason PosFromChar cannot simply forward its argument.
    {
        wxTextCtrl* u8 = new wxTextCtrl(host, wxID_ANY, wxEmptyString,
                                        wxPoint(0, 210), wxSize(400, 60));
        u8->SetValue(wxString::FromUTF8("h\xC3\xA9llo w\xC3\xB6rld"));
        CEdit ue;
        ue.m_hWnd = (HWND) u8;

        int wExpect = 0, hExpect = 0;
        u8->GetTextExtent(wxString::FromUTF8("h\xC3\xA9llo "), &wExpect, &hExpect);
        const CPoint w = ue.PosFromChar(7);   // byte 7 == character 6 == 'w'
        check(w.x == wExpect,
              "PosFromChar reads its argument as a byte offset, not a character one");
        check(ue.PosFromChar(13).x > w.x,
              "...and the end of the text is further right than its middle");
    }

    // Step 2: CEdit is a CWnd, not a CView, so this used to be a no-op.
    CPoint screen = a;
    ed.ClientToScreen(&screen);
    const wxPoint truth = tc->ClientToScreen(wxPoint(a.x, a.y));
    check(screen.x == truth.x && screen.y == truth.y,
          "ClientToScreen on a CEdit maps the anchor into screen coordinates");
    check(!(screen.x == a.x && screen.y == a.y),
          "...and the control is not at the screen origin, so the two differ");

    // Step 3: the position-only SetWindowPos the popup's OnInitDialog uses.
    wxFrame* popup = new wxFrame(nullptr, wxID_ANY, wxT("anchor-popup"),
                                 wxPoint(10, 20), wxSize(240, 160));
    CWnd pop;
    pop.m_hWnd = (HWND) popup;

    pop.SetWindowPos(nullptr, 300, 400, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    wxRect r = popup->GetRect();
    check(r.x == 300 && r.y == 400,
          "SetWindowPos(SWP_NOSIZE) moves the window to the anchor");
    check(r.width == 240 && r.height == 160,
          "...and leaves its size alone");

    pop.SetWindowPos(nullptr, 0, 0, 320, 180, SWP_NOMOVE | SWP_NOZORDER);
    r = popup->GetRect();
    check(r.width == 320 && r.height == 180,
          "SetWindowPos(SWP_NOMOVE) resizes it");
    check(r.x == 300 && r.y == 400,
          "...and leaves its position alone");

    // The always-on-top spelling must not move anything (mainfrm.cpp:1529).
    pop.SetWindowPos(&CWnd::wndTopMost, 0, 0, 0, 0,
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    r = popup->GetRect();
    check(r.x == 300 && r.y == 400 && r.width == 320 && r.height == 180,
          "...and a Z-order-only call changes neither");

    popup->Destroy();
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
                   bool, bool) = g_wxhook_file_dialog;
    g_wxhook_file_dialog = [](bool, const char*, const char*, const char*,
                              const char* filter, bool, bool) -> char* {
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

    // CComboBox::FindStringExact shares the same CB_FINDSTRINGEXACT contract
    // (wrap once past the end, fold case) — the headless double in
    // tests/test_mfc_ui.cpp mirrors this, but only the real hook, exercised
    // here, proves the shipping wx bridge actually honours it.
    {
        wxComboBox* combo = new wxComboBox(host, wxID_ANY);
        CComboBox cbox;
        cbox.m_hWnd = combo;
        cbox.AddString("red");
        cbox.AddString("green");
        cbox.AddString("blue");
        check(cbox.FindStringExact(-1, "blue") == 2, "CComboBox::FindStringExact finds");
        check(cbox.FindStringExact(2, "blue") == 2, "...wraps past the end");
        check(cbox.FindStringExact(-1, "BLUE") == 2, "...and folds case");
        check(cbox.FindStringExact(-1, "mauve") == CB_ERR, "...misses honestly");
        cbox.m_hWnd = nullptr;
    }

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

    // DecodeTextLenient is all-or-nothing per buffer: ONE stray CP1252 byte
    // sends the whole file down the Latin-1 fallback.  The file carried a 0x92
    // where an apostrophe belonged, so the popup rendered the invisible C1
    // control U+0092 instead of one.
    const wxString decoded = text ? wxString::FromUTF8(text, size) : wxString();
    check(!decoded.empty(),
          "...and the bundled bytes are valid UTF-8 (was: one CP1252 0x92)");
    check(decoded.Contains(wxT("locale's")),
          "...so the apostrophe in \"locale's\" is an apostrophe");
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

    // ── triggerlist truncation width goes negative at a narrow wrap column ─
    // world_debug.cpp computed the truncation width as (m_nWrapColumn - N)
    // with no floor; at a narrow enough column that goes zero or negative,
    // and CString::Left clamps n<=0 to empty, so every entry printed a bare
    // "..." with none of the actual pattern text.
    {
        CTrigger * pTrigger = new CTrigger;
        pTrigger->trigger  = "zzmatchzz";
        pTrigger->strLabel = "zztriggerzz";
        doc.GetTriggerArray().Add(pTrigger);

        doc.m_nWrapColumn = 20;
        doc.HyperlinkClicked("!!" DEBUG_PLUGIN_ID ":triggerlist()",
                             ACTION_SEND, true);
        check(sawLine("zzmatchzz"),
              "triggerlist shows the match text at a narrow wrap column "
              "(was: a bare \"...\")");
    }

    // ── a truncated match is the wrap BUDGET wide, not budget + "..." ──────
    // world_debug.cpp used to clamp the compare width (N) and the cut width
    // (N-3) to the 20-column floor INDEPENDENTLY, so at a narrow wrap column
    // both bottomed out at 20 and the "..." was appended on top: a long match
    // came back 23 columns wide (Left(20) + "..."), LONGER than the budget and
    // disagreeing with the portable twin's 20.  Cutting once, at budget-3,
    // keeps the result at the budget.
    {
        const std::string match = std::string(17, 'Q') + "XYZ"
                                + std::string(20, 'W');   // 40 chars, > budget
        CTrigger * pLong = new CTrigger;
        pLong->trigger  = match.c_str();
        pLong->strLabel = "zzlongzz";
        doc.GetTriggerArray().Add(pLong);

        doc.m_nWrapColumn = 20;                 // budget floors to 20; cut = 17
        doc.HyperlinkClicked("!!" DEBUG_PLUGIN_ID ":triggerlist()",
                             ACTION_SEND, true);

        // The listing wraps at column 20, so the match token spans several
        // CLines — reconstruct the whole output before matching.
        std::string all;
        for (POSITION p = doc.m_LineList.GetHeadPosition(); p; ) {
            const CLine* l = doc.m_LineList.GetNext(p);
            if (l && l->len > 0)
                all.append(l->text, l->len);
        }

        check(all.find(std::string(17, 'Q')) != std::string::npos,
              "triggerlist shows the Q run of a long match (not a bare \"...\")");
        check(all.find("XYZ") == std::string::npos,
              "triggerlist cuts a long match at the budget minus \"...\" "
              "(was: cut at the budget THEN \"...\" on top, 23 wide, revealing "
              "the 18th-20th characters)");
        check(all.find(match) == std::string::npos,
              "...and the long match really was truncated");
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

// ── 16. Connecting page, regexp recompile, sound browse, mapper alias (2026-08-19) ──
// Four more upstream defects in the same two mushgui-only TUs as section 15.

// ChangedPrefsP1 decides whether pressing OK on the World Properties
// "Connecting" page marks the world modified.  It compared nine of the ten
// members SavePrefsP1 writes, leaving out m_bUseSSL — so a user who ticked
// "Use SSL" and changed nothing else had it applied to the live document and
// thrown away at the next save decision.  The reloaded world then connected in
// the clear.
void TestUseSslIsAChange()
{
    printf("World-properties change detection (Use SSL):\n");

    CMUSHclientDoc doc;
    CPrefsP1 page1;

    doc.m_bUseSSL = 0;
    doc.LoadPrefsP1(page1);

    // The control.  Without it a gutted `return true` would pass everything
    // below.
    check(!doc.ChangedPrefsP1(page1),
          "an untouched Connecting page reports NO change");

    page1.m_bUseSSL = TRUE;
    check(doc.ChangedPrefsP1(page1),
          "ticking Use SSL IS a change (was: silently thrown away)");

    // ...and the other way round, from a world that already uses TLS.
    doc.m_bUseSSL = 1;
    doc.LoadPrefsP1(page1);
    check(!doc.ChangedPrefsP1(page1),
          "a TLS world round-trips as unchanged");
    page1.m_bUseSSL = FALSE;
    check(doc.ChangedPrefsP1(page1),
          "...and un-ticking it is a change too");

    // The member is an unsigned short and the dialog field a BOOL, so both
    // sides have to be normalised: any non-zero means the same thing.
    doc.m_bUseSSL = 1;
    doc.LoadPrefsP1(page1);
    page1.m_bUseSSL = 2;
    check(!doc.ChangedPrefsP1(page1),
          "...and a non-zero that is not 1 is still 'on', not a change");

    // A sibling term must still be compared, so a fix that replaced one rather
    // than adding one would show up.
    doc.LoadPrefsP1(page1);
    page1.m_port = doc.m_port + 1;
    check(doc.ChangedPrefsP1(page1),
          "changing the port is still a change (the other terms survive)");
}

// RecompileRegularExpressions runs over every trigger when the world's UTF-8
// flag changes.  It refreshed strRegexp only inside `if (pTrigger->regexp)` but
// called regcomp outside it, so a trigger whose compiled expression is NULL —
// which this function's own catch and EvaluateTrigger both produce — was
// recompiled from whatever the PREVIOUS trigger left in strRegexp, or from an
// empty pattern if it came first.  An empty pattern matches every line.
void TestRecompileKeepsEachTriggersOwnPattern()
{
    printf("Trigger regexp recompile (stale pattern):\n");

    CMUSHclientDoc doc;
    doc.m_mush_name   = "zz_selftest_recompile";
    doc.m_nWrapColumn = 80;
    doc.m_maxlines    = 5000;
    doc.SetUpOutputWindow();       // else a compile error has nowhere to note it

    CTrigger * pAlpha = new CTrigger;
    pAlpha->trigger  = "^alpha$";
    pAlpha->bRegexp  = true;
    pAlpha->regexp   = regcomp(pAlpha->trigger, 0);   // already compiled

    CTrigger * pBeta = new CTrigger;
    pBeta->trigger   = "^beta$";
    pBeta->bRegexp   = true;
    pBeta->regexp    = NULL;                          // the failed-compile state

    doc.m_TriggerMap.SetAt("alpha", pAlpha);
    doc.m_TriggerMap.SetAt("beta",  pBeta);

    check(doc.RecompileRegularExpressions() == 0,
          "both triggers recompile without error");

    // "Did nothing" fails here: pBeta would still be NULL.
    check(pAlpha->regexp != NULL && pBeta->regexp != NULL,
          "every trigger ends up with a compiled expression");

    // The map is a hash, so the iteration order is not ours to choose — but
    // under the defect BOTH orders are wrong: beta-first gives it the empty
    // pattern (which matches anything), alpha-first gives it "^alpha$".
    if (pAlpha->regexp && pBeta->regexp)
        {
        check(regexec(pAlpha->regexp, "alpha") != 0,
              "the already-compiled trigger still matches its own text");
        check(regexec(pAlpha->regexp, "beta") == 0,
              "...and not its neighbour's");
        check(regexec(pBeta->regexp, "beta") != 0,
              "the NULL-expression trigger matches ITS own text "
              "(was: the previous trigger's pattern)");
        check(regexec(pBeta->regexp, "alpha") == 0,
              "...and not its neighbour's (was: an empty pattern matching all)");
        }
}

// CPrefsP14::OnBrowseSounds — the world's beep sound — was the eighth "browse
// for a file" site, and the only one the lstrcpyn sweep missed, because it has
// no VER_PLATFORM_WIN32s ternary above it.  beep_sound is a script-settable
// alpha option with no length check, and the raw strcpy ran it straight past
// the end of a GetBuffer (_MAX_PATH) allocation.
void TestBeepSoundBrowseBoundsItsPath()
{
    printf("Beep sound browse (unbounded initial path):\n");

    static size_t s_leafLen = 0;
    static int s_calls = 0;
    char* (*saved)(bool, const char*, const char*, const char*, const char*,
                   bool, bool) = g_wxhook_file_dialog;
    g_wxhook_file_dialog = [](bool, const char*, const char*, const char* leaf,
                              const char*, bool, bool) -> char* {
        s_leafLen = leaf ? strlen(leaf) : 0;
        ++s_calls;
        return nullptr;                     // cancel — no file touched
    };

    struct Probe : CPrefsP14 { using CPrefsP14::OnBrowseSounds; };
    Probe page;
    page.m_strBeepSound = CString('x', 400);   // longer than the buffer

    s_calls = 0;
    page.OnBrowseSounds();

    check(s_calls == 1, "the file dialog was reached");
    check(s_leafLen == _MAX_PATH - 1,
          "an over-long beep-sound path is truncated to the buffer "
          "(was: 400 bytes copied into 261)");

    // An ordinary path must still arrive whole, so this is not a blanket clamp.
    page.m_strBeepSound = "beep.wav";
    s_calls = 0;
    page.OnBrowseSounds();
    check(s_calls == 1 && s_leafLen == strlen("beep.wav"),
          "an ordinary path still arrives in full");

    g_wxhook_file_dialog = saved;
}

// CMapDlg::AddAlias (Game ▸ Mapper ▸ Make alias) runs the same CAliasDlg as the
// world-properties Aliases page, but had its own copy of the dialog-to-CAlias
// field list — one that had fallen seven fields behind, so everything the user
// set in those seven was discarded.  Both now go through CopyAliasFromDialog.
void TestMapperAliasKeepsEveryDialogField()
{
    printf("Mapper 'make alias' (dropped dialog fields):\n");

    CMUSHclientDoc doc;

    static short s_seenSequence = 0;
    static bool  s_setEverything = false;
    intptr_t (*const savedRunDlg)(void*, unsigned) = g_run_mfc_dialog;
    g_run_mfc_dialog = [](void* self, unsigned idd) -> intptr_t {
        if (idd != IDD_EDIT_ALIAS)
            return IDCANCEL;
        CAliasDlg * d = (CAliasDlg *) self;
        s_seenSequence = d->m_iSequence;       // what AddAlias seeded it with
        d->m_name = "go_north";
        if (s_setEverything)
            {
            d->m_bTemporary             = TRUE;
            d->m_strVariable            = "last_move";
            d->m_iSequence              = 42;
            d->m_bKeepEvaluating        = TRUE;
            d->m_bOneShot               = TRUE;
            d->m_bEchoAlias             = TRUE;
            d->m_bOmitFromCommandHistory = TRUE;
            }
        return IDOK;
        };

    // The alias the dialog last added to the map (there is only ever one here).
    struct Finder {
        static CAlias * Only (CMUSHclientDoc & d) {
            CAlias * pFound = NULL;
            CString strName;
            for (POSITION pos = d.m_AliasMap.GetStartPosition(); pos;)
                d.m_AliasMap.GetNextAssoc(pos, strName, pFound);
            return pFound;
            }
        };

    struct Probe : CMapDlg { using CMapDlg::AddAlias; };

    // ── the seven dropped fields ───────────────────────────────────────────
    {
        Probe mapper;
        mapper.m_pDoc = &doc;
        s_setEverything = true;
        mapper.AddAlias("n");

        CAlias * pAlias = Finder::Only(doc);
        check(pAlias != NULL, "the alias reached the world's alias map");
        if (pAlias)
            {
            check(pAlias->name == "go_north" &&
                  pAlias->iSendTo == eSendToSpeedwalk,
                  "the fields it always copied are still copied");
            check(pAlias->bTemporary && pAlias->bKeepEvaluating &&
                  pAlias->bOneShot && pAlias->bEchoAlias &&
                  pAlias->bOmitFromCommandHistory,
                  "the five ticked boxes survive (was: the CAlias defaults)");
            check(pAlias->strVariable == "last_move",
                  "...as does the variable name");
            check(pAlias->iSequence == 42,
                  "...and the sequence the user typed");
            }
    }

    // ── the sequence the dialog is SEEDED with ─────────────────────────────
    // CAliasDlg's constructor zeroes m_iSequence while CAlias's uses
    // DEFAULT_ALIAS_SEQUENCE, so copying the field without seeding it first
    // would give every mapper alias a sequence of 0.
    {
        doc.m_AliasMap.RemoveAll();     // ownership: the CAlias above is leaked
        Probe mapper;
        mapper.m_pDoc = &doc;
        s_setEverything = false;
        s_seenSequence = -1;
        mapper.AddAlias("s");

        check(s_seenSequence == DEFAULT_ALIAS_SEQUENCE,
              "the dialog opens on the default sequence, not 0");
        CAlias * pAlias = Finder::Only(doc);
        check(pAlias && pAlias->iSequence == DEFAULT_ALIAS_SEQUENCE,
              "...so an untouched sequence lands as the default, not 0");
    }

    g_run_mfc_dialog = savedRunDlg;
}

// The Send-to-world and Paste pages validate their "delay in milliseconds"
// field at 0..10000, but paste_delay / send_file_delay are option-table entries
// bounded at 0..100000 — so a value a script or a world file legitimately set
// above 10000 made World Properties refuse to close, with no way to fix it in
// the dialog but to lower the number.  DDV_MinMaxLong is a real enforcer here.
// Runs one property page's data exchange, answering "did the validation refuse
// it?" — DDV_MinMaxLong signals that by throwing, the way MFC's Fail() does.
template <class T>
static bool DdxRefused (T & page, CDataExchange & dx)
{
    try { page.DoDataExchange(&dx); }
    catch (const CDdxValidationFail &) { return true; }
    return false;
}

void TestSendAndPasteDelaysAcceptTheOptionRange()
{
    printf("Send/Paste delay validation range:\n");

    // No control window, so the DDX calls are no-ops and only the DDV bounds
    // decide — which is exactly what is under test.  A refusal puts up a
    // message box first, and there is no event loop here to answer one, so the
    // hook stands in for it (and records that the box was asked for).
    CDataExchange dx;
    dx.m_bSaveAndValidate = TRUE;

    static int s_boxes = 0;
    int (*const savedBox)(const char*, const char*, unsigned) = g_wxhook_message_box;
    g_wxhook_message_box = [](const char*, const char*, unsigned) -> int {
        ++s_boxes;
        return IDOK;
    };

    struct P10 : CPrefsP10 { using CPrefsP10::DoDataExchange; };
    struct P13 : CPrefsP13 { using CPrefsP13::DoDataExchange; };

    {
        P10 page;
        page.m_nFileDelayPerLines = 1;       // its own bound is 1..100000
        page.m_iLineDelay = 50000;
        s_boxes = 0;
        check(!DdxRefused(page, dx) && s_boxes == 0,
              "Send-to-world accepts a 50000 ms delay (the option-table ceiling)");
        page.m_iLineDelay = 100001;
        check(DdxRefused(page, dx) && s_boxes == 1,
              "...but still refuses one past it (the bound was widened, not removed)");
        page.m_iLineDelay = -1;
        check(DdxRefused(page, dx),
              "...and still refuses a negative one");
    }

    {
        P13 page;
        page.m_nPasteDelayPerLines = 1;
        page.m_iLineDelay = 50000;
        s_boxes = 0;
        check(!DdxRefused(page, dx) && s_boxes == 0,
              "Paste accepts a 50000 ms delay too");
        page.m_iLineDelay = 100001;
        check(DdxRefused(page, dx) && s_boxes == 1,
              "...and refuses one past the ceiling");
    }

    g_wxhook_message_box = savedBox;
}

// ── 18. Owner-draw colour combos paint swatches, not names (2026-08-19) ────
// CBS_OWNERDRAWFIXED means the OWNER paints each row.  The bridge built every
// combo as a plain wxChoice/wxComboBox and never looked at the bit, so
// CColourComboBox::DrawItem — the code that fills each row with the custom
// colour it stands for — never ran, and the trigger/highlight/echo/text colour
// drop-downs listed "Custom3" where Windows shows a swatch.
//
// Five templates in the recovered resources carry the bit; all five must build
// owner-draw, and nothing else must.
//
// A CColourComboBox that also records the itemIDs it was handed.  DrawItem is
// virtual on CComboBox precisely so the bridge can reach it; overriding it here
// proves the canonical body really was called, and with which rows — so "the
// bridge silently drew nothing" cannot pass.
namespace {
class RecordingColourCombo : public CColourComboBox
{
public:
    mutable std::vector<int> drawn;
    void DrawItem(LPDRAWITEMSTRUCT dis) override
    {
        drawn.push_back((int) dis->itemID);
        CColourComboBox::DrawItem(dis);      // still paint, for real
    }
};

// The colour of one pixel of a rendered row.
bool RowPixel(const wxBitmap& bmp, int x, int y, wxColour& out)
{
    if (!bmp.IsOk()) return false;
    wxImage img = bmp.ConvertToImage();
    if (!img.IsOk() || x >= img.GetWidth() || y >= img.GetHeight()) return false;
    out.Set(img.GetRed(x, y), img.GetGreen(x, y), img.GetBlue(x, y));
    return true;
}
} // namespace

void TestOwnerDrawColourCombos()
{
    printf("Owner-draw colour combos (CBS_OWNERDRAWFIXED):\n");

    // (a) The control class follows the style bit.  IDD_EDIT_TRIGGER is the
    // ideal control: it carries BOTH kinds of read-only drop-down, built by the
    // same call in the same dialog — IDC_TRIGGER_COLOUR (0x50210213, the bit
    // set) and IDC_COLOUR_CHANGE_TYPE (0x50210003, the bit clear).
    const RcDialog* trig = MfcDialogResources().FindDialog(IDD_EDIT_TRIGGER);
    check(trig != nullptr, "IDD_EDIT_TRIGGER is in the recovered resources");
    if (!trig) return;

    wxDialog* dlg = BuildWxDialogFromTemplate(nullptr, *trig);
    wxWindow* colourWin = dlg->FindWindow(IDC_TRIGGER_COLOUR);
    wxWindow* plainWin  = dlg->FindWindow(IDC_COLOUR_CHANGE_TYPE);

    wxOwnerDrawnComboBox* odc = wxDynamicCast(colourWin, wxOwnerDrawnComboBox);
    check(odc != nullptr,
          "IDC_TRIGGER_COLOUR (CBS_OWNERDRAWFIXED) builds an owner-draw combo");
    check(plainWin != nullptr &&
          wxDynamicCast(plainWin, wxOwnerDrawnComboBox) == nullptr,
          "...while IDC_COLOUR_CHANGE_TYPE, same dialog, bit clear, does NOT");
    check(wxDynamicCast(plainWin, wxChoice) != nullptr,
          "...it is still the plain read-only wxChoice it always was");

    if (!odc) { dlg->Destroy(); return; }

    // (b) VALUE semantics are unchanged.  A wxOwnerDrawnComboBox is neither a
    // wxComboBox nor a wxChoice, so every CComboBox hook had to learn about it;
    // if one did not, GetCurSel answers CB_ERR and DDX_CBIndex quietly drops
    // the user's colour on OK.  Drive it exactly as the dialog does.
    RecordingColourCombo cb;
    CDataExchange dx;
    dx.m_pDlgWnd = CWnd::FromHandle((HWND) dlg);
    dx.m_bSaveAndValidate = FALSE;
    DDX_Control(&dx, IDC_TRIGGER_COLOUR, cb);      // binds m_hWnd + the paint hook
    check(cb.m_hWnd == (HWND) colourWin, "DDX_Control binds the owner-draw combo");

    cb.ResetContent();
    cb.AddString("(no change)");
    for (int i = 0; i < MAX_CUSTOM; ++i) {
        char name[32];
        snprintf(name, sizeof name, "Custom%d", i + 1);
        cb.AddString(name);
    }
    cb.AddString("Other ...");
    check(cb.GetCount() == MAX_CUSTOM + 2,
          "AddString reaches it (18 rows: no-change + 16 custom + other)");

    CString text;
    cb.GetLBText(3, text);
    check(text == "Custom3", "GetLBText reads a row back by index");
    check(cb.FindStringExact(-1, "Other ...") == MAX_CUSTOM + 1,
          "FindStringExact finds the last row");

    // The DDX round trip, both directions, with a non-degenerate index (7 is
    // neither 0 nor the count, so an off-by-one or a reset shows up).
    int member = 7;
    dx.m_bSaveAndValidate = FALSE;                 // member -> control
    DDX_CBIndex(&dx, IDC_TRIGGER_COLOUR, member);
    check(cb.GetCurSel() == 7, "DDX load selects the member's index");
    check(odc->GetSelection() == 7,
          "...and the wx control agrees (not just the shim's idea of it)");

    int readBack = -1;
    dx.m_bSaveAndValidate = TRUE;                  // control -> member
    DDX_CBIndex(&dx, IDC_TRIGGER_COLOUR, readBack);
    check(readBack == 7, "DDX save reads the same index back (was: -1, silently)");

    cb.SetCurSel(MAX_CUSTOM + 1);                  // "Other ..."
    dx.m_bSaveAndValidate = TRUE;
    DDX_CBIndex(&dx, IDC_TRIGGER_COLOUR, readBack);
    check(readBack == MAX_CUSTOM + 1, "...and follows a later user change");

    // Item data is how utils.choose keeps its keys straight; it must survive too.
    cb.SetItemData(2, 4242);
    check(cb.GetItemData(2) == 4242, "SetItemData/GetItemData round-trip");

    // (c) Something observable about the RENDERING.  Give the combo a palette
    // whose entries all differ, then render three rows and read the pixels back.
    for (int i = 0; i < MAX_CUSTOM; ++i) {
        cb.m_customtext[i] = RGB(0, 0, 0);
        cb.m_customback[i] = RGB(10 + i * 5, 20 + i * 5, 200 - i * 5);
    }
    cb.m_iOtherForeground = RGB(255, 255, 0);
    cb.m_iOtherBackground = RGB(8, 96, 160);

    const int W = 80, H = 16;
    cb.drawn.clear();

    // Row 0 is "(no change)": the canonical body offsets itemID by one, so it
    // falls past the custom range and draws black-on-white.
    wxBitmap b0 = wx_render_owner_draw_combo_item(colourWin, 0, 0, W, H);
    // Row 3 is Custom3 -> m_customback[2].
    wxBitmap b3 = wx_render_owner_draw_combo_item(colourWin, 3, 0, W, H);
    // Row 17 is "Other ..." -> m_iOtherBackground.
    wxBitmap b17 = wx_render_owner_draw_combo_item(colourWin, MAX_CUSTOM + 1, 0, W, H);

    check(cb.drawn.size() == 3,
          "the canonical DrawItem ran once per rendered row (was: never)");
    check(cb.drawn.size() == 3 && cb.drawn[0] == 0 && cb.drawn[1] == 3 &&
          cb.drawn[2] == MAX_CUSTOM + 1,
          "...with the itemIDs it was asked for, unshifted");

    // Sample the far right edge: the label is drawn from the left, so this is
    // background in every row and cannot be confused with glyph pixels.
    wxColour c0, c3, c17;
    check(RowPixel(b0, W - 2, H / 2, c0) &&
          RowPixel(b3, W - 2, H / 2, c3) &&
          RowPixel(b17, W - 2, H / 2, c17),
          "every rendered row produced a readable bitmap");
    check(c3 == wxColour(10 + 2 * 5, 20 + 2 * 5, 200 - 2 * 5),
          "Custom3's row is filled with m_customback[2] (the swatch)");
    check(c17 == wxColour(8, 96, 160),
          "\"Other ...\" is filled with m_iOtherBackground");
    check(c0 == wxColour(255, 255, 255),
          "\"(no change)\" stays white - the row-0 offset is honoured");
    check(!(c3 == c17) && !(c3 == c0),
          "the three rows really differ (a no-op painter would tie them)");

    // A control the bridge did NOT build owner-draw must not answer the painter
    // — otherwise the class check above could pass while everything renders.
    check(!wx_render_owner_draw_combo_item(plainWin, 1, 0, W, H).IsOk(),
          "the plain combo has no owner-draw painter at all");

    // (c2) Picking a row must still reach the canonical CBN_SELCHANGE handler —
    // that is what opens the colour picker for "Other ...", and what keeps the
    // two swatch buttons beside the combo in step.  The modal runner binds
    // wxEVT_COMBOBOX on the DIALOG (wx_mfc_impl.cpp), so the control's event has
    // to propagate up to it; a control parented anywhere else would silently
    // stop firing the handler.  wxOwnerDrawnComboBox raises wxEVT_COMBOBOX, the
    // same event as wxComboBox, so the existing bind is the right one.
    check(colourWin->GetParent() == dlg,
          "the owner-draw combo is a child of the dialog (events can reach it)");
    // `seen` outlives the binding: the handler stays attached until the dialog
    // is destroyed below, so it must not capture anything shorter-lived.
    int seen = -1;
    dlg->Bind(wxEVT_COMBOBOX, [&seen](wxCommandEvent& e) { seen = e.GetId(); });
    wxCommandEvent sel(wxEVT_COMBOBOX, colourWin->GetId());
    sel.SetEventObject(colourWin);
    colourWin->GetEventHandler()->ProcessEvent(sel);
    check(seen == IDC_TRIGGER_COLOUR,
          "...and a selection from it propagates there as CBN_SELCHANGE");

    dlg->Destroy();

    // (d) The other four owner-draw templates light up too — this fix is not
    // just the trigger dialog.  IDD_TEST_RESULT's copy is WS_DISABLED
    // (0x58210213), which must not change the control class.
    struct { int dlgId; int ctlId; const char* what; } rest[] = {
        { IDD_PREFS_P9,  IDC_ECHO_COLOUR,    "World > Commands echo colour" },
        { IDD_PREFS_P17, IDC_TEXT_COLOUR,    "World > Scripting error colour" },
        { IDD_HIGHLIGHT, IDC_TRIGGER_COLOUR, "Highlight Phrase colour" },
        { IDD_TEST_RESULT, IDC_TRIGGER_COLOUR, "Test Result colour (disabled)" },
    };
    for (const auto& r : rest)
    {
        const RcDialog* t = MfcDialogResources().FindDialog(r.dlgId);
        if (!t) { check(false, r.what); continue; }
        wxDialog* d = BuildWxDialogFromTemplate(nullptr, *t);
        check(wxDynamicCast(d->FindWindow(r.ctlId), wxOwnerDrawnComboBox) != nullptr,
              r.what);
        d->Destroy();
    }
}

int RunGuiBridgeSelfTest()
{
    printf("GUI-BRIDGE-SELFTEST: geometry, dialog controls, world defaults\n");

    TestGeometry();
    TestListBoxStyles();
    TestSortedListStyles();
    TestEditStyleValidators();
    TestDefaultButtons();
    TestNewWorldDefaults();
    TestProgressDialog();
    TestMessageBoxDecode();
    TestDialogKeyTranslation();
    TestPopupMenuBuild();
    TestRecentWorlds();
    TestClipboardRoundTrip();
    TestFunctionListView();
    TestDdxCharsetRoundTrip();
    TestMultiLineTriggerSeam();
    TestEditOffsetsAreBytes();
    TestCaretAnchorChain();
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
    TestUseSslIsAChange();
    TestRecompileKeepsEachTriggersOwnPattern();
    TestBeepSoundBrowseBoundsItsPath();
    TestMapperAliasKeepsEveryDialogField();
    TestSendAndPasteDelaysAcceptTheOptionRange();
    TestOwnerDrawColourCombos();

    printf("GUI-BRIDGE-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#endif // !_WIN32
