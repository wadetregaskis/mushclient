
/* regexp.h - Definitions etc. for regexp(3) routines. */

#ifndef __REGEXP_H
#define __REGEXP_H



#define MAX_WILDCARDS 10

// This is Perl-Compatible Regular Expressions 

/*************************************************
*       Perl-Compatible Regular Expressions      *
*************************************************/

/* Copyright (c) 1997-2000 University of Cambridge */

#define PCRE_EXP_DECL extern

#ifndef _WIN32
// On Windows these come via the precompiled header (stdafx.h).
// On macOS/Linux regexp.h may be included without stdafx.h, so pull them in directly.
#include <string>
#include <vector>
using std::string;
using std::vector;
#endif

#include "pcre/pcre.h"
                 
// compiled regular expression type

class t_regexp 
  {

  public:

  t_regexp () { 
      m_program = NULL; 
      m_extra = NULL;
      iTimeTaken = 0;
      m_iCaptureCount = -1;
      m_iCount = 0;
      m_iMatchAttempts = 0;
      m_iExecutionError = 0;
                };   // constructor
  ~t_regexp () { 
    if (m_program) 
      pcre_free (m_program); 
    if (m_extra) 
      pcre_free (m_extra); 
    };  // destructor

  // pairs of offsets from match
  vector<int> m_vOffsets;
  // Reusable pcre_exec output buffer + cached capture count, so regexec does not
  // heap-allocate an ovector and re-query PCRE_INFO_CAPTURECOUNT on every call --
  // both are per-compiled-program invariants, and regexec runs once per enabled
  // trigger per line.  Copied into m_vOffsets only on a successful match, so the
  // NOMATCH path still leaves m_vOffsets untouched.
  vector<int> m_vScratch;
  int m_iCaptureCount;   // -1 until first computed from the compiled program
  // count of matching wildcards
  int m_iCount;
  long m_iMatchAttempts;
  // the string we match on (to extract wildcards from)
  string m_sTarget;
  // the program itself
  pcre * m_program;	    

  // extra stuff for speed
  pcre_extra * m_extra;

  LONGLONG iTimeTaken;

  int m_iExecutionError;  // error code if failed execution

  // returns a numbered wildcard
  string GetWildcard (const int iNumber) const
    {
    if (iNumber >= 0 && iNumber < m_iCount)
      return string (
      &m_sTarget.c_str () [m_vOffsets [iNumber * 2]],
      m_vOffsets [(iNumber * 2) + 1] - m_vOffsets [iNumber * 2]).c_str ();
    else
      return "";
    };

  // returns a named wildcard
  string GetWildcard (const string& sName) const;

  // The positional wildcards a script callback receives:
  //   [0] = the whole match, [1 .. capture-count] = the numbered captures.
  // bMakeLowerCase mirrors the per-trigger "convert wildcards to lower case"
  // option (CTrigger::bLowercaseWildcard).  FixWildcard applies it when it
  // fills trigger_item->wildcards[], which is what a Windows ActiveScript (COM)
  // callback and GetTriggerInfo(101..110) are handed -- so an embedded engine
  // standing in for an ActiveScript host has to apply it too.  Lua does NOT:
  // its callback reads this regexp raw on Windows as well, and that asymmetry
  // is upstream behaviour, not an oversight.
  void GetCallbackWildcards (const bool bMakeLowerCase,
                             vector<string> & vResult) const;

  };

t_regexp * regcomp(const char *exp, const int options = 0);
int regexec(t_regexp *prog,
            const char *string,
            const int start_offset = 0);

bool CheckRegularExpression (const CString strRegexp, const int iOptions);

#endif  // #ifndef __REGEXP_H
