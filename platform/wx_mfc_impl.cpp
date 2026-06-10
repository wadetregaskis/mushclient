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
#include <wx/filename.h>
#include <wx/fontpicker.h>
#include <wx/dnd.h>        // wxFileDropTarget
#include <wx/dirdlg.h>     // wxDirDialog
#include <wx/notebook.h>   // wxNotebook (flat property-sheet bridge: hook_run_property_sheet_flat)
#include <wx/clipbrd.h>    // wxTheClipboard
#include <wx/dataobj.h>    // wxTextDataObject
#include <wx/colordlg.h>   // wxColourDialog
#include <wx/fontdlg.h>    // wxFontDialog, wxFontData
#include <wx/treebook.h>   // wxTreebook (World Properties property sheet)
#include <wx/listctrl.h>   // wxListCtrl (entity-manager object lists)
#include <wx/stdpaths.h>   // wxStandardPaths (bundled text resources)
#include <wx/fontenum.h>   // wxFontEnumerator (utils.getfontfamilies)
#include <wx/settings.h>   // wxSystemSettings (world.GetSysColor / GetSystemMetrics)
#include <wx/display.h>    // wxDisplay (screen-size system metrics)
#include <algorithm>
#include <unordered_map>
#include <map>
#include <string>
#include <fstream>         // bundled "TEXT" resources (licence / credits)
#include <sstream>
#include <vector>

// ── MUSHclient headers (MFC compat layer) ────────────────────────────────
// Must come AFTER wx so our stubs don't shadow wx internals inside this TU.
#include "stdafx.h"        // pulls in compat.h → mfc_ui.h → wx_mfc_hooks.h
#include "MUSHclient.h"    // CMUSHclientApp full definition
#include "mainfrm.h"       // CMainFrame full definition
#include "doc.h"           // CMUSHclientDoc (needed for hook_new_world)
#include "TextDocument.h"             // CTextDocument (for hook_create_text_window)
#include "platform/wx_world_child.h"  // CWorldChildFrame
#include "platform/wx_osx_text.h"     // wxOSXDisableSmartSubstitutions (macOS smart-quote fix)
#include "platform/wx_activity.h"     // ShowActivityWindow (Game ▸ Activity List)
#include "platform/ma_sound.h"        // miniaudio sound engine (volume/pan/buffers)
#include "platform/wx_notepad.h"      // CWxNotepadFrame
#include "platform/wx_dialog_bridge.h" // MfcDialogResources, BuildWxDialogFromTemplate
#include "dialogs/world_prefs/TreePropertySheet.h" // port CTreePropertySheet (page list)
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
// into both mushcore and mushapp) so that mushcli and tests can link without
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
static void world_list_remove(const std::string& path);

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
static const int ID_MENU_VIEW_ALWAYSONTOP = wxID_HIGHEST + 420;
static const int ID_MENU_VIEW_FULLSCREEN  = wxID_HIGHEST + 421;
static const int ID_MENU_VIEW_STATUSBAR   = wxID_HIGHEST + 422;

static wxMenu*    s_recentMenu     = nullptr;
static std::vector<std::string> s_recentWorlds;

// Load the recent list from config into s_recentWorlds.
static void recent_worlds_load()
{
    s_recentWorlds.clear();
    wxConfig cfg(wxT("mushclient"));
    for (int i = 0; i < kMaxRecentWorlds; ++i) {
        wxString key = wxString::Format(wxT("/recent/%d"), i);
        wxString val;
        if (!cfg.Read(key, &val) || val.IsEmpty()) break;
        s_recentWorlds.push_back(std::string(val.ToUTF8()));
    }
}

// Persist the current s_recentWorlds to config.
static void recent_worlds_save()
{
    wxConfig cfg(wxT("mushclient"));
    for (int i = 0; i < kMaxRecentWorlds; ++i) {
        wxString key = wxString::Format(wxT("/recent/%d"), i);
        if (i < (int)s_recentWorlds.size())
            cfg.Write(key, wxString::FromUTF8(s_recentWorlds[i].c_str()));
        else
            cfg.DeleteEntry(key);
    }
}

// Rebuild s_recentMenu from s_recentWorlds.
static void recent_worlds_update_menu()
{
    if (!s_recentMenu) return;
    while (s_recentMenu->GetMenuItemCount() > 0)
        s_recentMenu->Delete(s_recentMenu->FindItemByPosition(0));
    if (s_recentWorlds.empty()) {
        wxMenuItem* dummy = s_recentMenu->Append(kRecentMenuBase,
                                                  wxT("(no recent worlds)"));
        dummy->Enable(false);
        return;
    }
    for (int i = 0; i < (int)s_recentWorlds.size(); ++i) {
        wxString path  = wxString::FromUTF8(s_recentWorlds[i].c_str());
        wxString label = wxString::Format(wxT("&%d  %s"),
                                          (i + 1) % 10,
                                          wxFileName(path).GetFullName());
        s_recentMenu->Append(kRecentMenuBase + i, label);
    }
}

// Prepend path to s_recentWorlds (deduplicated), persist, and update the menu.
static void recent_worlds_add(const std::string& path)
{
    if (path.empty()) return;
    s_recentWorlds.erase(
        std::remove(s_recentWorlds.begin(), s_recentWorlds.end(), path),
        s_recentWorlds.end());
    s_recentWorlds.insert(s_recentWorlds.begin(), path);
    if ((int)s_recentWorlds.size() > kMaxRecentWorlds)
        s_recentWorlds.resize(kMaxRecentWorlds);
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

void wx_world_remove_from_list(const char* path)
{
    if (path && *path) world_list_remove(path);
}

// ── Helper: cast void* → wxWindow* ───────────────────────────────────────
static inline wxWindow* toWx(void* h) { return static_cast<wxWindow*>(h); }
static inline const wxWindow* toWxC(const void* h) { return static_cast<const wxWindow*>(h); }

// ── Helper: apply world font settings to an output pane ──────────────────
// ── World-list persistence ────────────────────────────────────────────────
// App.m_strWorldList is a '*'-delimited list of world file paths that are
// opened automatically on startup.  These helpers keep it up-to-date and
// flush changes to the preferences SQLite DB so they survive app restarts.

static void world_list_add(const std::string& path)
{
    if (path.empty()) return;

    // Parse the current list; skip if already present.
    std::string cur = static_cast<const char*>(App.m_strWorldList);
    if (!cur.empty()) {
        // Check for exact match of any '*'-separated entry.
        std::string check = cur;
        while (!check.empty()) {
            size_t sep = check.find('*');
            std::string entry = (sep == std::string::npos) ? check : check.substr(0, sep);
            if (entry == path) return;  // already in list
            if (sep == std::string::npos) break;
            check = check.substr(sep + 1);
        }
        cur += '*';
    }
    cur += path;
    App.m_strWorldList = CString(cur.c_str());
    App.db_write_string("prefs", "WorldList",
                        static_cast<const char*>(App.m_strWorldList));
}

static void world_list_remove(const std::string& path)
{
    if (path.empty()) return;

    std::string cur = static_cast<const char*>(App.m_strWorldList);
    if (cur.empty()) return;

    // Rebuild the '*'-delimited list, skipping the given path.
    std::string rebuilt;
    std::string check = cur;
    while (!check.empty()) {
        size_t sep = check.find('*');
        std::string entry = (sep == std::string::npos) ? check : check.substr(0, sep);
        if (entry != path) {
            if (!rebuilt.empty()) rebuilt += '*';
            rebuilt += entry;
        }
        if (sep == std::string::npos) break;
        check = check.substr(sep + 1);
    }
    if (rebuilt == cur) return;  // nothing changed
    App.m_strWorldList = CString(rebuilt.c_str());
    App.db_write_string("prefs", "WorldList",
                        static_cast<const char*>(App.m_strWorldList));
}

// ── Hook implementations ──────────────────────────────────────────────────

static void hook_invalidate(void* h, bool erase)
{
    if (auto* w = toWx(h)) w->Refresh(erase);
}

static void hook_show_window(void* h, int nCmd)
{
    if (auto* w = toWx(h)) {
        // SW_SHOW / SW_SHOWNORMAL → Show; SW_HIDE → Hide
        w->Show(nCmd != 0 /* SW_HIDE == 0 */);
        // SW_MAXIMIZE
        if (nCmd == 3 /*SW_MAXIMIZE*/)
            if (auto* tl = wxDynamicCast(w, wxTopLevelWindow))
                tl->Maximize();
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
                if (wxMessageBox(prompt, wxT("TLS Certificate Not Verified"),
                                 wxYES_NO | wxICON_EXCLAMATION, parent) == wxYES)
                {
                    doc->m_bSSLAllowInvalidCert = true;
                    doc->ConnectSocket();
                }
            } else {
                // Handshake failed: offer an unencrypted fallback connection.
                wxString prompt = wxString::Format(
                    wxT("TLS connection to \"%s\" failed.\n\n%s\n\n"
                        "Connect without encryption?"),
                    name, err);
                if (wxMessageBox(prompt, wxT("TLS Connection Failed"),
                                 wxYES_NO | wxICON_QUESTION, parent) == wxYES)
                {
                    doc->m_bUseSSL = false;
                    doc->ConnectSocket();
                }
            }
        });
        return TRUE;
    }

    (void)lp;
    return FALSE;
}

static int hook_message_box(const char* text, const char* caption, unsigned flags)
{
    // Map common MB_* flags to wxMessageBox styles.
    long style = wxOK;
    if (flags & 0x0001 /*MB_OKCANCEL*/)   style = wxOK | wxCANCEL;
    if (flags & 0x0002 /*MB_ABORTRETRYIGNORE*/) style = wxOK; // approximation
    if ((flags & 0x0003 /*MB_YESNOCANCEL*/) ||
        (flags & 0x0004 /*MB_YESNO*/))    style = wxYES_NO;
    if (flags & 0x0010 /*MB_ICONHAND*/)   style |= wxICON_ERROR;
    if (flags & 0x0020 /*MB_ICONQUESTION*/) style |= wxICON_QUESTION;
    if (flags & 0x0030 /*MB_ICONEXCLAMATION*/) style |= wxICON_WARNING;
    if (flags & 0x0040 /*MB_ICONINFORMATION*/) style |= wxICON_INFORMATION;

    wxString wxText    = text    ? wxString::FromUTF8(text)    : wxString();
    wxString wxCaption = caption ? wxString::FromUTF8(caption) : wxString("MUSHclient");

    int res = wxMessageBox(wxText, wxCaption, (int)style);

    // Map wx result to MB_* return codes.
    switch (res) {
    case wxOK:     return IDOK;
    case wxCANCEL: return IDCANCEL;
    case wxYES:    return IDYES;
    case wxNO:     return IDNO;
    default:       return IDOK;
    }
}

static void hook_set_status(const char* msg)
{
    if (s_statusBar)
        s_statusBar->SetStatusText(msg ? wxString::FromUTF8(msg) : wxString(), 0);
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
  { SEC_GAME, 32252, "&Auto Say",                            'D', true,  true  }, // ID_GAME_AUTOSAY
  { SEC_GAME, 32216, "Send &File\xE2\x80\xA6",               'D', false, true  }, // ID_GAME_PASTEFILE
  { SEC_GAME, 32956, "&Discard Queued Commands",             'D', false, true  }, // ID_INPUT_DISCARDQUEUEDCOMMANDS
  { SEC_GAME, 32316, "Reset All Timers",                     'D', false, true  }, // ID_GAME_RESETALLTIMERS
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
    {
        wxConfig cfg(wxT("mushclient"));
        long w = 1024, h = 768;
        long x = -1, y = -1;    // -1 = not saved; keep platform default
        bool maximized = false;
        cfg.Read(wxT("/geometry/main_w"), &w, 1024L);
        cfg.Read(wxT("/geometry/main_h"), &h,  768L);
        cfg.Read(wxT("/geometry/main_x"), &x, -1L);
        cfg.Read(wxT("/geometry/main_y"), &y, -1L);
        cfg.Read(wxT("/geometry/main_maximized"), &maximized, false);
        // Clamp to sanity: don't restore a 1×1 or off-screen window.
        if (w < 400) w = 1024;
        if (h < 300) h =  768;
        if (x >= 0 && y >= 0)
            frame->SetSize((int)x, (int)y, (int)w, (int)h);
        else
            frame->SetSize((int)w, (int)h);
        if (maximized)
            frame->Maximize(true);
    }

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
    fileMenu->AppendSeparator();
    fileMenu->Append(32246 /*ID_FILE_PREFERENCES*/, wxT("Preferences…"));
    append_canon_section(fileMenu, SEC_FILE);   // Import, Log Session, Open Startup, Reload Defaults
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT,   wxT("Quit MUSHclient\tCtrl+Q"));
    mb->Append(fileMenu, wxT("&File"));

    // ── Edit menu ─────────────────────────────────────────────────────────
    // Standard IDs (wxID_COPY etc.) are routed automatically by wxWidgets to
    // the focused window; no explicit handler is needed in our menu dispatcher.
    wxMenu* editMenu = new wxMenu();
    editMenu->Append(wxID_COPY,      wxT("Copy\tCtrl+C"));
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
    append_canon_section(editMenu, SEC_EDIT);   // Paste To World, Reload Names, Unique ID, Colour Picker, …
    editMenu->Append(32974 /*ID_EDIT_COPYASHTML*/, wxT("Copy as &HTML\tCtrl+Alt+C"));
    mb->Append(editMenu, wxT("&Edit"));

    wxMenu* connMenu = new wxMenu();
    // Note: Ctrl+K, Ctrl+D, and Ctrl+R are standard Unix input-bar shortcuts
    // (kill-to-end, delete-forward, and reverse history search respectively) so
    // those keys are not used as menu accelerators here.
    connMenu->Append(32289,                     wxT("Quick Connect…\tCtrl+Shift+K"));
    connMenu->AppendSeparator();
    connMenu->Append(32239 /*ID_CONNECTION_CONNECT*/,    wxT("Connect"));
    connMenu->Append(0xE200,                    wxT("Disconnect"));
    connMenu->Append(0xE201,                    wxT("Reconnect\tCtrl+Shift+R"));
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
    mb->Append(gameMenu, wxT("&Game"));

    // ── Display menu (output-pane view commands) ──────────────────────────
    // No Ctrl+F accelerator here — the inline find bar already binds it.
    wxMenu* displayMenu = new wxMenu();
    displayMenu->Append(32248 /*ID_DISPLAY_FIND*/,         wxT("&Find…"));
    displayMenu->Append(32249 /*ID_DISPLAY_FINDAGAIN*/,    wxT("Find &Again"));
    displayMenu->AppendSeparator();
    displayMenu->Append(32201 /*ID_DISPLAY_FREEZEOUTPUT*/, wxT("Free&ze Output"));
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
    // Output-selection commands (reimplemented on CWorldChildFrame).
    displayMenu->AppendSeparator();
    displayMenu->Append(32275 /*ID_DISPLAY_GOTOURL*/,         wxT("Go To &URL\tCtrl+Alt+J"));
    displayMenu->Append(32276 /*ID_DISPLAY_SENDMAILTO*/,      wxT("Send &Mail To…"));
    displayMenu->Append(33036 /*ID_DISPLAY_HIGHLIGHTPHRASE*/, wxT("&Highlight Word…\tCtrl+Alt+H"));
    displayMenu->Append(33044 /*ID_DISPLAY_MULTILINETRIGGER*/,wxT("&Multi-Line Trigger…"));
    mb->Append(displayMenu, wxT("&Display"));

    // ── View menu (port-local; real wx implementations, not the canonical
    // SetWindowPos/ModifyStyle paths which are Windows-only stubs) ─────────
    wxMenu* viewMenu = new wxMenu();
    viewMenu->AppendCheckItem(ID_MENU_VIEW_ALWAYSONTOP, wxT("Always On &Top"));
    viewMenu->AppendCheckItem(ID_MENU_VIEW_FULLSCREEN,  wxT("&Full Screen\tCtrl+Alt+F"));
    viewMenu->AppendSeparator();
    viewMenu->AppendCheckItem(ID_MENU_VIEW_STATUSBAR,   wxT("&Status Bar"));
    mb->Append(viewMenu, wxT("&View"));

    // ── Configure menu ────────────────────────────────────────────────────
    wxMenu* cfgMenu = new wxMenu();
    cfgMenu->Append(ID_MENU_CFG_TRIGGERS,  wxT("&Triggers…\tCtrl+T"));
    cfgMenu->Append(ID_MENU_CFG_ALIASES,   wxT("&Aliases…\tCtrl+Shift+A"));
    cfgMenu->Append(ID_MENU_CFG_TIMERS,    wxT("T&imers…\tCtrl+Shift+I"));
    cfgMenu->AppendSeparator();
    cfgMenu->Append(ID_MENU_CFG_VARIABLES, wxT("&Variables…\tCtrl+Shift+V"));
    cfgMenu->AppendSeparator();
    cfgMenu->Append(ID_MENU_CFG_MACROS,   wxT("&Macros && Keypad…"));
    cfgMenu->Append(ID_MENU_CFG_COLORS,   wxT("&Colors…"));
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
        if (!frame->IsIconized()) {
            wxConfig cfg(wxT("mushclient"));
            bool maximized = frame->IsMaximized();
            cfg.Write(wxT("/geometry/main_maximized"), maximized);
            if (!maximized) {
                wxRect r = frame->GetRect();
                cfg.Write(wxT("/geometry/main_x"), (long)r.x);
                cfg.Write(wxT("/geometry/main_y"), (long)r.y);
                cfg.Write(wxT("/geometry/main_w"), (long)r.width);
                cfg.Write(wxT("/geometry/main_h"), (long)r.height);
            }
        }
        // Stop the socket-poll timer before child frames are destroyed.
        if (pollTimer->IsRunning()) pollTimer->Stop();
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

        if (id == 32239 /*ID_CONNECTION_CONNECT*/) {
            // Enable only when disconnected and a world is open.
            evt.Enable(doc && doc->m_iConnectPhase == eConnectNotConnected);
        } else if (id == 0xE200 /*Disconnect*/) {
            // Enable only when connected.
            evt.Enable(doc && doc->m_iConnectPhase == eConnectConnectedToMud);
        } else if (id == 0xE201 /*Reconnect*/) {
            // Enable only when not connected.
            evt.Enable(doc && doc->m_iConnectPhase == eConnectNotConnected);
        } else if (id == wxID_SAVE || id == wxID_SAVEAS ||
                   id == wxID_CLOSE ||
                   id == 32158 /*ID_GAME_PREFERENCES*/ ||
                   id == ID_MENU_CFG_TRIGGERS  ||
                   id == ID_MENU_CFG_ALIASES   ||
                   id == ID_MENU_CFG_TIMERS    ||
                   id == ID_MENU_CFG_VARIABLES ||
                   id == ID_MENU_CFG_MACROS    ||
                   id == ID_MENU_CFG_COLORS    ||
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
            // Need an active world with scripting enabled.
            evt.Enable(doc != nullptr && doc->m_bEnableScripts);
        } else if (id == ID_CONN_LOG_TOGGLE) {
            // Need an active world; label flips between Start/Stop.
            evt.Enable(doc != nullptr);
            if (doc) {
                bool logging = (doc->m_logfile != nullptr);
                evt.SetText(logging ? wxT("Stop &Logging") : wxT("Start &Logging…"));
            }
        } else if (id == ID_MENU_VIEW_ALWAYSONTOP) {
            evt.Check((frame->GetWindowStyleFlag() & wxSTAY_ON_TOP) != 0);
        } else if (id == ID_MENU_VIEW_FULLSCREEN) {
            evt.Check(frame->IsFullScreen());
        } else if (id == ID_MENU_VIEW_STATUSBAR) {
            auto* child = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild());
            evt.Enable(child != nullptr);
            evt.Check(child && child->IsStatusBarStripShown());
        } else if (id == 32275 || id == 32276 || id == 32974 || id == 32272 ||
                   id == 32152 || id == 33036 || id == 33044) {
            // View-level output/input commands — need an active world.
            evt.Enable(doc != nullptr);
        } else {
            evt.Skip();  // default behaviour for all other items
        }
    });

    // ── Menu event routing via Bind() ─────────────────────────────────────
    frame->Bind(wxEVT_MENU, [frame](wxCommandEvent& evt) {
        int id = evt.GetId();

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
            long style = frame->GetWindowStyleFlag();
            frame->SetWindowStyleFlag((style & wxSTAY_ON_TOP)
                                      ? (style & ~wxSTAY_ON_TOP)
                                      : (style |  wxSTAY_ON_TOP));
            return;
        }
        if (id == ID_MENU_VIEW_FULLSCREEN) {
            frame->ShowFullScreen(!frame->IsFullScreen());
            return;
        }
        if (id == ID_MENU_VIEW_STATUSBAR) {
            // Toggle the active world's own status bar (the visible one on macOS;
            // the MDI parent's bar is hidden behind the child).
            if (auto* child = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild()))
                child->ShowStatusBarStrip(!child->IsStatusBarStripShown());
            return;
        }

        // ── View-level output/input commands (canonical handlers live on the
        // un-instantiated CMUSHView/CSendView; reimplemented on the child frame).
        if (id == 32275 || id == 32276 || id == 32974 || id == 32272 ||
            id == 32152 || id == 33036 || id == 33044) {
            if (auto* child = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild())) {
                switch (id) {
                case 32275: child->GotoSelectedURL();      break; // ID_DISPLAY_GOTOURL
                case 32276: child->SendMailToSelection();  break; // ID_DISPLAY_SENDMAILTO
                case 32974: child->CopySelectionHTML();    break; // ID_EDIT_COPYASHTML
                case 32272: child->RepeatLastCommand();    break; // ID_REPEAT_LAST_COMMAND
                case 32152: child->ShowCommandHistory();   break; // ID_GAME_COMMANDHISTORY
                case 33036: child->HighlightSelectedWord();break; // ID_DISPLAY_HIGHLIGHTPHRASE
                case 33044: child->MakeMultiLineTrigger(); break; // ID_DISPLAY_MULTILINETRIGGER
                }
            }
            return;
        }

        // ── Edit menu: Copy / Cut / Paste / Select All ─────────────────────
        // The frame's Bind intercepts menu events before wxMDI can route them
        // to the focused child control, so we dispatch explicitly here.
        // For a wxTextCtrl (input bar), post a standard wxCommandEvent so the
        // built-in edit handling fires.  For the output pane, call directly.
        if (id == wxID_COPY || id == wxID_CUT ||
            id == wxID_PASTE || id == wxID_SELECTALL)
        {
            wxWindow* focused = wxWindow::FindFocus();
            if (focused) {
                // Is it the output pane?
                if (auto* pane = dynamic_cast<COutputPane*>(focused)) {
                    if      (id == wxID_COPY)      pane->CopySelection();
                    else if (id == wxID_SELECTALL) pane->SelectAll();
                    // Cut and Paste are no-ops on a read-only output pane.
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

        // ── Activity List: per-world overview window ───────────────────────
        if (id == 32203 /*ID_DISPLAY_ACTIVITYLIST*/) {
            ShowActivityWindow();
            return;
        }

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

        // ── Display menu: connection diagnostics (toggle flags on the doc) ──
        if (id == 32971 /*ID_EDIT_DEBUGINCOMINGPACKETS*/ || id == 32334 /*ID_GAME_TRACE*/) {
            bool state = false;
            if (auto* c = dynamic_cast<CWorldChildFrame*>(frame->GetActiveChild())) {
                if (CMUSHclientDoc* doc = c->GetDoc()) {
                    bool& flag = (id == 32971) ? doc->m_bDebugIncomingPackets
                                               : doc->m_bTrace;
                    flag  = !flag;
                    state = flag;
                    // Both features stay silent until there is traffic/activity,
                    // which makes the toggle look like it did nothing.  Confirm
                    // the new state in the output window and say where the real
                    // output will appear.
                    if (id == 32971)
                        doc->Note(state
                            ? "[Debug] Incoming/outgoing packet logging ON — once "
                              "connected, raw bytes (including telnet IAC "
                              "negotiation and other invisible protocol traffic) "
                              "appear in a \"Packet debug\" notepad window."
                            : "[Debug] Packet logging OFF.");
                    else
                        doc->Note(state
                            ? "[Trace] ON — trigger, alias and timer matches will "
                              "be shown here as TRACE: lines."
                            : "[Trace] OFF.");
                }
            }
            if (wxMenuBar* bar = frame->GetMenuBar())
                if (wxMenuItem* mi = bar->FindItem(id))
                    mi->Check(state);
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
        // Not tied to any world; passing "" / 0 lets CreateTextWindow apply the
        // Notepad-preferences font and colours (with a black-on-white fallback).
        if (id == 40001 /*port-local: New standalone Notepad*/) {
            CreateTextWindow("",                 // contents
                             "Notepad",          // title
                             nullptr,            // no related world (standalone)
                             0,                  // unique document number
                             "",                 // font name → App.m_strNotepadFont
                             0,                  // font size → App.m_iNotepadFontSize
                             FW_NORMAL,          // weight
                             DEFAULT_CHARSET,    // charset
                             0, 0,               // colours → Notepad prefs / fallback
                             "", "",             // search string / line preamble
                             false, false, false, false, false,
                             0, eNotepadNormal);
            return;
        }

        if (id == wxID_EXIT || id == 0xE141 /*ID_APP_EXIT*/) {
            frame->Close(false);  // false = veto-able so EVT_CLOSE handler can confirm
        } else if (id == wxID_NEW || id == 0xE100 /*ID_FILE_NEW*/
                   || id == 32289 /*ID_CONNECTION_QUICK_CONNECT*/) {
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
        } else if (id == 0xE200 /*Disconnect*/ ||
                   id == 32240 /*ID_CONNECTION_DISCONNECT*/) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                doc->DisconnectWorld();
                if (s_statusBar)
                    s_statusBar->SetStatusText(wxT("Disconnected."));
            }
        } else if (id == 0xE201 /*Reconnect*/) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                if (doc->m_iConnectPhase == eConnectNotConnected) {
                    doc->m_bDisconnectOK = false;
                    doc->ConnectSocket();
                    if (s_statusBar)
                        s_statusBar->SetStatusText(wxT("Connecting…"));
                }
            }
        } else if (id == 32246 /*ID_FILE_PREFERENCES*/) {
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
            // logo (IDC_LOGO), version and links.  (Its Credits/License buttons
            // open CCreditsDlg, which shows "Could not load text" on the port —
            // the licence TEXT resources aren't bundled — rather than crashing.)
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
            if (doc->m_logfile) {
                // Stop logging.
                fclose(doc->m_logfile);
                doc->m_logfile = nullptr;
                if (s_statusBar)
                    s_statusBar->SetStatusText(wxT("Logging stopped."));
            } else {
                // Start logging — ask for a file.
                wxString defaultName = wxString::FromUTF8(
                    static_cast<const char*>(doc->m_strAutoLogFileName));
                wxFileDialog dlg(frame,
                                 wxT("Log to File"),
                                 wxEmptyString, defaultName,
                                 wxT("Log files (*.log;*.txt)|*.log;*.txt|All files (*.*)|*.*"),
                                 wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
                if (dlg.ShowModal() != wxID_OK) return;
                wxString path = dlg.GetPath();
                doc->m_logfile_name = CString(path.ToUTF8().data());
                doc->m_logfile = fopen(static_cast<const char*>(doc->m_logfile_name), "a+");
                if (!doc->m_logfile) {
                    wxMessageBox(wxT("Could not open log file:\n") + path,
                                 wxT("Logging Error"), wxOK | wxICON_ERROR, frame);
                } else {
                    if (s_statusBar)
                        s_statusBar->SetStatusText(
                            wxT("Logging to: ") + path);
                }
            }
        } else {
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

    // Initialise colour tables (same as hook_new_world; overridden by world file).
    doc->m_normalcolour[0] = RGB(  0,   0,   0);
    doc->m_normalcolour[1] = RGB(128,   0,   0);
    doc->m_normalcolour[2] = RGB(  0, 128,   0);
    doc->m_normalcolour[3] = RGB(128, 128,   0);
    doc->m_normalcolour[4] = RGB(  0,   0, 128);
    doc->m_normalcolour[5] = RGB(128,   0, 128);
    doc->m_normalcolour[6] = RGB(  0, 128, 128);
    doc->m_normalcolour[7] = RGB(192, 192, 192);
    doc->m_boldcolour[0]   = RGB(128, 128, 128);
    doc->m_boldcolour[1]   = RGB(255,   0,   0);
    doc->m_boldcolour[2]   = RGB(  0, 255,   0);
    doc->m_boldcolour[3]   = RGB(255, 255,   0);
    doc->m_boldcolour[4]   = RGB(  0,   0, 255);
    doc->m_boldcolour[5]   = RGB(255,   0, 255);
    doc->m_boldcolour[6]   = RGB(  0, 255, 255);
    doc->m_boldcolour[7]   = RGB(255, 255, 255);
    for (int i = 0; i < MAX_CUSTOM; ++i) {
        doc->m_customtext[i] = doc->m_normalcolour[i % 8];
        doc->m_customback[i] = RGB(0, 0, 0);
    }

    // Set pre-load defaults (may be overridden by the world file).
    doc->m_nWrapColumn = 80;
    doc->m_bUTF_8      = 1;
    doc->m_maxlines    = 5000;

    // Load world settings from XML.  This calls Serialize() → Load_World_XML()
    // which parses ip_address, port, triggers, aliases, timers, etc.
    if (!doc->OnOpenDocument(path)) {
        delete doc;
        // Remove from the startup world list — the file is missing or unreadable.
        world_list_remove(path);
        return nullptr;
    }

    // Record the file path on the document (the base OnOpenDocument doesn't), so
    // it can be identified as already-open above, and so Save writes back here.
    doc->SetPathName(path);

    // Patch custom-text colours to be visible (same as hook_new_world).
    for (int i = 0; i < MAX_CUSTOM; ++i)
        doc->m_customtext[i] = RGB(192, 192, 192);

    // Create and show the child frame.
    CWorldChildFrame* child = new CWorldChildFrame(mdiParent, doc);
    wx_register_world_frame(doc, child);  // keep the registry up-to-date
    child->UpdateTitle();
    child->Show();

    // All output callbacks (m_fn_line_ready, m_fn_note, m_fn_begin_batch,
    // m_fn_flush, m_fn_status) and initial pane state (font, scrollback,
    // background) are applied inside CWorldChildFrame's constructor.

    // Flush any notes produced during OnOpenDocument.
    doc->UpdateAllViews(nullptr);

    // Remember this world file path so it reopens on next launch.
    world_list_add(path);
    // Also add to the File → Open Recent list.
    recent_worlds_add(path);

    // Auto-connect if the world file says to (m_connect_now != 0).
    // Mirrors what Windows does: after opening the file the connection phase is
    // eConnectNotConnected and ConnectSocket() starts the async dial.
    if (doc->m_connect_now && doc->m_iConnectPhase == eConnectNotConnected)
        doc->ConnectSocket();

    return static_cast<void*>(doc);
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
                         path.IsEmpty() ? wxT("world.mcl") : wxFileName(path).GetFullName(),
                         wxT("MUSHclient world files (*.mcl)|*.mcl|All files (*.*)|*.*"),
                         wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK) return;
        path = dlg.GetPath();
    }

    if (path.IsEmpty()) return;

    std::string savePath = path.ToUTF8().data();
    if (!doc->OnSaveDocument(savePath.c_str())) {
        wxMessageBox(wxT("Failed to save world file."), wxT("Save Error"), wxICON_ERROR | wxOK);
        return;
    }

    // Remove the old path from the world list if save-as changed it.
    std::string oldPath = static_cast<const char*>(doc->GetPathName());
    if (!oldPath.empty() && oldPath != savePath)
        world_list_remove(oldPath);

    // Update the document's path so subsequent Ctrl+S uses the same file.
    doc->SetPathName(savePath.c_str());
    doc->SetModifiedFlag(FALSE);

    // Add to the startup world list so this world reopens on next launch.
    world_list_add(savePath);
    // Also add to the File → Open Recent list.
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
        wxTheClipboard->GetData(data);
        wxScopedCharBuffer buf = data.GetText().ToUTF8();
        if (buf.length() > 0)
            result = strdup(buf.data());
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

    // Use the MDI parent as the dialog owner when available.
    wxWindow* parent = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;

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

    wxWindow* parent = wxGetActiveWindow();
    if (!parent) parent = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;

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
    data.EnableEffects(false);
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
                         path.IsEmpty() ? wxT("world.mcl")
                                        : wxFileName(path).GetFullName(),
                         wxT("MUSHclient world files (*.mcl)|*.mcl"
                             "|All files (*.*)|*.*"),
                         wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK) return false;
        path = dlg.GetPath();
    }

    if (path.IsEmpty()) return false;

    std::string savePath = path.ToUTF8().data();
    if (!doc->OnSaveDocument(savePath.c_str())) {
        wxMessageBox(wxT("Failed to save world file."),
                     wxT("Save Error"), wxICON_ERROR | wxOK, parent);
        return false;
    }

    // Update path, clear modified flag, maintain world lists.
    std::string oldPath = static_cast<const char*>(doc->GetPathName());
    if (!oldPath.empty() && oldPath != savePath)
        world_list_remove(oldPath);
    doc->SetPathName(savePath.c_str());
    doc->SetModifiedFlag(FALSE);
    world_list_add(savePath);
    recent_worlds_add(savePath);
    return true;
}

// ── New-world hook ────────────────────────────────────────────────────────
// "Open a new world" flow:
//   1. Create CMUSHclientDoc, call OnNewDocument (→ Quick Connect dialog
//      → SetUpOutputWindow → OpenSession).  Lines accumulate in m_LineList.
//   2. On cancel/error: delete the doc and return nullptr.
//   3. On success: create CWorldChildFrame, show it, wire m_fn_line_ready,
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

    // ── Default ANSI colour tables ────────────────────────────────────────
    // On non-Windows these arrays are not populated by the MFC/registry
    // initialisation that runs on Windows, and they stay zero (all black)
    // unless a world file is loaded from XML.  For Quick Connect (no world
    // file) we initialise them to the standard VGA / xterm palette here,
    // matching what doc_portable.cpp does for the mushcore CLI build.
    doc->m_normalcolour[0/*BLACK*/]   = RGB(  0,   0,   0);
    doc->m_normalcolour[1/*RED*/]     = RGB(128,   0,   0);
    doc->m_normalcolour[2/*GREEN*/]   = RGB(  0, 128,   0);
    doc->m_normalcolour[3/*YELLOW*/]  = RGB(128, 128,   0);
    doc->m_normalcolour[4/*BLUE*/]    = RGB(  0,   0, 128);
    doc->m_normalcolour[5/*MAGENTA*/] = RGB(128,   0, 128);
    doc->m_normalcolour[6/*CYAN*/]    = RGB(  0, 128, 128);
    doc->m_normalcolour[7/*WHITE*/]   = RGB(192, 192, 192);

    doc->m_boldcolour[0/*BLACK*/]   = RGB(128, 128, 128);
    doc->m_boldcolour[1/*RED*/]     = RGB(255,   0,   0);
    doc->m_boldcolour[2/*GREEN*/]   = RGB(  0, 255,   0);
    doc->m_boldcolour[3/*YELLOW*/]  = RGB(255, 255,   0);
    doc->m_boldcolour[4/*BLUE*/]    = RGB(  0,   0, 255);
    doc->m_boldcolour[5/*MAGENTA*/] = RGB(255,   0, 255);
    doc->m_boldcolour[6/*CYAN*/]    = RGB(  0, 255, 255);
    doc->m_boldcolour[7/*WHITE*/]   = RGB(255, 255, 255);

    // Custom colours cycle through the normal palette (matching doc_portable.cpp).
    for (int i = 0; i < MAX_CUSTOM; ++i) {
        doc->m_customtext[i] = doc->m_normalcolour[i % 8];
        doc->m_customback[i] = RGB(0, 0, 0);
    }

    // eQuickConnect makes OnNewDocument show the Quick Connect dialog.
    App.m_TypeOfNewDocument = CMUSHclientApp::eQuickConnect;

    // Initialise display-critical fields that SetUpOutputWindow uses to size
    // the first CLine text buffer.  On Windows these come from the "default
    // world" settings that MFC loads before OnNewDocument; on non-Windows we
    // provide sane defaults here.  They will be overwritten later if the user
    // opens an actual world file or the window is resized.
    doc->m_nWrapColumn = 80;     // CLine allocates m_nWrapColumn bytes for text
    doc->m_bUTF_8      = 1;     // use UTF-8 throughout; 4× multiplier in CLine
    doc->m_maxlines    = 5000;  // SetUpOutputWindow sizes m_pLinePositions from this

    // OnNewDocument: shows dialog, calls SetUpOutputWindow, OpenSession.
    // Lines produced by Note() accumulate in doc->m_LineList but are not
    // forwarded to the pane yet (m_fn_line_ready is wired after this by the
    // CWorldChildFrame constructor; UpdateAllViews below drains the buffer).
    if (!doc->OnNewDocument()) {
        delete doc;   // user cancelled — destructor cleans up
        return nullptr;
    }

    // ── Fix note colours after OnNewDocument ─────────────────────────────
    // OnNewDocument → SetDefaults() sets m_iNoteTextColour = 4 (custom palette
    // index).  Our m_customtext[4] is initialised to m_normalcolour[4] = dark
    // blue — not readable on black.  GetStyleRGB is called lazily at
    // UpdateAllViews time (not at Note() time), so patching m_customtext[] here
    // still affects the lines that were buffered during OnNewDocument.
    // Set every custom-text slot to light grey so all custom colour indices
    // produce readable output when no world file has overridden them.
    for (int i = 0; i < MAX_CUSTOM; ++i)
        doc->m_customtext[i] = RGB(192, 192, 192);  // light grey

    // Create and show the child frame.
    CWorldChildFrame* child = new CWorldChildFrame(mdiParent, doc);
    wx_register_world_frame(doc, child);  // keep the registry up-to-date
    child->UpdateTitle();   // set title from m_mush_name
    child->Show();

    // All output callbacks (m_fn_line_ready, m_fn_note, m_fn_begin_batch,
    // m_fn_flush, m_fn_status) and initial pane state (font, scrollback,
    // background) are applied inside CWorldChildFrame's constructor.

    // Flush welcome banner + any output buffered during OnNewDocument.
    doc->UpdateAllViews(nullptr);

    return static_cast<void*>(doc);
}

// ── App InitInstance bootstrap ────────────────────────────────────────────
// Called by wx_app.cpp::OnInit() via the hook pointer to avoid including
// MUSHclient.h in wx_app.cpp (which would conflict with wx headers).

static int hook_init_instance()
{
    // Set basic CWinApp members that MFC normally sets before InitInstance.
    App.m_lpCmdLine = "";
    App.m_nCmdShow  = SW_SHOWNORMAL;

    int ok = App.InitInstance() != FALSE ? 1 : 0;
    if (ok)
        ApplyDockIconFromPrefs();   // honour the saved tray/taskbar-icon choice
    return ok;
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
    // Default to the global notepad font (Prefs ▸ Notepad ▸ Font…) when the
    // caller didn't specify one.
    td->m_strFontName           = (strFontName && *strFontName)
                                      ? strFontName
                                      : (LPCTSTR) App.m_strNotepadFont;
    td->m_iFontSize             = iFontSize > 0 ? iFontSize
                                               : (long) App.m_iNotepadFontSize;
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
    wxString wxText  = wxString::FromUTF8(sText ? sText : "");

    // CWxNotepadFrame constructor registers td with the template and shows.
    // The frame takes ownership of td.
    new CWxNotepadFrame(td, wxTitle, wxText);

    return static_cast<void*>(td);
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


// ── Notepad append ────────────────────────────────────────────────────────

static bool hook_append_to_notepad(const char* title, const char* text, bool replace)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return false;
    frame->AppendText(text ? text : "", replace);
    return true;
}


static char* hook_get_notepad_text(const char* title)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return nullptr;
    std::string text = frame->GetText();
    char* result = static_cast<char*>(malloc(text.size() + 1));
    if (result) memcpy(result, text.c_str(), text.size() + 1);
    return result;
}

static char** hook_get_notepad_list()
{
    std::vector<std::string> titles = CWxNotepadFrame::ListTitles();
    char** arr = static_cast<char**>(malloc((titles.size() + 1) * sizeof(char*)));
    if (!arr) return nullptr;
    for (size_t i = 0; i < titles.size(); ++i) {
        arr[i] = static_cast<char*>(malloc(titles[i].size() + 1));
        if (arr[i]) memcpy(arr[i], titles[i].c_str(), titles[i].size() + 1);
    }
    arr[titles.size()] = nullptr;
    return arr;
}

// ── Notepad activate / close / move / position ───────────────────────────

static bool hook_activate_notepad(const char* title)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return false;
    if (!frame->IsShown()) frame->Show(true);
    frame->Raise();
    return true;
}

static bool hook_close_notepad(const char* title)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return false;
    frame->Close(false);   // allow SaveModified check
    return true;
}

static bool hook_move_notepad(const char* title, int x, int y, int w, int h)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return false;
    frame->SetSize(x, y, w, h);
    return true;
}

static char* hook_get_notepad_position(const char* title)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return nullptr;
    wxRect r = frame->GetRect();
    char buf[80];
    snprintf(buf, sizeof(buf), "%d %d %d %d",
             r.GetLeft(), r.GetTop(), r.GetWidth(), r.GetHeight());
    char* result = static_cast<char*>(malloc(strlen(buf) + 1));
    if (result) strcpy(result, buf);
    return result;
}

// ── Notepad save ─────────────────────────────────────────────────────────

static bool hook_notepad_colour(const char* title, unsigned long fg, unsigned long bg)
{
    CWxNotepadFrame* f = CWxNotepadFrame::Find(title ? title : "");
    if (!f) return false;
    f->SetColours(fg, bg);
    return true;
}

static bool hook_notepad_font(const char* title, const char* fontName,
                               int size, int style, int charset)
{
    CWxNotepadFrame* f = CWxNotepadFrame::Find(title ? title : "");
    if (!f) return false;
    f->SetFont(fontName ? fontName : "", size, style, charset);
    return true;
}

static bool hook_notepad_readonly(const char* title, bool readOnly)
{
    CWxNotepadFrame* f = CWxNotepadFrame::Find(title ? title : "");
    if (!f) return false;
    f->SetReadOnly(readOnly);
    return true;
}

static bool hook_save_notepad(const char* title, const char* filename, bool /*replaceExisting*/)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return false;
    // Delegate to the frame's File→Save handler (same as menu Ctrl+S).
    // Pass the filename via a synthetic event if non-empty, otherwise show dialog.
    wxCommandEvent evt(wxEVT_MENU, wxID_SAVE);
    // We can't easily pass a filename through the event; instead call SaveFile
    // directly on the ctrl via the doc's m_fn_save_to_file if wired.
    // GetText() retrieves the current text; save it using wxTextCtrl::SaveFile.
    if (filename && *filename)
    {
        wxString path = wxString::FromUTF8(filename);
        // SaveFile is accessible from the wxTextCtrl inside the frame.
        // Since we can't access m_ctrl directly, use m_pDoc callback if set.
        // Fall back: AppendText trick: use the m_fn_save_to_file lambda.
        // The lambda is on the CTextDocument, which we can reach via FindNotepad.
        // For simplicity, use the hook_get_notepad_text + write ourselves.
        std::string text = frame->GetText();
        FILE* fp = fopen(filename, "w");
        if (!fp) return false;
        fwrite(text.c_str(), 1, text.size(), fp);
        fclose(fp);
        return true;
    }
    else
    {
        // Show save dialog — trigger via the frame's menu save handler.
        wxCommandEvent saveEvt(wxEVT_MENU, wxID_SAVE);
        frame->GetEventHandler()->ProcessEvent(saveEvt);
        return true;
    }
}

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
static wxMenu* wx_build_popup_menu(CMenu* m)
{
    wxMenu* wxm = new wxMenu();
    for (const CMenu::Item& it : m->m_items)
    {
        if (it.flags & MF_SEPARATOR)
            wxm->AppendSeparator();
        else if (it.flags & MF_POPUP)
        {
            CMenu*  child   = CMenu::FromMenuHandle((HMENU)(intptr_t) it.arg);
            wxMenu* wxchild = child ? wx_build_popup_menu(child) : new wxMenu();
            wxm->AppendSubMenu(wxchild, wxString::FromUTF8(it.text.c_str()));
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

static int hook_popup_menu(CMenu* menu, int /*x*/, int /*y*/)
{
    if (!menu) return 0;
    wxWindow* win = wxGetActiveWindow();
    if (!win) win = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    if (!win) return 0;

    wxMenu* wxm = wx_build_popup_menu(menu);
    // A context menu is invoked in response to a click, so anchor it at the
    // mouse cursor.  (The Left/Top the caller passes are in client coordinates
    // that the port's no-op ClientToScreen leaves unconverted, so the live
    // cursor position is the reliable anchor.)  GetPopupMenuSelectionFromUser
    // returns the chosen id without firing a menu event — exactly the
    // TPM_RETURNCMD behaviour CMiniWindow::Menu expects.
    wxPoint pt = win->ScreenToClient(::wxGetMousePosition());
    int sel = win->GetPopupMenuSelectionFromUser(*wxm, pt);
    delete wxm;
    return (sel > 0) ? sel : 0;   // wxID_NONE / negative ⇒ cancelled
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

    int result = dlg->ShowModal();

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

    wxTreebook* tb = new wxTreebook(&dlg, wxID_ANY);

    // Pages, index-aligned with the treebook (insertion order == add order ==
    // the canonical nPage that m_iWantedPage is compared against).
    std::vector<CPropertyPage*> pages;
    pages.reserve(sheet->m_portPages.size());

    for (const auto& pp : sheet->m_portPages)
    {
        CPropertyPage* page  = pp.pPage;
        wxPanel*       panel = build_canonical_sheet_page(tb, page);

        wxString label = wxString::FromUTF8((const char*) pp.caption);
        if ((pp.nType & tps_item_type_mask) == tps_item_branch)
            tb->AddPage(panel, label);
        else
            tb->AddSubPage(panel, label);

        pages.push_back(page);
    }

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
        if (leaving >= 0 && leaving < (int) pages.size()
            && pages[leaving] && !pages[leaving]->UpdateData(TRUE))
            e.Veto();   // validation failed (message shown); stay on this page
    });

    // Standard OK / Cancel.
    wxStdDialogButtonSizer* btns = new wxStdDialogButtonSizer();
    wxButton* ok = new wxButton(&dlg, wxID_OK);
    btns->AddButton(ok);
    btns->AddButton(new wxButton(&dlg, wxID_CANCEL));
    btns->Realize();
    ok->SetDefault();

    wxBoxSizer* top = new wxBoxSizer(wxVERTICAL);
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
        if (active >= 0 && active < (int) pages.size()
            && pages[active] && !pages[active]->UpdateData(TRUE))
            return;   // validation failed (message shown); stay open
        dlg.EndModal(wxID_OK);
    }, wxID_OK);

    int rc = dlg.ShowModal();

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
    wxDialog dlg(parent, wxID_ANY, wxT("Global Preferences"),
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
            && pages[leaving] && !pages[leaving]->UpdateData(TRUE))
            e.Veto();
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
        if (active >= 0 && active < (int) pages.size()
            && pages[active] && !pages[active]->UpdateData(TRUE))
            return;   // validation failed (message shown); stay open
        dlg.EndModal(wxID_OK);
    }, wxID_OK);

    int rc = dlg.ShowModal();

    for (CPropertyPage* page : pages)            // detach before panels die
        if (page) page->m_hWnd = nullptr;

    return (rc == wxID_OK) ? IDOK : IDCANCEL;
}

// ── Dialog control accessors (GetDlgItem / DDX) ───────────────────────────
static void* hook_get_dlg_item(void* parent, int id)
{
    return parent ? ((wxWindow*) parent)->FindWindow(id) : nullptr;
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

static void hook_button_set_check(void* hwnd, int state)
{
    wxWindow* w = (wxWindow*) hwnd;
    if (wxCheckBox* cb = wxDynamicCast(w, wxCheckBox))         cb->SetValue(state != 0);
    else if (wxRadioButton* rb = wxDynamicCast(w, wxRadioButton)) rb->SetValue(state != 0);
}

static int hook_button_get_check(void* hwnd)
{
    wxWindow* w = (wxWindow*) hwnd;
    if (wxCheckBox* cb = wxDynamicCast(w, wxCheckBox))            return cb->GetValue() ? 1 : 0;
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

void wx_mfc_install_hooks()
{
    g_run_mfc_dialog          = hook_run_dialog;
    g_end_mfc_dialog          = hook_end_dialog;
    g_run_mfc_property_sheet  = hook_run_property_sheet;
    g_run_mfc_property_sheet_flat = hook_run_property_sheet_flat;
    g_wxhook_get_dlg_item     = hook_get_dlg_item;
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
    g_wxhook_listctrl_get_next_item   = hook_listctrl_get_next_item;
    g_wxhook_listctrl_set_item_state  = hook_listctrl_set_item_state;
    g_wxhook_listctrl_get_item_state  = hook_listctrl_get_item_state;
    g_wxhook_listctrl_get_selected_count = hook_listctrl_get_selected_count;
    g_wxhook_listctrl_delete_item     = hook_listctrl_delete_item;
    g_wxhook_listctrl_delete_all_items = hook_listctrl_delete_all_items;
    g_wxhook_listctrl_ensure_visible  = hook_listctrl_ensure_visible;
    g_wxhook_listctrl_set_column_width = hook_listctrl_set_column_width;
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
    g_wxhook_radio_get                = hook_radio_get;
    g_wxhook_radio_set                = hook_radio_set;
    g_wxhook_make_owner_draw_button   = hook_make_owner_draw_button;
    g_wxhook_bind_owner_draw          = hook_bind_owner_draw;
    g_wxhook_slider_set_range         = hook_slider_set_range;
    g_wxhook_slider_get_pos           = hook_slider_get_pos;
    g_wxhook_slider_set_pos           = hook_slider_set_pos;
    g_wxhook_slider_set_page_size     = hook_slider_set_page_size;
    g_wxhook_bind_slider              = hook_bind_slider;
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
    g_wxhook_open_world_file    = hook_open_world_file;
    g_wxhook_set_status         = hook_set_status;
    g_wxhook_save_world         = hook_save_world;
    g_wxhook_file_dialog        = hook_file_dialog;
    g_wxhook_font_dialog        = hook_font_dialog;
    g_wxhook_get_clipboard_text   = hook_get_clipboard_text;
    g_wxhook_set_clipboard_text   = hook_set_clipboard_text;
    g_wxhook_create_text_window   = hook_create_text_window;
    g_wxhook_pick_colour          = hook_pick_colour;
    g_wxhook_menu_dialog          = hook_menu_dialog;
    g_wxhook_flash_icon           = hook_flash_icon;
    g_wxhook_move_main_window     = hook_move_main_window;
    g_wxhook_inputbox             = hook_inputbox;
    g_wxhook_directorypicker      = hook_directorypicker;
    g_wxhook_password_dialog      = hook_password_dialog;
    g_wxhook_popup_menu           = hook_popup_menu;
    // Sound is the miniaudio engine (platform/ma_sound.*), not wxSound, so these
    // are always installed (no wxUSE_SOUND gate).
    g_wxhook_play_sound           = hook_play_sound;
    g_wxhook_stop_sound           = hook_stop_sound;
    g_wxhook_sound_play           = hook_sound_play;
    g_wxhook_sound_adjust         = hook_sound_adjust;
    g_wxhook_sound_stop_buffer    = hook_sound_stop_buffer;
    g_wxhook_sound_status         = hook_sound_status;
    g_wxhook_load_text_resource   = hook_load_text_resource;
    g_wxhook_get_font_families    = hook_get_font_families;
    g_wxhook_font_has_glyph       = hook_font_has_glyph;
    g_wxhook_get_sys_color        = hook_get_sys_color;
    g_wxhook_get_system_metric    = hook_get_system_metric;
    g_wxhook_get_pixel            = wxgdi_get_pixel;   // defined in wx_gdi_compat.cpp
    g_wxhook_set_cursor           = hook_set_cursor;
    g_wxhook_set_main_background  = hook_set_main_background;
    g_wxhook_append_to_notepad    = hook_append_to_notepad;
    g_wxhook_get_notepad_text     = hook_get_notepad_text;
    g_wxhook_get_notepad_list     = hook_get_notepad_list;
    g_wxhook_activate_notepad     = hook_activate_notepad;
    g_wxhook_close_notepad        = hook_close_notepad;
    g_wxhook_move_notepad         = hook_move_notepad;
    g_wxhook_get_notepad_position = hook_get_notepad_position;
    g_wxhook_save_notepad         = hook_save_notepad;
    g_wxhook_notepad_colour       = hook_notepad_colour;
    g_wxhook_notepad_font         = hook_notepad_font;
    g_wxhook_notepad_readonly     = hook_notepad_readonly;

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
