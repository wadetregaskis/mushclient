// command_history_store.h -- a world's command history, on disk, for both frontends.
//
// One file per world, <data folder>/scrollback/<world id>.history, one command per
// line.  A command that contains a newline stays one entry: backslash is written "\\"
// and newline "\n", and an escape this doesn't know comes back as it was, so a file
// from before there was any escaping still loads.
//
// The text is whatever bytes the frontend holds: the world's code page on Windows,
// UTF-8 on the port.  A file isn't moved between them.

#pragma once

#include <string>
#include <vector>

namespace command_history {

std::string EncodeLine (const std::string & command);
std::string DecodeLine (const std::string & line);

// Oldest first.  False if there is no file (or it can't be read).  At most maxEntries,
// the newest; 0 means all of them.
bool Load (const std::string & path, std::vector<std::string> & commands, size_t maxEntries = 0);

// Replaces the file in one step: a crash leaves the old one or the new one.
bool Save (const std::string & path, const std::vector<std::string> & commands);

} // namespace command_history
