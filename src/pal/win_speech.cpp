// pal/win_speech.cpp -- pal::ISpeech on Windows.
//
// Screen readers, first one found:
//
//   NVDA    nvdaControllerClient.dll, beside the exe if it is in the package.  Loaded
//           when NVDA looks to be running, and not before.  The newer entry points are
//           looked up one at a time: isSpeaking is only there from NVDA 2026.3.
//   JAWS    the COM object it installs, FreedomSci.JawsApi: SayString.  Nothing to ship.
//           NOT TESTED: there is no JAWS on the machine this was written on.
//   Others  UI Automation notifications, for Narrator and anything else that sets
//           SPI_SETSCREENREADER.  From Windows 10 1709: looked up with GetProcAddress
//           (this builds for 0x0501).  NVDA ignores these when the app is not focused
//           (nvaccess/nvda#16862), which is one reason it has its own backend.
//
// The system voice is SAPI, created the first time someone asks for it, and not before: naming
// the voices, as the preferences do, reads SAPI's tokens and makes no voice.  ExitInstance calls
// shutdown () to release the voice and JAWS's object while COM is still there.
//
// Everything runs on the main thread.  COM is initialised for it on first use.

#include "stdafx.h"

#include "pal/ispeech.h"

// sapi.h says byte, which stdafx.h's "using namespace std" makes ambiguous with std::byte.
#define byte ::byte
#include <sapi.h>
#undef byte

#include <UIAutomationCore.h>
#include <string>

namespace pal {

namespace {

// LoadLibraryEx's, which a 0x0501 build does not see.
const DWORD kSearchDllLoadDir = 0x00000100;    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
const DWORD kSearchSystem32   = 0x00000800;    // LOAD_LIBRARY_SEARCH_SYSTEM32

std::wstring Wide (const char * utf8)
  {
  const int n = MultiByteToWideChar (CP_UTF8, 0, utf8, -1, NULL, 0);
  if (n <= 1)
    return std::wstring ();
  std::wstring out (n - 1, L'\0');
  MultiByteToWideChar (CP_UTF8, 0, utf8, -1, &out [0], n);
  return out;
  }

std::string Narrow (const wchar_t * wide)
  {
  const int n = WideCharToMultiByte (CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
  if (n <= 1)
    return std::string ();
  std::string out (n - 1, '\0');
  WideCharToMultiByte (CP_UTF8, 0, wide, -1, &out [0], n, NULL, NULL);
  return out;
  }

void EnsureCom ()
  {
  static bool bDone = false;
  if (!bDone)
    {
    bDone = true;
    CoInitializeEx (NULL, COINIT_APARTMENTTHREADED);   // MFC may have done so: that is fine
    }
  }

// The directory the exe is in, ending in a backslash.
std::wstring ExeDirectory ()
  {
  wchar_t path [MAX_PATH * 2] = L"";
  GetModuleFileNameW (NULL, path, MAX_PATH * 2);
  std::wstring dir = path;
  const size_t slash = dir.find_last_of (L"\\/");
  return slash == std::wstring::npos ? std::wstring () : dir.substr (0, slash + 1);
  }

// ---------------------------------------------------------------- NVDA

typedef DWORD (__stdcall * NvdaNoArgs) (void);
typedef DWORD (__stdcall * NvdaText) (const wchar_t *);
typedef DWORD (__stdcall * NvdaIsSpeaking) (unsigned char *);

class NvdaSpeech : public ISpeech
  {
  public:
    // False if there is no DLL.  Only tried once: it will not appear later.
    bool Load ()
      {
      if (m_bTried)
        return m_hDll != NULL;
      m_bTried = true;
      // Beside the exe only: this is not a name to let the search path find, and nor are
      // the DLLs it imports, which come from its own directory or System32.
      const std::wstring path = ExeDirectory () + L"nvdaControllerClient.dll";
      m_hDll = LoadLibraryExW (path.c_str (), NULL, kSearchDllLoadDir | kSearchSystem32);
      if (!m_hDll && GetLastError () == ERROR_INVALID_PARAMETER)    // Windows 7 without KB2533623
        m_hDll = LoadLibraryExW (path.c_str (), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (!m_hDll)
        return false;
      m_testIfRunning = (NvdaNoArgs)     GetProcAddress (m_hDll, "nvdaController_testIfRunning");
      m_cancelSpeech  = (NvdaNoArgs)     GetProcAddress (m_hDll, "nvdaController_cancelSpeech");
      m_speakText     = (NvdaText)       GetProcAddress (m_hDll, "nvdaController_speakText");
      m_brailleMessage= (NvdaText)       GetProcAddress (m_hDll, "nvdaController_brailleMessage");
      m_isSpeaking    = (NvdaIsSpeaking) GetProcAddress (m_hDll, "nvdaController_isSpeaking");
      if (!m_testIfRunning || !m_cancelSpeech || !m_speakText)
        {
        FreeLibrary (m_hDll);
        m_hDll = NULL;
        return false;
        }
      return true;
      }

    bool Running ()
      {
      if (!Load () || m_testIfRunning () != 0)
        return false;

      // An older NVDA answers RPC_S_PROCNUM_OUT_OF_RANGE or similar.  It can change
      // version between polls, if the player restarts it, so this is asked each time.
      unsigned char speaking = 0;
      m_bKnowsBusy = m_isSpeaking && m_isSpeaking (&speaking) == 0;
      return true;
      }

    unsigned capabilities () const override
      {
      return kSpeechScreenReader | kSpeechCanStop | kSpeechBraille | (m_bKnowsBusy ? kSpeechKnowsBusy : 0);
      }

    const char * name () const override { return "NVDA"; }

    bool speak (const char * utf8, SpeechPriority, bool bFlush) override
      {
      if (bFlush)
        m_cancelSpeech ();
      m_bBusyKnown = false;
      return m_speakText (Wide (utf8).c_str ()) == 0;
      }

    void stop () override
      {
      m_bBusyKnown = false;
      m_cancelSpeech ();
      }

    // A call into NVDA's process: while lines wait it is asked on every tick, so the answer
    // is kept for a little while.  Saying something makes it stale.
    bool isBusy () override
      {
      if (!m_bKnowsBusy)
        return false;
      const DWORD now = GetTickCount ();
      if (m_bBusyKnown && now - m_iBusyAskedAt < kBusyCacheMs)
        return m_bBusy;
      unsigned char speaking = 0;
      m_bBusy = m_isSpeaking (&speaking) == 0 && speaking;
      m_bBusyKnown = true;
      m_iBusyAskedAt = now;
      return m_bBusy;
      }

    void braille (const char * utf8) override
      {
      if (m_brailleMessage)
        m_brailleMessage (Wide (utf8).c_str ());
      }

  private:
    static const DWORD kBusyCacheMs = 150;

    bool m_bTried = false;
    bool m_bKnowsBusy = false;
    bool m_bBusyKnown = false;
    bool m_bBusy = false;
    DWORD m_iBusyAskedAt = 0;
    HMODULE m_hDll = NULL;
    NvdaNoArgs m_testIfRunning = nullptr;
    NvdaNoArgs m_cancelSpeech = nullptr;
    NvdaText m_speakText = nullptr;
    NvdaText m_brailleMessage = nullptr;
    NvdaIsSpeaking m_isSpeaking = nullptr;
  };

// ---------------------------------------------------------------- JAWS

class JawsSpeech : public ISpeech
  {
  public:
    ~JawsSpeech () override { Release (); }

    bool Running ()
      {
      const HWND hJaws = FindWindowW (L"JFWUI2", NULL);
      if (!hJaws)
        {
        Release ();
        return false;
        }
      // A JAWS restarted since the last look has a new window, and the old object is dead.
      if (m_pApi && hJaws != m_hJaws)
        Release ();
      if (m_pApi)
        return true;
      m_hJaws = hJaws;

      EnsureCom ();
      CLSID clsid;
      if (FAILED (CLSIDFromProgID (L"FreedomSci.JawsApi", &clsid)))
        return false;
      if (FAILED (CoCreateInstance (clsid, NULL, CLSCTX_ALL, IID_IDispatch, (void **) &m_pApi)))
        {
        m_pApi = nullptr;
        return false;
        }
      LPOLESTR sayString = const_cast <LPOLESTR> (L"SayString");
      LPOLESTR stopSpeech = const_cast <LPOLESTR> (L"StopSpeech");
      m_pApi->GetIDsOfNames (IID_NULL, &sayString, 1, LOCALE_USER_DEFAULT, &m_idSay);
      m_pApi->GetIDsOfNames (IID_NULL, &stopSpeech, 1, LOCALE_USER_DEFAULT, &m_idStop);
      return true;
      }

    unsigned capabilities () const override { return kSpeechScreenReader | kSpeechCanStop; }
    const char * name () const override { return "JAWS"; }

    bool speak (const char * utf8, SpeechPriority, bool bFlush) override
      {
      if (!m_pApi)
        return false;
      VARIANT args [2];    // right to left
      VariantInit (&args [0]);
      VariantInit (&args [1]);
      args [0].vt = VT_BOOL;
      args [0].boolVal = bFlush ? VARIANT_TRUE : VARIANT_FALSE;
      args [1].vt = VT_BSTR;
      args [1].bstrVal = SysAllocString (Wide (utf8).c_str ());
      DISPPARAMS params = { args, NULL, 2, 0 };
      const HRESULT hr = m_pApi->Invoke (m_idSay, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                                         &params, NULL, NULL, NULL);
      SysFreeString (args [1].bstrVal);
      // Its process gone: let it go, and the next look makes a new one if there is a JAWS.
      if (FAILED (hr) && hr != DISP_E_EXCEPTION)
        Release ();
      return SUCCEEDED (hr);
      }

    void stop () override
      {
      if (!m_pApi)
        return;
      DISPPARAMS params = { NULL, NULL, 0, 0 };
      m_pApi->Invoke (m_idStop, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &params, NULL, NULL, NULL);
      }

    void Release ()
      {
      if (m_pApi)
        m_pApi->Release ();
      m_pApi = nullptr;
      m_hJaws = NULL;
      }

    bool HoldsCom () const { return m_pApi != nullptr; }

  private:
    HWND m_hJaws = NULL;
    IDispatch * m_pApi = nullptr;
    DISPID m_idSay = DISPID_UNKNOWN;
    DISPID m_idStop = DISPID_UNKNOWN;
  };

}  // namespace

// tests reach this one directly: with a client listening, in tests/win/test_win_a11y_pane.cpp
bool RaiseUiaNotification (HWND hWnd, const char * utf8, int processing);

namespace {

// ---------------------------------------------------------------- UI Automation

// From uiautomationcoreapi.h, which hides them from a 0x0501 build.
enum { kNotificationKindOther = 4 };
enum
  {
  kNotificationProcessingImportantAll = 0,
  kNotificationProcessingAll = 2,
  kNotificationProcessingMostRecent = 3
  };

typedef HRESULT (WINAPI * RaiseNotification) (IRawElementProviderSimple *, int, int, BSTR, BSTR);
typedef HRESULT (WINAPI * HostProviderFromHwnd) (HWND, IRawElementProviderSimple **);

RaiseNotification g_raise = nullptr;
HostProviderFromHwnd g_hostProvider = nullptr;

bool LoadUia ()
  {
  static bool bTried = false;
  if (!bTried)
    {
    bTried = true;
    if (HMODULE hDll = LoadLibraryW (L"uiautomationcore.dll"))
      {
      g_raise = (RaiseNotification) GetProcAddress (hDll, "UiaRaiseNotificationEvent");
      g_hostProvider = (HostProviderFromHwnd) GetProcAddress (hDll, "UiaHostProviderFromHwnd");
      }
    }
  return g_raise && g_hostProvider;
  }

// The element a notification comes from.  The window's own host provider will not do: raising
// on that returns S_OK and no client hears anything (measured, with tests/win/uia_probe).
class NotificationSource : public IRawElementProviderSimple
  {
  public:
    explicit NotificationSource (HWND hWnd) : m_hWnd (hWnd) {}

    ULONG STDMETHODCALLTYPE AddRef () override { return InterlockedIncrement (&m_refs); }
    ULONG STDMETHODCALLTYPE Release () override
      {
      const LONG refs = InterlockedDecrement (&m_refs);
      if (!refs)
        delete this;
      return refs;
      }
    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void ** ppv) override
      {
      if (riid == __uuidof (IUnknown) || riid == __uuidof (IRawElementProviderSimple))
        {
        *ppv = static_cast <IRawElementProviderSimple *> (this);
        AddRef ();
        return S_OK;
        }
      *ppv = nullptr;
      return E_NOINTERFACE;
      }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions (ProviderOptions * pOptions) override
      {
      *pOptions = (ProviderOptions) (ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
      return S_OK;
      }
    HRESULT STDMETHODCALLTYPE GetPatternProvider (PATTERNID, IUnknown ** ppResult) override
      {
      *ppResult = nullptr;
      return S_OK;
      }
    HRESULT STDMETHODCALLTYPE GetPropertyValue (PROPERTYID, VARIANT * pResult) override
      {
      pResult->vt = VT_EMPTY;   // the window's own host provider answers for all of it
      return S_OK;
      }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider (IRawElementProviderSimple ** ppResult) override
      {
      return g_hostProvider (m_hWnd, ppResult);
      }

  private:
    LONG m_refs = 1;
    HWND m_hWnd;
  };

class UiaSpeech : public ISpeech
  {
  public:
    bool Load () { return LoadUia (); }

    unsigned capabilities () const override { return kSpeechScreenReader; }
    const char * name () const override { return "UI Automation"; }

    bool speak (const char * utf8, SpeechPriority priority, bool bFlush) override
      {
      CWnd * pMain = AfxGetMainWnd ();
      if (!pMain)
        return false;
      const int processing = bFlush ? kNotificationProcessingMostRecent :
                             priority == SpeechPriority::Assertive ? kNotificationProcessingImportantAll :
                                                                     kNotificationProcessingAll;
      return RaiseUiaNotification (pMain->GetSafeHwnd (), utf8, processing);
      }

    void stop () override {}    // a notification cannot be called back
  };

// ---------------------------------------------------------------- SAPI

class SapiSpeech : public ISpeech
  {
  public:
    ~SapiSpeech () override { Release (); }

    // Before COM goes.  Asked for again, it starts afresh.
    void Release ()
      {
      if (m_pVoice)
        m_pVoice->Release ();
      m_pVoice = nullptr;
      m_bTried = false;
      }

    bool HoldsCom () const { return m_pVoice != nullptr; }

    bool Load ()
      {
      if (m_bTried)
        return m_pVoice != nullptr;
      m_bTried = true;
      EnsureCom ();
      if (FAILED (CoCreateInstance (__uuidof (SpVoice), NULL, CLSCTX_ALL, __uuidof (ISpVoice),
                                    (void **) &m_pVoice)))
        m_pVoice = nullptr;
      return m_pVoice != nullptr;
      }

    unsigned capabilities () const override
      {
      return kSpeechKnowsBusy | kSpeechCanStop | kSpeechVoices;
      }

    const char * name () const override { return "SAPI"; }

    bool speak (const char * utf8, SpeechPriority, bool bFlush) override
      {
      DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML;
      if (bFlush)
        flags |= SPF_PURGEBEFORESPEAK;
      return SUCCEEDED (m_pVoice->Speak (Wide (utf8).c_str (), flags, NULL));
      }

    void stop () override
      {
      m_pVoice->Speak (L"", SPF_ASYNC | SPF_PURGEBEFORESPEAK, NULL);
      }

    bool isBusy () override
      {
      SPVOICESTATUS status;
      if (FAILED (m_pVoice->GetStatus (&status, NULL)))
        return false;
      return status.dwRunningState != SPRS_DONE && status.dwRunningState != 0;
      }

    void setRate (int rate) override
      {
      m_pVoice->SetRate (rate < -10 ? -10 : rate > 10 ? 10 : rate);
      }

    void setVolume (int volume) override
      {
      m_pVoice->SetVolume ((USHORT) (volume < 0 ? 0 : volume > 100 ? 100 : volume));
      }

    std::vector<std::string> voices () override { return Installed (); }

    void setVoice (const char * sName) override
      {
      const std::string wanted = sName;
      if (wanted.empty ())
        {
        m_pVoice->SetVoice (NULL);
        return;
        }
      ForEachVoice ([&] (ISpObjectToken * pToken, const std::string & name)
        {
        if (name != wanted)
          return false;
        m_pVoice->SetVoice (pToken);
        return true;
        });
      }

    // The installed voices, without making one: SAPI keeps them in the registry, as tokens.
    static std::vector<std::string> Installed ()
      {
      std::vector<std::string> names;
      ForEachVoice ([&] (ISpObjectToken *, const std::string & name)
        {
        names.push_back (name);
        return false;
        });
      return names;
      }

  private:
    template <typename F> static void ForEachVoice (F each)
      {
      EnsureCom ();
      ISpObjectTokenCategory * pCategory = nullptr;
      if (FAILED (CoCreateInstance (__uuidof (SpObjectTokenCategory), NULL, CLSCTX_ALL,
                                    __uuidof (ISpObjectTokenCategory), (void **) &pCategory)))
        return;
      IEnumSpObjectTokens * pEnum = nullptr;
      if (SUCCEEDED (pCategory->SetId (SPCAT_VOICES, FALSE)) &&
          SUCCEEDED (pCategory->EnumTokens (NULL, NULL, &pEnum)))
        {
        ISpObjectToken * pToken = nullptr;
        ULONG fetched = 0;
        bool bDone = false;
        while (!bDone && pEnum->Next (1, &pToken, &fetched) == S_OK && fetched == 1)
          {
          LPWSTR description = nullptr;
          if (SUCCEEDED (pToken->GetStringValue (NULL, &description)) && description)
            {
            bDone = each (pToken, Narrow (description));
            CoTaskMemFree (description);
            }
          pToken->Release ();
          }
        pEnum->Release ();
        }
      pCategory->Release ();
      }

    bool m_bTried = false;
    ISpVoice * m_pVoice = nullptr;
  };

// ---------------------------------------------------------------- the platform

class WinSpeechPlatform : public ISpeechPlatform
  {
  public:
    ISpeech * screenReader () override
      {
      // All of them set this while they run, so for anyone else this is the whole cost.
      // JAWS is asked for separately in case an old version does not.
      BOOL bFlag = FALSE;
      SystemParametersInfo (SPI_GETSCREENREADER, 0, &bFlag, 0);

      if (bFlag && m_nvda.Running ())
        return &m_nvda;
      if (m_jaws.Running ())
        return &m_jaws;
      if (bFlag && m_uia.Load ())
        return &m_uia;
      return nullptr;
      }

    ISpeech * ownVoice () override
      {
      return m_sapi.Load () ? &m_sapi : nullptr;
      }

    std::vector<std::string> ownVoiceNames () override { return SapiSpeech::Installed (); }

    void shutdown () override
      {
      m_jaws.Release ();
      m_sapi.Release ();
      }

    bool HoldsCom () const { return m_jaws.HoldsCom () || m_sapi.HoldsCom (); }

  private:
    NvdaSpeech m_nvda;
    JawsSpeech m_jaws;
    UiaSpeech m_uia;
    SapiSpeech m_sapi;
  };

}  // namespace

bool RaiseUiaNotification (HWND hWnd, const char * utf8, const int processing)
  {
  if (!LoadUia ())
    return false;
  NotificationSource * pSource = new NotificationSource (hWnd);
  BSTR text = SysAllocString (Wide (utf8).c_str ());
  BSTR activity = SysAllocString (L"MUSHclient.output");
  const HRESULT hr = g_raise (pSource, kNotificationKindOther, processing, text, activity);
  SysFreeString (text);
  SysFreeString (activity);
  pSource->Release ();
  return SUCCEEDED (hr);
  }

namespace {
WinSpeechPlatform * g_pWinPlatform = nullptr;
}

void InstallPlatformSpeech ()
  {
  static WinSpeechPlatform platform;
  g_pWinPlatform = &platform;
  g_speechPlatform = &platform;
  }

// For tests: is a COM object of the speech backends' still held?
bool WinSpeech_HoldsComForTest ()
  {
  return g_pWinPlatform && g_pWinPlatform->HoldsCom ();
  }

}  // namespace pal
