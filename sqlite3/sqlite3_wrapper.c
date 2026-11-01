// Wrapper to set SQLite compile-time options without modifying the amalgamated source
#ifdef _WIN32
// The project says _WIN32_WINNT=0x0501, which hides GetTickCount64 (Vista), and
// SQLite has called it unconditionally since 3.5x.  VS 2022 can't build for XP anyway.
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define SQLITE_SOUNDEX 1
#define SQLITE_ENABLE_FTS3 1
#define SQLITE_ENABLE_FTS3_PARENTHESIS 1
#define SQLITE_ENABLE_RTREE 1
#define SQLITE_USE_URI 1
#include "sqlite3.c"
