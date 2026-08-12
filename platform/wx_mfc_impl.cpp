// platform/wx_mfc_impl.cpp
// wxWidgets-backed implementations of the MFC compatibility hook functions.
//
// This is the ONLY file that includes both <wx/wx.h> and the MUSHclient
// MFC-compatibility headers.  It bridges the two worlds via the function
// pointers declared in wx_mfc_hooks.h.
//
// Include order is critical:
//   1. wx headers first (define wxUSE_*, etc.)
//   2. Then MUSHclient headers (compat.h, MUSHclient.h, …)
// This avoids conflicts between wx's and our own min/max, MessageBox, etc.

#ifndef _WIN32

// ── wxWidgets ─────────────────────────────────────────────────────────────
#include <wx/wx.h>
#include <wx/mdi.h>
#include <wx/config.h>     // wxConfig
#include <wx/spinctrl.h>
#include <wx/spinbutt.h>   // wxSpinButton + wxEVT_SPIN (CSpinButtonCtrl bridge)
#include <wx/filename.h>
#include <wx/fontpicker.h>
#include <wx/dnd.h>        // wxFileDropTarget
#include <wx/dirdlg.h>     // wxDirDialog
#include <wx/notebook.h>   // wxNotebook (flat property-sheet bridge: hook_run_property_sheet_flat)
#include <wx/hyperlink.h>  // wxHyperlinkCtrl (CStaticLink substitution)
#include <wx/clipbrd.h>    // wxTheClipboard
#include <wx/dataobj.h>    // wxTextDataObject
#include <wx/colordlg.h>   // wxColourDialog
#include <wx/fontdlg.h>    // wxFontDialog, wxFontData
#include <wx/treebook.h>   // wxTreebook (World Properties property sheet)
#include <wx/listctrl.h>   // wxListCtrl (entity-manager object lists)
#include <wx/stdpaths.h>   // wxStandardPaths (bundled text resources)
#include <wx/fontenum.h>   // wxFontEnumerator (utils.getfontfamilies)
#include <wx/settings.h>   // wxSystemSettings (world.GetSysColor / GetSystemMetrics)
#include <wx/dcbuffer.h>   // wxAutoBufferedPaintDC (config sheet caption band)
#include <wx/progdlg.h>    // wxProgressDialog (CProgressDlg's port backing)
#include <wx/display.h>    // wxDisplay (screen-size system metrics)
#include <algorithm>
#include <unordered_map>
#include <map>
#include <string>
#include <fstream>         // bundled "TEXT" resources (licence / credits)
#include <sstream>
#include <vector>
#include <filesystem>      // first-run seeding of bundled plugins (Discworld edition)

// ── MUSHclient headers (MFC compat layer) ────────────────────────────────
// Must come AFTER wx so our stubs don't shadow wx internals inside this TU.
#include "stdafx.h"        // pulls in compat.h → mfc_ui.h → wx_mfc_hooks.h
#include "MUSHclient.h"    // CMUSHclientApp full definition
#include "mainfrm.h"       // CMainFrame full definition
#include "doc.h"           // CMUSHclientDoc (needed for hook_new_world)
#include "TextDocument.h"             // CTextDocument (for hook_create_text_window)
#include "platform/app_config.h"   // OpenAppConfig -- never the raw wxConfig
#include <memory>
#include "platform/wx_world_child.h"  // CWorldChildFrame
#include "StatLink.h"                 // CStaticLink — the only real SubclassDlgItem client
#include "platform/wx_osx_text.h"     // wxOSXDisableSmartSubstitutions (macOS smart-quote fix)
#include "platform/wx_activity.h"     // ShowActivityWindow (Game ▸ Activity List)
#include "platform/ma_sound.h"        // miniaudio sound engine (volume/pan/buffers)
#include "platform/wx_notepad.h"      // CWxNotepadFrame
#include "platform/wx_dialog_bridge.h" // MfcDialogResources, BuildWxDialogFromTemplate
#include "platform/posix_transport.h"  // pal::PumpTransportCompletions (poll timer)
#include "platform/menu_ui_rules.h"    // MenuCommandUiState / PortMenuActionForCommandId
#include "platform/wx_mfc_logic.h"     // the pure decisions lifted out of the hooks below
#include "dialogs/world_prefs/TreePropertySheet.h" // port CTreePropertySheet (page list)
#include "dialogs/world_prefs/GenPropertyPage.h"    // CGenPropertyPage (entity-list idle reload)
#include "dialogs/ImmediateDlg.h"     // CImmediateDlg (canonical Execute-Lua dialog)
#include "dialogs/plugins/PluginsDlg.h" // CPluginsDlg (canonical plugin manager)
#include "platform/wx_gdi_compat.h"   // PaintOwnerDrawButton (real CDC lives there)
#include "scripting/scripting.h"     // CScriptEngine

// Lua C API — included after MUSHclient headers to avoid wx/macro conflicts.
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// Hook pointer definitions live in platform/wx_mfc_hooks_storage.cpp (compiled
// into both mushcore and mushapp) so that the tests can link without
// needing wx_mfc_impl.cpp.  wx_mfc_install_hooks() below assigns them all.

// ── Status bar pointer ─────────────────────────────────────────────────────
// Populated by hook_load_mdi_frame; used by hook_set_status.
static wxStatusBar* s_statusBar = nullptr;

// ── Doc → frame registry ───────────────────────────────────────────────────
// Maps each live CMUSHclientDoc* to its CWorldChildFrame so that hooks
// (e.g. hook_game_preferences) can look up the output pane without iterating
// the MDI child hierarchy (which is platform-dependent and unreliable).
static std::unordered_map<CMUSHclientDoc*, CWorldChildFrame*> s_docFrameMap;

// Forward declaration — defined in the World-list persistence section below.

// ── Recent Worlds list ────────────────────────────────────────────────────────
// Tracks up to kMaxRecentWorlds file paths most recently opened or saved,
// persisted to wxConfig at /recent/0 … /recent/N-1.  Shown in File → Open
// Recent so the user can quickly reopen worlds without browsing.
//
// This is distinct from App.m_strWorldList (auto-reopen on launch): that list
// controls what opens automatically; the Recent menu is a manual affordance.

static const int  kMaxRecentWorlds = 10;
static const int  kRecentMenuBase  = wxID_HIGHEST + 300;

// ── Configure menu IDs ────────────────────────────────────────────────────────
static const int ID_MENU_CFG_TRIGGERS  = wxID_HIGHEST + 400;
static const int ID_MENU_CFG_ALIASES   = wxID_HIGHEST + 401;
static const int ID_MENU_CFG_TIMERS    = wxID_HIGHEST + 402;
static const int ID_MENU_CFG_VARIABLES = wxID_HIGHEST + 403;
static const int ID_MENU_CFG_MACROS    = wxID_HIGHEST + 404;
static const int ID_MENU_CFG_COLORS    = wxID_HIGHEST + 405;
static const int ID_MENU_CFG_INFO      = wxID_HIGHEST + 406;

// Import / Export set IDs (one pair per set type)
static const int ID_MENU_IMPORT_TRIGGERS = wxID_HIGHEST + 420;
static const int ID_MENU_EXPORT_TRIGGERS = wxID_HIGHEST + 421;
static const int ID_MENU_IMPORT_ALIASES  = wxID_HIGHEST + 422;
static const int ID_MENU_EXPORT_ALIASES  = wxID_HIGHEST + 423;
static const int ID_MENU_IMPORT_TIMERS   = wxID_HIGHEST + 424;
static const int ID_MENU_EXPORT_TIMERS   = wxID_HIGHEST + 425;
static const int ID_MENU_IMPORT_COLORS   = wxID_HIGHEST + 426;
static const int ID_MENU_EXPORT_COLORS   = wxID_HIGHEST + 427;
static const int ID_MENU_IMPORT_MACROS   = wxID_HIGHEST + 428;
static const int ID_MENU_EXPORT_MACROS   = wxID_HIGHEST + 429;

// ── Connection / logging IDs ──────────────────────────────────────────────────
static const int ID_CONN_LOG_TOGGLE    = wxID_HIGHEST + 410;

// ── Game menu IDs ─────────────────────────────────────────────────────────────
static const int ID_MENU_GAME_LUA_EVAL = wxID_HIGHEST + 411;

// ── View menu IDs (port-local; real wx implementations, not canonical) ────────
// +420..422 are the Configure ▸ Import/Export items (above); the View toggles
// must use a distinct range or wx can't tell the commands apart (the View branch
// ran first, so Import/Export silently toggled View state and were unreachable).
static const int ID_MENU_VIEW_ALWAYSONTOP = wxID_HIGHEST + 430;

// ── Display menu: the macOS-shaped clear ──────────────────────────────────────
// Port-local, with no canonical counterpart: Windows has exactly one clear
// (Display ▸ Clear Output Buffer, which erases everything).  Terminal.app users
// expect ⌘K to keep the current prompt and throw away what is above it, which
// is a different operation and a different cut point -- see
// CMUSHclientDoc::CountLinesBeforePromptBlock.
static const int ID_MENU_DISPLAY_CLEAR_TO_START = wxID_HIGHEST + 431;
static const int ID_MENU_DISPLAY_CLEAR_TO_MARK  = wxID_HIGHEST + 432;
static const int ID_MENU_DISPLAY_CLEAR_SCROLLBACK = wxID_HIGHEST + 433;
static const int ID_MENU_DISPLAY_CLEAR_SCREEN     = wxID_HIGHEST + 434;

// Declared on CWorldChildFrame (it owns the commands) so the in-app selftest
// checks the same strings the menu is built from.  "Ctrl" is Command on macOS;
// "RawCtrl" is the physical Control key.
const char* const CWorldChildFrame::kAccelClearToStart    = "Ctrl+K";
const char* const CWorldChildFrame::kAccelClearToMark     = "Ctrl+L";
const char* const CWorldChildFrame::kAccelClearScrollback = "Alt+Ctrl+K";
const char* const CWorldChildFrame::kAccelClearScreen     = "RawCtrl+Ctrl+L";
// Canonical command ids, not private wxIDs: world.DoCommand("FullScreenMode")
// and world.DoCommand("ViewStatusbar") post these through
// Frame.PostMessage -> hook_post_message -> wxEVT_MENU, and accelerators.cpp
// publishes both names to scripts.  With private ids those commands returned
// eOK and did nothing.  (resource.h:1437 ID_VIEW_FULLSCREENMODE,
// mfc_ui.h:861 ID_VIEW_STATUS_BAR.)
static const int ID_MENU_VIEW_FULLSCREEN  = 32332;

// The window the user can actually see.  wxOSX hides the wxMDIParentFrame
// while a child exists -- the active child IS the on-screen window -- so
// window-level chrome commands must act on the child or they act on nothing.
// The !IsShown() guard is load-bearing in both directions: with no world open
// the parent is genuinely visible and is the right target, and on GTK/MSW the
// child is not a top-level window at all.  Same reasoning as
// SaveMainWindowGeometry, which already picks its target this way.
static wxTopLevelWindow* wx_visible_frame(wxMDIParentFrame* frame)
{
    if (!frame) return nullptr;
    if (!frame->IsShown())
        if (auto* child = wxDynamicCast(frame->GetActiveChild(), wxTopLevelWindow))
            if (child->IsShown())
                return child;
    return frame;
}

// The rect the visible window had before it went full screen.  macOS exits
// full screen asynchronously, so at quit time GetRect() still reports the
// full-screen bounds; without this, quitting while full screen would persist
// an off-screen rect and the next launch would come up invisible.  Windows
// avoids the same trap by un-full-screening before saving (mainfrm.cpp:549).
// Windows has ONE status bar, on CMainFrame, and MFC persists its visibility
// with SaveBarState/LoadBarState (both inert shims here).  The port gives each
// world frame its own bar because the parent's is hidden, so "show the status
// bar" has to be an app-wide setting applied to every frame -- otherwise the
// menu item toggles one window and the tick lies about the rest.
static bool   s_statusBarVisible     = true;
static bool   s_hadPreFullScreenRect = false;
static wxRect s_preFullScreenRect;

// Read by CWorldChildFrame's constructor so a world opened later comes up
// matching the current setting instead of always showing its bar.
bool wx_status_bar_visible() { return s_statusBarVisible; }

static void apply_status_bar_visibility(wxMDIParentFrame* frame)
{
    if (!frame) return;
    for (wxWindow* w : frame->GetChildren())
        if (auto* child = dynamic_cast<CWorldChildFrame*>(w))
            child->ShowStatusBarStrip(s_statusBarVisible);
}
static const int ID_MENU_VIEW_STATUSBAR   = 0xE801;

// View ▸ Always On Top.  Windows pins the MAIN frame (mainfrm.cpp
// OnViewAlwaysontop -> SetWindowPos(&wndTopMost)), which pins every world with
// it because they are MDI children INSIDE that frame.  On macOS each world is a
// real top-level window and the MDI parent is hidden while any child exists (see
// the macOS MDI geometry note), so pinning only the parent pins nothing you can
// see.  Pin the parent AND every world frame, which is the same user-visible
// result Windows gets.
static void apply_always_on_top(wxMDIParentFrame* frame, bool bOnTop)
{
    if (!frame) return;
    auto pin = [bOnTop](wxTopLevelWindow* w) {
        if (!w) return;
        long style = w->GetWindowStyleFlag();
        w->SetWindowStyleFlag(bOnTop ? (style |  wxSTAY_ON_TOP)
                                     : (style & ~wxSTAY_ON_TOP));
    };
    pin(frame);
    for (wxWindow* w : frame->GetChildren())
        if (auto* tlw = dynamic_cast<wxTopLevelWindow*>(w))
            pin(tlw);
}

static wxMenu*    s_recentMenu     = nullptr;
static std::vector<std::string> s_recentWorlds;

// Load the recent list out of `cfg`.  The config object is a parameter, not a
// local, so a test can round-trip this against a scratch file rather than the
// user's real preferences (same reason SaveWindowGeometryTo takes one).
//
// The walk STOPS at the first missing-or-empty slot, which is what makes the
// tail deletion in RecentWorldsSaveTo load-bearing rather than tidy-up.
void RecentWorldsLoadFrom(wxConfigBase& cfg, std::vector<std::string>& out,
                          int maxEntries)
{
    out.clear();
    for (int i = 0; i < maxEntries; ++i) {
        wxString key = wxString::Format(wxT("/recent/%d"), i);
        wxString val;
        if (!cfg.Read(key, &val) || val.IsEmpty()) break;
        out.push_back(std::string(val.ToUTF8()));
    }
}

// Persist `list` into `cfg`, deleting every slot past its end: a shorter list
// written over a longer one must not leave the old tail readable, or the next
// load resurrects worlds the user removed.
void RecentWorldsSaveTo(wxConfigBase& cfg, const std::vector<std::string>& list,
                        int maxEntries)
{
    for (int i = 0; i < maxEntries; ++i) {
        wxString key = wxString::Format(wxT("/recent/%d"), i);
        if (i < (int)list.size())
            cfg.Write(key, wxString::FromUTF8(list[i].c_str()));
        else
            cfg.DeleteEntry(key);
    }
}

// Move `path` to the front, dropping any earlier occurrence, and cap the list.
void RecentWorldsPromote(std::vector<std::string>& list,
                         const std::string& path,
                         int maxEntries)
{
    if (path.empty()) return;
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    list.insert(list.begin(), path);
    if ((int)list.size() > maxEntries)
        list.resize(maxEntries);
}

// The live list and its menu-id block, for the menu-bar selftest.
const std::vector<std::string>& RecentWorldsList() { return s_recentWorlds; }
int RecentWorldsMaxEntries()  { return kMaxRecentWorlds; }
int RecentWorldsMenuIdBase()  { return kRecentMenuBase; }

// Load the recent list from the user's config into s_recentWorlds.
static void recent_worlds_load()
{
    const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
    RecentWorldsLoadFrom(cfg, s_recentWorlds, kMaxRecentWorlds);
}

// Persist the current s_recentWorlds to the user's config.
static void recent_worlds_save()
{
    const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
    RecentWorldsSaveTo(cfg, s_recentWorlds, kMaxRecentWorlds);
}

// Rebuild `menu` from `list`.  Takes both as parameters so a test can drive it
// with a list of its own: the live one comes from the user's config, so an
// assertion against it alone would be strong or vacuous depending on whose
// machine it runs on.
void RecentWorldsBuildMenu(wxMenu* menu, const std::vector<std::string>& list,
                           int idBase)
{
    if (!menu) return;
    while (menu->GetMenuItemCount() > 0)
        menu->Delete(menu->FindItemByPosition(0));
    if (list.empty()) {
        // Disabled, and it must stay that way: it occupies the same id as the
        // first real entry, so an enabled placeholder would try to open a world
        // called "(no recent worlds)".
        wxMenuItem* dummy = menu->Append(idBase, wxT("(no recent worlds)"));
        dummy->Enable(false);
        return;
    }
    for (int i = 0; i < (int)list.size(); ++i) {
        wxString path  = wxString::FromUTF8(list[i].c_str());
        wxString label = wxString::Format(wxT("&%d  %s"),
                                          (i + 1) % 10,
                                          wxFileName(path).GetFullName());
        menu->Append(idBase + i, label);
    }
}

// Rebuild s_recentMenu from s_recentWorlds.
static void recent_worlds_update_menu()
{
    RecentWorldsBuildMenu(s_recentMenu, s_recentWorlds, kRecentMenuBase);
}

// Prepend path to s_recentWorlds (deduplicated), persist, and update the menu.
static void recent_worlds_add(const std::string& path)
{
    if (path.empty()) return;
    RecentWorldsPromote(s_recentWorlds, path, kMaxRecentWorlds);
    recent_worlds_save();
    recent_worlds_update_menu();
}

void wx_register_world_frame(CMUSHclientDoc* doc, CWorldChildFrame* frame)
{
    if (doc) s_docFrameMap[doc] = frame;
}

void wx_unregister_world_frame(CMUSHclientDoc* doc)
{
    if (doc) s_docFrameMap.erase(doc);
}

void wx_activate_world_frame(CMUSHclientDoc* doc)
{
    auto it = s_docFrameMap.find(doc);
    if (it == s_docFrameMap.end() || !it->second) return;
    CWorldChildFrame* frame = it->second;
    if (wxWindow* top = wxGetTopLevelParent(frame))
        top->Raise();                 // bring the app's main window forward
    frame->Activate();                // make it the active MDI child
}

// ── Helper: cast void* → wxWindow* ───────────────────────────────────────
static inline wxWindow* toWx(void* h) { return static_cast<wxWindow*>(h); }
static inline const wxWindow* toWxC(const void* h) { return static_cast<const wxWindow*>(h); }

// ── Helper: apply world font settings to an output pane ──────────────────
// (The world-list add/remove helpers that used to live here are gone: nothing
// in the port writes App.m_strWorldList any more.  Global Preferences does, via
// the canonical page, exactly as on Windows.)

// ── Hook implementations ──────────────────────────────────────────────────

static void hook_invalidate(void* h, bool erase)
{
    if (auto* w = toWx(h)) w->Refresh(erase);
}

static void hook_show_window(void* h, int nCmd)
{
    auto* w = toWx(h);
    if (!w) return;
    auto* tl = wxDynamicCast(w, wxTopLevelWindow);
    switch (nCmd) {
    case 0:           w->Hide(); break;                                       // SW_HIDE
    case 2: case 6:   w->Show(); if (tl) tl->Iconize(true);  break;           // SW_SHOWMINIMIZED / SW_MINIMIZE
    case 3:           w->Show(); if (tl) tl->Maximize();     break;           // SW_MAXIMIZE
    case 9:           w->Show(); if (tl) { tl->Iconize(false); tl->Maximize(false); } break; // SW_RESTORE
    default:          w->Show(); break;                                       // SW_SHOW / SW_SHOWNORMAL / …
    }
}

static void hook_update_window(void* h)
{
    if (auto* w = toWx(h)) { w->Update(); w->Refresh(); }
}

static void hook_get_client_rect(const void* h, RECT* r)
{
    if (!r) return;
    if (auto* w = toWxC(h)) {
        wxSize sz = w->GetClientSize();
        r->left = 0; r->top = 0;
        r->right = sz.x; r->bottom = sz.y;
    } else {
        r->left = r->top = r->right = r->bottom = 0;
    }
}

static void hook_set_window_text(void* h, const char* text)
{
    auto* w = toWx(h);
    if (!w) return;
    wxString s = wxString::FromUTF8(text ? text : "");
    // wxTextCtrl / wxComboBox ignore SetLabel — their text is the *value*.  This
    // is what DDX_Text(...FALSE) calls to push a member into an edit control, so
    // getting it wrong leaves every edit field blank.  ChangeValue (not SetValue)
    // avoids firing a change event during the DDX load.
    if (wxTextCtrl* tc = wxDynamicCast(w, wxTextCtrl))
        tc->ChangeValue(s);
    else if (wxComboBox* cb = wxDynamicCast(w, wxComboBox))
        cb->SetValue(s);
    else if (wxChoice* ch = wxDynamicCast(w, wxChoice))
        ch->SetStringSelection(s);   // read-only pop-up: select the matching item
    // Static text: set the label then re-wrap to the control's (fixed) width,
    // since MFC statics word-wrap within their rectangle and wxStaticText clips.
    else if (wxStaticText* st = wxDynamicCast(w, wxStaticText))
    {
        int width = st->GetSize().GetWidth();
        st->SetLabel(s);
        if (width > 0) st->Wrap(width);
    }
    else
        w->SetLabel(s);
}

static wxWindow* resolveTarget(void* h);   // defined below; null h → main frame

static void hook_move_window(void* h, int x, int y, int w, int ht, bool repaint)
{
    // Use resolveTarget (not toWx) so a null handle resolves to the MDI parent
    // frame — that is how world.MoveMainWindow reaches the main window, whose
    // Frame.m_hWnd is always null on the port.  For non-null handles
    // resolveTarget == toWx, so other callers are unaffected.
    if (auto* wnd = resolveTarget(h)) {
        wnd->SetSize(x, y, w, ht);
        if (repaint) wnd->Refresh();
    }
}

static void hook_destroy_window(void* h)
{
    if (auto* w = toWx(h)) w->Destroy();
}

static void hook_set_focus(void* h)
{
    if (auto* w = toWx(h)) w->SetFocus();
}

// Helper: resolve the target window, falling back to the MDI parent frame
// when h is null.  Frame.m_hWnd is always null on non-Windows (the real
// window is stored in App.m_pMainWnd->m_hWnd by hook_load_mdi_frame).
static wxWindow* resolveTarget(void* h)
{
    if (h) return toWx(h);
    return (App.m_pMainWnd && App.m_pMainWnd->m_hWnd)
         ? static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd)
         : nullptr;
}

static long hook_send_message(void* h, unsigned msg, uintptr_t wp, intptr_t lp)
{
    // Route WM_COMMAND messages to the wx event system.
    // Other messages are currently ignored (they trigger Windows-only paths).
    if (msg == WM_COMMAND) {
        if (auto* w = resolveTarget(h)) {
            wxCommandEvent evt(wxEVT_MENU, (int)wp);
            w->GetEventHandler()->ProcessEvent(evt);
        }
    }

    // List-view extended styles.  This is the ONLY mechanism the canonical code
    // has for the "Show grid lines in list views" global preference: every one
    // of its call sites (MUSHclient.cpp RestoreColumnConfiguration, the colour
    // picker, the plugin wizard, genpropertypage) applies it by sending
    // LVM_SETEXTENDEDLISTVIEWSTYLE.  Swallowing the message made the option
    // inert in every list in the app -- and it ships ON by default, so this was
    // wrong for everyone, not just for people who changed it.
    //
    // MASK TO LVS_EX_GRIDLINES AND NOTHING ELSE.  The Win32 LVS_EX_* bits
    // collide numerically with wx's VIEW-MODE bits:
    //     0x01 LVS_EX_GRIDLINES      == wxLC_VRULES   (what we want)
    //     0x10 LVS_EX_HEADERDRAGDROP == wxLC_LIST
    //     0x20 LVS_EX_FULLROWSELECT  == wxLC_REPORT
    // The canonical caller passes 0x31, so handing the raw value to wx would
    // set wxLC_LIST|wxLC_REPORT and flip every entity list out of report mode,
    // losing all its columns.
    if (msg == LVM_SETEXTENDEDLISTVIEWSTYLE || msg == LVM_GETEXTENDEDLISTVIEWSTYLE) {
        wxListCtrl* lc = wxDynamicCast((wxWindow*) h, wxListCtrl);
        if (!lc) return 0;
        const long kRules = wxLC_HRULES | wxLC_VRULES;
        if (msg == LVM_GETEXTENDEDLISTVIEWSTYLE)
            return (lc->GetWindowStyleFlag() & kRules) ? LVS_EX_GRIDLINES : 0;
        // Report what we actually honour, so the canonical read-modify-write
        // (GET | new bits -> SET) stays consistent instead of re-arming the
        // "getter lies, and the value gets stored" trap.
        lc->SetSingleStyle(kRules, (lp & LVS_EX_GRIDLINES) != 0);
        return 0;
    }

    // NOT handled, deliberately: LVM_GET/SETCOLUMNORDERARRAY (saved column
    // order).  They would map to wxListCtrl::Get/SetColumnsOrder, which on this
    // platform are unimplemented stubs -- SetColumnsOrder disassembles to
    // `mov w0,#0; ret`, GetColumnsOrder returns an empty array -- so a case here
    // would report success and change nothing.  Worse than the gap: the
    // canonical save path (MUSHclient.cpp SaveColumnConfiguration) pre-seeds the
    // array with identity "in case our SendMessage doesn't work", so it already
    // succeeds silently, and the backlog would read FIXED.
    // See docs/LIST-CONTROL-DECISION.md for the full analysis, the control swap
    // that WOULD fix it, and what that costs.

    (void)lp;
    return 0;
}

static int hook_post_message(void* h, unsigned msg, uintptr_t wp, intptr_t lp)
{
    if (msg == WM_COMMAND) {
        if (auto* w = resolveTarget(h)) {
            wxCommandEvent* evt = new wxCommandEvent(wxEVT_MENU, (int)wp);
            w->GetEventHandler()->QueueEvent(evt);
            (void)lp;
            return TRUE;
        }
    }

    // ── TLS handshake failure fallback prompt ─────────────────────────────────
    // Sent by doc.cpp::ContinueSSLHandshake() when the TLS handshake fails and
    // the user should be asked whether to retry without encryption.
    // wp is the CMUSHclientDoc* that failed; we defer the dialog to the next
    // event-loop iteration so we're not inside a socket callback stack.
    if (msg == WM_USER_SSL_FALLBACK_PROMPT && wp != 0) {
        CMUSHclientDoc* doc = reinterpret_cast<CMUSHclientDoc*>(wp);
        bool certFailure = (lp == 1);   // 1 = cert verification, 0 = handshake
        wxWindow* parent = App.m_pMainWnd
                         ? static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd)
                         : nullptr;
        wxTheApp->CallAfter([doc, parent, certFailure]() {
            // Verify the doc is still live before touching it.
            if (s_docFrameMap.find(doc) == s_docFrameMap.end())
                return;
            wxString name = wxString::FromUTF8(static_cast<const char*>(doc->m_mush_name));
            wxString err  = wxString::FromUTF8(static_cast<const char*>(doc->m_strSSLLastError));
            if (certFailure) {
                // Certificate could not be verified: stay encrypted, but require
                // the user to consent to accepting an unverifiable certificate.
                wxString prompt = wxString::Format(
                    wxT("The TLS certificate for \"%s\" could not be verified:\n\n"
                        "%s\n\n"
                        "This can mean the connection is being intercepted. "
                        "Connect anyway (still encrypted, but unverified)?"),
                    name, err);
                const bool agreed =
                    wxMessageBox(prompt, wxT("TLS Certificate Not Verified"),
                                 wxYES_NO | wxICON_EXCLAMATION, parent) == wxYES;
                if (doc->ApplyTlsFallbackChoice(/*bCertFailure=*/true, agreed))
                    doc->ConnectSocket();
            } else {
                // Handshake failed: offer an unencrypted fallback connection.
                wxString prompt = wxString::Format(
                    wxT("TLS connection to \"%s\" failed.\n\n%s\n\n"
                        "Connect without encryption?"),
                    name, err);
                const bool agreed =
                    wxMessageBox(prompt, wxT("TLS Connection Failed"),
                                 wxYES_NO | wxICON_QUESTION, parent) == wxYES;
                if (doc->ApplyTlsFallbackChoice(/*bCertFailure=*/false, agreed))
                    doc->ConnectSocket();
            }
        });
        return TRUE;
    }

    (void)lp;
    return FALSE;
}

// The decode half of hook_message_box, split out so it can be tested: the hook
// itself ends in ShowModal, and no in-app selftest can complete a modal (they
// all exit from wxApp::OnInit, before there is an event loop).  Since all three
// bugs recorded below lived in the decode and not in the dialog, that is
// exactly the half worth reaching.  Declared in platform/wx_mfc_logic.h.
long MessageBoxStyleForMbFlags(unsigned flags, MbRelabel* relabel_out)
{
    // The MB_* button set is the low nibble — an ENUMERATION, not bit flags — and
    // the icon is the 0xF0 nibble.  Decode each as a field; the old bitwise tests
    // mis-read e.g. MB_OKCANCEL(1) as Yes/No (1 & 0x0003 != 0) and lost Cancel.
    // Abort/Retry/Ignore and Retry/Cancel have no wx button set of their own, so
    // they borrow one and RELABEL it (wxMessageDialog::SetYesNoCancelLabels /
    // SetOKCancelLabels).  Without that the two sets used to collapse to plain
    // OK and OK/Cancel: utils.msgbox(..., "abortretryignore") showed one button
    // and could only ever return IDOK, so a script branching on abort vs retry
    // vs ignore always took the same arm.
    MbRelabel relabel = eNoRelabel;

    long style = wxOK;
    switch (flags & 0x000F) {
        case 0x0001: style = wxOK | wxCANCEL;     break;  // MB_OKCANCEL
        case 0x0002: style = wxYES_NO | wxCANCEL;         // MB_ABORTRETRYIGNORE
                     relabel = eAbortRetryIgnore; break;
        case 0x0003: style = wxYES_NO | wxCANCEL; break;  // MB_YESNOCANCEL
        case 0x0004: style = wxYES_NO;            break;  // MB_YESNO
        case 0x0005: style = wxOK | wxCANCEL;             // MB_RETRYCANCEL
                     relabel = eRetryCancel;      break;
        default:     style = wxOK;                break;  // MB_OK
    }
    switch (flags & 0x00F0) {
        case 0x0010: style |= wxICON_ERROR;       break;  // MB_ICONHAND / STOP
        case 0x0020: style |= wxICON_QUESTION;    break;  // MB_ICONQUESTION
        case 0x0030: style |= wxICON_WARNING;     break;  // MB_ICONEXCLAMATION
        case 0x0040: style |= wxICON_INFORMATION; break;  // MB_ICONINFORMATION / ASTERISK
        default:     break;
    }
    // The default-button field (MB_DEFBUTTON2 0x0100 / MB_DEFBUTTON3 0x0200) is
    // a THIRD field, and it was silently dropped -- so every prompt that
    // deliberately asks for a safe default got a dangerous one.  Return on
    // "Delete the selected N items?" answered Yes, and on the log-file prompt it
    // answered Overwrite.  The callers asking for it are the destructive ones:
    // the six delete confirmations in world properties, genpropertypage.cpp,
    // dialogs/logdlg.cpp, doc.cpp, CheckTyping in sendvw.cpp, and scripted
    // utils.msgbox (scripting/lua_utils.cpp, which exposes buttons 1-3).
    //
    // Win32 numbers the buttons in the order the set declares them, so which
    // button is "second" depends on the set -- for MB_YESNOCANCEL it is No, NOT
    // Cancel.  wx can only express the two negative defaults it has names for.
    const unsigned defBtn = flags & 0x0300;
    if (defBtn == 0x0100) {                      // MB_DEFBUTTON2
        if      (style & wxYES_NO) style |= wxNO_DEFAULT;      // [Yes, *No*, …]
        else if (style & wxCANCEL) style |= wxCANCEL_DEFAULT;  // [OK, *Cancel*]
    } else if (defBtn == 0x0200) {               // MB_DEFBUTTON3
        // Only Yes/No/Cancel has a third button; anything else has no such
        // button and Win32 falls back to the first, which is wx's default.
        if ((style & wxYES_NO) && (style & wxCANCEL)) style |= wxCANCEL_DEFAULT;
    }

    if (relabel_out) *relabel_out = relabel;
    return style;
}

// The result half: map wx's ShowModal answer back to the MB_* code the caller
// is branching on.  Split out with the decode, and for the same reason.
int MbResultForWxId(MbRelabel relabel, int res)
{
    // Honour the relabelling so the caller gets the code for the button it
    // actually asked for.
    if (relabel == eAbortRetryIgnore) {
        switch (res) {
        case wxID_YES:    return IDABORT;
        case wxID_NO:     return IDRETRY;
        case wxID_CANCEL: return IDIGNORE;
        default:          return IDIGNORE;   // dialog dismissed: the no-op arm
        }
    }
    if (relabel == eRetryCancel) {
        switch (res) {
        case wxID_OK:     return IDRETRY;
        case wxID_CANCEL: return IDCANCEL;
        default:          return IDCANCEL;
        }
    }
    switch (res) {
    case wxID_OK:     return IDOK;
    case wxID_CANCEL: return IDCANCEL;
    case wxID_YES:    return IDYES;
    case wxID_NO:     return IDNO;
    default:          return IDOK;
    }
}

static int hook_message_box(const char* text, const char* caption, unsigned flags)
{
    MbRelabel relabel = eNoRelabel;
    const long style  = MessageBoxStyleForMbFlags(flags, &relabel);

    wxString wxText    = text    ? wxString::FromUTF8(text)    : wxString();
    wxString wxCaption = caption ? wxString::FromUTF8(caption) : wxString("MUSHclient");

    wxMessageDialog dlg(nullptr, wxText, wxCaption, (int)style);
    if (relabel == eAbortRetryIgnore)
        dlg.SetYesNoCancelLabels(wxT("Abort"), wxT("Retry"), wxT("Ignore"));
    else if (relabel == eRetryCancel)
        dlg.SetOKCancelLabels(wxT("Retry"), wxT("Cancel"));

    return MbResultForWxId(relabel, dlg.ShowModal());
}

static void hook_set_status(const char* msg)
{
    // CMainFrame::SetStatusMessage funnels a lot through here -- "Reconnecting",
    // the queued-command counts, "Closing network connection", the mapper's
    // speedwalk line, find results, the notepad messages.  s_statusBar belongs
    // to the MDI parent, which wxOSX hides behind the active child, so all of
    // that was invisible.  Prefer the visible world frame's own pane 0.
    const wxString text = msg ? wxString::FromUTF8(msg) : wxString();
    if (wxTheApp) {
        if (auto* mdi = wxDynamicCast(wxTheApp->GetTopWindow(), wxMDIParentFrame))
            if (auto* child = dynamic_cast<CWorldChildFrame*>(mdi->GetActiveChild()))
                if (child->SetTransientStatus(text)) return;
    }
    if (s_statusBar)
        s_statusBar->SetStatusText(text, 0);
}


// ── MDI frame creation ────────────────────────────────────────────────────
// Called when CMDIFrameWnd::LoadFrame() is invoked on CMainFrame.
// Creates a real wxMDIParentFrame, builds a menu bar, and binds menu events
// via Bind() lambdas (avoids static event-table issues on macOS).

// Forward declarations — defined later in this file / in other TUs.
extern CMUSHclientApp App;
static void* hook_open_world_file(const char* path);
static void  hook_save_active_world(wxMDIParentFrame* frame, bool saveAs);
static void  hook_file_preferences(wxWindow* parent);
static void  SaveMainWindowGeometry(wxTopLevelWindow* frame);

// ── File drop target ──────────────────────────────────────────────────────
// Lets the user drag MUSHclient world files (.mcl, .mck, .mcp) directly onto
// the main window to open them — a common macOS/Linux workflow.

namespace {
class WorldFileDropTarget : public wxFileDropTarget
{
public:
    bool OnDropFiles(wxCoord /*x*/, wxCoord /*y*/,
                     const wxArrayString& files) override
    {
        bool opened = false;
        for (const wxString& path : files) {
            wxString ext = wxFileName(path).GetExt().Lower();
            if (ext == "mcl" || ext == "mck" || ext == "mcp") {
                hook_open_world_file(path.ToUTF8().data());
                opened = true;
            }
        }
        return opened;
    }
};
} // anonymous namespace

// ── Table-driven canonical menu commands ──────────────────────────────────
// Standard MUSHclient menu commands whose real handler already lives in a
// canonical MFC message map (doc.cpp / mainfrm.cpp / MUSHclient.cpp).  Rather
// than a bespoke build + dispatch + update-UI branch per command, the menu
// builder appends them from this one table and a single generic handler routes
// clicks via AfxDispatchCommand and enable/check state via AfxDispatchUpdateUI.
// Adding a standard command is then one row of data.
//   target:    'D' = active world doc, 'F' = &Frame (CMainFrame), 'A' = &App.
//   checkable: a toggle — check state comes from the canonical ON_UPDATE_COMMAND_UI.
//   needsDoc:  greyed out when no world window is active.
enum CanonSection { SEC_FILE, SEC_EDIT, SEC_CONN, SEC_DISPLAY, SEC_GAME, SEC_HELP };
struct CanonMenuCmd {
    CanonSection section;
    int          id;
    const char*  label;     // UTF-8; "&Mnemonic\tAccel"
    char         target;
    bool         checkable;
    bool         needsDoc;
};
static const CanonMenuCmd kCanonCmds[] = {
  // ── File ──
  { SEC_FILE, 32980, "&Import\xE2\x80\xA6\tCtrl+Alt+I",      'D', false, true  }, // ID_FILE_IMPORT
  { SEC_FILE, 32117, "&Log Session\xE2\x80\xA6",             'D', false, true  }, // ID_FILE_LOGSESSION
  { SEC_FILE, 32954, "Open Worlds In Startup List",          'F', false, false }, // ID_FILE_OPENWORLDSINSTARTUPLIST
  { SEC_FILE, 32330, "Reload Defaults",                      'D', false, true  }, // ID_FILE_RELOADDEFAULTS
  { SEC_FILE, 32984, "Plugin &Wizard\xE2\x80\xA6",           'D', false, true  }, // ID_FILE_PLUGINWIZARD
  // ── Edit ──
  { SEC_EDIT, 32244, "Paste To &World\xE2\x80\xA6\tShift+Ctrl+V", 'D', false, true  }, // ID_EDIT_PASTETOMUSH
  { SEC_EDIT, 32327, "Reload Names File\xE2\x80\xA6",        'F', false, false }, // ID_EDIT_RELOADNAMESFILE
  { SEC_EDIT, 33037, "Generate Unique &ID\xE2\x80\xA6",      'F', false, false }, // ID_EDIT_GENERATEUNIQUEID
  { SEC_EDIT, 32968, "Colour Picker\xE2\x80\xA6",            'A', false, false }, // ID_EDIT_COLOURPICKER
  { SEC_EDIT, 33052, "Convert Clipboard Forum Codes",        'F', false, false }, // ID_EDIT_CONVERTCLIPBOARDFORUMCODES
  // ── Connection ──
  { SEC_CONN, 32241, "&Auto Connect",                        'F', true,  false }, // ID_CONNECTION_AUTOCONNECT
  { SEC_CONN, 32341, "&Reconnect On Disconnect",             'F', true,  false }, // ID_CONNECTION_RECONNECTONDISCONNECT
  { SEC_CONN, 32975, "Connect To All Open &Worlds",          'F', false, false }, // ID_CONNECTION_CONNECTTOALLOPENWORLDS
  { SEC_CONN, 32976, "Connect To Worlds In Startup List",    'F', false, false }, // ID_CONNECTION_CONNECTTOWORLDSINSTARTUPLIST
  // ── Display ──
  { SEC_DISPLAY, 32324, "&Recall Text\xE2\x80\xA6\tCtrl+U",  'D', false, true  }, // ID_DISPLAY_RECALLTEXT
  { SEC_DISPLAY, 32291, "Clear Output Buffer\xE2\x80\xA6",   'D', false, true  }, // ID_DISPLAY_CLEAR_OUTPUT
  { SEC_DISPLAY, 32292, "Stop Sound Playing",                'F', false, false }, // ID_DISPLAY_STOPSOUNDPLAYING
  { SEC_DISPLAY, 32970, "No Command Echo",                   'D', true,  true  }, // ID_DISPLAY_NOCOMMANDECHO
  // ── Game ──
  { SEC_GAME, 32118, "Wrap &Output",                         'D', true,  true  }, // ID_GAME_WRAPLINES
  { SEC_GAME, 32252, "&Auto Say\tShift+Ctrl+A",              'D', true,  true  }, // ID_GAME_AUTOSAY
  { SEC_GAME, 32216, "Send &File\xE2\x80\xA6",               'D', false, true  }, // ID_GAME_PASTEFILE
  { SEC_GAME, 32956, "&Discard Queued Commands",             'D', false, true  }, // ID_INPUT_DISCARDQUEUEDCOMMANDS
  { SEC_GAME, 33049, "Show &Key Name\xE2\x80\xA6",           'D', false, true  }, // ID_INPUT_KEYNAME
  { SEC_GAME, 32316, "Reset All Timers",                     'D', false, true  }, // ID_GAME_RESETALLTIMERS
  { SEC_GAME, 33032, "Reset Connected Time",                 'D', false, true  }, // ID_GAME_RESETCONNECTEDTIME
  { SEC_GAME, 32958, "&Mapper",                              'D', false, true  }, // ID_GAME_MAPPER
  { SEC_GAME, 32959, "&Do Mapper Special\xE2\x80\xA6",       'D', false, true  }, // ID_GAME_DOMAPPERSPECIAL
  { SEC_GAME, 32985, "Add Mapper Comment\xE2\x80\xA6",       'D', false, true  }, // ID_GAME_DOMAPPERCOMMENT
  { SEC_GAME, 32991, "Chat Sessions",                        'D', false, true  }, // ID_GAME_CHATSESSIONS
  { SEC_GAME, 32321, "&Edit Script File\xE2\x80\xA6",        'D', false, true  }, // ID_GAME_EDITSCRIPTFILE
  { SEC_GAME, 32967, "Test &Trigger\xE2\x80\xA6",            'D', false, true  }, // ID_DEBUG_WORLD_INPUT
  { SEC_GAME, 32234, "Minimize &Program\tCtrl+M",            'A', false, false }, // ID_GAME_MINIMISEPROGRAM
  // ── Help (web links use the working ShellExecute shim) ──
  { SEC_HELP, 32237, "&Tip Of The Day\xE2\x80\xA6",          'F', false, false }, // ID_HELP_TIPOFTHEDAY
  { SEC_HELP, 32294, "&Getting Started\xE2\x80\xA6",         'A', false, false }, // ID_HELP_GETTINGSTARTED
  { SEC_HELP, 32986, "MUSHclient &Forum (web)",              'F', false, false }, // ID_HELP_FORUM
  { SEC_HELP, 32271, "MUSHclient &Web Page",                 'F', false, false }, // ID_WEB_PAGE
  { SEC_HELP, 32987, "Function &List (web)",                 'F', false, false }, // ID_HELP_FUNCTIONSWEBPAGE
  { SEC_HELP, 33029, "&Documentation (web)",                 'F', false, false }, // ID_HELP_DOCUMENTATIONWEBPAGE
  { SEC_HELP, 33030, "&Regular Expressions (web)",           'F', false, false }, // ID_HELP_REGULAREXPRESSIONSWEBPAGE
  { SEC_HELP, 32287, "&MUD Lists (web)",                     'F', false, false }, // ID_HELP_MUDLISTS
  { SEC_HELP, 33034, "&Plugins List (web)",                  'F', false, false }, // ID_HELP_PLUGINSLIST
  { SEC_HELP, 32293, "&Bug Reports / Suggestions\xE2\x80\xA6",'F', false, false }, // ID_HELP_BUGREPORTSUGGESTION
};

// Append every table command for one section to `menu`, led by a separator.
static void append_canon_section(wxMenu* menu, CanonSection sec)
{
    bool first = true;
    for (const CanonMenuCmd& c : kCanonCmds) {
        if (c.section != sec) continue;
        if (first) { menu->AppendSeparator(); first = false; }
        if (c.checkable) menu->AppendCheckItem(c.id, wxString::FromUTF8(c.label));
        else             menu->Append(c.id, wxString::FromUTF8(c.label));
    }
}
static const CanonMenuCmd* find_canon_cmd(int id)
{
    for (const CanonMenuCmd& c : kCanonCmds)
        if (c.id == id) return &c;
    return nullptr;
}
// Resolve a table target letter to its canonical CCmdTarget (or nullptr).
static CCmdTarget* canon_target(char t, CMUSHclientDoc* doc)
{
    switch (t) {
        case 'D': return doc;
        case 'F': return &Frame;
        case 'A': return &App;
    }
    return nullptr;
}

// ── Quit the application ──────────────────────────────────────────────────
// Cmd-Q / the app-menu "Quit" must terminate the process as reliably as the
// Dock icon's "Quit" — and on macOS it didn't.  The Dock's Quit uses Cocoa's
// -[NSApplication terminate:], which always ends the run loop.  Our menu
// handler instead closed every top-level window and trusted ExitOnFrameDelete
// to end the loop once the last one was destroyed — but on some macOS versions
// that final step never fires: the windows vanish yet the process lingers with
// just the stub app+Window menu bar, and a second Cmd-Q does nothing.  (Users
// confirmed the Dock's Quit worked while the menu's did not — the tell-tale
// that the two took different paths.)
//
// So on macOS terminate exactly the way the Dock does.  -[NSApplication
// terminate:] also closes every window — incl. standalone ones like a Notepad —
// with the normal close/veto handling, so it subsumes the old close-all sweep.
// On other platforms (GTK) the wx-idiomatic "close every top-level window" path
// is correct and ExitOnFrameDelete is reliable, so keep it there: standalone
// frames first (a veto then leaves the rest intact), the MDI parent — which
// takes its world children with it — last.
static void wx_quit_application()
{
#ifdef __APPLE__
    wxOSXTerminateApp();
#else
    const wxWindowList tops = wxTopLevelWindows;  // snapshot; Close() mutates it
    for (wxWindow* w : tops)                       // standalone frames first
        if (auto* tlw = wxDynamicCast(w, wxTopLevelWindow))
            if (!wxDynamicCast(tlw, wxMDIParentFrame))
                if (!tlw->Close(false)) return;    // vetoed → cancel quit
    for (wxWindow* w : tops)                         // MDI parent(s) + children last
        if (auto* mdi = wxDynamicCast(w, wxMDIParentFrame))
            if (!mdi->Close(false)) return;
#endif
}

static int hook_load_mdi_frame(void* selfVoid, unsigned /*id*/,
                                unsigned long /*style*/, void* /*parentHwnd*/)
{
    CWnd* self = static_cast<CWnd*>(selfVoid);

    // Create the top-level MDI parent window (two-phase for error detection).
    wxMDIParentFrame* frame = new wxMDIParentFrame();
    bool ok = frame->Create(
        nullptr,
        wxID_ANY,
        wxT("MUSHclient"),
        wxDefaultPosition,
        wxSize(1024, 768),
        wxDEFAULT_FRAME_STYLE);

    if (!ok) {
        frame->Destroy();
        return FALSE;
    }

    // ── Restore main window geometry ──────────────────────────────────────
    // Saved geometry survives app restarts via wxConfig.  We default to
    // 1024×768 centred on first run; subsequent runs restore the last size
    // and (on platforms that honour it) position.  Maximized state is also
    // preserved.  Iconized state is not (would be confusing on relaunch).
    //
    // NOTE: this is the MDI *parent*, which wxOSX never shows — on macOS the
    // window the user actually sees is the world child frame, and its own
    // first-run default is computed in CWorldChildFrame's constructor.  Changing
    // the numbers here alone has no visible effect on macOS.
    {
        const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;

        // First-run default size: the full usable screen height, and
        // CWorldChildFrame::kMaxDefaultWidthDIP wide unless the screen's usable
        // area (which already excludes the menu bar / Dock) is narrower.
        // Deliberately the same rule as the child frame: on platforms where the
        // MDI parent *is* visible this is the window being sized, and having the
        // two disagree is what let the child sit on a stale hard-coded default
        // unnoticed.  A fixed width rather than a column count because measuring
        // the font at app-init proved unreliable.
        int di = wxDisplay::GetFromWindow(frame);
        if (di < 0) di = 0;
        const wxRect scr = wxDisplay(di).GetClientArea();
        const long defW = std::min<long>(
            scr.width,
            frame->FromDIP((int) CWorldChildFrame::kMaxDefaultWidthDIP));
        const long defH = scr.height;

        long w = defW, h = defH;
        long x = -1, y = -1;    // -1 = not saved; keep platform default
        bool maximized = false;
        cfg.Read(wxT("/geometry/main_w"), &w, defW);
        cfg.Read(wxT("/geometry/main_h"), &h, defH);
        cfg.Read(wxT("/geometry/main_x"), &x, -1L);
        cfg.Read(wxT("/geometry/main_y"), &y, -1L);
        cfg.Read(wxT("/geometry/main_maximized"), &maximized, false);
        // Windows persists this with SaveBarState/LoadBarState; those shims are
        // inert here, so keep it beside the geometry.  Read before any world
        // frame exists so the first one comes up right.
        cfg.Read(wxT("/geometry/status_bar"), &s_statusBarVisible, true);
        // Clamp to sanity + the usable screen: never a tiny or oversized window.
        if (w < 400)        w = defW;
        if (h < 300)        h = defH;
        if (w > scr.width)  w = scr.width;
        if (h > scr.height) h = scr.height;
        if (x >= 0 && y >= 0)
            frame->SetSize((int)x, (int)y, (int)w, (int)h);
        else {
            frame->SetSize((int)w, (int)h);
            frame->Centre();
        }
        if (maximized)
            frame->Maximize(true);
    }

    // View ▸ Always On Top, restored at startup.  Windows does this at
    // MUSHclient.cpp:759-761 with SetWindowPos(&wndTopMost), a shim that is
    // inert here, so the saved preference never took effect on the port.
    // App.m_bAlwaysOnTop is already loaded by then (globalregistryoptions.cpp
    // persists it as "AlwaysOnTop").
    if (App.m_bAlwaysOnTop)
        apply_always_on_top(frame, true);

    // ── Menu bar ─────────────────────────────────────────────────────────
    wxMenuBar* mb = new wxMenuBar();

    wxMenu* fileMenu = new wxMenu();
    fileMenu->Append(0xE100,      wxT("New World\tCtrl+N"));
    fileMenu->Append(wxID_OPEN,   wxT("Open World File…\tCtrl+O"));
    // ── Open Recent submenu ───────────────────────────────────────────────
    // Populated immediately from config; updated each time a world is opened.
    s_recentMenu = new wxMenu();
    fileMenu->AppendSubMenu(s_recentMenu, wxT("Open Recent"));
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_SAVE,   wxT("Save World\tCtrl+S"));
    fileMenu->Append(wxID_SAVEAS, wxT("Save World As…"));
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_CLOSE,  wxT("Close World\tCtrl+Shift+W"));
    // Note: plain Ctrl+W is reserved for word-delete-backward in the input bar.
    fileMenu->AppendSeparator();
    fileMenu->Append(32250 /*ID_FILE_PRINT_WORLD*/, wxT("&Print World…\tCtrl+P"));
    // File ▸ Preferences uses wxID_PREFERENCES so wxWidgets automatically moves
    // it to the macOS application menu as "Settings…" (⌘,), where Mac users
    // expect it.  (No surrounding separator here — it leaves the File menu.)
    fileMenu->Append(wxID_PREFERENCES, wxT("Settings…"));
    append_canon_section(fileMenu, SEC_FILE);   // Import, Log Session, Open Startup, Reload Defaults
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT,   wxT("Quit MUSHclient\tCtrl+Q"));
    mb->Append(fileMenu, wxT("&File"));

    // ── Edit menu ─────────────────────────────────────────────────────────
    // Standard IDs (wxID_CUT etc.) are routed automatically by wxWidgets to
    // the focused window; no explicit handler is needed in our menu dispatcher.
    //
    // Copy is the exception, and deliberately uses the canonical MFC id instead.
    // A menu item with a STANDARD id gets AppKit's native action (copy:), and
    // AppKit then enables or disables it by asking the first responder whether it
    // can copy — overriding whatever wx's update-UI handler decided.  The first
    // responder here is always the command bar (nothing ever focuses the output
    // pane), so with text selected in the OUTPUT window the item was validated
    // against an empty command bar and greyed out, and clicking it would have
    // gone to the command bar rather than to us.  A non-standard id keeps the
    // item under wx's control, where CMUSHclientDoc::EditCopyTarget decides.
    // Verified: with the standard id the item stays grey however the world's
    // selection looks; with this one it enables and copies the selection.
    wxMenu* editMenu = new wxMenu();
    editMenu->Append(ID_EDIT_COPY,   wxT("Copy\tCtrl+C"));
    editMenu->Append(wxID_CUT,       wxT("Cut\tCtrl+X"));
    editMenu->Append(wxID_PASTE,     wxT("Paste\tCtrl+V"));
    editMenu->AppendSeparator();
    editMenu->Append(wxID_SELECTALL, wxT("Select All\tCtrl+A"));
    editMenu->AppendSeparator();
    editMenu->Append(wxID_FIND,      wxT("Find…\tCtrl+F"));
    editMenu->AppendSeparator();
    // Canonical "Flip To Notepad" (ID_EDIT_FLIPTONOTEPAD): opens this world's
    // scratch notepad, creating it the first time and re-activating it after.
    editMenu->Append(32953 /*ID_EDIT_FLIPTONOTEPAD*/,
                     wxT("Flip To Notepad\tCtrl+Shift+N"));
    // Port-local "New Notepad": always opens a fresh, standalone blank notepad
    // (not tied to any world). Inherits the Notepad-preferences font + colours.
    editMenu->Append(40001 /*port-local: New standalone Notepad*/,
                     wxT("New Notepad"));
    // Canonical "Close All Notepad Windows" lives in the Windows Window menu, but
    // on macOS wxMDIParentFrame owns the Window menu entirely (it injects the
    // native Minimize/Zoom/window-list items and drops our manual entries), so it
    // is homed here beside its New/Flip-Notepad siblings instead.  Dispatch is the
    // port-local handler below (the canonical one iterates the wrong doc template).
    editMenu->Append(32951 /*ID_WINDOW_CLOSEALLNOTEPADWINDOWS*/,
                     wxT("Close All Notepad Windows"));
    append_canon_section(editMenu, SEC_EDIT);   // Paste To World, Reload Names, Unique ID, Colour Picker, …
    editMenu->Append(32974 /*ID_EDIT_COPYASHTML*/, wxT("Copy as &HTML\tCtrl+Alt+C"));
    // Canonical Edit-menu entry (mushclient_dialogs.rc:2956), missing until now:
    // depending on the world's "ctrl_backspace_deletes_last_word" option this
    // either recalls the last word of the previous command (the default) or
    // deletes the word before the caret.  Its canonical handler is on the
    // never-instantiated CSendView, so it dispatches to the child frame below.
    editMenu->Append(32973 /*ID_REPEAT_LAST_WORD*/,
                     wxT("Recall Last Word\tCtrl+Back"));
    editMenu->AppendSeparator();
    editMenu->Append(32946 /*ID_EDIT_GOTOMATCHINGBRACE*/,   wxT("Go To Matching &Brace\tCtrl+E"));
    editMenu->Append(32947 /*ID_EDIT_SELECTTOMATCHINGBRACE*/,wxT("Select To Matching Brace\tShift+Ctrl+E"));
    mb->Append(editMenu, wxT("&Edit"));

    wxMenu* connMenu = new wxMenu();
    // Note: Ctrl+K, Ctrl+D, and Ctrl+R are standard Unix input-bar shortcuts
    // (kill-to-end, delete-forward, and reverse history search respectively) so
    // those keys are not used as menu accelerators here.
    connMenu->Append(32289,                     wxT("Quick Connect…\tCtrl+Shift+K"));
    connMenu->AppendSeparator();
    connMenu->Append(32239 /*ID_CONNECTION_CONNECT*/,    wxT("Connect"));
    connMenu->Append(kPortMenuIdDisconnect,     wxT("Disconnect"));
    connMenu->Append(kPortMenuIdReconnect,      wxT("Reconnect\tCtrl+Shift+R"));
    connMenu->AppendSeparator();
    connMenu->Append(ID_CONN_LOG_TOGGLE,        wxT("Start &Logging…"));
    append_canon_section(connMenu, SEC_CONN);   // Auto Connect, Reconnect, Connect-All, Connect-Startup
    mb->Append(connMenu, wxT("&Connection"));

    wxMenu* gameMenu = new wxMenu();
    gameMenu->Append(32158 /*ID_GAME_PREFERENCES*/,     wxT("World Properties…\tCtrl+,"));
    gameMenu->AppendSeparator();
    gameMenu->Append(32290 /*ID_GAME_RELOAD_SCRIPT_FILE*/, wxT("Reload Script"));
    gameMenu->Append(ID_MENU_GAME_LUA_EVAL,              wxT("Execute Lua…\tCtrl+Shift+L"));
    gameMenu->AppendSeparator();
    gameMenu->Append(32983 /*ID_FILE_PLUGINS*/,          wxT("Plugins…"));
    gameMenu->AppendSeparator();
    gameMenu->Append(32320 /*ID_INPUT_GLOBALCHANGE*/,    wxT("&Global Change…\tCtrl+Shift+G"));
    gameMenu->Append(32325 /*ID_GAME_SENDTOALLWORLDS*/,  wxT("&Send To All Worlds…\tCtrl+Alt+S"));
    gameMenu->Append(32203 /*ID_DISPLAY_ACTIVITYLIST*/,  wxT("&Activity List"));
    gameMenu->Append(32326 /*ID_EDIT_GENERATECHARACTERNAME*/, wxT("Generate Character &Name…"));
    gameMenu->Append(32323 /*ID_EDIT_SPELLCHECK*/,       wxT("Chec&k Spelling"));
    append_canon_section(gameMenu, SEC_GAME);   // Mapper, Reset Timers, Auto Say, Send File, Wrap Output, …
    gameMenu->AppendSeparator();
    gameMenu->Append(32152 /*ID_GAME_COMMANDHISTORY*/,  wxT("Command &History…\tCtrl+H"));
    gameMenu->Append(32272 /*ID_REPEAT_LAST_COMMAND*/,  wxT("&Repeat Last Command"));
    // "Quit From This World" sends the world's MAC_QUIT macro (default "QUIT")
    // after confirming, and suppresses auto-reconnect.  Canonically it sits in
    // the Input menu beside Repeat Last Command (mushclient_dialogs.rc:3008);
    // the port folds that menu into Game, so it goes here.
    //
    // NO ACCELERATOR, deliberately: the canonical one is Shift+Ctrl+Q, and wx
    // maps Ctrl to Command on macOS -- Shift+Cmd+Q is the system Log Out
    // shortcut.  Shadowing that from inside the app to quit a MUD session is a
    // trap worth avoiding; the menu item is reachable either way.
    gameMenu->Append(32217 /*ID_ACTIONS_QUIT*/,         wxT("&Quit From This World…"));
    gameMenu->Append(33046 /*ID_GAME_FUNCTIONSLIST*/,   wxT("&Functions List…"));
    mb->Append(gameMenu, wxT("&Game"));

    // ── Display menu (output-pane view commands) ──────────────────────────
    // No Ctrl+F accelerator here — the inline find bar already binds it.
    wxMenu* displayMenu = new wxMenu();
    displayMenu->Append(32248 /*ID_DISPLAY_FIND*/,         wxT("&Find…"));
    displayMenu->Append(32249 /*ID_DISPLAY_FINDAGAIN*/,    wxT("Find &Again"));
    displayMenu->AppendSeparator();
    // Freeze Output is a TOGGLE, and Windows ticks it from the view's m_freeze
    // (CMUSHView::OnUpdateDisplayFreezeoutput).  Built as a plain item here, it
    // could never show that tick, so after a couple of clicks nothing on screen
    // said whether the output was frozen or running.
    displayMenu->AppendCheckItem(32201 /*ID_DISPLAY_FREEZEOUTPUT*/, wxT("Free&ze Output"));
    displayMenu->Append(32963 /*ID_DISPLAY_GOTOLINE*/,     wxT("&Go To Line…"));
    displayMenu->AppendSeparator();
    // Connection diagnostics (toggle, reflected as a check mark).  "Debug
    // Incoming Packets" displays every received byte — including telnet
    // negotiation (IAC WILL/WONT…), MCCP, and other normally-invisible
    // protocol traffic.  "Trace" logs trigger/alias/timer/script activity.
    displayMenu->AppendCheckItem(32971 /*ID_EDIT_DEBUGINCOMINGPACKETS*/,
                                 wxT("&Debug Incoming Packets"));
    displayMenu->AppendCheckItem(32334 /*ID_GAME_TRACE*/, wxT("&Trace"));
    append_canon_section(displayMenu, SEC_DISPLAY); // Recall Text, Clear Output, Stop Sound, No Command Echo
    // Sits with Clear Output Buffer because they are the two clears, but it is
    // not the same command: this one keeps the prompt block and frees only what
    // is above it.  "Ctrl+" in a wx accelerator string is Command on macOS.
    auto clearItem = [displayMenu](int id, const char* label, const char* accel) {
        displayMenu->Append(id, wxString::FromUTF8(label) + wxT("\t") +
                                wxString::FromUTF8(accel));
    };
    clearItem(ID_MENU_DISPLAY_CLEAR_TO_START, "Clear to &Start",
              CWorldChildFrame::kAccelClearToStart);
    clearItem(ID_MENU_DISPLAY_CLEAR_TO_MARK, "Clear to Previous &Mark",
              CWorldChildFrame::kAccelClearToMark);
    clearItem(ID_MENU_DISPLAY_CLEAR_SCROLLBACK, "Clear Scroll&back",
              CWorldChildFrame::kAccelClearScrollback);
    // Erases nothing: the last line goes to the top and the scrollback stays.
    clearItem(ID_MENU_DISPLAY_CLEAR_SCREEN, "Clear Scr&een",
              CWorldChildFrame::kAccelClearScreen);
    // Output-selection commands (reimplemented on CWorldChildFrame).
    displayMenu->AppendSeparator();
    displayMenu->Append(32275 /*ID_DISPLAY_GOTOURL*/,         wxT("Go To &URL\tCtrl+Alt+J"));
    displayMenu->Append(32276 /*ID_DISPLAY_SENDMAILTO*/,      wxT("Send &Mail To…"));
    displayMenu->Append(33036 /*ID_DISPLAY_HIGHLIGHTPHRASE*/, wxT("&Highlight Word…\tCtrl+Alt+H"));
    displayMenu->Append(33044 /*ID_DISPLAY_MULTILINETRIGGER*/,wxT("&Multi-Line Trigger…"));
    displayMenu->Append(32331 /*ID_DISPLAY_TEXTATTRIBUTES*/,  wxT("Text &Attributes…"));
    displayMenu->Append(32318 /*ID_DISPLAY_BOOKMARKSELECTION*/, wxT("&Bookmark Selection\tShift+Ctrl+B"));
    displayMenu->Append(32319 /*ID_DISPLAY_GOTOBOOKMARK*/,    wxT("&Go To Bookmark\tCtrl+B"));
    mb->Append(displayMenu, wxT("&Display"));

    // ── View menu (port-local; real wx implementations, not the canonical
    // SetWindowPos/ModifyStyle paths which are Windows-only stubs) ─────────
    wxMenu* viewMenu = new wxMenu();
    viewMenu->AppendCheckItem(ID_MENU_VIEW_ALWAYSONTOP, wxT("Always On &Top"));
    viewMenu->AppendCheckItem(ID_MENU_VIEW_FULLSCREEN,  wxT("&Full Screen\tCtrl+Alt+F"));
    viewMenu->AppendSeparator();
    viewMenu->AppendCheckItem(ID_MENU_VIEW_STATUSBAR,   wxT("&Status Bar"));
    viewMenu->AppendSeparator();
    // The three canonical toolbars, in the canonical View-menu order (see the
    // IDR_MAINFRAME menu in MUSHclient.rc).  These live on the world window
    // rather than the hidden MDI parent — platform/wx_toolbar.cpp explains why.
    viewMenu->AppendCheckItem(0xE800 /*ID_VIEW_TOOLBAR*/,  wxT("&Toolbar"));
    viewMenu->AppendCheckItem(ID_VIEW_GAME_TOOLBAR,        wxT("&World Toolbar"));
    viewMenu->AppendCheckItem(ID_VIEW_ACTIVITYTOOLBAR,     wxT("&Activity Toolbar"));
    viewMenu->Append(ID_VIEW_RESET_TOOLBARS,               wxT("&Reset Toolbar Locations"));
    mb->Append(viewMenu, wxT("&View"));

    // ── Configure menu ────────────────────────────────────────────────────
    wxMenu* cfgMenu = new wxMenu();
    // Accelerators here follow one rule: a chord that already MEANS something
    // in MUSHclient must not mean something else on the port.  Shift+Ctrl+A is
    // Game ▸ Auto Say and Shift+Ctrl+I is Configure ▸ Info in the canonical
    // resource (mushclient_dialogs.rc:3014, 3083), so Aliases and Timers take
    // the resource's own Shift+Ctrl+9 / Shift+Ctrl+0 and hand those chords back
    // to the commands they belong to.  Ctrl+T is bound to nothing anywhere in
    // the resource, so Triggers keeps it as a macOS-native addition.
    cfgMenu->Append(ID_MENU_CFG_TRIGGERS,  wxT("&Triggers…\tCtrl+T"));
    cfgMenu->Append(ID_MENU_CFG_ALIASES,   wxT("&Aliases…\tShift+Ctrl+9"));
    cfgMenu->Append(ID_MENU_CFG_TIMERS,    wxT("T&imers…\tShift+Ctrl+0"));
    cfgMenu->AppendSeparator();
    // Shift+Ctrl+7 is what the canonical menu resource gives this command
    // (mushclient_dialogs.rc:3077, ID_GAME_CONFIGURE_VARIABLES).  The port had
    // invented Ctrl+Shift+V, which is ALSO the canonical accelerator for
    // Edit ▸ Paste To World (rc:2955) -- so the two commands claimed one key
    // and exactly one of them got it, silently.  The three Configure items
    // above still carry port-invented mnemonics rather than the resource's
    // Shift+Ctrl+8/9/0; those clash with nothing, so they are left alone.
    cfgMenu->Append(ID_MENU_CFG_VARIABLES, wxT("&Variables…\tShift+Ctrl+7"));
    cfgMenu->AppendSeparator();
    cfgMenu->Append(ID_MENU_CFG_MACROS,   wxT("&Macros && Keypad…"));
    cfgMenu->Append(ID_MENU_CFG_COLORS,   wxT("&Colors…"));
    cfgMenu->Append(ID_MENU_CFG_INFO,     wxT("In&fo…\tShift+Ctrl+I"));
    cfgMenu->AppendSeparator();
    // Import / Export submenus
    wxMenu* importMenu = new wxMenu();
    importMenu->Append(ID_MENU_IMPORT_TRIGGERS, wxT("&Triggers…"));
    importMenu->Append(ID_MENU_IMPORT_ALIASES,  wxT("&Aliases…"));
    importMenu->Append(ID_MENU_IMPORT_TIMERS,   wxT("T&imers…"));
    importMenu->Append(ID_MENU_IMPORT_COLORS,   wxT("&Colors…"));
    importMenu->Append(ID_MENU_IMPORT_MACROS,   wxT("&Macros…"));
    cfgMenu->AppendSubMenu(importMenu, wxT("&Import"));

    wxMenu* exportMenu = new wxMenu();
    exportMenu->Append(ID_MENU_EXPORT_TRIGGERS, wxT("&Triggers…"));
    exportMenu->Append(ID_MENU_EXPORT_ALIASES,  wxT("&Aliases…"));
    exportMenu->Append(ID_MENU_EXPORT_TIMERS,   wxT("T&imers…"));
    exportMenu->Append(ID_MENU_EXPORT_COLORS,   wxT("&Colors…"));
    exportMenu->Append(ID_MENU_EXPORT_MACROS,   wxT("&Macros…"));
    cfgMenu->AppendSubMenu(exportMenu, wxT("E&xport"));
    mb->Append(cfgMenu, wxT("&Configure"));

    // ── Window menu ───────────────────────────────────────────────────────
    // wxMDIParentFrame recognises a menu named exactly "&Window" and populates
    // it automatically with the list of open child frames on macOS/GTK.
    wxMenu* windowMenu = new wxMenu();
    windowMenu->Append(wxID_MDI_WINDOW_TILE_HORZ, wxT("Tile Horizontally"));
    windowMenu->Append(wxID_MDI_WINDOW_TILE_VERT, wxT("Tile Vertically"));
    windowMenu->Append(wxID_MDI_WINDOW_CASCADE,    wxT("Cascade"));
    windowMenu->AppendSeparator();
    windowMenu->Append(wxID_MDI_WINDOW_ARRANGE_ICONS, wxT("Arrange Icons"));
    mb->Append(windowMenu, wxT("&Window"));

    wxMenu* helpMenu = new wxMenu();
    helpMenu->Append(wxID_ABOUT, wxT("About MUSHclient"));
    append_canon_section(helpMenu, SEC_HELP);   // Tip, Getting Started, Forum/Web/Docs/Functions/Bug Reports
    mb->Append(helpMenu, wxT("&Help"));

    frame->SetMenuBar(mb);

    // ── Populate "Open Recent" submenu from persisted config ──────────────
    recent_worlds_load();
    recent_worlds_update_menu();

    // ── Status bar ────────────────────────────────────────────────────────
    // The MDI parent's status bar carries transient global messages.  On macOS
    // it is covered by the maximised child, so the *world* status line (world
    // name, duration, line counts, log) lives on each CWorldChildFrame's own
    // status bar instead — see CWorldChildFrame::UpdateStatusBar.
    s_statusBar = frame->CreateStatusBar(1);
    if (s_statusBar)
        s_statusBar->SetStatusText(wxT("Ready"), 0);

    // ── Drag-and-drop world files ─────────────────────────────────────────
    // Ownership of the drop target transfers to the window.
    frame->SetDropTarget(new WorldFileDropTarget());

    // ── Socket polling timer ──────────────────────────────────────────────
    // CAsyncSocket::Poll() must be called periodically on the GUI thread to
    // deliver OnConnect / OnReceive / OnClose callbacks.  20 ms is a
    // responsive polling interval that keeps the UI smooth.
    wxTimer* pollTimer = new wxTimer(frame);
    frame->Bind(wxEVT_TIMER, [](wxTimerEvent&) {
        CAsyncSocket::Poll(0);
        // Deliver PAL-transport main-thread completions (async DNS results)
        // alongside the socket pump — platform/posix_transport.h (T2d).
        pal::PumpTransportCompletions();
    }, pollTimer->GetId());
    pollTimer->Start(20);

    // ── App-level close confirmation ──────────────────────────────────────
    // EVT_CLOSE fires when the user closes the main window (via ×, Cmd+Q,
    // or our File → Quit handler).  If the global pref is set, ask before
    // destroying.  Also stop the poll timer here so it doesn't fire during
    // the child-frame teardown sequence.
    frame->Bind(wxEVT_CLOSE_WINDOW, [frame, pollTimer](wxCloseEvent& evt) {
        if (evt.CanVeto() && App.m_bConfirmBeforeClosingMushclient) {
            int answer = wxMessageBox(
                wxT("Are you sure you want to quit MUSHclient?"),
                wxT("Confirm Quit"),
                wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, frame);
            if (answer != wxYES) {
                evt.Veto();
                return;
            }
        }
        // Save main window geometry so the next session opens at the same size.
        SaveMainWindowGeometry(frame);
        // Stop and free the socket-poll timer before child frames are destroyed
        // (wxTimer is not a wxWindow, so the frame won't delete it for us). This
        // only runs on the committed-close path — the veto branch returned above.
        pollTimer->Stop();
        delete pollTimer;
        evt.Skip();  // proceed with default close (destroys child frames → the app)
    });

    // ── Menu update-UI: enable/disable items based on connection state ────
    // wxEVT_UPDATE_UI fires just before each menu item is shown, letting us
    // grey items that don't apply to the current state.
    frame->Bind(wxEVT_UPDATE_UI, [frame](wxUpdateUIEvent& evt) {
        // Helper: get the active child's doc, or nullptr.
        auto activeDoc = [frame]() -> CMUSHclientDoc* {
            wxMDIChildFrame* active = frame->GetActiveChild();
            if (!active) return nullptr;
            auto* child = dynamic_cast<CWorldChildFrame*>(active);
            return child ? child->GetDoc() : nullptr;
        };

        int id = evt.GetId();
        CMUSHclientDoc* doc = activeDoc();

        // Put a rule's answer onto the event.  One place, so an arm cannot
        // forget the check mark or the caption half of it.
        auto apply = [&evt](const MenuCmdUiState& st) {
            evt.Enable(st.enabled);
            if (st.checked >= 0) evt.Check(st.checked != 0);
            if (st.text) evt.SetText(wxString::FromUTF8(st.text));
        };

        // ── Table-driven canonical commands: enable + check come from the
        // canonical ON_UPDATE_COMMAND_UI handler, captured onto the menu item.
        if (const CanonMenuCmd* c = find_canon_cmd(id)) {
            CCmdTarget* tgt = canon_target(c->target, doc);
            bool enabled = true; int checked = -1;
            if (tgt) {
                CCmdUI cmdui;
                cmdui.m_nID            = (UINT) id;
                cmdui.m_pCaptureEnable = &enabled;
                cmdui.m_pCaptureCheck  = &checked;
                if (!AfxDispatchUpdateUI(tgt, &cmdui))
                    enabled = c->needsDoc ? (doc != nullptr) : true;
            } else {
                enabled = false;   // 'D' target with no active world
            }
            evt.Enable(enabled);
            if (c->checkable && checked >= 0) evt.Check(checked != 0);
            return;
        }

        // Connect / Disconnect / Reconnect used to be three hand-written
        // predicates here.  Two of them were copies of canonical update-UI
        // handlers that the document dispatches for itself — and a copy is how
        // Disconnect's rule (live on every non-zero phase, so a connect to a
        // host that never answers can still be cancelled) came adrift once
        // before.  All three now live in platform/menu_ui_rules.cpp with the
        // rest, and are answered by the MenuCommandUiState arm below.
        if (id == wxID_SAVE || id == wxID_SAVEAS ||
            id == wxID_CLOSE ||
            id == 32158 /*ID_GAME_PREFERENCES*/ ||
            id == ID_MENU_CFG_TRIGGERS  ||
            id == ID_MENU_CFG_ALIASES   ||
            id == ID_MENU_CFG_TIMERS    ||
            id == ID_MENU_CFG_VARIABLES ||
            id == ID_MENU_CFG_MACROS    ||
            id == ID_MENU_CFG_COLORS    ||
            id == ID_MENU_CFG_INFO      ||
            id == ID_MENU_IMPORT_TRIGGERS ||
            id == ID_MENU_EXPORT_TRIGGERS ||
            id == ID_MENU_IMPORT_ALIASES  ||
            id == ID_MENU_EXPORT_ALIASES  ||
            id == ID_MENU_IMPORT_TIMERS   ||
            id == ID_MENU_EXPORT_TIMERS   ||
            id == ID_MENU_IMPORT_COLORS   ||
            id == ID_MENU_EXPORT_COLORS   ||
            id == ID_MENU_IMPORT_MACROS   ||
            id == ID_MENU_EXPORT_MACROS) {
            // Need an active world.
            evt.Enable(doc != nullptr);
        } else if (id == ID_MENU_GAME_LUA_EVAL ||
                   id == 32290 /*ID_GAME_RELOAD_SCRIPT_FILE*/) {
            // Both need an active world with scripting on; the rule is in
            // menu_ui_rules.cpp, keyed by an action rather than by these ids
            // because ID_MENU_GAME_LUA_EVAL is wxID_HIGHEST-relative.
            apply(PortMenuRuleUiState(kPortRuleScripting, doc));
        } else if (id == ID_CONN_LOG_TOGGLE) {
            // Needs an active world; the label flips between Start and Stop,
            // which is the whole state this item shows — same file, same reason.
            apply(PortMenuRuleUiState(kPortRuleLogToggle, doc));
        } else if (id == ID_MENU_VIEW_ALWAYSONTOP) {
            // Read the preference, as CMainFrame::OnUpdateViewAlwaysontop does;
            // reading the parent's style flag reported the wrong thing once the
            // pin started being applied to the world frames as well.
            evt.Check(App.m_bAlwaysOnTop != 0);
        } else if (id == ID_MENU_VIEW_FULLSCREEN) {
            // Same target as the command arm, or the tick reports one window
            // while the other is the one that moved.
            evt.Check(wx_visible_frame(frame)->IsFullScreen());
        } else if (id == ID_MENU_VIEW_STATUSBAR) {
            evt.Enable(true);          // Windows' item is live with no world open
            evt.Check(s_statusBarVisible);
        } else if (id == 0xE800 /*ID_VIEW_TOOLBAR*/ || id == ID_VIEW_GAME_TOOLBAR ||
                   id == ID_VIEW_ACTIVITYTOOLBAR) {
            // The bars belong to world windows, so with none open there is
            // nothing to toggle (Windows keeps them on the always-present main
            // frame, where the item stays live).
            auto* active = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild());
            evt.Enable(active != nullptr);
            if (active) {
                CWorldToolBars::Which which =
                    (id == ID_VIEW_GAME_TOOLBAR)    ? CWorldToolBars::eWorld    :
                    (id == ID_VIEW_ACTIVITYTOOLBAR) ? CWorldToolBars::eActivity :
                                                      CWorldToolBars::eStandard;
                evt.Check(active->AreToolBarsVisible(which));
            }
        } else if (id == ID_VIEW_RESET_TOOLBARS) {
            evt.Enable(frame->GetActiveChild() != nullptr);
        } else if (id == ID_EDIT_COPY || id == 32974 /*ID_EDIT_COPYASHTML*/) {
            // Both share CMUSHView::OnUpdateNeedSelectionForCopy on Windows:
            // enabled when the output window has a selection, or failing that
            // the command bar does.  The same call decides what Copy acts on
            // (see the command arm), so the two cannot drift.
            //
            // The dialog case has to be answered here too, and for Copy only.
            // Since Copy is on a non-standard id, wxOSX validates its Cmd-C key
            // equivalent through the MENUBAR's frame — this handler — even while
            // a modal bridged dialog owns the keyboard.  Without this arm the
            // world's selection decided whether Cmd-C was live inside a dialog:
            // with nothing selected in the world, the item was greyed, AppKit
            // refused the key equivalent, and the command arm's
            // copy-from-the-focused-field path (added alongside it) could never
            // run.  That is the exact symmetry the command handler and the
            // wxTheApp fallback already have; the update-UI arm was the sibling
            // that got missed.
            //
            // Copy as HTML is deliberately NOT included: it only ever means the
            // output window, so a dialog field does not make it available.
            wxWindow* activeTop = wxGetActiveWindow();
            const bool worldIsFrontmost =
                (activeTop == nullptr) ||
                (activeTop == frame)   ||
                (activeTop == frame->GetActiveChild());

            if (id == ID_EDIT_COPY && !worldIsFrontmost) {
                auto* te = dynamic_cast<wxTextCtrl*>(wxWindow::FindFocus());
                long from = 0, to = 0;
                if (te) te->GetSelection(&from, &to);
                evt.Enable(te != nullptr && to > from);
            }
            else
                evt.Enable(doc != nullptr && doc->EditCopyTarget() != eCopyNothing);
        } else if (const MenuCmdUiState st = MenuCommandUiState(id, doc);
                   st.handled) {
            // Every command MFC greys for us on Windows because its only
            // handler lives on the un-instantiated CMUSHView / CSendView (or on
            // the document), plus the three per-world check marks and the
            // Connection tri-state.  The rules are data in
            // platform/menu_ui_rules.cpp so they can be tested without a live
            // frame — see tests/test_menu_rules.cpp.
            apply(st);
        } else {
            evt.Skip();  // default behaviour for all other items
        }
    });

    // Handle Quit at the *application* level too, so it works no matter which
    // window is frontmost.  On macOS the app-menu Quit (wxID_EXIT, what Cmd-Q
    // fires) is dispatched to the frontmost window; when that's a standalone
    // frame (e.g. a Notepad, whose parent is null) the per-frame handler below
    // never sees the event.  wxTheApp is the last link in the command-event
    // chain, so a binding here catches Quit from any window that doesn't consume
    // it — while a world/main window stays frontmost, the frame handler consumes
    // it first and this never fires (no double close).
    wxTheApp->Bind(wxEVT_MENU,
                   [](wxCommandEvent&) { wx_quit_application(); },
                   wxID_EXIT);

    // Copy, same argument, for the same reason: it is on a non-standard id (see
    // the Edit menu) so AppKit no longer routes it to the focused text control
    // for us.  A modal bridged dialog is its own top-level window, and a menu
    // event it does not handle need not reach the MDI frame at all — wxTheApp is
    // the last link in the chain, so catch it here and copy from whatever field
    // has the focus.  The frame's own handler runs first when it is frontmost, so
    // this only fires for the windows the frame cannot speak for.
    wxTheApp->Bind(wxEVT_MENU,
                   [](wxCommandEvent& evt) {
                       if (auto* te = dynamic_cast<wxTextCtrl*>(wxWindow::FindFocus()))
                           te->Copy();
                       else
                           evt.Skip();
                   },
                   ID_EDIT_COPY);

    // ── Menu event routing via Bind() ─────────────────────────────────────
    frame->Bind(wxEVT_MENU, [frame](wxCommandEvent& evt) {
        int id = evt.GetId();

        // ── Canonical MFC ids → this frame's own ids ──────────────────────
        // world.DoCommand(name) posts the canonical id from accelerators.cpp's
        // CommandIDs[] (ID_EDIT_CUT = 0xE123, ID_FILE_CLOSE = 0xE102, …), but a
        // handful of those commands are built on wx's standard ids here because
        // AppKit wants them there.  The arms below matched only the wx id, so
        // DoCommand("Cut" / "Paste" / "SelectAll" / "Undo" / "Close" / "About" /
        // "CascadeWindows" / "TileWindows" / …) fell through to evt.Skip() and
        // did nothing at all — while still returning eOK to the script.
        // Translating up front fixes the menu and DoCommand in one place.
        {
            int wxid = 0;
            switch (PortMenuActionForCommandId(id)) {
            case kPortMenuCloseWorld:   wxid = wxID_CLOSE;      break;
            case kPortMenuCut:          wxid = wxID_CUT;        break;
            case kPortMenuPaste:        wxid = wxID_PASTE;      break;
            case kPortMenuSelectAll:    wxid = wxID_SELECTALL;  break;
            case kPortMenuUndo:         wxid = wxID_UNDO;       break;
            case kPortMenuAbout:        wxid = wxID_ABOUT;      break;
            case kPortMenuCascade:      wxid = wxID_MDI_WINDOW_CASCADE;      break;
            case kPortMenuTileHorz:     wxid = wxID_MDI_WINDOW_TILE_HORZ;    break;
            case kPortMenuTileVert:     wxid = wxID_MDI_WINDOW_TILE_VERT;    break;
            case kPortMenuArrangeIcons: wxid = wxID_MDI_WINDOW_ARRANGE_ICONS;break;
            case kPortMenuNone:                                 break;
            }
            if (wxid != 0) {
                // Re-enter under the port's own id rather than just rewriting
                // the local `id`: the tile/cascade commands are NOT handled in
                // this lambda at all — they reach wxMDIParentFrame's own static
                // event table by falling through to evt.Skip(), which uses the
                // EVENT's id, not ours.  A fresh event is the only way to give
                // them the same treatment a menu click gets.  (No recursion: the
                // wx ids are not canonical MFC ids, so the switch above misses
                // them the second time round.)
                wxCommandEvent fwd(wxEVT_MENU, wxid);
                fwd.SetEventObject(evt.GetEventObject());
                frame->GetEventHandler()->ProcessEvent(fwd);
                return;
            }
        }

        // Helper: get the doc for the currently active MDI child, or nullptr.
        auto activeDoc = [frame]() -> CMUSHclientDoc* {
            wxMDIChildFrame* active = frame->GetActiveChild();
            if (!active) return nullptr;
            auto* child = dynamic_cast<CWorldChildFrame*>(active);
            return child ? child->GetDoc() : nullptr;
        };

        // ── Table-driven canonical commands: route the click to its MFC
        // message-map handler.  One path for every standard command. ──────────
        if (const CanonMenuCmd* c = find_canon_cmd(id)) {
            if (CCmdTarget* tgt = canon_target(c->target, activeDoc()))
                AfxDispatchCommand(tgt, 0 /*ON_COMMAND*/, id);
            return;
        }

        // ── View menu (port-local; real wx, since the canonical Always-On-Top /
        // Full-Screen handlers bottom out in Windows-only SetWindowPos/ModifyStyle).
        if (id == ID_MENU_VIEW_ALWAYSONTOP) {
            // Drive App.m_bAlwaysOnTop, not the wx style flag, and persist it —
            // the same two statements CMainFrame::OnViewAlwaysontop runs
            // (mainfrm.cpp).  Toggling only the style, as this used to, left the
            // global preference untouched, so the setting never survived a
            // restart and MUSHclient.cpp's startup restore had nothing to read.
            App.m_bAlwaysOnTop = !App.m_bAlwaysOnTop;
            apply_always_on_top(frame, App.m_bAlwaysOnTop != 0);
            App.db_write_int("prefs", "AlwaysOnTop", App.m_bAlwaysOnTop);
            return;
        }
        if (id == ID_MENU_VIEW_FULLSCREEN) {
            wxTopLevelWindow* target = wx_visible_frame(frame);
            const bool goingFull = !target->IsFullScreen();
            if (goingFull) {
                s_preFullScreenRect    = target->GetRect();
                s_hadPreFullScreenRect = true;
            } else {
                s_hadPreFullScreenRect = false;
            }
            target->ShowFullScreen(goingFull);
            return;
        }
        if (id == ID_MENU_VIEW_STATUSBAR) {
            // One setting, applied to every world frame -- see s_statusBarVisible.
            s_statusBarVisible = !s_statusBarVisible;
            apply_status_bar_visibility(frame);
            return;
        }
        if (id == 0xE800 /*ID_VIEW_TOOLBAR*/ || id == ID_VIEW_GAME_TOOLBAR ||
            id == ID_VIEW_ACTIVITYTOOLBAR   || id == ID_VIEW_RESET_TOOLBARS) {
            // Windows has one set of bars on the main frame, so a toggle there
            // is global.  Here every world window owns a set, so apply the
            // toggle to all of them or they would drift apart.  Read the new
            // state from the active window and push it to the rest.
            CWorldToolBars::Which which =
                (id == ID_VIEW_GAME_TOOLBAR)    ? CWorldToolBars::eWorld    :
                (id == ID_VIEW_ACTIVITYTOOLBAR) ? CWorldToolBars::eActivity :
                                                  CWorldToolBars::eStandard;
            bool show = true;
            if (id != ID_VIEW_RESET_TOOLBARS) {
                auto* active = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild());
                show = active ? !active->AreToolBarsVisible(which) : true;
            }
            for (wxWindow* w : frame->GetChildren())
                if (auto* child = dynamic_cast<CWorldChildFrame*>(w)) {
                    if (id == ID_VIEW_RESET_TOOLBARS) child->ResetToolBars();
                    else                              child->ShowToolBar(which, show);
                }
            return;
        }

        // ── View-level output/input commands (canonical handlers live on the
        // un-instantiated CMUSHView/CSendView; reimplemented on the child frame).
        if (id == 32275 || id == 32276 || id == 32974 || id == 32272 ||
            id == 32152 || id == 33036 || id == 33044 || id == 32331 ||
            id == 32946 || id == 32947 || id == 33046 ||
            id == 32318 || id == 32319 || id == 32973) {
            if (auto* child = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild())) {
                switch (id) {
                case 32275: child->GotoSelectedURL();      break; // ID_DISPLAY_GOTOURL
                case 32276: child->SendMailToSelection();  break; // ID_DISPLAY_SENDMAILTO
                case 32974: child->CopySelectionHTML();    break; // ID_EDIT_COPYASHTML
                case 32272: child->RepeatLastCommand();    break; // ID_REPEAT_LAST_COMMAND
                case 32973: child->RecallLastWord();       break; // ID_REPEAT_LAST_WORD
                case 32152: child->ShowCommandHistory();   break; // ID_GAME_COMMANDHISTORY
                case 33036: child->HighlightSelectedWord();break; // ID_DISPLAY_HIGHLIGHTPHRASE
                case 33044: child->MakeMultiLineTrigger(); break; // ID_DISPLAY_MULTILINETRIGGER
                case 32331: child->ShowTextAttributes();   break; // ID_DISPLAY_TEXTATTRIBUTES
                case 32946: child->GotoMatchingBrace(false); break; // ID_EDIT_GOTOMATCHINGBRACE
                case 32947: child->GotoMatchingBrace(true);  break; // ID_EDIT_SELECTTOMATCHINGBRACE
                case 33046: child->ShowFunctionsList();    break; // ID_GAME_FUNCTIONSLIST
                case 32318: child->BookmarkSelection();    break; // ID_DISPLAY_BOOKMARKSELECTION
                case 32319: child->GotoBookmark();         break; // ID_DISPLAY_GOTOBOOKMARK
                }
            }
            return;
        }

        // ── Edit menu: Copy ────────────────────────────────────────────────
        // Routed by SELECTION, not by keyboard focus, exactly as Windows does
        // (CMUSHView::OnEditCopy prefers the output window's selection and
        // falls back to the command window's).  The shared decision lives in
        // CMUSHclientDoc::EditCopyTarget (doc_editcopy_shared.cpp) so it cannot
        // disagree with the update-UI arm below.
        //
        // This used to ask wxWindow::FindFocus() whether the output pane had
        // focus — but nothing ever calls SetFocus() on COutputPane, so the test
        // was permanently false and Copy always consulted the (unselected)
        // command bar and did nothing.
        if (id == ID_EDIT_COPY) {
            // Taking Copy off wxID_COPY (see the comment where the Edit menu is
            // built) also gives up AppKit's free "copy from whatever text control
            // has the focus", which is what made Cmd+C work inside a bridged
            // dialog's fields.  Hand that case back: if the frontmost window is
            // not this world's, the world has no selection to speak for.
            wxWindow* activeTop = wxGetActiveWindow();
            auto* child = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild());
            const bool worldIsFrontmost =
                (activeTop == nullptr) || (activeTop == child) || (activeTop == frame);

            if (!worldIsFrontmost) {
                if (auto* te = dynamic_cast<wxTextCtrl*>(wxWindow::FindFocus()))
                    te->Copy();
                return;
            }

            CMUSHclientDoc* doc = child ? child->GetDoc() : nullptr;
            switch (doc ? doc->EditCopyTarget() : eCopyNothing) {
            case eCopyOutputSelection:
                if (COutputPane* pane = child->OutputPane())
                    pane->CopySelection();
                break;
            case eCopyCommandSelection:
                if (wxTextCtrl* inp = child->ActiveInputPublic())
                    inp->Copy();
                break;
            case eCopyNothing:
                break;
            }
            return;   // consumed — do not call evt.Skip()
        }

        // ── Edit menu: Cut / Paste / Select All / Undo ─────────────────────
        // The frame's Bind intercepts menu events before wxMDI can route them
        // to the focused child control, so we dispatch explicitly here.  These
        // stay focus-routed, which is also what Windows does: Cut, Paste and
        // Undo only ever apply to the command window, and ID_EDIT_SELECT_ALL is
        // handled by BOTH CMUSHView and CSendView, so whichever has the focus
        // wins.  (On the port that means Select All can only reach the command
        // bar, since the output pane is never focused — a separate gap.)
        //
        // Undo has no menu item here; it is reachable through
        // world.DoCommand("Undo"), which posts ID_EDIT_UNDO (translated to
        // wxID_UNDO at the top of this handler).
        if (id == wxID_CUT || id == wxID_PASTE || id == wxID_SELECTALL ||
            id == wxID_UNDO)
        {
            wxWindow* focused = wxWindow::FindFocus();
            if (focused) {
                // Is it the output pane?
                if (auto* pane = dynamic_cast<COutputPane*>(focused)) {
                    if (id == wxID_SELECTALL) pane->SelectAll();
                    // Cut, Paste and Undo are no-ops on a read-only output pane.
                } else {
                    // Forward to focused window (wxTextCtrl, etc.) via its
                    // own event handler so the built-in editing works.
                    wxCommandEvent fwd(wxEVT_MENU, id);
                    fwd.SetEventObject(focused);
                    focused->GetEventHandler()->ProcessEvent(fwd);
                }
            }
            return;   // consumed — do not call evt.Skip()
        }

        // ── Global Change: find/replace within the active input bar ────────
        if (id == 32320 /*ID_INPUT_GLOBALCHANGE*/) {
            if (auto* child = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild()))
                child->GlobalChangeInput();
            return;
        }

        // ── Send to All Worlds: cross-world broadcast.  Runs the canonical
        // CMainFrame::OnGameSendtoallworlds() via the message map — it presents
        // CSendToAllDlg (multi-select world list, bridged through the wxListBox
        // hooks) and sends the entered text to every selected connected world.
        if (id == 32325 /*ID_GAME_SENDTOALLWORLDS*/) {
            AfxDispatchCommand(&Frame, BN_CLICKED /*0 = ON_COMMAND*/, 32325);
            return;
        }

        // ── Close All Notepad Windows (Edit menu) ──────────────────────────
        // The canonical CMainFrame::OnWindowCloseallnotepadwindows iterates
        // m_pNormalDocTemplate (MFC CTextDocument notepads), but on the port
        // notepad windows are wx frames (CWxNotepadFrame) — so close those.
        // Iterate by frame *pointer*, not by title: several scratch notepads
        // share the title "Notepad", so a title-keyed lookup + deferred Close()
        // would close one repeatedly and orphan the rest.  Snapshot the frames
        // first, because Close() mutates the top-level window list.
        if (id == 32951 /*ID_WINDOW_CLOSEALLNOTEPADWINDOWS*/) {
            std::vector<CWxNotepadFrame*> pads;
            for (wxWindowList::compatibility_iterator node = wxTopLevelWindows.GetFirst();
                 node; node = node->GetNext())
                if (auto* pad = dynamic_cast<CWxNotepadFrame*>(node->GetData()))
                    pads.push_back(pad);
            for (CWxNotepadFrame* pad : pads)
                pad->Close(false);   // false → still honour the SaveModified prompt
            return;
        }

        // ── Activity List: per-world overview window ───────────────────────
        if (id == 32203 /*ID_DISPLAY_ACTIVITYLIST*/) {
            ShowActivityWindow();
            return;
        }

        // ── The 17 named macro slots (up/down/north/…/logout/quit) ────────
        // These have no key binding on any platform: Windows reaches them only
        // by command id, through CSendView's message map — which is dead here,
        // CSendView never being instantiated.  So world.DoCommand("North"), the
        // Input menu and the world toolbar all posted a WM_COMMAND that arrived
        // as this wxEVT_MENU and fell straight through to evt.Skip().  DoCommand
        // returned eOK the whole time, making it a completely silent no-op.
        // FireNamedMacroCommand owns the id→MAC_* table and returns false for
        // anything that is not one of the 17, so ordering here is not delicate.
        if (auto* child = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild()))
            if (child->FireNamedMacroCommand(id))
                return;

        // ── Generate Character Name (random name → canonical dialog) ───────
        if (id == 32326 /*ID_EDIT_GENERATECHARACTERNAME*/) {
            if (auto* child = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild()))
                child->GenerateCharacterName();
            return;
        }

        // ── Check Spelling (input spell-check via canonical Lua + dialog) ──
        if (id == 32323 /*ID_EDIT_SPELLCHECK*/) {
            if (auto* child = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild()))
                child->SpellCheckInput();
            return;
        }

        // ── Print World (output buffer → wxHtmlEasyPrinting preview) ───────
        if (id == 32250 /*ID_FILE_PRINT_WORLD*/) {
            if (auto* child = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild()))
                child->PrintWorld();
            return;
        }

        // ── Display menu: find / find-again / freeze / goto-line ───────────
        if (id == 32248 /*ID_DISPLAY_FIND*/      || id == 32249 /*ID_DISPLAY_FINDAGAIN*/ ||
            id == 32201 /*ID_DISPLAY_FREEZEOUTPUT*/ || id == 32963 /*ID_DISPLAY_GOTOLINE*/) {
            if (auto* c = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild())) {
                if      (id == 32248) c->ShowFindBar(true);
                else if (id == 32249) c->DoFind(true);
                else if (id == 32201) c->FreezeOutput();
                else                  c->GotoLine();
            }
            return;
        }

        // ── Display menu: the three macOS-shaped clears ────────────────────
        // Each frees a different slice of the buffer; none of them is Clear
        // Output Buffer, which erases the lot and is the canonical command.
        if (id == ID_MENU_DISPLAY_CLEAR_TO_START   ||
            id == ID_MENU_DISPLAY_CLEAR_TO_MARK    ||
            id == ID_MENU_DISPLAY_CLEAR_SCROLLBACK ||
            id == ID_MENU_DISPLAY_CLEAR_SCREEN) {
            if (auto* c = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild())) {
                if      (id == ID_MENU_DISPLAY_CLEAR_TO_START)   c->ClearToStart();
                else if (id == ID_MENU_DISPLAY_CLEAR_TO_MARK)    c->ClearToPreviousMark();
                else if (id == ID_MENU_DISPLAY_CLEAR_SCROLLBACK) c->ClearScrollback();
                else                                             c->ClearScreen();
            }
            return;
        }

        // ── Display menu: connection diagnostics (toggle flags on the doc) ──
        if (id == 32971 /*ID_EDIT_DEBUGINCOMINGPACKETS*/ || id == 32334 /*ID_GAME_TRACE*/) {
            if (auto* c = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild())) {
                if (CMUSHclientDoc* doc = c->GetDoc()) {
                    bool& flag = (id == 32971) ? doc->m_bDebugIncomingPackets
                                               : doc->m_bTrace;
                    flag  = !flag;
                    const bool state = flag;
                    // Both features stay silent until there is traffic/activity,
                    // which makes the toggle look like it did nothing.  Confirm
                    // the new state in the output window and say where the real
                    // output will appear.
                    //
                    // ASCII only.  Note() takes bytes and the output window
                    // decodes them with the *world's* encoding, so a UTF-8
                    // em-dash in an app-generated string shows up as "â€”" in
                    // any world that is not set to UTF-8.
                    if (id == 32971)
                        doc->Note(state
                            ? "[Debug] Incoming/outgoing packet logging ON - once "
                              "connected, raw bytes (including telnet IAC "
                              "negotiation and other invisible protocol traffic) "
                              "appear in a \"Packet debug\" notepad window."
                            : "[Debug] Packet logging OFF.");
                    else
                        doc->Note(state
                            ? "[Trace] ON - trigger, alias and timer matches will "
                              "be shown here as TRACE: lines."
                            : "[Trace] OFF.");
                }
            }
            // The tick is NOT set here.  Both flags are per-WORLD, so a tick
            // written once at click time described whichever world happened to
            // be active then and went stale the moment the user switched worlds
            // — showing "Trace on" over a world whose m_bTrace was false, and
            // turning it ON when the user meant to turn it off.  The update-UI
            // arm re-reads the active document instead (menu_ui_rules.cpp),
            // exactly as ON_UPDATE_COMMAND_UI does on Windows.
            return;
        }

        // ── Edit ▸ Flip To Notepad (canonical, on the active world doc) ──────
        // Find-or-create this world's scratch notepad and bring it forward.
        if (id == 32953 /*ID_EDIT_FLIPTONOTEPAD*/) {
            if (auto* c = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild()))
                if (CMUSHclientDoc* doc = c->GetDoc())
                    AfxDispatchCommand(doc, BN_CLICKED, 32953 /*ID_EDIT_FLIPTONOTEPAD*/);
            return;
        }

        // ── Edit ▸ New Notepad (port-local: always a fresh standalone pad) ───
        // Mirrors the canonical CMainFrame::OnEditNotesworkarea argument for
        // argument (mainfrm.cpp), so a fresh pad picks up the default *input*
        // font and black-on-white, which the Notepad-prefs colours then
        // override in CreateTextWindow.  Only the title differs: the canonical
        // passes "", which would leave the window unnamed here.
        if (id == 40001 /*port-local: New standalone Notepad*/) {
            CreateTextWindow("",                 // contents
                             "Notepad",          // title
                             nullptr,            // no related world (standalone)
                             0,                  // unique document number
                             App.m_strDefaultInputFont,
                             App.m_iDefaultInputFontHeight,
                             App.m_iDefaultInputFontWeight,
                             App.m_iDefaultInputFontCharset,
                             RGB (0, 0, 0),
                             RGB (255, 255, 255),
                             "", "",             // search string / line preamble
                             false, false, false, false, false,
                             0, eNotepadNormal);
            return;
        }

        if (id == wxID_EXIT || id == 0xE141 /*ID_APP_EXIT*/) {
            wx_quit_application();   // see wx_quit_application above
        } else if (id == wxID_NEW || id == 0xE100 /*ID_FILE_NEW*/
                   || id == 32289 /*ID_CONNECTION_QUICK_CONNECT*/) {
            // Call the canonical App method for the command, and let IT own the
            // flow selection — that is where Windows keeps it.  OnFileNew sets
            // eNormalNewDocument (the World Properties General page,
            // doc_construct.cpp:852); OnConnectionQuickConnect sets
            // eQuickConnect, opens the document and resets afterwards.
            //
            // Both used to land on App.OnFileNew() while hook_new_world forced
            // eQuickConnect, so File > New World (Ctrl+N) opened the Quick
            // Connect box and the canonical new-world flow was unreachable.
            // Setting the flag around OnFileNew() does NOT work as a fix:
            // OnFileNew's own first line resets it (MUSHclient.cpp:1364).
            if (id == 32289 /*ID_CONNECTION_QUICK_CONNECT*/)
                App.OnConnectionQuickConnect();
            else
                App.OnFileNew();
        } else if (id == wxID_OPEN || id == 0xE101 /*ID_FILE_OPEN*/) {
            // Show a file-open dialog filtered to MUSHclient world files.
            // Start in the default world directory if one is configured.
            wxString defaultDir = wxString::FromUTF8(
                static_cast<const char*>(App.m_strDefaultWorldFileDirectory));
            wxFileDialog dlg(frame,
                             wxT("Open World File"),
                             defaultDir, wxEmptyString,
                             wxT("MUSHclient world files (*.mcl;*.mck;*.mcp)|*.mcl;*.mck;*.mcp|All files (*.*)|*.*"),
                             wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dlg.ShowModal() == wxID_OK)
                hook_open_world_file(dlg.GetPath().ToUTF8().data());
        } else if (id == wxID_CLOSE) {
            // Close the currently active MDI child (world window).
            if (wxMDIChildFrame* active = frame->GetActiveChild())
                active->Close();
        } else if (id == wxID_SAVE || id == 0xE103 /*ID_FILE_SAVE*/) {
            hook_save_active_world(frame, /*saveAs=*/false);
        } else if (id == wxID_SAVEAS || id == 0xE104 /*ID_FILE_SAVE_AS*/) {
            hook_save_active_world(frame, /*saveAs=*/true);
        } else if (id == wxID_FIND) {
            // Edit → Find…: show find bar in the active child frame.
            if (wxMDIChildFrame* active = frame->GetActiveChild()) {
                if (auto* child = dynamic_cast<CWorldChildFrame*>(active))
                    child->ShowFindBar(true);
            }
        } else if (id == 32239 /*ID_CONNECTION_CONNECT*/) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                if (doc->m_iConnectPhase == eConnectNotConnected) {
                    doc->m_bDisconnectOK = false;
                    doc->ConnectSocket();
                }
            }
        } else if (id == kPortMenuIdDisconnect ||
                   id == 32240 /*ID_CONNECTION_DISCONNECT*/) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                doc->DisconnectWorld();
                if (s_statusBar)
                    s_statusBar->SetStatusText(wxT("Disconnected."));
            }
        } else if (id == kPortMenuIdReconnect) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                if (doc->m_iConnectPhase == eConnectNotConnected) {
                    doc->m_bDisconnectOK = false;
                    doc->ConnectSocket();
                    if (s_statusBar)
                        s_statusBar->SetStatusText(wxT("Connecting…"));
                }
            }
        } else if (id == wxID_PREFERENCES /*macOS app-menu Settings…*/ ||
                   id == 32246 /*ID_FILE_PREFERENCES*/) {
            // BOTH ids, for the same reason ID_MENU_VIEW_FULLSCREEN exists (see
            // the comment by its definition): the menu item has to be
            // wxID_PREFERENCES so wx relocates it to the macOS application menu,
            // but accelerators.cpp publishes this command to scripts and macros
            // under the CANONICAL id, and world.DoCommand posts that id through
            // Frame.PostMessage -> hook_post_message -> wxEVT_MENU.  Handling
            // only the wx id meant DoCommand("GlobalPreferences") -- and any key
            // a user bound to it -- returned eOK and did nothing, because
            // DoCommand posts and reports success without ever learning whether
            // anything handled the message.
            hook_file_preferences(frame);
        } else if (id == 32158 /*ID_GAME_PREFERENCES*/) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->GamePreferences(-1);
        } else if (id == 32290 /*ID_GAME_RELOAD_SCRIPT_FILE*/) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                if (doc->m_bEnableScripts || !doc->m_strScriptFilename.IsEmpty()) {
                    doc->DisableScripting();
                    doc->CreateScriptEngine();
                    if (s_statusBar)
                        s_statusBar->SetStatusText(wxT("Script reloaded."));
                } else {
                    wxMessageBox(wxT("No script file is configured for this world.\n"
                                     "Set one in Game → World Properties → Scripting."),
                                 wxT("Reload Script"), wxOK | wxICON_INFORMATION, frame);
                }
            }
        } else if (id == ID_MENU_GAME_LUA_EVAL) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                CScriptEngine* eng = doc->GetScriptEngine();
                if (!eng || !eng->L) {
                    wxMessageBox(
                        wxT("No active Lua scripting engine for this world.\n"
                            "Enable scripting in Game → World Properties → Scripting."),
                        wxT("Execute Lua"), wxOK | wxICON_INFORMATION, frame);
                } else {
                    // Canonical immediate-execution dialog (IDD_IMMEDIATE): OnRun
                    // (mapped to IDOK) runs the entered script via the world's
                    // script engine and keeps the dialog open.
                    CImmediateDlg dlg;
                    dlg.m_pDoc = doc;
                    dlg.DoModal();
                }
            }
        } else if (id == 32983 /*ID_FILE_PLUGINS*/) {
            // Canonical plugin manager (IDD_PLUGINS): lists the active world's
            // plugins with Add/Delete/Enable/Disable/Reload/Edit/Show-Description.
            // Same inline pattern as Execute Lua — CPluginsDlg is a CDialog, so
            // DoModal() renders via hook_run_dialog and its CListCtrl populates
            // through the wxListCtrl bridge.
            if (CMUSHclientDoc* doc = activeDoc()) {
                CPluginsDlg dlg;
                dlg.m_pDoc = doc;
                dlg.DoModal();
            }
        } else if (id == wxID_ABOUT) {
            // Canonical About box (IDD_ABOUTBOX) via the app's message map —
            // CMUSHclientApp::OnAppAbout sets the version + web/changes links and
            // runs CAboutDlg::DoModal, which the bridge renders: the app-icon
            // logo (IDC_LOGO), version and links.  Its Credits/License buttons
            // open CCreditsDlg, which loads the bundled licence/credits "TEXT"
            // resources (Contents/Resources/text/) via hook_load_text_resource.
            AfxDispatchCommand(&App, BN_CLICKED /*0 = ON_COMMAND*/, ID_APP_ABOUT);
        } else if (id >= kRecentMenuBase &&
                   id <  kRecentMenuBase + kMaxRecentWorlds) {
            // File → Open Recent → pick a specific entry.
            int idx = id - kRecentMenuBase;
            if (idx < (int)s_recentWorlds.size())
                hook_open_world_file(s_recentWorlds[idx].c_str());
        } else if (id == ID_MENU_CFG_TRIGGERS) {
            // Open the canonical World Properties at the Triggers page (matches
            // Windows' Game ▸ Configure ▸ Triggers).  The page IS the manager —
            // its list, Add/Edit/Remove/Find/etc. are all canonical code.
            if (CMUSHclientDoc* doc = activeDoc())
                doc->GamePreferences(ePageTriggers);
        } else if (id == ID_MENU_CFG_ALIASES) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->GamePreferences(ePageAliases);
        } else if (id == ID_MENU_CFG_TIMERS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->GamePreferences(ePageTimers);
        } else if (id == ID_MENU_CFG_VARIABLES) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                doc->GamePreferences(ePageVariables);
            }
        } else if (id == ID_MENU_CFG_INFO) {
            // Canonical World Properties ▸ Info page (rc:3083 gives it
            // Shift+Ctrl+I).  The page has always existed in the treebook; the
            // Configure menu simply had no item for it, and the chord that
            // should have opened it was opening Timers instead.
            if (CMUSHclientDoc* doc = activeDoc())
                doc->GamePreferences(ePageInfo);
        } else if (id == ID_MENU_CFG_MACROS) {
            // Canonical Macros page (CPrefsP6) in the World Properties treebook;
            // the Keypad page (CPrefsP12) is its tree sibling.  Editing a macro
            // opens the canonical CEditMacro (Replace/Send Now/Insert radios).
            if (CMUSHclientDoc* doc = activeDoc())
                doc->GamePreferences(ePageMacros);
        } else if (id == ID_MENU_CFG_COLORS) {
            // Canonical ANSI Colour page (CPrefsP5) in the World Properties
            // treebook — its owner-draw swatches + CColourPickerDlg (RGB sliders)
            // are all canonical code now that the owner-draw + slider bridges land.
            if (CMUSHclientDoc* doc = activeDoc())
                doc->GamePreferences(ePageANSIColours);
        } else if (id == ID_MENU_IMPORT_TRIGGERS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Load_Set(TRIGGER, "", nullptr);
        } else if (id == ID_MENU_EXPORT_TRIGGERS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Save_Set(TRIGGER, nullptr);
        } else if (id == ID_MENU_IMPORT_ALIASES) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Load_Set(ALIAS, "", nullptr);
        } else if (id == ID_MENU_EXPORT_ALIASES) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Save_Set(ALIAS, nullptr);
        } else if (id == ID_MENU_IMPORT_TIMERS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Load_Set(TIMER, "", nullptr);
        } else if (id == ID_MENU_EXPORT_TIMERS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Save_Set(TIMER, nullptr);
        } else if (id == ID_MENU_IMPORT_COLORS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Load_Set(COLOUR, "", nullptr);
        } else if (id == ID_MENU_EXPORT_COLORS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Save_Set(COLOUR, nullptr);
        } else if (id == ID_MENU_IMPORT_MACROS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Load_Set(MACRO, "", nullptr);
        } else if (id == ID_MENU_EXPORT_MACROS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Save_Set(MACRO, nullptr);
        } else if (id == ID_CONN_LOG_TOGGLE) {
            CMUSHclientDoc* doc = activeDoc();
            if (!doc) { evt.Skip(); return; }
            // Route through the canonical File ▸ Log Session handler
            // (ID_FILE_LOGSESSION = 32117), which self-toggles: if a log is open it
            // CloseLog()s (writing the postamble + honouring the close-confirm pref);
            // otherwise it runs CLogDlg (preamble, previous-buffer dump, append vs
            // truncate, the m_bLogOutput/m_log_input/m_bLogNotes choices) and writes
            // the preamble. The previous raw fopen("a+")/fclose bypassed all of that
            // — losing the postamble and the user's logging preferences.
            AfxDispatchCommand(doc, 0 /*ON_COMMAND*/, 32117 /*ID_FILE_LOGSESSION*/);
        } else {
            // MFC's own command routing: a command the frame does not handle
            // goes to the active DOCUMENT next, and CMUSHclientDoc's message map
            // has 60 real ON_COMMAND entries.  The port's menu items carry
            // private ids and are handled above, so the CANONICAL ids only ever
            // arrive here from world.DoCommand(name) or an accelerator — and
            // they used to fall straight through to evt.Skip(): DoCommand posted
            // the id, nothing consumed it, and it still returned eOK.
            //
            // Measured blast radius (ids that are DoCommand-nameable, have a doc
            // handler, and no other port reference): exactly 16 — the fourteen
            // Game > Configure pages with no toolbar/menu equivalent, plus
            // "Info" and "Immediate".  Every one opens a dialog.  Anything the
            // document does not handle still skips, because AfxDispatchCommand
            // only reports TRUE when it actually found an entry.
            if (!AfxDispatchCommand(activeDoc(), 0 /*ON_COMMAND*/, id))
                evt.Skip();
        }
    });

    // Store the wxWindow* in m_hWnd (HWND = void* on macOS/Linux)
    self->m_hWnd = static_cast<void*>(frame);

    // CMainFrame::OnCreate is a no-op on non-Windows (returns 0 immediately).
    CREATESTRUCT cs{};
    if (auto* mdiFrame = dynamic_cast<CMDIFrameWnd*>(self)) {
        if (mdiFrame->OnCreate(&cs) == -1) {
            frame->Destroy();
            self->m_hWnd = nullptr;
            return FALSE;
        }
    }

    return TRUE;
}

// ── wx_seed_new_world_defaults ────────────────────────────────────────────
// Pre-load defaults for a freshly constructed world document, shared by File ▸
// New World and Open World File (they used to carry two copies of this).
//
// Colours are deliberately NOT touched.  The document constructor already
// installed Gammon's palette — SetDefaultAnsiColours + SetDefaultCustomColours
// (doc_construct.cpp:316-319, bodies in Utilities.cpp:765/786) — which is
// exactly what Windows shows, and neither OnNewDocument nor OnOpenDocument
// resets it (SetDefaults(false) walks OptionsTable only; the colour arrays are
// not option-table items).  Both hooks used to re-seed m_customtext[] from
// m_normalcolour[i % 8] with a black background here, which made custom colour
// 1 black-on-black for any world file with no <custom> section (the loader
// keeps the current member value when the attribute is absent), and destroyed
// the readable defaults a new world is supposed to get.
//
// The ANSI tables the two hooks also wrote by hand were byte-for-byte what
// SetDefaultAnsiColours had already put there, so they are simply gone.
//
// What remains is genuinely port-only: on Windows these three come from the
// "default world" settings MFC loads before the document is opened.
void wx_seed_new_world_defaults(CMUSHclientDoc* doc)
{
    if (!doc) return;
    doc->m_nWrapColumn = 80;    // CLine allocates m_nWrapColumn bytes for text
    doc->m_bUTF_8      = 1;     // use UTF-8 throughout; 4x multiplier in CLine
    doc->m_maxlines    = 5000;  // SetUpOutputWindow sizes m_pLinePositions from this
}

// ── Open World File ───────────────────────────────────────────────────────
// Creates a CMUSHclientDoc, calls OnOpenDocument(path) to parse the XML world
// file, then creates and shows a CWorldChildFrame for it.
// Returns the new doc on success, nullptr on failure (file not found, invalid
// XML, user pressed cancel on a nested dialog, etc.).
static void* hook_open_world_file(const char* path)
{
    wxMDIParentFrame* mdiParent = nullptr;
    if (App.m_pMainWnd)
        mdiParent = static_cast<wxMDIParentFrame*>(
                        static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd));
    if (!mdiParent || !path || !*path) return nullptr;

    // If this world file is already open, activate its window instead of opening
    // a second copy.  This happens when a world is BOTH in the auto-reopen
    // WorldList (opened by InitInstance) AND passed on the command line / double-
    // clicked / chosen from File > Recent.  Matches Windows MUSHclient, which
    // focuses the existing world rather than loading a duplicate.
    {
        char canon[4096];
        std::string keyNew = ::realpath(path, canon) ? canon : path;
        for (const auto& kv : s_docFrameMap) {
            if (!kv.first || !kv.second) continue;
            CString existing = kv.first->GetPathName();
            if (existing.IsEmpty()) continue;
            char canon2[4096];
            const char* ep = static_cast<const char*>((LPCTSTR) existing);
            std::string keyOld = ::realpath(ep, canon2) ? canon2 : ep;
            if (keyNew == keyOld) {
                kv.second->Activate();
                kv.second->Raise();
                return kv.first;            // already open → return the existing doc
            }
        }
    }

    CMUSHclientDoc* doc = new CMUSHclientDoc();

    // Set pre-load defaults (may be overridden by the world file).  The colour
    // tables are the constructor's business — see wx_seed_new_world_defaults.
    wx_seed_new_world_defaults(doc);

    // Load world settings from XML.  This calls Serialize() → Load_World_XML()
    // which parses ip_address, port, triggers, aliases, timers, etc.
    if (!doc->OnOpenDocument(path)) {
        delete doc;
        // NOT removed from App.m_strWorldList: that list is the user's curated
        // startup set, and a world being temporarily unreadable (a removable
        // volume, a typo in a hand-edited path) is not consent to delete it.
        // Windows leaves it alone too.
        return nullptr;
    }

    // Record the file path on the document (the base OnOpenDocument doesn't), so
    // it can be identified as already-open above, and so Save writes back here.
    doc->SetPathName(path);

    // NOTE: do NOT patch m_customtext[] here, or before the load either.  This
    // runs immediately AFTER OnOpenDocument() parsed the world's own custom
    // colours out of the XML (xml/xml_load_world.cpp, GET_COLOUR("text",
    // m_customtext[i])), so touching them here threw those colours away on
    // every single world open.  And a pre-load patch is no better: the loader
    // keeps the current member value when the attribute is absent, so it just
    // redefines what "keep the default" means — a world file with no <custom>
    // section came up with custom colour 1 as black text on a black background.
    // Worlds that specify no custom colours keep the constructor's defaults
    // (SetDefaultCustomColours, doc_construct.cpp:319), which is also what
    // Windows does.

    // Create and show the child frame.
    CWorldChildFrame* child = new CWorldChildFrame(mdiParent, doc);
    wx_register_world_frame(doc, child);  // keep the registry up-to-date
    child->UpdateTitle();
    // "Open worlds Maximized" (File > Preferences > General).  Its canonical
    // reader is CSendView::OnInitialUpdate (sendvw.cpp), which the port never
    // constructs, so this used to fill the main window UNCONDITIONALLY -- the
    // port always behaved as though the option were on, and the option's
    // default is OFF.  When it is off we leave the size CWorldChildFrame's
    // constructor restored (the last-used child size, defaulting to 800x600).
    //
    // Filling means sizing to the main window's whole frame in screen coords:
    // on macOS the MDI child becomes the single on-screen window, so this is
    // NOT Maximize() (which zooms to the full screen, defeating the ~200-column
    // width) and NOT the client size at (0,0) (which would move the window to
    // the screen corner and lose the title-bar height).
    const wxRect want = mdiParent->GetScreenRect();
    child->Show();
    if (App.m_bOpenWorldsMaximised && want.width > 100 && want.height > 100)
        child->SetSize(want);

    // All output plumbing (the IWorldView seam) and initial pane state (font, scrollback,
    // background) are applied inside CWorldChildFrame's constructor.

    // Flush any notes produced during OnOpenDocument.
    doc->UpdateAllViews(nullptr);

    // Deliberately NOT added to App.m_strWorldList.  That list means "open these
    // at startup" and on Windows has exactly one writer, the Global Preferences
    // page (dialogs/global_prefs/GlobalPrefs.cpp).  The port used to treat it as
    // session state — add on open, drop on close — which restored your worlds but
    // silently destroyed whatever set you had curated.  File > Open Recent is the
    // right home for "where have I been", and it is maintained here.
    recent_worlds_add(path);

    // NO auto-connect here.  The canonical rule already ran: OnOpenDocument
    // above calls OpenSession, whose last act is
    //   if (App.m_bAutoConnectWorlds) ConnectSocket();     (doc.cpp:500)
    // -- the global "connect to worlds automatically" preference.  The port used
    // to dial a second time gated on doc->m_connect_now, which is a different
    // setting entirely: the auto-LOGIN method (none / MUSH / Diku / MXP,
    // doc.h:349).  So a world configured to send a MUSH login connected on open
    // even with auto-connect switched off, and a world with no auto-login never
    // connected on open even with it switched on.

    return static_cast<void*>(doc);
}

// ── Default filename for a world that has never been saved ────────────────
// The scrub-and-suffix rule itself now lives in utilities_shared.cpp, next to
// the other pure string utilities and where tests/ can reach it; this is only
// the doc → name lookup.
wxString DefaultWorldFileNameForName(const CString& worldName)
{
    const wxString out =
        wxString::FromUTF8((const char*) DefaultWorldFileName(worldName));
    // See the header.  FromUTF8 yields "" for a name that is not valid UTF-8 --
    // an old world file carrying Latin-1 bytes, say -- and the Save dialog must
    // not open with an empty filename.  Before the scrub was hoisted, the
    // conversion happened FIRST and this fallback came out of the existing
    // IsEmpty test; converting last means saying it explicitly.
    return out.IsEmpty() ? wxT("world.mcl") : out;
}

static wxString DefaultWorldFileNameFor(CMUSHclientDoc* doc)
{
    return DefaultWorldFileNameForName(doc ? doc->m_mush_name : CString());
}


// ── Save Active World ─────────────────────────────────────────────────────
// Saves the doc of the currently active MDI child frame.
// If saveAs==true OR the doc has no path yet, shows a save-file dialog first.
static void hook_save_active_world(wxMDIParentFrame* frame, bool saveAs)
{
    if (!frame) return;

    // Identify the active MDI child frame.
    wxMDIChildFrame* active = frame->GetActiveChild();
    if (!active) return;
    CWorldChildFrame* child = dynamic_cast<CWorldChildFrame*>(active);
    if (!child) return;

    // Access the doc via the public accessor pattern: child has m_doc.
    // We use the OutputPane's parent (child frame) to reach the doc.
    // Since we can't access m_doc directly (it's private), we work through
    // a dedicated accessor.  For now, add a GetDoc() to CWorldChildFrame.
    CMUSHclientDoc* doc = child->GetDoc();
    if (!doc) return;

    // Determine the save path.
    CString  pathCStr = doc->GetPathName();
    wxString path     = wxString::FromUTF8(static_cast<const char*>(pathCStr));

    if (saveAs || path.IsEmpty()) {
        // Use the directory of the existing path, or fall back to the default
        // world file directory configured in preferences.
        wxString saveDir = path.IsEmpty()
            ? wxString::FromUTF8(static_cast<const char*>(App.m_strDefaultWorldFileDirectory))
            : wxFileName(path).GetPath();
        wxFileDialog dlg(frame,
                         wxT("Save World File"),
                         saveDir,
                         path.IsEmpty() ? DefaultWorldFileNameFor(doc)
                                       : wxFileName(path).GetFullName(),
                         wxT("MUSHclient world files (*.mcl)|*.mcl|All files (*.*)|*.*"),
                         wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK) return;
        path = dlg.GetPath();
    }

    if (path.IsEmpty()) return;

    std::string savePath = path.ToUTF8().data();

    // Capture the old path BEFORE saving: DoSave -> CDocument::DoSave calls
    // SetPathName, so reading it afterwards would return the new path and the
    // save-as world-list cleanup below would never fire.
    std::string oldPath = static_cast<const char*>(doc->GetPathName());

    // DoSave, not OnSaveDocument: the canonical doc.cpp::DoSave runs the
    // on_world_save script handler and the OnPluginWorldSave callback before
    // delegating to the base (which does the actual write).
    if (!doc->DoSave(savePath.c_str(), TRUE)) {
        wxMessageBox(wxT("Failed to save world file."), wxT("Save Error"), wxICON_ERROR | wxOK);
        return;
    }

    // Update the document's path so subsequent Ctrl+S uses the same file.
    doc->SetPathName(savePath.c_str());
    doc->SetModifiedFlag(FALSE);

    // Saving a world does not change which worlds open at startup — see the
    // note in hook_open_world_file.
    recent_worlds_add(savePath);
}

// (Quick Connect is no longer a hand-built wx dialog: the canonical
// CQuickConnectDlg uses the inherited CDialog::DoModal, which the bridge renders
// from the recovered IDD_QUICK_CONNECT template — see doc_construct.cpp.)

// (The hand-built GlobalPrefsDlg + its MakeDirRow helper were removed: File ▸
//  Preferences now renders the canonical CGlobalPrefsSheet (12 CGlobalPrefsP*
//  pages) through the CPropertySheet bridge — see hook_file_preferences below
//  and hook_run_property_sheet_flat.)

// File ▸ Preferences — the app-wide global preferences sheet.  Routes to the
// canonical DoGlobalPrefs(), which builds CGlobalPrefsSheet (the 12 CGlobalPrefsP*
// pages) and runs the real load → DoModal → save (SaveGlobalsToDatabase) logic;
// the base CPropertySheet bridge renders the sheet as a wxNotebook (see
// hook_run_property_sheet_flat).  Replaces the former hand-built GlobalPrefsDlg.
void DoGlobalPrefs(CMUSHclientDoc* pCurrentDoc);   // dialogs/global_prefs/GlobalPrefs.cpp

// Apply MUSHclient's tray/taskbar-icon choice to the macOS Dock icon.  macOS has
// no system tray, so the Dock icon is where that choice lives here.  Index 0
// (the default) keeps the real app icon (IDR_MAINFRAME); 1..9 select the novelty
// icons (IDI_ICON1..9); 10 loads the user's custom .ico file.
static void ApplyDockIconFromPrefs()
{
    int sel = (int) App.m_iTrayIcon;
    wxBitmap bmp;
    if (sel >= 1 && sel <= 9)
        bmp = LoadDialogArt(287 + sel);          // IDI_ICON1 (288) .. IDI_ICON9 (296)
    else if (sel == 10 && !App.m_strTrayIconFileName.IsEmpty())
    {
        wxImage im;
        if (im.LoadFile(wxString::FromUTF8((const char*) App.m_strTrayIconFileName)))
            bmp = wxBitmap(im);
    }
    if (!bmp.IsOk())
        bmp = LoadDialogArt(128);                // IDR_MAINFRAME — the app icon (default)
    wxOSXSetDockIcon(bmp);
}

static void hook_file_preferences(wxWindow* /*parent*/)
{
    DoGlobalPrefs(NULL);
    ApplyDockIconFromPrefs();   // the Tray/Taskbar page may have changed the icon
    // Push the App-level settings that each world's output pane caches at
    // construction (currently the double-click word delimiters).  Without this
    // they were applied once per world open and a prefs change appeared to do
    // nothing until the world was closed and reopened.
    for (auto& kv : s_docFrameMap)
        if (kv.second)
            kv.second->ApplyGlobalPrefs();
}

// ── WorldPreferencesDlg ───────────────────────────────────────────────────
// Dialog for editing world properties: connection, display, scripting, options.
// Covers the settings needed to connect and run Lua scripts without the full
// Windows property-sheet UI.


// ── hook_get_clipboard_text / hook_set_clipboard_text ─────────────────────────
// System clipboard access for non-Windows.  Used by GetClipboardContents() and
// putontoclipboard() in doc.cpp (which power the world.GetClipboard() /
// world.SetClipboard() Lua API, the colour-from-clipboard picker, and
// IsClipboardXML()).
static char* hook_get_clipboard_text()
{
    if (!wxTheClipboard->Open()) return nullptr;
    char* result = nullptr;
    if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT) ||
        wxTheClipboard->IsSupported(wxDF_TEXT))
    {
        wxTextDataObject data;
        if (wxTheClipboard->GetData(data))
        {
            // The wxString MUST be a named local.  wxString::ToUTF8() may hand
            // back a buffer that points into the string's OWN storage rather
            // than a copy, so calling it on the temporary returned by GetText()
            // leaves the buffer dangling the moment the full expression ends —
            // and the length field survives that while the bytes do not.  This
            // was written as `data.GetText().ToUTF8()` and read back as an
            // empty string every time, on a buffer still reporting its true
            // length.  Everything downstream believed the clipboard was empty:
            // world.GetClipboard() returned "", and Edit ▸ Convert Clipboard
            // Forum Codes reported "0 changes made" and then wrote the empty
            // result BACK, destroying whatever the user had copied.
            const wxString  text = data.GetText();
            const wxScopedCharBuffer buf = text.ToUTF8();
            if (buf.length() > 0)
                result = strdup(buf.data());
        }
    }
    wxTheClipboard->Close();
    return result;   // caller must free()
}

static void hook_set_clipboard_text(const char* text)
{
    if (!text) return;
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text)));
        wxTheClipboard->Close();
    }
}

// ── Bridged-dialog parent stack ──────────────────────────────────────────────
// Innermost-last stack of bridged modal dialogs currently on screen (pushed by
// hook_run_dialog / hook_run_property_sheet around their ShowModal).  Nested
// native pickers (file/font) parent to the front entry: inside a bridged
// dialog's modal loop on macOS wxGetActiveWindow() returns null, and the other
// fallback — the MDI parent frame — is decoupled and may sit off-screen, so a
// picker parented to it can open invisibly (looking like the button did nothing).
std::vector<wxWindow*>& BridgedDialogStack()
{
    static std::vector<wxWindow*> s;
    return s;
}
static wxWindow* ActiveDialogParent()
{
    if (!BridgedDialogStack().empty()) return BridgedDialogStack().back();
    wxWindow* w = wxGetActiveWindow();
    return w ? w : (wxTheApp ? wxTheApp->GetTopWindow() : nullptr);
}

// The MDI "&Window" menu, or null.  wxMDIParentFrame auto-populates it with the
// open world child frames; we add disabled entries for modal bridged dialogs.
static wxMenu* MdiWindowMenu()
{
    wxWindow* top = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    wxMDIParentFrame* mdi = wxDynamicCast(top, wxMDIParentFrame);
    if (!mdi || !mdi->GetMenuBar()) return nullptr;
    int idx = mdi->GetMenuBar()->FindMenu(wxT("Window"));
    return (idx == wxNOT_FOUND) ? nullptr : mdi->GetMenuBar()->GetMenu(idx);
}

// Mark a bridged modal dialog as on screen: push it (so nested pickers parent to
// it) and add a disabled Window-menu entry so the user sees it's open even though
// it's app-modal.  Pair with pop_bridged_dialog().
static wxMenuItem* push_bridged_dialog(wxWindow* dlg)
{
    BridgedDialogStack().push_back(dlg);
    wxMenu* wm = MdiWindowMenu();
    if (!wm) return nullptr;
    wxString title = dlg->GetLabel();           // a TLW's label is its title
    if (title.IsEmpty()) title = wxT("Dialog");
    wxMenuItem* item = wm->Append(wxID_ANY, title);
    if (item) item->Enable(false);              // can't switch to an app-modal dialog
    return item;
}

static void pop_bridged_dialog(wxMenuItem* item)
{
    if (item) { if (wxMenu* wm = MdiWindowMenu()) wm->Destroy(item); }
    if (!BridgedDialogStack().empty()) BridgedDialogStack().pop_back();
}

// ── hook_file_dialog ──────────────────────────────────────────────────────────
// Generic file-open / file-save dialog used by CFileDialog::DoModal() on
// non-Windows.  Converts the MFC-style pipe-separated filter string to a
// wxFileDialog wildcard string, shows the dialog, and returns the selected path
// as a heap-allocated (malloc) UTF-8 string.  The caller must free() it.
// Returns nullptr if the user cancels or if something goes wrong.
static char* hook_file_dialog(bool bOpen,
                               const char* title,
                               const char* defaultDir,
                               const char* defaultFile,
                               const char* filter,
                               bool overwrite)
{
    // Convert MFC filter "Desc1|*.ext1|Desc2|*.ext2||" → wx "Desc1|*.ext1|Desc2|*.ext2"
    // MFC uses | between tokens and || (or trailing |) to end the list.
    // wx uses the same | separator but without a trailing || terminator.
    wxString wxFilter;
    if (filter && *filter) {
        wxFilter = wxString::FromUTF8(filter);
        // Strip trailing |s (MFC uses "||" as end-of-list sentinel).
        while (!wxFilter.IsEmpty() && wxFilter.Last() == '|')
            wxFilter.RemoveLast();
    }
    if (wxFilter.IsEmpty())
        wxFilter = wxT("All files (*.*)|*.*");

    wxString wxTitle = (title && *title)
                       ? wxString::FromUTF8(title)
                       : wxString(bOpen ? wxT("Open") : wxT("Save As"));
    wxString wxDir  = (defaultDir  && *defaultDir)
                      ? wxString::FromUTF8(defaultDir)  : wxString(wxEmptyString);
    wxString wxFile = (defaultFile && *defaultFile)
                      ? wxString::FromUTF8(defaultFile) : wxString(wxEmptyString);

    long style = bOpen
        ? (wxFD_OPEN | wxFD_FILE_MUST_EXIST)
        : (wxFD_SAVE | (overwrite ? wxFD_OVERWRITE_PROMPT : 0));

    // Parent to the front-most active window — usually the modal dialog this was
    // opened from (e.g. the Plugins manager's "Add…").  Parenting to the MDI
    // frame instead can leave the file dialog behind an app-modal dialog on
    // macOS, so it looks like nothing happened.  (hook_font_dialog does the same.)
    wxWindow* parent = ActiveDialogParent();

    wxFileDialog dlg(parent, wxTitle, wxDir, wxFile, wxFilter, style);
    if (dlg.ShowModal() != wxID_OK)
        return nullptr;

    return strdup(dlg.GetPath().ToUTF8().data());
}

// ── hook_font_dialog ──────────────────────────────────────────────────────────
// CFontDialog::DoModal() on non-Windows.  Seeds wxFontDialog from the incoming
// LOGFONT-style fields (face + lfHeight + weight + italic), shows it, and writes
// the chosen font back into *io.  Returns 1 on OK, 0 on cancel.  The point size
// is recovered from lfHeight using the same display PPI that GetDeviceCaps
// (LOGPIXELSY) reports, so it round-trips the caller's MulDiv exactly.
static int hook_font_dialog(void* /*parentHwnd*/, WxFontChoice* io)
{
    if (!io) return 0;

    wxWindow* parent = ActiveDialogParent();

    long h   = io->lfHeight < 0 ? -io->lfHeight : io->lfHeight;
    int  dpi = wxGetDisplayPPI().y;
    int  points = (dpi > 0 && h != 0)
                  ? (int)((h * 72 + dpi / 2) / dpi)
                  : (io->pointSize > 0 ? io->pointSize : 10);
    if (points <= 0) points = 10;

    wxFont initial(points,
                   wxFONTFAMILY_DEFAULT,
                   io->italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL,
                   io->weight >= FW_BOLD ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL,
                   false,
                   wxString::FromUTF8(io->faceName));

    wxFontData data;
    // Effects ON, and seed the colour from the caller.  Windows' CFontDialog is
    // opened with CF_EFFECTS and both takes and returns a colour; here effects
    // were disabled and the incoming io->color was dropped on the floor, so
    // ret.GetColour() handed back wxFontData's default and every caller —
    // including utils.fontpicker, whose third argument IS a preferred colour —
    // got black no matter what it asked for or what the user chose.
    data.EnableEffects(true);
    data.SetColour(wxColour(GetRValue(io->color),
                            GetGValue(io->color),
                            GetBValue(io->color)));
    if (initial.IsOk()) data.SetInitialFont(initial);

    wxFontDialog dlg(parent, data);
    if (dlg.ShowModal() != wxID_OK) return 0;

    wxFontData ret = dlg.GetFontData();
    wxFont     f   = ret.GetChosenFont();
    if (!f.IsOk()) return 0;

    wxString face = f.GetFaceName();
    strncpy(io->faceName, face.utf8_str(), sizeof(io->faceName) - 1);
    io->faceName[sizeof(io->faceName) - 1] = 0;
    io->pointSize = f.GetPointSize();
    io->weight    = (f.GetWeight() >= wxFONTWEIGHT_BOLD) ? FW_BOLD : FW_NORMAL;
    io->italic    = (f.GetStyle() != wxFONTSTYLE_NORMAL) ? 1 : 0;
    io->underline = f.GetUnderlined()    ? 1 : 0;
    io->strikeout = f.GetStrikethrough() ? 1 : 0;
    wxColour c    = ret.GetColour();
    io->color     = RGB(c.Red(), c.Green(), c.Blue());
    return 1;
}

// ── hook_save_world ───────────────────────────────────────────────────────────
// Saves the world file for the given doc.  If saveAs is true or the doc has no
// path, opens a save-file dialog.  parent is a wxWindow* used as dialog parent.
// Returns true if the file was saved, false if cancelled or on error.
static bool hook_save_world(void* vdoc, bool saveAs, void* vparent)
{
    auto* doc    = static_cast<CMUSHclientDoc*>(vdoc);
    auto* parent = static_cast<wxWindow*>(vparent);

    CString  pathCStr = doc->GetPathName();
    wxString path     = wxString::FromUTF8(static_cast<const char*>(pathCStr));

    if (saveAs || path.IsEmpty()) {
        wxString saveDir = path.IsEmpty()
            ? wxString::FromUTF8(
                  static_cast<const char*>(App.m_strDefaultWorldFileDirectory))
            : wxFileName(path).GetPath();
        wxFileDialog dlg(parent,
                         wxT("Save World File"),
                         saveDir,
                         path.IsEmpty() ? DefaultWorldFileNameFor(doc)
                                        : wxFileName(path).GetFullName(),
                         wxT("MUSHclient world files (*.mcl)|*.mcl"
                             "|All files (*.*)|*.*"),
                         wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK) return false;
        path = dlg.GetPath();
    }

    if (path.IsEmpty()) return false;

    std::string savePath = path.ToUTF8().data();

    // Old path first — DoSave calls SetPathName (see the File > Save hook).
    std::string oldPath = static_cast<const char*>(doc->GetPathName());

    // DoSave runs on_world_save + OnPluginWorldSave, then writes.
    if (!doc->DoSave(savePath.c_str(), TRUE)) {
        wxMessageBox(wxT("Failed to save world file."),
                     wxT("Save Error"), wxICON_ERROR | wxOK, parent);
        return false;
    }

    // Update path and clear the modified flag.  The startup world list is not
    // touched — see the note in hook_open_world_file.
    doc->SetPathName(savePath.c_str());
    doc->SetModifiedFlag(FALSE);
    recent_worlds_add(savePath);
    return true;
}

// ── New-world hook ────────────────────────────────────────────────────────
// "Open a new world" flow:
//   1. Create CMUSHclientDoc, call OnNewDocument (→ Quick Connect dialog
//      → SetUpOutputWindow → OpenSession).  Lines accumulate in m_LineList.
//   2. On cancel/error: delete the doc and return nullptr.
//   3. On success: create CWorldChildFrame (the doc's IWorldView), show it,
//      then flush all buffered lines (welcome banner + any MUD output so far).

static void* hook_new_world()
{
    // Retrieve the MDI parent frame.
    // InitInstance creates `new CMainFrame` and stores it in App.m_pMainWnd;
    // that object's m_hWnd is the wxMDIParentFrame* set by hook_load_mdi_frame.
    // The global `Frame` stub is a separate object whose m_hWnd is always null.
    wxMDIParentFrame* mdiParent = nullptr;
    if (App.m_pMainWnd)
        mdiParent = static_cast<wxMDIParentFrame*>(
                        static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd));
    if (!mdiParent) return nullptr;

    CMUSHclientDoc* doc = new CMUSHclientDoc();

    // NOT set here: App.m_TypeOfNewDocument is the CALLER's choice of flow
    // (see the File > New World / Quick Connect arm of the menu dispatcher).
    // Forcing eQuickConnect here made every route into this hook show the Quick
    // Connect dialog.

    // Initialise display-critical fields that SetUpOutputWindow uses to size
    // the first CLine text buffer.  They will be overwritten later if the user
    // opens an actual world file or the window is resized.  Colours are NOT
    // touched — see wx_seed_new_world_defaults.
    wx_seed_new_world_defaults(doc);

    // OnNewDocument: shows dialog, calls SetUpOutputWindow, OpenSession.
    // Lines produced by Note() accumulate in doc->m_LineList but are not
    // forwarded to the pane yet (the IWorldView is attached after this by the
    // CWorldChildFrame constructor; UpdateAllViews below drains the buffer).
    if (!doc->OnNewDocument()) {
        delete doc;   // user cancelled — destructor cleans up
        return nullptr;
    }

    // ── Custom colours after OnNewDocument: leave them alone ─────────────
    // OnNewDocument → SetDefaults() sets m_iNoteTextColour = 4 (custom palette
    // index), and the constructor's SetDefaultCustomColours already made
    // m_customtext[4] = RGB(0,128,255) — a readable blue on black, which is the
    // colour Windows notes come out in.  The port used to overwrite all sixteen
    // slots with light grey here, which was only ever needed because the block
    // above (now gone) had re-seeded the palette from m_normalcolour, making
    // slot 4 dark blue.  The grey patch then made every custom colour identical,
    // so a trigger or ColourNote picking colours 1-16 rendered them all the
    // same, and saving the world wrote the grey palette into the file for good.

    // Create and show the child frame.
    CWorldChildFrame* child = new CWorldChildFrame(mdiParent, doc);
    wx_register_world_frame(doc, child);  // keep the registry up-to-date
    child->UpdateTitle();   // set title from m_mush_name
    {   // fill the main window, but only if asked (see hook_open_world_file)
        const wxRect want = mdiParent->GetScreenRect();
        child->Show();
        if (App.m_bOpenWorldsMaximised && want.width > 100 && want.height > 100)
            child->SetSize(want);
    }

    // All output plumbing (the IWorldView seam) and initial pane state (font, scrollback,
    // background) are applied inside CWorldChildFrame's constructor.

    // Flush welcome banner + any output buffered during OnNewDocument.
    doc->UpdateAllViews(nullptr);

    return static_cast<void*>(doc);
}

// ── First-run seeding of bundled user data ────────────────────────────────
// A "Discworld edition" build (packaging/build-app.sh --with-discworld) ships a
// tree under Contents/Resources/userdata-seed/ that mirrors the per-user data
// dir: Quow's plugins (worlds/plugins/Quow*.xml) AND the mapper's data files
// (quow_plugins/maps/ — the room database, map images and sound effects).  On
// first run, copy anything the user doesn't already have into their writable
// data dir, so the plugins appear in the manager and the mapper finds its maps.
//
// Recursive (so the maps/ subtree is seeded, not just top-level plugin files)
// and non-destructive: directories are created and an existing file is NEVER
// overwritten — user edits, newer versions, and the per-user
// _quowmap_userdata.db the plugin creates for itself all survive a reinstall.
// No-op for a normal build (no userdata-seed dir present).
static void SeedBundledUserData()
{
    namespace fs = std::filesystem;
    try
    {
        // wxStandardPaths resolves Contents/Resources reliably (vs. guessing the
        // exe path).  On Linux this is the install prefix's share dir.
        const std::string resDir (wxStandardPaths::Get ().GetResourcesDir ().utf8_str ());
        const fs::path src = fs::path (resDir) / "userdata-seed";
        if (!fs::is_directory (src))
            return;   // ordinary build — nothing bundled to seed

        const fs::path dstRoot (PortableUserDataDir ());
        int seeded = 0;
        std::error_code ec;
        for (fs::recursive_directory_iterator it (src, ec), end; !ec && it != end;
             it.increment (ec))
        {
            std::error_code fec;
            if (!it->is_regular_file (fec) || fec)
                continue;
            const fs::path rel = fs::relative (it->path (), src, fec);
            if (fec || rel.empty ())
                continue;
            const fs::path target = dstRoot / rel;
            if (fs::exists (target, fec))
                continue;   // keep the user's existing copy
            fs::create_directories (target.parent_path (), fec);
            std::error_code cec;
            if (fs::copy_file (it->path (), target, cec) && !cec)
                ++seeded;
        }
        if (seeded)
            fprintf (stderr, "[plugins] seeded %d bundled file(s) into %s\n",
                     seeded, dstRoot.string ().c_str ());
    }
    catch (const std::exception & ex)
    {
        fprintf (stderr, "[plugins] bundled-data seeding skipped: %s\n", ex.what ());
    }
}

// ── App InitInstance bootstrap ────────────────────────────────────────────
// Called by wx_app.cpp::OnInit() via the hook pointer to avoid including
// MUSHclient.h in wx_app.cpp (which would conflict with wx headers).

static int hook_init_instance()
{
    // Resolve plugin-relative file paths against the user data dir — the macOS
    // analogue of the Windows "MUSHclient folder".  MUSHclient captures its
    // working directory from getcwd() ONCE at startup (the global `working_dir`,
    // the base Make_Absolute_Path prepends to relative paths), so the launch CWD
    // must already be the data dir — a Finder/Dock launch starts at "/", where a
    // plugin opening e.g. ".\quow_plugins\maps\_quowmap_database.db" (Quow's
    // mapper) would never find its data.  Must run BEFORE App.InitInstance()
    // captures working_dir.  (The later SetCurrentDirectory() calls only bracket
    // the save-file dialogs and restore the CWD, so they don't move this base.)
    {
        const std::string dataDir = PortableUserDataDir ();
        if (!dataDir.empty () && chdir (dataDir.c_str ()) != 0)
            fprintf (stderr, "[startup] could not chdir to %s\n", dataDir.c_str ());
    }

    // Set basic CWinApp members that MFC normally sets before InitInstance.
    App.m_lpCmdLine = "";
    App.m_nCmdShow  = SW_SHOWNORMAL;

    // Stage any bundled plugins + mapper data BEFORE InitInstance so worlds
    // auto-opened during startup already see them (Discworld edition only).
    SeedBundledUserData();

    int ok = App.InitInstance() != FALSE ? 1 : 0;
    if (ok)
        ApplyDockIconFromPrefs();   // honour the saved tray/taskbar-icon choice
    return ok;
}

// ── hook_open_default_world ───────────────────────────────────────────────
// Open the bundled default world if NOTHING else is open.  The world to open is
// named in Contents/Resources/default-world (written by build-app.sh
// --with-discworld — e.g. "Discworld.mcl"); a normal build has no such marker,
// so this is a no-op there.  Gated on zero open worlds so it never duplicates a
// world already opened from the saved startup list (WorldList) or one passed on
// the command line.  Called from wx_app.cpp::OnInit() AFTER the argv worlds.
static void hook_open_default_world()
{
    namespace fs = std::filesystem;
    try
    {
        // Something already open (saved WorldList, command-line world)? Leave it.
        if (App.m_pWorldDocTemplate &&
            App.m_pWorldDocTemplate->GetFirstDocPosition())
            return;

        const std::string resDir (wxStandardPaths::Get ().GetResourcesDir ().utf8_str ());
        const fs::path marker = fs::path (resDir) / "default-world";
        std::error_code ec;
        if (!fs::is_regular_file (marker, ec))
            return;   // ordinary build — no default world bundled

        std::ifstream in (marker);
        std::string name;
        std::getline (in, name);
        while (!name.empty () && (name.back () == '\r' || name.back () == '\n' ||
                                  name.back () == ' '  || name.back () == '\t'))
            name.pop_back ();
        if (name.empty ())
            return;

        const fs::path world = fs::path (PortableUserDataDir ()) / "worlds" / name;
        if (!fs::is_regular_file (world, ec))
        {
            fprintf (stderr, "[startup] default world not found: %s\n",
                     world.string ().c_str ());
            return;
        }
        fprintf (stderr, "[startup] opening default world: %s\n",
                 world.string ().c_str ());
        hook_open_world_file (world.string ().c_str ());
    }
    catch (const std::exception & ex)
    {
        fprintf (stderr, "[startup] default-world open skipped: %s\n", ex.what ());
    }
}

// ── SaveWindowGeometryTo ──────────────────────────────────────────────────
// Persist `frame`'s size / position / maximized state into `cfg`.  Split out of
// SaveMainWindowGeometry (below) so the in-app selftest can drive it against a
// throwaway config file instead of the user's real preferences.  Safe to call
// with a null frame.
void SaveWindowGeometryTo(wxConfigBase& cfg, wxTopLevelWindow* frame)
{
    if (!frame || frame->IsIconized()) return;
    // On macOS the window the user actually sees and resizes is the active MDI
    // *child* (it fills the frame and becomes the single on-screen window); the
    // MDI parent does NOT track the child's size.  So persist the active child's
    // geometry when there is one — otherwise a resize is silently lost and the
    // next launch reverts to the default size.  (On real-MDI platforms the child
    // is not a top-level window, so we fall back to the parent.)
    wxTopLevelWindow* geomWin = frame;
    if (wxMDIParentFrame* mdi = wxDynamicCast(frame, wxMDIParentFrame)) {
        if (wxMDIChildFrame* active = mdi->GetActiveChild()) {
            if (wxTopLevelWindow* tlw = wxDynamicCast(active, wxTopLevelWindow))
                if (!tlw->IsIconized())
                    geomWin = tlw;
        }
    }
    const bool maximized = geomWin->IsMaximized();
    cfg.Write(wxT("/geometry/main_maximized"), maximized);
    cfg.Write(wxT("/geometry/status_bar"), s_statusBarVisible);
    // Full screen is not a size worth restoring, and on macOS the exit is
    // animated so GetRect() here would still report the full-screen bounds
    // even if we asked it to leave first.  Use the rect stashed when the
    // user entered full screen.
    const wxRect r = (geomWin->IsFullScreen() && s_hadPreFullScreenRect)
                     ? s_preFullScreenRect
                     : geomWin->GetRect();
    if (!maximized) {
        cfg.Write(wxT("/geometry/main_x"), (long)r.x);
        cfg.Write(wxT("/geometry/main_y"), (long)r.y);
        cfg.Write(wxT("/geometry/main_w"), (long)r.width);
        cfg.Write(wxT("/geometry/main_h"), (long)r.height);
    }
    // The main_* keys above are read back onto the MDI *parent*, which wxOSX
    // never shows.  When the window we just measured is the child — i.e. the
    // window the user can actually see and drag — its size also has to go into
    // the keys the child reads at construction, or a Cmd-Q (which never runs
    // the child's close handler) persists the size somewhere nothing looks and
    // the next launch reopens the world at the previous size.  Same helper as
    // that close handler, so the two paths cannot drift apart again.
    if (geomWin != frame)
        wx_save_child_geometry(cfg, r.GetSize());
}

// ── SaveMainWindowGeometry ────────────────────────────────────────────────
// Persist the main window geometry to the real wxConfig so the next session
// opens the same.  Called from the parent's close handler AND from the quit
// hook, because [NSApp terminate:] does not reliably fire the close handler (so
// the geometry would otherwise be saved only when you close the window, not
// when you Quit).  Safe to call with a null frame.
static void SaveMainWindowGeometry(wxTopLevelWindow* frame)
{
    const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
    SaveWindowGeometryTo(cfg, frame);
    cfg.Flush();
}

// ── hook_query_quit_save ──────────────────────────────────────────────────
// Called from CMUSHWxApp::OSXOnShouldTerminate() — every quit path on macOS
// (Cmd-Q, the app menu, the Dock's Quit) funnels through Cocoa's
// applicationShouldTerminate:.  -[NSApp terminate:] does NOT run the MFC document
// close/save flow, so without this a quit silently discards unsaved world edits
// and loses plugin layout (Quow's miniwindow positions).  Returns 1 to allow the
// quit, 0 to cancel it (the user picked Cancel at a save prompt).
// MFC-parity top-level exception net (mirrors CWinApp::ProcessWndProcException):
// on Windows a CException* escaping a command handler — e.g. ThrowErrorException
// from regcomp on a bad user-typed regexp in the Find/Recall dialogs — is caught
// by the framework, reported in a message box, and the app keeps running.  wx has
// no such net, so wxApp::OnExceptionInMainLoop calls this from its catch block.
// Rethrows the in-flight exception; returns 1 if it was an MFC exception
// (reported + freed).  Anything else propagates back out unchanged.
static int hook_report_mfc_exception()
{
    try
    {
        throw;   // rethrow the exception OnExceptionInMainLoop caught
    }
    catch (CException* e)
    {
        char sMessage[500] = "";
        e->GetErrorMessage(sMessage, sizeof sMessage);
        e->Delete();
        ::UMessageBox(sMessage[0] ? sMessage : "An unexpected error occurred.",
                      MB_ICONEXCLAMATION);
        return 1;   // handled — keep the app running (Windows behaviour)
    }
}

static int hook_query_quit_save()
{
    // Persist the main window geometry on every quit — [NSApp terminate:] does
    // not reliably fire the MDI parent's close handler, so the size / position
    // would otherwise be lost on Cmd-Q / app-menu / Dock Quit.
    {
        wxWindow* mw = App.m_pMainWnd ? static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd)
                                      : nullptr;
        SaveMainWindowGeometry(wxDynamicCast(mw, wxTopLevelWindow));
    }

    // Notepad/text documents, and then worlds.  MFC's CWinApp::SaveAllModified
    // walks EVERY registered doc template in registration order, and
    // InitInstance registers m_pNormalDocTemplate first (MUSHclient.cpp) — so on
    // Windows the notepads are asked about before the worlds.  This pass used to
    // exist for worlds only, and bailed out entirely when there was no world
    // template, so an unsaved notepad was silently discarded by every quit.
    std::vector<CDocument*> notepads;
    if (App.m_pNormalDocTemplate)
    {
        POSITION npos = App.m_pNormalDocTemplate->GetFirstDocPosition();
        while (npos)
            if (CDocument* d = App.m_pNormalDocTemplate->GetNextDoc(npos))
                notepads.push_back(d);
    }

    std::vector<CMUSHclientDoc*> worlds;
    if (App.m_pWorldDocTemplate)
    {
        POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition();
        while (pos)
            if (CDocument* d = App.m_pWorldDocTemplate->GetNextDoc(pos))
                worlds.push_back(static_cast<CMUSHclientDoc*>(d));
    }

    // Guard the whole flow: a save raising (e.g. a write error, an MFC
    // CException) must not propagate through applicationShouldTerminate: — that
    // is an Objective-C++ boundary and a C++ exception crossing it crashes.
    try
    {
        // Phase 0 — the notepads.  SaveModified() is virtual and is exactly what
        // CDocTemplate::SaveAllModified calls; CTextDocument's override applies
        // the per-notepad "save on change" setting and the notepad types that
        // close without asking, then prompts only if the document is dirty.
        for (CDocument* d : notepads)
            if (!d->SaveModified())
                return 0;   // user cancelled → do not quit

        // Phase 1 — prompt for any world with unsaved edits.  SaveModified() is
        // the canonical Save / Discard / Cancel prompt: it auto-saves or prompts
        // per the world's settings, handles the variables-only-changed case, and
        // returns FALSE only if the user cancelled.  Run the whole prompt pass
        // before persisting layout so a late Cancel aborts cleanly.
        for (CMUSHclientDoc* d : worlds)
            if (!d->PortPromptSaveModified())
                return 0;   // user cancelled → do not quit

        // Phase 2 — fire each world's plugin OnPluginClose, then SaveState, just
        // as ~CPlugin does on a normal world close (which [NSApp terminate:]
        // skips).  Firing OnPluginClose is what persists window LAYOUT: Quow's
        // Cow Bar writes its miniwindow positions to its own SQLite database from
        // OnPluginClose, NOT from OnPluginSaveState (the plugin even comments
        // "Do not do database stuff here, as OnPluginClose is called FIRST!").
        // SaveState then writes the standard <plugin>-state.xml variables.
        for (CMUSHclientDoc* d : worlds)
            d->SendToAllPluginCallbacks(ON_PLUGIN_CLOSE);
        for (CMUSHclientDoc* d : worlds)
            for (auto pit = d->m_PluginList.begin(); pit != d->m_PluginList.end(); ++pit)
                if (*pit) (*pit)->SaveState();
    }
    catch (...)
    {
        fprintf(stderr, "[quit] save/close flow raised; proceeding with quit\n");
    }

    return 1;
}

// ── hook_create_text_window ───────────────────────────────────────────────
// Allocates a CTextDocument, fills its fields from the parameters, creates a
// CWxNotepadFrame that wires all m_fn_* callbacks and registers the doc with
// App.m_pNormalDocTemplate, then returns the doc pointer (the frame owns it).
static void* hook_create_text_window(
    const char* sText,
    const char* sTitle,
    CMUSHclientDoc* pDoc,
    long long iUniqueDocNum,
    const char* strFontName,
    long  iFontSize,
    long  iFontWeight,
    unsigned int iFontCharset,
    unsigned long textColour,
    unsigned long backColour,
    const char* strSearchString,
    const char* strRecallLinePreamble,
    bool bMatchCase,
    bool bRegexp,
    bool bCommands,
    bool bOutput,
    bool bNotes,
    int  iLines,
    int  iNotepadType)
{
    if (!App.m_pNormalDocTemplate)
        return nullptr;

    // Allocate the document object via the static factory (constructor is
    // protected in MFC document classes).
    CTextDocument* td = CTextDocument::CreateInstance();
    td->m_pRelatedWorld         = pDoc;
    td->m_iUniqueDocumentNumber = iUniqueDocNum;
    // Take the caller's font verbatim, exactly as Utilities.cpp does.  An
    // empty name is meaningful: MakeFont resolves it to the global fixed-pitch
    // font, matching CTextView::SetTheFont.  Do NOT substitute
    // App.m_strNotepadFont here — that global is read by nothing on Windows and
    // its preference controls are hidden, so consuming it made notepads ignore
    // the fixed-pitch font preference they follow there.
    td->m_strFontName           = strFontName ? strFontName : "";
    td->m_iFontSize             = iFontSize;
    td->m_iFontWeight           = iFontWeight;
    td->m_iFontCharset          = iFontCharset;
    td->m_textColour            = static_cast<COLORREF>(textColour);
    td->m_backColour            = static_cast<COLORREF>(backColour);
    td->m_strSearchString       = strSearchString       ? strSearchString       : "";
    td->m_strRecallLinePreamble = strRecallLinePreamble ? strRecallLinePreamble : "";
    td->m_bMatchCase            = bMatchCase;
    td->m_bRegexp               = bRegexp;
    td->m_bCommands             = bCommands;
    td->m_bOutput               = bOutput;
    td->m_bNotes                = bNotes;
    td->m_iLines                = iLines;
    td->m_iNotepadType          = iNotepadType;

    // Apply notepad-type colour overrides (matches Utilities.cpp logic).
    if (iNotepadType == eNotepadWorldLoadError) {
        td->m_textColour = RGB(128,   0,   0);   // maroon
        td->m_backColour = RGB(255, 228, 181);   // moccasin
    } else if (iNotepadType == eNotepadXMLcomments) {
        td->m_textColour = RGB(  0, 100,   0);   // darkgreen
        td->m_backColour = RGB(176, 224, 230);   // powderblue
    } else if (App.m_cNotepadTextColour != App.m_cNotepadBackColour) {
        td->m_textColour = App.m_cNotepadTextColour;
        td->m_backColour = App.m_cNotepadBackColour;
    }

    // Never leave a notepad with text == background — the default notepad
    // colours are 0/0 (black on black), which the canonical treats as "use the
    // system default" but which renders as unreadable here.  Fall back to black
    // text on a white background so e.g. the packet-debug notepad is legible.
    if (td->m_textColour == td->m_backColour) {
        td->m_textColour = RGB(0, 0, 0);
        td->m_backColour = RGB(255, 255, 255);
    }

    // Sanitise title for display (matches Utilities.cpp).
    CString strTitle = sTitle ? sTitle : "";
    td->m_strTitle = strTitle;
    strTitle.Replace(" ",  "");
    strTitle.Replace("#",  "");
    strTitle.Replace("%",  "");
    strTitle.Replace(";",  "");
    strTitle.Replace("/",  "-");
    strTitle.Replace("\\", "-");
    strTitle.Replace(":",  "-");
    td->SetTitle(strTitle);

    wxString wxTitle = wxString::FromUTF8(
        static_cast<const char*>(td->m_strTitle));
    // Lenient: the initial text is world output, so it can carry bytes that are
    // not valid UTF-8; FromUTF8 would have opened the notepad empty.
    wxString wxText  = sText ? DecodeUTF8Lenient(sText, strlen(sText)) : wxString();

    // CWxNotepadFrame constructor registers td with the template and shows.
    // The frame takes ownership of td.
    new CWxNotepadFrame(td, wxTitle, wxText);

    // Canonical CreateTextWindow (Utilities.cpp) marks the document modified
    // exactly when it has content to install — "nothing to put there" returns
    // before the SetModifiedFlag(TRUE) — so a notepad handed text by a script
    // or a trigger prompts on close, and an empty one does not.  Now that
    // SaveModified honours the flag, the port has to set it the same way.
    if (sText && *sText)
        td->SetModifiedFlag(TRUE);

    return static_cast<void*>(td);
}

// ── open_text_file ────────────────────────────────────────────────────────
// Open an existing file into a notepad window, as MFC's document/template
// machinery does for a non-world file.  Backs CWinApp::OpenDocumentFile for
// anything that is not a *.mcl world — the "edit script with notepad" paths
// (world_debug.cpp) and the Plugins dialog's Edit button, all of which cast the
// result to CTextDocument*.  Before this they got nullptr and reported
// "Unable to edit the plugin file ...".
//
// Unlike create_text_window above, the notepad this produces HAS A FILE behind
// it: SetPathName makes File > Save write back to it rather than prompting.
static void* hook_open_text_file(const char* path)
{
    if (!path || !*path)
        return nullptr;

    wxString wxPath = wxString::FromUTF8(path);
    if (!wxFileName::FileExists(wxPath))
        return nullptr;

    // Read the whole file.  wxFile handles the binary read; the notepad is a
    // text control, so anything unreadable as text simply displays oddly rather
    // than failing — the same as the Windows notepad.
    wxFile f;
    if (!f.Open(wxPath, wxFile::read))
        return nullptr;
    wxFileOffset len = f.Length();
    std::string  buf;
    if (len > 0) {
        buf.resize((size_t) len);
        ssize_t got = f.Read(&buf[0], (size_t) len);
        if (got < 0)
            return nullptr;
        buf.resize((size_t) got);
    }
    f.Close();

    static long long s_openedFileDocNum = 0;

    // Title is the leaf name, matching what MFC's doc template would title it.
    wxString leaf = wxFileName(wxPath).GetFullName();

    void* pv = hook_create_text_window(
        buf.c_str(),
        (const char*) leaf.ToUTF8(),
        nullptr,                       // not tied to a world
        --s_openedFileDocNum,          // negative: distinct from world-owned notepads
        nullptr, 0, 0, 0,              // font: take the global notepad defaults
        0, 0,                          // colours: ditto (0/0 -> defaults)
        "", "",                        // no search string / recall preamble
        false, false, false, false, false,
        0,
        eNotepadNormal);
    if (!pv)
        return nullptr;

    CTextDocument* td = static_cast<CTextDocument*>(pv);
    // The point of opening a FILE: saving writes back here instead of prompting.
    td->SetPathName(path);
    td->SetModifiedFlag(FALSE);
    return pv;
}

// ── Install all hooks ─────────────────────────────────────────────────────
// ── Sound hooks ───────────────────────────────────────────────────────────
// CMainFrame::PlaySoundFile and CancelSound delegate here on non-Windows so
// that trigger sounds and MSP audio actually play via wxWidgets.
//
// Audio runs through the miniaudio engine (platform/ma_sound.*): one mixer with
// up to MAX_SOUND_BUFFERS concurrent voices, each with its own volume, pan and
// loop flag — the same capabilities DirectSound gave the Windows build.  Plays
// .wav/.mp3/.flac.  (This replaced single-channel wxSound, which couldn't mix,
// pan or set volume.)  Plugins reference sounds with Windows-style backslash
// paths, so every path is normalised to POSIX via PortableFilePath() here.

// MSP / trigger sounds + the world-properties "test" button: auto-buffer, full
// volume, centred — the simple fire-and-forget path.
static bool hook_play_sound(const char* path, bool loop)
{
    if (!path || !*path) return false;
    std::string fixed = PortableFilePath(path);
    return ma_audio_play(0 /*auto buffer*/, fixed.c_str(), loop ? 1 : 0,
                         0.0 /*full volume*/, 0.0 /*centre*/) != 0;
}

// CancelSound / StopSound(0): stop every voice.
static void hook_stop_sound()
{
    ma_audio_stop(0);
}

// Buffered world.PlaySound: explicit buffer (0 = auto), volume %, pan %, loop.
static int hook_sound_play(int buffer, const char* path, int loop,
                           double volume, double pan)
{
    if (!path || !*path) return 0;
    std::string fixed = PortableFilePath(path);
    return ma_audio_play(buffer, fixed.c_str(), loop, volume, pan);
}

static int hook_sound_play_memory(int buffer, const void* data, size_t len,
                                  int loop, double volume, double pan)
{ return ma_audio_play_memory(buffer, data, len, loop, volume, pan); }

static int  hook_sound_adjust(int buffer, int loop, double volume, double pan)
{ return ma_audio_adjust(buffer, loop, volume, pan); }

static void hook_sound_stop_buffer(int buffer) { ma_audio_stop(buffer); }

static int  hook_sound_status(int buffer) { return ma_audio_status(buffer); }


// ── Bundled "TEXT" resources (licence + credits) ──────────────────────────
// The About box's CCreditsDlg loads its text via FindResource(.., "TEXT") +
// LoadResource/LockResource.  On the port those map here (mfc_ui.h) and we serve
// the text from Contents/Resources/text/, bundled from mushclient_resources.
static const char* hook_load_text_resource(int id, unsigned* size)
{
    const char* file = (id == 177) ? "credits.txt"      // IDR_CREDITS
                     : (id == 252) ? "agreement.txt"    // IDR_LICENSE
                     : nullptr;
    if (!file) { if (size) *size = 0; return nullptr; }

    static std::map<int, std::string> cache;
    auto it = cache.find(id);
    if (it == cache.end())
    {
        wxString name = wxString(wxT("text")) + wxFILE_SEP_PATH + wxString::FromUTF8(file);
        wxString cands[] = {
            wxStandardPaths::Get().GetResourcesDir() + wxFILE_SEP_PATH + name,
            wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath()
                + wxFILE_SEP_PATH + name,
        };
        std::string data;
        for (const wxString& p : cands)
        {
            std::ifstream f(p.fn_str(), std::ios::binary);
            if (f) { std::stringstream ss; ss << f.rdbuf(); data = ss.str(); break; }
        }
        it = cache.emplace(id, std::move(data)).first;
    }
    if (size) *size = (unsigned) it->second.size();
    return it->second.empty() ? nullptr : it->second.c_str();
}


// ── Fonts ─────────────────────────────────────────────────────────────────
// utils.getfontfamilies(): the installed font family names.
static char** hook_get_font_families()
{
    wxArrayString faces = wxFontEnumerator::GetFacenames();
    char** arr = (char**) malloc((faces.GetCount() + 1) * sizeof(char*));
    if (!arr) return nullptr;
    size_t n = 0;
    for (size_t i = 0; i < faces.GetCount(); ++i)
        arr[n++] = strdup(faces[i].utf8_str());
    arr[n] = nullptr;
    return arr;
}

// utils.glyph_available(): CoreText on macOS (wx_osx_text.mm); 0 elsewhere.
static int hook_font_has_glyph(const char* facename, unsigned int codepoint)
{
    return wxOSXFontHasGlyph(facename, codepoint);
}

// ::GetPixel(HDC,x,y) reader — implemented in wx_gdi_compat.cpp (it needs the
// DIBSection internals defined there); assigned to g_wxhook_get_pixel below.
extern unsigned long wxgdi_get_pixel(void* hdc, int x, int y);

// world.GetSysColor(COLOR_*) — map the Win32 system-colour index to the live
// GetKeyState(VK_*): live modifier/mouse-button state for world.GetInfo(294).
// wxGetKeyState reports the generic modifiers; wxGetMouseState the buttons.  wx
// can't portably distinguish left/right modifiers or read lock toggles, so those
// VK_* report "not pressed" (0) rather than a wrong value.  High bit = down.
static short hook_get_key_state(int vk)
{
    bool down = false;
    switch (vk)
    {
    case VK_SHIFT:    down = wxGetKeyState(WXK_SHIFT);            break;
    case VK_CONTROL:  down = wxGetKeyState(WXK_CONTROL);          break;
    case VK_MENU:     down = wxGetKeyState(WXK_ALT);              break;
    case VK_LBUTTON:  down = wxGetMouseState().LeftIsDown();      break;
    case VK_RBUTTON:  down = wxGetMouseState().RightIsDown();     break;
    case VK_MBUTTON:  down = wxGetMouseState().MiddleIsDown();    break;
    default:          return 0;   // L/R-specific modifiers + caps/num/scroll lock
    }
    return down ? (short) 0x8000 : 0;
}

// desktop theme colour.  The COLOR_* literals are the stable Win32 constants
// (only a handful are #defined in the compat headers, so spell them out).
// Returns COLORREF (0x00BBGGRR); unknown index → MFC light-grey default.
static unsigned long hook_get_sys_color(int idx)
{
    wxSystemColour which;
    switch (idx)
    {
    case 0:  which = wxSYS_COLOUR_SCROLLBAR;            break; // COLOR_SCROLLBAR
    case 1:  which = wxSYS_COLOUR_BACKGROUND;           break; // COLOR_BACKGROUND/DESKTOP
    case 2:  which = wxSYS_COLOUR_ACTIVECAPTION;        break; // COLOR_ACTIVECAPTION
    case 3:  which = wxSYS_COLOUR_INACTIVECAPTION;      break; // COLOR_INACTIVECAPTION
    case 4:  which = wxSYS_COLOUR_MENU;                 break; // COLOR_MENU
    case 5:  which = wxSYS_COLOUR_WINDOW;               break; // COLOR_WINDOW
    case 6:  which = wxSYS_COLOUR_WINDOWFRAME;          break; // COLOR_WINDOWFRAME
    case 7:  which = wxSYS_COLOUR_MENUTEXT;             break; // COLOR_MENUTEXT
    case 8:  which = wxSYS_COLOUR_WINDOWTEXT;           break; // COLOR_WINDOWTEXT
    case 9:  which = wxSYS_COLOUR_CAPTIONTEXT;          break; // COLOR_CAPTIONTEXT
    case 10: which = wxSYS_COLOUR_ACTIVEBORDER;         break; // COLOR_ACTIVEBORDER
    case 11: which = wxSYS_COLOUR_INACTIVEBORDER;       break; // COLOR_INACTIVEBORDER
    case 12: which = wxSYS_COLOUR_APPWORKSPACE;         break; // COLOR_APPWORKSPACE
    case 13: which = wxSYS_COLOUR_HIGHLIGHT;            break; // COLOR_HIGHLIGHT
    case 14: which = wxSYS_COLOUR_HIGHLIGHTTEXT;        break; // COLOR_HIGHLIGHTTEXT
    case 15: which = wxSYS_COLOUR_BTNFACE;              break; // COLOR_BTNFACE/3DFACE
    case 16: which = wxSYS_COLOUR_BTNSHADOW;            break; // COLOR_BTNSHADOW
    case 17: which = wxSYS_COLOUR_GRAYTEXT;             break; // COLOR_GRAYTEXT
    case 18: which = wxSYS_COLOUR_BTNTEXT;              break; // COLOR_BTNTEXT
    case 19: which = wxSYS_COLOUR_INACTIVECAPTIONTEXT;  break; // COLOR_INACTIVECAPTIONTEXT
    case 20: which = wxSYS_COLOUR_BTNHIGHLIGHT;         break; // COLOR_BTNHIGHLIGHT
    default: return 0xC0C0C0;
    }
    wxColour c = wxSystemSettings::GetColour(which);
    if (!c.IsOk()) return 0xC0C0C0;
    return (unsigned long) c.Red()
         | ((unsigned long) c.Green() << 8)
         | ((unsigned long) c.Blue()  << 16);
}

// world.GetSystemMetrics(SM_*) — map the indices scripts actually use to the
// live wx metric.  SM_* literals are stable Win32 constants; unmapped indices
// return 0 (matching the Win32 contract for unsupported metrics).
static int hook_get_system_metric(int idx)
{
    switch (idx)
    {
    case 0:  return wxSystemSettings::GetMetric(wxSYS_SCREEN_X);          // SM_CXSCREEN
    case 1:  return wxSystemSettings::GetMetric(wxSYS_SCREEN_Y);          // SM_CYSCREEN
    case 2:  return wxSystemSettings::GetMetric(wxSYS_VSCROLL_X);         // SM_CXVSCROLL
    case 3:  return wxSystemSettings::GetMetric(wxSYS_HSCROLL_Y);         // SM_CYHSCROLL
    case 4:  return wxSystemSettings::GetMetric(wxSYS_CAPTION_Y);         // SM_CYCAPTION
    case 5:  return wxSystemSettings::GetMetric(wxSYS_BORDER_X);          // SM_CXBORDER
    case 6:  return wxSystemSettings::GetMetric(wxSYS_BORDER_Y);          // SM_CYBORDER
    case 11: return wxSystemSettings::GetMetric(wxSYS_ICON_X);            // SM_CXICON
    case 12: return wxSystemSettings::GetMetric(wxSYS_ICON_Y);            // SM_CYICON
    case 13: return wxSystemSettings::GetMetric(wxSYS_CURSOR_X);          // SM_CXCURSOR
    case 14: return wxSystemSettings::GetMetric(wxSYS_CURSOR_Y);          // SM_CYCURSOR
    case 15: return wxSystemSettings::GetMetric(wxSYS_MENU_Y);            // SM_CYMENU
    case 16: return wxSystemSettings::GetMetric(wxSYS_SCREEN_X);          // SM_CXFULLSCREEN (≈)
    case 17: return wxSystemSettings::GetMetric(wxSYS_SCREEN_Y);          // SM_CYFULLSCREEN (≈)
    case 20: return wxSystemSettings::GetMetric(wxSYS_VSCROLL_ARROW_Y);   // SM_CYVSCROLL
    case 21: return wxSystemSettings::GetMetric(wxSYS_HSCROLL_ARROW_X);   // SM_CXHSCROLL
    case 43: return wxSystemSettings::GetMetric(wxSYS_MOUSE_BUTTONS);     // SM_CMOUSEBUTTONS
    case 49: return wxSystemSettings::GetMetric(wxSYS_SMALLICON_X);       // SM_CXSMICON
    case 50: return wxSystemSettings::GetMetric(wxSYS_SMALLICON_Y);       // SM_CYSMICON
    default: return 0;
    }
}

// world.SetCursor(n) — set the mouse cursor over the doc's output pane.  Maps
// the MUSHclient cursor index to the nearest wx stock cursor.
static void hook_set_cursor(void* docv, int cursorNum)
{
    CMUSHclientDoc* doc = static_cast<CMUSHclientDoc*>(docv);
    auto it = s_docFrameMap.find(doc);
    if (it == s_docFrameMap.end() || !it->second) return;
    COutputPane* pane = it->second->OutputPane();
    if (!pane) return;

    wxStockCursor sc;
    switch (cursorNum)
    {
    case -1: sc = wxCURSOR_BLANK;          break; // no cursor
    case  0: sc = wxCURSOR_ARROW;          break; // arrow
    case  1: sc = wxCURSOR_HAND;           break; // hand
    case  2: sc = wxCURSOR_IBEAM;          break; // I-beam
    case  3: sc = wxCURSOR_CROSS;          break; // cross
    case  4: sc = wxCURSOR_WAIT;           break; // hour-glass
    case  5: sc = wxCURSOR_ARROW;          break; // up arrow (no wx stock equivalent)
    case  6: sc = wxCURSOR_SIZENWSE;       break; // arrow nw-se
    case  7: sc = wxCURSOR_SIZENESW;       break; // arrow ne-sw
    case  8: sc = wxCURSOR_SIZEWE;         break; // arrow e-w
    case  9: sc = wxCURSOR_SIZENS;         break; // arrow n-s
    case 10: sc = wxCURSOR_SIZING;         break; // arrow all ways
    case 11: sc = wxCURSOR_NO_ENTRY;       break; // (X) no
    case 12: sc = wxCURSOR_QUESTION_ARROW; break; // help
    default: sc = wxCURSOR_ARROW;          break;
    }
    pane->SetCursor(wxCursor(sc));
}

// utils.setbackgroundcolour / world.SetFrameBackgroundColour — set the MDI
// parent frame's backdrop colour (the area behind world child windows).
static void hook_set_main_background(unsigned long colour)
{
    wxWindow* top = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    if (!top) return;
    wxColour c;   // default-constructed = invalid → resets to system default
    if (colour != 0xFFFFFFFF)   // 0xFFFFFFFF is MUSHclient's "use default" value
        c = wxColour((unsigned char)( colour        & 0xFF),   // COLORREF 0x00BBGGRR
                     (unsigned char)((colour >> 8)  & 0xFF),
                     (unsigned char)((colour >> 16) & 0xFF));
    top->SetBackgroundColour(c);
    if (wxMDIParentFrame* mdi = wxDynamicCast(top, wxMDIParentFrame))
        if (wxWindow* client = mdi->GetClientWindow()) {
            client->SetBackgroundColour(c);
            client->Refresh();
        }
    top->Refresh();
}


// ── Notepad hooks: REMOVED ────────────────────────────────────────────────
//
// Eleven g_wxhook_*_notepad hooks and their bodies lived here.  They were the
// GUI half of a twin: the only caller of nine of them was the headless stub
// file, and the only installer was this one, and the two adapter sets never
// co-link — so every body here was unreachable in the shipped app while every
// call site was permanently nullptr.  ~135 lines that could not run.
//
// The notepad family is adopted into mushcore now
// (docs/STUB-ADOPTION-2026-08.md), so both sides go through the real
// scripting/methods/methods_notepad.cpp and notepad_app_shared.cpp, which reach
// the window through CTextDocument's m_fn_* callbacks CWxNotepadFrame already
// wires.  g_wxhook_create_text_window and g_wxhook_open_text_file stay: those
// are genuinely live seams used by real code in both builds.

// ── Colour picker ─────────────────────────────────────────────────────────

static long hook_pick_colour(long suggested)
{
    wxColourData data;
    if (suggested != -1)
    {
        // COLORREF is 0x00BBGGRR — extract R, G, B.
        unsigned char r = (suggested >>  0) & 0xFF;
        unsigned char g = (suggested >>  8) & 0xFF;
        unsigned char b = (suggested >> 16) & 0xFF;
        data.SetColour(wxColour(r, g, b));
    }
    wxColourDialog dlg(wxGetActiveWindow(), &data);
    if (dlg.ShowModal() != wxID_OK)
        return -1;
    wxColour col = dlg.GetColourData().GetColour();
    // Pack back into COLORREF (0x00BBGGRR).
    return (long)(((unsigned long)col.Blue()  << 16) |
                  ((unsigned long)col.Green() <<  8) |
                   (unsigned long)col.Red());
}

// ── Menu / choice dialog ──────────────────────────────────────────────────

static char* hook_menu_dialog(const char* const* items, int count,
                               const char* defaultItem)
{
    if (count <= 0)
        return nullptr;

    wxArrayString choices;
    int selection = 0;
    for (int i = 0; i < count; ++i)
    {
        choices.Add(wxString::FromUTF8(items[i]));
        if (defaultItem && defaultItem[0] && strcmp(items[i], defaultItem) == 0)
            selection = i;
    }

    wxSingleChoiceDialog dlg(wxGetActiveWindow(),
                             wxT("Select an item:"),
                             wxT("Menu"),
                             choices);
    dlg.SetSelection(selection);
    if (dlg.ShowModal() != wxID_OK)
        return nullptr;

    wxString chosen = dlg.GetStringSelection();
    std::string utf8 = chosen.ToUTF8().data();
    char* result = static_cast<char*>(malloc(utf8.size() + 1));
    if (result)
        memcpy(result, utf8.c_str(), utf8.size() + 1);
    return result;
}

// ── Progress dialog (CProgressDlg) ────────────────────────────────────────
// See the hook declarations.  wxProgressDialog is used rather than a bridged
// modeless dialog because it brings its own pump: Update() dispatches events,
// which is what makes the window paint and the Cancel button clickable while
// the caller's send loop is still running on this thread.
//
// wxPD_APP_MODAL matches what the canonical dialog does on Windows -- it
// disables its parent for the duration (CProgressDlg::Create) -- and, more
// importantly here, stops the user starting a second send from the menu while
// one is in flight, which would re-enter the engine underneath us.

struct PortProgress {
    wxProgressDialog* dlg = nullptr;
    int               max = 100;
    bool              cancelled = false;
};

static void* hook_progress_open(const char* caption, const char* status, int maximum)
{
    if (maximum <= 0) maximum = 100;   // wx rejects a zero-length range
    PortProgress* p = new PortProgress;
    p->max = maximum;
    p->dlg = new wxProgressDialog(
        caption ? wxString::FromUTF8(caption) : wxString(wxT("Progress")),
        status  ? wxString::FromUTF8(status)  : wxString(),
        maximum,
        wxTheApp ? wxTheApp->GetTopWindow() : nullptr,
        wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);
    return p;
}

static void hook_progress_set_range(void* handle, int maximum)
{
    PortProgress* p = static_cast<PortProgress*>(handle);
    if (!p || !p->dlg) return;
    if (maximum <= 0) maximum = 100;
    // wxProgressDialog has no SetRange, so re-make it at the right size.  The
    // callers all do Create() then SetRange() before any real work, so this
    // happens once, before anything is on screen worth preserving.
    const wxString title = p->dlg->GetTitle();
    const wxWindow* parent = p->dlg->GetParent();
    p->dlg->Destroy();
    p->max = maximum;
    p->dlg = new wxProgressDialog(
        title, wxString(), maximum,
        const_cast<wxWindow*>(parent),
        wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);
}

static bool hook_progress_update(void* handle, int pos, const char* status)
{
    PortProgress* p = static_cast<PortProgress*>(handle);
    if (!p || !p->dlg) return true;
    if (p->cancelled) return false;          // stay cancelled once cancelled
    if (pos < 0)      pos = 0;
    if (pos > p->max) pos = p->max;          // wx asserts on an out-of-range pos
    const bool keepGoing = p->dlg->Update(
        pos, status ? wxString::FromUTF8(status) : wxString());
    if (!keepGoing) p->cancelled = true;
    return keepGoing;
}

static void hook_progress_set_title(void* handle, const char* title)
{
    PortProgress* p = static_cast<PortProgress*>(handle);
    if (!p || !p->dlg || !title) return;
    p->dlg->SetTitle(wxString::FromUTF8(title));
}

static void hook_progress_close(void* handle)
{
    PortProgress* p = static_cast<PortProgress*>(handle);
    if (!p) return;
    if (p->dlg) p->dlg->Destroy();
    delete p;
}

// ── Flash icon / request user attention ───────────────────────────────────
// Requests user attention on the main application frame.  On macOS this
// bounces the dock icon; on Linux it flashes the taskbar button.

static void hook_flash_icon()
{
    wxWindow* topWin = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    wxTopLevelWindow* top = wxDynamicCast(topWin, wxTopLevelWindow);
    if (top) top->RequestUserAttention(wxUSER_ATTENTION_INFO);
}

// ── Move main window ──────────────────────────────────────────────────────
// Repositions and/or resizes the MDI parent frame.

static void hook_move_main_window(long left, long top, long width, long height)
{
    wxWindow* win = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    if (win) win->SetSize((int)left, (int)top, (int)width, (int)height);
}


// ── utils.inputbox / utils.editbox ────────────────────────────────────────
// Shows a simple text entry dialog.  Returns heap-allocated string (caller
// frees) or nullptr on cancel.

static char* hook_inputbox(const char* title,
                            const char* prompt,
                            const char* defValue,
                            bool        multiline)
{
    wxWindow* parent = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    wxString result;
    if (multiline) {
        wxDialog dlg(parent, wxID_ANY, wxString::FromUTF8(title ? title : "Input"),
                     wxDefaultPosition, wxSize(400, 300),
                     wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        wxBoxSizer* sz = new wxBoxSizer(wxVERTICAL);
        if (prompt && *prompt)
            sz->Add(new wxStaticText(&dlg, wxID_ANY, wxString::FromUTF8(prompt)),
                    0, wxALL, 8);
        wxTextCtrl* txt = new wxTextCtrl(&dlg, wxID_ANY,
            wxString::FromUTF8(defValue ? defValue : ""),
            wxDefaultPosition, wxDefaultSize,
            wxTE_MULTILINE | wxTE_DONTWRAP);
        wxOSXDisableSmartSubstitutions(txt);
        sz->Add(txt, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
        wxStdDialogButtonSizer* btns = new wxStdDialogButtonSizer();
        btns->AddButton(new wxButton(&dlg, wxID_OK));
        btns->AddButton(new wxButton(&dlg, wxID_CANCEL));
        btns->Realize();
        sz->Add(btns, 0, wxALL, 8);
        dlg.SetSizer(sz);
        dlg.Centre();
        if (dlg.ShowModal() != wxID_OK) return nullptr;
        result = txt->GetValue();
    } else {
        wxTextEntryDialog dlg(parent,
            wxString::FromUTF8(prompt ? prompt : ""),
            wxString::FromUTF8(title ? title : "Input"),
            wxString::FromUTF8(defValue ? defValue : ""));
        if (dlg.ShowModal() != wxID_OK) return nullptr;
        result = dlg.GetValue();
    }
    return strdup(result.ToUTF8().data());
}

// ── utils.directorypicker ─────────────────────────────────────────────────

static char* hook_directorypicker(const char* title, const char* defPath)
{
    wxWindow* parent = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    wxDirDialog dlg(parent,
        wxString::FromUTF8(title ? title : "Select directory"),
        wxString::FromUTF8(defPath ? defPath : ""),
        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return nullptr;
    return strdup(dlg.GetPath().ToUTF8().data());
}

// ── WindowMenu popup ────────────────────────────────────────────────────────
// CMiniWindow::Menu() builds the plugin's context menu into the portable CMenu
// tree (platform/mfc_ui.h); render it as a wxWidgets popup and return the
// selected command id (TPM_RETURNCMD semantics), or 0 if cancelled.
//
// The translation is externally callable (platform/wx_mfc_logic.h) so a test
// can build a CMenu, render it and read the result back without ever showing
// it — the popup itself needs an event loop, which no in-app selftest has.
wxMenu* wx_build_popup_menu(CMenu* m)
{
    wxMenu* wxm = new wxMenu();
    for (const CMenu::Item& it : m->m_items)
    {
        if (it.flags & MF_SEPARATOR)
            wxm->AppendSeparator();
        else if (it.flags & MF_POPUP)
        {
            CMenu* child = CMenu::FromMenuHandle((HMENU)(intptr_t) it.arg);
            if (it.text.empty())
            {
                // An UNLABELLED popup: created on demand by CMenu::GetSubMenu
                // because the port has no menu-resource loader, so the title
                // the .rc would have supplied does not exist.  Rather than show
                // a nameless submenu the user cannot read, inline its items,
                // separated from the previous group.  The canonical code fills
                // these groups itself (see dialogs/EditDlg.cpp's regexp menu).
                if (child && !child->m_items.empty())
                {
                    if (wxm->GetMenuItemCount() > 0)
                        wxm->AppendSeparator();
                    wxMenu* inlined = wx_build_popup_menu(child);
                    while (inlined->GetMenuItemCount() > 0)
                        wxm->Append(inlined->Remove(inlined->FindItemByPosition(0)));
                    delete inlined;
                }
            }
            else
            {
                wxMenu* wxchild = child ? wx_build_popup_menu(child) : new wxMenu();
                wxm->AppendSubMenu(wxchild, wxString::FromUTF8(it.text.c_str()));
            }
        }
        else
        {
            // Regular command item.  Grayed placeholder items arrive with id 0.
            int id = (it.arg != 0) ? (int) it.arg : wxID_ANY;
            wxMenuItem* mi = (it.flags & MF_CHECKED)
                ? wxm->AppendCheckItem(id, wxString::FromUTF8(it.text.c_str()))
                : wxm->Append         (id, wxString::FromUTF8(it.text.c_str()));
            if (mi)
            {
                if (it.flags & MF_CHECKED) mi->Check(true);
                if (it.flags & MF_GRAYED)  mi->Enable(false);
            }
        }
    }
    return wxm;
}

// Approximate the popup's size, so the TPM_*ALIGN bits can be honoured: wx
// offers no way to measure a wxMenu before it is shown.  Consulted ONLY for
// alignments other than the default left/top, so an imperfect estimate can
// affect nothing but the menus that explicitly asked to hang off some other
// corner (world.WindowMenu's "~" prefix).
static wxSize wx_estimate_popup_size(wxWindow* ref, CMenu* menu)
{
    int widest = 0, height = 8;
    for (const CMenu::Item& it : menu->m_items) {
        if (it.flags & MF_SEPARATOR) { height += 8; continue; }
        const wxSize ext = ref->GetTextExtent(wxString::FromUTF8(it.text.c_str()));
        widest  = std::max(widest, ext.x);
        height += ext.y + 6;
    }
    // Leave room for the check-mark gutter on the left and the submenu arrow /
    // right margin on the right.
    return wxSize(widest + 44, height);
}

static int hook_popup_menu(CMenu* menu, int x, int y, unsigned flags)
{
    if (!menu) return 0;
    wxWindow* win = wxGetActiveWindow();
    if (!win) win = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    if (!win) return 0;

    wxMenu* wxm = wx_build_popup_menu(menu);

    // (x, y) are SCREEN coordinates, as Win32 TrackPopupMenu takes them — every
    // caller converts before calling (CMiniWindow::Menu through the world's
    // output pane, the dialogs through CWnd::ClientToScreen).  This used to
    // ignore them entirely and pop the menu up at the mouse cursor, which is
    // only right when the menu was raised BY a click: a plugin that raises one
    // from an alias, a trigger or a timer got its menu wherever the pointer
    // happened to be, possibly over another window altogether, and the "drop it
    // under my toolbar button" idiom never lined up.
    wxPoint anchor(x, y);
    if (anchor == wxPoint(0, 0))
        anchor = ::wxGetMousePosition();   // no usable anchor — the old behaviour
    if (flags & (TPM_CENTERALIGN | TPM_RIGHTALIGN |
                 TPM_VCENTERALIGN | TPM_BOTTOMALIGN)) {
        // The alignment bits name which edge of the MENU sits on the anchor;
        // wx always puts its top-left there, so move the anchor instead.
        const wxSize sz = wx_estimate_popup_size(win, menu);
        int ax = anchor.x, ay = anchor.y;
        TpmAlignAnchor(flags, sz.x, sz.y, &ax, &ay);
        anchor = wxPoint(ax, ay);
    }

    // GetPopupMenuSelectionFromUser takes coordinates in `win`'s client space
    // and returns the chosen id without firing a menu event — exactly the
    // TPM_RETURNCMD behaviour CMiniWindow::Menu expects.
    wxPoint pt = win->ScreenToClient(anchor);
    int sel = win->GetPopupMenuSelectionFromUser(*wxm, pt);
    delete wxm;
    return (sel > 0) ? sel : 0;   // wxID_NONE / negative ⇒ cancelled
}

// Convert a point in a world's output-pane client coordinates to screen
// coordinates — the port's stand-in for the ClientToScreen(pView->m_hWnd, …)
// that CMiniWindow::Menu does on Windows.
static void hook_output_client_to_screen(void* docp, int* x, int* y)
{
    CMUSHclientDoc* doc = static_cast<CMUSHclientDoc*>(docp);
    if (!doc || !x || !y) return;
    auto it = s_docFrameMap.find(doc);
    if (it == s_docFrameMap.end() || !it->second) return;
    COutputPane* pane = it->second->OutputPane();
    if (!pane) return;
    const wxPoint p = pane->ClientToScreen(wxPoint(*x, *y));
    *x = p.x;
    *y = p.y;
}

// ── password_dialog ───────────────────────────────────────────────────────
// Called from CPasswordDialog::DoModal() on non-Windows when connecting to a
// world that has a character name but no stored password.

static int hook_password_dialog(const char* charName, char** outPassword)
{
    wxWindow* parent = App.m_pMainWnd
                      ? static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd)
                      : nullptr;

    wxString prompt;
    if (charName && *charName)
        prompt.Printf(wxT("Enter password for %s:"),
                      wxString::FromUTF8(charName));
    else
        prompt = wxT("Enter password:");

    wxPasswordEntryDialog dlg(parent, prompt, wxT("Password Required"));
    if (dlg.ShowModal() != wxID_OK) return IDCANCEL;

    wxString pw = dlg.GetValue();
    if (pw.IsEmpty()) return IDCANCEL;

    *outPassword = strdup(pw.ToUTF8().data());
    return IDOK;
}

// ── MFC dialog bridge: DoModal / EndDialog ────────────────────────────────
// Maps a live CDialog* to the wxDialog backing its modal loop, so EndDialog
// (which the canonical OnOK/OnCancel call) can end the right ShowModal.
static std::map<void*, wxDialog*> s_mfcModalDialogs;

static void hook_end_dialog(void* self, int result)
{
    auto it = s_mfcModalDialogs.find(self);
    if (it != s_mfcModalDialogs.end() && it->second && it->second->IsModal())
        it->second->EndModal(result);
}

static void bind_list_notifications(wxWindow* win, CCmdTarget* target);  // defined below

// Simulate MFC's WM_KICKIDLE command-UI update for a bridged dialog: run each
// control's ON_UPDATE_COMMAND_UI handler (if any) so buttons enable/disable/
// check themselves by state (e.g. CPluginsDlg greys "Show Info" when the
// selected plugin has no description, "Disable" for a disabled plugin, etc.).
// Run every ON_UPDATE_COMMAND_UI handler in `target`'s map against the controls
// of `container` (a wxDialog or a property-sheet page's wxPanel) — MFC's
// WM_KICKIDLE.  `container` is only used to resolve control HWNDs via
// g_wxhook_get_dlg_item, so any wxWindow works.
static void pump_dialog_update_ui(CCmdTarget* target, wxWindow* container, const RcDialog& t)
{
    if (!target || !container) return;
    for (const auto& c : t.controls) {
        if (c.id <= 0) continue;   // IDC_STATIC (-1) and unnamed controls
        CCmdUI cmdui;
        cmdui.m_nID      = (UINT) c.id;
        cmdui.m_hWndCtrl = g_wxhook_get_dlg_item ? g_wxhook_get_dlg_item(container, c.id) : nullptr;
        if (cmdui.m_hWndCtrl)
            AfxDispatchUpdateUI(target, &cmdui);
    }

    // Entity-list pages defer their list reload — which also rewrites the
    // "N items." summary static — to idle via m_bReloadList (set by Add / Change /
    // Delete / Paste).  MFC consumes it in OnKickIdle, which the command-UI loop
    // above does not call, so honour it here; otherwise the list + count never
    // refresh on the port.
    if (auto* gp = dynamic_cast<CGenPropertyPage*>(target))
        if (gp->m_bReloadList) { gp->m_bReloadList = false; gp->LoadList(); }
}

// The key translation the dialog bridge's CHAR_HOOK performs before offering
// the keystroke to PreTranslateMessage.  Split out of the lambda (see the
// comment at the Bind below for what depends on it) because a CHAR_HOOK is
// delivered by the event loop, and no in-app selftest ever reaches one.
// Declared in platform/wx_mfc_logic.h.
bool DialogKeyToMsg(int wxKeyCode, bool altDown, MSG* msg)
{
    if (!msg) return false;
    switch (wxKeyCode) {
        case WXK_TAB:
            msg->message = WM_KEYDOWN;
            msg->wParam  = VK_TAB;
            return true;
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER:
            // Alt+Return is WM_SYSKEYDOWN on Windows, and that is the only
            // thing distinguishing "open the multi-line editor" from "press
            // the default button" for the three entity dialogs.
            msg->message = altDown ? WM_SYSKEYDOWN : WM_KEYDOWN;
            msg->wParam  = VK_RETURN;
            return true;
        default:
            return false;
    }
}

static intptr_t hook_run_dialog(void* self, unsigned idd)
{
    CDialog* cdlg = static_cast<CDialog*>(self);
    const RcDialog* t = MfcDialogResources().FindDialog((int)idd);
    if (!t)
    {
        std::fprintf(stderr, "[dialog-bridge] DoModal: template %u not found\n", idd);
        return IDCANCEL;
    }

    wxWindow* parent = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    wxDialog* dlg = BuildWxDialogFromTemplate(parent, *t);
    if (!dlg) return IDCANCEL;

    cdlg->m_hWnd = (HWND) dlg;
    s_mfcModalDialogs[self] = dlg;

    // Route control commands to the canonical handlers.  A control with an
    // explicit ON_BN_CLICKED handler in the message map wins (MFC dispatches the
    // mapped handler before the default) — this matters for dialogs that remap
    // IDOK, e.g. CImmediateDlg's ON_BN_CLICKED(IDOK, OnRun), which runs script
    // and deliberately does NOT close.  Only when no handler is mapped do IDOK/
    // IDCANCEL fall back to the CDialog OnOK/OnCancel virtuals (DDX + EndDialog).
    // Buttons, checkboxes and radios all send BN_CLICKED.
    dlg->Bind(wxEVT_BUTTON, [cdlg](wxCommandEvent& e) {
        int id = e.GetId();
        if (AfxDispatchCommand(cdlg, BN_CLICKED, (UINT) id)) return;  // mapped handler ran
        if (id == IDOK)          cdlg->OnOK();
        else if (id == IDCANCEL) cdlg->OnCancel();
        else                     e.Skip();
    });
    dlg->Bind(wxEVT_CHECKBOX, [cdlg](wxCommandEvent& e) {
        if (!AfxDispatchCommand(cdlg, BN_CLICKED, (UINT) e.GetId())) e.Skip();
    });
    dlg->Bind(wxEVT_RADIOBUTTON, [cdlg](wxCommandEvent& e) {
        if (!AfxDispatchCommand(cdlg, BN_CLICKED, (UINT) e.GetId())) e.Skip();
    });
    // The non-BN_CLICKED notification codes.  Their ON_* map entries exist and
    // are correct, but nothing emitted them, so twelve canonical handlers were
    // dead: the trigger dialog's colour and send-to combos, the colour picker's
    // sort combo, the function list's filter box, and the double-click-to-accept
    // in the spell checker, Choose Notepad, Complete Word and command history.
    // wx has one event per control family; MFC has one code per family, so the
    // mapping is one line each.
    dlg->Bind(wxEVT_TEXT, [cdlg](wxCommandEvent& e) {
        if (!AfxDispatchCommand(cdlg, EN_CHANGE, (UINT) e.GetId())) e.Skip();
    });
    // wxComboBox (drop-down with an edit) and wxChoice (drop-list) raise
    // different events; CBS_DROPDOWNLIST templates build the latter.
    dlg->Bind(wxEVT_COMBOBOX, [cdlg](wxCommandEvent& e) {
        if (!AfxDispatchCommand(cdlg, CBN_SELCHANGE, (UINT) e.GetId())) e.Skip();
    });
    dlg->Bind(wxEVT_CHOICE, [cdlg](wxCommandEvent& e) {
        if (!AfxDispatchCommand(cdlg, CBN_SELCHANGE, (UINT) e.GetId())) e.Skip();
    });
    dlg->Bind(wxEVT_LISTBOX, [cdlg](wxCommandEvent& e) {
        if (!AfxDispatchCommand(cdlg, LBN_SELCHANGE, (UINT) e.GetId())) e.Skip();
    });
    dlg->Bind(wxEVT_LISTBOX_DCLICK, [cdlg](wxCommandEvent& e) {
        if (!AfxDispatchCommand(cdlg, LBN_DBLCLK, (UINT) e.GetId())) e.Skip();
    });
    // A dialog posting itself a command -- CWnd::PostMessage(WM_COMMAND, ...) --
    // arrives here as a queued wxEVT_MENU on the dialog window (hook_post_message
    // above).  The wxEVT_BUTTON bind only sees real button clicks, so without this
    // such a command fell on the floor; CTimerDlg/CTriggerDlg/CAliasDlg post
    // IDC_EDIT_SEND that way from their Alt+Return handlers.  Unhandled ids Skip()
    // so menu commands raised while the dialog is frontmost still reach the frame.
    dlg->Bind(wxEVT_MENU, [cdlg](wxCommandEvent& e) {
        if (!AfxDispatchCommand(cdlg, BN_CLICKED, (UINT) e.GetId())) e.Skip();
    });
    bind_list_notifications(dlg, cdlg);   // list column-sort / double-click (e.g. CPluginsDlg)
    // Closing the window (X / Esc) behaves like Cancel.
    dlg->Bind(wxEVT_CLOSE_WINDOW, [dlg](wxCloseEvent&) {
        if (dlg->IsModal()) dlg->EndModal(IDCANCEL);
    });
    // Resizeable dialogs (WS_THICKFRAME) drive the canonical OnSize handler,
    // which re-lays-out controls via CWnd::MoveWindow (now wired to wx).  OnSize
    // is a virtual, so this reaches the derived dialog's override directly.
    dlg->Bind(wxEVT_SIZE, [cdlg, dlg](wxSizeEvent& e) {
        wxSize cs = dlg->GetClientSize();
        cdlg->OnSize(0 /*SIZE_RESTORED*/, cs.GetWidth(), cs.GetHeight());
        e.Skip();
    });
    // MFC's message pump offers every keystroke to the dialog's
    // PreTranslateMessage BEFORE the focused control sees it.  The bridge went
    // straight to ShowModal, so six canonical overrides were unreachable dead
    // code.  wxEVT_CHAR_HOOK is the same position in wx: the top-level window
    // gets it ahead of the focused child.
    //   TAB        -> CEditMultiLine inserts a literal tab (the "Tab inserts tab
    //                 in multi-line dialogs" global preference, default off)
    //   RETURN     -> CLuaInputBox / CLuaInputEditDlg swallow <enter> when the
    //                 dialog wants no default button
    //   Alt+RETURN -> CTimerDlg / CTriggerDlg / CAliasDlg open the multi-line
    //                 editor for the "send" text (MFC sees WM_SYSKEYDOWN)
    // Translate only those two keys and Skip() everything else: a blanket hook
    // would disturb wxDialog's own Esc->Cancel and Enter->default-button in every
    // dialog in the app.  (Don't be tempted to add wxTE_PROCESS_TAB in the dialog
    // builder as a shortcut for the tab case -- that hardcodes the preference ON
    // for every ES_MULTILINE edit regardless of the flag.)
    dlg->Bind(wxEVT_CHAR_HOOK, [cdlg](wxKeyEvent& e) {
        MSG msg{};
        if (!DialogKeyToMsg(e.GetKeyCode(), e.AltDown(), &msg)) {
            e.Skip();
            return;
        }
        // MFC's contract: TRUE means the override consumed the key.
        if (!cdlg->PreTranslateMessage(&msg)) e.Skip();
    });
    // Command-UI: run each control's ON_UPDATE_COMMAND_UI handler on idle so
    // buttons enable/disable/check by state (e.g. after a list-selection change),
    // mirroring MFC's WM_KICKIDLE.  wxEVT_IDLE fires once the event queue drains
    // after each interaction, which is exactly when control state may have changed.
    dlg->Bind(wxEVT_IDLE, [cdlg, dlg, t](wxIdleEvent& e) {
        pump_dialog_update_ui(cdlg, dlg, *t);
        e.Skip();
    });

    cdlg->OnInitDialog();
    pump_dialog_update_ui(cdlg, dlg, *t);   // set initial control state

    wxMenuItem* wmItem = push_bridged_dialog(dlg);  // parent for nested pickers + Window-menu entry
    int result = dlg->ShowModal();
    pop_bridged_dialog(wmItem);

    // MFC sends WM_DESTROY on the way out, for OK and Cancel alike, and the
    // canonical OnDestroy handlers are where per-dialog state is written back:
    // App.SaveColumnConfiguration (list column widths + the sort column and
    // direction) and CWindowPlacement::Save (the dialog's size/position).  The
    // matching Restore* calls already run from OnInitDialog, so until now the
    // port read that state but never wrote it -- which is why list columns and
    // dialog geometry never persisted.  Run it while the wx window still
    // exists, since the handlers read the live controls.
    cdlg->OnDestroy();

    cdlg->m_hWnd = nullptr;
    s_mfcModalDialogs.erase(self);
    dlg->Destroy();
    return result;
}

// ── MFC property-sheet bridge: CTreePropertySheet::DoModal -> wxTreebook ──────
// World Properties (24 pages).  The canonical CPropertyPage objects are reused
// verbatim; we render them as a native wxTreebook (branch -> top-level page,
// node/endbranch -> child of the last top-level page), populate each page panel
// from its recovered template, run its OnInitDialog (canonical setup +
// UpdateData(FALSE) DDX load), and route in-page control commands through the
// reactivated message map.  On OK every page does UpdateData(TRUE) so the
// canonical GamePreferences ChangedPrefsP*/SavePrefsP* see the edited values.
// Route a window's list-control notifications to the canonical ON_NOTIFY handlers
// on `target` (the owning dialog/page).  wxListEvent is a command event and
// propagates up to the parent, so binding on the dialog/page panel catches every
// list it contains.  Covers column-click (sort) and double-click (edit/show-info).
static void bind_list_notifications(wxWindow* win, CCmdTarget* target)
{
    if (!win || !target) return;
    win->Bind(wxEVT_LIST_COL_CLICK, [target](wxListEvent& e) {
        NMLISTVIEW nm{};
        nm.hdr.idFrom = (UINT) e.GetId();
        nm.hdr.code   = (UINT) LVN_COLUMNCLICK;
        // DORMANT MISMATCH, live the day columns can be reordered: GetColumn()
        // is the VISUAL index, and CGenPropertyPage::OnColumnclickItemList
        // stores iSubItem as m_last_col -- a LOGICAL column, persisted as
        // "<list> Sort Sequence".  The two coincide only because the column
        // order is always identity on the port (see
        // docs/LIST-CONTROL-DECISION.md §6).  Anyone making reordering work
        // must map visual -> logical here first, or clicking a moved header
        // sorts by the wrong field.
        nm.iSubItem   = e.GetColumn();
        LRESULT res = 0;
        if (!AfxDispatchNotify(target, (UINT) LVN_COLUMNCLICK, (UINT) e.GetId(), &nm.hdr, &res))
            e.Skip();
    });
    win->Bind(wxEVT_LIST_ITEM_ACTIVATED, [target](wxListEvent& e) {
        NMITEMACTIVATE nm{};
        nm.hdr.idFrom = (UINT) e.GetId();
        nm.hdr.code   = (UINT) NM_DBLCLK;
        nm.iItem      = e.GetIndex();
        LRESULT res = 0;
        if (!AfxDispatchNotify(target, (UINT) NM_DBLCLK, (UINT) e.GetId(), &nm.hdr, &res))
            e.Skip();
    });
    // Item selection change → LVN_ITEMCHANGED.  Without this, e.g. the canonical
    // CColourPickerDlg never updates its RGB sliders/preview when you pick a
    // colour from the list (OnItemchangedColourlist never fires), and entity
    // managers never enable/disable their Edit/Remove buttons.  Fire on both
    // select and deselect with the matching LVIS_SELECTED state so handlers that
    // test uNewState behave correctly.
    auto onSelChange = [target](wxListEvent& e) {
        bool selected = (e.GetEventType() == wxEVT_LIST_ITEM_SELECTED);
        NMLISTVIEW nm{};
        nm.hdr.idFrom = (UINT) e.GetId();
        nm.hdr.code   = (UINT) LVN_ITEMCHANGED;
        nm.iItem      = e.GetIndex();
        nm.uChanged   = LVIF_STATE;
        nm.uNewState  = selected ? LVIS_SELECTED : 0;
        nm.uOldState  = selected ? 0 : LVIS_SELECTED;
        LRESULT res = 0;
        if (!AfxDispatchNotify(target, (UINT) LVN_ITEMCHANGED, (UINT) e.GetId(), &nm.hdr, &res))
            e.Skip();
    };
    win->Bind(wxEVT_LIST_ITEM_SELECTED,   onSelChange);
    win->Bind(wxEVT_LIST_ITEM_DESELECTED, onSelChange);
}

// Build a panel for one canonical CPropertyPage under `bookParent` (a wxTreebook
// or wxNotebook): populate from its recovered template, bind its m_hWnd, route
// in-panel control commands to its canonical ON_BN_CLICKED/etc. handlers, and run
// OnInitDialog (DDX load via UpdateData(FALSE)).  Shared by the tree
// (hook_run_property_sheet) and flat (hook_run_property_sheet_flat) bridges.
static wxPanel* build_canonical_sheet_page(wxWindow* bookParent, CPropertyPage* page)
{
    wxPanel* panel = new wxPanel(bookParent, wxID_ANY);

    const RcDialog* t = page
        ? MfcDialogResources().FindDialog((int) page->m_nIDTemplate)
        : nullptr;
    if (t)
        PopulateWindowFromTemplate(panel, *t);
    else
        panel->SetMinClientSize(wxSize(420, 240));   // keep page-index alignment

    if (page)
    {
        page->m_hWnd = (HWND) panel;   // GetDlgItem/DDX search within this panel

        auto dispatch = [page](wxCommandEvent& e) {
            if (!AfxDispatchCommand(page, BN_CLICKED, (UINT) e.GetId())) e.Skip();
        };
        panel->Bind(wxEVT_BUTTON,      dispatch);
        panel->Bind(wxEVT_CHECKBOX,    dispatch);
        panel->Bind(wxEVT_RADIOBUTTON, dispatch);
        // Same non-BN_CLICKED codes as the modal-dialog path above -- the world
        // prefs pages carry four of the twelve (the trigger colour/send-to
        // combos and the two colour combos on the appearance pages).
        auto notify = [page](UINT code) {
            return [page, code](wxCommandEvent& e) {
                if (!AfxDispatchCommand(page, code, (UINT) e.GetId())) e.Skip();
            };
        };
        panel->Bind(wxEVT_TEXT,           notify(EN_CHANGE));
        panel->Bind(wxEVT_COMBOBOX,       notify(CBN_SELCHANGE));
        panel->Bind(wxEVT_CHOICE,         notify(CBN_SELCHANGE));
        panel->Bind(wxEVT_LISTBOX,        notify(LBN_SELCHANGE));
        panel->Bind(wxEVT_LISTBOX_DCLICK, notify(LBN_DBLCLK));
        bind_list_notifications(panel, page);   // list column-sort / double-click

        page->OnInitDialog();          // canonical setup + DDX load (UpdateData(FALSE))

        // Command-UI pump (WM_KICKIDLE) so ON_UPDATE_COMMAND_UI handlers run —
        // e.g. the entity pages' Edit/Copy/Delete buttons enable/disable with the
        // list selection (CGenPropertyPage::OnUpdateNeedSelection).  Pump on idle
        // but only for the page that's actually on screen (the treebook/notebook
        // hides the rest), plus once now to set the initial state.
        if (t) {
            panel->Bind(wxEVT_IDLE, [page, panel, t](wxIdleEvent& e) {
                if (panel->IsShownOnScreen())
                    pump_dialog_update_ui(page, panel, *t);
                e.Skip();
            });
            pump_dialog_update_ui(page, panel, *t);
        }
    }
    return panel;
}

// ── The config sheet's caption band ──────────────────────────────────────────
// Global Preferences ▸ General ▸ "Use colour gradient in &config"
// (App.m_bColourGradient) has exactly one consumer: configuration.cpp sets
// sheet.m_clrCaptionRight to yellow when it is on and to the same red as
// m_clrCaptionLeft when it is off, i.e. gradient vs flat.  Windows paints that
// pair across a band at the top of the sheet (CTreePropertySheet::
// RenderCaptionBar, a per-column _interpolate loop) with the current page's
// caption drawn into it.  The port's lightweight sheet recorded the colours so
// the canonical setup code would compile, and TreePropertySheet.h says outright
// that the wx chrome ignores them — so there was no band, and the checkbox did
// nothing at all.
//
// wxDC::GradientFillLinear with wxEAST is the same left-to-right ramp the
// Windows loop walks by hand, so a flat band (left == right) still comes out
// flat and the setting reads exactly as it does on Windows.
class CSheetCaptionBar : public wxPanel
{
public:
    CSheetCaptionBar(wxWindow* parent, COLORREF left, COLORREF right,
                     COLORREF textColour)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 30)),
          m_left (GetRValue(left),  GetGValue(left),  GetBValue(left)),
          m_right(GetRValue(right), GetGValue(right), GetBValue(right)),
          m_text (GetRValue(textColour), GetGValue(textColour),
                  GetBValue(textColour))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &CSheetCaptionBar::OnPaint, this);
    }

    void SetCaption(const wxString& s)
    {
        if (s == m_caption) return;
        m_caption = s;
        Refresh(false);
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxRect r(GetClientSize());
        dc.GradientFillLinear(r, m_left, m_right, wxEAST);

        if (m_caption.IsEmpty()) return;
        // Windows bottom-aligns the caption against the band; centring it reads
        // better with macOS metrics and the band is chrome, not the setting.
        wxFont f = GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        f.SetPointSize(f.GetPointSize() + 2);
        dc.SetFont(f);
        dc.SetTextForeground(m_text);
        dc.SetBackgroundMode(wxTRANSPARENT);
        wxCoord tw = 0, th = 0;
        dc.GetTextExtent(m_caption, &tw, &th);
        dc.DrawText(m_caption, 8, (r.height - th) / 2);
    }

    wxColour m_left, m_right, m_text;
    wxString m_caption;
};

static intptr_t hook_run_property_sheet(void* self)
{
    CTreePropertySheet* sheet = static_cast<CTreePropertySheet*>(self);
    if (!sheet) return IDCANCEL;

    wxWindow* parent = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    wxString title = sheet->m_caption.IsEmpty()
                       ? wxString(wxT("World Properties"))
                       : wxString::FromUTF8((const char*) sheet->m_caption);

    wxDialog dlg(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    CSheetCaptionBar* bar = new CSheetCaptionBar(&dlg,
                                                 sheet->m_clrCaptionLeft,
                                                 sheet->m_clrCaptionRight,
                                                 sheet->m_clrCaptionTextLeft);

    wxTreebook* tb = new wxTreebook(&dlg, wxID_ANY);

    // Pages, index-aligned with the treebook (insertion order == add order ==
    // the canonical nPage that m_iWantedPage is compared against).
    std::vector<CPropertyPage*> pages;
    pages.reserve(sheet->m_portPages.size());

    // Branches the canonical code asked to be shown expanded.  GamePreferences
    // ORs tps_item_expanded into the branch's nType when the "Auto-expand config
    // screens" global preference is on (configuration.cpp: `int iExpand =
    // App.m_bAutoExpand ? tps_item_expanded : 0;`), which is the default.  The
    // mask below is tps_item_type_mask == 3 and the expanded bit is 4, so this
    // bridge used to drop it on the floor and every branch came up collapsed.
    std::vector<size_t> expandNodes;

    for (const auto& pp : sheet->m_portPages)
    {
        CPropertyPage* page  = pp.pPage;
        wxPanel*       panel = build_canonical_sheet_page(tb, page);

        wxString label = wxString::FromUTF8((const char*) pp.caption);
        if ((pp.nType & tps_item_type_mask) == tps_item_branch) {
            if (pp.nType & tps_item_expanded)
                expandNodes.push_back(pages.size());   // == the treebook index
            tb->AddPage(panel, label);
        }
        else
            tb->AddSubPage(panel, label);

        pages.push_back(page);
    }

    // Expand the branches that asked for it.  After the build loop, because
    // wxTreebook::ExpandNode does not stick on a branch that has no children
    // yet, and before SetSelection so the opening page is already visible.
    for (size_t n : expandNodes)
        tb->ExpandNode(n, true);

    // Open at the wanted page (m_iWantedPage == add-order index; see
    // TreePropertySheet.cpp OnInitDialog `nPage == m_iWantedPage`).
    int wanted = sheet->m_iWantedPage;
    if (wanted >= 0 && wanted < (int) tb->GetPageCount())
        tb->SetSelection((size_t) wanted);

    // Validate-on-leave, matching MFC's OnKillActive: when the user navigates
    // away from a page, capture its controls into members (UpdateData(TRUE)) and
    // run its DDV; if validation fails the page already showed its message, so
    // veto the page change and stay put.  Bound AFTER the initial SetSelection so
    // it doesn't fire during setup.  Pages never visited keep their LoadPrefs
    // values (valid) — exactly as on Windows, where unvisited pages aren't
    // validated and GamePreferences' SavePrefsP* then writes back unchanged.
    tb->Bind(wxEVT_TREEBOOK_PAGE_CHANGING, [&pages](wxBookCtrlEvent& e) {
        int leaving = e.GetOldSelection();
        if (leaving >= 0 && leaving < (int) pages.size() && pages[leaving]) {
            if (!pages[leaving]->UpdateData(TRUE)) {
                e.Veto();   // validation failed (message shown); stay on this page
                return;
            }
            // Then run the page's own OnKillActive.  Some canonical pages harvest
            // state that DDX cannot reach: CPrefsP3/CPrefsP5 capture their colour
            // swatches here (SaveSwatches), because the swatch buttons hold the
            // colour in the control and DoDataExchange only DDX_Control's them.
            // Calling UpdateData alone -- as this bridge used to -- silently threw
            // every colour pick away.
            pages[leaving]->OnKillActive();
        }
    });

    // Arrival side, matching MFC's OnSetActive -- the counterpart of the leave
    // handler above, and the other half of MFC's page-switch pair (OnKillActive
    // on the old page, then OnSetActive on the new one).  Nothing on the port
    // ever called it, so every canonical OnSetActive body was dead code, and
    // with it:
    //   * m_doc->m_last_prefs_page = m_page_number -- written by all 24
    //     overrides, and the ONLY writer of the member that Game > World
    //     Properties reads (configuration.cpp GamePreferences, iPage == -1) to
    //     reopen on the page you last used.  With no writer it stayed at its
    //     doc_construct.cpp seed, so the sheet always opened on General.
    //   * CPrefsP3::LoadSwatches -- re-read the colour swatches on return, so
    //     the custom-colour page reflects edits made since it was last shown.
    //   * CPrefsP9 / CPrefsP17 -- rebuild their colour combos from CPrefsP3's
    //     m_strCustomName, which is how a custom colour renamed on the Custom
    //     Colours page shows up in the input/notes colour lists without
    //     closing and reopening the sheet.
    // (The ((CTreePropertySheet *) GetParent ())->PageHasChanged (this) in each
    // body is the documented port no-op: the shim's GetParent returns NULL and
    // the port's PageHasChanged has an empty body that touches no members --
    // see TreePropertySheet.h.  The wxTreebook drives its own selection.)
    // Bound after the initial SetSelection, like the leave handler.
    tb->Bind(wxEVT_TREEBOOK_PAGE_CHANGED, [&pages, sheet, bar](wxBookCtrlEvent& e) {
        int arriving = e.GetSelection();
        if (arriving >= 0 && arriving < (int) pages.size() && pages[arriving])
            pages[arriving]->OnSetActive();
        // Keep the caption band on the page you are looking at, as Windows'
        // OnPaint does (it draws m_acPropPages[GetActiveIndex()].sCaption).
        if (arriving >= 0 && arriving < (int) sheet->m_portPages.size())
            bar->SetCaption(wxString::FromUTF8(
                (const char*) sheet->m_portPages[arriving].caption));
        e.Skip();
    });

    // ... including the page the sheet opens on, which MFC activates too (so
    // just opening the sheet and pressing OK records the page you were on).
    {
        int opening = tb->GetSelection();
        if (opening >= 0 && opening < (int) pages.size() && pages[opening])
            pages[opening]->OnSetActive();
        if (opening >= 0 && opening < (int) sheet->m_portPages.size())
            bar->SetCaption(wxString::FromUTF8(
                (const char*) sheet->m_portPages[opening].caption));
    }

    // Standard OK / Cancel.
    wxStdDialogButtonSizer* btns = new wxStdDialogButtonSizer();
    wxButton* ok = new wxButton(&dlg, wxID_OK);
    btns->AddButton(ok);
    btns->AddButton(new wxButton(&dlg, wxID_CANCEL));
    btns->Realize();
    ok->SetDefault();

    wxBoxSizer* top = new wxBoxSizer(wxVERTICAL);
    top->Add(bar,  0, wxEXPAND);
    top->Add(tb,   1, wxEXPAND | wxALL, 6);
    top->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    dlg.SetSizer(top);
    dlg.Fit();
    wxSize sz = dlg.GetSize();
    dlg.SetSize(wxSize(std::max(sz.x, 680), std::max(sz.y, 520)));
    dlg.CentreOnParent();

    // OK: validate the ACTIVE page only (MFC validates the active page's
    // OnKillActive on OK; other pages were already captured/validated when the
    // user navigated away from them, and unvisited pages keep their loaded
    // values).  If the active page fails validation it has shown its message, so
    // keep the sheet open.  Accept (EndModal) only when the active page passes.
    // (Cancel keeps wxDialog's default: ends the modal loop with wxID_CANCEL.)
    dlg.Bind(wxEVT_BUTTON, [&dlg, &pages, tb](wxCommandEvent& e) {
        if (e.GetId() != wxID_OK) { e.Skip(); return; }
        int active = tb->GetSelection();
        if (active >= 0 && active < (int) pages.size() && pages[active]) {
            if (!pages[active]->UpdateData(TRUE))
                return;   // validation failed (message shown); stay open
            pages[active]->OnKillActive();   // capture what DDX cannot (see above)
        }
        // Then OnOK every page, as CPropertySheet::OnOK does.  Pages that do not
        // override it get the base no-op, so this is only ever additive; the ones
        // that DO override it (CPrefsP3/P5 colour swatches, CPrefsP9, CPrefsP17,
        // CPrefsP22/P23, CGlobalPrefsP1/P10/P12) were previously never asked to
        // commit their state at all, so their settings could not be saved.
        for (CPropertyPage* page : pages)
            if (page)
                page->OnOK();
        dlg.EndModal(wxID_OK);
    }, wxID_OK);

    wxMenuItem* wmItem = push_bridged_dialog(&dlg);  // nested pickers parent here + Window-menu entry
    int rc = dlg.ShowModal();
    pop_bridged_dialog(wmItem);

    // Each page's OnDestroy, as MFC's WM_DESTROY does -- on Cancel too.  For the
    // entity pages this is CGenPropertyPage::OnDestroy: it records the selected
    // item, frees the per-row CString* it stored as item data (a leak until
    // now), and calls App.SaveColumnConfiguration -- the write half of the
    // RestoreColumnConfiguration that already runs in OnInitDialog, which is why
    // list column widths and the sort column never persisted.  CPrefsP6 does the
    // same for the macro list.  Must run before the panels die: the handlers
    // read the live controls.
    for (CPropertyPage* page : pages)
        if (page) page->OnDestroy();

    for (CPropertyPage* page : pages)            // detach before panels die
        if (page) page->m_hWnd = nullptr;

    return (rc == wxID_OK) ? IDOK : IDCANCEL;
}

// Flat (tabbed) property sheet — base CPropertySheet::DoModal.  Used by plain
// CPropertySheet subclasses such as CGlobalPrefsSheet (File ▸ Preferences).
// Builds a wxNotebook from the base m_flatPages list (one tab per canonical
// CPropertyPage, tab label = the page template's caption), running each page's
// OnInitDialog/DDX.  Validate-on-leave + validate-active-on-OK mirror MFC's
// OnKillActive; unvisited pages keep their loaded values, which the caller
// (DoGlobalPrefs) then writes back unchanged.  Returns IDOK/IDCANCEL.
static intptr_t hook_run_property_sheet_flat(void* self)
{
    CPropertySheet* sheet = static_cast<CPropertySheet*>(self);
    if (!sheet) return IDCANCEL;

    wxWindow* parent = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    // Window title from the sheet's caption: a literal (LPCTSTR ctor / SetTitle),
    // or a known string-resource id from the UINT ctor (the port has no string
    // table) — IDS_PROPSHT_CAPTION1=142 (Global Prefs), 304 (Plugin Wizard).
    wxString sheetTitle =
        !sheet->m_strSheetCaption.IsEmpty()
            ? wxString::FromUTF8((LPCTSTR) sheet->m_strSheetCaption)
        : (sheet->m_nSheetCaptionId == 304) ? wxString(wxT("Plugin Wizard"))
        : (sheet->m_nSheetCaptionId == 142) ? wxString(wxT("Global Preferences"))
        :                                     wxString(wxT("Properties"));
    wxDialog dlg(parent, wxID_ANY, sheetTitle,
                 wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    wxNotebook* nb = new wxNotebook(&dlg, wxID_ANY);

    std::vector<CPropertyPage*> pages;
    pages.reserve(sheet->m_flatPages.size());

    int pageNo = 0;
    for (CPropertyPage* page : sheet->m_flatPages)
    {
        wxPanel* panel = build_canonical_sheet_page(nb, page);

        const RcDialog* t = page
            ? MfcDialogResources().FindDialog((int) page->m_nIDTemplate)
            : nullptr;
        wxString label = (t && !t->caption.empty())
                           ? wxString::FromUTF8(t->caption.c_str())
                           : wxString::Format(wxT("Page %d"), pageNo + 1);
        nb->AddPage(panel, label);

        pages.push_back(page);
        ++pageNo;
    }

    // Validate-on-leave (MFC OnKillActive): capture + validate the page being
    // left; veto the tab change if it fails (the page already showed its msg).
    nb->Bind(wxEVT_NOTEBOOK_PAGE_CHANGING, [&pages](wxBookCtrlEvent& e) {
        int leaving = e.GetOldSelection();
        if (leaving >= 0 && leaving < (int) pages.size()
            && pages[leaving]) {
            if (!pages[leaving]->UpdateData(TRUE)) {
                e.Veto();
                return;
            }
            pages[leaving]->OnKillActive();   // see the tree bridge above
        }
    });

    wxStdDialogButtonSizer* btns = new wxStdDialogButtonSizer();
    wxButton* ok = new wxButton(&dlg, wxID_OK);
    btns->AddButton(ok);
    btns->AddButton(new wxButton(&dlg, wxID_CANCEL));
    btns->Realize();
    ok->SetDefault();

    wxBoxSizer* top = new wxBoxSizer(wxVERTICAL);
    top->Add(nb,   1, wxEXPAND | wxALL, 6);
    top->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    dlg.SetSizer(top);
    dlg.Fit();
    wxSize sz = dlg.GetSize();
    dlg.SetSize(wxSize(std::max(sz.x, 680), std::max(sz.y, 520)));
    dlg.CentreOnParent();

    // OK: validate the active page only (others validated on leave; unvisited
    // pages keep their loaded values).  Keep the sheet open on failure.
    dlg.Bind(wxEVT_BUTTON, [&dlg, &pages, nb](wxCommandEvent& e) {
        if (e.GetId() != wxID_OK) { e.Skip(); return; }
        int active = nb->GetSelection();
        if (active >= 0 && active < (int) pages.size() && pages[active]) {
            if (!pages[active]->UpdateData(TRUE))
                return;   // validation failed (message shown); stay open
            pages[active]->OnKillActive();
        }
        // …then OnOK every page, as CPropertySheet::OnOK does.  This bridge had
        // the same gap as the tree bridge above: File > Preferences pages that
        // commit their state in OnOK (CGlobalPrefsP1 startup-world list,
        // CGlobalPrefsP10, CGlobalPrefsP12 notepad colours) were never asked to,
        // so those settings could not be saved at all.
        for (CPropertyPage* page : pages)
            if (page)
                page->OnOK();
        dlg.EndModal(wxID_OK);
    }, wxID_OK);

    wxMenuItem* wmItem = push_bridged_dialog(&dlg);  // nested pickers parent here + Window-menu entry
    int rc = dlg.ShowModal();
    pop_bridged_dialog(wmItem);

    // Each page's OnDestroy, as MFC's WM_DESTROY does (see the tree sheet above).
    // No global-prefs page overrides it today, so this is currently a base no-op
    // -- it is here so a page that starts saving state on close works by itself.
    for (CPropertyPage* page : pages)
        if (page) page->OnDestroy();

    for (CPropertyPage* page : pages)            // detach before panels die
        if (page) page->m_hWnd = nullptr;

    return (rc == wxID_OK) ? IDOK : IDCANCEL;
}

// ── Dialog control accessors (GetDlgItem / DDX) ───────────────────────────
static void* hook_get_dlg_item(void* parent, int id)
{
    return parent ? ((wxWindow*) parent)->FindWindow(id) : nullptr;
}

// CStaticLink's handlers are protected, and the port's ON_WM_* macros are empty
// so there is no message-map route to them.  Reach OnLButtonDown with the
// standard derived-class member-pointer idiom: the access check passes inside
// the derived class, and the resulting pointer-to-member is ordinary to call.
// This needs no edit to the shared Windows source and no cast of the object.
struct CStaticLinkPortGlue : public CStaticLink {
    static void Click(CStaticLink* p) {
        void (CStaticLink::*fn)(UINT, CPoint) = &CStaticLinkPortGlue::OnLButtonDown;
        (p->*fn)(0, CPoint(0, 0));
    }
};

// g_wxhook_subclass_dlg_item.  Generic part: nothing — returning null lets
// SubclassDlgItem simply bind the existing child, which is right for any control
// the port can already drive.  Special part: CStaticLink, the only real
// SubclassDlgItem client in the tree (MUSHclient.cpp x3, welcome.cpp,
// welcome1.cpp, prefspropertypages.cpp x2).  Those render as plain grey static
// text today — not blue, not underlined, no hand cursor, click does nothing —
// because their whole message map expands to nothing.
//
// Swap in a wxHyperlinkCtrl, which draws itself and owns its own mouse handling,
// rather than styling the wxStaticText and binding wxEVT_LEFT_DOWN to it: on
// wxOSX wxStaticText is peered on an NSTextField whose mouse delivery to wx is
// unreliable.
static void* hook_subclass_dlg_item(void* parent, int id, void* pwnd)
{
    wxWindow* child = parent ? ((wxWindow*) parent)->FindWindow(id) : nullptr;
    CWnd*     w     = (CWnd*) pwnd;
    if (!child || !w) return nullptr;
    if (!w->IsKindOf(RUNTIME_CLASS(CStaticLink))) return nullptr;

    wxStaticText* st = wxDynamicCast(child, wxStaticText);
    if (!st) return nullptr;

    CStaticLink* link      = (CStaticLink*) w;
    wxWindow*    parentWin = st->GetParent();
    wxPoint      pos       = st->GetPosition();
    wxSize       size      = st->GetSize();
    wxString     text      = st->GetLabel();
    st->Destroy();          // safe: dialog setup, not inside this widget's event

    // The URL is NOT known yet — DDX_Text for these controls runs later, in
    // UpdateData(FALSE).  So leave wx's own URL empty and let the canonical
    // handler re-read the control text at click time (StatLink.cpp:92).
    wxHyperlinkCtrl* hl = new wxHyperlinkCtrl(parentWin, id, text, wxEmptyString,
                                              pos, size,
                                              wxHL_ALIGN_LEFT | wxNO_BORDER);
    auto wxFromCOLORREF = [](COLORREF c) {
        return wxColour(GetRValue(c), GetGValue(c), GetBValue(c));
    };
    hl->SetNormalColour (wxFromCOLORREF(CStaticLink::g_colorUnvisited));
    hl->SetHoverColour  (wxFromCOLORREF(CStaticLink::g_colorUnvisited));  // Windows has no hover colour
    hl->SetVisitedColour(wxFromCOLORREF(CStaticLink::g_colorVisited));
    // Deliberately NO SetFont here: wxGenericHyperlinkCtrl::Create already
    // underlines its font, and copying the plain static's font over it would
    // strip the underline — i.e. undo half of what this fix is for.

    hl->Bind(wxEVT_HYPERLINK, [link](wxHyperlinkEvent&) {
        CStaticLinkPortGlue::Click(link);   // canonical Navigate() → ShellExecute
        // Mirror MFC: go purple only when Navigate() actually succeeded, which
        // is what CStaticLink signals by setting m_color to the visited colour.
        if (wxHyperlinkCtrl* c = wxDynamicCast((wxWindow*) link->m_hWnd, wxHyperlinkCtrl)) {
            c->SetVisited(link->m_color == CStaticLink::g_colorVisited);
            c->Refresh();
        }
    });
    return hl;
}

static char* hook_get_window_text(void* hwnd)
{
    wxWindow* w = (wxWindow*) hwnd;
    if (!w) return nullptr;
    wxString s;
    if (wxTextCtrl* t = wxDynamicCast(w, wxTextCtrl))      s = t->GetValue();
    else if (wxComboBox* c = wxDynamicCast(w, wxComboBox)) s = c->GetValue();
    else if (wxChoice* ch = wxDynamicCast(w, wxChoice))    s = ch->GetStringSelection();
    else                                                   s = w->GetLabel();
    return strdup(s.utf8_str());
}

// ── CEdit selection / line / insertion (the canonical dialogs use these for
// Go-To-Line, insert-at-caret of special chars / regexp tokens, Tab, and the
// Notes-page Find).  hwnd is the live wxTextCtrl bound by DDX_Control. ──────
static void hook_edit_get_sel(void* hwnd, int* s, int* e) {
    long from = 0, to = 0;
    if (auto* t = wxDynamicCast((wxWindow*) hwnd, wxTextCtrl)) t->GetSelection(&from, &to);
    if (s) *s = (int) from;
    if (e) *e = (int) to;
}
static void hook_edit_set_sel(void* hwnd, int s, int e) {
    auto* t = wxDynamicCast((wxWindow*) hwnd, wxTextCtrl);
    if (!t) return;
    if (s < 0) { long ip = t->GetInsertionPoint(); t->SetSelection(ip, ip); return; }  // SetSel(-1,..) deselects
    long end = (e < 0) ? t->GetLastPosition() : (long) e;                              // SetSel(s,-1) → to end
    t->SetSelection((long) s, end);
    t->ShowPosition(end);
}
static void hook_edit_replace_sel(void* hwnd, const char* text) {
    auto* t = wxDynamicCast((wxWindow*) hwnd, wxTextCtrl);
    if (!t) return;
    long from = 0, to = 0; t->GetSelection(&from, &to);
    wxString s = wxString::FromUTF8(text ? text : "");
    if (from != to) t->Replace(from, to, s);   // replace the selection
    else            t->WriteText(s);            // or insert at the caret
}
static int hook_edit_line_info(void* hwnd, int op, int arg) {
    auto* t = wxDynamicCast((wxWindow*) hwnd, wxTextCtrl);
    if (!t) return 0;
    long col = 0, line = 0;
    switch (op) {
    case 0: return (int) t->GetNumberOfLines();                          // line count
    case 1:                                                              // char index of line `arg`
        if (arg < 0) { if (!t->PositionToXY(t->GetInsertionPoint(), &col, &line)) line = 0; }
        else         line = arg;
        { long pos = t->XYToPosition(0, line); return pos < 0 ? 0 : (int) pos; }
    case 2:                                                              // length of line holding char `arg`
        if (!t->PositionToXY(arg < 0 ? t->GetInsertionPoint() : (long) arg, &col, &line)) return 0;
        return (int) t->GetLineLength(line);
    case 3:                                                              // line of char `arg`
        if (!t->PositionToXY(arg < 0 ? t->GetInsertionPoint() : (long) arg, &col, &line)) return 0;
        return (int) line;
    case 4: return (int) t->GetLastPosition();                           // total text length
    }
    return 0;
}

// CWnd::SetFont — used by FixFont (Utilities.cpp) to put the fixed-pitch font on
// the trigger/alias/timer/script editors.  wxWindow::SetFont copies the font, so
// the caller's CFont may be destroyed afterwards.
static void hook_set_font(void* hwnd, void* font)
{
    // NOT resolveTarget(): that maps a null handle to the MDI parent frame, so an
    // unbound control would restyle the whole main window.
    if (!hwnd) return;
    wxWindow* w = toWx(hwnd);
    wxFont*   f = (wxFont*) font;
    if (!w || !f || !f->IsOk()) return;
    w->SetFont(*f);
    w->Refresh();
}

// BST_UNCHECKED = 0, BST_CHECKED = 1, BST_INDETERMINATE = 2.  A BS_AUTO3STATE
// template builds a 3-state wxCheckBox (see wx_dialog_bridge.cpp), and its third
// state has to survive the round trip through DDX_Check -- otherwise a trigger's
// "don't care" style match reads back as "must not match".  Set3StateValue
// asserts on a 2-state box, so the 3-state arm is taken only when there is one.
static void hook_button_set_check(void* hwnd, int state)
{
    wxWindow* w = (wxWindow*) hwnd;
    if (wxCheckBox* cb = wxDynamicCast(w, wxCheckBox)) {
        if (cb->Is3State())
            cb->Set3StateValue(state == 2 ? wxCHK_UNDETERMINED
                             : state == 1 ? wxCHK_CHECKED
                                          : wxCHK_UNCHECKED);
        else
            cb->SetValue(state != 0);
    }
    else if (wxRadioButton* rb = wxDynamicCast(w, wxRadioButton)) rb->SetValue(state != 0);
}

static int hook_button_get_check(void* hwnd)
{
    wxWindow* w = (wxWindow*) hwnd;
    if (wxCheckBox* cb = wxDynamicCast(w, wxCheckBox)) {
        if (!cb->Is3State()) return cb->GetValue() ? 1 : 0;
        switch (cb->Get3StateValue()) {
            case wxCHK_CHECKED:      return 1;
            case wxCHK_UNDETERMINED: return 2;
            default:                 return 0;
        }
    }
    if (wxRadioButton* rb = wxDynamicCast(w, wxRadioButton))      return rb->GetValue() ? 1 : 0;
    return 0;
}

static void hook_enable_window(void* hwnd, int enable)
{
    if (hwnd) ((wxWindow*) hwnd)->Enable(enable != 0);
}

// ── CListCtrl → wxListCtrl (entity-manager object lists) ──────────────────
// `hwnd` is the wxListCtrl* (bound onto a CListCtrl member by DDX_Control).
// The canonical CGenPropertyPage list pages drive these in report mode and stash
// a CString* (the object name) as each row's item data.
namespace {
inline wxListCtrl* LC(void* hwnd) { return wxDynamicCast((wxWindow*) hwnd, wxListCtrl); }
}

static void hook_listctrl_insert_column(void* hwnd, int col, const char* heading,
                                        int fmt, int width)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc) return;
    // LVCFMT_RIGHT=1, LVCFMT_CENTER=2 → wxLIST_FORMAT_*; default left.
    int wxfmt = (fmt == 1) ? wxLIST_FORMAT_RIGHT
              : (fmt == 2) ? wxLIST_FORMAT_CENTRE
              :              wxLIST_FORMAT_LEFT;
    lc->InsertColumn(col, wxString::FromUTF8(heading ? heading : ""), wxfmt,
                     width > 0 ? width : wxLIST_AUTOSIZE);
}

static int hook_listctrl_insert_item(void* hwnd, int item, const char* text)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc) return -1;
    return lc->InsertItem(item, wxString::FromUTF8(text ? text : ""));
}

static void hook_listctrl_set_item_text(void* hwnd, int item, int col, const char* text)
{
    if (wxListCtrl* lc = LC(hwnd))
        lc->SetItem(item, col, wxString::FromUTF8(text ? text : ""));
}

static int hook_listctrl_get_item_count(void* hwnd)
{
    wxListCtrl* lc = LC(hwnd);
    return lc ? lc->GetItemCount() : 0;
}

static void hook_listctrl_set_item_data(void* hwnd, int item, void* data)
{
    if (wxListCtrl* lc = LC(hwnd))
        lc->SetItemPtrData(item, (wxUIntPtr) data);
}

static void* hook_listctrl_get_item_data(void* hwnd, int item)
{
    wxListCtrl* lc = LC(hwnd);
    return lc ? (void*) lc->GetItemData(item) : nullptr;
}

// MFC: GetItemText(item, subitem) — the label of a row (subitem 0) or of one of
// its columns.  Caller frees.  CListCtrl::GetItemText was the one accessor in
// the shim with no hook behind it, hardcoded to return an empty CString, and
// the Functions dialog's Copy button is its only caller: it copied "" over
// whatever the user had on the clipboard.
static char* hook_listctrl_get_item_text(void* hwnd, int item, int subitem)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc) return nullptr;
    // Named local: utf8_str() can return a view into the string's own storage.
    const wxString s = lc->GetItemText(item, subitem);
    return strdup(s.utf8_str().data());
}

// MFC: GetNextItem(start, flags) returns the next item after `start` matching
// `flags` (LVNI_ALL=0 → any; LVNI_SELECTED=2 → selected), or -1.  start==-1
// means "from the beginning".
static int hook_listctrl_get_next_item(void* hwnd, int start, int flags)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc) return -1;
    int geom = (flags & 0x2 /*LVNI_SELECTED*/)
                 ? wxLIST_NEXT_ALL : wxLIST_NEXT_ALL;
    int state = (flags & 0x2 /*LVNI_SELECTED*/)
                 ? wxLIST_STATE_SELECTED : wxLIST_STATE_DONTCARE;
    return lc->GetNextItem(start, geom, state);
}

static void hook_listctrl_set_item_state(void* hwnd, int item, unsigned state, unsigned mask)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc) return;
    long wxstate = 0, wxmask = 0;
    if (mask & 0x2 /*LVIS_SELECTED*/) { wxmask |= wxLIST_STATE_SELECTED;
        if (state & 0x2) wxstate |= wxLIST_STATE_SELECTED; }
    if (mask & 0x1 /*LVIS_FOCUSED*/)  { wxmask |= wxLIST_STATE_FOCUSED;
        if (state & 0x1) wxstate |= wxLIST_STATE_FOCUSED; }
    lc->SetItemState(item, wxstate, wxmask);
}

static unsigned hook_listctrl_get_item_state(void* hwnd, int item, unsigned mask)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc) return 0;
    unsigned out = 0;
    if ((mask & 0x2) && lc->GetItemState(item, wxLIST_STATE_SELECTED)) out |= 0x2;
    if ((mask & 0x1) && lc->GetItemState(item, wxLIST_STATE_FOCUSED))  out |= 0x1;
    return out;
}

static int hook_listctrl_get_selected_count(void* hwnd)
{
    wxListCtrl* lc = LC(hwnd);
    return lc ? lc->GetSelectedItemCount() : 0;
}

static void hook_listctrl_delete_item(void* hwnd, int item)
{
    if (wxListCtrl* lc = LC(hwnd)) lc->DeleteItem(item);
}

static void hook_listctrl_delete_all_items(void* hwnd)
{
    if (wxListCtrl* lc = LC(hwnd)) lc->DeleteAllItems();
}

static void hook_listctrl_ensure_visible(void* hwnd, int item)
{
    if (wxListCtrl* lc = LC(hwnd)) lc->EnsureVisible(item);
}

static void hook_listctrl_set_column_width(void* hwnd, int col, int width)
{
    if (wxListCtrl* lc = LC(hwnd)) lc->SetColumnWidth(col, width);
}

static int hook_listctrl_get_column_width(void* hwnd, int col)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc || col < 0 || col >= lc->GetColumnCount())
        return 0;
    return lc->GetColumnWidth(col);
}

// CListCtrl::SortItems(cmp, lparam): cmp compares two rows by their item data
// (the CString* the page stored).  wxListCtrl::SortItems does exactly this —
// it reorders rows (text + data move together) by a comparator over item data.
namespace {
struct ListSortCtx { int (*cmp)(intptr_t, intptr_t, intptr_t); intptr_t lparam; };
int wxCALLBACK ListSortTrampoline(wxIntPtr a, wxIntPtr b, wxIntPtr ctxp)
{
    ListSortCtx* ctx = (ListSortCtx*) ctxp;
    return ctx->cmp((intptr_t) a, (intptr_t) b, ctx->lparam);
}
}
static void hook_listctrl_sort_items(void* hwnd,
        int (*cmp)(intptr_t, intptr_t, intptr_t), intptr_t lparam)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc || !cmp) return;
    ListSortCtx ctx{cmp, lparam};
    lc->SortItems(ListSortTrampoline, (wxIntPtr) &ctx);
}

// ── CListBox → wxListBox ──────────────────────────────────────────────────
namespace { inline wxListBox* LBX(void* hwnd) { return wxDynamicCast((wxWindow*) hwnd, wxListBox); } }
static int  hook_listbox_add_string(void* hwnd, const char* text)
{
    wxListBox* lb = LBX(hwnd);
    return lb ? (int) lb->Append(wxString::FromUTF8(text ? text : "")) : LB_ERR;
}
static void hook_listbox_reset_content(void* hwnd)
{
    if (wxListBox* lb = LBX(hwnd)) lb->Clear();
}
static int  hook_listbox_get_count(void* hwnd)
{
    wxListBox* lb = LBX(hwnd);
    return lb ? (int) lb->GetCount() : 0;
}
static int  hook_listbox_get_cur_sel(void* hwnd)
{
    wxListBox* lb = LBX(hwnd);
    if (!lb) return LB_ERR;
    int sel = lb->GetSelection();
    return (sel == wxNOT_FOUND) ? LB_ERR : sel;
}
static int  hook_listbox_set_cur_sel(void* hwnd, int sel)
{
    wxListBox* lb = LBX(hwnd);
    if (!lb) return LB_ERR;
    if (sel < 0) { lb->SetSelection(wxNOT_FOUND); return LB_ERR; }
    if (sel >= (int) lb->GetCount()) return LB_ERR;
    lb->SetSelection(sel);
    return sel;
}
static char* hook_listbox_get_text(void* hwnd, int index)
{
    wxListBox* lb = LBX(hwnd);
    if (!lb || index < 0 || index >= (int) lb->GetCount()) return nullptr;
    return strdup(lb->GetString(index).utf8_str());
}
static void hook_listbox_set_item_data(void* hwnd, int index, intptr_t data)
{
    wxListBox* lb = LBX(hwnd);
    if (lb && index >= 0 && index < (int) lb->GetCount())
        lb->SetClientData(index, reinterpret_cast<void*>(data));
}
static intptr_t hook_listbox_get_item_data(void* hwnd, int index)
{
    wxListBox* lb = LBX(hwnd);
    if (lb && index >= 0 && index < (int) lb->GetCount())
        return reinterpret_cast<intptr_t>(lb->GetClientData(index));
    return 0;
}
static int hook_listbox_get_sel_count(void* hwnd)
{
    wxListBox* lb = LBX(hwnd);
    if (!lb) return 0;
    wxArrayInt sel;
    return (int) lb->GetSelections(sel);
}
static int hook_listbox_get_sel_items(void* hwnd, int maxItems, int* buffer)
{
    wxListBox* lb = LBX(hwnd);
    if (!lb || !buffer || maxItems <= 0) return 0;
    wxArrayInt sel;
    int n = (int) lb->GetSelections(sel);
    int m = (n < maxItems) ? n : maxItems;
    for (int i = 0; i < m; ++i) buffer[i] = sel[i];
    return m;
}
static int hook_listbox_get_sel(void* hwnd, int index)
{
    wxListBox* lb = LBX(hwnd);
    if (lb && index >= 0 && index < (int) lb->GetCount())
        return lb->IsSelected(index) ? 1 : 0;
    return 0;
}
static int hook_listbox_set_sel(void* hwnd, int index, int select)
{
    wxListBox* lb = LBX(hwnd);
    if (!lb || index < 0 || index >= (int) lb->GetCount()) return LB_ERR;
    if (select) lb->SetSelection(index);   // multi-select: adds without clearing others
    else        lb->Deselect(index);
    return 0;
}

// ── CComboBox → wxComboBox (editable) or wxChoice (read-only pop-up) ───────
// CBS_DROPDOWNLIST combos are built as wxChoice (native pop-up menu); editable
// combos as wxComboBox.  Both derive from wxItemContainer, which provides all
// the item operations the CComboBox hooks need, so we bridge against that base.
namespace {
inline wxItemContainer* CB(void* hwnd) {
    wxWindow* w = (wxWindow*) hwnd;
    if (wxComboBox* c = wxDynamicCast(w, wxComboBox)) return c;
    if (wxChoice*   c = wxDynamicCast(w, wxChoice))   return c;
    return nullptr;
}
}
static int hook_combo_add_string(void* hwnd, const char* text)
{
    wxItemContainer* cb = CB(hwnd);
    if (!cb) return -1;   // CB_ERR
    return (int) cb->Append(wxString::FromUTF8(text ? text : ""));
}
static int hook_combo_get_count(void* hwnd)
{
    wxItemContainer* cb = CB(hwnd);
    return cb ? (int) cb->GetCount() : 0;
}
static int hook_combo_get_cur_sel(void* hwnd)
{
    wxItemContainer* cb = CB(hwnd);
    if (!cb) return -1;
    int sel = cb->GetSelection();
    return (sel == wxNOT_FOUND) ? -1 : sel;   // CB_ERR == -1
}
static void hook_combo_set_cur_sel(void* hwnd, int sel)
{
    wxItemContainer* cb = CB(hwnd);
    if (!cb) return;
    if (sel < 0 || sel >= (int) cb->GetCount()) cb->SetSelection(wxNOT_FOUND);
    else                                        cb->SetSelection(sel);
}
static int hook_combo_find_string_exact(void* hwnd, int after, const char* text)
{
    wxItemContainer* cb = CB(hwnd);
    if (!cb) return -1;
    // MFC: search starts AFTER index `after` (-1 → from start), wraps once.
    int n = (int) cb->GetCount();
    wxString want = wxString::FromUTF8(text ? text : "");
    for (int i = 0; i < n; ++i) {
        int idx = ((after + 1 + i) % n);
        if (cb->GetString(idx).IsSameAs(want, /*caseSensitive*/false)) return idx;
    }
    return -1;
}
static char* hook_combo_get_lb_text(void* hwnd, int index)
{
    wxItemContainer* cb = CB(hwnd);
    if (!cb || index < 0 || index >= (int) cb->GetCount()) return nullptr;
    return strdup(cb->GetString(index).utf8_str());
}
static void hook_combo_set_item_data(void* hwnd, int index, intptr_t data)
{
    wxItemContainer* cb = CB(hwnd);
    if (cb && index >= 0 && index < (int) cb->GetCount())
        cb->SetClientData(index, reinterpret_cast<void*>(data));
}
static intptr_t hook_combo_get_item_data(void* hwnd, int index)
{
    wxItemContainer* cb = CB(hwnd);
    if (cb && index >= 0 && index < (int) cb->GetCount())
        return reinterpret_cast<intptr_t>(cb->GetClientData(index));
    return 0;
}
static void hook_combo_reset_content(void* hwnd)
{
    if (wxItemContainer* cb = CB(hwnd)) cb->Clear();
}

// ── Radio-button group (DDX_Radio) ────────────────────────────────────────
// Collect the group's radio buttons in child/z order: start at firstId's radio,
// then take following siblings that are radios, stopping at the next one with
// wxRB_GROUP (a new group) — mirroring MFC's "consecutive until next WS_GROUP".
namespace {
std::vector<wxRadioButton*> RadioGroup(void* dlgp, int firstId)
{
    std::vector<wxRadioButton*> group;
    wxWindow* dlg = (wxWindow*) dlgp;
    if (!dlg) return group;
    wxWindow* first = dlg->FindWindow(firstId);
    wxRadioButton* firstRb = wxDynamicCast(first, wxRadioButton);
    if (!firstRb) return group;
    // Walk the first radio's parent's children in order; collect from firstRb on.
    wxWindow* parent = firstRb->GetParent();
    if (!parent) { group.push_back(firstRb); return group; }
    bool started = false;
    for (wxWindowList::compatibility_iterator node = parent->GetChildren().GetFirst();
         node; node = node->GetNext())
    {
        wxRadioButton* rb = wxDynamicCast(node->GetData(), wxRadioButton);
        if (!started) {
            if (node->GetData() == firstRb) { started = true; group.push_back(firstRb); }
            continue;
        }
        if (!rb) continue;                                  // skip non-radios
        if (rb->HasFlag(wxRB_GROUP)) break;                 // next group starts
        group.push_back(rb);
    }
    return group;
}
}
static int hook_radio_get(void* dlg, int firstId)
{
    std::vector<wxRadioButton*> g = RadioGroup(dlg, firstId);
    for (size_t i = 0; i < g.size(); ++i)
        if (g[i]->GetValue()) return (int) i;
    return -1;
}
static void hook_radio_set(void* dlg, int firstId, int index)
{
    std::vector<wxRadioButton*> g = RadioGroup(dlg, firstId);
    if (index >= 0 && index < (int) g.size())
        g[index]->SetValue(true);
}

// ── Owner-draw buttons (BS_OWNERDRAW → canonical CButton::DrawItem) ────────
// A self-painting wx control whose paint handler runs the canonical DrawItem
// (e.g. CColourButton's colour swatch).  The backing CButton* is bound by
// DDX_Control via g_wxhook_bind_owner_draw; until then the control paints blank.
namespace {
class OwnerDrawButton : public wxControl
{
public:
    OwnerDrawButton(wxWindow* parent, wxWindowID id, const wxString& label,
                    const wxPoint& pos, const wxSize& size)
        : wxControl(parent, id, pos, size, wxBORDER_NONE)
    {
        SetLabel(label);
        Bind(wxEVT_PAINT, &OwnerDrawButton::OnPaint, this);
        // A swatch click edits the colour; route to the canonical handler
        // (BN_CLICKED) so e.g. CPrefsP5::On<colour> opens the colour picker.
        Bind(wxEVT_LEFT_UP, &OwnerDrawButton::OnClick, this);
    }
    CButton* m_cbtn = nullptr;   // canonical backing button (set by bind hook)

private:
    void OnPaint(wxPaintEvent&)
    {
        wxPaintDC pdc(this);
        wxSize sz = GetClientSize();
        if (sz.x <= 0 || sz.y <= 0) return;

        if (!m_cbtn) {                       // not yet bound: neutral fill
            pdc.SetBackground(wxBrush(GetBackgroundColour()));
            pdc.Clear();
            return;
        }

        // Paint via the canonical CButton::DrawItem.  The CDC work lives in
        // wx_gdi_compat.cpp (the only TU built with MUSHCLIENT_FULL_BUILD → the
        // real wx-backed CDC; here we see the stub CDC whose CreateCompatibleDC
        // is a no-op, which is why the helper can't live in this file).
        wxBitmap* painted = PaintOwnerDrawButton(m_cbtn, sz.x, sz.y, !IsThisEnabled());
        if (painted) {
            pdc.DrawBitmap(*painted, 0, 0, false);
            delete painted;
        }
    }

    void OnClick(wxMouseEvent& e)
    {
        // A BN_CLICKED from a child routes to the PARENT dialog's message map
        // (e.g. CPrefsP5::OnRedSwatch), not the button's.  Emit a wxEVT_BUTTON
        // command that bubbles up to the panel's dispatch binding (which calls
        // AfxDispatchCommand on the page), matching how real buttons are routed.
        wxCommandEvent evt(wxEVT_BUTTON, GetId());
        evt.SetEventObject(this);
        wxPostEvent(GetParent(), evt);
        e.Skip();
    }
};
} // namespace

static void* hook_make_owner_draw_button(void* parent, int id, const char* text,
                                         int x, int y, int w, int h)
{
    return new OwnerDrawButton((wxWindow*) parent,
                               id < 0 ? wxID_ANY : id,
                               wxString::FromUTF8(text ? text : ""),
                               wxPoint(x, y), wxSize(w, h));
}

static void hook_bind_owner_draw(void* hwnd, void* cbtn)
{
    if (OwnerDrawButton* odb = dynamic_cast<OwnerDrawButton*>((wxWindow*) hwnd)) {
        odb->m_cbtn = static_cast<CButton*>(cbtn);
        odb->Refresh();   // repaint now that the backing button is available
        // The swatch colour (CColourButton::m_colour) is usually assigned in the
        // page's OnInitDialog, which runs AFTER this DDX_Control bind.  Without a
        // later repaint the swatch keeps the default (black) it was painted with
        // here.  Queue a repaint for the next event-loop pass — by then
        // OnInitDialog has set the colour.  (If the control is destroyed first,
        // wxEvtHandler removes the pending call, so this is safe.)
        odb->CallAfter([odb]{ odb->Refresh(); });
    }
}

// ── CSliderCtrl → wxSlider ────────────────────────────────────────────────
namespace {
inline wxSlider* SL(void* hwnd) { return wxDynamicCast((wxWindow*) hwnd, wxSlider); }
}
static void hook_slider_set_range(void* hwnd, int lo, int hi)
{
    if (wxSlider* s = SL(hwnd)) s->SetRange(lo, hi);
}
static int hook_slider_get_pos(void* hwnd)
{
    wxSlider* s = SL(hwnd);
    return s ? s->GetValue() : 0;
}
static void hook_slider_set_pos(void* hwnd, int pos)
{
    if (wxSlider* s = SL(hwnd)) s->SetValue(pos);
}
static void hook_slider_set_page_size(void* hwnd, int n)
{
    if (wxSlider* s = SL(hwnd)) s->SetPageSize(n > 0 ? n : 1);
}
// Bind a slider's move event to the owning dialog's canonical OnHScroll (the
// ON_WM_HSCROLL handler that recomputes derived state, e.g. the colour swatch).
static void hook_bind_slider(void* hwnd, void* dlgp)
{
    wxSlider* s = SL(hwnd);
    if (!s || !dlgp) return;
    CDialog* dlg = static_cast<CDialog*>(dlgp);
    s->Bind(wxEVT_SLIDER, [dlg, hwnd](wxCommandEvent& e) {
        // MFC OnHScroll(nSBCode, nPos, pScrollBar): pScrollBar is the moved
        // control as a CScrollBar* — OnHScroll re-reads each slider's GetPos, so
        // the CWnd wrapper for this slider's HWND suffices.
        dlg->OnHScroll(SB_THUMBTRACK, (UINT) e.GetInt(),
                       (CScrollBar*) CWnd::FromHandle((HWND) hwnd));
        e.Skip();
    });
}

// ── CSpinButtonCtrl → wxSpinButton (msctls_updown32) ──────────────────────────
// A spin button nudges its "buddy" edit (its preceding control, UDS_AUTOBUDDY)
// up/down within [lo,hi]; the buddy holds the value the dialog's DDX reads.  The
// two are kept in sync both ways (type into the edit → spin; click spin → edit).
extern WORD WxKeyToWinVK(int wxCode);   // defined in wx_world_child.cpp

namespace {
inline int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

class WxSpinButton : public wxSpinButton {
public:
    WxSpinButton(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size)
        : wxSpinButton(parent, id, pos, size, wxSP_VERTICAL | wxSP_ARROW_KEYS)
    {
        SetRange(0, 100);   // overwritten by the page's SetRange() in OnInitDialog
        Bind(wxEVT_SPIN, &WxSpinButton::OnSpin, this);
    }
    void SetBuddy(wxTextCtrl* b)
    {
        m_buddy = b;
        if (m_buddy) m_buddy->Bind(wxEVT_TEXT, &WxSpinButton::OnBuddyText, this);
        SyncFromBuddy();
    }
    void SetLogicalRange(int lo, int hi) { if (hi < lo) hi = lo; SetRange(lo, hi); SyncFromBuddy(); }
    int  LogicalPos() const
    {
        if (m_buddy) { long v = 0; if (m_buddy->GetValue().ToLong(&v)) return (int) v; }
        return GetValue();
    }
    void SetLogicalPos(int p)
    {
        p = clamp_int(p, GetMin(), GetMax());
        SetValue(p);
        if (m_buddy) m_buddy->ChangeValue(wxString::Format(wxT("%d"), p));
    }
private:
    wxTextCtrl* m_buddy = nullptr;
    void SyncFromBuddy()
    {
        if (!m_buddy) return;
        long v = 0;
        if (m_buddy->GetValue().ToLong(&v)) SetValue(clamp_int((int) v, GetMin(), GetMax()));
    }
    void OnBuddyText(wxCommandEvent& e) { SyncFromBuddy(); e.Skip(); }
    void OnSpin(wxSpinEvent& e)
    {
        int v = clamp_int(GetValue(), GetMin(), GetMax());   // NSStepper already moved ±1
        SetValue(v);
        if (m_buddy) m_buddy->ChangeValue(wxString::Format(wxT("%d"), v));
        e.Skip();
    }
};
inline WxSpinButton* SPN(void* h) { return dynamic_cast<WxSpinButton*>((wxWindow*) h); }
} // namespace

static void* hook_make_spin(void* parent, int id, int x, int y, int w, int h)
{
    return new WxSpinButton((wxWindow*) parent, id < 0 ? wxID_ANY : id,
                            wxPoint(x, y), wxSize(w, h));
}
static void hook_spin_set_range(void* h, int lo, int hi) { if (WxSpinButton* s = SPN(h)) s->SetLogicalRange(lo, hi); }
static void hook_spin_set_pos(void* h, int p)            { if (WxSpinButton* s = SPN(h)) s->SetLogicalPos(p); }
static int  hook_spin_get_pos(void* h)                   { WxSpinButton* s = SPN(h); return s ? s->LogicalPos() : 0; }
static void hook_bind_spin(void* spin, void* buddy)
{
    if (WxSpinButton* s = SPN(spin))
        s->SetBuddy(wxDynamicCast((wxWindow*) buddy, wxTextCtrl));
}

// ── CHotKeyCtrl → key-capture control (msctls_hotkey32, the Key Name dialog) ──
// Captures one key + Shift/Ctrl/Alt, shows the chord via the canonical
// KeyCodeToString, and remembers the Windows VK / HOTKEYF_* so GetHotKey() (read
// on idle by CKeyNameDlg::OnUpdateKeyName) can fill the dialog's "Name" field.
namespace {
class WxHotKeyCtrl : public wxTextCtrl {
public:
    WxHotKeyCtrl(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size)
        : wxTextCtrl(parent, id, wxEmptyString, pos, size,
                     wxTE_READONLY | wxTE_CENTRE | wxTE_PROCESS_TAB)
    {
        Bind(wxEVT_KEY_DOWN, &WxHotKeyCtrl::OnKey, this);
    }
    WORD m_vk = 0, m_mod = 0;   // m_mod holds HOTKEYF_* flags
private:
    void OnKey(wxKeyEvent& e)
    {
        int code = e.GetKeyCode();
        if (code == WXK_SHIFT || code == WXK_CONTROL ||
            code == WXK_ALT || code == WXK_NONE) { e.Skip(); return; }  // bare modifier
        WORD vk = WxKeyToWinVK(code);
        if (vk == 0) { e.Skip(); return; }    // a key MUSHclient cannot name
        WORD mod = 0;
        if (e.ShiftDown())                         mod |= HOTKEYF_SHIFT;
        if (e.ControlDown() || e.RawControlDown()) mod |= HOTKEYF_CONTROL;   // Cmd or Ctrl
        if (e.AltDown())                           mod |= HOTKEYF_ALT;
        m_vk = vk; m_mod = mod;
        BYTE fVirt = 0;
        if (mod & HOTKEYF_SHIFT)   fVirt |= FSHIFT;
        if (mod & HOTKEYF_CONTROL) fVirt |= FCONTROL;
        if (mod & HOTKEYF_ALT)     fVirt |= FALT;
        ChangeValue(wxString::FromUTF8((LPCTSTR) KeyCodeToString(fVirt, vk)));
        // consume (no Skip) so the key isn't beeped or forwarded
    }
};
inline WxHotKeyCtrl* HK(void* h) { return dynamic_cast<WxHotKeyCtrl*>((wxWindow*) h); }
} // namespace

static void* hook_make_hotkey(void* parent, int id, int x, int y, int w, int h)
{
    return new WxHotKeyCtrl((wxWindow*) parent, id < 0 ? wxID_ANY : id,
                            wxPoint(x, y), wxSize(w, h));
}
static void hook_hotkey_get(void* hwnd, unsigned short* vk, unsigned short* mod)
{
    WxHotKeyCtrl* c = HK(hwnd);
    if (vk)  *vk  = c ? c->m_vk : 0;
    if (mod) *mod = c ? c->m_mod : 0;
}

// ── CWnd geometry: screen rect + client/screen point conversion ───────────────
static void hook_get_window_rect(const void* hwnd, RECT* r)
{
    if (!r) return;
    wxWindow* w = (wxWindow*) hwnd;
    if (!w) { r->left = r->top = r->right = r->bottom = 0; return; }
    wxRect g = w->GetScreenRect();
    r->left = g.x; r->top = g.y; r->right = g.x + g.width; r->bottom = g.y + g.height;
}
// rcNormalPosition for GetWindowPlacement.  wxWindow::GetRect() is exactly the
// Windows split: parent-client coordinates for a child, screen coordinates for
// a top-level window.  Reporting the client box here instead (what this used to
// do) pins every control's left/top at 0, which silently collapsed layouts that
// position one control relative to another -- CEditMultiLine::OnSize sized its
// text box to `Cancel.top - 10`, i.e. -10, so the "edit send text" dialog came
// up with no visible text area at all.
static void hook_get_placement_rect(const void* h, RECT* r)
{
    if (!r) return;
    if (auto* w = toWxC(h)) {
        wxRect g = w->GetRect();
        r->left = g.x; r->top = g.y; r->right = g.x + g.width; r->bottom = g.y + g.height;
    } else {
        r->left = r->top = r->right = r->bottom = 0;
    }
}
static void hook_client_to_screen(const void* hwnd, int* x, int* y)
{
    wxWindow* w = (wxWindow*) hwnd;
    if (!w || !x || !y) return;
    wxPoint p = w->ClientToScreen(wxPoint(*x, *y));
    *x = p.x; *y = p.y;
}
static void hook_screen_to_client(const void* hwnd, int* x, int* y)
{
    wxWindow* w = (wxWindow*) hwnd;
    if (!w || !x || !y) return;
    wxPoint p = w->ScreenToClient(wxPoint(*x, *y));
    *x = p.x; *y = p.y;
}

void wx_mfc_install_hooks()
{
    g_wxhook_progress_open      = hook_progress_open;
    g_wxhook_progress_set_range = hook_progress_set_range;
    g_wxhook_progress_update    = hook_progress_update;
    g_wxhook_progress_close     = hook_progress_close;
    g_wxhook_progress_set_title = hook_progress_set_title;
    g_run_mfc_dialog          = hook_run_dialog;
    g_end_mfc_dialog          = hook_end_dialog;
    g_run_mfc_property_sheet  = hook_run_property_sheet;
    g_run_mfc_property_sheet_flat = hook_run_property_sheet_flat;
    g_wxhook_get_dlg_item     = hook_get_dlg_item;
    g_wxhook_subclass_dlg_item = hook_subclass_dlg_item;
    g_wxhook_get_window_text  = hook_get_window_text;
    g_wxhook_button_set_check = hook_button_set_check;
    g_wxhook_button_get_check = hook_button_get_check;
    g_wxhook_enable_window    = hook_enable_window;
    g_wxhook_listctrl_insert_column   = hook_listctrl_insert_column;
    g_wxhook_listctrl_insert_item     = hook_listctrl_insert_item;
    g_wxhook_listctrl_set_item_text   = hook_listctrl_set_item_text;
    g_wxhook_listctrl_get_item_count  = hook_listctrl_get_item_count;
    g_wxhook_listctrl_set_item_data   = hook_listctrl_set_item_data;
    g_wxhook_listctrl_get_item_data   = hook_listctrl_get_item_data;
    g_wxhook_listctrl_get_item_text   = hook_listctrl_get_item_text;
    g_wxhook_listctrl_get_next_item   = hook_listctrl_get_next_item;
    g_wxhook_listctrl_set_item_state  = hook_listctrl_set_item_state;
    g_wxhook_listctrl_get_item_state  = hook_listctrl_get_item_state;
    g_wxhook_listctrl_get_selected_count = hook_listctrl_get_selected_count;
    g_wxhook_listctrl_delete_item     = hook_listctrl_delete_item;
    g_wxhook_listctrl_delete_all_items = hook_listctrl_delete_all_items;
    g_wxhook_listctrl_ensure_visible  = hook_listctrl_ensure_visible;
    g_wxhook_listctrl_set_column_width = hook_listctrl_set_column_width;
    g_wxhook_listctrl_get_column_width = hook_listctrl_get_column_width;
    g_wxhook_listctrl_sort_items      = hook_listctrl_sort_items;
    g_wxhook_listbox_add_string       = hook_listbox_add_string;
    g_wxhook_listbox_reset_content    = hook_listbox_reset_content;
    g_wxhook_listbox_get_count        = hook_listbox_get_count;
    g_wxhook_listbox_get_cur_sel      = hook_listbox_get_cur_sel;
    g_wxhook_listbox_set_cur_sel      = hook_listbox_set_cur_sel;
    g_wxhook_listbox_get_text         = hook_listbox_get_text;
    g_wxhook_listbox_set_item_data    = hook_listbox_set_item_data;
    g_wxhook_listbox_get_item_data    = hook_listbox_get_item_data;
    g_wxhook_listbox_get_sel_count    = hook_listbox_get_sel_count;
    g_wxhook_listbox_get_sel_items    = hook_listbox_get_sel_items;
    g_wxhook_listbox_get_sel          = hook_listbox_get_sel;
    g_wxhook_listbox_set_sel          = hook_listbox_set_sel;
    g_wxhook_combo_add_string         = hook_combo_add_string;
    g_wxhook_combo_get_count          = hook_combo_get_count;
    g_wxhook_combo_get_cur_sel        = hook_combo_get_cur_sel;
    g_wxhook_combo_set_cur_sel        = hook_combo_set_cur_sel;
    g_wxhook_combo_find_string_exact  = hook_combo_find_string_exact;
    g_wxhook_combo_get_lb_text        = hook_combo_get_lb_text;
    g_wxhook_combo_reset_content      = hook_combo_reset_content;
    g_wxhook_combo_set_item_data      = hook_combo_set_item_data;
    g_wxhook_combo_get_item_data      = hook_combo_get_item_data;
    g_wxhook_radio_get                = hook_radio_get;
    g_wxhook_radio_set                = hook_radio_set;
    g_wxhook_make_owner_draw_button   = hook_make_owner_draw_button;
    g_wxhook_bind_owner_draw          = hook_bind_owner_draw;
    g_wxhook_slider_set_range         = hook_slider_set_range;
    g_wxhook_slider_get_pos           = hook_slider_get_pos;
    g_wxhook_slider_set_pos           = hook_slider_set_pos;
    g_wxhook_slider_set_page_size     = hook_slider_set_page_size;
    g_wxhook_bind_slider              = hook_bind_slider;
    g_wxhook_make_spin                = hook_make_spin;
    g_wxhook_spin_set_range           = hook_spin_set_range;
    g_wxhook_spin_set_pos             = hook_spin_set_pos;
    g_wxhook_spin_get_pos             = hook_spin_get_pos;
    g_wxhook_bind_spin                = hook_bind_spin;
    g_wxhook_make_hotkey              = hook_make_hotkey;
    g_wxhook_hotkey_get               = hook_hotkey_get;
    g_wxhook_get_window_rect          = hook_get_window_rect;
    g_wxhook_get_placement_rect       = hook_get_placement_rect;
    g_wxhook_client_to_screen         = hook_client_to_screen;
    g_wxhook_screen_to_client         = hook_screen_to_client;
    g_wxhook_invalidate       = hook_invalidate;
    g_wxhook_show_window      = hook_show_window;
    g_wxhook_update_window    = hook_update_window;
    g_wxhook_get_client_rect  = hook_get_client_rect;
    g_wxhook_set_window_text  = hook_set_window_text;
    g_wxhook_move_window      = hook_move_window;
    g_wxhook_destroy_window   = hook_destroy_window;
    g_wxhook_set_focus        = hook_set_focus;
    g_wxhook_send_message     = hook_send_message;
    g_wxhook_post_message     = hook_post_message;
    g_wxhook_message_box      = hook_message_box;
    g_wxhook_load_mdi_frame   = hook_load_mdi_frame;
    g_wxhook_init_instance    = hook_init_instance;
    g_wxhook_new_world          = hook_new_world;
    g_wxhook_show_activity_window = []() { ShowActivityWindow(); };
    g_wxhook_open_world_file    = hook_open_world_file;
    g_wxhook_open_default_world = hook_open_default_world;
    g_wxhook_query_quit_save    = hook_query_quit_save;
    g_wxhook_report_mfc_exception = hook_report_mfc_exception;
    g_wxhook_set_status         = hook_set_status;
    g_wxhook_save_world         = hook_save_world;
    g_wxhook_file_dialog        = hook_file_dialog;
    g_wxhook_font_dialog        = hook_font_dialog;
    g_wxhook_get_clipboard_text   = hook_get_clipboard_text;
    g_wxhook_set_clipboard_text   = hook_set_clipboard_text;
    g_wxhook_create_text_window   = hook_create_text_window;
    g_wxhook_open_text_file       = hook_open_text_file;
    g_wxhook_pick_colour          = hook_pick_colour;
    g_wxhook_menu_dialog          = hook_menu_dialog;
    g_wxhook_flash_icon           = hook_flash_icon;
    g_wxhook_move_main_window     = hook_move_main_window;
    g_wxhook_inputbox             = hook_inputbox;
    g_wxhook_directorypicker      = hook_directorypicker;
    g_wxhook_password_dialog      = hook_password_dialog;
    g_wxhook_popup_menu           = hook_popup_menu;
    g_wxhook_output_client_to_screen = hook_output_client_to_screen;
    // Sound is the miniaudio engine (platform/ma_sound.*), not wxSound, so these
    // are always installed (no wxUSE_SOUND gate).
    g_wxhook_play_sound           = hook_play_sound;
    g_wxhook_stop_sound           = hook_stop_sound;
    g_wxhook_sound_play           = hook_sound_play;
    g_wxhook_sound_play_memory    = hook_sound_play_memory;
    g_wxhook_sound_adjust         = hook_sound_adjust;
    g_wxhook_sound_stop_buffer    = hook_sound_stop_buffer;
    g_wxhook_sound_status         = hook_sound_status;
    g_wxhook_load_text_resource   = hook_load_text_resource;
    g_wxhook_get_font_families    = hook_get_font_families;
    g_wxhook_font_has_glyph       = hook_font_has_glyph;
    g_wxhook_get_sys_color        = hook_get_sys_color;
    g_wxhook_get_system_metric    = hook_get_system_metric;
    g_wxhook_get_key_state        = hook_get_key_state;
    g_wxhook_edit_get_sel         = hook_edit_get_sel;
    g_wxhook_edit_set_sel         = hook_edit_set_sel;
    g_wxhook_edit_replace_sel     = hook_edit_replace_sel;
    g_wxhook_edit_line_info       = hook_edit_line_info;
    g_wxhook_set_font             = hook_set_font;
    g_wxhook_get_pixel            = wxgdi_get_pixel;   // defined in wx_gdi_compat.cpp
    g_wxhook_set_cursor           = hook_set_cursor;
    g_wxhook_set_main_background  = hook_set_main_background;
    // The eleven notepad hooks are gone — see "Notepad hooks: REMOVED" above.

    // Wire the system-beep hook so doc.cpp's MessageBeep() calls produce
    // an audible alert on macOS/Linux via wxWidgets.
    g_fn_system_beep = []() { wxBell(); };
}

#endif // !_WIN32

// ── utils.inputbox / utils.editbox ────────────────────────────────────────
// Shows a simple text entry dialog.  Returns heap-allocated string (caller
// frees) or nullptr on cancel.
// NOTE: appended after the end of the file by the port script; the
// wx_mfc_install_hooks() call below must remain LAST so these static
// functions are visible to it.
