// platform/wx_world_child.cpp — MDI child frame: output pane + command input.
//
// Include order is critical (same rule as wx_mfc_impl.cpp):
//   1. wx headers first (so wx's min/max, MessageBox etc. are not shadowed)
//   2. MUSHclient / MFC-compat headers after

#ifndef _WIN32

// ── wxWidgets ─────────────────────────────────────────────────────────────
#include <wx/wx.h>
#include <wx/mdi.h>
#include <wx/config.h>
#include <wx/clipbrd.h>   // wxTheClipboard, wxTextDataObject, wxDF_TEXT
#include <string>

// ── MUSHclient headers ────────────────────────────────────────────────────
// stdafx.h and MUSHclient.h must precede doc.h: MUSHclient.h pulls in
// OtherTypes.h (MACRO_COUNT, CAliasMap, CTriggerMap, …) that doc.h needs.
#include "stdafx.h"                   // compat.h → mfc_ui.h → wx_mfc_hooks.h
#include "MUSHclient.h"               // CMUSHclientApp + OtherTypes.h + all prerequisites
#include "doc.h"                      // CMUSHclientDoc, SendMsg, etc.
#include "miniwindow.h"               // CMiniWindow, MiniWindowVector, MINIWINDOW_* flags
#include "scripting/scripting.h"      // CScriptEngine, lua_State* — for Shift+Tab globals
#include "platform/wx_world_child.h"  // our own header (CWorldChildFrame, COutputPane)

// MW_MOUSE_* flag constants (from mushview.h — copied here to avoid pulling in
// all of mushview.h which declares the Windows-only CMUSHView class).
#ifndef MW_MOUSE_SHIFT
#  define MW_MOUSE_SHIFT       0x01
#  define MW_MOUSE_CTRL        0x02
#  define MW_MOUSE_ALT         0x04
#  define MW_MOUSE_LH          0x10
#  define MW_MOUSE_RH          0x20
#  define MW_MOUSE_DBL         0x40
#  define MW_MOUSE_NOT_FIRST   0x80
#endif
#include "platform/wx_gdi_compat.h"   // WxDCFromCDC
#include "platform/wx_mfc_hooks.h"    // g_wxhook_set_status

#include <list>
#include <set>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>

// App singleton — defined in platform/mushapp_globals.cpp for the mushapp build.
extern CMUSHclientApp App;

// ── Event IDs ─────────────────────────────────────────────────────────────
static const int ID_INPUT        = wxID_HIGHEST + 200;
static const int ID_WORLD_TIMER  = wxID_HIGHEST + 201;
static const int ID_FIND_INPUT   = wxID_HIGHEST + 202;
static const int ID_FIND_NEXT    = wxID_HIGHEST + 203;
static const int ID_FIND_PREV    = wxID_HIGHEST + 204;

// Maximum commands kept in history (matches MUSHclient Windows default)
static const int kMaxHistory = 200;

// ── History file path ─────────────────────────────────────────────────────────
// Returns the path to the per-world persistent command-history file, creating
// ~/.mushclient/ if needed.  Uses the world name to distinguish files so that
// different worlds maintain separate histories.  Returns "" on failure.
//
// The world name is sanitised: only [A-Za-z0-9._-] are kept; runs of other
// characters are collapsed to a single underscore.  E.g. "Broken Lands"
// → "history_Broken_Lands.txt".  Falls back to "history.txt" if empty.
static std::string GetHistoryFilePath(const std::string& worldName)
{
    const char* home = getenv("HOME");
    if (!home || !*home) return {};
    std::string dir = std::string(home) + "/.mushclient";
    ::mkdir(dir.c_str(), 0700);   // safe no-op if already exists

    if (worldName.empty()) return dir + "/history.txt";

    // Sanitise: keep alphanumeric + a handful of safe punctuation chars;
    // collapse anything else to a single underscore.
    std::string safe;
    for (unsigned char c : worldName) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            safe += (char)c;
        } else {
            if (!safe.empty() && safe.back() != '_')
                safe += '_';
        }
    }
    // Trim trailing underscores left by a name ending in punctuation.
    while (!safe.empty() && safe.back() == '_') safe.pop_back();
    if (safe.empty()) safe = "default";

    return dir + "/history_" + safe + ".txt";
}

// ── Event table ───────────────────────────────────────────────────────────
wxBEGIN_EVENT_TABLE(CWorldChildFrame, wxMDIChildFrame)
    EVT_TEXT_ENTER(ID_INPUT,              CWorldChildFrame::OnInputEnter)
    EVT_TIMER(     ID_WORLD_TIMER,        CWorldChildFrame::OnWorldTimer)
    EVT_CLOSE(                            CWorldChildFrame::OnClose)
    EVT_SIZE(                             CWorldChildFrame::OnSize)
    EVT_ACTIVATE(                         CWorldChildFrame::OnActivate)
wxEND_EVENT_TABLE()

// ── Constructor ───────────────────────────────────────────────────────────
CWorldChildFrame::CWorldChildFrame(wxMDIParentFrame* parent,
                                   CMUSHclientDoc*   doc,
                                   const wxString&   title)
    : wxMDIChildFrame(parent, wxID_ANY, title,
                      wxDefaultPosition, wxSize(800, 600))
    , m_doc(doc)
    , m_worldTimer(nullptr)
{
    // Vertical sizer: output pane takes all space; input bar at bottom.
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    m_output = new COutputPane(this);
    sizer->Add(m_output, 1 /*stretch*/, wxEXPAND);

    // ── Find bar (hidden by default, shown by Ctrl+F) ─────────────────────
    m_findBar = new wxPanel(this, wxID_ANY);
    {
        wxBoxSizer* fs = new wxBoxSizer(wxHORIZONTAL);
        fs->Add(new wxStaticText(m_findBar, wxID_ANY, wxT("Find:")),
                0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 6);
        m_findInput = new wxTextCtrl(m_findBar, ID_FIND_INPUT, wxEmptyString,
                                     wxDefaultPosition, wxSize(220, -1),
                                     wxTE_PROCESS_ENTER);
        fs->Add(m_findInput, 0, wxALIGN_CENTER_VERTICAL);
        wxButton* btnNext = new wxButton(m_findBar, ID_FIND_NEXT, wxT("▼"),
                                         wxDefaultPosition, wxSize(28, -1));
        wxButton* btnPrev = new wxButton(m_findBar, ID_FIND_PREV, wxT("▲"),
                                         wxDefaultPosition, wxSize(28, -1));
        fs->Add(btnNext, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
        fs->Add(btnPrev, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);
        // Case-sensitive toggle ("Aa").  Restore the last user preference.
        m_findCaseBox = new wxCheckBox(m_findBar, wxID_ANY, wxT("Aa"));
        m_findCaseBox->SetToolTip(wxT("Case-sensitive search"));
        {
            wxConfig cfg(wxT("mushclient"));
            bool savedCase = false;
            cfg.Read(wxT("/find/case_sensitive"), &savedCase, false);
            m_findCaseBox->SetValue(savedCase);
        }
        fs->Add(m_findCaseBox, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
        // Match-count label ("3 of 47" or "Not found").
        m_findLabel = new wxStaticText(m_findBar, wxID_ANY, wxEmptyString,
                                       wxDefaultPosition, wxSize(90, -1));
        fs->Add(m_findLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
        int idFindClose = wxID_HIGHEST + 205;
        wxButton* btnClose = new wxButton(m_findBar, idFindClose, wxT("✕"),
                                          wxDefaultPosition, wxSize(28, -1));
        fs->Add(btnClose, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
        m_findBar->SetSizer(fs);

        // Enter / Shift+Enter in the find box: find next / previous.
        m_findInput->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
            DoFind(true);
        });
        m_findInput->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            // Live search: highlight as the user types; clear stale count.
            bool cs = m_findCaseBox && m_findCaseBox->IsChecked();
            m_output->HighlightSearch(m_findInput->GetValue(), cs);
            if (m_findLabel) m_findLabel->SetLabel(wxEmptyString);
        });
        // Case toggle: re-highlight immediately, reset stale count, and persist.
        m_findCaseBox->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            bool cs = m_findCaseBox->IsChecked();
            m_output->HighlightSearch(m_findInput ? m_findInput->GetValue() : wxString(), cs);
            if (m_findLabel) m_findLabel->SetLabel(wxEmptyString);
            wxConfig cfg(wxT("mushclient"));
            cfg.Write(wxT("/find/case_sensitive"), cs);
        });
        m_findInput->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
            if (e.GetKeyCode() == WXK_ESCAPE) {
                ShowFindBar(false);
            } else if (e.GetKeyCode() == WXK_RETURN && e.ShiftDown()) {
                DoFind(false);
            } else {
                e.Skip();
            }
        });
        btnNext->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DoFind(true);  });
        btnPrev->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DoFind(false); });
        btnClose->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowFindBar(false); });
    }
    sizer->Add(m_findBar, 0, wxEXPAND);
    m_findBar->Hide();

    // Return keyboard focus to the input bar after a mouse selection so the
    // user can keep typing without clicking back in the input bar.
    m_output->m_fn_return_focus = [this]() {
        wxTextCtrl* inp = ActiveInput();
        if (inp) inp->SetFocus();
    };

    // world.Pause(bPause): Lua scripts can scroll-lock or unlock the pane.
    if (m_doc) {
        m_doc->m_fn_set_paused = [this](bool bPause) {
            if (m_output) m_output->SetPaused(bPause);
        };

        // world.Repaint(): force a repaint of the output pane.
        m_doc->m_fn_repaint = [this]() {
            if (m_output) m_output->Refresh(false);
        };

        // world.SetStatus(msg): show a message in the MDI parent's status bar.
        // This overrides the connection-state message until the next timer tick
        // refreshes it, or until a new world.SetStatus() call replaces it.
        m_doc->m_fn_status = [this](const std::string& msg) {
            if (g_wxhook_set_status && IsActiveWorld())
                g_wxhook_set_status(msg.c_str());
        };

        // world.SetTitle(s): update the MDI child-frame title bar.
        // MUD scripts commonly call this to show HP/MP/status in the title bar.
        m_doc->m_fn_set_title = [this](const std::string& title) {
            wxString wxTitle = wxString::FromUTF8(title.c_str());
            SetTitle(wxTitle.IsEmpty()
                     ? wxString::FromUTF8(
                           static_cast<const char*>(m_doc->m_mush_name))
                     : wxTitle);
        };

        // world.SetMainTitle(s): update the main application window title.
        m_doc->m_fn_set_main_title = [](const std::string& title) {
            // g_wxhook_load_mdi_frame sets s_wxMainFrame; look for it via the App.
            if (wxFrame* top = dynamic_cast<wxFrame*>(wxTheApp->GetTopWindow()))
                top->SetTitle(wxString::FromUTF8(title.c_str()));
        };

        // world.SetBackgroundColour(rgb): update output pane background.
        m_doc->m_fn_set_background_colour = [this](unsigned long col) {
            ApplyPaneBackground(col);
        };

        // SetOption("max_output_lines", n): resize the scrollback buffer.
        m_doc->m_fn_set_max_lines = [this](int n) {
            if (m_output) m_output->SetMaxLines(n);
        };

        // world.DeleteLines(n): remove the last n lines from the visual buffer.
        m_doc->m_fn_delete_lines = [this](long n) {
            if (m_output) m_output->DeleteTailLines(n);
        };

        // world.SelectCommand(): select all text in the command input bar.
        m_doc->m_fn_select_command = [this]() {
            wxTextCtrl* inp = ActiveInput();
            if (inp) inp->SetSelection(-1, -1);  // wxWidgets: -1,-1 = select all
        };

        // world.DeleteCommandHistory(): clear the input history.
        m_doc->m_fn_delete_command_history = [this]() {
            m_history.clear();
            m_histIdx = -1;
            m_histSaved.Clear();
            // Rewrite (empty) history file so the cleared history persists.
            if (!m_historyFilePath.empty()) {
                FILE* f = fopen(m_historyFilePath.c_str(), "w");
                if (f) fclose(f);
            }
        };

        // world.PushCommand(): save current bar text to history, clear bar.
        m_doc->m_fn_push_command = [this]() -> std::string {
            wxTextCtrl* inp = ActiveInput();
            if (!inp) return {};
            std::string text(inp->GetValue().ToUTF8());
            if (!text.empty()) {
                wxString wxText = wxString::FromUTF8(text.c_str());
                if (m_history.empty() || m_history.back() != wxText)
                    m_history.push_back(wxText);
                if ((int)m_history.size() > kMaxHistory)
                    m_history.erase(m_history.begin());
            }
            inp->Clear();
            m_histIdx = -1;
            ApplyHistRSearch(false);
            return text;
        };

        // world.GetCommandList(count): return up to count most-recent history
        // entries, newest first.  count <= 0 means all.
        m_doc->m_fn_get_command_list = [this](long count) -> std::vector<std::string> {
            std::vector<std::string> result;
            if (m_history.empty()) return result;
            long total = (long)m_history.size();
            long n = (count <= 0) ? total : std::min(count, total);
            result.reserve((size_t)n);
            for (long i = total - 1; i >= total - n; --i)
                result.push_back(std::string(m_history[(size_t)i].ToUTF8()));
            return result;
        };

        // GetInfo(236/237): query command input selection range.
        m_doc->m_fn_get_command_selection = [this](long& selStart, long& selEnd) {
            wxTextCtrl* inp = ActiveInput();
            if (!inp) { selStart = selEnd = 0; return; }
            long from = 0, to = 0;
            inp->GetSelection(&from, &to);
            selStart = from;
            selEnd   = to;
        };

        // world.GetSelectionStartLine / GetSelectionEndLine / …Column:
        // let Lua scripts query what text is currently selected in the output pane.
        m_doc->m_fn_get_output_selection = [this](long& sl, long& sc,
                                                   long& el, long& ec) -> bool {
            if (!m_output) return false;
            return m_output->GetSelectionInfo(sl, sc, el, ec);
        };

        // world.GetClipboard() / world.SetClipboard(): system clipboard access.
        m_doc->m_fn_get_clipboard = []() -> std::string {
            std::string result;
            if (wxTheClipboard->Open()) {
                if (wxTheClipboard->IsSupported(wxDF_TEXT)) {
                    wxTextDataObject data;
                    wxTheClipboard->GetData(data);
                    result = std::string(data.GetText().ToUTF8());
                }
                wxTheClipboard->Close();
            }
            return result;
        };
        m_doc->m_fn_set_clipboard = [](const std::string& text) {
            if (wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(
                    wxString::FromUTF8(text.c_str())));
                wxTheClipboard->Close();
            }
        };

        // world.GetWorldWindowPosition(): return screen rect of the output pane.
        // Scripts use this to position miniwindows relative to the output area.
        m_doc->m_fn_get_output_rect = [this](int& x, int& y, int& w, int& h) {
            if (m_output) {
                wxPoint pos = m_output->ClientToScreen(wxPoint(0, 0));
                wxSize  sz  = m_output->GetClientSize();
                x = pos.x; y = pos.y; w = sz.x; h = sz.y;
            } else {
                x = 0; y = 0; w = 800; h = 600;
            }
        };

        // world.GetMainWindowPosition(): return screen rect of the MDI parent frame.
        m_doc->m_fn_get_main_rect = [this](int& x, int& y, int& w, int& h) {
            wxMDIParentFrame* parent = wxDynamicCast(GetParent(), wxMDIParentFrame);
            if (parent) {
                wxRect r = parent->GetScreenRect();
                x = r.x; y = r.y; w = r.width; h = r.height;
            } else {
                x = 0; y = 0; w = 1024; h = 768;
            }
        };

        // world.ActivateClient(): restore the main frame if iconized and raise it.
        m_doc->m_fn_activate_client = [this]() {
            wxMDIParentFrame* parent = wxDynamicCast(GetParent(), wxMDIParentFrame);
            if (parent) {
                if (parent->IsIconized()) parent->Restore();
                parent->Raise();
            }
            // Also bring this child frame to the front within the MDI.
            Activate();
        };

        // world.GetCommand(): return the text currently in the command input bar.
        m_doc->m_fn_get_command = [this]() -> std::string {
            wxTextCtrl* inp = ActiveInput();
            if (!inp) return {};
            return std::string(inp->GetValue().ToUTF8());
        };

        // world.SetCommand(text): set the command bar contents.
        // Per the Windows API contract, returns false (eCommandNotEmpty) and does
        // NOT overwrite if the bar is already non-empty.  Returns true on success.
        m_doc->m_fn_set_command = [this](const std::string& text) -> bool {
            wxTextCtrl* inp = ActiveInput();
            if (!inp) return false;
            if (!inp->GetValue().IsEmpty()) return false;  // eCommandNotEmpty
            inp->SetValue(wxString::FromUTF8(text.c_str()));
            inp->SetInsertionPointEnd();
            m_histIdx = -1; ApplyHistRSearch(false);
            return true;
        };

        // world.SetCommandSelection(first, last): select a range in the command bar.
        // Indices are 1-based character positions (0 = end of bar).
        m_doc->m_fn_set_command_selection = [this](long first, long last) {
            wxTextCtrl* inp = ActiveInput();
            if (!inp) return;
            long len = (long)inp->GetValue().size();
            long f = (first  <= 0) ? len : first  - 1;
            long l = (last   <= 0) ? len : last   - 1;
            inp->SetSelection(f, l);
        };

        // world.SetOutputFont(face, pts, bold, italic): change the output pane font.
        // pts == 0 means "keep current size".
        m_doc->m_fn_set_output_font = [this](const std::string& face, int pts,
                                              bool bold, bool italic) {
            if (!m_output) return;
            wxFont cur = m_output->GetCurrentFont();
            wxString faceName = face.empty()
                ? cur.GetFaceName()
                : wxString::FromUTF8(face.c_str());
            int size = (pts > 0) ? pts : cur.GetPointSize();
            wxFontStyle  style  = italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL;
            wxFontWeight weight = bold   ? wxFONTWEIGHT_BOLD  : wxFONTWEIGHT_NORMAL;
            wxFont f(size, wxFONTFAMILY_TELETYPE, style, weight,
                     /*underline=*/false, faceName);
            if (!f.IsOk()) f = cur;  // fall back to current font on failure
            m_output->SetOutputFont(f);
            // Update doc font fields so Ctrl+S saves the new font to the world file.
            {
                std::string faceStr = f.GetFaceName().ToUTF8().data();
                m_doc->m_font_name   = CString(faceStr.c_str());
                m_doc->m_font_height = -(f.GetPointSize() * 96 / 72);
                m_doc->m_font_weight = (f.GetWeight() >= wxFONTWEIGHT_BOLD) ? 700 : 400;
            }
            // Persist face name and size so they survive restart even without saving.
            wxConfig cfg(wxT("mushclient"));
            cfg.Write(wxT("/display/font_name"), faceName);
            cfg.Write(wxT("/display/font_size"), (long)f.GetPointSize());
            // Keep input bars in sync.
            if (m_input)         { m_input->SetFont(f);         m_input->Refresh(); }
            if (m_inputPassword) { m_inputPassword->SetFont(f); m_inputPassword->Refresh(); }
            Layout();
        };

        // world.SetScroll(line): scroll the output pane to a specific line number.
        // Line is 1-based; negative values scroll relative to the end.
        // pos == -1 → bottom (auto-scroll); pos == 0 → top.
        m_doc->m_fn_set_scroll = [this](long pos) {
            if (!m_output) return;
            if (pos <= 0) {
                m_output->ScrollToTop();
            } else {
                // Convert 1-based → 0-based.
                m_output->ScrollToLine((int)(pos - 1));
            }
        };

        // world.SetSelection(sl, el, sc, ec): set text selection in the output pane.
        // Lines and columns are 1-based; 0 means start/end of line.
        // All-zeros clears the selection.
        m_doc->m_fn_set_selection = [this](int sl, int el, int sc, int ec) {
            if (m_output) m_output->SetSelectionRange(sl, el, sc, ec);
        };

        // ── Output display callbacks ──────────────────────────────────────
        // Route the engine's text output to the pane.  Wired here so they are
        // never accidentally overwritten after construction.
        m_doc->m_fn_line_ready = [this](SOutputLine line) {
            if (m_output) m_output->AppendLine(std::move(line));
        };
        m_doc->m_fn_note = [this](const std::string& text) {
            if (m_output) m_output->AppendNote(text);
        };
        m_doc->m_fn_begin_batch = [this]() {
            if (m_output) m_output->BeginBatch();
        };
        m_doc->m_fn_flush = [this]() {
            if (m_output) m_output->EndBatch();
        };

        // ── Apply initial pane state from the doc ─────────────────────────
        // For quick-connect worlds the doc fields are at defaults (no face,
        // black background, 5000-line scrollback).  For world-file worlds the
        // doc has already been loaded via OnOpenDocument before the constructor
        // runs, so these reflect the saved world settings.
        if (m_output) {
            // Font: m_font_name is face name; m_font_height is LOGFONT lfHeight
            // (negative pixels at 96 DPI).  Skip if not configured.
            const char* face = static_cast<const char*>(m_doc->m_font_name);
            int         lfH  = (int)m_doc->m_font_height;
            if (face && *face && lfH != 0) {
                int pixH   = (lfH < 0) ? -lfH : lfH;
                int points = (pixH * 72 + 48) / 96;   // round to nearest pt
                points = std::max(COutputPane::kMinFontSize,
                                  std::min(COutputPane::kMaxFontSize, points));
                wxFontWeight weight = (m_doc->m_font_weight >= 700)
                                      ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL;
                wxFont font(points, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                            weight, false, wxString::FromUTF8(face));
                if (font.IsOk()) m_output->SetOutputFont(font);
            }
            // Scrollback limit.
            if (m_doc->m_maxlines > 0) m_output->SetMaxLines(m_doc->m_maxlines);
            // Background colour (NO_COLOUR = 0xFFFFFFFF → black).
            ApplyPaneBackground(m_doc->m_iBackgroundColour);
        }
    }

    // "Put in Input Bar" context-menu action: paste selected output text into
    // the command input for review/editing before the user presses Enter.
    m_output->m_fn_put_in_input = [this](const wxString& text) {
        wxTextCtrl* inp = ActiveInput();
        if (inp) {
            inp->SetValue(text);
            inp->SetInsertionPointEnd();
            inp->SetFocus();
        }
    };

    // "Send to MUD" context-menu action: evaluate the selected text as a command
    // through the normal alias/trigger pipeline, exactly as if typed and Enter pressed.
    m_output->m_fn_send_to_mud = [this](const wxString& text) {
        if (!m_doc || text.IsEmpty()) return;
        CString cmd(text.ToUTF8().data());
        bool bOmit = false;
        m_doc->EvaluateCommand(cmd, /*countThem=*/true, bOmit);
        m_doc->UpdateAllViews(nullptr);
    };

    // ── Session logging callbacks ──────────────────────────────────────────
    // Each line appended to the output pane is forwarded to LogLine() so that
    // the log file always reflects exactly what the user sees on screen.
    m_output->m_fn_on_line_appended = [this](const SOutputLine& line) {
        LogLine(line);
    };

    // Tells the context menu whether a log is currently open.
    m_output->m_fn_is_logging = [this]() -> bool {
        return IsLogging();
    };

    // Toggle logging on/off: shows a save dialog when starting.
    m_output->m_fn_toggle_log = [this]() {
        if (IsLogging()) {
            StopLogging();
            if (m_output)
                m_output->AppendNote("** Logging stopped **", RGB(200, 200, 0));
        } else {
            // Default filename: WORLDNAME_YYYYMMDD_HHMMSS.log
            std::string worldName = m_doc
                ? static_cast<const char*>(m_doc->m_mush_name) : "world";
            time_t now = time(nullptr);
            char ts[32];
            strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&now));

            // Sanitise worldName for use in filename (same rules as history).
            std::string safe;
            for (unsigned char c : worldName) {
                if (std::isalnum(c) || c == '-' || c == '_' || c == '.') safe += (char)c;
                else if (!safe.empty() && safe.back() != '_') safe += '_';
            }
            while (!safe.empty() && safe.back() == '_') safe.pop_back();
            if (safe.empty()) safe = "world";

            std::string defName = safe + "_" + ts + ".log";
            const char* home = getenv("HOME");
            std::string defDir = home ? std::string(home) + "/Documents" : ".";

            wxFileDialog dlg(this,
                             wxT("Save Session Log"),
                             wxString::FromUTF8(defDir.c_str()),
                             wxString::FromUTF8(defName.c_str()),
                             wxT("Log files (*.log;*.txt)|*.log;*.txt|All files (*.*)|*.*"),
                             wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (dlg.ShowModal() != wxID_OK) return;

            std::string path = dlg.GetPath().ToUTF8().data();
            if (StartLogging(path)) {
                if (m_output)
                    m_output->AppendNote(
                        "** Logging to " + path + " **", RGB(200, 200, 0));
            } else {
                wxMessageBox(
                    wxString::FromUTF8(("Could not open log file:\n" + path).c_str()),
                    wxT("Logging Error"), wxOK | wxICON_ERROR, this);
            }
        }
    };

    // F3 / Shift+F3 from the output pane fires the find bar.
    m_output->m_fn_find_next_key = [this](bool fwd) {
        ShowFindBar(true);
        DoFind(fwd);
    };

    // Forward printable characters typed in the output pane to the input bar
    // so the user can type without clicking back into it after scrolling.
    m_output->m_fn_key_to_input = [this](wxChar ch) {
        wxTextCtrl* inp = ActiveInput();
        if (!inp) return;
        inp->AppendText(wxString(ch));
        inp->SetFocus();
        inp->SetInsertionPointEnd();
    };

    // When the auto-scroll state changes, update the status bar.
    // "↑ Scrolled back" reminds the user that new output is not visible yet.
    // Restoring auto-scroll shows the connection status again.
    m_output->m_fn_autoscroll_changed = [this](bool autoNow) {
        if (m_doc) m_doc->m_gui_paused = !autoNow;
        if (IsActiveWorld())
            UpdateStatusBar();
    };

    // When the font zoom changes, the character grid changes without a resize
    // event.  Use CallAfter so the new metrics are already committed before we
    // read CharWidth()/LineHeight() and send NAWS.
    // We also persist the new font size so it survives across restarts.
    m_output->m_fn_grid_changed = [this]() {
        // Persist font size immediately (synchronous — size is already committed).
        {
            wxConfig cfg(wxT("mushclient"));
            cfg.Write(wxT("/display/font_size"), (long)m_output->FontSize());
        }
        // Keep the input bars in sync with the output pane font so Ctrl+scroll
        // zoom also updates the input bar typeface.
        {
            const wxFont& outFont = m_output->GetCurrentFont();
            if (m_input)         { m_input->SetFont(outFont);         m_input->Refresh(); }
            if (m_inputPassword) { m_inputPassword->SetFont(outFont); m_inputPassword->Refresh(); }
            Layout();  // re-measure input bar height for the new font size
        }
        CallAfter([this]() {
            if (!m_doc || !m_output) return;
            int charW = m_output->CharWidth();
            int lineH = m_output->LineHeight();
            if (charW <= 0 || lineH <= 0) return;
            int paneW, paneH;
            m_output->GetClientSize(&paneW, &paneH);
            if (paneW <= 0 || paneH <= 0) return;
            int cols = paneW / charW;
            int rows = paneH / lineH;
            m_doc->m_nWrapColumn    = (unsigned short)std::max(1, std::min(cols, 65535));
            m_doc->m_gui_cols       = cols;
            m_doc->m_gui_rows       = rows;
            m_doc->m_gui_font_height = lineH;
            m_doc->m_gui_font_width  = charW;
            // Also update the Windows-API font metric fields so GetInfo(212/213)
            // returns real values in the mushapp (mushgui) build.
            m_doc->m_FontHeight      = lineH;
            m_doc->m_FontWidth       = charW;
            m_doc->m_InputFontHeight = lineH;
            m_doc->m_InputFontWidth  = charW;
            m_doc->SendNAWS(cols, rows);
            // Persist face name + LOGFONT height so font zoom survives save/reload.
            const wxFont& zf = m_output->GetCurrentFont();
            if (zf.IsOk()) {
                std::string faceStr = zf.GetFaceName().ToUTF8().data();
                m_doc->m_font_name   = CString(faceStr.c_str());
                m_doc->m_font_height = -(zf.GetPointSize() * 96 / 72);
                m_doc->m_font_weight = (zf.GetWeight() >= wxFONTWEIGHT_BOLD) ? 700 : 400;
            }
        });
    };

    // ── Miniwindow compositing callback ───────────────────────────────────
    // Called from COutputPane::OnPaint() twice per frame: once before text
    // (bUnder=true, MINIWINDOW_DRAW_UNDERNEATH) and once after (bUnder=false).
    // The DC is already positioned at client origin (scroll removed by caller).
    // We implement our own rectangle calculation here rather than calling the
    // Windows-only CMUSHView::Calculate_MiniWindow_Rectangles().
    m_output->m_fn_composite_miniwindows =
        [this](wxDC& destDC, int clientW, int clientH, bool bUnder)
    {
        if (!m_doc) return;
        MiniWindowVector& order = m_doc->m_MiniWindowsOrder;
        if (order.empty()) return;

        // ── Phase 1: compute m_rect for ALL visible miniwindows ──────────
        // Mirrors CMUSHView::Calculate_MiniWindow_Rectangles() which runs
        // once for ALL windows before any blit pass — the underneath/on-top
        // flag does not affect position calculation, only rendering order.
        // We skip Phase 1 on the second pass (bUnder=false) to avoid
        // overwriting rects that were correctly computed in the first pass.
        // "frame" rect = this child frame; "client" rect = output pane.
        if (bUnder) {
        int frameW, frameH;
        GetClientSize(&frameW, &frameH);

        // Track four corner points exactly as the Windows implementation does.
        // pt_topleft: grows right/down as top-left corners are placed.
        // pt_topright: grows left/down as top-right corners are placed.
        // pt_bottomright: grows left/up as bottom-right corners are placed.
        // pt_bottomleft: grows right/up as bottom-left corners are placed.
        int tl_x = 0,       tl_y = 0;
        int tr_x = clientW, tr_y = 0;
        int br_x = clientW, br_y = clientH;
        int bl_x = 0,       bl_y = clientH;

        // Centered-wall deferred lists.
        std::list<CMiniWindow*> top_ones, right_ones, bottom_ones, left_ones;
        long top_widths = 0, right_widths = 0, bottom_widths = 0, left_widths = 0;

        for (auto& kv : order) {
            CMiniWindow* mw = kv.second;
            mw->m_bTemporarilyHide = false;

            if (!mw->GetShow()) continue;
            // Position calculation is independent of underneath/on-top status:
            // process ALL visible windows so corners/wall positions are global.

            long iW = mw->GetWidth(), iH = mw->GetHeight();

            if (mw->GetFlags() & MINIWINDOW_ABSOLUTE_LOCATION) {
                CPoint loc = mw->GetLocation();
                mw->m_rect = CRect(loc.x, loc.y, loc.x + iW, loc.y + iH);
                continue;
            }

            switch (mw->GetPosition()) {
            case 0:  // stretch to output pane
                mw->m_rect = CRect(0, 0, clientW, clientH);
                continue;
            case 1:  // stretch with aspect ratio
                if (iH > 0)
                    mw->m_rect = CRect(0, 0, (int)(clientH * (double)iW / iH), clientH);
                continue;
            case 2:  // stretch to frame
                mw->m_rect = CRect(0, 0, frameW, frameH);
                continue;
            case 3:  // stretch to frame with aspect ratio
                if (iH > 0)
                    mw->m_rect = CRect(0, 0, (int)(frameH * (double)iW / iH), frameH);
                continue;
            case 4:  // top-left corner
                mw->m_rect = CRect(0, 0, iW, iH);
                tl_x = std::max(tl_x, (int)iW);
                tl_y = std::max(tl_y, (int)iH);
                break;
            case 5:  // top wall, centered L-R (deferred)
                top_widths += iW;
                top_ones.push_back(mw);
                break;
            case 6:  // top-right corner
                mw->m_rect = CRect(clientW - iW, 0, clientW, iH);
                tr_x = std::min(tr_x, clientW - (int)iW);
                tr_y = std::max(tr_y, (int)iH);
                break;
            case 7:  // right wall, centered T-B (deferred)
                right_widths += iH;
                right_ones.push_back(mw);
                break;
            case 8:  // bottom-right corner
                mw->m_rect = CRect(clientW - iW, clientH - iH, clientW, clientH);
                br_x = std::min(br_x, clientW - (int)iW);
                br_y = std::min(br_y, clientH - (int)iH);
                break;
            case 9:  // bottom wall, centered L-R (deferred)
                bottom_widths += iW;
                bottom_ones.push_back(mw);
                break;
            case 10: // bottom-left corner
                mw->m_rect = CRect(0, clientH - iH, iW, clientH);
                bl_x = std::max(bl_x, (int)iW);
                bl_y = std::min(bl_y, clientH - (int)iH);
                break;
            case 11: // left wall, centered T-B (deferred)
                left_widths += iH;
                left_ones.push_back(mw);
                break;
            case 12: // center
                mw->m_rect = CRect((clientW - iW) / 2, (clientH - iH) / 2,
                                   (clientW + iW) / 2, (clientH + iH) / 2);
                continue;
            case 13: // tile — no pre-calculation needed
                continue;
            default:
                continue;
            }
        }

        // ── Phase 1b: available room for each wall ───────────────────────
        long top_room    = tr_x - tl_x;   // horizontal span between top corners
        long right_room  = br_y - tr_y;   // vertical span between right corners
        long bottom_room = br_x - bl_x;   // horizontal span between bottom corners
        long left_room   = bl_y - tl_y;   // vertical span between left corners

        // Trim items that won't fit (remove from back, mark hidden).
        while (!top_ones.empty() && top_widths > top_room) {
            CMiniWindow* back = top_ones.back(); top_ones.pop_back();
            top_widths -= back->GetWidth();
            back->m_bTemporarilyHide = true;
        }
        while (!right_ones.empty() && right_widths > right_room) {
            CMiniWindow* back = right_ones.back(); right_ones.pop_back();
            right_widths -= back->GetHeight();
            back->m_bTemporarilyHide = true;
        }
        while (!bottom_ones.empty() && bottom_widths > bottom_room) {
            CMiniWindow* back = bottom_ones.back(); bottom_ones.pop_back();
            bottom_widths -= back->GetWidth();
            back->m_bTemporarilyHide = true;
        }
        while (!left_ones.empty() && left_widths > left_room) {
            CMiniWindow* back = left_ones.back(); left_ones.pop_back();
            left_widths -= back->GetHeight();
            back->m_bTemporarilyHide = true;
        }

        // Distribute evenly: gap before first + between each + gap after last.
        if (!top_ones.empty()) {
            long gap = (top_room - top_widths) / ((long)top_ones.size() + 1);
            long x = tl_x + gap;
            for (CMiniWindow* mw : top_ones) {
                long iW = mw->GetWidth(), iH = mw->GetHeight();
                mw->m_rect = CRect(x, 0, x + iW, iH);
                x += iW + gap;
            }
        }
        if (!right_ones.empty()) {
            long gap = (right_room - right_widths) / ((long)right_ones.size() + 1);
            long y = tr_y + gap;
            for (CMiniWindow* mw : right_ones) {
                long iW = mw->GetWidth(), iH = mw->GetHeight();
                mw->m_rect = CRect(clientW - iW, y, clientW, y + iH);
                y += iH + gap;
            }
        }
        if (!bottom_ones.empty()) {
            long gap = (bottom_room - bottom_widths) / ((long)bottom_ones.size() + 1);
            long x = bl_x + gap;
            for (CMiniWindow* mw : bottom_ones) {
                long iW = mw->GetWidth(), iH = mw->GetHeight();
                mw->m_rect = CRect(x, clientH - iH, x + iW, clientH);
                x += iW + gap;
            }
        }
        if (!left_ones.empty()) {
            long gap = (left_room - left_widths) / ((long)left_ones.size() + 1);
            long y = tl_y + gap;
            for (CMiniWindow* mw : left_ones) {
                long iW = mw->GetWidth(), iH = mw->GetHeight();
                mw->m_rect = CRect(0, y, iW, y + iH);
                y += iH + gap;
            }
        }

        } // end if (bUnder) — Phase 1 (rect calculation) runs on first pass only

        // ── Phase 2: blit each visible miniwindow onto destDC ───────────
        for (auto& kv : order) {
            CMiniWindow* mw = kv.second;
            if (!mw->GetShow() || mw->m_bTemporarilyHide) continue;

            bool wantUnder = (mw->GetFlags() & MINIWINDOW_DRAW_UNDERNEATH) != 0;
            if (wantUnder != bUnder) continue;

            wxDC* srcDC = WxDCFromCDC(mw->GetDC());
            if (!srcDC) continue;

            long iW = mw->GetWidth(), iH = mw->GetHeight();
            if (iW <= 0 || iH <= 0) continue;

            bool bTransparent = (mw->GetFlags() & MINIWINDOW_TRANSPARENT) != 0;
            short pos = mw->GetPosition();

            // Tile mode: special case.
            if (pos == 13) {
                int across = clientW / iW + 1;
                int down   = clientH / iH + 1;
                for (int tx = 0; tx < across; ++tx)
                    for (int ty = 0; ty < down; ++ty)
                        destDC.Blit(tx * iW, ty * iH, iW, iH, srcDC, 0, 0, wxCOPY);
                continue;
            }

            const CRect& r = mw->m_rect;
            int destX = r.left, destY = r.top;
            int destW = r.Width(), destH = r.Height();
            if (destW <= 0 || destH <= 0) continue;

            // Stretch modes (0–3): StretchBlit.
            if (pos <= 3 && !(mw->GetFlags() & MINIWINDOW_ABSOLUTE_LOCATION)) {
                destDC.StretchBlit(destX, destY, destW, destH,
                                   srcDC, 0, 0, iW, iH, wxCOPY);
                continue;
            }

            // Normal / transparent 1:1 blit.
            if (bTransparent) {
                // Build a masked bitmap using the miniwindow's background colour
                // as the transparency key, then draw it with the mask applied.
                COLORREF bg = mw->GetBackgroundColour();
                wxColour transCol(GetRValue(bg), GetGValue(bg), GetBValue(bg));
                wxBitmap srcBmp(iW, iH);
                {
                    wxMemoryDC tmpDC(srcBmp);
                    tmpDC.Blit(0, 0, iW, iH, srcDC, 0, 0, wxCOPY);
                }   // tmpDC destructor deselects srcBmp
                srcBmp.SetMask(new wxMask(srcBmp, transCol));
                destDC.DrawBitmap(srcBmp, destX, destY, /*useMask=*/true);
            } else {
                destDC.Blit(destX, destY, iW, iH, srcDC, 0, 0, wxCOPY);
            }
        }
    };

    // ── Miniwindow mouse-event callbacks ──────────────────────────────────
    // These are called from COutputPane with raw client-space coordinates.
    // They mirror CMUSHView::Mouse_Down/Up/Move_MiniWindow() but without the
    // Windows CMUSHView member state dependencies.

    // Helper: send a mouse event to a hotspot's script callback.
    // Equivalent to CMUSHView::Send_Mouse_Event_To_Plugin() but uses wxWidgets
    // key-state APIs instead of GetKeyState(VK_*).
    auto send_mw_event =
        [](CMUSHclientDoc* pDoc, DISPID& dispid,
               const std::string& pluginId, const std::string& routine,
               const std::string& hotspotId, long flags)
    {
        if (routine.empty()) return;

        // Add keyboard modifier flags (same bits as Windows impl).
        if (wxGetKeyState(WXK_SHIFT))   flags |= MW_MOUSE_SHIFT;
        if (wxGetKeyState(WXK_CONTROL)) flags |= MW_MOUSE_CTRL;
        if (wxGetKeyState(WXK_ALT))     flags |= MW_MOUSE_ALT;

        if (pluginId.empty()) {
            if (dispid == DISPID_UNKNOWN)
                return;
            pDoc->ExecuteHotspotScript(dispid, routine.c_str(),
                                       flags, hotspotId.c_str());
            return;
        }
        CPlugin* pPlugin = pDoc->GetPlugin(pluginId.c_str());
        if (!pPlugin || !pPlugin->m_bEnabled) return;
        DISPID id = pPlugin->m_ScriptEngine->GetDispid(routine.c_str());
        if (id == DISPID_UNKNOWN) return;
        CPlugin* savedPlugin = pDoc->m_CurrentPlugin;
        pDoc->m_CurrentPlugin = pPlugin;
        CScriptDispatchID dispid_info(id);
        CScriptCallInfo callinfo(routine.c_str(), dispid_info);
        pPlugin->ExecutePluginScript(callinfo, flags, hotspotId.c_str());
        pDoc->m_CurrentPlugin = savedPlugin;
    };

    // Helper: find which miniwindow (and hotspot) a client-space point is over.
    // Returns the miniwindow pointer (or nullptr); sets sHotspotId, pHotspot,
    // sMiniwindowId by reference. Mirrors CMUSHView::Mouse_Over_Miniwindow().
    auto mw_hit_test =
        [](CMUSHclientDoc* pDoc, int x, int y,
           std::string& sHotspotId, CHotspot*& pHotspot, std::string& sMiniwindowId)
        -> CMiniWindow*
    {
        pHotspot = nullptr;
        CPoint pt(x, y);
        for (auto it = pDoc->m_MiniWindowsOrder.rbegin();
             it != pDoc->m_MiniWindowsOrder.rend(); ++it)
        {
            CMiniWindow* mw = it->second;
            if (!mw->GetShow()) continue;
            if (mw->GetFlags() & (MINIWINDOW_DRAW_UNDERNEATH | MINIWINDOW_IGNORE_MOUSE))
                continue;
            if (!mw->m_rect.PtInRect(pt)) continue;

            CPoint rel(x - mw->m_rect.left, y - mw->m_rect.top);
            for (auto& hkv : mw->m_Hotspots) {
                if (hkv.second->m_rect.PtInRect(rel)) {
                    pHotspot      = hkv.second;
                    sHotspotId    = hkv.first;
                    sMiniwindowId = it->first;
                    return mw;
                }
            }
            pHotspot = nullptr;
            sMiniwindowId = it->first;
            return mw;
        }
        sMiniwindowId.clear();
        return nullptr;
    };

    // Mouse-down callback: fire MouseDown script, returns true if consumed.
    // flags carries the button identity: MW_MOUSE_LH (0x10) or MW_MOUSE_RH (0x20).
    m_output->m_fn_miniwindow_mouse_down =
        [this, send_mw_event, mw_hit_test](int x, int y, long flags) -> bool
    {
        if (!m_doc) return false;
        std::string sHotspotId, sMiniwindowId;
        CHotspot* pHotspot = nullptr;
        CMiniWindow* mw = mw_hit_test(m_doc, x, y, sHotspotId, pHotspot, sMiniwindowId);
        if (!mw) return false;

        mw->m_last_mouseposition = CPoint(x - mw->m_rect.left, y - mw->m_rect.top);
        mw->m_last_mouse_update++;

        if (pHotspot) {
            mw->m_sMouseDownHotspot = sHotspotId;
            m_mw_mouseDown = true;
            mw->m_bExecutingScript = true;
            send_mw_event(m_doc, pHotspot->m_dispid_MouseDown,
                          mw->m_sCallbackPlugin,
                          pHotspot->m_sMouseDown,
                          sHotspotId, flags);
            mw->m_bExecutingScript = false;
        }
        m_mw_prevWindow = sMiniwindowId;
        return true;   // consumed — don't start text-selection drag
    };

    // Mouse-up callback: fire MouseUp script.
    // flags carries the button identity: MW_MOUSE_LH (0x10) or MW_MOUSE_RH (0x20).
    m_output->m_fn_miniwindow_mouse_up =
        [this, send_mw_event, mw_hit_test](int x, int y, long flags) -> bool
    {
        if (!m_doc) return false;
        m_mw_mouseDown = false;

        std::string sHotspotId, sMiniwindowId;
        CHotspot* pHotspot = nullptr;
        CMiniWindow* mw = mw_hit_test(m_doc, x, y, sHotspotId, pHotspot, sMiniwindowId);
        if (!mw) return false;

        mw->m_last_mouseposition = CPoint(x - mw->m_rect.left, y - mw->m_rect.top);
        mw->m_last_mouse_update++;

        if (pHotspot && !mw->m_sMouseDownHotspot.empty()) {
            mw->m_bExecutingScript = true;
            send_mw_event(m_doc, pHotspot->m_dispid_MouseUp,
                          mw->m_sCallbackPlugin,
                          pHotspot->m_sMouseUp,
                          sHotspotId, flags);
            mw->m_bExecutingScript = false;
        }
        mw->m_sMouseDownHotspot.clear();
        return pHotspot != nullptr;
    };

    // Mouse-move callback: fire MouseOver / CancelMouseOver scripts.
    m_output->m_fn_miniwindow_mouse_move =
        [this, send_mw_event, mw_hit_test](int x, int y)
    {
        if (!m_doc || m_mw_mouseDown) return;   // ignore moves while button held

        std::string sHotspotId, sMiniwindowId;
        CHotspot* pHotspot = nullptr;
        CMiniWindow* mw = mw_hit_test(m_doc, x, y, sHotspotId, pHotspot, sMiniwindowId);

        if (mw) {
            mw->m_last_mouseposition = CPoint(x - mw->m_rect.left, y - mw->m_rect.top);
            mw->m_last_mouse_update++;
        }

        // If we've left the previous miniwindow, fire its CancelMouseOver.
        if (sMiniwindowId != m_mw_prevWindow && !m_mw_prevWindow.empty()) {
            auto it = m_doc->m_MiniWindows.find(m_mw_prevWindow);
            if (it != m_doc->m_MiniWindows.end()) {
                CMiniWindow* old_mw = it->second;
                if (!old_mw->m_sMouseOverHotspot.empty()) {
                    auto hit = old_mw->m_Hotspots.find(old_mw->m_sMouseOverHotspot);
                    std::string oldHotspot = old_mw->m_sMouseOverHotspot;
                    old_mw->m_sMouseOverHotspot.clear();
                    if (hit != old_mw->m_Hotspots.end()) {
                        old_mw->m_bExecutingScript = true;
                        send_mw_event(m_doc,
                                      hit->second->m_dispid_CancelMouseOver,
                                      old_mw->m_sCallbackPlugin,
                                      hit->second->m_sCancelMouseOver,
                                      oldHotspot, 0);
                        old_mw->m_bExecutingScript = false;
                    }
                }
            }
            m_mw_prevWindow.clear();
        }

        if (!mw || !pHotspot) {
            // Cancel current hotspot in the current window if we left it.
            if (mw && !mw->m_sMouseOverHotspot.empty() &&
                mw->m_sMouseOverHotspot != sHotspotId)
            {
                auto hit = mw->m_Hotspots.find(mw->m_sMouseOverHotspot);
                std::string old = mw->m_sMouseOverHotspot;
                mw->m_sMouseOverHotspot.clear();
                if (hit != mw->m_Hotspots.end()) {
                    mw->m_bExecutingScript = true;
                    send_mw_event(m_doc,
                                  hit->second->m_dispid_CancelMouseOver,
                                  mw->m_sCallbackPlugin,
                                  hit->second->m_sCancelMouseOver,
                                  old, 0);
                    mw->m_bExecutingScript = false;
                }
            }
            m_mw_prevWindow = sMiniwindowId;
            return;
        }

        m_mw_prevWindow = sMiniwindowId;

        // Cancel previous hotspot if different.
        std::string oldHotspotId = mw->m_sMouseOverHotspot;
        if (!oldHotspotId.empty() && oldHotspotId != sHotspotId) {
            auto hit = mw->m_Hotspots.find(oldHotspotId);
            mw->m_sMouseOverHotspot.clear();
            if (hit != mw->m_Hotspots.end()) {
                mw->m_bExecutingScript = true;
                send_mw_event(m_doc,
                              hit->second->m_dispid_CancelMouseOver,
                              mw->m_sCallbackPlugin,
                              hit->second->m_sCancelMouseOver,
                              oldHotspotId, 0);
                mw->m_bExecutingScript = false;
            }
        }

        // Re-check: hotspot might have been deleted by the cancel callback.
        if (mw->m_Hotspots.find(sHotspotId) == mw->m_Hotspots.end())
            return;

        // Fire MouseOver if we're newly over this hotspot.
        bool firstTime = (mw->m_sMouseOverHotspot != sHotspotId);
        mw->m_sMouseOverHotspot = sHotspotId;
        long flags = firstTime ? 0 : 0x80;  // 0x80 = not first time
        mw->m_bExecutingScript = true;
        send_mw_event(m_doc,
                      pHotspot->m_dispid_MouseOver,
                      mw->m_sCallbackPlugin,
                      pHotspot->m_sMouseOver,
                      sHotspotId, flags);
        mw->m_bExecutingScript = false;
    };

    // ── Miniwindow mouse-wheel callback ───────────────────────────────────
    // Forwards scroll-wheel events to miniwindow hotspot scripts.
    // delta > 0 = wheel forward (up), delta < 0 = backward (down).
    m_output->m_fn_miniwindow_mouse_wheel =
        [this, send_mw_event, mw_hit_test](int x, int y, int delta) -> bool
    {
        if (!m_doc) return false;
        std::string sHotspotId, sMiniwindowId;
        CHotspot* pHotspot = nullptr;
        CMiniWindow* mw = mw_hit_test(m_doc, x, y, sHotspotId, pHotspot, sMiniwindowId);
        if (!mw || !pHotspot) return false;
        if (pHotspot->m_sScrollwheelCallback.empty()) return false;

        long flags = (delta < 0) ? 0x100 /*MW_MOUSE_SCROLL_BACK*/ : 0;
        mw->m_bExecutingScript = true;
        send_mw_event(m_doc, pHotspot->m_dispid_ScrollwheelCallback,
                      mw->m_sCallbackPlugin,
                      pHotspot->m_sScrollwheelCallback,
                      sHotspotId, flags);
        mw->m_bExecutingScript = false;
        return true;
    };

    // ── Tooltip callback ───────────────────────────────────────────────────
    // Returns the tooltip text for the client-space point (x, y).
    // Priority: miniwindow hotspot tooltip > MXP run hint > empty.
    m_output->m_fn_get_tooltip =
        [this, mw_hit_test](int x, int y) -> std::string
    {
        if (!m_doc) return {};

        // 1. Check miniwindow hotspot tooltip.
        std::string sHotspotId, sMiniwindowId;
        CHotspot* pHotspot = nullptr;
        CMiniWindow* mw = mw_hit_test(m_doc, x, y, sHotspotId, pHotspot, sMiniwindowId);
        if (pHotspot && !pHotspot->m_sTooltipText.empty())
            return pHotspot->m_sTooltipText;

        // 2. Check MXP hint text on the output line under the cursor.
        // Use PointToChar for wrap-aware hit-testing: it returns the absolute
        // character column within the logical line, which we then walk to find
        // which run the cursor is over (same approach as link detection).
        SelPoint pt = m_output->PointToChar(x, y);
        const SOutputLine* line = m_output->GetLine(pt.line);
        if (!line) return {};

        int c = 0;
        for (const auto& run : *line) {
            int runLen = (int)run.text.size();
            if (pt.col >= c && pt.col < c + runLen)
                return run.hint;   // may be empty — that's fine
            c += runLen;
        }
        return {};
    };

    // ── MXP hyperlink / send-link / plain-URL callback ───────────────────────
    // action is either:
    //   • An http:// or https:// URL auto-detected from plain text
    //     → open in the system default browser via wxLaunchDefaultBrowser().
    //   • A pipe-delimited MXP command string from an <A href="..."> element
    //     → sent to the MUD via EvaluateCommand() (popup for multi-choice).
    m_output->m_fn_link_clicked = [this](const std::string& action) {
        if (action.empty()) return;

        // Plain URL from auto-detection.
        if ((action.size() >= 7 && action.compare(0, 7, "http://")  == 0) ||
            (action.size() >= 8 && action.compare(0, 8, "https://") == 0))
        {
            wxLaunchDefaultBrowser(wxString::FromUTF8(action.c_str()));
            return;
        }

        if (!m_doc) return;

        // Split the pipe-delimited alternatives, skipping empty segments.
        std::vector<std::string> choices;
        {
            size_t start = 0;
            for (;;) {
                size_t sep = action.find('|', start);
                std::string part = action.substr(start,
                    (sep == std::string::npos) ? std::string::npos : sep - start);
                if (!part.empty()) choices.push_back(std::move(part));
                if (sep == std::string::npos) break;
                start = sep + 1;
            }
        }
        if (choices.empty()) return;

        // Single action: send immediately without a menu.
        if (choices.size() == 1) {
            CString cCmd(choices[0].c_str());
            bool bOmit = false;
            m_doc->EvaluateCommand(cCmd, /*countThem=*/true, bOmit);
            m_doc->UpdateAllViews(nullptr);
            return;
        }

        // Multiple alternatives — present a popup menu so the user can choose.
        // We bind the handlers directly on the wxMenu object (wxMenu extends
        // wxEvtHandler); when the menu goes out of scope the bindings are freed.
        // Menu events are dispatched to the menu's own handler chain first, so
        // the Bind() calls below will be reached before the window's handlers.
        wxMenu linkMenu;
        for (const std::string& ch : choices) {
            wxMenuItem* item = linkMenu.Append(
                wxID_ANY, wxString::FromUTF8(ch.c_str(), ch.size()));
            linkMenu.Bind(wxEVT_MENU, [this, cmd = ch](wxCommandEvent&) {
                CString cCmd(cmd.c_str());
                bool bOmit = false;
                m_doc->EvaluateCommand(cCmd, /*countThem=*/true, bOmit);
                m_doc->UpdateAllViews(nullptr);
            }, item->GetId());
        }
        // PopupMenu() runs its own event loop and returns after the user
        // picks an item or dismisses the menu.  The selected item's handler
        // fires synchronously inside PopupMenu() before it returns.
        m_output->PopupMenu(&linkMenu);
        // linkMenu destructs here, automatically cleaning up all Bind() handlers.
    };

    m_input = new wxTextCtrl(this, ID_INPUT, wxEmptyString,
                             wxDefaultPosition, wxDefaultSize,
                             wxTE_PROCESS_ENTER);
    sizer->Add(m_input, 0, wxEXPAND);

    // Password-mode input bar: shown in place of m_input when the MUD sends
    // IAC WILL ECHO (telnet echo suppression) to signal a password prompt.
    // Uses wxTE_PASSWORD so characters are masked.  Hidden by default.
    m_inputPassword = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxDefaultSize,
                                     wxTE_PASSWORD | wxTE_PROCESS_ENTER);
    sizer->Add(m_inputPassword, 0, wxEXPAND);
    m_inputPassword->Hide();

    // Apply the output pane's monospace font to both input bars so that the
    // input line uses the same typeface as the output — consistent look and
    // feel, and correct column-count alignment for users who type long lines.
    {
        const wxFont& outFont = m_output->GetCurrentFont();
        m_input->SetFont(outFont);
        m_inputPassword->SetFont(outFont);
    }
    // Wire the same send/key handlers directly to m_inputPassword.
    m_inputPassword->Bind(wxEVT_TEXT_ENTER,
        [this](wxCommandEvent& e) { OnInputEnter(e); });
    m_inputPassword->Bind(wxEVT_KEY_DOWN,
        &CWorldChildFrame::OnInputKeyDown, this);

    SetSizer(sizer);

    // ── Restore child frame size from config ──────────────────────────────
    // We persist a single "last used" child frame size (not per-world, since
    // all worlds share the same MDI client area).  This prevents the window
    // from always defaulting to 800×600 after the user resizes it once.
    {
        wxConfig cfg(wxT("mushclient"));
        long w = 800, h = 600;
        cfg.Read(wxT("/geometry/child_w"), &w, 800L);
        cfg.Read(wxT("/geometry/child_h"), &h, 600L);
        if (w >= 200 && h >= 100)
            SetSize((int)w, (int)h);
    }

    m_input->SetFocus();

    // ── Load persistent command history ───────────────────────────────────
    // Each world keeps its own history file keyed by world name, so different
    // MUDs don't pollute each other's command history.  The path is fixed once
    // at construction time (m_historyFilePath) so a mid-session name change in
    // World Properties doesn't cause a load/save mismatch.
    {
        std::string worldName = m_doc
            ? static_cast<const char*>(m_doc->m_mush_name) : "";
        m_historyFilePath = GetHistoryFilePath(worldName);

        std::string histPath = m_historyFilePath;
        if (!histPath.empty()) {
            if (FILE* f = fopen(histPath.c_str(), "r")) {
                char buf[8192];
                while (fgets(buf, (int)sizeof(buf), f)) {
                    std::string line = buf;
                    // Strip trailing newline / carriage-return.
                    while (!line.empty() &&
                           (line.back() == '\n' || line.back() == '\r'))
                        line.pop_back();
                    if (!line.empty())
                        m_history.push_back(wxString::FromUTF8(line.c_str()));
                }
                fclose(f);
                // Trim to the maximum if the file was longer.
                while ((int)m_history.size() > kMaxHistory)
                    m_history.erase(m_history.begin());
            }
        }
    }

    // ── Input bar: key handler for history navigation ─────────────────────
    // Bind directly to m_input so we only see input-bar key events.
    m_input->Bind(wxEVT_KEY_DOWN, &CWorldChildFrame::OnInputKeyDown, this);

    // ── Multi-line paste splitter ──────────────────────────────────────────
    // If the clipboard contains newlines, send each complete line as a MUD
    // command and leave the trailing fragment in the input bar for review.
    m_input->Bind(wxEVT_TEXT_PASTE, &CWorldChildFrame::OnInputPaste, this);

    // ── Output pane: redirect printable keys to the input bar ─────────────
    // When the user clicks in the output pane and then types, we want the
    // text to end up in the input bar rather than being silently discarded.
    m_output->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& evt) {
        int code = evt.GetKeyCode();
        // Ctrl+F → show the find bar.
        if (code == 'F' && evt.ControlDown()) {
            ShowFindBar(true);
            return;
        }
        // Let Ctrl+C, Ctrl+A, Ctrl+±/0 (zoom), and arrow keys be handled by
        // the output pane's own handler.
        bool paneOwns = evt.ControlDown() ||
                        code == WXK_UP || code == WXK_DOWN ||
                        code == WXK_LEFT || code == WXK_RIGHT ||
                        code == WXK_PAGEUP || code == WXK_PAGEDOWN ||
                        code == WXK_HOME || code == WXK_END;
        if (paneOwns) { evt.Skip(); return; }
        // Anything else (printable characters, Escape, backspace…) →
        // redirect focus to the input bar.
        if (wxTextCtrl* inp = ActiveInput()) inp->SetFocus();
        evt.Skip();   // let wxTextCtrl receive the event after getting focus
    });

    // ── Restore persistent font size ──────────────────────────────────────────
    // Apply the user's last font size preference.  We do this after all
    // callbacks are wired so m_fn_grid_changed fires and updates NAWS/wrapping.
    {
        wxConfig cfg(wxT("mushclient"));
        long savedSize = 0;
        cfg.Read(wxT("/display/font_size"), &savedSize, 0L);
        if (savedSize >= COutputPane::kMinFontSize &&
            savedSize <= COutputPane::kMaxFontSize &&
            savedSize != m_output->FontSize())
        {
            m_output->AdjustFontSize((int)savedSize - m_output->FontSize());
        }
    }

    // ── Per-world timer ────────────────────────────────────────────────────
    // Fires every 100 ms to drive CheckTimers() (world/plugin timers) and
    // SendToAllPluginCallbacks(ON_PLUGIN_TICK) (every ~40 ms via accumulator).
    // Also flushes any output the timer callbacks may have produced.
    m_worldTimer = new wxTimer(this, ID_WORLD_TIMER);
    m_worldTimer->Start(100);
}

// ── UpdateTitle ───────────────────────────────────────────────────────────
void CWorldChildFrame::UpdateTitle()
{
    if (!m_doc) return;
    wxString title = wxString::FromUTF8(
        static_cast<const char*>(m_doc->m_mush_name));
    if (!title.empty())
        SetTitle(title);
}

// ── TriggerNAWS ───────────────────────────────────────────────────────────
// Re-sends a NAWS packet with the current character-grid dimensions.
// Called after a font change so the MUD sees the updated col/row count.
// Uses CallAfter to ensure the output pane has finished re-measuring its font
// metrics before we read CharWidth() and LineHeight().
void CWorldChildFrame::TriggerNAWS()
{
    if (!m_doc || !m_output) return;
    CallAfter([this]() {
        if (!m_doc || !m_output) return;
        int charW = m_output->CharWidth();
        int lineH = m_output->LineHeight();
        if (charW <= 0 || lineH <= 0) return;
        int paneW, paneH;
        m_output->GetClientSize(&paneW, &paneH);
        if (paneW <= 0 || paneH <= 0) return;
        int cols = paneW / charW;
        m_doc->m_nWrapColumn = (unsigned short)std::max(1, std::min(cols, 65535));
        m_doc->SendNAWS(cols, paneH / lineH);
    });
}

// ── UpdateInputBarColors ──────────────────────────────────────────────────
// Derives input bar fg/bg from the output pane's current background colour
// and applies them to both the normal and password input bars.
// ITU-R BT.601 luma: white text on dark backgrounds, dark text on light.
void CWorldChildFrame::UpdateInputBarColors()
{
    unsigned long col = m_output ? m_output->PaneBackground() : 0UL;
    int r = static_cast<int>(GetRValue(col));
    int g = static_cast<int>(GetGValue(col));
    int b = static_cast<int>(GetBValue(col));
    int luma = (r * 299 + g * 587 + b * 114) / 1000;

    wxColour inputFg = (luma < 128) ? wxColour(220, 220, 220) : wxColour(20, 20, 20);
    wxColour inputBg = wxColour(r, g, b);
    for (wxTextCtrl* inp : {m_input, m_inputPassword}) {
        if (!inp || !inp->GetHandle()) continue;  // skip if platform window not yet realized
        inp->SetForegroundColour(inputFg);
        inp->SetBackgroundColour(inputBg);
        inp->Refresh();
    }
}

// ── ApplyPaneBackground ───────────────────────────────────────────────────
// Applies col as the output pane background and styles the input bar to match.
// When col == 0xFFFFFFFF (NO_COLOUR) the default black background is kept.
void CWorldChildFrame::ApplyPaneBackground(unsigned long col)
{
    if (!m_output) return;

    // 0xFFFFFFFF is NO_COLOUR — world hasn't specified a custom background.
    if (col == 0xFFFFFFFFUL)
        col = 0;  // default black

    m_output->SetPaneBackground(col);
    UpdateInputBarColors();
}

// ── ApplyHistRSearch ──────────────────────────────────────────────────────
// Sets m_histRSearch and tints the input bar to signal Ctrl+R mode.
// Tints the background deep blue when entering; restores world colors on exit.
// The no-op guard (m_histRSearch == on) prevents redundant repaints on every
// key press in the common case where we're not in Ctrl+R mode.
void CWorldChildFrame::ApplyHistRSearch(bool on)
{
    if (m_histRSearch == on) return;
    m_histRSearch = on;
    if (!m_input) return;

    if (on) {
        // Deep-blue tint: clearly distinguishable from the normal input bar but
        // not so aggressive that it's distracting.  Text is pale blue so it's
        // legible against the tinted background regardless of the world theme.
        m_input->SetBackgroundColour(wxColour(20, 45, 95));
        m_input->SetForegroundColour(wxColour(160, 210, 255));
    } else {
        // Restore to match the output pane's background.
        UpdateInputBarColors();
    }
    m_input->Refresh();
}

// ── ShowFindBar ───────────────────────────────────────────────────────────
void CWorldChildFrame::ShowFindBar(bool show)
{
    if (!m_findBar) return;
    if (show) {
        m_findBar->Show();
        if (m_findInput) {
            m_findInput->SetFocus();
            m_findInput->SelectAll();
        }
    } else {
        m_findBar->Hide();
        if (m_output) m_output->HighlightSearch(wxEmptyString, false);
        if (m_findLabel) m_findLabel->SetLabel(wxEmptyString);
        if (wxTextCtrl* inp = ActiveInput()) inp->SetFocus();
    }
    Layout();
}

// ── DoFind ─────────────────────────────────────────────────────────────────
void CWorldChildFrame::DoFind(bool forward)
{
    if (!m_findInput || !m_output) return;
    wxString term = m_findInput->GetValue();

    if (term.IsEmpty()) {
        if (m_findLabel) m_findLabel->SetLabel(wxEmptyString);
        return;
    }

    bool caseSensitive = m_findCaseBox && m_findCaseBox->IsChecked();
    int matchLine = -1, ordinal = 0, totalMatches = 0;
    bool found = m_output->FindNext(term, forward,
                                    &matchLine, &ordinal, &totalMatches,
                                    caseSensitive);

    if (found) {
        // Reset background in case a previous search had left it red.
        m_findInput->SetBackgroundColour(wxNullColour);
        m_findInput->Refresh();

        if (m_findLabel) {
            wxString countStr;
            if (totalMatches >= 9999)
                countStr = wxString::Format(wxT("%d of 9999+"), ordinal);
            else
                countStr = wxString::Format(wxT("%d of %d"), ordinal, totalMatches);
            m_findLabel->SetLabel(countStr);
        }
    } else {
        // Flash the find input red briefly to signal "not found".
        m_findInput->SetBackgroundColour(wxColour(255, 100, 100));
        m_findInput->Refresh();
        if (m_findLabel) m_findLabel->SetLabel(wxT("Not found"));
        CallAfter([this]() {
            if (m_findInput) {
                m_findInput->SetBackgroundColour(wxNullColour);
                m_findInput->Refresh();
            }
        });
    }
}

// ── ActiveInput ───────────────────────────────────────────────────────────
// Returns the currently visible input control: the password bar when telnet
// echo suppression is active, the normal bar otherwise.
wxTextCtrl* CWorldChildFrame::ActiveInput() const
{
    if (m_inputPassword && m_inputPassword->IsShown())
        return m_inputPassword;
    return m_input;
}

// ── ApplyNoEcho ───────────────────────────────────────────────────────────
// Switches the input bar between normal and password (masked) mode.
// Called from OnWorldTimer whenever m_doc->m_bNoEcho changes.
void CWorldChildFrame::ApplyNoEcho(bool noEcho)
{
    if (noEcho) {
        // Switch to the password bar.  Clear any partial command first so it
        // isn't accidentally sent as a password.
        if (m_input) m_input->Hide();
        if (m_inputPassword) {
            m_inputPassword->Clear();
            m_inputPassword->Show();
            m_inputPassword->SetFocus();
        }
    } else {
        // Switch back to the normal bar.  Wipe the password field for security.
        if (m_inputPassword) {
            m_inputPassword->Clear();
            m_inputPassword->Hide();
        }
        if (m_input) {
            m_input->Show();
            m_input->SetFocus();
        }
    }
    Layout();
    m_lastNoEcho = noEcho;
}

// ── IsActiveWorld ─────────────────────────────────────────────────────────
// Returns true when this frame is the currently active MDI child.
// Used to guard status-bar updates so background worlds don't overwrite the
// status text for the world the user is currently looking at.
bool CWorldChildFrame::IsActiveWorld() const
{
    wxMDIParentFrame* parent =
        dynamic_cast<wxMDIParentFrame*>(GetParent());
    return parent && (parent->GetActiveChild() == this);
}

// ── Session logging ───────────────────────────────────────────────────────
//
// Logging captures the plain-text content of each incoming output line to a
// UTF-8 text file.  Horizontal rules are written as a dash line.  Style
// attributes (colour, bold, underline) are not preserved — the log is plain
// text, suitable for reading or grepping later.

bool CWorldChildFrame::StartLogging(const std::string& path)
{
    if (m_logFile) return false;   // already logging

    m_logFile = fopen(path.c_str(), "a");
    if (!m_logFile) return false;

    m_logFilePath = path;

    // Write a timestamped header.
    time_t now = time(nullptr);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    std::string worldName = m_doc
        ? static_cast<const char*>(m_doc->m_mush_name) : "Unknown";
    fprintf(m_logFile,
            "===== Log started %s — World: %s =====\n",
            ts, worldName.c_str());
    fflush(m_logFile);
    return true;
}

void CWorldChildFrame::StopLogging()
{
    if (!m_logFile) return;

    time_t now = time(nullptr);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(m_logFile, "===== Log stopped %s =====\n", ts);
    fclose(m_logFile);
    m_logFile = nullptr;
    m_logFilePath.clear();
}

void CWorldChildFrame::LogLine(const SOutputLine& line)
{
    if (!m_logFile) return;

    // Horizontal rules become a dashed separator.
    if (line.size() == 1 && line[0].horiz_rule) {
        fputs("----------------------------------------\n", m_logFile);
        fflush(m_logFile);
        return;
    }

    // Concatenate run texts; skip empty and horiz_rule runs.
    std::string text;
    for (const auto& run : line) {
        if (!run.text.empty() && !run.horiz_rule)
            text += run.text;
    }
    text += '\n';
    fputs(text.c_str(), m_logFile);
    fflush(m_logFile);
}

// ── UpdateStatusBar ───────────────────────────────────────────────────────
// Writes the current connection state to the MDI parent's status bar.
// Only meaningful when this frame is the active MDI child; callers should
// check IsActiveWorld() before calling (or let it be a no-op when inactive).
void CWorldChildFrame::UpdateStatusBar()
{
    if (!g_wxhook_set_status || !m_doc) return;

    int phase = m_doc->m_iConnectPhase;

    // Build a world-name prefix ("WorldName — ") for every message so the user
    // always knows which world is active in the MDI parent's status bar.
    std::string worldName = static_cast<const char*>(m_doc->m_mush_name);
    std::string prefix;
    if (!worldName.empty()) {
        prefix = worldName;
        prefix += " \xe2\x80\x94 ";   // " — " (em dash)
    }

    // Script-provided status message takes priority over connection state.
    if (!m_doc->m_strStatusMessage.IsEmpty()) {
        std::string msg = prefix + static_cast<const char*>(m_doc->m_strStatusMessage);
        g_wxhook_set_status(msg.c_str());
        return;
    }

    if (phase == eConnectConnectedToMud) {
        std::string msg = prefix + "Connected to ";
        msg += static_cast<const char*>(m_doc->m_server);
        msg += ":";
        msg += std::to_string(m_doc->m_port);
        // If logging, append the log indicator.
        if (IsLogging())
            msg += "  \xf0\x9f\x94\xb4 Logging";   // 🔴
        g_wxhook_set_status(msg.c_str());
    } else if (m_reconnectCountdown > 0) {
        int secs = (m_reconnectCountdown + 9) / 10;   // round up
        std::string msg = prefix + "Reconnecting in " + std::to_string(secs) + "s\xe2\x80\xa6";
        g_wxhook_set_status(msg.c_str());
    } else if (m_output && !m_output->IsAutoScrolling()) {
        // Scrolled-back indicator: no world prefix so the arrow stands out.
        g_wxhook_set_status(
            "\xe2\x86\x91 Scrolled back \xe2\x80\x94 press Escape or scroll to bottom to resume");
    } else {
        const char* stateStr = "Not connected.";
        switch (phase) {
        case eConnectMudNameLookup:
        case eConnectProxyNameLookup:        stateStr = "Resolving\xe2\x80\xa6";     break;
        case eConnectConnectingToMud:
        case eConnectConnectingToProxy:      stateStr = "Connecting\xe2\x80\xa6";    break;
        case eConnectAwaitingProxyResponse1:
        case eConnectAwaitingProxyResponse2:
        case eConnectAwaitingProxyResponse3: stateStr = "Connecting via proxy\xe2\x80\xa6"; break;
        case eConnectAwaitingSSLHandshake:   stateStr = "TLS handshake\xe2\x80\xa6"; break;
        case eConnectDisconnecting:          stateStr = "Disconnecting\xe2\x80\xa6"; break;
        default:                             stateStr = "Not connected.";             break;
        }
        std::string msg = prefix + stateStr;
        g_wxhook_set_status(msg.c_str());
    }
}

// ── OnActivate ────────────────────────────────────────────────────────────
// Called when this MDI child is activated (gets focus) or deactivated.
// On activation, immediately refresh the status bar so switching between
// worlds gives instant visual feedback in the status bar.
void CWorldChildFrame::OnActivate(wxActivateEvent& evt)
{
    if (evt.GetActive())
        UpdateStatusBar();
    evt.Skip();
}

// ── OnInputKeyDown ─────────────────────────────────────────────────────────
// Up/Down arrows navigate command history; all other keys pass through normally.
void CWorldChildFrame::OnInputKeyDown(wxKeyEvent& evt)
{
    int code = evt.GetKeyCode();

    // Escape: snap the output pane back to the bottom (re-engage auto-scroll).
    // Useful when the user has scrolled up and wants to return to live output.
    if (code == WXK_ESCAPE) {
        if (m_output) m_output->ScrollToBottom();
        return;   // consume — don't close dialogs or deselect in the input bar
    }

    // Page Up / Page Down: scroll the output pane one page without leaving the
    // input bar.  This lets the user page through output while still typing.
    if (code == WXK_PAGEUP) {
        if (m_output) m_output->ScrollPage(-1);
        return;
    }
    if (code == WXK_PAGEDOWN) {
        if (m_output) m_output->ScrollPage(+1);
        return;
    }

    // Ctrl+Home / Ctrl+End: jump to the very top or bottom of the output buffer
    // without leaving the input bar.  Mirrors the Ctrl+Home/End shortcuts that
    // work when the output pane itself has focus (COutputPane::OnKeyDown).
    if (code == WXK_HOME && evt.ControlDown()) {
        if (m_output) m_output->ScrollToTop();
        return;
    }
    if (code == WXK_END && evt.ControlDown()) {
        if (m_output) m_output->ScrollToBottom();
        return;
    }

    wxTextCtrl* inp = ActiveInput();

    // Ctrl+U: kill the entire input line (standard Unix terminal shortcut).
    // Saves to the kill ring so Ctrl+Y can restore it.
    if (code == 'U' && evt.ControlDown()) {
        if (inp) {
            m_killBuf = inp->GetValue();
            inp->Clear();
            m_histIdx = -1; ApplyHistRSearch(false);
        }
        return;
    }

    // Ctrl+W / Ctrl+Backspace: kill the word immediately before the cursor.
    // Ctrl+W is the Unix terminal convention; Ctrl+Backspace matches most GUI
    // apps on Windows and macOS.  Both skip trailing spaces then delete back
    // to the next word boundary.  Saves to the kill ring.
    bool deleteWordBack = (code == 'W' && evt.ControlDown()) ||
                         (code == WXK_BACK && evt.ControlDown());
    if (deleteWordBack) {
        if (inp) {
            long pos = inp->GetInsertionPoint();
            if (pos > 0) {
                wxString text = inp->GetValue();
                long newPos = pos;
                // Skip over any trailing whitespace.
                while (newPos > 0 && text[newPos - 1] == ' ')
                    --newPos;
                // Skip over the word itself (stop at any whitespace).
                while (newPos > 0 && text[newPos - 1] != ' ')
                    --newPos;
                m_killBuf = text.Mid(newPos, pos - newPos);
                inp->Remove(newPos, pos);
                m_histIdx = -1; ApplyHistRSearch(false);
            }
        }
        return;
    }

    // Ctrl+D: delete character forward (emacs/Unix convention).
    if (code == 'D' && evt.ControlDown()) {
        if (inp) {
            long pos = inp->GetInsertionPoint();
            long len = (long)inp->GetValue().size();
            if (pos < len) {
                inp->Remove(pos, pos + 1);
                m_histIdx = -1; ApplyHistRSearch(false);
            }
        }
        return;
    }

    // Ctrl+Delete: delete the word immediately after the cursor.
    // Skips leading spaces then deletes forward to the next space boundary.
    if (code == WXK_DELETE && evt.ControlDown()) {
        if (inp) {
            long pos = inp->GetInsertionPoint();
            wxString text = inp->GetValue();
            long len = (long)text.size();
            if (pos < len) {
                long newPos = pos;
                // Skip over leading whitespace.
                while (newPos < len && text[newPos] == ' ')
                    ++newPos;
                // Skip over the word.
                while (newPos < len && text[newPos] != ' ')
                    ++newPos;
                inp->Remove(pos, newPos);
                m_histIdx = -1; ApplyHistRSearch(false);
            }
        }
        return;
    }

    // Ctrl+K: kill from the cursor to the end of the input line.
    // Saves the killed text to the kill ring so Ctrl+Y can restore it.
    if (code == 'K' && evt.ControlDown()) {
        if (inp) {
            long pos = inp->GetInsertionPoint();
            wxString text = inp->GetValue();
            long len = (long)text.size();
            if (pos < len) {
                m_killBuf = text.Mid(pos);
                inp->Remove(pos, len);
                m_histIdx = -1; ApplyHistRSearch(false);
            }
        }
        return;
    }

    // Ctrl+Y: yank (paste) the most recently killed text at the cursor.
    // Complements Ctrl+K / Ctrl+W / Ctrl+U.  No-op if the kill ring is empty.
    if (code == 'Y' && evt.ControlDown()) {
        if (inp && !m_killBuf.IsEmpty()) {
            long pos = inp->GetInsertionPoint();
            inp->SetInsertionPoint(pos);
            inp->WriteText(m_killBuf);
            m_histIdx = -1; ApplyHistRSearch(false);
        }
        return;
    }

    // Ctrl+A: move cursor to the beginning of the input line (Unix terminal).
    // Note: Ctrl+A is already bound to SelectAll on macOS for text controls,
    // so we only intercept it explicitly here; the result is the same on all
    // platforms — cursor moves to position 0.
    if (code == 'A' && evt.ControlDown()) {
        if (inp) inp->SetInsertionPoint(0);
        return;
    }

    // Ctrl+E: move cursor to the end of the input line (Unix terminal).
    if (code == 'E' && evt.ControlDown()) {
        if (inp) inp->SetInsertionPointEnd();
        return;
    }

    // Ctrl+Left / Ctrl+Right (Windows / Linux) or Alt+Left / Alt+Right (macOS):
    // move the insertion point one word backward or forward.
    // Word boundary: alphanumeric characters and underscores.  The cursor skips
    // any leading non-word characters and then jumps over the next word token.
    //
    // When Shift is also held, let the native wxTextCtrl handler run instead
    // (it already produces the correct word-selection behaviour on all platforms)
    // so we don't have to reimplement extend-selection logic.
    if ((code == WXK_LEFT || code == WXK_RIGHT) &&
        (evt.ControlDown() || evt.AltDown()))
    {
        if (evt.ShiftDown()) { evt.Skip(); return; }   // native handles Shift+word-nav
        if (inp) {
            long     pos  = inp->GetInsertionPoint();
            wxString text = inp->GetValue();
            long     len  = (long)text.size();
            if (code == WXK_LEFT) {
                // Skip non-word chars backward, then skip the preceding word.
                while (pos > 0 && !wxIsalnum(text[pos-1]) && text[pos-1] != '_') --pos;
                while (pos > 0 &&  (wxIsalnum(text[pos-1]) || text[pos-1] == '_')) --pos;
            } else {
                // Skip non-word chars forward, then skip the next word.
                while (pos < len && !wxIsalnum(text[pos]) && text[pos] != '_') ++pos;
                while (pos < len &&  (wxIsalnum(text[pos]) || text[pos] == '_')) ++pos;
            }
            inp->SetInsertionPoint(pos);
        }
        return;
    }

    // Ctrl+P / Ctrl+N: emacs-style aliases for history Up/Down.
    // Re-map the key code so the existing Up/Down handlers below take over
    // without duplicating any logic.  These must be checked before the
    // Tab and Up/Down blocks so the re-mapped value is seen by them.
    if (evt.ControlDown() && code == 'P') code = WXK_UP;
    else if (evt.ControlDown() && code == 'N') code = WXK_DOWN;

    // Shift+Tab: show a popup list of script-defined completion items and/or
    // Lua global function names (mirroring Windows FunctionMenu behaviour).
    // Scripts populate the list with world.ShiftTabCompleteItem(); the list
    // is filtered to items that start with whatever is currently in the input.
    if (code == WXK_TAB && inp == m_input && evt.ShiftDown() && m_doc) {
        std::vector<std::string> candidates;

        // ── Lua global functions ──────────────────────────────────────────
        if (m_doc->m_bTabCompleteFunctions) {
            CScriptEngine* eng = m_doc->GetScriptEngine();
            if (eng && eng->L) {
                lua_State* LS = eng->L;
                lua_pushvalue(LS, LUA_GLOBALSINDEX);
                lua_pushnil(LS);
                while (lua_next(LS, -2)) {
                    if (lua_type(LS, -1) == LUA_TFUNCTION &&
                        lua_type(LS, -2) == LUA_TSTRING) {
                        const char* fname = lua_tostring(LS, -2);
                        if (fname && fname[0] != '_')
                            candidates.push_back(fname);
                    }
                    lua_pop(LS, 1);
                }
                lua_pop(LS, 1);
            }
        }

        // ── Script-registered completion items ───────────────────────────
        for (const auto& item : m_doc->m_ExtraShiftTabCompleteItems)
            candidates.push_back(item);

        if (candidates.empty()) return;  // nothing to show — consume Shift+Tab

        // Sort and deduplicate (case-insensitive).
        std::sort(candidates.begin(), candidates.end(),
            [](const std::string& a, const std::string& b) {
                return strcasecmp(a.c_str(), b.c_str()) < 0;
            });
        candidates.erase(std::unique(candidates.begin(), candidates.end()),
                         candidates.end());

        // Filter to items that start with the current (partial) input.
        wxString partial = inp->GetValue().Lower();
        if (!partial.IsEmpty()) {
            std::vector<std::string> filtered;
            for (const auto& c : candidates) {
                wxString lc = wxString::FromUTF8(c.c_str()).Lower();
                if (lc.StartsWith(partial))
                    filtered.push_back(c);
            }
            if (!filtered.empty())
                candidates = std::move(filtered);
        }

        // Build wxArrayString for the dialog.
        wxArrayString wxCands;
        wxCands.Alloc(candidates.size());
        for (const auto& c : candidates)
            wxCands.Add(wxString::FromUTF8(c.c_str()));

        wxSingleChoiceDialog dlg(this,
            "Choose a function or command:",
            "Shift+Tab Completion",
            wxCands);

        // Pre-select the first entry.
        dlg.SetSelection(0);

        if (dlg.ShowModal() == wxID_OK) {
            inp->SetValue(wxCands[dlg.GetSelection()]);
            inp->SetInsertionPointEnd();
        }
        return;
    }

    // Tab: complete the current partial command.
    //
    // Phase 1 — history prefix search: scan backward through m_history for
    // the most recent command that starts with the current input.  A second Tab
    // cycles further back.  Not available in password mode.
    //
    // Phase 2 — output-word fallback: if history is exhausted (or there are no
    // history matches at all), scan the last ~300 output lines for words that
    // start with the partial input and cycle through them.  An extra Tab at the
    // end of the output-word list restores the original partial input.
    if (code == WXK_TAB && inp == m_input) {
        wxString partial = m_input->GetValue();
        if (partial.IsEmpty()) return;  // consume Tab without action

        // ── If we're already in an output-word cycle, just advance ───────────
        if (m_tabWordIdx >= 0) {
            ++m_tabWordIdx;
            if (m_tabWordIdx >= (int)m_tabWords.size()) {
                // Wrapped past the last candidate: restore the original partial.
                m_tabWordIdx = -1;
                m_tabWords.clear();
                m_input->SetValue(m_tabPartial);
                m_input->SetInsertionPointEnd();
            } else {
                m_input->SetValue(m_tabWords[m_tabWordIdx]);
                m_input->SetInsertionPointEnd();
            }
            return;
        }

        // ── Phase 1: history prefix search ───────────────────────────────────
        wxString lowerPartial = partial.Lower();
        {
            int searchFrom = (m_histIdx >= 0) ? m_histIdx - 1
                                               : (int)m_history.size() - 1;
            for (int i = searchFrom; i >= 0; --i) {
                const wxString& hist = m_history[i];
                // Match: history entry starts with partial (case-insensitive)
                // and is strictly longer (to avoid completing to identical text).
                if (hist.Lower().StartsWith(lowerPartial) &&
                    hist.Len() > partial.Len()) {
                    m_histIdx = i;
                    m_input->SetValue(hist);
                    m_input->SetInsertionPointEnd();
                    return;
                }
            }
        }

        // ── Phase 2: output-word completion fallback ──────────────────────────
        // History exhausted.  Build a unique, most-recent-first list of words
        // from the last ~300 output lines that start with the partial input.
        // Tokens: runs of [A-Za-z0-9_-] characters.
        m_tabPartial  = partial;
        m_tabWords.clear();
        m_tabWordIdx  = -1;

        if (m_output) {
            std::set<wxString> seen;
            int startLine = std::max(0, m_output->LineCount() - 300);
            for (int li = m_output->LineCount() - 1; li >= startLine; --li) {
                const SOutputLine* line = m_output->GetLine(li);
                if (!line) continue;
                for (const auto& run : *line) {
                    wxString text = wxString::FromUTF8(run.text.c_str());
                    wxString word;
                    for (size_t ci = 0; ci <= text.size(); ++ci) {
                        wxChar ch = (ci < text.size()) ? text[ci] : wxChar('\0');
                        bool wordChar = ci < text.size() &&
                                        (wxIsalnum(ch) || ch == '_' || ch == '-');
                        if (wordChar) {
                            word += ch;
                        } else if (!word.IsEmpty()) {
                            wxString wl = word.Lower();
                            if (wl.StartsWith(lowerPartial) &&
                                word.Len() > partial.Len() &&
                                seen.find(wl) == seen.end())
                            {
                                seen.insert(wl);
                                m_tabWords.push_back(word);
                            }
                            word.clear();
                        }
                    }
                }
            }
        }

        if (m_tabWords.empty()) return;  // no candidates — consume Tab silently

        m_tabWordIdx = 0;
        m_input->SetValue(m_tabWords[0]);
        m_input->SetInsertionPointEnd();
        return;
    }

    if (code == WXK_UP && inp == m_input) {
        // Pressing Up exits any ongoing output-word tab cycle and Ctrl+R mode.
        m_tabWordIdx  = -1;
        m_tabWords.clear();
        ApplyHistRSearch(false);  // ↑ always uses prefix (startsWith) match
        if (m_history.empty()) return;
        if (m_histIdx < 0) {
            // Start browsing: save the current partial input as the prefix filter.
            // If the user has typed "say ", ↑ will only cycle through "say …"
            // entries — like zsh's up-line-or-beginning-search.  An empty input
            // browses all entries from newest to oldest.
            m_histSaved   = m_input->GetValue();
            wxString lpfx = m_histSaved.Lower();
            // Find the most recent entry that starts with the current input.
            m_histIdx = -1;
            for (int i = (int)m_history.size() - 1; i >= 0; --i) {
                if (lpfx.IsEmpty() || m_history[i].Lower().StartsWith(lpfx)) {
                    m_histIdx = i;
                    break;
                }
            }
            if (m_histIdx < 0) return;  // no match — consume but don't change input
        } else {
            // Continue searching backward for the next older match.
            wxString lpfx = m_histSaved.Lower();
            int found = -1;
            for (int i = m_histIdx - 1; i >= 0; --i) {
                if (lpfx.IsEmpty() || m_history[i].Lower().StartsWith(lpfx)) {
                    found = i;
                    break;
                }
            }
            if (found < 0) return;  // at oldest match — stay put
            m_histIdx = found;
        }
        m_input->SetValue(m_history[m_histIdx]);
        m_input->SetInsertionPointEnd();
        return;  // consume — don't scroll the output pane
    }

    if (code == WXK_DOWN && inp == m_input) {
        // Pressing Down exits any ongoing output-word tab cycle and Ctrl+R mode.
        m_tabWordIdx  = -1;
        m_tabWords.clear();
        ApplyHistRSearch(false);  // ↓ always uses prefix (startsWith) match
        if (m_histIdx < 0) { evt.Skip(); return; }
        wxString lpfx = m_histSaved.Lower();
        // Search forward for the next newer match.
        int found = -1;
        for (int i = m_histIdx + 1; i < (int)m_history.size(); ++i) {
            if (lpfx.IsEmpty() || m_history[i].Lower().StartsWith(lpfx)) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            // Past the newest match: restore the partial input the user was typing.
            m_histIdx = -1;
            m_input->SetValue(m_histSaved);
        } else {
            m_histIdx = found;
            m_input->SetValue(m_history[m_histIdx]);
        }
        m_input->SetInsertionPointEnd();
        return;
    }

    // Ctrl+R: reverse incremental history search (substring match, newest first).
    // Mirrors Ctrl+R in bash/zsh: each press cycles to the next older entry
    // whose text contains the current input as a substring (case-insensitive).
    // Not available in password mode.
    if (code == 'R' && evt.ControlDown() && inp == m_input) {
        m_tabWordIdx = -1;
        m_tabWords.clear();
        ApplyHistRSearch(true);

        // Use the current input as the query on first press; on subsequent
        // presses keep m_histSaved (so editing history entries mid-cycle
        // doesn't confuse the search).
        wxString query = (m_histIdx < 0) ? m_input->GetValue() : m_histSaved;
        wxString lq    = query.Lower();

        int startFrom;
        if (m_histIdx < 0) {
            // Enter Ctrl+R mode: save the current text as the query.
            m_histSaved = m_input->GetValue();
            startFrom   = (int)m_history.size() - 1;
        } else {
            // Continue backward from one before the current position.
            startFrom   = m_histIdx - 1;
        }

        int found = -1;
        for (int i = startFrom; i >= 0; --i) {
            bool match = lq.IsEmpty()
                ? true
                : m_history[i].Lower().Contains(lq);
            if (match) { found = i; break; }
        }

        if (found >= 0) {
            m_histIdx = found;
            m_input->SetValue(m_history[m_histIdx]);
            m_input->SetInsertionPointEnd();
        } else if (m_histIdx < 0) {
            // No match at all on first press — stay put (don't change input).
        }
        // If already cycling and there are no more older matches, stay put.
        return;
    }

    // Ctrl+F → open the find bar.
    if (code == 'F' && evt.ControlDown()) {
        ShowFindBar(true);
        return;   // consume — don't forward to the doc
    }

    // F3 / Shift+F3 → find next / previous (opens the bar if not already shown).
    if (code == WXK_F3) {
        ShowFindBar(true);
        DoFind(!evt.ShiftDown());
        return;
    }

    // Ctrl+L → clear the output pane (same as the menu item and Ctrl+L in the
    // output pane itself; terminal convention: clear screen without disconnect).
    if (code == 'L' && evt.ControlDown()) {
        if (m_output) m_output->ClearOutput();
        return;
    }

    // Any non-navigation key cancels history browsing, Ctrl+R mode, and tab cycling.
    if (code != WXK_UP && code != WXK_DOWN) {
        m_histIdx    = -1;
        ApplyHistRSearch(false);
        m_tabWordIdx = -1;
        m_tabWords.clear();
    }

    evt.Skip();
}

// ── OnInputPaste ──────────────────────────────────────────────────────────
// Multi-line paste splitter.
//
// When the user pastes text that contains newlines, split it at each '\n'.
// Every line except the last is sent as a MUD command (exactly as if the user
// had typed it and pressed Enter); the last fragment is left in the input bar
// so the user can review and edit it before sending.  Single-line pastes fall
// through to the normal wxTextCtrl paste behaviour.
//
// Any text already in the input bar is prepended to the first pasted line:
//   typed "say " + paste "hello\nworld"
//   → sends "say hello", leaves "world" in the input bar.
void CWorldChildFrame::OnInputPaste(wxClipboardTextEvent& evt)
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

    // Normalise CRLF / CR → LF.
    clipText.Replace(wxT("\r\n"), wxT("\n"));
    clipText.Replace(wxT("\r"),   wxT("\n"));

    if (!clipText.Contains('\n')) {
        evt.Skip();   // single-line: let wxWidgets insert it normally
        return;
    }

    // Suppress the default paste — we handle the text ourselves.
    // (Not calling evt.Skip() suppresses the default paste on wxTextCtrl.)

    wxArrayString lines = wxSplit(clipText, '\n');

    // Prepend whatever the user had already typed to the first pasted line.
    wxString prefix = m_input->GetValue();
    m_input->Clear();
    m_histIdx = -1; ApplyHistRSearch(false);  // exit history browsing

    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        wxString cmd = (i == 0) ? prefix + lines[i] : lines[i];

        // Track in history (dedup consecutive identical entries).
        if (!cmd.IsEmpty()) {
            if (m_history.empty() || m_history.back() != cmd)
                m_history.push_back(cmd);
            if ((int)m_history.size() > kMaxHistory)
                m_history.erase(m_history.begin());
        }

        if (!m_doc) continue;
        CString cCmd(cmd.ToUTF8().data());
        bool bOmit = false;
        m_doc->EvaluateCommand(cCmd, /*countThem=*/true, bOmit);
    }

    // Flush output from alias callbacks / triggers before leaving.
    if (m_doc) m_doc->UpdateAllViews(nullptr);

    // Leave the trailing fragment in the input bar.
    m_input->SetValue(lines.back());
    m_input->SetInsertionPointEnd();
}

// ── OnInputEnter ──────────────────────────────────────────────────────────
void CWorldChildFrame::OnInputEnter(wxCommandEvent& /*evt*/)
{
    if (!m_doc) return;

    wxTextCtrl* inp = ActiveInput();
    if (!inp) return;

    wxString text = inp->GetValue();
    inp->Clear();
    m_histIdx = -1; ApplyHistRSearch(false);   // exit history browsing on send

    // Don't record passwords in history — m_bNoEcho signals a masked prompt.
    bool isPassword = m_doc->m_bNoEcho;
    if (!isPassword && !text.empty()) {
        // Add to history (no duplicates at the tail; trim if over limit).
        if (m_history.empty() || m_history.back() != text)
            m_history.push_back(text);
        if ((int)m_history.size() > kMaxHistory)
            m_history.erase(m_history.begin());
    }

    // EvaluateCommand processes aliases, speed-walks, and scripting callbacks
    // before sending the text to the MUD — the same path Windows uses for
    // keyboard input from CMUSHclientView::OnChar / ProcessCommand.
    CString cmd(text.ToUTF8().data());
    bool bOmitFromLog = isPassword;   // don't log passwords either
    m_doc->EvaluateCommand(cmd, true /*countThem*/, bOmitFromLog);

    // Flush any output produced by alias scripts / triggers to the pane.
    m_doc->UpdateAllViews(nullptr);
}

// ── OnWorldTimer ──────────────────────────────────────────────────────────
// Called every 100 ms.  Drives MUSHclient world/plugin timers, flushes
// output, updates UI on phase changes, and runs the auto-reconnect countdown.
void CWorldChildFrame::OnWorldTimer(wxTimerEvent& /*evt*/)
{
    if (!m_doc) return;

    // ── World timers (triggers based on elapsed time, speed-walk queues, etc.)
    m_doc->CheckTimers();

    // ── ON_PLUGIN_TICK: Windows fires this every ~40 ms.  We drive it via an
    //    accumulator so that the average rate matches even though our timer
    //    only fires every 100 ms.  Each 100 ms tick we fire it 2–3 times,
    //    keeping the long-run rate at 1 call per 40 ms.
    m_pluginTickAccum += 100;
    while (m_pluginTickAccum >= 40) {
        m_pluginTickAccum -= 40;
        m_doc->SendToAllPluginCallbacks(ON_PLUGIN_TICK);
    }

    // Flush any output produced by the timer callbacks.
    m_doc->UpdateAllViews(nullptr);

    // ── Connection-phase change: update title and output pane notes ───────
    int phase     = m_doc->m_iConnectPhase;
    int prevPhase = m_lastConnectPhase;

    if (phase != prevPhase) {
        m_lastConnectPhase = phase;

        // Build the window title suffix (empty when connected = clean title).
        const char* titleStatus = nullptr;   // nullptr → no suffix
        switch (phase) {
        case eConnectMudNameLookup:
        case eConnectProxyNameLookup:        titleStatus = "Resolving\xe2\x80\xa6";     break;
        case eConnectConnectingToMud:
        case eConnectConnectingToProxy:      titleStatus = "Connecting\xe2\x80\xa6";    break;
        case eConnectAwaitingProxyResponse1:
        case eConnectAwaitingProxyResponse2:
        case eConnectAwaitingProxyResponse3: titleStatus = "Proxy\xe2\x80\xa6";         break;
        case eConnectAwaitingSSLHandshake:   titleStatus = "TLS\xe2\x80\xa6";           break;
        case eConnectConnectedToMud:         titleStatus = nullptr;                     break;
        case eConnectDisconnecting:          titleStatus = "Disconnecting\xe2\x80\xa6"; break;
        default:                             titleStatus = "Closed";                    break;
        }

        wxString name = wxString::FromUTF8(
            static_cast<const char*>(m_doc->m_mush_name));
        if (name.IsEmpty()) name = wxT("World");

        if (!titleStatus)
            SetTitle(name);
        else
            SetTitle(name + wxT(" (") + wxString::FromUTF8(titleStatus) + wxT(")"));

        // Common strings used in output-pane notes.
        std::string server   = static_cast<const char*>(m_doc->m_server);
        std::string port     = std::to_string(m_doc->m_port);
        std::string hostport = server + ":" + port;

        // ── Output pane annotations ───────────────────────────────────────
        // Suppress the startup pass (prevPhase == -1, phase == eConnectNotConnected)
        // so we don't show a spurious "not connected" note when a world first opens.
        if (prevPhase >= 0 && m_output) {

            if (phase == eConnectConnectedToMud) {
                // Green — successful connection.
                m_output->AppendNote("** Connected to " + hostport + " **",
                                     RGB(68, 204, 68));
                m_reconnectCountdown = 0;
                TriggerNAWS();

            } else if (phase == eConnectNotConnected) {
                if (prevPhase == eConnectConnectedToMud ||
                    prevPhase == eConnectDisconnecting) {
                    // Amber — link dropped or user-initiated disconnect.
                    m_output->AppendNote("** Disconnected from " + hostport + " **",
                                         RGB(255, 136, 0));
                    // Auto-reconnect on unexpected link failure.
                    if (!m_doc->m_bDisconnectOK && App.m_bReconnectOnLinkFailure) {
                        m_reconnectCountdown = 50;  // 50 × 100 ms = 5 s
                    }
                } else if (prevPhase != eConnectNotConnected) {
                    // Red — connection attempt failed.
                    m_output->AppendNote("** Connection to " + hostport + " failed **",
                                         RGB(255, 68, 68));
                }

            } else if (phase == eConnectMudNameLookup &&
                       prevPhase == eConnectNotConnected) {
                // Cyan — new connection attempt starting.
                m_output->AppendNote("** Connecting to " + hostport + "\xe2\x80\xa6 **",
                                     RGB(0, 204, 204));
            }
        }

        // Startup case: already connected when the frame opens (rare but safe).
        if (prevPhase < 0 && phase == eConnectConnectedToMud)
            TriggerNAWS();

        // Refresh the status bar for the active world.
        if (IsActiveWorld())
            UpdateStatusBar();
    }

    // ── Telnet echo suppression (password mode) ───────────────────────────
    // The MUD sends IAC WILL ECHO to suppress echo (password prompts) and
    // IAC WONT ECHO to restore it.  We swap the input bar for a masked one.
    if (m_doc->m_bNoEcho != m_lastNoEcho)
        ApplyNoEcho(m_doc->m_bNoEcho);

    // ── Auto-reconnect countdown ──────────────────────────────────────────
    if (m_reconnectCountdown > 0) {
        if (phase != eConnectNotConnected) {
            // Reconnect already started (phase advanced) — clear the counter.
            m_reconnectCountdown = 0;
        } else {
            --m_reconnectCountdown;
            if (m_reconnectCountdown == 0) {
                // Time to fire the reconnect.
                if (m_output)
                    m_output->AppendNote("** Reconnecting\xe2\x80\xa6 **",
                                         RGB(255, 204, 0));
                m_doc->m_bDisconnectOK = false;
                m_doc->ConnectSocket();
            }
            // Refresh status bar with countdown (only if active).
            if (IsActiveWorld())
                UpdateStatusBar();
        }
    }
}

// ── OnClose ───────────────────────────────────────────────────────────────
void CWorldChildFrame::OnClose(wxCloseEvent& evt)
{
    // ── Unsaved-changes prompt ────────────────────────────────────────────
    // If the document has been modified (triggers, aliases, properties changed)
    // since it was last saved, offer to save before closing.
    // Only when the close event can be vetoed (not a forced app shutdown).
    if (evt.CanVeto() && m_doc && m_doc->IsModified()) {
        wxString name = wxString::FromUTF8(
            static_cast<const char*>(m_doc->m_mush_name));
        if (name.IsEmpty()) name = wxT("this world");

        int answer = wxMessageBox(
            wxT("Save changes to ") + name + wxT(" before closing?"),
            wxT("Unsaved Changes"),
            wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxICON_QUESTION, this);

        if (answer == wxCANCEL) {
            evt.Veto();
            return;
        }
        if (answer == wxYES) {
            // Attempt to save.  If the user cancels the save-as dialog (no path
            // yet) or the save fails, veto the close so no work is lost.
            if (g_wxhook_save_world) {
                bool saved = g_wxhook_save_world(m_doc, /*saveAs=*/false, this);
                if (!saved) {
                    evt.Veto();
                    return;
                }
            }
        }
        // answer == wxNO → discard changes and fall through to close.
    }

    // ── Close confirmation ────────────────────────────────────────────────
    // General "are you sure?" prompt (separate from the save prompt above).
    // Skipped when we already asked about saving — no need to ask twice.
    else if (evt.CanVeto() && App.m_bConfirmBeforeClosingWorld && m_doc &&
             !m_doc->IsModified()) {
        wxString name = wxString::FromUTF8(
            static_cast<const char*>(m_doc->m_mush_name));
        if (name.IsEmpty()) name = wxT("this world");
        int answer = wxMessageBox(
            wxT("Close ") + name + wxT("?"),
            wxT("Confirm Close"),
            wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this);
        if (answer != wxYES) {
            evt.Veto();
            return;
        }
    }

    // ── Save child frame size for next session ────────────────────────────
    // Persist the current window dimensions so the next world that opens uses
    // the same size instead of reverting to the 800×600 default.
    {
        wxSize sz = GetSize();
        if (sz.x >= 200 && sz.y >= 100) {
            wxConfig cfg(wxT("mushclient"));
            cfg.Write(wxT("/geometry/child_w"), (long)sz.x);
            cfg.Write(wxT("/geometry/child_h"), (long)sz.y);
        }
    }

    // ── Save persistent command history ───────────────────────────────────
    // Write each command on its own line to the same per-world file determined
    // at construction time, so the path is consistent regardless of later
    // World Properties renames.
    if (!m_history.empty()) {
        std::string histPath = m_historyFilePath;
        if (!histPath.empty()) {
            if (FILE* f = fopen(histPath.c_str(), "w")) {
                for (const wxString& cmd : m_history)
                    fprintf(f, "%s\n", cmd.ToUTF8().data());
                fclose(f);
            }
        }
    }

    // Close any open log file before the doc is destroyed.
    if (IsLogging()) StopLogging();

    // Stop the world timer before we touch the doc — prevents a timer callback
    // from firing between m_fn_line_ready=nullptr and delete doc.
    if (m_worldTimer) {
        m_worldTimer->Stop();
        m_worldTimer = nullptr;
    }

    if (m_doc) {
        // Capture the world file path before clearing the doc, so we can
        // remove it from the startup-reopen list below.
        std::string worldPath = static_cast<const char*>(m_doc->GetPathName());

        // Clear all callbacks BEFORE deleting the doc so that any final output
        // flushed by the destructor doesn't call AppendLine on the dying pane.
        m_doc->m_fn_line_ready      = nullptr;
        m_doc->m_fn_note            = nullptr;
        m_doc->m_fn_begin_batch     = nullptr;
        m_doc->m_fn_flush           = nullptr;
        m_doc->m_fn_get_command          = nullptr;
        m_doc->m_fn_set_command          = nullptr;
        m_doc->m_fn_set_output_font      = nullptr;
        m_doc->m_fn_set_scroll           = nullptr;
        m_doc->m_fn_set_selection        = nullptr;
        m_doc->m_fn_set_command_selection = nullptr;
        m_doc->m_fn_set_paused           = nullptr;
        m_doc->m_fn_repaint                  = nullptr;
        m_doc->m_fn_status                   = nullptr;
        m_doc->m_fn_set_title                = nullptr;
        m_doc->m_fn_set_main_title           = nullptr;
        m_doc->m_fn_set_background_colour    = nullptr;
        m_doc->m_fn_set_max_lines            = nullptr;
        m_doc->m_fn_get_clipboard            = nullptr;
        m_doc->m_fn_set_clipboard            = nullptr;
        m_doc->m_fn_get_output_rect          = nullptr;
        m_doc->m_fn_get_main_rect            = nullptr;
        m_doc->m_fn_activate_client          = nullptr;
        m_doc->m_fn_delete_lines             = nullptr;
        m_doc->m_fn_select_command           = nullptr;
        m_doc->m_fn_delete_command_history   = nullptr;
        m_doc->m_fn_push_command             = nullptr;
        m_doc->m_fn_get_command_list         = nullptr;
        m_doc->m_fn_get_command_selection    = nullptr;
        m_doc->m_fn_get_output_selection     = nullptr;

        // Remove from the doc→frame registry before deleting so that no hook
        // can look up a dangling frame pointer during the destructor.
        wx_unregister_world_frame(m_doc);

        // Remove from the startup world list so the world isn't automatically
        // reopened next time — the user explicitly closed it.
        if (!worldPath.empty())
            wx_world_remove_from_list(worldPath.c_str());

        // Disconnect and release the doc.  Deletes socket, triggers, timers, etc.
        CMUSHclientDoc* doc = m_doc;
        m_doc = nullptr;    // prevent re-entrancy
        delete doc;
    }
    evt.Skip();  // let wxWidgets destroy the frame and its child widgets
}

// ── OnSize ────────────────────────────────────────────────────────────────────
// Called whenever the frame (and therefore the output pane) is resized.
// Propagates the new character-grid dimensions to the MUD via a NAWS packet.
//
// We must defer the NAWS send via CallAfter: evt.Skip() marks the event for
// further processing but the base-class OnSize (which calls Layout() to resize
// child windows) runs AFTER our handler returns.  Reading the pane's client
// size before returning gives stale dimensions.  CallAfter executes on the
// next event-loop iteration, after Layout() has already updated child sizes.
void CWorldChildFrame::OnSize(wxSizeEvent& evt)
{
    evt.Skip();  // let wxMDIChildFrame / sizer resize children

    CallAfter([this]() {
        if (!m_doc || !m_output) return;

        int charW = m_output->CharWidth();
        int lineH = m_output->LineHeight();
        if (charW <= 0 || lineH <= 0) return;

        int paneW, paneH;
        m_output->GetClientSize(&paneW, &paneH);
        if (paneW <= 0 || paneH <= 0) return;

        int cols = paneW / charW;
        int rows = paneH / lineH;
        // Keep m_nWrapColumn in sync so the engine wraps output at the correct
        // column width.  Matches what cli_main.cpp does on SIGWINCH.
        m_doc->m_nWrapColumn = (unsigned short)std::max(1, std::min(cols, 65535));
        m_doc->SendNAWS(cols, rows);
    });
}

#endif // !_WIN32
