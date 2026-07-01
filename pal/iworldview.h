// pal/iworldview.h — the per-world window seam of the Platform Abstraction Layer.
//
// IWorldView is what the engine calls to drive one world's output window: push
// finished output lines, notes, title/status, request repaints, and pull a few
// pieces of frontend state (the current command text, the output selection, the
// scroll position). It is the typed successor to the loose bundle of ~39 m_fn_*
// std::function callbacks currently in doc_stub.h — a de-facto, untyped version
// of this interface that the headless build already injects.
//
// Implemented by: the native Win32/MFC view (Windows), the wx output pane
// (mac/Linux), and a recording fake (tests). See docs/PAL.md.
//
// NB: still guarded on !_WIN32 while SOutputLine (platform/output_types.h) is;
// promoting SOutputLine to a cross-platform canonical type is a P4 step, after
// which this guard is removed and the Windows view implements IWorldView too.

#pragma once
#ifndef _WIN32

#include <string>
#include <string_view>
#include "pal/pal_types.h"
#include "platform/output_types.h"   // SOutputLine / SOutputRun

namespace pal {

class IWorldView {
public:
    virtual ~IWorldView() = default;

    // ── core → frontend: display ──────────────────────────────────────────
    // A finished output line (one '\n'). `pending` lines are the live
    // typing-indicator row and replace, not append.
    virtual void appendLine(SOutputLine&& line) = 0;
    virtual void note(std::string_view text) = 0;        // out-of-band note
    virtual void setTitle(std::string_view title) = 0;   // world/window title
    virtual void setStatus(std::string_view text) = 0;   // status-bar text
    virtual void setBackgroundColour(Colour c) = 0;
    virtual void setMaxLines(int n) = 0;
    virtual void requestRepaint(Rect area) = 0;          // core mutated the buffer
    virtual void clearOutput() = 0;
    virtual void setPaused(bool paused) = 0;
    virtual void flash() = 0;                             // bell / attention

    // ── frontend → core: pulls (state the engine needs to read) ───────────
    virtual std::string currentCommand() const = 0;      // input box text
    virtual Range       outputSelection() const = 0;     // selected char range
    virtual int         scrollPos() const = 0;
    virtual Size        outputAreaChars() const = 0;     // for wrap-column math
};

} // namespace pal

#endif // !_WIN32
