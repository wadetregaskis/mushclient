// platform/output_types.h — shared styled-line types for GUI output.
//
// Defines SOutputRun and SOutputLine, used by:
//   • doc_stub.h (mushcore portable layer)
//   • doc.h      (mushapp full layer, non-Windows only)
//   • wx_outputpane.h / wx_world_child.h (GUI rendering layer)
//
// Kept in a standalone header so it can be included from files that must not
// pull in the full CMUSHclientDoc definition.

#pragma once
#ifndef _WIN32

#include <cstdint>
#include <string>
#include <vector>
// No compat.h here — output_types.h is included from inside doc.h (which is
// itself included after wx headers in wx_mfc_impl.cpp).  Pulling in compat.h
// → mfc_ui.h from that context breaks the wx/MFC stub co-existence.
// Use uint32_t directly; COLORREF is typedef uint32_t so they are identical.
// Files that need RGB() / GetRValue() etc. must include compat.h themselves.

// ── SOutputRun ────────────────────────────────────────────────────────────────
// One contiguous styled span of text in a MUD output line.
// fg/bg are Windows COLORREF values (0x00BBGGRR); bold/underline mirror ANSI.
// horiz_rule: if true, this is a horizontal-rule line (text is empty; the
//             renderer draws a full-width separator line instead).
// action: non-empty when this run is an MXP hyperlink or send-link.  Contains
//         the command(s) to send to the MUD (pipe-delimited for multi-action).
//         When clicked, the GUI should send action to EvaluateCommand().
// hint:   flyover tooltip text from the MXP href hint attribute.
struct SOutputRun {
    std::string text;
    uint32_t    fg            = 0x00C0C0C0u;  // default: light grey  (RGB 192,192,192)
    uint32_t    bg            = 0x00000000u;  // default: black        (RGB   0,  0,  0)
    bool        bold          = false;
    bool        underline     = false;
    bool        italic        = false;        // ANSI italic / MUSHclient BLINK mapped to italic
    bool        strikethrough = false;        // ANSI strikethrough (STRIKEOUT flag)
    bool        horiz_rule    = false;        // true → render as a horizontal separator
    std::string action;                       // MXP link action (empty = not a link)
    std::string hint;                         // MXP link tooltip hint
};

// ── SOutputLine ───────────────────────────────────────────────────────────────
// A complete styled line: vector of consecutive SOutputRun spans.
// Produced by DisplayMsg() on each '\n'; consumed by COutputPane::AppendLine().
// A line with a single run where run.horiz_rule==true is a horizontal rule.
using SOutputLine = std::vector<SOutputRun>;

#endif // !_WIN32
