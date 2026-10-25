// platform/winreg.h
// Stub for Windows registry API on non-Windows platforms.
// MUSHclient uses registry access only on Windows; these stubs satisfy the
// compiler so dialog files that include winreg.h compile cleanly.

#pragma once

#ifndef _WIN32

typedef void* HKEY;

#define HKEY_LOCAL_MACHINE  ((HKEY)(uintptr_t)0x80000002)
#define HKEY_CURRENT_USER   ((HKEY)(uintptr_t)0x80000001)
#define HKEY_CLASSES_ROOT   ((HKEY)(uintptr_t)0x80000000)

#define REG_SZ              1
#define REG_DWORD           4
#define REG_BINARY          3
#define REG_EXPAND_SZ       2

#define ERROR_SUCCESS       0L
#define ERROR_FILE_NOT_FOUND 2L

#include "windows_types.h"  // for LONG, DWORD, LPTSTR, LPCTSTR, etc.

inline LONG RegOpenKeyEx(HKEY, LPCTSTR, DWORD, DWORD, HKEY*) { return ERROR_FILE_NOT_FOUND; }
inline LONG RegQueryValueEx(HKEY, LPCTSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
inline LONG RegCloseKey(HKEY) { return ERROR_SUCCESS; }
inline LONG RegCreateKeyEx(HKEY, LPCTSTR, DWORD, LPTSTR, DWORD, DWORD, void*, HKEY*, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
inline LONG RegSetValueEx(HKEY, LPCTSTR, DWORD, DWORD, const BYTE*, DWORD) { return ERROR_SUCCESS; }
inline LONG RegDeleteKey(HKEY, LPCTSTR) { return ERROR_SUCCESS; }
inline LONG RegDeleteValue(HKEY, LPCTSTR) { return ERROR_SUCCESS; }

#endif // !_WIN32
