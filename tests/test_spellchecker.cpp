// tests/test_spellchecker.cpp -- spellchecker.lua with a system spell checker.
//
// The real script, loaded into a bare Lua state whose `utils` is a stand-in:
// a fake system engine that knows a handful of words, and a dialog that
// answers as each test says.  That pins what the script does with an engine
// -- the engine decides; MUSHclient's own dictionary (userdict.txt) is
// consulted first; "Add to MUSHclient" writes that file and "Add to System"
// teaches the engine -- on every platform, with no engine or dialog of the
// platform's own involved.  The engines themselves are each platform's:
// tests/win/test_spell_engine.cpp for Windows'.

#include "mc_test.h"

#include "stdafx.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// The script, found from this file: tests/ is beside src/.
std::string ScriptPath ()
  {
  const fs::path here = fs::path (__FILE__).parent_path ();
  for (const fs::path & p : { here / ".." / "src" / "scripting" / "spellchecker.lua",
                              fs::path ("..") / "src" / "scripting" / "spellchecker.lua",
                              fs::path ("src") / "scripting" / "spellchecker.lua" })
    {
    std::error_code ec;
    if (fs::exists (p, ec))
      return p.string ();
    }
  return "";
  }

std::string Temp ()
  {
  std::error_code ec;
  const fs::path p = fs::temp_directory_path (ec) / "mc_spellchecker";
  fs::remove_all (p, ec);
  fs::create_directories (p, ec);
  std::string s = p.generic_string ();
  return s + "/";
  }

std::string Get (const std::string & name)
  {
  std::ifstream f (fs::path (name), std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf ();
  return ss.str ();
  }

bool Run (lua_State * L, const std::string & code)
  {
  if (luaL_dostring (L, code.c_str ()) != 0)
    {
    fprintf (stderr, "Lua: %s\n", lua_tostring (L, -1));
    lua_pop (L, 1);
    return false;
    }
  return true;
  }

std::string Result (lua_State * L, const std::string & expression)
  {
  if (luaL_dostring (L, ("return " + expression).c_str ()) != 0)
    {
    std::string e = lua_tostring (L, -1);
    lua_pop (L, 1);
    return "error: " + e;
    }
  const char * s = lua_tostring (L, -1);
  std::string r = s ? s : "nil";
  lua_pop (L, 1);
  return r;
  }

// A Lua state with the stand-in utils, and the script loaded from `dir`.
lua_State * Load (const std::string & dir)
  {
  lua_State * L = luaL_newstate ();
  luaL_openlibs (L);
  Run (L,
    "dir = [[" + dir + "]]\n"
    "known = { the = true, quick = true, fox = true }\n"
    "taught, dialogs, answer = {}, {}, 'ignore'\n"
    "utils = {\n"
    "  info = function () return { resource_directory = dir, data_directory = dir, app_directory = dir } end,\n"
    "  metaphone = function () error 'the built-in lists are not for use here' end,\n"
    "  edit_distance = function () return 0 end,\n"
    "  spell_engine = function () return 'Fake (en)' end,\n"
    "  spell_check = function (w) return known [w:lower ()] == true end,\n"
    "  spell_suggest = function (w) return { 'suggested', 'another' } end,\n"
    "  spell_add_word = function (w) table.insert (taught, w); known [w:lower ()] = true; return true end,\n"
    "  spellcheckdialog = function (word, suggestions, system)\n"
    "    table.insert (dialogs, { word = word, first = suggestions [1], system = system })\n"
    "    return answer, word\n"
    "  end,\n"
    "}\n");
  Run (L, "dofile [[" + ScriptPath () + "]]");
  return L;
  }

}   // namespace

TEST(SpellChecker, TheSystemEngineDecides)
  {
  ASSERT_TRUE (!ScriptPath ().empty ());
  const std::string dir = Temp ();
  lua_State * L = Load (dir);

  EXPECT_EQ (std::string ("quikc grue"),
             Result (L, "table.concat (spellcheck_string ('The quikc fox, grue'), ' ')"));

  // the dialog is told there is a system dictionary, and offered its suggestions
  EXPECT_EQ (std::string ("The quikc fox"), Result (L, "spellcheck ('The quikc fox')"));
  EXPECT_EQ (std::string ("quikc suggested true"),
             Result (L, "dialogs [1].word .. ' ' .. dialogs [1].first .. ' ' .. tostring (dialogs [1].system)"));

  lua_close (L);
  fs::remove_all (fs::path (dir));
  }

TEST(SpellChecker, MUSHclientsOwnDictionaryComesFirst)
  {
  const std::string dir = Temp ();
  std::ofstream (fs::path (dir + "userdict.txt"), std::ios::binary) << "Grue\r\nAnkh-Morpork\r\n";
  lua_State * L = Load (dir);
  EXPECT_EQ (std::string ("quikc"),
             Result (L, "table.concat (spellcheck_string ('the quikc grue'), ' ')"));
  lua_close (L);
  fs::remove_all (fs::path (dir));
  }

TEST(SpellChecker, AddToMUSHclientWritesItsDictionaryNotTheSystems)
  {
  const std::string dir = Temp ();
  lua_State * L = Load (dir);
  Run (L, "answer = 'add'; spellcheck ('a quikc fox')");
  EXPECT_TRUE (Get (dir + "userdict.txt").find ("quikc") != std::string::npos);
  EXPECT_EQ (std::string ("0"), Result (L, "#taught"));
  EXPECT_EQ (std::string (""), Result (L, "table.concat (spellcheck_string ('quikc'), ' ')"));
  lua_close (L);

  // and it is still there next time
  L = Load (dir);
  EXPECT_EQ (std::string (""), Result (L, "table.concat (spellcheck_string ('quikc'), ' ')"));
  lua_close (L);
  fs::remove_all (fs::path (dir));
  }

TEST(SpellChecker, AddToSystemTeachesTheEngine)
  {
  const std::string dir = Temp ();
  lua_State * L = Load (dir);
  Run (L, "answer = 'addsystem'; spellcheck ('a grue')");
  EXPECT_EQ (std::string ("grue"), Result (L, "taught [1]"));
  EXPECT_TRUE (Get (dir + "userdict.txt").find ("grue") == std::string::npos);
  lua_close (L);
  fs::remove_all (fs::path (dir));
  }

TEST(SpellChecker, AddSpellCheckWordUsesMUSHclientsDictionary)
  {
  const std::string dir = Temp ();
  lua_State * L = Load (dir);
  Run (L, "spellcheck_add_word ('zorkmid', 'i')");
  EXPECT_TRUE (Get (dir + "userdict.txt").find ("zorkmid") != std::string::npos);
  EXPECT_EQ (std::string (""), Result (L, "table.concat (spellcheck_string ('zorkmid'), ' ')"));
  lua_close (L);
  fs::remove_all (fs::path (dir));
  }

MC_TEST_MAIN()
