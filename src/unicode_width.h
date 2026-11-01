// unicode_width.h : how many cells text takes, and where its grapheme clusters are.
//
// A "cell" is one column of a fixed-pitch display.  An ideograph or an emoji takes two,
// a Latin letter one, a combining accent none.  A grapheme cluster is what a reader
// calls one character: "e" with an accent, a flag (two code points), a family emoji
// (seven).  They are the units of wrapping, selection and the port's drawing.
//
// The contract with plugins does NOT change: GetLineInfo code 2, CStyle::iLength and
// selection columns are still bytes.  This is about where text goes on the screen.

#ifndef MUSHCLIENT_UNICODE_WIDTH_H
#define MUSHCLIENT_UNICODE_WIDTH_H

#include <cstddef>

namespace unicode {

const char * TablesVersion ();      // of the Unicode data they were generated from

// The width of one code point by itself: 0, 1 or 2.
int CodePointWidth (unsigned int iCodePoint);

// Walks a line's code points one at a time, saying where clusters begin (UAX #29,
// extended grapheme clusters) and what each cluster comes to in cells.  It keeps only
// the little it needs of what came before, so that wrapping can use it as text arrives.
class ClusterTracker
  {
  public:
    ClusterTracker () { Reset (); }
    void Reset ();

    // Takes the next code point.  Returns true if it begins a new cluster.  Afterwards
    // iClusterCells is the width of the cluster as it stands, and iDelta what this code
    // point added to the width of the line.
    bool Add (unsigned int iCodePoint);

    int iClusterCells;
    int iDelta;

  private:
    unsigned char m_iPrev;        // property of the previous code point
    unsigned char m_iIndic;       // where we are in a conjunct (GB9c)
    bool m_bPictoSeq;             // Pictographic Extend* so far (GB11)
    bool m_bAfterPictoZwj;        // ... and then a ZWJ
    bool m_bOddRegional;          // an odd number of regional indicators so far (GB12, GB13)
    bool m_bEmpty;
  };

// Decodes the UTF-8 code point at p, which has iAvailable bytes (more than zero).
// Returns its length; a bad sequence is one byte long and comes back as U+FFFD.
int DecodeUtf8 (const char * p, size_t iAvailable, unsigned int & iCodePoint);

// The cells taken by a whole string of UTF-8.
int Utf8Cells (const char * p, size_t iLength);

// The byte offset of the first cluster boundary at or after iOffset, and at or before it.
// Used to make a selection cover whole clusters.
size_t Utf8ClusterEnd (const char * p, size_t iLength, size_t iOffset);
size_t Utf8ClusterStart (const char * p, size_t iLength, size_t iOffset);

// Does this string have anything GDI can't do by itself: clusters of more than one code
// point other than simple Latin accents, emoji, or scripts that need shaping?
bool Utf8NeedsShaping (const char * p, size_t iLength);

}  // namespace unicode

#endif  // MUSHCLIENT_UNICODE_WIDTH_H
