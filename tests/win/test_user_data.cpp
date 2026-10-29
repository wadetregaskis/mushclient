// tests/win/test_user_data.cpp -- importing a MUSHclient 5 installed on Windows
// itself (legacy_import.cpp), with its VirtualStore shadow.
//
// Windows-only because the shadow is: Windows virtualised a 32-bit,
// manifest-less 5.x under Program Files, sending its writes to
// %LOCALAPPDATA%\VirtualStore, so the newest of its files are there.  That is
// the part most easily got backwards, and what a real 5.06 installation looks
// like.  The Wine side, which runs everywhere, is tests/test_legacy_import.cpp.

#include "mc_test.h"

#include "stdafx.h"
#include "MUSHclient.h"
#include "legacy_import.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using legacy::Install;

namespace {

std::string TempFolder (const char * pName)
  {
  std::error_code ec;
  const fs::path p = fs::temp_directory_path (ec) / (std::string ("mc_user_data_") + pName);
  fs::remove_all (p, ec);
  return p.string () + "\\";
  }

void RemoveTree (const std::string & dir)
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

void MakePrefsDb (const std::string & name, const std::string & extraSql)
  {
  std::error_code ec;
  fs::create_directories (fs::path (name).parent_path (), ec);
  sqlite3 * db = NULL;
  sqlite3_open (name.c_str (), &db);
  Exec (db, kSchema);
  Exec (db, extraSql);
  sqlite3_close (db);
  }

}   // namespace

TEST(UserDataTranslate, PathsIntoTheOldFolderOrItsShadowMoveToTheirNewHomes)
  {
  const std::string program = TempFolder ("program");
  Put (program + "fonts\\standard.flf", "flf2a");

  Install old;
  old.folder = old.windowsFolder = "C:\\Old\\MUSHclient\\";
  old.shadow = "C:\\Shadow\\MUSHclient\\";
  const std::string data = "D:\\Data\\MUSHclient\\";

  EXPECT_EQ (std::string ("D:\\Data\\MUSHclient\\worlds\\"),
             legacy::TranslatePath ("C:\\Old\\MUSHclient\\worlds\\", old, data, program));
  EXPECT_EQ (std::string ("D:\\Data\\MUSHclient\\Worlds\\plugins\\state\\"),
             legacy::TranslatePath ("c:\\old\\MUSHCLIENT\\Worlds\\plugins\\state\\", old, data, program));
  EXPECT_EQ (std::string ("D:\\Data\\MUSHclient\\scripts\\mine.lua"),
             legacy::TranslatePath ("C:\\Shadow\\MUSHclient\\scripts\\mine.lua", old, data, program));
  EXPECT_EQ (program + "fonts\\standard.flf",
             legacy::TranslatePath ("fonts\\standard.flf", old, data, program));
  // elsewhere on Windows is already a path on this machine
  EXPECT_EQ (std::string ("E:\\MUD\\logs\\"),
             legacy::TranslatePath ("E:\\MUD\\logs\\", old, data, program));
  EXPECT_EQ (std::string ("C:\\Old\\MUSHclient\\..\\Elsewhere\\x.txt"),
             legacy::TranslatePath ("..\\Elsewhere\\x.txt", old, data, program));

  RemoveTree (program);
  }

TEST(UserDataImport, TheVirtualStoreShadowWinsOverTheFolder)
  {
  const std::string folder  = TempFolder ("old");
  const std::string shadow  = TempFolder ("shadow");
  const std::string data    = TempFolder ("data");
  const std::string program = TempFolder ("program2");

  // The folder's database is stale; the shadow's is the one 5.x really used.
  MakePrefsDb (folder + "mushclient_prefs.sqlite",
               "INSERT INTO prefs VALUES ('WordDelimiters', 'stale');");
  MakePrefsDb (shadow + "mushclient_prefs.sqlite",
               "INSERT INTO prefs VALUES ('WordDelimiters', '.,;');"
               "INSERT INTO prefs VALUES ('DefaultLogFileDirectory', '" + folder + "logs\\');");
  // a world in each: the shadow's copy is the one 5.x last wrote
  Put (folder + "worlds\\a.mcl", "folder's");
  Put (shadow + "worlds\\a.mcl", "shadow's");
  Put (folder + "MUSHclient.ini", "[Recent File List]\r\nFile1=" + folder + "worlds\\a.mcl\r\n");

  sqlite3 * db = NULL;
  sqlite3_open (":memory:", &db);
  Exec (db, kSchema);
  Exec (db, "INSERT INTO prefs VALUES ('WordDelimiters', 'default');"
            "INSERT INTO prefs VALUES ('DefaultLogFileDirectory', 'default');");

  Install old;
  old.folder = old.windowsFolder = folder;
  old.shadow = shadow;
  const legacy::Result result = legacy::Import (old, db, data, program);

  EXPECT_EQ (std::string (".,;"), Scalar (db, "SELECT value FROM prefs WHERE name = 'WordDelimiters'"));
  EXPECT_EQ (data + "logs\\", Scalar (db, "SELECT value FROM prefs WHERE name = 'DefaultLogFileDirectory'"));
  EXPECT_EQ (std::string ("shadow's"), Get (data + "worlds\\a.mcl"));
  ASSERT_EQ ((size_t) 1, result.recentFiles.size ());
  EXPECT_EQ (data + "worlds\\a.mcl", result.recentFiles [0]);
  EXPECT_EQ (1, result.worlds);
  EXPECT_EQ (std::string ("stale"), [&] {
    sqlite3 * o = NULL;
    sqlite3_open ((folder + "mushclient_prefs.sqlite").c_str (), &o);
    const std::string v = Scalar (o, "SELECT value FROM prefs WHERE name = 'WordDelimiters'");
    sqlite3_close (o);
    return v; } ());

  sqlite3_close (db);
  RemoveTree (folder); RemoveTree (shadow); RemoveTree (data); RemoveTree (program);
  }

TEST(UserDataFind, ThisMachinesOwn5xIsFoundIfThereIsOne)
  {
  // Not an assertion about this machine having one: that each one found
  // qualifies -- a folder that exists, with 5.x's database or exe in it.
  for (const Install & old : legacy::Find ("Z:\\no-data\\", "Z:\\no-program\\"))
    {
    EXPECT_TRUE (fs::is_directory (fs::path (old.folder)));
    EXPECT_EQ (old.folder, old.windowsFolder);
    EXPECT_TRUE (old.prefix.empty ());
    }
  }
