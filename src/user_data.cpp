// user_data.cpp -- where MUSHclient 6 keeps the files it writes, on Windows, and
// the one-off import of a MUSHclient 5 installation.  See user_data.h.

#include "stdafx.h"
#include "MUSHclient.h"
#include "user_data.h"
#include "version.h"     // VERSION_STRING: the seeded-version marker

#ifdef _WIN32

#include <shlobj.h>
#include <set>
#include <string>

using namespace userdata;

namespace {

CString s_strProgramDir;
CString s_strDataDir;
bool    s_bPortable = true;     // until UserData_SetUp says otherwise: beside the exe

// Top-level folders of a MUSHclient folder that hold the user's own files
// rather than the program's.
const char * const kUserFolders [] =
  { "worlds", "logs", "sounds", "scripts", "quow_plugins" };

CString WithSlash (CString s)
  {
  if (!s.IsEmpty () && s.Right (1) != "\\" && s.Right (1) != "/")
    s += "\\";
  return s;
  }

bool Exists (const CString & s)
  {
  return GetFileAttributes (s) != INVALID_FILE_ATTRIBUTES;
  }

bool FileExists (const CString & s)
  {
  const DWORD a = GetFileAttributes (s);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
  }

bool DirExists (const CString & s)
  {
  const DWORD a = GetFileAttributes (s);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
  }

void MakeDirs (CString strDir)
  {
  while (strDir.GetLength () > 3 && (strDir.Right (1) == "\\" || strDir.Right (1) == "/"))
    strDir = strDir.Left (strDir.GetLength () - 1);
  if (!DirExists (strDir))
    SHCreateDirectoryEx (NULL, strDir, NULL);
  }

bool StartsWithNoCase (const CString & s, const CString & strPrefix)
  {
  return !strPrefix.IsEmpty () && s.GetLength () >= strPrefix.GetLength () &&
         s.Left (strPrefix.GetLength ()).CompareNoCase (strPrefix) == 0;
  }

CString KnownFolder (int csidl)
  {
  char path [MAX_PATH];
  if (FAILED (SHGetFolderPath (NULL, csidl | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, path)))
    return "";
  return WithSlash (path);
  }

bool ReadWholeFile (const CString & strName, std::string & contents)
  {
  FILE * f = fopen (strName, "rb");
  if (!f)
    return false;
  contents.clear ();
  char buf [8192];
  size_t n;
  while ((n = fread (buf, 1, sizeof buf, f)) > 0)
    contents.append (buf, n);
  fclose (f);
  return true;
  }

bool WriteWholeFile (const CString & strName, const std::string & contents)
  {
  FILE * f = fopen (strName, "wb");
  if (!f)
    return false;
  const bool ok = fwrite (contents.data (), 1, contents.size (), f) == contents.size ();
  return fclose (f) == 0 && ok;
  }

// A 5.x SQLite database, opened from a copy.  Opening the original could leave
// a -journal or -shm beside it -- and in Program Files could not even do that --
// and the 5.x folder is to be left exactly as it was.  The -wal goes too: a
// database 5.x had open in WAL mode keeps its newest pages there.
class CLegacyDatabase
  {
  public:
    explicit CLegacyDatabase (const CString & strName)
      {
      char dir [MAX_PATH], name [MAX_PATH];
      if (!FileExists (strName) || !GetTempPath (sizeof dir, dir) ||
          !GetTempFileName (dir, "mcl", 0, name))
        return;
      m_strCopy = name;
      if (!CopyFile (strName, m_strCopy, FALSE))
        return;
      if (FileExists (strName + "-wal"))
        CopyFile (strName + "-wal", m_strCopy + "-wal", FALSE);
      if (sqlite3_open (m_strCopy, &m_db) != SQLITE_OK)
        {
        sqlite3_close (m_db);
        m_db = NULL;
        }
      }
    ~CLegacyDatabase ()
      {
      if (m_db)
        sqlite3_close (m_db);
      if (!m_strCopy.IsEmpty ())
        {
        DeleteFile (m_strCopy);
        DeleteFile (m_strCopy + "-wal");
        DeleteFile (m_strCopy + "-shm");
        DeleteFile (m_strCopy + "-journal");
        }
      }
    sqlite3 * db () const { return m_db; }

  private:
    CString   m_strCopy;
    sqlite3 * m_db = NULL;
  };

// The two places a 5.x file can be, newest first.
std::vector<CString> Roots (const LegacyInstall & old)
  {
  std::vector<CString> v;
  if (!old.strShadow.IsEmpty ())
    v.push_back (old.strShadow);
  v.push_back (old.strFolder);
  return v;
  }

CString FirstExisting (const LegacyInstall & old, const CString & strRelative)
  {
  for (const CString & root : Roots (old))
    if (FileExists (root + strRelative))
      return root + strRelative;
  return "";
  }

CString TranslateList (const CString & strList, const LegacyInstall & old,
                       const CString & strDataDir, const CString & strProgramDir)
  {
  vector<string> v;
  StringToVector ((LPCTSTR) strList, v, "*");
  CString strOut;
  for (const string & item : v)
    {
    if (!strOut.IsEmpty ())
      strOut += "*";
    strOut += TranslatePath (item.c_str (), old, strDataDir, strProgramDir);
    }
  return strOut;
  }

void ApplyPreference (sqlite3 * db, const CString & strName, CString strValue,
                      const LegacyInstall & old,
                      const CString & strDataDir, const CString & strProgramDir)
  {
  if (IsPathListPreference (strName))
    strValue = TranslateList (strValue, old, strDataDir, strProgramDir);
  else if (IsPathPreference (strName))
    strValue = TranslatePath (strValue, old, strDataDir, strProgramDir);

  // UPDATE, not INSERT: PopulateDatabase wrote a row for every preference
  // this version has, so a name 5.x had and 6.00 does not is dropped.
  sqlite3_stmt * stmt = NULL;
  if (sqlite3_prepare_v2 (db, "UPDATE prefs SET value = ?1 WHERE name = ?2",
                          -1, &stmt, NULL) != SQLITE_OK)
    return;
  sqlite3_bind_text (stmt, 1, strValue, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 2, strName,  -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
  }

// The global preferences: 5.x's database if it has one, else the registry,
// which is where they were before 4.x moved them into the database (and what
// 5.x itself copies on its first run).  Plus the world window positions.
bool ImportPreferences (const LegacyInstall & old, sqlite3 * db,
                        const CString & strDataDir, const CString & strProgramDir)
  {
  const CString strOld = FirstExisting (old, PREFERENCES_DATABASE_FILE);
  if (!strOld.IsEmpty ())
    {
    CLegacyDatabase legacy (strOld);
    if (!legacy.db ())
      return false;

    sqlite3_stmt * stmt = NULL;
    if (sqlite3_prepare_v2 (legacy.db (), "SELECT name, value FROM prefs",
                            -1, &stmt, NULL) == SQLITE_OK)
      {
      while (sqlite3_step (stmt) == SQLITE_ROW)
        {
        const char * name  = (const char *) sqlite3_column_text (stmt, 0);
        const char * value = (const char *) sqlite3_column_text (stmt, 1);
        if (name && value)
          ApplyPreference (db, name, value, old, strDataDir, strProgramDir);
        }
      sqlite3_finalize (stmt);
      }

    sqlite3_stmt * insert = NULL;
    if (sqlite3_prepare_v2 (legacy.db (), "SELECT name, value FROM worlds",
                            -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2 (db, "INSERT OR REPLACE INTO worlds (name, value) VALUES (?1, ?2)",
                            -1, &insert, NULL) == SQLITE_OK)
      {
      while (sqlite3_step (stmt) == SQLITE_ROW)
        {
        sqlite3_bind_value (insert, 1, sqlite3_column_value (stmt, 0));
        sqlite3_bind_value (insert, 2, sqlite3_column_value (stmt, 1));
        sqlite3_step (insert);
        sqlite3_reset (insert);
        }
      }
    sqlite3_finalize (insert);
    sqlite3_finalize (stmt);
    return true;
    }

  HKEY hKey;
  if (RegOpenKeyEx (HKEY_CURRENT_USER,
                    "Software\\Gammon Software Solutions\\MUSHclient\\Global prefs",
                    0, KEY_READ, &hKey) != ERROR_SUCCESS)
    return false;
  bool bAny = false;
  for (DWORD i = 0; ; i++)
    {
    char name [256];
    BYTE data [8192];
    DWORD nameLen = sizeof name, dataLen = sizeof data - 1, type;
    const LONG rc = RegEnumValue (hKey, i, name, &nameLen, NULL, &type, data, &dataLen);
    if (rc == ERROR_NO_MORE_ITEMS)
      break;
    if (rc != ERROR_SUCCESS)
      continue;
    CString strValue;
    if (type == REG_SZ || type == REG_EXPAND_SZ)
      {
      data [dataLen] = 0;
      strValue = (const char *) data;
      }
    else if (type == REG_DWORD && dataLen == sizeof (DWORD))
      strValue.Format ("%lu", * (DWORD *) data);
    else
      continue;
    ApplyPreference (db, name, strValue, old, strDataDir, strProgramDir);
    bAny = true;
    }
  RegCloseKey (hKey);
  return bAny;
  }

// File > Recent Files, from 5.x's MUSHclient.ini.
void ImportRecentFiles (const LegacyInstall & old, const CString & strIniFile,
                        const CString & strDataDir, const CString & strProgramDir)
  {
  const CString strOld = FirstExisting (old, "MUSHclient.ini");
  if (strOld.IsEmpty ())
    return;
  for (int i = 1; i <= 16; i++)
    {
    CString strKey;
    strKey.Format ("File%i", i);
    char value [MAX_PATH * 2] = "";
    GetPrivateProfileString ("Recent File List", strKey, "", value, sizeof value, strOld);
    if (value [0])
      WritePrivateProfileString ("Recent File List", strKey,
                                 TranslatePath (value, old, strDataDir, strProgramDir),
                                 strIniFile);
    }
  }

// Words the user added to 5.x's spell checker.  They live in its spell.sqlite
// (Add inserts them there, flagged user = 1, and never writes the text file),
// which 6.00 does not copy: it builds its own from the dictionaries, reading
// userdict.txt as it does -- so that is where they go.
int ImportSpellingWords (const LegacyInstall & old, const CString & strDataDir)
  {
  const CString strUserDict = strDataDir + "userdict.txt";
  std::set<std::string> have, add;

  std::string text;
  if (ReadWholeFile (strUserDict, text))
    {
    vector<string> v;
    StringToVector (text, v, "\n");
    for (string w : v)
      {
      while (!w.empty () && (w.back () == '\r' || w.back () == ' ')) w.pop_back ();
      if (!w.empty ()) have.insert (w);
      }
    }

  const CString strOldDict = FirstExisting (old, "spell\\userdict.txt");
  if (!strOldDict.IsEmpty () && ReadWholeFile (strOldDict, text))
    {
    vector<string> v;
    StringToVector (text, v, "\n");
    for (string w : v)
      {
      while (!w.empty () && (w.back () == '\r' || w.back () == ' ')) w.pop_back ();
      if (!w.empty () && !have.count (w)) add.insert (w);
      }
    }

  const CString strOldDb = FirstExisting (old, "spell\\spell.sqlite");
  if (!strOldDb.IsEmpty ())
    {
    CLegacyDatabase legacy (strOldDb);
    sqlite3_stmt * stmt = NULL;
    if (legacy.db () &&
        sqlite3_prepare_v2 (legacy.db (), "SELECT name FROM words WHERE user = 1",
                            -1, &stmt, NULL) == SQLITE_OK)
      {
      while (sqlite3_step (stmt) == SQLITE_ROW)
        {
        const char * w = (const char *) sqlite3_column_text (stmt, 0);
        if (w && *w && !have.count (w)) add.insert (w);
        }
      }
    sqlite3_finalize (stmt);
    }

  if (add.empty ())
    return 0;
  std::string out;
  ReadWholeFile (strUserDict, out);
  if (!out.empty () && out.back () != '\n')
    out += "\r\n";
  for (const std::string & w : add)
    out += w + "\r\n";
  return WriteWholeFile (strUserDict, out) ? (int) add.size () : 0;
  }

// 5.x's VirtualStore shadow of a folder: %LOCALAPPDATA%\VirtualStore\ followed
// by the folder's path without its drive.
CString ShadowOf (const CString & strFolder)
  {
  if (strFolder.GetLength () < 3 || strFolder [1] != ':')
    return "";
  const CString strLocal = KnownFolder (CSIDL_LOCAL_APPDATA);
  if (strLocal.IsEmpty ())
    return "";
  const CString strShadow = strLocal + "VirtualStore\\" + strFolder.Mid (3);
  return DirExists (strShadow) ? strShadow : "";
  }

// The bundled plugins and worlds, and the Discworld edition's mapper and its
// maps, are the user's to change -- the mapper even writes its own database
// beside its maps -- so they go into the data folder.  Once per version, so a
// file the user deleted stays deleted until an upgrade; and never over a file
// already there, so their changes survive one.
void Seed ()
  {
  const CString strMarker = s_strDataDir + "seeded-version.txt";
  std::string seeded;
  if (ReadWholeFile (strMarker, seeded) && seeded == VERSION_STRING)
    return;
  // World files are translated on the way, as an import's are.  The bundled
  // ones hold only relative paths, which this leaves alone -- but 6.00
  // installed into a 5.x's own folder finds that 5.x's worlds here too, and
  // they hold paths into this folder that belong in the data folder now.
  LegacyInstall program;
  program.strFolder = s_strProgramDir;
  CopyTree (s_strProgramDir + "worlds",       s_strDataDir + "worlds",
            &program, s_strDataDir, s_strProgramDir);
  CopyTree (s_strProgramDir + "quow_plugins", s_strDataDir + "quow_plugins",
            &program, s_strDataDir, s_strProgramDir);
  MakeDirs (s_strDataDir + "worlds\\plugins\\state");
  MakeDirs (s_strDataDir + "logs");
  MakeDirs (s_strDataDir + "sounds");
  WriteWholeFile (strMarker, VERSION_STRING);
  }

}   // namespace

// ── userdata:: ────────────────────────────────────────────────────────────

namespace userdata {

CString TranslatePath (const CString & strPath, const LegacyInstall & old,
                       const CString & strDataDir, const CString & strProgramDir)
  {
  if (strPath.IsEmpty ())
    return strPath;

  CString strFull = strPath;
  strFull.Replace ('/', '\\');
  const bool bAbsolute = (strFull.GetLength () >= 2 && strFull [1] == ':') ||
                         strFull.Left (1) == "\\";
  if (!bAbsolute)
    {
    // relative to the folder 5.x ran in
    while (strFull.Left (2) == ".\\")
      strFull = strFull.Mid (2);
    strFull = old.strFolder + strFull;
    }

  for (const CString & root : Roots (old))
    {
    if (!StartsWithNoCase (strFull, root))
      continue;
    const CString strRest = strFull.Mid (root.GetLength ());
    if (strRest.Left (2) == "..")
      return strFull;     // out of the old folder: the absolute path is right
    CString strTop = strRest;
    if (strTop.Find ('\\') >= 0)
      strTop = strTop.Left (strTop.Find ('\\'));
    bool bUser = strRest.IsEmpty ();
    for (const char * p : kUserFolders)
      if (strTop.CompareNoCase (p) == 0)
        bUser = true;
    if (!bUser && Exists (strProgramDir + strRest))
      return strProgramDir + strRest;
    return strDataDir + strRest;
    }

  return strPath;         // somewhere of the user's choosing
  }

CString TranslateText (const CString & strText, const LegacyInstall & old,
                       const CString & strDataDir, const CString & strProgramDir)
  {
  CString strLower = strText;
  strLower.MakeLower ();
  std::vector<CString> roots = Roots (old);
  for (CString & r : roots)
    r.MakeLower ();

  CString strOut;
  int i = 0;
  const int len = strText.GetLength ();
  while (i < len)
    {
    int best = -1;
    for (const CString & r : roots)
      {
      const int k = strLower.Find (r, i);
      if (k >= 0 && (best < 0 || k < best))
        best = k;
      }
    if (best < 0)
      break;
    // A path runs to whatever ends a quoted string, an XML value, a line or
    // one of MUSHclient's "*"-separated lists.  Not to a space: "Program
    // Files" has one.
    int end = best;
    while (end < len && !strchr ("\"'<>\r\n\t*|", strText [end]))
      end++;
    strOut += strText.Mid (i, best - i);
    strOut += TranslatePath (strText.Mid (best, end - best), old, strDataDir, strProgramDir);
    i = end;
    }
  strOut += strText.Mid (i);
  return strOut;
  }

bool IsPathPreference (const char * pName)
  {
  static const char * const names [] = {
    "AsciiArtFont", "DefaultAliasesFile", "DefaultColoursFile", "DefaultLogFileDirectory",
    "DefaultMacrosFile", "DefaultNameGenerationFile", "DefaultTimersFile",
    "DefaultTriggersFile", "DefaultWorldFileDirectory", "PluginsDirectory",
    "StateFilesDirectory", "TrayIconFileName",
    };
  for (const char * p : names)
    if (_stricmp (p, pName) == 0)
      return true;
  return false;
  }

bool IsPathListPreference (const char * pName)
  {
  return _stricmp (pName, "WorldList") == 0 || _stricmp (pName, "PluginList") == 0;
  }

int CopyTree (const CString & strFrom, const CString & strTo,
              const LegacyInstall * pOld,
              const CString & strDataDir, const CString & strProgramDir,
              int * piWorlds)
  {
  const CString strSrc = WithSlash (strFrom), strDst = WithSlash (strTo);
  if (!DirExists (strSrc))
    return 0;
  MakeDirs (strDst);

  WIN32_FIND_DATA fd;
  HANDLE h = FindFirstFile (strSrc + "*", &fd);
  if (h == INVALID_HANDLE_VALUE)
    return 0;
  int n = 0;
  do
    {
    const CString strName = fd.cFileName;
    if (strName == "." || strName == "..")
      continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      {
      // not into a junction: it could lead anywhere, including round again
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        n += CopyTree (strSrc + strName, strDst + strName, pOld,
                       strDataDir, strProgramDir, piWorlds);
      continue;
      }
    if (Exists (strDst + strName))
      continue;           // never over a file already there
    const bool bWorld = strName.GetLength () > 4 &&
                        strName.Right (4).CompareNoCase (".mcl") == 0;
    bool ok;
    std::string contents;
    if (bWorld && pOld && ReadWholeFile (strSrc + strName, contents))
      {
      const CString strText (contents.data (), (int) contents.size ());
      const CString strNew = TranslateText (strText, *pOld, strDataDir, strProgramDir);
      ok = WriteWholeFile (strDst + strName, std::string ((LPCTSTR) strNew, strNew.GetLength ()));
      }
    else
      ok = CopyFile (strSrc + strName, strDst + strName, TRUE) != 0;
    if (ok)
      {
      n++;
      if (bWorld && piWorlds)
        ++*piWorlds;
      }
    } while (FindNextFile (h, &fd));
  FindClose (h);
  return n;
  }

CString ImportInstall (const LegacyInstall & old, sqlite3 * db, const CString & strIniFile,
                       const CString & strDataDir, const CString & strProgramDir)
  {
  // Files first: a world 5.x listed to open at startup is then already where
  // the translated WorldList says it is.  The shadow first: where both have a
  // file, it is the one 5.x saw.
  int nFiles = 0, nWorlds = 0;
  for (const char * folder : kUserFolders)
    {
    if (_stricmp (folder, "logs") == 0)
      continue;           // can be huge, and are only read: left where they are
    for (const CString & root : Roots (old))
      nFiles += CopyTree (root + folder, strDataDir + folder, &old,
                          strDataDir, strProgramDir, &nWorlds);
    }

  const bool bPrefs = ImportPreferences (old, db, strDataDir, strProgramDir);
  ImportRecentFiles (old, strIniFile, strDataDir, strProgramDir);
  const int nWords = ImportSpellingWords (old, strDataDir);

  if (!bPrefs && nFiles == 0 && nWords == 0)
    return "";

  sqlite3_stmt * stmt = NULL;
  if (sqlite3_prepare_v2 (db, "INSERT OR REPLACE INTO control (name, value) VALUES ('Imported from', ?1)",
                          -1, &stmt, NULL) == SQLITE_OK)
    {
    sqlite3_bind_text (stmt, 1, old.strFolder, -1, SQLITE_TRANSIENT);
    sqlite3_step (stmt);
    }
  sqlite3_finalize (stmt);

  CString strFolder = old.strFolder;
  strFolder.TrimRight ('\\');
  CString strWhat = "Your settings have";
  if (nWorlds)
    strWhat.Format ("Your settings and %i world file%s have", nWorlds, nWorlds == 1 ? "" : "s");
  CString strSummary;
  strSummary.Format (
    "%s been copied from MUSHclient 5, in %s, which has been left as it was.",
    (LPCTSTR) strWhat, (LPCTSTR) strFolder);
  for (const CString & root : Roots (old))
    if (DirExists (root + "logs"))
      {
      CString strLogs = root + "logs";
      strSummary += " Its logs are still in " + strLogs + ".";
      break;
      }
  return strSummary;
  }

}   // namespace userdata

// ── The app's side ────────────────────────────────────────────────────────

void UserData_SetUp (const CString & strProgramDirectory)
  {
  s_strProgramDir = WithSlash (strProgramDirectory);
  s_bPortable = FileExists (s_strProgramDir + "portable.txt");
  if (!s_bPortable)
    {
    const CString strAppData = KnownFolder (CSIDL_APPDATA);
    if (strAppData.IsEmpty ())
      s_bPortable = true;   // nowhere else to go
    else
      s_strDataDir = strAppData + "MUSHclient\\";
    }
  if (s_bPortable)
    s_strDataDir = s_strProgramDir;
  else
    {
    MakeDirs (s_strDataDir);
    Seed ();
    }
  SetCurrentDirectory (s_strDataDir);
  }

bool UserData_IsPortable ()
  {
  return s_bPortable;
  }

const CString & UserData_Directory ()
  {
  return s_strDataDir;
  }

CString UserData_DefaultPreference (const char * pName, const CString & strDefault)
  {
  if (!IsPathPreference (pName) || s_strProgramDir.IsEmpty ())
    return strDefault;
  // The defaults are written as 5.x's were, relative to the MUSHclient folder
  // -- so the program's folder plays the old one, and the same rule sorts them.
  LegacyInstall program;
  program.strFolder = s_strProgramDir;
  return TranslatePath (strDefault, program, s_strDataDir, s_strProgramDir);
  }

CString UserData_ImportVersion5 (sqlite3 * db, const CString & strIniFile)
  {
  std::vector<CString> candidates;

  // where 5.x's installer said it went (6.00's installer keeps a key of its own)
  HKEY hKey;
  if (RegOpenKeyEx (HKEY_CURRENT_USER,
                    "Software\\Gammon Software Solutions\\MUSHclient\\General Options",
                    0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
    char value [MAX_PATH];
    DWORD len = sizeof value - 1, type;
    if (RegQueryValueEx (hKey, "InstallDir", NULL, &type, (BYTE *) value, &len) == ERROR_SUCCESS &&
        type == REG_SZ)
      {
      value [len] = 0;
      candidates.push_back (WithSlash (value));
      }
    RegCloseKey (hKey);
    }
  // and where it goes by default
  for (const char * var : { "ProgramFiles(x86)", "ProgramW6432", "ProgramFiles" })
    {
    char value [MAX_PATH];
    if (GetEnvironmentVariable (var, value, sizeof value))
      candidates.push_back (WithSlash (value) + "MUSHclient\\");
    }

  std::set<CString> tried;
  for (const CString & strFolder : candidates)
    {
    CString strKey = strFolder;
    strKey.MakeLower ();
    if (!tried.insert (strKey).second || strFolder.CompareNoCase (s_strDataDir) == 0)
      continue;
    LegacyInstall old;
    old.strFolder = strFolder;
    old.strShadow = ShadowOf (strFolder);
    // 5.x makes its preferences database the first time it runs, so a folder
    // with one is a 5.x that ran -- even when it is 6.00's own folder, 6.00
    // having been installed where 5.x was: the database is 5.x's, left behind.
    // Without one, a MUSHclient.exe anywhere but here is a 5.x too old to have
    // used a database, whose preferences are in the registry.
    if (FirstExisting (old, PREFERENCES_DATABASE_FILE).IsEmpty () &&
        (strFolder.CompareNoCase (s_strProgramDir) == 0 ||
         !FileExists (strFolder + "MUSHclient.exe")))
      continue;
    return ImportInstall (old, db, strIniFile, s_strDataDir, s_strProgramDir);
    }
  return "";
  }

#endif  // _WIN32
