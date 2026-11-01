// tests/test_autosave.cpp -- saving a world a few seconds after it changes, and what
// closing does about it: doc_autosave_shared.cpp, through a real document and a real
// world file.  Also the command history file.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "command_history_store.h"
#include "world_backups.h"
#include "scripting/errors.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path TempFolder (const char * name)
  {
  const char * dir = getenv ("TEMP");
  fs::path path = fs::path (dir ? dir : "/tmp") / (std::string ("mc_autosave_") + name);
  std::error_code ec;
  fs::remove_all (path, ec);
  fs::create_directories (path, ec);
  return path;
  }

std::string ReadText (const fs::path & path)
  {
  std::ifstream f (path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf ();
  return s.str ();
  }

// A world with a name, and the option on, whatever it was.
struct AutosaveFixture
  {
  CMUSHclientDoc doc;
  fs::path folder;
  fs::path world;
  unsigned int savedOption;

  explicit AutosaveFixture (const char * name, const bool bNamed = true)
    {
    savedOption = App.m_bAutosaveWorlds;
    App.m_bAutosaveWorlds = 1;
    folder = TempFolder (name);
    world = folder / "autosave test.mcl";
    doc.m_mush_name = "Autosave Test";
    if (bNamed)
      doc.SetPathName (world.u8string ().c_str (), FALSE);
    doc.SetModifiedFlag (FALSE);    // a bare document says it has changed
    }

  ~AutosaveFixture ()
    {
    App.m_bAutosaveWorlds = savedOption;
    doc.SetModifiedFlag (FALSE);
    }

  // As if the change had been made this long ago.
  void Age (const time_t seconds)
    {
    doc.m_tAutosaveLastChange -= seconds;
    doc.m_tAutosaveFirstChange -= seconds;
    }
  };

} // anonymous namespace

TEST (Autosave, WaitsForAQuietMoment)
  {
  AutosaveFixture f ("quiet");
  f.doc.SetModifiedFlag ();
  EXPECT_TRUE (f.doc.m_tAutosaveFirstChange != 0);

  // Not straight away.
  f.doc.AutosaveTick ();
  EXPECT_FALSE (fs::exists (f.world));
  EXPECT_TRUE (f.doc.IsModified () != 0);

  // A further change starts the wait again.
  f.Age (6);
  f.doc.SetModifiedFlag ();
  f.doc.AutosaveTick ();
  EXPECT_FALSE (fs::exists (f.world));

  // And once it has been quiet for a few seconds, it is saved.
  f.Age (8);
  f.doc.AutosaveTick ();
  ASSERT_TRUE (fs::exists (f.world));
  EXPECT_TRUE (f.doc.IsModified () == 0);
  EXPECT_TRUE (ReadText (f.world).find ("Autosave Test") != std::string::npos);
  EXPECT_TRUE (f.doc.m_tAutosaveFirstChange == 0);
  }

TEST (Autosave, AConstantStreamOfChangesStillGetsSaved)
  {
  AutosaveFixture f ("stream");
  f.doc.SetModifiedFlag ();
  f.Age (61);
  f.doc.SetModifiedFlag ();       // only just changed again
  EXPECT_TRUE (time (NULL) - f.doc.m_tAutosaveLastChange < 2);
  f.doc.AutosaveTick ();
  EXPECT_TRUE (fs::exists (f.world));
  EXPECT_TRUE (f.doc.IsModified () == 0);
  }

TEST (Autosave, NothingHappensWithNothingChanged)
  {
  AutosaveFixture f ("idle");
  f.doc.AutosaveTick ();
  EXPECT_FALSE (fs::exists (f.world));
  }

TEST (Autosave, TheGlobalPreferenceTurnsItOff)
  {
  AutosaveFixture f ("off");
  App.m_bAutosaveWorlds = 0;
  f.doc.SetModifiedFlag ();
  f.Age (100);
  f.doc.AutosaveTick ();
  EXPECT_FALSE (fs::exists (f.world));
  EXPECT_TRUE (f.doc.IsModified () != 0);
  }

TEST (Autosave, ANeverSavedWorldWaitsToBeNamed)
  {
  AutosaveFixture f ("unnamed", false);
  f.doc.SetModifiedFlag ();
  f.Age (100);
  f.doc.AutosaveTick ();
  EXPECT_TRUE (f.doc.IsModified () != 0);
  EXPECT_TRUE (fs::is_empty (f.folder));

  // And closing it doesn't save it either: that's for the question it still asks.
  f.doc.SaveWorldBeforeClosing ();
  EXPECT_TRUE (f.doc.IsModified () != 0);
  }

TEST (Autosave, VariablesAreSavedOnASlowerCount)
  {
  AutosaveFixture f ("variables");
  f.doc.m_bVariablesChanged = true;

  // Seen for the first time.  Nothing marks these as they happen, as they can happen
  // on every line; so nothing restarts this wait either.
  f.doc.AutosaveTick ();
  EXPECT_TRUE (f.doc.m_tAutosaveFirstChange != 0);
  EXPECT_FALSE (fs::exists (f.world));

  f.Age (30);
  f.doc.AutosaveTick ();
  EXPECT_FALSE (fs::exists (f.world));

  f.Age (31);
  f.doc.AutosaveTick ();
  EXPECT_TRUE (fs::exists (f.world));
  EXPECT_FALSE (f.doc.m_bVariablesChanged);
  }

TEST (Autosave, AFailureIsQuietAndKeepsTheWorldModified)
  {
  AutosaveFixture f ("readonly");
  // A path it can't create.  No dialog: the test host fails any message box it sees.
  f.doc.SetPathName ((f.folder / "no such folder" / "x.mcl").u8string ().c_str (), FALSE);
  f.doc.SetModifiedFlag ();
  f.Age (100);

  const long before = f.doc.m_LineList.GetCount ();
  f.doc.AutosaveTick ();
  EXPECT_TRUE (f.doc.IsModified () != 0);
  EXPECT_TRUE (f.doc.m_tAutosaveRetry > time (NULL));
  EXPECT_GT ((long) f.doc.m_LineList.GetCount (), before - 1);

  // It doesn't try again for a while.
  const time_t retry = f.doc.m_tAutosaveRetry;
  f.doc.AutosaveTick ();
  EXPECT_TRUE (f.doc.m_tAutosaveRetry == retry);
  }

TEST (Autosave, ClosingSavesWithoutAsking)
  {
  AutosaveFixture f ("closing");
  f.doc.SetModifiedFlag ();
  f.doc.SaveWorldBeforeClosing ();
  EXPECT_TRUE (fs::exists (f.world));
  EXPECT_TRUE (f.doc.IsModified () == 0);

  // Including when only variables have changed.
  fs::remove (f.world);
  f.doc.m_bVariablesChanged = true;
  f.doc.SaveWorldBeforeClosing ();
  EXPECT_TRUE (fs::exists (f.world));
  EXPECT_FALSE (f.doc.m_bVariablesChanged);
  }

TEST (Autosave, ClosingWithItOffIsAsItWas)
  {
  AutosaveFixture f ("closing_off");
  App.m_bAutosaveWorlds = 0;
  f.doc.SetModifiedFlag ();
  f.doc.SaveWorldBeforeClosing ();
  EXPECT_FALSE (fs::exists (f.world));

  // The per-world option still works.
  f.doc.m_bSaveWorldAutomatically = TRUE;
  f.doc.SaveWorldBeforeClosing ();
  EXPECT_TRUE (fs::exists (f.world));
  }

TEST (Autosave, ACopyIsKeptOfWhatItReplaces)
  {
  AutosaveFixture f ("backup");
  // The folder the world's backups go in is in the working directory.  Start clean.
  const fs::path backups = fs::u8path ((const char *) f.doc.WorldBackupsFolder ());
  std::error_code ec;
  fs::remove_all (backups, ec);

  // The first save has nothing to copy.
  f.doc.SetModifiedFlag ();
  f.Age (10);
  f.doc.AutosaveTick ();
  ASSERT_TRUE (fs::exists (f.world));
  size_t copies = 0;
  for (fs::directory_iterator it (backups, ec), end; !ec && it != end; it.increment (ec))
    copies++;
  EXPECT_EQ (0u, copies);
  const std::string first = ReadText (f.world);

  // A world opened in a new session, which changes something: the old one is kept.
  AutosaveFixture again ("backup2");
  again.doc.SetPathName (f.world.u8string ().c_str (), FALSE);
  again.doc.m_mush_name = "Now Called Something Else";
  again.doc.SetModifiedFlag ();
  again.Age (10);
  again.doc.AutosaveTick ();
  EXPECT_TRUE (ReadText (f.world).find ("Now Called Something Else") != std::string::npos);

  copies = 0;
  bool bHasOriginal = false;
  for (fs::directory_iterator it (backups, ec), end; !ec && it != end; it.increment (ec))
    {
    copies++;
    if (ReadText (it->path ()) == first)
      bHasOriginal = true;
    }
  EXPECT_EQ (1u, copies);
  EXPECT_TRUE (bHasOriginal);
  fs::remove_all (backups, ec);
  }

// What the stock Automatic_Backup plugin does: world.Save (name, true), a copy under
// another name.  It's no reason to take a backup.
TEST (Autosave, SavingACopyStillWorks)
  {
  AutosaveFixture f ("plugin_copy");
  const fs::path backups = fs::u8path ((const char *) f.doc.WorldBackupsFolder ());
  std::error_code ec;
  fs::remove_all (backups, ec);

  {
  std::ofstream existing (f.world, std::ios::binary);
  existing << "the world file";
  }
  const fs::path copy = f.folder / "copy.MCL";
  f.doc.SetModifiedFlag ();
  EXPECT_FALSE (f.doc.Save (copy.u8string ().c_str ()));   // which says false for success
  EXPECT_TRUE (fs::exists (copy));
  EXPECT_FALSE (fs::exists (backups));
  EXPECT_EQ (std::string ("the world file"), ReadText (f.world));
  }

// A world file with no ID gets one, and the file has to end up with it, or what's
// filed under it would never be found again.
TEST (Autosave, AWorldWithNoIDGetsOneSaved)
  {
  AutosaveFixture f ("noid");
  f.doc.m_bWorldIDIsNew = true;
  f.doc.m_strWorldID = "testnoid";
  f.doc.AutosaveTick ();
  EXPECT_TRUE (f.doc.IsModified () != 0);
  EXPECT_FALSE (f.doc.m_bWorldIDIsNew);
  f.Age (10);
  f.doc.AutosaveTick ();
  EXPECT_TRUE (ReadText (f.world).find ("testnoid") != std::string::npos);
  }

// Every world file already out there doesn't mention save_scrollback, and it's to be on
// for them, with an off switch that survives being saved and loaded.
TEST (SaveScrollbackOption, OnForAnExistingWorldAndOffStaysOff)
  {
  const fs::path folder = TempFolder ("option");
  const fs::path old = folder / "old.mcl";
  {
  std::ofstream f (old, std::ios::binary);
  f << "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
       "<world name=\"Old\" site=\"example.com\" port=\"4000\" world_file_version=\"15\"></world>\n</muclient>\n";
  }
  {
  CMUSHclientDoc fresh;
  EXPECT_EQ (1, (int) fresh.m_bSaveScrollback);
  ASSERT_TRUE (fresh.LoadWorldFromFile (old.u8string ().c_str ()));
  EXPECT_EQ (1, (int) fresh.m_bSaveScrollback);

  // Turned off, saved, and loaded again.
  fresh.m_bSaveScrollback = 0;
  fresh.SetPathName ((folder / "saved.mcl").u8string ().c_str (), FALSE);
  ASSERT_TRUE (fresh.OnSaveDocument ((folder / "saved.mcl").u8string ().c_str ()));
  CMUSHclientDoc again;
  ASSERT_TRUE (again.LoadWorldFromFile ((folder / "saved.mcl").u8string ().c_str ()));
  EXPECT_EQ (0, (int) again.m_bSaveScrollback);
  }
  }

// ---------------- command history ----------------

TEST (CommandHistory, RoundTrips)
  {
  const fs::path folder = TempFolder ("history");
  const std::string path = (folder / "sub" / "w.history").u8string ();
  const std::vector<std::string> commands = {
    "look", "say hello\nsay again", "back\\slash", "\\n is not a newline", "caf\xC3\xA9", "last" };
  EXPECT_TRUE (command_history::Save (path, commands));
  std::vector<std::string> got;
  EXPECT_TRUE (command_history::Load (path, got));
  EXPECT_TRUE (commands == got);
  EXPECT_FALSE (fs::exists (fs::u8path (path + ".tmp")));
  }

TEST (CommandHistory, KeepsTheNewest)
  {
  const fs::path folder = TempFolder ("history_limit");
  const std::string path = (folder / "w.history").u8string ();
  command_history::Save (path, { "one", "two", "three", "four" });
  std::vector<std::string> got;
  EXPECT_TRUE (command_history::Load (path, got, 2));
  ASSERT_EQ (2u, got.size ());
  EXPECT_TRUE (got [0] == "three");
  EXPECT_TRUE (got [1] == "four");
  }

TEST (CommandHistory, AWindowsMultiLineCommandIsOneEntry)
  {
  EXPECT_TRUE (command_history::EncodeLine ("a\r\nb") == "a\\nb");
  EXPECT_TRUE (command_history::DecodeLine ("a\\nb") == "a\nb");
  }

TEST (CommandHistory, MissingAndOldFiles)
  {
  const fs::path folder = TempFolder ("history_old");
  std::vector<std::string> got { "untouched" };
  EXPECT_FALSE (command_history::Load ((folder / "none").u8string (), got));

  // From before there was any escaping, with Windows line ends and a blank line.
  {
  std::ofstream f (folder / "old", std::ios::binary);
  f << "c:\\path\\to\\thing\r\n\r\nplain\n";
  }
  EXPECT_TRUE (command_history::Load ((folder / "old").u8string (), got));
  ASSERT_EQ (2u, got.size ());
  EXPECT_TRUE (got [0] == "c:\\path\\to\\thing");
  EXPECT_TRUE (got [1] == "plain");
  }
