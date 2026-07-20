// tests/test_display.cpp
// Intent-comment tests for the shared display pipeline (doc_shared.cpp
// DisplayMsg / AddToLine / StartNewLine, campaign-b display step 7) and the
// CLine invariants (Line.cpp).  These pin behaviour called out in source
// comments: CR-clears-line, split UTF-8 buffering, embedded NULs, note/line
// type stamping, the JUMP_SIZE line-position index, auto-wrap (UTF-8 and
// word wrap), and style-run splitting across a wrap.

#include "mc_test.h"
// B-3 atomic flip: real headers (portable stub deleted; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"   // eOK etc. (self-sufficiency for the
                                // real-header flip; no-op today)

#include <cstring>
#include <cstdio>
#include <string>
#include <unistd.h>

#include "flags.h"   // eEnabled / eOmitFromOutput / ... (AddTriggerEx flags)
#include "pal/itransport.h"

// ── helpers ───────────────────────────────────────────────────────────────

// A transport that just records the bytes the doc sends to the "MUD".  Used by
// the send-side tests (speedwalk/execute/immediate arms, Pueblo negotiation).
namespace {
struct DispFakeTransport : pal::ITransport {
    std::string sent;
    void resolve(const char*, pal::ResolveToken) override {}
    void cancelResolve() override {}
    void connect(unsigned int, unsigned short, const pal::TransportOptions&) override {}
    void send(const char* data, size_t n) override { sent.append(data, n); }
    void startTls(const pal::TlsParams&) override {}
    void close() override {}
    std::string lastErrorString() const override { return "fake"; }
};
} // namespace

static void make_doc(CMUSHclientDoc& doc)
{
    doc.m_iConnectPhase = eConnectConnectedToMud;
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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

// doc_shared.cpp AddToLine word wrap (audit §3): retitled from
// WordMovesIntactToContinuationLine — StyleRunSplitAcrossWrapKeepsLengthsAndColours
// (below) already covers the space-kept / word-moved split, so this test's unique
// value is the no-char-lost/duplicated invariant: on a plain uncoloured wrap the two
// lines' text concatenates back to the exact original input.
TEST(WrapTest, NoCharsLostOrDuplicatedAcrossWrap)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    // the load-bearing invariant: nothing lost or duplicated across the split
    EXPECT_TRUE(line_text(l0) + line_text(l1) == "aaa bbb cccc");
}

// doc_shared.cpp AddToLine: "if one style is shared we have to make a copy
// and adjust lengths" — when a wrap splits inside a coloured run, both
// lines' style lengths still sum to their text length and the continuation
// keeps the run's colour.
TEST(WrapTest, StyleRunSplitAcrossWrapKeepsLengthsAndColours)
{
    CMUSHclientDoc doc; make_doc(doc);
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
// with silver-on-black, matching a fresh Windows world.

// last (0-based) closed line index — count-2 because the tail is the
// in-progress current line.
static CLine* closed_tail(CMUSHclientDoc& doc)
{
    int n = (int)doc.m_LineList.GetCount();
    return n >= 2 ? nth_line(doc, n - 2) : nullptr;
}

TEST(NotingTest, NoteClosesLineAndStampsCommentFlag)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    int before = (int)doc.m_LineList.GetCount();
    doc.Tell("");
    EXPECT_EQ(before, (int)doc.m_LineList.GetCount());
    EXPECT_EQ(0, doc.m_pCurrentLine->len);
}

TEST(NotingTest, NoteDeferredWhileNotesNotWantedThenDrained)
{
    // mid-telnet-negotiation (m_bNotesNotWantedNow) a note may not be stuck
    // inside the partial MUD line: it is queued with its colours...
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    doc.SetNoteColour(5);
    doc.SetNoteColour(-1);                   // below range: ignored
    EXPECT_EQ(5, doc.GetNoteColour());
    doc.SetNoteColour(MAX_CUSTOM + 1);       // above range: ignored
    EXPECT_EQ(5, doc.GetNoteColour());
}

TEST(NotingTest, GetNoteColourForeBackPerMode)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_customback[2] = 0x445566;
    doc.SetNoteColour(3);                    // palette mode, index 2
    doc.SetNoteColourFore(0xABCDEF12);       // masked to 24 bits
    EXPECT_TRUE(doc.m_bNotesInRGB);
    EXPECT_EQ(0xCDEF12L, doc.GetNoteColourFore());
    EXPECT_EQ(0x445566L, doc.GetNoteColourBack());  // palette back preserved
}

TEST(NotingTest, SetNoteColourBackConvertsSameColourForegroundFirst)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    doc.SetNoteColour(2);
    EXPECT_FALSE(doc.m_bNotesInRGB);
    doc.ColourTell("red", "blue", "hi");
    EXPECT_FALSE(doc.m_bNotesInRGB);        // back to palette mode
    EXPECT_EQ(2, doc.GetNoteColour());
}

TEST(NotingTest, ColourNoteClosesTheLine)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.ColourNote("red", "blue", "x");
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(line_text(l0) == "x");
    EXPECT_EQ(COMMENT, l0->flags & NOTE_OR_COMMAND);
    EXPECT_EQ(0, doc.m_pCurrentLine->len);   // newline appended
}

TEST(NotingTest, NoteStyleMasksToTextStyleBits)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    int lines_before = (int)doc.m_LineList.GetCount();
    doc.Hyperlink("", "text", "hint", "", "", FALSE);
    EXPECT_EQ(lines_before, (int)doc.m_LineList.GetCount());
    EXPECT_EQ(0, doc.m_pCurrentLine->len);
}

TEST(NotingTest, NoteHrEmitsHorizRuleLine)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_EQ(30013L, doc.OpenLog("/nonexistent_mc_dir/x.log", FALSE));  // eCouldNotOpenFile
    EXPECT_FALSE(doc.IsLogOpen());
}

TEST(LoggingTest, NoteLoggedWithPreambleAndPostamble)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    doc.AnsiNote("\x1b[38;5;196m\x1b[48;5;21mX");
    CStyle* s = style_at(nth_line(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(xterm_256_colours[196], s->iForeColour);
    EXPECT_EQ(xterm_256_colours[21],  s->iBackColour);
}

TEST(AnsiNoteTest, TwentyFourBitColourCollectsRGBComponents)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.AnsiNote("\x1b[38;2;10;20;30m\x1b[48;2;40;50;60mX");
    CStyle* s = style_at(nth_line(doc, 0), 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ(RGB(10, 20, 30), s->iForeColour);
    EXPECT_EQ(RGB(40, 50, 60), s->iBackColour);
}

// Parameterised (audit §4): the AnsiNote style-bit cases as a table of
// {sequence, setMask@idxSet, clearMask@idxClear}.  Besides the original
// underline+inverse case, this closes the previously-uncovered SGR 5/6 (blink) and
// SGR 24/25/27 (cancel underline/blink/inverse) arms (gaps in methods_noting.cpp).
// The cancel rows set the bit on "AB" then cancel it for "CD" (index 2).
TEST(AnsiNoteTest, StyleBitsSetAndCancel)
{
    struct Case {
        const char* seq; int idxSet; unsigned setMask; int idxClear; unsigned clearMask;
        const char* note;
    };
    const Case cases[] = {
        { "\x1b[4;7mX",            0, UNDERLINE | INVERSE, -1, 0,         "SGR 4;7 underline+inverse" },
        { "\x1b[5mX",              0, BLINK,               -1, 0,         "SGR 5 (slow blink) → BLINK" },
        { "\x1b[6mX",              0, BLINK,               -1, 0,         "SGR 6 (fast blink) → BLINK" },
        { "\x1b[4mAB\x1b[24mCD",   0, UNDERLINE,            2, UNDERLINE, "SGR 24 cancels underline" },
        { "\x1b[5mAB\x1b[25mCD",   0, BLINK,                2, BLINK,     "SGR 25 cancels blink" },
        { "\x1b[7mAB\x1b[27mCD",   0, INVERSE,              2, INVERSE,   "SGR 27 cancels inverse" },
    };
    for (const Case& c : cases) {
        CMUSHclientDoc doc; make_doc(doc);
        doc.AnsiNote(c.seq);
        CLine* l0 = nth_line(doc, 0);
        ASSERT_NE(nullptr, l0);
        CStyle* sSet = style_at(l0, c.idxSet);
        ASSERT_NE(nullptr, sSet);
        EXPECT_TRUE((sSet->iFlags & c.setMask) == c.setMask);   // c.note
        if (c.idxClear >= 0) {
            CStyle* sClear = style_at(l0, c.idxClear);
            ASSERT_NE(nullptr, sClear);
            EXPECT_TRUE((sClear->iFlags & c.clearMask) == 0);   // bit cancelled
        }
    }
}

TEST(AnsiNoteTest, CancelBoldDropsHiliteMidLine)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
    doc.NoteStyle(UNDERLINE);
    COLORREF old_fore = doc.m_iNoteColourFore, old_back = doc.m_iNoteColourBack;
    doc.AnsiNote("\x1b[1;31;42mX");
    EXPECT_EQ(UNDERLINE, doc.GetNoteStyle());
    EXPECT_TRUE(doc.m_bNotesInRGB);              // was RGB before, still RGB
    EXPECT_EQ(old_fore, doc.m_iNoteColourFore);
    EXPECT_EQ(old_back, doc.m_iNoteColourBack);
}

// ═══ Trigger pipeline: ProcessPreviousLine / ProcessOneTriggerSequence /
//     SendTo — triggers fired by lines arriving through DisplayMsg ══════════

static bool any_line_contains(CMUSHclientDoc& doc, const char* text)
{
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (std::string(l->text, l->len).find(text) != std::string::npos)
            return true;
    }
    return false;
}

static int line_occurrences(CMUSHclientDoc& doc, const char* text)
{
    int n = 0;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (std::string(l->text, l->len).find(text) != std::string::npos)
            n++;
    }
    return n;
}

// A matched trigger with send-to-output writes its (wildcard-expanded)
// response into the output buffer.
TEST(TriggerPipeline, SendToOutputWritesExpandedResponse)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("t1", "* says *", "reply-to-%1",
                                          eEnabled, -1, 0, "", "",
                                          eSendToOutput, 100));
    feed(doc, "bob says hi\n");
    EXPECT_TRUE(any_line_contains(doc, "reply-to-bob"));
}

// omit_from_output removes the matched line from the buffer entirely.
TEST(TriggerPipeline, OmitFromOutputRemovesMatchedLine)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("om", "secret*", "",
                                          eEnabled | eOmitFromOutput,
                                          -1, 0, "", "", eSendToWorld, 100));
    feed(doc, "before\n");
    feed(doc, "secret stuff\n");
    feed(doc, "after\n");
    EXPECT_TRUE(any_line_contains(doc, "before"));
    EXPECT_TRUE(any_line_contains(doc, "after"));
    EXPECT_FALSE(any_line_contains(doc, "secret"));
}

// send-to-variable stores the expanded response in the variable named by the
// trigger's label (methods: SendTo eSendToVariable).
TEST(TriggerPipeline, SendToVariableStoresWildcard)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("capture", "* says *", "%2",
                                          eEnabled, -1, 0, "", "",
                                          eSendToVariable, 100));
    feed(doc, "bob says hello-there\n");
    VARIANT v = doc.GetVariable("capture");
    ASSERT_EQ(VT_BSTR, (int)v.vt);
    EXPECT_STREQ("hello-there", (const char*)CString(v.bstrVal));
}

// lowercase_wildcard lowers the expansion before it is used.
TEST(TriggerPipeline, LowercaseWildcardLowersExpansion)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("lower", "shout *", "%1",
                                          eEnabled | eLowercaseWildcard,
                                          -1, 0, "", "", eSendToVariable, 100));
    feed(doc, "shout LOUD-WORDS\n");
    VARIANT v = doc.GetVariable("lower");
    ASSERT_EQ(VT_BSTR, (int)v.vt);
    EXPECT_STREQ("loud-words", (const char*)CString(v.bstrVal));
}

// keep_evaluating lets a later trigger (higher sequence) also fire; without it
// evaluation stops at the first match.
TEST(TriggerPipeline, KeepEvaluatingRunsLaterTriggerToo)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("first", "combo*", "first-fired",
                                          eEnabled | eKeepEvaluating,
                                          -1, 0, "", "", eSendToOutput, 10));
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("second", "combo*", "second-fired",
                                          eEnabled, -1, 0, "", "",
                                          eSendToOutput, 20));
    feed(doc, "combo attack\n");
    EXPECT_TRUE(any_line_contains(doc, "first-fired"));
    EXPECT_TRUE(any_line_contains(doc, "second-fired"));
}

TEST(TriggerPipeline, WithoutKeepEvaluatingLaterTriggerSkipped)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("first", "combo*", "first-fired",
                                          eEnabled, -1, 0, "", "",
                                          eSendToOutput, 10));
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("second", "combo*", "second-fired",
                                          eEnabled, -1, 0, "", "",
                                          eSendToOutput, 20));
    feed(doc, "combo attack\n");
    EXPECT_TRUE(any_line_contains(doc, "first-fired"));
    EXPECT_FALSE(any_line_contains(doc, "second-fired"));
}

// A one-shot trigger fires once and deletes itself.
TEST(TriggerPipeline, OneShotTriggerDeletedAfterFiring)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("os", "onceonly*", "once-fired",
                                          eEnabled | eTriggerOneShot,
                                          -1, 0, "", "", eSendToOutput, 100));
    feed(doc, "onceonly now\n");
    EXPECT_TRUE(any_line_contains(doc, "once-fired"));
    EXPECT_EQ(VT_EMPTY, (int)doc.GetTriggerInfo("os", 1).vt);  // gone
    feed(doc, "onceonly again\n");
    EXPECT_EQ(1, line_occurrences(doc, "once-fired"));
}

// repeat applies the trigger's COLOUR CHANGE to every match on the line
// (ProcessPreviousLine: "repeating only applies to regular expressions, with
// repeat requested" — it rescans from the previous match's end column).
TEST(TriggerPipeline, RepeatColoursEveryMatchOnTheLine)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("rep", "ding", "",
                                          eEnabled | eTriggerRegularExpression,
                                          -1, 0, "", "", eSendToWorld, 100));
    ASSERT_EQ((long)eOK, doc.SetTriggerOption("rep", "repeat", "1"));
    // custom colour 17 → OTHER_CUSTOM (option subtracts 1), i.e. RGB "other"
    ASSERT_EQ((long)eOK, doc.SetTriggerOption("rep", "custom_colour", "17"));
    ASSERT_EQ((long)eOK, doc.SetTriggerOption("rep", "other_text_colour",
                                              "1193046"));   // 0x123456
    feed(doc, "ding x ding y ding\n");

    CLine* l = nth_line(doc, 0);
    ASSERT_NE(nullptr, l);
    int recoloured = 0;
    for (POSITION pos = l->styleList.GetHeadPosition(); pos; ) {
        CStyle* s = l->styleList.GetNext(pos);
        if ((s->iFlags & COLOUR_RGB) && s->iForeColour == 0x123456)
            recoloured++;
    }
    EXPECT_EQ(3, recoloured);    // all three "ding"s recoloured
}

// match_style: a trigger restricted to ANSI-red text only fires on text whose
// style carries the required foreground palette index (ProcessPreviousLine's
// get_foreground / iMatch block).
TEST(TriggerPipeline, ColourMatchTriggerNeedsMatchingAnsiColour)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("cm", "alarm*", "colour-fired",
                                          eEnabled, -1, 0, "", "",
                                          eSendToOutput, 100));
    // require foreground = ANSI red (palette index 1)
    char match[16];
    snprintf(match, sizeof match, "%d", TRIGGER_MATCH_TEXT | (1 << 4));
    ASSERT_EQ((long)eOK, doc.SetTriggerOption("cm", "match_style", match));

    feed(doc, "alarm plain\n");                       // default white text
    EXPECT_FALSE(any_line_contains(doc, "colour-fired"));

    feed(doc, "\x1b[31malarm red\x1b[0m\n");          // ANSI red
    EXPECT_TRUE(any_line_contains(doc, "colour-fired"));
}

// send-to-status routes the response into the status-line message.
TEST(TriggerPipeline, SendToStatusSetsStatusMessage)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("st", "status*", "the status text",
                                          eEnabled, -1, 0, "", "",
                                          eSendToStatus, 100));
    feed(doc, "status update\n");
    EXPECT_STREQ("the status text", (const char*)doc.m_strStatusMessage);
}

// send-to-command-queue routes through SendMsg(queue=true).  Offline, SendMsg
// bails before queuing (world closed) so the queue stays empty — the arm runs
// but nothing is enqueued.
TEST(TriggerPipeline, SendToQueueArmOfflineLeavesQueueEmpty)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("q", "enqueue*", "queued-cmd",
                                          eEnabled, -1, 0, "", "",
                                          eSendToCommandQueue, 100));
    feed(doc, "enqueue this\n");
    EXPECT_EQ(1L, doc.GetTriggerInfo("q", 21).lVal);   // matched
    EXPECT_EQ(VT_EMPTY, (int)doc.GetQueue().vt);       // nothing queued offline
    doc.DiscardQueue();
}

// send-to-log writes the response into the open log file.
TEST(TriggerPipeline, SendToLogWritesToLogFile)
{
    CMUSHclientDoc doc; make_doc(doc);
    std::string logpath = fresh_log_path();       // audit §4: use the shared helpers
    ASSERT_EQ((long)eOK, doc.OpenLog(logpath.c_str(), FALSE));
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("lg", "logthis*", "logged-response",
                                          eEnabled, -1, 0, "", "",
                                          eSendToLogFile, 100));
    feed(doc, "logthis now\n");
    doc.CloseLog();
    std::string log = slurp(logpath);
    remove(logpath.c_str());
    EXPECT_TRUE(log_contains(log, "logged-response"));
}

// omit_from_log keeps the matched line out of the log while it stays in the
// output buffer.
TEST(TriggerPipeline, OmitFromLogSuppressesLineInLogOnly)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bLogOutput = TRUE;                       // log incoming lines
    std::string logpath = fresh_log_path();        // audit §4: use the shared helpers
    ASSERT_EQ((long)eOK, doc.OpenLog(logpath.c_str(), FALSE));
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("ol", "hidden*", "",
                                          eEnabled | eOmitFromLog,
                                          -1, 0, "", "", eSendToWorld, 100));
    feed(doc, "visible line\n");
    feed(doc, "hidden line\n");
    doc.CloseLog();
    std::string log = slurp(logpath);
    remove(logpath.c_str());
    EXPECT_TRUE(log_contains(log, "visible line"));
    EXPECT_FALSE(log_contains(log, "hidden line"));
    EXPECT_TRUE(any_line_contains(doc, "hidden line"));   // still displayed
}

// A multi-line regexp trigger (lines_to_match = 2) matches across the joined
// recent lines.
TEST(TriggerPipeline, MultiLineTriggerMatchesAcrossTwoLines)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("ml", "first-part\\nsecond-part",
                                          "multi-fired",
                                          eEnabled | eTriggerRegularExpression,
                                          -1, 0, "", "", eSendToOutput, 100));
    ASSERT_EQ((long)eOK, doc.SetTriggerOption("ml", "multi_line", "1"));
    ASSERT_EQ((long)eOK, doc.SetTriggerOption("ml", "lines_to_match", "2"));
    feed(doc, "first-part\n");
    EXPECT_FALSE(any_line_contains(doc, "multi-fired"));
    feed(doc, "second-part\n");
    EXPECT_TRUE(any_line_contains(doc, "multi-fired"));
}

// send-to-speedwalk / send-to-execute / send-immediate arms run without a
// connection (the world-closed branches of SendTo).
TEST(TriggerPipeline, SpeedwalkExecuteImmediateArmsOffline)
{
    CMUSHclientDoc doc; make_doc(doc);
    // Attach a recording transport so the three SendTo arms have an observable
    // sink — without it every send is swallowed (ConnectionOpen() == false) and
    // the test could only re-check that the triggers matched.  Speedwalk delay
    // stays at its default 0 so the speedwalk bytes send immediately rather than
    // queueing onto m_QueuedCommandsList (which never drains headless).
    DispFakeTransport* t = new DispFakeTransport();
    doc.m_pTransport = t;                       // doc destructor tears it down

    ASSERT_EQ((long)eOK, doc.AddTriggerEx("sw", "walknow*", "2n",
                                          eEnabled, -1, 0, "", "",
                                          eSendToSpeedwalk, 100));
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("ex", "execnow*", "some command",
                                          eEnabled, -1, 0, "", "",
                                          eSendToExecute, 100));
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("im", "sendnow*", "immediate cmd",
                                          eEnabled, -1, 0, "", "",
                                          eSendImmediate, 100));
    feed(doc, "walknow\n");
    feed(doc, "execnow\n");
    feed(doc, "sendnow\n");

    // all three matched (info 21 = nMatched) ...
    EXPECT_EQ(1L, doc.GetTriggerInfo("sw", 21).lVal);
    EXPECT_EQ(1L, doc.GetTriggerInfo("ex", 21).lVal);
    EXPECT_EQ(1L, doc.GetTriggerInfo("im", 21).lVal);
    // ... AND each arm actually put its expanded command on the wire.  The
    // speedwalk "2n" expands its single-letter directions to the full words, so
    // it sends "north\r\nnorth\r\n".
    EXPECT_NE(std::string::npos, t->sent.find("north\r\nnorth"));  // speedwalk 2n
    EXPECT_NE(std::string::npos, t->sent.find("some command"));    // execute
    EXPECT_NE(std::string::npos, t->sent.find("immediate cmd"));   // immediate
}

// The trigger sound arm: a configured sound file is attempted (and fails
// headless) without disturbing the rest of the pipeline.
TEST(TriggerPipeline, TriggerSoundArmHeadless)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTriggerEx("snd", "noise*", "sound-fired",
                                          eEnabled, -1, 0,
                                          "/no/such/beep.wav", "",
                                          eSendToOutput, 100));
    feed(doc, "noise here\n");
    EXPECT_TRUE(any_line_contains(doc, "sound-fired"));
}

// ═══ MXP feed + extended ANSI colours through DisplayMsg ═════════════════

// With MXP on, tags are consumed and their effects applied: <B> bolds the
// enclosed text (HILITE style bit), entities are decoded.
TEST(MxpDisplay, BoldTagConsumedAndStyleApplied)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.MXP_On();
    feed(doc, "pre <B>bolded</B> post\n");
    CLine* l = nth_line(doc, doc.m_LineList.GetCount() > 1 ? 1 : 0);
    // find the line that carries the text (MXP_On may note a message first)
    CLine* target = nullptr;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* cand = doc.m_LineList.GetNext(pos);
        if (line_text(cand).find("bolded") != std::string::npos) { target = cand; break; }
    }
    ASSERT_NE(nullptr, target);
    EXPECT_TRUE(line_text(target).find("<B>") == std::string::npos);  // tag consumed
    size_t at = line_text(target).find("bolded");
    CStyle* s = style_at(target, (int)at);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & HILITE) != 0);
    (void)l;
}

TEST(MxpDisplay, EntityDecodedAndUnknownTagSkipped)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.MXP_On();
    feed(doc, "a &lt;tag&gt; &amp; more\n");
    bool found = false;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (line_text(l).find("a <tag> & more") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);

    // an unknown element reports an MXP error but the surrounding text lives on
    feed(doc, "x <NOSUCHTAG> y\n");
    bool kept = false;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (line_text(l).find("y") != std::string::npos) kept = true;
    }
    EXPECT_TRUE(kept);
}

TEST(MxpDisplay, ColourTagAppliesRgbForeground)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.MXP_On();
    feed(doc, "<COLOR RED>tinted</COLOR>\n");
    CLine* target = nullptr;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* cand = doc.m_LineList.GetNext(pos);
        if (line_text(cand).find("tinted") != std::string::npos) { target = cand; break; }
    }
    ASSERT_NE(nullptr, target);
    size_t at = line_text(target).find("tinted");
    CStyle* s = style_at(target, (int)at);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & COLOURTYPE) == COLOUR_RGB);
    EXPECT_EQ((COLORREF)RGB(255, 0, 0), s->iForeColour);
}

// xterm-256 (38;5;n) and truecolor (38;2;r;g;b) ANSI arms.  In ANSI colour
// mode the 256-index is stored raw (translation to RGB is deferred to display
// time); truecolor switches the style to RGB immediately.
TEST(AnsiDisplay, Xterm256ForegroundColour)
{
    CMUSHclientDoc doc; make_doc(doc);
    feed(doc, "\x1b[38;5;196mX\x1b[0m\n");        // 196 = pure red in the cube
    CLine* l = nth_line(doc, 0);
    ASSERT_NE(nullptr, l);
    CStyle* s = style_at(l, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & COLOURTYPE) == COLOUR_ANSI);
    EXPECT_EQ((COLORREF)196, s->iForeColour);     // raw index, deferred lookup
}

TEST(AnsiDisplay, TruecolorForegroundColour)
{
    CMUSHclientDoc doc; make_doc(doc);
    feed(doc, "\x1b[38;2;1;2;3mY\x1b[0m\n");
    CLine* l = nth_line(doc, 0);
    ASSERT_NE(nullptr, l);
    CStyle* s = style_at(l, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & COLOURTYPE) == COLOUR_RGB);
    EXPECT_EQ((COLORREF)RGB(1, 2, 3), s->iForeColour);
}

TEST(AnsiDisplay, Xterm256BackgroundColour)
{
    CMUSHclientDoc doc; make_doc(doc);
    feed(doc, "\x1b[48;5;21mZ\x1b[0m\n");         // 21 = pure blue in the cube
    CLine* l = nth_line(doc, 0);
    ASSERT_NE(nullptr, l);
    CStyle* s = style_at(l, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_TRUE((s->iFlags & COLOURTYPE) == COLOUR_ANSI);
    EXPECT_EQ((COLORREF)21, s->iBackColour);      // raw index, deferred lookup
}

// ═══ doc_shared.cpp direct arms ══════════════════════════════════════════

// ShiftTabCompleteItem populates the extra Shift+Tab completion set.
TEST(DocSharedArms, ShiftTabCompleteItemAccumulates)
{
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_EQ((long)eOK, doc.ShiftTabCompleteItem("alpha"));
    EXPECT_EQ((long)eOK, doc.ShiftTabCompleteItem("beta"));
    EXPECT_EQ(2u, (unsigned)doc.m_ExtraShiftTabCompleteItems.size());
}

// OnGameTrace is a protected afx_msg handler; reach it for tests via a derived
// using-declaration (legal member-pointer access, no doc.h change).
static void call_OnGameTrace(CMUSHclientDoc& d)
{
    struct Access : CMUSHclientDoc { using CMUSHclientDoc::OnGameTrace; };
    (d.*(&Access::OnGameTrace))();
}

// OnGameTrace toggles tracing and notes "Trace on/off" into the output.
TEST(DocSharedArms, OnGameTraceTogglesAndNotes)
{
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_FALSE(doc.m_bTrace);
    call_OnGameTrace(doc);
    EXPECT_TRUE(doc.m_bTrace);
    call_OnGameTrace(doc);
    EXPECT_FALSE(doc.m_bTrace);
}

// FormatTime substitutes %N (world name) and strftime codes.
TEST(DocSharedArms, FormatTimeSubstitutesWorldName)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_mush_name = "MyWorld";
    CString s = doc.FormatTime(CTime(2026, 7, 16, 12, 34, 56), "%N-%Y", false);
    EXPECT_STREQ("MyWorld-2026", (const char*)s);
}

// ═══ Round-4 coverage: DisplayMsg paragraph/Pueblo/UTF-8/telnet arms ═══════

// <P> mode: a single newline inside a paragraph is converted to a space, so
// the paragraph keeps flowing on one line.
TEST(ParagraphMode, NewlineBecomesSpace)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bInParagraph = true;
    feed(doc, "word1\nword2");
    ASSERT_TRUE(doc.m_pCurrentLine != NULL);
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "word1 word2");
}

// <P> mode: a newline after a period gets TWO spaces (sentence spacing).
TEST(ParagraphMode, NewlineAfterPeriodBecomesTwoSpaces)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bInParagraph = true;
    feed(doc, "end.\nNext");
    ASSERT_TRUE(doc.m_pCurrentLine != NULL);
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "end.  Next");
}

// <P> mode: a blank line (two newlines in a row) really does break the
// paragraph — two hard new lines are started.
TEST(ParagraphMode, DoubleNewlineBreaksParagraph)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bInParagraph = true;
    feed(doc, "para");
    int before = (int)doc.m_LineList.GetCount();
    feed(doc, "\n\n");
    EXPECT_EQ(before + 2, (int)doc.m_LineList.GetCount());
}

// <P> mode: consecutive spaces collapse (extras are discarded), and a
// newline that follows a space is NOT converted to another space.
TEST(ParagraphMode, ConsecutiveSpacesCollapse)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bInParagraph = true;
    feed(doc, "a    b");
    ASSERT_TRUE(doc.m_pCurrentLine != NULL);
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "a b");
}

// <P> mode: "sentence. next" typed with ONE space gets sentence spacing —
// the space after . ! or ? is doubled.
TEST(ParagraphMode, SpaceAfterSentencePunctuationDoubled)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bInParagraph = true;
    feed(doc, "Stop! go");
    ASSERT_TRUE(doc.m_pCurrentLine != NULL);
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "Stop!  go");
}

// <P> mode under MXP: tabs are treated as spaces.
TEST(ParagraphMode, TabTreatedAsSpaceUnderMxp)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iUseMXP = eOnCommandMXP;
    doc.MXP_On();
    doc.m_bInParagraph = true;
    feed(doc, "a\tb");
    ASSERT_TRUE(doc.m_pCurrentLine != NULL);
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "a b");
}

// Pueblo startup: "</xch_mudtext>" arriving mid-stream (Pueblo enabled, MXP
// off) flips Pueblo active in permanently-secure MXP mode and swallows the
// tag itself.
TEST(PuebloTest, StartTagActivatesPuebloSecureMode)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bPueblo = TRUE;
    ASSERT_TRUE(!doc.m_bPuebloActive);
    feed(doc, "before</xch_mudtext>after");
    EXPECT_TRUE(doc.m_bPuebloActive);
    EXPECT_EQ((int)eMXP_perm_secure, (int)doc.m_iMXP_defaultMode);
    ASSERT_TRUE(doc.m_pCurrentLine != NULL);
    // the tag bytes never reach the line
    EXPECT_TRUE(line_text(doc.m_pCurrentLine).find("xch_mudtext") == std::string::npos);
}

// ProcessPreviousLine: the Pueblo announce line activates Pueblo level 1.10
// and replies to the server (PUEBLO_REPLY1).
TEST(PuebloTest, AnnounceLineNegotiatesLevel110)
{
    CMUSHclientDoc doc; make_doc(doc);
    DispFakeTransport* t = new DispFakeTransport();
    doc.m_pTransport = t;
    doc.m_bPueblo = TRUE;
    feed(doc, "This world is Pueblo 1.10 enhanced.\n");
    EXPECT_STREQ("1.10", (LPCTSTR)doc.m_iPuebloLevel);
    EXPECT_TRUE(t->sent.find("PUEBLOCLIENT") != std::string::npos);
}

// ProcessPreviousLine: Pueblo 1.0 announce takes the plain-reply arm.
TEST(PuebloTest, AnnounceLineLevel10UsesPlainReply)
{
    CMUSHclientDoc doc; make_doc(doc);
    DispFakeTransport* t = new DispFakeTransport();
    doc.m_pTransport = t;
    doc.m_bPueblo = TRUE;
    feed(doc, "This world is Pueblo 1.0 Enhanced\n");
    EXPECT_STREQ("1.0", (LPCTSTR)doc.m_iPuebloLevel);
    EXPECT_TRUE(t->sent.find("PUEBLOCLIENT") != std::string::npos);
}

// ProcessPreviousLine: the announce line with Pueblo detection disabled does
// NOT activate Pueblo (warning arm), same when it is already active.
TEST(PuebloTest, AnnounceIgnoredWhenDetectionOffOrAlreadyActive)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bPueblo = FALSE;
    feed(doc, "This world is Pueblo 1.10 enhanced.\n");
    EXPECT_FALSE(doc.m_bPuebloActive);

    // now with detection on but already active: level must not change
    CMUSHclientDoc doc2; make_doc(doc2);
    doc2.m_bPueblo = TRUE;
    doc2.m_bPuebloActive = true;
    doc2.m_iPuebloLevel = "9.9";
    feed(doc2, "This world is Pueblo 1.10 enhanced.\n");
    EXPECT_STREQ("9.9", (LPCTSTR)doc2.m_iPuebloLevel);
}

// UTF-8 collection: 3-byte (E2 82 AC = €) and 4-byte (F0 9F 98 80) leads set
// the right byte counters and render once complete.
TEST(Utf8Phase, ThreeAndFourByteSequencesCollect)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bUTF_8 = TRUE;

    feed(doc, "\xE2");
    EXPECT_EQ(HAVE_UTF8_CHARACTER, doc.m_phase);
    EXPECT_EQ(2, doc.m_iUTF8BytesLeft);
    feed(doc, "\x82\xAC");
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "\xE2\x82\xAC");

    feed(doc, "\xF0");
    EXPECT_EQ(3, doc.m_iUTF8BytesLeft);
    feed(doc, "\x9F\x98\x80");
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "\xE2\x82\xAC\xF0\x9F\x98\x80");
}

// UTF-8: a lone continuation byte (0x85) used as a lead byte is bogus — the
// raw byte is flushed to the line instead of being swallowed.
TEST(Utf8Phase, BogusLeadByteFlushedRaw)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bUTF_8 = TRUE;
    feed(doc, "a\x85z");
    EXPECT_EQ(NONE, doc.m_phase);
    std::string text = line_text(doc.m_pCurrentLine);
    EXPECT_TRUE(text.find('a') != std::string::npos);
    EXPECT_TRUE(text.find('\x85') != std::string::npos);
    EXPECT_TRUE(text.find('z') != std::string::npos);
}

// UTF-8: an ASCII byte arriving mid-sequence aborts collection; the partial
// bytes are flushed and the ASCII byte displays normally.
TEST(Utf8Phase, AsciiMidSequenceAbortsCollection)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bUTF_8 = TRUE;
    feed(doc, "\xC3");
    EXPECT_EQ(HAVE_UTF8_CHARACTER, doc.m_phase);
    feed(doc, "A");
    EXPECT_EQ(NONE, doc.m_phase);
    std::string text = line_text(doc.m_pCurrentLine);
    EXPECT_TRUE(text.find('\xC3') != std::string::npos);
    EXPECT_TRUE(text.find('A') != std::string::npos);
}

// Telnet dispatch through the DisplayMsg loop: IAC WONT/DO/DONT reach their
// phase handlers (the refusal responses land on the wire).
TEST(TelnetDispatch, WontDoDontPhasesAnswered)
{
    CMUSHclientDoc doc; make_doc(doc);
    DispFakeTransport* t = new DispFakeTransport();
    doc.m_pTransport = t;

    // IAC DO <200> (unknown option): client must refuse with IAC WONT <200>
    const char do_unknown[] = { '\xFF', '\xFD', '\xC8' };
    doc.DisplayMsg(do_unknown, 3, 0);
    EXPECT_TRUE(t->sent.find("\xFF\xFC\xC8") != std::string::npos);

    // IAC DONT <200>: acknowledged silently (phase machine consumes it)
    t->sent.clear();
    const char dont_unknown[] = { '\xFF', '\xFE', '\xC8' };
    doc.DisplayMsg(dont_unknown, 3, 0);
    EXPECT_EQ(NONE, doc.m_phase);

    // IAC WONT <1> (echo): phase machine consumes it without output text
    const char wont_echo[] = { '\xFF', '\xFC', '\x01' };
    doc.DisplayMsg(wont_echo, 3, 0);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_TRUE(doc.m_pCurrentLine == NULL || doc.m_pCurrentLine->len == 0);
}

// IAC IAC is data, not command: exactly one 0xFF byte lands on the line.
TEST(TelnetDispatch, DoubledIacIsLiteralByte)
{
    CMUSHclientDoc doc; make_doc(doc);
    feed(doc, "\xFF\xFF");
    ASSERT_TRUE(doc.m_pCurrentLine != NULL);
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "\xFF");
    EXPECT_EQ(NONE, doc.m_phase);
}

// Beep: BEL with beeps enabled takes the MessageBeep arm (no sound file) and
// the PlaySoundFile arm (file configured); neither corrupts the line.
TEST(DisplayArms, BeepArmsDoNotAffectLineText)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_enable_beeps = 1;
    feed(doc, "a\ab");
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "ab");

    doc.m_strBeepSound = "/nonexistent-beep.wav";
    feed(doc, "c\ad");
    EXPECT_TRUE(line_text(doc.m_pCurrentLine) == "abcd");
}

// Tab at or past the wrap column starts a soft new line instead of padding.
TEST(DisplayArms, TabAtWrapColumnSoftWraps)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_wrap = TRUE;
    doc.m_nWrapColumn = 20;
    feed(doc, "12345678901234567890");   // exactly at the wrap column
    int lines = (int)doc.m_LineList.GetCount();
    feed(doc, "\t");
    EXPECT_EQ(lines + 1, (int)doc.m_LineList.GetCount());
    // soft wrap: previous line has NO hard return
    CLine* prev = nth_line(doc, lines - 1);
    ASSERT_NE(nullptr, prev);
    EXPECT_FALSE(prev->hard_return);
}

// m_bKeepCommandsOnSameLine: user input appends to the pending MUD prompt
// line, switching to the echo colour mid-line.
TEST(DisplayArms, KeepCommandsOnSameLineUsesEchoColour)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bKeepCommandsOnSameLine = TRUE;
    doc.m_echo_colour = 11;              // custom palette index, not SAMECOLOUR
    feed(doc, "prompt> ");               // partial MUD line
    doc.DisplayMsg("look\n", 5, USER_INPUT);
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    EXPECT_TRUE(line_text(l0) == "prompt> look");
    CStyle* s = style_at(l0, 8);         // style covering 'l' of "look"
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((int)COLOUR_CUSTOM, (int)(s->iFlags & COLOURTYPE));
    EXPECT_EQ(11, (int)s->iForeColour);
}

// SetNewLineColour: user input on a FRESH line stamps the echo colour as the
// line's opening style.
TEST(DisplayArms, UserInputFreshLineStampsEchoColour)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_echo_colour = 3;
    doc.DisplayMsg("north\n", 6, USER_INPUT);
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    CStyle* s = style_at(l0, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((int)COLOUR_CUSTOM, (int)(s->iFlags & COLOURTYPE));
    EXPECT_EQ(3, (int)s->iForeColour);
    EXPECT_TRUE((l0->flags & USER_INPUT) != 0);
}

// SetNewLineColour: notes with "same colour" + custom-16-is-default take the
// custom 15-on-0 arm.
TEST(DisplayArms, NoteSameColourCustom16DefaultArm)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bNotesInRGB = false;
    doc.m_iNoteTextColour = SAMECOLOUR;
    doc.m_bCustom16isDefaultColour = TRUE;
    doc.Note("hello");
    CLine* l0 = nth_line(doc, 0);
    ASSERT_NE(nullptr, l0);
    CStyle* s = style_at(l0, 0);
    ASSERT_NE(nullptr, s);
    EXPECT_EQ((int)COLOUR_CUSTOM, (int)(s->iFlags & COLOURTYPE));
    EXPECT_EQ(15, (int)s->iForeColour);
    EXPECT_EQ(0,  (int)s->iBackColour);
}

// MXP room-name collection phase: a newline in HAVE_MXP_ROOM_NAME phase
// reverts the MXP mode (mode_change -1) and resets the phase.
TEST(MxpSpecialPhase, NewlineEndsRoomNameCollection)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iUseMXP = eOnCommandMXP;
    doc.MXP_On();
    doc.m_phase = HAVE_MXP_ROOM_NAME;
    feed(doc, "The Great Hall\n");
    EXPECT_EQ(NONE, doc.m_phase);
    // collected specially — the room name is NOT displayed as line text
    if (doc.m_pCurrentLine)
        EXPECT_TRUE(line_text(doc.m_pCurrentLine).find("Great Hall") == std::string::npos);
}

// MXP script mode: incoming text while m_bMXP_script (not secure) is
// discarded wholesale.
TEST(MxpSpecialPhase, ScriptModeDiscardsIncomingText)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iUseMXP = eOnCommandMXP;
    doc.MXP_On();
    doc.m_bMXP_script = true;
    feed(doc, "discard me");
    EXPECT_TRUE(doc.m_pCurrentLine == NULL || doc.m_pCurrentLine->len == 0);
}

// MXP secure-once mode: any character other than '<' restores the previous
// mode immediately.
TEST(MxpSpecialPhase, SecureOnceRevertsOnNonTag)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iUseMXP = eOnCommandMXP;
    doc.MXP_On();
    doc.m_iMXP_mode = eMXP_secure_once;
    doc.m_iMXP_previousMode = eMXP_open;
    feed(doc, "x");
    EXPECT_EQ((int)eMXP_open, (int)doc.m_iMXP_mode);
}

// ═══ Round-4 coverage: ProcessPreviousLine arms ═════════════════════════════

// Mapping: a line that equals the mapping-failure string removes the last
// queued direction from the map list.
TEST(PrevLineArms, MappingFailureLineRemovesLastDirection)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bMapping = TRUE;
    doc.m_strMappingFailure = "Alas, you cannot go that way.";
    doc.m_strMapList.AddTail("north");
    doc.m_strMapList.AddTail("east");
    feed(doc, "Alas, you cannot go that way.\n");
    EXPECT_EQ(1, (int)doc.m_strMapList.GetCount());
    EXPECT_STREQ("north", (LPCTSTR)doc.m_strMapList.GetHead());
}

// Mapping: the regexp variant of the failure matcher.
TEST(PrevLineArms, MappingFailureRegexpVariant)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bMapping = TRUE;
    doc.m_bMapFailureRegexp = 1;
    doc.m_strMappingFailure = "cannot go";
    doc.m_MapFailureRegexp = regcomp("cannot go", 0);
    doc.m_strMapList.AddTail("west");
    feed(doc, "You cannot go there\n");
    EXPECT_EQ(0, (int)doc.m_strMapList.GetCount());
}

// Chat snoop: a connected, snooping chat peer drives the ANSI-reconstruction
// path (bold/underline/blink/inverse + colour-change arms,
// ProcessPreviousLine.cpp:320-431), then the stop-snooping arm when permission
// is revoked, without disturbing the line list.
//
// COVERAGE-ONLY for the reconstruction *content*: the bytes the reconstruction
// sends to the snoop socket are not observable in this headless harness — the
// chat socket's CChatSocket::OnSend consumes m_outstanding_output but never
// reaches a readable descriptor (verified: even a real socketpair fd assigned to
// m_hSocket yields no peer bytes, because the port's chat send path does not go
// through ::send here).  What IS asserted is the load-bearing invariant that the
// snoop pass leaves the normal output pipeline intact (both fed lines still land
// in the line list, before and after the snoop-permission change).
TEST(PrevLineArms, ChatSnoopReconstructsAnsiRuns)
{
    CMUSHclientDoc doc; make_doc(doc);
    CChatSocket* snooper = new CChatSocket(&doc);
    snooper->m_iChatStatus = eChatConnected;
    snooper->m_bCanSnoop = true;
    snooper->m_bHeIsSnooping = true;
    doc.m_ChatList.AddTail(snooper);

    // bold red on blue, underline+blink+inverse transitions, then reset — walks
    // every reconstruction arm.
    feed(doc, "\x1B[1;31;44mhot\x1B[0;4;5;7mweird\x1B[0mplain\n");
    ASSERT_TRUE(nth_line(doc, 0) != nullptr);
    EXPECT_TRUE(line_text(nth_line(doc, 0)).find("hot") != std::string::npos);

    // snoop-permission revoked: the stop-snooping arm still leaves output intact
    snooper->m_bCanSnoop = false;
    feed(doc, "another line\n");
    EXPECT_TRUE(line_text(nth_line(doc, 1)).find("another") != std::string::npos);

    doc.m_ChatList.RemoveAll();
    delete snooper;
}

// LogLineInHTMLcolour called DIRECTLY on an explicit head POSITION (the public
// entry point). Trimmed (audit §3): the full <font>/<span>/<u> golden now lives in
// HtmlColourOutputLoggingFromPipeline below (which drives the same emitter through
// the m_bLogOutput+m_bLogHTML+m_bLogInColour pipeline); this test only pins that the
// direct-POSITION call reaches the emitter and produces HTML-colour output.
TEST(PrevLineArms, HtmlColourLogEmitsFontSpanUnderline)
{
    CMUSHclientDoc doc; make_doc(doc);
    std::string path = fresh_log_path();
    ASSERT_EQ((long)eOK, doc.OpenLog(path.c_str(), FALSE));

    feed(doc, "\x1B[31;44mred-on-blue\x1B[0m \x1B[4munder\x1B[0m\n");
    POSITION headpos = doc.m_LineList.GetHeadPosition();
    doc.LogLineInHTMLcolour(headpos);
    doc.CloseLog();

    std::string log = slurp(path);
    remove(path.c_str());
    EXPECT_TRUE(log_contains(log, "<font color="));
}

// Output-line logging: a '%'-bearing preamble/postamble goes through
// FormatTime substitution before being written.
TEST(PrevLineArms, OutputLogPreambleFormatsTimeCodes)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bLogOutput = TRUE;
    doc.m_strLogLinePreambleOutput = "[%Y] ";
    doc.m_strLogLinePostambleOutput = " {%Y}";
    std::string path = fresh_log_path();          // audit §4: use the shared helpers
    ASSERT_EQ((long)eOK, doc.OpenLog(path.c_str(), FALSE));
    feed(doc, "logged output line\n");
    doc.CloseLog();

    std::string log = slurp(path);
    remove(path.c_str());
    // the year expanded (e.g. "[2026]"), no literal "%Y" left behind
    EXPECT_FALSE(log_contains(log, "[%Y]"));
    EXPECT_TRUE(log_contains(log, "logged output line"));
    EXPECT_TRUE(log_contains(log, "[2"));
    EXPECT_TRUE(log_contains(log, "{2"));
}

// ═══ Round-4 tail: HTML-colour output logging, note-log preambles,
//     Pueblo variants ═════════════════════════════════════════════════════

// MUD output logged in HTML-colour mode goes through LogLineInHTMLcolour
// from the pipeline itself; plain-HTML mode escapes the markup instead.
// This test owns the full <font>/<span>/<u> golden (audit §3, folded in from the
// trimmed HtmlColourLogEmitsFontSpanUnderline): colour changes emit <font>, a
// non-black background opens <span>, underline wraps in <u>, and both close.
TEST(PrevLineArms, HtmlColourOutputLoggingFromPipeline)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bLogOutput = TRUE;
    doc.m_bLogHTML = TRUE;
    doc.m_bLogInColour = TRUE;
    std::string path = fresh_log_path();
    ASSERT_EQ((long)eOK, doc.OpenLog(path.c_str(), FALSE));
    feed(doc, "\x1B[31;44mred-on-blue\x1B[0m \x1B[4munder\x1B[0m\r\n");
    doc.CloseLog();

    std::string log = slurp(path);
    remove(path.c_str());
    EXPECT_TRUE(log_contains(log, "<font color="));
    EXPECT_TRUE(log_contains(log, "red-on-blue"));
    EXPECT_TRUE(log_contains(log, "<span style="));   // non-black background
    EXPECT_TRUE(log_contains(log, "</span>"));
    EXPECT_TRUE(log_contains(log, "<u>under</u>"));    // underline run
    EXPECT_TRUE(log_contains(log, "</font>"));

    // plain-HTML (no colour) escapes markup characters
    CMUSHclientDoc doc2; make_doc(doc2);
    doc2.m_bLogOutput = TRUE;
    doc2.m_bLogHTML = TRUE;
    doc2.m_bLogInColour = FALSE;
    std::string path2 = fresh_log_path();
    ASSERT_EQ((long)eOK, doc2.OpenLog(path2.c_str(), FALSE));
    feed(doc2, "a <b> & c\r\n");
    doc2.CloseLog();
    std::string log2 = slurp(path2);
    remove(path2.c_str());
    EXPECT_TRUE(log_contains(log2, "&lt;b&gt;"));
}

// Notes logged with '%'-bearing note preamble/postamble format the time.
TEST(PrevLineArms, NoteLogPreambleFormatsTimeCodes)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bLogNotes = TRUE;
    doc.m_strLogLinePreambleNotes = "[N%Y] ";
    doc.m_strLogLinePostambleNotes = " {N%Y}";
    std::string path = fresh_log_path();          // audit §4: use the shared helpers
    ASSERT_EQ((long)eOK, doc.OpenLog(path.c_str(), FALSE));
    doc.Note("a logged note");
    doc.CloseLog();

    std::string log = slurp(path);
    remove(path.c_str());
    EXPECT_TRUE(log_contains(log, "[N2"));        // %Y expanded
    EXPECT_TRUE(log_contains(log, "a logged note"));
    EXPECT_TRUE(log_contains(log, "{N2"));
}

// Pueblo announce variants: detection off via "no MXP", and level 2.50.
TEST(PuebloTest, NoMxpSettingAndLevel250)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bPueblo = TRUE;
    doc.m_iUseMXP = eNoMXP;
    feed(doc, "This world is Pueblo 1.10 enhanced.\n");
    EXPECT_TRUE(!doc.m_bPuebloActive);        // refused: MXP set to "No"

    CMUSHclientDoc doc2; make_doc(doc2);
    DispFakeTransport* t = new DispFakeTransport();
    doc2.m_pTransport = t;
    doc2.m_bPueblo = TRUE;
    feed(doc2, "This world is Pueblo 2.50 enhanced.\n");
    EXPECT_STREQ("2.50", (LPCTSTR)doc2.m_iPuebloLevel);
    EXPECT_TRUE(t->sent.find("PUEBLOCLIENT") != std::string::npos);
}

// ── round 5: outstanding-note drain (non-RGB), wrap splits, guards ────────

// Draining deferred notes when the note colours were NOT in RGB mode: the
// restore path clears m_bNotesInRGB instead of putting colours back.
TEST(NotingR5, DeferredNoteDrainWithoutRgbColours)
{
    CMUSHclientDoc doc; make_doc(doc);
    feed(doc, "partial");
    doc.m_bNotesNotWantedNow = true;
    doc.m_bNotesInRGB = false;     // plain palette notes
    doc.Note("plain deferred");
    ASSERT_EQ((size_t)1, doc.m_OutstandingLines.size());
    doc.m_bNotesNotWantedNow = false;
    feed(doc, " done\n");
    EXPECT_TRUE(doc.m_OutstandingLines.empty());
    EXPECT_FALSE(doc.m_bNotesInRGB);
    CLine* note_line = nth_line(doc, 1);
    ASSERT_NE(nullptr, note_line);
    EXPECT_TRUE(line_text(note_line) == "plain deferred");
}

// A long line of Big5 double-byte characters wraps at a character boundary
// (the DBCS split-point arm), not mid-character.
TEST(WrapR5, Big5DoubleByteSplitPoint)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_wrap = TRUE;
    doc.m_nWrapColumn = 20;
    doc.m_bUTF_8 = FALSE;

    std::string big5;
    for (int i = 0; i < 15; ++i) { big5 += (char)0xA4; big5 += (char)0x40; }
    big5 += '\n';
    doc.DisplayMsg(big5.c_str(), (int)big5.size(), 0);

    // Big5 detection splits at the last whole-character boundary BEFORE the
    // wrap column (len 18 < 20, even).  Without the DBCS arm the line would be
    // hard-broken at the wrap column (len == 20): still even, so the old
    // len%2==0 check alone was vacuous.  Pin "split earlier than the column, on
    // a character boundary" — the mutation that disables detection breaks it.
    ASSERT_TRUE(doc.m_LineList.GetCount() >= 2);
    CLine* l0 = nth_line(doc, 0);
    EXPECT_EQ(0, l0->len % 2);                 // whole-character boundary
    EXPECT_LT(l0->len, (int)doc.m_nWrapColumn); // detection split before the column
}

// DisplayMsg / SetNewLineColour guards when no line exists yet.
TEST(GuardsR5, NoCurrentLineAndNoLinePositions)
{
    CMUSHclientDoc doc; make_doc(doc);
    feed(doc, "seed\n");

    // SetNewLineColour with no current line: silently returns.  (The saved
    // line stays owned by m_LineList; DisplayMsg simply opens another.)
    doc.m_pCurrentLine = nullptr;
    doc.SetNewLineColour(0);

    // DisplayMsg with no current line starts one
    doc.DisplayMsg("x", 1, 0);
    EXPECT_NE(nullptr, doc.m_pCurrentLine);

    // DisplayMsg before the line-positions array exists: early return
    POSITION* savedPos = doc.m_pLinePositions;
    doc.m_pLinePositions = nullptr;
    doc.DisplayMsg("y", 1, 0);
    doc.m_pLinePositions = savedPos;
}

MC_TEST_MAIN()
