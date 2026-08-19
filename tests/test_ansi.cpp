// tests/test_ansi.cpp
// Unit tests for CMUSHclientDoc::InterpretANSIcode() and
// CMUSHclientDoc::Interpret256ANSIcode().
//
// Strategy: create a CMUSHclientDoc with a fresh CLine, feed it ANSI codes,
// then inspect the style(s) appended to the line's styleList.

#include "mc_test.h"
// B-3 atomic flip: real headers (the headless stub is gone; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"

#include <cstring>

// ── Test fixture helpers ───────────────────────────────────────────────────

// Build a doc with a current line ready to receive styles.
static void make_doc(CMUSHclientDoc& doc)
{
    doc.m_pCurrentLine = new CLine();
    // Seed the line with a plain-text style (mimics normal operation).
    doc.m_iFlags      = COLOUR_ANSI;
    doc.m_iForeColour = WHITE;
    doc.m_iBackColour = BLACK;
    // AddStyle creates the initial style on the line.
    doc.AddStyle(COLOUR_ANSI, WHITE, BLACK, 0,
                 static_cast<CAction*>(nullptr));
}

// Shorthand: get the last (tail) style from the current line.
static CStyle* tail(CMUSHclientDoc& doc)
{
    if (!doc.m_pCurrentLine) return nullptr;
    if (doc.m_pCurrentLine->styleList.IsEmpty()) return nullptr;
    return doc.m_pCurrentLine->styleList.GetTail();
}

// Count styles on the current line.
static int style_count(CMUSHclientDoc& doc)
{
    if (!doc.m_pCurrentLine) return 0;
    return doc.m_pCurrentLine->styleList.GetCount();
}

// ── Basic colour codes ────────────────────────────────────────────────────

TEST(ANSITest, DefaultStyleIsWhiteOnBlack)
{
    CMUSHclientDoc doc; make_doc(doc);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((unsigned short)COLOUR_ANSI, s->iFlags & COLOURTYPE);
    EXPECT_EQ((COLORREF)WHITE, s->iForeColour);
    EXPECT_EQ((COLORREF)BLACK, s->iBackColour);
}

TEST(ANSITest, BoldSetsHiliteFlag)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_BOLD);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & HILITE) != 0);
}

TEST(ANSITest, CancelBoldClearsHiliteFlag)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_BOLD);
    doc.InterpretANSIcode(ANSI_CANCEL_BOLD);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & HILITE) == 0);
}

TEST(ANSITest, CancelStrikeoutClearsOnlyStrikeoutFlag)
{
    // SGR 29 ("not strikethrough") must clear only STRIKEOUT and leave every
    // other active attribute untouched — exactly like the other ANSI_CANCEL_*
    // codes (see CancelBoldClearsHiliteFlag above). Regression test: the
    // implementation used `iFlags &= STRIKEOUT` (missing the `~`), which wipes
    // every OTHER flag and perversely *keeps* strikeout if it was set.
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_BOLD);
    doc.InterpretANSIcode(ANSI_UNDERLINE);
    doc.InterpretANSIcode(ANSI_STRIKEOUT);
    doc.InterpretANSIcode(ANSI_CANCEL_STRIKEOUT);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & STRIKEOUT) == 0);   // cancelled
    EXPECT_TRUE((s->iFlags & HILITE)    != 0);   // bold must survive
    EXPECT_TRUE((s->iFlags & UNDERLINE) != 0);   // underline must survive
}

TEST(ANSITest, UnderlineSetsUnderlineFlag)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_UNDERLINE);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & UNDERLINE) != 0);
}

TEST(ANSITest, InverseSetsInverseFlag)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_INVERSE);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & INVERSE) != 0);
}

TEST(ANSITest, ResetClearsAllFlags)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_BOLD);
    doc.InterpretANSIcode(ANSI_UNDERLINE);
    doc.InterpretANSIcode(ANSI_INVERSE);
    doc.InterpretANSIcode(ANSI_RESET);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & HILITE)     == 0);
    EXPECT_TRUE((s->iFlags & UNDERLINE)  == 0);
    EXPECT_TRUE((s->iFlags & INVERSE)    == 0);
    EXPECT_EQ((COLORREF)WHITE, s->iForeColour);
    EXPECT_EQ((COLORREF)BLACK, s->iBackColour);
}

// ── Foreground colour codes ───────────────────────────────────────────────

// Parameterised (audit §4, mirrors the BrightForeground table style): every 30-37
// foreground code stores its colour index in iForeColour and keeps COLOUR_ANSI.
// Also closes the previously-untested BLACK/YELLOW/MAGENTA/CYAN/WHITE arms.
TEST(ANSITest, ForegroundColourCodesSetColourIndex)
{
    struct { int code; COLORREF expected; } cases[] = {
        { ANSI_TEXT_BLACK,   BLACK   }, { ANSI_TEXT_RED,     RED     },
        { ANSI_TEXT_GREEN,   GREEN   }, { ANSI_TEXT_YELLOW,  YELLOW  },
        { ANSI_TEXT_BLUE,    BLUE    }, { ANSI_TEXT_MAGENTA, MAGENTA },
        { ANSI_TEXT_CYAN,    CYAN    }, { ANSI_TEXT_WHITE,   WHITE   },
    };
    for (auto& tc : cases) {
        CMUSHclientDoc doc; make_doc(doc);
        doc.InterpretANSIcode(tc.code);
        CStyle* s = tail(doc);
        ASSERT_NE(nullptr, s);
        EXPECT_EQ(tc.expected, s->iForeColour);
        EXPECT_EQ((unsigned short)COLOUR_ANSI, s->iFlags & COLOURTYPE);
    }
}

TEST(ANSITest, ForegroundDefaultRestoresWhite)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_TEXT_RED);
    doc.InterpretANSIcode(ANSI_SET_FOREGROUND_DEFAULT);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)WHITE, s->iForeColour);
}

// ── Background colour codes ───────────────────────────────────────────────

// Parameterised (audit §4): every 40-47 background code stores its colour index in
// iBackColour.  Closes the previously-untested BLACK/GREEN/YELLOW/BLUE/MAGENTA/CYAN/
// WHITE arms (only RED was tested before).
TEST(ANSITest, BackgroundColourCodesSetColourIndex)
{
    struct { int code; COLORREF expected; } cases[] = {
        { ANSI_BACK_BLACK,   BLACK   }, { ANSI_BACK_RED,     RED     },
        { ANSI_BACK_GREEN,   GREEN   }, { ANSI_BACK_YELLOW,  YELLOW  },
        { ANSI_BACK_BLUE,    BLUE    }, { ANSI_BACK_MAGENTA, MAGENTA },
        { ANSI_BACK_CYAN,    CYAN    }, { ANSI_BACK_WHITE,   WHITE   },
    };
    for (auto& tc : cases) {
        CMUSHclientDoc doc; make_doc(doc);
        doc.InterpretANSIcode(tc.code);
        CStyle* s = tail(doc);
        ASSERT_NE(nullptr, s);
        EXPECT_EQ(tc.expected, s->iBackColour);
    }
}

TEST(ANSITest, BackgroundDefaultRestoresBlack)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_BACK_RED);
    doc.InterpretANSIcode(ANSI_SET_BACKGROUND_DEFAULT);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)BLACK, s->iBackColour);
}

// ── 256-colour mode ───────────────────────────────────────────────────────

TEST(ANSITest, ForegroundEnter256Mode)
{
    CMUSHclientDoc doc; make_doc(doc);
    // ESC[38;... → should transition to HAVE_FOREGROUND_256_START
    doc.InterpretANSIcode(ANSI_TEXT_256_COLOUR);
    EXPECT_EQ(HAVE_FOREGROUND_256_START, doc.m_phase);
}

TEST(ANSITest, BackgroundEnter256Mode)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_BACK_256_COLOUR);
    EXPECT_EQ(HAVE_BACKGROUND_256_START, doc.m_phase);
}

// Parameterised (audit §4): the 256-colour apply path for fg (ESC[38;5;N) and bg
// (ESC[48;5;N).  When the current style is COLOUR_ANSI the raw index is stored in
// iForeColour/iBackColour and COLOUR_ANSI is kept; GetStyleRGB later resolves via
// xterm_256_colours[index] (index >= 8 path).  Only the fg arm advanced the phase to
// DOING_CODE in the original tests, so checkPhase preserves that exact assertion.
TEST(ANSITest, Colour256Applied)
{
    struct { int enterCode; int index; bool foreground; bool checkPhase; } cases[] = {
        { ANSI_TEXT_256_COLOUR, 196, true,  true  },  // fg xterm bright red (index 196)
        { ANSI_BACK_256_COLOUR,  46, false, false },  // bg xterm green (index 46)
    };
    for (auto& tc : cases) {
        CMUSHclientDoc doc; make_doc(doc);
        doc.InterpretANSIcode(tc.enterCode);   // → HAVE_*_256_START
        doc.Interpret256ANSIcode(5);           // → HAVE_*_256_FINISH
        doc.Interpret256ANSIcode(tc.index);    // → apply colour
        CStyle* s = tail(doc);
        ASSERT_NE(nullptr, s);
        if (tc.foreground) EXPECT_EQ((COLORREF)tc.index, s->iForeColour);
        else               EXPECT_EQ((COLORREF)tc.index, s->iBackColour);
        EXPECT_EQ((unsigned short)COLOUR_ANSI, s->iFlags & COLOURTYPE);
        if (tc.checkPhase) EXPECT_EQ(DOING_CODE, doc.m_phase);
    }
}

TEST(ANSITest, Foreground256ColourRgbLookup)
{
    // When GetStyleRGB is called on a style with COLOUR_ANSI and iForeColour >= 8,
    // it should resolve to xterm_256_colours[iForeColour].
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_TEXT_256_COLOUR);
    doc.Interpret256ANSIcode(5);
    doc.Interpret256ANSIcode(196);   // index 196 → bright red
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    COLORREF fg = 0, bg = 0;
    doc.GetStyleRGB(s, fg, bg);
    EXPECT_EQ(xterm_256_colours[196], fg);
}

// ── Combined attributes ───────────────────────────────────────────────────

TEST(ANSITest, BoldAndColourCombine)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_BOLD);
    doc.InterpretANSIcode(ANSI_TEXT_CYAN);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & HILITE) != 0);
    EXPECT_EQ((COLORREF)CYAN, s->iForeColour);
}

TEST(ANSITest, MultipleCodesCreateMultipleStyles)
{
    CMUSHclientDoc doc; make_doc(doc);
    int before = style_count(doc);

    // First code: foreground → red.  AddStyle coalesces onto the (empty) seed
    // style, so the count is unchanged, but the run now carries red.
    doc.InterpretANSIcode(ANSI_TEXT_RED);
    ASSERT_NE(nullptr, tail(doc));
    EXPECT_EQ((COLORREF)RED, tail(doc)->iForeColour);
    // Simulate text written under the red run so the next code cannot coalesce
    // it away (AddStyle only drops a zero-length tail style).
    tail(doc)->iLength = 3;

    // Second code: background → blue.  A genuinely new style is appended,
    // inheriting the red foreground.
    doc.InterpretANSIcode(ANSI_BACK_BLUE);
    EXPECT_EQ(before + 1, style_count(doc));
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)RED,  s->iForeColour);   // foreground persists
    EXPECT_EQ((COLORREF)BLUE, s->iBackColour);   // background from the 2nd code
}

// ── GetStyleRGB ───────────────────────────────────────────────────────────

TEST(ANSITest, GetStyleRGBAnsiNormal)
{
    CMUSHclientDoc doc; make_doc(doc);
    CStyle s;
    s.iFlags      = COLOUR_ANSI;
    s.iForeColour = RED;
    s.iBackColour = BLACK;

    COLORREF fg = 0, bg = 0;
    doc.GetStyleRGB(&s, fg, bg);

    EXPECT_EQ(doc.m_normalcolour[RED],   fg);
    EXPECT_EQ(doc.m_normalcolour[BLACK], bg);
}

TEST(ANSITest, GetStyleRGBAnsiHilite)
{
    CMUSHclientDoc doc; make_doc(doc);
    CStyle s;
    s.iFlags      = COLOUR_ANSI | HILITE;
    s.iForeColour = GREEN;
    s.iBackColour = BLACK;

    COLORREF fg = 0, bg = 0;
    doc.GetStyleRGB(&s, fg, bg);

    EXPECT_EQ(doc.m_boldcolour[GREEN],   fg);
    EXPECT_EQ(doc.m_normalcolour[BLACK], bg);
}

TEST(ANSITest, GetStyleRGBRgbPassthrough)
{
    CMUSHclientDoc doc; make_doc(doc);
    CStyle s;
    s.iFlags      = COLOUR_RGB;
    s.iForeColour = RGB(10, 20, 30);
    s.iBackColour = RGB(40, 50, 60);

    COLORREF fg = 0, bg = 0;
    doc.GetStyleRGB(&s, fg, bg);

    EXPECT_EQ(RGB(10, 20, 30), fg);
    EXPECT_EQ(RGB(40, 50, 60), bg);
}

TEST(ANSITest, GetStyleRGBHiliteInverseBoldsPostSwapForeground)
{
    // Windows parity (doc.cpp): with HILITE|INVERSE, the inverse swap happens
    // FIRST, then bold (HILITE) is applied to the post-swap foreground — i.e.
    // the cell that is now the foreground (originally the background index).
    // colour2 (background cell) stays normal intensity.
    // Pre-fix the portable code bolded the ORIGINAL foreground then swapped, so
    // it lightened the wrong half: it produced colour1=normal-blue,
    // colour2=bold-red instead of colour1=bold-blue, colour2=normal-red.
    CMUSHclientDoc doc; make_doc(doc);
    CStyle s;
    s.iFlags      = COLOUR_ANSI | HILITE | INVERSE;
    s.iForeColour = 1;   // RED
    s.iBackColour = 4;   // BLUE

    COLORREF c1 = 0, c2 = 0;
    doc.GetStyleRGB(&s, c1, c2);

    EXPECT_EQ(doc.m_boldcolour[4],   c1);   // bold BLUE (post-swap fg, bolded)
    EXPECT_EQ(doc.m_normalcolour[1], c2);   // normal RED (post-swap bg)
}

TEST(ANSITest, GetStyleRGBInverseSwapsFgBg)
{
    CMUSHclientDoc doc; make_doc(doc);
    CStyle s;
    s.iFlags      = COLOUR_RGB | INVERSE;
    s.iForeColour = RGB(10, 20, 30);
    s.iBackColour = RGB(40, 50, 60);

    COLORREF fg = 0, bg = 0;
    doc.GetStyleRGB(&s, fg, bg);

    // When INVERSE: colour1 = background, colour2 = foreground
    EXPECT_EQ(RGB(40, 50, 60), fg);
    EXPECT_EQ(RGB(10, 20, 30), bg);
}

// ── Bright / high-intensity colour codes (90–107) ────────────────────────
// These are widely used by modern MUDs; previously silently dropped.

TEST(ANSITest, BrightForegroundMapsToXtermIndex8Plus)
{
    // 90 → bright black (index 8), 91 → bright red (9), ... 97 → bright white (15)
    struct { int code; COLORREF expected_index; } cases[] = {
        {  90,  8 }, {  91,  9 }, {  92, 10 }, {  93, 11 },
        {  94, 12 }, {  95, 13 }, {  96, 14 }, {  97, 15 },
    };
    for (auto& tc : cases) {
        CMUSHclientDoc doc; make_doc(doc);
        doc.InterpretANSIcode(tc.code);
        CStyle* s = tail(doc);
        ASSERT_NE(nullptr, s);
        EXPECT_EQ(tc.expected_index, s->iForeColour);
        EXPECT_EQ((unsigned short)COLOUR_ANSI, s->iFlags & COLOURTYPE);
    }
}

TEST(ANSITest, BrightBackgroundMapsToXtermIndex8Plus)
{
    // 100 → bright black bg (index 8), ... 107 → bright white bg (15)
    struct { int code; COLORREF expected_index; } cases[] = {
        { 100,  8 }, { 101,  9 }, { 102, 10 }, { 103, 11 },
        { 104, 12 }, { 105, 13 }, { 106, 14 }, { 107, 15 },
    };
    for (auto& tc : cases) {
        CMUSHclientDoc doc; make_doc(doc);
        doc.InterpretANSIcode(tc.code);
        CStyle* s = tail(doc);
        ASSERT_NE(nullptr, s);
        EXPECT_EQ(tc.expected_index, s->iBackColour);
    }
}

TEST(ANSITest, BrightForegroundGetStyleRGBResolvesToBrightPalette)
{
    // After ESC[92m (bright green, index 10), GetStyleRGB should resolve to
    // xterm_256_colours[10] — not m_normalcolour[2].
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(92);   // bright green
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    COLORREF fg = 0, bg = 0;
    doc.GetStyleRGB(s, fg, bg);
    EXPECT_EQ(xterm_256_colours[10], fg);
    EXPECT_NE(doc.m_normalcolour[GREEN], fg);   // must differ from dim green
}

TEST(ANSITest, BrightForegroundAfterResetIsNormal)
{
    // ESC[92m then ESC[0m should restore normal (white on black).
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(92);
    doc.InterpretANSIcode(ANSI_RESET);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)WHITE, s->iForeColour);
    EXPECT_EQ((COLORREF)BLACK, s->iBackColour);
}

TEST(ANSITest, BrightForegroundInCustomModeConvertsToRGB)
{
    // Regression: a bright SGR code (90-97 / 100-107) arriving while the current
    // style is in COLOUR_CUSTOM mode (custom_16_is_default_colour worlds) must
    // convert the style to RGB and apply the bright colour, exactly as the
    // standard 30-37 / 40-47 codes already do. The CUSTOM->RGB conversion guard
    // omitted the bright ranges, so a bright code fell through to the non-RGB
    // switch, set iForeColour to a small index (8) while LEAVING the
    // COLOUR_CUSTOM flag set — at display the style was then read as
    // m_customtext[8] (a custom-palette entry) instead of the intended bright
    // colour.
    CMUSHclientDoc doc; make_doc(doc);
    // Seed a custom-palette style. Use fore index 15 (not the 8 the buggy path
    // writes) so the buggy path actually commits a visible style instead of
    // hitting the "nothing changed" early-return.
    doc.AddStyle(COLOUR_CUSTOM, 15, 15, 0, static_cast<CAction*>(nullptr));
    ASSERT_EQ((unsigned short)COLOUR_CUSTOM, tail(doc)->iFlags & COLOURTYPE);

    doc.InterpretANSIcode(90);   // bright black foreground

    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    // Pre-fix BUG: s stayed COLOUR_CUSTOM with iForeColour == 8.
    EXPECT_EQ((unsigned short)COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_EQ(doc.m_boldcolour[BLACK], s->iForeColour);   // bright black, resolved
}

// ── DisplayMsg end-to-end ANSI parsing ───────────────────────────────────
// These tests drive DisplayMsg with raw byte strings that include ESC
// sequences and verify the resulting doc style state.  They catch the
// class of bug where the ESC phase was silently reset to NONE instead of
// transitioning to HAVE_ESC, causing sequences like "[31m" to be emitted
// literally to stdout.
// (The old stdout-silencing RAII is gone: the display flip removed the
// twin's stdout ANSI emission, so DisplayMsg no longer writes to stdout.)

// Parameterised (audit §4): the DisplayMsg end-to-end ANSI cases feed a raw byte
// string (ESC sequences included) and inspect the resulting doc style state.  They
// differ only in the sequence and which of fore/back/hilite is checked.
TEST(ANSITest, DisplayMsg_AnsiSequencesSetStyleState)
{
    struct {
        const char* seq; COLORREF fore; bool checkBack; COLORREF back; bool wantHilite;
    } cases[] = {
        { "\033[31mHello",        RED,   false, 0,     false },  // foreground red
        { "\033[92mHello",        10,    false, 0,     false },  // bright green → index 10
        { "\033[31m\033[0mHello", WHITE, true,  BLACK, false },  // reset restores defaults
        { "\033[1;34mHello",      BLUE,  false, 0,     true  },  // bold + blue
    };
    for (auto& tc : cases) {
        CMUSHclientDoc doc; make_doc(doc);
        doc.DisplayMsg(tc.seq, (int)strlen(tc.seq), 0, false);
        EXPECT_EQ(tc.fore, doc.m_iForeColour);
        if (tc.checkBack)  EXPECT_EQ(tc.back, doc.m_iBackColour);
        if (tc.wantHilite) EXPECT_TRUE((doc.m_iFlags & HILITE) != 0);
    }
}

TEST(ANSITest, DisplayMsg_ESCInterruptsMXPStateCleanly)
{
    // If an ESC arrives while the phase is in a non-NONE state (e.g. after
    // a partial MXP tag was started), the phase must transition to HAVE_ESC
    // not stay stuck or go to NONE, and the subsequent ANSI sequence must
    // parse correctly.
    CMUSHclientDoc doc; make_doc(doc);
    // Force a non-NONE phase then deliver an ESC sequence.
    doc.m_phase = HAVE_ESC;   // simulate being mid-other-sequence
    const char seq[] = "\033[32mHello";
    doc.DisplayMsg(seq, (int)strlen(seq), 0, false);
    EXPECT_EQ((COLORREF)GREEN, doc.m_iForeColour);
}

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────

// ansi.cpp Interpret256ANSIcode: "code out of range, give up" — a 256-colour
// index outside 0..255 abandons the colour (m_phase = DOING_CODE) without
// indexing xterm_256_colours out of bounds; the existing style is unchanged.
TEST(ANSITest, Colour256IndexOutOfRangeAbandonsSequence)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_TEXT_256_COLOUR);   // ESC[38;
    doc.Interpret256ANSIcode(5);                   // 8-bit colour selector
    doc.Interpret256ANSIcode(300);                 // out of range: abandoned
    EXPECT_EQ(DOING_CODE, doc.m_phase);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)WHITE, s->iForeColour);    // style left unchanged
    EXPECT_EQ((unsigned short)COLOUR_ANSI, s->iFlags & COLOURTYPE);
}

// doc_shared.cpp StartNewLine: "Commented out because of bug #418 - a style
// change at the very end of the line was being discarded" — a colour change
// immediately before the newline survives: the finished line keeps the
// zero-length style and the next line begins in the new colour.
TEST(ANSITest, ColourChangeAtLineEndCarriesToNextLine)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    const char seq[] = "abc\033[31m\ndef\n";
    doc.DisplayMsg(seq, (int)strlen(seq), 0, false);

    // find the two completed lines
    POSITION pos = doc.m_LineList.GetHeadPosition();
    CLine* l0 = doc.m_LineList.GetNext(pos);
    CLine* l1 = doc.m_LineList.GetNext(pos);
    ASSERT_NE(nullptr, l0);
    ASSERT_NE(nullptr, l1);
    EXPECT_STREQ("abc", (const char*)CString(l0->text, l0->len));
    EXPECT_STREQ("def", (const char*)CString(l1->text, l1->len));

    // the end-of-line style change was NOT discarded from the finished line
    CStyle* sTail = l0->styleList.GetTail();
    ASSERT_NE(nullptr, sTail);
    EXPECT_EQ((COLORREF)RED, sTail->iForeColour);
    EXPECT_EQ(0, sTail->iLength);

    // and the next line starts in the newly-set colour
    CStyle* sHead = l1->styleList.GetHead();
    ASSERT_NE(nullptr, sHead);
    EXPECT_EQ((COLORREF)RED, sHead->iForeColour);
}

// ═══ Round-4 coverage: the RGB-mode colour arms of InterpretANSIcode ═══════

// These put the style into COLOUR_RGB mode directly, then feed plain ANSI
// codes, which must resolve against the palettes immediately (not at
// display time).

TEST(AnsiRgbMode, ForegroundCodesResolveAgainstPalettes)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pCurrentLine->styleList.GetTail()->iFlags =
        (doc.m_pCurrentLine->styleList.GetTail()->iFlags & ~COLOURTYPE) | COLOUR_RGB;

    doc.InterpretANSIcode(ANSI_TEXT_RED);          // 31 → normal red
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((unsigned short)COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_EQ(doc.m_normalcolour[1], s->iForeColour);

    doc.InterpretANSIcode(ANSI_BOLD);
    doc.InterpretANSIcode(ANSI_TEXT_GREEN);        // bold green
    s = tail(doc);
    EXPECT_EQ(doc.m_boldcolour[2], s->iForeColour);

    doc.InterpretANSIcode(ANSI_SET_FOREGROUND_DEFAULT);   // 39 → bold white
    s = tail(doc);
    EXPECT_EQ(doc.m_boldcolour[7], s->iForeColour);

    doc.InterpretANSIcode(ANSI_CANCEL_BOLD);
    doc.InterpretANSIcode(ANSI_BACK_MAGENTA);      // 45 → normal magenta bg
    s = tail(doc);
    EXPECT_EQ(doc.m_normalcolour[5], s->iBackColour);

    doc.InterpretANSIcode(ANSI_SET_BACKGROUND_DEFAULT);   // 49 → black bg
    s = tail(doc);
    EXPECT_EQ(doc.m_normalcolour[0], s->iBackColour);
}

TEST(AnsiRgbMode, InverseSwapsColourTargets)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pCurrentLine->styleList.GetTail()->iFlags =
        (doc.m_pCurrentLine->styleList.GetTail()->iFlags & ~COLOURTYPE) | COLOUR_RGB;

    doc.InterpretANSIcode(ANSI_INVERSE);
    doc.InterpretANSIcode(ANSI_TEXT_CYAN);         // fore code lands on BACK
    CStyle* s = tail(doc);
    EXPECT_EQ(doc.m_normalcolour[6], s->iBackColour);

    doc.InterpretANSIcode(ANSI_BACK_RED);          // back code lands on FORE
    s = tail(doc);
    EXPECT_EQ(doc.m_normalcolour[1], s->iForeColour);

    // alternative-inverse honours bold for the swapped colour
    doc.m_bAlternativeInverse = TRUE;
    doc.InterpretANSIcode(ANSI_BOLD);
    doc.InterpretANSIcode(ANSI_TEXT_BLUE);
    s = tail(doc);
    EXPECT_EQ(doc.m_boldcolour[4], s->iBackColour);
    doc.InterpretANSIcode(ANSI_BACK_GREEN);
    s = tail(doc);
    EXPECT_EQ(doc.m_boldcolour[2], s->iForeColour);
}

TEST(AnsiRgbMode, BrightBackgroundAndInverseArm)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_pCurrentLine->styleList.GetTail()->iFlags =
        (doc.m_pCurrentLine->styleList.GetTail()->iFlags & ~COLOURTYPE) | COLOUR_RGB;

    doc.InterpretANSIcode(104);                    // bright blue background
    CStyle* s = tail(doc);
    EXPECT_EQ(doc.m_boldcolour[4], s->iBackColour);

    doc.InterpretANSIcode(ANSI_INVERSE);
    doc.InterpretANSIcode(105);                    // inverse: goes to FORE
    s = tail(doc);
    EXPECT_EQ(doc.m_boldcolour[5], s->iForeColour);
}

TEST(AnsiRgbMode, Custom16DefaultColourArm)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bCustom16isDefaultColour = TRUE;
    doc.m_customtext[15] = RGB(11, 22, 33);
    doc.m_customback[15] = RGB(44, 55, 66);
    doc.m_pCurrentLine->styleList.GetTail()->iFlags =
        (doc.m_pCurrentLine->styleList.GetTail()->iFlags & ~COLOURTYPE) | COLOUR_RGB;

    doc.InterpretANSIcode(ANSI_SET_FOREGROUND_DEFAULT);
    CStyle* s = tail(doc);
    EXPECT_EQ(RGB(11, 22, 33), s->iForeColour);
    doc.InterpretANSIcode(ANSI_SET_BACKGROUND_DEFAULT);
    s = tail(doc);
    EXPECT_EQ(RGB(44, 55, 66), s->iBackColour);
}

// In ANSI mode with custom-16-default, codes 39/49 switch to CUSTOM colour 15;
// a subsequent palette code converts the style to RGB (the custom→RGB arm).
TEST(AnsiRgbMode, AnsiCustomDefaultThenPaletteConvertsToRgb)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bCustom16isDefaultColour = TRUE;

    doc.InterpretANSIcode(ANSI_SET_FOREGROUND_DEFAULT);   // → COLOUR_CUSTOM 15
    CStyle* s = tail(doc);
    EXPECT_EQ((unsigned short)COLOUR_CUSTOM, s->iFlags & COLOURTYPE);
    EXPECT_EQ((COLORREF)15, s->iForeColour);

    // the NEXT palette-family code converts custom → RGB, then resolves
    // itself against the custom-16 default backgrounds
    doc.m_customback[15] = RGB(9, 8, 7);
    doc.InterpretANSIcode(ANSI_SET_BACKGROUND_DEFAULT);
    s = tail(doc);
    EXPECT_EQ((unsigned short)COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_EQ(RGB(9, 8, 7), s->iBackColour);

    doc.InterpretANSIcode(ANSI_TEXT_RED);                 // plain palette code
    s = tail(doc);
    EXPECT_EQ(doc.m_normalcolour[1], s->iForeColour);
}

// ANSI_RESET in ANSI mode with custom-16-default: both colours to custom 15.
TEST(AnsiRgbMode, ResetWithCustom16Default)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bCustom16isDefaultColour = TRUE;
    doc.InterpretANSIcode(ANSI_TEXT_RED);
    doc.InterpretANSIcode(ANSI_RESET);
    CStyle* s = tail(doc);
    EXPECT_EQ((unsigned short)COLOUR_CUSTOM, s->iFlags & COLOURTYPE);
    EXPECT_EQ((COLORREF)15, s->iForeColour);
    EXPECT_EQ((COLORREF)15, s->iBackColour);
}

// RGB-mode bold upgrade/downgrade: BOLD re-maps a normal palette colour to
// its bold twin, CANCEL_BOLD maps it back.
TEST(AnsiRgbMode, BoldUpgradeAndDowngradeScanPalette)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bCustom16isDefaultColour = TRUE;   // the scans are gated on this
    doc.m_pCurrentLine->styleList.GetTail()->iFlags =
        (doc.m_pCurrentLine->styleList.GetTail()->iFlags & ~COLOURTYPE) | COLOUR_RGB;

    doc.InterpretANSIcode(ANSI_TEXT_RED);          // normalcolour[1]
    doc.InterpretANSIcode(ANSI_BOLD);              // upgrade scan
    CStyle* s = tail(doc);
    EXPECT_EQ(doc.m_boldcolour[1], s->iForeColour);

    doc.InterpretANSIcode(ANSI_CANCEL_BOLD);       // downgrade scan
    s = tail(doc);
    EXPECT_EQ(doc.m_normalcolour[1], s->iForeColour);
}

// ...and the scans must reach the LAST palette entry.  m_normalcolour and
// m_boldcolour are [8], indexed BLACK=0..WHITE=7, but both loops used to stop
// at i < 7 -- so white text (the commonest colour there is) never swapped, and
// bold/unbold silently did nothing to it in RGB mode.  The test above cannot
// see this: it exercises index 1.
TEST(AnsiRgbMode, BoldScanReachesTheLastPaletteEntry)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bCustom16isDefaultColour = TRUE;   // the scans are gated on this
    doc.m_pCurrentLine->styleList.GetTail()->iFlags =
        (doc.m_pCurrentLine->styleList.GetTail()->iFlags & ~COLOURTYPE) | COLOUR_RGB;

    // the fixture has to be able to tell the two apart, or nothing is proved
    ASSERT_NE(doc.m_normalcolour[WHITE], doc.m_boldcolour[WHITE]);

    doc.InterpretANSIcode(ANSI_TEXT_WHITE);        // normalcolour[7]
    CStyle* s = tail(doc);
    EXPECT_EQ(doc.m_normalcolour[WHITE], s->iForeColour);

    doc.InterpretANSIcode(ANSI_BOLD);              // upgrade scan
    s = tail(doc);
    EXPECT_EQ(doc.m_boldcolour[WHITE], s->iForeColour);

    doc.InterpretANSIcode(ANSI_CANCEL_BOLD);       // downgrade scan
    s = tail(doc);
    EXPECT_EQ(doc.m_normalcolour[WHITE], s->iForeColour);
}

// Blink family + cancel codes.
TEST(AnsiRgbMode, BlinkAndCancelCodes)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_BLINK);
    EXPECT_TRUE((tail(doc)->iFlags & BLINK) != 0);
    doc.InterpretANSIcode(ANSI_CANCEL_BLINK);
    EXPECT_TRUE((tail(doc)->iFlags & BLINK) == 0);
    doc.InterpretANSIcode(ANSI_FAST_BLINK);
    EXPECT_TRUE((tail(doc)->iFlags & BLINK) != 0);
    doc.InterpretANSIcode(ANSI_CANCEL_SLOW_BLINK);
    EXPECT_TRUE((tail(doc)->iFlags & BLINK) == 0);
    doc.InterpretANSIcode(ANSI_UNDERLINE);
    EXPECT_TRUE((tail(doc)->iFlags & UNDERLINE) != 0);
    doc.InterpretANSIcode(ANSI_CANCEL_UNDERLINE);
    EXPECT_TRUE((tail(doc)->iFlags & UNDERLINE) == 0);
    doc.InterpretANSIcode(ANSI_INVERSE);
    EXPECT_TRUE((tail(doc)->iFlags & INVERSE) != 0);
    doc.InterpretANSIcode(ANSI_CANCEL_INVERSE);
    EXPECT_TRUE((tail(doc)->iFlags & INVERSE) == 0);
}

// 24-bit BACKGROUND collection through the stream phases, and the abandoned
// 38/48 sequence (neither ;2 nor ;5) arm.
TEST(AnsiRgbMode, TwentyFourBitBackgroundAndAbandonedSequence)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iConnectPhase = eConnectConnectedToMud;
    const char seq[] = "\x1B[48;2;12;34;56mX";
    doc.DisplayMsg(seq, (int)sizeof seq - 1, 0);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((unsigned short)COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_EQ(RGB(12, 34, 56), s->iBackColour);

    // ESC[48;9m — invalid second parameter abandons the sequence
    const char bad[] = "\x1B[48;9mY";
    doc.DisplayMsg(bad, (int)sizeof bad - 1, 0);
    EXPECT_EQ(NONE, doc.m_phase);

    // ESC[38;9m — same for foreground
    const char bad2[] = "\x1B[38;9mZ";
    doc.DisplayMsg(bad2, (int)sizeof bad2 - 1, 0);
    EXPECT_EQ(NONE, doc.m_phase);
}

// 256-colour codes with INVERSE active swap their target.
TEST(AnsiRgbMode, Xterm256WithInverseSwapsTarget)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iConnectPhase = eConnectConnectedToMud;
    // enter RGB mode first (24-bit fore), then inverse + 256-colour codes
    const char seq[] = "\x1B[38;2;1;2;3m\x1B[7m\x1B[38;5;196m\x1B[48;5;21mX";
    doc.DisplayMsg(seq, (int)sizeof seq - 1, 0);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(xterm_256_colours[196], s->iBackColour);   // fore code → back
    EXPECT_EQ(xterm_256_colours[21],  s->iForeColour);   // back code → fore
}


// Interpret256ANSIcode, ANSI (index-storage) branch, foreground + INVERSE:
// with the style still in COLOUR_ANSI mode and INVERSE set, a foreground 256
// code (ESC[38;5;N) stores the raw index in iBackColour (the swapped target)
// — ansi.cpp line 483 `iBackColour = iCode;`. The RGB-mode counterpart is
// covered by AnsiRgbMode.Xterm256WithInverseSwapsTarget; this pins the ANSI arm.
TEST(ANSITest, Interpret256AnsiForegroundInverseStoresIndexInBackground)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_INVERSE);            // COLOUR_ANSI | INVERSE
    doc.InterpretANSIcode(ANSI_TEXT_256_COLOUR);    // -> HAVE_FOREGROUND_256_START
    doc.Interpret256ANSIcode(5);                    // 8-bit selector -> *_FINISH
    doc.Interpret256ANSIcode(196);                  // apply: fg-256 + INVERSE
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    // index-storage branch: the raw index lands in the BACKGROUND (swapped)
    EXPECT_EQ((COLORREF)196, s->iBackColour);
    // foreground index left untouched (it would become 196 if the arm swapped)
    EXPECT_EQ((COLORREF)WHITE, s->iForeColour);
    // still ANSI index mode, not RGB (proves the else-if ANSI branch ran)
    EXPECT_EQ((unsigned short)COLOUR_ANSI, s->iFlags & COLOURTYPE);
    EXPECT_TRUE((s->iFlags & INVERSE) != 0);
}

// Interpret256ANSIcode, ANSI (index-storage) branch, background + INVERSE:
// a background 256 code (ESC[48;5;N) with INVERSE set stores the raw index in
// iForeColour (the swapped target) — ansi.cpp line 490 `iForeColour = iCode;`.
TEST(ANSITest, Interpret256AnsiBackgroundInverseStoresIndexInForeground)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.InterpretANSIcode(ANSI_INVERSE);            // COLOUR_ANSI | INVERSE
    doc.InterpretANSIcode(ANSI_BACK_256_COLOUR);    // -> HAVE_BACKGROUND_256_START
    doc.Interpret256ANSIcode(5);                    // 8-bit selector -> *_FINISH
    doc.Interpret256ANSIcode(46);                   // apply: bg-256 + INVERSE
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    // index-storage branch: the raw index lands in the FOREGROUND (swapped)
    EXPECT_EQ((COLORREF)46, s->iForeColour);
    // background index left untouched (it would become 46 if the arm swapped)
    EXPECT_EQ((COLORREF)BLACK, s->iBackColour);
    EXPECT_EQ((unsigned short)COLOUR_ANSI, s->iFlags & COLOURTYPE);
    EXPECT_TRUE((s->iFlags & INVERSE) != 0);
}

// Interpret256ANSIcode, custom->RGB conversion (ansi.cpp lines 399-404):
// a 256-colour code arriving while the current style is COLOUR_CUSTOM first
// resolves the custom entry to RGB (GetStyleRGB) and flips the style to
// COLOUR_RGB, so the xterm colour is applied through the RGB apply-path and the
// resolved custom background is preserved.
TEST(ANSITest, Interpret256FromCustomModeConvertsStyleToRgb)
{
    CMUSHclientDoc doc; make_doc(doc);
    // distinctive custom-palette entries so the assertion is deterministic
    doc.m_customtext[15] = RGB(11, 22, 33);
    doc.m_customback[15] = RGB(44, 55, 66);
    // seed a COLOUR_CUSTOM style (fore/back custom index 15)
    doc.AddStyle(COLOUR_CUSTOM, 15, 15, 0, static_cast<CAction*>(nullptr));
    ASSERT_EQ((unsigned short)COLOUR_CUSTOM, tail(doc)->iFlags & COLOURTYPE);

    doc.InterpretANSIcode(ANSI_TEXT_256_COLOUR);    // -> HAVE_FOREGROUND_256_START
    doc.Interpret256ANSIcode(5);                    // 8-bit selector -> *_FINISH
    doc.Interpret256ANSIcode(196);                  // apply foreground xterm 196

    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    // custom -> RGB conversion happened (line 402-403 cleared custom, set RGB)
    EXPECT_EQ((unsigned short)COLOUR_RGB, s->iFlags & COLOURTYPE);
    // foreground resolved through the RGB apply-path (xterm palette)
    EXPECT_EQ(xterm_256_colours[196], s->iForeColour);
    // background carried across from the resolved custom entry (GetStyleRGB ran)
    EXPECT_EQ(RGB(44, 55, 66), s->iBackColour);
}

MC_TEST_MAIN()
