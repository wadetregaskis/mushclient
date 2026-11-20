// world_backups.h -- rolling copies of a world file, kept when it is about to be saved
// over, so that a bad change (or one that makes the client crash) can be undone.
//
// They live in <data folder>/backups/<world file's name>-<hash of its path>/, named by
// when the copy was taken, and are ordinary world files: to go back to one, open it
// and Save As over the original, or copy it there.  File > World Backups opens the
// folder.
//
// A copy is taken before a save if the world hasn't been backed up this session, or
// the newest copy is an hour old.  (The first of those is what makes a setting that
// crashes the client recoverable: it is saved a few seconds after it is changed, and
// the copy from before that survives any number of crashes.)  A copy is not taken if
// it would be the same as the newest one.
//
// What's kept:
//
//   the newest 5, however close together;
//   the newest 10 that are at least an hour apart;
//   the newest one of each of the last 7 days.
//
// All pure file work, called from the UI thread: a world file is small.

#pragma once

#include <ctime>
#include <string>
#include <vector>

namespace world_backups {

// The folder for this world file.  backupsRoot is <data folder>/backups, with a
// trailing separator.
std::string FolderFor (const std::string & backupsRoot, const std::string & worldPath);

struct Entry
  {
  std::string name;
  time_t      when;
  };

// Which to delete from these, given the time now.  Pure, for testing.  Days are local.
std::vector<std::string> ChooseToDelete (std::vector<Entry> entries, time_t now);

// If this file name is one of the copies, when it was taken.  Anything else in the
// folder is left alone.  For testing.
bool TimeOfCopy (const std::string & name, time_t & when);

// Take a copy if one is due, and prune.  bFirstThisSession: this world hasn't been
// saved since it was opened.  True if a copy was made.
bool BackUpIfDue (const std::string & backupsRoot, const std::string & worldPath,
                  bool bFirstThisSession, time_t now);

} // namespace world_backups
