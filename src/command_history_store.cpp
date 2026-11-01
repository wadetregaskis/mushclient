// command_history_store.cpp -- see command_history_store.h.

#include "stdafx.h"
#include "command_history_store.h"
#include "scrollback_store.h"

#include <cstdio>

namespace command_history {

std::string EncodeLine (const std::string & command)
  {
  std::string out;
  out.reserve (command.size ());
  for (const char c : command)
    {
    if (c == '\\')
      out += "\\\\";
    else if (c == '\n')
      out += "\\n";
    else if (c != '\r')    // Windows' command box puts these in a multi-line command
      out += c;
    }
  return out;
  }

std::string DecodeLine (const std::string & line)
  {
  std::string out;
  out.reserve (line.size ());
  for (size_t i = 0; i < line.size (); i++)
    {
    if (line [i] == '\\' && i + 1 < line.size ())
      {
      if (line [i + 1] == '\\')
        {
        out += '\\';
        i++;
        continue;
        }
      if (line [i + 1] == 'n')
        {
        out += '\n';
        i++;
        continue;
        }
      }
    out += line [i];
    }
  return out;
  }

bool Load (const std::string & path, std::vector<std::string> & commands, const size_t maxEntries)
  {
  FILE * f = fopen (path.c_str (), "rb");
  if (!f)
    return false;

  std::string all;
  char buffer [65536];
  size_t got;
  while ((got = fread (buffer, 1, sizeof buffer, f)) > 0)
    all.append (buffer, got);
  fclose (f);

  commands.clear ();
  size_t at = 0;
  while (at < all.size ())
    {
    size_t end = all.find ('\n', at);
    if (end == std::string::npos)
      end = all.size ();
    size_t lineEnd = end;
    if (lineEnd > at && all [lineEnd - 1] == '\r')
      lineEnd--;
    if (lineEnd > at)
      commands.push_back (DecodeLine (all.substr (at, lineEnd - at)));
    at = end + 1;
    }

  if (maxEntries && commands.size () > maxEntries)
    commands.erase (commands.begin (), commands.end () - (ptrdiff_t) maxEntries);
  return true;
  }

bool Save (const std::string & path, const std::vector<std::string> & commands)
  {
  std::string all;
  for (const std::string & command : commands)
    {
    all += EncodeLine (command);
    all += '\n';
    }
  return scrollback::WriteFileAtomically (path, all.data (), all.size ());
  }

} // namespace command_history
