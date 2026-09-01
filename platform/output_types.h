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
#include <memory>
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
// ── SRunLink ──────────────────────────────────────────────────────────────
// The MXP link payload of a run.  Split out of SOutputRun because it is set on
// roughly one run in a hundred but was costing every run two std::strings (48
// of 112 bytes, plus two constructions and two destructions per run).  Held by
// shared_ptr<const>: it is written once at emit and never mutated, so copies of
// a run share it for free and no custom copy constructor is needed.
struct SRunLink {
    std::string action;        // MXP link action (what a click sends/executes)
    std::string hint;          // MXP link tooltip hint
    // The style's ACTIONTYPE bits (ACTION_SEND / ACTION_HYPERLINK /
    // ACTION_PROMPT, OtherTypes.h) already masked with ACTIONTYPE.  Stored raw
    // rather than as an enum because this header must not pull in compat.h.
    // Windows branches on it when a link is clicked — a prompt-link fills the
    // command bar instead of sending — so the view has to carry it to the click.
    int         action_type = 0;
};

#include <ctime>   // time_t — SOutputLine::arrived

// ── SRunEncoding ──────────────────────────────────────────────────────────
// How to READ a run's bytes: the two facts SOutputRun carries about its own
// text, which its MXP action/hint bytes share (they came off the same wire in
// the same world).  Anything that hands those strings on to be DISPLAYED — a
// tooltip, a context-menu label, the command bar — has to hand this on with
// them, or it has nothing to decode with and falls back to bare FromUTF8,
// which is all-or-nothing: one legacy byte and the label comes back "".
struct SRunEncoding {
    bool utf8    = true;   // SOutputRun::utf8
    int  charset = 0;      // SOutputRun::charset (a Windows LOGFONT charset)
};

struct SOutputRun {
    std::string text;
    uint32_t    fg            = 0x00C0C0C0u;  // default: light grey  (RGB 192,192,192)
    uint32_t    bg            = 0x00000000u;  // default: black        (RGB   0,  0,  0)

    // Null on an ordinary run.  Use the accessors below rather than testing it:
    // they answer the empty string / 0 for a non-link, which is what every
    // caller wants and what the old plain members did.
    std::shared_ptr<const SRunLink> link;

    // The style INPUTS fg/bg were resolved from, so a palette change can
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
    int32_t     fore_idx      = 0;            // iForeColour (index, or a literal RGB)
    int32_t     back_idx      = 0;            // iBackColour
    uint16_t    style_flags   = 0;            // iFlags & STYLE_BITS
    // Windows LOGFONT charset (used iff !utf8); the doc's m_font_charset.
    int16_t     charset       = 0;

    // Bitfields rather than bools: eight separate bools cost eight bytes plus
    // padding on a struct that exists once per styled span of every line in the
    // scrollback.
    unsigned    bold          : 1;
    unsigned    underline     : 1;
    unsigned    italic        : 1;   // ANSI italic / MUSHclient BLINK
    unsigned    strikethrough : 1;   // ANSI strikethrough (STRIKEOUT flag)
    unsigned    horiz_rule    : 1;   // true → render as a horizontal separator
    // Encoding of `text`.  utf8 (the default, and the common modern case) means
    // `text` is UTF-8.  When false the world is in its native 8-bit charset and
    // `charset` is the LOGFONT charset the renderer must decode with — mirroring
    // the Windows 8-bit text path.
    unsigned    utf8          : 1;

    SOutputRun()
        : bold(0), underline(0), italic(0),
          strikethrough(0), horiz_rule(0), utf8(1) {}

    // ── link accessors ────────────────────────────────────────────────────
    // A non-link run answers "" / 0, exactly as the old plain members did, so
    // callers need no null check.
    static const std::string& NoText()
        { static const std::string kEmpty; return kEmpty; }
    const std::string& action() const      { return link ? link->action : NoText(); }
    const std::string& hint() const        { return link ? link->hint   : NoText(); }
    int                action_type() const { return link ? link->action_type : 0; }
    bool               isLink() const      { return link && !link->action.empty(); }

    // The encoding of text(), action() and hint() alike — travel it with any of
    // them that leaves the run.
    SRunEncoding encoding() const
        { return SRunEncoding{ utf8 != 0, (int) charset }; }

    void setLink(const std::string& action, const std::string& hint, int type)
    {
        if (action.empty() && hint.empty() && type == 0) { link.reset(); return; }
        SRunLink l;
        l.action = action;
        l.hint   = hint;
        l.action_type = type;
        link = std::make_shared<const SRunLink>(std::move(l));
    }
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
    // The line's USER_INPUT flag (an echoed command).  The line-preamble
    // (timestamp) family shows a different preamble per kind, as Windows does.
    bool userInput = false;
    // When the line arrived, as a Unix time (CLine::m_theTime).  Only the
    // output-buffer FADE reads it: Windows works out each line's opacity from
    // how long ago it arrived (CMUSHView::OnDraw, mushview.cpp:1096-1121), so
    // the view has to know, and the pane holds no CLine of its own.  0 means
    // "unknown", which the fade treats as "do not fade".
    time_t arrived = 0;
    // CLine::hard_return: false when the ENGINE wrapped this line at the wrap
    // column and the text continues on the next one.  Copy takes it from here:
    // CMUSHView::GetSelection writes ENDLINE only after a line whose
    // hard_return is set (mushview.cpp:4069-4085), so a soft-wrapped group is
    // one unbroken run on the clipboard.  TRUE by default deliberately: a line
    // built by anything other than BuildLineFromCLine (a selftest fixture, a
    // synthesised rule) is a whole line, and defaulting the other way would
    // silently glue such lines together.
    bool hardReturn = true;
    // Copy every LINE-level flag -- not the runs -- from another line.
    // DetectURLs rebuilds a line's runs into a fresh SOutputLine and would
    // otherwise drop all of these for any line containing "http"; keeping the
    // list here means the next flag added cannot be forgotten there.
    void copyFlagsFrom (const SOutputLine & other)
    {
        pending   = other.pending;
        bookmark  = other.bookmark;
        comment   = other.comment;
        userInput = other.userInput;
        arrived   = other.arrived;
        hardReturn = other.hardReturn;
    }
    // Inherit all std::vector constructors (default, initializer_list, etc.).
    using std::vector<SOutputRun>::vector;
};

