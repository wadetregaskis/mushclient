// platform/direct.h — Windows direct.h (directory functions) stub
#pragma once
#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#ifndef _chdir
#  define _chdir  chdir
#  define _getcwd getcwd
#  define _mkdir(p) mkdir((p), 0777)
#  define _rmdir  rmdir
#  define _access access
#endif
#endif
