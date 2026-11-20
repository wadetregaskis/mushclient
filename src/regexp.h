
/* regexp.h - Definitions etc. for regexp(3) routines. */

#ifndef __REGEXP_H
#define __REGEXP_H



#define MAX_WILDCARDS 10

// This is Perl-Compatible Regular Expressions

/*************************************************
*       Perl-Compatible Regular Expressions      *
*************************************************/

/* Copyright (c) 1997-2000 University of Cambridge */

#ifndef _WIN32
// On Windows these come via the precompiled header (stdafx.h).
// On macOS/Linux regexp.h may be included without stdafx.h, so pull them in directly.
#include <string>
#include <vector>
using std::string;
using std::vector;
#endif

#include "mc_pcre.h"

// compiled regular expression type

class t_regexp
  {

  public:

  t_regexp () {
      m_program = NULL;
      m_match = NULL;
      iTimeTaken = 0;
      m_iCaptureCount = -1;
      m_iJitOnly = -1;
      m_iCount = 0;
      m_iMatchAttempts = 0;
      m_iExecutionError = 0;
                };   // constructor
  ~t_regexp () {
    if (m_program)
      pcre2_code_free (m_program);   // and its JIT code
    if (m_match)
      pcre2_match_data_free (m_match);
    };  // destructor

  // compiled by EvaluateTrigger from a trigger's pattern with its variables
  // expanded (CTrigger::strExpandedPattern says which) -- not by anything else
  bool m_bFromExpansion = false;

  // pairs of offsets from match
  vector<int> m_vOffsets;
  // Reusable match data + cached capture count, so regexec does not heap-allocate
  // them and re-query PCRE2_INFO_CAPTURECOUNT on every call -- both are
  // per-compiled-program invariants, and regexec runs once per enabled trigger per
  // line.  Copied into m_vOffsets only on a successful match, so the NOMATCH path
  // still leaves m_vOffsets untouched.
  pcre2_match_data * m_match;
  // Literals of which every match must contain one (usually there is just one;
  // an alternation can give several: mc_pcre_required_literals), found when
  // the pattern is compiled: a subject with none of them is a NOMATCH without
  // running the pattern at all.  Empty if there are none we can be sure of.
  vector<string> m_vRequired;
  vector<int> m_vRequiredAnchor;    // mc_pcre_literal_anchor, for each
  bool m_bRequiredCaseless = false;
  // With a set: PCRE2's own "last code unit every match has" (or -1).  Where the
  // subject lacks it, PCRE2 rejects the subject faster than the set can be
  // searched, so the pattern is simply run.  (A choice of the cheaper way only:
  // the answer is PCRE2's either way.)
  int m_iLastCodeUnit = -1;
  bool m_bUTF8 = false;
  int m_iJitOnly;        // mc_pcre_is_jit_only: -1 until asked
  int m_iCaptureCount;   // -1 until first computed from the compiled program
  // count of matching wildcards
  int m_iCount;
  long m_iMatchAttempts;
  // the string we match on (to extract wildcards from)
  string m_sTarget;
  // the program itself
  pcre2_code * m_program;

  LONGLONG iTimeTaken;

  int m_iExecutionError;  // error code if failed execution

  // returns a numbered wildcard
  string GetWildcard (const int iNumber) const
    {
    // the second test: \K in a lookahead can leave the end before the start
    if (iNumber >= 0 && iNumber < m_iCount &&
        m_vOffsets [(iNumber * 2) + 1] >= m_vOffsets [iNumber * 2])
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
// The same, for a subject whose length the caller knows (the trigger path).
int regexec(t_regexp *prog,
            const char *string,
            const int length,
            const int start_offset);

bool CheckRegularExpression (const CString strRegexp, const int iOptions);

#endif  // #ifndef __REGEXP_H
