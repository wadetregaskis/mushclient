// pal/win_a11y_pane.cpp -- see win_a11y_pane.h.
//
// Appending goes through the Text Object Model rather than the selection: moving the
// selection to the end and back would make a screen reader read the caret moving, and
// would lose the player's place if there were any mistake in putting it back.  Nothing
// here goes through the process's code page: text arrives as UTF-8, and the control is
// created with the wide API.

#include "stdafx.h"

#include "pal/win_a11y_pane.h"
#include "MUSHclient.h"
#include "doc.h"
#include "a11y.h"

#include <commctrl.h>
#include <oleacc.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>

#include <map>
#include <string>

#pragma comment (lib, "comctl32.lib")
#pragma comment (lib, "oleacc.lib")

// {8CC497C0-A1DF-11CE-8098-00AA0047BE5D}
static const GUID kIID_ITextDocument =
  { 0x8CC497C0, 0xA1DF, 0x11CE, { 0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D } };

// oleacc.h's, which no library defines
static const GUID kCLSID_AccPropServices =
  { 0xB5F8350B, 0x0548, 0x48B1, { 0xA6, 0xEE, 0x88, 0xBD, 0x00, 0xB4, 0xA5, 0xE7 } };
static const GUID kPROPID_ACC_NAME =
  { 0x608D3DF8, 0x8128, 0x4AA7, { 0xA4, 0x28, 0xF5, 0x5E, 0x49, 0x26, 0x72, 0x91 } };

namespace {

const int  kMaxLines = 5000;
const int  kTrimAt = 5500;       // not every line: trimming is the expensive bit
const UINT_PTR kSubclassId = 0x4D43;   // "MC"
const int  kPaneControlId = 0x7F21;    // nothing the splitter thinks of as one of its panes

std::wstring Wide (const char * utf8)
  {
  const int n = MultiByteToWideChar (CP_UTF8, 0, utf8, -1, NULL, 0);
  if (n <= 1)
    return std::wstring ();
  std::wstring out (n - 1, L'\0');
  MultiByteToWideChar (CP_UTF8, 0, utf8, -1, &out [0], n);
  return out;
  }

// Which pane a window handle is.  Few enough that a map is fine.
std::map<HWND, CWinA11yPane *> g_panes;
int g_iLivePanes = 0;      // for tests: that a pane object goes with its world

// An Alt+Shift key that was ours never reached DefWindowProc, so it still thinks Alt was
// pressed on its own, and letting go of Alt would open the menu bar.  That key-up is ours too.
bool g_bEatAltUp = false;

void SetAccessibleName (HWND hWnd, const wchar_t * sName)
  {
  IAccPropServices * pServices = nullptr;
  if (FAILED (CoCreateInstance (kCLSID_AccPropServices, NULL, CLSCTX_SERVER, __uuidof (IAccPropServices),
                                (void **) &pServices)))
    return;
  pServices->SetHwndPropStr (hWnd, (DWORD) OBJID_CLIENT, CHILDID_SELF, kPROPID_ACC_NAME, sName);
  pServices->Release ();
  }

bool TranslateKey (const WPARAM vk, A11yKey & key, int & ch)
  {
  ch = 0;
  switch (vk)
    {
    case VK_UP:     key = A11yKey::Up;       return true;
    case VK_DOWN:   key = A11yKey::Down;     return true;
    case VK_LEFT:   key = A11yKey::Left;     return true;
    case VK_RIGHT:  key = A11yKey::Right;    return true;
    case VK_HOME:   key = A11yKey::Home;     return true;
    case VK_END:    key = A11yKey::End;      return true;
    case VK_PRIOR:  key = A11yKey::PageUp;   return true;
    case VK_NEXT:   key = A11yKey::PageDown; return true;
    case VK_RETURN: key = A11yKey::Return;   return true;
    case VK_F6:     key = A11yKey::F6;       return true;
    case VK_F12:    key = A11yKey::F12;      return true;
    case VK_OEM_COMMA:  key = A11yKey::Character; ch = ','; return true;
    case VK_OEM_PERIOD: key = A11yKey::Character; ch = '.'; return true;
    default: break;
    }
  if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
    {
    key = A11yKey::Character;
    ch = (int) vk;
    return true;
    }
  return false;
  }

bool DispatchKey (CMUSHclientDoc * pDoc, const MSG * pMsg)
  {
  if (pMsg->message != WM_KEYDOWN && pMsg->message != WM_SYSKEYDOWN)
    return false;
  A11yKey key;
  int ch;
  if (!TranslateKey (pMsg->wParam, key, ch))
    return false;
  const bool bAlt = GetKeyState (VK_MENU) < 0;
  if (!A11y_Key (*pDoc, key, ch, GetKeyState (VK_CONTROL) < 0, bAlt, GetKeyState (VK_SHIFT) < 0))
    return false;
  if (bAlt)
    g_bEatAltUp = true;
  return true;
  }

}  // namespace

class CWinA11yPane
  {
  public:
    CWinA11yPane (HWND hOutputView, HWND hCommandBox, CMUSHclientDoc * pDoc)
      : m_hView (hOutputView), m_hCommand (hCommandBox), m_pDoc (pDoc)
      {
      g_iLivePanes++;
      LoadLibraryW (L"msftedit.dll");

      m_hWnd = CreateWindowExW (0, L"RICHEDIT50W", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS | WS_TABSTOP |
                                ES_MULTILINE | ES_READONLY | ES_NOHIDESEL | ES_AUTOVSCROLL,
                                0, 0, 0, 0, GetParent (hOutputView), (HMENU) (INT_PTR) kPaneControlId,
                                GetModuleHandleW (NULL), NULL);
      if (!m_hWnd)
        return;

      SendMessageW (m_hWnd, EM_EXLIMITTEXT, 0, 256 * 1024 * 1024);
      SendMessageW (m_hWnd, EM_SETUNDOLIMIT, 0, 0);
      SendMessageW (m_hWnd, EM_SETTARGETDEVICE, 0, 1);     // no wrapping: a line is a line

      IRichEditOle * pOle = nullptr;
      if (SendMessageW (m_hWnd, EM_GETOLEINTERFACE, 0, (LPARAM) &pOle) && pOle)
        {
        pOle->QueryInterface (kIID_ITextDocument, (void **) &m_pTom);
        pOle->Release ();
        }

      SetAccessibleName (m_hWnd, L"Output");
      SetAccessibleName (m_hCommand, L"Command");

      g_panes [m_hWnd] = this;
      SetWindowSubclass (m_hWnd, PaneProc, kSubclassId, (DWORD_PTR) this);
      SetWindowSubclass (m_hView, ViewProc, kSubclassId, (DWORD_PTR) this);
      Layout ();
      }

    ~CWinA11yPane ()
      {
      g_iLivePanes--;
      if (m_hView)
        RemoveWindowSubclass (m_hView, ViewProc, kSubclassId);
      if (m_pTom)
        m_pTom->Release ();
      if (m_hWnd)
        {
        const bool bHadFocus = GetFocus () == m_hWnd;
        RemoveWindowSubclass (m_hWnd, PaneProc, kSubclassId);
        g_panes.erase (m_hWnd);
        DestroyWindow (m_hWnd);
        if (bHadFocus && m_hCommand)
          SetFocus (m_hCommand);
        }
      }

    // The same size as the view, and behind it: nothing shows, and nothing looks different.
    void Layout ()
      {
      if (!m_hWnd || !m_hView)
        return;
      RECT r;
      GetWindowRect (m_hView, &r);
      MapWindowPoints (NULL, GetParent (m_hWnd), (POINT *) &r, 2);
      SetWindowPos (m_hWnd, m_hView, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
      }

    long StoryLength ()
      {
      long n = 0;
      ITextRange * pRange = nullptr;
      if (m_pTom && SUCCEEDED (m_pTom->Range (0, 0, &pRange)) && pRange)
        {
        pRange->GetStoryLength (&n);
        pRange->Release ();
        }
      return n;
      }

    void Insert (const std::wstring & text, long position)
      {
      ITextRange * pRange = nullptr;
      if (m_pTom && SUCCEEDED (m_pTom->Range (position, position, &pRange)) && pRange)
        {
        BSTR b = SysAllocString (text.c_str ());
        SendMessageW (m_hWnd, EM_SETREADONLY, FALSE, 0);   // the TOM refuses a read-only story
        pRange->SetText (b);
        SendMessageW (m_hWnd, EM_SETREADONLY, TRUE, 0);
        SysFreeString (b);
        pRange->Release ();
        }
      }

    void Append (const char * utf8)
      {
      if (!m_hWnd)
        return;

      if (!m_strPending.empty ())
        m_strPending += L'\r';
      m_strPending += Wide (utf8);
      m_iLines++;
      }

    // Together, because a flood is thousands of lines and each insert takes a while.
    void Flush ()
      {
      if (!m_hWnd || m_strPending.empty ())
        return;

      // A story always ends in a paragraph mark that cannot be got past, so a line
      // goes in before it, and starts with the mark that ends the line before.
      const long n = StoryLength ();
      std::wstring text = n > 1 ? L"\r" : L"";
      text += m_strPending;
      m_strPending.clear ();
      Insert (text, n > 0 ? n - 1 : 0);

      if (m_iLines > kTrimAt)
        Trim ();
      }

    void Trim ()
      {
      const long lines = (long) SendMessageW (m_hWnd, EM_GETLINECOUNT, 0, 0);
      m_iLines = lines;
      if (lines <= kMaxLines)
        return;
      const long end = (long) SendMessageW (m_hWnd, EM_LINEINDEX, lines - kMaxLines, 0);
      ITextRange * pRange = nullptr;
      if (end > 0 && m_pTom && SUCCEEDED (m_pTom->Range (0, end, &pRange)) && pRange)
        {
        SendMessageW (m_hWnd, EM_SETREADONLY, FALSE, 0);
        pRange->SetText (NULL);
        SendMessageW (m_hWnd, EM_SETREADONLY, TRUE, 0);
        pRange->Release ();
        }
      m_iLines = kMaxLines;
      }

    void Reset (const char * utf8)
      {
      if (!m_hWnd)
        return;
      m_strPending.clear ();
      std::wstring text = Wide (utf8);
      for (size_t i = 0; (i = text.find (L'\n', i)) != std::wstring::npos; )
        text [i] = L'\r';
      if (!text.empty () && text.back () == L'\r')
        text.pop_back ();
      SETTEXTEX st = { ST_DEFAULT, 1200 };
      SendMessageW (m_hWnd, EM_SETTEXTEX, (WPARAM) &st, (LPARAM) text.c_str ());
      m_iLines = (long) SendMessageW (m_hWnd, EM_GETLINECOUNT, 0, 0);
      if (m_iLines > kMaxLines)
        Trim ();
      }

    bool Focused () const
      {
      return m_hWnd && GetFocus () == m_hWnd;
      }

    void Focus (const bool bPane)
      {
      if (!bPane)
        {
        SetFocus (m_hCommand);
        return;
        }
      if (!m_hWnd)
        return;
      Flush ();

      // On arriving, at the newest line: that is what they will want to hear about.
      const long lines = (long) SendMessageW (m_hWnd, EM_GETLINECOUNT, 0, 0);
      const long start = (long) SendMessageW (m_hWnd, EM_LINEINDEX, lines > 0 ? lines - 1 : 0, 0);
      CHARRANGE range = { start, start };
      SendMessageW (m_hWnd, EM_EXSETSEL, 0, (LPARAM) &range);
      SetFocus (m_hWnd);
      SendMessageW (m_hWnd, EM_SCROLLCARET, 0, 0);
      }

    CMUSHclientDoc * Doc () const { return m_pDoc; }

  private:
    static LRESULT CALLBACK PaneProc (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR, DWORD_PTR refData)
      {
      CWinA11yPane * pPane = (CWinA11yPane *) refData;
      switch (message)
        {
        case WM_CHAR:
          // Typing belongs in the command box, as it does from the output window.
          if (wParam >= 32 && wParam != 127)
            {
            SetFocus (pPane->m_hCommand);
            PostMessageW (pPane->m_hCommand, WM_CHAR, wParam, lParam);
            return 0;
            }
          if (wParam == VK_TAB || wParam == VK_RETURN || wParam == VK_ESCAPE)
            return 0;
          break;

        case WM_KEYDOWN:
          if (wParam == VK_TAB || wParam == VK_RETURN || wParam == VK_ESCAPE)
            {
            SetFocus (pPane->m_hCommand);
            return 0;
            }
          break;

        case WM_NCDESTROY:
          RemoveWindowSubclass (hWnd, PaneProc, kSubclassId);
          g_panes.erase (hWnd);
          pPane->m_hWnd = NULL;
          break;

        default:
          break;
        }
      return DefSubclassProc (hWnd, message, wParam, lParam);
      }

    static LRESULT CALLBACK ViewProc (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR, DWORD_PTR refData)
      {
      CWinA11yPane * pPane = (CWinA11yPane *) refData;
      const LRESULT result = DefSubclassProc (hWnd, message, wParam, lParam);
      if (message == WM_WINDOWPOSCHANGED || message == WM_SIZE)
        pPane->Layout ();
      else if (message == WM_NCDESTROY)
        {
        RemoveWindowSubclass (hWnd, ViewProc, kSubclassId);
        pPane->m_hView = NULL;
        pPane->m_hCommand = NULL;
        }
      return result;
      }

    HWND m_hWnd = NULL;
    HWND m_hView;
    HWND m_hCommand;
    CMUSHclientDoc * m_pDoc;
    ITextDocument * m_pTom = nullptr;
    long m_iLines = 0;
    std::wstring m_strPending;
  };

CWinA11yPane * WinA11yPane_Create (HWND hOutputView, HWND hCommandBox, CMUSHclientDoc * pDoc)
  {
  return new CWinA11yPane (hOutputView, hCommandBox, pDoc);
  }

void WinA11yPane_Destroy (CWinA11yPane * pPane)                    { delete pPane; }
void WinA11yPane_Append (CWinA11yPane * pPane, const char * utf8)  { pPane->Append (utf8); }
void WinA11yPane_Flush (CWinA11yPane * pPane)                      { pPane->Flush (); }
void WinA11yPane_Reset (CWinA11yPane * pPane, const char * utf8)   { pPane->Reset (utf8); }
bool WinA11yPane_Focused (const CWinA11yPane * pPane)              { return pPane->Focused (); }
void WinA11yPane_Focus (CWinA11yPane * pPane, const bool bPane)    { pPane->Focus (bPane); }

bool WinA11y_CommandBoxKey (CMUSHclientDoc * pDoc, const MSG * pMsg)
  {
  return DispatchKey (pDoc, pMsg);
  }

int WinA11yPane_LiveCountForTest ()
  {
  return g_iLivePanes;
  }

bool WinA11y_PreTranslate (MSG * pMsg)
  {
  if (g_bEatAltUp && pMsg->wParam == VK_MENU)
    {
    if (pMsg->message == WM_KEYUP || pMsg->message == WM_SYSKEYUP)
      {
      g_bEatAltUp = false;
      return true;
      }
    if ((pMsg->message == WM_KEYDOWN || pMsg->message == WM_SYSKEYDOWN) && !(pMsg->lParam & (1 << 30)))
      g_bEatAltUp = false;     // pressed afresh: not the one we owe
    }

  if (g_panes.empty ())
    return false;
  if (pMsg->message != WM_KEYDOWN && pMsg->message != WM_SYSKEYDOWN && pMsg->message != WM_CHAR &&
      pMsg->message != WM_KEYUP && pMsg->message != WM_SYSKEYUP)
    return false;

  const auto it = g_panes.find (pMsg->hwnd);
  if (it == g_panes.end ())
    return false;

  if (DispatchKey (it->second->Doc (), pMsg))
    return true;

  // Ctrl+1 and so on carry on to the frame, as they would from anywhere else.  Whereas
  // moving around and copying are the pane's, and the frame's accelerators would send them
  // to the output view instead.
  const WPARAM vk = pMsg->wParam;
  const bool bCtrl = GetKeyState (VK_CONTROL) < 0;
  const bool bAlt = GetKeyState (VK_MENU) < 0;
  bool bPanes = !bAlt && !bCtrl;
  if (!bAlt && bCtrl)
    bPanes = vk == 'C' || vk == 'A' || vk == VK_INSERT || vk == VK_HOME || vk == VK_END ||
             vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN;
  // A function key is a macro, from here as well.
  if (bPanes && vk >= VK_F1 && vk <= VK_F24)
    bPanes = false;
  if (!bPanes)
    return false;

  TranslateMessage (pMsg);
  DispatchMessageW (pMsg);
  return true;
  }
