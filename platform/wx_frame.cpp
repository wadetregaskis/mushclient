// platform/wx_frame.cpp — MUSHclient main GUI window (Phase 5)
//
// CWorldFrame: a wxFrame containing:
//   ┌────────────────────────────────┐
//   │ Scrollback output (COutputPane)│  ← styled MUD text, custom renderer
//   │                                │
//   ├────────────────────────────────┤
//   │ Input bar (wxTextCtrl)         │  ← user types commands here
//   └────────────────────────────────┘
//
// Networking uses CAsyncSocket::Poll() driven by a wxTimer (every 20 ms).
// All I/O happens on the GUI thread — no locking needed.  DisplayMsg() routes
// styled output through m_fn_line_ready → AppendLine(), which delegates to
// COutputPane — a custom wxScrolledWindow that renders per-run fg/bg/bold.

#ifndef _WIN32

#include "platform/wx_frame.h"
#include "platform/doc_stub.h"
#include "platform/wx_outputpane.h"
#include "worldsock.h"
#include "platform/posix_socket.h"

#include <wx/wx.h>
#include <wx/splitter.h>
#include <wx/timer.h>
#include <wx/font.h>
#include <wx/filedlg.h>
#include <wx/clipbrd.h>   // wxTheClipboard
#include <wx/dataobj.h>   // wxTextDataObject
#include <wx/config.h>    // wxConfig — persists recent files
#include <wx/filename.h>  // wxFileName — strips path for MRU menu labels
#include <algorithm>      // std::remove (MRU deduplication)
#include <sys/ioctl.h>

// ── Custom event definitions ──────────────────────────────────────────────────
wxDEFINE_EVENT(EVT_MUD_LINE,   wxThreadEvent);
wxDEFINE_EVENT(EVT_MUD_STATUS, wxThreadEvent);

// ── Timer / menu IDs ─────────────────────────────────────────────────────────
static const int ID_POLL_TIMER        = wxID_HIGHEST + 1;
static const int ID_INPUT_CTRL        = wxID_HIGHEST + 2;
static const int ID_MENU_CONNECT      = wxID_HIGHEST + 3;
static const int ID_MENU_CLOSE        = wxID_HIGHEST + 4;
static const int ID_MENU_OPEN_WORLD   = wxID_HIGHEST + 5;
static const int ID_MENU_RECONNECT    = wxID_HIGHEST + 6;
// Recent-worlds MRU: kMaxRecentWorlds consecutive IDs (9 slots)
static const int ID_MENU_RECENT_FIRST = wxID_HIGHEST + 10;
static const int ID_MENU_RECENT_LAST  = ID_MENU_RECENT_FIRST + kMaxRecentWorlds - 1;

// ── Event table ───────────────────────────────────────────────────────────────
wxBEGIN_EVENT_TABLE(CWorldFrame, wxFrame)
    EVT_TEXT_ENTER(ID_INPUT_CTRL,      CWorldFrame::OnInputEnter)
    EVT_TIMER(ID_POLL_TIMER,           CWorldFrame::OnTimer)
    EVT_MENU(ID_MENU_CONNECT,          CWorldFrame::OnMenuConnect)
    EVT_MENU(ID_MENU_OPEN_WORLD,       CWorldFrame::OnMenuOpenWorld)
    EVT_MENU(ID_MENU_RECONNECT,        CWorldFrame::OnMenuReconnect)
    EVT_MENU(ID_MENU_CLOSE,            CWorldFrame::OnMenuCloseWorld)
    EVT_CLOSE(                         CWorldFrame::OnClose)
wxEND_EVENT_TABLE()

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
CWorldFrame::CWorldFrame(const std::string& host, int port)
    : wxFrame(nullptr, wxID_ANY,
              wxString::Format("MUSHclient — %s:%d", host.c_str(), port),
              wxDefaultPosition, wxSize(900, 600))
    , m_doc(std::make_unique<CMUSHclientDoc>())
    , m_host(host)
    , m_port(port)
    , m_timer(std::make_unique<wxTimer>(this, ID_POLL_TIMER))
{
    // ── Layout ────────────────────────────────────────────────────────────────
    wxPanel* panel = new wxPanel(this);
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    // Custom output pane: COutputPane (wxScrolledWindow) renders SOutputLine
    // directly with per-run fg/bg/bold/underline, capped scrollback, and
    // auto-scroll.  Much more capable than wxTextCtrl for MUD output.
    m_pane = new COutputPane(panel, /*maxLines=*/5000);

    // Monospace font for input bar
    wxFont monoFont(12, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                    wxFONTWEIGHT_NORMAL);

    // Input text control: single-line, processes Enter
    m_input = new wxTextCtrl(panel, ID_INPUT_CTRL, wxEmptyString,
                              wxDefaultPosition, wxDefaultSize,
                              wxTE_PROCESS_ENTER);
    m_input->SetFont(monoFont);
    m_input->SetBackgroundColour(wxColour(20, 20, 20));
    m_input->SetForegroundColour(wxColour(220, 220, 220));

    sizer->Add(m_pane,  1, wxEXPAND | wxALL, 2);
    sizer->Add(m_input, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 2);
    panel->SetSizer(sizer);

    // ── Menu bar ──────────────────────────────────────────────────────────────
    wxMenuBar* menuBar = new wxMenuBar();

    wxMenu* worldMenu = new wxMenu();
    worldMenu->Append(ID_MENU_CONNECT,    "&New Connection…\tCtrl+N",
                      "Open a connection to another MUD");
    worldMenu->Append(ID_MENU_OPEN_WORLD, "&Open World File…\tCtrl+O",
                      "Load a .mcl world file (triggers, aliases, plugins)");

    // Recent Worlds submenu — populated from wxConfig after the menu bar is set
    m_recentMenu = new wxMenu();
    worldMenu->AppendSubMenu(m_recentMenu, "&Recent Worlds",
                             "Re-open a recently used world file");

    worldMenu->AppendSeparator();
    worldMenu->Append(ID_MENU_RECONNECT,  "&Reconnect\tCtrl+R",
                      "Disconnect and reconnect to the current MUD");
    worldMenu->AppendSeparator();
    worldMenu->Append(ID_MENU_CLOSE,      "&Close World\tCtrl+W",
                      "Close this world window");
    menuBar->Append(worldMenu, "&World");

    SetMenuBar(menuBar);

    // Status bar
    m_status = CreateStatusBar(2);
    m_status->SetStatusText("Connecting...", 0);

    // ── Wire up GUI output callbacks to this frame ────────────────────────────
    m_doc->m_fn_line_ready = [this](SOutputLine&& line) {
        AppendLine(std::move(line));
    };
    m_doc->m_fn_note = [this](const std::string& text) {
        AppendNote(text);
    };
    m_doc->m_fn_status = [this](const std::string& text) {
        // Status bar field 0 = connection state (updated by OnTimer)
        // Field 1 = MUD-supplied status line (updated here)
        m_status->SetStatusText(wxString::FromUTF8(text.c_str()), 1);
    };
    // Return keyboard focus to the input bar after the user finishes a
    // mouse-drag selection in the output pane (so they can keep typing).
    m_pane->m_fn_return_focus = [this]() {
        if (m_input) m_input->SetFocus();
    };

    // ── Initialise doc settings ───────────────────────────────────────────────
    App.MXP_LoadElements();
    App.MXP_LoadEntities();
    App.MXP_LoadColours();

    m_doc->m_mush_name           = CString(host.c_str());
    m_doc->m_bConvertGAtoNewline = 1;
    m_doc->m_bNAWS               = TRUE;
    m_doc->m_bUTF_8              = 1;
    m_doc->m_nWrapColumn         = 80;  // updated on resize

    // ── Connect ───────────────────────────────────────────────────────────────
    Connect();

    // ── Start poll timer (20 ms — imperceptible latency for MUD use) ─────────
    m_timer->Start(20);

    // Key-down on the input bar — drives command history navigation
    m_input->Bind(wxEVT_KEY_DOWN, &CWorldFrame::OnInputKeyDown, this);

    // Paste intercept — split multi-line clipboard text into individual commands
    m_input->Bind(wxEVT_TEXT_PASTE, &CWorldFrame::OnInputPaste, this);

    // Recent Worlds: load from config and bind menu range handler on *this* frame
    // (dynamic Bind is used because EVT_MENU_RANGE in a static table would fire
    //  on every CWorldFrame instance for the same range — dynamic binding scopes
    //  it correctly to this frame).
    LoadRecentFiles();
    Bind(wxEVT_MENU, &CWorldFrame::OnMenuRecentWorld, this,
         ID_MENU_RECENT_FIRST, ID_MENU_RECENT_LAST);

    // Give focus to the input bar
    m_input->SetFocus();
}

CWorldFrame::~CWorldFrame()
{
    m_timer->Stop();
    if (m_doc && m_doc->m_pSocket) {
        delete m_doc->m_pSocket;
        m_doc->m_pSocket = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Connect to the MUD
// ─────────────────────────────────────────────────────────────────────────────
void CWorldFrame::Connect()
{
    if (m_doc->m_pSocket) {
        delete m_doc->m_pSocket;
        m_doc->m_pSocket = nullptr;
    }

    CWorldSocket* sock = new CWorldSocket(m_doc.get());
    m_doc->m_pSocket = sock;

    if (!sock->Create()) {
        AppendNote("[Error: could not create socket]");
        return;
    }

    m_doc->m_iConnectPhase = eConnectConnecting;
    AppendNote(std::string("[Connecting to ") + m_host + "...]");

    if (!sock->Connect(m_host.c_str(), (UINT)m_port)) {
        if (sock->GetLastError() != WSAEWOULDBLOCK) {
            AppendNote("[Error: connect failed immediately]");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Poll timer — drives all socket I/O on the GUI thread
// ─────────────────────────────────────────────────────────────────────────────
void CWorldFrame::OnTimer(wxTimerEvent&)
{
    if (!m_doc->m_pSocket) return;

    // Non-blocking poll: dispatches OnConnect / OnReceive / OnSend / OnClose
    CAsyncSocket::Poll(0);

    // Update status bar with connection state
    if (m_doc->m_iConnectPhase == eConnectConnectedToMud) {
        m_status->SetStatusText(
            wxString::Format("Connected — %s:%d", m_host.c_str(), m_port), 0);
        m_status->SetStatusText(
            wxString::Format("In: %ld  Out: %ld",
                             m_doc->m_nBytesIn, m_doc->m_nBytesOut), 1);
    } else if (m_doc->m_iConnectPhase == eConnectNotConnected) {
        m_status->SetStatusText("Disconnected", 0);
        m_timer->Stop();  // stop polling when disconnected
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// User pressed Enter in the input bar
// ─────────────────────────────────────────────────────────────────────────────
void CWorldFrame::OnInputEnter(wxCommandEvent&)
{
    wxString text = m_input->GetValue();
    m_input->Clear();

    if (text.IsEmpty()) return;

    // Push to history (deduplicate consecutive identical commands)
    if (m_history.empty() || m_history.back() != text)
        m_history.push_back(text);
    m_historyPos  = -1;       // reset to "live" position
    m_historyDraft = wxEmptyString;

    SendCommand(text);
}

// ─────────────────────────────────────────────────────────────────────────────
// SendCommand — alias-check, then send to the MUD
// ─────────────────────────────────────────────────────────────────────────────
void CWorldFrame::SendCommand(const wxString& wxCmd)
{
    if (!m_doc->m_pSocket ||
        m_doc->m_iConnectPhase != eConnectConnectedToMud)
        return;

    std::string cmd = wxCmd.ToUTF8().data();

    // Echo the command to the output window (like a real terminal)
    {
        SOutputLine echoLine;
        SOutputRun  run;
        run.text = cmd;
        run.fg   = RGB(192, 192, 128);   // dim yellow for echoed input
        run.bg   = RGB(0,   0,   0);
        echoLine.push_back(std::move(run));
        AppendLine(std::move(echoLine));
    }

    // EvaluateCommand checks aliases (including plugin aliases), handles
    // speedwalk, and calls SendMsg() for each resulting line.  It returns TRUE
    // if it consumed the input (alias matched or speedwalk expanded); if it
    // returns FALSE the command was sent raw via SendMsg() internally too.
    // Either way the actual socket write is done inside EvaluateCommand /
    // SendMsg — we don't need to touch the socket here.
    bool bOmitFromLog = false;
    m_doc->EvaluateCommand(CString(cmd.c_str()), /*bCountThem=*/true,
                           bOmitFromLog, /*bTest=*/false);
}

// ─────────────────────────────────────────────────────────────────────────────
// OnInputKeyDown — Up/Down arrow command-history navigation
// ─────────────────────────────────────────────────────────────────────────────
void CWorldFrame::OnInputKeyDown(wxKeyEvent& evt)
{
    const int key = evt.GetKeyCode();

    if ((key == WXK_UP || key == WXK_DOWN) && !m_history.empty()) {
        // Save the current draft on first UP from the live line
        if (m_historyPos == -1 && key == WXK_UP)
            m_historyDraft = m_input->GetValue();

        if (key == WXK_UP) {
            if (m_historyPos == -1)
                m_historyPos = (int)m_history.size() - 1;
            else if (m_historyPos > 0)
                --m_historyPos;
        } else {  // WXK_DOWN
            if (m_historyPos == -1) {
                evt.Skip();
                return;  // already at live line, nothing to do
            }
            ++m_historyPos;
        }

        if (m_historyPos >= (int)m_history.size()) {
            // Scrolled past the end — restore the draft
            m_historyPos = -1;
            m_input->SetValue(m_historyDraft);
        } else {
            m_input->SetValue(m_history[m_historyPos]);
        }
        // Move cursor to end of restored text
        m_input->SetInsertionPointEnd();
        return;  // don't call evt.Skip() — we consumed the key
    }

    evt.Skip();  // let wxWidgets process all other keys normally
}

// ─────────────────────────────────────────────────────────────────────────────
// Append a styled line from the protocol engine to the output pane
// ─────────────────────────────────────────────────────────────────────────────
void CWorldFrame::AppendLine(SOutputLine&& line)
{
    m_pane->AppendLine(std::move(line));
}

// ─────────────────────────────────────────────────────────────────────────────
// Append a status / note message (dim grey)
// ─────────────────────────────────────────────────────────────────────────────
void CWorldFrame::AppendNote(const std::string& text)
{
    m_pane->AppendNote(text);
}

// ─────────────────────────────────────────────────────────────────────────────
// Window close handler
// ─────────────────────────────────────────────────────────────────────────────
void CWorldFrame::OnClose(wxCloseEvent& evt)
{
    m_timer->Stop();
    if (m_doc && m_doc->m_pSocket) {
        delete m_doc->m_pSocket;
        m_doc->m_pSocket = nullptr;
    }
    evt.Skip();  // let wxWidgets destroy the frame
}

// ─────────────────────────────────────────────────────────────────────────────
// World > New Connection — show connect dialog and open a new CWorldFrame
// ─────────────────────────────────────────────────────────────────────────────
// Forward-declare the dialog class (defined in wx_app.cpp)
class CConnectDialog;
void CWorldFrame::OnMenuConnect(wxCommandEvent&)
{
    // Re-use the CConnectDialog from wx_app.cpp by re-including its definition.
    // Since it's a local class there, the cleanest approach is a simple inline
    // dialog here that mirrors it — avoids a shared header for one small dialog.
    wxDialog dlg(this, wxID_ANY, "New Connection",
                 wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE);

    wxBoxSizer* top  = new wxBoxSizer(wxVERTICAL);
    wxFlexGridSizer* grid = new wxFlexGridSizer(2, 2, 8, 8);
    grid->AddGrowableCol(1, 1);

    grid->Add(new wxStaticText(&dlg, wxID_ANY, "Host:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* hostCtrl = new wxTextCtrl(&dlg, wxID_ANY, m_host,
                                    wxDefaultPosition, wxSize(260, -1),
                                    wxTE_PROCESS_ENTER);
    grid->Add(hostCtrl, 1, wxEXPAND);

    grid->Add(new wxStaticText(&dlg, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* portCtrl = new wxTextCtrl(&dlg, wxID_ANY,
                                    wxString::Format("%d", m_port),
                                    wxDefaultPosition, wxSize(80, -1),
                                    wxTE_PROCESS_ENTER);
    grid->Add(portCtrl, 0);

    top->Add(grid, 1, wxEXPAND | wxALL, 12);
    top->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL),
             0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    dlg.SetSizerAndFit(top);

    if (wxButton* btn = dynamic_cast<wxButton*>(dlg.FindWindow(wxID_OK)))
        btn->SetLabel("Connect");

    auto endOK = [&dlg](wxCommandEvent&){ dlg.EndModal(wxID_OK); };
    hostCtrl->Bind(wxEVT_TEXT_ENTER, endOK);
    portCtrl->Bind(wxEVT_TEXT_ENTER, endOK);
    hostCtrl->SetFocus();

    if (dlg.ShowModal() != wxID_OK) return;

    wxString newHost = hostCtrl->GetValue().Trim();
    long     newPort = 23;
    portCtrl->GetValue().ToLong(&newPort);

    if (newHost.IsEmpty()) return;

    CWorldFrame* frame = new CWorldFrame(newHost.ToStdString(), (int)newPort);
    frame->Show(true);
}

// ─────────────────────────────────────────────────────────────────────────────
// World > Open World File — load a .mcl file into the current doc
// ─────────────────────────────────────────────────────────────────────────────
void CWorldFrame::OnMenuOpenWorld(wxCommandEvent&)
{
    wxFileDialog dlg(this,
                     "Open World File",
                     wxEmptyString, wxEmptyString,
                     "MUSHclient World files (*.mcl)|*.mcl"
                     "|XML files (*.xml)|*.xml"
                     "|All files (*)|*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dlg.ShowModal() != wxID_OK) return;

    std::string path = dlg.GetPath().ToUTF8().data();

    // Stop polling while we reinitialise the doc
    m_timer->Stop();

    // Disconnect cleanly if connected
    if (m_doc->m_pSocket) {
        delete m_doc->m_pSocket;
        m_doc->m_pSocket = nullptr;
    }
    m_doc->m_iConnectPhase = eConnectNotConnected;

    // Re-wire output callbacks onto the fresh doc
    m_doc->m_fn_line_ready = [this](SOutputLine&& line) {
        AppendLine(std::move(line));
    };
    m_doc->m_fn_note = [this](const std::string& text) {
        AppendNote(text);
    };
    m_doc->m_fn_status = [this](const std::string& text) {
        m_status->SetStatusText(wxString::FromUTF8(text.c_str()), 1);
    };

    if (!m_doc->LoadWorldFromFile(path.c_str())) {
        AppendNote("[Error: could not load world file: " + path + "]");
        m_timer->Start(20);
        return;
    }

    // Update connection target from the world file (site / port)
    if (!m_doc->m_mush_name.IsEmpty())
        m_host = std::string((const char*)m_doc->m_mush_name);
    if (m_doc->m_port > 0)
        m_port = m_doc->m_port;

    // Update window title
    wxString title = wxString::Format("MUSHclient — %s:%d", m_host.c_str(), m_port);
    if (!m_doc->m_mush_name.IsEmpty()) {
        // Use world name in title if the file gave us one
        title = wxString::Format("MUSHclient — %s (%s:%d)",
                                  (const char*)m_doc->m_mush_name,
                                  m_host.c_str(), m_port);
    }
    SetTitle(title);

    AppendNote(std::string("[World file loaded: ") + path + "]");

    // Track in the MRU list (persisted to wxConfig)
    AddRecentFile(dlg.GetPath());

    // Reconnect with the (possibly updated) host and port
    Connect();
    m_timer->Start(20);
}

// ─────────────────────────────────────────────────────────────────────────────
// World > Reconnect
// ─────────────────────────────────────────────────────────────────────────────
void CWorldFrame::OnMenuReconnect(wxCommandEvent&)
{
    m_timer->Stop();

    if (m_doc->m_pSocket) {
        delete m_doc->m_pSocket;
        m_doc->m_pSocket = nullptr;
    }
    m_doc->m_iConnectPhase = eConnectNotConnected;

    AppendNote("[Reconnecting...]");
    Connect();
    m_timer->Start(20);
}

// ─────────────────────────────────────────────────────────────────────────────
// World > Close World
// ─────────────────────────────────────────────────────────────────────────────
void CWorldFrame::OnMenuCloseWorld(wxCommandEvent&)
{
    Close(true);
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-line paste splitter
//
// When the user pastes text that contains newlines, split it at each '\n'.
// Every line except the last is sent as a command (just as if the user had
// typed it and pressed Enter); the last fragment is left in the input bar so
// the user can review/edit it before sending.  If the pasted text has no
// newlines the default wxWidgets paste behaviour is used unchanged.
//
// Example paste of "north\nlook\ngo west":
//   → sends "north", sends "look", leaves "go west" in the input bar.
//
// Any text already typed in the input bar is prepended to the first pasted line:
//   typed "say " + paste "hello\nworld"
//   → sends "say hello", leaves "world" in input bar.
// ─────────────────────────────────────────────────────────────────────────────
void CWorldFrame::OnInputPaste(wxClipboardTextEvent& evt)
{
    wxString clipText;
    if (wxTheClipboard->Open()) {
        if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT) ||
            wxTheClipboard->IsSupported(wxDF_TEXT)) {
            wxTextDataObject data;
            wxTheClipboard->GetData(data);
            clipText = data.GetText();
        }
        wxTheClipboard->Close();
    }

    // Normalise CRLF / CR → LF
    clipText.Replace("\r\n", "\n");
    clipText.Replace("\r",   "\n");

    if (!clipText.Contains('\n')) {
        evt.Skip();   // single-line: let wxWidgets insert it normally
        return;
    }

    // Suppress the default paste — we'll handle the text ourselves
    // (evt.Veto() is not available for wxClipboardTextEvent; simply not calling
    //  evt.Skip() achieves the same effect for paste events on wxTextCtrl.)

    wxArrayString lines = wxSplit(clipText, '\n');

    // Prepend whatever the user had already typed to the first pasted line
    wxString prefix = m_input->GetValue();
    m_input->Clear();

    // Send all complete lines (everything before the final fragment)
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        wxString cmd = (i == 0) ? prefix + lines[i] : lines[i];

        // Add to command history (dedup consecutive identical entries)
        if (!cmd.IsEmpty()) {
            if (m_history.empty() || m_history.back() != cmd)
                m_history.push_back(cmd);
        }

        m_historyPos   = -1;
        m_historyDraft = wxEmptyString;

        SendCommand(cmd);
    }

    // Leave the trailing fragment in the input bar
    m_input->SetValue(lines.back());
    m_input->SetInsertionPointEnd();
}

// ─────────────────────────────────────────────────────────────────────────────
// Recent Worlds — MRU helpers
// ─────────────────────────────────────────────────────────────────────────────

// Read up to kMaxRecentWorlds paths from wxConfig and populate the submenu.
void CWorldFrame::LoadRecentFiles()
{
    wxConfig cfg("MUSHclient");
    cfg.SetPath("/RecentWorlds");

    m_recentFiles.clear();
    for (int i = 0; i < kMaxRecentWorlds; ++i) {
        wxString key = wxString::Format("file%d", i);
        wxString val;
        if (cfg.Read(key, &val) && !val.IsEmpty())
            m_recentFiles.push_back(val);
    }

    RebuildRecentMenu();
}

// Persist the current MRU list to wxConfig.
void CWorldFrame::SaveRecentFiles()
{
    wxConfig cfg("MUSHclient");
    // DeleteGroup path is relative to current group (root here — no leading '/')
    cfg.DeleteGroup("RecentWorlds");
    cfg.SetPath("/RecentWorlds");  // SetPath accepts absolute paths

    for (int i = 0; i < (int)m_recentFiles.size(); ++i) {
        cfg.Write(wxString::Format("file%d", i), m_recentFiles[i]);
    }
    cfg.Flush();
}

// Prepend path to the MRU list (de-duplicate, cap at kMaxRecentWorlds),
// rebuild the submenu, and save.
void CWorldFrame::AddRecentFile(const wxString& path)
{
    // Remove any existing entry for this path (case-sensitive)
    m_recentFiles.erase(
        std::remove(m_recentFiles.begin(), m_recentFiles.end(), path),
        m_recentFiles.end());

    // Newest at front
    m_recentFiles.insert(m_recentFiles.begin(), path);

    // Cap at maximum
    if ((int)m_recentFiles.size() > kMaxRecentWorlds)
        m_recentFiles.resize(kMaxRecentWorlds);

    RebuildRecentMenu();
    SaveRecentFiles();
}

// Repopulate m_recentMenu from m_recentFiles.
// The submenu is owned by the menu bar; we can safely destroy and re-add items.
void CWorldFrame::RebuildRecentMenu()
{
    // Clear all existing items
    while (m_recentMenu->GetMenuItemCount() > 0)
        m_recentMenu->Delete(m_recentMenu->FindItemByPosition(0));

    if (m_recentFiles.empty()) {
        m_recentMenu->Append(wxID_ANY, "(none)")->Enable(false);
        return;
    }

    for (int i = 0; i < (int)m_recentFiles.size(); ++i) {
        // Show just the filename in the menu label; full path in the help string
        wxFileName fn(m_recentFiles[i]);
        wxString label = wxString::Format("&%d  %s", i + 1, fn.GetFullName());
        m_recentMenu->Append(ID_MENU_RECENT_FIRST + i, label,
                             m_recentFiles[i]);  // full path as status bar hint
    }
}

// Handler for World > Recent Worlds > item N
void CWorldFrame::OnMenuRecentWorld(wxCommandEvent& evt)
{
    int idx = evt.GetId() - ID_MENU_RECENT_FIRST;
    if (idx < 0 || idx >= (int)m_recentFiles.size()) return;

    wxString path = m_recentFiles[idx];

    // ── Mirror the logic from OnMenuOpenWorld ────────────────────────────────
    m_timer->Stop();

    if (m_doc->m_pSocket) {
        delete m_doc->m_pSocket;
        m_doc->m_pSocket = nullptr;
    }
    m_doc->m_iConnectPhase = eConnectNotConnected;

    m_doc->m_fn_line_ready = [this](SOutputLine&& line) {
        AppendLine(std::move(line));
    };
    m_doc->m_fn_note = [this](const std::string& text) {
        AppendNote(text);
    };
    m_doc->m_fn_status = [this](const std::string& text) {
        m_status->SetStatusText(wxString::FromUTF8(text.c_str()), 1);
    };

    std::string pathStd = path.ToUTF8().data();
    if (!m_doc->LoadWorldFromFile(pathStd.c_str())) {
        AppendNote("[Error: could not load world file: " + pathStd + "]");
        // Move the bad entry to the end so it doesn't block good ones,
        // but keep it in the list (user may fix the path)
        m_timer->Start(20);
        return;
    }

    if (!m_doc->m_mush_name.IsEmpty())
        m_host = std::string((const char*)m_doc->m_mush_name);
    if (m_doc->m_port > 0)
        m_port = m_doc->m_port;

    wxString title = wxString::Format("MUSHclient — %s:%d", m_host.c_str(), m_port);
    if (!m_doc->m_mush_name.IsEmpty())
        title = wxString::Format("MUSHclient — %s (%s:%d)",
                                  (const char*)m_doc->m_mush_name,
                                  m_host.c_str(), m_port);
    SetTitle(title);

    AppendNote("[World file loaded: " + pathStd + "]");

    // Bump to top of MRU (it was already there unless loaded from a stale list)
    AddRecentFile(path);

    Connect();
    m_timer->Start(20);
}

#endif // !_WIN32
