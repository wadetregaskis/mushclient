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

MC_TEST_MAIN()
