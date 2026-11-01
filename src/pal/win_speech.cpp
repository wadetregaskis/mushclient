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
// The system voice is SAPI, created the first time someone asks for it, and not before.
//
// Everything runs on the main thread.  COM is initialised for it on first use.

#include "stdafx.h"

#include "pal/ispeech.h"

// sapi.h says byte, which stdafx.h's "using namespace std" makes ambiguous with std::byte.
#define byte ::byte
#include <sapi.h>
#undef byte

#include <string>

namespace pal {

ISpeechPlatform * g_speechPlatformWin = nullptr;

namespace {

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
      // Beside the exe only: this is not a name to let the search path find.
      m_hDll = LoadLibraryW ((ExeDirectory () + L"nvdaControllerClient.dll").c_str ());
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
      return m_speakText (Wide (utf8).c_str ()) == 0;
      }

    void stop () override { m_cancelSpeech (); }

    bool isBusy () override
      {
      unsigned char speaking = 0;
      return m_bKnowsBusy && m_isSpeaking (&speaking) == 0 && speaking;
      }

    void braille (const char * utf8) override
      {
      if (m_brailleMessage)
        m_brailleMessage (Wide (utf8).c_str ());
      }

  private:
    bool m_bTried = false;
    bool m_bKnowsBusy = false;
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
      if (!FindWindowW (L"JFWUI2", NULL))
        {
        Release ();
        return false;
        }
      if (m_pApi)
        return true;

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
      return SUCCEEDED (hr);
      }

    void stop () override
      {
      if (!m_pApi)
        return;
      DISPPARAMS params = { NULL, NULL, 0, 0 };
      m_pApi->Invoke (m_idStop, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &params, NULL, NULL, NULL);
      }

  private:
    void Release ()
      {
      if (m_pApi)
        m_pApi->Release ();
      m_pApi = nullptr;
      }

    IDispatch * m_pApi = nullptr;
    DISPID m_idSay = DISPID_UNKNOWN;
    DISPID m_idStop = DISPID_UNKNOWN;
  };

// ---------------------------------------------------------------- UI Automation

// From uiautomationcoreapi.h, which hides them from a 0x0501 build.
enum { kNotificationKindOther = 4 };
enum
  {
  kNotificationProcessingImportantAll = 0,
  kNotificationProcessingAll = 2,
  kNotificationProcessingMostRecent = 3
  };

typedef HRESULT (WINAPI * RaiseNotification) (void *, int, int, BSTR, BSTR);
typedef HRESULT (WINAPI * HostProviderFromHwnd) (HWND, void **);

class UiaSpeech : public ISpeech
  {
  public:
    bool Load ()
      {
      if (m_bTried)
        return m_raise != nullptr;
      m_bTried = true;
      HMODULE hDll = LoadLibraryW (L"uiautomationcore.dll");
      if (!hDll)
        return false;
      m_raise = (RaiseNotification) GetProcAddress (hDll, "UiaRaiseNotificationEvent");
      m_hostProvider = (HostProviderFromHwnd) GetProcAddress (hDll, "UiaHostProviderFromHwnd");
      if (!m_hostProvider)
        m_raise = nullptr;
      return m_raise != nullptr;
      }

    unsigned capabilities () const override { return kSpeechScreenReader; }
    const char * name () const override { return "UI Automation"; }

    bool speak (const char * utf8, SpeechPriority priority, bool bFlush) override
      {
      CWnd * pMain = AfxGetMainWnd ();
      if (!m_raise || !pMain)
        return false;

      void * pProvider = nullptr;
      if (FAILED (m_hostProvider (pMain->GetSafeHwnd (), &pProvider)) || !pProvider)
        return false;

      BSTR text = SysAllocString (Wide (utf8).c_str ());
      BSTR activity = SysAllocString (L"MUSHclient.output");
      const int processing = bFlush ? kNotificationProcessingMostRecent :
                             priority == SpeechPriority::Assertive ? kNotificationProcessingImportantAll :
                                                                     kNotificationProcessingAll;
      const HRESULT hr = m_raise (pProvider, kNotificationKindOther, processing, text, activity);
      SysFreeString (text);
      SysFreeString (activity);
      ((IUnknown *) pProvider)->Release ();
      return SUCCEEDED (hr);
      }

    void stop () override {}    // a notification cannot be called back

  private:
    bool m_bTried = false;
    RaiseNotification m_raise = nullptr;
    HostProviderFromHwnd m_hostProvider = nullptr;
  };

// ---------------------------------------------------------------- SAPI

class SapiSpeech : public ISpeech
  {
  public:
    ~SapiSpeech () override
      {
      if (m_pVoice)
        m_pVoice->Release ();
      }

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

    std::vector<std::string> voices () override
      {
      std::vector<std::string> names;
      ForEachVoice ([&] (ISpObjectToken *, const std::string & name)
        {
        names.push_back (name);
        return false;
        });
      return names;
      }

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

  private:
    template <typename F> void ForEachVoice (F each)
      {
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

  private:
    NvdaSpeech m_nvda;
    JawsSpeech m_jaws;
    UiaSpeech m_uia;
    SapiSpeech m_sapi;
  };

}  // namespace

void InstallPlatformSpeech ()
  {
  static WinSpeechPlatform platform;
  g_speechPlatform = &platform;
  }

}  // namespace pal
