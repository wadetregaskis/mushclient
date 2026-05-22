// platform/cderr.h — stub for Windows <cderr.h>
// Common dialog error codes; MUSHclient references these in GlobalPrefs.
#pragma once
#ifndef _WIN32
#define CDERR_DIALOGFAILURE   0xFFFF
#define CDERR_GENERALCODES    0x0000
#define CDERR_STRUCTSIZE      0x0001
#define CDERR_INITIALIZATION  0x0002
#define CDERR_NOTEMPLATE      0x0003
#define CDERR_NOHINSTANCE     0x0004
#define CDERR_LOADSTRFAILURE  0x0005
#define CDERR_FINDRESFAILURE  0x0006
#define CDERR_LOADRESFAILURE  0x0007
#define CDERR_LOCKRESFAILURE  0x0008
#define CDERR_MEMALLOCFAILURE 0x0009
#define CDERR_MEMLOCKFAILURE  0x000A
#define CDERR_NOHOOK          0x000B
#define CDERR_REGISTERMSGFAIL 0x000C
inline DWORD CommDlgExtendedError() { return 0; }
#endif // !_WIN32
