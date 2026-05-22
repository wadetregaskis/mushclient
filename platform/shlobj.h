// platform/shlobj.h — stub for Windows <shlobj.h>
// Shell object types needed by GlobalPrefs.cpp and similar.
// The full shell-folder browser stubs are already in mfc_ui.h (BROWSEINFO etc).
#pragma once
#ifndef _WIN32
// Pull in the browser stubs from mfc_ui.h via the master compat header.
// This file exists purely so that #include <shlobj.h> / #include "shlobj.h"
// resolves without error when building on non-Windows.
#include "platform/compat.h"

// Additional SHGetSpecialFolderPath stub
#ifndef CSIDL_APPDATA
#  define CSIDL_APPDATA      0x001a
#  define CSIDL_PERSONAL     0x0005
#  define CSIDL_DESKTOPDIRECTORY 0x0010
#  define CSIDL_COMMON_APPDATA 0x0023
#endif

#include <cstring>
inline BOOL SHGetSpecialFolderPath(HWND, LPTSTR buf, int, BOOL) {
    if (buf) buf[0] = '\0';
    return FALSE;
}
#endif // !_WIN32
