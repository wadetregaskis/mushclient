// world_backups.cpp -- see world_backups.h.

#include "stdafx.h"
#include "world_backups.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace world_backups {

namespace {

const time_t kHour = 60 * 60;
const time_t kDay = 24 * kHour;
const size_t kKeepNewest = 5;
const size_t kKeepHourly = 10;
const int    kKeepDays = 7;

// FNV-1a: only to tell "a/x.mcl" from "b/x.mcl".
unsigned long PathHash (const std::string & path)
  {
  unsigned long hash = 2166136261u;
  for (const char c : path)
    {
    hash ^= (unsigned char) (c == '\\' ? '/' : tolower ((unsigned char) c));
    hash = (hash * 16777619u) & 0xFFFFFFFFu;
    }
  return hash;
  }

// 20261031-235959.mcl and the like: the name is the time, so that it survives copying.
std::string NameFor (const time_t when, const std::string & extension)
  {
  char buffer [32] = "";
  const struct tm * t = localtime (&when);
  if (t)
    strftime (buffer, sizeof buffer, "%Y%m%d-%H%M%S", t);
  return std::string (buffer) + extension;
  }

bool TimeOf (const std::string & name, time_t & when)
  {
  struct tm t;
  memset (&t, 0, sizeof t);
  int year, month, day, hour, minute, second;
  if (sscanf (name.c_str (), "%4d%2d%2d-%2d%2d%2d", &year, &month, &day, &hour, &minute, &second) != 6)
    return false;
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  t.tm_isdst = -1;
  when = mktime (&t);
  return when != (time_t) -1;
  }

std::string ReadAll (const fs::path & path)
  {
  std::ifstream f (path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf ();
  return s.str ();
  }

} // anonymous namespace

std::string FolderFor (const std::string & backupsRoot, const std::string & worldPath)
  {
  const fs::path world = fs::u8path (worldPath);
  std::string stem = world.stem ().u8string ();
  for (char & c : stem)
    if (strchr ("<>:\"/\\|?*", c) || (unsigned char) c < 32)
      c = '_';
  char hash [16];
  snprintf (hash, sizeof hash, "%08lx", PathHash (worldPath));
  return backupsRoot + stem + "-" + hash;
  }

std::vector<std::string> ChooseToDelete (std::vector<Entry> entries, const time_t now)
  {
  // Newest first.
  std::sort (entries.begin (), entries.end (),
             [] (const Entry & a, const Entry & b) { return a.when > b.when; });

  std::set<std::string> keep;

  for (size_t i = 0; i < entries.size () && i < kKeepNewest; i++)
    keep.insert (entries [i].name);

  size_t hourly = 0;
  time_t lastHourly = 0;
  for (const Entry & e : entries)
    {
    if (hourly >= kKeepHourly)
      break;
    if (hourly == 0 || lastHourly - e.when >= kHour)
      {
      keep.insert (e.name);
      lastHourly = e.when;
      hourly++;
      }
    }

  std::set<long long> days;
  for (const Entry & e : entries)
    {
    if (now - e.when > kKeepDays * kDay)
      break;
    if (days.insert ((long long) (e.when / kDay)).second)
      keep.insert (e.name);
    }

  std::vector<std::string> doomed;
  for (const Entry & e : entries)
    if (!keep.count (e.name))
      doomed.push_back (e.name);
  return doomed;
  }

bool BackUpIfDue (const std::string & backupsRoot, const std::string & worldPath,
                  const bool bFirstThisSession, const time_t now)
  {
  std::error_code ec;
  const fs::path world = fs::u8path (worldPath);
  if (!fs::is_regular_file (world, ec))
    return false;    // nothing there yet to lose

  const fs::path folder = fs::u8path (FolderFor (backupsRoot, worldPath));
  fs::create_directories (folder, ec);

  std::vector<Entry> entries;
  for (fs::directory_iterator it (folder, ec), end; !ec && it != end; it.increment (ec))
    {
    Entry e;
    e.name = it->path ().filename ().u8string ();
    if (TimeOf (e.name, e.when))
      entries.push_back (e);
    }

  const Entry * pNewest = nullptr;
  for (const Entry & e : entries)
    if (!pNewest || e.when > pNewest->when)
      pNewest = &e;

  if (!bFirstThisSession && pNewest && now - pNewest->when < kHour)
    return false;

  // No copy if it would be the same again.  (A session that changes nothing and a
  // client restarted repeatedly would otherwise push everything useful out.)
  if (pNewest && ReadAll (folder / fs::u8path (pNewest->name)) == ReadAll (world))
    return false;

  const Entry fresh = { NameFor (now, world.extension ().u8string ()), now };
  fs::copy_file (world, folder / fs::u8path (fresh.name), fs::copy_options::overwrite_existing, ec);
  if (ec)
    return false;
  entries.push_back (fresh);

  for (const std::string & name : ChooseToDelete (entries, now))
    {
    std::error_code ignore;
    fs::remove (folder / fs::u8path (name), ignore);
    }
  return true;
  }

} // namespace world_backups
