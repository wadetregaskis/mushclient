// tests/test_bundled_files.cpp -- bundled_files.cpp: keeping the user's copies
// of the bundled plugins up to date across upgrades.
//
// Each test plays a sequence of versions against one data folder: a "shipped"
// tree that changes between Sync calls, as the program's own folder does
// between releases, and the user's copies, which the test edits and deletes
// between them as a user would.

#include "mc_test.h"

#include "stdafx.h"
#include "bundled_files.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string N (std::string s)
  {
  for (char & c : s)
    if (c == '/')
      c = (char) fs::path::preferred_separator;
  return s;
  }

std::string Temp (const char * name)
  {
  std::error_code ec;
  const fs::path p = fs::temp_directory_path (ec) / (std::string ("mc_bundled_") + name);
  fs::remove_all (p, ec);
  fs::create_directories (p, ec);
  return p.string () + (char) fs::path::preferred_separator;
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
  std::ofstream (fs::path (name), std::ios::binary | std::ios::trunc) << contents;
  }

std::string Get (const std::string & name)
  {
  std::ifstream f (fs::path (name), std::ios::binary);
  if (!f)
    return "<missing>";
  std::ostringstream ss;
  ss << f.rdbuf ();
  return ss.str ();
  }

std::string Plugin (const char * name, const char * version)
  {
  return std::string ("<muclient>\n<plugin\n   name=\"") + name +
         "\"\n   author=\"x\"\n   version=\"" + version + "\"\n>\n</plugin>\n</muclient>\n";
  }

// Records what it was asked, and answers as told.
struct Asker
  {
  bundled::Answer answer;
  std::vector<std::string> asked;      // relative paths
  std::vector<std::string> names;
  std::vector<std::string> backups;
  bundled::AskFn fn ()
    {
    return [this] (const std::string & relative, const std::string & name, const std::string & backup) {
      asked.push_back (relative);
      names.push_back (name);
      backups.push_back (backup);
      return answer;
    };
    }
  };

}   // namespace

TEST(BundledFiles, AnUpgradeReplacesWhatTheUserLeftAloneAndAsksAboutTheRest)
  {
  const std::string ship = Temp ("ship"), data = Temp ("data");
  const std::string manifest = data + "bundled-files.txt";
  const std::vector<std::string> trees = { "worlds", "quow_plugins" };

  // ── version 1, a first run: everything is copied ──
  Put (ship + N ("worlds/plugins/Plain.xml"),   Plugin ("Plain", "1"));
  Put (ship + N ("worlds/plugins/Edited.xml"),  Plugin ("Edited", "1"));
  Put (ship + N ("worlds/plugins/Deleted.xml"), Plugin ("Deleted", "1"));
  Put (ship + N ("worlds/Discworld.mcl"),       "world 1");
  Put (ship + N ("quow_plugins/maps/rooms.db"), "maps 1");
  Put (ship + N ("elsewhere/not-synced.txt"),   "x");
  Asker ask;
  bundled::Stats s = bundled::Sync (ship, data, trees, manifest, ask.fn ());
  EXPECT_EQ (5, s.copied);
  EXPECT_EQ (Plugin ("Plain", "1"), Get (data + N ("worlds/plugins/Plain.xml")));
  EXPECT_EQ (std::string ("maps 1"), Get (data + N ("quow_plugins/maps/rooms.db")));
  EXPECT_EQ (std::string ("<missing>"), Get (data + N ("elsewhere/not-synced.txt")));
  EXPECT_TRUE (ask.asked.empty ());

  // ── the user's doing ──
  Put (data + N ("worlds/plugins/Edited.xml"), Plugin ("Edited", "1") + "<!-- mine -->");
  fs::remove (fs::path (data + N ("worlds/plugins/Deleted.xml")));
  Put (data + N ("worlds/Discworld.mcl"), "world 1, used");
  Put (data + N ("worlds/plugins/Mine.xml"), "their own plugin");

  // ── version 2 ──
  Put (ship + N ("worlds/plugins/Plain.xml"),   Plugin ("Plain", "2"));
  Put (ship + N ("worlds/plugins/Edited.xml"),  Plugin ("Edited", "2"));
  Put (ship + N ("worlds/plugins/Deleted.xml"), Plugin ("Deleted", "2"));
  Put (ship + N ("worlds/plugins/New.xml"),     Plugin ("New", "2"));
  Put (ship + N ("worlds/Discworld.mcl"),       "world 2");
  Put (ship + N ("quow_plugins/maps/rooms.db"), "maps 2");
  ask.answer.upgrade = false;            // "Keep my copy"
  s = bundled::Sync (ship, data, trees, manifest, ask.fn ());

  // left alone: upgraded, unasked
  EXPECT_EQ (Plugin ("Plain", "2"), Get (data + N ("worlds/plugins/Plain.xml")));
  EXPECT_EQ (std::string ("maps 2"), Get (data + N ("quow_plugins/maps/rooms.db")));
  // changed: asked about, by the plugin's own name, and kept as told
  ASSERT_EQ ((size_t) 1, ask.asked.size ());
  EXPECT_EQ (N ("worlds/plugins/Edited.xml"), ask.asked [0]);
  EXPECT_EQ (std::string ("Edited"), ask.names [0]);
  EXPECT_EQ (Plugin ("Edited", "1") + "<!-- mine -->", Get (data + N ("worlds/plugins/Edited.xml")));
  // deleted: stays deleted; new: copied; a world: never replaced, never asked
  EXPECT_EQ (std::string ("<missing>"), Get (data + N ("worlds/plugins/Deleted.xml")));
  EXPECT_EQ (Plugin ("New", "2"), Get (data + N ("worlds/plugins/New.xml")));
  EXPECT_EQ (std::string ("world 1, used"), Get (data + N ("worlds/Discworld.mcl")));
  // their own plugin, which never came from here: untouched
  EXPECT_EQ (std::string ("their own plugin"), Get (data + N ("worlds/plugins/Mine.xml")));
  EXPECT_EQ (1, s.copied);
  EXPECT_EQ (2, s.upgraded);
  EXPECT_EQ (1, s.kept);

  // ── version 3: kept last time, not remembered, so asked again; upgrade ──
  Put (ship + N ("worlds/plugins/Edited.xml"), Plugin ("Edited", "3"));
  ask.asked.clear ();
  ask.answer.upgrade = true;
  bundled::Sync (ship, data, trees, manifest, ask.fn ());
  ASSERT_EQ ((size_t) 1, ask.asked.size ());
  EXPECT_EQ (Plugin ("Edited", "3"), Get (data + N ("worlds/plugins/Edited.xml")));
  EXPECT_EQ (Plugin ("Edited", "1") + "<!-- mine -->",
             Get (data + N ("worlds/plugins/Edited.xml.backup")));
  EXPECT_EQ (std::string ("Edited.xml.backup"), ask.backups [0]);

  // ── and now it is the shipped one again, unchanged: upgraded unasked ──
  Put (ship + N ("worlds/plugins/Edited.xml"), Plugin ("Edited", "4"));
  ask.asked.clear ();
  bundled::Sync (ship, data, trees, manifest, ask.fn ());
  EXPECT_TRUE (ask.asked.empty ());
  EXPECT_EQ (Plugin ("Edited", "4"), Get (data + N ("worlds/plugins/Edited.xml")));

  Remove (ship); Remove (data);
  }

TEST(BundledFiles, ARememberedChoiceIsNotAskedAgain)
  {
  const std::string ship = Temp ("ship_r"), data = Temp ("data_r");
  const std::string manifest = data + "bundled-files.txt";
  const std::vector<std::string> trees = { "worlds" };
  Put (ship + N ("worlds/plugins/Keep.xml"),    Plugin ("Keep", "1"));
  Put (ship + N ("worlds/plugins/Upgrade.xml"), Plugin ("Upgrade", "1"));
  Asker ask;
  bundled::Sync (ship, data, trees, manifest, ask.fn ());
  Put (data + N ("worlds/plugins/Keep.xml"),    "mine, keep");
  Put (data + N ("worlds/plugins/Upgrade.xml"), "mine, replace");

  // version 2: asked about both; "don't ask again", keeping one, upgrading the other
  Put (ship + N ("worlds/plugins/Keep.xml"),    Plugin ("Keep", "2"));
  Put (ship + N ("worlds/plugins/Upgrade.xml"), Plugin ("Upgrade", "2"));
  bundled::Sync (ship, data, trees, manifest,
                 [] (const std::string & relative, const std::string &, const std::string &) {
                   bundled::Answer a;
                   a.upgrade  = relative.find ("Upgrade") != std::string::npos;
                   a.remember = true;
                   return a;
                 });
  EXPECT_EQ (std::string ("mine, keep"), Get (data + N ("worlds/plugins/Keep.xml")));
  EXPECT_EQ (Plugin ("Upgrade", "2"), Get (data + N ("worlds/plugins/Upgrade.xml")));

  // they change both again; version 3 asks about neither, and does as remembered
  Put (data + N ("worlds/plugins/Keep.xml"),    "mine, keep, again");
  Put (data + N ("worlds/plugins/Upgrade.xml"), "mine, replace, again");
  Put (ship + N ("worlds/plugins/Keep.xml"),    Plugin ("Keep", "3"));
  Put (ship + N ("worlds/plugins/Upgrade.xml"), Plugin ("Upgrade", "3"));
  bundled::Sync (ship, data, trees, manifest, ask.fn ());
  EXPECT_TRUE (ask.asked.empty ());
  EXPECT_EQ (std::string ("mine, keep, again"), Get (data + N ("worlds/plugins/Keep.xml")));
  EXPECT_EQ (Plugin ("Upgrade", "3"), Get (data + N ("worlds/plugins/Upgrade.xml")));
  // a second backup does not overwrite the first
  EXPECT_EQ (std::string ("mine, replace"),
             Get (data + N ("worlds/plugins/Upgrade.xml.backup")));
  EXPECT_EQ (std::string ("mine, replace, again"),
             Get (data + N ("worlds/plugins/Upgrade.xml.backup2")));

  Remove (ship); Remove (data);
  }

TEST(BundledFiles, ACopyThatPredatesTheManifestIsAskedAboutIfItDiffers)
  {
  // A data folder seeded before there was a manifest (or by hand): a file the
  // same as the shipped one is simply recorded; one that differs might be the
  // user's work, so it is not replaced unasked.
  const std::string ship = Temp ("ship_u"), data = Temp ("data_u");
  Put (ship + N ("worlds/plugins/Same.xml"),  Plugin ("Same", "2"));
  Put (ship + N ("worlds/plugins/Other.xml"), Plugin ("Other", "2"));
  Put (data + N ("worlds/plugins/Same.xml"),  Plugin ("Same", "2"));
  Put (data + N ("worlds/plugins/Other.xml"), Plugin ("Other", "1"));
  Asker ask;
  bundled::Sync (ship, data, { "worlds" }, data + "bundled-files.txt", ask.fn ());
  ASSERT_EQ ((size_t) 1, ask.asked.size ());
  EXPECT_EQ (N ("worlds/plugins/Other.xml"), ask.asked [0]);
  EXPECT_EQ (Plugin ("Other", "1"), Get (data + N ("worlds/plugins/Other.xml")));
  Remove (ship); Remove (data);
  }

TEST(BundledFiles, AChangedCopyIsNotAskedAboutWhenThereIsNothingNew)
  {
  // They changed a plugin; the next version ships the same one as before.
  // There is nothing to offer them, so nothing to ask -- at this version or
  // any later one that still ships it unchanged.
  const std::string ship = Temp ("ship_n"), data = Temp ("data_n");
  const std::string manifest = data + "bundled-files.txt";
  Put (ship + N ("worlds/plugins/Same.xml"), Plugin ("Same", "1"));
  Asker ask;
  bundled::Sync (ship, data, { "worlds" }, manifest, ask.fn ());
  Put (data + N ("worlds/plugins/Same.xml"), "mine");
  bundled::Sync (ship, data, { "worlds" }, manifest, ask.fn ());
  bundled::Sync (ship, data, { "worlds" }, manifest, ask.fn ());
  EXPECT_TRUE (ask.asked.empty ());
  EXPECT_EQ (std::string ("mine"), Get (data + N ("worlds/plugins/Same.xml")));

  // and when a new one does come, they are asked
  Put (ship + N ("worlds/plugins/Same.xml"), Plugin ("Same", "2"));
  bundled::Sync (ship, data, { "worlds" }, manifest, ask.fn ());
  EXPECT_EQ ((size_t) 1, ask.asked.size ());
  Remove (ship); Remove (data);
  }

TEST(BundledFiles, DataAPluginWroteIsKeptUnasked)
  {
  // The mapper writes its database beside its maps; a new map database in
  // the next version is not a reason to ask about, or replace, theirs.  A
  // plugin's state is never bundled at all, even where the program's folder
  // has some (a MUSHclient 5's, installed over).
  const std::string ship = Temp ("ship_d"), data = Temp ("data_d");
  const std::string manifest = data + "bundled-files.txt";
  const std::vector<std::string> trees = { "worlds", "quow_plugins" };
  Put (ship + N ("quow_plugins/maps/rooms.db"), "maps 1");
  Put (ship + N ("worlds/plugins/state/abc-state.xml"), "<state 1>");
  Asker ask;
  bundled::Sync (ship, data, trees, manifest, ask.fn ());
  EXPECT_EQ (std::string ("<missing>"), Get (data + N ("worlds/plugins/state/abc-state.xml")));
  Put (data + N ("quow_plugins/maps/rooms.db"), "maps 1, with their notes");
  Put (ship + N ("quow_plugins/maps/rooms.db"), "maps 2");
  const bundled::Stats s = bundled::Sync (ship, data, trees, manifest, ask.fn ());
  EXPECT_TRUE (ask.asked.empty ());
  EXPECT_EQ (1, s.kept);
  EXPECT_EQ (std::string ("maps 1, with their notes"), Get (data + N ("quow_plugins/maps/rooms.db")));
  Remove (ship); Remove (data);
  }

TEST(BundledFiles, TheFingerprintNoticesANewBuild)
  {
  const std::string ship = Temp ("ship_f");
  Put (ship + N ("worlds/plugins/A.xml"), "one");
  const std::string before = bundled::Fingerprint (ship, { "worlds" });
  EXPECT_EQ (before, bundled::Fingerprint (ship, { "worlds" }));
  Put (ship + N ("worlds/plugins/A.xml"), "one, longer");
  EXPECT_TRUE (before != bundled::Fingerprint (ship, { "worlds" }));
  Remove (ship);
  }

TEST(BundledFiles, AWorldIsTransformedAsItIsCopied)
  {
  const std::string ship = Temp ("ship_w"), data = Temp ("data_w");
  Put (ship + N ("worlds/A.mcl"), "path: OLD");
  Put (ship + N ("worlds/plugins/P.xml"), "path: OLD");
  bundled::Sync (ship, data, { "worlds" }, data + "bundled-files.txt", nullptr,
                 [] (const std::string & text) {
                   std::string t = text;
                   t.replace (t.find ("OLD"), 3, "NEW");
                   return t;
                 });
  EXPECT_EQ (std::string ("path: NEW"), Get (data + N ("worlds/A.mcl")));
  EXPECT_EQ (std::string ("path: OLD"), Get (data + N ("worlds/plugins/P.xml")));
  Remove (ship); Remove (data);
  }

TEST(BundledFiles, APluginIsCalledByItsOwnName)
  {
  EXPECT_EQ (std::string ("Quow_Minimap"),
             bundled::PluginName ("<?xml?>\n<muclient>\n<plugin\n  name=\"Quow_Minimap\"\n  id=\"x\"\n>"));
  EXPECT_EQ (std::string ("Single"), bundled::PluginName ("<plugin name = 'Single' >"));
  EXPECT_EQ (std::string ("Real"),
             bundled::PluginName ("<plugin plugin_name=\"no\" name=\"Real\">"));
  EXPECT_EQ (std::string (""), bundled::PluginName ("<world name=\"not a plugin\">"));
  }

MC_TEST_MAIN()
