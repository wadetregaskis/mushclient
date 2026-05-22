#pragma once
// platform/wx_frame.h — MUSHclient main GUI frame (Phase 5)
//
// CWorldFrame drives the telnet/ANSI/MXP protocol engine via a wxTimer
// (20 ms poll) that calls CAsyncSocket::Poll() on the GUI thread.
// Styled output from DisplayMsg() is routed through m_fn_line_ready and
// appended to COutputPane via AppendLine().

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/timer.h>
#include <memory>
#include <string>
#include <vector>

#include "platform/wx_outputpane.h"   // COutputPane

// Maximum number of recent world files remembered in the World menu
static constexpr int kMaxRecentWorlds = 9;

// Forward-declared from doc_stub.h / worldsock.h
class CMUSHclientDoc;
class CWorldSocket;

// ─────────────────────────────────────────────────────────────────────────────
// CWorldFrame
// ─────────────────────────────────────────────────────────────────────────────
class CWorldFrame : public wxFrame
{
public:
    CWorldFrame(const std::string& host, int port);
    ~CWorldFrame();

private:
    // ── Widgets ──────────────────────────────────────────────────────────────
    COutputPane* m_pane   = nullptr;  // scrollback / output area (custom)
    wxTextCtrl*  m_input  = nullptr;  // command input bar
    wxStatusBar* m_status = nullptr;

    // ── Protocol engine ──────────────────────────────────────────────────────
    std::unique_ptr<CMUSHclientDoc> m_doc;
    std::string  m_host;
    int          m_port;

    // ── Poll timer — drives socket I/O on the GUI thread ─────────────────────
    std::unique_ptr<wxTimer> m_timer;

    // ── Command history (Up/Down arrow) ──────────────────────────────────────
    std::vector<wxString> m_history;   // past commands, oldest first
    int       m_historyPos = -1;       // -1 = at the "live" input line
    wxString  m_historyDraft;          // saved draft when browsing history

    // ── Recent worlds MRU (World > Recent Worlds submenu) ────────────────────
    std::vector<wxString> m_recentFiles;  // paths, newest first, max kMaxRecentWorlds
    wxMenu*               m_recentMenu = nullptr;  // owned by menu bar

    void LoadRecentFiles();            // read from wxConfig on startup
    void SaveRecentFiles();            // persist to wxConfig after any change
    void AddRecentFile(const wxString& path);  // prepend + dedup + refresh menu
    void RebuildRecentMenu();          // repopulate m_recentMenu from m_recentFiles

    // ── Networking ───────────────────────────────────────────────────────────
    void Connect();
    void SendCommand(const wxString& cmd);  // alias-check then send to MUD

    // ── Output helpers ────────────────────────────────────────────────────────
    void AppendLine(SOutputLine&& line);
    void AppendNote(const std::string& text);

    // ── Event handlers ────────────────────────────────────────────────────────
    void OnInputEnter(wxCommandEvent& evt);
    void OnInputKeyDown(wxKeyEvent& evt);
    void OnInputPaste(wxClipboardTextEvent& evt);  // multi-line paste splitter
    void OnTimer(wxTimerEvent& evt);
    void OnClose(wxCloseEvent& evt);
    void OnMenuConnect(wxCommandEvent& evt);
    void OnMenuOpenWorld(wxCommandEvent& evt);
    void OnMenuRecentWorld(wxCommandEvent& evt);  // one of the MRU entries
    void OnMenuReconnect(wxCommandEvent& evt);
    void OnMenuCloseWorld(wxCommandEvent& evt);

    wxDECLARE_EVENT_TABLE();
};

#endif // !_WIN32
