// tests/test_unicode_wrap.cpp
// The wrap column counts cells in a UTF-8 world (src/doc_shared.cpp AddToLine): a wide
// character is two, a combining mark none, and a grapheme cluster is never split.  Plain
// ASCII and every non-UTF-8 world are exactly as they were, a byte being a column.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "unicode_width.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

void MakeDoc (CMUSHclientDoc & doc, const bool bUtf8, const int iWrap, const bool bWordWrap = false)
{
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_bUTF_8 = bUtf8 ? TRUE : FALSE;
    doc.m_nWrapColumn = iWrap;
    doc.m_wrap = bWordWrap;
}

void Feed (CMUSHclientDoc & doc, const std::string & s)
{
    doc.DisplayMsg (s.data (), (int) s.size (), 0);
}

std::vector<std::string> Lines (CMUSHclientDoc & doc)
{
    std::vector<std::string> out;
    for (POSITION pos = doc.m_LineList.GetHeadPosition (); pos; )
    {
        CLine * l = doc.m_LineList.GetNext (pos);
        out.push_back (std::string (l->text, l->len));
    }
    return out;
}

std::string Repeat (const std::string & s, int n)
{
    std::string out;
    for (int i = 0; i < n; i++)
        out += s;
    return out;
}

const std::string kIdeograph = "\xE4\xB8\xAD";            // U+4E2D: 3 bytes, 2 cells
const std::string kEmoji = "\xF0\x9F\x98\x80";            // U+1F600: 4 bytes, 2 cells
const std::string kAccented = "e\xCC\x81";                // e + U+0301: 3 bytes, 1 cell
const std::string kFlag = "\xF0\x9F\x87\xA6\xF0\x9F\x87\xBA";   // AU: 8 bytes, 2 cells
const std::string kFamily = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"; // 18 bytes, 2 cells

}  // end of anonymous namespace

TEST(UnicodeWrapTest, AsciiWrapsWhereItAlwaysDid) {
    CMUSHclientDoc doc;
    MakeDoc (doc, true, 20);
    Feed (doc, Repeat ("x", 45) + "\n");
    const std::vector<std::string> lines = Lines (doc);
    ASSERT_TRUE(lines.size () >= 3);
    EXPECT_EQ(20u, lines [0].size ());
    EXPECT_EQ(20u, lines [1].size ());
    EXPECT_EQ(5u, lines [2].size ());
}

TEST(UnicodeWrapTest, IdeographsAreTwoColumnsEach) {
    CMUSHclientDoc doc;
    MakeDoc (doc, true, 20);
    Feed (doc, Repeat (kIdeograph, 11) + "\n");
    const std::vector<std::string> lines = Lines (doc);
    ASSERT_TRUE(lines.size () >= 2);
    EXPECT_TRUE(lines [0] == Repeat (kIdeograph, 10));
    EXPECT_TRUE(lines [1] == kIdeograph);
}

TEST(UnicodeWrapTest, AWideCharacterDoesNotStraddleTheEdge) {
    CMUSHclientDoc doc;
    MakeDoc (doc, true, 20);
    Feed (doc, Repeat ("x", 19) + kIdeograph + "y\n");
    const std::vector<std::string> lines = Lines (doc);
    ASSERT_TRUE(lines.size () >= 2);
    EXPECT_EQ(19u, lines [0].size ());     // one column left empty
    EXPECT_TRUE(lines [1] == kIdeograph + "y");
}

TEST(UnicodeWrapTest, CombiningMarksStayWithTheirLetterAndTakeNoRoom) {
    CMUSHclientDoc doc;
    MakeDoc (doc, true, 20);
    Feed (doc, Repeat (kAccented, 21) + "\n");
    const std::vector<std::string> lines = Lines (doc);
    ASSERT_TRUE(lines.size () >= 2);
    EXPECT_TRUE(lines [0] == Repeat (kAccented, 20));   // 60 bytes, 20 columns
    EXPECT_TRUE(lines [1] == kAccented);
}

TEST(UnicodeWrapTest, EmojiFlagsAndJoinedSequencesStayWhole) {
    for (const std::string * cluster : { &kEmoji, &kFlag, &kFamily }) {
        CMUSHclientDoc doc;
        MakeDoc (doc, true, 20);
        Feed (doc, Repeat (*cluster, 11) + "\n");
        const std::vector<std::string> lines = Lines (doc);
        ASSERT_TRUE(lines.size () >= 2);
        EXPECT_TRUE(lines [0] == Repeat (*cluster, 10));
        EXPECT_TRUE(lines [1] == *cluster);
    }
}

TEST(UnicodeWrapTest, WordWrapCountsCellsToo) {
    CMUSHclientDoc doc;
    MakeDoc (doc, true, 20, true);
    // 8 ideographs (16 cells), a space, then a 6-cell word: it goes to the next line
    Feed (doc, Repeat (kIdeograph, 8) + " " + Repeat (kIdeograph, 3) + "\n");
    const std::vector<std::string> lines = Lines (doc);
    ASSERT_TRUE(lines.size () >= 2);
    EXPECT_EQ(0u, lines [0].find (Repeat (kIdeograph, 8)));
    EXPECT_NE(std::string::npos, lines [1].find (Repeat (kIdeograph, 3)));
    EXPECT_EQ(std::string::npos, lines [0].find (Repeat (kIdeograph, 9)));
}

TEST(UnicodeWrapTest, CharactersArrivingInPiecesCountTheSame) {
    CMUSHclientDoc doc;
    MakeDoc (doc, true, 20);
    const std::string text = Repeat (kEmoji + "a", 8) + "\n";    // 24 cells
    for (char c : text)
        Feed (doc, std::string (1, c));
    const std::vector<std::string> lines = Lines (doc);
    ASSERT_TRUE(lines.size () >= 2);
    // 6 pairs is 18 cells, then one more emoji makes 20 and the "a" goes on the next
    EXPECT_TRUE(lines [0] == Repeat (kEmoji + "a", 6) + kEmoji);
    EXPECT_TRUE(lines [1].substr (0, 1) == "a");
}

TEST(UnicodeWrapTest, ALineStartingAfterAWrapCountsFromNothing) {
    CMUSHclientDoc doc;
    MakeDoc (doc, true, 10);
    Feed (doc, Repeat (kIdeograph, 12) + "\n");
    const std::vector<std::string> lines = Lines (doc);
    ASSERT_TRUE(lines.size () >= 3);
    EXPECT_TRUE(lines [0] == Repeat (kIdeograph, 5));
    EXPECT_TRUE(lines [1] == Repeat (kIdeograph, 5));
    EXPECT_TRUE(lines [2] == Repeat (kIdeograph, 2));
}

// Latin-1, Big5: bytes are columns, and nothing here looks at them as UTF-8.
TEST(UnicodeWrapTest, NonUtf8WorldsAreByteExact) {
    CMUSHclientDoc doc;
    MakeDoc (doc, false, 20);
    Feed (doc, Repeat ("\xE4\xB8\xAD", 9) + "\n");      // 27 bytes
    const std::vector<std::string> lines = Lines (doc);
    ASSERT_TRUE(lines.size () >= 2);
    size_t iTotal = 0;
    for (const std::string & line : lines)
        iTotal += line.size ();
    EXPECT_EQ(27u, iTotal);
}

// The plugin contract: bytes.  A wrapped CJK line still reports its length in bytes.
TEST(UnicodeWrapTest, LineLengthsAreStillBytes) {
    CMUSHclientDoc doc;
    MakeDoc (doc, true, 20);
    Feed (doc, Repeat (kIdeograph, 4) + "\n");
    CLine * line = doc.m_LineList.GetHead ();
    EXPECT_EQ(12, (int) line->len);
    int iStyleBytes = 0;
    for (POSITION pos = line->styleList.GetHeadPosition (); pos; )
        iStyleBytes += line->styleList.GetNext (pos)->iLength;
    EXPECT_EQ(12, iStyleBytes);
}

// ---------------------------------------------------------------------------------------
// A line's buffer grows past four bytes a column for marks (up to 64 times the wrap
// column), and what moves to the next line at a wrap is counted in cells.  So a word of
// heavily marked letters is a few cells and hundreds of bytes, and it once went into a
// new line sized at four bytes a column: a heap overflow any server could drive.  These
// check that every line's text fits its buffer, that the debug heap is intact, and that
// no break falls inside a UTF-8 sequence.

namespace {

const std::string kMark = "\xCC\x81";            // U+0301: 2 bytes, no cells
const std::string kMark3 = "\xE2\x83\x90";       // U+20D0: 3 bytes, no cells

// The debug heap notices a write past the end of a block when the block is freed, or
// when it's checked, and says so in a report: a test fails if any report comes while it
// runs (AddToLine's own ASSERT against a write past the end is one too).
#if defined(_MSC_VER) && defined(_DEBUG)
int g_iHeapReports = 0;

int __cdecl CountHeapReports (int iType, char *, int *)
{
    if (iType == _CRT_ERROR || iType == _CRT_ASSERT)
        g_iHeapReports++;
    return FALSE;   // and let it be reported as usual
}

struct HeapWatch
{
    HeapWatch () { g_iHeapReports = 0; _CrtSetReportHook2 (_CRT_RPTHOOK_INSTALL, CountHeapReports); }
    ~HeapWatch () { _CrtSetReportHook2 (_CRT_RPTHOOK_REMOVE, CountHeapReports); }
};

bool HeapIntact ()
{
    return _CrtCheckMemory () != FALSE && g_iHeapReports == 0;
}
#else
struct HeapWatch {};
bool HeapIntact () { return true; }
#endif

// Every line's bytes inside its buffer, and none of them starting or ending part-way
// through a character.
void ExpectSoundLines (CMUSHclientDoc & doc)
{
    for (POSITION pos = doc.m_LineList.GetHeadPosition (); pos; )
    {
        CLine * l = doc.m_LineList.GetNext (pos);
        EXPECT_TRUE(l->len <= l->iMemoryAllocated);
        if (l->len == 0)
            continue;
        EXPECT_TRUE((((unsigned char) l->text [0]) & 0xC0) != 0x80);
        // the last character is complete
        int i = l->len - 1;
        while (i > 0 && i > l->len - 4 && (((unsigned char) l->text [i]) & 0xC0) == 0x80)
            i--;
        const unsigned char c = (unsigned char) l->text [i];
        const int iWant = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        EXPECT_EQ(iWant, l->len - i);
    }
    EXPECT_TRUE(HeapIntact ());
}

std::string Zalgo (const int iLetters, const int iMarks, const std::string & mark = kMark)
{
    std::string out;
    for (int i = 0; i < iLetters; i++)
        out += std::string (1, (char) ('a' + i % 26)) + Repeat (mark, iMarks);
    return out;
}

size_t LongestLine (CMUSHclientDoc & doc)
{
    size_t iLongest = 0;
    for (const std::string & line : Lines (doc))
        iLongest = line.size () > iLongest ? line.size () : iLongest;
    return iLongest;
}

}  // end of anonymous namespace

// The overflow itself: wrap 80, 70 columns of words, then twelve letters with sixteen
// marks each.  The eleventh letter passes the edge and the ten before it -- 330 bytes --
// move to a line whose buffer was 320.
TEST(UnicodeWrapTest, AMarkedWordMovedAtASpaceFitsItsNewLine) {
    HeapWatch watch;
    for (const bool bIndent : { false, true }) {
        CMUSHclientDoc doc;
        MakeDoc (doc, true, 80, true);
        doc.m_indent_paras = bIndent;
        const std::string word = Zalgo (12, 16);
        Feed (doc, Repeat ("abcd ", 14) + word + " and more words after it\n");
        ExpectSoundLines (doc);
        EXPECT_GT(LongestLine (doc), 320u);    // past the size a new line starts with
        const std::vector<std::string> lines = Lines (doc);
        ASSERT_TRUE(lines.size () >= 2);
        EXPECT_EQ(bIndent ? 1u : 0u, lines [1].find (word.substr (0, 10 * 33)));
    }
}

TEST(UnicodeWrapTest, MarkedWordsArrivingAByteAtATimeFitToo) {
    HeapWatch watch;
    CMUSHclientDoc doc;
    MakeDoc (doc, true, 80, true);
    const std::string text = Repeat ("abcd ", 14) + Zalgo (12, 16) + " " + Zalgo (30, 12, kMark3) + "\n";
    for (char c : text)
        Feed (doc, std::string (1, c));
    ExpectSoundLines (doc);
    EXPECT_GT(LongestLine (doc), 320u);
}

// Words of marked letters and wide characters, wrapping at spaces, many times over.
TEST(UnicodeWrapTest, ManyMarkedAndWideWordsWrapSoundly) {
    HeapWatch watch;
    for (const int iWrap : { 20, 33, 80 }) {
        CMUSHclientDoc doc;
        MakeDoc (doc, true, iWrap, true);
        std::string text;
        for (int i = 0; i < 60; i++)
        {
            text += Zalgo (1 + i % 9, (i * 7) % 40, i % 3 ? kMark : kMark3) + " ";
            if (i % 5 == 0)
                text += Repeat (kIdeograph, 1 + i % 4) + kMark + " ";
            if (i % 7 == 0)
                text += kFamily + Repeat (kMark, 20) + " ";
        }
        Feed (doc, text + "\n");
        ExpectSoundLines (doc);
        // nothing lost: every letter is still there
        std::string all;
        for (const std::string & line : Lines (doc))
            all += line;
        EXPECT_TRUE(all == text);
    }
}

// One letter and a flood of marks: the buffer stops growing at 64 times the wrap column
// and the line breaks there.  The break comes on the second byte of a mark (the letter is
// one byte, each mark two), which used to put that byte at the head of the next line.
TEST(UnicodeWrapTest, TheEmergencyBreakKeepsACharacterWhole) {
    HeapWatch watch;
    for (const std::string * mark : { &kMark, &kMark3 }) {
        for (const bool bBytewise : { false, true }) {
            CMUSHclientDoc doc;
            MakeDoc (doc, true, 20, true);
            const std::string text = "a" + Repeat (*mark, 3000) + "\n";
            if (bBytewise)
                for (char c : text)
                    Feed (doc, std::string (1, c));
            else
                Feed (doc, text);
            ExpectSoundLines (doc);
            const std::vector<std::string> lines = Lines (doc);
            EXPECT_TRUE(lines.size () >= 3);
            EXPECT_TRUE(LongestLine (doc) <= 20u * 64);
        }
    }
}

// The same flood after a space: the break can go at the space, and does.
TEST(UnicodeWrapTest, AFloodOfMarksAfterASpaceBreaksAtTheSpace) {
    HeapWatch watch;
    CMUSHclientDoc doc;
    MakeDoc (doc, true, 20, true);
    doc.m_indent_paras = false;     // the space stays on the first line
    Feed (doc, "word a" + Repeat (kMark, 1000) + "\n");
    ExpectSoundLines (doc);
    const std::vector<std::string> lines = Lines (doc);
    ASSERT_TRUE(lines.size () >= 2);
    EXPECT_TRUE(lines [0] == "word ");
    EXPECT_EQ(0u, lines [1].find ("a" + kMark));
}
