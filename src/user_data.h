// user_data.h -- where MUSHclient 6 keeps the files it writes, on Windows, and
// the one-off import of a MUSHclient 5 installation's settings and worlds.
//
// MUSHclient 5 wrote everything into the folder it was installed in: the
// preferences database, MUSHclient.ini, worlds, plugin state, logs, the spell
// checker's database.  Installed under Program Files that only ever worked
// because Windows virtualised a 32-bit program with no manifest, quietly
// redirecting those writes to %LOCALAPPDATA%\VirtualStore.  A 64-bit program,
// and any program with a manifest (6.00 has one), gets no such redirection: the
// writes simply fail.  So an installed 6.00 keeps them in a folder of its own,
// %APPDATA%\MUSHclient\, as the port already does (~/Library/Application
// Support/MUSHclient/ on macOS, ~/.local/share/MUSHclient/ on Linux):
//
//   mushclient_prefs.sqlite, MUSHclient.ini   global preferences, recent files
//   worlds\, worlds\plugins\, ...\state\      worlds, plugins, plugin state
//   logs\, sounds\, scripts\                  the user's own
//   spell.sqlite, userdict.txt                the spell checker's
//
// and makes that the current directory, which is what relative paths in world
// files and plugins are relative to.  What ships with the program stays in the
// program's folder and is found from there (lua\, fonts\, locale\, spell\...),
// except the parts that are user data to begin with -- the bundled plugins and
// worlds, and the Discworld edition's quow_plugins\ with its maps -- which are
// copied into the data folder, never over a file already there, the first time
// a version runs (as the port's SeedBundledUserData does).
//
// A portable copy -- the zip, which has a portable.txt beside the exe -- keeps
// everything beside the exe instead, as MUSHclient always has.
//
// 6.00 does not share files or settings with 5.x: not its folder, not its
// preferences database, and not its registry key (HKCU\Software\Gammon Software
// Solutions\MUSHclient, which 6.00 no longer reads or writes as it runs).  The
// first time 6.00 runs -- when its preferences database does not exist yet -- it
// copies what 5.x had, translating paths that pointed into the old folder, and
// leaves the 5.x installation exactly as it was.

#pragma once

#ifdef _WIN32

#include <vector>

struct sqlite3;

// Decide portable or installed, make the data folder (seeding it if this
// version has not yet), and make it the current directory.  Before anything
// captures the current directory -- InitInstance's working_dir.
void UserData_SetUp (const CString & strProgramDirectory);

bool            UserData_IsPortable ();
const CString & UserData_Directory ();          // ends in a backslash

// The default for a global preference, made absolute: a folder of the user's
// own (".\\logs\\") in the data folder, a file that ships with the program
// ("fonts\\standard.flf") in the program's.  Anything else is returned as is.
CString UserData_DefaultPreference (const char * pName, const CString & strDefault);

// First run: copy a MUSHclient 5 installation's settings and files, if there is
// one.  `db` is the new, just-populated preferences database; `strIniFile` the
// new MUSHclient.ini.  Returns a sentence or two for the welcome dialog, or an
// empty string if there was nothing to import.
CString UserData_ImportVersion5 (sqlite3 * db, const CString & strIniFile);

// ── The parts with no side effects on the machine, for the tests ──────────
namespace userdata {

// A 5.x installation: its folder, and that folder's VirtualStore shadow, where
// a virtualised 5.x really wrote (empty if there is none).  Both end in a
// backslash.  Where both have a file, the shadow's is the one 5.x saw.
struct LegacyInstall
  {
  CString strFolder;
  CString strShadow;
  };

// A path as 5.x had it -- absolute, or relative to its folder -- as 6.00 should
// have it: under the old folder (or its shadow), the user's own files
// (worlds\, logs\, sounds\, scripts\, quow_plugins\) move to strDataDir, and
// anything else is the program's own if strProgramDir has it.  A path outside
// the old folder is left alone.
CString TranslatePath (const CString & strPath, const LegacyInstall & old,
                       const CString & strDataDir, const CString & strProgramDir);

// The same for every path in a world file or other text that starts with the
// old folder or its shadow, wherever it appears.
CString TranslateText (const CString & strText, const LegacyInstall & old,
                       const CString & strDataDir, const CString & strProgramDir);

// Whether a global preference holds a path, or a "*"-separated list of them.
bool IsPathPreference (const char * pName);
bool IsPathListPreference (const char * pName);

// Copy a folder's files into another, recursively, never replacing a file that
// is already there.  A world file (.mcl) is rewritten with TranslateText on the
// way, when `pOld` is given.  Returns how many files were copied; `piWorlds`,
// if given, is how many of them were world files.
int CopyTree (const CString & strFrom, const CString & strTo,
              const LegacyInstall * pOld = NULL,
              const CString & strDataDir = "", const CString & strProgramDir = "",
              int * piWorlds = NULL);

// Everything UserData_ImportVersion5 does for one installation, with the
// folders given rather than found.  Returns the welcome dialog's sentences.
CString ImportInstall (const LegacyInstall & old, sqlite3 * db, const CString & strIniFile,
                       const CString & strDataDir, const CString & strProgramDir);

}   // namespace userdata

#endif  // _WIN32
