// tests/posix_compat.h — the handful of POSIX calls the test suite uses as
// scaffolding, on Windows.
//
// The suite reaches for <unistd.h> and <sys/resource.h> for temp-file names,
// short sleeps and RSS leak checks — scaffolding around the assertions, never
// the thing under test.  On macOS and Linux this header is those two includes
// and nothing else; on Windows it maps the five calls that actually get used.
//
// Deliberately small.  Anything genuinely POSIX-shaped that a test *asserts
// on* — socketpair, dup2, setrlimit — is not here: a suite that needs one of
// those is opted out of the Windows target by name in
// tests/win/gen_test_vcxproj.ps1, with the reason recorded there.  Faking a
// socketpair would make the test pass while testing something else.

#pragma once

#ifndef _WIN32

#include <unistd.h>
#include <sys/resource.h>

#else   // _WIN32

// winsock2.h BEFORE windows.h, always.  Several tests include this header
// ahead of stdafx.h, and windows.h arriving first makes afxwin.h stop with
// "#error MFC requires use of Winsock2.h" — so a header meant to be safe
// anywhere has to get the order right itself rather than rely on its includer.
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <direct.h>   // _getcwd (this file is never compiled with platform/ on
                     // the include path, so this is the CRT header, not the shim)
#include <process.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

#ifndef getpid
#define getpid _getpid
#endif
#ifndef unlink
#define unlink _unlink
#endif
#ifndef getcwd
#define getcwd _getcwd
#endif

// usleep: microseconds, and Sleep takes milliseconds.  Round UP, so a test
// that sleeps 500 us to let something settle does not get 0.
inline void usleep(unsigned long usec)
{
    ::Sleep((DWORD) ((usec + 999) / 1000));
}

// getrusage, only as much of it as the RSS leak checks read.  ru_maxrss is
// the peak working set; on Linux that is kilobytes, on macOS bytes, and the
// tests that use it compare a delta against a generous bound rather than an
// absolute, so kilobytes (matching Linux) is the useful choice here.
#define RUSAGE_SELF 0

struct rusage {
    long ru_maxrss;
};

inline int getrusage(int /*who*/, struct rusage* usage)
{
    if (usage == nullptr)
        return -1;
    PROCESS_MEMORY_COUNTERS pmc;
    ZeroMemory(&pmc, sizeof pmc);
    pmc.cb = sizeof pmc;
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof pmc))
        return -1;
    usage->ru_maxrss = (long) (pmc.PeakWorkingSetSize / 1024);
    return 0;
}

#endif  // _WIN32
