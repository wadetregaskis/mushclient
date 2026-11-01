#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

/*

  CLine is the class for "lines" from the MUSH, which are shown in the output
  window.

  By making them a class we can produce a collection class which is a linked
  list of CLine's.

  Also, the constructor allocates memory for the text of the line and its first
  style item, and the destructor de-allocates the line's memory, and the style list.

*/

// CLine constructor

CLine::CLine (const long nLineNumber, 
              const unsigned int nWrapColumn,
              const unsigned short iFlags,      
              const COLORREF       iForeColour,
              const COLORREF       iBackColour,
              const bool bUnicode
              )
  {
  hard_return = false;
  len = 0;
  m_iCharCount = 0;   // stays in step with len (see CLine::m_iCharCount)
  m_iPreambleOffset = 0;
  m_iMXPLineTag = 0;
  m_theTime = CTime::GetCurrentTime();
  QueryPerformanceCounter (&m_lineHighPerformanceTime);
  m_nLineNumber = nLineNumber;

  if (bUnicode)
    iMemoryAllocated = nWrapColumn * 4;
  else
    iMemoryAllocated = nWrapColumn;

  // allocate 4 bytes per character for UTF-8

#ifdef USE_REALLOC
  text = (char *) malloc (iMemoryAllocated);
#else
  text = new char [iMemoryAllocated];
#endif
  ASSERT (text);
  if (!text)
    throw std::bad_alloc();
  flags = 0;      // no special flags yet (ie. normal output line)

  CStyle * pStyle; 

  // have at least one style item in the list
  styleList.AddTail (pStyle = NEWSTYLE);

  pStyle->iFlags = iFlags;
  pStyle->iForeColour = iForeColour;
  pStyle->iBackColour = iBackColour;

  }   // end of CLine::CLine

// A saved line coming back.  Unlike the constructor above this does not read the
// clock, does not allocate a default style, and sizes the text buffer exactly; and on
// Windows its style list allocates nodes one at a time rather than MFC's default ten,
// which is most of what a live line costs in memory beyond its text.
CLine::CLine (const long nLineNumber,
              const CTime & theTime,
              const LONGLONG iHighPerformanceTime,
              const unsigned char iLineFlags,
              const bool bHardReturn,
              const int iCharCount,
              const short iPreambleOffset,
              const char * pText,
              const int iLength)
#ifdef _WIN32
  : styleList (1)
#endif
  {
  hard_return = bHardReturn;
  flags = iLineFlags;
  len = iLength;
  m_iCharCount = iCharCount;
  m_iPreambleOffset = iPreambleOffset;
  m_theTime = theTime;
  m_lineHighPerformanceTime.QuadPart = iHighPerformanceTime;
  m_nLineNumber = nLineNumber;
  iMemoryAllocated = iLength > 0 ? iLength : 1;

#ifdef USE_REALLOC
  text = (char *) malloc (iMemoryAllocated);
#else
  text = new char [iMemoryAllocated];
#endif
  ASSERT (text);
  if (!text)
    throw std::bad_alloc();
  if (iLength > 0)
    memcpy (text, pText, iLength);
  }   // end of CLine::CLine (restored)

// CLine destructor

CLine::~CLine ()
  {
#ifdef USE_REALLOC
  free (text);
#else
  delete [] text;
#endif

// delete styles list

  for (POSITION pos = styleList.GetHeadPosition(); pos; )
      DELETESTYLE (styleList.GetNext (pos));
  
  styleList.RemoveAll();

  }

// for tracking down style allocation errors

CStyle * GetNewStyle (const char * filename, const long linenumber)
  {
  CStyle * pNewStyle = new CStyle;
  TRACE3 ("new CStyle at %p at file %s line %ld\n",
          pNewStyle,
          filename,
          linenumber);

  return pNewStyle;
  }

void DeleteStyle (CStyle * pStyle, const char * filename, const long linenumber)
  {
  TRACE3 ("delete CStyle at %p at file %s line %ld\n",
          pStyle,
          filename,
          linenumber);

  delete pStyle;
  }
