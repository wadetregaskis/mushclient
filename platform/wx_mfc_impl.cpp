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
#include <wx/spinctrl.h>
#include <wx/filename.h>
#include <wx/fontpicker.h>
#include <wx/clrpicker.h>  // wxColourPickerCtrl
#include <wx/dnd.h>        // wxFileDropTarget
#include <wx/dirdlg.h>     // wxDirDialog
#include <algorithm>
#include <unordered_map>
#include <string>
#include <vector>

// ── MUSHclient headers (MFC compat layer) ────────────────────────────────
// Must come AFTER wx so our stubs don't shadow wx internals inside this TU.
#include "stdafx.h"        // pulls in compat.h → mfc_ui.h → wx_mfc_hooks.h
#include "MUSHclient.h"    // CMUSHclientApp full definition
#include "mainfrm.h"       // CMainFrame full definition
#include "doc.h"           // CMUSHclientDoc (needed for hook_new_world)
#include "platform/wx_world_child.h"  // CWorldChildFrame

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
// Converts LOGFONT-style height (negative = pixels at 96 DPI) to wx point size
// and applies the face name from the doc to the pane.  Falls through to the
// pane's default font if m_font_name is empty or m_font_height is zero.
static void ApplyWorldFont(COutputPane* pane, CMUSHclientDoc* doc)
{
    if (!pane || !doc) return;

    // m_font_name: face name (e.g. "Courier New"); empty → keep default.
    // m_font_height: LOGFONT lfHeight (negative of pixel height at screen DPI).
    // Assume 96 DPI (standard Windows setting); convert to points.
    const char* face = static_cast<const char*>(doc->m_font_name);
    int         lfH  = (int)doc->m_font_height;

    if (face && *face && lfH != 0) {
        int pixH    = (lfH < 0) ? -lfH : lfH;
        int points  = (pixH * 72 + 48) / 96;  // round to nearest pt; 48 = 96/2
        points      = std::max(COutputPane::kMinFontSize,
                               std::min(COutputPane::kMaxFontSize, points));
        // Bold if m_font_weight >= FW_BOLD (700).
        wxFontWeight weight = (doc->m_font_weight >= 700) ? wxFONTWEIGHT_BOLD
                                                          : wxFONTWEIGHT_NORMAL;
        wxFont font(points, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, weight,
                    false, wxString::FromUTF8(face));
        if (font.IsOk())
            pane->SetOutputFont(font);
    }
}

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
    if (auto* w = toWx(h)) w->SetLabel(wxString::FromUTF8(text ? text : ""));
}

static void hook_move_window(void* h, int x, int y, int w, int ht, bool repaint)
{
    if (auto* wnd = toWx(h)) {
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
        wxWindow* parent = App.m_pMainWnd
                         ? static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd)
                         : nullptr;
        wxTheApp->CallAfter([doc, parent]() {
            // Verify the doc is still live before touching it.
            if (s_docFrameMap.find(doc) == s_docFrameMap.end())
                return;
            wxString prompt = wxString::Format(
                wxT("TLS connection to \"%s\" failed.\n\n%s\n\n"
                    "Connect without encryption?"),
                wxString::FromUTF8(static_cast<const char*>(doc->m_mush_name)),
                wxString::FromUTF8(static_cast<const char*>(doc->m_strSSLLastError)));
            if (wxMessageBox(prompt, wxT("TLS Connection Failed"),
                             wxYES_NO | wxICON_QUESTION, parent) == wxYES)
            {
                doc->m_bUseSSL = false;
                doc->ConnectSocket();
            }
        });
        (void)lp;
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
        s_statusBar->SetStatusText(msg ? wxString::FromUTF8(msg) : wxString());
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
    fileMenu->Append(32246 /*ID_FILE_PREFERENCES*/, wxT("Preferences…"));
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT,   wxT("Quit MUSHclient\tCtrl+Q"));
    mb->Append(fileMenu, wxT("&File"));

    wxMenu* connMenu = new wxMenu();
    // Note: Ctrl+K, Ctrl+D, and Ctrl+R are standard Unix input-bar shortcuts
    // (kill-to-end, delete-forward, and reverse history search respectively) so
    // those keys are not used as menu accelerators here.
    connMenu->Append(32289,       wxT("Quick Connect…\tCtrl+Shift+K"));
    connMenu->AppendSeparator();
    connMenu->Append(0xE200,      wxT("Disconnect"));
    connMenu->Append(0xE201,      wxT("Reconnect\tCtrl+Shift+R"));
    mb->Append(connMenu, wxT("&Connection"));

    wxMenu* gameMenu = new wxMenu();
    gameMenu->Append(32158 /*ID_GAME_PREFERENCES*/, wxT("World Properties…\tCtrl+,"));
    mb->Append(gameMenu, wxT("&Game"));

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
    mb->Append(helpMenu, wxT("&Help"));

    frame->SetMenuBar(mb);

    // ── Populate "Open Recent" submenu from persisted config ──────────────
    recent_worlds_load();
    recent_worlds_update_menu();

    // ── Status bar ────────────────────────────────────────────────────────
    s_statusBar = frame->CreateStatusBar(1);
    if (s_statusBar)
        s_statusBar->SetStatusText(wxT("Ready"));

    // ── Drag-and-drop world files ─────────────────────────────────────────
    // Ownership of the drop target transfers to the window.
    frame->SetDropTarget(new WorldFileDropTarget());

    // ── Socket polling timer ──────────────────────────────────────────────
    // CAsyncSocket::Poll() must be called periodically on the GUI thread to
    // deliver OnConnect / OnReceive / OnClose callbacks.  20 ms matches the
    // interval used by the mushcore CWorldFrame path in wx_frame.cpp.
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

        if (id == 0xE200 /*Disconnect*/) {
            // Enable only when connected.
            evt.Enable(doc && doc->m_iConnectPhase == eConnectConnectedToMud);
        } else if (id == 0xE201 /*Reconnect*/) {
            // Enable only when not connected.
            evt.Enable(doc && doc->m_iConnectPhase == eConnectNotConnected);
        } else if (id == wxID_SAVE || id == wxID_SAVEAS ||
                   id == wxID_CLOSE ||
                   id == 32158 /*ID_GAME_PREFERENCES*/) {
            // Need an active world.
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
        } else if (id == 0xE200 /*Disconnect*/) {
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
        } else if (id == wxID_ABOUT) {
            wxString ver = wxString::FromUTF8(
                static_cast<const char*>(MUSHCLIENT_VERSION));
            wxMessageBox(
                wxT("MUSHclient version ") + ver + wxT("\n")
                wxT("macOS/Linux port — wxWidgets GUI\n\n")
                wxT("Original Windows version by Nick Gammon\n")
                wxT("http://www.gammon.com.au/mushclient"),
                wxT("About MUSHclient"),
                wxOK | wxICON_INFORMATION, frame);
        } else if (id >= kRecentMenuBase &&
                   id <  kRecentMenuBase + kMaxRecentWorlds) {
            // File → Open Recent → pick a specific entry.
            int idx = id - kRecentMenuBase;
            if (idx < (int)s_recentWorlds.size())
                hook_open_world_file(s_recentWorlds[idx].c_str());
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

    // Patch custom-text colours to be visible (same as hook_new_world).
    for (int i = 0; i < MAX_CUSTOM; ++i)
        doc->m_customtext[i] = RGB(192, 192, 192);

    // Create and show the child frame.
    CWorldChildFrame* child = new CWorldChildFrame(mdiParent, doc);
    wx_register_world_frame(doc, child);  // keep the registry up-to-date
    child->UpdateTitle();
    child->Show();

    // Wire the output callbacks.
    COutputPane* pane = child->OutputPane();
    doc->m_fn_line_ready = [pane](SOutputLine line) {
        pane->AppendLine(std::move(line));
    };
    doc->m_fn_note = [pane](const std::string& text) {
        pane->AppendNote(text);
    };
    doc->m_fn_begin_batch = [pane]() { pane->BeginBatch(); };
    doc->m_fn_flush       = [pane]() { pane->EndBatch(); };
    doc->m_fn_status = [child](const std::string& /*text*/) {
        if (child->IsActiveWorld()) child->UpdateStatusBar();
    };

    // Wire input-bar / font callbacks for scripting APIs.
    doc->m_fn_get_command = [child]() -> std::string {
        wxTextCtrl* inp = child->OutputPane() ? child->ActiveInputPublic() : nullptr;
        return inp ? std::string(inp->GetValue().ToUTF8()) : std::string();
    };
    doc->m_fn_set_command = [child](const std::string& text) -> bool {
        wxTextCtrl* inp = child->ActiveInputPublic();
        if (!inp) return false;
        if (!inp->GetValue().IsEmpty()) return false;   // eCommandNotEmpty
        inp->SetValue(wxString::FromUTF8(text.c_str()));
        inp->SetInsertionPointEnd();
        return true;
    };
    doc->m_fn_set_output_font = [pane](const std::string& face, int pts, bool bold, bool italic) {
        wxFont f = pane->GetCurrentFont();
        if (!face.empty())
            f.SetFaceName(wxString::FromUTF8(face.c_str()));
        if (pts > 0) f.SetPointSize(pts);
        f.SetWeight(bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        f.SetStyle(italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL);
        pane->SetOutputFont(f);
    };
    doc->m_fn_set_scroll = [pane](long position) {
        if (position == -1)
            pane->ScrollToBottom();
        else if (position >= 0)
            pane->ScrollToLine((int)position);   // treat as 0-based line index
    };
    doc->m_fn_set_selection = [pane](int sl, int el, int sc, int ec) {
        pane->SetSelectionRange(sl, el, sc, ec);
    };
    doc->m_fn_set_command_selection = [child](long first, long last) {
        wxTextCtrl* inp = child->ActiveInputPublic();
        if (!inp) return;
        long len = (long)inp->GetValue().size();
        long f = (first > 0) ? first - 1 : 0;   // 1-based → 0-based
        long l = (last  < 0) ? len       : last; // -1 → end
        inp->SetSelection(f, l);
    };

    // Apply display settings loaded from the world file.
    ApplyWorldFont(pane, doc);
    if (doc->m_maxlines > 0)
        pane->SetMaxLines(doc->m_maxlines);
    child->ApplyPaneBackground(doc->m_iBackgroundColour);

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

// ── Quick Connect dialog ──────────────────────────────────────────────────
// Shows a small wx dialog with World Name / Server / Port fields.
// Fills *outName and *outAddr with heap-allocated strings (caller frees).
// Returns IDOK on success, IDCANCEL if the user cancels.

namespace {
class QuickConnectDlg : public wxDialog
{
public:
    wxTextCtrl* m_name;
    wxTextCtrl* m_addr;
    wxSpinCtrl* m_port;

    QuickConnectDlg(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, wxT("Quick Connect"),
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        wxBoxSizer* vs = new wxBoxSizer(wxVERTICAL);
        wxFlexGridSizer* grid = new wxFlexGridSizer(3, 2, 6, 8);
        grid->AddGrowableCol(1, 1);

        auto addRow = [&](const wxString& label, wxWindow* ctrl) {
            grid->Add(new wxStaticText(this, wxID_ANY, label),
                      0, wxALIGN_CENTER_VERTICAL);
            grid->Add(ctrl, 1, wxEXPAND);
        };

        m_name = new wxTextCtrl(this, wxID_ANY, wxT("My World"),
                                wxDefaultPosition, wxSize(280, -1));
        m_addr = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                wxDefaultPosition, wxSize(280, -1));
        m_port = new wxSpinCtrl(this, wxID_ANY, wxT("4000"),
                                wxDefaultPosition, wxDefaultSize,
                                wxSP_ARROW_KEYS, 1, 65535, 4000);

        addRow(wxT("World name:"), m_name);
        addRow(wxT("Server:"),     m_addr);
        addRow(wxT("Port:"),       m_port);

        vs->Add(grid, 1, wxEXPAND | wxALL, 12);
        vs->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
                wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

        SetSizerAndFit(vs);
        m_addr->SetFocus();

        // Allow OK only when server is non-empty.
        m_addr->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            if (wxButton* ok = dynamic_cast<wxButton*>(FindWindowById(wxID_OK)))
                ok->Enable(!m_addr->GetValue().IsEmpty());
        });
        if (wxButton* ok = dynamic_cast<wxButton*>(FindWindowById(wxID_OK)))
            ok->Enable(false);
    }
};
} // anonymous namespace

// ── GlobalPrefsDlg ────────────────────────────────────────────────────────
// Application-wide preferences dialog.  Covers the settings most relevant on
// non-Windows: connection behaviour, disconnect notifications, and display.
// These map directly to CMUSHclientApp members loaded from the preferences DB.

namespace {
class GlobalPrefsDlg : public wxDialog
{
public:
    // Connection group
    wxCheckBox* m_reconnectOnFail;
    wxCheckBox* m_notifyOnDisconnect;
    wxCheckBox* m_notifyCannotConnect;
    // Display group
    wxCheckBox* m_errorsToOutput;
    // Behaviour group
    wxCheckBox* m_confirmCloseWorld;
    wxCheckBox* m_confirmCloseApp;
    // Paths group
    wxTextCtrl* m_worldDir;     // default world file directory

    explicit GlobalPrefsDlg(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, wxT("Preferences"),
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        wxBoxSizer* vs = new wxBoxSizer(wxVERTICAL);

        // ── Connection ────────────────────────────────────────────────────
        {
            wxStaticBoxSizer* box =
                new wxStaticBoxSizer(wxVERTICAL, this, wxT("Connection"));
            m_reconnectOnFail    = new wxCheckBox(this, wxID_ANY,
                wxT("Reconnect automatically on link failure"));
            m_notifyOnDisconnect = new wxCheckBox(this, wxID_ANY,
                wxT("Notify when world disconnects"));
            m_notifyCannotConnect = new wxCheckBox(this, wxID_ANY,
                wxT("Notify if connection attempt fails"));
            m_reconnectOnFail->SetValue(App.m_bReconnectOnLinkFailure != 0);
            m_notifyOnDisconnect->SetValue(App.m_bNotifyOnDisconnect != 0);
            m_notifyCannotConnect->SetValue(App.m_bNotifyIfCannotConnect != 0);
            box->Add(m_reconnectOnFail,    0, wxALL, 6);
            box->Add(m_notifyOnDisconnect, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
            box->Add(m_notifyCannotConnect, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
            vs->Add(box, 0, wxEXPAND | wxALL, 10);
        }

        // ── Display ───────────────────────────────────────────────────────
        {
            wxStaticBoxSizer* box =
                new wxStaticBoxSizer(wxVERTICAL, this, wxT("Display"));
            m_errorsToOutput = new wxCheckBox(this, wxID_ANY,
                wxT("Show script errors in the output window"));
            m_errorsToOutput->SetValue(App.m_bErrorNotificationToOutputWindow != 0);
            box->Add(m_errorsToOutput, 0, wxALL, 6);
            vs->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        }

        // ── Behaviour ─────────────────────────────────────────────────────
        {
            wxStaticBoxSizer* box =
                new wxStaticBoxSizer(wxVERTICAL, this, wxT("Behaviour"));
            m_confirmCloseWorld = new wxCheckBox(this, wxID_ANY,
                wxT("Confirm before closing a world"));
            m_confirmCloseApp   = new wxCheckBox(this, wxID_ANY,
                wxT("Confirm before quitting MUSHclient"));
            m_confirmCloseWorld->SetValue(App.m_bConfirmBeforeClosingWorld != 0);
            m_confirmCloseApp->SetValue(App.m_bConfirmBeforeClosingMushclient != 0);
            box->Add(m_confirmCloseWorld, 0, wxALL, 6);
            box->Add(m_confirmCloseApp,   0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
            vs->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        }

        // ── Paths ─────────────────────────────────────────────────────────
        {
            wxStaticBoxSizer* box =
                new wxStaticBoxSizer(wxVERTICAL, this, wxT("Paths"));
            wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(this, wxID_ANY, wxT("World file directory:")),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

            m_worldDir = new wxTextCtrl(this, wxID_ANY,
                wxString::FromUTF8(static_cast<const char*>(App.m_strDefaultWorldFileDirectory)),
                wxDefaultPosition, wxSize(280, -1));
            row->Add(m_worldDir, 1, wxEXPAND);

            wxButton* browseBtn = new wxButton(this, wxID_ANY, wxT("…"),
                                               wxDefaultPosition, wxSize(28, -1));
            browseBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                wxDirDialog dlg(this, wxT("Choose default world directory"),
                                m_worldDir->GetValue(),
                                wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
                if (dlg.ShowModal() == wxID_OK)
                    m_worldDir->SetValue(dlg.GetPath());
            });
            row->Add(browseBtn, 0, wxLEFT, 4);

            box->Add(row, 0, wxEXPAND | wxALL, 6);
            vs->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        }

        vs->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
                wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        SetSizerAndFit(vs);
    }
};
} // anonymous namespace

static void hook_file_preferences(wxWindow* parent)
{
    GlobalPrefsDlg dlg(parent);
    if (dlg.ShowModal() != wxID_OK) return;

    App.m_bReconnectOnLinkFailure          = dlg.m_reconnectOnFail->GetValue()    ? 1 : 0;
    App.m_bNotifyOnDisconnect              = dlg.m_notifyOnDisconnect->GetValue()  ? 1 : 0;
    App.m_bNotifyIfCannotConnect           = dlg.m_notifyCannotConnect->GetValue() ? 1 : 0;
    App.m_bErrorNotificationToOutputWindow = dlg.m_errorsToOutput->GetValue()      ? 1 : 0;
    App.m_bConfirmBeforeClosingWorld       = dlg.m_confirmCloseWorld->GetValue()   ? 1 : 0;
    App.m_bConfirmBeforeClosingMushclient  = dlg.m_confirmCloseApp->GetValue()     ? 1 : 0;

    // World directory path
    if (dlg.m_worldDir) {
        App.m_strDefaultWorldFileDirectory =
            CString(dlg.m_worldDir->GetValue().ToUTF8().data());
    }

    // Persist to the preferences DB (mirrors what mainfrm.cpp does on Windows).
    App.db_write_int("prefs", "ReconnectOnLinkFailure",
                     App.m_bReconnectOnLinkFailure);
    App.db_write_int("prefs", "NotifyOnDisconnect",
                     App.m_bNotifyOnDisconnect);
    App.db_write_int("prefs", "NotifyIfCannotConnect",
                     App.m_bNotifyIfCannotConnect);
    App.db_write_int("prefs", "ErrorNotificationToOutputWindow",
                     App.m_bErrorNotificationToOutputWindow);
    App.db_write_int("prefs", "ConfirmBeforeClosingWorld",
                     App.m_bConfirmBeforeClosingWorld);
    App.db_write_int("prefs", "ConfirmBeforeClosingMushclient",
                     App.m_bConfirmBeforeClosingMushclient);
    App.db_write_string("prefs", "DefaultWorldFileDirectory",
                        static_cast<const char*>(App.m_strDefaultWorldFileDirectory));
}

// ── WorldPreferencesDlg ───────────────────────────────────────────────────
// Dialog for editing world properties: connection, display, scripting, options.
// Covers the settings needed to connect and run Lua scripts without the full
// Windows property-sheet UI.

namespace {
class WorldPreferencesDlg : public wxDialog
{
public:
    // Connection
    wxTextCtrl*          m_name;
    wxTextCtrl*          m_server;
    wxSpinCtrl*          m_port;
    wxCheckBox*          m_autoConnect;
    // Display
    wxFontPickerCtrl*    m_fontPicker;
    wxColourPickerCtrl*  m_bgColorPicker;
    // Scripting
    wxTextCtrl*          m_scriptFile;
    wxButton*            m_scriptBrowse;
    wxTextCtrl*          m_scriptPrefix;
    // Options
    wxSpinCtrl*          m_scrollback;
    wxSpinCtrl*          m_wrapColumn;
    wxCheckBox*          m_enableBeeps;

    struct Params {
        // Connection
        wxString name, server;
        int      port;
        bool     autoConnect;
        // Display
        wxFont   currentFont;
        int      scrollbackLines;
        wxColour bgColor;
        // Scripting
        wxString scriptFile;
        wxString scriptPrefix;
        // Options
        int  wrapColumn;
        bool enableBeeps;
    };

    explicit WorldPreferencesDlg(wxWindow* parent, const Params& p)
        : wxDialog(parent, wxID_ANY, wxT("World Properties"),
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_scriptBrowse(nullptr)
        , m_scrollback(nullptr)
        , m_wrapColumn(nullptr)
        , m_bgColorPicker(nullptr)
        , m_enableBeeps(nullptr)
    {
        wxBoxSizer* vs = new wxBoxSizer(wxVERTICAL);

        // Helper: add a label+control pair to a flex grid
        // (captured below per group, so we use a lambda that closes over grid)

        // ── Connection settings ──────────────────────────────────────────
        {
            wxStaticBoxSizer* box =
                new wxStaticBoxSizer(wxVERTICAL, this, wxT("Connection"));
            wxFlexGridSizer* g = new wxFlexGridSizer(3, 2, 6, 8);
            g->AddGrowableCol(1, 1);

            auto row = [&](const wxString& lbl, wxWindow* ctrl) {
                g->Add(new wxStaticText(this, wxID_ANY, lbl),
                       0, wxALIGN_CENTER_VERTICAL);
                g->Add(ctrl, 1, wxEXPAND);
            };

            m_name   = new wxTextCtrl(this, wxID_ANY, p.name,
                                      wxDefaultPosition, wxSize(340, -1));
            m_server = new wxTextCtrl(this, wxID_ANY, p.server);
            m_port   = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                                      wxDefaultPosition, wxSize(90, -1),
                                      wxSP_ARROW_KEYS, 1, 65535,
                                      p.port > 0 ? p.port : 23);
            row(wxT("World name:"),     m_name);
            row(wxT("Server address:"), m_server);
            row(wxT("Port:"),           m_port);

            box->Add(g, 1, wxEXPAND | wxALL, 8);
            vs->Add(box, 0, wxEXPAND | wxALL, 12);
        }

        // ── Display ──────────────────────────────────────────────────────
        {
            wxStaticBoxSizer* box =
                new wxStaticBoxSizer(wxVERTICAL, this, wxT("Display"));
            wxFlexGridSizer* g = new wxFlexGridSizer(2, 2, 6, 8);
            g->AddGrowableCol(1, 1);

            auto row = [&](const wxString& lbl, wxWindow* ctrl) {
                g->Add(new wxStaticText(this, wxID_ANY, lbl),
                       0, wxALIGN_CENTER_VERTICAL);
                g->Add(ctrl, 1, wxEXPAND);
            };

            wxFont initFont = p.currentFont.IsOk()
                ? p.currentFont
                : wxFont(12, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                         wxFONTWEIGHT_NORMAL);
            m_fontPicker = new wxFontPickerCtrl(this, wxID_ANY, initFont,
                                                wxDefaultPosition, wxDefaultSize,
                                                wxFNTP_DEFAULT_STYLE);
            row(wxT("Output font:"), m_fontPicker);

            wxColour initBg = p.bgColor.IsOk() ? p.bgColor : wxColour(0, 0, 0);
            m_bgColorPicker = new wxColourPickerCtrl(this, wxID_ANY, initBg,
                                                     wxDefaultPosition, wxDefaultSize,
                                                     wxCLRP_DEFAULT_STYLE);
            // Place colour picker left-aligned (don't expand)
            g->Add(new wxStaticText(this, wxID_ANY, wxT("Background:")),
                   0, wxALIGN_CENTER_VERTICAL);
            g->Add(m_bgColorPicker, 0, wxALIGN_CENTER_VERTICAL);

            box->Add(g, 1, wxEXPAND | wxALL, 8);
            vs->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        }

        // ── Scripting ────────────────────────────────────────────────────
        {
            wxStaticBoxSizer* box =
                new wxStaticBoxSizer(wxVERTICAL, this, wxT("Scripting (Lua)"));
            wxFlexGridSizer* g = new wxFlexGridSizer(2, 3, 6, 8);
            g->AddGrowableCol(1, 1);

            // Script file row: label | text field | Browse button
            g->Add(new wxStaticText(this, wxID_ANY, wxT("Script file:")),
                   0, wxALIGN_CENTER_VERTICAL);
            m_scriptFile = new wxTextCtrl(this, wxID_ANY, p.scriptFile);
            g->Add(m_scriptFile, 1, wxEXPAND);
            m_scriptBrowse = new wxButton(this, wxID_ANY, wxT("Browse\xe2\x80\xa6"),
                                          wxDefaultPosition, wxDefaultSize,
                                          wxBU_EXACTFIT);
            g->Add(m_scriptBrowse, 0, wxALIGN_CENTER_VERTICAL);

            // Script prefix row: label | text field | empty spacer
            g->Add(new wxStaticText(this, wxID_ANY, wxT("Script prefix:")),
                   0, wxALIGN_CENTER_VERTICAL);
            m_scriptPrefix = new wxTextCtrl(this, wxID_ANY, p.scriptPrefix,
                                            wxDefaultPosition, wxSize(60, -1));
            g->Add(m_scriptPrefix, 0);   // don't expand — prefix is short
            g->AddSpacer(0);             // spacer in third column

            box->Add(g, 1, wxEXPAND | wxALL, 8);

            // Hint label below the grid.
            wxStaticText* hint = new wxStaticText(this, wxID_ANY,
                wxT("Leave script file empty for no scripting. ")
                wxT("The prefix (e.g. \">\") lets you run Lua\n")
                wxT("from the command bar by typing prefix+code."));
            wxFont small = hint->GetFont();
            small.SetPointSize(std::max(8, small.GetPointSize() - 1));
            hint->SetFont(small);
            box->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

            vs->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        }

        // ── Options ──────────────────────────────────────────────────────
        {
            wxStaticBoxSizer* box =
                new wxStaticBoxSizer(wxVERTICAL, this, wxT("Options"));

            m_autoConnect = new wxCheckBox(this, wxID_ANY,
                                           wxT("Connect automatically when world opens"));
            m_autoConnect->SetValue(p.autoConnect);
            box->Add(m_autoConnect, 0, wxALL, 8);

            m_enableBeeps = new wxCheckBox(this, wxID_ANY,
                                           wxT("Enable beeps (BEL character)"));
            m_enableBeeps->SetValue(p.enableBeeps);
            box->Add(m_enableBeeps, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

            // Two spinners on one row: scrollback + wrap column.
            wxBoxSizer* spRow = new wxBoxSizer(wxHORIZONTAL);

            spRow->Add(new wxStaticText(this, wxID_ANY, wxT("Scrollback:")),
                       0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            m_scrollback = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                                          wxDefaultPosition, wxSize(80, -1),
                                          wxSP_ARROW_KEYS, 100, 50000,
                                          p.scrollbackLines > 0 ? p.scrollbackLines : 5000);
            spRow->Add(m_scrollback, 0, wxALIGN_CENTER_VERTICAL);
            spRow->Add(new wxStaticText(this, wxID_ANY, wxT("lines")),
                       0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 6);

            spRow->Add(new wxStaticText(this, wxID_ANY, wxT("Wrap at column:")),
                       0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            m_wrapColumn = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                                          wxDefaultPosition, wxSize(70, -1),
                                          wxSP_ARROW_KEYS, 20, 500,
                                          p.wrapColumn > 0 ? p.wrapColumn : 80);
            spRow->Add(m_wrapColumn, 0, wxALIGN_CENTER_VERTICAL);

            box->Add(spRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
            vs->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        }

        vs->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
                wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

        SetSizerAndFit(vs);
        m_name->SetFocus();
        m_name->SelectAll();

        // OK enabled only when server is non-empty.
        auto updateOk = [this](wxCommandEvent&) {
            if (wxButton* ok = dynamic_cast<wxButton*>(FindWindowById(wxID_OK)))
                ok->Enable(!m_server->GetValue().IsEmpty());
        };
        m_server->Bind(wxEVT_TEXT, updateOk);
        if (wxButton* ok = dynamic_cast<wxButton*>(FindWindowById(wxID_OK)))
            ok->Enable(!p.server.IsEmpty());

        // Browse button: open a file chooser for the Lua script.
        m_scriptBrowse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxString cur = m_scriptFile->GetValue();
            wxString dir, file;
            if (!cur.IsEmpty()) {
                dir  = wxFileName(cur).GetPath();
                file = wxFileName(cur).GetFullName();
            }
            wxFileDialog dlg(this,
                             wxT("Choose Lua Script File"),
                             dir, file,
                             wxT("Lua files (*.lua)|*.lua|All files (*.*)|*.*"),
                             wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dlg.ShowModal() == wxID_OK)
                m_scriptFile->SetValue(dlg.GetPath());
        });
    }
};
} // anonymous namespace

static bool hook_game_preferences(void* vdoc, int /*iPage*/)
{
    auto* doc = static_cast<CMUSHclientDoc*>(vdoc);

    // Locate the child frame (and its output pane) that owns this doc via the
    // registry maintained by wx_register/unregister_world_frame.
    wxWindow*         parent     = App.m_pMainWnd
                                  ? static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd)
                                  : nullptr;
    CWorldChildFrame* childFrame = nullptr;
    COutputPane*      pane       = nullptr;
    {
        auto it = s_docFrameMap.find(doc);
        if (it != s_docFrameMap.end()) {
            childFrame = it->second;
            pane       = childFrame->OutputPane();
        }
    }

    // Populate dialog parameters from the doc.
    WorldPreferencesDlg::Params p;
    p.name        = wxString::FromUTF8(static_cast<const char*>(doc->m_mush_name));
    p.server      = wxString::FromUTF8(static_cast<const char*>(doc->m_server));
    p.port        = doc->m_port;
    p.autoConnect = (doc->m_connect_now != 0);
    p.enableBeeps = (doc->m_enable_beeps != 0);

    // Font: use pane's live font (post Ctrl+scroll zoom) or fall back.
    if (pane) p.currentFont = pane->GetCurrentFont();

    // Background colour: clamp NO_COLOUR (0xFFFFFFFF) → black.
    unsigned long bgRef = doc->m_iBackgroundColour;
    if (bgRef == 0xFFFFFFFFUL) bgRef = 0;
    p.bgColor = wxColour(GetRValue(bgRef), GetGValue(bgRef), GetBValue(bgRef));

    p.scrollbackLines = doc->m_maxlines > 0 ? (int)doc->m_maxlines : 5000;
    p.wrapColumn      = doc->m_nWrapColumn  > 0 ? (int)doc->m_nWrapColumn : 80;

    // Scripting
    p.scriptFile   = wxString::FromUTF8(static_cast<const char*>(doc->m_strScriptFilename));
    p.scriptPrefix = wxString::FromUTF8(static_cast<const char*>(doc->m_strScriptPrefix));

    WorldPreferencesDlg dlg(parent, p);
    if (dlg.ShowModal() != wxID_OK)
        return false;

    // ── Apply connection settings ─────────────────────────────────────────
    doc->m_mush_name   = CString(dlg.m_name->GetValue().ToUTF8().data());
    doc->m_server      = CString(dlg.m_server->GetValue().ToUTF8().data());
    doc->m_port        = static_cast<unsigned short>(dlg.m_port->GetValue());
    doc->m_connect_now = dlg.m_autoConnect->GetValue() ? 1 : 0;
    doc->m_enable_beeps = dlg.m_enableBeeps->GetValue() ? 1 : 0;

    // ── Apply font ────────────────────────────────────────────────────────
    if (pane && dlg.m_fontPicker) {
        wxFont chosen = dlg.m_fontPicker->GetSelectedFont();
        if (chosen.IsOk()) {
            pane->SetOutputFont(chosen);

            std::string face = chosen.GetFaceName().ToUTF8().data();
            doc->m_font_name   = CString(face.c_str());
            // Convert wx points → LOGFONT negative-pixel convention at 96 DPI.
            doc->m_font_height = -(chosen.GetPointSize() * 96 / 72);
            doc->m_font_weight = (chosen.GetWeight() >= wxFONTWEIGHT_BOLD)
                                 ? 700 : 400;
            if (childFrame) childFrame->TriggerNAWS();
        }
    }

    // ── Apply scrollback buffer size ─────────────────────────────────────
    if (dlg.m_scrollback) {
        int newMax = dlg.m_scrollback->GetValue();
        if (newMax != (int)doc->m_maxlines) {
            doc->m_maxlines = newMax;
            if (pane) pane->SetMaxLines(newMax);
        }
    }

    // ── Apply wrap column ─────────────────────────────────────────────────
    // m_nWrapColumn governs where the engine word-wraps incoming text.
    // Also update m_gui_cols so the value reported to NAWS stays consistent.
    if (dlg.m_wrapColumn) {
        int newWrap = dlg.m_wrapColumn->GetValue();
        if (newWrap != (int)doc->m_nWrapColumn) {
            doc->m_nWrapColumn = (unsigned short)newWrap;
            doc->m_gui_cols    = newWrap;
            if (childFrame) childFrame->TriggerNAWS();
        }
    }

    // ── Apply background colour ───────────────────────────────────────────
    if (dlg.m_bgColorPicker) {
        wxColour chosen = dlg.m_bgColorPicker->GetColour();
        unsigned long newBg = RGB(chosen.Red(), chosen.Green(), chosen.Blue());
        unsigned long curBg = doc->m_iBackgroundColour;
        if (curBg == 0xFFFFFFFFUL) curBg = 0;
        if (newBg != curBg) {
            doc->m_iBackgroundColour = newBg;
            if (childFrame) childFrame->ApplyPaneBackground(newBg);
        }
    }

    // ── Apply scripting settings ──────────────────────────────────────────
    // Script prefix is always safe to set in place.
    doc->m_strScriptPrefix = CString(dlg.m_scriptPrefix->GetValue().ToUTF8().data());

    // Script file: if changed, reload the script engine.
    // Use the same DisableScripting/CreateScriptEngine path as
    // CMUSHclientDoc::OnGameReloadScriptFile so all cleanup runs correctly.
    {
        CString newFile(dlg.m_scriptFile->GetValue().ToUTF8().data());
        if (newFile != doc->m_strScriptFilename) {
            doc->m_strScriptFilename = newFile;
            if (newFile.IsEmpty()) {
                doc->m_bEnableScripts = 0;
                doc->DisableScripting();
            } else {
                // Language is always Lua on non-Windows.
                doc->m_strLanguage    = CString("Lua");
                doc->m_bEnableScripts = 1;
                doc->DisableScripting();
                doc->CreateScriptEngine();
            }
        }
    }

    // Mark the document dirty so Ctrl+S will offer to save.
    doc->SetModifiedFlag(TRUE);

    // If currently connected, note that server/port changes take effect on reconnect.
    if (doc->m_iConnectPhase == eConnectConnectedToMud) {
        wxMessageBox(
            wxT("World properties updated.\n\n")
            wxT("Font and scripting changes take effect immediately.\n")
            wxT("Changes to server address and port will take effect\n")
            wxT("the next time you reconnect."),
            wxT("World Properties"),
            wxOK | wxICON_INFORMATION, parent);
    }

    return true;
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

static int hook_quick_connect_dlg(char** outName, char** outAddr, int* outPort)
{
    // Use the MDI parent frame as dialog parent.
    // Frame.m_hWnd holds the wxMDIParentFrame* set by hook_load_mdi_frame.
    // Same as hook_new_world: use App.m_pMainWnd, not the stub Frame global.
    wxWindow* parent = App.m_pMainWnd
                     ? static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd)
                     : nullptr;

    QuickConnectDlg dlg(parent);

    // Pre-fill with the last-used values so repeat connections need fewer clicks.
    {
        wxConfig cfg(wxT("mushclient"));
        wxString lastAddr;
        long     lastPort = 0;
        wxString lastName;
        if (cfg.Read(wxT("/quickconnect/addr"), &lastAddr) && !lastAddr.IsEmpty()) {
            dlg.m_addr->SetValue(lastAddr);
            if (wxButton* ok = dynamic_cast<wxButton*>(dlg.FindWindowById(wxID_OK)))
                ok->Enable(true);
        }
        if (cfg.Read(wxT("/quickconnect/port"), &lastPort) && lastPort > 0)
            dlg.m_port->SetValue((int)lastPort);
        if (cfg.Read(wxT("/quickconnect/name"), &lastName) && !lastName.IsEmpty())
            dlg.m_name->SetValue(lastName);
    }

    if (dlg.ShowModal() != wxID_OK)
        return IDCANCEL;

    // Persist for next time.
    {
        wxConfig cfg(wxT("mushclient"));
        cfg.Write(wxT("/quickconnect/addr"), dlg.m_addr->GetValue());
        cfg.Write(wxT("/quickconnect/port"), (long)dlg.m_port->GetValue());
        cfg.Write(wxT("/quickconnect/name"), dlg.m_name->GetValue());
    }

    *outName = strdup(dlg.m_name->GetValue().ToUTF8().data());
    *outAddr = strdup(dlg.m_addr->GetValue().ToUTF8().data());
    *outPort = dlg.m_port->GetValue();
    return IDOK;
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
    // forwarded anywhere yet (m_fn_line_ready is still null).
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

    // Wire the callbacks now that the window is visible.
    COutputPane* pane = child->OutputPane();
    doc->m_fn_line_ready = [pane](SOutputLine line) {
        pane->AppendLine(std::move(line));
    };
    doc->m_fn_note = [pane](const std::string& text) {
        pane->AppendNote(text);
    };
    doc->m_fn_begin_batch = [pane]() { pane->BeginBatch(); };
    doc->m_fn_flush       = [pane]() { pane->EndBatch(); };
    doc->m_fn_status = [child](const std::string& /*text*/) {
        if (child->IsActiveWorld()) child->UpdateStatusBar();
    };

    // Wire input-bar / font callbacks for scripting APIs.
    doc->m_fn_get_command = [child]() -> std::string {
        wxTextCtrl* inp = child->ActiveInputPublic();
        return inp ? std::string(inp->GetValue().ToUTF8()) : std::string();
    };
    doc->m_fn_set_command = [child](const std::string& text) -> bool {
        wxTextCtrl* inp = child->ActiveInputPublic();
        if (!inp) return false;
        if (!inp->GetValue().IsEmpty()) return false;
        inp->SetValue(wxString::FromUTF8(text.c_str()));
        inp->SetInsertionPointEnd();
        return true;
    };
    doc->m_fn_set_output_font = [pane](const std::string& face, int pts, bool bold, bool italic) {
        wxFont f = pane->GetCurrentFont();
        if (!face.empty())
            f.SetFaceName(wxString::FromUTF8(face.c_str()));
        if (pts > 0) f.SetPointSize(pts);
        f.SetWeight(bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        f.SetStyle(italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL);
        pane->SetOutputFont(f);
    };
    doc->m_fn_set_scroll = [pane](long position) {
        if (position == -1)
            pane->ScrollToBottom();
        else if (position >= 0)
            pane->ScrollToLine((int)position);   // treat as 0-based line index
    };
    doc->m_fn_set_selection = [pane](int sl, int el, int sc, int ec) {
        pane->SetSelectionRange(sl, el, sc, ec);
    };
    doc->m_fn_set_command_selection = [child](long first, long last) {
        wxTextCtrl* inp = child->ActiveInputPublic();
        if (!inp) return;
        long len = (long)inp->GetValue().size();
        long f = (first > 0) ? first - 1 : 0;   // 1-based → 0-based
        long l = (last  < 0) ? len       : last; // -1 → end
        inp->SetSelection(f, l);
    };

    // Apply background colour (Quick Connect worlds use default black unless
    // a world-file background has been specified — m_iBackgroundColour starts
    // as NO_COLOUR from doc_construct.cpp so ApplyPaneBackground will just use
    // black, which is the correct default for new quick-connect worlds).
    child->ApplyPaneBackground(doc->m_iBackgroundColour);

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

    return App.InitInstance() != FALSE ? 1 : 0;
}

// ── Install all hooks ─────────────────────────────────────────────────────
void wx_mfc_install_hooks()
{
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
    g_wxhook_quick_connect_dlg  = hook_quick_connect_dlg;
    g_wxhook_set_status         = hook_set_status;
    g_wxhook_game_preferences   = hook_game_preferences;
    g_wxhook_save_world         = hook_save_world;

    // Wire the system-beep hook so doc.cpp's MessageBeep() calls produce
    // an audible alert on macOS/Linux via wxWidgets.
    g_fn_system_beep = []() { wxBell(); };
}

#endif // !_WIN32
