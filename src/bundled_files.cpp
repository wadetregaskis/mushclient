// bundled_files.cpp -- keeping the user's copies of the bundled files up to
// date.  See bundled_files.h.

#include "stdafx.h"
#include "bundled_files.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace bundled {

namespace {

// The manifest: one line per file the user was given,
//   <hash> TAB <remembered choice: "", "keep" or "upgrade"> TAB <path>
// the path relative to the data folder, with forward slashes on every
// platform.  An empty hash marks a file known but never copied from here.
struct Entry
  {
  std::string hash;
  std::string choice;
  };
typedef std::map<std::string, Entry> Manifest;

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
  std::error_code ec;
  fs::create_directories (name.parent_path (), ec);
  std::ofstream f (name, std::ios::binary | std::ios::trunc);
  if (!f)
    return false;
  f.write (contents.data (), (std::streamsize) contents.size ());
  return (bool) f;
  }

// FNV-1a: telling one version of a file from another, not resisting anyone.
std::string Hash (const std::string & data)
  {
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : data)
    {
    h ^= c;
    h *= 1099511628211ULL;
    }
  char buf [17];
  snprintf (buf, sizeof buf, "%016llx", (unsigned long long) h);
  return buf;
  }

Manifest Load (const std::string & name)
  {
  Manifest m;
  std::string text;
  if (!ReadWholeFile (fs::path (name), text))
    return m;
  std::istringstream ss (text);
  std::string line;
  while (std::getline (ss, line))
    {
    if (!line.empty () && line.back () == '\r')
      line.pop_back ();
    const size_t a = line.find ('\t');
    const size_t b = a == std::string::npos ? a : line.find ('\t', a + 1);
    if (b == std::string::npos || b + 1 >= line.size ())
      continue;
    Entry e;
    e.hash   = line.substr (0, a);
    e.choice = line.substr (a + 1, b - a - 1);
    m [line.substr (b + 1)] = e;
    }
  return m;
  }

void Save (const std::string & name, const Manifest & m)
  {
  std::string text =
    "# The files MUSHclient copied here, so that an upgrade can tell the ones you\n"
    "# have changed from the ones you have not.  hash, remembered choice, path.\n";
  for (const auto & e : m)
    text += e.second.hash + "\t" + e.second.choice + "\t" + e.first + "\n";
  WriteWholeFile (fs::path (name), text);
  }

bool EndsWithNoCase (const std::string & s, const char * suffix)
  {
  const size_t n = strlen (suffix);
  if (s.size () < n)
    return false;
  for (size_t i = 0; i < n; i++)
    if (tolower ((unsigned char) s [s.size () - n + i]) != tolower ((unsigned char) suffix [i]))
      return false;
  return true;
  }

std::string NameFor (const std::string & key, const std::string & contents)
  {
  if (EndsWithNoCase (key, ".xml"))
    {
    const std::string name = PluginName (contents);
    if (!name.empty ())
      return name;
    }
  return key;
  }

}   // namespace

std::string PluginName (const std::string & xml)
  {
  const size_t tag = xml.find ("<plugin");
  if (tag == std::string::npos)
    return "";
  const size_t close = xml.find ('>', tag);
  const std::string attrs = xml.substr (tag, close == std::string::npos ? std::string::npos : close - tag);
  size_t at = 0;
  while ((at = attrs.find ("name", at)) != std::string::npos)
    {
    // the attribute itself, not the end of another one's name ("plugin_name")
    const bool start = at > 0 && isspace ((unsigned char) attrs [at - 1]);
    size_t i = at + 4;
    while (i < attrs.size () && isspace ((unsigned char) attrs [i]))
      i++;
    if (start && i < attrs.size () && attrs [i] == '=')
      {
      i++;
      while (i < attrs.size () && isspace ((unsigned char) attrs [i]))
        i++;
      if (i < attrs.size () && (attrs [i] == '"' || attrs [i] == '\''))
        {
        const size_t end = attrs.find (attrs [i], i + 1);
        if (end != std::string::npos)
          return attrs.substr (i + 1, end - i - 1);
        }
      }
    at += 4;
    }
  return "";
  }

Stats Sync (const std::string & from, const std::string & to,
            const std::vector<std::string> & subtrees,
            const std::string & manifestName,
            const AskFn & ask, const TransformFn & transformWorld)
  {
  Stats stats;
  Manifest manifest = Load (manifestName);

  for (const std::string & sub : subtrees)
    {
    const fs::path root = fs::path (from) / sub;
    std::error_code ec;
    if (!fs::is_directory (root, ec))
      continue;
    for (fs::recursive_directory_iterator it (root, ec), end; !ec && it != end; it.increment (ec))
      {
      std::error_code fec;
      if (!it->is_regular_file (fec))
        continue;
      const fs::path relative = fs::path (sub) / it->path ().lexically_relative (root);
      const std::string key = relative.generic_string ();
      const fs::path mine = fs::path (to) / relative;
      const bool world = EndsWithNoCase (key, ".mcl");

      std::string shipped;
      if (!ReadWholeFile (it->path (), shipped))
        continue;
      if (world && transformWorld)
        {
        const std::string t = transformWorld (shipped);
        if (!t.empty ())
          shipped = t;
        }
      const std::string shippedHash = Hash (shipped);
      const auto known = manifest.find (key);

      if (!fs::exists (mine, fec))
        {
        // Given before and gone now: they deleted it, and it stays deleted.
        if (known != manifest.end ())
          continue;
        if (WriteWholeFile (mine, shipped))
          {
          manifest [key].hash = shippedHash;
          stats.copied++;
          }
        continue;
        }

      std::string theirs;
      if (!ReadWholeFile (mine, theirs))
        continue;
      const std::string theirHash = Hash (theirs);
      if (theirHash == shippedHash)
        {
        manifest [key].hash = shippedHash;   // already this version
        continue;
        }
      if (world)
        {
        manifest [key];                      // known, so a deletion is respected
        continue;
        }

      const bool unchanged = known != manifest.end () && !known->second.hash.empty () &&
                             known->second.hash == theirHash;
      bool upgrade;
      if (unchanged)
        upgrade = true;                      // nothing of theirs to lose
      else if (known != manifest.end () && known->second.choice == "upgrade")
        upgrade = true;
      else if (known != manifest.end () && known->second.choice == "keep")
        upgrade = false;
      else
        {
        const Answer answer = ask ? ask (fs::path (relative).make_preferred ().string (), NameFor (key, theirs))
                                  : Answer ();
        upgrade = answer.upgrade;
        if (answer.remember)
          manifest [key].choice = answer.upgrade ? "upgrade" : "keep";
        }

      if (upgrade)
        {
        if (!unchanged)
          {
          fs::path backup = mine;
          backup += ".backup";
          fs::copy_file (mine, backup, fs::copy_options::overwrite_existing, fec);
          }
        if (WriteWholeFile (mine, shipped))
          {
          manifest [key].hash = shippedHash;
          stats.upgraded++;
          }
        }
      else
        {
        // Their copy stays, and the hash recorded stays what was given before
        // -- so it still reads as theirs, changed, at the next upgrade.
        manifest [key];
        stats.kept++;
        }
      }
    }

  Save (manifestName, manifest);
  return stats;
  }

}   // namespace bundled
