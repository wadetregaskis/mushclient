// platform/wx_world_child.h — wxMDIChildFrame for one MUSHclient world window.
//
// CWorldChildFrame wraps a COutputPane (scrolled styled-text output widget)
// and a wxTextCtrl command-input bar into one MDI child window.  It holds a
// weak (non-owning) pointer to the CMUSHclientDoc that owns the connection;
// when the user closes the frame the doc is deleted.
//
// Include order requirement (same as wx_mfc_impl.cpp):
//   1. wx headers first
//   2. MUSHclient / MFC-compat headers after

#pragma once
#ifndef _WIN32

// Forward declarations for the doc↔frame registry defined in wx_mfc_impl.cpp.
// Call wx_register_world_frame when a CWorldChildFrame is created, and
// wx_unregister_world_frame in its OnClose before the doc is deleted.
class CMUSHclientDoc;
class CWorldChildFrame;
void wx_register_world_frame(CMUSHclientDoc* doc, CWorldChildFrame* frame);
void wx_unregister_world_frame(CMUSHclientDoc* doc);
// Raise + activate a doc's world frame, if it still has one (e.g. the Activity
// window switching to the world the user double-clicked).  No-op if none.
void wx_activate_world_frame(CMUSHclientDoc* doc);

#include <wx/wx.h>
#include <wx/mdi.h>
#include <wx/timer.h>
#include <wx/textctrl.h>              // wxTextCtrl, wxTextAttr (info bar)
#include <cstdio>
#include <string>
#include <vector>
#include <memory>
#include "platform/wx_outputpane.h"   // COutputPane → output_types.h
#include "platform/wx_toolbar.h"      // CWorldToolBars — Standard/World/Activity bars
#include "pal/iworldview.h"           // pal::IWorldView — this frame is the wx adapter

// ── CWorldChildFrame ──────────────────────────────────────────────────────────
// The frame IS this world's pal::IWorldView adapter (L3 in docs/PAL.md): the
// engine drives output through the IWorldView methods below, which forward to
// the wx output pane / input bar.  See the "── pal::IWorldView ──" section.
class CWorldChildFrame : public wxMDIChildFrame, public pal::IWorldView
{
public:
    // ── First-run window width ─────────────────────────────────────────────
    // Logical points (DIPs), not device pixels: 1000 here is 2000 physical
    // pixels on a 2x Retina display.  The first-run default width is this, or
    // the usable screen width if that is narrower.  Public because
    // hook_load_mdi_frame sizes the MDI parent by the same rule, and the two
    // drifting apart is exactly how the child ended up on a stale hard-coded
    // default nobody noticed.  An enum rather than `static const int` so that
    // binding it to std::min's const reference does not odr-use it.
    enum { kMaxDefaultWidthDIP = 1000 };

    // parent  — the top-level MDI parent frame
    // doc     — the world document (non-owning pointer; do not delete here)
    // title   — initial window title (updated to world name after connect)
    CWorldChildFrame(wxMDIParentFrame* parent,
                     CMUSHclientDoc*   doc,
                     const wxString&   title = wxT("New World"));

    // Called by hook_new_world after OnNewDocument() returns to update the
    // window title from the world name stored in the doc.
    void UpdateTitle();

    // Give external code access to the output pane (e.g. to set the font).
    COutputPane* OutputPane() const { return m_output; }

    // View ▸ Toolbar / World Toolbar / Activity Toolbar / Reset Toolbar
    // Locations.  Driven from the View-menu handler in wx_mfc_impl.cpp, which
    // applies the change to every world window so they do not drift apart.
    void ShowToolBar(CWorldToolBars::Which which, bool show)
        { if (m_toolbars) m_toolbars->SetBarVisible(which, show); }
    bool AreToolBarsVisible(CWorldToolBars::Which which) const
        { return m_toolbars && m_toolbars->IsBarVisible(which); }
    void ResetToolBars()
        { if (m_toolbars) m_toolbars->ResetLocations(); }

    // Re-read App.m_iActivityButtonBarStyle after the prefs sheet closes.
    void ReloadToolBarStyle()
        { if (m_toolbars) m_toolbars->ReloadActivityStyle(); }

    // Give external code read-only access to the associated world document.
    CMUSHclientDoc* GetDoc() const { return m_doc; }

    // Public access to the active input bar so scripting callbacks in
    // wx_mfc_impl.cpp can read/write it for world.GetCommand/SetCommand.
    wxTextCtrl* ActiveInputPublic() const { return ActiveInput(); }

    // ── pal::IWorldView ───────────────────────────────────────────────────
    // The engine's typed output seam.  In the current step only appendLine()
    // is engine-driven (via CMUSHclientDoc::EmitOutputLine); the rest are the
    // adapter's complete, ready surface and become live as their engine paths
    // migrate off the remaining m_fn_* callbacks.  Definitions: wx_world_child.cpp.
    void        appendLine(SOutputLine&& line) override;
    void        note(const char* text) override;
    void        setBookmark(long lineIndex, bool set) override;
    void        setTitle(const char* title) override;
    void        setStatus(const char* text) override;
    void        setBackgroundColour(pal::Colour c) override;
    void        setMaxLines(int n) override;
    void        requestRepaint(pal::Rect area) override;
    void        clearOutput() override;
    void        setPaused(bool paused) override;
    void        flash() override;
    std::string currentCommand() const override;
    pal::TextRange outputSelection() const override;
    int         scrollPos() const override;
    pal::Size   outputAreaChars() const override;
    bool        setCommand(const char* text) override;
    void        selectCommand() override;
    pal::Range  commandSelection() const override;
    void        setCommandSelection(long first, long last) override;
    void        replaceCommandSelection(long start, long end,
                                        const char* text) override;
    std::string pushCommand() override;
    std::vector<std::string> commandList(long count) const override;
    void        addToCommandHistory(const char* text) override;
    void        deleteCommandHistory() override;
    void        setInputFont(const char* face, int pointSize,
                             bool bold, bool italic) override;
    void        beginBatch() override;
    void        endBatch() override;
    void        deleteLines(long n) override;
    void        setScroll(long line) override;
    void        setOutputSelection(int startLine, int endLine,
                                   int startCol, int endCol) override;
    void        setOutputFont(const char* face, int pointSize,
                              bool bold, bool italic) override;
    void        textRectChanged() override;
    pal::Rect   outputRect() const override;
    void        setMainTitle(const char* title) override;
    void        moveWorldWindow(long left, long top,
                                long width, long height) override;
    void        setWindowStatus(int status) override;
    void        activateClient() override;
    bool        isFullscreen() const override;
    bool        saveWorld() override;
    pal::Rect   mainRect() const override;
    void        infobarShow(bool visible) override;
    void        infobarClear() override;
    void        infobarText(const char* text) override;
    void        infobarColour(pal::Colour c) override;
    void        infobarBackground(pal::Colour c) override;
    void        infobarFont(const char* face, int pointSize,
                            int style) override;
    pal::Size   infobarSize() const override;

    // Find/replace within the command-input text (Game ▸ Global Change).
    // Runs the canonical CGlobalChangeDlg for the from/to prompt; the input is a
    // wx control so the apply step lives here (CSendView is stubbed on the port).
    void GlobalChangeInput();

    // Generate a random character name (Game ▸ Generate Character Name) via the
    // canonical GenerateName() + CGeneratedNameDlg.
    void GenerateCharacterName();

    // Alt/Option+Arrow partial-command recall (alt_arrow_recalls_partial).
    // False when Windows would decline and do an ordinary history walk instead,
    // so the caller falls through to the plain arrow handling.
    bool AltArrowRecall(bool bUp);

    // Fire macro slot idx (the port's CSendView::SendMacro).  False if the slot
    // is empty or out of range, so a key-driven caller can fall through.
    bool FireMacro(int idx);

    // Command-id entry point for the 17 NAMED macro slots (up/down/north/…/
    // logout/quit), which have no key binding on any platform and are reached
    // only by id — world.DoCommand, the Input menu, the world toolbar.  False
    // if id is not one of them.
    bool FireNamedMacroCommand(int id);

    // Spell-check the command-input text (Game ▸ Check Spelling) — runs the
    // canonical spell-check Lua (which shows the suggestion dialog per word).
    void SpellCheckInput();

    // Print the world output buffer (File ▸ Print World) via wxHtmlEasyPrinting.
    void PrintWorld();

    // ── View-level output/input commands ──────────────────────────────────
    // The canonical handlers for these live on CMUSHView / CSendView (not
    // instantiated on the port), so they are reimplemented here against the
    // wx output pane / input bar / canonical dialogs.
    void GotoSelectedURL();       // Display ▸ Go To URL  (selection → browser)
    void SendMailToSelection();   // Display ▸ Send Mail To (selection → mailto:)
    void CopySelectionHTML();     // Edit ▸ Copy as HTML
    void RepeatLastCommand();     // Input ▸ Repeat Last Command
    void ShowCommandHistory();    // Input ▸ Command History (pick a past command)
    void HighlightSelectedWord(); // Display ▸ Highlight Word (canonical dialog → trigger)
    void MakeMultiLineTrigger();  // Display ▸ Multi-Line Trigger (canonical dialog → trigger)
    void GotoMatchingBrace(bool select); // Edit ▸ Go To / Select To Matching Brace (input bar)
    void ShowFunctionsList();     // Game ▸ Functions List (canonical reference dialog)
    void ShowTextAttributes();    // Display ▸ Text Attributes (style of selected char)
    void BookmarkSelection();     // Display ▸ Bookmark Selection (toggle on output line)
    void GotoBookmark();          // Display ▸ Go To Bookmark (scroll to next bookmark)

    // Output-pane Display commands.
    void FreezeOutput();   // toggle autoscroll pause (Display ▸ Freeze Output)
    void GotoLine();       // prompt for a line number and scroll the output to it

    // Re-send NAWS with the current pane dimensions.  Call after a font change
    // so the MUD learns the new character-grid size.
    void TriggerNAWS();

    // Apply the world's background colour to the output pane and style the input
    // bar to complement it.  col is a COLORREF; 0xFF000000 (NO_COLOUR) = default
    // black.  Called by hook_open_world_file / hook_new_world after the doc is
    // loaded so custom world backgrounds take effect on first display.
    void ApplyPaneBackground(unsigned long col);

    // True when the world's ANSI palette (or anything else the pane background
    // is derived from) has changed since the last call; snapshots the new state
    // as a side effect, so it reports each change exactly once.  Polled from the
    // world timer -- see the call site for why this is not hooked to the dialog.
    bool PaletteChanged();
    COLORREF m_lastNormalColour[8] = {0};
    COLORREF m_lastBoldColour[8]   = {0};
    COLORREF m_lastCustomBack15    = 0;
    COLORREF m_lastBackgroundColour = 0;
    BOOL     m_lastCustom16Default = 0;
    bool     m_paletteSnapshotValid = false;

    // Show/hide this world's status bar (View ▸ Status Bar).
    void ShowStatusBarStrip(bool show);
    // Put a one-off message in pane 0 (the app-wide status messages that
    // CMainFrame::SetStatusMessage sends).  Returns false when this frame has
    // no visible strip, so the caller can fall back to the MDI parent's bar.
    bool SetTransientStatus(const wxString& text);
    bool IsStatusBarStripShown() const;

    // Show/hide the inline find bar (Ctrl+F to show, Escape in find bar to hide).
    void ShowFindBar(bool show);

    // Execute the current find-bar search term, forward or backward.
    void DoFind(bool forward);

    // Re-push the App-level (File > Preferences) settings that the output pane
    // caches at construction.  Called for every open world after the global
    // prefs sheet closes so they do not sit stale until the world is reopened.
    void ApplyGlobalPrefs();

private:
    COutputPane*         m_output         = nullptr; // scrolled styled-text widget
    wxTextCtrl*          m_input          = nullptr; // normal command input bar
    wxTextCtrl*          m_inputPassword  = nullptr; // masked input — shown when telnet suppresses echo
    wxTextCtrl*          m_infobar        = nullptr; // world.Info* styled status strip (hidden until ShowInfoBar)
    wxStatusBar*         m_statusBar      = nullptr; // per-world status bar (macOS: parent MDI status bar is hidden by the child)
    wxTextAttr           m_infobarStyle;             // current style applied to appended Info text
    CMUSHclientDoc*      m_doc            = nullptr; // non-owning reference to the world doc
    // Standard/World/Activity bars (wxOSX: the MDI parent is hidden, so they
    // live here — same reason as m_statusBar above).  Owned: the wxToolBars
    // themselves are frame children and wx frees those, but this wrapper is
    // ours.  unique_ptr because the frame has no destructor to hand-clean it.
    std::unique_ptr<CWorldToolBars> m_toolbars;
    wxTimer              m_worldTimer;                // drives CheckTimers() / ON_PLUGIN_TICK.
                                                      // A value member so it is stopped when the
                                                      // frame is destroyed — even on Cmd+Q teardown,
                                                      // where OnClose does not run — preventing a
                                                      // timer callback into a freed frame.

    // ── Find bar ──────────────────────────────────────────────────────────
    wxPanel*      m_findBar      = nullptr;   // collapsible panel shown by Ctrl+F
    wxTextCtrl*   m_findInput    = nullptr;   // search term text box
    wxStaticText* m_findLabel    = nullptr;   // "N of M" match count label
    wxCheckBox*   m_findCaseBox  = nullptr;   // "Aa" case-sensitive toggle

    // ── Command bar sizing ─────────────────────────────────────────────────
    // The bar is multi-line (like Windows' ES_MULTILINE command window) and
    // grows to fit pasted text, up to this many lines; past that it scrolls.
    // enum, not `static const int`: std::min binds a const reference, which
    // odr-uses the constant and would need an out-of-line definition.
    enum { kMaxInputBarLines = 8 };
    int                   m_inputBarLines = 0;  // 0 = height not yet applied

    // ── Command history ────────────────────────────────────────────────────
    std::vector<wxString> m_history;           // sent commands, oldest first
    int                   m_histIdx = -1;       // -1 = not browsing; ≥0 = index into m_history
    wxString              m_histSaved;          // text in input when user started browsing
    bool                  m_histRSearch = false; // true = Ctrl+R substring mode (vs prefix)
    // Which end of the history the browse cursor has run past, if any.  Windows
    // keeps the same idea in CSendView::m_iHistoryStatus (eAtTop/eInMiddle/
    // eAtBottom, sendvw.h) and needs it for World Properties > Input > Commands
    // > "Arrow keys wrap around": running off an end parks there and clears the
    // bar, and only the NEXT press wraps to the far end.  Without it the wrap
    // option has nothing to test and the port could never wrap.
    enum EHistPark { eHistNotParked, eHistParkedOld, eHistParkedNew };
    EHistPark             m_histParked = eHistNotParked;
    std::string           m_historyFilePath;    // per-world history file, fixed at construction

    // ── Kill ring (emacs-style) ────────────────────────────────────────────
    // Ctrl+K and Ctrl+W save the killed text here; Ctrl+Y pastes it back.
    // Only the most-recent kill is stored (single-slot, not a true ring).
    wxString m_killBuf;

    // Tab completion lives in doc_tabcomplete_shared.cpp (canonical body,
    // shared with Windows); this frame only forwards the key.

    // ── Connection status tracking ─────────────────────────────────────────
    int  m_lastConnectPhase    = -1;   // tracks phase changes to update title/status
    int  m_reconnectCountdown  = 0;    // ticks until auto-reconnect (0 = idle)
    bool m_lastNoEcho          = false; // last known m_doc->m_bNoEcho state
    int  m_pluginTickAccum     = 0;    // ms accumulated toward next ON_PLUGIN_TICK (~40 ms)
    int  m_speedWalkAccum      = 0;    // ms accumulated toward next queued-command drain
    int  m_scriptCheckAccum    = 0;    // ms accumulated toward next script-file mtime poll (2 s)
    int  m_timerCheckAccum     = 0;    // ms accumulated toward next CheckTimers (App.m_nTimerInterval)
    int  m_settleResizeAccum   = 0;    // ms since open, for the plugin-layout settle re-notify
    int  m_settleResizeState   = 0;    // 0→fire at 2 s, 1→fire at 5 s, 2→done (see OnWorldTimer)

    // Refresh GetInfo(280/281) geometry from the pane and fire
    // ON_PLUGIN_WORLD_OUTPUT_RESIZED, so plugins (re)lay out their docked
    // miniwindows.  Used to settle the startup layout (see OnWorldTimer).
    void NotifyPluginLayoutSettle();

    // ── Session log ────────────────────────────────────────────────────────
    // Log is written as plain UTF-8 text to a user-chosen file.  Logging starts
    // when the user chooses "Start Logging…" from the context menu and stops on
    // "Stop Logging", window close, or disconnect.
    FILE*       m_logFile     = nullptr;  // open file handle; nullptr = not logging
    std::string m_logFilePath;            // path to the current log file

    // Start a new log session: opens path for appending, writes a header.
    // Returns true on success.  No-op if already logging.
    bool StartLogging(const std::string& path);

    // Stop the current log session: writes a footer and closes the file.
    // No-op if not logging.
    void StopLogging();

    // Returns true if a log file is currently open.
    bool IsLogging() const { return m_logFile != nullptr; }

    // Apply the correct foreground/background colors to both input bars based on
    // the output pane's background.  Factors out the luma logic so it can be
    // called from both ApplyPaneBackground and from the Ctrl+R mode exit path.
    void UpdateInputBarColors();
    int  HistoryLimit() const;   // world's m_nHistoryLines, with a fallback
    // Last input colours pushed to the controls, so OnWorldTimer can notice a
    // prefs change and re-apply without repainting every tick.  0xFFFFFFFF =
    // "nothing applied yet".
    unsigned long m_lastInputFg = 0xFFFFFFFFUL;
    unsigned long m_lastInputBg = 0xFFFFFFFFUL;
    // Likewise for the output-pane options the ctor applies once (-1/absurd
    // sentinels so the first tick always pushes the real values).
    long m_lastMaxLines   = -1;
    bool m_lastAutoFreeze = false;
    bool m_lastCopySel    = false;
    bool m_lastCopyHtml   = false;
    int  m_lastPixelOffset = -1;

    // Enter or leave Ctrl+R reverse-history-search mode.  Sets m_histRSearch and
    // tints the input bar blue (on) or restores its normal colors (off).
    // No-op when the state is already correct (safe to call redundantly).
    void ApplyHistRSearch(bool on);

    // Write a single output line to the log file as plain text.
    void LogLine(const SOutputLine& line);

    // (miniwindow mouse-tracking state is doc-homed now, used by the
    //  engine-owned routing: CMUSHclientDoc::m_sPreviousMiniWindow +
    //  pane capture via MiniWindowMouseHooks — docs/PAL.md S4)

    // ── Event handlers ────────────────────────────────────────────────────
    // User pressed Enter in the input bar — send text to the MUD.
    void OnInputEnter(wxCommandEvent& evt);
    // Up/Down keys in the input bar for history navigation.
    void OnInputKeyDown(wxKeyEvent& evt);
    // Lua Accelerator() API: check a key event against the doc's accelerator
    // map and fire the command if it matches.  Returns true if matched.
    bool FireAcceleratorIfMatch(wxKeyEvent& evt);
    void FireAcceleratorCommand(WORD nID);
    // Grow/shrink the multi-line command bar to fit its content (1..8 lines).
    void UpdateInputBarHeight();
    // Frame is about to close — delete the doc (disconnects + frees memory).
    void OnClose(wxCloseEvent& evt);
    // 100 ms world timer: process MUSHclient timers and flush output.
    // ON_PLUGIN_TICK is fired at ~40 ms via m_pluginTickAccum.
    void OnWorldTimer(wxTimerEvent& evt);
    // Frame resized — send updated NAWS window size to the MUD.
    void OnSize(wxSizeEvent& evt);
    // Frame activated/deactivated — refresh the status bar.
    void OnActivate(wxActivateEvent& evt);

    // ── Helpers ───────────────────────────────────────────────────────────
    // Write the current connection status to the MDI parent's status bar.
    // Called from OnActivate and OnWorldTimer (only when this frame is active).
    void UpdateStatusBar();

    // Returns the currently-visible input control (m_input or m_inputPassword).
    wxTextCtrl* ActiveInput() const;

    // ── Wrap column + NAWS ─────────────────────────────────────────────────
    // Recompute the engine's wrap column from the visible text width — but only
    // when the world asked for that (world option auto_wrap_window_width), which
    // is the gate Windows applies in CMUSHView::AutoWrapWindowWidth — then tell
    // the MUD the size.  Every trigger (frame resize, font/zoom change, a plugin
    // moving the text rectangle) goes through here so they cannot disagree.
    // skipIfUnchanged suppresses a redundant NAWS packet when nothing moved.
    // Returns false if the pane has no usable metrics yet.
    bool ApplyWrapColumnAndNAWS(bool skipIfUnchanged = false);

    // Switch the input bar between normal and password (masked) mode.
    // Called whenever m_doc->m_bNoEcho changes.
    void ApplyNoEcho(bool noEcho);

    // Returns true when this frame is the active MDI child.
    bool IsActiveWorld() const;

    wxDECLARE_EVENT_TABLE();
};

#endif // !_WIN32
