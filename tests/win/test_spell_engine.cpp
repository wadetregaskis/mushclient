// tests/win/test_spell_engine.cpp -- spell_engine_win.cpp: Windows' own spell
// checker (ISpellChecker) behind MUSHclient's.
//
// Against the real thing, so it says something only where Windows has a spell
// checker in a language the words below are English in; elsewhere -- a Windows
// before 8, or one with no English -- it checks only that there is no engine,
// which is what makes MUSHclient fall back to its own lists.

#include "mc_test.h"

#include "stdafx.h"
#include "spell_engine.h"

#include <algorithm>
#include <memory>
#include <string>

TEST(WindowsSpellEngine, ChecksSuggestsAndIsNamed)
  {
  const HRESULT init = CoInitializeEx (NULL, COINIT_APARTMENTTHREADED);
  std::unique_ptr<ISpellEngine> engine (CreatePlatformSpellEngine ());
  if (engine)
    {
    EXPECT_EQ (std::string ("Windows ("), engine->Name ().substr (0, 9));
    const bool english = engine->Name ().find ("(en") != std::string::npos;
    if (english)
      {
      EXPECT_TRUE  (engine->Check ("receive"));
      EXPECT_FALSE (engine->Check ("recieve"));
      const std::vector<std::string> s = engine->Suggest ("recieve");
      EXPECT_TRUE (std::find (s.begin (), s.end (), "receive") != s.end ());
      // the word list the built-in checker cannot match: one sound code apart
      const std::vector<std::string> q = engine->Suggest ("quikc");
      EXPECT_TRUE (std::find (q.begin (), q.end (), "quick") != q.end ());
      }
    EXPECT_TRUE (engine->Check (""));     // nothing to say about nothing
    }
  if (SUCCEEDED (init))
    CoUninitialize ();
  }
