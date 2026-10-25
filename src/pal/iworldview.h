// pal/iworldview.h — the per-world window seam of the Platform Abstraction Layer.
//
// IWorldView is what the engine calls to drive one world's output window: push
// finished output lines, notes, title/status, request repaints, and pull a few
// pieces of frontend state (the current command text, the output selection, the
// scroll position). It is the typed successor to the loose bundle of ~39 m_fn_*
// std::function callbacks the retired headless stub carried — a de-facto, untyped version
// of this interface that the headless build already injects.
//
// Implemented by: the native Win32/MFC view (Windows), the wx output pane
// (mac/Linux), and a recording fake (tests). See docs/PAL.md.
//
// Cross-platform since P4 (SOutputLine promoted with it): MSVC compiles this
// interface too, and BOTH platforms now have a live adapter — the wx
// CWorldChildFrame on mac/Linux, and the doc-owned CWinWorldView on Windows
// (created in doc_construct.cpp under #ifdef _WIN32, WV-2).  So engine code may
// be routed through this seam without losing it on Windows; that is what
// doc_tabcomplete_shared.cpp relies on.  (This comment claimed "no Windows
// adapter exists yet" until 2026-07-25 — the third and last copy of that stale
// claim; cf. eb48ce4 for doc.h.)
//
// NOTE for anyone adding a method here: the Windows adapter drives real MFC,
// whose overload sets are WIDER than the port's shim.  A parameter type that is
// unambiguous against platform/mfc_ui.h can be ambiguous against afxwin.h, and
// the macOS build cannot see it — see the (int) casts and their comment in
// CWinWorldView::replaceCommandSelection.

#pragma once

#include <string>
#include <vector>

// NOTE: no C++17 in this header (no std::string_view) — the Windows build
// compiles at /std:c++14.  String parameters are NUL-terminated const char*
// (the pal/isurface.h + pal/itransport.h convention); std::string stays for
// the pull-side return values.
#include "pal/pal_types.h"
#include "platform/output_types.h"   // SOutputLine / SOutputRun

namespace pal {

class IWorldView {
public:
    virtual ~IWorldView() = default;

    // ── core → frontend: display ──────────────────────────────────────────
    // A finished output line (one '\n'). `pending` lines are the live
    // typing-indicator row and replace, not append.
    //
    // Contract is "consume-or-pull": an adapter may render the pushed line
    // (the wx pane) OR ignore the payload and invalidate so its view pulls
    // the engine's m_LineList (Windows CMUSHView).  This is why the display
    // family is deliberately NOT single-sourced across platforms — the full
    // rationale + declined alternatives are in docs/PAL.md, "IWorldView —
    // display-family rendering: pull vs push (decision)".
    virtual void appendLine(SOutputLine&& line) = 0;
    virtual void note(const char* text) = 0;        // out-of-band note
    // world.Bookmark / Display ▸ Bookmark Selection: the BOOKMARK flag on the
    // engine's CLine `lineIndex` (0-based, as CMUSHView::BookmarkLine takes it)
    // has just been set/cleared — reflect it.  The doc is the single source of
    // truth: `set` is the flag's new value, so a view that mirrors the flag can
    // store it without re-reading the line list.
    virtual void setBookmark(long lineIndex, bool set) = 0;
    virtual void setTitle(const char* title) = 0;   // world/window title
    virtual void setStatus(const char* text) = 0;   // status-bar text
    virtual void setBackgroundColour(Colour c) = 0;
    virtual void setMaxLines(int n) = 0;
    virtual void requestRepaint(Rect area) = 0;          // core mutated the buffer
    virtual void clearOutput() = 0;
    virtual void setPaused(bool paused) = 0;
    virtual void flash() = 0;                             // bell / attention

    // ── frontend → core: pulls (state the engine needs to read) ───────────
    // The read side of setPaused: is the output window frozen?  Windows draws
    // the Display ▸ Freeze Output tick from CMUSHView::m_freeze
    // (OnUpdateDisplayFreezeoutput), and the port's menu needs the same answer
    // from whichever frontend owns the scrollback.
    //
    // Deliberately NOT pure: an adapter that cannot freeze is honestly "never
    // paused", and a new pure virtual here would break the Windows build from a
    // tree where pal/win_worldview.cpp cannot be compiled.  Both real adapters
    // do override it.
    virtual bool isPaused() const { return false; }
    virtual std::string currentCommand() const = 0;      // input box text
    virtual TextRange   outputSelection() const = 0;     // selection line/col (1-based; 0=none)
    virtual int         scrollPos() const = 0;
    virtual Size        outputAreaChars() const = 0;     // for wrap-column math

    // ── core ↔ frontend: the command input bar (P2 command family) ─────────
    // world.SetCommand: fill the bar ONLY if it is empty; false = not empty.
    virtual bool        setCommand(const char* text) = 0;
    // world.SelectCommand: select all text in the bar.
    virtual void        selectCommand() = 0;
    // GetInfo 236/237: the bar's selection endpoints, in characters.
    virtual Range       commandSelection() const = 0;
    // world.SetCommandSelection(first, last): 1-based; <= 0 means the end
    // (only First is decremented — CEdit::SetSel(First - 1, Last) semantics).
    virtual void        setCommandSelection(long first, long last) = 0;
    // Replace the characters in [start, end) with `text`, as one undoable
    // edit.  Offsets are RAW (0-based) — the same space commandSelection()
    // reports, NOT the 1-based scripting space of setCommandSelection above.
    // Used by tab completion (doc_tabcomplete_shared.cpp), which computes the
    // word under the cursor itself.
    virtual void        replaceCommandSelection(long start, long end,
                                                const char* text) = 0;
    // world.PushCommand: save the bar's text to history, clear it, return it.
    virtual std::string pushCommand() = 0;
    // world.GetCommandList(n): up to n most-recent history entries, newest
    // first; n <= 0 means all.
    virtual std::vector<std::string> commandList(long count) const = 0;
    // world.AddToCommandHistory: append (consecutive duplicates suppressed).
    virtual void        addToCommandHistory(const char* text) = 0;
    // world.DeleteCommandHistory: clear the history.
    virtual void        deleteCommandHistory() = 0;
    // Input-bar font (face "" / size 0 = keep current).
    virtual void        setInputFont(const char* face, int pointSize,
                                     bool bold, bool italic) = 0;
    // world.SetCommandWindowHeight(px): make the command bar `px` pixels tall.
    // false = refused, and nothing moved — the engine reports eBadParameter,
    // which is what the original body returned on all three of its own refusal
    // paths.  An adapter is expected to decline a height that would leave
    // either pane unusable; the Windows one keeps the canonical
    // "top view under 20 pixels" rule, and the wx one adds a floor of one
    // whole line because it has no sash for the user to drag back.
    //
    // Deliberately NOT pure, for the reason isPaused gives below.  Both real
    // adapters override.
    virtual bool        setCommandWindowHeight(int /*px*/) { return false; }

    // ── core → frontend: output-pane management (P2 output family) ────────
    // Bracket a run of appendLine calls: defer per-line scrollbar/repaint
    // work until endBatch (one update per batch).
    virtual void beginBatch() = 0;
    virtual void endBatch() = 0;
    // Buffer trim (world.DeleteLines / buffer-size change): drop the last n
    // lines from the display.
    virtual void deleteLines(long n) = 0;
    // Drop the OLDEST n lines the view is showing.  deleteLines above takes
    // them off the TAIL (world.DeleteLines); this is the head, which is what
    // Clear Scrollback and Clear to Previous Mark need.
    virtual void deleteHeadLines(long n) = 0;
    // world.SetScroll: -1 = bottom (resume auto-scroll), 0 = top,
    // > 0 = 1-based line. (-2 "no change" is filtered engine-side.)
    virtual void setScroll(long line) = 0;
    // world.SetSelection: 1-based line/col endpoints; all zeros clears.
    virtual void setOutputSelection(int startLine, int endLine,
                                    int startCol, int endCol) = 0;
    // world.SetOutputFont (face "" / size 0 = keep current).
    virtual void setOutputFont(const char* face, int pointSize,
                               bool bold, bool italic) = 0;
    // The reserved text rectangle changed (world.TextRectangle): recompute
    // the wrap column + NAWS for the new visible width.
    virtual void textRectChanged() = 0;
    // Screen rect of the output pane (world.GetWorldWindowPosition).
    virtual Rect outputRect() const = 0;
    // World Properties ▸ Output ▸ "Adjust size to width": resize THIS WORLD'S
    // WINDOW so the output text area ends up `textWidth` pixels wide.  The
    // window moves; the wrap column does NOT (the caller has already clamped
    // the column and turned it into pixels).  false = refused, and nothing
    // moved — the caller reports eBadParameter, which is what Windows already
    // returns when it cannot find a frame to resize.
    //
    // Deliberately NOT pure, for the reason isPaused gives below: an adapter
    // with no window to resize honestly cannot do this, and a new pure virtual
    // here would break the Windows build from a tree where
    // pal/win_worldview.cpp cannot be compiled.  Both real adapters override.
    virtual bool setOutputWidthPixels(int /*textWidth*/) { return false; }

    // ── core → frontend: world/main window management (P2 window family) ──
    // world.SetMainTitle: the application (main frame) title, set from this
    // world.
    virtual void setMainTitle(const char* title) = 0;
    // world.MoveWorldWindow: reposition/resize this world's window (pixels,
    // relative to the MDI parent client area).
    virtual void moveWorldWindow(long left, long top,
                                 long width, long height) = 0;
    // world.SetWorldWindowStatus: 1=maximize, 2=minimize, 3=restore, 4=normal.
    virtual void setWindowStatus(int status) = 0;
    // world.ActivateClient: restore/raise the app, bring this world to front.
    virtual void activateClient() = 0;
    // GetInfo(125): is this world's frame full-screen?
    virtual bool isFullscreen() const = 0;
    // world.Save: save the world file to its current path; false = no path.
    virtual bool saveWorld() = 0;
    // Screen rect of the main (MDI parent) frame (world.GetMainWindowPosition).
    virtual Rect mainRect() const = 0;
    // GetInfo(249)/(250): the CLIENT rect of whatever "the main window" means
    // on this platform -- origin (0,0), as GetClientRect reports one.
    //
    // Windows answers Frame.GetClientRect: the MDI parent's client area, inside
    // its toolbars and status bar.  There is no such window to measure on
    // macOS -- the MDI parent is never shown and Frame.m_hWnd is permanently
    // null -- so the wx adapter measures the world child, which IS the window
    // the user sees.  Same reasoning, and the same documented divergence, as
    // isFullscreen above.
    //
    // Deliberately NOT pure, for the reason isPaused gives: an adapter with no
    // window to measure has nothing to say, and the doc-side accessor reports
    // false for it so the caller keeps its own approximation.
    virtual Rect mainClientRect() const { return { 0, 0, 0, 0 }; }

    // Is there a main window to measure at all?
    //
    // This exists because mainClientRect alone cannot distinguish "I have no
    // window" from "I have one and its client area is empty", and the two need
    // different answers.  A MINIMISED main frame is the second: GetClientRect
    // honestly reports 0 x 0, and the right answer for GetInfo(249)/(250) is
    // then 0 -- which is what Windows answered before this seam existed.  Read
    // as "no window", it instead produced a number derived from m_gui_rows /
    // m_gui_cols / m_gui_font_*, four members nothing on Windows ever writes,
    // so a plugin laying itself out against the main window got a plausible
    // 468 x 800 for a window that was not on screen.  Measured, and the whole
    // story is docs/WINDOWS-VALIDATION.md 10ap / 10ar.
    //
    // FALSE is the safe default: an adapter that does not override this is
    // treated exactly as it was before the predicate existed, i.e. the
    // doc-side accessor falls back.  Only an adapter that really owns a window
    // should say true.
    virtual bool hasMainWindow() const { return false; }

    // ── core → frontend: miniwindow mouse routing ─────────────────────────
    // Re-run the miniwindow mouse-move hit test at a client-space point the
    // frontend is not reporting right now.  WindowAddHotspot uses it for the
    // "the pointer is already sitting on the hotspot I just created" case, so
    // the MouseOver callback fires immediately instead of waiting for the next
    // real mouse motion (methods_miniwindows.cpp).  Frontend-side because the
    // hooks the routing needs (capture, modifier keys, tooltips) are the
    // view's: the Windows adapter runs the original CMUSHView view walk, the
    // wx frame calls the shared MouseMoveMiniWindow with its own hooks.
    virtual void mouseMoveMiniWindow(long x, long y) = 0;
    // Is the frontend holding the mouse capture right now?  WindowAddHotspot
    // reads it to SKIP the re-detect above mid-drag: while a hotspot's
    // MouseDown callback owns the pointer, adding a hotspot from the drag's own
    // Move/Release callback must not re-enter the routing and fire a second
    // MoveCallback plus a second ON_PLUGIN_MOUSE_MOVED.  Upstream asked
    // ::GetCapture() directly, which off Windows is a permanent nullptr.
    //
    // Deliberately NOT pure, for the same reason as isPaused above: an adapter
    // with no capture concept is honestly "never captured".
    virtual bool mouseCaptured() const { return false; }

    // ── core → frontend: the info bar (P2 infobar family) ─────────────────
    // world.ShowInfoBar / InfoClear / Info / InfoColour / InfoBackground /
    // InfoFont.  Colours are COLORREF; font style bits 1=bold, 2=italic,
    // 4=underline, 8=strikeout (size 0 keeps the current size).
    virtual void infobarShow(bool visible) = 0;
    virtual void infobarClear() = 0;
    virtual void infobarText(const char* text) = 0;
    virtual void infobarColour(Colour c) = 0;
    virtual void infobarBackground(Colour c) = 0;
    virtual void infobarFont(const char* face, int pointSize,
                             int style) = 0;
    // GetInfo(257/258): the info bar's pixel size (0,0 when hidden).
    virtual Size infobarSize() const = 0;
};

} // namespace pal

