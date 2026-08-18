// notepad_convert_shared.cpp
//
// The notepad Convert-menu transforms, single-sourced.
//
// The canonical implementations are CTextView::OnConvert* (TextView.cpp), which
// run against a Win32 multiline CEdit.  That control's buffer genuinely holds
// CRLF line endings, and every end-of-line transform there is keyed on the CR:
//   * Unix→DOS inserts a CR before each LF,
//   * DOS→Unix strips CR,
//   * Mac→DOS drops LF and doubles CR into CRLF,
//   * DOS→Mac drops LF,
//   * Remove-end-of-lines only does anything at all inside `if (*p == '\r')`.
//
// The port drives a wxTextCtrl instead, and wxOSX converts every CR to LF on the
// way in: SetValue("a\r\nb") reads back as "a\n\nb", and GetValue() can never
// return a '\r'.  Feeding the control's text straight to the canonical logic
// therefore made Remove-end-of-lines a silent no-op and turned DOS→Mac /
// Mac→DOS into "delete every line break in the document".
//
// The fix is to keep the canonical bodies byte-faithful and normalise at the
// BOUNDARY: expand the control's LF to the CRLF the canonical code expects on
// the way in, and collapse CRLF (or a stray lone CR) back to LF on the way out,
// which is the only thing the control can hold.  Windows behaviour is the
// reference, so the transforms themselves are untouched.
//
// Lives in mushcore (not in the wx notepad frame) so the headless test suite can
// pin it — the wx frame is GUI-only and unreachable from tests/.

#ifndef _WIN32

#include "stdafx.h"
#include "MUSHclient.h"

// ── Boundary normalisation ──────────────────────────────────────────────────

// Control convention (LF-only) → canonical CEdit convention (CRLF).
// A lone CR is accepted defensively; wxOSX never produces one.
CString NotepadTextToCanonical (const CString & in)
  {
  std::string out;
  const char * p = in;
  while (*p)
    {
    if (*p == '\r')
      {
      out += "\r\n";
      p++;
      if (*p == '\n')
        p++;            // it was already a CRLF pair
      }
    else if (*p == '\n')
      {
      out += "\r\n";
      p++;
      }
    else
      out += *p++;
    }
  return CString (out.c_str ());
  }   // end of NotepadTextToCanonical

// Canonical CEdit convention (CRLF) → control convention (LF-only).
CString NotepadTextFromCanonical (const CString & in)
  {
  std::string out;
  const char * p = in;
  while (*p)
    {
    if (*p == '\r')
      {
      out += '\n';
      p++;
      if (*p == '\n')
        p++;            // CRLF collapses to a single LF
      }
    else
      out += *p++;
    }
  return CString (out.c_str ());
  }   // end of NotepadTextFromCanonical

// ── The transforms (faithful copies of CTextView::OnConvert*) ───────────────

namespace {

CString NP_UnixToDos (const CString & in)          // LF → CRLF (drop stray CR)
  {
  std::string out;
  for (const char * p = in; *p; ++p)
    {
    if (*p == '\r')
      continue;
    if (*p == '\n')
      out += '\r';
    out += *p;
    }
  return CString (out.c_str ());
  }

CString NP_DosToUnix (const CString & in)          // strip CR
  {
  std::string out;
  for (const char * p = in; *p; ++p)
    if (*p != '\r')
      out += *p;
  return CString (out.c_str ());
  }

CString NP_MacToDos (const CString & in)           // CR → CRLF (drop stray LF)
  {
  std::string out;
  for (const char * p = in; *p; ++p)
    {
    if (*p == '\n')
      continue;
    out += *p;
    if (*p == '\r')
      out += '\n';
    }
  return CString (out.c_str ());
  }

CString NP_DosToMac (const CString & in)           // strip LF
  {
  std::string out;
  for (const char * p = in; *p; ++p)
    if (*p != '\n')
      out += *p;
  return CString (out.c_str ());
  }

CString NP_RemoveExtraBlanks (const CString & in)  // collapse runs of space/tab
  {
  std::string out;
  for (const char * p = in; *p; ++p)
    {
    if (*p == ' ' || *p == '\t')
      {
      if (p [1] != ' ' && p [1] != '\t')
        out += ' ';
      continue;
      }
    out += *p;
    }
  return CString (out.c_str ());
  }

CString NP_QuoteLines (const CString & in, const char * q)  // prefix every line
  {
  std::string out;
  out += q;
  for (const char * p = in; *p; ++p)
    {
    out += *p;
    if (*p == '\n')
      out += q;
    }
  return CString (out.c_str ());
  }

CString NP_HtmlSpecial (const CString & in)        // < > & "  → entities
  {
  std::string out;
  for (const char * p = in; *p; ++p)
    switch (*p)
      {
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '&':  out += "&amp;";  break;
      case '"':  out += "&quot;"; break;
      default:   out += *p;
      }
  return CString (out.c_str ());
  }

CString NP_RemoveEndOfLines (const CString & in)   // join lines, keep paragraphs
  {
  std::string out;
  char last = ' ';
  for (const char * p = in; *p; )
    {
    if (*p == '\r')
      {
      if (*++p == '\n')
        ++p;                                      // skip the CR (+ LF)
      const char * s = p;
      while (*s == ' ' || *s == '\t')
        ++s;                                      // a blank line → paragraph break
      if (s [0] == '\r' && s [1] == '\n')
        {
        p = s + 2;
        out += "\r\n";
        }
      else if (last != ' ')
        out += ' ';                               // else fold to a single space
      }
    else
      {
      last = *p;
      out += *p;
      ++p;
      }
    }
  return CString (out.c_str ());
  }

}   // end of anonymous namespace

// ── NotepadConvertText ──────────────────────────────────────────────────────
// `src` and `dst` are both in the CONTROL's convention (LF-only); the canonical
// CRLF buffer exists only inside this function.  Returns false if the transform
// failed (base64 decode on invalid input, or an unknown op), leaving `dst`
// untouched — the caller then leaves the notepad alone, as CTextView does.

bool NotepadConvertText (const int op, const CString & src, CString & dst)
  {
  const CString strCanonical = NotepadTextToCanonical (src);
  CString strResult;

  switch (op)
    {
    case eNotepadConvertUpperCase:
      strResult = strCanonical;
      strResult.MakeUpper ();
      break;
    case eNotepadConvertLowerCase:
      strResult = strCanonical;
      strResult.MakeLower ();
      break;
    case eNotepadConvertUnixToDos:      strResult = NP_UnixToDos (strCanonical);         break;
    case eNotepadConvertDosToUnix:      strResult = NP_DosToUnix (strCanonical);         break;
    case eNotepadConvertMacToDos:       strResult = NP_MacToDos (strCanonical);          break;
    case eNotepadConvertDosToMac:       strResult = NP_DosToMac (strCanonical);          break;
    case eNotepadConvertRemoveEol:      strResult = NP_RemoveEndOfLines (strCanonical);  break;
    case eNotepadConvertRemoveBlanks:   strResult = NP_RemoveExtraBlanks (strCanonical); break;
    case eNotepadConvertQuoteLines:
      // Whatever the preference holds, including an explicitly-cleared empty
      // string (a no-op quote) — the canonical CTextView::OnConvertQuotelines
      // (TextView.cpp:477-521) has no "> " fallback of its own, so inventing
      // one here quoted lines the user had asked NOT to be quoted.
      strResult = NP_QuoteLines (strCanonical, (LPCTSTR) App.m_strNotepadQuoteString);
      break;
    case eNotepadConvertQuoteForum:     strResult = QuoteForumCodes (strCanonical);      break;
    case eNotepadConvertBase64Encode:
      strResult = encodeBase64 ((LPCTSTR) strCanonical, true);
      break;
    case eNotepadConvertBase64Decode:
      try
        {
        strResult = decodeBase64 ((LPCTSTR) strCanonical);
        }
      catch (CException * e)
        {
        // Report the failure the way the canonical
        // CTextView::OnConvertBase64decode does (TextView.cpp:1226): a failed
        // decode used to be swallowed silently here, with only the false
        // return telling the caller not to touch the control — the wx
        // frontend turned that into a bare beep, with no indication of WHY.
        e->ReportError ();
        e->Delete ();
        return false;
        }
      catch (...)
        {
        return false;
        }
      break;
    case eNotepadConvertHtmlSpecial:    strResult = NP_HtmlSpecial (strCanonical);       break;
    default:
      return false;
    }

  dst = NotepadTextFromCanonical (strResult);
  return true;
  }   // end of NotepadConvertText

#endif // !_WIN32
