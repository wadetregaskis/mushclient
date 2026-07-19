// tests/test_display.cpp
// Intent-comment tests for the shared display pipeline (doc_shared.cpp
// DisplayMsg / AddToLine / StartNewLine, campaign-b display step 7) and the
// CLine invariants (Line.cpp).  These pin behaviour called out in source
// comments: CR-clears-line, split UTF-8 buffering, embedded NULs, note/line
// type stamping, the JUMP_SIZE line-position index, auto-wrap (UTF-8 and
// word wrap), and style-run splitting across a wrap.

#include "mc_test.h"
#include "platform/doc_stub.h"

#include <cstring>
#include <string>

// ── helpers ───────────────────────────────────────────────────────────────

static CMUSHclientDoc make_doc()
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    return doc;
}

static void feed(CMUSHclientDoc& doc, const char* s)
{
    doc.DisplayMsg(s, (int)strlen(s), 0);
}

static std::string line_text(CLine* l)
{
    return std::string(l->text, l->len);
}

// 0-based line from the head of m_LineList.
static CLine* nth_line(CMUSHclientDoc& doc, int idx)
{
    POSITION pos = doc.m_LineList.GetHeadPosition();
    for (int i = 0; pos; i++) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (i == idx) return l;
    }
    return nullptr;
}

// The style covering the (0-based) character index of a line.
static CStyle* style_at(CLine* line, int idx)
{
    int off = 0;
    for (POSITION pos = line->styleList.GetHeadPosition(); pos; ) {
        CStyle* s = line->styleList.GetNext(pos);
        if (s->iLength > 0 && idx < off + s->iLength) return s;
        off += s->iLength;
    }
    return nullptr;
}

static int style_length_sum(CLine* line)
{
    int sum = 0;
    for (POSITION pos = line->styleList.GetHeadPosition(); pos; )
        sum += line->styleList.GetNext(pos)->iLength;
    return sum;
}

// ── DisplayMsg behaviour ──────────────────────────────────────────────────

// doc_shared.cpp '\r' case: with m_bCarriageReturnClearsLine, a CR *not*
// followed by LF wipes the pending line's text and resets its style list to
// one default style; a normal CRLF pair does not clear.
TEST(DisplayTest, CarriageReturnClearsPendingLineWhenEnabled)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bCarriageReturnClearsLine = TRUE;

    feed(doc, "abc\rdef\n");
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(line_text(l0) == "def");     // "abc" wiped by the bare CR

    feed(doc, "xyz\r\n");                    // CRLF pair: no clearing
    CLine* l1 = nth_line(doc, 1);
    ASSERT_NE(nullptr, l1);
    EXPECT_TRUE(line_text(l1) == "xyz");
}

TEST(DisplayTest, CarriageReturnIgnoredWhenOptionOff)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bCarriageReturnClearsLine = FALSE;
    feed(doc, "abc\rdef\n");
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(line_text(l0) == "abcdef");  // CR simply ignored
}

// doc_shared.cpp: "do not display UTF-8 characters until they have
// completely arrived" — a character split across two packets is buffered
// and rendered exactly once when its last byte arrives.
TEST(DisplayTest, Utf8CharacterSplitAcrossPacketsRendersOnce)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bUTF_8 = TRUE;

    feed(doc, "a\xC3");                          // lead byte only
    EXPECT_EQ(HAVE_UTF8_CHARACTER, doc.m_phase); // buffered, not displayed
    ASSERT_TRUE(doc.m_pCurrentLine != NULL);
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "a");  // no partial bytes

    feed(doc, "\xA9");                           // continuation completes é
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "a\xC3\xA9");
}

// doc_shared.cpp: "decompressed data has a size, not a null terminator.
// Also, compressed data may have imbedded nulls." — DisplayMsg processes
// exactly `size` bytes; text after an embedded NUL is still displayed.
TEST(DisplayTest, BytesAfterEmbeddedNulStillDisplayed)
{
    CMUSHclientDoc doc = make_doc();
    const char pkt[] = { 'a', 'b', '\0', 'c', 'd', '\n' };
    doc.DisplayMsg(pkt, 6, 0);
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(line_text(l0) == "abcd");    // "cd" survived the NUL
}

// doc_shared.cpp: "make sure notes start on a new line" — a Note issued
// while a partial MUD line (prompt without newline) is pending finalises the
// prompt line and starts its own fresh line.
TEST(DisplayTest, NoteStartsOnFreshLineAfterPartialMudLine)
{
    CMUSHclientDoc doc = make_doc();
    feed(doc, "prompt> ");                   // partial MUD line, no '\n'
    doc.Note("hello");

    CLine* l0 = nth_line(doc, 0);
    CLine* l1 = nth_line(doc, 1);
    ASSERT_NE(nullptr, l0);
    ASSERT_NE(nullptr, l1);
    EXPECT_TRUE(line_text(l0) == "prompt> ");        // prompt finalised
    EXPECT_TRUE((l0->flags & COMMENT) == 0);
    EXPECT_TRUE(line_text(l1) == "hello");           // note on its own line
    EXPECT_TRUE((l1->flags & COMMENT) != 0);
}

// doc_shared.cpp SetNewLineColour: "*This* is the logical spot to do it - on
// the first character for the new line" — line type flags are stamped when
// the first character arrives, so MUD output following an echoed user
// command is not mis-flagged as user input.
TEST(DisplayTest, LineTypeStampedOnFirstCharacterNotAtCreation)
{
    CMUSHclientDoc doc = make_doc();
    doc.DisplayMsg("look\n", 5, USER_INPUT);   // echoed user command
    feed(doc, "You see nothing.\n");           // MUD output follows

    CLine* l0 = nth_line(doc, 0);
    CLine* l1 = nth_line(doc, 1);
    ASSERT_NE(nullptr, l0);
    ASSERT_NE(nullptr, l1);
    EXPECT_TRUE(line_text(l0) == "look");
    EXPECT_TRUE((l0->flags & USER_INPUT) != 0);
    EXPECT_TRUE(line_text(l1) == "You see nothing.");
    EXPECT_TRUE((l1->flags & USER_INPUT) == 0);   // NOT user input
    EXPECT_TRUE((l1->flags & COMMENT) == 0);
}

// doc_shared.cpp StartNewLine: "we match the modulus on 1, not zero ..." —
// the JUMP_SIZE line-position index stays consistent, so GetLineInfo(n,1)
// returns the right text for lines beyond the first JUMP_SIZE (=100).
TEST(DisplayTest, GetLineInfoCorrectAcrossJumpIndexBoundary)
{
    CMUSHclientDoc doc = make_doc();
    char buf[32];
    for (int i = 1; i <= 150; i++) {
        snprintf(buf, sizeof buf, "line %d\n", i);
        feed(doc, buf);
    }

    struct { long n; const char* expect; } cases[] = {
        { 1,   "line 1"   },
        { 100, "line 100" },
        { 101, "line 101" },   // first line beyond the JUMP boundary
        { 150, "line 150" },
    };
    for (auto& tc : cases) {
        VARIANT v = doc.GetLineInfo(tc.n, 1);
        ASSERT_EQ((unsigned short)VT_BSTR, v.vt);
        EXPECT_STREQ(tc.expect, (const char*)CString(v.bstrVal));
        VariantClear(&v);
    }
}

// doc.h:1234 (m_iFlags/m_iForeColour/m_iBackColour): "we save the current
// style here on any style change *from the mud* - we don't want to mix up
// notes/user input with mud-set styles."
TEST(DisplayTest, RememberedMudStyleUnaffectedByNoteColours)
{
    CMUSHclientDoc doc = make_doc();
    feed(doc, "\033[31mred");                  // MUD sets red
    EXPECT_EQ((COLORREF)RED, doc.m_iForeColour);

    doc.m_iNoteTextColour = 5;                 // notes in a custom colour
    doc.Note("a note");
    EXPECT_EQ((COLORREF)RED, doc.m_iForeColour);   // MUD style undisturbed

    feed(doc, "more\n");                       // MUD text resumes...
    CLine* l = nullptr;
    for (int i = 0; nth_line(doc, i); i++)     // find the "more" line
        if (line_text(nth_line(doc, i)) == "more") l = nth_line(doc, i);
    ASSERT_NE(nullptr, l);
    CStyle* s = style_at(l, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)RED, s->iForeColour);  // ...in the MUD-set colour
    EXPECT_EQ((unsigned short)COLOUR_ANSI, s->iFlags & COLOURTYPE);
}

// ── Auto-wrap ─────────────────────────────────────────────────────────────

// doc_shared.cpp AddToLine: "for Unicode the width of the line is
// characters, not stored bytes" — a UTF-8 world wraps at m_nWrapColumn
// CHARACTERS, so multi-byte text wraps at the same visual width as ASCII.
TEST(WrapTest, Utf8WrapColumnCountsCharactersNotBytes)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bUTF_8 = TRUE;
    doc.m_nWrapColumn = 10;

    std::string s;
    for (int i = 0; i < 12; i++) s += "\xC3\xA9";   // 12 × é (2 bytes each)
    s += "\n";
    doc.DisplayMsg(s.c_str(), (int)s.size(), 0);

    CLine* l0 = nth_line(doc, 0);
    CLine* l1 = nth_line(doc, 1);
    ASSERT_NE(nullptr, l0);
    ASSERT_NE(nullptr, l1);
    EXPECT_EQ(20, l0->len);                  // 10 characters = 20 bytes
    EXPECT_EQ(4,  l1->len);                  // remaining 2 characters
    EXPECT_FALSE(l0->hard_return);           // soft (wrap) break
    EXPECT_TRUE(l1->hard_return);            // real newline
}

// doc_shared.cpp AddToLine: "any character starting 10xxxxxx will *not* be
// the start of a character ... we do *not* want to split it by putting it
// onto a new line" — a multi-byte character reaching the wrap column moves
// to the new line intact.
TEST(WrapTest, WrapNeverSplitsUtf8Sequence)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bUTF_8 = TRUE;
    doc.m_nWrapColumn = 10;

    // 9 ASCII chars, then é arriving at the wrap column, then more text
    feed(doc, "aaaaaaaaa\xC3\xA9""bb\n");

    CLine* l0 = nth_line(doc, 0);
    CLine* l1 = nth_line(doc, 1);
    ASSERT_NE(nullptr, l0);
    ASSERT_NE(nullptr, l1);
    // é stayed together (its continuation byte never begins a line)
    EXPECT_EQ(11, l0->len);                          // "aaaaaaaaa" + C3 A9
    EXPECT_EQ((char)0xC3, l0->text[9]);
    EXPECT_EQ((char)0xA9, l0->text[10]);
    EXPECT_TRUE(line_text(l1) == "bb");
    EXPECT_TRUE(((unsigned char)l1->text[0] & 0xC0) != 0x80);
}

// doc_shared.cpp AddToLine word wrap: with !m_indent_paras the space stays
// at the end of the first line ("one more on this line (the space)") and
// the whole partial word moves to the continuation line — no characters
// lost or duplicated.
TEST(WrapTest, WordMovesIntactToContinuationLine)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_wrap = 1;
    doc.m_indent_paras = 0;
    doc.m_nWrapColumn = 10;

    feed(doc, "aaa bbb cccc\n");

    CLine* l0 = nth_line(doc, 0);
    CLine* l1 = nth_line(doc, 1);
    ASSERT_NE(nullptr, l0);
    ASSERT_NE(nullptr, l1);
    EXPECT_TRUE(line_text(l0) == "aaa bbb ");   // space kept on first line
    EXPECT_TRUE(line_text(l1) == "cccc");       // word moved whole
    EXPECT_TRUE(line_text(l0) + line_text(l1) == "aaa bbb cccc");
}

// doc_shared.cpp AddToLine: "if one style is shared we have to make a copy
// and adjust lengths" — when a wrap splits inside a coloured run, both
// lines' style lengths still sum to their text length and the continuation
// keeps the run's colour.
TEST(WrapTest, StyleRunSplitAcrossWrapKeepsLengthsAndColours)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_wrap = 1;
    doc.m_indent_paras = 0;
    doc.m_nWrapColumn = 10;

    feed(doc, "aa \033[31mbbbb cccc\n");   // red run spans the wrap point

    CLine* l0 = nth_line(doc, 0);
    CLine* l1 = nth_line(doc, 1);
    ASSERT_NE(nullptr, l0);
    ASSERT_NE(nullptr, l1);
    EXPECT_TRUE(line_text(l0) == "aa bbbb ");
    EXPECT_TRUE(line_text(l1) == "cccc");

    // style lengths sum exactly to each line's len — nothing lost in the split
    EXPECT_EQ(l0->len, style_length_sum(l0));
    EXPECT_EQ(l1->len, style_length_sum(l1));

    // the split run keeps its colour on both sides
    CStyle* s0 = style_at(l0, 4);            // inside "bbbb"
    CStyle* s1 = style_at(l1, 0);            // continuation "cccc"
    ASSERT_NE(nullptr, s0);
    ASSERT_NE(nullptr, s1);
    EXPECT_EQ((COLORREF)RED, s0->iForeColour);
    EXPECT_EQ((COLORREF)RED, s1->iForeColour);
    // and the first line still starts un-coloured
    CStyle* sHead = style_at(l0, 0);
    ASSERT_NE(nullptr, sHead);
    EXPECT_EQ((COLORREF)WHITE, sHead->iForeColour);
}

// ── CLine construction invariants ────────────────────────────────────────

// Line.cpp ctor: "have at least one style item in the list" (AddToLine /
// SetNewLineColour call styleList.GetTail() unchecked) and "allocate 4 bytes
// per character for UTF-8".
TEST(LineTest, NewLineSeededWithOneStyleAndUtf8SizedBuffer)
{
    CLine ansi(1, 80, COLOUR_ANSI, RED, BLUE, false);
    EXPECT_EQ(1, (int)ansi.styleList.GetCount());
    CStyle* s = ansi.styleList.GetTail();
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((COLORREF)RED,  s->iForeColour);
    EXPECT_EQ((COLORREF)BLUE, s->iBackColour);
    EXPECT_EQ(0, s->iLength);
    EXPECT_EQ(80, ansi.iMemoryAllocated);

    CLine utf8(2, 80, COLOUR_ANSI, RED, BLUE, true);
    EXPECT_EQ(1, (int)utf8.styleList.GetCount());
    EXPECT_EQ(320, utf8.iMemoryAllocated);   // 4 bytes per character
}

// ═══════════════════════════════════════════════════════════════════════════
// methods_noting.cpp — Note/Tell/ColourNote/AnsiNote/Hyperlink/NoteHr and the
// note-colour accessor family.  Headless the doc starts in RGB-notes mode
// with silver-on-black (doc_stub.h), matching a fresh Windows world.

// last (0-based) closed line index — count-2 because the tail is the
// in-progress current line.
static CLine* closed_tail(CMUSHclientDoc& doc)
{
    int n = (int)doc.m_LineList.GetCount();
    return n >= 2 ? nth_line(doc, n - 2) : nullptr;
}

TEST(NotingTest, NoteClosesLineAndStampsCommentFlag)
{
    CMUSHclientDoc doc = make_doc();
    doc.Note("hello");
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(line_text(l0) == "hello");
    EXPECT_EQ(COMMENT, l0->flags & NOTE_OR_COMMAND);
    // the newline was appended: a fresh current line is now the tail
    EXPECT_TRUE(doc.m_pCurrentLine != l0);
    EXPECT_EQ(0, doc.m_pCurrentLine->len);
}

TEST(NotingTest, TellAppendsWithoutClosingLine)
{
    CMUSHclientDoc doc = make_doc();
    doc.Tell("abc");
    doc.Tell("def");
    EXPECT_TRUE(doc.m_pCurrentLine != nullptr);
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "abcdef");
    EXPECT_EQ(COMMENT, doc.m_pCurrentLine->flags & NOTE_OR_COMMAND);
    EXPECT_EQ(1, (int)doc.m_LineList.GetCount());   // still the one open line
}

TEST(NotingTest, TellOnPartialMudLineForcesLineChange)
{
    // a note must not continue the pending MUD-output line (colour bleed)
    CMUSHclientDoc doc = make_doc();
    feed(doc, "mud");                    // partial output line, flags 0
    doc.Tell("note");
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(line_text(l0) == "mud");     // closed early
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "note");
    EXPECT_EQ(COMMENT, doc.m_pCurrentLine->flags & NOTE_OR_COMMAND);
}

TEST(NotingTest, TellEmptyMessageIsNoop)
{
    CMUSHclientDoc doc = make_doc();
    int before = (int)doc.m_LineList.GetCount();
    doc.Tell("");
    EXPECT_EQ(before, (int)doc.m_LineList.GetCount());
    EXPECT_EQ(0, doc.m_pCurrentLine->len);
}

TEST(NotingTest, NoteDeferredWhileNotesNotWantedThenDrained)
{
    // mid-telnet-negotiation (m_bNotesNotWantedNow) a note may not be stuck
    // inside the partial MUD line: it is queued with its colours...
    CMUSHclientDoc doc = make_doc();
    feed(doc, "partial");
    doc.m_bNotesNotWantedNow = true;
    doc.NoteColourRGB(0x123456, 0x654321);
    doc.Note("deferred");
    ASSERT_EQ((size_t)1, doc.m_OutstandingLines.size());
    EXPECT_EQ((COLORREF)0x123456, doc.m_OutstandingLines.front().m_cText);
    EXPECT_EQ((COLORREF)0x654321, doc.m_OutstandingLines.front().m_cBack);
    EXPECT_EQ(0, strncmp(doc.m_OutstandingLines.front().m_sText.c_str(),
                         "deferred", 8));

    // ...and replayed when the MUD line completes (ProcessPreviousLine's
    // trailing OutputOutstandingLines).  NOTE: calling OutputOutstandingLines
    // directly with a partial MUD line pending would re-enter itself via
    // Tell→DisplayMsg→ProcessPreviousLine and clear the list mid-iteration
    // (latent hazard — unreachable from the shipping call sites, which only
    // drain when the current line is empty/fresh).
    doc.m_bNotesNotWantedNow = false;
    feed(doc, " done\n");
    EXPECT_TRUE(doc.m_OutstandingLines.empty());
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(line_text(l0) == "partial done");
    CLine* note_line = nth_line(doc, 1);
    ASSERT_NE(nullptr, note_line);
    EXPECT_TRUE(line_text(note_line) == "deferred");
    EXPECT_EQ(COMMENT, note_line->flags & NOTE_OR_COMMAND);
    CStyle* s = style_at(note_line, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_EQ((COLORREF)0x123456, s->iForeColour);
    EXPECT_EQ((COLORREF)0x654321, s->iBackColour);
}

TEST(NotingTest, NoteRgbStampsRgbStyle)
{
    CMUSHclientDoc doc = make_doc();
    // NoteColourRGB masks off the high byte (COLORREF is 24-bit)
    doc.NoteColourRGB(0xFF123456, 0x00654321);
    doc.Note("x");
    CStyle* s = style_at(nth_line(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_EQ((COLORREF)0x123456, s->iForeColour);
    EXPECT_EQ((COLORREF)0x654321, s->iBackColour);
}

TEST(NotingTest, NoteCustomPaletteStampsIndexOnBlack)
{
    CMUSHclientDoc doc = make_doc();
    doc.SetNoteColour(5);                    // one-based → palette index 4
    EXPECT_EQ(5, doc.GetNoteColour());
    doc.Note("x");
    CStyle* s = style_at(nth_line(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_CUSTOM, s->iFlags & COLOURTYPE);
    EXPECT_EQ((COLORREF)4, s->iForeColour);  // palette INDEX, not RGB
    EXPECT_EQ((COLORREF)BLACK, s->iBackColour);
}

TEST(NotingTest, NoteSameColourStampsAnsiWhiteOnBlack)
{
    CMUSHclientDoc doc = make_doc();
    doc.SetNoteColour(0);                    // 0 = "same colour" sentinel
    EXPECT_EQ(0, doc.GetNoteColour());
    doc.Note("x");
    CStyle* s = style_at(nth_line(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_ANSI, s->iFlags & COLOURTYPE);
    EXPECT_EQ((COLORREF)WHITE, s->iForeColour);
    EXPECT_EQ((COLORREF)BLACK, s->iBackColour);
}

TEST(NotingTest, SetNoteColourRejectsOutOfRange)
{
    CMUSHclientDoc doc = make_doc();
    doc.SetNoteColour(5);
    doc.SetNoteColour(-1);                   // below range: ignored
    EXPECT_EQ(5, doc.GetNoteColour());
    doc.SetNoteColour(MAX_CUSTOM + 1);       // above range: ignored
    EXPECT_EQ(5, doc.GetNoteColour());
}

TEST(NotingTest, GetNoteColourForeBackPerMode)
{
    CMUSHclientDoc doc = make_doc();
    // RGB mode: the stored RGB values
    doc.NoteColourRGB(0x00000A, 0x00000B);
    EXPECT_EQ(0x0AL, doc.GetNoteColourFore());
    EXPECT_EQ(0x0BL, doc.GetNoteColourBack());
    // custom palette mode: that palette entry's text/back
    doc.m_customtext[2] = 0x111111; doc.m_customback[2] = 0x222222;
    doc.SetNoteColour(3);
    EXPECT_EQ(0x111111L, doc.GetNoteColourFore());
    EXPECT_EQ(0x222222L, doc.GetNoteColourBack());
    // "same colour": ANSI white-on-black palette...
    doc.SetNoteColour(0);
    doc.m_bCustom16isDefaultColour = FALSE;
    doc.m_normalcolour[WHITE] = 0x333333; doc.m_normalcolour[BLACK] = 0x444444;
    EXPECT_EQ(0x333333L, doc.GetNoteColourFore());
    EXPECT_EQ(0x444444L, doc.GetNoteColourBack());
    // ...unless custom-16 is the default colour
    doc.m_bCustom16isDefaultColour = TRUE;
    doc.m_customtext[15] = 0x555555; doc.m_customback[15] = 0x666666;
    EXPECT_EQ(0x555555L, doc.GetNoteColourFore());
    EXPECT_EQ(0x666666L, doc.GetNoteColourBack());
}

TEST(NotingTest, SetNoteColourForeConvertsPaletteBackgroundFirst)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_customback[2] = 0x445566;
    doc.SetNoteColour(3);                    // palette mode, index 2
    doc.SetNoteColourFore(0xABCDEF12);       // masked to 24 bits
    EXPECT_TRUE(doc.m_bNotesInRGB);
    EXPECT_EQ(0xCDEF12L, doc.GetNoteColourFore());
    EXPECT_EQ(0x445566L, doc.GetNoteColourBack());  // palette back preserved
}

TEST(NotingTest, SetNoteColourBackConvertsSameColourForegroundFirst)
{
    CMUSHclientDoc doc = make_doc();
    doc.SetNoteColour(0);                    // "same colour"
    doc.m_bCustom16isDefaultColour = FALSE;
    doc.m_normalcolour[WHITE] = 0x777777;
    doc.SetNoteColourBack(0x000123);
    EXPECT_TRUE(doc.m_bNotesInRGB);
    EXPECT_EQ(0x777777L, doc.GetNoteColourFore());  // white carried over
    EXPECT_EQ(0x000123L, doc.GetNoteColourBack());
}

TEST(NotingTest, NoteColourNameSetsRgbFromNamesAndHex)
{
    CMUSHclientDoc doc = make_doc();
    doc.NoteColourName("red", "blue");
    EXPECT_TRUE(doc.m_bNotesInRGB);
    EXPECT_EQ((long)RGB(255, 0, 0), doc.GetNoteColourFore());
    EXPECT_EQ((long)RGB(0, 0, 255), doc.GetNoteColourBack());
    doc.NoteColourName("#102030", "#405060");
    EXPECT_EQ((long)RGB(0x10, 0x20, 0x30), doc.GetNoteColourFore());
    EXPECT_EQ((long)RGB(0x40, 0x50, 0x60), doc.GetNoteColourBack());
    // empty/unknown names leave the colour untouched
    doc.NoteColourName("", "no-such-colour-name");
    EXPECT_EQ((long)RGB(0x10, 0x20, 0x30), doc.GetNoteColourFore());
    EXPECT_EQ((long)RGB(0x40, 0x50, 0x60), doc.GetNoteColourBack());
}

TEST(NotingTest, ColourTellStampsRequestedColoursAndRestores)
{
    CMUSHclientDoc doc = make_doc();
    COLORREF old_fore = doc.m_iNoteColourFore, old_back = doc.m_iNoteColourBack;
    doc.ColourTell("red", "blue", "hi");
    CStyle* s = style_at(doc.m_pCurrentLine, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_EQ(RGB(255, 0, 0), s->iForeColour);
    EXPECT_EQ(RGB(0, 0, 255), s->iBackColour);
    // colours restored afterwards (was RGB mode already)
    EXPECT_EQ(old_fore, doc.m_iNoteColourFore);
    EXPECT_EQ(old_back, doc.m_iNoteColourBack);
}

TEST(NotingTest, ColourTellFromPaletteModeRestoresPaletteMode)
{
    CMUSHclientDoc doc = make_doc();
    doc.SetNoteColour(2);
    EXPECT_FALSE(doc.m_bNotesInRGB);
    doc.ColourTell("red", "blue", "hi");
    EXPECT_FALSE(doc.m_bNotesInRGB);        // back to palette mode
    EXPECT_EQ(2, doc.GetNoteColour());
}

TEST(NotingTest, ColourNoteClosesTheLine)
{
    CMUSHclientDoc doc = make_doc();
    doc.ColourNote("red", "blue", "x");
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(line_text(l0) == "x");
    EXPECT_EQ(COMMENT, l0->flags & NOTE_OR_COMMAND);
    EXPECT_EQ(0, doc.m_pCurrentLine->len);   // newline appended
}

TEST(NotingTest, NoteStyleMasksToTextStyleBits)
{
    CMUSHclientDoc doc = make_doc();
    doc.NoteStyle(HILITE | UNDERLINE);
    EXPECT_EQ(HILITE | UNDERLINE, doc.GetNoteStyle());
    doc.NoteStyle((short)0xFF);              // out-of-range bits dropped
    EXPECT_EQ(0xFF & TEXT_STYLE, doc.GetNoteStyle());
    // and the style bits land on the note's style run
    doc.NoteStyle(UNDERLINE | INVERSE);
    doc.Note("x");
    CStyle* s = style_at(nth_line(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(UNDERLINE | INVERSE, s->iFlags & TEXT_STYLE);
}

TEST(NotingTest, HyperlinkSendStampsActionAndUnderline)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_iHyperlinkColour = 0x0000EE;
    doc.Hyperlink("do this", "click", "a hint", "", "", FALSE);
    CLine* l = doc.m_pCurrentLine;
    ASSERT_NE(nullptr, l);
    EXPECT_TRUE(line_text(l) == "click");
    CStyle* s = style_at(l, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(COLOUR_RGB, s->iFlags & COLOURTYPE);
    EXPECT_TRUE((s->iFlags & ACTION_SEND) != 0);
    EXPECT_FALSE((s->iFlags & ACTION_HYPERLINK) != 0);
    EXPECT_TRUE((s->iFlags & UNDERLINE) != 0);
    EXPECT_EQ((COLORREF)0x0000EE, s->iForeColour);  // empty colour → hyperlink colour
    ASSERT_TRUE(s->pAction != NULL);
    EXPECT_STREQ("do this", (const char*)s->pAction->m_strAction);
    EXPECT_STREQ("a hint",  (const char*)s->pAction->m_strHint);
}

TEST(NotingTest, HyperlinkUrlUsesActionAsTextAndHintFallback)
{
    CMUSHclientDoc doc = make_doc();
    doc.Hyperlink("http://x", "", "", "", "", TRUE);
    CLine* l = doc.m_pCurrentLine;
    ASSERT_NE(nullptr, l);
    EXPECT_TRUE(line_text(l) == "http://x");   // empty Text → Action shown
    CStyle* s = style_at(l, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & ACTION_HYPERLINK) != 0);
    EXPECT_FALSE((s->iFlags & ACTION_SEND) != 0);
    ASSERT_TRUE(s->pAction != NULL);
    EXPECT_STREQ("http://x", (const char*)s->pAction->m_strHint);  // hint falls back to action
}

TEST(NotingTest, HyperlinkEmptyActionIsNoop)
{
    CMUSHclientDoc doc = make_doc();
    int lines_before = (int)doc.m_LineList.GetCount();
    doc.Hyperlink("", "text", "hint", "", "", FALSE);
    EXPECT_EQ(lines_before, (int)doc.m_LineList.GetCount());
    EXPECT_EQ(0, doc.m_pCurrentLine->len);
}

TEST(NotingTest, NoteHrEmitsHorizRuleLine)
{
    CMUSHclientDoc doc = make_doc();
    feed(doc, "abc");                        // pending partial line
    doc.NoteHr();
    // line 0 = "abc" (wrapped up), line 1 = the HR, line 2 = new current
    CLine* l0 = nth_line(doc, 0);
    CLine* l1 = nth_line(doc, 1);
    ASSERT_NE(nullptr, l0);
    ASSERT_NE(nullptr, l1);
    EXPECT_TRUE(line_text(l0) == "abc");
    EXPECT_EQ(HORIZ_RULE, l1->flags);
    EXPECT_TRUE(doc.m_pCurrentLine != l1);
}

// ── Logging: OpenLog/WriteLog/CloseLog + the note-logging pipeline ───────
// (methods_logging.cpp, doc_shared OpenLog/CloseLog, and the
// ProcessPreviousLine note-logging / LogLineInHTMLcolour paths.)

#include <cstdio>
#include <cstdlib>

static std::string fresh_log_path()
{
    static int counter = 0;
    const char* tmp = getenv("TMPDIR");
    char buf[512];
    snprintf(buf, sizeof buf, "%s/mc_test_log_%d_%d.txt",
             tmp ? tmp : "/tmp", (int)getpid(), ++counter);
    remove(buf);
    return buf;
}

static std::string slurp(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "<no file>";
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static bool log_contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}

TEST(LoggingTest, OpenWriteFlushCloseLifecycle)
{
    CMUSHclientDoc doc = make_doc();
    std::string path = fresh_log_path();
    EXPECT_EQ(0L, doc.OpenLog(path.c_str(), FALSE));       // eOK
    EXPECT_TRUE(doc.IsLogOpen());
    EXPECT_EQ(30015L, doc.OpenLog(path.c_str(), FALSE));   // eLogFileAlreadyOpen
    EXPECT_EQ(0L, doc.WriteLog("hello"));                  // newline appended
    EXPECT_EQ(0L, doc.FlushLog());
    EXPECT_TRUE(log_contains(slurp(path), "hello\n"));
    EXPECT_EQ(0L, doc.CloseLog());
    EXPECT_FALSE(doc.IsLogOpen());
    EXPECT_EQ(30014L, doc.CloseLog());                     // eLogFileNotOpen
    EXPECT_EQ(30014L, doc.WriteLog("x"));
    EXPECT_EQ(30014L, doc.FlushLog());
    remove(path.c_str());
}

TEST(LoggingTest, OpenLogBadPathReturnsCouldNotOpenFile)
{
    CMUSHclientDoc doc = make_doc();
    EXPECT_EQ(30013L, doc.OpenLog("/nonexistent_mc_dir/x.log", FALSE));  // eCouldNotOpenFile
    EXPECT_FALSE(doc.IsLogOpen());
}

TEST(LoggingTest, NoteLoggedWithPreambleAndPostamble)
{
    CMUSHclientDoc doc = make_doc();
    std::string path = fresh_log_path();
    ASSERT_EQ(0L, doc.OpenLog(path.c_str(), FALSE));
    doc.m_bLogNotes = TRUE;
    doc.m_strLogLinePreambleNotes  = "PRE:";
    doc.m_strLogLinePostambleNotes = ":POST";
    doc.Note("logged note");
    doc.CloseLog();
    std::string log = slurp(path);
    EXPECT_TRUE(log_contains(log, "PRE:logged note:POST\n"));
    remove(path.c_str());
}

TEST(LoggingTest, NoteLoggedAsHtmlEscapesMarkup)
{
    CMUSHclientDoc doc = make_doc();
    std::string path = fresh_log_path();
    ASSERT_EQ(0L, doc.OpenLog(path.c_str(), FALSE));
    doc.m_bLogNotes = TRUE;
    doc.m_bLogHTML  = TRUE;
    doc.Note("a<b>&c");
    doc.CloseLog();
    std::string log = slurp(path);
    EXPECT_TRUE(log_contains(log, "a&lt;b&gt;&amp;c"));
    remove(path.c_str());
}

TEST(LoggingTest, NoteLoggedInHtmlColourEmitsFontSpanAndUnderline)
{
    CMUSHclientDoc doc = make_doc();
    std::string path = fresh_log_path();
    ASSERT_EQ(0L, doc.OpenLog(path.c_str(), FALSE));
    doc.m_bLogNotes    = TRUE;
    doc.m_bLogHTML     = TRUE;
    doc.m_bLogInColour = TRUE;
    doc.NoteColourRGB(RGB(0xAB, 0xCD, 0xEF), RGB(0x11, 0x22, 0x33));
    doc.NoteStyle(UNDERLINE);
    doc.Note("shiny");
    doc.CloseLog();
    std::string log = slurp(path);
    EXPECT_TRUE(log_contains(log, "<font color=\"#ABCDEF\">"));
    // non-black background → span with both colours
    EXPECT_TRUE(log_contains(log, "background: #112233"));
    EXPECT_TRUE(log_contains(log, "<u>shiny</u>"));
    EXPECT_TRUE(log_contains(log, "</font>"));
    remove(path.c_str());
}

TEST(LoggingTest, LogRawSuppressesNotePreambleFormatting)
{
    CMUSHclientDoc doc = make_doc();
    std::string path = fresh_log_path();
    ASSERT_EQ(0L, doc.OpenLog(path.c_str(), FALSE));
    doc.m_bLogNotes = TRUE;
    doc.m_bLogRaw   = TRUE;      // raw mode: PPL's cooked note logging skipped
    doc.m_strLogLinePreambleNotes = "PRE:";
    doc.Note("raw note");
    doc.CloseLog();
    std::string log = slurp(path);
    EXPECT_FALSE(log_contains(log, "PRE:"));
    remove(path.c_str());
}

// ── AnsiNote: the ESC[...m state machine over Tell ───────────────────────

TEST(AnsiNoteTest, PlainTextBecomesCommentLine)
{
    CMUSHclientDoc doc = make_doc();
    unsigned short old_style = doc.GetNoteStyle();
    doc.AnsiNote("hello");
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(line_text(l0) == "hello");
    EXPECT_EQ(COMMENT, l0->flags & NOTE_OR_COMMAND);
    EXPECT_EQ(old_style, doc.GetNoteStyle());    // style restored
}

TEST(AnsiNoteTest, ForegroundCodeColoursFollowingTextOnly)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_normalcolour[1] = 0x0000CC;            // ANSI red slot
    COLORREF plain_fore = doc.m_iNoteColourFore;
    doc.AnsiNote("pre\x1b[31mred");
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    // the ESC byte is swallowed by DisplayMsg (NOTE_OR_COMMAND lines drop ESC)
    EXPECT_TRUE(line_text(l0) == "prered");
    CStyle* before = style_at(l0, 0);
    CStyle* after  = style_at(l0, 3);
    ASSERT_NE(nullptr, before);
    ASSERT_NE(nullptr, after);
    EXPECT_EQ(plain_fore, before->iForeColour);  // block before ESC keeps old colour
    EXPECT_EQ((COLORREF)0x0000CC, after->iForeColour);
    EXPECT_EQ(COLOUR_RGB, after->iFlags & COLOURTYPE);
}

TEST(AnsiNoteTest, BoldUsesBoldPaletteAndHiliteBit)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_boldcolour[2] = 0x00CC00;              // ANSI green (bold) slot
    doc.AnsiNote("\x1b[1;32mX");
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(line_text(l0) == "X");
    CStyle* s = style_at(l0, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & HILITE) != 0);
    EXPECT_EQ((COLORREF)0x00CC00, s->iForeColour);
}

TEST(AnsiNoteTest, Xterm256ForegroundAndBackground)
{
    Generate256colours();
    CMUSHclientDoc doc = make_doc();
    doc.AnsiNote("\x1b[38;5;196m\x1b[48;5;21mX");
    CStyle* s = style_at(nth_line(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(xterm_256_colours[196], s->iForeColour);
    EXPECT_EQ(xterm_256_colours[21],  s->iBackColour);
}

TEST(AnsiNoteTest, TwentyFourBitColourCollectsRGBComponents)
{
    CMUSHclientDoc doc = make_doc();
    doc.AnsiNote("\x1b[38;2;10;20;30m\x1b[48;2;40;50;60mX");
    CStyle* s = style_at(nth_line(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(RGB(10, 20, 30), s->iForeColour);
    EXPECT_EQ(RGB(40, 50, 60), s->iBackColour);
}

TEST(AnsiNoteTest, UnderlineAndInverseBitsApply)
{
    CMUSHclientDoc doc = make_doc();
    doc.AnsiNote("\x1b[4;7mX");
    CStyle* s = style_at(nth_line(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & UNDERLINE) != 0);
    EXPECT_TRUE((s->iFlags & INVERSE) != 0);
}

TEST(AnsiNoteTest, CancelBoldDropsHiliteMidLine)
{
    CMUSHclientDoc doc = make_doc();
    doc.AnsiNote("\x1b[1mAB\x1b[22mCD");
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(line_text(l0) == "ABCD");
    CStyle* bold_part  = style_at(l0, 0);
    CStyle* plain_part = style_at(l0, 2);
    ASSERT_NE(nullptr, bold_part);
    ASSERT_NE(nullptr, plain_part);
    EXPECT_TRUE((bold_part->iFlags & HILITE) != 0);
    EXPECT_FALSE((plain_part->iFlags & HILITE) != 0);
}

TEST(AnsiNoteTest, ResetRestoresNormalWhiteOnBlack)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_normalcolour[WHITE] = 0x00ABCD;
    doc.m_normalcolour[1]     = 0x0000CC;
    doc.AnsiNote("\x1b[31mred\x1b[0mplain");
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(line_text(l0) == "redplain");
    CStyle* plain = style_at(l0, 3);
    ASSERT_NE(nullptr, plain);
    EXPECT_EQ((COLORREF)0x00ABCD, plain->iForeColour);
}

TEST(AnsiNoteTest, StateAndColoursRestoredAfterCall)
{
    CMUSHclientDoc doc = make_doc();
    doc.NoteStyle(UNDERLINE);
    COLORREF old_fore = doc.m_iNoteColourFore, old_back = doc.m_iNoteColourBack;
    doc.AnsiNote("\x1b[1;31;42mX");
    EXPECT_EQ(UNDERLINE, doc.GetNoteStyle());
    EXPECT_TRUE(doc.m_bNotesInRGB);              // was RGB before, still RGB
    EXPECT_EQ(old_fore, doc.m_iNoteColourFore);
    EXPECT_EQ(old_back, doc.m_iNoteColourBack);
}

MC_TEST_MAIN()
