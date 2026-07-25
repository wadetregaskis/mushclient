// doc_tabcomplete_shared.cpp — Tab completion in the command bar.
//
// ONE BODY, EVERY BUILD (Windows, macOS/Linux, headless tests).
//
// This was CSendView::OnKeysTab + CSendView::TabCompleteOneLine in sendvw.cpp.
// It is not view work: it reads world options and the world's output buffer and
// writes a word into the input bar.  The only view-shaped part is "read the bar,
// splice a word into it", which pal::IWorldView already exposes.
//
// Why it moved: the wx port never instantiates CSendView (CChildFrame's splitter
// is a stub, so OnCreateClient bails), so OnKeysTab was dead code there and the
// port grew a from-scratch Tab handler in wx_world_child.cpp with materially
// different behaviour — it completed against the COMMAND HISTORY rather than the
// output buffer, replaced the whole input line rather than the word under the
// cursor, tokenised on a hardcoded [A-Za-z0-9_-] instead of
// App.m_strWordDelimiters, cycled through candidates (Windows takes the first
// match), and ignored m_strTabCompletionDefaults, m_bLowerCaseTabCompletion and
// m_bTabCompletionSpace entirely — which is why those three world options read
// as dead settings on the port.  Sharing the canonical body fixes all of that at
// once and stops the two copies drifting again.
//
// Windows behaviour is unchanged: same algorithm, same order (defaults list
// first, then the output buffer newest-first), same option handling.  The
// select-then-replace pair — including its LockWindowUpdate/UnlockWindowUpdate
// anti-flicker bracket — now lives in the Windows adapter's
// replaceCommandSelection (pal/win_worldview.cpp), which is the same sequence
// this file used to run inline.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

/*
 * Compare strings, case insensitive, for prefix matching.
 * Return TRUE if astr not a prefix of bstr
 */
bool str_prefix (const char *astr, const char *bstr)
{
    if ( !astr )
	    return true;

    if ( !bstr )
	    return true;

    if (*astr == 0)
      return true;

    for ( ; *astr; astr++, bstr++ )
    {
	  if ( tolower(*astr) != tolower(*bstr) )
	      return true;
    }

    return false;
}

// Scan one line for a word that extends strWord, and if found replace the
// characters [nStartChar, nEndChar) in the command bar with it.  Returns true
// once it has completed something (the caller stops scanning).
bool CMUSHclientDoc::TabCompleteOneLine (const int nStartChar,
                                         const int nEndChar,
                                         const CString & strWord,
                                         const CString & strLine)
  {
const char * p = strLine;

while (*p)
  {
// skip leading non-alpha/numeric
  while (*p && !isalnum (*p))
    p++;

  if (*p == 0)
    break;

  if (str_prefix (strWord, p))
    {
    // no match - skip until the end of the word
    while (*p && isalnum (*p))
      p++;

    } // end of no match
  else
    { // match on desired word
    const char * p1 = p;
    int len = 0;
    // find end of word, assuming word consists of letters or numbers

    while (!isspace ((unsigned char) *p1) && strchr (App.m_strWordDelimiters, *p1) == NULL)
      p1++, len++;

    // don't match on same length word
    if (len > strWord.GetLength () )
      {
      // build up replacement word
      CString sReplacement = CString (p, len);
      // make lower case if wanted
      if (m_bLowerCaseTabCompletion)
        sReplacement.MakeLower ();

      // plugins might change tab-complete string
      SendToAllPluginCallbacksRtn (ON_PLUGIN_TABCOMPLETE, sReplacement);

      if (m_bTabCompletionSpace)
        sReplacement = sReplacement + " ";

      // select the characters already typed and replace them, allowing undo
      ReplaceCommandSelectionRange (nStartChar, nEndChar, sReplacement);
      // done
      return true;    // done it!
      }
    else
      p = p1;   // skip this word
    }   // end of match

  }   // end of scanning line for words

  return false;   // no match

  } // end of CMUSHclientDoc::TabCompleteOneLine

// Complete the word under the cursor in the command bar: try the world's
// tab-completion defaults list first, then the most recent output lines.
void CMUSHclientDoc::TabCompleteCommand ()
{
int nStartChar;
int nEndChar;
CString strCurrent;

//save old config
bool old_bTabCompletionSpace = m_bTabCompletionSpace;

  // find where cursor is

  long lStart = 0, lEnd = 0;
  GetCommandSelection (lStart, lEnd);
  nStartChar = (int) lStart;
  nEndChar   = (int) lEnd;

  // don't bother if something selected

  if (nStartChar != nEndChar)
    return;

  // get current text

  strCurrent = GetCommandText ().c_str ();

  // don't bother if nothing typed

  if (strCurrent.IsEmpty ())
    return;

  // ignore if not at end of line or on a delimiter

  if (nEndChar < strCurrent.GetLength ())
    {
    unsigned char c = strCurrent [nEndChar]; // what is the next character?
    if (!isspace (c) && strchr (App.m_strWordDelimiters, c) == NULL)
      m_bTabCompletionSpace = true;
    }

  // ignore if at start of line

  if (nEndChar <= 0)
    {
    m_bTabCompletionSpace = old_bTabCompletionSpace;
    return;
    }

  // search backwards for another delimiter
  for (nStartChar = nEndChar - 1; nStartChar >= 0; nStartChar--)
    {
    unsigned char c = strCurrent [nStartChar]; // what is the next character?
    if (isspace (c) || strchr (App.m_strWordDelimiters, c) != NULL)
      break;
    }

  // move forwards past delimiter

  nStartChar++;

  // ignore if left of cursor is a delimiter too

  if (nStartChar == nEndChar)
    {
    m_bTabCompletionSpace = old_bTabCompletionSpace;
    return;
    }

  CString sWord = strCurrent.Mid (nStartChar, nEndChar - nStartChar);

  sWord.MakeLower ();

  // we are getting somewhere now ...

  // search for that word in the default tab completion list

  if (TabCompleteOneLine (nStartChar, nEndChar, sWord,
                          m_strTabCompletionDefaults))
    {
    m_bTabCompletionSpace = old_bTabCompletionSpace;
    return;   // found it - ignore output buffer
    }


  // search for that word in the output buffer

  int iCount = 0;

  for (POSITION pos = m_LineList.GetTailPosition (); pos; )
    {
    CLine * pLine = m_LineList.GetPrev (pos);
    CString strLine = CString (pLine->text, pLine->len);

    if (++iCount > m_iTabCompletionLines)
      break;    // hit line limit
    if (TabCompleteOneLine (nStartChar, nEndChar, sWord, strLine))
      {
      m_bTabCompletionSpace = old_bTabCompletionSpace;
      return;  // stop when we succeed
      }
    }   // end of scanning each line

  m_bTabCompletionSpace = old_bTabCompletionSpace;
  return;

}   // end of CMUSHclientDoc::TabCompleteCommand
