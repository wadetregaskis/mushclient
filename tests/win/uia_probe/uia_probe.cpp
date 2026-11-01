// uia_probe.cpp -- a native UI Automation client, against the running client: what a screen
// reader using UIA sees, and what it hears.
//
//   uia_probe <pid> <seconds>
//
// Finds the process's top-level window, reports the output pane and command box, walks the
// pane's text by line and word through ITextRangeProvider, then listens for notification
// events for <seconds>.  Prints one line per fact; run_uia_probe.ps1 asserts on them.
// Built and run by hand under the GUI lock: the test binary has no window to ask about.

#define _WIN32_WINNT 0x0A00
#define UNICODE
#include <windows.h>
#include <uiautomation.h>
#include <cstdio>
#include <string>

#pragma comment (lib, "ole32.lib")
#pragma comment (lib, "oleaut32.lib")
#pragma comment (lib, "user32.lib")

struct FindWindowData { DWORD pid; HWND found; };

static BOOL CALLBACK EachTop (HWND h, LPARAM l)
  {
  FindWindowData * d = (FindWindowData *) l;
  DWORD pid = 0;
  GetWindowThreadProcessId (h, &pid);
  if (pid == d->pid && IsWindowVisible (h) && !GetWindow (h, GW_OWNER))
    {
    d->found = h;
    return FALSE;
    }
  return TRUE;
  }

static void Print (const char * label, BSTR b)
  {
  printf ("%s: ", label);
  if (b)
    {
    char buf [2048];
    int n = WideCharToMultiByte (CP_UTF8, 0, b, -1, buf, sizeof buf - 1, NULL, NULL);
    buf [n > 0 ? n : 0] = 0;
    for (char * p = buf; *p; p++)
      if (*p == '\r' || *p == '\n')
        *p = '|';
    fputs (buf, stdout);
    }
  printf ("\n");
  fflush (stdout);
  }

class Listener : public IUIAutomationNotificationEventHandler
  {
  LONG m_refs = 1;
  public:
  int count = 0;
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
  HRESULT STDMETHODCALLTYPE HandleNotificationEvent (IUIAutomationElement *, NotificationKind kind,
                                                     NotificationProcessing processing,
                                                     BSTR text, BSTR activity) override
    {
    count++;
    printf ("notification kind=%d processing=%d\n", (int) kind, (int) processing);
    Print ("  text", text);
    Print ("  activity", activity);
    return S_OK;
    }
  };

static IUIAutomationTextRange * Walk (IUIAutomationTextRange * range, TextUnit unit, int n, const char * label)
  {
  IUIAutomationTextRange * r = nullptr;
  range->Clone (&r);
  r->MoveEndpointByRange (TextPatternRangeEndpoint_End, r, TextPatternRangeEndpoint_Start);
  for (int i = 0; i < n; i++)
    {
    r->ExpandToEnclosingUnit (unit);
    BSTR b = nullptr;
    r->GetText (200, &b);
    Print (label, b);
    SysFreeString (b);
    int moved = 0;
    r->Move (unit, 1, &moved);
    if (!moved)
      break;
    }
  return r;
  }

int wmain (int argc, wchar_t ** argv)
  {
  if (argc < 3)
    return 2;
  const DWORD pid = (DWORD) _wtoi (argv [1]);
  const int seconds = _wtoi (argv [2]);

  CoInitializeEx (NULL, COINIT_MULTITHREADED);
  IUIAutomation5 * uia = nullptr;
  if (FAILED (CoCreateInstance (__uuidof (CUIAutomation8), NULL, CLSCTX_INPROC_SERVER, __uuidof (IUIAutomation5), (void **) &uia)))
    {
    printf ("no IUIAutomation5\n");
    return 1;
    }

  FindWindowData d = { pid, NULL };
  EnumWindows (EachTop, (LPARAM) &d);
  if (!d.found)
    {
    printf ("no window for %lu\n", pid);
    return 1;
    }

  IUIAutomationElement * root = nullptr;
  uia->ElementFromHandle (d.found, &root);
  BSTR b = nullptr;
  root->get_CurrentName (&b);
  Print ("main", b);
  SysFreeString (b);

  // the pane is a Document with this name
  VARIANT name;
  VariantInit (&name);
  name.vt = VT_BSTR;
  name.bstrVal = SysAllocString (L"Output");
  IUIAutomationCondition * cond = nullptr;
  uia->CreatePropertyCondition (UIA_NamePropertyId, name, &cond);
  IUIAutomationElement * pane = nullptr;
  root->FindFirst (TreeScope_Descendants, cond, &pane);
  printf ("pane found: %s\n", pane ? "yes" : "no");

  if (pane)
    {
    CONTROLTYPEID type = 0;
    pane->get_CurrentControlType (&type);
    printf ("pane type: %d (document is %d)\n", type, UIA_DocumentControlTypeId);
    IUIAutomationTextPattern * text = nullptr;
    pane->GetCurrentPatternAs (UIA_TextPatternId, __uuidof (IUIAutomationTextPattern), (void **) &text);
    printf ("text pattern: %s\n", text ? "yes" : "no");
    if (text)
      {
      IUIAutomationTextRange * doc = nullptr;
      text->get_DocumentRange (&doc);
      Walk (doc, TextUnit_Line, 4, "line");
      Walk (doc, TextUnit_Word, 4, "word");
      Walk (doc, TextUnit_Character, 3, "char");
      IUIAutomationTextRangeArray * sel = nullptr;
      text->GetSelection (&sel);
      int count = 0;
      if (sel)
        sel->get_Length (&count);
      printf ("selection ranges: %d\n", count);
      }
    }

  Listener listener;
  HRESULT hr = uia->AddNotificationEventHandler (root, TreeScope_Subtree, NULL, &listener);
  printf ("listening: 0x%08lx\n", hr);
  fflush (stdout);
  Sleep (seconds * 1000);
  printf ("notifications: %d\n", listener.count);
  uia->RemoveNotificationEventHandler (root, &listener);
  return 0;
  }
