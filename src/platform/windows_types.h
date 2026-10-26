// platform/windows_types.h
// Basic Windows type aliases for non-Windows platforms.
// On Windows this header is never included (stdafx.h pulls the real windows.h).

#pragma once

#ifndef _WIN32

#include <stdint.h>
#include <stddef.h>
#include <cassert>
#include <cstring>
#include <string>    // mc_tcsftime's %# rewrite

// ── Integer types ──────────────────────────────────────────────────────────

typedef int                BOOL;
typedef unsigned int       UINT;
typedef uint32_t           DWORD;   // always 32-bit, matching Windows
typedef uint32_t           ULONG;
typedef int32_t            LONG;    // always 32-bit, matching Windows
typedef unsigned short     WORD;
typedef unsigned char      BYTE;
typedef int                INT;
typedef short              SHORT;
typedef unsigned short     USHORT;

typedef int64_t            LONGLONG;
typedef uint64_t           ULONGLONG;
typedef int64_t            __int64;       // MSVC extension used in source
typedef uint64_t           UINT64;

typedef void *             LPVOID;
typedef const void *       LPCVOID;
typedef LONG *             PLONG;

// ── COM / dispatch types ────────────────────────────────────────────────────
typedef void *  LPDISPATCH;
typedef char *  BSTR;           // Windows wide-char string; MB build treats as char*
typedef BSTR *  LPBSTR;
typedef void *  LPUNKNOWN;
typedef void *  IUnknown;

// ── Path and size constants ────────────────────────────────────────────────
#ifndef _MAX_PATH
#  define _MAX_PATH   260
#  define _MAX_DRIVE    3
#  define _MAX_DIR    256
#  define _MAX_FNAME  256
#  define _MAX_EXT    256
#  define MAX_PATH    260
#endif

// ── Wide-char alias ────────────────────────────────────────────────────────
typedef wchar_t WCHAR;

// ── Character types ────────────────────────────────────────────────────────

// MUSHclient is compiled with MultiByte character set (not Unicode/wide),
// so TCHAR == char throughout.
typedef char               TCHAR;
typedef char *             LPTSTR;
typedef const char *       LPCTSTR;
typedef char *             LPSTR;
typedef const char *       LPCSTR;
typedef wchar_t *          LPWSTR;
typedef const wchar_t *    LPCWSTR;

// TCHAR string function aliases — map Windows _tcs* names to POSIX equivalents.
#include <time.h>
#ifndef _tcsftime
// NOT plain strftime (2026-08-12): MSVC's strftime understands the %# flag
// (no leading zero); POSIX spells that %-.  A bare alias fed %# through raw,
// so FormatTime — used by the Activity window's Since column, log-file
// preambles/postambles and auto-log filenames — printed "#I:58 PM" instead
// of "2:58 PM" for any Windows-authored format.  Same rewrite CTime::Format
// (platform/mfc_time.h) already does; FormatTime bypasses that shim.
inline size_t mc_tcsftime(char* out, size_t maxsize, const char* fmt,
                          const struct tm* tmv)
{
    std::string cleanFmt;
    if (fmt) {
        cleanFmt.reserve(strlen(fmt) + 1);
        for (const char* p = fmt; *p; ++p) {
            if (p[0] == '%' && p[1] == '#') { cleanFmt += "%-"; ++p; }
            else                            cleanFmt += *p;
        }
    }
    return strftime(out, maxsize, cleanFmt.c_str(), tmv);
}
#  define _tcsftime mc_tcsftime
#endif
#ifndef _tcscpy
#  define _tcscpy   strcpy
#endif
#ifndef _tcsncpy
#  define _tcsncpy  strncpy
#endif
#ifndef _tcslen
#  define _tcslen   strlen
#endif
#ifndef _tcscmp
#  define _tcscmp   strcmp
#endif
#ifndef _tcsicmp
#  define _tcsicmp  strcasecmp
#endif

// ── Boolean constants ──────────────────────────────────────────────────────

#ifndef TRUE
#  define TRUE  1
#endif
#ifndef FALSE
#  define FALSE 0
#endif

// ── Colour type ────────────────────────────────────────────────────────────
// COLORREF encodes RGB as 0x00BBGGRR (Windows byte order).

typedef uint32_t COLORREF;

// constexpr, like Windows' macro: RGB (...) in a table initialiser is then a
// constant, not a narrowing conversion (the option tables store it as double).
constexpr COLORREF RGB(BYTE r, BYTE g, BYTE b)
{
    return (COLORREF)(((BYTE)(r)) | ((DWORD)((BYTE)(g)) << 8) | ((DWORD)((BYTE)(b)) << 16));
}
inline BYTE GetRValue(COLORREF rgb) { return (BYTE)(rgb & 0xFF); }
inline BYTE GetGValue(COLORREF rgb) { return (BYTE)((rgb >> 8) & 0xFF); }
inline BYTE GetBValue(COLORREF rgb) { return (BYTE)((rgb >> 16) & 0xFF); }

// ── Byte-extraction macros ─────────────────────────────────────────────────
#ifndef LOBYTE
#  define LOBYTE(w) ((BYTE)((DWORD)(w) & 0xFF))
#  define HIBYTE(w) ((BYTE)(((DWORD)(w) >> 8) & 0xFF))
#endif

// Note: BLACK and WHITE are NOT defined here as macros; they are defined
// as ANSI colour-index enum values (0 and 7) in stdafx.h.

// ── LARGE_INTEGER ──────────────────────────────────────────────────────────

union LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
};

// ── Handle types (stubs) ───────────────────────────────────────────────────

typedef void *  HANDLE;
// ── Pointer-size integer types ────────────────────────────────────────────
typedef uintptr_t  UINT_PTR;
typedef intptr_t   INT_PTR;
typedef uintptr_t  DWORD_PTR;
typedef uintptr_t  ULONG_PTR;
typedef intptr_t   LONG_PTR;

typedef void *  HWND;
typedef void *  HINSTANCE;
typedef void *  HMODULE;
typedef void *  HFONT;
typedef void *  HICON;
typedef void *  HCURSOR;
typedef void *  HBITMAP;
typedef void *  HPEN;
typedef void *  HBRUSH;
typedef UINT_PTR HMENU;  // integer handle — allows (UINT) cast without -fpermissive
typedef void *  HDC;
typedef void *  HGDIOBJ;
typedef void *  HRGN;
typedef void *  HTREEITEM;
typedef void *  HACCEL;
typedef void *  HGLOBAL;
typedef void *  HLOCAL;
typedef UINT_PTR WPARAM;   // pointer-sized, matching Windows WPARAM
typedef LONG_PTR LPARAM;   // pointer-sized, matching Windows LPARAM
typedef LONG_PTR LRESULT;  // pointer-sized, matching Windows LRESULT

#define INVALID_HANDLE_VALUE  ((HANDLE)(LONGLONG)-1)

// ── Memory helpers ────────────────────────────────────────────────────────
// The (void *) casts are GCC's documented opt-out of -Wclass-memaccess: the
// shim gives several Windows structs default member initialisers, which makes
// them "non-trivial" to GCC, though they are byte-filled exactly as on Windows.
#define ZeroMemory(ptr, size)  memset((void *)(ptr), 0, (size))
#define CopyMemory(dst, src, n) memcpy((void *)(dst), (src), (n))
#define FillMemory(ptr, n, v)   memset((void *)(ptr), (v), (n))

// ── MessageBox flags ──────────────────────────────────────────────────────

#define MB_OK                0x00000000L
#define MB_OKCANCEL          0x00000001L
#define MB_ABORTRETRYIGNORE  0x00000002L
#define MB_YESNOCANCEL       0x00000003L
#define MB_YESNO             0x00000004L
#define MB_RETRYCANCEL       0x00000005L
#define MB_ICONERROR         0x00000010L
#define MB_ICONWARNING       0x00000030L
#define MB_ICONINFORMATION   0x00000040L
#define MB_DEFBUTTON1        0x00000000L
#define MB_DEFBUTTON3        0x00000200L
#define MB_APPLMODAL         0x00000000L
#define MB_SYSTEMMODAL       0x00001000L
#define MB_TASKMODAL         0x00002000L
#define WM_APP               0x8000

// ── Win32 error codes ─────────────────────────────────────────────────────
#ifndef ERROR_FILE_NOT_FOUND
#  define ERROR_FILE_NOT_FOUND    2L
#  define ERROR_PATH_NOT_FOUND    3L
#  define ERROR_BAD_FORMAT        11L
#endif

// ── ShellExecute error codes ──────────────────────────────────────────────
#define SE_ERR_FNF            2   // file not found
#define SE_ERR_PNF            3   // path not found
#define SE_ERR_ACCESSDENIED   5
#define SE_ERR_OOM            8   // out of memory
#define SE_ERR_DLLNOTFOUND    32
#define SE_ERR_SHARE          26
#define SE_ERR_ASSOCINCOMPLETE 27
#define SE_ERR_DDETIMEOUT     28
#define SE_ERR_DDEFAIL        29
#define SE_ERR_DDEBUSY        30
#define SE_ERR_NOASSOC        31

// ── snprintf alias ────────────────────────────────────────────────────────
#ifndef _snprintf
#  define _snprintf snprintf
#endif

// ── COM / dispatch stubs ───────────────────────────────────────────────────
// COM is Windows-only; we stub just enough for the type-checker to be happy.

typedef LONG    HRESULT;
typedef LONG    DISPID;

#define DISPID_UNKNOWN  (-1)
#define S_OK            ((HRESULT)0L)
#define S_FALSE         ((HRESULT)1L)
#define E_FAIL          ((HRESULT)0x80004005L)
#define E_INVALIDARG    ((HRESULT)0x80070057L)
#define E_NOINTERFACE   ((HRESULT)0x80004002L)
#define E_NOTIMPL       ((HRESULT)0x80004001L)
#define E_OUTOFMEMORY   ((HRESULT)0x8007000EL)
#define E_UNEXPECTED    ((HRESULT)0x8000FFFFL)
#define FAILED(hr)      (((HRESULT)(hr)) < 0)
#define SUCCEEDED(hr)   (((HRESULT)(hr)) >= 0)

// ── Pointer types ─────────────────────────────────────────────────────────

typedef UINT *  PUINT;
typedef DWORD * PDWORD;
typedef BOOL *  PBOOL;
typedef LONG *  PLONG;
typedef void *  PVOID;
typedef LONG *  LPLONG;
typedef DWORD * LPDWORD;
typedef WORD *  LPWORD;
typedef int *   LPINT;
typedef BYTE *  LPBYTE;
typedef BYTE *  PBYTE;

// RECT pointer aliases
struct RECT; // forward
typedef RECT *        LPRECT;
typedef const RECT *  LPCRECT;

// POINT / SIZE pointer aliases
struct POINT;
typedef POINT *       LPPOINT;
struct SIZE;
typedef SIZE *        LPSIZE;

// NMHDR — common header for WM_NOTIFY
#ifndef NMHDR
struct NMHDR {
    HWND  hwndFrom = nullptr;
    UINT  idFrom   = 0;
    UINT  code     = 0;
};
#define NMHDR NMHDR
#endif

// SIZE_T — matches platform's size_t
typedef size_t SIZE_T;
typedef ptrdiff_t SSIZE_T;

// Clipboard / drag-drop types
typedef WORD   CLIPFORMAT;
typedef DWORD  DROPEFFECT;

// ── String helpers ─────────────────────────────────────────────────────────
// lstrcpyn — safe string copy (like strncpy but always NUL-terminates)
#ifndef lstrcpyn
inline LPTSTR lstrcpyn(LPTSTR dst, LPCTSTR src, int n) {
    if (n > 0) { strncpy(dst, src, n - 1); dst[n - 1] = '\0'; }
    return dst;
}
#endif

// AfxIsValidString — stub: any non-null pointer is "valid"
#ifndef AfxIsValidString
inline BOOL AfxIsValidString(LPCTSTR /*psz*/, int /*nLength*/ = -1) { return TRUE; }
#endif

// TRACE0-TRACE3 macros.  Compiled out, but the arguments are named inside an
// unevaluated sizeof: a variable whose only reader is a TRACE (as on Windows,
// where these print in debug builds) is then not "set but never used".
#ifndef TRACE0
#  define TRACE0(s)             ((void)0)
#endif
#ifndef TRACE1
#  define TRACE1(fmt, a)        ((void)sizeof(a))
#endif
#ifndef TRACE2
#  define TRACE2(fmt, a, b)     ((void)sizeof(a), (void)sizeof(b))
#endif
#ifndef TRACE3
#  define TRACE3(fmt, a, b, c)  ((void)sizeof(a), (void)sizeof(b), (void)sizeof(c))
#endif

// _stricmp / _strnicmp — MSVC names for POSIX strcasecmp / strncasecmp
#ifndef _stricmp
#  define _stricmp   strcasecmp
#  define _strnicmp  strncasecmp
#  define _strtoi64  strtoll
#endif

// wsprintf — Windows documents a 1,024-byte maximum output, so honour that
// bound rather than aliasing sprintf (which macOS also deprecates).
#ifndef wsprintf
#include <cstdarg>
#include <cstdio>
inline int wsprintf(char* buf, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, 1024, fmt, ap);
    va_end(ap);
    return (n < 0 || n < 1024) ? n : 1023;   // characters stored, as Windows reports
}
#endif

// process.h equivalents
#ifndef _getpid
#  include <unistd.h>
#  define _getpid getpid
#endif

// AfxThrowArchiveException: MFC throws this on a bad serialize/schema, and the
// callers (LoadPlugin, Serialize, the XML import paths) all "catch
// (CArchiveException*)".  So we MUST throw a CArchiveException* — but that class
// is not defined this early, so forward-declare the thrower here and define it
// in mfc_ui.h (after CArchiveException).  Throwing the wrong type (a
// std::runtime_error) slips past those catch handlers and terminates the app —
// e.g. when loading an unsupported VBScript plugin.
#ifndef AfxThrowArchiveException
// BOTH builds: forward-declare here, real inline thrower in mfc_ui.h (after
// CArchiveException).  The headless build used to throw a std::runtime_error
// placeholder, but with the REAL loader (xml/xml_load_world.cpp) in mushcore
// the catch (CArchiveException*) handlers (LoadPlugin, ImportXML, Load_Set,
// LoadWorldFromFile) must actually catch it — mfc_ui.h is included by every
// mushcore TU via compat.h, so the real thrower is always available.
void AfxThrowArchiveException(int cause, LPCTSTR name = nullptr);
#endif

// ── Windows geometry structs ───────────────────────────────────────────────
struct RECT  { LONG left, top, right, bottom; };
struct POINT { LONG x, y; };
struct SIZE  { LONG cx, cy; };

// ── Misc Windows macros ────────────────────────────────────────────────────

#ifndef ASSERT
#  define ASSERT(expr)  assert(expr)
#endif

#ifndef VERIFY
#  define VERIFY(expr)  ((void)(expr))
#endif

#ifndef TRACE
// As TRACE0-3 above: compiled out, arguments named in an unevaluated sizeof.
namespace mc_trace_detail { template <class... T> int names (const T&...); }
#  define TRACE(...)    ((void)sizeof(::mc_trace_detail::names(__VA_ARGS__)))
#endif

// BASED_CODE is a 16-bit Windows artefact; ignore it
#define BASED_CODE

// AFX_MANAGE_STATE is an MFC macro for managing module state; no-op here
#define AFX_MANAGE_STATE(x)

// SYSTEMTIME — Windows calendar/clock structure
struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay;
    WORD wHour, wMinute, wSecond, wMilliseconds;
};

// SAFEARRAY — COM automation safe array stub
struct SAFEARRAYBOUND { ULONG cElements; LONG lLbound; };
struct SAFEARRAY {
    USHORT cDims = 0;
    USHORT fFeatures = 0;
    ULONG  cbElements = 0;
    ULONG  cLocks = 0;
    PVOID  pvData = nullptr;
    SAFEARRAYBOUND rgsabound[1];
};
// SafeArray functions.  In the full (mushapp/GUI) build these have REAL
// implementations in platform/mfc_ui.h — they need the VARIANT layout, which is
// only defined there — so here we just forward-declare the one VariantClear()
// needs (the rest are defined and used entirely after mfc_ui.h is included).
// In the headless (mushcore) build there is no VARIANT array support, so the
// no-op stubs below stand in (overloaded for LONG (int32_t) and long).
HRESULT SafeArrayDestroy(SAFEARRAY*);

// SysFreeString / SysAllocString — COM BSTR helpers
#ifndef BSTR
typedef char* BSTR;
#endif
inline void   SysFreeString(BSTR p) { delete[] p; }
inline BSTR   SysAllocString(const char* s) {
    if (!s) return nullptr;
    size_t n = strlen(s)+1;
    char* p = new char[n];
    memcpy(p, s, n);
    return p;
}

// GetModuleFileNameA stub
inline DWORD GetModuleFileNameA(HMODULE, char* buf, DWORD n) { if(buf&&n)buf[0]=0; return 0; }

// WM_USER message offset (same value as Windows)
#define WM_USER 0x0400

// _itoa — MSVC CRT helper; not in POSIX, implement via snprintf
#include <cstdio>
inline char* _itoa(int value, char* str, int radix) {
    if      (radix == 16) snprintf(str, 34, "%x", (unsigned)value);
    else if (radix ==  8) snprintf(str, 34, "%o", (unsigned)value);
    else                  snprintf(str, 34, "%d", value);
    return str;
}

#endif // !_WIN32
