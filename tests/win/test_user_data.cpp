// tests/win/test_user_data.cpp -- user_data.cpp: where 6.00 keeps its files on
// Windows, and the first-run import of a MUSHclient 5 installation.
//
// Windows-only because user_data.cpp is: on macOS and Linux the port has had
// folders of its own from the start (platform/compat.h), and there is no 5.x
// there to import from.
//
// The import is driven end to end against a 5.x installation built in a temp
// folder -- a real preferences database, world file, spell database and
// MUSHclient.ini -- with a VirtualStore shadow beside it, because the shadow is
// what a 5.x under Program Files really wrote to and is the part most easily
// got backwards.

#include "mc_test.h"

#include "stdafx.h"
#include "MUSHclient.h"
#include "user_data.h"

#include <shellapi.h>
#include <string>

using userdata::LegacyInstall;

namespace {

CString TempFolder (const char * pName)
  {
  char dir [MAX_PATH];
  GetTempPath (sizeof dir, dir);
  CString s;
  s.Format ("%smc_user_data_%lu_%s\\", dir, GetCurrentProcessId (), pName);
  return s;
  }

void RemoveTree (const CString & strDir)
  {
  CString s = strDir;
  s.TrimRight ('\\');
  std::string from ((LPCTSTR) s);
  from.push_back ('\0');            // SHFileOperation wants a double NUL
  SHFILEOPSTRUCT op = {};
  op.wFunc  = FO_DELETE;
  op.pFrom  = from.c_str ();
  op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
  SHFileOperation (&op);
  }

void Put (const CString & strName, const std::string & contents)
  {
  CString dir = strName.Left (strName.ReverseFind ('\\'));
  SHCreateDirectoryEx (NULL, dir, NULL);
  FILE * f = fopen (strName, "wb");
  if (f)
    {
    fwrite (contents.data (), 1, contents.size (), f);
    fclose (f);
    }
  }

std::string Get (const CString & strName)
  {
  std::string s;
  FILE * f = fopen (strName, "rb");
  if (!f)
    return s;
  char buf [4096];
  size_t n;
  while ((n = fread (buf, 1, sizeof buf, f)) > 0)
    s.append (buf, n);
  fclose (f);
  return s;
  }

void Exec (sqlite3 * db, const char * sql)
  {
  sqlite3_exec (db, sql, NULL, NULL, NULL);
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

// A preferences database shaped as MUSHclient makes one.
void MakePrefsDb (const CString & strName, const char * extraSql)
  {
  SHCreateDirectoryEx (NULL, strName.Left (strName.ReverseFind ('\\')), NULL);
  sqlite3 * db = NULL;
  sqlite3_open (strName, &db);
  Exec (db, "CREATE TABLE control (name VARCHAR(10) NOT NULL PRIMARY KEY, value INT NOT NULL);"
            "CREATE TABLE prefs (name VARCHAR(50) NOT NULL PRIMARY KEY, value TEXT NOT NULL);"
            "CREATE TABLE worlds (name VARCHAR(50) NOT NULL PRIMARY KEY, value TEXT NOT NULL);");
  Exec (db, extraSql);
  sqlite3_close (db);
  }

}   // namespace

// ── TranslatePath ──────────────────────────────────────────────────────────

TEST(UserDataTranslate, PathsIntoTheOldFolderMoveToTheirNewHomes)
  {
  const CString program = TempFolder ("program");
  Put (program + "fonts\\standard.flf", "flf2a");

  LegacyInstall old;
  old.strFolder = "C:\\Old\\MUSHclient\\";
  old.strShadow = "C:\\Shadow\\MUSHclient\\";
  const CString data = "D:\\Data\\MUSHclient\\";

  // the user's folders go to the data folder, however they were written
  EXPECT_STREQ ("D:\\Data\\MUSHclient\\worlds\\",
                userdata::TranslatePath ("C:\\Old\\MUSHclient\\worlds\\", old, data, program));
  EXPECT_STREQ ("D:\\Data\\MUSHclient\\logs\\",
                userdata::TranslatePath (".\\logs\\", old, data, program));
  EXPECT_STREQ ("D:\\Data\\MUSHclient\\Worlds\\plugins\\state\\",
                userdata::TranslatePath ("c:\\old\\MUSHCLIENT\\Worlds\\plugins\\state\\", old, data, program));
  EXPECT_STREQ ("D:\\Data\\MUSHclient\\scripts\\mine.lua",
                userdata::TranslatePath ("C:\\Shadow\\MUSHclient\\scripts\\mine.lua", old, data, program));
  EXPECT_STREQ ("D:\\Data\\MUSHclient\\worlds\\x.mcl",
                userdata::TranslatePath ("C:/Old/MUSHclient/worlds/x.mcl", old, data, program));

  // the program's files go to the program's folder -- if it has them
  EXPECT_STREQ (program + "fonts\\standard.flf",
                userdata::TranslatePath ("fonts\\standard.flf", old, data, program));
  EXPECT_STREQ ("D:\\Data\\MUSHclient\\fonts\\mine.flf",
                userdata::TranslatePath ("fonts\\mine.flf", old, data, program));

  // and anywhere else is left alone
  EXPECT_STREQ ("E:\\MUD\\logs\\",
                userdata::TranslatePath ("E:\\MUD\\logs\\", old, data, program));
  EXPECT_STREQ ("C:\\Old\\MUSHclient\\..\\Elsewhere\\x.txt",
                userdata::TranslatePath ("..\\Elsewhere\\x.txt", old, data, program));
  EXPECT_STREQ ("", userdata::TranslatePath ("", old, data, program));

  RemoveTree (program);
  }

TEST(UserDataTranslate, EveryPathInAWorldFileIsTranslated)
  {
  LegacyInstall old;
  old.strFolder = "C:\\Program Files (x86)\\MUSHclient\\";
  const CString data = "C:\\Users\\me\\AppData\\Roaming\\MUSHclient\\";
  const CString text =
    "<world auto_log_file_name=\"C:\\Program Files (x86)\\MUSHclient\\logs\\%N.txt\"\r\n"
    "  script_filename=\"C:\\PROGRAM FILES (X86)\\MUSHclient\\scripts\\a.lua\" >\r\n"
    "<include name=\"C:\\Program Files (x86)\\MUSHclient\\worlds\\plugins\\X.xml\" plugin=\"y\"/>\r\n"
    "<include name=\"D:\\Mine\\Y.xml\" plugin=\"y\"/>\r\n";
  const CString expect =
    "<world auto_log_file_name=\"C:\\Users\\me\\AppData\\Roaming\\MUSHclient\\logs\\%N.txt\"\r\n"
    "  script_filename=\"C:\\Users\\me\\AppData\\Roaming\\MUSHclient\\scripts\\a.lua\" >\r\n"
    "<include name=\"C:\\Users\\me\\AppData\\Roaming\\MUSHclient\\worlds\\plugins\\X.xml\" plugin=\"y\"/>\r\n"
    "<include name=\"D:\\Mine\\Y.xml\" plugin=\"y\"/>\r\n";
  EXPECT_STREQ (expect, userdata::TranslateText (text, old, data, "C:\\Prog\\"));
  }

TEST(UserDataTranslate, OnlyPathPreferencesAreTreatedAsPaths)
  {
  EXPECT_TRUE  (userdata::IsPathPreference ("DefaultLogFileDirectory"));
  EXPECT_TRUE  (userdata::IsPathPreference ("pluginsdirectory"));
  EXPECT_TRUE  (userdata::IsPathPreference ("AsciiArtFont"));
  EXPECT_FALSE (userdata::IsPathPreference ("WordDelimiters"));
  EXPECT_FALSE (userdata::IsPathPreference ("WorldList"));
  EXPECT_TRUE  (userdata::IsPathListPreference ("WorldList"));
  EXPECT_TRUE  (userdata::IsPathListPreference ("PluginList"));
  EXPECT_FALSE (userdata::IsPathListPreference ("LuaScript"));
  }

// ── CopyTree ──────────────────────────────────────────────────────────────

TEST(UserDataCopy, CopiesRecursivelyAndNeverOverAnExistingFile)
  {
  const CString from = TempFolder ("copy_from"), to = TempFolder ("copy_to");
  Put (from + "a.txt", "new a");
  Put (from + "sub\\b.txt", "new b");
  Put (to + "a.txt", "the user's a");

  EXPECT_EQ (1, userdata::CopyTree (from, to));
  EXPECT_EQ (std::string ("the user's a"), Get (to + "a.txt"));
  EXPECT_EQ (std::string ("new b"), Get (to + "sub\\b.txt"));
  EXPECT_EQ (0, userdata::CopyTree (from, to));          // nothing left to copy
  EXPECT_EQ (0, userdata::CopyTree (from + "missing", to));

  RemoveTree (from);
  RemoveTree (to);
  }

// ── ImportInstall, end to end ─────────────────────────────────────────────

TEST(UserDataImport, CopiesAndTranslatesAMUSHclient5Installation)
  {
  const CString folder  = TempFolder ("old");
  const CString shadow  = TempFolder ("shadow");
  const CString data    = TempFolder ("data");
  const CString program = TempFolder ("program2");
  RemoveTree (folder); RemoveTree (shadow); RemoveTree (data); RemoveTree (program);
  Put (program + "fonts\\standard.flf", "flf2a");

  // The folder's database is stale; the shadow's is the one 5.x really used,
  // so its values must win.
  MakePrefsDb (folder + PREFERENCES_DATABASE_FILE,
               "INSERT INTO prefs VALUES ('WordDelimiters', 'stale');");
  MakePrefsDb (shadow + PREFERENCES_DATABASE_FILE, (LPCTSTR) CString (
               "INSERT INTO prefs VALUES ('WordDelimiters', '.,;');"
               "INSERT INTO prefs VALUES ('DefaultLogFileDirectory', '" + folder + "logs\\');"
               "INSERT INTO prefs VALUES ('AsciiArtFont', 'fonts\\standard.flf');"
               "INSERT INTO prefs VALUES ('WorldList', '" + folder + "worlds\\a.mcl*E:\\b.mcl');"
               "INSERT INTO prefs VALUES ('NoSuchPreference', 'x');"
               "INSERT INTO worlds VALUES ('Some world', '1,2,3,4');"));

  Put (folder + "worlds\\a.mcl",
       std::string ("<world script_filename=\"") + (LPCTSTR) folder + "scripts\\a.lua\"/>");
  Put (folder + "worlds\\plugins\\state\\s.xml", "state");
  Put (folder + "worlds\\plugins\\Shipped.xml", "5.x's copy");
  Put (folder + "logs\\big.txt", "a log");
  Put (folder + "MUSHclient.ini",
       std::string ("[Recent File List]\r\nFile1=") + (LPCTSTR) folder + "worlds\\a.mcl\r\n");

  // a word the user added to 5.x's spell checker, in its database only
  {
  SHCreateDirectoryEx (NULL, folder + "spell", NULL);
  sqlite3 * sp = NULL;
  sqlite3_open (folder + "spell\\spell.sqlite", &sp);
  Exec (sp, "CREATE TABLE words (word_id INTEGER PRIMARY KEY, name, metaphone, user INT);"
            "INSERT INTO words VALUES (1, 'grue', 'KR', 1);"
            "INSERT INTO words VALUES (2, 'house', 'HS', 0);");
  sqlite3_close (sp);
  }

  // 6.00's side: a new database as PopulateDatabase leaves it, and the
  // shipped copy of a plugin already seeded
  Put (data + "worlds\\plugins\\Shipped.xml", "6.00's copy");
  sqlite3 * db = NULL;
  sqlite3_open (":memory:", &db);
  Exec (db, "CREATE TABLE control (name VARCHAR(10) NOT NULL PRIMARY KEY, value INT NOT NULL);"
            "CREATE TABLE prefs (name VARCHAR(50) NOT NULL PRIMARY KEY, value TEXT NOT NULL);"
            "CREATE TABLE worlds (name VARCHAR(50) NOT NULL PRIMARY KEY, value TEXT NOT NULL);"
            "INSERT INTO prefs VALUES ('WordDelimiters', 'default');"
            "INSERT INTO prefs VALUES ('DefaultLogFileDirectory', 'default');"
            "INSERT INTO prefs VALUES ('AsciiArtFont', 'default');"
            "INSERT INTO prefs VALUES ('WorldList', '');");

  LegacyInstall old;
  old.strFolder = folder;
  old.strShadow = shadow;
  const CString ini = data + "MUSHclient.ini";
  const CString summary = userdata::ImportInstall (old, db, ini, data, program);

  EXPECT_TRUE (summary.Find ("1 world file ") >= 0);
  EXPECT_TRUE (summary.Find (folder + "logs") >= 0);

  // preferences: the shadow's, translated; unknown ones dropped
  EXPECT_EQ (std::string (".,;"), Scalar (db, "SELECT value FROM prefs WHERE name = 'WordDelimiters'"));
  EXPECT_EQ (std::string ((LPCTSTR) (data + "logs\\")),
             Scalar (db, "SELECT value FROM prefs WHERE name = 'DefaultLogFileDirectory'"));
  EXPECT_EQ (std::string ((LPCTSTR) (program + "fonts\\standard.flf")),
             Scalar (db, "SELECT value FROM prefs WHERE name = 'AsciiArtFont'"));
  EXPECT_EQ (std::string ((LPCTSTR) (data + "worlds\\a.mcl*E:\\b.mcl")),
             Scalar (db, "SELECT value FROM prefs WHERE name = 'WorldList'"));
  EXPECT_EQ (std::string ("<none>"), Scalar (db, "SELECT value FROM prefs WHERE name = 'NoSuchPreference'"));
  EXPECT_EQ (std::string ("1,2,3,4"), Scalar (db, "SELECT value FROM worlds WHERE name = 'Some world'"));
  EXPECT_EQ (std::string ((LPCTSTR) folder), Scalar (db, "SELECT value FROM control WHERE name = 'Imported from'"));

  // files: copied and translated, never over 6.00's own, logs left behind
  EXPECT_EQ (std::string ("<world script_filename=\"") + (LPCTSTR) data + "scripts\\a.lua\"/>",
             Get (data + "worlds\\a.mcl"));
  EXPECT_EQ (std::string ("state"), Get (data + "worlds\\plugins\\state\\s.xml"));
  EXPECT_EQ (std::string ("6.00's copy"), Get (data + "worlds\\plugins\\Shipped.xml"));
  EXPECT_EQ (std::string (""), Get (data + "logs\\big.txt"));

  // the recent files list, translated
  char mru [MAX_PATH] = "";
  GetPrivateProfileString ("Recent File List", "File1", "", mru, sizeof mru, ini);
  EXPECT_STREQ (data + "worlds\\a.mcl", mru);

  // the added word, for 6.00's spell database to pick up when it builds
  const std::string dict = Get (data + "userdict.txt");
  EXPECT_TRUE (dict.find ("grue") != std::string::npos);
  EXPECT_TRUE (dict.find ("house") == std::string::npos);

  // and 5.x's folder exactly as it was: no -journal, no -shm, nothing new
  EXPECT_EQ (std::string ("5.x's copy"), Get (folder + "worlds\\plugins\\Shipped.xml"));
  EXPECT_EQ (INVALID_FILE_ATTRIBUTES, GetFileAttributes (shadow + PREFERENCES_DATABASE_FILE "-journal"));
  EXPECT_EQ (INVALID_FILE_ATTRIBUTES, GetFileAttributes (folder + "spell\\spell.sqlite-shm"));

  sqlite3_close (db);
  RemoveTree (folder); RemoveTree (shadow); RemoveTree (data); RemoveTree (program);
  }

TEST(UserDataImport, NothingToImportSaysNothing)
  {
  const CString folder = TempFolder ("empty_old"), data = TempFolder ("empty_data");
  RemoveTree (folder); RemoveTree (data);
  SHCreateDirectoryEx (NULL, folder, NULL);
  sqlite3 * db = NULL;
  sqlite3_open (":memory:", &db);
  Exec (db, "CREATE TABLE control (name VARCHAR(10) NOT NULL PRIMARY KEY, value INT NOT NULL);"
            "CREATE TABLE prefs (name VARCHAR(50) NOT NULL PRIMARY KEY, value TEXT NOT NULL);"
            "CREATE TABLE worlds (name VARCHAR(50) NOT NULL PRIMARY KEY, value TEXT NOT NULL);");
  LegacyInstall old;
  old.strFolder = folder;
  // No database in the folder, so the registry is consulted -- and on a
  // machine with a 5.x it may answer.  Either way nothing is copied.
  const CString summary = userdata::ImportInstall (old, db, data + "MUSHclient.ini", data, data);
  EXPECT_TRUE (summary.IsEmpty () || summary.Find ("Your settings have been copied") == 0);
  sqlite3_close (db);
  RemoveTree (folder); RemoveTree (data);
  }
