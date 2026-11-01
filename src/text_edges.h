// text_edges.h — where a world's text meets Windows' controls.
//
// A world's text is bytes in that world's own encoding: UTF-8, or for a world not
// marked UTF-8 whatever its server and plugins use.  Scripts see those bytes,
// exactly, whichever it is.  Controls hold UTF-16.  MFC here is multi-byte, so left
// to itself it would convert between the two through the process's code page, which
// knows nothing about the world: a UTF-8 world's text appears as mojibake (or, with
// the 64-bit builds' UTF-8 manifest, a Latin-1 world's letters become U+FFFD, and are
// written back that way when the dialog is OK'd).
//
// So what belongs to a world goes through the wide APIs and converts directly with
// the world's code page.  The process's code page is not involved, so it behaves the
// same with or without the manifest, and on any system.
//
// docs/UTF8-PLAN-2026-10.md has the whole design.

#pragma once

class CMUSHclientDoc;

#ifdef _WIN32

// The encoding of a world's text: CP_UTF8 if it is marked UTF-8, otherwise the code
// page of its output font's character set if that names one (it is what the output
// window draws with), otherwise the machine's own ANSI code page.  That last is
// the system locale's even with the UTF-8 manifest, whose GetACP says 65001.
UINT WorldCodePage (const CMUSHclientDoc * pDoc);

// What the machine itself is set to, which GetACP and GetOEMCP stop saying under the
// manifest: for GetInfo (299) and (300).
UINT MachineAnsiCodePage ();
UINT MachineOemCodePage ();

// The machine's ANSI code page, whatever the process has asked for; 1252 if the
// machine itself is set to UTF-8, which cannot hold arbitrary bytes.
UINT SystemAnsiCodePage ();

// A world's bytes and UTF-16.  Bytes the code page cannot read become U+FFFD, and
// characters it cannot write become its default character.
CStringW WorldTextToWide (const CMUSHclientDoc * pDoc, const char * s, int iLength = -1);
CString  WideToWorldText (const CMUSHclientDoc * pDoc, const wchar_t * s, int iLength = -1);

// A control's text as a world's bytes, and back, through the wide APIs.
void WindowTextToWorld (HWND hWnd, const CMUSHclientDoc * pDoc, CString & strText);
void WorldToWindowText (HWND hWnd, const CMUSHclientDoc * pDoc, const char * sText);

// The world's text, through the process's code page: for the places that cannot take
// UTF-16, such as CF_TEXT or a notepad's ANSI window.
CString WorldTextToAnsi (const CMUSHclientDoc * pDoc, const char * s);
CString AnsiToWorldText (const CMUSHclientDoc * pDoc, const char * s);

// An edit control's selection, and replacing it, as byte offsets into the world's
// bytes.  EM_GETSEL counts UTF-16 units, which only matches bytes for ASCII.
void EditGetSelBytes (HWND hEdit, const CMUSHclientDoc * pDoc, int & nStart, int & nEnd);
void EditSetSelBytes (HWND hEdit, const CMUSHclientDoc * pDoc, int nStart, int nEnd,
                      BOOL bNoScroll = FALSE);
void EditReplaceSel  (HWND hEdit, const CMUSHclientDoc * pDoc, const char * sText,
                      BOOL bCanUndo = FALSE);

// While one exists, DDX_Text moves text between this world and the dialog's controls
// (world configuration, and the dialogs it opens).  The dialogs outside it hold no
// world's text, so they convert as MFC does.
class CDialogsForWorld
  {
  public:
    explicit CDialogsForWorld (const CMUSHclientDoc * pDoc);
    ~CDialogsForWorld ();
  private:
    const CMUSHclientDoc * m_pPrevious;
  };

void MC_DDX_TextString (CDataExchange * pDX, int nIDC, CString & value);

// Keyboard input in full.  In a multi-byte app messages are fetched narrow, and a
// character needing more than two bytes in the process's code page arrives as "?":
// every kanji, with the UTF-8 manifest.  This fetches them wide; call it with a
// WM_CHAR or WM_IME_CHAR just fetched, and it returns true if it has given the
// character to an edit control itself.
bool HandleWideCharMessage (const MSG & msg);

#endif // _WIN32

#ifndef _WIN32
inline UINT MachineAnsiCodePage () { return GetACP (); }
inline UINT MachineOemCodePage ()  { return GetOEMCP (); }
#endif

// The command box, on every platform.  Off Windows its text is the world's bytes.
#ifdef _WIN32
inline void CmdBoxGetText (CEdit & e, const CMUSHclientDoc * d, CString & s)   { WindowTextToWorld (e.m_hWnd, d, s); }
inline void CmdBoxSetText (CEdit & e, const CMUSHclientDoc * d, const char * s) { WorldToWindowText (e.m_hWnd, d, s); }
inline void CmdBoxReplaceSel (CEdit & e, const CMUSHclientDoc * d, const char * s, BOOL bUndo = FALSE) { EditReplaceSel (e.m_hWnd, d, s, bUndo); }
inline void CmdBoxGetSel (CEdit & e, const CMUSHclientDoc * d, int & a, int & b) { EditGetSelBytes (e.m_hWnd, d, a, b); }
inline void CmdBoxSetSel (CEdit & e, const CMUSHclientDoc * d, int a, int b, BOOL bNoScroll = FALSE) { EditSetSelBytes (e.m_hWnd, d, a, b, bNoScroll); }
void WorldTextToClipboard (const CMUSHclientDoc * pDoc, const CString & strText);
BOOL ClipboardToWorldText (const CMUSHclientDoc * pDoc, CString & strText, const bool bWarning = true);
#else
inline void CmdBoxGetText (CEdit & e, const CMUSHclientDoc *, CString & s)   { e.GetWindowText (s); }
inline void CmdBoxSetText (CEdit & e, const CMUSHclientDoc *, const char * s) { e.SetWindowText (s); }
inline void CmdBoxReplaceSel (CEdit & e, const CMUSHclientDoc *, const char * s, BOOL bUndo = FALSE) { e.ReplaceSel (s, bUndo); }
inline void CmdBoxGetSel (CEdit & e, const CMUSHclientDoc *, int & a, int & b) { e.GetSel (a, b); }
inline void CmdBoxSetSel (CEdit & e, const CMUSHclientDoc *, int a, int b, BOOL bNoScroll = FALSE) { e.SetSel (a, b, bNoScroll); }
template <class D> inline CString WorldTextToAnsi (const D *, const char * s) { return CString (s); }
template <class D> inline CString AnsiToWorldText (const D *, const char * s) { return CString (s); }
template <class D> inline void WorldTextToClipboard (const D * d, const CString & s) { putontoclipboard (s, d->m_bUTF_8); }
template <class D> inline BOOL ClipboardToWorldText (const D * d, CString & s, const bool bWarning = true) { return GetClipboardContents (s, d->m_bUTF_8, bWarning); }
#endif
