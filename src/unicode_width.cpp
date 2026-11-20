// unicode_width.cpp : see unicode_width.h.  No Windows, no wx: it's arithmetic.

#include "stdafx.h"
#include "unicode_width.h"

namespace unicode {

namespace {

struct CodePointRange { unsigned int iFirst, iLast; };
struct CodePointProp  { unsigned int iFirst, iLast; unsigned char iProp; };

enum
  {
  eGB_Other, eGB_CR, eGB_LF, eGB_Control, eGB_Extend, eGB_ZWJ, eGB_Regional_Indicator,
  eGB_Prepend, eGB_SpacingMark, eGB_L, eGB_V, eGB_T, eGB_LV, eGB_LVT, eGB_Pictographic,
  };

enum { eInCB_None, eInCB_Consonant, eInCB_Extend, eInCB_Linker };

#include "unicode_tables.inc"

template <size_t N>
bool InRanges (const CodePointRange (& table) [N], unsigned int c)
  {
  size_t lo = 0, hi = N;
  while (lo < hi)
    {
    const size_t mid = (lo + hi) / 2;
    if (c > table [mid].iLast)
      lo = mid + 1;
    else if (c < table [mid].iFirst)
      hi = mid;
    else
      return true;
    }
  return false;
  }

template <size_t N>
unsigned char PropOf (const CodePointProp (& table) [N], unsigned int c)
  {
  size_t lo = 0, hi = N;
  while (lo < hi)
    {
    const size_t mid = (lo + hi) / 2;
    if (c > table [mid].iLast)
      lo = mid + 1;
    else if (c < table [mid].iFirst)
      hi = mid;
    else
      return table [mid].iProp;
    }
  return 0;
  }

// where a conjunct has got to: GB9c is Consonant [Extend Linker]* Linker [Extend Linker]* x Consonant
enum { eIndicNone, eIndicConsonant, eIndicLinked };

}  // end of anonymous namespace

const char * TablesVersion ()
  {
  return kUnicodeVersion;
  }

int CodePointWidth (unsigned int c)
  {
  if (c < 0x300)      // nothing below here is a mark or wide, and most text is here
    return (c >= 0x20 && c < 0x7F) || c >= 0xA0 ? 1 : 0;
  if (InRanges (kZero, c))
    return 0;
  if (InRanges (kWide, c))
    return 2;
  return 1;
  }

void ClusterTracker::Reset ()
  {
  iClusterCells = 0;
  iDelta = 0;
  m_iPrev = eGB_Other;
  m_iIndic = eIndicNone;
  m_bPictoSeq = false;
  m_bAfterPictoZwj = false;
  m_bOddRegional = false;
  m_bEmpty = true;
  }

bool ClusterTracker::Add (unsigned int c)
  {
  const unsigned char iProp = c < 0x300 && c != 0xAD
                                ? (c == '\r' ? eGB_CR : c == '\n' ? eGB_LF : (c < 0x20 || (c >= 0x7F && c < 0xA0)) ? eGB_Control :
                                   (c == 0xA9 || c == 0xAE) ? eGB_Pictographic : eGB_Other)
                                : PropOf (kGraphemeBreak, c);
  const unsigned char iIndic = c < 0x300 ? (unsigned char) eInCB_None : PropOf (kIndicConjunct, c);

  bool bBreak;

  if (m_bEmpty)                                                          // GB1
    bBreak = true;
  else if (m_iPrev == eGB_CR && iProp == eGB_LF)                         // GB3
    bBreak = false;
  else if (m_iPrev == eGB_Control || m_iPrev == eGB_CR || m_iPrev == eGB_LF)   // GB4
    bBreak = true;
  else if (iProp == eGB_Control || iProp == eGB_CR || iProp == eGB_LF)   // GB5
    bBreak = true;
  else if (m_iPrev == eGB_L && (iProp == eGB_L || iProp == eGB_V || iProp == eGB_LV || iProp == eGB_LVT))  // GB6
    bBreak = false;
  else if ((m_iPrev == eGB_LV || m_iPrev == eGB_V) && (iProp == eGB_V || iProp == eGB_T))    // GB7
    bBreak = false;
  else if ((m_iPrev == eGB_LVT || m_iPrev == eGB_T) && iProp == eGB_T)   // GB8
    bBreak = false;
  else if (iProp == eGB_Extend || iProp == eGB_ZWJ)                      // GB9
    bBreak = false;
  else if (iProp == eGB_SpacingMark)                                     // GB9a
    bBreak = false;
  else if (m_iPrev == eGB_Prepend)                                       // GB9b
    bBreak = false;
  else if (iIndic == eInCB_Consonant && m_iIndic == eIndicLinked)        // GB9c
    bBreak = false;
  else if (m_bAfterPictoZwj && iProp == eGB_Pictographic)                // GB11
    bBreak = false;
  else if (m_iPrev == eGB_Regional_Indicator && iProp == eGB_Regional_Indicator && m_bOddRegional)  // GB12, GB13
    bBreak = false;
  else
    bBreak = true;                                                       // GB999

  // what we need to remember for next time

  m_bAfterPictoZwj = m_bPictoSeq && iProp == eGB_ZWJ;
  if (iProp == eGB_Pictographic)
    m_bPictoSeq = true;
  else if (iProp != eGB_Extend)
    m_bPictoSeq = false;

  if (iProp == eGB_Regional_Indicator)
    m_bOddRegional = !m_bOddRegional;
  else
    m_bOddRegional = false;

  if (iIndic == eInCB_Consonant)
    m_iIndic = eIndicConsonant;
  else if (iIndic == eInCB_Linker && m_iIndic != eIndicNone)
    m_iIndic = eIndicLinked;
  else if (iIndic != eInCB_Extend)
    m_iIndic = eIndicNone;

  m_iPrev = iProp;
  m_bEmpty = false;

  // and the width

  const int iWidth = CodePointWidth (c);

  if (bBreak)
    {
    iClusterCells = iWidth;
    iDelta = iWidth;
    return true;
    }

  // A cluster is as wide as its base.  A joined emoji is one picture, however many it
  // was made from, and a syllable is its first part.  But U+FE0F asks for a picture,
  // and that is two cells; U+FE0E asks for text, and that's one.
  int iNew = iClusterCells;
  if (c == 0xFE0F)
    iNew = 2;
  else if (c == 0xFE0E && iNew == 2)
    iNew = 1;
  else if (iClusterCells == 0)
    iNew = iWidth;
  else if (iProp == eGB_SpacingMark && iNew < 2)
    iNew += iWidth;   // a vowel sign that takes a place of its own

  iDelta = iNew - iClusterCells;
  iClusterCells = iNew;
  return false;
  }

int DecodeUtf8 (const char * p, size_t iAvailable, unsigned int & iCodePoint)
  {
  const unsigned char c = (unsigned char) p [0];

  if (c < 0x80)
    {
    iCodePoint = c;
    return 1;
    }

  int iLength;
  unsigned int iValue, iMinimum;

  if ((c & 0xE0) == 0xC0)
    {
    iLength = 2;
    iValue = c & 0x1F;
    iMinimum = 0x80;
    }
  else if ((c & 0xF0) == 0xE0)
    {
    iLength = 3;
    iValue = c & 0x0F;
    iMinimum = 0x800;
    }
  else if ((c & 0xF8) == 0xF0)
    {
    iLength = 4;
    iValue = c & 0x07;
    iMinimum = 0x10000;
    }
  else
    {
    iCodePoint = 0xFFFD;
    return 1;
    }

  if ((size_t) iLength > iAvailable)
    {
    iCodePoint = 0xFFFD;
    return 1;
    }

  for (int i = 1; i < iLength; i++)
    {
    const unsigned char next = (unsigned char) p [i];
    if ((next & 0xC0) != 0x80)
      {
      iCodePoint = 0xFFFD;
      return 1;
      }
    iValue = (iValue << 6) | (next & 0x3F);
    }

  if (iValue < iMinimum || iValue > 0x10FFFF || (iValue >= 0xD800 && iValue <= 0xDFFF))
    {
    iCodePoint = 0xFFFD;
    return 1;
    }

  iCodePoint = iValue;
  return iLength;
  }

int Utf8Cells (const char * p, size_t iLength)
  {
  // ASCII needs no tracker at all, until the first thing that isn't
  size_t i = 0;
  while (i < iLength && (unsigned char) p [i] < 0x80 && (unsigned char) p [i] >= 0x20)
    i++;
  if (i == iLength)
    return (int) iLength;

  ClusterTracker tracker;
  int iCells = 0;
  for (i = 0; i < iLength; )
    {
    unsigned int iCodePoint;
    i += DecodeUtf8 (p + i, iLength - i, iCodePoint);
    tracker.Add (iCodePoint);
    iCells += tracker.iDelta;
    }
  return iCells;
  }

size_t Utf8ClusterEnd (const char * p, size_t iLength, size_t iOffset)
  {
  if (iOffset >= iLength)
    return iLength;

  ClusterTracker tracker;
  size_t i = 0;
  while (i < iLength)
    {
    unsigned int iCodePoint;
    const int n = DecodeUtf8 (p + i, iLength - i, iCodePoint);
    if (tracker.Add (iCodePoint) && i >= iOffset)
      return i;
    i += n;
    }
  return iLength;
  }

size_t Utf8ClusterStart (const char * p, size_t iLength, size_t iOffset)
  {
  if (iOffset >= iLength)
    return iLength;

  ClusterTracker tracker;
  size_t i = 0, iStart = 0;
  while (i < iLength)
    {
    unsigned int iCodePoint;
    const int n = DecodeUtf8 (p + i, iLength - i, iCodePoint);
    if (tracker.Add (iCodePoint))
      {
      if (i > iOffset)
        return iStart;
      iStart = i;
      }
    i += n;
    }
  return iStart;
  }

bool Utf8NeedsShaping (const char * p, size_t iLength)
  {
  ClusterTracker tracker;
  for (size_t i = 0; i < iLength; )
    {
    const unsigned char c = (unsigned char) p [i];

    // ASCII can't start anything
    if (c < 0x80)
      {
      tracker.Reset ();
      i++;
      continue;
      }

    unsigned int iCodePoint;
    const int n = DecodeUtf8 (p + i, iLength - i, iCodePoint);
    i += n;

    // emoji, and everything else outside the BMP that GDI's font linking does badly
    if (iCodePoint >= 0x1F000)
      return true;

    // Hebrew to the end of the South and South-East Asian blocks: Uniscribe's job
    if (iCodePoint >= 0x0590 && iCodePoint <= 0x1CFF)
      return true;

    // variation selectors, joiners and enclosing marks
    if (iCodePoint == 0x200D || (iCodePoint >= 0xFE00 && iCodePoint <= 0xFE0F) || iCodePoint == 0x20E3)
      return true;

    // any mark: GDI puts it after the letter unless the font has that exact composition
    if (CodePointWidth (iCodePoint) == 0 && iCodePoint >= 0x300 && iCodePoint < 0x370)
      return true;
    }
  return false;
  }

}  // namespace unicode
