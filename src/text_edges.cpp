// text_edges.cpp — see text_edges.h.

#include "stdafx.h"

#ifdef _WIN32

#include "MUSHclient.h"
#include "doc.h"
#include "text_edges.h"

#undef DDX_Text       // this file is where the real one is called from

static const CMUSHclientDoc * g_pDialogWorld = NULL;

static UINT SystemLocaleCodePage (const LCTYPE type, const UINT fallback)
  {
  DWORD cp = 0;
  if (!GetLocaleInfoA (LOCALE_SYSTEM_DEFAULT, type | LOCALE_RETURN_NUMBER,
                       (LPSTR) &cp, sizeof cp) || cp == 0)
    return fallback;
  return cp;
  }

UINT MachineAnsiCodePage ()
  {
  return SystemLocaleCodePage (LOCALE_IDEFAULTANSICODEPAGE, 1252);
  }

UINT MachineOemCodePage ()
  {
  return SystemLocaleCodePage (LOCALE_IDEFAULTCODEPAGE, 437);
  }

UINT SystemAnsiCodePage ()
  {
  const UINT cp = MachineAnsiCodePage ();
  return cp == CP_UTF8 ? 1252 : cp;
  } // end of SystemAnsiCodePage

UINT WorldCodePage (const CMUSHclientDoc * pDoc)
  {
  if (pDoc == NULL)
    return SystemAnsiCodePage ();
  if (pDoc->m_bUTF_8)
    return CP_UTF8;
  CHARSETINFO csi;
  if (pDoc->m_font_charset != ANSI_CHARSET && pDoc->m_font_charset != DEFAULT_CHARSET &&
      TranslateCharsetInfo ((DWORD *) (DWORD_PTR) pDoc->m_font_charset, &csi, TCI_SRCCHARSET))
    return csi.ciACP;
  return SystemAnsiCodePage ();
  } // end of WorldCodePage

static CStringW ToWide (UINT cp, const char * s, int iLength)
  {
  if (iLength < 0)
    iLength = (int) strlen (s);
  CStringW w;
  if (iLength == 0)
    return w;
  const int n = MultiByteToWideChar (cp, 0, s, iLength, NULL, 0);
  if (n > 0)
    {
    MultiByteToWideChar (cp, 0, s, iLength, w.GetBuffer (n), n);
    w.ReleaseBuffer (n);
    }
  return w;
  }

static CString FromWide (UINT cp, const wchar_t * s, int iLength)
  {
  if (iLength < 0)
    iLength = (int) wcslen (s);
  CString a;
  if (iLength == 0)
    return a;
  const int n = WideCharToMultiByte (cp, 0, s, iLength, NULL, 0, NULL, NULL);
  if (n > 0)
    {
    WideCharToMultiByte (cp, 0, s, iLength, a.GetBuffer (n), n, NULL, NULL);
    a.ReleaseBuffer (n);
    }
  return a;
  }

CStringW WorldTextToWide (const CMUSHclientDoc * pDoc, const char * s, int iLength)
  {
  return ToWide (WorldCodePage (pDoc), s, iLength);
  }

CString WideToWorldText (const CMUSHclientDoc * pDoc, const wchar_t * s, int iLength)
  {
  return FromWide (WorldCodePage (pDoc), s, iLength);
  }

// Text to and from a window.  A window whose procedure is narrow (everything MFC has
// subclassed: the command box, and any control with a CWnd attached) gets wide messages
// through a Windows thunk that allows two bytes per character, which is too few for
// UTF-8: a kanji just disappears.  So with the process on UTF-8 such a window is
// talked to narrowly, in UTF-8.  Otherwise the wide calls are right: a plain control's
// procedure is wide, and on a code page with no such character nothing could carry it.
static bool TalkNarrowInUtf8 (HWND hWnd)
  {
  return GetACP () == CP_UTF8 && !IsWindowUnicode (hWnd);
  }

static CStringW WideTextOf (HWND hWnd)
  {
  CStringW w;
  if (TalkNarrowInUtf8 (hWnd))
    {
    const int n = GetWindowTextLengthA (hWnd);
    if (n > 0)
      {
      CString a;
      const int got = GetWindowTextA (hWnd, a.GetBuffer (n + 1), n + 1);
      a.ReleaseBuffer (got);
      w = ToWide (CP_UTF8, a, a.GetLength ());
      }
    return w;
    }
  const int n = GetWindowTextLengthW (hWnd);
  if (n > 0)
    {
    const int got = GetWindowTextW (hWnd, w.GetBuffer (n + 1), n + 1);
    w.ReleaseBuffer (got);
    }
  return w;
  }

static void SetWideText (HWND hWnd, const CStringW & w)
  {
  if (TalkNarrowInUtf8 (hWnd))
    SetWindowTextA (hWnd, FromWide (CP_UTF8, w, w.GetLength ()));
  else
    SetWindowTextW (hWnd, w);
  }

static void ReplaceSelWide (HWND hEdit, const wchar_t * s, const BOOL bCanUndo)
  {
  if (TalkNarrowInUtf8 (hEdit))
    SendMessageA (hEdit, EM_REPLACESEL, (WPARAM) bCanUndo,
                  (LPARAM) (LPCSTR) FromWide (CP_UTF8, s, -1));
  else
    SendMessageW (hEdit, EM_REPLACESEL, (WPARAM) bCanUndo, (LPARAM) s);
  }

void WindowTextToWorld (HWND hWnd, const CMUSHclientDoc * pDoc, CString & strText)
  {
  const CStringW w = WideTextOf (hWnd);
  strText = WideToWorldText (pDoc, w, w.GetLength ());
  }

void WorldToWindowText (HWND hWnd, const CMUSHclientDoc * pDoc, const char * sText)
  {
  // Only when it differs, as AfxSetWindowText does: it stops the flicker, and
  // an edit control losing its undo for no reason.
  const CStringW w = WorldTextToWide (pDoc, sText);
  if (WideTextOf (hWnd) != w)
    SetWideText (hWnd, w);
  }

CString WorldTextToAnsi (const CMUSHclientDoc * pDoc, const char * s)
  {
  // A UTF-8 world without the manifest stays as it always was: its bytes, shown as
  // mojibake, come back intact; converting would lose what the code page cannot hold.
  const UINT cp = WorldCodePage (pDoc);
  if (cp == GetACP () || cp == CP_UTF8)
    return CString (s);
  const CStringW w = ToWide (cp, s, -1);
  return FromWide (CP_ACP, w, w.GetLength ());
  }

CString AnsiToWorldText (const CMUSHclientDoc * pDoc, const char * s)
  {
  const UINT cp = WorldCodePage (pDoc);
  if (cp == GetACP () || cp == CP_UTF8)
    return CString (s);
  const CStringW w = ToWide (CP_ACP, s, -1);
  return FromWide (cp, w, w.GetLength ());
  }

// The byte offset in the world's encoding of a UTF-16 offset into a control's text,
// and the reverse.
static int WideToByteOffset (const CMUSHclientDoc * pDoc, const CStringW & w, int iWide)
  {
  iWide = min (iWide, w.GetLength ());
  return iWide <= 0 ? 0 : WideToWorldText (pDoc, w, iWide).GetLength ();
  }

static int ByteToWideOffset (const CMUSHclientDoc * pDoc, const CStringW & w, int iBytes)
  {
  if (iBytes <= 0)
    return 0;
  const CString a = WideToWorldText (pDoc, w, w.GetLength ());
  iBytes = min (iBytes, a.GetLength ());
  return WorldTextToWide (pDoc, a, iBytes).GetLength ();
  }

void EditGetSelBytes (HWND hEdit, const CMUSHclientDoc * pDoc, int & nStart, int & nEnd)
  {
  DWORD s = 0, e = 0;
  SendMessageW (hEdit, EM_GETSEL, (WPARAM) &s, (LPARAM) &e);
  const CStringW w = WideTextOf (hEdit);
  nStart = WideToByteOffset (pDoc, w, (int) s);
  nEnd   = WideToByteOffset (pDoc, w, (int) e);
  }

void EditSetSelBytes (HWND hEdit, const CMUSHclientDoc * pDoc, int nStart, int nEnd, BOOL bNoScroll)
  {
  const CStringW w = WideTextOf (hEdit);
  const int s = nStart < 0 ? nStart : ByteToWideOffset (pDoc, w, nStart);
  const int e = nEnd   < 0 ? nEnd   : ByteToWideOffset (pDoc, w, nEnd);
  SendMessageW (hEdit, EM_SETSEL, (WPARAM) s, (LPARAM) e);
  if (!bNoScroll)
    SendMessageW (hEdit, EM_SCROLLCARET, 0, 0);
  }

void EditReplaceSel (HWND hEdit, const CMUSHclientDoc * pDoc, const char * sText, BOOL bCanUndo)
  {
  ReplaceSelWide (hEdit, WorldTextToWide (pDoc, sText), bCanUndo);
  }

// The clipboard holds UTF-16 too.  putontoclipboard's "not Unicode" sets only CF_TEXT,
// read through the process's code page; a world whose text is in another needs it
// converted first.
void WorldTextToClipboard (const CMUSHclientDoc * pDoc, const CString & strText)
  {
  const UINT cp = WorldCodePage (pDoc);
  if (cp == CP_UTF8 || cp == GetACP ())
    {
    putontoclipboard (strText, cp == CP_UTF8);
    return;
    }
  const CStringW w = ToWide (cp, strText, strText.GetLength ());
  putontoclipboard (FromWide (CP_UTF8, w, w.GetLength ()), true);
  }

BOOL ClipboardToWorldText (const CMUSHclientDoc * pDoc, CString & strText, const bool bWarning)
  {
  const UINT cp = WorldCodePage (pDoc);
  if (cp == CP_UTF8 || cp == GetACP ())
    return GetClipboardContents (strText, cp == CP_UTF8, bWarning);
  CString strUtf8;
  if (!GetClipboardContents (strUtf8, true, bWarning))
    return FALSE;
  const CStringW w = ToWide (CP_UTF8, strUtf8, strUtf8.GetLength ());
  strText = FromWide (cp, w, w.GetLength ());
  return TRUE;
  }

CDialogsForWorld::CDialogsForWorld (const CMUSHclientDoc * pDoc)
  : m_pPrevious (g_pDialogWorld)
  {
  g_pDialogWorld = pDoc;
  }

CDialogsForWorld::~CDialogsForWorld ()
  {
  g_pDialogWorld = m_pPrevious;
  }

void MC_DDX_TextString (CDataExchange * pDX, int nIDC, CString & value)
  {
  if (g_pDialogWorld == NULL)
    {
    ::DDX_Text (pDX, nIDC, value);
    return;
    }
  HWND hWndCtrl = pDX->PrepareEditCtrl (nIDC);
  if (pDX->m_bSaveAndValidate)
    WindowTextToWorld (hWndCtrl, g_pDialogWorld, value);
  else
    WorldToWindowText (hWndCtrl, g_pDialogWorld, value);
  }

bool HandleWideCharMessage (const MSG & msg)
  {
  if (msg.message != WM_CHAR && msg.message != WM_IME_CHAR)
    return false;
  if (msg.wParam < 0x80)
    return false;       // ASCII is the same either way, and has its uses (tab, return ...)

  char sClass [16];
  if (!GetClassNameA (msg.hwnd, sClass, sizeof sClass) || _stricmp (sClass, "Edit") != 0)
    return false;

  // A character beyond U+FFFF comes as two messages: keep the first half until the
  // second, as one EM_REPLACESEL.
  static wchar_t cHighSurrogate = 0;
  const wchar_t c = (wchar_t) msg.wParam;
  if (c >= 0xD800 && c <= 0xDBFF)
    {
    cHighSurrogate = c;
    return true;
    }
  wchar_t s [3] = { c, 0, 0 };
  if (c >= 0xDC00 && c <= 0xDFFF && cHighSurrogate)
    {
    s [0] = cHighSurrogate;
    s [1] = c;
    }
  cHighSurrogate = 0;
  ReplaceSelWide (msg.hwnd, s, TRUE);
  return true;
  } // end of HandleWideCharMessage

#endif // _WIN32
