// platform/output_types.h — shared styled-line types for GUI output.
//
// Defines SOutputRun and SOutputLine, used by:
//   • doc.h      (mushapp full layer, non-Windows only)
//   • wx_outputpane.h / wx_world_child.h (GUI rendering layer)
//
// Kept in a standalone header so it can be included from files that must not
// pull in the full CMUSHclientDoc definition.

#pragma once

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
    // The style's ACTIONTYPE bits (ACTION_SEND / ACTION_HYPERLINK /
    // ACTION_PROMPT, OtherTypes.h) already masked with ACTIONTYPE.  Stored raw
    // rather than as an enum because this header must not pull in compat.h.
    // Windows branches on it when a link is clicked — a prompt-link fills the
    // command bar instead of sending — so the view has to carry it to the click.
    int         action_type   = 0;
    // Encoding of `text`.  utf8==true (the default, and the common modern case)
    // means `text` is UTF-8.  When false the world is in its native 8-bit charset
    // and `charset` is the Windows LOGFONT charset (the doc's m_font_charset) the
    // renderer must decode the bytes with — mirroring the Windows 8-bit text path.
    bool        utf8          = true;
    int         charset       = 0;            // Windows charset enum (used iff !utf8)
    // The style INPUTS fg/bg above were resolved from, so a palette change can
    // re-resolve this run in place — pal::resolveStyleColours(style_flags,
    // fore_idx, back_idx, palette, xterm256, fg, bg) reproduces them exactly.
    //
    // Without these the view holds only the resolved answer, so recolouring the
    // scrollback meant re-emitting all of it (clear the pane, reset the cursor,
    // re-materialise every string, rebuild the wrap cache).  That is O(buffer)
    // on a path a script can trigger with world.SetNormalColour, and unbounded
    // once max_output_lines is 0.
    //
    // Raw, like action_type above, because this header must not pull in the MFC
    // shim: the bit meanings are in pal/style_colour.h.
    uint16_t    style_flags   = 0;            // iFlags & STYLE_BITS
    int32_t     fore_idx      = 0;            // iForeColour (index, or a literal RGB)
    int32_t     back_idx      = 0;            // iBackColour
};

// ── SOutputLine ───────────────────────────────────────────────────────────────
// A complete styled line: vector of consecutive SOutputRun spans.
// Produced by DisplayMsg() on each '\n'; consumed by COutputPane::AppendLine().
// A line with a single run where run.horiz_rule==true is a horizontal rule.
//
// pending: true while the line is still being built (no trailing '\n' yet).
// A pending SOutputLine replaces the current "typing indicator" row in the
// output pane without being committed to m_lines.
struct SOutputLine : std::vector<SOutputRun> {
    bool pending  = false;
    bool bookmark = false;   // Display ▸ Bookmark Selection toggles this
    // The line's COMMENT flag (a Note rather than MUD output).  A send-link on
    // a note line is EXECUTED, not sent — that is how world.Hyperlink and the
    // !!pluginID:sub(arg) plugin-call convention work — so the click handler
    // needs it alongside the run's action_type.
    bool comment  = false;
    // Inherit all std::vector constructors (default, initializer_list, etc.).
    using std::vector<SOutputRun>::vector;
};

