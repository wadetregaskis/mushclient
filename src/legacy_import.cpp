// legacy_import.cpp -- copying a MUSHclient 5 installation into MUSHclient 6.
// See legacy_import.h.

#include "stdafx.h"
#include "legacy_import.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace legacy {

namespace {

#ifdef _WIN32
const char kSep = '\\';
#else
const char kSep = '/';
#endif

// Top-level folders of a MUSHclient folder that hold the user's own files
// rather than the program's.
const char * const kUserFolders [] =
  { "worlds", "logs", "sounds", "scripts", "quow_plugins" };

const char * const kPrefsDb = "mushclient_prefs.sqlite";

std::string Lower (std::string s)
  {
  for (char & c : s)
    c = (char) tolower ((unsigned char) c);
  return s;
  }

bool EqualNoCase (const std::string & a, const std::string & b)
  {
  return Lower (a) == Lower (b);
  }

bool StartsNoCase (const std::string & s, const std::string & prefix)
  {
  return !prefix.empty () && s.size () >= prefix.size () &&
         EqualNoCase (s.substr (0, prefix.size ()), prefix);
  }

std::string Slash (std::string s)
  {
  if (!s.empty () && s.back () != '\\' && s.back () != '/')
    s += kSep;
  return s;
  }

// A Windows path's tail, in this platform's form.
std::string Native (std::string s)
  {
  std::replace (s.begin (), s.end (), '\\', kSep);
  return s;
  }

fs::path P (const std::string & s)
  {
  return fs::path (s);
  }

bool Exists (const std::string & s)
  {
  std::error_code ec;
  return fs::exists (P (s), ec);
  }

bool IsDir (const std::string & s)
  {
  std::error_code ec;
  return fs::is_directory (P (s), ec);
  }

bool IsUserFolder (const std::string & name)
  {
  for (const char * p : kUserFolders)
    if (EqualNoCase (name, p))
      return true;
  return false;
  }

// Whether a folder holds a file of this name, in any case: 5.x's installer
// wrote "mushclient.exe", and a case-sensitive file system keeps what it wrote.
bool HasFileNoCase (const std::string & dir, const std::string & name)
  {
  std::error_code ec;
  for (fs::directory_iterator it (P (dir), ec), end; !ec && it != end; it.increment (ec))
    if (EqualNoCase (it->path ().filename ().string (), name))
      return true;
  return false;
  }

bool ReadWholeFile (const fs::path & name, std::string & contents)
  {
  std::ifstream f (name, std::ios::binary);
  if (!f)
    return false;
  std::ostringstream ss;
  ss << f.rdbuf ();
  contents = ss.str ();
  return true;
  }

bool WriteWholeFile (const fs::path & name, const std::string & contents)
  {
  std::ofstream f (name, std::ios::binary | std::ios::trunc);
  if (!f)
    return false;
  f.write (contents.data (), (std::streamsize) contents.size ());
  return (bool) f;
  }

bool IsWorldFile (const std::string & name)
  {
  return name.size () > 4 && EqualNoCase (name.substr (name.size () - 4), ".mcl");
  }

// The two places a 5.x file can be, newest first.
std::vector<std::string> Roots (const Install & old)
  {
  std::vector<std::string> v;
  if (!old.shadow.empty ())
    v.push_back (old.shadow);
  v.push_back (old.folder);
  return v;
  }

std::string FirstExisting (const Install & old, const std::string & relative)
  {
  for (const std::string & root : Roots (old))
    if (Exists (root + relative))
      return root + relative;
  return "";
  }

// A Windows path 5.x wrote, as a path on this machine.  Under Wine a drive is
// whatever the prefix's dosdevices link says -- c: is ../drive_c and z: is /
// unless someone changed them -- and on Windows it already is one.
std::string MapWindowsPath (const std::string & path, const Install & old)
  {
  if (old.prefix.empty () || path.size () < 3 || path [1] != ':' || path [2] != '\\')
    return path;
  const char drive = (char) tolower ((unsigned char) path [0]);
  const fs::path devices = P (old.prefix) / "dosdevices";
  std::error_code ec;
  fs::path target = fs::read_symlink (devices / (std::string (1, drive) + ":"), ec);
  if (ec)
    {
    if (drive == 'c')
      target = P (old.prefix) / "drive_c";
    else if (drive == 'z')
      target = fs::path ("/");
    else
      return path;
    }
  else if (target.is_relative ())
    target = (devices / target).lexically_normal ();
  return Slash (target.string ()) + Native (path.substr (3));
  }

// 5.x's database, opened from a copy.  Opening the original could leave a
// -journal or -shm beside it -- in Program Files it could not even do that --
// and the 5.x folder is to be left exactly as it was.  The -wal goes too: a
// database 5.x had open in WAL mode keeps its newest pages there.
class CLegacyDatabase
  {
  public:
    explicit CLegacyDatabase (const std::string & name)
      {
      static std::atomic<unsigned> serial (0);
      std::error_code ec;
      if (name.empty () || !fs::exists (P (name), ec))
        return;
      const unsigned long long now = (unsigned long long)
        std::chrono::steady_clock::now ().time_since_epoch ().count ();
      m_copy = fs::temp_directory_path (ec) /
               ("mclegacy_" + std::to_string (now) + "_" + std::to_string (serial++) + ".sqlite");
      if (ec || !fs::copy_file (P (name), m_copy, fs::copy_options::overwrite_existing, ec) || ec)
        {
        m_copy.clear ();
        return;
        }
      fs::copy_file (P (name + "-wal"), WithSuffix ("-wal"), fs::copy_options::overwrite_existing, ec);
      // sqlite3_open takes UTF-8, whatever the platform's own encoding
      if (sqlite3_open (m_copy.u8string ().c_str (), &m_db) != SQLITE_OK)
        {
        sqlite3_close (m_db);
        m_db = nullptr;
        }
      }
    ~CLegacyDatabase ()
      {
      if (m_db)
        sqlite3_close (m_db);
      if (!m_copy.empty ())
        {
        std::error_code ec;
        fs::remove (m_copy, ec);
        for (const char * suffix : { "-wal", "-shm", "-journal" })
          fs::remove (WithSuffix (suffix), ec);
        }
      }
    sqlite3 * db () const { return m_db; }

  private:
    fs::path WithSuffix (const char * suffix) const
      {
      fs::path p = m_copy;
      p += suffix;
      return p;
      }
    fs::path  m_copy;
    sqlite3 * m_db = nullptr;
  };

std::string TranslateList (const std::string & list, const Install & old,
                           const std::string & dataDir, const std::string & programDir)
  {
  std::string out, item;
  std::istringstream ss (list);
  while (std::getline (ss, item, '*'))
    {
    if (item.empty ())
      continue;
    if (!out.empty ())
      out += "*";
    out += TranslatePath (item, old, dataDir, programDir);
    }
  return out;
  }

void ApplyPreference (sqlite3 * db, const std::string & name, std::string value,
                      const Install & old,
                      const std::string & dataDir, const std::string & programDir)
  {
  if (IsPathListPreference (name.c_str ()))
    value = TranslateList (value, old, dataDir, programDir);
  else if (IsPathPreference (name.c_str ()))
    {
    bool resolved;
    value = TranslatePath (value, old, dataDir, programDir, &resolved);
    // One of 5.x's own files this version does not have where it looked --
    // 6.00's default (made for this platform) is better than a dead path.
    if (!resolved)
      return;
    }

  // UPDATE, not INSERT: PopulateDatabase wrote a row for every preference
  // this version has, so a name 5.x had and 6.00 does not is dropped.
  sqlite3_stmt * stmt = nullptr;
  if (sqlite3_prepare_v2 (db, "UPDATE prefs SET value = ?1 WHERE name = ?2",
                          -1, &stmt, nullptr) != SQLITE_OK)
    return;
  sqlite3_bind_text (stmt, 1, value.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 2, name.c_str (),  -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
  }

// The global preferences: 5.x's database if it has one, else the registry,
// which is where they were before 4.x moved them into the database (and what
// 5.x itself copied on its first run).  Plus the world window positions.
bool ImportPreferences (const Install & old, sqlite3 * db,
                        const std::string & dataDir, const std::string & programDir)
  {
  const std::string oldDb = FirstExisting (old, kPrefsDb);
  if (!oldDb.empty ())
    {
    CLegacyDatabase legacy (oldDb);
    if (!legacy.db ())
      return false;

    sqlite3_stmt * stmt = nullptr;
    if (sqlite3_prepare_v2 (legacy.db (), "SELECT name, value FROM prefs",
                            -1, &stmt, nullptr) == SQLITE_OK)
      {
      while (sqlite3_step (stmt) == SQLITE_ROW)
        {
        const char * name  = (const char *) sqlite3_column_text (stmt, 0);
        const char * value = (const char *) sqlite3_column_text (stmt, 1);
        if (name && value)
          ApplyPreference (db, name, value, old, dataDir, programDir);
        }
      }
    sqlite3_finalize (stmt);

    sqlite3_stmt * insert = nullptr;
    stmt = nullptr;
    if (sqlite3_prepare_v2 (legacy.db (), "SELECT name, value FROM worlds",
                            -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_prepare_v2 (db, "INSERT OR REPLACE INTO worlds (name, value) VALUES (?1, ?2)",
                            -1, &insert, nullptr) == SQLITE_OK)
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

  const RegistryValues values =
    ReadRegistry (old, "Software\\Gammon Software Solutions\\MUSHclient\\Global prefs");
  for (const auto & v : values)
    ApplyPreference (db, v.first, v.second, old, dataDir, programDir);
  return !values.empty ();
  }

// File > Recent Files, from 5.x's MUSHclient.ini: [Recent File List] File1..
// with File1 the newest.
std::vector<std::string> ReadRecentFiles (const Install & old,
                                          const std::string & dataDir,
                                          const std::string & programDir)
  {
  std::vector<std::string> files;
  std::string text;
  const std::string ini = FirstExisting (old, "MUSHclient.ini");
  if (ini.empty () || !ReadWholeFile (P (ini), text))
    return files;
  std::istringstream ss (text);
  std::string line;
  bool in = false;
  std::vector<std::pair<int, std::string>> numbered;
  while (std::getline (ss, line))
    {
    if (!line.empty () && line.back () == '\r')
      line.pop_back ();
    if (!line.empty () && line [0] == '[')
      {
      in = EqualNoCase (line, "[Recent File List]");
      continue;
      }
    const size_t eq = line.find ('=');
    if (!in || eq == std::string::npos || !StartsNoCase (line, "File"))
      continue;
    const int n = atoi (line.substr (4, eq - 4).c_str ());
    const std::string value = line.substr (eq + 1);
    if (n > 0 && !value.empty ())
      numbered.push_back (std::make_pair (n, value));
    }
  std::sort (numbered.begin (), numbered.end ());
  for (auto it = numbered.rbegin (); it != numbered.rend (); ++it)
    files.push_back (TranslatePath (it->second, old, dataDir, programDir));
  return files;
  }

std::set<std::string> Lines (const std::string & text)
  {
  std::set<std::string> lines;
  std::istringstream ss (text);
  std::string w;
  while (std::getline (ss, w))
    {
    while (!w.empty () && (w.back () == '\r' || w.back () == ' '))
      w.pop_back ();
    if (!w.empty ())
      lines.insert (w);
    }
  return lines;
  }

// Words the user added to 5.x's spell checker.  They live in its spell.sqlite
// (Add inserts them there, flagged user = 1, and never writes the text file),
// which 6.00 does not copy: it builds its own from the dictionaries, reading
// userdict.txt as it does -- so that is where they go.
int ImportSpellingWords (const Install & old, const std::string & dataDir)
  {
  const fs::path userDict = P (dataDir + "userdict.txt");
  std::string text;
  ReadWholeFile (userDict, text);
  const std::set<std::string> have = Lines (text);
  std::set<std::string> add;

  std::string oldText;
  const std::string oldDict = FirstExisting (old, Native ("spell\\userdict.txt"));
  if (!oldDict.empty () && ReadWholeFile (P (oldDict), oldText))
    for (const std::string & w : Lines (oldText))
      if (!have.count (w))
        add.insert (w);

  CLegacyDatabase legacy (FirstExisting (old, Native ("spell\\spell.sqlite")));
  sqlite3_stmt * stmt = nullptr;
  if (legacy.db () &&
      sqlite3_prepare_v2 (legacy.db (), "SELECT name FROM words WHERE user = 1",
                          -1, &stmt, nullptr) == SQLITE_OK)
    while (sqlite3_step (stmt) == SQLITE_ROW)
      {
      const char * w = (const char *) sqlite3_column_text (stmt, 0);
      if (w && *w && !have.count (w))
        add.insert (w);
      }
  sqlite3_finalize (stmt);

  if (add.empty ())
    return 0;
  if (!text.empty () && text.back () != '\n')
    text += "\r\n";
  for (const std::string & w : add)
    text += w + "\r\n";
  return WriteWholeFile (userDict, text) ? (int) add.size () : 0;
  }

// Wine writes a string with C-like escapes: \\ \" \n \t, and \x followed by
// up to four hex digits for anything else.
std::string ReadQuoted (const std::string & s, size_t & i)
  {
  std::string out;
  while (i < s.size ())
    {
    const char c = s [i];
    if (c == '"')
      {
      ++i;
      break;
      }
    if (c != '\\' || i + 1 >= s.size ())
      {
      out += c;
      ++i;
      continue;
      }
    const char e = s [i + 1];
    i += 2;
    if (e == 'n')
      out += '\n';
    else if (e == 't')
      out += '\t';
    else if (e == 'r')
      out += '\r';
    else if (e == '0')
      out += '\0';
    else if (e == 'x')
      {
      unsigned v = 0;
      int digits = 0;
      while (digits < 4 && i < s.size () && isxdigit ((unsigned char) s [i]))
        {
        v = v * 16 + (unsigned) (isdigit ((unsigned char) s [i]) ? s [i] - '0'
                                 : tolower ((unsigned char) s [i]) - 'a' + 10);
        ++i;
        ++digits;
        }
      out += v < 256 ? (char) v : '?';
      }
    else
      out += e;
    }
  return out;
  }

#ifdef _WIN32
// 5.x's VirtualStore shadow of a folder: %LOCALAPPDATA%\VirtualStore\ followed
// by the folder's path without its drive.
std::string ShadowOf (const std::string & folder)
  {
  char local [MAX_PATH];
  if (folder.size () < 3 || folder [1] != ':' ||
      FAILED (SHGetFolderPathA (NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, local)))
    return "";
  const std::string shadow = Slash (local) + "VirtualStore\\" + folder.substr (3);
  return IsDir (shadow) ? shadow : "";
  }
#endif

// 5.x makes its preferences database the first time it runs, so a folder with
// one is a 5.x that ran -- even 6.00's own folder, 6.00 installed where 5.x
// was: the database is 5.x's, left behind.  Without one, a MUSHclient.exe
// anywhere but 6.00's own folder is a 5.x too old to have used a database,
// whose preferences are in the registry.
bool Qualifies (const Install & old, const std::string & programDir)
  {
  if (!FirstExisting (old, kPrefsDb).empty ())
    return true;
  return !EqualNoCase (old.folder, programDir) && HasFileNoCase (old.folder, "mushclient.exe");
  }

void AddUnique (std::vector<Install> & found, const Install & old)
  {
  for (const Install & i : found)
    if (EqualNoCase (i.folder, old.folder))
      return;
  found.push_back (old);
  }

}   // namespace

// ── Finding one ───────────────────────────────────────────────────────────

std::vector<Install> InPrefix (const std::string & prefixIn, const std::string & programDir)
  {
  std::vector<Install> found;
  const std::string prefix = Slash (prefixIn);
  if (!IsDir (prefix + "drive_c"))
    return found;

  std::vector<std::string> windowsFolders;
  // where 5.x's installer said it went...
  Install probe;
  probe.prefix = prefix;
  for (const auto & v : ReadRegistry (probe, "Software\\Gammon Software Solutions\\MUSHclient\\General Options"))
    if (EqualNoCase (v.first, "InstallDir") && !v.second.empty ())
      windowsFolders.push_back (Slash (v.second));
  // ...and where it goes by default, in a 64-bit prefix and a 32-bit one
  windowsFolders.push_back ("C:\\Program Files (x86)\\MUSHclient\\");
  windowsFolders.push_back ("C:\\Program Files\\MUSHclient\\");

  for (std::string windowsFolder : windowsFolders)
    {
    std::replace (windowsFolder.begin (), windowsFolder.end (), '/', '\\');
    if (windowsFolder.back () != '\\')
      windowsFolder += '\\';
    Install old;
    old.prefix        = prefix;
    old.windowsFolder = windowsFolder;
    old.folder        = Slash (MapWindowsPath (windowsFolder, old));
    if (IsDir (old.folder) && Qualifies (old, programDir))
      AddUnique (found, old);
    }
  return found;
  }

std::vector<Install> Find (const std::string & dataDir, const std::string & programDir)
  {
  std::vector<Install> found;

#ifdef _WIN32
  std::vector<std::string> folders;
  // where 5.x's installer said it went (6.00's installer keeps a key of its own)
  Install native;
  for (const auto & v : ReadRegistry (native, "Software\\Gammon Software Solutions\\MUSHclient\\General Options"))
    if (EqualNoCase (v.first, "InstallDir") && !v.second.empty ())
      folders.push_back (Slash (v.second));
  // and where it goes by default
  for (const char * var : { "ProgramFiles(x86)", "ProgramW6432", "ProgramFiles" })
    if (const char * value = getenv (var))
      folders.push_back (Slash (value) + "MUSHclient\\");

  for (const std::string & folder : folders)
    {
    if (EqualNoCase (folder, dataDir) || !IsDir (folder))
      continue;
    Install old;
    old.folder = old.windowsFolder = folder;
    old.shadow = ShadowOf (folder);
    if (Qualifies (old, programDir))
      AddUnique (found, old);
    }
#else
  // Wine prefixes, in the usual places.  Each is a folder with a drive_c.
  std::vector<std::string> prefixes;
  auto add = [&prefixes] (const std::string & p) {
    if (!p.empty () && IsDir (Slash (p) + "drive_c"))
      prefixes.push_back (Slash (p));
  };
  auto addEach = [&add] (const std::string & dir) {
    std::error_code ec;
    for (fs::directory_iterator it (P (dir), ec), end; !ec && it != end; it.increment (ec))
      add (it->path ().string ());
  };
  const char * home = getenv ("HOME");
  const std::string h = home ? home : "";
  if (const char * wine = getenv ("WINEPREFIX"))
    add (wine);
  if (!h.empty ())
    {
    add (h + "/.wine");
#if defined (__APPLE__)
    // CrossOver's bottles; Wineskin and Porting Kit wrappers.  Not Whisky's,
    // under ~/Library/Containers: reading another app's container makes macOS
    // ask the user's permission, and this search runs unasked.  A bottle there
    // can still be chosen through File > Import from MUSHclient 5.
    addEach (h + "/Library/Application Support/CrossOver/Bottles");
    for (const std::string apps : { h + "/Applications/Wineskin", h + "/Applications" })
      {
      std::error_code ec;
      for (fs::directory_iterator it (P (apps), ec), end; !ec && it != end; it.increment (ec))
        if (it->path ().extension () == ".app")
          add ((it->path () / "Contents" / "SharedSupport" / "prefix").string ());
      }
#else
    addEach (h + "/.local/share/wineprefixes");       // winetricks
    addEach (h + "/.PlayOnLinux/wineprefix");         // PlayOnLinux
    addEach (h + "/.local/share/bottles/bottles");    // Bottles
    addEach (h + "/.var/app/com.usebottles.bottles/data/bottles/bottles");  // Bottles, as a Flatpak
    addEach (h + "/Games");                           // Lutris
#endif
    }
  for (const std::string & prefix : prefixes)
    for (const Install & old : InPrefix (prefix, programDir))
      if (!EqualNoCase (old.folder, dataDir))
        AddUnique (found, old);
#endif

  return found;
  }

bool FromFolder (const std::string & folderIn, Install & old)
  {
  const std::string folder = Slash (folderIn);

  // a Wine prefix or bottle: the 5.x in it
  const std::vector<Install> inPrefix = InPrefix (folder, "");
  if (!inPrefix.empty ())
    {
    old = inPrefix [0];
    return true;
    }

  // 5.x's own folder
  if (!Exists (folder + kPrefsDb) && !HasFileNoCase (folder, "mushclient.exe"))
    return false;
  old = Install ();
  old.folder = folder;
#ifdef _WIN32
  old.windowsFolder = folder;
  old.shadow        = ShadowOf (folder);
#else
  // Inside a prefix (…/drive_c/Program Files (x86)/MUSHclient/), it knew
  // itself by the path under drive_c.  Copied out of one, it can only be
  // guessed: where 5.x installs by default.  Only absolute paths in its
  // settings depend on it.
  old.windowsFolder = "C:\\Program Files (x86)\\MUSHclient\\";
  // Absolute, not canonical: the prefix is reported as the user would know it
  // (macOS's /var is a link to /private/var).
  std::error_code ec;
  const fs::path here = fs::absolute (P (folder), ec).lexically_normal ();
  for (fs::path p = here; !p.empty () && p != p.root_path (); p = p.parent_path ())
    if (p.filename () == "drive_c")
      {
      old.prefix = Slash (p.parent_path ().string ());
      std::string rel = here.lexically_relative (p).string ();
      std::replace (rel.begin (), rel.end (), '/', '\\');
      old.windowsFolder = "C:\\" + rel;
      if (old.windowsFolder.back () != '\\')
        old.windowsFolder += '\\';
      break;
      }
#endif
  return true;
  }

// ── Translating ───────────────────────────────────────────────────────────

std::string TranslatePath (const std::string & path, const Install & old,
                           const std::string & dataDir, const std::string & programDir,
                           bool * pResolved)
  {
  if (pResolved)
    *pResolved = true;
  if (path.empty ())
    return path;

  std::string full = path;
  std::replace (full.begin (), full.end (), '/', '\\');
  const bool absolute = (full.size () >= 2 && full [1] == ':') || full [0] == '\\';
  if (!absolute)
    {
    // relative to the folder 5.x ran in
    while (full.compare (0, 2, ".\\") == 0)
      full.erase (0, 2);
    full = old.windowsFolder + full;
    }

  // On Windows the shadow is a Windows path too; under Wine there is none.
  std::vector<std::string> roots;
  if (!old.shadow.empty ())
    roots.push_back (old.shadow);
  roots.push_back (old.windowsFolder);
  for (const std::string & root : roots)
    {
    if (!StartsNoCase (full, root))
      continue;
    const std::string rest = full.substr (root.size ());
    if (rest.compare (0, 2, "..") == 0)
      return MapWindowsPath (full, old);   // out of the old folder: kept
    const bool user = rest.empty () || IsUserFolder (rest.substr (0, rest.find ('\\')));
    const std::string native = Native (rest);
    if (!user && Exists (programDir + native))
      return programDir + native;
    if (!user && pResolved)
      *pResolved = false;
    return dataDir + native;
    }

  return MapWindowsPath (path, old);       // somewhere of the user's choosing
  }

std::string TranslateText (const std::string & text, const Install & old,
                           const std::string & dataDir, const std::string & programDir)
  {
  const std::string lower = Lower (text);
  std::vector<std::string> roots;
  if (!old.shadow.empty ())
    roots.push_back (Lower (old.shadow));
  roots.push_back (Lower (old.windowsFolder));

  std::string out;
  size_t i = 0;
  while (i < text.size ())
    {
    size_t best = std::string::npos;
    for (const std::string & r : roots)
      {
      const size_t k = r.empty () ? std::string::npos : lower.find (r, i);
      if (k < best)
        best = k;
      }
    if (best == std::string::npos)
      break;
    // A path runs to whatever ends a quoted string, an XML value, a line or
    // one of MUSHclient's "*"-separated lists.  Not to a space: "Program
    // Files" has one.
    size_t end = best;
    while (end < text.size () && !strchr ("\"'<>\r\n\t*|", text [end]))
      end++;
    out += text.substr (i, best - i);
    out += TranslatePath (text.substr (best, end - best), old, dataDir, programDir);
    i = end;
    }
  out += text.substr (i);
  return out;
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
    if (EqualNoCase (p, pName))
      return true;
  return false;
  }

bool IsPathListPreference (const char * pName)
  {
  return EqualNoCase (pName, "WorldList") || EqualNoCase (pName, "PluginList");
  }

// ── Copying ───────────────────────────────────────────────────────────────

int CopyTree (const std::string & from, const std::string & to,
              const Install * pOld,
              const std::string & dataDir, const std::string & programDir,
              int * pWorlds)
  {
  std::error_code ec;
  const fs::path src = P (from), dst = P (to);
  if (!fs::is_directory (src, ec))
    return 0;
  fs::create_directories (dst, ec);

  int n = 0;
  for (fs::directory_iterator it (src, ec), end; !ec && it != end; it.increment (ec))
    {
    std::error_code fec;
    const fs::path & path = it->path ();
    const std::string name = path.filename ().string ();
    // not through a link: it could lead anywhere, including round again
    if (it->is_symlink (fec))
      continue;
    if (it->is_directory (fec))
      {
      n += CopyTree (path.string (), (dst / name).string (), pOld, dataDir, programDir, pWorlds);
      continue;
      }
    if (!it->is_regular_file (fec))
      continue;
    const fs::path target = dst / name;
    if (fs::exists (target, fec))
      continue;           // never over a file already there
    const bool world = IsWorldFile (name);
    bool ok;
    std::string text;
    if (world && pOld && ReadWholeFile (path, text))
      ok = WriteWholeFile (target, TranslateText (text, *pOld, dataDir, programDir));
    else
      ok = fs::copy_file (path, target, fs::copy_options::none, fec) && !fec;
    if (ok)
      {
      n++;
      if (world && pWorlds)
        ++*pWorlds;
      }
    }
  return n;
  }

// ── The registry ──────────────────────────────────────────────────────────

RegistryValues ParseWineRegistry (const std::string & text, const std::string & key)
  {
  RegistryValues values;
  std::istringstream ss (text);
  std::string line;
  bool in = false;
  while (std::getline (ss, line))
    {
    if (!line.empty () && line.back () == '\r')
      line.pop_back ();
    if (!line.empty () && line [0] == '[')
      {
      // [Software\\Gammon Software Solutions\\MUSHclient\\Global prefs] 1698000000
      const size_t close = line.find (']');
      std::string name;
      for (size_t i = 1; i < close && close != std::string::npos; i++)
        {
        if (line [i] == '\\' && i + 1 < close && line [i + 1] == '\\')
          i++;
        name += line [i];
        }
      in = EqualNoCase (name, key);
      continue;
      }
    if (!in || line.empty () || line [0] != '"')
      continue;
    size_t i = 1;
    const std::string name = ReadQuoted (line, i);
    if (i >= line.size () || line [i] != '=')
      continue;
    const std::string v = line.substr (i + 1);
    if (!v.empty () && v [0] == '"')
      {
      size_t j = 1;
      values.push_back (std::make_pair (name, ReadQuoted (v, j)));
      }
    else if (v.compare (0, 6, "dword:") == 0)
      values.push_back (std::make_pair (name,
        std::to_string (strtoul (v.c_str () + 6, nullptr, 16))));
    }
  return values;
  }

RegistryValues ReadRegistry (const Install & old, const std::string & key)
  {
  if (!old.prefix.empty ())
    {
    std::string text;
    if (!ReadWholeFile (P (old.prefix + "user.reg"), text))
      return RegistryValues ();
    return ParseWineRegistry (text, key);
    }

  RegistryValues values;
#ifdef _WIN32
  HKEY hKey;
  if (RegOpenKeyExA (HKEY_CURRENT_USER, key.c_str (), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
    return values;
  for (DWORD i = 0; ; i++)
    {
    char name [256];
    BYTE data [8192];
    DWORD nameLen = sizeof name, dataLen = sizeof data - 1, type;
    const LONG rc = RegEnumValueA (hKey, i, name, &nameLen, NULL, &type, data, &dataLen);
    if (rc == ERROR_NO_MORE_ITEMS)
      break;
    if (rc != ERROR_SUCCESS)
      continue;
    if (type == REG_SZ || type == REG_EXPAND_SZ)
      {
      data [dataLen] = 0;
      values.push_back (std::make_pair (std::string (name), std::string ((const char *) data)));
      }
    else if (type == REG_DWORD && dataLen == sizeof (DWORD))
      values.push_back (std::make_pair (std::string (name),
                                        std::to_string (* (DWORD *) data)));
    }
  RegCloseKey (hKey);
#endif
  return values;
  }

// ── Importing ─────────────────────────────────────────────────────────────

Result Import (const Install & old, sqlite3 * db,
               const std::string & dataDir, const std::string & programDir)
  {
  Result result;

  // Files first: a world 5.x listed to open at startup is then already where
  // the translated WorldList says it is.  The shadow first: where both have a
  // file, it is the one 5.x saw.
  for (const char * folder : kUserFolders)
    {
    if (EqualNoCase (folder, "logs"))
      continue;           // can be huge, and are only read: left where they are
    for (const std::string & root : Roots (old))
      result.files += CopyTree (root + folder, dataDir + folder, &old,
                                dataDir, programDir, &result.worlds);
    }

  result.prefs       = ImportPreferences (old, db, dataDir, programDir);
  result.recentFiles = ReadRecentFiles (old, dataDir, programDir);
  result.words       = ImportSpellingWords (old, dataDir);

  if (result.Anything ())
    {
    sqlite3_stmt * stmt = nullptr;
    if (sqlite3_prepare_v2 (db, "INSERT OR REPLACE INTO control (name, value) VALUES ('Imported from', ?1)",
                            -1, &stmt, nullptr) == SQLITE_OK)
      {
      sqlite3_bind_text (stmt, 1, old.folder.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      }
    sqlite3_finalize (stmt);
    }
  return result;
  }

std::string Where (const Install & old)
  {
  std::string s = old.folder;
  while (s.size () > 1 && (s.back () == '\\' || s.back () == '/'))
    s.pop_back ();
  return s;
  }

std::string Describe (const Install & old, const Result & result)
  {
  std::string what = "Your settings have";
  if (result.worlds)
    what = "Your settings and " + std::to_string (result.worlds) + " world file" +
           (result.worlds == 1 ? "" : "s") + " have";
  std::string s = what + " been copied from MUSHclient 5, in " + Where (old) +
                  ", which has been left as it was.";
  for (const std::string & root : Roots (old))
    if (IsDir (root + "logs"))
      {
      s += " Its logs are still in " + root + "logs.";
      break;
      }
  return s;
  }

}   // namespace legacy
