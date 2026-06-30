// tests/test_ansi.cpp
// Unit tests for CMUSHclientDoc::InterpretANSIcode() and
// CMUSHclientDoc::Interpret256ANSIcode().
//
// Strategy: create a CMUSHclientDoc with a fresh CLine, feed it ANSI codes,
// then inspect the style(s) appended to the line's styleList.

#include "mc_test.h"
#include "platform/doc_stub.h"

#include <cstring>

// ── Test fixture helpers ───────────────────────────────────────────────────

// Build a doc with a current line ready to receive styles.
static CMUSHclientDoc make_doc()
{
    CMUSHclientDoc doc;
    doc.m_pCurrentLine = new CLine();
    // Seed the line with a plain-text style (mimics normal operation).
    doc.m_iFlags      = COLOUR_ANSI;
    doc.m_iForeColour = WHITE;
    doc.m_iBackColour = BLACK;
    // AddStyle creates the initial style on the line.
    doc.AddStyle(COLOUR_ANSI, WHITE, BLACK, 0,
                 static_cast<CAction*>(nullptr));
    return doc;
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
    CMUSHclientDoc doc = make_doc();
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((unsigned short)COLOUR_ANSI, s->iFlags & COLOURTYPE);
    EXPECT_EQ((COLORREF)WHITE, s->iForeColour);
    EXPECT_EQ((COLORREF)BLACK, s->iBackColour);
}

TEST(ANSITest, BoldSetsHiliteFlag)
{
    CMUSHclientDoc doc = make_doc();
    doc.InterpretANSIcode(ANSI_BOLD);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & HILITE) != 0);
}

TEST(ANSITest, CancelBoldClearsHiliteFlag)
{
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
    doc.InterpretANSIcode(ANSI_UNDERLINE);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & UNDERLINE) != 0);
}

TEST(ANSITest, InverseSetsInverseFlag)
{
    CMUSHclientDoc doc = make_doc();
    doc.InterpretANSIcode(ANSI_INVERSE);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & INVERSE) != 0);
}

TEST(ANSITest, ResetClearsAllFlags)
{
    CMUSHclientDoc doc = make_doc();
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

TEST(ANSITest, ForegroundRedSetsColourIndex)
{
    CMUSHclientDoc doc = make_doc();
    doc.InterpretANSIcode(ANSI_TEXT_RED);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    // ANSI red = colour index 1
    EXPECT_EQ((COLORREF)RED, s->iForeColour);
    EXPECT_EQ((unsigned short)COLOUR_ANSI, s->iFlags & COLOURTYPE);
}

TEST(ANSITest, ForegroundGreenSetsColourIndex)
{
    CMUSHclientDoc doc = make_doc();
    doc.InterpretANSIcode(ANSI_TEXT_GREEN);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)GREEN, s->iForeColour);
}

TEST(ANSITest, ForegroundBlueSetsColourIndex)
{
    CMUSHclientDoc doc = make_doc();
    doc.InterpretANSIcode(ANSI_TEXT_BLUE);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)BLUE, s->iForeColour);
}

TEST(ANSITest, ForegroundDefaultRestoresWhite)
{
    CMUSHclientDoc doc = make_doc();
    doc.InterpretANSIcode(ANSI_TEXT_RED);
    doc.InterpretANSIcode(ANSI_SET_FOREGROUND_DEFAULT);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)WHITE, s->iForeColour);
}

// ── Background colour codes ───────────────────────────────────────────────

TEST(ANSITest, BackgroundRedSetsColourIndex)
{
    CMUSHclientDoc doc = make_doc();
    doc.InterpretANSIcode(ANSI_BACK_RED);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)RED, s->iBackColour);
}

TEST(ANSITest, BackgroundDefaultRestoresBlack)
{
    CMUSHclientDoc doc = make_doc();
    doc.InterpretANSIcode(ANSI_BACK_RED);
    doc.InterpretANSIcode(ANSI_SET_BACKGROUND_DEFAULT);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)BLACK, s->iBackColour);
}

// ── 256-colour mode ───────────────────────────────────────────────────────

TEST(ANSITest, ForegroundEnter256Mode)
{
    CMUSHclientDoc doc = make_doc();
    // ESC[38;... → should transition to HAVE_FOREGROUND_256_START
    doc.InterpretANSIcode(ANSI_TEXT_256_COLOUR);
    EXPECT_EQ(HAVE_FOREGROUND_256_START, doc.m_phase);
}

TEST(ANSITest, BackgroundEnter256Mode)
{
    CMUSHclientDoc doc = make_doc();
    doc.InterpretANSIcode(ANSI_BACK_256_COLOUR);
    EXPECT_EQ(HAVE_BACKGROUND_256_START, doc.m_phase);
}

TEST(ANSITest, Foreground256ColourApplied)
{
    CMUSHclientDoc doc = make_doc();
    // ESC[38;5;196m — bright red in xterm palette (index 196).
    // When the current style is COLOUR_ANSI, the implementation stores the
    // raw index in iForeColour and keeps COLOUR_ANSI; GetStyleRGB then uses
    // xterm_256_colours[iForeColour] at render time (index >= 8 path).
    doc.InterpretANSIcode(ANSI_TEXT_256_COLOUR);   // → HAVE_FOREGROUND_256_START
    doc.Interpret256ANSIcode(5);                    // → HAVE_FOREGROUND_256_FINISH
    doc.Interpret256ANSIcode(196);                  // → apply colour, phase = DOING_CODE
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    // Index stored in iForeColour, type stays COLOUR_ANSI
    EXPECT_EQ((COLORREF)196, s->iForeColour);
    EXPECT_EQ((unsigned short)COLOUR_ANSI, s->iFlags & COLOURTYPE);
    // Phase advances to DOING_CODE (will be reset to NONE on next normal char)
    EXPECT_EQ(DOING_CODE, doc.m_phase);
}

TEST(ANSITest, Background256ColourApplied)
{
    CMUSHclientDoc doc = make_doc();
    // ESC[48;5;46m — xterm green (index 46), same ANSI-mode index storage rule.
    doc.InterpretANSIcode(ANSI_BACK_256_COLOUR);
    doc.Interpret256ANSIcode(5);
    doc.Interpret256ANSIcode(46);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)46, s->iBackColour);
    EXPECT_EQ((unsigned short)COLOUR_ANSI, s->iFlags & COLOURTYPE);
}

TEST(ANSITest, Foreground256ColourRgbLookup)
{
    // When GetStyleRGB is called on a style with COLOUR_ANSI and iForeColour >= 8,
    // it should resolve to xterm_256_colours[iForeColour].
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
    doc.InterpretANSIcode(ANSI_BOLD);
    doc.InterpretANSIcode(ANSI_TEXT_CYAN);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & HILITE) != 0);
    EXPECT_EQ((COLORREF)CYAN, s->iForeColour);
}

TEST(ANSITest, MultipleCodesCreateMultipleStyles)
{
    CMUSHclientDoc doc = make_doc();
    // Each InterpretANSIcode that changes anything appends a new style.
    int before = style_count(doc);
    doc.InterpretANSIcode(ANSI_TEXT_RED);
    doc.InterpretANSIcode(ANSI_BACK_BLUE);
    // At least as many styles as before + 2 changes
    EXPECT_GE(style_count(doc), before);
}

// ── GetStyleRGB ───────────────────────────────────────────────────────────

TEST(ANSITest, GetStyleRGBAnsiNormal)
{
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
    CStyle s;
    s.iFlags      = COLOUR_RGB;
    s.iForeColour = RGB(10, 20, 30);
    s.iBackColour = RGB(40, 50, 60);

    COLORREF fg = 0, bg = 0;
    doc.GetStyleRGB(&s, fg, bg);

    EXPECT_EQ(RGB(10, 20, 30), fg);
    EXPECT_EQ(RGB(40, 50, 60), bg);
}

TEST(ANSITest, GetStyleRGBInverseSwapsFgBg)
{
    CMUSHclientDoc doc = make_doc();
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
        CMUSHclientDoc doc = make_doc();
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
        CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
    doc.InterpretANSIcode(92);
    doc.InterpretANSIcode(ANSI_RESET);
    CStyle* s = tail(doc);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)WHITE, s->iForeColour);
    EXPECT_EQ((COLORREF)BLACK, s->iBackColour);
}

// ── DisplayMsg end-to-end ANSI parsing ───────────────────────────────────
// These tests drive DisplayMsg with raw byte strings that include ESC
// sequences and verify the resulting doc style state.  They catch the
// class of bug where the ESC phase was silently reset to NONE instead of
// transitioning to HAVE_ESC, causing sequences like "[31m" to be emitted
// literally to stdout.
//
// DisplayMsg writes to stdout; redirect it to /dev/null for the test.

#include <cstdio>

// RAII helper: redirect stdout to /dev/null for the duration of the object.
// The original stdout fd is dup'd first so it can be restored cleanly even
// in headless / CI environments where /dev/tty is unavailable.
#include <unistd.h>
struct SilenceStdout {
    int saved_fd = -1;
    SilenceStdout() {
        fflush(stdout);
        saved_fd = dup(STDOUT_FILENO);
        freopen("/dev/null", "w", stdout);
    }
    ~SilenceStdout() {
        if (saved_fd >= 0) {
            fflush(stdout);
            dup2(saved_fd, STDOUT_FILENO);
            close(saved_fd);
        }
    }
};

TEST(ANSITest, DisplayMsg_ANSIRedSetsStyle)
{
    SilenceStdout _s;
    CMUSHclientDoc doc = make_doc();
    // "\033[31m" — ESC [ 3 1 m → foreground red
    const char seq[] = "\033[31mHello";
    doc.DisplayMsg(seq, (int)strlen(seq), 0, false);
    EXPECT_EQ((COLORREF)RED, doc.m_iForeColour);
}

TEST(ANSITest, DisplayMsg_BrightGreenSetsIndex10)
{
    SilenceStdout _s;
    CMUSHclientDoc doc = make_doc();
    const char seq[] = "\033[92mHello";
    doc.DisplayMsg(seq, (int)strlen(seq), 0, false);
    EXPECT_EQ((COLORREF)10, doc.m_iForeColour);
}

TEST(ANSITest, DisplayMsg_ResetRestoresDefaults)
{
    SilenceStdout _s;
    CMUSHclientDoc doc = make_doc();
    const char seq[] = "\033[31m\033[0mHello";
    doc.DisplayMsg(seq, (int)strlen(seq), 0, false);
    EXPECT_EQ((COLORREF)WHITE, doc.m_iForeColour);
    EXPECT_EQ((COLORREF)BLACK, doc.m_iBackColour);
}

TEST(ANSITest, DisplayMsg_MultiParamSequence)
{
    SilenceStdout _s;
    CMUSHclientDoc doc = make_doc();
    // "\033[1;34m" — bold + blue
    const char seq[] = "\033[1;34mHello";
    doc.DisplayMsg(seq, (int)strlen(seq), 0, false);
    EXPECT_EQ((COLORREF)BLUE, doc.m_iForeColour);
    EXPECT_TRUE((doc.m_iFlags & HILITE) != 0);
}

TEST(ANSITest, DisplayMsg_ESCInterruptsMXPStateCleanly)
{
    // If an ESC arrives while the phase is in a non-NONE state (e.g. after
    // a partial MXP tag was started), the phase must transition to HAVE_ESC
    // not stay stuck or go to NONE, and the subsequent ANSI sequence must
    // parse correctly.
    SilenceStdout _s;
    CMUSHclientDoc doc = make_doc();
    // Force a non-NONE phase then deliver an ESC sequence.
    doc.m_phase = HAVE_ESC;   // simulate being mid-other-sequence
    const char seq[] = "\033[32mHello";
    doc.DisplayMsg(seq, (int)strlen(seq), 0, false);
    EXPECT_EQ((COLORREF)GREEN, doc.m_iForeColour);
}

MC_TEST_MAIN()
