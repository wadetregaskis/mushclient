// bundled_files.h -- keeping the user's copies of the files that ship with
// MUSHclient up to date.
//
// The bundled plugins (and the Discworld edition's world, mapper and maps) are
// copied into the user's data folder, because the user may change them and
// plugins write beside them; see user_data.h (Windows) and SeedBundledUserData
// (wx_mfc_impl.cpp).  Copying only files the user does not have would mean an
// upgrade never delivered a fixed plugin to anyone who had run an older
// version.  So each copy's contents are recorded, as a hash, in a manifest in
// the data folder, and when a new version runs:
//
//   - a copy the user has not changed is replaced with the new version;
//   - a copy the user has changed is not touched without asking, if this
//     version brings a new one: they choose, plugin by plugin, to upgrade it
//     (their copy is kept beside it, as <name>.backup) or keep theirs, and can
//     have the choice remembered.  Only plugins and scripts are asked about;
//     any other file that differs -- the mapper's database, say -- is data a
//     plugin wrote, and is kept;
//   - a copy the user deleted stays deleted;
//   - a world file is never replaced: once used, it is the user's.
//
// Shared by both builds; only the question is asked differently.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace bundled {

struct Answer
  {
  bool upgrade  = false;   // replace their copy with the new version
  bool remember = false;   // and do the same, unasked, from now on
  };

// What to ask about one changed file: its path relative to the data folder,
// something to call it by -- a plugin's name, or the path -- and the name
// (beside it) their copy would be kept as, if upgraded.
typedef std::function<Answer (const std::string & relative, const std::string & name,
                              const std::string & backup)> AskFn;

// A world file's contents as they should be copied (translated, say); empty
// for as they are.
typedef std::function<std::string (const std::string & text)> TransformFn;

struct Stats
  {
  int copied   = 0;   // new to the user
  int upgraded = 0;   // replaced: unchanged, or the user said so
  int kept     = 0;   // changed by the user, and left
  };

// Bring `subtrees` (relative folders, e.g. "worlds") of `from` into `to`,
// recording what was copied in `manifest`.  Paths in this platform's form,
// folders ending in a separator.
Stats Sync (const std::string & from, const std::string & to,
            const std::vector<std::string> & subtrees,
            const std::string & manifest,
            const AskFn & ask, const TransformFn & transformWorld = nullptr);

// Whether `file`, in `to`, is exactly as Sync last copied it there: nothing of
// the user's would be lost by replacing it.
bool Untouched (const std::string & manifest, const std::string & to,
                const std::filesystem::path & file);

// Something that changes whenever the files under `subtrees` of `from` might
// have: a new version, or a new build of the same one.  Cheap -- sizes and
// times, not contents -- so it can be checked at every start.
std::string Fingerprint (const std::string & from, const std::vector<std::string> & subtrees);

// The name a plugin file gives itself (<plugin name="...">), or empty.
std::string PluginName (const std::string & xml);

}   // namespace bundled
