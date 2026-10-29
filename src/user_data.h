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
// Solutions\MUSHclient, which 6.00 no longer reads or writes as it runs).  It
// offers to import them instead (legacy_import.h).

#pragma once

#ifdef _WIN32

// Decide portable or installed, make the data folder (seeding it if this
// version has not yet), and make it the current directory.  Before anything
// captures the current directory -- InitInstance's working_dir.
void UserData_SetUp (const CString & strProgramDirectory);

bool            UserData_IsPortable ();
const CString & UserData_Directory ();          // ends in a backslash
const CString & UserData_ProgramDirectory ();   // likewise

// The default for a global preference, made absolute: a folder of the user's
// own (".\logs\\") in the data folder, a file that ships with the program
// ("fonts\standard.flf") in the program's.  Anything else is returned as is.
CString UserData_DefaultPreference (const char * pName, const CString & strDefault);

#endif  // _WIN32
