// tests/test_unicode_wrap.cpp
// The wrap column counts cells in a UTF-8 world (src/doc_shared.cpp AddToLine): a wide
// character is two, a combining mark none, and a grapheme cluster is never split.  Plain
// ASCII and every non-UTF-8 world are exactly as they were, a byte being a column.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "unicode_width.h"

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
