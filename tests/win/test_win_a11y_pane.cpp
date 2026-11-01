// tests/win/test_win_a11y_pane.cpp -- the text pane, as a real RichEdit in real windows.
// What a screen reader reads is the control's text and caret, so those are what is asserted.
// NVDA reading it is tested by hand: docs/ACCESSIBILITY.md says how.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "a11y.h"
#include "pal/win_a11y_pane.h"
#include "pal/ispeech.h"
#include "dialogs/global_prefs/GlobalPrefs.h"
#include "dialogs/global_prefs/GlobalPrefsSheet.h"

#include <UIAutomation.h>
#include <functional>
#include <thread>
#include <oleacc.h>
#include <richedit.h>
#include <string>

namespace {

struct PaneFixture
  {
  HWND hParent = NULL, hView = NULL, hCommand = NULL, hRich = NULL;
  CWinA11yPane * pPane = nullptr;

  PaneFixture ()
    {
    hParent = CreateWindowExW (0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 600, 400, NULL, NULL, NULL, NULL);
    hView = CreateWindowExW (0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 600, 300, hParent, NULL, NULL, NULL);
    hCommand = CreateWindowExW (0, L"EDIT", L"", WS_CHILD | WS_VISIBLE, 0, 300, 600, 50, hParent, NULL, NULL, NULL);
    pPane = WinA11yPane_Create (hView, hCommand, nullptr);
    hRich = FindWindowExW (hParent, NULL, L"RICHEDIT50W", NULL);
    }

  ~PaneFixture ()
    {
    WinA11yPane_Destroy (pPane);
    DestroyWindow (hParent);
    }

  long lines () const
    {
    WinA11yPane_Flush (pPane);
    return (long) SendMessageW (hRich, EM_GETLINECOUNT, 0, 0);
    }

  std::wstring text () const
    {
    const int n = GetWindowTextLengthW (hRich);
    std::wstring s (n + 1, L'\0');
    GetWindowTextW (hRich, &s [0], n + 1);
    s.resize (wcslen (s.c_str ()));
    return s;
    }

  std::wstring line (long i) const
    {
    WinA11yPane_Flush (pPane);
    wchar_t buf [512] = L"";
    * (WORD *) buf = 500;
    const LRESULT n = SendMessageW (hRich, EM_GETLINE, i, (LPARAM) buf);
    std::wstring s (buf, n);
    while (!s.empty () && (s.back () == L'\r' || s.back () == L'\n'))
      s.pop_back ();
    return s;
    }
  };

}  // namespace

TEST (WinA11yPane, ItIsThereSameSizeAndBehindTheView)
{
  PaneFixture f;
  ASSERT_TRUE (f.hRich != NULL);

  RECT rView, rRich;
  GetWindowRect (f.hView, &rView);
  GetWindowRect (f.hRich, &rRich);
  EXPECT_EQ (rView.left, rRich.left);
  EXPECT_EQ (rView.top, rRich.top);
  EXPECT_EQ (rView.right, rRich.right);
  EXPECT_EQ (rView.bottom, rRich.bottom);

  // Immediately behind it in the Z order, so the view's pixels are what shows
  EXPECT_TRUE (GetWindow (f.hView, GW_HWNDNEXT) == f.hRich);

  // It follows the view
  SetWindowPos (f.hView, NULL, 0, 0, 500, 200, SWP_NOZORDER);
  GetWindowRect (f.hView, &rView);
  GetWindowRect (f.hRich, &rRich);
  EXPECT_EQ (rView.right, rRich.right);
  EXPECT_EQ (rView.bottom, rRich.bottom);
}

TEST (WinA11yPane, LinesAppendAndAreReadOnlyAfterwards)
{
  PaneFixture f;
  WinA11yPane_Append (f.pPane, "first");
  WinA11yPane_Append (f.pPane, "second line");
  EXPECT_EQ (2L, f.lines ());
  EXPECT_TRUE (f.line (0) == L"first");
  EXPECT_TRUE (f.line (1) == L"second line");
  EXPECT_TRUE ((GetWindowLongW (f.hRich, GWL_STYLE) & ES_READONLY) != 0);
}

TEST (WinA11yPane, TextIsUtf8ToUtf16)
{
  PaneFixture f;
  WinA11yPane_Append (f.pPane, "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80");
  const std::wstring got = f.line (0);
  ASSERT_EQ (10u, got.size ());
  EXPECT_EQ (0x00E9, (int) got [3]);
  EXPECT_EQ (0x65E5, (int) got [5]);
  EXPECT_EQ (0x672C, (int) got [6]);
  EXPECT_EQ (0xD83D, (int) got [8]);
  EXPECT_EQ (0xDE00, (int) got [9]);
}

TEST (WinA11yPane, TheCaretStaysPutWhileLinesArrive)
{
  PaneFixture f;
  for (int i = 0; i < 20; i++)
    WinA11yPane_Append (f.pPane, ("line " + std::to_string (i)).c_str ());
  WinA11yPane_Flush (f.pPane);

  const long start = (long) SendMessageW (f.hRich, EM_LINEINDEX, 5, 0);
  CHARRANGE range = { start + 2, start + 2 };
  SendMessageW (f.hRich, EM_EXSETSEL, 0, (LPARAM) &range);

  for (int i = 0; i < 100; i++)
    {
    WinA11yPane_Append (f.pPane, "more");
    WinA11yPane_Flush (f.pPane);
    }

  CHARRANGE after = { 0, 0 };
  SendMessageW (f.hRich, EM_EXGETSEL, 0, (LPARAM) &after);
  EXPECT_EQ (range.cpMin, after.cpMin);
  EXPECT_EQ (range.cpMax, after.cpMax);
}

TEST (WinA11yPane, FiveThousandLinesAndTrimmedAndTheCaretStaysOnItsLine)
{
  PaneFixture f;
  for (int i = 0; i < 5400; i++)
    WinA11yPane_Append (f.pPane, ("line " + std::to_string (i)).c_str ());
  EXPECT_EQ (5400L, f.lines ());     // flushed in one: trimming is at a flush

  // the caret on line 5300
  const long start = (long) SendMessageW (f.hRich, EM_LINEINDEX, 5300, 0);
  CHARRANGE range = { start, start };
  SendMessageW (f.hRich, EM_EXSETSEL, 0, (LPARAM) &range);

  for (int i = 5400; i < 5600; i++)
    WinA11yPane_Append (f.pPane, ("line " + std::to_string (i)).c_str ());

  EXPECT_LE (f.lines (), 5500L);
  EXPECT_GE (f.lines (), 5000L);
  EXPECT_TRUE (f.line (f.lines () - 1) == L"line 5599");

  CHARRANGE after = { 0, 0 };
  SendMessageW (f.hRich, EM_EXGETSEL, 0, (LPARAM) &after);
  const long caretLine = (long) SendMessageW (f.hRich, EM_LINEFROMCHAR, after.cpMin, 0);
  EXPECT_TRUE (f.line (caretLine) == L"line 5300");
}

TEST (WinA11yPane, ResetReplacesEverything)
{
  PaneFixture f;
  WinA11yPane_Append (f.pPane, "old");
  WinA11yPane_Reset (f.pPane, "one\ntwo\nthree\n");
  EXPECT_EQ (3L, f.lines ());
  EXPECT_TRUE (f.line (2) == L"three");
  WinA11yPane_Append (f.pPane, "four");
  EXPECT_TRUE (f.line (3) == L"four");
}

TEST (WinA11yPane, ALongLineIsOneLine)
{
  PaneFixture f;
  const std::string longLine (2000, 'x');
  WinA11yPane_Append (f.pPane, longLine.c_str ());
  EXPECT_EQ (1L, f.lines ());     // no wrapping: a screen reader's line is the MUD's line
}

// What makes a label in en.rc work: the static directly before a control names it, even one
// of no size, and that is what check_dialog_labels.py is keeping true.
TEST (WinA11yPane, AZeroSizeStaticNamesTheEditAfterIt)
{
  CoInitializeEx (NULL, COINIT_APARTMENTTHREADED);

  // Created top-down: the first sibling created is the top of the Z order, as in a dialog
  HWND hParent = CreateWindowExW (0, L"#32770", L"", WS_POPUP, 0, 0, 300, 200, NULL, NULL, NULL, NULL);
  ASSERT_TRUE (hParent != NULL);
  CreateWindowExW (0, L"STATIC", L"Name of the thing:", WS_CHILD | WS_VISIBLE, 10, 10, 0, 0, hParent, NULL, NULL, NULL);
  HWND hEdit = CreateWindowExW (0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 10, 10, 100, 20, hParent, NULL, NULL, NULL);
  ShowWindow (hParent, SW_SHOWNA);

  IAccessible * pAcc = nullptr;
  ASSERT_TRUE (SUCCEEDED (AccessibleObjectFromWindow (hEdit, (DWORD) OBJID_CLIENT, __uuidof (IAccessible), (void **) &pAcc)));
  VARIANT self;
  VariantInit (&self);
  self.vt = VT_I4;
  self.lVal = CHILDID_SELF;
  BSTR name = NULL;
  EXPECT_TRUE (SUCCEEDED (pAcc->get_accName (self, &name)));
  EXPECT_TRUE (name != NULL);
  if (name)
    {
    const std::wstring got = name;
    EXPECT_TRUE (got.find (L"Name of the thing") != std::wstring::npos);
    SysFreeString (name);
    }
  pAcc->Release ();
  DestroyWindow (hParent);
}

// The Accessibility page of Global Preferences, created for real from the real resource.
TEST (WinA11yPrefs, ThePageCreatesAndRoundTrips)
{
  HMODULE hDll = LoadLibraryExW (L"..\WinRel-ARM64\locale\en.dll", NULL, LOAD_LIBRARY_AS_DATAFILE);
  if (!hDll)
    hDll = LoadLibraryExW (L"locale\en.dll", NULL, LOAD_LIBRARY_AS_DATAFILE);
  if (!hDll)
    return;    // only the x64 and Win32 runs that have no en.dll where this looks
  HINSTANCE hSaved = AfxGetResourceHandle ();
  AfxSetResourceHandle (hDll);

  CGlobalPrefsSheet sheet;
  EXPECT_EQ (13, sheet.GetPageCount ());

  CGlobalPrefsP14 page;
  page.m_iSpeechMode = eSpeechAlways;
  page.m_iTextPane = eTextPaneNever;
  page.m_iRate = -3;
  page.m_iVolume = 55;
  HWND hParent = CreateWindowExW (0, L"STATIC", L"", WS_POPUP, 0, 0, 500, 400, NULL, NULL, NULL, NULL);
  CWnd parent;
  parent.Attach (hParent);
  const BOOL bCreated = page.Create (CGlobalPrefsP14::IDD, &parent);
  EXPECT_TRUE (bCreated != FALSE);
  if (bCreated)
    {
    EXPECT_EQ (eSpeechAlways, ((CComboBox *) page.GetDlgItem (IDC_SPEECH_MODE))->GetCurSel ());
    page.m_iVolume = 0;
    EXPECT_TRUE (page.UpdateData (TRUE) != FALSE);
    EXPECT_EQ (55, page.m_iVolume);
    EXPECT_EQ (-3, page.m_iRate);
    EXPECT_EQ (eTextPaneNever, page.m_iTextPane);
    page.DestroyWindow ();
    }
  parent.Detach ();
  DestroyWindow (hParent);
  AfxSetResourceHandle (hSaved);
  FreeLibrary (hDll);
}


// ---------------------------------------------------------------- UI Automation notifications

namespace pal { bool RaiseUiaNotification (HWND hWnd, const char * utf8, int processing); }

namespace {

class NotificationListener : public IUIAutomationNotificationEventHandler
  {
  LONG m_refs = 1;
  public:
  volatile LONG count = 0;
  std::wstring text;
  ULONG STDMETHODCALLTYPE AddRef () override { return InterlockedIncrement (&m_refs); }
  ULONG STDMETHODCALLTYPE Release () override { return InterlockedDecrement (&m_refs); }
  HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void ** ppv) override
    {
    if (riid == __uuidof (IUnknown) || riid == __uuidof (IUIAutomationNotificationEventHandler))
      {
      *ppv = this;
      AddRef ();
      return S_OK;
      }
    *ppv = nullptr;
    return E_NOINTERFACE;
    }
  HRESULT STDMETHODCALLTYPE HandleNotificationEvent (IUIAutomationElement *, NotificationKind,
                                                     NotificationProcessing, BSTR t, BSTR) override
    {
    text = t ? t : L"";
    InterlockedIncrement (&count);
    return S_OK;
    }
  };

void Pump (const DWORD ms, const std::function<bool ()> & done)
  {
  const DWORD start = GetTickCount ();
  while (GetTickCount () - start < ms && !done ())
    {
    MSG msg;
    while (PeekMessageW (&msg, NULL, 0, 0, PM_REMOVE))
      {
      TranslateMessage (&msg);
      DispatchMessageW (&msg);
      }
    Sleep (10);
    }
  }

}  // namespace

// What Narrator gets.  The listener is a real UIA client on its own thread, as a client would
// be in its own process.  The window is off the screen, so it shows nobody anything.
TEST (WinUia, ANotificationReachesAClient)
{
  HWND hWnd = CreateWindowExW (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"a11y test", WS_POPUP,
                               -3000, -3000, 100, 100, NULL, NULL, NULL, NULL);
  ASSERT_TRUE (hWnd != NULL);
  ShowWindow (hWnd, SW_SHOWNA);

  NotificationListener listener;
  volatile LONG ready = 0, finished = 0, stop = 0;
  std::thread client ([&]
    {
    CoInitializeEx (NULL, COINIT_MULTITHREADED);
    IUIAutomation5 * uia = nullptr;
    IUIAutomationElement * element = nullptr;
    if (SUCCEEDED (CoCreateInstance (__uuidof (CUIAutomation8), NULL, CLSCTX_INPROC_SERVER,
                                     __uuidof (IUIAutomation5), (void **) &uia)) &&
        SUCCEEDED (uia->ElementFromHandle (hWnd, &element)) && element &&
        SUCCEEDED (uia->AddNotificationEventHandler (element, TreeScope_Subtree, NULL, &listener)))
      {
      InterlockedExchange (&ready, 1);
      while (!stop)
        Sleep (10);
      uia->RemoveNotificationEventHandler (element, &listener);
      }
    else
      InterlockedExchange (&ready, -1);
    if (element)
      element->Release ();
    if (uia)
      uia->Release ();
    CoUninitialize ();
    InterlockedExchange (&finished, 1);
    });

  Pump (10000, [&] { return ready != 0; });
  EXPECT_EQ (1, (int) ready);
  if (ready == 1)
    {
    Pump (300, [] { return false; });
    EXPECT_TRUE (pal::RaiseUiaNotification (hWnd, "caf\xC3\xA9 is open", 2));
    Pump (5000, [&] { return listener.count > 0; });
    EXPECT_EQ (1, (int) listener.count);
    EXPECT_TRUE (listener.text.size () == 12 && listener.text [3] == 0xE9);
    }
  InterlockedExchange (&stop, 1);
  Pump (5000, [&] { return finished != 0; });
  client.join ();
  DestroyWindow (hWnd);
}
// The system voice, for real, at volume 0: nobody hears a test.
TEST (WinSapi, ItSpeaksAndItsVoicesAreThere)
{
  pal::ISpeechPlatform * pSaved = pal::g_speechPlatform;
  pal::InstallPlatformSpeech ();
  pal::ISpeech * pVoice = pal::g_speechPlatform->ownVoice ();
  if (!pVoice)
    {
    pal::g_speechPlatform = pSaved;
    return;    // Windows without SAPI, which some server editions are
    }
  pVoice->setVolume (0);
  const std::vector<std::string> voices = pVoice->voices ();
  EXPECT_FALSE (voices.empty ());
  if (!voices.empty ())
    pVoice->setVoice (voices [0].c_str ());
  pVoice->setVoice ("");
  pVoice->setRate (5);
  pVoice->setRate (0);
  EXPECT_TRUE ((pVoice->capabilities () & pal::kSpeechKnowsBusy) != 0);
  EXPECT_TRUE (pVoice->speak ("test", pal::SpeechPriority::Normal, false));
  (void) pVoice->isBusy ();
  pVoice->stop ();
  pal::g_speechPlatform = pSaved;
}
