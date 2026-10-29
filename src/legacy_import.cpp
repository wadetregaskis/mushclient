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

// The user folder `name` is, spelt as this version spells it ("Worlds" ->
// "worlds": a case-sensitive file system would otherwise have two), or null.
const char * UserFolder (const std::string & name)
  {
  for (const char * p : kUserFolders)
    if (EqualNoCase (name, p))
      return p;
  return nullptr;
  }

// A file name as UTF-8, which every name has.  Not path::string (): on Windows
// that is the ANSI code page, and MSVC's throws for a name it cannot spell --
// a Japanese one on a Western machine, say -- which would end the program.
std::string U8 (const fs::path & p)
  {
  const auto s = p.u8string ();   // std::string in C++17, std::u8string after
  return std::string (s.begin (), s.end ());
  }

// A symbolic link, or on Windows a junction (which is_symlink does not count):
// either could lead anywhere, including round again.
bool IsLink (const fs::directory_entry & entry)
  {
  std::error_code ec;
  if (entry.is_symlink (ec))
    return true;
#ifdef _WIN32
  const DWORD a = GetFileAttributesW (entry.path ().c_str ());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  return false;
#endif
  }

// Whether a folder holds a file of this name, in any case: 5.x's installer
// wrote "mushclient.exe", and a case-sensitive file system keeps what it wrote.
bool HasFileNoCase (const std::string & dir, const std::string & name)
  {
  std::error_code ec;
  for (fs::directory_iterator it (P (dir), ec), end; !ec && it != end; it.increment (ec))
    if (EqualNoCase (U8 (it->path ().filename ()), name))
      return true;
  return false;
  }

#ifndef _WIN32
// 5.x's text is in the Windows code page -- Western, as good as always -- and
// this platform's is UTF-8.  Valid UTF-8 (ASCII included) is left as it is.
std::string FromWindowsText (const std::string & s)
  {
  bool ascii = true;
  for (unsigned char c : s)
    if (c >= 0x80)
      ascii = false;
  if (ascii)
    return s;
  // valid UTF-8 already?
  size_t i = 0;
  bool utf8 = true;
  while (i < s.size () && utf8)
    {
    const unsigned char c = (unsigned char) s [i];
    const int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (n < 0)
      utf8 = false;
    for (int k = 1; utf8 && k <= n; k++)
      if (i + k >= s.size () || ((unsigned char) s [i + k] & 0xC0) != 0x80)
        utf8 = false;
    i += n + 1;
    }
  if (utf8)
    return s;
  // cp1252's 0x80-0x9F; the rest of its upper half is Latin-1's
  static const unsigned short k80 [32] = {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178 };
  std::string out;
  for (unsigned char c : s)
    {
    const unsigned u = c < 0x80 ? c : c < 0xA0 ? k80 [c - 0x80] : c;
    if (u < 0x80)
      out += (char) u;
    else if (u < 0x800)
      {
      out += (char) (0xC0 | (u >> 6));
      out += (char) (0x80 | (u & 0x3F));
      }
    else
      {
      out += (char) (0xE0 | (u >> 12));
      out += (char) (0x80 | ((u >> 6) & 0x3F));
      out += (char) (0x80 | (u & 0x3F));
      }
    }
  return out;
  }

// Settings that name this platform's fonts, or size them for its screens: a
// Windows MUSHclient's FixedSys is no font here.
bool IsWindowsFontPreference (const std::string & name)
  {
  for (const char * p : { "DefaultInputFont", "DefaultOutputFont", "NotepadFont", "FixedPitchFont",
                          "PrinterFont", "DefaultInputFontHeight", "DefaultOutputFontHeight",
                          "NotepadFontSize", "FixedPitchFontSize", "PrinterFontSize" })
    if (EqualNoCase (name, p))
      return true;
  return false;
  }
#endif

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
#ifndef _WIN32
  if (IsWindowsFontPreference (name))
    return;               // this platform's default, which suits it
  value = FromWindowsText (value);
#endif
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
      // a UTF-16 unit; as UTF-8, this side's encoding (a surrogate pair's
      // halves become '?': names in the registry outside the BMP are a rarity)
      if (v < 0x80)
        out += (char) v;
      else if (v >= 0xD800 && v < 0xE000)
        out += '?';
      else if (v < 0x800)
        {
        out += (char) (0xC0 | (v >> 6));
        out += (char) (0x80 | (v & 0x3F));
        }
      else
        {
        out += (char) (0xE0 | (v >> 12));
        out += (char) (0x80 | ((v >> 6) & 0x3F));
        out += (char) (0x80 | (v & 0x3F));
        }
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
    for (const std::string apps : { h + "/Applications/Wineskin", h + "/Applications",
                                    std::string ("/Applications/Wineskin"), std::string ("/Applications") })
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

bool FromFolder (const std::string & folderIn, Install & old, const std::string & programDir)
  {
  const std::string folder = Slash (folderIn);

  // a Wine prefix or bottle: the 5.x in it -- or a Wineskin or Porting Kit
  // wrapper, whose prefix is inside the app (chosen as the app, or a folder
  // part way down to the prefix)
  for (const std::string & prefix : { folder, folder + "Contents/SharedSupport/prefix/",
                                      folder + "SharedSupport/prefix/", folder + "prefix/" })
    {
    const std::vector<Install> inPrefix = InPrefix (prefix, programDir);
    if (!inPrefix.empty ())
      {
      old = inPrefix [0];
      return true;
      }
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
  // not 6.00's own folder, unless a 5.x once ran there
  return programDir.empty () || Qualifies (old, programDir);
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
    std::string rest = full.substr (root.size ());
    if (rest.compare (0, 2, "..") == 0)
      return MapWindowsPath (full, old);   // out of the old folder: kept
    const size_t slash = rest.find ('\\');
    const char * folder = UserFolder (rest.substr (0, slash));
    const bool user = rest.empty () || folder;
    if (folder)
      rest = folder + (slash == std::string::npos ? "" : rest.substr (slash));
    const std::string native = Native (rest);
    if (user)
      return dataDir + native;
    // the program's: this version's own, if it has one...
    if (Exists (programDir + native))
      return programDir + native;
    // ...else the old one's, where it still is (one of the user's files
    // beside 5.x's, which is not copied): there, not somewhere it is not
    for (const std::string & here : Roots (old))
      if (Exists (here + native))
        return here + native;
    if (pResolved)
      *pResolved = false;
    return dataDir + native;
    }

  // Somewhere of the user's choosing, made a path on this machine.  Under
  // Wine a drive with no mapping is nowhere here.
  const std::string mapped = MapWindowsPath (full, old);
  if (pResolved && !old.prefix.empty () && mapped == full)
    *pResolved = false;
  return mapped == full ? path : mapped;
  }

std::string TranslateText (const std::string & text, const Install & old,
                           const std::string & dataDir, const std::string & programDir)
  {
  const std::string lower = Lower (text);

  // Each root as written plainly (C:\MUSHclient\), with forward slashes
  // (C:/MUSHclient/) and as a script's string literal writes it
  // (C:\\MUSHclient\\): a path found in one form goes back in the same form.
  enum Form { ePlain, eForward, eDoubled };
  struct Root { std::string text; Form form; };
  std::vector<Root> roots;
  for (const std::string & r : { old.shadow, old.windowsFolder })
    {
    if (r.empty ())
      continue;
    std::string forward = Lower (r), doubled;
    std::replace (forward.begin (), forward.end (), '\\', '/');
    for (char c : Lower (r))
      doubled += c == '\\' ? std::string ("\\\\") : std::string (1, c);
    roots.push_back ({ doubled, eDoubled });   // before plain, which it contains
    roots.push_back ({ Lower (r), ePlain });
    roots.push_back ({ forward, eForward });
    }

  std::string out;
  size_t i = 0;
  while (i < text.size ())
    {
    size_t best = std::string::npos;
    Form form = ePlain;
    for (const Root & r : roots)
      {
      const size_t k = lower.find (r.text, i);
      if (k < best)
        {
        best = k;
        form = r.form;
        }
      }
    if (best == std::string::npos)
      break;
    // A path runs to whatever ends a quoted string, an XML value or entity
    // (a world file writes a quote in text as &quot;), a line or one of
    // MUSHclient's "*"-separated lists.  Not to a space: "Program Files" has
    // one.
    size_t end = best;
    while (end < text.size () && !strchr ("\"'<>&\r\n\t*|", text [end]))
      end++;
    std::string found = text.substr (best, end - best);
    if (form == eDoubled)
      {
      std::string single;
      for (size_t k = 0; k < found.size (); k++)
        {
        single += found [k];
        if (found [k] == '\\' && k + 1 < found.size () && found [k + 1] == '\\')
          k++;
        }
      found = single;
      }
    std::string translated = TranslatePath (found, old, dataDir, programDir);
    if (form == eForward)
      std::replace (translated.begin (), translated.end (), '\\', '/');
    else if (form == eDoubled)
      {
      std::string doubled;
      for (char c : translated)
        doubled += c == '\\' ? std::string ("\\\\") : std::string (1, c);
      translated = doubled;
      }
    out += text.substr (i, best - i);
    out += translated;
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

namespace {

// CopyTree, in paths rather than strings, so that a file is copied under the
// name it has, whatever the code page could spell.
int CopyPaths (const fs::path & src, const fs::path & dst,
               const Install * pOld,
               const std::string & dataDir, const std::string & programDir,
               int * pWorlds, const ReplaceableFn & replaceable, int depth)
  {
  std::error_code ec;
  if (depth > 32 || !fs::is_directory (src, ec))
    return 0;
  fs::create_directories (dst, ec);

  int n = 0;
  for (fs::directory_iterator it (src, ec), end; !ec && it != end; it.increment (ec))
    {
    std::error_code fec;
    const fs::path & path = it->path ();
    const fs::path target = dst / path.filename ();
    if (IsLink (*it))
      continue;
    if (it->is_directory (fec))
      {
      n += CopyPaths (path, target, pOld, dataDir, programDir, pWorlds, replaceable, depth + 1);
      continue;
      }
    if (!it->is_regular_file (fec))
      continue;
    const bool world = IsWorldFile (U8 (path.filename ()));
    // Never over a file already there -- but a world file still exactly as
    // this version installed it holds nothing of theirs, and 5.x's is theirs.
    if (fs::exists (target, fec) &&
        !(world && replaceable && replaceable (target)))
      continue;
    bool ok;
    std::string text;
    if (world && pOld && ReadWholeFile (path, text))
      ok = WriteWholeFile (target, TranslateText (text, *pOld, dataDir, programDir));
    else
      ok = fs::copy_file (path, target, fs::copy_options::overwrite_existing, fec) && !fec;
    if (ok)
      {
      n++;
      if (world && pWorlds)
        ++*pWorlds;
      }
    }
  return n;
  }

}   // namespace

int CopyTree (const std::string & from, const std::string & to,
              const Install * pOld,
              const std::string & dataDir, const std::string & programDir,
              int * pWorlds, const ReplaceableFn & replaceable)
  {
  return CopyPaths (P (from), P (to), pOld, dataDir, programDir, pWorlds, replaceable, 0);
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
  // as long as the longest of them: a WorldList can run to many kilobytes
  DWORD maxName = 0, maxData = 0;
  RegQueryInfoKeyA (hKey, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &maxName, &maxData, NULL, NULL);
  std::vector<char> name (maxName + 2);
  std::vector<BYTE> data (maxData + 2);
  for (DWORD i = 0; ; i++)
    {
    DWORD nameLen = (DWORD) name.size (), dataLen = (DWORD) data.size () - 1, type;
    const LONG rc = RegEnumValueA (hKey, i, name.data (), &nameLen, NULL, &type, data.data (), &dataLen);
    if (rc == ERROR_NO_MORE_ITEMS)
      break;
    if (rc != ERROR_SUCCESS)
      continue;
    if (type == REG_SZ || type == REG_EXPAND_SZ)
      {
      data [dataLen] = 0;
      values.push_back (std::make_pair (std::string (name.data ()), std::string ((const char *) data.data ())));
      }
    else if (type == REG_DWORD && dataLen == sizeof (DWORD))
      values.push_back (std::make_pair (std::string (name.data ()),
                                        std::to_string (* (DWORD *) data.data ())));
    }
  RegCloseKey (hKey);
#endif
  return values;
  }

// ── Importing ─────────────────────────────────────────────────────────────

Result Import (const Install & old, sqlite3 * db,
               const std::string & dataDir, const std::string & programDir,
               const ReplaceableFn & replaceable)
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
                                dataDir, programDir, &result.worlds, replaceable);
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
  const std::string worlds = std::to_string (result.worlds) + " world file" +
                             (result.worlds == 1 ? "" : "s");
  std::string what;
  if (result.prefs && result.worlds)
    what = "Your settings and " + worlds + " have";
  else if (result.prefs)
    what = "Your settings have";
  else if (result.worlds)
    what = "Your " + worlds + " have";
  else
    what = "Your files have";
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
