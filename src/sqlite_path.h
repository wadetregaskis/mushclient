/* sqlite_path.h -- a file name for sqlite3_open, on Windows.

   sqlite3_open takes UTF-8.  MUSHclient's Windows file names are in the ANSI
   code page -- GetInfo's folders, the data folder, whatever a script built
   from them -- and the two agree only while the name is ASCII.  Under a
   profile folder like C:\Users\José\ the preferences database, a plugin's
   database or the spell checker's would not open at all.

   So a name that is not valid UTF-8 is converted from the ANSI code page.  One
   that is valid UTF-8 (all ASCII names are) is passed as it is, as it always
   was: a script may have built it in UTF-8 on purpose.

   C and C++ alike: lsqlite3.c includes it too. */

#ifndef MUSHCLIENT_SQLITE_PATH_H
#define MUSHCLIENT_SQLITE_PATH_H

#ifdef _WIN32

#include <windows.h>
#include <stdlib.h>

/* A malloc'd UTF-8 copy of `name` to open instead, or NULL to open `name`
   itself.  free() it. */
static __inline char * sqlite_path_utf8 (const char * name)
  {
  int wide, bytes;
  WCHAR * w;
  char * utf8;

  if (name == NULL ||
      MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, NULL, 0) > 0)
    return NULL;   /* UTF-8 already */

  wide = MultiByteToWideChar (CP_ACP, 0, name, -1, NULL, 0);
  if (wide <= 0)
    return NULL;
  w = (WCHAR *) malloc (wide * sizeof (WCHAR));
  if (w == NULL)
    return NULL;
  MultiByteToWideChar (CP_ACP, 0, name, -1, w, wide);

  bytes = WideCharToMultiByte (CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  utf8 = bytes > 0 ? (char *) malloc (bytes) : NULL;
  if (utf8 != NULL)
    WideCharToMultiByte (CP_UTF8, 0, w, -1, utf8, bytes, NULL, NULL);
  free (w);
  return utf8;
  }

#endif  /* _WIN32 */

#endif  /* MUSHCLIENT_SQLITE_PATH_H */
