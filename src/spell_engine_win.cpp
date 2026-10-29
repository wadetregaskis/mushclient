// spell_engine_win.cpp -- Windows' spell checker (ISpellChecker, Windows 8 and
// later) behind MUSHclient's.  See spell_engine.h.
//
// The project is built for _WIN32_WINNT 0x0501, for which the SDK declares no
// ISpellChecker; this file alone is built for Windows 10, which it can be
// because it includes no MFC and shares no structure with anything that does.
// The interfaces are COM, so nothing is linked that an older Windows would
// lack: there, creating the factory fails and the built-in lists are used.

#ifdef _WIN32

#undef  _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef  NTDDI_VERSION
#undef  WINVER
#define WINVER 0x0A00

#include <windows.h>
#include <objbase.h>
#include <spellcheck.h>
#include <vector>

#include "spell_engine.h"

namespace {

std::wstring Wide (const std::string & s)
  {
  const int n = MultiByteToWideChar (CP_ACP, 0, s.c_str (), -1, NULL, 0);
  std::wstring w (n > 0 ? n - 1 : 0, L'\0');
  if (n > 1)
    MultiByteToWideChar (CP_ACP, 0, s.c_str (), -1, &w [0], n);
  return w;
  }

std::string Narrow (const wchar_t * w)
  {
  const int n = WideCharToMultiByte (CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
  std::string s (n > 0 ? n - 1 : 0, '\0');
  if (n > 1)
    WideCharToMultiByte (CP_ACP, 0, w, -1, &s [0], n, NULL, NULL);
  return s;
  }

class CWindowsSpellEngine : public ISpellEngine
  {
  public:
    CWindowsSpellEngine (ISpellChecker * checker, const std::wstring & language)
      : m_checker (checker), m_language (language) {}
    ~CWindowsSpellEngine () override
      {
      m_checker->Release ();
      }

    std::string Name () override
      {
      return "Windows (" + Narrow (m_language.c_str ()) + ")";
      }

    bool Check (const std::string & word) override
      {
      const std::wstring w = Wide (word);
      IEnumSpellingError * errors = NULL;
      if (w.empty () || FAILED (m_checker->Check (w.c_str (), &errors)) || !errors)
        return true;    // nothing it can say: do not nag
      ISpellingError * error = NULL;
      const bool wrong = errors->Next (&error) == S_OK;
      if (error)
        error->Release ();
      errors->Release ();
      return !wrong;
      }

    std::vector<std::string> Suggest (const std::string & word) override
      {
      std::vector<std::string> suggestions;
      IEnumString * list = NULL;
      if (FAILED (m_checker->Suggest (Wide (word).c_str (), &list)) || !list)
        return suggestions;
      LPOLESTR s = NULL;
      while (list->Next (1, &s, NULL) == S_OK)
        {
        suggestions.push_back (Narrow (s));
        CoTaskMemFree (s);
        }
      list->Release ();
      return suggestions;
      }

    bool AddWord (const std::string & word) override
      {
      return SUCCEEDED (m_checker->Add (Wide (word).c_str ()));
      }

  private:
    ISpellChecker * m_checker;
    std::wstring    m_language;
  };

}   // namespace

ISpellEngine * CreatePlatformSpellEngine ()
  {
  ISpellCheckerFactory * factory = NULL;
  if (FAILED (CoCreateInstance (__uuidof (SpellCheckerFactory), NULL, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS (&factory))))
    return NULL;   // before Windows 8, or COM not initialised

  // The languages the user reads Windows in, best first -- which is what they
  // write in, rather than the regional format (dates and numbers), which an
  // English speaker in Germany may well have set to German -- then that
  // format's language, each also without its region, then English, which
  // every Windows with the spell checker has.
  std::vector<std::wstring> candidates;
  auto add = [&candidates] (const std::wstring & language) {
    if (language.empty ())
      return;
    candidates.push_back (language);
    const size_t dash = language.find (L'-');
    if (dash != std::wstring::npos)
      candidates.push_back (language.substr (0, dash));
  };
  ULONG count = 0, size = 0;
  if (GetUserPreferredUILanguages (MUI_LANGUAGE_NAME, &count, NULL, &size) && size > 0)
    {
    std::vector<wchar_t> list (size);
    if (GetUserPreferredUILanguages (MUI_LANGUAGE_NAME, &count, list.data (), &size))
      for (const wchar_t * p = list.data (); *p; p += wcslen (p) + 1)   // double-NUL-terminated
        add (p);
    }
  wchar_t locale [LOCALE_NAME_MAX_LENGTH] = L"";
  GetUserDefaultLocaleName (locale, LOCALE_NAME_MAX_LENGTH);
  add (locale);
  add (L"en-US");

  ISpellEngine * engine = NULL;
  for (const std::wstring & language : candidates)
    {
    BOOL supported = FALSE;
    if (language.empty () || FAILED (factory->IsSupported (language.c_str (), &supported)) || !supported)
      continue;
    ISpellChecker * checker = NULL;
    if (SUCCEEDED (factory->CreateSpellChecker (language.c_str (), &checker)) && checker)
      {
      engine = new CWindowsSpellEngine (checker, language);
      break;
      }
    }
  factory->Release ();
  return engine;
  }

#endif  // _WIN32
