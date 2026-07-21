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
#include <netdb.h>  // getaddrinfo, freeaddrinfo

// ── MUSHclient headers ────────────────────────────────────────────────────
// stdafx.h and MUSHclient.h must precede doc.h: MUSHclient.h pulls in
// OtherTypes.h (MACRO_COUNT, CAliasMap, CTriggerMap, …) that doc.h needs.
#include "stdafx.h"                   // compat.h → mfc_ui.h → wx_mfc_hooks.h
#include "MUSHclient.h"               // CMUSHclientApp + OtherTypes.h + all prerequisites
#include "doc.h"                      // CMUSHclientDoc, SendMsg, etc.
#include "dialogs/GlobalChangeDlg.h"  // CGlobalChangeDlg (Game ▸ Global Change)
#include "dialogs/GeneratedNameDlg.h" // CGeneratedNameDlg (Game ▸ Generate Character Name)
#include "dialogs/HighlightPhraseDlg.h"  // CHighlightPhraseDlg (Display ▸ Highlight Word)
#include "dialogs/MultiLineTriggerDlg.h" // CMultiLineTriggerDlg (Display ▸ Multi-Line Trigger)
#include "dialogs/TextAttributesDlg.h"   // CTextAttributesDlg (Display ▸ Text Attributes)
#include "miniwindow.h"               // CMiniWindow, MiniWindowVector, MINIWINDOW_* flags
#include "scripting/scripting.h"      // CScriptEngine, lua_State* — for Shift+Tab globals
#include "platform/wx_world_child.h"  // our own header (CWorldChildFrame, COutputPane)
#include "platform/wx_print.h"        // PrintOutputPane (File ▸ Print World)
#include <wx/numdlg.h>                // wxGetNumberFromUser (Display ▸ Go To Line)

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
#  define MW_MOUSE_SCROLL_BACK 0x100
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

// Fallback command-history depth, used only when the doc is unavailable.  The
// real limit is the world's own "Lines in command history" setting
// (m_nHistoryLines, default 1000, range 20-5000) — see HistoryLimit().
static const int kMaxHistoryFallback = 1000;

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

// The MUD should wrap to the VISIBLE text width: the full output width minus any
// area a plugin reserved via world.TextRectangle (e.g. Quow's docked
// miniwindows).  Without this the MUD wraps to the full width and the client
// re-wraps long lines unexpectedly.  m_gui_cols / GetInfo(281) stays the FULL
// width — plugins compute their own margins from it; only the engine wrap column
// and NAWS use this reduced count.
static int EffectiveTextCols(CMUSHclientDoc* doc, int paneW, int charW)
{
    if (charW <= 0) return 1;
    int textW = paneW;
    if (doc)
    {
        const CRect& tr = doc->m_TextRectangle;
        if (!(tr.left == 0 && tr.top == 0 && tr.right == 0 && tr.bottom == 0))
        {
            int rw = (int) (tr.right - tr.left);
            if (rw > 0 && rw < paneW) textW = rw;
        }
    }
    return std::max(1, textW / charW);
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

    // world.Pause(bPause) now drives IWorldView::setPaused (this frame) via
    // CMUSHclientDoc::EmitPaused; see the pal::IWorldView section below.
    if (m_doc) {
        // world.Repaint() now drives IWorldView::requestRepaint (this frame) via
        // CMUSHclientDoc::EmitRepaint; body in the pal::IWorldView section below.

        // Async DNS now runs inside the PAL transport (pal::ITransport, T2d):
        // its getaddrinfo thread posts the completion to a main-thread mailbox
        // drained by pal::PumpTransportCompletions() (wx_mfc_impl poll timer),
        // gate-guarded against doc/transport teardown mid-lookup.

        // world.ClearOutput() now drives IWorldView::clearOutput (this frame) via
        // CMUSHclientDoc::EmitClearOutput; the engine resets the emission counter.

        // world.SetStatus(msg) and world.SetTitle(s) now drive
        // IWorldView::setStatus / ::setTitle (this frame) via
        // CMUSHclientDoc::EmitStatus / ::EmitTitle; the bodies live in the
        // pal::IWorldView section below.

        // world.SetBackgroundColour(rgb) and SetOption("max_output_lines", n) now
        // drive IWorldView::setBackgroundColour / ::setMaxLines (this frame) via
        // CMUSHclientDoc::EmitBackgroundColour / ::EmitMaxLines; bodies below.

        // world.GetSelectionStartLine / …Column now drive IWorldView::outputSelection
        // (this frame) via CMUSHclientDoc::GetOutputSelection; body in the section below.

        // world.GetCommand() now drives IWorldView::currentCommand (this frame) via
        // CMUSHclientDoc::GetCommandText; body in the section below.

        // GetInfo(296) scroll offset now drives IWorldView::scrollPos (this frame)
        // via CMUSHclientDoc::GetScrollPos; body in the section below.

        // ── Output display seam ───────────────────────────────────────────
        // This frame IS the world's pal::IWorldView (see the header): the engine
        // drives finished output lines through it via EmitOutputLine → appendLine.
        // Set here so it is never accidentally overwritten after construction.
        m_doc->m_pWorldView = this;
        // ── Apply initial pane state from the doc ─────────────────────────
        // For quick-connect worlds the doc fields are at defaults (no face,
        // black background, 5000-line scrollback).  For world-file worlds the
        // doc has already been loaded via OnOpenDocument before the constructor
        // runs, so these reflect the saved world settings.
        if (m_output) {
            // Font: m_font_name is face name; m_font_height encodes size as either:
            //   negative → LOGFONT pixel height at 96 DPI (from world XML saved on
            //              Windows, e.g. -12 = 9pt); convert to points.
            //   positive → already a point size (scripting options default = 12;
            //              App.m_iDefaultOutputFontHeight default = 9).
            // Skip altogether if neither face nor height is configured.
            const char* face = static_cast<const char*>(m_doc->m_font_name);
            int         lfH  = (int)m_doc->m_font_height;
            if ((face && *face) || lfH != 0) {
                int points;
                if (lfH < 0)
                    points = (-lfH * 72 + 48) / 96;  // LOGFONT pixels → points
                else if (lfH > 0)
                    points = lfH;                      // already a point size
                else
                    points = COutputPane::kDefaultFontSize;
                points = std::max(COutputPane::kMinFontSize,
                                  std::min(COutputPane::kMaxFontSize, points));
                wxFontWeight weight = (m_doc->m_font_weight >= 700)
                                      ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL;
                // An empty or Windows-only face name (e.g. "FixedSys") still
                // produces a valid font via wxFONTFAMILY_TELETYPE fallback.
                wxString faceName = (face && *face)
                                    ? wxString::FromUTF8(face) : wxString();
                wxFont font(points, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                            weight, false, faceName);
                if (font.IsOk()) m_output->SetOutputFont(font);
            }
            // Scrollback limit.
            if (m_doc->m_maxlines > 0) m_output->SetMaxLines(m_doc->m_maxlines);
            // Auto-freeze: if the world has it disabled, the pane should keep
            // auto-scrolling even when the user has scrolled up.
            m_output->SetAllowAutoFreeze(m_doc->m_bAutoFreeze != 0);
            // Start-paused: if the world's "auto_pause" option is set, begin
            // with auto-scroll disengaged so output doesn't scroll on arrival.
            if (m_doc->m_bStartPaused)
                m_output->SetPaused(true);
            // Background colour (NO_COLOUR = 0xFFFFFFFF → black).
            ApplyPaneBackground(m_doc->m_iBackgroundColour);
            // Word delimiters for double-click selection (global pref).
            m_output->SetWordDelimitersDblClick(
                static_cast<const char*>(App.m_strWordDelimitersDblClick));
            // Auto-copy selection to clipboard (world pref).
            m_output->SetCopySelectionToClipboard(
                m_doc->m_bCopySelectionToClipboard != 0);
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

    // Forward accelerator key events from the output pane to the world frame's
    // FireAcceleratorIfMatch so Lua Accelerator() bindings fire even when the
    // user has scrolled the output pane and focus is there rather than the input.
    m_output->m_fn_accelerator_check = [this](wxKeyEvent& e) {
        return FireAcceleratorIfMatch(e);
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
            int textCols = EffectiveTextCols(m_doc, paneW, charW);
            m_doc->m_nWrapColumn    = (unsigned short)std::max(1, std::min(textCols, 65535));
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
            m_doc->SendNAWS(textCols, rows);
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

    // Keep the doc's GUI geometry (GetInfo 280/281) current on every pane resize
    // — including layout-driven shrinks that don't resize the frame (the input
    // bar claiming its space at startup), which the frame's OnSize misses.  This
    // is why a plugin's bottom margin (computed from GetInfo(280)) used to land
    // too low: GetInfo(280) kept the taller startup height.
    m_output->m_fn_resized = [this]() {
        if (!m_doc || !m_output) return;
        int charW = m_output->CharWidth();
        int lineH = m_output->LineHeight();
        if (charW <= 0 || lineH <= 0) return;
        int paneW, paneH;
        m_output->GetClientSize(&paneW, &paneH);
        if (paneW <= 0 || paneH <= 0) return;
        m_doc->m_gui_cols        = paneW / charW;
        m_doc->m_gui_rows        = paneH / lineH;
        m_doc->m_gui_font_height = lineH;
        m_doc->m_gui_font_width  = charW;
        // The frame's OnSize fires OnPluginWorldOutputResized, but at startup the
        // pane shrink (the input bar claiming its space) settles the geometry
        // through HERE only.  Without re-notifying, Quow's plugins keep the
        // miniwindow positions they computed from the stale start-up geometry and
        // stay stacked at the top-left until the first manual resize.  Defer it
        // (CallAfter is auto-cancelled if the frame closes) so the plugins don't
        // re-enter wx in the middle of the resize.
        CallAfter([this]() {
            if (m_doc) m_doc->SendToAllPluginCallbacks(ON_PLUGIN_WORLD_OUTPUT_RESIZED);
        });
    };

    // ── Miniwindow compositing callback ───────────────────────────────────
    // Called from COutputPane::OnPaint() twice per frame: once before text
    // (bUnder=true, MINIWINDOW_DRAW_UNDERNEATH) and once after (bUnder=false).
    // The DC is already positioned at client origin (scroll removed by caller).
    // We implement our own rectangle calculation here rather than calling the
    // Windows-only CMUSHView::Calculate_MiniWindow_Rectangles().
    // Text rectangle (world.TextRectangle): expose the world's reserved text
    // area so the output pane shifts/narrows its text to avoid docked
    // miniwindows.  Mirrors CMUSHView::HaveTextRectangle() — active when the
    // rectangle is not all-zero.
    m_output->m_fn_text_rect = [this](int& l, int& t, int& r, int& b) -> bool {
        if (!m_doc) return false;
        const CRect& tr = m_doc->m_TextRectangle;
        if (tr.left == 0 && tr.top == 0 && tr.right == 0 && tr.bottom == 0)
            return false;
        l = tr.left; t = tr.top; r = tr.right; b = tr.bottom;
        return true;
    };

    // ── SetBackgroundImage / SetForegroundImage (Windows OnDraw parity) ────
    // The exact Blit_Bitmap !bUseRect placement: modes 0/2 stretch to the
    // pane/frame, 1/3 aspect-stretch, 4-12 positioned 1:1 (corners / wall
    // centres / centre), 13 tile. Formerly the images loaded on the port but
    // the view never painted them.
    auto draw_doc_image = [this](wxDC& dc, int clientW, int clientH,
                                 CBitmap& bmp, int mode)
    {
        // Draw straight from the wxBitmap the shim already holds — a wxMemoryDC
        // source, no shim CDC (whose CreateCompatibleDC/SelectObject depend on
        // the FULL_BUILD guard that isn't set for this TU).
        wxBitmap* wxbmp = WxBitmapFromCBitmap(&bmp);
        if (!wxbmp || !wxbmp->IsOk())
            return;   // no image loaded
        const int w = wxbmp->GetWidth(), h = wxbmp->GetHeight();
        if (w <= 0 || h <= 0)
            return;

        int fw = 0, fh = 0;
        GetClientSize(&fw, &fh);              // frame rect, for modes 2/3

        wxMemoryDC src(*wxbmp);
        switch (mode) {                       // == Blit_Bitmap's !bUseRect placement
        case 0:  dc.StretchBlit(0, 0, clientW, clientH, &src, 0, 0, w, h, wxCOPY); break;
        case 1:  dc.StretchBlit(0, 0, (int)(clientH * (double)w / h), clientH,
                                &src, 0, 0, w, h, wxCOPY); break;
        case 2:  dc.StretchBlit(0, 0, fw, fh, &src, 0, 0, w, h, wxCOPY); break;
        case 3:  dc.StretchBlit(0, 0, (int)(fh * (double)w / h), fh,
                                &src, 0, 0, w, h, wxCOPY); break;
        case 4:  dc.Blit(0, 0, w, h, &src, 0, 0, wxCOPY); break;
        case 5:  dc.Blit((clientW - w) / 2, 0, w, h, &src, 0, 0, wxCOPY); break;
        case 6:  dc.Blit(clientW - w, 0, w, h, &src, 0, 0, wxCOPY); break;
        case 7:  dc.Blit(clientW - w, (clientH - h) / 2, w, h, &src, 0, 0, wxCOPY); break;
        case 8:  dc.Blit(clientW - w, clientH - h, w, h, &src, 0, 0, wxCOPY); break;
        case 9:  dc.Blit((clientW - w) / 2, clientH - h, w, h, &src, 0, 0, wxCOPY); break;
        case 10: dc.Blit(0, clientH - h, w, h, &src, 0, 0, wxCOPY); break;
        case 11: dc.Blit(0, (clientH - h) / 2, w, h, &src, 0, 0, wxCOPY); break;
        case 12: dc.Blit((clientW - w) / 2, (clientH - h) / 2, w, h, &src, 0, 0, wxCOPY); break;
        case 13: {                            // tile
            const int across = clientW / w + 1, down = clientH / h + 1;
            for (int tx = 0; tx < across; ++tx)
                for (int ty = 0; ty < down; ++ty)
                    dc.Blit(tx * w, ty * h, w, h, &src, 0, 0, wxCOPY);
            break;
        }
        }
    };

    m_output->m_fn_draw_background_image =
        [this, draw_doc_image](wxDC& dc, int cw, int ch) {
        if (m_doc)
            draw_doc_image(dc, cw, ch, m_doc->m_BackgroundBitmap,
                           m_doc->m_iBackgroundMode);
    };
    m_output->m_fn_draw_foreground_image =
        [this, draw_doc_image](wxDC& dc, int cw, int ch) {
        if (m_doc)
            draw_doc_image(dc, cw, ch, m_doc->m_ForegroundBitmap,
                           m_doc->m_iForegroundMode);
    };

    // ── ON_PLUGIN_DRAW_OUTPUT_WINDOW (docs/PAL.md S4c) ─────────────────────
    // Windows passes a 1-based DOC line index plus the pixel scroll offset.
    // The pane speaks in its own line list, which tail-aligns with the doc's
    // (both append one entry per completed output line and trim to the same
    // maximum), so the doc line = doc count - pane count + pane index.
    m_output->m_fn_draw_output_window = [this](int firstPaneLine, int pixelOffset) {
        if (!m_doc) return;
        long docLine = (long)m_doc->GetLastLine()
                     - (long)m_output->LineCount() + firstPaneLine + 1;   // 1-based
        if (docLine < 1) docLine = 1;
        m_doc->SendToAllPluginCallbacks(ON_PLUGIN_DRAW_OUTPUT_WINDOW,
                                        docLine, pixelOffset, "", false, false);
    };

    m_output->m_fn_composite_miniwindows =
        [this](wxDC& destDC, int clientW, int clientH, bool bUnder)
    {
        if (!m_doc) return;
        MiniWindowVector& order = m_doc->m_MiniWindowsOrder;
        if (order.empty()) return;

        // ── Phase 1: compute m_rect for this pass's miniwindows ─────────
        // Engine-owned placement maths shared verbatim with the Windows view
        // (CalculateMiniWindowRectangles, miniwindow_compose.cpp, docs/PAL.md
        // S4). Runs per pass with pass-scoped corner tracking, exactly as
        // CMUSHView::OnDraw always has — including the original's quirky
        // m_rect right/bottom values for corner/wall position modes, which
        // WindowInfo exposes to scripts (Windows parity over local tidiness).
        // "frame" rect = this child frame; "client" rect = output pane.
        {
        int frameW = 0, frameH = 0;
        GetClientSize(&frameW, &frameH);
        RECT clientrect = { 0, 0, clientW, clientH };
        RECT ownerrect  = { 0, 0, frameW, frameH };
        CalculateMiniWindowRectangles(m_doc, bUnder, clientrect, ownerrect);
        }

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

            if (pos <= 3 && !(mw->GetFlags() & MINIWINDOW_ABSOLUTE_LOCATION)) {
                // Stretch modes (0–3): StretchBlit to the landed rect. (The
                // port stretches these; Windows' Blit_Bitmap draws miniwindows
                // 1:1 even in modes 0–3 — an existing, deliberate divergence.)
                int destW = r.Width(), destH = r.Height();
                if (destW > 0 && destH > 0)
                    destDC.StretchBlit(destX, destY, destW, destH,
                                       srcDC, 0, 0, iW, iH, wxCOPY);
            } else if (bTransparent) {
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

            // == Blit_Bitmap's bUseRect mutation on Windows: after a paint,
            // m_rect holds {left, top, WIDTH, HEIGHT} — WindowInfo 12/13
            // expose exactly this to scripts, so keep the convention (the
            // hit-test above reads left/top + size and doesn't care).
            mw->m_rect.right  = iW;
            mw->m_rect.bottom = iH;
        }
    };

    // ── Miniwindow mouse-event callbacks ──────────────────────────────────
    // Engine-owned routing (miniwindow_compose.cpp, docs/PAL.md S4): the old
    // CMUSHView Mouse_* family, shared verbatim with the Windows view. The
    // pane forwards raw client-space coordinates; WxMouseHooks supplies the
    // view services (capture + keyboard modifiers; tooltips are a no-op here
    // because the pane polls m_fn_get_tooltip on every motion, and mouse-leave
    // needs no arming — wx delivers wxEVT_LEAVE_WINDOW natively).
    struct WxMouseHooks : public MiniWindowMouseHooks {
        COutputPane* pane;
        WxMouseHooks(COutputPane* p) : pane(p) {}
        virtual bool IsCaptured() { return pane && pane->HasCapture(); }
        virtual void SetMouseCapture() {
            if (pane && !pane->HasCapture()) pane->CaptureMouse();
        }
        virtual void ReleaseMouseCapture() {
            if (pane && pane->HasCapture()) pane->ReleaseMouse();
        }
        virtual long KeyboardModifiers() {
            long flags = 0;
            if (wxGetKeyState(WXK_SHIFT))       flags |= MW_MOUSE_SHIFT;
            if (wxGetKeyState(WXK_RAW_CONTROL)) flags |= MW_MOUSE_CTRL;
            if (wxGetKeyState(WXK_ALT))         flags |= MW_MOUSE_ALT;
            return flags;
        }
    };

    m_output->m_fn_miniwindow_mouse_down = [this](int x, int y, long flags) -> bool {
        if (!m_doc) return false;
        WxMouseHooks hooks(m_output);
        return MouseDownMiniWindow(m_doc, CPoint(x, y), flags, hooks);
    };

    m_output->m_fn_miniwindow_mouse_up = [this](int x, int y, long flags) -> bool {
        if (!m_doc) return false;
        WxMouseHooks hooks(m_output);
        return MouseUpMiniWindow(m_doc, CPoint(x, y), flags, hooks);
    };

    m_output->m_fn_miniwindow_mouse_move = [this](int x, int y) -> int {
        if (!m_doc) return -999;
        WxMouseHooks hooks(m_output);
        MouseMoveMiniWindow(m_doc, CPoint(x, y), hooks);
        // Cursor: a fresh hit-test AFTER the move processing — the callbacks
        // it dispatched may have deleted the hotspot or the window (this is
        // what Windows' OnSetCursor does with its own MouseOverMiniWindow
        // pass). Returns the hotspot cursor (-1..12), 0 over a bare
        // miniwindow, -999 when not over one.
        std::string cid, cwid;
        CHotspot* ch = nullptr;
        CMiniWindow* cmw = MouseOverMiniWindow(m_doc, CPoint(x, y), cid, ch, cwid);
        return cmw ? (ch ? (int)ch->m_Cursor : 0) : -999;
    };

    // Scroll wheel: pack the flags exactly as CMUSHView::OnMouseWheel does —
    // direction bit + |delta| in the high-order word (the pane passes the raw
    // wxEVT_MOUSEWHEEL rotation, same +/-120-per-notch units as Win32 zDelta).
    m_output->m_fn_miniwindow_mouse_wheel = [this](int x, int y, int delta) -> bool {
        if (!m_doc) return false;
        WxMouseHooks hooks(m_output);
        long flags = (delta < 0 ? MW_MOUSE_SCROLL_BACK : 0)
                   | ((long)(delta < 0 ? -delta : delta) << 16);
        return MouseWheelMiniWindow(m_doc, CPoint(x, y), flags, hooks);
    };

    // ── Tooltip callback ───────────────────────────────────────────────────
    // Returns the tooltip text for the client-space point (x, y).
    // Priority: miniwindow hotspot tooltip > MXP run hint > empty.
    m_output->m_fn_get_tooltip =
        [this](int x, int y) -> std::string
    {
        if (!m_doc) return {};

        // 1. Check miniwindow hotspot tooltip (engine-owned hit-test).
        std::string sHotspotId, sMiniwindowId;
        CHotspot* pHotspot = nullptr;
        MouseOverMiniWindow(m_doc, CPoint(x, y), sHotspotId, pHotspot, sMiniwindowId);
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

    // Fire OnPluginCommandChanged per-keystroke — mirrors CSendView::OnChange()
    // in sendvw.cpp (line 1175).  Re-entrancy guard prevents recursion if a
    // plugin callback itself modifies the command bar.
    m_input->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        if (!m_doc) return;
        static thread_local bool s_firing = false;
        if (!s_firing) {
            s_firing = true;
            m_doc->SendToAllPluginCallbacks(ON_PLUGIN_COMMAND_CHANGED);
            s_firing = false;
        }
    });

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

    // …but if the world configures its own input font (World Properties > Input
    // > Commands), that wins.  Previously the output font was applied here
    // unconditionally and nothing ever read m_input_font_*, so a configured
    // input font was visible only until the next world open, and a new world
    // seeded from App.m_strDefaultInputFont never showed it at all.  Same
    // point-size normalisation as the output font: negative = LOGFONT pixel
    // height at 96 DPI, positive = a point size.
    if (m_doc) {
        const char* face = static_cast<const char*>(m_doc->m_input_font_name);
        const int   lfH  = (int) m_doc->m_input_font_height;
        if ((face && *face) || lfH != 0) {
            int points;
            if (lfH < 0)       points = (-lfH * 72 + 48) / 96;
            else if (lfH > 0)  points = lfH;
            else               points = COutputPane::kDefaultFontSize;
            setInputFont(face ? face : "", points,
                         m_doc->m_input_font_weight >= 600,
                         m_doc->m_input_font_italic != 0);
        }
    }
    // Wire the same send/key handlers directly to m_inputPassword.
    m_inputPassword->Bind(wxEVT_TEXT_ENTER,
        [this](wxCommandEvent& e) { OnInputEnter(e); });
    m_inputPassword->Bind(wxEVT_KEY_DOWN,
        &CWorldChildFrame::OnInputKeyDown, this);

    // ── Info bar (world.Info / InfoClear / InfoColour / InfoBackground /
    // InfoFont / ShowInfoBar) ─────────────────────────────────────────────────
    // A short read-only styled strip at the very bottom, hidden until a script
    // calls ShowInfoBar(true).  On Windows this is a CRichEditCtrl docked on the
    // main frame; per-world here.  world.Info appends text in the current style;
    // InfoColour/InfoFont update that style; InfoBackground recolours the strip.
    m_infobar = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                               wxDefaultPosition, wxSize(-1, 24),
                               wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 |
                               wxTE_NO_VSCROLL | wxBORDER_SIMPLE);
    m_infobar->SetFont(m_output->GetCurrentFont());
    sizer->Add(m_infobar, 0, wxEXPAND);
    m_infobar->Hide();
    m_infobarStyle = wxTextAttr(*wxBLACK);
    m_infobarStyle.SetFont(m_infobar->GetFont());

    // ── Per-world status bar (bottom of the child frame) ──────────────────
    // On macOS the wxMDIParentFrame's own status bar is covered by the
    // maximised child, so each world frame carries its own.  Five fields
    // mirroring the Windows status line; UpdateStatusBar() fills them.
    m_statusBar = new wxStatusBar(this, wxID_ANY,
                                  wxSTB_DEFAULT_STYLE & ~wxSTB_SIZEGRIP);
    m_statusBar->SetFieldsCount(5);
    {
        const int widths[5] = { -1, 160, 90, 130, 70 };  // -1 = stretch (message)
        m_statusBar->SetStatusWidths(5, widths);
    }
    sizer->Add(m_statusBar, 0, wxEXPAND);

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
                while ((int)m_history.size() > HistoryLimit())
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
        if (code == 'F' && evt.RawControlDown()) {
            ShowFindBar(true);
            return;
        }
        // Let Ctrl+C, Ctrl+A, Ctrl+±/0 (zoom), and arrow keys be handled by
        // the output pane's own handler.
        bool paneOwns = evt.RawControlDown() ||
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
    // Apply the user's last font-size preference (written by Ctrl+scroll zoom),
    // but ONLY for worlds that do not carry a font of their own.
    //
    // This is a single *global* value.  Applying it unconditionally overrode the
    // per-world font that was just applied from the doc above, so a font chosen
    // in World Properties > Appearance > Output was silently replaced by whatever
    // size the user had last zoomed to in any other world — one of the reasons
    // the font setting appeared to do nothing.  An explicit per-world font now
    // wins; zooming such a world persists the new size into the world itself
    // (see the m_fn_grid_changed writeback above), so zoom still survives a
    // restart for those worlds via the world file rather than this global.
    // Fontless worlds (e.g. quick-connect) keep the old global behaviour.
    {
        const char* worldFace =
            m_doc ? static_cast<const char*>(m_doc->m_font_name) : NULL;
        const bool worldHasOwnFont =
            m_doc && ((worldFace && *worldFace) || (int)m_doc->m_font_height != 0);

        wxConfig cfg(wxT("mushclient"));
        long savedSize = 0;
        cfg.Read(wxT("/display/font_size"), &savedSize, 0L);
        if (!worldHasOwnFont &&
            savedSize >= COutputPane::kMinFontSize &&
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
    // Style the input bars now that they exist.  ApplyPaneBackground() above ran
    // before m_input/m_inputPassword were created, so without this the command
    // bar kept wx's default white-on-black-text look and the world's configured
    // input colours were not visible until something else happened to call
    // UpdateInputBarColors (e.g. leaving Ctrl+R search).
    UpdateInputBarColors();

    m_worldTimer.SetOwner(this, ID_WORLD_TIMER);
    m_worldTimer.Start(100);

    // Register this world document with the application's world doc-template
    // so that GetWorldList() / GetWorld() / GetWorldById() can enumerate all
    // open worlds.  Matched by RemoveDocument() in OnClose.
    if (m_doc && App.m_pWorldDocTemplate)
        App.m_pWorldDocTemplate->AddDocument(m_doc);
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
        int textCols = EffectiveTextCols(m_doc, paneW, charW);
        m_doc->m_nWrapColumn = (unsigned short)std::max(1, std::min(textCols, 65535));
        m_doc->SendNAWS(textCols, paneH / lineH);
    });
}

// ── UpdateInputBarColors ──────────────────────────────────────────────────
// Derives input bar fg/bg from the output pane's current background colour
// and applies them to both the normal and password input bars.
// ITU-R BT.601 luma: white text on dark backgrounds, dark text on light.
// Command-history depth: the world's own "Lines in command history" setting.
// Read at trim time (not cached) so a prefs change takes effect immediately.
int CWorldChildFrame::HistoryLimit() const
{
    if (m_doc && m_doc->m_nHistoryLines > 0)
        return (int) m_doc->m_nHistoryLines;
    return kMaxHistoryFallback;
}

void CWorldChildFrame::UpdateInputBarColors()
{
    unsigned long col = m_output ? m_output->PaneBackground() : 0UL;
    int r = static_cast<int>(GetRValue(col));
    int g = static_cast<int>(GetGValue(col));
    int b = static_cast<int>(GetBValue(col));
    int luma = (r * 299 + g * 587 + b * 114) / 1000;

    // Default: derive a readable pair from the output-pane background.
    wxColour inputFg = (luma < 128) ? wxColour(220, 220, 220) : wxColour(20, 20, 20);
    wxColour inputBg = wxColour(r, g, b);

    // But an explicitly configured input colour (World Properties > Input >
    // Commands) wins.  Those two settings previously did nothing at all on the
    // port because the pair was always derived from the pane background.  The
    // option defaults are black-on-white (scriptingoptions_numeric.inc:86,91);
    // treat those as "not set" so untouched worlds keep the readable
    // auto-derived pair rather than snapping to literal black-on-white.
    if (m_doc) {
        const COLORREF fg = m_doc->m_input_text_colour;
        const COLORREF bg = m_doc->m_input_background_colour;
        const bool fgSet = (fg != RGB(0, 0, 0));
        const bool bgSet = (bg != RGB(255, 255, 255));
        if (fgSet || bgSet) {
            if (fgSet)
                inputFg = wxColour(GetRValue(fg), GetGValue(fg), GetBValue(fg));
            if (bgSet)
                inputBg = wxColour(GetRValue(bg), GetGValue(bg), GetBValue(bg));
        }
    }
    for (wxTextCtrl* inp : {m_input, m_inputPassword}) {
        if (!inp) continue;
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

// ── Global Change (input find/replace) ────────────────────────────────────
// Mirrors CSendView::OnInputGlobalchange (stubbed on the port): prompt with the
// canonical CGlobalChangeDlg, then replace every occurrence in the input bar.
void CWorldChildFrame::GlobalChangeInput()
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp)
        return;

    // Remember the last from/to across invocations (CSendView kept these as
    // view members; a static is the port equivalent — shared, which is fine).
    static CString s_lastFrom, s_lastTo;

    CGlobalChangeDlg dlg;
    dlg.m_strChangeFrom = s_lastFrom;
    dlg.m_strChangeTo   = s_lastTo;
    if (dlg.DoModal() != IDOK)
        return;
    s_lastFrom = dlg.m_strChangeFrom;
    s_lastTo   = dlg.m_strChangeTo;

    CString strCurrent(inp->GetValue().ToUTF8().data());
    strCurrent.Replace("\r", "");   // drop carriage-returns

    // Canonical backslash handling: \\ -> \, \n -> newline, \t -> tab.
    CString from = dlg.m_strChangeFrom, to = dlg.m_strChangeTo;
    from.Replace("\\\\", "\x01"); from.Replace("\\n", "\n"); from.Replace("\\t", "\t"); from.Replace("\x01", "\\");
    to.Replace  ("\\\\", "\x01"); to.Replace  ("\\n", "\n"); to.Replace  ("\\t", "\t"); to.Replace  ("\x01", "\\");

    if (from.IsEmpty())
        return;   // nothing to find

    CString strNew = Replace(strCurrent, from, to);   // replace all (canonical helper)
    inp->SetValue(wxString::FromUTF8(static_cast<const char*>(strNew)));
    inp->SetInsertionPointEnd();
}

// ── Generate Character Name ────────────────────────────────────────────────
// Mirrors CSendView::OnEditGeneratecharactername (stubbed on the port).
void CWorldChildFrame::GenerateCharacterName()
{
    if (!m_doc)
        return;

    // Resolve the names file.  Its default is a bare "names.txt", which
    // GenerateName() would look for in the current working directory (not found
    // → silent failure).  If the path is empty or has no directory component,
    // resolve it among the bundled resources, where the build ships it.
    if (App.m_strDefaultNameGenerationFile.IsEmpty() ||
        App.m_strDefaultNameGenerationFile.Find('/') == -1)
    {
        CString dir = ExtractDirectory(App.m_strMUSHclientFileName);
#if defined (__APPLE__)
        // .app: resources live in Contents/Resources/, not Contents/MacOS/.
        dir += "../Resources/";
#endif
        App.m_strDefaultNameGenerationFile = dir + "names.txt";
    }

    CString strName = GenerateName();
    if (strName.IsEmpty())
        return;   // no usable names file

    // Canonical dialog (bridged): shows the name, Try Again / Copy / Send to World.
    CGeneratedNameDlg dlg;
    dlg.m_strName     = strName;
    dlg.m_pDoc        = m_doc;
    dlg.m_strFileName = App.m_strDefaultNameGenerationFile;
    dlg.DoModal();
}

// ── Spell-check the input ──────────────────────────────────────────────────
// Mirrors CSendView::OnEditSpellcheck (stubbed): runs the canonical spell-check
// Lua over the input text (it pops the suggestion dialog per misspelling) and
// writes the corrected text back.
void CWorldChildFrame::SpellCheckInput()
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp)
        return;

    long from = 0, to = 0;
    inp->GetSelection(&from, &to);
    bool bAll = (from == to);   // no selection → check the whole line
    wxString wtext = bAll ? inp->GetValue() : inp->GetStringSelection();

    CString corrected = App.SpellCheckString(CString(wtext.ToUTF8().data()), bAll);
    wxString wcorrected = wxString::FromUTF8(static_cast<const char*>(corrected));

    if (bAll)
        inp->SetValue(wcorrected);
    else
        inp->Replace(from, to, wcorrected);
    inp->SetInsertionPointEnd();
}

// ── Print the world output buffer ──────────────────────────────────────────
void CWorldChildFrame::PrintWorld()
{
    PrintOutputPane(this, GetTitle(), m_output);
}

// ── View-level output/input commands ──────────────────────────────────────────
// Ports of CMUSHView/CSendView handlers (those views are not instantiated on the
// port).  The output selection comes from the wx pane; ShellExecute is the
// macOS `open` shim, the same path the canonical handlers use.

void CWorldChildFrame::GotoSelectedURL()
{
    if (!m_output) return;
    CString sel((const char*) m_output->GetSelectionText().utf8_str());
    sel.TrimLeft();  sel.TrimRight();
    if (sel.IsEmpty())
        ::TMessageBox("No URL selected", MB_ICONEXCLAMATION);
    else if (sel.GetLength() > 512)
        ::TMessageBox("URL too long", MB_ICONEXCLAMATION);
    else
    {
        CString upper = sel;  upper.MakeUpper();
        if (upper.Left(15).Find("://") == -1)
            sel = "http://" + sel;
        if ((long) ShellExecute((HWND)nullptr, "open", (LPCTSTR) sel, NULL, NULL, SW_SHOWNORMAL) <= 32)
            ::UMessageBox(TFormat("Unable to open the URL \"%s\"", (const char*) sel),
                          MB_ICONEXCLAMATION);
    }
    if (wxTextCtrl* inp = ActiveInput()) inp->SetFocus();
}

void CWorldChildFrame::SendMailToSelection()
{
    if (!m_output) return;
    CString sel((const char*) m_output->GetSelectionText().utf8_str());
    sel.TrimLeft();  sel.TrimRight();
    if (sel.IsEmpty())
        ::TMessageBox("No email address selected", MB_ICONEXCLAMATION);
    else if (sel.GetLength() > 128)
        ::TMessageBox("Email address too long", MB_ICONEXCLAMATION);
    else if (sel.Find('@') == -1)
        ::UMessageBox(TFormat("Email address \"%s\" invalid - does not contain a \"@\"",
                              (const char*) sel), MB_ICONEXCLAMATION);
    else
    {
        CString upper = sel;  upper.MakeUpper();
        if (upper.Left(7) != "MAILTO:")
            sel = "mailto:" + sel;
        if ((long) ShellExecute((HWND)nullptr, "open", (LPCTSTR) sel, NULL, NULL, SW_SHOWNORMAL) <= 32)
            ::UMessageBox(TFormat("Unable to send mail to \"%s\"", (const char*) sel),
                          MB_ICONEXCLAMATION);
    }
    if (wxTextCtrl* inp = ActiveInput()) inp->SetFocus();
}

void CWorldChildFrame::CopySelectionHTML()
{
    if (m_output)
        m_output->CopySelectionAsHTML(m_output->GetCurrentFont().GetFaceName());
}

void CWorldChildFrame::RepeatLastCommand()
{
    if (!m_doc || m_history.empty()) return;
    CString cmd((const char*) m_history.back().utf8_str());
    bool bOmit = false;
    if (!cmd.IsEmpty())
        m_doc->EvaluateCommand(cmd, true /*countThem*/, bOmit);
}

void CWorldChildFrame::ShowCommandHistory()
{
    if (m_history.empty()) {
        wxMessageBox(wxT("No command history yet."), wxT("Command History"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    wxArrayString choices;   // most-recent first
    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it)
        choices.Add(*it);
    wxSingleChoiceDialog dlg(this, wxT("Pick a command to place in the input bar:"),
                             wxT("Command History"), choices);
    if (dlg.ShowModal() == wxID_OK) {
        if (wxTextCtrl* inp = ActiveInput()) {
            inp->SetValue(dlg.GetStringSelection());
            inp->SetInsertionPointEnd();
            inp->SetFocus();
        }
    }
}

void CWorldChildFrame::HighlightSelectedWord()
{
    if (!m_doc || !m_output) return;

    CHighlightPhraseDlg dlg;
    dlg.m_iOtherForeground = m_doc->m_normalcolour[WHITE];
    dlg.m_iOtherBackground = m_doc->m_normalcolour[BLACK];

    // Pre-fill from the selection (first line, <=80 chars) + its style colour.
    long sLine, sCol, eLine, eCol;
    if (m_output->GetSelectionInfo(sLine, sCol, eLine, eCol) &&
        (eLine > sLine || (eLine == sLine && eCol > sCol)))
    {
        CString strSelection((const char*) m_output->GetSelectionText().utf8_str());
        int nl = strSelection.Find("\n");
        if (nl != -1) strSelection = strSelection.Left(nl);
        if (strSelection.GetLength() > 80) strSelection = strSelection.Left(80);
        dlg.m_strText = strSelection;

        CLine* pLine = m_doc->m_LineList.GetAt(m_doc->GetLinePosition(sLine));
        if (pLine) {
            CStyle* pStyle; int iCol; POSITION fp;
            if (m_doc->FindStyle(pLine, sCol, iCol, pStyle, fp))
                m_doc->GetStyleRGB(pStyle, dlg.m_iOtherForeground, dlg.m_iOtherBackground);
        }
    }

    memcpy(dlg.m_customtext, m_doc->m_customtext, sizeof(dlg.m_customtext));
    memcpy(dlg.m_customback, m_doc->m_customback, sizeof(dlg.m_customback));
    for (int i = 0; i < MAX_CUSTOM; i++)
        dlg.m_strCustomName[i] = m_doc->m_strCustomColourName[i];
    dlg.m_iColour = OTHER_CUSTOM + 1;
    dlg.m_bWord   = true;

    if (dlg.DoModal() != IDOK) { if (wxTextCtrl* inp = ActiveInput()) inp->SetFocus(); return; }

    CString strText;
    if (dlg.m_bWord) strText += "\\b";
    strText += ConvertToRegularExpression(dlg.m_strText, false, false);
    if (dlg.m_bWord) strText += "\\b";

    t_regexp* regexp = NULL;
    try {
        regexp = regcomp(strText, (dlg.m_bMatchCase ? 0 : PCRE_CASELESS) |
                                  (m_doc->m_bUTF_8 ? PCRE_UTF8 : 0));
    } catch (CException* e) {
        e->Delete();
        ::TMessageBox("Cannot compile regular expression");
        return;
    }

    CString strTriggerName;
    strTriggerName.Format("*trigger%s", (LPCTSTR) App.GetUniqueString());
    CTrigger* trigger_item;
    m_doc->GetTriggerMap().SetAt(strTriggerName, trigger_item = new CTrigger);
    trigger_item->nUpdateNumber    = App.GetUniqueNumber();
    trigger_item->strInternalName  = strTriggerName;
    trigger_item->trigger          = strText;
    trigger_item->colour           = dlg.m_iColour - 1;
    trigger_item->bKeepEvaluating  = true;
    trigger_item->ignore_case      = !dlg.m_bMatchCase;
    trigger_item->bRegexp          = true;
    trigger_item->bRepeat          = true;
    trigger_item->regexp           = regexp;
    trigger_item->iSendTo          = eSendToWorld;
    trigger_item->iSequence        = 90;
    trigger_item->strGroup         = "Highlighted Words";
    trigger_item->iOtherForeground = dlg.m_iOtherForeground;
    trigger_item->iOtherBackground = dlg.m_iOtherBackground;
    m_doc->SortTriggers();
    m_doc->SetModifiedFlag(TRUE);
}

void CWorldChildFrame::MakeMultiLineTrigger()
{
    if (!m_doc || !m_output) return;

    CMultiLineTriggerDlg dlg;
    CString strSelection((const char*) m_output->GetSelectionText().utf8_str());
    strSelection = strSelection.Left(10000);
    strSelection.Replace("\r", "");

    dlg.m_strTriggerText = ConvertToRegularExpression(strSelection, false, false);
    dlg.m_strTriggerText.Replace("\\n", "\r\n");   // show linebreaks
    dlg.m_bMatchCase = true;

    if (dlg.DoModal() != IDOK) return;

    CString strText = dlg.m_strTriggerText;
    int iCount = 1;
    for (const char* p = strText; *p; p++)
        if (*p == '\n') iCount++;

    strText.Replace(ENDLINE, "\\n");
    strText += "\\Z";

    t_regexp* regexp = NULL;
    try {
        regexp = regcomp(strText, (dlg.m_bMatchCase ? 0 : PCRE_CASELESS) |
                                  (m_doc->m_bUTF_8 ? PCRE_UTF8 : 0));
    } catch (CException* e) {
        e->Delete();
        ::TMessageBox("Cannot compile regular expression");
        return;
    }

    CString strTriggerName;
    strTriggerName.Format("*trigger%s", (LPCTSTR) App.GetUniqueString());
    CTrigger* trigger_item;
    m_doc->GetTriggerMap().SetAt(strTriggerName, trigger_item = new CTrigger);
    trigger_item->nUpdateNumber   = App.GetUniqueNumber();
    trigger_item->strInternalName = strTriggerName;
    trigger_item->trigger         = strText;
    trigger_item->colour          = SAMECOLOUR;
    trigger_item->ignore_case     = !dlg.m_bMatchCase;
    trigger_item->contents        = "%0";
    trigger_item->bRegexp         = true;
    trigger_item->bMultiLine      = true;
    trigger_item->bKeepEvaluating = true;
    trigger_item->regexp          = regexp;
    trigger_item->iSendTo         = eSendToOutput;
    trigger_item->strGroup        = "Multi Line";
    trigger_item->iLinesToMatch   = iCount;
    m_doc->SortTriggers();
    m_doc->SetModifiedFlag(TRUE);
}

// Edit ▸ Go To / Select To Matching Brace — operates on the command input bar.
// (Canonical FindMatchingBrace works on a CEdit; the port input is a wxTextCtrl,
// so the brace scan is reimplemented here.)
void CWorldChildFrame::GotoMatchingBrace(bool select)
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp) return;
    wxString text = inp->GetValue();
    const long len = (long) text.length();
    if (len == 0) return;

    static const wxString kOpen = wxT("([{"), kClose = wxT(")]}");
    long pos = inp->GetInsertionPoint();

    // Find a brace at the cursor, else just before it.
    long bracePos = -1; int dir = 0; wxUniChar want;
    auto classify = [&](long p) -> bool {
        if (p < 0 || p >= len) return false;
        int oi = kOpen.Find(text[p]);
        if (oi != wxNOT_FOUND)  { bracePos = p; dir = +1; want = kClose[oi]; return true; }
        int ci = kClose.Find(text[p]);
        if (ci != wxNOT_FOUND)  { bracePos = p; dir = -1; want = kOpen[ci];  return true; }
        return false;
    };
    if (!classify(pos) && !classify(pos - 1)) return;

    const wxUniChar brace = text[bracePos];
    int depth = 0;
    long p = bracePos;
    for (; p >= 0 && p < len; p += dir) {
        if      (text[p] == brace) ++depth;
        else if (text[p] == want)  { if (--depth == 0) break; }
    }
    if (depth != 0 || p < 0 || p >= len) return;   // no match

    if (select)
        inp->SetSelection(wxMin(bracePos, p), wxMax(bracePos, p) + 1);
    else
        inp->SetInsertionPoint(p);
    inp->ShowPosition(p);
    inp->SetFocus();
}

// Game ▸ Functions List — the canonical scripting-function reference window
// (IDD_FUNCTION_LIST), seeded with the input-bar text + selection.
void CWorldChildFrame::ShowFunctionsList()
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp || !m_doc) return;
    CString text((const char*) inp->GetValue().utf8_str());
    long from = 0, to = 0;
    inp->GetSelection(&from, &to);
    bool bLua = m_doc->GetScriptEngine() && m_doc->GetScriptEngine()->L;
    ::ShowFunctionslist(text, (int) from, (int) to, bLua);
}

// Display ▸ Text Attributes — show the style (colour/flags) of the first
// selected character via the canonical CTextAttributesDlg (IDD_TEXT_ATTRIBUTES).
void CWorldChildFrame::ShowTextAttributes()
{
    if (!m_doc || !m_output) return;

    long sLine, sCol, eLine, eCol;
    if (!m_output->GetSelectionInfo(sLine, sCol, eLine, eCol) ||
        !(eLine > sLine || (eLine == sLine && eCol > sCol)))
        return;   // need a non-empty selection

    CLine* pLine = m_doc->m_LineList.GetAt(m_doc->GetLinePosition(sLine));
    if (!pLine) return;
    long startcol = MIN(sCol, pLine->len);

    CStyle* pStyle; int iCol; POSITION fp;
    if (!m_doc->FindStyle(pLine, startcol + 1, iCol, pStyle, fp)) {
        ::TMessageBox("Cannot find style of this character");
        return;
    }

    static const char* sColours[8] =
        { "Black","Red","Green","Yellow","Blue","Magenta","Cyan","White" };
    unsigned int iStyle = pStyle->iFlags;

    CTextAttributesDlg dlg;
    dlg.m_pDoc   = m_doc;
    dlg.m_iLine  = sLine + 1;       // 1-relative
    dlg.m_pLine  = pLine;
    dlg.m_strLetter = (startcol < pLine->len) ? CString(pLine->text[startcol]) : CString(" ");

    if ((iStyle & COLOURTYPE) == COLOUR_CUSTOM) {
        dlg.m_strTextColour   = "Custom";
        dlg.m_strBackColour   = "Custom";
        dlg.m_strCustomColour = m_doc->m_strCustomColourName[pStyle->iForeColour];
    } else if ((iStyle & COLOURTYPE) == COLOUR_RGB) {
        dlg.m_strTextColour = CFormat("R=%i, G=%i, B=%i",
            GetRValue(pStyle->iForeColour), GetGValue(pStyle->iForeColour), GetBValue(pStyle->iForeColour));
        dlg.m_strBackColour = CFormat("R=%i, G=%i, B=%i",
            GetRValue(pStyle->iBackColour), GetGValue(pStyle->iBackColour), GetBValue(pStyle->iBackColour));
        dlg.m_strCustomColour = "RGB";
    } else {   // COLOUR_ANSI
        dlg.m_strTextColour = (pStyle->iForeColour >= 8)
            ? CFormat("R=%i, G=%i, B=%i", GetRValue(xterm_256_colours[pStyle->iForeColour]),
                      GetGValue(xterm_256_colours[pStyle->iForeColour]), GetBValue(xterm_256_colours[pStyle->iForeColour]))
            : CString(sColours[pStyle->iForeColour & 7]);
        dlg.m_strBackColour = (pStyle->iBackColour >= 8)
            ? CFormat("R=%i, G=%i, B=%i", GetRValue(xterm_256_colours[pStyle->iBackColour]),
                      GetGValue(xterm_256_colours[pStyle->iBackColour]), GetBValue(xterm_256_colours[pStyle->iBackColour]))
            : CString(sColours[pStyle->iBackColour & 7]);
        dlg.m_strCustomColour = "n/a";
    }
    dlg.DoModal();
}

void CWorldChildFrame::BookmarkSelection()
{
    if (m_output) m_output->ToggleBookmark();
}

void CWorldChildFrame::GotoBookmark()
{
    if (m_output) m_output->GotoNextBookmark();
}

// ── Freeze Output: toggle the output pane's autoscroll pause ────────────────
void CWorldChildFrame::FreezeOutput()
{
    if (m_output)
        m_output->SetPaused(!m_output->IsPaused());
}

// ── Go To Line: prompt for a line number and scroll the output to it ────────
void CWorldChildFrame::GotoLine()
{
    if (!m_output)
        return;
    int total = m_output->LineCount();
    if (total <= 0)
        return;
    long line = wxGetNumberFromUser(
        wxString::Format(wxT("Go to which line? (1 - %d)"), total),
        wxT("Line:"), wxT("Go To Line"), total, 1, total, this);
    if (line >= 1)
        m_output->ScrollToLine((int) line - 1);
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
    if (!m_doc || !m_statusBar) return;

    auto pane = [this](int field, const std::string& s) {
        m_statusBar->SetStatusText(wxString::FromUTF8(s), field);
    };

    int phase = m_doc->m_iConnectPhase;
    const bool connected = (phase == eConnectConnectedToMud);

    // ── Pane 1: world name (its own field now, no longer a message prefix) ──
    pane(1, static_cast<const char*>(m_doc->m_mush_name));

    // ── Pane 0 (stretch): main status / connection state ───────────────────
    // Script status takes priority, then scrollback, reconnect countdown, the
    // connected server, otherwise the in-progress connection phase.
    std::string msg;
    if (!m_doc->m_strStatusMessage.IsEmpty()) {
        msg = static_cast<const char*>(m_doc->m_strStatusMessage);
    } else if (m_output && !m_output->IsAutoScrolling()) {
        msg = "\xe2\x86\x91 Scrolled back \xe2\x80\x94 press Escape or scroll to bottom to resume";
    } else if (m_reconnectCountdown > 0) {
        msg = "Reconnecting in " + std::to_string((m_reconnectCountdown + 9) / 10) + "s\xe2\x80\xa6";
    } else if (connected) {
        msg = "Connected to ";
        msg += static_cast<const char*>(m_doc->m_server);
        msg += ":" + std::to_string(m_doc->m_port);
    } else {
        switch (phase) {
        case eConnectMudNameLookup:
        case eConnectProxyNameLookup:        msg = "Resolving\xe2\x80\xa6";     break;
        case eConnectConnectingToMud:
        case eConnectConnectingToProxy:      msg = "Connecting\xe2\x80\xa6";    break;
        case eConnectAwaitingProxyResponse1:
        case eConnectAwaitingProxyResponse2:
        case eConnectAwaitingProxyResponse3: msg = "Connecting via proxy\xe2\x80\xa6"; break;
        case eConnectAwaitingSSLHandshake:   msg = "TLS handshake\xe2\x80\xa6"; break;
        case eConnectDisconnecting:          msg = "Disconnecting\xe2\x80\xa6"; break;
        default:                             msg = "Not connected.";             break;
        }
    }
    pane(0, msg);

    // ── Pane 2: connection duration (H:MM:SS) while connected ──────────────
    if (connected) {
        time_t start = m_doc->m_tConnectTime.GetTime();
        time_t now   = time(nullptr);
        long secs = (start > 0 && now >= start) ? (long)(now - start) : 0;
        char buf[32];
        snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", secs / 3600, (secs / 60) % 60, secs % 60);
        pane(2, buf);
    } else {
        pane(2, "");
    }

    // ── Pane 3: lines received ↓ / sent ↑ this connection ──────────────────
    {
        char buf[64];
        snprintf(buf, sizeof buf, "\xe2\x86\x93%ld \xe2\x86\x91%ld",
                 (long) m_doc->m_nTotalLinesReceived, (long) m_doc->m_nTotalLinesSent);
        pane(3, buf);
    }

    // ── Pane 4: log indicator ──────────────────────────────────────────────
    pane(4, IsLogging() ? "\xf0\x9f\x94\xb4 Log" : "");
}

void CWorldChildFrame::ShowStatusBarStrip(bool show)
{
    if (m_statusBar && m_statusBar->IsShown() != show) {
        m_statusBar->Show(show);
        Layout();   // reclaim/yield the strip's height
    }
}

bool CWorldChildFrame::IsStatusBarStripShown() const
{
    return m_statusBar && m_statusBar->IsShown();
}

// ── OnActivate ────────────────────────────────────────────────────────────
// Called when this MDI child is activated (gets focus) or deactivated.
// On activation, refresh the status bar so switching worlds gives immediate
// visual feedback.  Also fires on_world_get_focus / on_world_lose_focus Lua
// callbacks and OnPluginGetFocus / OnPluginLoseFocus plugin callbacks,
// mirroring CMUSHView::OnSetFocus / OnKillFocus in mushview.cpp.
void CWorldChildFrame::OnActivate(wxActivateEvent& evt)
{
    if (evt.GetActive()) {
        UpdateStatusBar();
        if (m_doc) {
            m_doc->FireWorldGetFocus();
            if (!m_doc->m_bWorldClosing)
                m_doc->SendToAllPluginCallbacks(ON_PLUGIN_GETFOCUS);
        }
    } else {
        if (m_doc) {
            m_doc->FireWorldLoseFocus();
            if (!m_doc->m_bWorldClosing)
                m_doc->SendToAllPluginCallbacks(ON_PLUGIN_LOSEFOCUS);
        }
    }
    evt.Skip();
}

// ── FireAcceleratorIfMatch / FireAcceleratorCommand ────────────────────────
// Non-Windows implementation of the Lua Accelerator()/AcceleratorTo() API.
//
// On Windows, MUSHclient registers a HACCEL table with the OS; the OS detects
// matching key presses and dispatches WM_COMMAND to CSendView::OnAcceleratorCommand.
// On non-Windows we replicate that dispatch manually: convert the wx key event to
// the Windows ACCEL encoding (FVIRTKEY | modifier flags, plus the VK code), look
// it up in m_AcceleratorToCommandMap, and call FireAcceleratorCommand if found.
//
// The wx→Windows VK mapping only covers keys that appear in the VirtualKeys[]
// table in accelerators.cpp (the set a user can actually name in a Lua script).

// Not static: also reused by the Key Name dialog's hotkey control (wx_mfc_impl.cpp).
WORD WxKeyToWinVK(int wxCode)
{
    // Upper-case alpha A-Z: wx delivers the upper-case code point, same as Windows VK
    if (wxCode >= 'A' && wxCode <= 'Z') return (WORD)wxCode;
    // Digit 0-9: same code point on both platforms
    if (wxCode >= '0' && wxCode <= '9') return (WORD)wxCode;

    switch (wxCode) {
        // Function keys
        case WXK_F1:  return VK_F1;
        case WXK_F2:  return VK_F1 + 1;
        case WXK_F3:  return VK_F1 + 2;
        case WXK_F4:  return VK_F1 + 3;
        case WXK_F5:  return VK_F1 + 4;
        case WXK_F6:  return VK_F1 + 5;
        case WXK_F7:  return VK_F1 + 6;
        case WXK_F8:  return VK_F1 + 7;
        case WXK_F9:  return VK_F1 + 8;
        case WXK_F10: return VK_F10;
        case WXK_F11: return VK_F11;
        case WXK_F12: return VK_F12;
#ifdef WXK_F13
        case WXK_F13: return VK_F13; case WXK_F14: return VK_F14;
        case WXK_F15: return VK_F15; case WXK_F16: return VK_F16;
        case WXK_F17: return VK_F17; case WXK_F18: return VK_F18;
        case WXK_F19: return VK_F19; case WXK_F20: return VK_F20;
        case WXK_F21: return VK_F21; case WXK_F22: return VK_F22;
        case WXK_F23: return VK_F23; case WXK_F24: return VK_F24;
#endif
        // Navigation
        case WXK_LEFT:     return VK_LEFT;
        case WXK_RIGHT:    return VK_RIGHT;
        case WXK_UP:       return VK_UP;
        case WXK_DOWN:     return VK_DOWN;
        case WXK_HOME:     return VK_HOME;
        case WXK_END:      return VK_END;
        case WXK_PAGEUP:   return VK_PRIOR;
        case WXK_PAGEDOWN: return VK_NEXT;
        case WXK_INSERT:   return VK_INSERT;
        case WXK_DELETE:   return VK_DELETE;
        // Common editing / whitespace
        case WXK_BACK:   return VK_BACK;
        case WXK_TAB:    return VK_TAB;
        case WXK_RETURN: return VK_RETURN;
        case WXK_ESCAPE: return VK_ESCAPE;
        case WXK_SPACE:  return VK_SPACE;
        // Numpad
        case WXK_NUMPAD0:    return VK_NUMPAD0;
        case WXK_NUMPAD1:    return VK_NUMPAD0 + 1;
        case WXK_NUMPAD2:    return VK_NUMPAD0 + 2;
        case WXK_NUMPAD3:    return VK_NUMPAD0 + 3;
        case WXK_NUMPAD4:    return VK_NUMPAD0 + 4;
        case WXK_NUMPAD5:    return VK_NUMPAD0 + 5;
        case WXK_NUMPAD6:    return VK_NUMPAD0 + 6;
        case WXK_NUMPAD7:    return VK_NUMPAD0 + 7;
        case WXK_NUMPAD8:    return VK_NUMPAD0 + 8;
        case WXK_NUMPAD9:    return VK_NUMPAD0 + 9;
        case WXK_MULTIPLY:   return VK_MULTIPLY;
        case WXK_ADD:        return VK_ADD;
        case WXK_SUBTRACT:   return VK_SUBTRACT;
        case WXK_DECIMAL:    return VK_DECIMAL;
        case WXK_DIVIDE:     return VK_DIVIDE;
        default: return 0;
    }
}

bool CWorldChildFrame::FireAcceleratorIfMatch(wxKeyEvent& evt)
{
    if (!m_doc || m_doc->m_AcceleratorToCommandMap.empty())
        return false;

    WORD vk = WxKeyToWinVK(evt.GetKeyCode());
    if (vk == 0) return false;

    BYTE fVirt = FVIRTKEY | FNOINVERT;
    if (evt.ShiftDown())        fVirt |= FSHIFT;
    if (evt.RawControlDown())   fVirt |= FCONTROL;
    if (evt.AltDown())          fVirt |= FALT;

    long lookup = ((long)fVirt << 16) | vk;
    auto it = m_doc->m_AcceleratorToCommandMap.find(lookup);
    if (it == m_doc->m_AcceleratorToCommandMap.end())
        return false;

    FireAcceleratorCommand(it->second);
    return true;
}

void CWorldChildFrame::FireAcceleratorCommand(WORD nID)
{
    if (!m_doc) return;

    string sCommand = m_doc->m_CommandToMacroMap[nID];
    if (sCommand.empty()) return;

    BOOL bSavedAutoSay = m_doc->m_bEnableAutoSay;
    m_doc->m_bEnableAutoSay = FALSE;
    m_doc->m_iCurrentActionSource = eUserAccelerator;

    if (m_doc->m_CommandToSendToMap[nID] == eSendToExecute) {
        // Same path as CSendView::SendCommand: evaluate the string through
        // aliases/scripting without adding it to command history.
        bool bOmitAccelLog = false;
        m_doc->EvaluateCommand(sCommand.c_str(), /*countThem=*/true, bOmitAccelLog);
    } else {
        // Resolve the key name for the trace message (linear search — rare path).
        CString key = "unknown key";
        for (auto& kv : m_doc->m_AcceleratorToCommandMap) {
            if (kv.second == nID) {
                key = KeyCodeToString((BYTE)(kv.first >> 16), (WORD)kv.first);
                break;
            }
        }

        CString strExtraOutput;
        CPlugin* pSavedPlugin = m_doc->m_CurrentPlugin;
        m_doc->m_CurrentPlugin = m_doc->GetPlugin(m_doc->m_CommandToPluginMap[nID].c_str());

        if (m_doc->m_CurrentPlugin || m_doc->m_CommandToPluginMap[nID].empty()) {
            m_doc->SendTo(m_doc->m_CommandToSendToMap[nID],
                          sCommand.c_str(),
                          TRUE,   // omit from output
                          TRUE,   // omit from log
                          TFormat("Accelerator: %s", (LPCTSTR)key),
                          "",     // variable name (unused for most destinations)
                          strExtraOutput);
        }

        m_doc->m_CurrentPlugin = pSavedPlugin;

        if (!strExtraOutput.IsEmpty())
            m_doc->DisplayMsg(strExtraOutput, strExtraOutput.GetLength(), COMMENT);
    }

    m_doc->m_iCurrentActionSource = eUnknownActionSource;
    m_doc->m_bEnableAutoSay = bSavedAutoSay;
}

// ── OnInputKeyDown ─────────────────────────────────────────────────────────
// Up/Down arrows navigate command history; all other keys pass through normally.
void CWorldChildFrame::OnInputKeyDown(wxKeyEvent& evt)
{
    int code = evt.GetKeyCode();

    // Lua Accelerator() API — check before built-in key bindings so scripts
    // can override any key (including Escape, PgUp/Dn, etc.).
    if (FireAcceleratorIfMatch(evt))
        return;

    // Escape: snap the output pane back to the bottom (re-engage auto-scroll).
    // If escape_deletes_input is set, also clear the command bar.
    if (code == WXK_ESCAPE) {
        if (m_output) m_output->ScrollToBottom();
        if (m_doc && m_doc->m_bEscapeDeletesInput) {
            wxTextCtrl* inp = ActiveInput();
            if (inp) {
                // "Save deleted command in history" (m_bSaveDeletedCommand):
                // keep what Escape throws away so it can still be recalled.
                // Never for a masked password prompt.
                wxString discarded = inp->GetValue();
                if (m_doc->m_bSaveDeletedCommand && !discarded.empty() &&
                    !m_doc->m_bNoEcho) {
                    if (m_history.empty() || m_history.back() != discarded)
                        m_history.push_back(discarded);
                    if ((int) m_history.size() > HistoryLimit())
                        m_history.erase(m_history.begin());
                }
                inp->Clear();
            }
        }
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
    if (code == WXK_HOME && evt.RawControlDown()) {
        if (m_output) m_output->ScrollToTop();
        return;
    }
    if (code == WXK_END && evt.RawControlDown()) {
        if (m_output) m_output->ScrollToBottom();
        return;
    }

    wxTextCtrl* inp = ActiveInput();

    // Ctrl+U: kill the entire input line (standard Unix terminal shortcut).
    // Saves to the kill ring so Ctrl+Y can restore it.
    if (code == 'U' && evt.RawControlDown()) {
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
    bool deleteWordBack = (code == 'W' && evt.RawControlDown()) ||
                         (code == WXK_BACK && evt.RawControlDown());
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
    if (code == 'D' && evt.RawControlDown()) {
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
    if (code == WXK_DELETE && evt.RawControlDown()) {
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
    if (code == 'K' && evt.RawControlDown()) {
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
    if (code == 'Y' && evt.RawControlDown()) {
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
    if (code == 'A' && evt.RawControlDown()) {
        if (inp) inp->SetInsertionPoint(0);
        return;
    }

    // Ctrl+E: move cursor to the end of the input line (Unix terminal).
    if (code == 'E' && evt.RawControlDown()) {
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
        (evt.RawControlDown() || evt.AltDown()))
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
    // Both are user-settable (World Properties > Input > Commands); they were
    // previously hardwired on, so a user who wanted Ctrl+P/Ctrl+N free for
    // something else could not turn them off.
    if (evt.RawControlDown() && code == 'P' &&
        m_doc && m_doc->m_bCtrlPGoesToPreviousCommand)
        code = WXK_UP;
    else if (evt.RawControlDown() && code == 'N' &&
             m_doc && m_doc->m_bCtrlNGoesToNextCommand)
        code = WXK_DOWN;

    // Ctrl+Z: jump the output pane to the end of the buffer (opt-in setting).
    // Without this it fell through to the text control's native undo.
    if (evt.RawControlDown() && code == 'Z' &&
        m_doc && m_doc->m_bCtrlZGoesToEndOfBuffer && m_output) {
        m_output->ScrollToBottom();
        return;
    }

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
                    CString sMatch(hist.ToUTF8().data());
                    m_doc->SendToAllPluginCallbacksRtn(ON_PLUGIN_TABCOMPLETE, sMatch);
                    if (sMatch == "\t") return;  // plugin cancelled
                    m_input->SetValue(wxString::FromUTF8((const char*)sMatch));
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
            // How far back to scan is the world's "Tab completion lines"
            // setting (m_iTabCompletionLines, default 200); the port used to
            // hardcode 300 and ignore it.  Read live — no seam needed.
            const int scanLines =
                (m_doc && m_doc->m_iTabCompletionLines > 0)
                    ? (int) m_doc->m_iTabCompletionLines : 200;
            int startLine = std::max(0, m_output->LineCount() - scanLines);
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
        {
            CString sMatch(m_tabWords[0].ToUTF8().data());
            m_doc->SendToAllPluginCallbacksRtn(ON_PLUGIN_TABCOMPLETE, sMatch);
            if (sMatch == "\t") return;  // plugin cancelled
            m_tabWords[0] = wxString::FromUTF8((const char*)sMatch);
        }
        m_input->SetValue(m_tabWords[0]);
        m_input->SetInsertionPointEnd();
        return;
    }

    // "Arrow keys traverse command history" (m_bArrowsChangeHistory).  When the
    // user turns this off they want Up/Down to move the caret instead, so fall
    // through to the text control rather than browsing history.
    if (code == WXK_UP && inp == m_input && m_doc && !m_doc->m_bArrowsChangeHistory) {
        evt.Skip();
        return;
    }
    if (code == WXK_DOWN && inp == m_input && m_doc && !m_doc->m_bArrowsChangeHistory) {
        evt.Skip();
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
    if (code == 'R' && evt.RawControlDown() && inp == m_input) {
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
    if (code == 'F' && evt.RawControlDown()) {
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
    if (code == 'L' && evt.RawControlDown()) {
        if (m_output) m_output->ClearOutput();
        return;
    }

    // ── F-key and numpad macros ──────────────────────────────────────────────
    // Map function keys (F1-F12, Shift+F1-F12, Ctrl+F1-F12) to m_macros[] and
    // numpad keys to m_keypad[] when m_keypad_enable is set.  User-configured
    // macros take priority over built-in bindings (e.g. F3 for Find).
    // All macros are processed on the input bar, not the output pane, because
    // we want them available while typing.
    if (!evt.AltDown() && m_doc) {
        // Helper: dispatch a macro by index.
        // REPLACE_COMMAND → set input bar; SEND_NOW → send via EvaluateCommand;
        // ADD_TO_COMMAND → insert at cursor.
        auto fireMacro = [this, inp](int idx) -> bool {
            if (idx < 0 || idx >= MACRO_COUNT) return false;
            const CString& mac = m_doc->m_macros[idx];
            if (mac.IsEmpty()) return false;
            switch (m_doc->m_macro_type[idx]) {
            case REPLACE_COMMAND:
                if (inp) {
                    inp->SetValue(wxString::FromUTF8(static_cast<const char*>(mac)));
                    inp->SetInsertionPointEnd();
                }
                break;
            case SEND_NOW: {
                bool bOmit = false;
                m_doc->EvaluateCommand(mac, /*count=*/true, bOmit);
                m_doc->UpdateAllViews(nullptr);
                break;
            }
            case ADD_TO_COMMAND:
                if (inp) inp->WriteText(wxString::FromUTF8(static_cast<const char*>(mac)));
                break;
            default:
                break;
            }
            return true;
        };

        // Helper: send a keypad key by index.
        auto fireKeypad = [this](int idx) -> bool {
            if (!m_doc->m_keypad_enable) return false;
            if (idx < 0 || idx >= eKeypad_Max_Items) return false;
            const CString& kp = m_doc->m_keypad[idx];
            if (kp.IsEmpty()) return false;
            bool bOmit = false;
            m_doc->EvaluateCommand(kp, /*count=*/true, bOmit);
            m_doc->UpdateAllViews(nullptr);
            return true;
        };

        bool ctrl  = evt.RawControlDown();
        bool shift = evt.ShiftDown();
        int macIdx  = -1;
        int kpIdx   = -1;

        if (!ctrl && !shift) {
            switch (code) {
            case WXK_F1:  macIdx = MAC_F1;  break;
            case WXK_F2:  macIdx = MAC_F2;  break;
            case WXK_F3:  macIdx = MAC_F3;  break;
            case WXK_F4:  macIdx = MAC_F4;  break;
            case WXK_F5:  macIdx = MAC_F5;  break;
            case WXK_F6:  macIdx = MAC_F6;  break;
            case WXK_F7:  macIdx = MAC_F7;  break;
            case WXK_F8:  macIdx = MAC_F8;  break;
            case WXK_F9:  macIdx = MAC_F9;  break;
            case WXK_F10: macIdx = MAC_F10; break;
            case WXK_F11: macIdx = MAC_F11; break;
            case WXK_F12: macIdx = MAC_F12; break;
            // Numpad digits
            case WXK_NUMPAD0:        kpIdx = eKeypad_0;    break;
            case WXK_NUMPAD1:        kpIdx = eKeypad_1;    break;
            case WXK_NUMPAD2:        kpIdx = eKeypad_2;    break;
            case WXK_NUMPAD3:        kpIdx = eKeypad_3;    break;
            case WXK_NUMPAD4:        kpIdx = eKeypad_4;    break;
            case WXK_NUMPAD5:        kpIdx = eKeypad_5;    break;
            case WXK_NUMPAD6:        kpIdx = eKeypad_6;    break;
            case WXK_NUMPAD7:        kpIdx = eKeypad_7;    break;
            case WXK_NUMPAD8:        kpIdx = eKeypad_8;    break;
            case WXK_NUMPAD9:        kpIdx = eKeypad_9;    break;
            case WXK_NUMPAD_DECIMAL: kpIdx = eKeypad_Dot;  break;
            case WXK_NUMPAD_DIVIDE:  kpIdx = eKeypad_Slash; break;
            case WXK_NUMPAD_MULTIPLY:kpIdx = eKeypad_Star; break;
            case WXK_NUMPAD_SUBTRACT:kpIdx = eKeypad_Dash; break;
            case WXK_NUMPAD_ADD:     kpIdx = eKeypad_Plus; break;
            default: break;
            }
        } else if (shift && !ctrl) {
            switch (code) {
            case WXK_F1:  macIdx = MAC_SHIFT_F1;  break;
            case WXK_F2:  macIdx = MAC_SHIFT_F2;  break;
            case WXK_F3:  macIdx = MAC_SHIFT_F3;  break;
            case WXK_F4:  macIdx = MAC_SHIFT_F4;  break;
            case WXK_F5:  macIdx = MAC_SHIFT_F5;  break;
            case WXK_F6:  macIdx = MAC_SHIFT_F6;  break;
            case WXK_F7:  macIdx = MAC_SHIFT_F7;  break;
            case WXK_F8:  macIdx = MAC_SHIFT_F8;  break;
            case WXK_F9:  macIdx = MAC_SHIFT_F9;  break;
            case WXK_F10: macIdx = MAC_SHIFT_F10; break;
            case WXK_F11: macIdx = MAC_SHIFT_F11; break;
            case WXK_F12: macIdx = MAC_SHIFT_F12; break;
            default: break;
            }
        } else if (ctrl && !shift) {
            switch (code) {
            case WXK_F1:  macIdx = MAC_CTRL_F1;  break;
            case WXK_F2:  macIdx = MAC_CTRL_F2;  break;
            case WXK_F3:  macIdx = MAC_CTRL_F3;  break;
            case WXK_F5:  macIdx = MAC_CTRL_F5;  break;
            case WXK_F6:  macIdx = MAC_CTRL_F6;  break;
            case WXK_F7:  macIdx = MAC_CTRL_F7;  break;
            case WXK_F8:  macIdx = MAC_CTRL_F8;  break;
            case WXK_F9:  macIdx = MAC_CTRL_F9;  break;
            case WXK_F10: macIdx = MAC_CTRL_F10; break;
            case WXK_F11: macIdx = MAC_CTRL_F11; break;
            case WXK_F12: macIdx = MAC_CTRL_F12; break;
            // Ctrl+numpad
            case WXK_NUMPAD0:        kpIdx = eCtrl_Keypad_0;    break;
            case WXK_NUMPAD1:        kpIdx = eCtrl_Keypad_1;    break;
            case WXK_NUMPAD2:        kpIdx = eCtrl_Keypad_2;    break;
            case WXK_NUMPAD3:        kpIdx = eCtrl_Keypad_3;    break;
            case WXK_NUMPAD4:        kpIdx = eCtrl_Keypad_4;    break;
            case WXK_NUMPAD5:        kpIdx = eCtrl_Keypad_5;    break;
            case WXK_NUMPAD6:        kpIdx = eCtrl_Keypad_6;    break;
            case WXK_NUMPAD7:        kpIdx = eCtrl_Keypad_7;    break;
            case WXK_NUMPAD8:        kpIdx = eCtrl_Keypad_8;    break;
            case WXK_NUMPAD9:        kpIdx = eCtrl_Keypad_9;    break;
            case WXK_NUMPAD_DECIMAL: kpIdx = eCtrl_Keypad_Dot;  break;
            case WXK_NUMPAD_DIVIDE:  kpIdx = eCtrl_Keypad_Slash; break;
            case WXK_NUMPAD_MULTIPLY:kpIdx = eCtrl_Keypad_Star; break;
            case WXK_NUMPAD_SUBTRACT:kpIdx = eCtrl_Keypad_Dash; break;
            case WXK_NUMPAD_ADD:     kpIdx = eCtrl_Keypad_Plus; break;
            default: break;
            }
        }

        if (macIdx >= 0 && fireMacro(macIdx)) return;
        if (kpIdx  >= 0 && fireKeypad(kpIdx))  return;
    }

    // Any non-navigation key cancels history browsing, Ctrl+R mode, and tab cycling.
    if (code != WXK_UP && code != WXK_DOWN) {
        m_histIdx    = -1;
        ApplyHistRSearch(false);
        m_tabWordIdx = -1;
        m_tabWords.clear();
    }

    // Return / Enter in an empty input field.
    // On macOS, wxEVT_TEXT_ENTER is not fired for empty wxTextCtrl, so pressing
    // Return on a blank line never calls OnInputEnter — sending the empty line
    // (which some MUDs use to keep the connection alive) becomes impossible.
    // Detect it here and call OnInputEnter directly.
    if ((code == WXK_RETURN || code == WXK_NUMPAD_ENTER) &&
        !evt.RawControlDown() && !evt.AltDown() && !evt.MetaDown()) {
        if (inp && inp->GetValue().IsEmpty()) {
            wxCommandEvent dummy(wxEVT_TEXT_ENTER);
            OnInputEnter(dummy);
            return;  // consume — don't insert a newline
        }
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
            if ((int)m_history.size() > HistoryLimit())
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

    // "Spell check on send" (m_bSpellCheckOnSend): check the line before it goes
    // anywhere, then re-read the possibly-corrected text.  Mirrors sendvw.cpp:329.
    // Collapse any selection first so SpellCheckInput checks the whole line, and
    // never run it on a masked password prompt.
    if (m_doc->m_bSpellCheckOnSend && !m_doc->m_bNoEcho) {
        inp->SetSelection(0, 0);
        SpellCheckInput();
    }

    wxString text = inp->GetValue();
    inp->Clear();
    m_histIdx = -1; ApplyHistRSearch(false);   // exit history browsing on send

    // Don't record passwords in history — m_bNoEcho signals a masked prompt.
    bool isPassword = m_doc->m_bNoEcho;
    if (!isPassword && !text.empty()) {
        // Add to history (no duplicates at the tail; trim if over limit).
        if (m_history.empty() || m_history.back() != text)
            m_history.push_back(text);
        if ((int)m_history.size() > HistoryLimit())
            m_history.erase(m_history.begin());
    }

    // Unpause output on send (unpause_on_send world option, default on).
    if (m_doc->m_bUnpauseOnSend && m_output && m_output->IsPaused())
        m_output->SetPaused(false);

    // EvaluateCommand processes aliases, speed-walks, and scripting callbacks
    // before sending the text to the MUD — the same path Windows uses for
    // keyboard input from CMUSHclientView::OnChar / ProcessCommand.
    // EvaluateCommand drops empty strings immediately (evaluate.cpp), so bypass
    // it for blank Enter presses and send the bare newline directly.
    CString cmd(text.ToUTF8().data());
    bool bOmitFromLog = isPassword;   // don't log passwords either

    // Fire OnPluginCommandEntered — mirrors sendvw.cpp line 352.
    // Plugins may modify cmd, or return "\t" (discard) / "\r" (ignore silently).
    if (!cmd.IsEmpty()) {
        m_doc->SendToAllPluginCallbacksRtn(ON_PLUGIN_COMMAND_ENTERED, cmd);
        if (cmd == "\t") {
            // Plugin discarded the command; no-op (input already cleared).
            m_doc->UpdateAllViews(nullptr);
            return;
        }
        if (cmd == "\r") {
            // Plugin said ignore silently.
            m_doc->UpdateAllViews(nullptr);
            return;
        }
    }

    // "Auto repeat last command" (m_bAutoRepeat): a blank Enter re-sends the
    // previous command instead of sending a bare newline.  Mirrors sendvw.cpp:725.
    if (cmd.IsEmpty() && m_doc->m_bAutoRepeat && !isPassword && !m_history.empty())
        cmd = CString(m_history.back().ToUTF8().data());

    // Auto-say (World Properties > Input > Auto Say).  Handles the override
    // prefix, the three exclusions and the prepend+send itself — including its
    // own backslash translation on the re-evaluate arm — so it runs BEFORE the
    // plain-send translation below, mirroring sendvw.cpp's ordering.  Returns
    // true when it consumed the command; it may also strip the override prefix
    // and return false, in which case we send the stripped text normally.
    if (!cmd.IsEmpty() && m_doc->ApplyAutoSay(cmd)) {
        m_doc->UpdateAllViews(nullptr);
        return;
    }

    // "Translate backslash sequences" (m_bTranslateBackslashSequences): turn \n,
    // \t, \\ etc. in the typed line into the real characters, exactly as
    // sendvw.cpp:678/707 does at its Execute() call.
    if (!cmd.IsEmpty() && m_doc->m_bTranslateBackslashSequences)
        cmd = ::FixupEscapeSequences(cmd);

    if (cmd.IsEmpty()) {
        // m_display_my_input: whether to echo the user's command back to the output window.
        m_doc->DoSendMsg(cmd, m_doc->m_display_my_input, !bOmitFromLog);
    } else {
        m_doc->EvaluateCommand(cmd, true /*countThem*/, bOmitFromLog);
    }

    // Flush any output produced by alias scripts / triggers to the pane.
    m_doc->UpdateAllViews(nullptr);
}

// ── NotifyPluginLayoutSettle ───────────────────────────────────────────────
// Refresh the doc's GUI geometry (GetInfo(280/281) = rows*lineH / cols*charW)
// from the current pane size, then fire ON_PLUGIN_WORLD_OUTPUT_RESIZED so
// plugins re-lay-out their docked miniwindows.  Used to settle the startup
// layout for plugins (e.g. Quow's Cow Bar) that defer their layout until after
// the world connects; see OnWorldTimer.
void CWorldChildFrame::NotifyPluginLayoutSettle()
{
    if (!m_doc) return;
    if (m_output) {
        int charW = m_output->CharWidth();
        int lineH = m_output->LineHeight();
        int paneW = 0, paneH = 0;
        m_output->GetClientSize(&paneW, &paneH);
        if (charW > 0 && lineH > 0 && paneW > 0 && paneH > 0) {
            m_doc->m_gui_cols        = paneW / charW;
            m_doc->m_gui_rows        = paneH / lineH;
            m_doc->m_gui_font_width  = charW;
            m_doc->m_gui_font_height = lineH;
        }
    }
    m_doc->SendToAllPluginCallbacks(ON_PLUGIN_WORLD_OUTPUT_RESIZED);
}

// ── OnWorldTimer ──────────────────────────────────────────────────────────
// Called every 100 ms.  Drives MUSHclient world/plugin timers, flushes
// output, updates UI on phase changes, and runs the auto-reconnect countdown.
void CWorldChildFrame::OnWorldTimer(wxTimerEvent& /*evt*/)
{
    // Pick up a change to the world's input colours (World Properties > Input >
    // Commands).  Those live on the doc with no notification seam, so poll them
    // — but only repaint when they actually differ from what we last applied,
    // so this costs two comparisons per tick in the normal case.
    if (m_doc) {
        const unsigned long fg = (unsigned long) m_doc->m_input_text_colour;
        const unsigned long bg = (unsigned long) m_doc->m_input_background_colour;
        if (fg != m_lastInputFg || bg != m_lastInputBg) {
            m_lastInputFg = fg;
            m_lastInputBg = bg;
            if (!m_histRSearch)   // don't fight the Ctrl+R tint
                UpdateInputBarColors();
        }
    }

    // Same treatment for the output-pane options that the constructor applies
    // once and nothing ever re-applied, so changing them in World Properties >
    // Appearance > Output only took effect after closing and reopening the
    // world: scrollback limit, freeze-on-scrollback, auto-copy selection, and
    // whether that auto-copy is HTML.  Cheap scalar compares; only touch the
    // pane when something actually changed.
    if (m_doc && m_output) {
        const long maxl   = (long) m_doc->m_maxlines;
        const bool freeze = m_doc->m_bAutoFreeze != 0;
        const bool copySel= m_doc->m_bCopySelectionToClipboard != 0;
        const bool asHtml = m_doc->m_bAutoCopyInHTML != 0;
        if (maxl != m_lastMaxLines || freeze != m_lastAutoFreeze ||
            copySel != m_lastCopySel || asHtml != m_lastCopyHtml) {
            m_lastMaxLines  = maxl;
            m_lastAutoFreeze= freeze;
            m_lastCopySel   = copySel;
            m_lastCopyHtml  = asHtml;
            if (maxl > 0) m_output->SetMaxLines((int) maxl);
            m_output->SetAllowAutoFreeze(freeze);
            m_output->SetCopySelectionToClipboard(copySel);
            m_output->SetAutoCopyInHTML(asHtml);
        }
    }

    if (!m_doc) return;

    // ── Speed-walk / command queue draining ──────────────────────────────────
    // On Windows, CTimerWnd fires a WM_TIMER at m_iSpeedWalkDelay ms intervals
    // and drains one queued command each time.  On non-Windows, m_pTimerWnd is
    // null, so we replicate that rate here via an accumulator.
    if (!m_doc->m_QueuedCommandsList.IsEmpty()) {
        int delay = m_doc->m_iSpeedWalkDelay;   // 0 = flush all immediately
        if (delay <= 0) {
            // No delay — drain the entire queue now.
            while (!m_doc->m_QueuedCommandsList.IsEmpty()) {
                CString strCommand = m_doc->m_QueuedCommandsList.RemoveHead();
                char cMessageType = strCommand[0];
                m_doc->DoSendMsg(strCommand.Mid(1),
                                 toupper(cMessageType) == QUEUE_WITH_ECHO ||
                                 toupper(cMessageType) == IMMEDIATE_WITH_ECHO,
                                 cMessageType >= 'A');
            }
            m_speedWalkAccum = 0;
        } else {
            // Drain one QUEUE_* command each time the accumulator reaches the delay.
            m_speedWalkAccum += 100;    // 100 ms per timer tick
            while (m_speedWalkAccum >= delay && !m_doc->m_QueuedCommandsList.IsEmpty()) {
                m_speedWalkAccum -= delay;
                // Drain through to (and including) the next QUEUE_* command;
                // interleaved IMMEDIATE_* commands fall through in the same slot,
                // matching CTimerWnd::OnTimer (which only breaks on QUEUE_*).
                for (;;) {
                    CString strCommand = m_doc->m_QueuedCommandsList.RemoveHead();
                    char cMessageType = strCommand[0];
                    m_doc->DoSendMsg(strCommand.Mid(1),
                                     toupper(cMessageType) == QUEUE_WITH_ECHO ||
                                     toupper(cMessageType) == IMMEDIATE_WITH_ECHO,
                                     cMessageType >= 'A');
                    if (toupper(cMessageType) == QUEUE_WITH_ECHO ||
                        toupper(cMessageType) == QUEUE_WITHOUT_ECHO)
                        break;   // QUEUE_* command consumes this delay slot
                    if (m_doc->m_QueuedCommandsList.IsEmpty())
                        break;
                }
            }
        }
        m_doc->ShowQueuedCommands();
    } else {
        m_speedWalkAccum = 0;   // reset when queue is empty
    }

    // ── World timers (triggers based on elapsed time, speed-walk queues, etc.)
    m_doc->CheckTimers();

    // ── Script file monitoring: poll mtime every 2 s ─────────────────────
    // Equivalent to the Windows FindFirstChangeNotification thread, but runs
    // on the main thread so UMessageBox / scripting calls are always safe.
    m_scriptCheckAccum += 100;
    if (m_scriptCheckAccum >= 2000) {
        m_scriptCheckAccum = 0;
        m_doc->OnScriptFileChanged(false);
    }

    // ── ON_PLUGIN_TICK: Windows fires this every ~40 ms.  We drive it via an
    //    accumulator so that the average rate matches even though our timer
    //    only fires every 100 ms.  Each 100 ms tick we fire it 2–3 times,
    //    keeping the long-run rate at 1 call per 40 ms.
    m_pluginTickAccum += 100;
    while (m_pluginTickAccum >= 40) {
        m_pluginTickAccum -= 40;
        m_doc->SendToAllPluginCallbacks(ON_PLUGIN_TICK);
    }

    // ── Plugin-layout settle re-notify ───────────────────────────────────────
    // Some plugins (e.g. Quow's Cow Bar) only lay out their docked miniwindows
    // on an ON_PLUGIN_WORLD_OUTPUT_RESIZED that arrives *after* they finish
    // initialising — which for them is after the world connects, or a ~4 s
    // fallback timer (they guard layout on a "bPreInstalled" flag set then).
    // Every resize notification we send fires during the initial open (0–1 s),
    // i.e. BEFORE that, so without this the windows stay stacked at the top-left
    // until the first manual resize.  Re-send the notification at ~2 s and ~5 s
    // after open (after both the connect and the fallback) so the layout settles
    // by itself — exactly what a manual resize would trigger.
    // The connect-phase handler below fires this immediately on connect (the
    // earliest the plugins will honour it); these ticks are the fallback for
    // worlds that never connect (Quow flips its flag on a ~4 s timer even then).
    if (m_settleResizeState < 2) {
        m_settleResizeAccum += 100;
        if (m_settleResizeAccum >= (m_settleResizeState == 0 ? 2000 : 5000)) {
            m_settleResizeState++;
            NotifyPluginLayoutSettle();
        }
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

        // Reaching "connected" is when Quow-style plugins flip the flag that
        // gates their real miniwindow layout (they defer it during load as a
        // crash guard).  Fire the layout re-notify now so docked windows snap
        // into place at connect-time — the earliest possible — instead of
        // waiting for the 2 s/5 s fallback ticks above.
        if (phase == eConnectConnectedToMud && m_settleResizeState < 2) {
            NotifyPluginLayoutSettle();
            m_settleResizeState = 2;   // fallback ticks no longer needed
        }

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

// ── pal::IWorldView ─────────────────────────────────────────────────────────
// This frame is the wx adapter for one world's output view.  Each method
// forwards to the output pane / input bar.  Only appendLine() is engine-driven
// today (CMUSHclientDoc::EmitOutputLine); the others are the adapter's ready
// surface, dormant until their engine paths migrate off the m_fn_* callbacks.

void CWorldChildFrame::appendLine(SOutputLine&& line)
{
    if (m_output) m_output->AppendLine(std::move(line));
}

void CWorldChildFrame::note(const char* text)
{
    if (m_output) m_output->AppendNote(std::string(text));
}

void CWorldChildFrame::setTitle(const char* title)
{
    // world.SetTitle(s): update the MDI child-frame title bar. An empty title
    // falls back to the world name (MUD scripts commonly set HP/MP here).
    wxString wxTitle = wxString::FromUTF8(title);
    SetTitle(wxTitle.IsEmpty()
             ? wxString::FromUTF8(static_cast<const char*>(m_doc->m_mush_name))
             : wxTitle);
}

void CWorldChildFrame::setStatus(const char* text)
{
    // world.SetStatus(msg): show a message in the shared main-window status bar,
    // but only for the active world (overrides the connection-state message
    // until the next timer tick or the next SetStatus call replaces it).
    if (g_wxhook_set_status && IsActiveWorld())
        g_wxhook_set_status(std::string(text).c_str());
}

void CWorldChildFrame::setBackgroundColour(pal::Colour c)
{
    ApplyPaneBackground(c);
}

void CWorldChildFrame::setMaxLines(int n)
{
    if (m_output) m_output->SetMaxLines(n);
}

void CWorldChildFrame::requestRepaint(pal::Rect /*area*/)
{
    // The engine only ever needs a full-pane refresh here; the sub-rect is
    // advisory and the pane repaints lazily, so ignore it. Refresh(false) = no
    // background erase (same semantics as the old repaint callback).
    if (m_output) m_output->Refresh(false);
}

void CWorldChildFrame::clearOutput()
{
    if (m_output) m_output->ClearOutput();
}

void CWorldChildFrame::setPaused(bool paused)
{
    if (m_output) m_output->SetPaused(paused);
}

void CWorldChildFrame::flash()
{
    RequestUserAttention();
}

std::string CWorldChildFrame::currentCommand() const
{
    wxTextCtrl* in = ActiveInput();
    return in ? std::string(in->GetValue().utf8_str()) : std::string();
}

pal::TextRange CWorldChildFrame::outputSelection() const
{
    // Line/col endpoints (1-based; 0 = no selection), from the pane's selection
    // state — same semantics as the old selection callback.
    pal::TextRange r;
    if (m_output) {
        long sl = 0, sc = 0, el = 0, ec = 0;
        if (m_output->GetSelectionInfo(sl, sc, el, ec))
            r = { (int) sl, (int) sc, (int) el, (int) ec };
    }
    return r;
}

int CWorldChildFrame::scrollPos() const
{
    return m_output ? (int) m_output->GetScrollPositionY() : 0;
}

pal::Size CWorldChildFrame::outputAreaChars() const
{
    return m_doc ? pal::Size{ m_doc->m_gui_cols, m_doc->m_gui_rows }
                 : pal::Size{ 80, 24 };
}

// ── the command input bar (P2 command family) ────────────────────────────────

// world.SetCommand(text): set the command bar contents.  Per the Windows API
// contract, returns false (eCommandNotEmpty) and does NOT overwrite if the bar
// is already non-empty.  Returns true on success.
bool CWorldChildFrame::setCommand(const char* text)
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp) return false;
    if (!inp->GetValue().IsEmpty()) return false;  // eCommandNotEmpty
    inp->SetValue(wxString::FromUTF8(text));
    inp->SetInsertionPointEnd();
    m_histIdx = -1; ApplyHistRSearch(false);
    return true;
}

// world.SelectCommand(): select all text in the command input bar.
void CWorldChildFrame::selectCommand()
{
    wxTextCtrl* inp = ActiveInput();
    if (inp) inp->SetSelection(-1, -1);  // wxWidgets: -1,-1 = select all
}

// GetInfo(236/237): the command input selection range.
pal::Range CWorldChildFrame::commandSelection() const
{
    pal::Range r;
    wxTextCtrl* inp = ActiveInput();
    if (inp) {
        long from = 0, to = 0;
        inp->GetSelection(&from, &to);
        r.start = (int) from;
        r.end   = (int) to;
    }
    return r;
}

// world.SetCommandSelection(first, last): select a range in the command bar.
// Indices are 1-based character positions (0 = end of bar).
void CWorldChildFrame::setCommandSelection(long first, long last)
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp) return;
    long len = (long)inp->GetValue().size();
    long f = (first  <= 0) ? len : first  - 1;
    // End is exclusive (one past the last char), matching Windows'
    // CEdit::SetSel(First - 1, Last): only the start is decremented.
    long l = (last   <= 0) ? len : last;
    inp->SetSelection(f, l);
}

// world.PushCommand(): save current bar text to history, clear bar.
std::string CWorldChildFrame::pushCommand()
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp) return {};
    std::string text(inp->GetValue().ToUTF8());
    if (!text.empty()) {
        wxString wxText = wxString::FromUTF8(text.c_str());
        if (m_history.empty() || m_history.back() != wxText)
            m_history.push_back(wxText);
        if ((int)m_history.size() > HistoryLimit())
            m_history.erase(m_history.begin());
    }
    inp->Clear();
    m_histIdx = -1;
    ApplyHistRSearch(false);
    return text;
}

// world.GetCommandList(count): up to count most-recent history entries,
// newest first.  count <= 0 means all.
std::vector<std::string> CWorldChildFrame::commandList(long count) const
{
    std::vector<std::string> result;
    if (m_history.empty()) return result;
    long total = (long)m_history.size();
    long n = (count <= 0) ? total : std::min(count, total);
    result.reserve((size_t)n);
    for (long i = total - 1; i >= total - n; --i)
        result.push_back(std::string(m_history[(size_t)i].ToUTF8()));
    return result;
}

// world.AddToCommandHistory(text): inject a string into history.
// Mirrors the duplicate-suppression logic in methods_sending.cpp.
void CWorldChildFrame::addToCommandHistory(const char* text)
{
    if (!*text) return;
    wxString entry = wxString::FromUTF8(text);
    if (!m_history.empty() && m_history.back() == entry)
        return;   // same as last entry — don't add duplicate
    m_history.push_back(entry);
    if ((int)m_history.size() > HistoryLimit())
        m_history.erase(m_history.begin());
    m_histIdx = -1;  // reset browsing position
}

// world.DeleteCommandHistory(): clear the input history.
void CWorldChildFrame::deleteCommandHistory()
{
    m_history.clear();
    m_histIdx = -1;
    m_histSaved.Clear();
    // Rewrite (empty) history file so the cleared history persists.
    if (!m_historyFilePath.empty()) {
        FILE* f = fopen(m_historyFilePath.c_str(), "w");
        if (f) fclose(f);
    }
}

// world.SetInputFont(face, pts, bold, italic): change the command-bar font.
// face "" / pts 0 mean "keep current".
void CWorldChildFrame::setInputFont(const char* face, int pointSize,
                                    bool bold, bool italic)
{
    wxFont cur = m_input ? m_input->GetFont() : wxNullFont;
    wxString faceName = !*face
        ? (cur.IsOk() ? cur.GetFaceName() : wxString())
        : wxString::FromUTF8(face);
    int size = (pointSize > 0) ? pointSize : (cur.IsOk() ? cur.GetPointSize() : 10);
    wxFontStyle  style  = italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL;
    wxFontWeight weight = bold   ? wxFONTWEIGHT_BOLD  : wxFONTWEIGHT_NORMAL;
    wxFont f(size, wxFONTFAMILY_TELETYPE, style, weight,
             /*underline=*/false, faceName);
    if (!f.IsOk()) return;
    if (m_input)         { m_input->SetFont(f);         m_input->Refresh(); }
    if (m_inputPassword) { m_inputPassword->SetFont(f); m_inputPassword->Refresh(); }
    Layout();
}

// ── output-pane management (P2 output family) ────────────────────────────────

// Bracket a run of appendLine calls: one scrollbar update + repaint per batch.
void CWorldChildFrame::beginBatch()
{
    if (m_output) m_output->BeginBatch();
}

void CWorldChildFrame::endBatch()
{
    if (m_output) m_output->EndBatch();
}

// world.DeleteLines(n): remove the last n lines from the visual buffer.
void CWorldChildFrame::deleteLines(long n)
{
    if (m_output) m_output->DeleteTailLines(n);
}

// world.SetScroll(line): -1 = bottom (resume auto-scroll, matching Windows'
// "-1 goes to the end"), 0 = top, > 0 = 1-based line number.
// (-2 "no change" is filtered engine-side.)
void CWorldChildFrame::setScroll(long pos)
{
    if (!m_output) return;
    if (pos == -1) {
        m_output->ScrollToBottom();
    } else if (pos <= 0) {
        m_output->ScrollToTop();
    } else {
        // Convert 1-based → 0-based.
        m_output->ScrollToLine((int)(pos - 1));
    }
}

// world.SetSelection(sl, el, sc, ec): set text selection in the output pane.
// Lines and columns are 1-based; 0 means start/end of line.
// All-zeros clears the selection.
void CWorldChildFrame::setOutputSelection(int sl, int el, int sc, int ec)
{
    if (m_output) m_output->SetSelectionRange(sl, el, sc, ec);
}

// world.SetOutputFont(face, pts, bold, italic): change the output pane font.
// pts == 0 means "keep current size".
void CWorldChildFrame::setOutputFont(const char* face, int pointSize,
                                     bool bold, bool italic)
{
    if (!m_output) return;
    wxFont cur = m_output->GetCurrentFont();
    wxString faceName = !*face
        ? cur.GetFaceName()
        : wxString::FromUTF8(face);
    int size = (pointSize > 0) ? pointSize : cur.GetPointSize();
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
    // NOTE: the input bars are deliberately NOT synced here.  The input font is
    // a separate setting (World Properties > Input → ChangeInputFont →
    // setInputFont), and on Windows changing the output font never touches it.
    // Syncing here would clobber the user's configured input font every time the
    // output font changed — configuration.cpp applies the input font first and
    // the output font second, so the output font would always win.  The input
    // bars are still seeded from the output font at construction, and Ctrl+scroll
    // zoom still updates both on purpose (see m_fn_grid_changed).
    Layout();
}

// world.TextRectangle(): a plugin changed the reserved text area — recompute
// the engine wrap column + re-send NAWS for the new visible width, so the MUD
// wraps to the same width the client shows (no surprise client-side
// re-wrapping).  Plugins set the rectangle DURING OnPluginWorldOutputResized,
// i.e. AFTER the resize handler already sent NAWS for the full width, so this
// re-sends it; gated on an actual change to avoid NAWS spam.
void CWorldChildFrame::textRectChanged()
{
    if (!m_doc || !m_output) return;
    int charW = m_output->CharWidth();
    int lineH = m_output->LineHeight();
    if (charW <= 0 || lineH <= 0) return;
    int paneW, paneH;
    m_output->GetClientSize(&paneW, &paneH);
    if (paneW <= 0 || paneH <= 0) return;
    int textCols = EffectiveTextCols(m_doc, paneW, charW);
    unsigned short newWrap = (unsigned short)std::max(1, std::min(textCols, 65535));
    if (newWrap == m_doc->m_nWrapColumn) return;   // unchanged → no NAWS spam
    m_doc->m_nWrapColumn = newWrap;
    m_doc->SendNAWS(textCols, paneH / lineH);
}

// world.GetWorldWindowPosition(): screen rect of the output pane.
// Scripts use this to position miniwindows relative to the output area.
pal::Rect CWorldChildFrame::outputRect() const
{
    if (m_output) {
        wxPoint pos = m_output->ClientToScreen(wxPoint(0, 0));
        wxSize  sz  = m_output->GetClientSize();
        return { pos.x, pos.y, sz.x, sz.y };
    }
    return { 0, 0, 800, 600 };
}

// ── world/main window management (P2 window family) ──────────────────────────

// world.SetMainTitle(s): update the main application window title.
void CWorldChildFrame::setMainTitle(const char* title)
{
    if (wxFrame* top = dynamic_cast<wxFrame*>(wxTheApp->GetTopWindow()))
        top->SetTitle(wxString::FromUTF8(title));
}

// world.MoveWorldWindow(l,t,w,h): reposition/resize this MDI child frame.
// Coordinates are in pixels relative to the MDI parent client area.
void CWorldChildFrame::moveWorldWindow(long l, long t, long w, long h)
{
    SetSize((int)l, (int)t, (int)w, (int)h);
}

// world.SetWorldWindowStatus(n): change the MDI child frame window state.
// 1=maximize, 2=iconize/minimize, 3=restore, 4=show normal.
void CWorldChildFrame::setWindowStatus(int status)
{
    switch (status)
      {
      case 1: Maximize(true);                    break;
      case 2: Iconize(true);                     break;
      case 3: Restore();                         break;
      case 4: Maximize(false); Iconize(false);   break;
      }
}

// world.ActivateClient(): restore the main frame if iconized and raise it.
void CWorldChildFrame::activateClient()
{
    wxMDIParentFrame* parent = wxDynamicCast(GetParent(), wxMDIParentFrame);
    if (parent) {
        if (parent->IsIconized()) parent->Restore();
        parent->Raise();
    }
    // Also bring this child frame to the front within the MDI.
    Activate();
}

// world.GetInfo(125): is this world's frame full-screen?
bool CWorldChildFrame::isFullscreen() const
{
    return IsFullScreen();
}

// world.Save(): save the world file to its current path.  Returns false
// if the world has no path yet (the caller should handle that case).
bool CWorldChildFrame::saveWorld()
{
    CString pathCStr = m_doc->GetPathName();
    std::string savePath = static_cast<const char*>(pathCStr);
    if (savePath.empty()) return false;
    return m_doc->OnSaveDocument(savePath.c_str()) != FALSE;
}

// world.GetMainWindowPosition(): screen rect of the MDI parent frame.
pal::Rect CWorldChildFrame::mainRect() const
{
    wxMDIParentFrame* parent = wxDynamicCast(GetParent(), wxMDIParentFrame);
    if (parent) {
        wxRect r = parent->GetScreenRect();
        return { r.x, r.y, r.width, r.height };
    }
    return { 0, 0, 1024, 768 };
}

// ── the info bar (P2 infobar family) ─────────────────────────────────────────
// On Windows these drive a CRichEditCtrl on the main frame; here the world
// child frame hosts a styled wxTextCtrl.  world.Info appends text in the
// current style; InfoColour/InfoFont update that style; InfoBackground
// recolours the strip.

void CWorldChildFrame::infobarShow(bool visible)
{
    if (m_infobar->IsShown() != visible) { m_infobar->Show(visible); Layout(); }
}

void CWorldChildFrame::infobarClear()
{
    m_infobar->Clear();
    m_infobarStyle = wxTextAttr(*wxBLACK);
    m_infobarStyle.SetFont(m_infobar->GetFont());
    m_infobar->SetBackgroundColour(wxColour(255, 255, 255));
}

void CWorldChildFrame::infobarText(const char* text)
{
    long start = m_infobar->GetLastPosition();
    m_infobar->AppendText(wxString::FromUTF8(text));
    m_infobar->SetStyle(start, m_infobar->GetLastPosition(), m_infobarStyle);
}

void CWorldChildFrame::infobarColour(pal::Colour rgb)
{
    m_infobarStyle.SetTextColour(
        wxColour(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)));
}

void CWorldChildFrame::infobarBackground(pal::Colour rgb)
{
    m_infobar->SetBackgroundColour(
        wxColour(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)));
    m_infobar->Refresh();
}

void CWorldChildFrame::infobarFont(const char* face, int pointSize, int style)
{
    wxFont f = m_infobarStyle.HasFont() ? m_infobarStyle.GetFont()
                                        : m_infobar->GetFont();
    if (!!*face) f.SetFaceName(wxString::FromUTF8(face));
    if (pointSize > 0) f.SetPointSize(pointSize);
    f.SetWeight   ((style & 1) ? wxFONTWEIGHT_BOLD  : wxFONTWEIGHT_NORMAL);
    f.SetStyle    ((style & 2) ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL);
    f.SetUnderlined((style & 4) != 0);
    m_infobarStyle.SetFont(f);
    m_infobarStyle.SetFontUnderlined  ((style & 4) != 0);
    m_infobarStyle.SetFontStrikethrough((style & 8) != 0);
}

// world.GetInfo(257/258): live InfoBar size (0,0 while hidden).
pal::Size CWorldChildFrame::infobarSize() const
{
    if (m_infobar && m_infobar->IsShown()) {
        wxSize s = m_infobar->GetSize();
        return { s.x, s.y };
    }
    return { 0, 0 };
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
    // from firing between m_pWorldView=nullptr and delete doc.  (The value
    // member also stops itself when the frame is destroyed, covering the Cmd+Q
    // teardown path where OnClose never runs.)
    m_worldTimer.Stop();

    if (m_doc) {
        // Capture the world file path before clearing the doc, so we can
        // remove it from the startup-reopen list below.
        std::string worldPath = static_cast<const char*>(m_doc->GetPathName());

        // Detach the view BEFORE deleting the doc so that any final output
        // flushed by the destructor doesn't call AppendLine on the dying pane
        // (m_pWorldView is this frame; every Emit*/pull helper no-ops on null).
        m_doc->m_pWorldView = nullptr;

        // Deregister from the world doc-template so GetWorldList() no longer
        // returns this world after it is destroyed.
        if (App.m_pWorldDocTemplate)
            App.m_pWorldDocTemplate->RemoveDocument(m_doc);

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
        int textCols = EffectiveTextCols(m_doc, paneW, charW);
        // Keep m_nWrapColumn in sync so the engine wraps output at the visible
        // text width (full pane minus any plugin TextRectangle).  Matches what
        // a CLI would do on SIGWINCH.
        m_doc->m_nWrapColumn = (unsigned short)std::max(1, std::min(textCols, 65535));
        // Keep the GUI geometry fields current too — GetInfo(280/281) report the
        // output pane's pixel height/width from these (rows*lineH, cols*charW), and
        // plugins (e.g. Quow's screen margins) compute their bottom/right margins
        // from them.  The font-change path sets these; without doing it here as
        // well they'd keep the *startup* height after the pane later shrinks (when
        // the input bar takes its space), so GetInfo(280) over-reported and bottom
        // margins landed too low.
        m_doc->m_gui_cols        = cols;
        m_doc->m_gui_rows        = rows;
        m_doc->m_gui_font_height = lineH;
        m_doc->m_gui_font_width  = charW;
        m_doc->m_FontHeight      = lineH;
        m_doc->m_FontWidth       = charW;
        m_doc->m_InputFontHeight = lineH;
        m_doc->m_InputFontWidth  = charW;
        m_doc->SendNAWS(textCols, rows);
        // Fire OnPluginWorldOutputResized — mirrors mushview.cpp CMUSHView::OnSize().
        m_doc->SendToAllPluginCallbacks(ON_PLUGIN_WORLD_OUTPUT_RESIZED);
    });
}

#endif // !_WIN32
