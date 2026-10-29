// legacy_import.h -- copying a MUSHclient 5 installation's settings and files
// into MUSHclient 6.
//
// MUSHclient 6 does not share files or settings with 5.x (see user_data.h for
// where 6.00 keeps its own).  It offers to import them instead: once, on its
// first run, if it finds a 5.x, and whenever asked through File > Import from
// MUSHclient 5.  On Windows a 5.x is found where its installer put it (with its
// VirtualStore shadow, where a virtualised 5.x really wrote).  On macOS and
// Linux 5.x only ever ran under Wine, so the places looked in are the usual
// Wine prefixes and bottles -- none that macOS guards with a permission prompt,
// since the search runs unasked; anything else can be chosen by hand.
//
// What is copied: worlds, scripts, sounds and quow_plugins\ (the Discworld
// mapper's data), never over a file already there; the global preferences and
// world window positions, which replace 6.00's; the recent files list; and the
// words added to its spell checker.  Every path that pointed into the old
// folder is translated -- in the preferences, and inside world files -- the
// user's own files to the data folder, the program's to the program's folder.
// Logs are left where they are.  The 5.x installation is only ever read: its
// databases are opened from temporary copies.
//
// Shared by both builds.  Paths are std::string in the platform's own form
// (backslashes on Windows, slashes elsewhere), each folder ending in its
// separator; the paths 5.x itself wrote are Windows paths wherever it ran.

#pragma once

#include <string>
#include <utility>
#include <vector>

struct sqlite3;

namespace legacy {

struct Install
  {
  std::string folder;         // its folder, on this machine
  std::string shadow;         // Windows: its VirtualStore shadow, if it has one
  std::string windowsFolder;  // its folder as 5.x knew it: "C:\Program Files (x86)\MUSHclient\"
  std::string prefix;         // the Wine prefix it ran in, if it did
  };

// The 5.x installations found in the usual places, best first.  Never one in
// `dataDir` (a portable 6.00 there).
std::vector<Install> Find (const std::string & dataDir, const std::string & programDir);

// A folder the user chose: 5.x's own folder, or a Wine prefix or bottle with a
// 5.x in it.  False if it is neither.
bool FromFolder (const std::string & folder, Install & old);

// The installations of 5.x in one Wine prefix.
std::vector<Install> InPrefix (const std::string & prefix, const std::string & programDir);

// A path as 5.x had it -- absolute, or relative to its folder -- as 6.00 should
// have it.  Under the old folder, the user's own (worlds\, logs\, sounds\,
// scripts\, quow_plugins\) move to dataDir, and the program's are the program's
// if programDir has them; `pResolved`, if given, says false when it has not.
// A path elsewhere is kept, made a path on this machine under Wine.
std::string TranslatePath (const std::string & path, const Install & old,
                           const std::string & dataDir, const std::string & programDir,
                           bool * pResolved = nullptr);

// The same for every path into the old folder in a world file or other text.
std::string TranslateText (const std::string & text, const Install & old,
                           const std::string & dataDir, const std::string & programDir);

bool IsPathPreference (const char * pName);
bool IsPathListPreference (const char * pName);   // "*"-separated paths

// Copy a folder's files into another, recursively, never over a file already
// there; a world file is rewritten with TranslateText on the way when `pOld`
// is given.  Returns how many were copied; `pWorlds` counts the world files.
int CopyTree (const std::string & from, const std::string & to,
              const Install * pOld = nullptr,
              const std::string & dataDir = "", const std::string & programDir = "",
              int * pWorlds = nullptr);

// The values under one key of 5.x's registry: the real one on Windows, the
// prefix's user.reg under Wine.  DWORDs as decimal.
typedef std::vector<std::pair<std::string, std::string>> RegistryValues;
RegistryValues ReadRegistry (const Install & old, const std::string & key);
RegistryValues ParseWineRegistry (const std::string & text, const std::string & key);

struct Result
  {
  bool prefs  = false;    // global preferences imported
  int  files  = 0;
  int  worlds = 0;
  int  words  = 0;        // spelling words added
  std::vector<std::string> recentFiles;   // oldest first, translated
  bool Anything () const { return prefs || files || words || !recentFiles.empty (); }
  };

// Import from `old` into the preferences database `db` (whose prefs table
// PopulateDatabase has filled) and the data folder.  The caller wraps it in a
// transaction, and re-reads the preferences after.
Result Import (const Install & old, sqlite3 * db,
               const std::string & dataDir, const std::string & programDir);

// For a message: the folder, without its trailing separator.
std::string Where (const Install & old);

// What was done, in a sentence or two.
std::string Describe (const Install & old, const Result & result);

}   // namespace legacy
