// pal/path_safety.h — is a name from a file or a peer one we should open?
//
// Two places hand the client a path it did not choose: a chat peer offering a
// file (chatsock.cpp, which saves it under the offered name) and a world or
// plugin file naming another file to load (<include name="...">, and the
// script and log file names among a world's options).  Windows gives three
// kinds of name a meaning no ordinary file has:
//
//   * device names -- CON, PRN, AUX, NUL, COM0-9, LPT0-9 (and the superscript
//     digits, which Windows also maps: COM¹ is COM1), CONIN$, CONOUT$ -- that
//     open the device wherever the folder is, with or without an extension,
//     and with trailing dots or spaces, which Windows strips;
//   * the device namespaces, \\.\ and \\?\ (\\.\pipe\x, \\.\COM1,
//     \\?\GLOBALROOT\...), and the NT object namespace, \??\;
//   * UNC names, \\host\share\..., where merely opening the file makes Windows
//     connect to the host and offer it the user's NTLM credentials.
//
// Header-only, std::string and nothing else, so the Windows build, the port
// and the headless tests share one body (the pal/chat_protocol.h treatment).
// Byte-oriented: the digits after COM/LPT are checked both as the Windows-1252
// superscripts (B9, B2, B3) and as their UTF-8 forms (C2 B9, C2 B2, C2 B3),
// because a name can reach CreateFile through either code page.

#pragma once

#include <string>

namespace pal
  {

inline bool IsPathSeparator (const char c)
  {
  return c == '\\' || c == '/';
  }

// The last component of a path: after the last slash of either kind, and after
// a drive's colon ("C:NUL" opens the device too).
inline std::string LastPathComponent (const std::string & path)
  {
  std::string::size_type i = path.find_last_of ("\\/");
  std::string name = (i == std::string::npos) ? path : path.substr (i + 1);
  if (name.size () >= 2 && name [1] == ':')
    name = name.substr (2);
  return name;
  }

// One path component (no slashes): is it a name Windows gives a device?
inline bool IsWindowsDeviceName (const std::string & component)
  {
  // Windows drops trailing dots and spaces from a name before using it:
  // "NUL.", "CON " and "AUX. ." are the devices.
  std::string name = component;
  while (!name.empty () && (name.back () == '.' || name.back () == ' '))
    name.pop_back ();

  // The device is the part before the first dot: "NUL.txt", "com1.tar.gz".
  std::string stem = name.substr (0, name.find ('.'));
  while (!stem.empty () && stem.back () == ' ')
    stem.pop_back ();       // "CON .txt" is CON too

  for (char & c : stem)
    if (c >= 'a' && c <= 'z')
      c = (char) (c - 'a' + 'A');

  static const char * const kNames [] =
    { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };
  for (const char * p : kNames)
    if (stem == p)
      return true;

  if (stem.size () >= 4 &&
      (stem.compare (0, 3, "COM") == 0 || stem.compare (0, 3, "LPT") == 0))
    {
    const std::string digit = stem.substr (3);
    if (digit.size () == 1 && digit [0] >= '0' && digit [0] <= '9')
      return true;
    static const char * const kSuperscripts [] =
      { "\xB9", "\xB2", "\xB3",               // Windows-1252 ¹ ² ³
        "\xC2\xB9", "\xC2\xB2", "\xC2\xB3" }; // the same in UTF-8
    for (const char * p : kSuperscripts)
      if (digit == p)
        return true;
    }

  return false;
  }

// \\.\..., \\?\... (either slash), or \??\...
inline bool IsDeviceNamespacePath (const std::string & path)
  {
  if (path.size () >= 4 &&
      IsPathSeparator (path [0]) && IsPathSeparator (path [1]) &&
      (path [2] == '.' || path [2] == '?') && IsPathSeparator (path [3]))
    return true;
  if (path.size () >= 4 &&
      IsPathSeparator (path [0]) && path [1] == '?' && path [2] == '?' &&
      IsPathSeparator (path [3]))
    return true;
  // "\\." or "\\?" alone, or followed by nothing useful, is no better
  if (path.size () == 3 &&
      IsPathSeparator (path [0]) && IsPathSeparator (path [1]) &&
      (path [2] == '.' || path [2] == '?'))
    return true;
  return false;
  }

// \\host\share\... (either slash) that is not one of the device namespaces.
inline bool IsUncPath (const std::string & path)
  {
  return path.size () >= 2 &&
         IsPathSeparator (path [0]) && IsPathSeparator (path [1]) &&
         !IsDeviceNamespacePath (path);
  }

// "\\host\share" of a UNC path, lower case and with backslashes, so two names
// for the same share compare equal; "" when the path is not UNC (or names no
// share).
inline std::string UncShareRoot (const std::string & path)
  {
  if (!IsUncPath (path))
    return "";
  std::string::size_type i = 2;
  while (i < path.size () && IsPathSeparator (path [i]))
    i++;                                    // \\\host is \\host to Windows
  const std::string::size_type hostStart = i;
  while (i < path.size () && !IsPathSeparator (path [i]))
    i++;
  if (i == hostStart || i >= path.size ())
    return "";                              // no host, or no share
  const std::string host = path.substr (hostStart, i - hostStart);
  while (i < path.size () && IsPathSeparator (path [i]))
    i++;
  const std::string::size_type shareStart = i;
  while (i < path.size () && !IsPathSeparator (path [i]))
    i++;
  if (i == shareStart)
    return "";
  std::string root = "\\\\" + host + "\\" + path.substr (shareStart, i - shareStart);
  for (char & c : root)
    if (c >= 'A' && c <= 'Z')
      c = (char) (c - 'A' + 'a');
  return root;
  }

  }   // namespace pal
