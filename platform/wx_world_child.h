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
#include <functional>                 // std::function — the close-teardown observer
#include <string>
#include <vector>
#include <memory>
#include "platform/wx_outputpane.h"   // COutputPane → output_types.h
#include "platform/wx_print.h"        // SPrintLine — BuildPrintLines
#include "platform/wx_toolbar.h"      // CWorldToolBars — Standard/World/Activity bars
#include "pal/iworldview.h"           // pal::IWorldView — this frame is the wx adapter

// ── The one writer of the visible world window's persisted geometry ───────
// /geometry/child_w + /geometry/child_h (+ child_x + child_y) are what
// CWorldChildFrame's constructor reads back, so every path that wants
// "remember this window's size and position" must come through here.  Two
// do: the frame's own close handler, and the macOS quit hook
// (wx_mfc_impl.cpp) — [NSApp terminate:] never runs the close handler, and
// when the quit path wrote a different key set instead, a Cmd-Q silently
// lost the size/position the user had just dragged out.  Rects with a size
// below the frame's own minimum (200x100) are ignored entirely — position
// included — exactly as the close handler always did for size.
class wxConfigBase;   // <wx/config.h> is included by the .cpp, not this header
void wx_save_child_geometry(wxConfigBase& cfg, const wxRect& rect);
// ── Miniwindow compositing / tooltips (wx_world_child.cpp) ───────────────────
// Free functions, not frame members: they need only the doc, and the in-app
// compositor selftest (platform/wx_composite_selftest.cpp) drives the real
// bodies without building a window.  See the definitions for the Windows
// sources they mirror (CMUSHView::OnDraw + Blit_Bitmap, and
// CMUSHView::Mouse_Tooltip_MiniWindow).
// Transparency-mask cache instrumentation, for the composite selftest.  The
// cache is invisible from outside -- the drawn result is identical whether the
// mask was rebuilt or reused, which is exactly what makes a broken one hard to
// notice -- so the counters are exposed rather than inferred.
void wx_miniwindow_mask_cache_stats(long& builds, long& hits);
void wx_miniwindow_mask_cache_reset();
long wx_miniwindow_mask_cache_size();
// Release the entries belonging to one document.  The compositor's own sweep is
// scoped to the doc being painted (it is the only one whose miniwindow order it
// can see), so a closing world has to hand its bitmaps back itself.
void wx_miniwindow_mask_cache_forget(const CMUSHclientDoc* doc);

void wx_composite_miniwindows(CMUSHclientDoc* doc, wxDC& destDC,
                              int clientW, int clientH,
                              int frameW, int frameH, bool bUnder);
bool wx_miniwindow_tooltip(CMUSHclientDoc* doc, int x, int y,
                           std::string& text);

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

    // ── Wrap column + NAWS ─────────────────────────────────────────────────
    // Recompute the engine's wrap column from the visible text width — but only
    // when the world asked for that (world option auto_wrap_window_width), which
    // is the gate Windows applies in CMUSHView::AutoWrapWindowWidth — then tell
    // the MUD the size.  Every trigger (frame resize, font/zoom change, a plugin
    // moving the text rectangle) goes through here so they cannot disagree.
    // skipIfUnchanged suppresses a redundant NAWS packet when nothing moved.
    // Returns false if the pane has no usable metrics yet.
    // Public so the in-app selftest can drive the recompute directly: it is the
    // only way to observe world.TextRectangle reaching m_nWrapColumn and NAWS
    // (a resize event cannot pin the arithmetic, only that something changed).
    bool ApplyWrapColumnAndNAWS(bool skipIfUnchanged = false);

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
    // Everything OnClose writes to disk before teardown: the up-arrow history
    // file and the session log's postamble+close.  Public because
    // [NSApp terminate:] skips OnClose entirely, so the quit hook calls it too.
    void        PersistBeforeTeardown();

    // The per-world command-history file this frame loaded from and will save
    // to, fixed at construction.  Public for the in-app selftest, which has to
    // look at what reached disk — and the name is no longer derivable from the
    // world name alone, since a hash of the full name disambiguates it.
    const std::string& HistoryFilePath() const { return m_historyFilePath; }

    // Both command bars, for the in-app selftest.  The password swap is the one
    // place where BOTH have to be inspected at once — shown/hidden, and empty or
    // not — and ActiveInputPublic() by construction can only ever see one.
    wxTextCtrl* NormalInputPublic()   const { return m_input; }
    wxTextCtrl* PasswordInputPublic() const { return m_inputPassword; }

    // The world timer's echo-suppression edge detector (see the body).  Public
    // because it IS the seam: driving the real 100 ms tick would test the swap
    // but never the latch that stops it repeating.  Returns true if it swapped.
    bool PollNoEcho();

    // ── Close teardown ─────────────────────────────────────────────────────
    // The steps Cmd+W runs once the prompts are settled, in the order they
    // must happen.  Each is a hazard if it moves: a running timer can call into
    // a half-freed doc; a plugin's OnPluginClose / SaveState is where Quow-class
    // packs persist their miniwindow layout, so it has to run while the pane is
    // still attached; and the view must be detached before anything is deleted
    // or the doc's final flush paints into a dying pane.
    enum class CloseStep {
        TimerStopped,
        WorldCloseFired,      // the world's own OnWorldClose script
        // (No PluginsClosed / PluginStateSaved step: ~CPlugin does both, once,
        // during DocDeleted — see the note in RunCloseTeardown.)
        ViewDetached,         // m_pWorldView = nullptr
        Deregistered,         // out of the doc template + the doc→frame registry
        DocDeleted
    };
    using CloseObserver = std::function<void(CloseStep)>;

    // Called after each step completes.  Both the frame and its doc are gone by
    // the time OnClose returns, so this is the only way the order can be read
    // back — the in-app selftest installs one and closes a real world.
    void SetCloseTeardownObserver(CloseObserver observe)
        { m_closeObserver = std::move(observe); }

    // Ordered teardown; see the body.  Runs the same steps whether it is
    // reached through the close handler or called directly.
    void RunCloseTeardown(const CloseObserver& observe = CloseObserver());

    // Is the 100 ms world timer still armed?  Asserted by the teardown test:
    // "stopped before the doc is touched" is otherwise unobservable.
    bool IsWorldTimerRunning() const { return m_worldTimer.IsRunning(); }

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

    // Display ▸ Clear Output Buffer (ID_DISPLAY_CLEAR_OUTPUT), the canonical
    // command — prompt, engine buffer, pane, find state, all of it.  Ctrl+L and
    // the output pane's context menu both route here rather than clearing the
    // pane behind the engine's back; see the body for why that mattered.
    void        ClearOutputBuffer();
    // Display ▸ Clear to Start (⌘K), port-local: keep the server's current
    // prompt and free everything above it.  Returns how many lines went, so a
    // caller (the selftest) can tell "nothing to do" from "did nothing".
    long        ClearToStart();
    // Display ▸ Clear to Previous Mark (⌘L): keep the last command the user
    // typed and everything since.  Returns the number of lines freed; 0 when
    // there is no such command in the buffer, so the command does nothing.
    long        ClearToPreviousMark();
    // Display ▸ Clear Scrollback (⌥⌘K): free everything that has scrolled out
    // of sight above the viewport, keeping the visible screen intact.
    long        ClearScrollback();
    // Display ▸ Clear Screen (⌃⌘L): scroll the last line to the top of the
    // viewport and erase nothing.  The only one of the four that frees no
    // memory — and the only one that can be undone, by scrolling back up.
    bool        ClearScreen();
    // The 1-based DOC line number that pane row `firstPaneLine` shows -- what
    // ON_PLUGIN_DRAW_OUTPUT_WINDOW hands plugins so they can align miniwindows
    // to the text.  Public because its only caller is inside OnPaint, which no
    // test can reach; see the body.
    long        PaneLineToDocLine(int firstPaneLine) const;
    // The four clears' menu accelerators, in wx's FromString notation, where
    // "Ctrl" is Command on macOS and "RawCtrl" the physical Control key.
    // Defined next to the menu that uses them (wx_mfc_impl.cpp) and named here
    // so the in-app selftest checks the same strings the menu is built from --
    // an accelerator wx cannot parse yields a menu item with no shortcut and
    // no error, which is invisible until someone reaches for the key.
    static const char* const kAccelClearToStart;      // Cmd-K
    static const char* const kAccelClearToMark;       // Cmd-L
    static const char* const kAccelClearScrollback;   // Alt-Cmd-K
    static const char* const kAccelClearScreen;       // Ctrl-Cmd-L
    void        setPaused(bool paused) override;
    bool        isPaused() const override;
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
    void        deleteHeadLines(long n) override;
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
    void        mouseMoveMiniWindow(long x, long y) override;
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
    // False when the user CANCELLED the suggestion dialog: sendvw.cpp:344 makes
    // that abort the send and leave the text in the bar, so the caller has to be
    // able to tell cancel from "checked clean".
    bool SpellCheckInput();

    // Print the world output buffer (File ▸ Print World) via wxHtmlEasyPrinting.
    // Print the output buffer, or (2026-08-12) just the selection.  On
    // Windows the selection choice is the print dialog's radio; the port
    // opens a preview frame with no such dialog, so File carries a separate
    // "Print Selection" item instead (greyed without one — see
    // HasOutputSelection).  The line builder is public for the selftest.
    void PrintWorld(bool selectionOnly = false);
    bool BuildPrintLines(bool selectionOnly, std::vector<SPrintLine>& lines) const;
    bool HasOutputSelection() const;
    // Ctrl+click alias menu (2026-08-13): word under the hit for GetInfo(86),
    // then a wxMenu of every enabled, labelled alias flagged "menu", and the
    // chosen one executed -- the doc's hoisted trio does the engine half.
    // col is the pane's CHARACTER column (converted to bytes inside).
    void ShowAliasMenu(int lineIdx, int col, const wxPoint& panePos);
    // Whether the command bar's wxEVT_CONTEXT_MENU should become the alias
    // menu above instead of AppKit's native edit menu (2026-08-15 fix for
    // BUG-2, GUI drive run 2026-08-12/13: Ctrl+click on the command bar is
    // intercepted by macOS before it ever reaches wxEVT_LEFT_DOWN, so it has
    // to be caught here instead -- see BuildInputBar). A context-menu event
    // carries no modifier state of its own; the caller polls
    // wxGetKeyState(WXK_RAW_CONTROL) and passes the result in, which is also
    // what lets a selftest pin the RULE without faking OS keyboard state.
    bool WantsInputBarAliasMenu(bool rawControlDown) const;

    // ── View-level output/input commands ──────────────────────────────────
    // The canonical handlers for these live on CMUSHView / CSendView (not
    // instantiated on the port), so they are reimplemented here against the
    // wx output pane / input bar / canonical dialogs.
    void GotoSelectedURL();       // Display ▸ Go To URL  (selection → browser)
    void SendMailToSelection();   // Display ▸ Send Mail To (selection → mailto:)
    void CopySelectionHTML();     // Edit ▸ Copy as HTML
    void EditSelectAll();         // Edit ▸ Select All (output pane or command bar)
    void RepeatLastCommand();     // Input ▸ Repeat Last Command
    void RecallLastWord();        // Edit ▸ Recall Last Word (also Ctrl+Backspace)
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
    void GotoLine();       // prompt for a line number and go to it
    // Go To Line without the prompt: select line `line` (1-based), remember it
    // in the doc's m_lastGoTo, and scroll only if it is off screen.  Public so
    // the in-app selftest can drive it — the modal cannot complete there.
    void GotoLineTo(long line);

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
    // The three "Show bold / italic / underline" checkboxes: they gate the run
    // flags BuildOutputLine sets, so a change has to invalidate cached
    // scrollback exactly as a palette change does.
    unsigned short m_lastShowBold      = 0;
    unsigned short m_lastShowItalic    = 0;
    unsigned short m_lastShowUnderline = 0;
    bool     m_paletteSnapshotValid = false;

    // Show/hide this world's status bar (View ▸ Status Bar).
    void ShowStatusBarStrip(bool show);
    // Put a one-off message in pane 0 (the app-wide status messages that
    // CMainFrame::SetStatusMessage sends).  Returns false when this frame has
    // no visible strip, so the caller can fall back to the MDI parent's bar.
    bool SetTransientStatus(const wxString& text);
    // Selftest probe: pane 0's current text (the transient/status line).
    wxString StatusPane0() const
        { return m_statusBar ? m_statusBar->GetStatusText(0) : wxString(); }
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
    // Last wall-clock second the output-buffer fade was repainted, so the
    // world timer redraws at most once a second while text is fading.
    time_t      m_lastFadeRepaint = 0;
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

    // Installed by SetCloseTeardownObserver; empty in normal use.
    CloseObserver m_closeObserver;

    // ── Connection status tracking ─────────────────────────────────────────
    int  m_lastConnectPhase    = -1;   // tracks phase changes to update title/status
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
    long m_lastAutoResize = -1;   // auto-resize option poll (see OnWorldTimer)
    int  m_lastWrapInput  = -1;   // wrap_input poll (-1 = not yet seen)
    unsigned long m_lastInputFg = 0xFFFFFFFFUL;
    unsigned long m_lastInputBg = 0xFFFFFFFFUL;
    // Likewise for the output-pane options the ctor applies once (-1/absurd
    // sentinels so the first tick always pushes the real values).
    long m_lastMaxLines   = -1;
    bool m_lastAutoFreeze = false;
    bool m_lastCopySel    = false;
    bool m_lastCopyHtml   = false;
    bool m_lastKeepPause  = false;   // world option keep_pause_at_bottom
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
    // Create (or re-create) the command input bar — ONE body for the
    // constructor and the wrap_input toggle, so a rebuild can never silently
    // drop a binding (the smart-quote disable is exactly the wiring a second
    // creation site would have lost).  wrap_input's wrap mode is a
    // creation-time style on wxOSX, hence rebuild-on-toggle.  When `old` is
    // non-null the user's text, caret, font and focus are carried across and
    // the sizer slot is replaced in place.
    void BuildInputBar(bool wrapInput);
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
    void        SaveCommandHistoryFile();
    // Scrolling brings faded text back to full intensity for a while.
    void        CancelOutputFade();

    // Switch the input bar between normal and password (masked) mode.
    // Called from PollNoEcho whenever m_doc->m_bNoEcho changes.
    void ApplyNoEcho(bool noEcho);

    // Returns true when this frame is the active MDI child.
    bool IsActiveWorld() const;

    wxDECLARE_EVENT_TABLE();
};

#endif // !_WIN32
