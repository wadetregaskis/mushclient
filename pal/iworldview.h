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
// interface too.  No Windows adapter exists yet — the doc's m_pWorldView stays
// NULL there, every push helper is inert and every pull reports empty/zero —
// but the seam is ready for one (the real P4/P5 step: CMUSHView becomes the
// native adapter and the Windows display path emits through it).

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
    virtual void setTitle(const char* title) = 0;   // world/window title
    virtual void setStatus(const char* text) = 0;   // status-bar text
    virtual void setBackgroundColour(Colour c) = 0;
    virtual void setMaxLines(int n) = 0;
    virtual void requestRepaint(Rect area) = 0;          // core mutated the buffer
    virtual void clearOutput() = 0;
    virtual void setPaused(bool paused) = 0;
    virtual void flash() = 0;                             // bell / attention

    // ── frontend → core: pulls (state the engine needs to read) ───────────
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

    // ── core → frontend: output-pane management (P2 output family) ────────
    // Bracket a run of appendLine calls: defer per-line scrollbar/repaint
    // work until endBatch (one update per batch).
    virtual void beginBatch() = 0;
    virtual void endBatch() = 0;
    // Buffer trim (world.DeleteLines / buffer-size change): drop the last n
    // lines from the display.
    virtual void deleteLines(long n) = 0;
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

