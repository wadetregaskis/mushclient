// tests/test_legacy_import.cpp -- legacy_import.cpp: importing a MUSHclient 5.
//
// Driven against a fake Wine prefix built in a temp folder -- a user.reg, a
// drive_c with a 5.x in it holding a real preferences database, world file,
// spell database and MUSHclient.ini -- because that is how a 5.x looks from
// macOS and Linux, and because it runs the same on every platform, Windows
// included.  A native Windows 5.x, with its VirtualStore shadow, is
// tests/win/test_user_data.cpp's.

#include "mc_test.h"

#include "stdafx.h"
#include "legacy_import.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using legacy::Install;

namespace {

// A path written with backslashes, in this platform's form.
std::string N (std::string s)
  {
  for (char & c : s)
    if (c == '\\')
      c = (char) fs::path::preferred_separator;
  return s;
  }

std::string Temp (const char * name)
  {
  std::error_code ec;
  const fs::path p = fs::temp_directory_path (ec) / (std::string ("mc_legacy_") + name);
  fs::remove_all (p, ec);
  fs::create_directories (p, ec);
  std::string s = p.string ();
  s += (char) fs::path::preferred_separator;
  return s;
  }

void Remove (const std::string & dir)
  {
  std::error_code ec;
  fs::remove_all (fs::path (dir), ec);
  }

void Put (const std::string & name, const std::string & contents)
  {
  std::error_code ec;
  fs::create_directories (fs::path (name).parent_path (), ec);
  std::ofstream (fs::path (name), std::ios::binary) << contents;
  }

std::string Get (const std::string & name)
  {
  std::ifstream f (fs::path (name), std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf ();
  return ss.str ();
  }

bool Exists (const std::string & name)
  {
  std::error_code ec;
  return fs::exists (fs::path (name), ec);
  }

void Exec (sqlite3 * db, const std::string & sql)
  {
  sqlite3_exec (db, sql.c_str (), NULL, NULL, NULL);
  }

std::string Scalar (sqlite3 * db, const char * sql)
  {
  std::string s = "<none>";
  sqlite3_stmt * stmt = NULL;
  if (sqlite3_prepare_v2 (db, sql, -1, &stmt, NULL) == SQLITE_OK &&
      sqlite3_step (stmt) == SQLITE_ROW && sqlite3_column_text (stmt, 0))
    s = (const char *) sqlite3_column_text (stmt, 0);
  sqlite3_finalize (stmt);
  return s;
  }

const char * const kSchema =
  "CREATE TABLE control (name VARCHAR(10) NOT NULL PRIMARY KEY, value INT NOT NULL);"
  "CREATE TABLE prefs (name VARCHAR(50) NOT NULL PRIMARY KEY, value TEXT NOT NULL);"
  "CREATE TABLE worlds (name VARCHAR(50) NOT NULL PRIMARY KEY, value TEXT NOT NULL);";

void MakeDb (const std::string & name, const std::string & sql)
  {
  std::error_code ec;
  fs::create_directories (fs::path (name).parent_path (), ec);
  sqlite3 * db = NULL;
  sqlite3_open (name.c_str (), &db);
  Exec (db, sql);
  sqlite3_close (db);
  }

// A new database as PopulateDatabase leaves it, for the preferences named.
sqlite3 * NewDb ()
  {
  sqlite3 * db = NULL;
  sqlite3_open (":memory:", &db);
  Exec (db, kSchema);
  Exec (db, "INSERT INTO prefs VALUES ('WordDelimiters', 'default');"
            "INSERT INTO prefs VALUES ('DefaultLogFileDirectory', 'default');"
            "INSERT INTO prefs VALUES ('AsciiArtFont', 'default');"
            "INSERT INTO prefs VALUES ('TrayIconFileName', 'default');"
            "INSERT INTO prefs VALUES ('WorldList', '');");
  return db;
  }

const char * const kWinFolder = "C:\\Program Files (x86)\\MUSHclient\\";

Install WineInstall (const std::string & prefix)
  {
  Install old;
  old.prefix        = prefix;
  old.windowsFolder = kWinFolder;
  old.folder        = prefix + N ("drive_c\\Program Files (x86)\\MUSHclient\\");
  return old;
  }

}   // namespace

// ── The registry, as Wine keeps it ────────────────────────────────────────

TEST(LegacyRegistry, ReadsOneKeyOfAWineUserReg)
  {
  const std::string reg =
    "WINE REGISTRY Version 2\n"
    ";; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n"
    "\n"
    "[Software\\\\Gammon Software Solutions\\\\MUSHclient\\\\General Options] 1698000000\n"
    "#time=1d9f0a\n"
    "\"InstallDir\"=\"C:\\\\Program Files (x86)\\\\MUSHclient\\\\\"\n"
    "\n"
    "[Software\\\\Gammon Software Solutions\\\\MUSHclient\\\\Global prefs] 1698000001\n"
    "\"AsciiArtFont\"=\"fonts\\\\standard.flf\"\r\n"
    "\"TimerInterval\"=dword:0000000a\n"
    "\"Quoted\"=\"say \\\"hi\\\"\"\n"
    "\"Binary\"=hex:01,02\n"
    "\n"
    "[Software\\\\Other] 1\n"
    "\"AsciiArtFont\"=\"not this one\"\n";

  const legacy::RegistryValues v =
    legacy::ParseWineRegistry (reg, "Software\\Gammon Software Solutions\\MUSHclient\\Global prefs");
  ASSERT_EQ ((size_t) 3, v.size ());
  EXPECT_EQ (std::string ("AsciiArtFont"), v [0].first);
  EXPECT_EQ (std::string ("fonts\\standard.flf"), v [0].second);
  EXPECT_EQ (std::string ("10"), v [1].second);
  EXPECT_EQ (std::string ("say \"hi\""), v [2].second);

  const legacy::RegistryValues g =
    legacy::ParseWineRegistry (reg, "software\\gammon software solutions\\mushclient\\general options");
  ASSERT_EQ ((size_t) 1, g.size ());
  EXPECT_EQ (std::string (kWinFolder), g [0].second);
  }

// ── Translating ───────────────────────────────────────────────────────────

TEST(LegacyTranslate, WinePathsBecomePathsOnThisMachine)
  {
  const std::string prefix  = Temp ("prefix_t");
  const std::string program = Temp ("program_t");
  const std::string data    = Temp ("data_t");
  Put (program + N ("fonts\\standard.flf"), "flf2a");
  const Install old = WineInstall (prefix);

  // the user's folders go to the data folder, however they were written
  EXPECT_EQ (data + N ("logs\\"),
             legacy::TranslatePath ("C:\\Program Files (x86)\\MUSHclient\\logs\\", old, data, program));
  EXPECT_EQ (data + N ("worlds\\"), legacy::TranslatePath (".\\worlds\\", old, data, program));
  EXPECT_EQ (data + N ("worlds\\x.mcl"),
             legacy::TranslatePath ("c:/program files (x86)/mushclient/worlds/x.mcl", old, data, program));

  // the program's files go to the program's folder -- if it has them
  bool resolved = false;
  EXPECT_EQ (program + N ("fonts\\standard.flf"),
             legacy::TranslatePath ("fonts\\standard.flf", old, data, program, &resolved));
  EXPECT_TRUE (resolved);
  legacy::TranslatePath ("fonts\\gone.flf", old, data, program, &resolved);
  EXPECT_FALSE (resolved);

  // anywhere else is where Wine's drives put it: c: in the prefix, z: at /
  EXPECT_EQ (prefix + N ("drive_c\\MUD\\logs\\"),
             legacy::TranslatePath ("C:\\MUD\\logs\\", old, data, program));
  EXPECT_EQ (std::string ("/") + N ("home\\me\\x.txt"),
             legacy::TranslatePath ("Z:\\home\\me\\x.txt", old, data, program));
  EXPECT_EQ (std::string ("D:\\Unmapped\\x.txt"),
             legacy::TranslatePath ("D:\\Unmapped\\x.txt", old, data, program));

  Remove (prefix); Remove (program); Remove (data);
  }

TEST(LegacyTranslate, EveryPathInAWorldFileIsTranslated)
  {
  const Install old = WineInstall (N ("\\wine\\"));
  const std::string data = N ("\\data\\");
  const std::string text =
    "<world auto_log_file_name=\"C:\\Program Files (x86)\\MUSHclient\\logs\\%N.txt\"\r\n"
    "<include name=\"C:\\PROGRAM FILES (X86)\\MUSHclient\\worlds\\plugins\\X.xml\" plugin=\"y\"/>\r\n"
    "<include name=\"Y.xml\" plugin=\"y\"/>\r\n";
  const std::string expect =
    "<world auto_log_file_name=\"" + data + N ("logs\\%N.txt") + "\"\r\n"
    "<include name=\"" + data + N ("worlds\\plugins\\X.xml") + "\" plugin=\"y\"/>\r\n"
    "<include name=\"Y.xml\" plugin=\"y\"/>\r\n";
  EXPECT_EQ (expect, legacy::TranslateText (text, old, data, N ("\\prog\\")));
  }

// ── Finding one ───────────────────────────────────────────────────────────

TEST(LegacyFind, FindsA5xInAWinePrefix)
  {
  const std::string prefix = Temp ("prefix_f");
  // where 5.x's installer said it went, with an exe and no database: one too
  // old to have used one
  Put (prefix + "user.reg",
       "WINE REGISTRY Version 2\n"
       "[Software\\\\Gammon Software Solutions\\\\MUSHclient\\\\General Options] 1\n"
       "\"InstallDir\"=\"C:\\\\Games\\\\MUSHclient\"\n");
  Put (prefix + N ("drive_c\\Games\\MUSHclient\\MUSHCLIENT.EXE"), "MZ");
  // and the default place, with a database
  MakeDb (prefix + N ("drive_c\\Program Files (x86)\\MUSHclient\\mushclient_prefs.sqlite"), kSchema);
  // not a 5.x: nothing that says one ran
  Put (prefix + N ("drive_c\\Program Files\\MUSHclient\\readme.txt"), "x");

  const std::vector<Install> found = legacy::InPrefix (prefix, "");
  ASSERT_EQ ((size_t) 2, found.size ());
  EXPECT_EQ (std::string ("C:\\Games\\MUSHclient\\"), found [0].windowsFolder);
  EXPECT_EQ (prefix + N ("drive_c\\Games\\MUSHclient\\"), found [0].folder);
  EXPECT_EQ (std::string (kWinFolder), found [1].windowsFolder);
  EXPECT_EQ (prefix, found [1].prefix);

  // chosen by hand: the prefix itself...
  Install chosen;
  EXPECT_TRUE (legacy::FromFolder (prefix, chosen));
  EXPECT_EQ (found [0].folder, chosen.folder);
#ifndef _WIN32
  // ...or 5.x's folder inside it, which knows where it is in the prefix
  EXPECT_TRUE (legacy::FromFolder (prefix + "drive_c/Program Files (x86)/MUSHclient", chosen));
  EXPECT_EQ (std::string (kWinFolder), chosen.windowsFolder);
  EXPECT_EQ (prefix, chosen.prefix);
#endif
  // ...but not just any folder
  EXPECT_FALSE (legacy::FromFolder (prefix + N ("drive_c\\Program Files\\MUSHclient"), chosen));

  Remove (prefix);
  }

// ── Importing, end to end ─────────────────────────────────────────────────

TEST(LegacyImport, CopiesAndTranslatesA5xFromAWinePrefix)
  {
  const std::string prefix  = Temp ("prefix_i");
  const std::string data    = Temp ("data_i");
  const std::string program = Temp ("program_i");
  Put (program + N ("fonts\\standard.flf"), "flf2a");
  const Install old = WineInstall (prefix);
  const std::string folder = old.folder;

  MakeDb (folder + "mushclient_prefs.sqlite", std::string (kSchema) +
          "INSERT INTO prefs VALUES ('WordDelimiters', '.,;');"
          "INSERT INTO prefs VALUES ('DefaultLogFileDirectory', 'C:\\Program Files (x86)\\MUSHclient\\logs\\');"
          "INSERT INTO prefs VALUES ('AsciiArtFont', 'fonts\\standard.flf');"
          "INSERT INTO prefs VALUES ('TrayIconFileName', 'C:\\Program Files (x86)\\MUSHclient\\gone.ico');"
          "INSERT INTO prefs VALUES ('WorldList', 'C:\\Program Files (x86)\\MUSHclient\\worlds\\A.MCL*Z:\\b.mcl');"
          "INSERT INTO prefs VALUES ('NoSuchPreference', 'x');"
          "INSERT INTO worlds VALUES ('Some world', '1,2,3,4');");
  // an upper-case extension, as 5.x writes one
  Put (folder + N ("worlds\\A.MCL"),
       "<world script_filename=\"C:\\Program Files (x86)\\MUSHclient\\scripts\\a.lua\"/>");
  Put (folder + N ("worlds\\plugins\\state\\s.xml"), "state");
  Put (folder + N ("worlds\\plugins\\Shipped.xml"), "5.x's copy");
  Put (folder + N ("logs\\big.txt"), "a log");
  Put (folder + "MUSHclient.ini",
       "[Settings]\r\nFile1=not this\r\n"
       "[Recent File List]\r\n"
       "File1=C:\\Program Files (x86)\\MUSHclient\\worlds\\A.MCL\r\n"
       "File2=Z:\\older.mcl\r\n");
  MakeDb (folder + N ("spell\\spell.sqlite"),
          "CREATE TABLE words (word_id INTEGER PRIMARY KEY, name, metaphone, user INT);"
          "INSERT INTO words VALUES (1, 'grue', 'KR', 1);"
          "INSERT INTO words VALUES (2, 'house', 'HS', 0);");

  // 6.00's own copy of a bundled plugin, already here
  Put (data + N ("worlds\\plugins\\Shipped.xml"), "6.00's copy");
  sqlite3 * db = NewDb ();

  const legacy::Result result = legacy::Import (old, db, data, program);
  EXPECT_TRUE (result.prefs);
  EXPECT_EQ (1, result.worlds);
  EXPECT_EQ (1, result.words);

  // preferences: translated, unknown ones dropped, unresolvable ones left
  EXPECT_EQ (std::string (".,;"), Scalar (db, "SELECT value FROM prefs WHERE name = 'WordDelimiters'"));
  EXPECT_EQ (data + N ("logs\\"),
             Scalar (db, "SELECT value FROM prefs WHERE name = 'DefaultLogFileDirectory'"));
  EXPECT_EQ (program + N ("fonts\\standard.flf"),
             Scalar (db, "SELECT value FROM prefs WHERE name = 'AsciiArtFont'"));
  EXPECT_EQ (std::string ("default"),
             Scalar (db, "SELECT value FROM prefs WHERE name = 'TrayIconFileName'"));
  EXPECT_EQ (data + N ("worlds\\A.MCL") + "*" + "/" + N ("b.mcl"),
             Scalar (db, "SELECT value FROM prefs WHERE name = 'WorldList'"));
  EXPECT_EQ (std::string ("<none>"), Scalar (db, "SELECT value FROM prefs WHERE name = 'NoSuchPreference'"));
  EXPECT_EQ (std::string ("1,2,3,4"), Scalar (db, "SELECT value FROM worlds WHERE name = 'Some world'"));
  EXPECT_EQ (folder, Scalar (db, "SELECT value FROM control WHERE name = 'Imported from'"));

  // files: copied and translated, never over 6.00's own, logs left behind
  EXPECT_EQ ("<world script_filename=\"" + data + N ("scripts\\a.lua") + "\"/>",
             Get (data + N ("worlds\\A.MCL")));
  EXPECT_EQ (std::string ("state"), Get (data + N ("worlds\\plugins\\state\\s.xml")));
  EXPECT_EQ (std::string ("6.00's copy"), Get (data + N ("worlds\\plugins\\Shipped.xml")));
  EXPECT_FALSE (Exists (data + N ("logs\\big.txt")));

  // recent files, oldest first
  ASSERT_EQ ((size_t) 2, result.recentFiles.size ());
  EXPECT_EQ ("/" + N ("older.mcl"), result.recentFiles [0]);
  EXPECT_EQ (data + N ("worlds\\A.MCL"), result.recentFiles [1]);

  // the added word, for the new spell database to pick up when it builds
  const std::string dict = Get (data + "userdict.txt");
  EXPECT_TRUE (dict.find ("grue") != std::string::npos);
  EXPECT_TRUE (dict.find ("house") == std::string::npos);

  // and 5.x exactly as it was: nothing beside its databases, its files intact
  EXPECT_EQ (std::string ("5.x's copy"), Get (folder + N ("worlds\\plugins\\Shipped.xml")));
  EXPECT_FALSE (Exists (folder + "mushclient_prefs.sqlite-journal"));
  EXPECT_FALSE (Exists (folder + N ("spell\\spell.sqlite-shm")));
  EXPECT_FALSE (Exists (folder + N ("spell\\spell.sqlite-wal")));

  const std::string said = legacy::Describe (old, result);
  EXPECT_TRUE (said.find ("1 world file ") != std::string::npos);
  EXPECT_TRUE (said.find (folder + "logs") != std::string::npos);

  sqlite3_close (db);
  Remove (prefix); Remove (data); Remove (program);
  }

TEST(LegacyImport, AnOlder5xHasItsPreferencesInTheRegistry)
  {
  const std::string prefix = Temp ("prefix_r");
  const std::string data   = Temp ("data_r");
  const Install old = WineInstall (prefix);
  Put (old.folder + "mushclient.exe", "MZ");
  Put (prefix + "user.reg",
       "WINE REGISTRY Version 2\n"
       "[Software\\\\Gammon Software Solutions\\\\MUSHclient\\\\Global prefs] 1\n"
       "\"WordDelimiters\"=\".!\"\n"
       "\"DefaultLogFileDirectory\"=\"C:\\\\Program Files (x86)\\\\MUSHclient\\\\logs\\\\\"\n");
  sqlite3 * db = NewDb ();

  const legacy::Result result = legacy::Import (old, db, data, data);
  EXPECT_TRUE (result.prefs);
  EXPECT_EQ (std::string (".!"), Scalar (db, "SELECT value FROM prefs WHERE name = 'WordDelimiters'"));
  EXPECT_EQ (data + N ("logs\\"),
             Scalar (db, "SELECT value FROM prefs WHERE name = 'DefaultLogFileDirectory'"));
  EXPECT_EQ (std::string ("Your settings have been copied"),
             legacy::Describe (old, result).substr (0, 30));

  sqlite3_close (db);
  Remove (prefix); Remove (data);
  }

TEST(LegacyImport, NothingThereImportsNothing)
  {
  const std::string prefix = Temp ("prefix_n");
  const std::string data   = Temp ("data_n");
  const Install old = WineInstall (prefix);
  sqlite3 * db = NewDb ();
  const legacy::Result result = legacy::Import (old, db, data, data);
  EXPECT_FALSE (result.Anything ());
  EXPECT_EQ (std::string ("<none>"), Scalar (db, "SELECT value FROM control WHERE name = 'Imported from'"));
  sqlite3_close (db);
  Remove (prefix); Remove (data);
  }

MC_TEST_MAIN()
