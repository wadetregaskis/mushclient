// platform/process.h — Windows process.h stub for non-Windows builds
#pragma once
#ifndef _WIN32
#include <unistd.h>
#ifndef _getpid
#  define _getpid getpid
#endif
#ifndef _beginthread
#  define _beginthread(fn, stack, arg) ((void)0)
#endif
#endif
