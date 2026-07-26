// platform/mfc_ui.h
// MFC GUI compatibility stubs for macOS / Linux builds.
//
// Provides enough of MFC's GUI surface (CDocument, CView, CDialog, CDC,
// frame classes, message-map macros, etc.) that the existing Windows source
// files (doc.cpp, MUSHview.cpp, dialogs/*.cpp, …) compile unchanged on
// non-Windows platforms.  None of these stubs produce actual UI; the real
// wxWidgets wiring lives in platform/wx_*.  The goal here is compilation
// correctness with minimal churn in the shared source files.
//
// Include order in compat.h:
//   windows_types.h  ← primitive types (already included before this file)
//   mfc_object.h     ← CObject, DECLARE_DYNAMIC (already included)
//   mfc_string.h     ← CString (already included)
//   mfc_collections.h← POSITION, CTypedPtrList/Map/Array (already included)
//   mfc_ui.h         ← this file

#pragma once

#ifndef _WIN32

// Marks mfc_ui.h as the authoritative provider of these MFC-compat types.
#define MFC_UI_H_INCLUDED

#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <cstring>
#include <cstdio>
#include <cerrno>     // errno — CFile::Open failure cause (CFileException)
#include <algorithm>
#include <unistd.h>   // readlink, ssize_t, realpath
#include <stdlib.h>   // realpath, free()
#include <sys/stat.h> // stat() — used by CFile::GetStatus
#include <time.h>     // clock_gettime / nanosleep — GetTickCount / timeGetTime / Sleep
#if defined(__APPLE__)
#  include <mach-o/dyld.h>  // _NSGetExecutablePath
#endif

// wx hook function pointers — must be included after stdlib but before any
// class definitions that call through the hooks (e.g. CFileDialog::DoModal).
// wx_mfc_hooks.h is self-contained: no wx or MFC includes.
#include "platform/wx_mfc_hooks.h"

// ── Mixed-type min / max for legacy MSVC-style calls ─────────────────────
// std::min / std::max require identical types; these templates handle the
// common case of comparing int vs long (or double vs int) in the source.
#ifndef _MSC_VER
template<class A, class B>
inline auto min(A a, B b) -> decltype(a < b ? a : b) { return a < b ? a : b; }
template<class A, class B>
inline auto max(A a, B b) -> decltype(a > b ? a : b) { return a > b ? a : b; }
#endif

// ── afx_msg / AFXAPI / CALLBACK decorators ────────────────────────────────
// In MFC / Win32 these trigger special compiler or calling-convention
// processing.  On macOS / Linux they expand to nothing.
#define afx_msg    /* nothing */
#define AFXAPI     /* nothing */
#define CALLBACK   /* nothing */
#define WINAPI     /* nothing */
#define APIENTRY   /* nothing */
#define PASCAL     /* nothing */
// Calling convention specifiers — x86-only, no-op on ARM64/Linux
#ifndef __stdcall
#  define __stdcall  /* nothing */
#endif
#ifndef __cdecl
#  define __cdecl    /* nothing */
#endif
#ifndef __fastcall
#  define __fastcall /* nothing */
#endif
#define CDECL      /* nothing */

// HELP_CONTEXT — WinHelp command; unused on non-Windows
#ifndef HELP_CONTEXT
#  define HELP_CONTEXT 0x0001
#endif

// ── Additional handle types not in windows_types.h ────────────────────────
#ifndef HGDIOBJ
typedef void*  HGDIOBJ;
#endif

// ── Error sentinels used by CListBox / CComboBox ─────────────────────────
#ifndef LB_ERR
#  define LB_ERR     (-1)
#  define LB_ERRSPACE (-2)
#endif
#ifndef CB_ERR
#  define CB_ERR     (-1)
#  define CB_ERRSPACE (-2)
#endif

// IDC_STATIC — standard ID for static controls (-1 in Windows)
#ifndef IDC_STATIC
#  define IDC_STATIC  (-1)
#endif

// ── MB_* / IDOK / IDCANCEL / etc. — already in windows_types.h ───────────
// Add only the ones that may be missing:
#ifndef IDOK
#  define IDOK      1
#  define IDCANCEL  2
#  define IDABORT   3
#  define IDRETRY   4
#  define IDIGNORE  5
#  define IDYES     6
#  define IDNO      7
#endif
#ifndef MB_DEFBUTTON2
#  define MB_DEFBUTTON2     0x00000100L
#  define MB_APPLMODAL      0x00000000L
#  define MB_ICONEXCLAMATION MB_ICONWARNING
#  define MB_ICONQUESTION   0x00000020L
#  define MB_SETFOREGROUND  0x00010000L
#endif

// ── LOWORD / HIWORD / MAKELONG ─────────────────────────────────────────────
#ifndef LOWORD
#  define LOWORD(l)  ((WORD)((DWORD_PTR)(l) & 0xFFFF))
#  define HIWORD(l)  ((WORD)(((DWORD_PTR)(l) >> 16) & 0xFFFF))
#  define MAKELONG(lo,hi) ((DWORD)(((WORD)(lo)) | ((DWORD)((WORD)(hi))) << 16))
#  define MAKEWPARAM(lo,hi) ((WPARAM)MAKELONG(lo,hi))
#  define MAKELPARAM(lo,hi) ((LPARAM)MAKELONG(lo,hi))
#endif

// ── WM_* message identifiers (a subset) ────────────────────────────────────
#ifndef WM_CREATE
#  define WM_CREATE       0x0001
#  define WM_DESTROY      0x0002
#  define WM_MOVE         0x0003
#  define WM_SIZE         0x0005
#  define WM_SETFOCUS     0x0007
#  define WM_KILLFOCUS    0x0008
#  define WM_PAINT        0x000F
#  define WM_CLOSE        0x0010
#  define WM_TIMER        0x0113
#  define WM_HSCROLL      0x0114
#  define WM_VSCROLL      0x0115
#  define WM_MOUSEMOVE    0x0200
#  define WM_LBUTTONDOWN  0x0201
#  define WM_LBUTTONUP    0x0202
#  define WM_RBUTTONDOWN  0x0204
#  define WM_RBUTTONUP    0x0205
#  define WM_MBUTTONDOWN  0x0207
#  define WM_MBUTTONUP    0x0208
#  define WM_MOUSEWHEEL   0x020A
#  define WM_KEYDOWN      0x0100
#  define WM_KEYUP        0x0101
#  define WM_CHAR         0x0102
#  define WM_CONTEXTMENU  0x007B
#  define WM_COMMAND      0x0111
#  define WM_NOTIFY       0x004E
#  define WM_INITDIALOG   0x0110
#  define WM_DROPFILES    0x0233
#  define WM_ERASEBKGND   0x0014
#  define WM_NCPAINT      0x0085
#  define WM_NCLBUTTONDOWN 0x00A1
#  define WM_GETDLGCODE   0x0087
#  define WM_INITMENUPOPUP 0x0117
#  define WM_SETTINGCHANGE 0x001A
#  define WM_COPYDATA     0x004A
#  define WM_MENUCOMMAND  0x0126
#  define WM_ACTIVATE     0x0006
#  define WM_SYSCOMMAND   0x0112
#  define WM_GETMINMAXINFO 0x0024
#  define WM_SHOWWINDOW   0x0018
#  define WM_WINDOWPOSCHANGED 0x0047
#  define SC_CLOSE        0xF060
#  define SC_KEYMENU      0xF100
#  define WM_SYSCHAR      0x0106
#  define WM_SYSKEYDOWN   0x0104
#  define WM_SYSKEYUP     0x0105
#  define WM_MOUSELEAVE   0x02A3
#  define WM_SETTEXT      0x000C
#endif

// Window styles (subset)
#ifndef WS_CHILD
#  define WS_CHILD        0x40000000L
#  define WS_VSCROLL      0x00200000L
#  define WS_HSCROLL      0x00100000L
#  define WS_BORDER       0x00800000L
#  define WS_POPUP        0x80000000L
#  define WS_VISIBLE      0x10000000L
#  define WS_DISABLED     0x08000000L
#  define WS_CLIPSIBLINGS 0x04000000L
#  define WS_CLIPCHILDREN 0x02000000L
#  define FWS_ADDTOTITLE  0x8000L
#endif

// Edit control styles
#ifndef ES_LEFT
#  define ES_LEFT         0x0000L
#  define ES_CENTER       0x0001L
#  define ES_RIGHT        0x0002L
#  define ES_MULTILINE    0x0004L
#  define ES_UPPERCASE    0x0008L
#  define ES_LOWERCASE    0x0010L
#  define ES_PASSWORD     0x0020L
#  define ES_AUTOVSCROLL  0x0040L
#  define ES_AUTOHSCROLL  0x0080L
#  define ES_NOHIDESEL    0x0100L
#  define ES_OEMCONVERT   0x0400L
#  define ES_READONLY     0x0800L
#  define ES_WANTRETURN   0x1000L
#  define ES_NUMBER       0x2000L
#endif

// AFX default view style
#ifndef AFX_WS_DEFAULT_VIEW
#  define AFX_WS_DEFAULT_VIEW (WS_CHILD | WS_VISIBLE | WS_BORDER)
#endif

// Virtual key codes
#ifndef VK_SHIFT
#  define VK_BACK    0x08
#  define VK_TAB     0x09
#  define VK_RETURN  0x0D
#  define VK_SHIFT   0x10
#  define VK_CONTROL 0x11
#  define VK_MENU    0x12
#  define VK_ESCAPE  0x1B
#  define VK_PRIOR   0x21
#  define VK_NEXT    0x22
#  define VK_END     0x23
#  define VK_HOME    0x24
#  define VK_LEFT    0x25
#  define VK_UP      0x26
#  define VK_RIGHT   0x27
#  define VK_DOWN    0x28
#  define VK_INSERT  0x2D
#  define VK_DELETE  0x2E
#  define VK_F1      0x70
#  define VK_F2      0x71
#  define VK_F3      0x72
#  define VK_F4      0x73
#  define VK_F5      0x74
#  define VK_F6      0x75
#  define VK_F7      0x76
#  define VK_F8      0x77
#  define VK_F9      0x78
#  define VK_F10     0x79
#  define VK_F11     0x7A
#  define VK_F12     0x7B
#  define VK_NUMPAD0 0x60
#  define VK_SPACE   0x20
// Extended numpad keys
#  define VK_NUMPAD1    0x61
#  define VK_NUMPAD2    0x62
#  define VK_NUMPAD3    0x63
#  define VK_NUMPAD4    0x64
#  define VK_NUMPAD5    0x65
#  define VK_NUMPAD6    0x66
#  define VK_NUMPAD7    0x67
#  define VK_NUMPAD8    0x68
#  define VK_NUMPAD9    0x69
#  define VK_MULTIPLY   0x6A
#  define VK_ADD        0x6B
#  define VK_SEPARATOR  0x6C
#  define VK_SUBTRACT   0x6D
#  define VK_DECIMAL    0x6E
#  define VK_DIVIDE     0x6F
// Extended function keys F13-F24
#  define VK_F13    0x7C
#  define VK_F14    0x7D
#  define VK_F15    0x7E
#  define VK_F16    0x7F
#  define VK_F17    0x80
#  define VK_F18    0x81
#  define VK_F19    0x82
#  define VK_F20    0x83
#  define VK_F21    0x84
#  define VK_F22    0x85
#  define VK_F23    0x86
#  define VK_F24    0x87
// Mouse button VK codes
#  define VK_LBUTTON    0x01
#  define VK_RBUTTON    0x02
#  define VK_MBUTTON    0x04
// Modifier left/right variants
#  define VK_LSHIFT     0xA0
#  define VK_RSHIFT     0xA1
#  define VK_LCONTROL   0xA2
#  define VK_RCONTROL   0xA3
#  define VK_LMENU      0xA4
#  define VK_RMENU      0xA5
#  define VK_LWIN       0x5B
#  define VK_RWIN       0x5C
#  define VK_APPS       0x5D
// Toggle keys
#  define VK_CAPITAL    0x14
#  define VK_NUMLOCK    0x90
#  define VK_SCROLL     0x91
// Misc keys
#  define VK_CANCEL     0x03
#  define VK_CLEAR      0x0C
#  define VK_PAUSE      0x13
#  define VK_SELECT     0x29
#  define VK_PRINT      0x2A
#  define VK_EXECUTE    0x2B
#  define VK_SNAPSHOT   0x2C
#  define VK_HELP       0x2F
// IME keys
#  define VK_KANA       0x15
#  define VK_HANGEUL    0x15
#  define VK_HANGUL     0x15
#  define VK_JUNJA      0x17
#  define VK_FINAL      0x18
#  define VK_HANJA      0x19
#  define VK_KANJI      0x19
#  define VK_CONVERT    0x1C
#  define VK_NONCONVERT 0x1D
#  define VK_ACCEPT     0x1E
#  define VK_MODECHANGE 0x1F
// Rarely-used
#  define VK_PROCESSKEY 0xE5
#  define VK_ATTN       0xF6
#  define VK_CRSEL      0xF7
#  define VK_EXSEL      0xF8
#  define VK_EREOF      0xF9
#  define VK_PLAY       0xFA
#  define VK_ZOOM       0xFB
#  define VK_NONAME     0xFC
#  define VK_PA1        0xFD
#  define VK_OEM_CLEAR  0xFE
#endif

// Mouse key flags
#ifndef MK_LBUTTON
#  define MK_LBUTTON  0x0001
#  define MK_RBUTTON  0x0002
#  define MK_SHIFT    0x0004
#  define MK_CONTROL  0x0008
#  define MK_MBUTTON  0x0010
#endif

// Scrollbar request codes
#ifndef SB_LINEUP
#  define SB_LINEUP          0
#  define SB_LINEDOWN        1
#  define SB_PAGEUP          2
#  define SB_PAGEDOWN        3
#  define SB_THUMBPOSITION   4
#  define SB_THUMBTRACK      5
#  define SB_TOP             6
#  define SB_BOTTOM          7
#  define SB_ENDSCROLL       8
#  define SB_SETTEXT         0x0401
#  define SBT_OWNERDRAW      0x1000
#endif

// SCROLLINFO and flags
#ifndef SIF_RANGE
#  define SIF_RANGE          0x0001
#  define SIF_PAGE           0x0002
#  define SIF_POS            0x0004
#  define SIF_DISABLENOSCROLL 0x0008
#  define SIF_TRACKPOS       0x0010
#  define SIF_ALL            (SIF_RANGE|SIF_PAGE|SIF_POS|SIF_TRACKPOS)
struct SCROLLINFO {
    UINT cbSize  = sizeof(SCROLLINFO);
    UINT fMask   = 0;
    int  nMin    = 0;
    int  nMax    = 0;
    UINT nPage   = 0;
    int  nPos    = 0;
    int  nTrackPos = 0;
    SCROLLINFO() = default;
};
#endif

// System metrics (additional)
#ifndef SM_CXFRAME
#  define SM_CXFRAME      32
#  define SM_CYFRAME      33
#  define SM_CYMENU       15
#  define SM_CXSIZEFRAME  SM_CXFRAME
#  define SM_CYSIZEFRAME  SM_CYFRAME
#endif

// Button state values
#ifndef BST_CHECKED
#  define BST_UNCHECKED     0x0000
#  define BST_CHECKED       0x0001
#  define BST_INDETERMINATE 0x0002
#  define BST_PUSHED        0x0004
#  define BST_FOCUS         0x0008
#endif

// Font dialog flags
#ifndef CF_SCREENFONTS
#  define CF_SCREENFONTS        0x00000001L
#  define CF_PRINTERFONTS       0x00000002L
#  define CF_BOTH               0x00000003L
#  define CF_EFFECTS            0x00000100L
#  define CF_INITTOLOGFONTSTRUCT 0x00000040L
#  define CF_USESTYLE           0x00000080L
#  define CF_FORCEFONTEXIST     0x00010000L
#  define CF_SCALABLEONLY       0x00020000L
#  define CF_NOVERTFONTS        0x01000000L
#endif

// CHOOSEFONT — defined after LOGFONT below (requires LOGFONT to be complete).

// Common dialog error codes
#ifndef FNERR_BUFFERTOOSMALL
#  define FNERR_SUBCLASSFAILURE 0x3001
#  define FNERR_INVALIDFILENAME 0x3002
#  define FNERR_BUFFERTOOSMALL  0x3003
#endif

// COM calling convention
#ifndef STDAPICALLTYPE
#  define STDAPICALLTYPE
#endif

// Memory status
struct MEMORYSTATUS {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    SIZE_T dwTotalPhys;
    SIZE_T dwAvailPhys;
    SIZE_T dwTotalPageFile;
    SIZE_T dwAvailPageFile;
    SIZE_T dwTotalVirtual;
    SIZE_T dwAvailVirtual;
};
inline void GlobalMemoryStatus(MEMORYSTATUS* ms) {
    if (ms) { ms->dwMemoryLoad = 0; ms->dwTotalPhys = 0; ms->dwAvailPhys = 0; }
}

// Hit-test return codes
#ifndef HTCLIENT
#  define HTCLIENT    1
#  define HTCAPTION   2
#  define HTNOWHERE   0
#endif

// Cursor ID constants
#ifndef IDC_ARROW
#  define IDC_ARROW     ((LPCTSTR)(ULONG_PTR)32512)
#  define IDC_IBEAM     ((LPCTSTR)(ULONG_PTR)32513)
#  define IDC_WAIT      ((LPCTSTR)(ULONG_PTR)32514)
#  define IDC_CROSS     ((LPCTSTR)(ULONG_PTR)32515)
#  define IDC_UPARROW   ((LPCTSTR)(ULONG_PTR)32516)
#  define IDC_SIZENWSE  ((LPCTSTR)(ULONG_PTR)32642)
#  define IDC_SIZENESW  ((LPCTSTR)(ULONG_PTR)32643)
#  define IDC_SIZEWE    ((LPCTSTR)(ULONG_PTR)32644)
#  define IDC_SIZENS    ((LPCTSTR)(ULONG_PTR)32645)
#  define IDC_SIZEALL   ((LPCTSTR)(ULONG_PTR)32646)
#  define IDC_NO        ((LPCTSTR)(ULONG_PTR)32648)
#  define IDC_HAND      ((LPCTSTR)(ULONG_PTR)32649)
#  define IDC_HELP      ((LPCTSTR)(ULONG_PTR)32651)
#endif

// Track Popup Menu flags
#ifndef TPM_LEFTALIGN
#  define TPM_LEFTALIGN     0x0000L
#  define TPM_CENTERALIGN   0x0004L
#  define TPM_RIGHTALIGN    0x0008L
#  define TPM_TOPALIGN      0x0000L
#  define TPM_VCENTERALIGN  0x0010L
#  define TPM_BOTTOMALIGN   0x0020L
#  define TPM_NONOTIFY      0x0080L
#  define TPM_RETURNCMD     0x0100L
#  define TPM_LEFTBUTTON    0x0000L
#  define TPM_RIGHTBUTTON   0x0002L
#endif

// ToolTip styles and messages
#ifndef TTS_ALWAYSTIP
#  define TTS_ALWAYSTIP   0x01
#  define TTS_NOPREFIX    0x02
#  define TTS_BALLOON     0x40
#  define TTM_SETDELAYTIME  (WM_USER + 3)
#  define TTM_SETMAXTIPWIDTH (WM_USER + 24)
#  define TTDT_AUTOMATIC    0
#  define TTDT_RESHOW       1
#  define TTDT_AUTOPOP      2
#  define TTDT_INITIAL      3
#endif

// TrackMouseEvent
#ifndef TME_LEAVE
#  define TME_LEAVE       0x00000002L
#  define TME_HOVER       0x00000001L
struct TRACKMOUSEEVENT {
    DWORD cbSize      = sizeof(TRACKMOUSEEVENT);
    DWORD dwFlags     = 0;
    HWND  hwndTrack   = nullptr;
    DWORD dwHoverTime = 0;
    TRACKMOUSEEVENT() = default;
};
inline BOOL _TrackMouseEvent(TRACKMOUSEEVENT*) { return TRUE; }
#endif

// Mouse wheel
#ifndef WHEEL_DELTA
#  define WHEEL_DELTA       120
#  define WHEEL_PAGESCROLL  ((UINT)(~0))
#  define SPI_GETWHEELSCROLLLINES 104
#endif

// GDI error sentinel
#ifndef GDI_ERROR
#  define GDI_ERROR ((DWORD)0xFFFFFFFFL)
#endif
#ifndef GGI_MARK_NONEXISTING_GLYPHS
#  define GGI_MARK_NONEXISTING_GLYPHS 0x0001
#endif

// SystemParametersInfo action codes
#ifndef SPI_GETNONCLIENTMETRICS
#  define SPI_GETNONCLIENTMETRICS  0x0029
#  define SPI_SETNONCLIENTMETRICS  0x002A
#endif
// NONCLIENTMETRICS and CHOOSEFONT require LOGFONT — defined after LOGFONT below.

// Accelerator table flags
#ifndef FVIRTKEY
#  define FVIRTKEY  0x01
#  define FSHIFT    0x04
#  define FCONTROL  0x08
#  define FALT      0x10
#endif

// Region operation codes
#ifndef RGN_AND
#  define RGN_AND      1
#  define RGN_OR       2
#  define RGN_XOR      3
#  define RGN_DIFF     4
#  define RGN_COPY     5
#  define NULLREGION   1
#  define SIMPLEREGION 2
#  define COMPLEXREGION 3
#endif

// Print dialog flags
#ifndef PD_SELECTION
#  define PD_SELECTION        0x00000001L
#  define PD_PAGENUMS         0x00000002L
#  define PD_NOSELECTION      0x00000004L
#  define PD_COLLATE          0x00000010L
#  define PD_PRINTTOFILE      0x00000020L
#  define PD_RETURNDEFAULT    0x00000400L
#  define PD_RETURNDC         0x00000100L
#endif

// Brush styles
#ifndef BS_NULL
#  define BS_NULL     1
#  define BS_SOLID    0
#  define BS_HATCHED  2
#  define BS_HOLLOW   BS_NULL
#endif

// StretchBlt modes
#ifndef HALFTONE
#  define BLACKONWHITE 1
#  define WHITEONBLACK 2
#  define COLORONCOLOR 3
#  define HALFTONE     4
#endif

// BITMAP structure
#ifndef BITMAP
struct BITMAP {
    LONG   bmType       = 0;
    LONG   bmWidth      = 0;
    LONG   bmHeight     = 0;
    LONG   bmWidthBytes = 0;
    WORD   bmPlanes     = 0;
    WORD   bmBitsPixel  = 0;
    LPVOID bmBits       = nullptr;
    BITMAP() = default;
};
#endif

// LOGBRUSH structure
#ifndef LOGBRUSH
struct LOGBRUSH {
    UINT      lbStyle = BS_SOLID;
    COLORREF  lbColor = 0;
    ULONG_PTR lbHatch = 0;
    LOGBRUSH() = default;
};
#endif

// TOOLTIPTEXT structure
#ifndef TOOLTIPTEXT
#ifndef TOOLTIPTEXT
struct TOOLTIPTEXT {
    NMHDR   hdr      = {};
    LPSTR   lpszText = nullptr;
    char    szText[80] = {};
    HINSTANCE hinst  = nullptr;
    UINT    uFlags   = 0;
    TOOLTIPTEXT() { szText[0] = 0; }
};
#define TOOLTIPTEXT TOOLTIPTEXT
#endif
#endif

// Common Win32 rect helpers
#ifndef SetRect
inline void SetRect(RECT* r, int l, int t, int ri, int b) { r->left=l; r->top=t; r->right=ri; r->bottom=b; }
inline void OffsetRect(RECT* r, int dx, int dy) { r->left+=dx; r->right+=dx; r->top+=dy; r->bottom+=dy; }
inline void InflateRect(RECT* r, int dx, int dy) { r->left-=dx; r->right+=dx; r->top-=dy; r->bottom+=dy; }
inline BOOL IntersectRect(RECT* d, const RECT* s1, const RECT* s2) {
    d->left=s1->left>s2->left?s1->left:s2->left;
    d->top=s1->top>s2->top?s1->top:s2->top;
    d->right=s1->right<s2->right?s1->right:s2->right;
    d->bottom=s1->bottom<s2->bottom?s1->bottom:s2->bottom;
    return d->left<d->right && d->top<d->bottom ? TRUE : FALSE;
}
inline BOOL IsRectEmpty(const RECT* r) { return r->left>=r->right || r->top>=r->bottom; }
inline BOOL EqualRect(const RECT* a, const RECT* b) {
    return a->left==b->left && a->top==b->top && a->right==b->right && a->bottom==b->bottom;
}
#endif

// Scrollbar info helpers (global stubs)
#ifndef GetScrollInfo
inline BOOL GetScrollInfo(HWND, int, SCROLLINFO* psi) { if(psi){psi->nPos=0;psi->nMin=0;psi->nMax=0;} return TRUE; }
inline BOOL SetScrollInfo(HWND, int, const SCROLLINFO*, BOOL) { return TRUE; }
#endif

// Other Win32 function stubs
#ifndef GetKeyState
inline SHORT GetKeyState(int vk)  { return g_wxhook_get_key_state ? (SHORT) g_wxhook_get_key_state(vk) : (SHORT) 0; }
inline HWND  GetCapture()      { return nullptr; }
inline BOOL  IsWindow(HWND h)  { return h != nullptr; }
inline BOOL  LockWindowUpdate(HWND)   { return FALSE; }
inline void  UnlockWindowUpdate()     {}
inline BOOL  SetMenuDefaultItem(HMENU, UINT, UINT) { return FALSE; }
inline BOOL  InvalidateRgn(HWND, HRGN, BOOL = TRUE) { return TRUE; }
inline BOOL  ScrollWindow(HWND, int, int, const RECT*, const RECT*) { return TRUE; }
inline BOOL  SystemParametersInfo(UINT, UINT, PVOID, UINT) { return FALSE; }
inline BOOL  GetTextExtentPoint32W(HDC, const WCHAR*, int, SIZE*) { return FALSE; }
inline BOOL  GetTextExtentPoint32W(HDC hdc, const WCHAR* s, int n, CSize* sz) { (void)hdc;(void)s;(void)n;(void)sz; return FALSE; }
inline BOOL  ExtTextOutW(HDC, int, int, UINT, const RECT*, const WCHAR*, UINT, const INT*) { return FALSE; }
inline BOOL  GetTextExtentPoint32(HDC, LPCTSTR, int, SIZE*) { return FALSE; }
#define GetKeyState GetKeyState
#define SystemParametersInfo SystemParametersInfo
#endif


// Common control notification codes
#ifndef LVN_FIRST
#  define LVN_FIRST        ((UINT)(-100))
#  define LVN_ITEMCHANGED  (LVN_FIRST-1)
#  define LVN_COLUMNCLICK  (LVN_FIRST-8)
#  define TVN_FIRST        ((UINT)(-400))
#  define TVN_SELCHANGED   (TVN_FIRST-2)
#  define TVN_SELCHANGEDW  (TVN_FIRST-51)
#  define NM_CLICK         (-2)
#  define NM_DBLCLK        (-3)
#  define NM_RETURN        (-4)
#  define NM_RCLICK        (-5)
#  define NM_RDBLCLK       (-6)
#  define NM_CUSTOMDRAW    (-12)
#  define CBN_SELCHANGE    1
#  define BN_CLICKED       0
#  define EN_CHANGE        0x0300
#  define LBN_SELCHANGE    1
#  define LBN_DBLCLK       2
#  define TVIS_EXPANDED      0x0020
#  define TVIS_SELECTED      0x0002
#  define TVGN_CHILD         0x0004
#  define TVGN_NEXT          0x0001
#  define TVGN_PARENT        0x0003
#  define TVGN_ROOT          0x0000
#  define TVI_ROOT           ((HTREEITEM)(ULONG_PTR)(ULONG_MAX - 0x0000FFFF))
#  define TVI_FIRST          ((HTREEITEM)(ULONG_PTR)(ULONG_MAX - 0x0000FFFE))
#  define TVI_LAST           ((HTREEITEM)(ULONG_PTR)(ULONG_MAX - 0x0000FFFD))
#  define TVI_SORT           ((HTREEITEM)(ULONG_PTR)(ULONG_MAX - 0x0000FFFC))
#  define TVS_HASLINES       0x0001
#  define TVS_LINESATROOT    0x0002
#  define TVS_HASBUTTONS     0x0004
#  define TVS_SHOWSELALWAYS  0x0020
#  define TVS_EDITLABELS     0x0008
#  define WC_TREEVIEW        "SysTreeView32"
#  define TVINSERTSTRUCT_PADDING 0  // placeholder
// TreeView sort callback
typedef int (CALLBACK *PFNTVCOMPARE)(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort);
struct TVSORTCB { HTREEITEM hParent; PFNTVCOMPARE lpfnCompare; LPARAM lParam; };
// Button style constants
#  define BS_CHECKBOX        0x0002L
#  define BS_AUTOCHECKBOX    0x0003L
#  define BS_RADIOBUTTON     0x0004L
#  define BS_AUTORADIOBUTTON 0x0009L
#  define BS_PUSHBUTTON      0x0000L
#  define BS_DEFPUSHBUTTON   0x0001L
#  define BS_NOTIFY          0x4000L
// TreeView additional constants
#  define TVGN_CARET         0x0009
#  define TVC_BYKEYBOARD     0x0002
#  define TVC_BYMOUSE        0x0001
// Property sheet PSP flags
#  define PSP_PREMATURE      0x0400
// ListView
#  define LVNI_FOCUSED       0x0001
#  define LVNI_SELECTED_FLAG 0x0002
// RichEdit char format
#  define CFE_STRIKEOUT      0x0008
#  ifndef CFM_STRIKEOUT
#  define CFM_STRIKEOUT      0x0008
#  endif
// Misc window messages / IDs
#  define WM_KICKIDLE        0x036A
#  define ID_APPLY_NOW       0x3021
#endif

// ShowWindow flags
#ifndef SW_SHOW
#  define SW_HIDE            0
#  define SW_SHOWNORMAL      1
#  define SW_NORMAL          1
#  define SW_SHOWMINIMIZED   2
#  define SW_SHOWMAXIMIZED   3
#  define SW_MAXIMIZE        3
#  define SW_SHOW            5
#  define SW_MINIMIZE        6
#  define SW_RESTORE         9
#  define SW_SHOWNOACTIVATE  4
#endif

// List-view extended styles
#ifndef LVM_SETEXTENDEDLISTVIEWSTYLE
#  define LVM_SETEXTENDEDLISTVIEWSTYLE  (0x1000 + 54)
#  define LVM_GETEXTENDEDLISTVIEWSTYLE  (0x1000 + 55)
#  define LVS_EX_FULLROWSELECT          0x00000020
#  define LVS_EX_GRIDLINES              0x00000001
#  define LVIR_BOUNDS                   0
#endif

// Custom-draw codes (NM_CUSTOMDRAW)
#ifndef CDDS_PREPAINT
#  define CDDS_PREPAINT           0x00000001
#  define CDDS_POSTPAINT          0x00000002
#  define CDDS_ITEMPREPAINT       0x00010001
#  define CDDS_SUBITEM            0x00020000
#  define CDRF_DODEFAULT          0x00000000
#  define CDRF_NEWFONT            0x00000002
#  define CDRF_SKIPDEFAULT        0x00000004
#  define CDRF_NOTIFYITEMDRAW     0x00000020
#  define CDRF_NOTIFYSUBITEMDRAW  0x00000040
#endif

// Hotkey control flags
#ifndef HOTKEYF_SHIFT
#  define HOTKEYF_SHIFT   0x01
#  define HOTKEYF_CONTROL 0x02
#  define HOTKEYF_ALT     0x04
#endif

// Color-choose dialog flags
#ifndef CC_RGBINIT
#  define CC_RGBINIT   0x00000001
#  define CC_FULLOPEN  0x00000002
#endif

// LoadImage / resource flags
#ifndef IMAGE_BITMAP
#  define IMAGE_BITMAP          0
#  define IMAGE_ICON            1
#  define IMAGE_CURSOR          2
#  define LR_LOADFROMFILE       0x0010
#  define LR_CREATEDIBSECTION   0x2000
#  define LR_DEFAULTSIZE        0x0040
#endif

// Font weight constants
#ifndef FW_DONTCARE
#  define FW_DONTCARE   0
#  define FW_THIN       100
#  define FW_NORMAL     400
#  define FW_BOLD       700
#  define FW_HEAVY      900
#endif

// Non-client mouse messages
#ifndef WM_NCRBUTTONDOWN
#  define WM_NCRBUTTONDOWN  0x00A4
#  define WM_NCRBUTTONUP    0x00A5
#  define WM_NCMBUTTONDOWN  0x00A7
#  define WM_NCMBUTTONUP    0x00A8
#endif

// MB_ICONSTOP (alias)
#ifndef MB_ICONSTOP
#  define MB_ICONSTOP MB_ICONERROR
#endif

// PeekMessage flag
#ifndef PM_REMOVE
#  define PM_REMOVE  0x0001
#  define PM_NOREMOVE 0x0000
#endif

// Raster capabilities
#ifndef RASTERCAPS
#  define RASTERCAPS 38
#  define RC_PALETTE 0x0100
#endif

// MFC help base IDs
#ifndef HID_BASE_COMMAND
#  define HID_BASE_COMMAND  0x10000
#  define HID_BASE_RESOURCE 0x20000
#endif

// ── Standard MFC command IDs ─────────────────────────────────────────────────
// These are defined in afxres.h on Windows; provide them here for non-Windows.
#ifndef ID_FILE_NEW
#  define ID_FILE_NEW           0xE100
#  define ID_FILE_OPEN          0xE101
#  define ID_FILE_CLOSE         0xE102
#  define ID_FILE_SAVE          0xE103
#  define ID_FILE_SAVE_AS       0xE104
#  define ID_FILE_PAGE_SETUP    0xE105
#  define ID_FILE_PRINT_SETUP   0xE106
#  define ID_FILE_PRINT         0xE107
#  define ID_FILE_PRINT_PREVIEW 0xE108
#  define ID_FILE_PRINT_DIRECT  0xE109
#  define ID_FILE_SEND_MAIL     0xE10A
#  define ID_FILE_MRU_FIRST     0xE110
#  define ID_FILE_MRU_LAST      0xE11F
#endif
#ifndef ID_EDIT_CLEAR
#  define ID_EDIT_CLEAR         0xE120
#  define ID_EDIT_CLEAR_ALL     0xE121
#  define ID_EDIT_COPY          0xE122
#  define ID_EDIT_CUT           0xE123
#  define ID_EDIT_FIND          0xE124
#  define ID_EDIT_PASTE         0xE125
#  define ID_EDIT_PASTE_LINK    0xE126
#  define ID_EDIT_PASTE_SPECIAL 0xE127
#  define ID_EDIT_REPEAT        0xE128
#  define ID_EDIT_REPLACE       0xE129
#  define ID_EDIT_SELECT_ALL    0xE12A
#  define ID_EDIT_UNDO          0xE12B
#  define ID_EDIT_REDO          0xE12C
#endif
#ifndef ID_APP_ABOUT
#  define ID_APP_ABOUT          0xE140
#  define ID_APP_EXIT           0xE141
#  define ID_DEFAULT_HELP       0xE142
#  define ID_HELP               0xE143
#  define ID_HELP_INDEX         0xE144
#  define ID_HELP_USING         0xE145
#  define ID_CONTEXT_HELP       0xE146
#endif
#ifndef ID_VIEW_TOOLBAR
#  define ID_VIEW_TOOLBAR       0xE800
#  define ID_VIEW_STATUS_BAR    0xE801
#endif
#ifndef ID_WINDOW_NEW
#  define ID_WINDOW_NEW         0xE130
#  define ID_WINDOW_ARRANGE     0xE131
#  define ID_WINDOW_CASCADE     0xE132
#  define ID_WINDOW_TILE_HORZ   0xE133
#  define ID_WINDOW_TILE_VERT   0xE134
#  define ID_NEXT_PANE          0xE150
#  define ID_PREV_PANE          0xE151
#endif
#ifndef ID_SEPARATOR
#  define ID_SEPARATOR          0       // status bar separator pane
#endif
#ifndef ID_INDICATOR_CAPS
#  define ID_INDICATOR_CAPS     0xE011
#  define ID_INDICATOR_NUM      0xE012
#  define ID_INDICATOR_SCRL     0xE013
#  define ID_INDICATOR_OVR      0xE014
#  define ID_INDICATOR_REC      0xE015
#endif

// MAKEINTRESOURCE — needed in non-Windows builds
#ifndef MAKEINTRESOURCE
#  define MAKEINTRESOURCE(id) ((LPCTSTR)(ULONG_PTR)(WORD)(id))
#endif

// _atoi64 — MSVC intrinsic (use strtoll on POSIX)
#ifndef _atoi64
#  define _atoi64(s)  strtoll((s), nullptr, 10)
#endif

// MB_ICONERROR already defined; alias
#ifndef MB_ICONERROR
#  define MB_ICONERROR 0x00000010
#endif

// SetScrollInfo flags
#ifndef SB_HORZ
#  define SB_HORZ   0
#  define SB_VERT   1
#  define SB_BOTH   3
#  define SB_CTL    2
#endif

// Font weight
#ifndef FW_NORMAL
#  define FW_THIN         100
#  define FW_NORMAL       400
#  define FW_BOLD         700
#endif
// Font charset constants (guarded separately — FW_NORMAL may come from system headers)
#ifndef DEFAULT_CHARSET
#  define DEFAULT_CHARSET     1
#  define ANSI_CHARSET        0
#  define OEM_CHARSET       255
#  define SYMBOL_CHARSET      2
#  define SHIFTJIS_CHARSET  128
#  define GB2312_CHARSET    134
#  define HANGEUL_CHARSET   129
#  define CHINESEBIG5_CHARSET 136
#  define JOHAB_CHARSET     130
#  define HEBREW_CHARSET    177
#  define ARABIC_CHARSET    178
#  define GREEK_CHARSET     161
#  define TURKISH_CHARSET   162
#  define VIETNAMESE_CHARSET 163
#  define THAI_CHARSET      222
#  define EASTEUROPE_CHARSET 238
#  define RUSSIAN_CHARSET   204
#  define MAC_CHARSET        77
#  define BALTIC_CHARSET    186
#endif
#ifndef OUT_DEFAULT_PRECIS
#  define OUT_DEFAULT_PRECIS   0
#  define OUT_STRING_PRECIS    1
#  define OUT_CHARACTER_PRECIS 2
#  define OUT_STROKE_PRECIS    3
#  define OUT_TT_PRECIS        4
#  define OUT_DEVICE_PRECIS    5
#  define OUT_RASTER_PRECIS    6
#  define OUT_TT_ONLY_PRECIS   7
#  define OUT_OUTLINE_PRECIS   8
#  define CLIP_DEFAULT_PRECIS  0
#  define DEFAULT_QUALITY      0
#  define DEFAULT_PITCH        0
#  define FIXED_PITCH          1
#  define VARIABLE_PITCH       2
#  define MONO_FONT            8
#  define FF_DONTCARE          0
#  define FF_ROMAN            16
#  define FF_SWISS            32
#  define FF_MODERN           48
#  define FF_SCRIPT           64
#  define FF_DECORATIVE       80
#  define TRUETYPE_FONTTYPE    0x004
#  define RASTER_FONTTYPE      0x001
#  define DEVICE_FONTTYPE      0x002
#endif

// Pen styles
#ifndef PS_SOLID
#  define PS_SOLID          0
#  define PS_DASH           1
#  define PS_DOT            2
#  define PS_DASHDOT        3
#  define PS_DASHDOTDOT     4
#  define PS_NULL           5
#  define PS_INSIDEFRAME    6
#  define PS_USERSTYLE      7
#  define PS_ALTERNATE      8
#  define PS_STYLE_MASK     0x0000000F
#  define PS_ENDCAP_ROUND   0x00000000
#  define PS_ENDCAP_SQUARE  0x00000100
#  define PS_ENDCAP_FLAT    0x00000200
#  define PS_ENDCAP_MASK    0x00000F00
#  define PS_JOIN_ROUND     0x00000000
#  define PS_JOIN_BEVEL     0x00001000
#  define PS_JOIN_MITER     0x00002000
#  define PS_JOIN_MASK      0x0000F000
#  define PS_COSMETIC       0x00000000
#  define PS_GEOMETRIC      0x00010000
#  define PS_TYPE_MASK      0x000F0000
#endif

// Brush hatch styles
#ifndef HS_HORIZONTAL
#  define HS_HORIZONTAL     0
#  define HS_VERTICAL       1
#  define HS_FDIAGONAL      2
#  define HS_BDIAGONAL      3
#  define HS_CROSS          4
#  define HS_DIAGCROSS      5
#endif

// Stock objects
#ifndef NULL_BRUSH
#  define WHITE_BRUSH    0
#  define LTGRAY_BRUSH   1
#  define GRAY_BRUSH     2
#  define DKGRAY_BRUSH   3
#  define BLACK_BRUSH    4
#  define NULL_BRUSH     5
#  define HOLLOW_BRUSH   NULL_BRUSH
#  define WHITE_PEN      6
#  define BLACK_PEN      7
#  define NULL_PEN       8
#  define SYSTEM_FONT    13
#  define DEFAULT_GUI_FONT 17
#endif

// ROP2 codes
#ifndef R2_COPYPEN
#  define R2_COPYPEN     13
#  define R2_XORPEN      7
#  define R2_NOTXORPEN   6
#  define SRCCOPY        0x00CC0020UL
#  define SRCPAINT       0x00EE0086UL
#  define SRCAND         0x008800C6UL
#  define SRCINVERT      0x00660046UL
#  define PATCOPY        0x00F00021UL
#  define PATPAINT       0x00FB0A09UL
#  define PATINVERT      0x005A0049UL
#  define DSTINVERT      0x00550009UL
#  define BLACKNESS      0x00000042UL
#  define WHITENESS      0x00FF0062UL
#endif

// Device capabilities
#ifndef LOGPIXELSX
#  define LOGPIXELSX  88
#  define LOGPIXELSY  90
#  define HORZRES     8
#  define VERTRES     10
#  define BITSPIXEL   12
#  define PLANES      14
#endif

// Text flags
#ifndef DT_LEFT
#  define DT_LEFT       0x00000000
#  define DT_CENTER     0x00000001
#  define DT_RIGHT      0x00000002
#  define DT_TOP        0x00000000
#  define DT_VCENTER    0x00000004
#  define DT_BOTTOM     0x00000008
#  define DT_SINGLELINE 0x00000020
#  define DT_WORDBREAK  0x00000010
#  define DT_NOCLIP     0x00000100
#  define DT_NOPREFIX   0x00000800
#  define DT_CALCRECT   0x00000400
#  define DT_END_ELLIPSIS 0x00008000
#  define TRANSPARENT   1
#  define OPAQUE        2
#endif

// List-view styles
#ifndef LVS_REPORT
#  define LVS_REPORT    0x0001
#  define LVS_ICON      0x0000
#  define LVS_SMALLICON 0x0002
#  define LVS_LIST      0x0003
#  define LVS_SORTASCENDING   0x0010
#  define LVS_SORTDESCENDING  0x0020
#  define LVS_EX_FULLROWSELECT 0x00000020
#  define LVS_EX_GRIDLINES    0x00000001
#  define LVS_EX_CHECKBOXES   0x00000004
#  define LVCF_FMT    0x0001
#  define LVCF_WIDTH  0x0002
#  define LVCF_TEXT   0x0004
#  define LVCF_SUBITEM 0x0008
#  define LVCFMT_LEFT  0x0000
#  define LVCFMT_RIGHT 0x0001
#  define LVCFMT_CENTER 0x0002
#  define LVIF_TEXT   0x0001
#  define LVIF_IMAGE  0x0002
#  define LVIF_PARAM  0x0004
#  define LVIF_STATE  0x0008
#  define LVIS_SELECTED 0x0002
#  define LVIS_FOCUSED  0x0001
#  define LVNI_SELECTED 0x0002
#  define LVNI_ALL      0x0000
#endif

// Map mode
#ifndef MM_TEXT
#  define MM_TEXT       1
#  define MM_LOMETRIC   2
#  define MM_HIMETRIC   3
#  define MM_LOENGLISH  4
#  define MM_HIENGLISH  5
#  define MM_TWIPS      6
#  define MM_ISOTROPIC  7
#  define MM_ANISOTROPIC 8
#endif

// ── Forward declarations (resolve circular references) ────────────────────
class CFont;
class CRgn;
class CView;
class CDocument;
class CDataExchange;
class CStatusBar;
class CToolBar;
class CPropertyPage;
class CDocTemplate;
class CFrameWnd;
// PRINTDLG stub (defined early so CPrintDialog can embed it by value)
struct PRINTDLG {
    DWORD lStructSize = sizeof(PRINTDLG);
    HWND  hwndOwner   = nullptr;
    HDC   hDC         = nullptr;
    DWORD Flags       = 0;
    WORD  nFromPage   = 0;
    WORD  nToPage     = 0;
    WORD  nMinPage    = 0;
    WORD  nMaxPage    = 0;
    WORD  nCopies     = 1;
};
// X509 comes from <openssl/ssl.h>; include it if not already included.
// Our stub is at platform/openssl/ssl.h; real builds supply it via -I.
#include <openssl/ssl.h>
class COleSafeArray;  // defined later in this file

// ── VARTYPE — COM variant type tag ────────────────────────────────────────
typedef unsigned short VARTYPE;
// LCID — locale ID
typedef DWORD LCID;

// ── POINT / SIZE / RECT ────────────────────────────────────────────────────
// All defined in windows_types.h; nothing extra needed here.

// ── Common structs ─────────────────────────────────────────────────────────
struct MSG {
    HWND   hwnd;
    UINT   message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD  time;
    POINT  pt;
};

struct CREATESTRUCT {
    LPVOID     lpCreateParams;
    HINSTANCE  hInstance;
    HMENU      hMenu;
    HWND       hwndParent;
    int        cy, cx, y, x;
    LONG       style;
    LPCTSTR    lpszName;
    LPCTSTR    lpszClass;
    DWORD      dwExStyle;
};

struct WINDOWPLACEMENT {
    UINT length;
    UINT flags;
    UINT showCmd;
    POINT ptMinPosition;
    POINT ptMaxPosition;
    RECT  rcNormalPosition;
};

// CWnd::GetWindowPlacement — out-of-line (WINDOWPLACEMENT is forward-declared in
// compat.h).  Fills rcNormalPosition with the control's size (0,0,w,h); callers
// like GetButtonSize() only use the width/height.  Enough for the canonical
// OnSize relayout (e.g. CPluginsDlg) to compute correct button dimensions.
inline BOOL CWnd::GetWindowPlacement(WINDOWPLACEMENT* wp) const {
    if (!wp) return FALSE;
    wp->length  = sizeof(WINDOWPLACEMENT);
    wp->flags   = 0;
    wp->showCmd = 1;   // SW_SHOWNORMAL
    wp->ptMinPosition.x = wp->ptMinPosition.y = 0;
    wp->ptMaxPosition.x = wp->ptMaxPosition.y = 0;
    wp->rcNormalPosition.left = wp->rcNormalPosition.top = 0;
    wp->rcNormalPosition.right = wp->rcNormalPosition.bottom = 0;
    if (g_wxhook_get_client_rect)
        g_wxhook_get_client_rect(m_hWnd, &wp->rcNormalPosition);
    return TRUE;
}

typedef CREATESTRUCT *        LPCREATESTRUCT;
typedef const CREATESTRUCT *  LPCCREATESTRUCT;

#define DRAWITEMSTRUCT_defined
struct DRAWITEMSTRUCT {
    UINT   CtlType;
    UINT   CtlID;
    UINT   itemID;
    UINT   itemAction;
    UINT   itemState;
    HWND   hwndItem;
    HDC    hDC;
    RECT   rcItem;
    DWORD_PTR itemData;
};
typedef DRAWITEMSTRUCT *  LPDRAWITEMSTRUCT;

struct MEASUREITEMSTRUCT {
    UINT  CtlType;
    UINT  CtlID;
    UINT  itemID;
    UINT  itemWidth;
    UINT  itemHeight;
    DWORD_PTR itemData;
};
typedef MEASUREITEMSTRUCT * LPMEASUREITEMSTRUCT;

struct DELETEITEMSTRUCT {
    UINT     CtlType;
    UINT     CtlID;
    UINT     itemID;
    HWND     hwndItem;
    DWORD_PTR itemData;
};
typedef DELETEITEMSTRUCT * LPDELETEITEMSTRUCT;

struct COMPAREITEMSTRUCT {
    UINT  CtlType;
    UINT  CtlID;
    HWND  hwndItem;
    UINT  itemID1;
    DWORD_PTR itemData1;
    UINT  itemID2;
    DWORD_PTR itemData2;
    DWORD dwLocaleId;
};

// CCreateContext — passed when creating frame/view pairs
struct CCreateContext {
    CRuntimeClass*  m_pNewViewClass  = nullptr;
    CDocument*      m_pCurrentDoc    = nullptr;
    CDocTemplate*   m_pNewDocTemplate= nullptr;
    CView*          m_pLastView      = nullptr;
    CFrameWnd*      m_pCurrentFrame  = nullptr;
};

// Resource handle
typedef void* HRSRC;

// Palette types
struct PALETTEENTRY { BYTE peRed = 0; BYTE peGreen = 0; BYTE peBlue = 0; BYTE peFlags = 0; };
struct LOGPALETTE    { WORD palVersion = 0x300; WORD palNumEntries = 0; PALETTEENTRY palPalEntry[1]; };

// DIB section info
#ifndef BITMAPINFOHEADER_DEFINED
#define BITMAPINFOHEADER_DEFINED
struct BITMAPINFOHEADER {
    DWORD biSize;
    LONG  biWidth; LONG biHeight;
    WORD  biPlanes; WORD biBitCount;
    DWORD biCompression; DWORD biSizeImage;
    LONG  biXPelsPerMeter; LONG biYPelsPerMeter;
    DWORD biClrUsed; DWORD biClrImportant;
};
#endif
struct DIBSECTION {
    BITMAP          dsBm;
    BITMAPINFOHEADER dsBmih;
    DWORD           dsBitfields[3] = {};
    HANDLE          dshSection = nullptr;
    DWORD           dsOffset   = 0;
};

// CHOOSECOLOR — embedded in CColorDialog as m_cc
struct CHOOSECOLOR {
    DWORD       lStructSize    = sizeof(CHOOSECOLOR);
    HWND        hwndOwner      = nullptr;
    HWND        hInstance      = nullptr;
    COLORREF    rgbResult      = 0;
    COLORREF*   lpCustColors   = nullptr;
    DWORD       Flags          = 0;
    LPARAM      lCustData      = 0;
    void*       lpfnHook       = nullptr;
    LPCTSTR     lpTemplateName = nullptr;
};

// NMCUSTOMDRAW — common custom-draw header (embedded in NMLVCUSTOMDRAW as nmcd)
struct NMCUSTOMDRAW {
    NMHDR     hdr;
    DWORD     dwDrawStage = 0;
    HDC       hdc         = nullptr;
    RECT      rc          = {};
    DWORD_PTR dwItemSpec  = 0;
    UINT      uItemState  = 0;
    LPARAM    lItemlParam = 0;
};

// NMLVCUSTOMDRAW — custom-draw notification for list controls
struct NMLVCUSTOMDRAW {
    NMCUSTOMDRAW nmcd;
    COLORREF  clrText     = 0;
    COLORREF  clrTextBk   = 0;
    int       iSubItem    = 0;
};

struct LOGFONT {
    LONG  lfHeight;
    LONG  lfWidth;
    LONG  lfEscapement;
    LONG  lfOrientation;
    LONG  lfWeight;
    BYTE  lfItalic;
    BYTE  lfUnderline;
    BYTE  lfStrikeOut;
    BYTE  lfCharSet;
    BYTE  lfOutPrecision;
    BYTE  lfClipPrecision;
    BYTE  lfQuality;
    BYTE  lfPitchAndFamily;
    TCHAR lfFaceName[32];
};

struct TEXTMETRIC {
    LONG tmHeight;
    LONG tmAscent;
    LONG tmDescent;
    LONG tmInternalLeading;
    LONG tmExternalLeading;
    LONG tmAveCharWidth;
    LONG tmMaxCharWidth;
    LONG tmWeight;
    LONG tmOverhang;
    LONG tmDigitizedAspectX;
    LONG tmDigitizedAspectY;
    TCHAR tmFirstChar;
    TCHAR tmLastChar;
    TCHAR tmDefaultChar;
    TCHAR tmBreakChar;
    BYTE tmItalic;
    BYTE tmUnderlined;
    BYTE tmStruckOut;
    BYTE tmPitchAndFamily;
    BYTE tmCharSet;
};
// ANSI variants (A = ANSI, same as non-A on non-Windows)
typedef LOGFONT   LOGFONTA;
typedef TEXTMETRIC TEXTMETRICA;
#ifndef CONST
#  define CONST const
#endif
// EnumFontFamiliesEx callback typedef
typedef int (CALLBACK* FONTENUMPROCEX)(CONST LOGFONTA*, CONST TEXTMETRICA*, DWORD, LPARAM);
inline int EnumFontFamiliesEx(HDC, LOGFONTA*, FONTENUMPROCEX, LPARAM, DWORD) { return 1; }

// ── CHOOSEFONT — requires LOGFONT to be complete ───────────────────────────
#ifndef CHOOSEFONT_defined
#define CHOOSEFONT_defined
struct CHOOSEFONT {
    DWORD    lStructSize    = sizeof(CHOOSEFONT);
    HWND     hwndOwner      = nullptr;
    HDC      hDC            = nullptr;
    LOGFONT* lpLogFont      = nullptr;
    INT      iPointSize     = 0;
    DWORD    Flags          = 0;
    COLORREF rgbColors      = 0;
    LPARAM   lCustData      = 0;
    void*    lpfnHook       = nullptr;
    LPCTSTR  lpTemplateName = nullptr;
    HINSTANCE hInstance     = nullptr;
    LPTSTR   lpszStyle      = nullptr;
    WORD     nFontType      = 0;
    INT      nSizeMin       = 0;
    INT      nSizeMax       = 0;
};
#endif

// ── NONCLIENTMETRICS — requires LOGFONT to be complete ────────────────────
#ifndef NONCLIENTMETRICS_defined
#define NONCLIENTMETRICS_defined
struct NONCLIENTMETRICS {
    UINT    cbSize           = sizeof(NONCLIENTMETRICS);
    int     iBorderWidth     = 1;
    int     iScrollWidth     = 16;
    int     iScrollHeight    = 16;
    int     iCaptionWidth    = 18;
    int     iCaptionHeight   = 18;
    LOGFONT lfCaptionFont    = {};
    int     iSmCaptionWidth  = 12;
    int     iSmCaptionHeight = 15;
    LOGFONT lfSmCaptionFont  = {};
    int     iMenuWidth       = 18;
    int     iMenuHeight      = 18;
    LOGFONT lfMenuFont       = {};
    LOGFONT lfStatusFont     = {};
    LOGFONT lfMessageFont    = {};
};
#endif

#ifndef NMHDR
struct NMHDR { HWND hwndFrom = nullptr; UINT idFrom = 0; UINT code = 0; };
#define NMHDR NMHDR
#endif

struct LVITEM {
    UINT   mask;
    int    iItem;
    int    iSubItem;
    UINT   state;
    UINT   stateMask;
    LPTSTR pszText;
    int    cchTextMax;
    int    iImage;
    LPARAM lParam;
};

struct LVCOLUMN {
    UINT   mask;
    int    fmt;
    int    cx;
    LPTSTR pszText;
    int    cchTextMax;
    int    iSubItem;
};

struct TVITEM {
    UINT      mask;
    HTREEITEM hItem;
    UINT      state;
    UINT      stateMask;
    LPTSTR    pszText;
    int       cchTextMax;
    int       iImage;
    int       iSelectedImage;
    int       cChildren;
    LPARAM    lParam;
};

struct TVINSERTSTRUCT {
    HTREEITEM hParent;
    HTREEITEM hInsertAfter;
    TVITEM    item;
};

struct NMTREEVIEW { NMHDR hdr; UINT action; TVITEM itemOld; TVITEM itemNew; };
struct NMLISTVIEW { NMHDR hdr; int iItem; int iSubItem; UINT uNewState; UINT uOldState; UINT uChanged; LPARAM lParam; };
struct NMITEMACTIVATE { NMHDR hdr; int iItem; int iSubItem; UINT uNewState; UINT uOldState; UINT uChanged; POINT ptAction; LPARAM lParam; UINT uKeyFlags; };
#define NOTIFYICONDATA_defined
struct NOTIFYICONDATA { DWORD cbSize; HWND hWnd; UINT uID; UINT uFlags; UINT uCallbackMessage; HICON hIcon; TCHAR szTip[128]; };

// ── Geometry classes ───────────────────────────────────────────────────────

class CPoint {
public:
    int x = 0, y = 0;
    CPoint() = default;
    CPoint(int x_, int y_) : x(x_), y(y_) {}
    CPoint(const POINT& p) : x(p.x), y(p.y) {}
    CPoint(DWORD dw) : x((int)(short)LOWORD(dw)), y((int)(short)HIWORD(dw)) {}
    void Offset(int dx, int dy) { x += dx; y += dy; }
    bool operator==(const CPoint& o) const { return x==o.x && y==o.y; }
    bool operator!=(const CPoint& o) const { return !(*this==o); }
    CPoint operator+(const CPoint& o) const { return {x+o.x, y+o.y}; }
    CPoint operator-(const CPoint& o) const { return {x-o.x, y-o.y}; }
    CPoint& operator+=(const CPoint& o) { x+=o.x; y+=o.y; return *this; }
    CPoint& operator-=(const CPoint& o) { x-=o.x; y-=o.y; return *this; }
    operator POINT() const { POINT p; p.x=x; p.y=y; return p; }
    // Allow CPoint = SIZE (Windows MFC implicit conversion)
    CPoint& operator=(const SIZE& s) { x=s.cx; y=s.cy; return *this; }
    // Allow CPoint* to be passed as POINT*
    operator POINT*() { return reinterpret_cast<POINT*>(this); }
    operator const POINT*() const { return reinterpret_cast<const POINT*>(this); }
};

class CSize {
public:
    int cx = 0, cy = 0;
    CSize() = default;
    CSize(int cx_, int cy_) : cx(cx_), cy(cy_) {}
    CSize(const SIZE& s) : cx(s.cx), cy(s.cy) {}
    bool operator==(const CSize& o) const { return cx==o.cx && cy==o.cy; }
    bool operator!=(const CSize& o) const { return !(*this==o); }
    CSize operator+(const CSize& o) const { return {cx+o.cx, cy+o.cy}; }
    CSize operator-(const CSize& o) const { return {cx-o.cx, cy-o.cy}; }
    operator SIZE() const { SIZE s; s.cx=cx; s.cy=cy; return s; }
    // Allow CSize = int  (Windows scroll-position shorthand: sets cy)
    CSize& operator=(int n) { cy = n; return *this; }
    CSize& operator=(long n) { cy = (int)n; return *this; }
};

// CRect inherits from RECT so that CRect* implicitly converts to RECT*
// (required by Win32 APIs that take RECT* and MFC code that passes &myRect).
class CRect : public RECT {
public:
    CRect()                              { RECT::left=0; RECT::top=0; RECT::right=0; RECT::bottom=0; }
    CRect(int l, int t, int r, int b)   { RECT::left=l; RECT::top=t; RECT::right=r; RECT::bottom=b; }
    CRect(const RECT& r)                { RECT::left=r.left; RECT::top=r.top; RECT::right=r.right; RECT::bottom=r.bottom; }
    CRect(CPoint tl, CPoint br) { RECT::left=tl.x; RECT::top=tl.y; RECT::right=br.x; RECT::bottom=br.y; }
    CRect(CPoint tl, CSize sz) { RECT::left=tl.x; RECT::top=tl.y; RECT::right=tl.x+sz.cx; RECT::bottom=tl.y+sz.cy; }
    int  Width()  const { return right - left; }
    int  Height() const { return bottom - top; }
    bool IsRectEmpty() const { return Width()<=0 || Height()<=0; }
    bool IsRectNull()  const { return left==0 && top==0 && right==0 && bottom==0; }
    CPoint TopLeft()    const { return {left, top}; }
    CPoint BottomRight()const { return {right, bottom}; }
    CPoint CenterPoint()const { return {(left+right)/2, (top+bottom)/2}; }
    CSize  Size()       const { return {Width(), Height()}; }
    void SetRect(int l, int t, int r, int b) { left=l; top=t; right=r; bottom=b; }
    void SetRectEmpty() { left=top=right=bottom=0; }
    bool PtInRect(CPoint p) const { return p.x>=left && p.x<right && p.y>=top && p.y<bottom; }
    bool PtInRect(POINT p)  const { return p.x>=left && p.x<right && p.y>=top && p.y<bottom; }
    void InflateRect(int dx, int dy) { left-=dx; top-=dy; right+=dx; bottom+=dy; }
    void InflateRect(int l, int t, int r, int b) { left-=l; top-=t; right+=r; bottom+=b; }
    void DeflateRect(int dx, int dy) { left+=dx; top+=dy; right-=dx; bottom-=dy; }
    void OffsetRect(int dx, int dy) { left+=dx; top+=dy; right+=dx; bottom+=dy; }
    void MoveToXY(int x, int y) { right=x+Width(); bottom=y+Height(); left=x; top=y; }
    bool IntersectRect(const CRect* r1, const CRect* r2) {
        if (!r1 || !r2) return false;
        left=std::max(r1->left,r2->left); top=std::max(r1->top,r2->top);
        right=std::min(r1->right,r2->right); bottom=std::min(r1->bottom,r2->bottom);
        return !IsRectEmpty();
    }
    bool UnionRect(const CRect* r1, const CRect* r2) {
        if (!r1 || !r2) return false;
        left=std::min(r1->left,r2->left); top=std::min(r1->top,r2->top);
        right=std::max(r1->right,r2->right); bottom=std::max(r1->bottom,r2->bottom);
        return true;
    }
    void NormalizeRect() {
        if (left>right)  std::swap(left,right);
        if (top>bottom)  std::swap(top,bottom);
    }
    bool operator==(const CRect& o) const {
        return left==o.left && top==o.top && right==o.right && bottom==o.bottom;
    }
    bool operator!=(const CRect& o) const { return !(*this==o); }
    CRect& operator&=(const CRect& o) {
        left=std::max(left,o.left); top=std::max(top,o.top);
        right=std::min(right,o.right); bottom=std::min(bottom,o.bottom);
        return *this;
    }
    CRect& operator|=(const CRect& o) {
        left=std::min(left,o.left); top=std::min(top,o.top);
        right=std::max(right,o.right); bottom=std::max(bottom,o.bottom);
        return *this;
    }
    // Pointer conversion — lets CRect pass directly where RECT*/LPCRECT expected
    operator RECT*()             { return static_cast<RECT*>(this); }
    operator const RECT*() const { return static_cast<const RECT*>(this); }
    // Note: RECT& conversions are handled implicitly by public inheritance from RECT.
};

// ── Runtime-class infrastructure ───────────────────────────────────────────
// CRuntimeClass, DECLARE_DYNCREATE, IMPLEMENT_DYNCREATE, RUNTIME_CLASS
// are defined in mfc_object.h (which is included before this file).

// ── Message / dispatch / interface map DECLARE macros — all no-ops ─────────
#define DECLARE_MESSAGE_MAP()
#define DECLARE_DISPATCH_MAP()
#define DECLARE_INTERFACE_MAP()
#define DECLARE_OLECREATE(cls)
#define DECLARE_OLECREATE_EX(cls)
#define DECLARE_OLETYPELIB(cls, tlid, major, minor)
#define DECLARE_PROPPAGEIDS(cls, count)
#define BEGIN_MESSAGE_MAP(cls, base)
#define END_MESSAGE_MAP()
#define ON_COMMAND(id, fn)
#define ON_UPDATE_COMMAND_UI(id, fn)
#define ON_UPDATE_COMMAND_UI_RANGE(id1, id2, fn)
#define ON_COMMAND_RANGE(id1, id2, fn)
#define ON_COMMAND_EX(id, fn)
#define ON_COMMAND_EX_RANGE(id1, id2, fn)
#define ON_MESSAGE(msg, fn)
#define ON_REGISTERED_MESSAGE(msg, fn)
#define ON_NOTIFY(notif, id, fn)
#define ON_NOTIFY_EX(notif, id, fn)
#define ON_NOTIFY_EX_RANGE(notif, id1, id2, fn)
#define ON_NOTIFY_REFLECT(notif, fn)
#define ON_NOTIFY_REFLECT_EX(notif, fn)
#define ON_WM_CREATE()
#define ON_WM_DESTROY()
#define ON_WM_CLOSE()
#define ON_WM_PAINT()
#define ON_WM_TIMER()
#define ON_WM_SIZE()
#define ON_WM_MOVE()
#define ON_WM_SIZING()
#define ON_WM_MOVING()
#define ON_WM_SETFOCUS()
#define ON_WM_KILLFOCUS()
#define ON_WM_SETCURSOR()
#define ON_WM_ACTIVATE()
#define ON_WM_LBUTTONDOWN()
#define ON_WM_LBUTTONUP()
#define ON_WM_LBUTTONDBLCLK()
#define ON_WM_RBUTTONDOWN()
#define ON_WM_RBUTTONUP()
#define ON_WM_RBUTTONDBLCLK()
#define ON_WM_MBUTTONDOWN()
#define ON_WM_MBUTTONUP()
#define ON_WM_MOUSEMOVE()
#define ON_WM_MOUSEWHEEL()
#define ON_WM_KEYDOWN()
#define ON_WM_KEYUP()
#define ON_WM_SYSKEYDOWN()
#define ON_WM_SYSKEYUP()
#define ON_WM_SYSCHAR()
#define ON_WM_CHAR()
#define ON_WM_HSCROLL()
#define ON_WM_VSCROLL()
#define ON_WM_ERASEBKGND()
#define ON_WM_CONTEXTMENU()
#define ON_WM_NCLBUTTONDOWN()
#define ON_WM_NCPAINT()
#define ON_WM_GETDLGCODE()
#define ON_WM_DROPFILES()
#define ON_WM_INITMENU()
#define ON_WM_INITMENUPOPUP()
#define ON_WM_MENUCOMMAND()
#define ON_WM_SHOWWINDOW()
#define ON_WM_WINDOWPOSCHANGED()
#define ON_WM_GETMINMAXINFO()
#define ON_WM_SYSCOMMAND()
#define ON_WM_DRAWITEM()
#define ON_CBN_SELCHANGE(id, fn)
#define ON_BN_CLICKED(id, fn)
#define ON_EN_CHANGE(id, fn)
#define ON_EN_SETFOCUS(id, fn)
#define ON_EN_KILLFOCUS(id, fn)
#define ON_CONTROL_REFLECT(code, fn)
#define ON_WM_CTLCOLOR()
#define ON_WM_CTLCOLOR_REFLECT()
#define ON_WM_NCHITTEST()
#define ON_WM_NCCREATE()
#define ON_WM_NCDESTROY()
#define ON_WM_NCMOUSEMOVE()
#define ON_WM_MOUSEACTIVATE()
#define ON_WM_CHILDNOTIFY()
#define ON_WM_QUERYDRAGICON()
#define ON_WM_QUERYNEWPALETTE()
#define ON_WM_PALETTECHANGED()
#define ON_LBN_SELCHANGE(id, fn)
#define ON_LBN_DBLCLK(id, fn)
#define ON_LVN_ITEMCHANGED(id, fn)
#define ON_LVN_COLUMNCLICK(id, fn)
#define ON_LVN_GETDISPINFO(id, fn)
#define ON_TVN_SELCHANGED(id, fn)
#define ON_TVN_ITEMEXPANDING(id, fn)
#define ON_NM_CLICK(id, fn)
#define ON_NM_DBLCLK(id, fn)
#define ON_NM_RCLICK(id, fn)
#define ON_NM_RETURN(id, fn)
#define ON_CONTROL(notif, id, fn)
#define ON_CONTROL_RANGE(notif, id1, id2, fn)
#define ON_MYMESSAGE(msg, fn)

// ── Reactivated message-map macros (approach a) ─────────────────────────────
// Replace the no-op stubs above with real AFX_MSGMAP table generation so
// CDialog can route control commands to the canonical handlers.  Macros NOT
// overridden here (ON_WM_*, ON_UPDATE_COMMAND_UI, ON_NOTIFY, ON_MESSAGE,
// ON_COMMAND_EX, ...) remain no-ops and simply vanish from the table — they are
// not yet routed.  Each real macro emits one brace entry WITH a trailing comma,
// so interleaved empty macros disappear cleanly between entries.
#undef DECLARE_MESSAGE_MAP
#undef BEGIN_MESSAGE_MAP
#undef END_MESSAGE_MAP
#undef ON_COMMAND
#undef ON_COMMAND_RANGE
#undef ON_BN_CLICKED
#undef ON_CONTROL
#undef ON_CONTROL_RANGE
#undef ON_CBN_SELCHANGE
#undef ON_EN_CHANGE
#undef ON_LBN_SELCHANGE
#undef ON_LBN_DBLCLK
#undef ON_NOTIFY
#undef ON_NM_DBLCLK
#undef ON_LVN_COLUMNCLICK

#define DECLARE_MESSAGE_MAP() \
public: \
    static const AFX_MSGMAP* GetThisMessageMap(); \
    virtual const AFX_MSGMAP* GetMessageMap() const;

#define BEGIN_MESSAGE_MAP(theClass, baseClass) \
    const AFX_MSGMAP* theClass::GetMessageMap() const { return theClass::GetThisMessageMap(); } \
    const AFX_MSGMAP* theClass::GetThisMessageMap() { \
        typedef theClass ThisClass; (void) sizeof (ThisClass); \
        typedef baseClass TheBaseClass; \
        static const AFX_MSGMAP_ENTRY _entries[] = {

#define END_MESSAGE_MAP() \
            { 0, 0, 0, 0, AfxSig_end, (AFX_PMSG)0 } \
        }; \
        static const AFX_MSGMAP _map = { &TheBaseClass::GetThisMessageMap, _entries }; \
        return &_map; \
    }

// Command/control handlers are void() (AfxSig_vv); range handlers are void(UINT)
// (AfxSig_vw).  The static_cast does the class downcast to CCmdTarget (handling
// any this-pointer adjustment); for the VW case the result is then reinterpreted
// to the void() AFX_PMSG slot (same class, signature-only — safe) and cast back
// in the dispatcher.
#define _AFX_CMD_VV(code, id, lastid, fn) \
    { WM_COMMAND, (UINT)(code), (UINT)(id), (UINT)(lastid), AfxSig_vv, \
      static_cast<void (CCmdTarget::*)()>(&ThisClass::fn) },
#define _AFX_CMD_VW(code, id, lastid, fn) \
    { WM_COMMAND, (UINT)(code), (UINT)(id), (UINT)(lastid), AfxSig_vw, \
      reinterpret_cast<AFX_PMSG>(static_cast<void (CCmdTarget::*)(UINT)>(&ThisClass::fn)) },

#define ON_COMMAND(id, fn)                   _AFX_CMD_VV(0, id, id, fn)
#define ON_BN_CLICKED(id, fn)                _AFX_CMD_VV(BN_CLICKED, id, id, fn)
#define ON_CONTROL(code, id, fn)             _AFX_CMD_VV(code, id, id, fn)
#define ON_CBN_SELCHANGE(id, fn)             _AFX_CMD_VV(CBN_SELCHANGE, id, id, fn)
#define ON_EN_CHANGE(id, fn)                 _AFX_CMD_VV(EN_CHANGE, id, id, fn)
#define ON_LBN_SELCHANGE(id, fn)             _AFX_CMD_VV(LBN_SELCHANGE, id, id, fn)
#define ON_LBN_DBLCLK(id, fn)                _AFX_CMD_VV(LBN_DBLCLK, id, id, fn)
#define ON_COMMAND_RANGE(id1, id2, fn)       _AFX_CMD_VW(0, id1, id2, fn)
#define ON_CONTROL_RANGE(code, id1, id2, fn) _AFX_CMD_VW(code, id1, id2, fn)

// WM_NOTIFY handlers — void(NMHDR*, LRESULT*).  The member-fn pointer is parked in
// the void()-typed AFX_PMSG slot (signature-only reinterpret; same class) and cast
// back in AfxDispatchNotify, mirroring the _AFX_CMD_VW trick.
#define _AFX_NOTIFY(notif, id, fn) \
    { WM_NOTIFY, (UINT)(notif), (UINT)(id), (UINT)(id), AfxSig_notify, \
      reinterpret_cast<AFX_PMSG>(static_cast<void (CCmdTarget::*)(NMHDR*, LRESULT*)>(&ThisClass::fn)) },
#define ON_NOTIFY(notif, id, fn)        _AFX_NOTIFY(notif, id, fn)
#define ON_NM_DBLCLK(id, fn)            _AFX_NOTIFY(NM_DBLCLK, id, fn)
#define ON_LVN_COLUMNCLICK(id, fn)      _AFX_NOTIFY(LVN_COLUMNCLICK, id, fn)

// Dispatch a WM_COMMAND-style notification to the first matching handler in the
// target's message-map chain.  Returns TRUE if handled.
inline BOOL AfxDispatchCommand(CCmdTarget* target, UINT code, UINT id) {
    if (!target) return FALSE;
    for (const AFX_MSGMAP* m = target->GetMessageMap(); m;
         m = m->pfnGetBaseMap ? m->pfnGetBaseMap() : nullptr) {
        for (const AFX_MSGMAP_ENTRY* e = m->lpEntries; e->nSig != (UINT_PTR)AfxSig_end; ++e) {
            if (e->nMessage == (UINT)WM_COMMAND && e->nCode == code &&
                id >= e->nID && id <= e->nLastID) {
                if (e->nSig == (UINT_PTR)AfxSig_vw) {
                    void (CCmdTarget::*pfn)(UINT) =
                        reinterpret_cast<void (CCmdTarget::*)(UINT)>(e->pfn);
                    (target->*pfn)(id);
                } else {
                    (target->*(e->pfn))();
                }
                return TRUE;
            }
        }
    }
    return FALSE;
}

// Dispatch a WM_NOTIFY-style notification (e.g. LVN_COLUMNCLICK, NM_DBLCLK) to the
// first matching ON_NOTIFY handler in the target's message-map chain.  `pnmh`
// points at the (possibly extended, e.g. NMLISTVIEW) notification struct; the
// handler receives it as NMHDR* and casts as needed.  Returns TRUE if handled.
inline BOOL AfxDispatchNotify(CCmdTarget* target, UINT code, UINT id,
                              NMHDR* pnmh, LRESULT* pResult) {
    if (!target) return FALSE;
    for (const AFX_MSGMAP* m = target->GetMessageMap(); m;
         m = m->pfnGetBaseMap ? m->pfnGetBaseMap() : nullptr) {
        for (const AFX_MSGMAP_ENTRY* e = m->lpEntries; e->nSig != (UINT_PTR)AfxSig_end; ++e) {
            if (e->nMessage == (UINT)WM_NOTIFY && e->nSig == (UINT_PTR)AfxSig_notify &&
                e->nCode == code && id >= e->nID && id <= e->nLastID) {
                void (CCmdTarget::*pfn)(NMHDR*, LRESULT*) =
                    reinterpret_cast<void (CCmdTarget::*)(NMHDR*, LRESULT*)>(e->pfn);
                (target->*pfn)(pnmh, pResult);
                return TRUE;
            }
        }
    }
    return FALSE;
}

// ── ON_UPDATE_COMMAND_UI (command-UI enable/check state) ───────────────────
// MFC's WM_KICKIDLE-driven UI update.  The early stub block #defined these as
// no-ops; redefine them to emit real map entries (handler sig void(CCmdUI*)) so
// the bridge can run them and enable/disable/check dialog controls by state.
class CCmdUI;  // fully defined later in this file
#undef ON_UPDATE_COMMAND_UI
#undef ON_UPDATE_COMMAND_UI_RANGE
#define _AFX_CMDUI(id, lastid, fn) \
    { WM_COMMAND, 0xFFFFu /*cmdui marker, never a real notification code*/, \
      (UINT)(id), (UINT)(lastid), AfxSig_cmdui, \
      reinterpret_cast<AFX_PMSG>(static_cast<void (CCmdTarget::*)(CCmdUI*)>(&ThisClass::fn)) },
#define ON_UPDATE_COMMAND_UI(id, fn)             _AFX_CMDUI(id, id, fn)
#define ON_UPDATE_COMMAND_UI_RANGE(id1, id2, fn) _AFX_CMDUI(id1, id2, fn)
// AfxDispatchUpdateUI is defined after CCmdUI (needs its complete type).

// ── Dispatch-map macros — no-ops ───────────────────────────────────────────
// The full DISPATCH_MAP block in doc.cpp is guarded #ifdef _WIN32 separately;
// these macros exist to suppress errors if the guard is ever removed.
#define BEGIN_DISPATCH_MAP(cls, base)
#define END_DISPATCH_MAP()
#define DISP_PROPERTY(cls, name, mem, type)
#define DISP_PROPERTY_NOTIFY(cls, name, mem, fn, type)
#define DISP_PROPERTY_EX(cls, name, get, set, type)
#define DISP_PROPERTY_PARAM(cls, name, get, set, type, ...)
#define DISP_FUNCTION(cls, name, fn, ret, ...)
#define DISP_FUNCTION_ID(cls, name, id, fn, ret, ...)
#define DISP_DEFVALUE(cls, name)

// ── Interface-map macros — no-ops ──────────────────────────────────────────
#define BEGIN_INTERFACE_MAP(cls, base)
#define END_INTERFACE_MAP()
#define INTERFACE_PART(cls, iid, local)
#define INTERFACE_AGGREGATE(cls, agg)

// ── VARIANT stub ─────────────────────────────────────────────────────────
// COM VARIANT — used in CMUSHclientApp::GetGlobalOption on all platforms.
#ifndef VARIANT_DEFINED
#define VARIANT_DEFINED
struct VARIANT {
    unsigned short vt = 0;
    union {
        int       intVal;
        short     iVal;      // VT_I2
        long      lVal;      // VT_I4
        float     fltVal;    // VT_R4
        double    dblVal;    // VT_R8
        double    date;      // VT_DATE
        BOOL      boolVal;   // VT_BOOL
        char*     bstrVal;   // VT_BSTR
        void*     byref;
        SAFEARRAY* parray;  // VT_ARRAY
    };
};
#endif

// ── VARIANT helpers ────────────────────────────────────────────────────────
// VariantInit — Windows OLE; no-op on non-Windows.
// VariantClear — clears the variant AND, for a VT_ARRAY variant, frees the
// backing SafeArray (and its elements).  0x2000 == VT_ARRAY (defined below).
// SafeArrayDestroy is forward-declared in windows_types.h for the full build.
inline void VariantClear(VARIANT* pv) {
    if (!pv) return;
    if ((pv->vt & 0x2000) && pv->parray) SafeArrayDestroy(pv->parray);
    pv->vt = 0;
    pv->lVal = 0;
}
inline void VariantInit(VARIANT* pv)  { if (pv) { pv->vt = 0; pv->lVal = 0; } }

// ── COleTemplateServer stub ────────────────────────────────────────────────
// COM server object — Windows-only; no-op stub for compilation.
class CDocTemplate;
class COleTemplateServer {
public:
    void ConnectTemplate(const struct GUID&, CDocTemplate*, BOOL) {}
    static void RegisterAll() {}
};

// ── COM / IID stubs ────────────────────────────────────────────────────────
#ifndef GUID_DEFINED
#define GUID_DEFINED
struct GUID {
    unsigned long  Data1;
    unsigned short Data2, Data3;
    unsigned char  Data4[8];
};
typedef GUID IID;
typedef GUID CLSID;
typedef const CLSID* REFCLSID;
typedef const IID*   REFIID;
#endif

// ── VT_ type tags ─────────────────────────────────────────────────────────
// All VT_* constants defined here with individual guards so they can coexist
// with any enum-based definitions in system headers.
#ifndef VT_EMPTY
#  define VT_EMPTY    0
#endif
#ifndef VT_NULL
#  define VT_NULL     1
#endif
#ifndef VT_I2
#  define VT_I2       2
#endif
#ifndef VT_I4
#  define VT_I4       3
#endif
#ifndef VT_R4
#  define VT_R4       4
#endif
#ifndef VT_R8
#  define VT_R8       5
#endif
#ifndef VT_DATE
#  define VT_DATE     7
#endif
#ifndef VT_BSTR
#  define VT_BSTR     8
#endif
#ifndef VT_BOOL
#  define VT_BOOL     11
#endif
#ifndef VT_VARIANT
#  define VT_VARIANT  12
#endif
#ifndef VT_UI1
#  define VT_UI1      17
#endif
#ifndef VT_ARRAY
#  define VT_ARRAY    0x2000
#endif

// ── Real SafeArray implementation (all non-Windows builds) ──────────────────
// MUSHclient's GetPluginList/GetVariableList/GetTriggerList/... build a
// one-dimensional COleSafeArray of VARIANTs and hand it back as a VT_ARRAY
// VARIANT; pushVariant() (lua_methods.cpp) walks it via these functions to make
// a Lua table.  The elements live as a heap VARIANT[] in SAFEARRAY::pvData;
// VT_BSTR elements own a strdup'd copy of their string (freed by Destroy), so
// callers that reuse a temporary CString per element are safe.
inline HRESULT SafeArrayGetLBound(SAFEARRAY* psa, UINT, long* pl) {
    if (pl) *pl = psa ? psa->rgsabound[0].lLbound : 0;
    return S_OK;
}
inline HRESULT SafeArrayGetLBound(SAFEARRAY* psa, UINT d, LONG* pl) {
    long t = 0; SafeArrayGetLBound(psa, d, &t); if (pl) *pl = (LONG) t; return S_OK;
}
inline HRESULT SafeArrayGetUBound(SAFEARRAY* psa, UINT, long* pl) {
    if (pl) *pl = psa ? (psa->rgsabound[0].lLbound + (long) psa->rgsabound[0].cElements - 1) : -1;
    return S_OK;
}
inline HRESULT SafeArrayGetUBound(SAFEARRAY* psa, UINT d, LONG* pl) {
    long t = -1; SafeArrayGetUBound(psa, d, &t); if (pl) *pl = (LONG) t; return S_OK;
}
inline HRESULT SafeArrayGetElement(SAFEARRAY* psa, long* index, void* pv) {
    if (!psa || !psa->pvData || !index || !pv) return E_FAIL;
    long i = index[0] - psa->rgsabound[0].lLbound;
    if (i < 0 || (unsigned long) i >= psa->rgsabound[0].cElements) return E_FAIL;
    *(VARIANT*) pv = ((VARIANT*) psa->pvData)[i];   // shallow copy; bstrVal is borrowed
    return S_OK;
}
inline HRESULT SafeArrayGetElement(SAFEARRAY* psa, LONG* index, void* pv) {
    long ix[2] = { index ? index[0] : 0, index ? index[1] : 0 };
    return SafeArrayGetElement(psa, ix, pv);
}
inline HRESULT SafeArrayDestroy(SAFEARRAY* psa) {
    if (!psa) return S_OK;
    if (VARIANT* data = (VARIANT*) psa->pvData) {
        for (unsigned long i = 0; i < psa->rgsabound[0].cElements; ++i)
            if (data[i].vt == VT_BSTR && data[i].bstrVal) free(data[i].bstrVal);
        delete [] data;
    }
    delete psa;
    return S_OK;
}

// VTS_* string-encoded type tags for DISP_FUNCTION — these are only
// consumed by the DISP_* macros which are no-ops on non-Windows.
#ifndef VTS_BSTR
#  define VTS_BSTR    "\x08"
#  define VTS_I2      "\x02"
#  define VTS_I4      "\x03"
#  define VTS_R8      "\x05"
#  define VTS_BOOL    "\x0B"
#  define VTS_NONE    nullptr
#endif

// ── CCmdUI ────────────────────────────────────────────────────────────────
class CMenu;  // forward-declared; fully defined later in this file
class CCmdUI {
public:
    UINT   m_nID     = 0;
    int    m_nIndex  = 0;
    CMenu* m_pMenu   = nullptr;   // non-null when routing through a menu
    void*  m_hWndCtrl = nullptr;  // the control this CCmdUI updates (port: wxWindow*)
    // Capture mode: when these are non-null, Enable/SetCheck record into them
    // rather than routing to a control hook.  Used to drive a wx MENU item from
    // a canonical ON_UPDATE_COMMAND_UI handler (there is no control HWND then).
    bool*  m_pCaptureEnable = nullptr;
    int*   m_pCaptureCheck  = nullptr;
    void Enable(BOOL bOn = TRUE) {
        if (m_pCaptureEnable) { *m_pCaptureEnable = (bOn != FALSE); return; }
        if (m_hWndCtrl && g_wxhook_enable_window)
            g_wxhook_enable_window(m_hWndCtrl, bOn != FALSE);
    }
    void SetCheck(int nCheck = 1) {
        if (m_pCaptureCheck) { *m_pCaptureCheck = nCheck; return; }
        // BM_SETCHECK (0x00F1) is #defined later in this header than CCmdUI.
        if (m_hWndCtrl && g_wxhook_send_message)
            g_wxhook_send_message(m_hWndCtrl, 0x00F1 /*BM_SETCHECK*/, (uintptr_t)nCheck, 0);
    }
    void SetText(LPCTSTR s) {
        if (m_hWndCtrl && g_wxhook_set_window_text)
            g_wxhook_set_window_text(m_hWndCtrl, s ? s : "");
    }
    void SetRadio(BOOL bOn = TRUE) { SetCheck(bOn ? 1 : 0); }
    void ContinueRouting() {}
};

// Dispatch a command-UI update to the first matching ON_UPDATE_COMMAND_UI handler
// in the target's message-map chain.  pCmdUI->m_nID selects the control; the
// handler then enables/checks/sets-text on it via the CCmdUI.  Returns TRUE if a
// handler ran (so the caller can leave the control in its default state if not).
inline BOOL AfxDispatchUpdateUI(CCmdTarget* target, CCmdUI* pCmdUI) {
    if (!target || !pCmdUI) return FALSE;
    for (const AFX_MSGMAP* m = target->GetMessageMap(); m;
         m = m->pfnGetBaseMap ? m->pfnGetBaseMap() : nullptr) {
        for (const AFX_MSGMAP_ENTRY* e = m->lpEntries; e->nSig != (UINT_PTR)AfxSig_end; ++e) {
            if (e->nSig == (UINT_PTR)AfxSig_cmdui &&
                pCmdUI->m_nID >= e->nID && pCmdUI->m_nID <= e->nLastID) {
                void (CCmdTarget::*pfn)(CCmdUI*) =
                    reinterpret_cast<void (CCmdTarget::*)(CCmdUI*)>(e->pfn);
                (target->*pfn)(pCmdUI);
                return TRUE;
            }
        }
    }
    return FALSE;
}

// ── GDI object stubs / declarations ──────────────────────────────────────
//
// Under MUSHCLIENT_FULL_BUILD (mushapp + mushgui) these classes carry only
// member declarations; the real wx-backed implementations live in
// platform/wx_gdi_compat.cpp (compiled into mushgui only).
//
// Without MUSHCLIENT_FULL_BUILD (mushcore, tests) the inline no-op stubs
// below are used — they are never called at runtime in those targets.
//
// m_impl is present in both builds so struct layout is identical everywhere.
// m_hObject is kept for code that reads it directly (HGDIOBJ conversions).

class CGdiObject {
public:
    HGDIOBJ m_hObject = nullptr;
    void*   m_impl    = nullptr;   // wx backing object (CDCImpl*, wxPen*, …)
    operator HGDIOBJ() const { return m_hObject; }
    bool CreateStockObject(int) { return false; }
#ifndef MUSHCLIENT_FULL_BUILD
    virtual ~CGdiObject() {}
    bool DeleteObject() { return true; }
#else
    virtual ~CGdiObject();
    bool DeleteObject();
#endif
};

class CPen : public CGdiObject {
public:
    CPen() = default;
    CPen(int style, int width, COLORREF color);   // impl in wx_gdi_compat
#ifndef MUSHCLIENT_FULL_BUILD
    ~CPen() {}
    bool CreatePen(int, int, COLORREF) { return false; }
    bool CreatePenIndirect(void*) { return false; }
    BOOL Attach(HGDIOBJ) { return FALSE; }
    HGDIOBJ Detach() { return nullptr; }
#else
    ~CPen();
    bool CreatePen(int nPenStyle, int nWidth, COLORREF crColor);
    bool CreatePenIndirect(void*) { return false; }
    BOOL Attach(HGDIOBJ);
    HGDIOBJ Detach();
#endif
};

class CBitmap;   // forward for CBrush::CreatePatternBrush(CBitmap*)

class CBrush : public CGdiObject {
public:
    CBrush() = default;
    explicit CBrush(COLORREF c);   // impl in wx_gdi_compat
#ifndef MUSHCLIENT_FULL_BUILD
    ~CBrush() {}
    bool CreateSolidBrush(COLORREF)              { return false; }
    bool CreateHatchBrush(int, COLORREF)         { return false; }
    bool CreatePatternBrush(CBitmap*)            { return false; }
    bool CreatePatternBrush(HBITMAP)             { return false; }
    bool CreateDIBPatternBrush(const void*, UINT){ return false; }
    bool CreateSysColorBrush(int)                { return false; }
    bool CreateBrushIndirect(const LOGBRUSH*)    { return false; }
    bool CreateStockObject(int)                  { return false; }
    HBRUSH GetSafeHandle() const                 { return nullptr; }
#else
    ~CBrush();
    bool CreateSolidBrush(COLORREF);
    bool CreateHatchBrush(int nIndex, COLORREF crColor);
    bool CreatePatternBrush(CBitmap* pBitmap);
    bool CreatePatternBrush(HBITMAP)             { return false; }   // unused
    bool CreateDIBPatternBrush(const void*, UINT){ return false; }
    bool CreateSysColorBrush(int)                { return false; }
    bool CreateBrushIndirect(const LOGBRUSH* plb);
    bool CreateStockObject(int)                  { return false; }
    HBRUSH GetSafeHandle() const                 { return nullptr; }
#endif
};

class CFont : public CGdiObject {
public:
    CFont() = default;
#ifndef MUSHCLIENT_FULL_BUILD
    ~CFont() {}
    bool CreateFont(int, int, int, int, int, BYTE, BYTE, BYTE, BYTE,
                    BYTE, BYTE, BYTE, BYTE, LPCTSTR)       { return false; }
    bool CreateFontIndirect(const LOGFONT*)                { return false; }
    bool CreatePointFont(int, LPCTSTR, void* = nullptr)    { return false; }
    bool CreatePointFontIndirect(const LOGFONT*, void* = nullptr) { return false; }
    int  GetLogFont(LOGFONT* lf)       { if (lf) *lf = {}; return 0; }
    int  GetObject(int, LOGFONT* lf) const { if(lf)*lf={}; return 0; }
    HGDIOBJ GetSafeHandle() const      { return nullptr; }
#else
    ~CFont();
    bool CreateFont(int nHeight, int nWidth, int nEscapement, int nOrientation,
                    int nWeight, BYTE bItalic, BYTE bUnderline, BYTE cStrikeOut,
                    BYTE nCharSet, BYTE nOutPrecision, BYTE nClipPrecision,
                    BYTE nQuality, BYTE nPitchAndFamily, LPCTSTR lpszFacename);
    bool CreateFontIndirect(const LOGFONT* lplf);
    bool CreatePointFont(int nPointSize, LPCTSTR lpszFaceName, void* = nullptr);
    bool CreatePointFontIndirect(const LOGFONT* lpLogFont, void* = nullptr);
    int  GetLogFont(LOGFONT* lf);
    int  GetObject(int, LOGFONT* lf) const;
    HGDIOBJ GetSafeHandle() const { return m_hObject; }
#endif
};

class CBitmap : public CGdiObject {
public:
    CBitmap() = default;
#ifndef MUSHCLIENT_FULL_BUILD
    ~CBitmap() {}
    HBITMAP CreateCompatibleBitmap(void*, int, int)                        { return nullptr; }
    HBITMAP LoadBitmap(UINT)                                               { return nullptr; }
    HBITMAP LoadBitmap(LPCTSTR)                                            { return nullptr; }
    HBITMAP CreateDIBSection(HDC, const void*, UINT, void**, HANDLE, DWORD){ return nullptr; }
    BOOL GetBitmap(void*)                                                  { return FALSE; }
    BOOL GetBitmap(BITMAP* bm)                 { if(bm)*bm={}; return FALSE; }
    bool CreateBitmap(int, int, UINT, UINT, const void*)                   { return false; }
    bool CreateBitmapIndirect(const BITMAP*)                               { return false; }
    int  GetObject(int, void*) const                                       { return 0; }
    BOOL Attach(HGDIOBJ)                                                   { return FALSE; }
    HGDIOBJ Detach()                                                       { return nullptr; }
#else
    ~CBitmap();
    HBITMAP CreateCompatibleBitmap(void*, int nWidth, int nHeight);
    HBITMAP LoadBitmap(UINT)   { return nullptr; }
    HBITMAP LoadBitmap(LPCTSTR){ return nullptr; }
    HBITMAP CreateDIBSection(HDC, const void*, UINT, void**, HANDLE, DWORD){ return nullptr; }
    BOOL GetBitmap(void*)      { return FALSE; }
    BOOL GetBitmap(BITMAP* bm);
    bool CreateBitmap(int nWidth, int nHeight, UINT nPlanes, UINT nBitCount, const void* lpBits);
    bool CreateBitmapIndirect(const BITMAP*) { return false; }
    int  GetObject(int nCount, void* lpObject) const;
    bool DeleteObject();
    BOOL Attach(HGDIOBJ hObject);
    HGDIOBJ Detach();
#endif
};

class CRgn : public CGdiObject {
public:
    bool CreateRectRgn(int,int,int,int)        { return false; }
    bool CreateRectRgnIndirect(const RECT*)    { return false; }
    bool CreateEllipticRgn(int,int,int,int)   { return false; }
    bool CreateRoundRectRgn(int,int,int,int,int,int) { return false; }
    bool CreatePolygonRgn(const POINT*,int,int){ return false; }
    bool CombineRgn(CRgn*,CRgn*,int)          { return false; }
    BOOL PtInRegion(int,int) const             { return FALSE; }
    BOOL PtInRegion(POINT) const               { return FALSE; }
    BOOL RectInRegion(const RECT*) const       { return FALSE; }
    int  OffsetRgn(int,int)                    { return NULLREGION; }
    int  GetRgnBox(LPRECT)                     { return NULLREGION; }
};

class CPalette : public CGdiObject {
public:
    bool CreateHalftonePalette(void*)              { return false; }
    UINT GetNearestPaletteIndex(COLORREF)          { return 0; }
    bool CreatePalette(const LOGPALETTE*)          { return false; }
    int  GetEntryCount() const                     { return 0; }
    UINT GetPaletteEntries(UINT, UINT, PALETTEENTRY*) const { return 0; }
    UINT SetPaletteEntries(UINT, UINT, const PALETTEENTRY*) { return 0; }
};

// ── CDC ───────────────────────────────────────────────────────────────────
//
// In the full build (MUSHCLIENT_FULL_BUILD) every method is declared here
// and implemented in platform/wx_gdi_compat.cpp using wxMemoryDC.
// Without MUSHCLIENT_FULL_BUILD the inline no-op stubs are used (mushcore).

#ifndef MUSHCLIENT_FULL_BUILD
// ── CDC inline no-op stubs (mushcore / tests) ─────────────────────────────
class CDC {
public:
    HDC m_hDC       = nullptr;
    HDC m_hAttribDC = nullptr;
    void* m_impl    = nullptr;
    virtual ~CDC() {}

    HDC GetSafeHdc() const { return m_hDC; }
    operator HDC() const { return m_hDC; }
    static CDC* FromHandle(HDC) { static CDC dc; return &dc; }

    CGdiObject* SelectObject(CGdiObject*) { return nullptr; }
    CPen*    SelectObject(CPen*)    { return nullptr; }
    CBrush*  SelectObject(CBrush*)  { return nullptr; }
    CFont*   SelectObject(CFont*)   { return nullptr; }
    CBitmap* SelectObject(CBitmap*) { return nullptr; }
    CGdiObject* SelectStockObject(int) { return nullptr; }
    HGDIOBJ SelectObject(HGDIOBJ)  { return nullptr; }

    int SelectClipRgn(CRgn*) { return 0; }
    int ExcludeClipRect(int,int,int,int) { return 0; }
    int IntersectClipRect(int,int,int,int) { return 0; }
    int IntersectClipRect(const CRect& r) { return 0; }
    int ExcludeClipRect(LPCRECT) { return 0; }
    bool GetClipBox(RECT*) { return false; }

    bool Rectangle(int,int,int,int) { return false; }
    bool Rectangle(LPCRECT) { return false; }
    bool Ellipse(int,int,int,int) { return false; }
    bool RoundRect(int,int,int,int,int,int) { return false; }
    bool LineTo(int,int) { return false; }
    bool MoveTo(int,int,POINT* =nullptr) { return false; }
    bool Polyline(const POINT*,int) { return false; }
    bool Polygon(const POINT*,int) { return false; }
    bool PolyBezier(const POINT*,int) { return false; }
    bool Arc(int,int,int,int,int,int,int,int) { return false; }
    bool Chord(int,int,int,int,int,int,int,int) { return false; }
    bool FloodFill(int,int,COLORREF) { return false; }
    void FillRect(LPCRECT, CBrush*) {}
    void FillRect(const CRect*, CBrush*) {}
    void FrameRect(LPCRECT, CBrush*) {}
    void FillSolidRect(LPCRECT, COLORREF) {}
    void FillSolidRect(const CRect*, COLORREF) {}
    void FillSolidRect(int,int,int,int,COLORREF) {}
    void Draw3dRect(LPCRECT, COLORREF, COLORREF) {}
    void Draw3dRect(int,int,int,int,COLORREF,COLORREF) {}
    void DrawEdge(LPRECT, UINT, UINT) {}
    void DrawFocusRect(LPCRECT) {}
    bool PatBlt(int,int,int,int,DWORD) { return false; }
    bool BitBlt(int,int,int,int,CDC*,int,int,DWORD) { return false; }
    bool StretchBlt(int,int,int,int,CDC*,int,int,int,int,DWORD) { return false; }
    bool MaskBlt(int,int,int,int,CDC*,int,int,HBITMAP,int,int,DWORD) { return false; }
    bool ScrollDC(int,int,LPCRECT,LPCRECT,CRgn*,LPRECT) { return false; }
    bool DrawDragRect(LPCRECT,SIZE,LPCRECT,SIZE,CBrush* =nullptr,CBrush* =nullptr) { return false; }
    void DrawIcon(int,int,HICON) {}
    void DrawIcon(CPoint,HICON) {}

    bool TextOut(int,int,LPCTSTR,int=-1) { return false; }
    bool TextOut(int x, int y, const CString& s);
    bool ExtTextOut(int,int,UINT,LPCRECT,LPCTSTR,int,const int*) { return false; }
    int  DrawText(LPCTSTR,int,LPRECT,UINT) { return 0; }
    int  DrawText(const CString&,LPRECT,UINT) { return 0; }
    int  DrawTextEx(LPTSTR,int,LPRECT,UINT,void*) { return 0; }
    bool GrayString(CBrush*,BOOL(*)(HDC,LPARAM,int),LPARAM,int,int,int,int,int) { return false; }
    bool TabbedTextOut(int,int,LPCTSTR,int,int,LPINT,int) { return false; }

    COLORREF SetTextColor(COLORREF) { return 0; }
    COLORREF GetTextColor() const { return 0; }
    COLORREF SetBkColor(COLORREF) { return 0; }
    COLORREF GetBkColor() const { return 0; }
    int      SetBkMode(int) { return 0; }
    int      GetBkMode() const { return 0; }
    int      SetROP2(int) { return 0; }
    int      GetROP2() const { return 0; }
    UINT     SetTextAlign(UINT) { return 0; }
    UINT     GetTextAlign() const { return 0; }
    int      SetTextCharacterExtra(int) { return 0; }

    bool GetTextMetrics(TEXTMETRIC*) { return false; }
    CSize GetTextExtent(LPCTSTR, int=-1) { return {}; }
    CSize GetTextExtent(const CString&) { return {}; }
    CSize GetOutputTextExtent(LPCTSTR, int=-1) { return {}; }
    CSize GetOutputTextExtent(const CString&) { return {}; }
    int   GetDeviceCaps(int) { return 0; }
    bool  GetTextFace(int, LPTSTR) { return false; }

    CPoint SetViewportOrg(int,int) { return {}; }
    CPoint SetWindowOrg(int,int) { return {}; }
    CSize  SetViewportExt(int,int) { return {}; }
    CSize  SetWindowExt(int,int) { return {}; }
    CPoint OffsetViewportOrg(int,int) { return {}; }
    bool   SetMapMode(int) { return false; }

    template<class T> bool CreateCompatibleDC(T*) { return false; }
    bool CreateCompatibleDC(std::nullptr_t) { return false; }
    bool DeleteDC() { return true; }

    CPalette* SelectPalette(CPalette*,BOOL) { return nullptr; }
    UINT RealizePalette() { return 0; }

    int  SaveDC() { return 0; }
    bool RestoreDC(int) { return false; }

    BOOL m_bPrinting = FALSE;
    BOOL IsPrinting() const { return m_bPrinting; }
    int  SetStretchBltMode(int) { return 0; }
    int  GetStretchBltMode() const { return 0; }

    BOOL DPtoLP(LPPOINT, int=1) const { return TRUE; }
    BOOL DPtoLP(LPRECT) const { return TRUE; }
    BOOL DPtoLP(LPSIZE) const { return TRUE; }
    BOOL DPtoLP(CPoint* p) const { (void)p; return TRUE; }
    BOOL DPtoLP(CRect*  r) const { (void)r; return TRUE; }
    BOOL LPtoDP(LPPOINT, int=1) const { return TRUE; }
    BOOL LPtoDP(LPRECT) const { return TRUE; }
    BOOL LPtoDP(CPoint* p) const { (void)p; return TRUE; }

    bool Ellipse(const CRect& r)               { return Ellipse(r.left,r.top,r.right,r.bottom); }
    bool RoundRect(const CRect& r, CPoint pt)  { return RoundRect(r.left,r.top,r.right,r.bottom,pt.x,pt.y); }
    bool Chord(const CRect& r,CPoint p1,CPoint p2) { return Chord(r.left,r.top,r.right,r.bottom,p1.x,p1.y,p2.x,p2.y); }
    bool Pie(int,int,int,int,int,int,int,int) { return false; }
    bool Pie(const CRect&,CPoint,CPoint)       { return false; }
    bool InvertRect(const RECT*)               { return false; }
    bool InvertRect(const CRect*)              { return false; }
    bool ExtFloodFill(int,int,COLORREF,UINT)   { return false; }
    BOOL SetPixelV(int,int,COLORREF)           { return FALSE; }
    COLORREF GetPixel(int,int) const           { return 0; }
    int  SetPolyFillMode(int)                  { return 0; }
    int  GetPolyFillMode() const               { return 0; }
    int  GetTextFace(CString& s) const         { s = ""; return 0; }
    int  SetGraphicsMode(int)                  { return 0; }
    BOOL SetWorldTransform(const void*)        { return FALSE; }
    BOOL ModifyWorldTransform(const void*,DWORD) { return FALSE; }
    BOOL GradientFill(void*,DWORD,void*,DWORD,DWORD) { return FALSE; }
    CFont*   GetCurrentFont()   const { return nullptr; }
    CBrush*  GetCurrentBrush()  const { return nullptr; }
    CPen*    GetCurrentPen()    const { return nullptr; }
    CBitmap* GetCurrentBitmap() const { return nullptr; }
};

#else  // MUSHCLIENT_FULL_BUILD — declarations only; impl in wx_gdi_compat.cpp

class CDC {
public:
    HDC m_hDC       = nullptr;
    HDC m_hAttribDC = nullptr;
    void* m_impl    = nullptr;   // CDCImpl* — wx-backed state
    BOOL  m_bPrinting = FALSE;

    virtual ~CDC();

    HDC GetSafeHdc() const { return m_hDC; }
    operator HDC() const { return m_hDC; }
    static CDC* FromHandle(HDC hDC);

    // GDI object selection
    CGdiObject* SelectObject(CGdiObject*);
    CPen*    SelectObject(CPen*);
    CBrush*  SelectObject(CBrush*);
    CFont*   SelectObject(CFont*);
    CBitmap* SelectObject(CBitmap*);
    CGdiObject* SelectStockObject(int nIndex);
    HGDIOBJ SelectObject(HGDIOBJ) { return nullptr; }

    // Clipping
    int  SelectClipRgn(CRgn*) { return 0; }
    int  ExcludeClipRect(int,int,int,int) { return 0; }
    int  IntersectClipRect(int,int,int,int) { return 0; }
    int  IntersectClipRect(const CRect& r) { return 0; }
    int  ExcludeClipRect(LPCRECT) { return 0; }
    bool GetClipBox(RECT*) { return false; }

    // Drawing primitives
    bool Rectangle(int x1, int y1, int x2, int y2);
    bool Rectangle(LPCRECT lpRect);
    bool Ellipse(int x1, int y1, int x2, int y2);
    bool Ellipse(const CRect& r);
    bool RoundRect(int x1, int y1, int x2, int y2, int x3, int y3);
    bool RoundRect(const CRect& r, CPoint pt);
    bool LineTo(int x, int y);
    bool MoveTo(int x, int y, POINT* lpPoint = nullptr);
    bool Polyline(const POINT* lpPoints, int nCount);
    bool Polygon(const POINT* lpPoints, int nCount);
    bool PolyBezier(const POINT* lpPoints, int nCount);
    bool Arc(int x1,int y1,int x2,int y2,int x3,int y3,int x4,int y4);
    bool Chord(int x1,int y1,int x2,int y2,int x3,int y3,int x4,int y4);
    bool Chord(const CRect& r, CPoint p1, CPoint p2);
    bool Pie(int x1,int y1,int x2,int y2,int xS,int yS,int xE,int yE);
    bool Pie(const CRect& r, CPoint p1, CPoint p2);
    bool FloodFill(int x, int y, COLORREF crColor);
    bool ExtFloodFill(int x, int y, COLORREF crColor, UINT nFillType);
    void FillRect(LPCRECT lpRect, CBrush* pBrush);
    void FillRect(const CRect* pRect, CBrush* pBrush);
    void FrameRect(LPCRECT lpRect, CBrush* pBrush);
    void FillSolidRect(LPCRECT lpRect, COLORREF clr);
    void FillSolidRect(const CRect* lpRect, COLORREF clr);
    void FillSolidRect(int x, int y, int cx, int cy, COLORREF clr);
    void Draw3dRect(LPCRECT lpRect, COLORREF clrTopLeft, COLORREF clrBottomRight);
    void Draw3dRect(int x,int y,int cx,int cy,COLORREF clrTopLeft,COLORREF clrBottomRight);
    void DrawEdge(LPRECT lpRect, UINT nEdge, UINT nFlags);
    void DrawFocusRect(LPCRECT lpRect) {}
    bool PatBlt(int x, int y, int nWidth, int nHeight, DWORD dwRop);
    bool BitBlt(int x, int y, int nWidth, int nHeight,
                CDC* pSrcDC, int xSrc, int ySrc, DWORD dwRop);
    bool StretchBlt(int x, int y, int nWidth, int nHeight,
                    CDC* pSrcDC, int xSrc, int ySrc, int nSrcWidth, int nSrcHeight,
                    DWORD dwRop);
    bool MaskBlt(int,int,int,int,CDC*,int,int,HBITMAP,int,int,DWORD) { return false; }
    bool ScrollDC(int,int,LPCRECT,LPCRECT,CRgn*,LPRECT) { return false; }
    bool DrawDragRect(LPCRECT,SIZE,LPCRECT,SIZE,CBrush* =nullptr,CBrush* =nullptr) { return false; }
    void DrawIcon(int,int,HICON) {}
    void DrawIcon(CPoint,HICON) {}
    BOOL SetPixelV(int x, int y, COLORREF crColor);
    COLORREF GetPixel(int x, int y) const;
    bool InvertRect(const RECT* lpRect);
    bool InvertRect(const CRect* lpRect);

    // Text output
    bool TextOut(int x, int y, LPCTSTR lpszString, int nCount = -1);
    bool TextOut(int x, int y, const CString& str);
    bool ExtTextOut(int x, int y, UINT nOptions, LPCRECT lpRect,
                    LPCTSTR lpszString, int nCount, const int* lpDxWidths);
    int  DrawText(LPCTSTR lpszString, int nCount, LPRECT lpRect, UINT nFormat);
    int  DrawText(const CString& str, LPRECT lpRect, UINT nFormat);
    int  DrawTextEx(LPTSTR,int,LPRECT,UINT,void*) { return 0; }
    bool GrayString(CBrush*,BOOL(*)(HDC,LPARAM,int),LPARAM,int,int,int,int,int) { return false; }
    bool TabbedTextOut(int,int,LPCTSTR,int,int,LPINT,int) { return false; }

    // Text / colour attributes
    COLORREF SetTextColor(COLORREF crColor);
    COLORREF GetTextColor() const;
    COLORREF SetBkColor(COLORREF crColor);
    COLORREF GetBkColor() const;
    int      SetBkMode(int nBkMode);
    int      GetBkMode() const;
    int      SetROP2(int nDrawMode);
    int      GetROP2() const;
    UINT     SetTextAlign(UINT nFlags);
    UINT     GetTextAlign() const;
    int      SetTextCharacterExtra(int nCharExtra);

    // Metrics / info
    bool  GetTextMetrics(TEXTMETRIC* lpMetrics);
    CSize GetTextExtent(LPCTSTR lpszString, int nCount = -1);
    CSize GetTextExtent(const CString& str);
    CSize GetOutputTextExtent(LPCTSTR lpszString, int nCount = -1);
    CSize GetOutputTextExtent(const CString& str);
    int   GetDeviceCaps(int nIndex);
    bool  GetTextFace(int nCount, LPTSTR lpszFacename) { return false; }
    int   GetTextFace(CString& rString) const;

    // Viewport / window origin
    CPoint SetViewportOrg(int x, int y);
    CPoint SetWindowOrg(int x, int y);
    CSize  SetViewportExt(int,int) { return {}; }
    CSize  SetWindowExt(int,int) { return {}; }
    CPoint OffsetViewportOrg(int,int) { return {}; }
    bool   SetMapMode(int) { return false; }

    // Compatible DC creation (pDC == nullptr → screenless MemDC; HDC/void* ignored)
    bool CreateCompatibleDC(CDC* pDC = nullptr);
    template<class T> bool CreateCompatibleDC(T*) { return CreateCompatibleDC((CDC*)nullptr); }
    bool DeleteDC();

    // Palette
    CPalette* SelectPalette(CPalette*,BOOL) { return nullptr; }
    UINT RealizePalette() { return 0; }

    // Save / restore DC state
    int  SaveDC();
    bool RestoreDC(int nSavedDC);

    // Printing
    BOOL IsPrinting() const { return m_bPrinting; }
    int  SetStretchBltMode(int nStretchMode);
    int  GetStretchBltMode() const;

    // Poly fill mode
    int SetPolyFillMode(int nPolyFillMode);
    int GetPolyFillMode() const;

    // Coordinate transforms (identity — no scaling)
    BOOL DPtoLP(LPPOINT, int=1) const { return TRUE; }
    BOOL DPtoLP(LPRECT)  const { return TRUE; }
    BOOL DPtoLP(LPSIZE)  const { return TRUE; }
    BOOL DPtoLP(CPoint*) const { return TRUE; }
    BOOL DPtoLP(CRect*)  const { return TRUE; }
    BOOL LPtoDP(LPPOINT, int=1) const { return TRUE; }
    BOOL LPtoDP(LPRECT)  const { return TRUE; }
    BOOL LPtoDP(CPoint*) const { return TRUE; }

    // GDI transforms (world transforms — not implemented)
    int  SetGraphicsMode(int) { return 0; }
    BOOL SetWorldTransform(const void*) { return FALSE; }
    BOOL ModifyWorldTransform(const void*, DWORD) { return FALSE; }

    // Gradient fill (stub — complex, rarely used)
    BOOL GradientFill(void*, DWORD, void*, DWORD, DWORD) { return FALSE; }

    // Current GDI object accessors
    CFont*   GetCurrentFont()   const;
    CBrush*  GetCurrentBrush()  const;
    CPen*    GetCurrentPen()    const;
    CBitmap* GetCurrentBitmap() const;
};

#endif  // MUSHCLIENT_FULL_BUILD / CDC declaration

// Inline the CString overload for the mushcore (non-full-build) branch.
// In the full-build branch CDC::TextOut(int,int,const CString&) is declared
// above and implemented out-of-line in wx_gdi_compat.cpp.
#ifndef MUSHCLIENT_FULL_BUILD
inline bool CDC::TextOut(int x, int y, const CString& s) {
    return TextOut(x, y, (LPCTSTR)s, s.GetLength());
}
#endif

// CClientDC / CWindowDC / CPaintDC — paint-context helpers
class CClientDC : public CDC {
public:
    explicit CClientDC(CWnd*) {}
};
class CWindowDC : public CDC {
public:
    explicit CWindowDC(CWnd*) {}
};
class CPaintDC : public CDC {
public:
    explicit CPaintDC(CWnd*) {}
};
class CMemDC : public CDC {
public:
    CMemDC(CDC*, CRect*) {}
};

// ── CPrintInfo ────────────────────────────────────────────────────────────
class CPrintInfo {
public:
    CDC*  m_pDC       = nullptr;
    UINT  m_nCurPage  = 1;
    bool  m_bPreview  = false;
    bool  m_bDirect   = false;
    bool  m_bDocObject= false;
    UINT  GetMinPage() const { return 1; }
    UINT  GetMaxPage() const { return 1; }
    void  SetMinPage(UINT) {}
    void  SetMaxPage(UINT) {}
    bool  GetDirect() const { return m_bDirect; }
};

// ── CDumpContext ──────────────────────────────────────────────────────────
class CDumpContext {
public:
    CDumpContext& operator<<(LPCTSTR)         { return *this; }
    CDumpContext& operator<<(int)             { return *this; }
    CDumpContext& operator<<(long)            { return *this; }
    CDumpContext& operator<<(unsigned long)   { return *this; }
    CDumpContext& operator<<(double)          { return *this; }
    CDumpContext& operator<<(const CObject*)  { return *this; }
    CDumpContext& operator<<(const CString&)  { return *this; }
    CDumpContext& operator<<(const void*)     { return *this; }
    void SetDepth(int) {}
};

// ── CArchiveException ─────────────────────────────────────────────────────
// (may also be in mfc_exceptions.h — guard against double-definition)
#ifndef CARCHIVE_EXCEPTION_DEFINED
#  define CARCHIVE_EXCEPTION_DEFINED
class CArchiveException : public CException {
public:
    enum { none=0, genericException, readOnly, endOfFile, writeOnly,
           badIndex, badClass, badSchema };
    int m_cause = 0;
    void Delete() { delete this; }
};
#endif

// Real thrower (forward-declared in windows_types.h): throw a CArchiveException*
// so the catch (CArchiveException*) handlers in LoadPlugin / Serialize / the XML
// import paths actually catch it, instead of an unhandled std::runtime_error
// terminating the whole application (e.g. when a world or plugin has a bad schema
// or uses an unsupported script language).  Both builds: mushcore drives the
// REAL loader (xml/xml_load_world.cpp) now, so its archive-exception contract
// must hold headless too.
inline void AfxThrowArchiveException(int cause, LPCTSTR /*name*/) {
    CArchiveException* e = new CArchiveException();
    e->m_cause = cause;
    throw e;
}

// ── CFileStatus ────────────────────────────────────────────────────────────
// Defined here (before CFile) so CFile::GetStatus can populate it inline.
// The duplicate guard at the bottom of the file keeps it idempotent.
#ifndef CFileStatus_defined
#define CFileStatus_defined
struct CFileStatus {
    CTime    m_ctime;
    CTime    m_mtime;
    CTime    m_atime;
    LONG     m_size      = 0;
    BYTE     m_attribute = 0;
    BYTE     _m_padding  = 0;
    TCHAR    m_szFullName[_MAX_PATH] = {};
};
#endif

// ── CFile ─────────────────────────────────────────────────────────────────
class CFile {
public:
    enum OpenFlags {
        modeRead        = 0x0000,
        modeWrite       = 0x0001,
        modeReadWrite   = 0x0002,
        shareCompat     = 0x0000,
        shareExclusive  = 0x0010,
        shareDenyWrite  = 0x0020,
        shareDenyRead   = 0x0030,
        shareDenyNone   = 0x0040,
        modeNoInherit   = 0x0080,
        modeCreate      = 0x1000,
        modeNoTruncate  = 0x2000,
        typeText        = 0x4000,
        typeBinary      = 0x8000
    };
    enum SeekPos { begin=0, current=1, end=2 };
    // File attribute constants (exposed as CFile::readOnly etc on Windows)
    enum Attribute {
        normal    = 0x00,
        readOnly  = 0x01,
        hidden    = 0x02,
        system    = 0x04,
        volume    = 0x08,
        directory = 0x10,
        archive   = 0x20
    };

protected:
    FILE*   m_fp      = nullptr;
    CString m_strPath;

    // Translate MFC OpenFlags bitmask to fopen mode string.
    static const char* FlagsToMode(UINT flags) {
        bool write = (flags & modeWrite) || (flags & modeReadWrite);
        bool read  = !(flags & modeWrite);
        bool create = (flags & modeCreate);
        if (create && write)  return (flags & modeReadWrite) ? "w+b" : "wb";
        if (write && read)    return "r+b";
        if (write)            return "r+b";  // plain modeWrite: open existing for write at pos 0 (MFC), not append
        return "rb";   // default: read-only
    }

    // Translate Windows-style backslash separators to POSIX '/'.  MUSHclient and
    // many plugins embed backslash paths (e.g. ".\\worlds\\plugins\\state\\");
    // POSIX treats '\\' as an ordinary filename character, so normalize before
    // any fopen()/stat()/rename()/remove() call.  Kept self-contained so this
    // header has no include-ordering dependency on compat.h's PortableFilePath().
    static std::string PosixPath(LPCTSTR path) {
        std::string s(path ? path : "");
        for (std::string::size_type i = 0; i < s.size(); ++i)
            if (s[i] == '\\') s[i] = '/';
        return s;
    }

public:
    CFile() = default;

    // Open file by path + flags.  MFC-faithful: THROWS a heap CFileException*
    // on failure (m_cause/m_lOsError/m_strFileName populated) — every canonical
    // construction site wraps this in try/catch (CFileException*/CException*),
    // so files no longer just silently fail to open.  Callers that want the
    // non-throwing form use the default ctor + Open(), which returns FALSE.
    CFile(LPCTSTR path, UINT flags) {
        CFileException e;
        if (!Open(path, flags, &e))
            AfxThrowFileException(e.m_cause, e.m_lOsError, path);
    }

    virtual ~CFile() { if (m_fp) { fclose(m_fp); m_fp = nullptr; } }

    // Non-copyable (owns FILE*)
    CFile(const CFile&)            = delete;
    CFile& operator=(const CFile&) = delete;

    // MFC signature: returns FALSE on failure (never throws); fills *pError
    // (a caller-owned CFileException, NOT thrown) when supplied.
    virtual BOOL Open(LPCTSTR path, UINT flags, CFileException* pError = nullptr) {
        if (m_fp) { fclose(m_fp); m_fp = nullptr; }
        const char* mode = FlagsToMode(flags);
        m_fp = fopen(PosixPath(path).c_str(), mode);
        if (m_fp) { m_strPath = path; return TRUE; }
        if (pError) {
            pError->m_cause    = CFileException::ErrnoToException(errno);
            pError->m_lOsError = errno;
            pError->m_strFileName = path;
        }
        return FALSE;
    }
    virtual void Close() { if (m_fp) { fclose(m_fp); m_fp = nullptr; } }
    virtual UINT Read(void* buf, UINT count) {
        if (!m_fp) return 0;
        return (UINT)fread(buf, 1, count, m_fp);
    }
    virtual void Write(const void* buf, UINT count) {
        if (m_fp) fwrite(buf, 1, count, m_fp);
    }
    virtual LONG Seek(LONG offset, UINT from) {
        if (!m_fp) return 0;
        int whence = (from == begin) ? SEEK_SET : (from == current) ? SEEK_CUR : SEEK_END;
        fseek(m_fp, offset, whence);
        return (LONG)ftell(m_fp);
    }
    virtual DWORD GetLength() const {
        if (!m_fp) return 0;
        long pos = ftell(m_fp);
        fseek(m_fp, 0, SEEK_END);
        long len = ftell(m_fp);
        fseek(m_fp, pos, SEEK_SET);
        return (DWORD)len;
    }
    virtual DWORD GetPosition() const {
        if (!m_fp) return 0;
        return (DWORD)ftell(m_fp);
    }
    virtual void SetLength(DWORD) {}
    virtual void Flush() { if (m_fp) fflush(m_fp); }
    virtual void Abort() { if (m_fp) { fclose(m_fp); m_fp = nullptr; } }
    bool IsOpen() const { return m_fp != nullptr; }

    void SeekToBegin() { Seek(0, begin); }
    void SeekToEnd()   { Seek(0, end); }

    CString GetFileName() const {
        // Return just the filename portion of the path
        const char* s = m_strPath;
        const char* slash = strrchr(s, '/');
        return slash ? CString(slash + 1) : m_strPath;
    }
    CString GetFilePath() const { return m_strPath; }
    void SetFilePath(LPCTSTR path) { m_strPath = path; }

    static bool Rename(LPCTSTR from, LPCTSTR to) { return ::rename(PosixPath(from).c_str(), PosixPath(to).c_str()) == 0; }
    static bool Remove(LPCTSTR path) { return ::remove(PosixPath(path).c_str()) == 0; }

    // Populate a CFileStatus from POSIX stat().  Returns true on success.
    static bool GetStatus(LPCTSTR path, CFileStatus& st) {
        struct ::stat s;
        if (!path || ::stat(PosixPath(path).c_str(), &s) != 0) return false;
        st.m_ctime = CTime((time_t)s.st_ctime);
        st.m_mtime = CTime((time_t)s.st_mtime);
        st.m_atime = CTime((time_t)s.st_atime);
        st.m_size  = (LONG)s.st_size;
        st.m_attribute = S_ISDIR(s.st_mode) ? 0x10 :
                         !(s.st_mode & S_IWUSR) ? 0x01 : 0x00;
        strncpy(st.m_szFullName, path, _MAX_PATH - 1);
        st.m_szFullName[_MAX_PATH - 1] = '\0';
        return true;
    }
    static bool GetStatus(LPCTSTR, void*) { return false; }
};

class CStdioFile : public CFile {
public:
    CStdioFile() = default;
    CStdioFile(LPCTSTR path, UINT mode) : CFile(path, mode) {}
    CStdioFile(FILE* fp) { m_fp = fp; }  // attach to existing FILE*

    bool ReadString(CString& rString) {
        if (!m_fp) return false;
        rString.Empty();
        int c;
        while ((c = fgetc(m_fp)) != EOF) {
            if (c == '\n') return true;
            rString += (char)c;
        }
        return !rString.IsEmpty();
    }
    bool ReadString(LPTSTR buf, UINT max) {
        if (!m_fp || max == 0) return false;
        return fgets(buf, (int)max, m_fp) != nullptr;
    }
    void WriteString(LPCTSTR str) {
        if (m_fp) fputs(str, m_fp);
    }
};

// ── CMemFile ──────────────────────────────────────────────────────────────
// An in-memory CFile backed by a std::vector<BYTE>.  Supports two modes:
//
//  1. Writable (default constructor): starts empty, grows as data is written.
//     Detach() transfers ownership of a malloc'd copy to the caller.
//
//  2. Read-only (buffer constructor): wraps an external byte array.  Writes
//     are silently ignored; Detach() returns nullptr.
//
// Used by ImportXML / ExportXML Lua APIs, clipboard XML paste detection, and
// several world-debug helpers.
class CMemFile : public CFile {
public:
    // Default: empty writable in-memory file.
    explicit CMemFile(UINT /*nGrowBytes*/ = 1024)
        : m_readBuf(nullptr), m_readBufSize(0), m_pos(0), m_ownsBuf(true) {}

    // Attach to an existing external buffer (read-only semantics).
    CMemFile(BYTE* lpBuffer, UINT nBufferSize, UINT /*nGrowBytes*/ = 0)
        : m_readBuf(lpBuffer), m_readBufSize(nBufferSize),
          m_pos(0), m_ownsBuf(false) {}

    // CFile interface --------------------------------------------------------
    UINT Read(void* buf, UINT count) override {
        UINT avail = GetLength() - m_pos;
        UINT n = count < avail ? count : avail;
        if (n > 0) {
            memcpy(buf, (m_readBuf ? m_readBuf : m_buf.data()) + m_pos, n);
            m_pos += n;
        }
        return n;
    }

    void Write(const void* buf, UINT count) override {
        if (!m_ownsBuf || !buf || count == 0) return;
        const BYTE* p = static_cast<const BYTE*>(buf);
        m_buf.insert(m_buf.end(), p, p + count);
    }

    LONG Seek(LONG offset, UINT from) override {
        LONG size = (LONG)GetLength();
        LONG newPos;
        if      (from == begin)   newPos = offset;
        else if (from == current) newPos = (LONG)m_pos + offset;
        else                      newPos = size + offset;
        if (newPos < 0) newPos = 0;
        if (newPos > size) newPos = size;
        m_pos = (UINT)newPos;
        return (LONG)m_pos;
    }

    DWORD GetLength() const override {
        return m_readBuf ? m_readBufSize : (DWORD)m_buf.size();
    }
    DWORD GetPosition() const override { return m_pos; }

    void SeekToBegin() { m_pos = 0; }
    void SeekToEnd()   { m_pos = GetLength(); }
    void Close() override {}   // nothing to close for an in-memory file
    void Flush() override {}

    // CMemFile-specific ------------------------------------------------------
    // Transfers ownership of the internal buffer to the caller as a malloc'd
    // block (callers do free(p)).  Returns nullptr for read-only instances.
    BYTE* Detach() {
        if (!m_ownsBuf) return nullptr;
        BYTE* p = static_cast<BYTE*>(malloc(m_buf.size() + 1));
        if (p) {
            if (!m_buf.empty()) memcpy(p, m_buf.data(), m_buf.size());
            p[m_buf.size()] = '\0';   // null-terminate for string callers
        }
        m_buf.clear();
        m_pos = 0;
        return p;
    }

    // Returns a pointer into the internal buffer at the current read position.
    BYTE* GetMemoryPos() const {
        return m_readBuf ? m_readBuf + m_pos
                         : (m_buf.empty() ? nullptr
                                          : const_cast<BYTE*>(m_buf.data()) + m_pos);
    }

private:
    std::vector<BYTE> m_buf;       // owned writable buffer (mode 1)
    BYTE*             m_readBuf;   // external read-only buffer (mode 2)
    UINT              m_readBufSize;
    UINT              m_pos;
    bool              m_ownsBuf;
};

// ── CArchive ──────────────────────────────────────────────────────────────
// Non-Windows implementation: stores the backing CFile* and mode so that
// callers like Load_World_XML can call ar.GetFile()->GetFilePath() and
// BuildStructure(ar.GetFile()) with a real CFile that has working I/O.
class CArchive {
public:
    enum Mode { load = 0, store = 1 };
    CArchive(CFile* pFile, Mode mode, int = 4096, void* = nullptr)
        : m_pFile(pFile), m_mode(mode) {}
    ~CArchive() {}
    bool IsLoading() const { return m_mode == load; }
    bool IsStoring() const { return m_mode == store; }
    void Close() {}
    void Flush() {}
    CFile* GetFile() const { return m_pFile; }
    CString m_strFileName;

    CArchive& operator<<(BYTE)      { return *this; }
    CArchive& operator<<(WORD)      { return *this; }
    CArchive& operator<<(DWORD)     { return *this; }
    CArchive& operator<<(int)       { return *this; }
    CArchive& operator<<(long)      { return *this; }
    CArchive& operator<<(float)     { return *this; }
    CArchive& operator<<(double)    { return *this; }
    CArchive& operator<<(bool)      { return *this; }
    CArchive& operator<<(const CString&) { return *this; }
    CArchive& operator<<(const CObject*) { return *this; }
    CArchive& operator<<(const POINT&)   { return *this; }
    CArchive& operator<<(const RECT&)    { return *this; }

    CArchive& operator>>(BYTE&)      { return *this; }
    CArchive& operator>>(WORD&)      { return *this; }
    CArchive& operator>>(DWORD&)     { return *this; }
    CArchive& operator>>(int&)       { return *this; }
    CArchive& operator>>(long&)      { return *this; }
    CArchive& operator>>(float&)     { return *this; }
    CArchive& operator>>(double&)    { return *this; }
    CArchive& operator>>(bool&)      { return *this; }
    CArchive& operator>>(CString&)   { return *this; }
    CArchive& operator>>(CObject*&)  { return *this; }
    CArchive& operator>>(POINT&)     { return *this; }
    CArchive& operator>>(RECT&)      { return *this; }

    void Read(void* buf, UINT count) {
        if (m_pFile && buf && count) m_pFile->Read(buf, count);
    }
    void Write(const void* buf, UINT count) {
        if (m_pFile && buf && count) m_pFile->Write(buf, count);
    }
    BOOL ReadString(CString& str) {
        if (!m_pFile) return FALSE;
        str.Empty();
        char ch;
        while (m_pFile->Read(&ch, 1) == 1) {
            if (ch == '\n') return TRUE;
            if (ch != '\r') str += ch;
        }
        return !str.IsEmpty();
    }
    bool ReadString(LPTSTR buf, UINT max) {
        if (!m_pFile || !buf || max == 0) return false;
        UINT i = 0;
        char ch;
        while (i + 1 < max && m_pFile->Read(&ch, 1) == 1) {
            if (ch == '\n') break;
            if (ch != '\r') buf[i++] = ch;
        }
        buf[i] = '\0';
        return i > 0;
    }
    void WriteString(LPCTSTR str) {
        if (m_pFile && str) m_pFile->Write(str, (UINT)strlen(str));
    }

private:
    CFile* m_pFile = nullptr;
    Mode   m_mode  = load;
};

// ── CWnd extension ────────────────────────────────────────────────────────
// CWnd is defined in compat.h.  Add more methods here via a forward extension.
// To avoid redefinition we use a helper that compat.h's CWnd can inline:
//
//   Already in compat.h CWnd:
//     GetSafeHwnd, IsWindow, SendMessage, PostMessage,
//     Invalidate, UpdateWindow, SetWindowText, GetWindowText,
//     ShowWindow, EnableWindow, SetFocus, SetTimer, KillTimer

// ── CCmdTarget ────────────────────────────────────────────────────────────
// Minimal base is defined in compat.h (must precede CWnd).
// Add MFC automation/cursor methods here by reopening the class:
// (C++ does not allow reopening, so they live in the compat.h definition.)
// Additional global-scope helpers that code uses as CCmdTarget members are
// added as free functions where needed.

// ── CDocTemplate ─────────────────────────────────────────────────────────
class CDocTemplate {
public:
    virtual ~CDocTemplate() {}
    virtual CDocument* OpenDocumentFile(LPCTSTR) { return nullptr; }
};

class CMultiDocTemplate : public CDocTemplate {
public:
    HACCEL m_hAccelTable = nullptr;  // Windows stores accel table here

    // Real document list so FindNotepad / GetNotepadList can iterate.
    std::vector<CDocument*> m_docs;

    // MFC-real registry names (CDocTemplate::AddDocument/RemoveDocument on
    // Windows; the invented RegisterDoc/UnregisterDoc are gone — Phase 2
    // Stage 2).  Docs register here so the GetFirstDocPosition/GetNextDoc
    // iteration below finds them.
    // Also records the owning template on the document, as MFC does, so
    // CDocument::GetDocTemplate() can return it.  It used to return nullptr
    // unconditionally, which made CMUSHclientDoc::DoSave's empty-path arm
    // (doc.cpp:2204) trip its ASSERT.
    virtual void AddDocument(CDocument* d);
    virtual void RemoveDocument(CDocument* d) {
        auto it = std::find(m_docs.begin(), m_docs.end(), d);
        if (it != m_docs.end()) m_docs.erase(it);
    }

    CMultiDocTemplate(UINT, const CRuntimeClass*, const CRuntimeClass*, const CRuntimeClass*) {}

    // Document iteration — backed by m_docs; returns nullptr when empty.
    POSITION GetFirstDocPosition() const {
        return m_docs.empty() ? nullptr : (POSITION)(uintptr_t)1;
    }
    CDocument* GetNextDoc(POSITION& pos) const {
        if (!pos) return nullptr;
        size_t idx = (size_t)(uintptr_t)pos - 1;
        if (idx >= m_docs.size()) { pos = nullptr; return nullptr; }
        pos = (idx + 1 < m_docs.size()) ? (POSITION)(uintptr_t)(idx + 2) : nullptr;
        return m_docs[idx];
    }

    // Override OpenDocumentFile to route through the appropriate wxWidgets hook:
    //   • path == nullptr / empty → Quick Connect dialog (new world)
    //   • path provided           → open existing world file
    virtual CDocument* OpenDocumentFile(LPCTSTR path) override {
        if (path && *path) {
            if (g_wxhook_open_world_file)
                return static_cast<CDocument*>(g_wxhook_open_world_file(path));
        } else {
            if (g_wxhook_new_world)
                return static_cast<CDocument*>(g_wxhook_new_world());
        }
        return nullptr;
    }
};

class CSingleDocTemplate : public CDocTemplate {
public:
    CSingleDocTemplate(UINT, const CRuntimeClass*, const CRuntimeClass*, const CRuntimeClass*) {}
};

// ── CDocument ─────────────────────────────────────────────────────────────
// Forward-declare CView so CDocument can hold a vector of them.
class CView;

class CDocument : public CCmdTarget {
    SHIM_RUNTIME_CLASS(CDocument, CCmdTarget)
public:
    CString m_strTitle;
    CString m_strPathName;
    bool    m_bModified = false;
    // Set by CDocTemplate::AddDocument (as MFC does).  GetDocTemplate() below
    // returns it; canonical code ASSERTs it is non-null.
    CDocTemplate* m_pDocTemplate = nullptr;

    // View list — backed by a std::vector; POSITION is void* index+1
    std::vector<CView*> m_viewList;

    virtual ~CDocument() {}

    POSITION GetFirstViewPosition() const {
        return m_viewList.empty() ? nullptr : (POSITION)1;
    }
    CView* GetNextView(POSITION& pos) const {
        if (!pos) return nullptr;
        size_t idx = (size_t)(uintptr_t)pos - 1;
        if (idx >= m_viewList.size()) { pos = nullptr; return nullptr; }
        pos = (idx + 1 < m_viewList.size()) ? (POSITION)(uintptr_t)(idx+2) : nullptr;
        return m_viewList[idx];
    }
    void AddView(CView* pView) { m_viewList.push_back(pView); }
    void RemoveView(CView* pView) {
        auto it = std::find(m_viewList.begin(), m_viewList.end(), pView);
        if (it != m_viewList.end()) m_viewList.erase(it);
    }

    void UpdateAllViews(CView* /*sender*/, LPARAM /*hint*/=0L, CObject* /*pHint*/=nullptr) {}

    void    SetTitle(LPCTSTR title) { m_strTitle = title; }
    CString GetTitle() const        { return m_strTitle; }
    void    SetPathName(LPCTSTR path, BOOL /*bAddMRU*/=TRUE) { m_strPathName = path; }
    CString GetPathName() const     { return m_strPathName; }
    void    SetModifiedFlag(BOOL mod=TRUE) { m_bModified = (mod!=FALSE); }
    BOOL    IsModified() const      { return m_bModified ? TRUE : FALSE; }
    CDocTemplate* GetDocTemplate() const { return m_pDocTemplate; }

    virtual BOOL OnNewDocument()            { return TRUE; }
    // Open a world file: opens the CFile, wraps it in a CArchive (load mode),
    // and calls Serialize() so that CMUSHclientDoc::Serialize → Load_World_XML
    // can parse the XML using the real CFile* for I/O.
    virtual BOOL OnOpenDocument(LPCTSTR lpszPathName) {
        CFile* f = new CFile();
        if (!f->Open(lpszPathName, CFile::modeRead | CFile::shareDenyWrite)) {
            delete f;
            return FALSE;
        }
        BOOL ok = TRUE;
        try {
            CArchive ar(f, CArchive::load);
            Serialize(ar);
            ar.Close();
        } catch (CException* e) {
            e->Delete();
            ok = FALSE;
        } catch (...) {
            ok = FALSE;
        }
        f->Close();
        delete f;
        return ok;
    }
    // Save the document to a file.  Opens the path for writing, wraps it in
    // a CArchive (store mode), and calls Serialize() so Save_World_XML can
    // write the XML via ar.WriteString().
    virtual BOOL OnSaveDocument(LPCTSTR lpszPathName) {
        CFile* f = new CFile();
        UINT flags = CFile::modeWrite | CFile::modeCreate | CFile::shareDenyWrite;
        if (!f->Open(lpszPathName, flags)) {
            delete f;
            return FALSE;
        }
        BOOL ok = TRUE;
        try {
            CArchive ar(f, CArchive::store);
            Serialize(ar);
            ar.Close();
        } catch (CException* e) {
            e->Delete();
            ok = FALSE;
        } catch (...) {
            ok = FALSE;
        }
        f->Flush();
        f->Close();
        delete f;
        return ok;
    }
    virtual void OnCloseDocument()          {}
    virtual void DeleteContents()           {}
    virtual void Serialize(CArchive&)       {}
    virtual void Dump(CDumpContext&) const  {}
    virtual void AssertValid() const        {}
    virtual BOOL SaveModified()             { return TRUE; }
    // Real MFC's CDocument::DoSave writes via OnSaveDocument (which Serialises).
    // The old no-op was fine while CMUSHclientDoc::DoSave was a headless inline
    // that called OnSaveDocument itself; the adopted doc.cpp::DoSave delegates to
    // THIS base, so it must do the write (else world.Save produces no file).
    virtual BOOL DoSave(LPCTSTR lpszPathName, BOOL /*bReplace*/=TRUE) {
        CString strName(lpszPathName ? lpszPathName : "");
        if (strName.IsEmpty())
            return FALSE;                  // no dialog to prompt with headless
        if (!OnSaveDocument(strName))
            return FALSE;
        SetPathName(strName);
        return TRUE;
    }
    virtual BOOL DoFileSave()               { return TRUE; }

    void SetChangedFlag(BOOL mod=TRUE) { SetModifiedFlag(mod); }
    // IsKindOf inherited from CObject
    // COM/Automation stub
    LPVOID GetIDispatch(BOOL=TRUE) { return nullptr; }

    // Command handlers — Windows MFC routing; no-ops on non-Windows.
    virtual void OnFileSave()   {}
    virtual void OnFileSaveAs() {}
    virtual void OnFileClose()  {}
};

// ── CView ─────────────────────────────────────────────────────────────────
class CView : public CWnd {
    SHIM_RUNTIME_CLASS(CView, CWnd)
public:
    CDocument* m_pDocument = nullptr;
    virtual CDocument* GetDocument() const { return m_pDocument; }

    // Standard MFC command handlers referenced by message maps (stubs on the
    // port — printing goes through wx / is unsupported here).
    void OnFilePrint() {}
    void OnFilePrintPreview() {}

    virtual void OnDraw(CDC*) {}
    virtual void OnInitialUpdate() {}
    virtual void OnUpdate(CView*, LPARAM, CObject*) {}
    virtual void OnBeginPrinting(CDC*, CPrintInfo*) {}
    virtual void OnEndPrinting(CDC*, CPrintInfo*) {}
    virtual void OnPrint(CDC*, CPrintInfo*) {}
    virtual void OnPrepareDC(CDC*, CPrintInfo* = nullptr) {}
    virtual BOOL OnPreparePrinting(CPrintInfo*) { return FALSE; }
    virtual BOOL DoPreparePrinting(CPrintInfo*) { return FALSE; }

    // Window/keyboard/mouse message handlers (overridable)
    virtual void OnChar(UINT, UINT, UINT) {}
    virtual void OnSysChar(UINT, UINT, UINT) {}
    virtual void OnKeyDown(UINT, UINT, UINT) {}
    virtual void OnKeyUp(UINT, UINT, UINT) {}
    virtual void OnSysKeyDown(UINT, UINT, UINT) {}
    virtual void OnSysKeyUp(UINT, UINT, UINT) {}
    virtual void OnSysCommand(UINT, LPARAM) {}
    virtual void OnLButtonDown(UINT, CPoint) {}
    virtual void OnLButtonUp(UINT, CPoint) {}
    virtual void OnLButtonDblClk(UINT, CPoint) {}
    virtual void OnRButtonDown(UINT, CPoint) {}
    virtual void OnRButtonUp(UINT, CPoint) {}
    virtual void OnRButtonDblClk(UINT, CPoint) {}
    virtual void OnMButtonDown(UINT, CPoint) {}
    virtual void OnMButtonUp(UINT, CPoint) {}
    virtual void OnMouseMove(UINT, CPoint) {}
    virtual BOOL OnMouseWheel(UINT, short, CPoint) { return FALSE; }
    virtual void OnSize(UINT, int, int) {}
    virtual void OnVScroll(UINT, UINT, CScrollBar*) {}
    virtual void OnHScroll(UINT, UINT, CScrollBar*) {}
    virtual void OnContextMenu(CWnd*, CPoint) {}
    virtual BOOL OnSetCursor(CWnd*, UINT, UINT) { return FALSE; }
    virtual void OnActivateView(BOOL, CView*, CView*) {}
    virtual BOOL PreCreateWindow(CREATESTRUCT&) { return TRUE; }
    virtual BOOL PreTranslateMessage(MSG*) { return FALSE; }
    virtual LRESULT WindowProc(UINT, WPARAM, LPARAM) { return 0; }

    // Scrollbar helpers
    void EnableScrollBarCtrl(int, BOOL=TRUE) {}
    void SetScrollInfo(int, SCROLLINFO*, BOOL=TRUE) {}
    BOOL GetScrollInfo(int, SCROLLINFO*) { return FALSE; }
    void ScrollWindow(int, int, const RECT* = nullptr, const RECT* = nullptr) {}

    CWnd* GetOwner() const { return GetParent(); }

    // IsKindOf inherited from CObject.  Geometry routes to the wx window via the
    // hooks (m_hWnd is the wxWindow*); CRect overloads delegate through RECT*
    // (CRect : public RECT), CPoint is handled separately (it is not a POINT).
    void GetClientRect(RECT* r) const { if (g_wxhook_get_client_rect && r) g_wxhook_get_client_rect((const void*) m_hWnd, r); else if (r) { r->left=r->top=r->right=r->bottom=0; } }
    void GetClientRect(CRect* r) const { GetClientRect((RECT*) r); }
    void GetClientRect(CRect& r) const { GetClientRect((RECT*) &r); }
    // Pointer overloads — must be repeated here or CWnd's versions are hidden
    void ClientToScreen(POINT* p) const { if (g_wxhook_client_to_screen && p) { int x=(int)p->x, y=(int)p->y; g_wxhook_client_to_screen((const void*) m_hWnd, &x, &y); p->x=x; p->y=y; } }
    void ClientToScreen(CPoint* p) const { if (g_wxhook_client_to_screen && p) { int x=p->x, y=p->y; g_wxhook_client_to_screen((const void*) m_hWnd, &x, &y); p->x=x; p->y=y; } }
    void ClientToScreen(CRect* r) const { if (g_wxhook_client_to_screen && r) { int l=(int)r->left,t=(int)r->top,ri=(int)r->right,b=(int)r->bottom; g_wxhook_client_to_screen((const void*) m_hWnd,&l,&t); g_wxhook_client_to_screen((const void*) m_hWnd,&ri,&b); r->left=l;r->top=t;r->right=ri;r->bottom=b; } }
    void ClientToScreen(CRect& r) const { ClientToScreen(&r); }
    void ScreenToClient(POINT* p) const { if (g_wxhook_screen_to_client && p) { int x=(int)p->x, y=(int)p->y; g_wxhook_screen_to_client((const void*) m_hWnd, &x, &y); p->x=x; p->y=y; } }
    void ScreenToClient(CPoint* p) const { if (g_wxhook_screen_to_client && p) { int x=p->x, y=p->y; g_wxhook_screen_to_client((const void*) m_hWnd, &x, &y); p->x=x; p->y=y; } }
    void ScreenToClient(CRect* r) const { if (g_wxhook_screen_to_client && r) { int l=(int)r->left,t=(int)r->top,ri=(int)r->right,b=(int)r->bottom; g_wxhook_screen_to_client((const void*) m_hWnd,&l,&t); g_wxhook_screen_to_client((const void*) m_hWnd,&ri,&b); r->left=l;r->top=t;r->right=ri;r->bottom=b; } }
    void ScreenToClient(CRect& r) const { ScreenToClient(&r); }
    void GetWindowRect(RECT* r) const { if (g_wxhook_get_window_rect && r) g_wxhook_get_window_rect((const void*) m_hWnd, r); else if (r) { r->left=r->top=r->right=r->bottom=0; } }
    void GetWindowRect(CRect* r) const { GetWindowRect((RECT*) r); }
    void GetWindowRect(CRect& r) const { GetWindowRect((RECT*) &r); }
    void Invalidate(BOOL=TRUE) {}
    void InvalidateRect(const RECT*, BOOL=TRUE) {}
    void InvalidateRect(const CRect*, BOOL=TRUE) {}
    // InvalidateRgn(CRgn*) — called as member with 1 arg in view code
    void InvalidateRgn(CRgn*, BOOL = TRUE) {}
    CFrameWnd* GetParentFrame() const { return nullptr; }

    // Scrollbar (3-arg form used in view code)
    BOOL GetScrollInfo(int, SCROLLINFO*, int) { return FALSE; }

    // AssertValid / Dump — MFC debug virtuals
    virtual void AssertValid() const {}
    virtual void Dump(CDumpContext&) const {}

    // Printing member
    BOOL m_bPrinting = FALSE;
};

class CScrollView : public CView {
    SHIM_RUNTIME_CLASS(CScrollView, CView)
public:
    enum { MM_NONE=0, MM_SCALETOFIT=-1 };
    void SetScrollSizes(int, CSize, CSize=CSize(), CSize=CSize()) {}
    void SetScaleToFitSize(CSize) {}
    void ScrollToPosition(CPoint) {}
    CPoint GetScrollPosition() const { return {}; }
    CSize  GetTotalSize() const { return {}; }
    void   OnScrollBy(CSize, BOOL=TRUE) {}
};

class CEdit;  // forward-declare; fully defined after CComboBox below
class CEditView : public CScrollView {
    SHIM_RUNTIME_CLASS(CEditView, CScrollView)
public:
    // Standard CEditView command handlers referenced by message maps (stubs).
    void OnEditSelectAll() {}
    void OnEditFind() {}
    void OnEditRepeat() {}
    void OnEditReplace() {}
    void OnUpdateNeedFind(CCmdUI*) {}   // referenced by CTextView's message map
    // CEditView wraps a CEdit internally; expose minimal surface
    void SetWindowText(LPCTSTR) {}
    void GetWindowText(CString&) const {}
    void SetReadOnly(BOOL=TRUE) {}
    // SetFont is inherited from CWnd (real, via g_wxhook_set_font) — don't shadow.
    int  GetLineCount() const { return 0; }
    // Returns the embedded CEdit control (MFC API).
    // Defined inline after CEdit is fully declared.
    CEdit& GetEditCtrl();
    void OnEditCopy() {}
    void OnUpdateNeedSel(CCmdUI*) {}
    void OnEditPaste() {}
    void OnEditCut() {}
    void OnEditUndo() {}
    virtual void OnDestroy() {}
    BOOL OnChange() { return TRUE; }
    // SetTabStops — MFC CEditView configures tab width on the embedded CEdit.
    void SetTabStops(int) {}
    // Archive helpers used by CEditView-based notepad/script views.
    void WriteToArchive(CArchive&) {}
    void ReadFromArchive(CArchive&, UINT) {}
    // Win95-specific shadow buffer members (never populated on non-Windows;
    // referenced inside bWin95 branches that are always false at runtime).
    char* m_pShadowBuffer = nullptr;
    int   m_nShadowSize   = 0;
};

// ── Open File Dialog flags (OFN_*) ─────────────────────────────────────────
// Defined here, before OPENFILENAME and CFileDialog, so inline methods can use
// them.  The duplicate guard at the bottom of the file is retained for safety.
#ifndef OFN_HIDEREADONLY
#  define OFN_HIDEREADONLY      0x00000004
#  define OFN_NOCHANGEDIR       0x00000008
#  define OFN_OVERWRITEPROMPT   0x00000002
#  define OFN_FILEMUSTEXIST     0x00001000
#  define OFN_PATHMUSTEXIST     0x00000800
#  define OFN_ALLOWMULTISELECT  0x00000200
#  define OFN_ENABLESIZING      0x00800000
#  define OFN_LONGNAMES         0x00200000
#  define OFN_EXPLORER          0x00080000
#  define OFN_CREATEPROMPT      0x00002000
#  define OFN_NOREADONLYRETURN  0x00008000
#  define OFN_NOTESTFILECREATE  0x00010000
#endif

// ── OPENFILENAME — must precede CFileDialog which holds an m_ofn member ──
struct OPENFILENAME {
    DWORD   lStructSize      = sizeof(OPENFILENAME);
    HWND    hwndOwner        = nullptr;
    HINSTANCE hInstance      = nullptr;
    LPCTSTR lpstrFilter      = nullptr;
    LPTSTR  lpstrCustomFilter= nullptr;
    DWORD   nMaxCustFilter   = 0;
    DWORD   nFilterIndex     = 0;
    LPTSTR  lpstrFile        = nullptr;
    DWORD   nMaxFile         = 0;
    LPTSTR  lpstrFileTitle   = nullptr;
    DWORD   nMaxFileTitle    = 0;
    LPCTSTR lpstrInitialDir  = nullptr;
    LPCTSTR lpstrTitle       = nullptr;
    DWORD   Flags            = 0;
    WORD    nFileOffset      = 0;
    WORD    nFileExtension   = 0;
    LPCTSTR lpstrDefExt      = nullptr;
    LPARAM  lCustData        = 0;
    void*   lpfnHook         = nullptr;
    LPCTSTR lpTemplateName   = nullptr;
};
typedef OPENFILENAME * LPOPENFILENAME;

// ── CDialog ───────────────────────────────────────────────────────────────
class CDialog : public CWnd {
    SHIM_RUNTIME_CLASS(CDialog, CWnd)
public:
    UINT m_nIDHelp = 0;
    UINT m_nIDTemplate = 0;            // dialog resource id (IDD), set by ctor
    explicit CDialog(UINT nID = 0, CWnd* pParent = nullptr) : m_nIDTemplate(nID) { m_pParentWnd = pParent; }
    // DoModal: the bridge builds the wxDialog from template m_nIDTemplate, runs
    // OnInitDialog, shows it modally, and dispatches OK/Cancel.  No bridge (CLI/
    // tests) -> IDCANCEL, the prior stub behaviour.
    virtual INT_PTR DoModal()          { return g_run_mfc_dialog ? g_run_mfc_dialog(this, m_nIDTemplate) : IDCANCEL; }
    virtual void    EndDialog(int nResult) { if (g_end_mfc_dialog) g_end_mfc_dialog(this, nResult); }
    virtual BOOL    OnInitDialog()     { UpdateData(FALSE); return TRUE; }         // initial DDX load
    virtual void    OnOK()             { if (UpdateData(TRUE)) EndDialog(IDOK); }  // MFC default: validation veto keeps it open
    virtual void    OnCancel()         { EndDialog(IDCANCEL); }                    // MFC default
    BOOL   UpdateData(BOOL bSaveAndValidate = TRUE);    // defined below (needs CDataExchange)
    CWnd*  GetDlgItem(int id) const    { return CWnd::GetDlgItem(id); }
    void   SetDlgItemText(int id, LPCTSTR s) { if (CWnd* w = GetDlgItem(id)) w->SetWindowText(s); }
    void   GetDlgItemText(int id, CString& s) const { if (CWnd* w = GetDlgItem(id)) w->GetWindowText(s); else s = ""; }
    int    GetDlgItemInt(int, BOOL* = nullptr, BOOL=TRUE) const { return 0; }
    void   SetDlgItemInt(int, UINT, BOOL=TRUE) {}
    void   GotoDlgCtrl(CWnd*) {}
    void   NextDlgCtrl() {}
    void   PrevDlgCtrl() {}
    virtual void DoDataExchange(CDataExchange*) {}

    // Parent window pointer (MFC sets this in Create)
    CWnd* m_pParentWnd = nullptr;

    // Modeless dialog support
    virtual BOOL Create(UINT, CWnd* = nullptr)   { return FALSE; }
    virtual BOOL CreateIndirect(LPVOID, CWnd*, HINSTANCE=nullptr) { return FALSE; }
    BOOL   DestroyWindow() { return FALSE; }

    // IsDialogMessage — member variant (dispatches to global stub)
    BOOL IsDialogMessage(MSG* pMsg) { return FALSE; }

    // Overridable window event handlers
    virtual int  OnCreate(CREATESTRUCT*) { return 0; }
    virtual void OnDestroy()  {}
    virtual void OnSize(UINT, int, int) {}
    virtual void OnHScroll(UINT, UINT, CScrollBar*) {}
    virtual void OnSysCommand(UINT, LPARAM) {}
    virtual void PostNcDestroy() {}
    virtual BOOL PreTranslateMessage(MSG*) { return FALSE; }
    virtual void OnTimer(UINT_PTR) {}

    // SetIcon — used in OnInitDialog to set dialog icon
    HICON SetIcon(HICON, BOOL = TRUE) { return nullptr; }

    // OnCtlColor — called by framework to get brush for control background
    virtual HBRUSH OnCtlColor(CDC*, CWnd*, UINT) { return nullptr; }

    // GetSafeOwner — instance and static variants
    CWnd* GetSafeOwner() const { return nullptr; }
    static CWnd* GetSafeOwner(CWnd* pParent, HWND* = nullptr) { return pParent; }

    // UpdateDialogControls — updates toolbar/menu UI state (no-op stub)
    void UpdateDialogControls(CCmdTarget*, BOOL = TRUE) {}
    // Radio / check button helpers — defined out-of-line below (after CButton),
    // since they route through CButton::Get/SetCheck on GetDlgItem's result.
    void CheckRadioButton(int first, int last, int check);
    void CheckDlgButton(int id, UINT check);
    BOOL IsDlgButtonChecked(int id) const;
    // Map dialog coords to screen pixels
    void MapDialogRect(LPRECT) const {}
    // SendDlgItemMessage
    LRESULT SendDlgItemMessage(int, UINT, WPARAM=0, LPARAM=0) { return 0; }
};

// CDataExchange stub (used by DoDataExchange)
// Thrown by CDataExchange::Fail() (MFC's DDV/validation failure signal) and
// caught by CDialog::UpdateData, which then returns FALSE.  Matches MFC: the
// first failed validation aborts the rest of DoDataExchange, so only ONE message
// box shows per UpdateData (not a cascade), and the OK is vetoed.
struct CDdxValidationFail {};

class CDataExchange {
public:
    BOOL m_bSaveAndValidate = TRUE;
    CWnd* m_pDlgWnd = nullptr;
    void  PrepareCtrl(int) {}
    void  PrepareEditCtrl(int) {}
    // MFC's CDataExchange::Fail() sets focus then throws CUserException; the port
    // throws a lightweight sentinel that UpdateData catches.  Canonical
    // DoDataExchange code already expects Fail() to abort (Windows contract).
    void  Fail() { throw CDdxValidationFail(); }
};

// CDialog::UpdateData — run DoDataExchange in the requested direction (FALSE =
// members -> controls, TRUE = controls -> members).  Defined here now that
// CDataExchange is complete.
inline BOOL CDialog::UpdateData(BOOL bSaveAndValidate) {
    CDataExchange dx;
    dx.m_bSaveAndValidate = bSaveAndValidate;
    dx.m_pDlgWnd = this;
    try {
        DoDataExchange(&dx);
    } catch (const CDdxValidationFail&) {
        return FALSE;   // a DDV / pDX->Fail() failed; the page already showed its message
    }
    return TRUE;
}

// DDX / DDV exchange functions.  DDX_Text(CString) is real (needs only CWnd);
// the control-class-specific ones (DDX_Check, ...) are real but defined after
// their control classes below.  The rest remain no-ops for now.
inline void DDX_Text(CDataExchange* pDX, int id, CString& s) {
    CWnd* w = (pDX && pDX->m_pDlgWnd) ? pDX->m_pDlgWnd->GetDlgItem(id) : nullptr;
    if (!w) return;
    if (pDX->m_bSaveAndValidate) w->GetWindowText(s);
    else                         w->SetWindowText((LPCTSTR) s);
}
// Numeric DDX_Text: save formats the value into the control's text; load reads
// the control text and parses it back.  Mirrors MFC's AfxTextFloatFormat /
// integer DDX (minus the locale plumbing).  Without these, every numeric prefs
// field (port, font sizes, line spacing, timer intervals, …) silently failed to
// load OR save on the port.
inline void DDX_TextNumImpl(CDataExchange* pDX, int id, CString& text) {
    CWnd* w = (pDX && pDX->m_pDlgWnd) ? pDX->m_pDlgWnd->GetDlgItem(id) : nullptr;
    if (!w) { text.Empty(); return; }
    if (pDX->m_bSaveAndValidate) w->GetWindowText(text);   // control -> text (caller parses)
    else                         w->SetWindowText((LPCTSTR) text);  // text -> control
}
inline void DDX_Text(CDataExchange* pDX, int id, int& value) {
    CString s;
    if (!pDX->m_bSaveAndValidate) s.Format("%d", value);
    DDX_TextNumImpl(pDX, id, s);
    if (pDX->m_bSaveAndValidate) value = (int) strtol((LPCTSTR) s, nullptr, 10);
}
inline void DDX_Text(CDataExchange* pDX, int id, short& value) {
    CString s;
    if (!pDX->m_bSaveAndValidate) s.Format("%d", (int) value);
    DDX_TextNumImpl(pDX, id, s);
    if (pDX->m_bSaveAndValidate) value = (short) strtol((LPCTSTR) s, nullptr, 10);
}
inline void DDX_Text(CDataExchange* pDX, int id, long& value) {
    CString s;
    if (!pDX->m_bSaveAndValidate) s.Format("%ld", value);
    DDX_TextNumImpl(pDX, id, s);
    if (pDX->m_bSaveAndValidate) value = strtol((LPCTSTR) s, nullptr, 10);
}
inline void DDX_Text(CDataExchange* pDX, int id, UINT& value) {
    CString s;
    if (!pDX->m_bSaveAndValidate) s.Format("%u", value);
    DDX_TextNumImpl(pDX, id, s);
    if (pDX->m_bSaveAndValidate) value = (UINT) strtoul((LPCTSTR) s, nullptr, 10);
}
// DWORD == UINT == uint32_t; no separate overload needed
inline void DDX_Text(CDataExchange* pDX, int id, float& value) {
    CString s;
    if (!pDX->m_bSaveAndValidate) s.Format("%g", (double) value);
    DDX_TextNumImpl(pDX, id, s);
    if (pDX->m_bSaveAndValidate) value = (float) strtod((LPCTSTR) s, nullptr);
}
inline void DDX_Text(CDataExchange* pDX, int id, double& value) {
    CString s;
    if (!pDX->m_bSaveAndValidate) s.Format("%g", value);
    DDX_TextNumImpl(pDX, id, s);
    if (pDX->m_bSaveAndValidate) value = strtod((LPCTSTR) s, nullptr);
}
// Note: BOOL == int so no separate BOOL& overload needed.  DDX_Check is real,
// but defined after CButton below (it casts GetDlgItem's result to CButton*).
// DDX_Radio: member <-> which radio in the group starting at `firstId` is set
// (0-based index; -1 = none).  Routes to the radio-group hook keyed on the
// dialog's m_hWnd.
inline void DDX_Radio(CDataExchange* pDX, int firstId, int& value) {
    if (!pDX || !pDX->m_pDlgWnd) return;
    void* dlg = pDX->m_pDlgWnd->m_hWnd;
    if (pDX->m_bSaveAndValidate) {
        if (g_wxhook_radio_get) value = g_wxhook_radio_get(dlg, firstId);
    } else {
        if (g_wxhook_radio_set) g_wxhook_radio_set(dlg, firstId, value);
    }
}
// DDX_CBIndex / DDX_CBString are real, but defined after CComboBox below (they
// cast GetDlgItem's CWnd* to CComboBox* — legal since CComboBox adds no data).
void DDX_CBIndex(CDataExchange* pDX, int id, int& index);
void DDX_CBString(CDataExchange* pDX, int id, CString& value);
inline void DDX_CBStringExact(CDataExchange*, int, CString&) {}
inline void DDX_LBIndex(CDataExchange*, int, int&) {}
inline void DDX_LBString(CDataExchange*, int, CString&) {}
// DDX_Control subclasses a CWnd member onto the live dialog control (MFC: the
// member becomes the C++ wrapper for that HWND).  On the port we just point the
// member's m_hWnd at the control's wxWindow (looked up via the dialog), so
// control-class methods (CListCtrl/CStatic/CButton/…) route through the hooks to
// the real widget.  Direction-independent: the binding is needed for both load
// and save.
inline void DDX_Control(CDataExchange* pDX, int id, CWnd& ctl) {
    if (pDX && pDX->m_pDlgWnd) {
        if (g_wxhook_get_dlg_item)
            ctl.m_hWnd = (HWND) g_wxhook_get_dlg_item(pDX->m_pDlgWnd->m_hWnd, id);
        // Owner-draw (BS_OWNERDRAW, e.g. CColourButton): tell the wx widget which
        // CButton backs it, so its paint handler can call the canonical DrawItem.
        if (g_wxhook_bind_owner_draw && ctl.m_hWnd)
            g_wxhook_bind_owner_draw(ctl.m_hWnd, &ctl);
        // Slider (msctls_trackbar32): tell the wx slider which dialog owns it, so
        // its move event drives the canonical CWnd::OnHScroll.  The hook no-ops
        // for non-slider controls.
        if (g_wxhook_bind_slider && ctl.m_hWnd)
            g_wxhook_bind_slider(ctl.m_hWnd, pDX->m_pDlgWnd);
    }
}
inline void DDV_MinMaxInt(CDataExchange*, int, int, int) {}
inline void DDV_MinMaxLong(CDataExchange*, long, long, long) {}
inline void DDV_MinMaxUInt(CDataExchange*, UINT, UINT, UINT) {}
inline void DDV_MinMaxDouble(CDataExchange*, double, double, double) {}
inline void DDV_MinMaxFloat(CDataExchange*, float, float, float) {}
// DDX_MinMaxString, DDV_MinMaxString, DDV_MaxChars: real implementations live in
// DDV_validation.cpp (compiled in mushapp).  Provide stubs only when that file
// is NOT compiled (i.e. the portable mushcore build).
// DDV_MaxChars: called internally but not defined in DDV_validation.cpp — always stub.
inline void DDV_MaxChars(CDataExchange*, CString&, int) {}

// The following four are DEFINED in DDV_validation.cpp (in the mushapp full build).
// Always declare them so callers can see the signature.
// In the portable mushcore build also provide inline stubs (no DDV_validation.cpp).
void AFXAPI DDX_MinMaxString(CDataExchange*, int, CString&);
void AFXAPI DDV_MinMaxString(CDataExchange*, CString&, int, int);
void AFXAPI DDX_MinMaxCBString(CDataExchange*, int, CString&);
void AFXAPI DDV_MinMaxCBString(CDataExchange*, CString&, int, int);
#ifndef MUSHCLIENT_FULL_BUILD
inline void DDX_MinMaxString(CDataExchange*, int, CString&)           {}
inline void DDV_MinMaxString(CDataExchange*, CString&, int, int)      {}
inline void DDX_MinMaxCBString(CDataExchange*, int, CString&)         {}
inline void DDV_MinMaxCBString(CDataExchange*, CString&, int, int)    {}
#endif // MUSHCLIENT_FULL_BUILD

// ── Common dialog controls ─────────────────────────────────────────────────

class CListBox : public CWnd {
public:
    int       AddString(LPCTSTR s) {
        return g_wxhook_listbox_add_string
             ? g_wxhook_listbox_add_string(m_hWnd, s ? s : "") : LB_ERR;
    }
    int       InsertString(int, LPCTSTR) { return LB_ERR; }
    int       DeleteString(UINT) { return LB_ERR; }
    void      ResetContent() {
        if (g_wxhook_listbox_reset_content) g_wxhook_listbox_reset_content(m_hWnd);
    }
    int       GetCount() const {
        return g_wxhook_listbox_get_count ? g_wxhook_listbox_get_count(m_hWnd) : 0;
    }
    int       GetCurSel() const {
        return g_wxhook_listbox_get_cur_sel ? g_wxhook_listbox_get_cur_sel(m_hWnd) : LB_ERR;
    }
    int       SetCurSel(int sel) {
        return g_wxhook_listbox_set_cur_sel ? g_wxhook_listbox_set_cur_sel(m_hWnd, sel) : LB_ERR;
    }
    int       GetSel(int i) const {
        return g_wxhook_listbox_get_sel ? g_wxhook_listbox_get_sel(m_hWnd, i) : 0;
    }
    int       SetSel(int i, BOOL sel = TRUE) {
        return g_wxhook_listbox_set_sel ? g_wxhook_listbox_set_sel(m_hWnd, i, sel != FALSE) : LB_ERR;
    }
    int       GetText(int, LPTSTR) const { return LB_ERR; }
    void      GetText(int i, CString& s) const {
        char* p = g_wxhook_listbox_get_text ? g_wxhook_listbox_get_text(m_hWnd, i) : nullptr;
        if (p) { s = p; ::free(p); } else s.Empty();
    }
    int       GetTextLen(int) const { return LB_ERR; }
    void      SetItemData(int i, DWORD_PTR d) {
        if (g_wxhook_listbox_set_item_data) g_wxhook_listbox_set_item_data(m_hWnd, i, (intptr_t) d);
    }
    DWORD_PTR GetItemData(int i) const {
        return g_wxhook_listbox_get_item_data ? (DWORD_PTR) g_wxhook_listbox_get_item_data(m_hWnd, i) : 0;
    }
    int       FindString(int, LPCTSTR) const { return LB_ERR; }
    int       FindStringExact(int, LPCTSTR) const { return LB_ERR; }
    int       SelectString(int, LPCTSTR) { return LB_ERR; }
    int       GetSelCount() const {
        return g_wxhook_listbox_get_sel_count ? g_wxhook_listbox_get_sel_count(m_hWnd) : 0;
    }
    int       GetSelItems(int nMax, LPINT buf) const {
        return g_wxhook_listbox_get_sel_items ? g_wxhook_listbox_get_sel_items(m_hWnd, nMax, buf) : 0;
    }
    void      SetColumnWidth(int) {}
    void      SetHorizontalExtent(int) {}
    int       GetHorizontalExtent() const { return 0; }
    void      Dir(UINT, LPCTSTR) {}
};

// CComboBox wraps a wxComboBox.  List ops (AddString/GetCurSel/SetCurSel/…)
// route through g_wxhook_combo_* keyed on m_hWnd (bound by DDX_Control); the
// edit text uses the same CWnd Get/SetWindowText hooks (wxComboBox handles both
// in hook_get/set_window_text).  Null hooks (mushcore) → safe CB_ERR/no-op.
class CComboBox : public CWnd {
public:
    int       AddString(LPCTSTR s) {
        return g_wxhook_combo_add_string ? g_wxhook_combo_add_string(m_hWnd, s ? s : "") : CB_ERR;
    }
    int       InsertString(int, LPCTSTR) { return CB_ERR; }
    int       DeleteString(UINT) { return CB_ERR; }
    void      ResetContent() {
        if (g_wxhook_combo_reset_content) g_wxhook_combo_reset_content(m_hWnd);
    }
    int       GetCount() const {
        return g_wxhook_combo_get_count ? g_wxhook_combo_get_count(m_hWnd) : 0;
    }
    int       GetCurSel() const {
        return g_wxhook_combo_get_cur_sel ? g_wxhook_combo_get_cur_sel(m_hWnd) : CB_ERR;
    }
    int       SetCurSel(int sel) {
        if (g_wxhook_combo_set_cur_sel) g_wxhook_combo_set_cur_sel(m_hWnd, sel);
        return sel;
    }
    int       GetLBText(int, LPTSTR) const { return CB_ERR; }
    void      GetLBText(int index, CString& s) const {
        s.Empty();
        if (g_wxhook_combo_get_lb_text) {
            char* t = g_wxhook_combo_get_lb_text(m_hWnd, index);
            if (t) { s = t; free(t); }
        }
    }
    int       GetLBTextLen(int) const { return CB_ERR; }
    // GetWindowText/SetWindowText: use the CWnd hooks (wxComboBox supported there).
    void      GetWindowText(CString& s) const { CWnd::GetWindowText(s); }
    void      SetWindowText(LPCTSTR s)        { CWnd::SetWindowText(s); }
    int       GetWindowTextLength() const { return 0; }
    void      SetItemData(int, DWORD_PTR) {}
    DWORD_PTR GetItemData(int) const { return 0; }
    int       FindString(int after, LPCTSTR s) const { return FindStringExact(after, s); }
    int       FindStringExact(int after, LPCTSTR s) const {
        return g_wxhook_combo_find_string_exact
             ? g_wxhook_combo_find_string_exact(m_hWnd, after, s ? s : "") : CB_ERR;
    }
    int       SelectString(int after, LPCTSTR s) {
        int idx = FindStringExact(after, s);
        if (idx != CB_ERR) SetCurSel(idx);
        return idx;
    }
    void      ShowDropDown(BOOL=TRUE) {}
    void      GetDroppedControlRect(LPRECT) const {}
    BOOL      GetDroppedState() const { return FALSE; }
    void      LimitText(int) {}
    int       SetEditSel(int, int) { return CB_ERR; }
    BOOL      GetEditSel(LPDWORD, LPDWORD) const { return FALSE; }
};

// DDX_CBIndex: member <-> combo's selected index.  DDX_CBString: member <->
// the selected item's text.  Defined here now that CComboBox is complete.
inline void DDX_CBIndex(CDataExchange* pDX, int id, int& index) {
    CWnd* w = (pDX && pDX->m_pDlgWnd) ? pDX->m_pDlgWnd->GetDlgItem(id) : nullptr;
    if (!w) return;
    CComboBox* cb = (CComboBox*) w;   // CComboBox adds no data over CWnd
    if (pDX->m_bSaveAndValidate) {
        int sel = cb->GetCurSel();
        // Preserve the member when the combo is empty / has no selection.  Some
        // recovered .rc templates carry no embedded combo items (the decompiler
        // doesn't extract CB item lists), so a combo populated only via the .rc
        // would otherwise clobber a valid member with -1 (CB_ERR) on save and
        // fail the page's validation (e.g. "Unknown proxy server type").
        if (sel != CB_ERR) index = sel;
    } else {
        cb->SetCurSel(index);
    }
}
inline void DDX_CBString(CDataExchange* pDX, int id, CString& value) {
    CWnd* w = (pDX && pDX->m_pDlgWnd) ? pDX->m_pDlgWnd->GetDlgItem(id) : nullptr;
    if (!w) return;
    CComboBox* cb = (CComboBox*) w;
    if (pDX->m_bSaveAndValidate) {
        int sel = cb->GetCurSel();
        if (sel >= 0) cb->GetLBText(sel, value);
        else          cb->GetWindowText(value);   // editable combo: free text
    } else {
        if (cb->SelectString(-1, (LPCTSTR) value) == CB_ERR)
            cb->SetWindowText((LPCTSTR) value);    // not in list → set edit text
    }
}

class CEdit : public CWnd {
public:
    // NOTE: do NOT shadow Get/SetWindowText with stubs — CWnd's real versions
    // route to the wx hooks.  Stubbing them here silently broke direct reads/
    // writes such as CImmediateDlg::OnRun (Execute Lua read an empty script).
    int  GetWindowTextLength() const { return g_wxhook_edit_line_info ? g_wxhook_edit_line_info((void*) m_hWnd, 4, 0) : 0; }
    void LimitText(int) {}
    void SetSel(int s, int e, BOOL=FALSE) { if (g_wxhook_edit_set_sel) g_wxhook_edit_set_sel((void*) m_hWnd, s, e); }
    void GetSel(DWORD& nS, DWORD& nE) const { int a=0,b=0; if (g_wxhook_edit_get_sel) g_wxhook_edit_get_sel((void*) m_hWnd, &a, &b); nS=(DWORD)a; nE=(DWORD)b; }
    void GetSel(int&  nS, int&  nE) const { int a=0,b=0; if (g_wxhook_edit_get_sel) g_wxhook_edit_get_sel((void*) m_hWnd, &a, &b); nS=a; nE=b; }
    void ReplaceSel(LPCTSTR t, BOOL=FALSE) { if (g_wxhook_edit_replace_sel) g_wxhook_edit_replace_sel((void*) m_hWnd, t); }
    void SetReadOnly(BOOL=TRUE) {}
    int  GetLineCount() const { return g_wxhook_edit_line_info ? g_wxhook_edit_line_info((void*) m_hWnd, 0, 0) : 0; }
    int  LineIndex(int n=-1) const { return g_wxhook_edit_line_info ? g_wxhook_edit_line_info((void*) m_hWnd, 1, n) : 0; }
    int  LineLength(int n=-1) const { return g_wxhook_edit_line_info ? g_wxhook_edit_line_info((void*) m_hWnd, 2, n) : 0; }
    int  GetLine(int, LPTSTR, int) const { return 0; }
    void Clear() {}
    void Copy() {}
    void Cut() {}
    void Paste() {}
    void Undo() {}
    BOOL CanUndo() const { return FALSE; }
    void EmptyUndoBuffer() {}
    void SetMargins(UINT, UINT) {}
    DWORD GetMargins() const { return 0; }
    // SetFont is inherited from CWnd (real, via g_wxhook_set_font) — don't shadow.
    // Additional CEdit methods used in dialog code
    void  SetTabStops(int) {}
    void  SetTabStops(int, const int*) {}
    BOOL  GetModify() const { return FALSE; }
    void  SetModify(BOOL = TRUE) {}
    int   LineFromChar(int n = -1) const { return g_wxhook_edit_line_info ? g_wxhook_edit_line_info((void*) m_hWnd, 3, n) : 0; }
    BOOL  LineScroll(int, int = 0) { return FALSE; }
    int   GetFirstVisibleLine() const { return 0; }
    CPoint PosFromChar(UINT) const { return CPoint(); }  // char index → client coords
    // Handle-based access (used in chatsock.cpp)
    HLOCAL GetHandle() const { return nullptr; }
    void   SetHandle(HLOCAL) {}
    // SetTabStops with single int arg (already defined above via int, const int*)
};

// CEditView::GetEditCtrl() defined here now that CEdit is complete.
inline CEdit& CEditView::GetEditCtrl() { static CEdit e; return e; }

class CButton : public CWnd {
public:
    void  SetCheck(int n) { if (g_wxhook_button_set_check) g_wxhook_button_set_check(m_hWnd, n); }
    int   GetCheck() const { return g_wxhook_button_get_check ? g_wxhook_button_get_check(m_hWnd) : 0; }
    void  SetState(BOOL) {}
    UINT  GetState() const { return 0; }
    void  SetIcon(HICON) {}
    HICON GetIcon() const { return nullptr; }
    void  SetBitmap(HBITMAP) {}
    void  SetButtonStyle(UINT, BOOL=TRUE) {}
    BOOL  Create(LPCTSTR, DWORD, const RECT&, CWnd*, UINT) { return FALSE; }
    // Owner-draw paint entry point (BS_OWNERDRAW).  Subclasses (CColourButton)
    // override this to paint via the passed CDC; the owner-draw wx bridge calls
    // it with a CDC over the control's paint bitmap.  Base = no custom paint.
    virtual void DrawItem(LPDRAWITEMSTRUCT) {}
};

class CStatic : public CWnd {
    SHIM_RUNTIME_CLASS(CStatic, CWnd)
public:
    // Get/SetWindowText inherited from CWnd (route to the wx hooks) — don't
    // shadow them: stubbing SetWindowText was why the entity-list summary count
    // ("0 triggers.") never refreshed after LoadList.
    void    SetBitmap(HBITMAP) {}
    HBITMAP GetBitmap() const { return nullptr; }
    void    SetIcon(HICON) {}
    HICON   GetIcon() const { return nullptr; }
    BOOL    Create(LPCTSTR, DWORD, const RECT&, CWnd*, UINT=0) { return FALSE; }
};

// DDX_Check — checkbox / radio-button state <-> int (BST_CHECKED / BST_UNCHECKED).
// Defined here, after CButton, since it casts GetDlgItem's CWnd* to CButton*
// (the control classes add no data, so the cast is valid — MFC's contract).
inline void DDX_Check(CDataExchange* pDX, int id, int& value) {
    CWnd* w = (pDX && pDX->m_pDlgWnd) ? pDX->m_pDlgWnd->GetDlgItem(id) : nullptr;
    if (!w) return;
    CButton* b = (CButton*) w;
    if (pDX->m_bSaveAndValidate) value = b->GetCheck();
    else                         b->SetCheck(value);
}

// CDialog radio/check helpers — route through CButton::Get/SetCheck on the
// GetDlgItem result (same cast contract DDX_Check relies on). Previously no-op
// stubs, which left e.g. the Timer dialog's At/Every fields stuck and the
// custom-icon radio unselectable after Browse.
inline void CDialog::CheckRadioButton(int first, int last, int check) {
    for (int id = first; id <= last; ++id)
        if (CWnd* w = GetDlgItem(id)) ((CButton*) w)->SetCheck(id == check ? 1 : 0);
}
inline void CDialog::CheckDlgButton(int id, UINT check) {
    if (CWnd* w = GetDlgItem(id)) ((CButton*) w)->SetCheck((int) check);
}
inline BOOL CDialog::IsDlgButtonChecked(int id) const {
    CWnd* w = GetDlgItem(id);
    return (w && ((CButton*) w)->GetCheck()) ? TRUE : FALSE;
}

// CListCtrl wraps a report-mode wxListCtrl (the entity-manager object list).
// Each method routes through a g_wxhook_listctrl_* hook keyed on m_hWnd (the
// wxListCtrl*, bound by DDX_Control).  Methods the canonical pages don't use
// remain inert.  Off-GUI builds (mushcore) have null hooks → safe no-ops.
class CListCtrl : public CWnd {
    SHIM_RUNTIME_CLASS(CListCtrl, CWnd)
public:
    int       InsertItem(int item, LPCTSTR text) {
        return g_wxhook_listctrl_insert_item
             ? g_wxhook_listctrl_insert_item(m_hWnd, item, text ? text : "") : -1;
    }
    int       InsertItem(const LVITEM* p) {
        return (p && g_wxhook_listctrl_insert_item)
             ? g_wxhook_listctrl_insert_item(m_hWnd, p->iItem,
                   p->pszText ? p->pszText : "") : -1;
    }
    BOOL      SetItem(const LVITEM*)      { return FALSE; }
    BOOL      SetItemText(int item, int col, LPCTSTR text) {
        if (g_wxhook_listctrl_set_item_text)
            g_wxhook_listctrl_set_item_text(m_hWnd, item, col, text ? text : "");
        return TRUE;
    }
    BOOL      SetItemData(int item, DWORD_PTR data) {
        if (g_wxhook_listctrl_set_item_data)
            g_wxhook_listctrl_set_item_data(m_hWnd, item, (void*) data);
        return TRUE;
    }
    DWORD_PTR GetItemData(int item) const {
        return g_wxhook_listctrl_get_item_data
             ? (DWORD_PTR) g_wxhook_listctrl_get_item_data(m_hWnd, item) : 0;
    }
    CString   GetItemText(int, int) const{ return {}; }
    int       GetItemCount() const {
        return g_wxhook_listctrl_get_item_count
             ? g_wxhook_listctrl_get_item_count(m_hWnd) : 0;
    }
    int       GetNextItem(int start, int flags) const {
        return g_wxhook_listctrl_get_next_item
             ? g_wxhook_listctrl_get_next_item(m_hWnd, start, flags) : -1;
    }
    BOOL      DeleteItem(int item) {
        if (g_wxhook_listctrl_delete_item) g_wxhook_listctrl_delete_item(m_hWnd, item);
        return TRUE;
    }
    void      DeleteAllItems() {
        if (g_wxhook_listctrl_delete_all_items) g_wxhook_listctrl_delete_all_items(m_hWnd);
    }
    int       InsertColumn(int col, LPCTSTR heading, int fmt=LVCFMT_LEFT, int width=100, int=-1) {
        if (g_wxhook_listctrl_insert_column)
            g_wxhook_listctrl_insert_column(m_hWnd, col, heading ? heading : "", fmt, width);
        return col;
    }
    int       InsertColumn(int, const LVCOLUMN*) { return -1; }
    BOOL      GetColumn(int, LVCOLUMN*) const { return FALSE; }
    BOOL      SetColumn(int, const LVCOLUMN*) { return FALSE; }
    int       GetSelectedCount() const {
        return g_wxhook_listctrl_get_selected_count
             ? g_wxhook_listctrl_get_selected_count(m_hWnd) : 0;
    }
    UINT      GetItemState(int item, UINT mask) const {
        return g_wxhook_listctrl_get_item_state
             ? g_wxhook_listctrl_get_item_state(m_hWnd, item, mask) : 0;
    }
    BOOL      SetItemState(int item, UINT state, UINT mask) {
        if (g_wxhook_listctrl_set_item_state)
            g_wxhook_listctrl_set_item_state(m_hWnd, item, state, mask);
        return TRUE;
    }
    void      SortItems(int(*cmp)(LPARAM,LPARAM,LPARAM), LPARAM lp) {
        if (g_wxhook_listctrl_sort_items)
            g_wxhook_listctrl_sort_items(m_hWnd,
                (int(*)(intptr_t,intptr_t,intptr_t)) cmp, (intptr_t) lp);
    }
    void      EnsureVisible(int item, BOOL=FALSE) {
        if (g_wxhook_listctrl_ensure_visible) g_wxhook_listctrl_ensure_visible(m_hWnd, item);
    }
    void      SetExtendedStyle(DWORD) {}
    DWORD     GetExtendedStyle() const   { return 0; }
    CEdit*    EditLabel(int)             { return nullptr; }
    BOOL      GetItemRect(int, LPRECT, int) const { return FALSE; }
    BOOL      GetSubItemRect(int, int, int, CRect&) const { return FALSE; }
    BOOL      GetSubItemRect(int, int, int, RECT&) const { return FALSE; }
    int       HitTest(CPoint, UINT* = nullptr) const { return -1; }
    BOOL      Scroll(CSize) { return FALSE; }
    int       GetTopIndex() const { return 0; }
    int       GetCountPerPage() const { return 0; }
    BOOL      RedrawItems(int,int) { return TRUE; }   // wxListCtrl repaints itself
    void      SetBkColor(COLORREF) {}
    void      SetTextColor(COLORREF) {}
    void      SetTextBkColor(COLORREF) {}
    BOOL      SetImageList(void*, int) { return FALSE; }
    int       GetColumnWidth(int) const { return 0; }
    BOOL      SetColumnWidth(int col, int width) {
        if (g_wxhook_listctrl_set_column_width) g_wxhook_listctrl_set_column_width(m_hWnd, col, width);
        return TRUE;
    }
};

// ── Forward declarations needed for CTabCtrl ─────────────────────────────
class CImageList;  // defined later in this file
typedef void* HIMAGELIST;

// ── Tab control structs / constants ───────────────────────────────────────
#ifndef TCIF_TEXT
#  define TCIF_TEXT     0x0001
#  define TCIF_IMAGE    0x0002
#  define TCIF_PARAM    0x0008
#  define TCIF_STATE    0x0010
#endif
#ifndef TCS_BOTTOM
#  define TCS_BOTTOM        0x0002
#  define TCS_SINGLELINE    0x0000
#  define TCS_MULTILINE     0x0200
#  define TCS_FOCUSNEVER    0x8000
#  define TCS_FORCEICONLEFT 0x0010
#  define TCS_FORCELABELLEFT 0x0020
#  define TCS_FIXEDWIDTH    0x0400
#  define TCS_RAGGEDRIGHT   0x0800
#endif
#ifndef RDW_FRAME
#  define RDW_INVALIDATE    0x0001
#  define RDW_ERASE         0x0004
#  define RDW_FRAME         0x0400
#  define RDW_ALLCHILDREN   0x0080
#endif
struct TC_ITEM {
    UINT  mask    = 0;
    UINT  dwState = 0;
    UINT  dwStateMask = 0;
    char* pszText = nullptr;
    int   cchTextMax = 0;
    int   iImage  = -1;
    LPARAM lParam = 0;
};
typedef TC_ITEM TCITEM;
struct TCHITTESTINFO {
    POINT pt = {};
    UINT  flags = 0;
};
#ifndef ILC_COLOR
#  define ILC_COLOR     0x00FE
#  define ILC_COLORDDB  0x00FE
#  define ILC_COLOR4    0x0004
#  define ILC_COLOR8    0x0008
#  define ILC_COLOR16   0x0010
#  define ILC_COLOR24   0x0018
#  define ILC_COLOR32   0x0020
#  define ILC_MASK      0x0001
#  define ILC_MIRROR    0x2000
#  define ILC_PERITEMMIRROR 0x8000
#endif

class CTabCtrl : public CWnd {
public:
    int  GetCurSel() const { return -1; }
    int  SetCurSel(int) { return -1; }
    BOOL InsertItem(int, const TCITEM*) { return FALSE; }
    BOOL InsertItem(int, void*) { return FALSE; }
    BOOL DeleteAllItems() { return FALSE; }
    BOOL DeleteItem(int) { return FALSE; }
    int  GetItemCount() const { return 0; }
    BOOL GetItemRect(int, LPRECT) const { return FALSE; }
    void AdjustRect(BOOL, LPRECT) {}
    BOOL GetItem(int, TCITEM*) const { return FALSE; }
    BOOL SetItem(int, const TCITEM*) { return FALSE; }
    int  HitTest(const TCHITTESTINFO*) const { return -1; }
    CImageList* SetImageList(CImageList*) { return nullptr; }
    CImageList* GetImageList() const { return nullptr; }
    BOOL Create(DWORD, const RECT&, CWnd*, UINT) { return FALSE; }
    void RemoveImage(int) {}
    BOOL RedrawWindow(const RECT* =nullptr, CRgn* =nullptr, UINT =0) {
        if (g_wxhook_invalidate) g_wxhook_invalidate(m_hWnd, true);
        return TRUE;
    }
};

class CTreeCtrl : public CWnd {
public:
    HTREEITEM InsertItem(LPCTSTR, HTREEITEM=nullptr, HTREEITEM=nullptr) { return nullptr; }
    HTREEITEM InsertItem(const TVINSERTSTRUCT*) { return nullptr; }
    BOOL      DeleteItem(HTREEITEM)         { return FALSE; }
    void      DeleteAllItems()              {}
    HTREEITEM GetSelectedItem() const       { return nullptr; }
    BOOL      SelectItem(HTREEITEM)         { return FALSE; }
    BOOL      GetItem(TVITEM*) const        { return FALSE; }
    BOOL      SetItem(const TVITEM*)        { return FALSE; }
    BOOL      Expand(HTREEITEM, UINT)       { return FALSE; }
    HTREEITEM GetChildItem(HTREEITEM) const { return nullptr; }
    HTREEITEM GetNextSiblingItem(HTREEITEM) const { return nullptr; }
    HTREEITEM GetPrevSiblingItem(HTREEITEM) const { return nullptr; }
    HTREEITEM GetParentItem(HTREEITEM) const{ return nullptr; }
    HTREEITEM GetRootItem() const           { return nullptr; }
    CString   GetItemText(HTREEITEM) const  { return {}; }
    DWORD_PTR GetItemData(HTREEITEM) const  { return 0; }
    BOOL      SetItemData(HTREEITEM, DWORD_PTR) { return FALSE; }
    BOOL      SetItemText(HTREEITEM, LPCTSTR)   { return FALSE; }
    UINT      GetItemState(HTREEITEM, UINT) const { return 0; }
    BOOL      SetItemState(HTREEITEM, UINT, UINT) { return FALSE; }
    BOOL      SortChildren(HTREEITEM) { return FALSE; }
    BOOL      SortChildrenCB(TVSORTCB*, BOOL=FALSE) { return FALSE; }
    void      SetImageList(void*, int) {}
    HTREEITEM HitTest(CPoint, UINT* = nullptr) const { return nullptr; }
    BOOL      GetItemRect(HTREEITEM, LPRECT, BOOL) const { return FALSE; }
    BOOL      EnsureVisible(HTREEITEM) { return FALSE; }
    void      Invalidate(BOOL=TRUE) {}
    BOOL      Create(LPCTSTR, LPCTSTR, DWORD, const RECT&, CWnd*, UINT) { return FALSE; }
    // Additional TreeCtrl methods used in TreePropertySheet
    HTREEITEM GetNextItem(HTREEITEM, UINT) const { return nullptr; }
    BOOL      Select(HTREEITEM, UINT) { return FALSE; }
    BOOL      ItemHasChildren(HTREEITEM) const { return FALSE; }
};

class CProgressCtrl : public CWnd {
public:
    void SetRange(int,int)      {}
    void SetRange32(int,int)    {}
    int  SetPos(int)            { return 0; }
    int  GetPos() const         { return 0; }
    int  OffsetPos(int)         { return 0; }
    int  StepIt()               { return 0; }
    int  SetStep(int)           { return 0; }
    void SetBarColor(COLORREF)  {}
};

// CSliderCtrl wraps a wxSlider (msctls_trackbar32).  Methods route through
// g_wxhook_slider_* keyed on m_hWnd (bound by DDX_Control).  Off-GUI builds
// (mushcore) have null hooks → safe no-ops / 0.
class CSliderCtrl : public CWnd {
public:
    void SetRange(int lo, int hi, BOOL = FALSE) {
        if (g_wxhook_slider_set_range) g_wxhook_slider_set_range(m_hWnd, lo, hi);
    }
    void SetPos(int pos) {
        if (g_wxhook_slider_set_pos) g_wxhook_slider_set_pos(m_hWnd, pos);
    }
    int  GetPos() const {
        return g_wxhook_slider_get_pos ? g_wxhook_slider_get_pos(m_hWnd) : 0;
    }
    void SetPageSize(int n) {
        if (g_wxhook_slider_set_page_size) g_wxhook_slider_set_page_size(m_hWnd, n);
    }
    int  GetPageSize() const   { return 0; }
};

class CSpinButtonCtrl : public CWnd {
public:
    // hwnd is the wxSpinButton (GetDlgItem(IDC_SPIN_*) → CWnd with m_hWnd set).
    // The arrows step the buddy edit within [lo,hi]; the buddy holds the value.
    void SetRange(int lo, int hi)   { if (g_wxhook_spin_set_range) g_wxhook_spin_set_range((void*) m_hWnd, lo, hi); }
    void SetRange32(int lo, int hi) { if (g_wxhook_spin_set_range) g_wxhook_spin_set_range((void*) m_hWnd, lo, hi); }
    void SetPos(int p)         { if (g_wxhook_spin_set_pos) g_wxhook_spin_set_pos((void*) m_hWnd, p); }
    int  GetPos() const        { return g_wxhook_spin_get_pos ? g_wxhook_spin_get_pos((void*) m_hWnd) : 0; }
    void SetBuddy(CWnd*)       {}     // buddy auto-wired at create (UDS_AUTOBUDDY) by the bridge
    CWnd* GetBuddy() const     { return nullptr; }
    void SetBase(int)          {}
};

class CIPAddressCtrl : public CWnd {
public:
    BOOL IsBlank() const { return TRUE; }
    void ClearAddress() {}
    int  GetAddress(DWORD&) const { return 0; }
    void SetAddress(DWORD) {}
};

class CDateTimeCtrl : public CWnd {};
class CMonthCalCtrl : public CWnd {};
class CAnimateCtrl  : public CWnd {};
class CHotKeyCtrl : public CWnd {
public:
    void SetHotKey(WORD, WORD) {}
    // hwnd is the wx key-capture control (made by g_wxhook_make_hotkey); read the
    // last-pressed Windows VK + HOTKEYF_* modifiers it captured.
    void GetHotKey(WORD& vk, WORD& mod) const {
        unsigned short v = 0, m = 0;
        if (g_wxhook_hotkey_get) g_wxhook_hotkey_get((void*) m_hWnd, &v, &m);
        vk = v; mod = m;
    }
    DWORD GetHotKey() const {
        unsigned short v = 0, m = 0;
        if (g_wxhook_hotkey_get) g_wxhook_hotkey_get((void*) m_hWnd, &v, &m);
        return (DWORD)((v & 0xFF) | ((m & 0xFF) << 8));   // LOBYTE=vk, HIBYTE=mods
    }
    void SetRules(WORD, WORD) {}
};
class CRichEditCtrl : public CWnd {
public:
    int  GetTextLength() const { return 0; }
    // Get/SetWindowText inherited from CWnd (wx hooks) — don't shadow.
    void AppendText(LPCTSTR) {}
    void SetReadOnly(BOOL=TRUE) {}
    // SetFont is inherited from CWnd (real, via g_wxhook_set_font) — don't shadow.
    long StreamIn(int, void*) { return 0; }
    long StreamOut(int, void*) { return 0; }
    BOOL GetDefaultCharFormat(struct CHARFORMAT& cf) const { return FALSE; }
    BOOL SetDefaultCharFormat(const struct CHARFORMAT&) { return FALSE; }
    BOOL SetSelectionCharFormat(struct CHARFORMAT&) { return FALSE; }
    void ReplaceSel(LPCTSTR, BOOL=FALSE) {}
    COLORREF SetBackgroundColor(BOOL, COLORREF) { return 0; }
    void SetSel(long, long) {}
    void GetSel(long&, long&) const {}
    int  GetSel() const { return 0; }
};

class CScrollBar : public CWnd {
public:
    void SetScrollRange(int, int, BOOL=TRUE) {}
    void GetScrollRange(int*, int*) const {}
    int  SetScrollPos(int, BOOL=TRUE) { return 0; }
    int  GetScrollPos() const { return 0; }
    BOOL EnableScrollBar(UINT=0) { return FALSE; }
};

class CToolTipCtrl : public CWnd {
public:
    BOOL Create(CWnd*, DWORD=0) { return FALSE; }
    void Activate(BOOL) {}
    BOOL AddTool(CWnd*, LPCTSTR=nullptr, LPCRECT=nullptr, UINT=0) { return TRUE; }
    BOOL AddTool(CWnd*, UINT, LPCRECT=nullptr, UINT=0) { return TRUE; }
    void DelTool(CWnd*, UINT=0) {}
    void UpdateTipText(LPCTSTR, CWnd*, UINT=0) {}
    void SetMaxTipWidth(int) {}
    void RelayEvent(MSG*) {}
    void SendMessage(UINT, WPARAM=0, LPARAM=0) {}
    int  GetDelayTime(DWORD) const { return 0; }
    void SetDelayTime(DWORD, int) {}
};

class CHeaderCtrl : public CWnd {
public:
    int  GetItemCount() const { return 0; }
    BOOL GetItem(int, void*) const { return FALSE; }
    BOOL SetItem(int, void*) { return FALSE; }
    int  InsertItem(int, void*) { return -1; }
    BOOL DeleteItem(int) { return FALSE; }
    void Layout(void*) {}
    int  OrderToIndex(int) const { return -1; }
};

class CStatusBarCtrl : public CWnd {
public:
    BOOL SetParts(int, int*) { return FALSE; }
    void SetText(LPCTSTR, int=0, int=0) {}
    BOOL GetRect(int, LPRECT) const { return FALSE; }
    BOOL GetRect(int, CRect&) const { return FALSE; }
};

// ── MFC string collection types ───────────────────────────────────────────
// mfc_ui.h is the sole provider of these MFC string-collection types on
// non-Windows (every build sees them through this header).

// CStringList — stable-pointer POSITION, compatible with MFC iteration idiom
class CStringList : public CObject {
    std::list<CString> m_data;
public:
    void     RemoveAll()               { m_data.clear(); }
    bool     IsEmpty() const           { return m_data.empty(); }
    int      GetCount() const          { return (int)m_data.size(); }
    POSITION AddTail(const CString& s) { m_data.push_back(s);  return (POSITION)&m_data.back(); }
    POSITION AddHead(const CString& s) { m_data.push_front(s); return (POSITION)&m_data.front(); }
    CString  RemoveHead() { CString v=m_data.front(); m_data.pop_front(); return v; }
    CString  RemoveTail() { CString v=m_data.back();  m_data.pop_back();  return v; }
    const CString& GetHead() const { return m_data.front(); }
    const CString& GetTail() const { return m_data.back(); }

    POSITION GetHeadPosition() const {
        return m_data.empty() ? nullptr : (POSITION)&m_data.front();
    }
    POSITION GetTailPosition() const {
        return m_data.empty() ? nullptr : (POSITION)&m_data.back();
    }
    CString& GetNext(POSITION& pos) const {
        CString* p = (CString*)pos;
        for (auto it = m_data.begin(); it != m_data.end(); ++it) {
            if (&*it == p) {
                auto cur = it++;
                pos = (it != m_data.end()) ? (POSITION)&*it : nullptr;
                return const_cast<CString&>(*cur);
            }
        }
        pos = nullptr;
        return const_cast<CString&>(m_data.back()); // fallback
    }
    CString& GetPrev(POSITION& pos) const {
        CString* p = (CString*)pos;
        for (auto it = m_data.rbegin(); it != m_data.rend(); ++it) {
            if (&*it == p) {
                auto cur = it++;
                pos = (it != m_data.rend()) ? (POSITION)&*it : nullptr;
                return const_cast<CString&>(*cur);
            }
        }
        pos = nullptr;
        return const_cast<CString&>(m_data.front());
    }
    CString& GetAt(POSITION pos) const { return const_cast<CString&>(*(CString*)pos); }
    void     RemoveAt(POSITION pos) {
        CString* p = (CString*)pos;
        for (auto it = m_data.begin(); it != m_data.end(); ++it)
            if (&*it == p) { m_data.erase(it); return; }
    }
    POSITION Find(const CString& val, POSITION = nullptr) const {
        for (auto& s : m_data) if (s == val) return (POSITION)&s;
        return nullptr;
    }
    POSITION FindIndex(int n) const {
        int i = 0;
        for (auto& s : m_data) if (i++ == n) return (POSITION)&s;
        return nullptr;
    }
    POSITION InsertAfter(POSITION pos, const CString& v) {
        for (auto it = m_data.begin(); it != m_data.end(); ++it)
            if (&*it == (CString*)pos) { auto r = m_data.insert(std::next(it), v); return (POSITION)&*r; }
        return AddTail(v);
    }
    POSITION InsertBefore(POSITION pos, const CString& v) {
        for (auto it = m_data.begin(); it != m_data.end(); ++it)
            if (&*it == (CString*)pos) { auto r = m_data.insert(it, v); return (POSITION)&*r; }
        return AddHead(v);
    }
};

// CArray<TYPE,ARG_TYPE> — minimal MFC dynamic array template
template<class TYPE, class ARG_TYPE = const TYPE&>
class CArray {
    std::vector<TYPE> m_data;
public:
    void  SetSize(int nNewSize, int = -1) { m_data.resize(nNewSize); }
    int   GetSize() const  { return (int)m_data.size(); }
    int   GetCount() const { return (int)m_data.size(); }
    bool  IsEmpty() const  { return m_data.empty(); }
    TYPE& GetAt(int i)     { return m_data[i]; }
    const TYPE& GetAt(int i) const { return m_data[i]; }
    TYPE* GetData()        { return m_data.data(); }
    const TYPE* GetData() const { return m_data.data(); }
    void  Add(ARG_TYPE v)  { m_data.push_back(v); }
    void  RemoveAll()      { m_data.clear(); }
    TYPE& operator[](int i) { return m_data[i]; }
    const TYPE& operator[](int i) const { return m_data[i]; }
};

// CStringArray — dynamic array of CStrings
class CStringArray {
    std::vector<CString> m_data;
public:
    int      GetSize() const       { return (int)m_data.size(); }
    int      GetCount() const      { return (int)m_data.size(); }
    bool     IsEmpty() const       { return m_data.empty(); }
    CString& GetAt(int i)          { return m_data[i]; }
    const CString& GetAt(int i) const { return m_data[i]; }
    void     SetAt(int i, LPCTSTR s){ m_data[i] = s; }
    void     SetSize(int n, int=-1) { m_data.resize(n); }
    int      Add(LPCTSTR s)        { m_data.push_back(s); return (int)m_data.size()-1; }
    int      Add(const CString& s) { m_data.push_back(s); return (int)m_data.size()-1; }
    void     RemoveAll()           { m_data.clear(); }
    CString& operator[](int i)     { return m_data[i]; }
    const CString& operator[](int i) const { return m_data[i]; }
};

// CMapStringToString — string-keyed CString map
class CMapStringToString {
    std::map<std::string, CString> m_data;
public:
    void RemoveAll()    { m_data.clear(); }
    bool IsEmpty() const { return m_data.empty(); }
    int  GetCount() const { return (int)m_data.size(); }
    void SetAt(LPCTSTR key, LPCTSTR val) { m_data[std::string(key)] = CString(val); }
    void SetAt(const CString& key, const CString& val) { m_data[std::string((LPCTSTR)key)] = val; }
    bool RemoveKey(LPCTSTR key) { return m_data.erase(std::string(key)) > 0; }
    bool RemoveKey(const CString& key) { return RemoveKey((LPCTSTR)key); }
    bool Lookup(LPCTSTR key, CString& val) const {
        auto it = m_data.find(std::string(key));
        if (it == m_data.end()) return false;
        val = it->second; return true;
    }
    bool Lookup(const CString& key, CString& val) const { return Lookup((LPCTSTR)key, val); }
    CString& operator[](LPCTSTR key) { return m_data[std::string(key)]; }
    POSITION GetStartPosition() const {
        return m_data.empty() ? nullptr : (POSITION)&*m_data.begin();
    }
    void GetNextAssoc(POSITION& pos, CString& key, CString& val) const {
        using P = std::pair<const std::string, CString>;
        P* p = (P*)pos;
        for (auto it = m_data.begin(); it != m_data.end(); ++it) {
            if (&*it == p) {
                key = it->first.c_str();
                val = it->second;
                ++it;
                pos = (it != m_data.end()) ? (POSITION)&*it : nullptr;
                return;
            }
        }
        pos = nullptr;
    }
};

// ── Global GDI function stubs ──────────────────────────────────────────────
// These appear as ::SaveDC(hdc) / ::RestoreDC(hdc, n) in MUSHview.cpp.
inline int  SaveDC(HDC)          { return 0; }
inline BOOL RestoreDC(HDC, int)  { return FALSE; }
inline BOOL DeleteDC(HDC)        { return FALSE; }
inline HDC  CreateCompatibleDC(HDC) { return nullptr; }
inline HBITMAP CreateCompatibleBitmap(HDC,int,int) { return nullptr; }
#ifndef MUSHCLIENT_FULL_BUILD
inline HGDIOBJ SelectObject(HDC, HGDIOBJ) { return nullptr; }
#else
// Real implementation in wx_gdi_compat.cpp (selects a DIBSection's wxBitmap
// into the CDC); resolved at the mushgui link step.  Mirrors CreateDIBSection
// and GradientFill below.
HGDIOBJ SelectObject(HDC hdc, HGDIOBJ hObject);
#endif
#ifndef MUSHCLIENT_FULL_BUILD
inline BOOL DeleteObject(HGDIOBJ) { return FALSE; }
#else
// Real implementation in wx_gdi_compat.cpp — frees a CreateDIBSection handle
// (DIBSectionData + pixel buffer + wxBitmap) and erases it from g_dibSections.
// miniwindow.cpp calls this on raw CreateDIBSection HBITMAPs; a no-op leaked them.
BOOL DeleteObject(HGDIOBJ hObject);
#endif
inline BOOL BitBlt(HDC,int,int,int,int,HDC,int,int,DWORD) { return FALSE; }
inline BOOL Rectangle(HDC,int,int,int,int) { return FALSE; }
inline BOOL MoveToEx(HDC,int,int,POINT*) { return FALSE; }
inline BOOL LineTo(HDC,int,int) { return FALSE; }
inline COLORREF SetTextColor(HDC, COLORREF) { return 0; }
inline COLORREF SetBkColor(HDC, COLORREF) { return 0; }
inline int SetBkMode(HDC, int) { return 0; }
inline BOOL TextOut(HDC,int,int,LPCTSTR,int) { return FALSE; }
inline int DrawText(HDC,LPCTSTR,int,LPRECT,UINT) { return 0; }
inline BOOL GetTextMetrics(HDC, TEXTMETRIC*) { return FALSE; }
#ifndef MUSHCLIENT_FULL_BUILD
inline int GetDeviceCaps(HDC, int) { return 0; }
#else
int GetDeviceCaps(HDC hdc, int nIndex);  // implemented in wx_gdi_compat.cpp
#endif
// GetSystemMetrics defined later in this file — avoid duplication
inline BOOL FillRect(HDC, LPCRECT, HBRUSH) { return FALSE; }
inline BOOL FillRect(HDC hdc, const CRect* r, HBRUSH br) { return FillRect(hdc, (LPCRECT)(const RECT*)r, br); }
inline HBRUSH CreateSolidBrush(COLORREF) { return nullptr; }
inline HPEN CreatePen(int,int,COLORREF) { return nullptr; }
inline HFONT CreateFontIndirect(const LOGFONT*) { return nullptr; }
inline HFONT GetStockObject(int) { return nullptr; }
inline BOOL SetWindowPos(HWND,HWND,int,int,int,int,UINT) { return FALSE; }
inline BOOL GetClientRect(HWND, RECT*) { return FALSE; }
inline BOOL PtInRect(const RECT* r, POINT p)
    { return r && p.x>=r->left && p.x<r->right && p.y>=r->top && p.y<r->bottom; }
inline BOOL GetWindowRect(HWND, RECT*) { return FALSE; }
inline BOOL InvalidateRect(HWND, LPCRECT, BOOL) { return FALSE; }
inline BOOL UpdateWindow(HWND) { return FALSE; }
inline BOOL ShowWindow(HWND, int) { return FALSE; }
inline BOOL EnableWindow(HWND, BOOL) { return FALSE; }
inline HWND GetDlgItem(HWND, int) { return nullptr; }
inline BOOL SetDlgItemText(HWND, int, LPCTSTR) { return FALSE; }
inline UINT GetDlgItemInt(HWND, int, BOOL*, BOOL) { return 0; }
inline BOOL CheckDlgButton(HWND, int, UINT) { return FALSE; }
inline UINT IsDlgButtonChecked(HWND, int) { return 0; }
// Single-argument form used as CWnd member — defined on CWnd below
// Route to the wx message-box hook (same path as ::UMessageBox) so utils.msgbox
// and the startup error/info boxes show a real dialog and return the user's
// choice; fall back to stderr for headless builds where the hook is unset.
inline int  MessageBox(HWND, LPCTSTR msg, LPCTSTR caption, UINT type)
    {
    if (g_wxhook_message_box)
        return g_wxhook_message_box(msg ? msg : "",
                                    caption ? caption : "MUSHclient", type);
    fprintf(stderr, "MUSHclient [%s]: %s\n",
            caption ? caption : "MUSHclient", msg ? msg : "");
    return (type & MB_YESNO) ? IDNO : IDOK;
    }
inline BOOL PostMessage(HWND, UINT, WPARAM, LPARAM) { return FALSE; }
inline LRESULT SendMessage(HWND, UINT, WPARAM, LPARAM) { return 0; }
inline BOOL SendDlgItemMessage(HWND, int, UINT, WPARAM, LPARAM) { return FALSE; }
inline int  SetScrollPos(HWND, int, int, BOOL) { return 0; }
inline int  GetScrollPos(HWND, int) { return 0; }
inline BOOL SetScrollRange(HWND, int, int, int, BOOL) { return FALSE; }

// Additional Win32 API stubs needed by dialog code
inline HANDLE LoadImage(HINSTANCE, LPCTSTR, UINT, int, int, UINT) { return nullptr; }
// FindResource/LoadResource/SizeofResource/LockResource for RT-"TEXT"
// int-resources (the About box's licence + credits) route to bundled files via
// g_wxhook_load_text_resource.  The "handle" carried through the chain is just
// the resource id; the bytes are resolved (and cached) by the hook on Size/Lock.
// `type` is a string for "TEXT" (pointer > 0xFFFF); `name` is MAKEINTRESOURCE(id)
// (value <= 0xFFFF).  Anything else → null (the canonical caller then degrades).
inline HRSRC  FindResource(HINSTANCE, LPCTSTR name, LPCTSTR type) {
    if (type && (uintptr_t) type > 0xFFFF && std::strcmp(type, "TEXT") == 0 &&
        name != 0 && (uintptr_t) name <= 0xFFFF)
        return (HRSRC)(uintptr_t) name;          // handle = resource id
    return nullptr;
}
inline HANDLE LoadResource(HINSTANCE, HRSRC h) { return (HANDLE) h; }
inline DWORD  SizeofResource(HINSTANCE, HRSRC h) {
    unsigned sz = 0;
    if (g_wxhook_load_text_resource)
        g_wxhook_load_text_resource((int)(uintptr_t) h, &sz);
    return sz;
}
inline void*  LockResource(HANDLE h) {
    unsigned sz = 0;
    return g_wxhook_load_text_resource
         ? (void*) g_wxhook_load_text_resource((int)(uintptr_t) h, &sz) : nullptr;
}
inline BOOL   GetDIBColorTable(HDC, UINT, UINT, void*) { return FALSE; }
inline BOOL   PeekMessage(MSG*, HWND, UINT, UINT, UINT) { return FALSE; }
inline BOOL   TranslateMessage(const MSG*) { return FALSE; }
inline LRESULT DispatchMessage(const MSG*) { return 0; }
inline BOOL   IsDialogMessage(HWND, MSG*) { return FALSE; }
inline BOOL   DrawFocusRect(HDC, const RECT*) { return FALSE; }
inline BOOL   DrawFocusRect(HDC hdc, const CRect* r) { return DrawFocusRect(hdc, (const RECT*)r); }
inline HCURSOR LoadCursor(HINSTANCE, LPCTSTR) { return nullptr; }
inline HICON  LoadIcon(HINSTANCE, LPCTSTR) { return nullptr; }
// GetSystemMenu: declared below with correct HMENU return type
inline HINSTANCE AfxGetResourceHandle() { return nullptr; }
inline HINSTANCE AfxGetInstanceHandle() { return nullptr; }
// AfxRegisterWndClass — on Windows returns an atom cast to LPCTSTR; here we
// return a fixed stub class name so CreateEx callers see a valid non-null string.
inline LPCTSTR AfxRegisterWndClass(UINT, HCURSOR=nullptr, HBRUSH=nullptr, HICON=nullptr) { return "MUSHclientWnd"; }
// GetButtonSize — declared in stdafx.h; implemented in Utilities.cpp


// SWP_ flags for SetWindowPos
#ifndef SWP_NOSIZE
#  define SWP_NOSIZE       0x0001
#  define SWP_NOMOVE       0x0002
#  define SWP_NOZORDER     0x0004
#  define SWP_NOREDRAW     0x0008
#  define SWP_NOACTIVATE   0x0010
#  define SWP_SHOWWINDOW   0x0040
#  define SWP_HIDEWINDOW   0x0080
#  define SWP_FRAMECHANGED 0x0020
#  define HWND_TOP         ((HWND)0)
#  define HWND_BOTTOM      ((HWND)1)
#  define HWND_TOPMOST     ((HWND)-1)
#  define HWND_NOTOPMOST   ((HWND)-2)
#endif

// ODT / ODS flags for owner-draw controls
#ifndef ODT_MENU
#  define ODT_MENU        1
#  define ODT_LISTBOX     2
#  define ODT_COMBOBOX    3
#  define ODT_BUTTON      4
#  define ODS_SELECTED    0x0001
#  define ODS_GRAYED      0x0002
#  define ODS_DISABLED    0x0004
#  define ODS_CHECKED     0x0008
#  define ODS_FOCUS       0x0010
#  define ODA_DRAWENTIRE  0x0001
#  define ODA_SELECT      0x0002
#  define ODA_FOCUS       0x0004
#endif

// Common dialogs
class CColorDialog : public CDialog {
public:
    CHOOSECOLOR m_cc;  // embedded by value (Windows CColorDialog::m_cc)
    CColorDialog(COLORREF cr=0, DWORD=0, CWnd* = nullptr) { m_cc.rgbResult = cr; }
    COLORREF GetColor() const { return m_cc.rgbResult; }
};

class CFontDialog : public CDialog {
public:
    LOGFONT    m_lf = {};
    CHOOSEFONT m_cf = {};
    // Results populated by DoModal() on IDOK.
    CString  m_resFace;
    int      m_resPoints    = 0;          // chosen size, whole points
    int      m_resWeight    = FW_NORMAL;
    bool     m_resItalic    = false;
    bool     m_resUnderline = false;
    bool     m_resStrike    = false;
    int      m_resCharset   = 0;
    COLORREF m_resColor     = 0;

    CFontDialog(LOGFONT* pLF = nullptr, DWORD = 0, CDC* = nullptr, CWnd* = nullptr) {
        if (pLF) m_lf = *pLF;
        m_cf.lpLogFont = &m_lf;
    }

    // Run wxFontDialog via the bridge (g_wxhook_font_dialog).  Falls back to
    // IDCANCEL in headless/mushcore builds where the hook is null.
    INT_PTR DoModal() override {
        if (!g_wxhook_font_dialog) return IDCANCEL;
        WxFontChoice fc = {};
        strncpy(fc.faceName, m_lf.lfFaceName, sizeof(fc.faceName) - 1);
        fc.lfHeight = m_lf.lfHeight;
        fc.weight   = m_lf.lfWeight ? (int) m_lf.lfWeight : (int) FW_NORMAL;
        fc.italic   = m_lf.lfItalic ? 1 : 0;
        if (g_wxhook_font_dialog(nullptr, &fc) != 1)
            return IDCANCEL;
        m_resFace      = fc.faceName;
        m_resPoints    = fc.pointSize;
        m_resWeight    = fc.weight;
        m_resItalic    = fc.italic    != 0;
        m_resUnderline = fc.underline != 0;
        m_resStrike    = fc.strikeout != 0;
        m_resColor     = fc.color;
        m_resCharset   = m_lf.lfCharSet;   // dialog does not choose a charset
        // Mirror the result back into m_lf so callers reading m_cf.lpLogFont
        // (== &m_lf) observe the chosen face/weight/italic.
        strncpy(m_lf.lfFaceName, fc.faceName, sizeof(m_lf.lfFaceName) - 1);
        m_lf.lfFaceName[sizeof(m_lf.lfFaceName) - 1] = 0;
        m_lf.lfWeight = fc.weight;
        m_lf.lfItalic = (BYTE)(fc.italic ? 1 : 0);
        return IDOK;
    }

    CString  GetFaceName()  const { return m_resFace; }
    CString  GetStyleName() const { return {}; }
    int      GetSize()      const { return m_resPoints * 10; }   // tenths of a point
    int      GetWeight()    const { return m_resWeight; }
    COLORREF GetColor()     const { return m_resColor; }
    BOOL     IsItalic()     const { return m_resItalic    ? TRUE : FALSE; }
    BOOL     IsBold()       const { return m_resWeight >= FW_BOLD ? TRUE : FALSE; }
    BOOL     IsUnderline()  const { return m_resUnderline ? TRUE : FALSE; }
    BOOL     IsStrikeOut()  const { return m_resStrike    ? TRUE : FALSE; }
    int      GetCharset()   const { return m_resCharset; }
};

// Defined out-of-line: needs CDocument complete to touch m_pDocTemplate.
inline void CMultiDocTemplate::AddDocument(CDocument* d) {
    m_docs.push_back(d);
    if (d) d->m_pDocTemplate = this;
}

class CFileDialog : public CDialog {
public:
    OPENFILENAME m_ofn = {};   // MFC exposes the raw OPENFILENAME struct
    bool    m_bOpenFileDialog = true;
    CString m_strResult;       // path selected by the user (set by DoModal on OK)

    CFileDialog(BOOL bOpenFileDialog,
                LPCTSTR lpszDefExt      = nullptr,
                LPCTSTR lpszFileName    = nullptr,
                DWORD   dwFlags         = 0,
                LPCTSTR lpszFilter      = nullptr,
                CWnd*   /*pParentWnd*/  = nullptr)
        : m_bOpenFileDialog(bOpenFileDialog != FALSE)
    {
        m_ofn.lpstrDefExt = lpszDefExt;
        m_ofn.Flags       = dwFlags;
        m_ofn.lpstrFilter = lpszFilter;
        // lpszFileName and lpstrFile/lpstrTitle are usually set by callers
        // after construction via m_ofn.lpstrFile / m_ofn.lpstrTitle.
        if (lpszFileName && *lpszFileName)
            m_strResult = lpszFileName;  // pre-fill as hint
    }

    INT_PTR DoModal() override {
        // Derive the initial-filename hint from whichever is set.
        const char* initFile = m_ofn.lpstrFile && *m_ofn.lpstrFile
                                   ? m_ofn.lpstrFile
                                   : static_cast<const char*>(m_strResult);

        // Strip any directory component from the hint so wxFileDialog gets
        // just the leaf name in its "File name" field.
        const char* leaf = initFile ? initFile : "";
        const char* p    = strrchr(leaf, '/');
        if (!p) p        = strrchr(leaf, '\\');
        if (p)  leaf     = p + 1;

        // Initial directory: prefer m_ofn.lpstrInitialDir; fall back to
        // extracting it from the full initial path.
        CString initDir;
        if (m_ofn.lpstrInitialDir && *m_ofn.lpstrInitialDir) {
            initDir = m_ofn.lpstrInitialDir;
        } else if (initFile && (p = strrchr(initFile, '/')) != nullptr) {
            initDir = CString(initFile, (int)(p - initFile));
        }

        bool overwrite = (m_ofn.Flags & OFN_OVERWRITEPROMPT) != 0;

        if (g_wxhook_file_dialog) {
            char* path = g_wxhook_file_dialog(
                m_bOpenFileDialog,
                m_ofn.lpstrTitle,
                initDir.IsEmpty() ? nullptr : static_cast<const char*>(initDir),
                leaf,
                m_ofn.lpstrFilter,
                overwrite);
            if (path) {
                m_strResult = path;
                free(path);
                return IDOK;
            }
        }
        return IDCANCEL;
    }

    CString GetPathName() const { return m_strResult; }

    CString GetFileName() const {
        int sep = m_strResult.ReverseFind('/');
        if (sep < 0) sep = m_strResult.ReverseFind('\\');
        return (sep >= 0) ? m_strResult.Mid(sep + 1) : m_strResult;
    }

    CString GetFileExt() const {
        CString name = GetFileName();
        int dot = name.ReverseFind('.');
        return (dot >= 0) ? name.Mid(dot + 1) : CString{};
    }

    CString GetFileTitle() const {
        CString name = GetFileName();
        int dot = name.ReverseFind('.');
        return (dot >= 0) ? name.Left(dot) : name;
    }

    void SetDefExt(LPCTSTR ext) { m_ofn.lpstrDefExt = ext; }

    // Multi-file selection (OFN_ALLOWMULTISELECT): not implemented — single-
    // select only on non-Windows.  Iteration returns the one selected file.
    POSITION GetStartPosition() const {
        return m_strResult.IsEmpty() ? nullptr : (POSITION)1;
    }
    CString GetNextPathName(POSITION& pos) const {
        pos = nullptr;
        return m_strResult;
    }
};

class CFindReplaceDialog : public CDialog {
public:
    static CFindReplaceDialog* GetNotifier(LPARAM) { return nullptr; }
    CString GetFindString() const    { return {}; }
    CString GetReplaceString() const { return {}; }
    BOOL IsTerminating() const       { return FALSE; }
    BOOL FindNext() const            { return FALSE; }
    BOOL ReplaceCurrent() const      { return FALSE; }
    BOOL ReplaceAll() const          { return FALSE; }
    BOOL MatchCase() const           { return FALSE; }
    BOOL MatchWholeWord() const      { return FALSE; }
    BOOL SearchDown() const          { return FALSE; }
};

// PROPSHEETPAGE stub — used by CPropertyPage::m_psp
struct PROPSHEETPAGE { DWORD dwFlags = 0; };
#ifndef PSP_PREMATURE
#  define PSP_PREMATURE 0x0400
#endif

class CPropertyPage : public CDialog {
    SHIM_RUNTIME_CLASS(CPropertyPage, CDialog)
public:
    PROPSHEETPAGE m_psp;   // Windows exposes this raw struct member
    explicit CPropertyPage(UINT id = 0) : CDialog(id) {}
    virtual BOOL OnSetActive()   { return TRUE; }
    virtual BOOL OnKillActive()  { return TRUE; }
    virtual void OnReset()       {}
    virtual void OnOK()          {}
    virtual void OnCancel()      {}
    virtual BOOL OnApply()       { return TRUE; }
    void CancelToClose()         {}   // changes Cancel→Close once changes applied
    void SetModified(BOOL=TRUE)  {}
    void MapWindowPoints(CWnd*, RECT*) const {}
    virtual void OnRButtonDown(UINT, CPoint) {}
    virtual void OnContextMenu(CWnd*, CPoint) {}
    virtual BOOL OnHelpInfo(void*) { return FALSE; }
};

class CPropertySheet : public CWnd {
    SHIM_RUNTIME_CLASS(CPropertySheet, CWnd)
public:
    CPropertySheet(LPCTSTR caption=nullptr, CWnd* = nullptr, UINT=0) { if (caption) m_strSheetCaption = caption; }
    CPropertySheet(UINT nCaptionId, CWnd* = nullptr, UINT=0) : m_nSheetCaptionId(nCaptionId) {}
    // Port: record the added pages so the bridge can render a wxNotebook of the
    // canonical CPropertyPage subclasses (see g_run_mfc_property_sheet_flat /
    // hook_run_property_sheet_flat).  CTreePropertySheet overrides AddPage/DoModal
    // (tree structure → wxTreebook) and is unaffected.
    void AddPage(CPropertyPage* p) { if (p) m_flatPages.push_back(p); }
    // Out-of-line (in wx_tree_property_sheet.cpp): routes to the flat-sheet hook.
    // Virtual so CTreePropertySheet::DoModal properly overrides it.
    virtual INT_PTR DoModal();
    void SetActivePage(int)   {}
    void SetActivePage(CPropertyPage*) {}
    int  GetActiveIndex() const { return 0; }
    CPropertyPage* GetActivePage() const { return nullptr; }
    CPropertyPage* GetPage(int) const { return nullptr; }
    int  GetPageCount() const   { return 0; }
    void EnableStackedTabs(BOOL) {}
    void SetTitle(LPCTSTR s, UINT=0) { if (s) m_strSheetCaption = s; }
    virtual BOOL OnInitDialog() { return TRUE; }
    void SetWizardMode() {}
    void SetFinishText(LPCTSTR) {}
    void SetWizardButtons(DWORD) {}
    int  GetPageIndex(CPropertyPage*) const { return -1; }
    CTabCtrl* GetTabControl() const { return nullptr; }
    // Mouse/paint virtuals needed by TreePropertySheet which subclasses CPropertySheet
    virtual void OnLButtonDown(UINT, CPoint) {}
    virtual void OnLButtonUp(UINT, CPoint) {}
    virtual void OnMouseMove(UINT, CPoint) {}
    virtual void OnPaint() {}
    virtual BOOL OnEraseBkgnd(CDC*) { return FALSE; }
    virtual void OnSelchangeTab(NMHDR*, LRESULT*) {}
    // Property page helpers
    CPropertyPage* GetPage(CPropertyPage*) const { return nullptr; }
    void           RemovePage(CPropertyPage*) {}
    void           RemovePage(int) {}

    // Port: pages recorded by AddPage(), handed to the flat-sheet bridge hook.
    std::vector<CPropertyPage*> m_flatPages;
    // Port: the sheet's window caption.  Set directly from the LPCTSTR ctor /
    // SetTitle, or as a string-resource id from the UINT ctor (the bridge maps
    // the id to text — the port has no string table).  See hook_run_property_sheet*.
    CString m_strSheetCaption;
    UINT    m_nSheetCaptionId = 0;
};

// ── Splitter window ────────────────────────────────────────────────────────
class CSplitterWnd : public CWnd {
public:
    BOOL CreateStatic(CWnd*, int, int, DWORD=0, UINT=0) { return FALSE; }
    BOOL CreateView(int, int, const CRuntimeClass*, CSize, void*) { return FALSE; }
    CWnd* GetPane(int, int) const { return nullptr; }
    void  SetColumnInfo(int, int, int) {}
    void  SetRowInfo(int, int, int) {}
    int   GetRowCount() const { return 0; }
    int   GetColumnCount() const { return 0; }
    void  GetRowInfo(int, int&, int&) const {}
    void  GetColumnInfo(int, int&, int&) const {}
    void  RecalcLayout() {}
    virtual void OnLButtonUp(UINT, CPoint) {}
};

// ── Frame / app classes ────────────────────────────────────────────────────
class CFrameWnd : public CWnd {
    SHIM_RUNTIME_CLASS(CFrameWnd, CWnd)
public:
    // Standard frame help/context command handlers referenced by message maps
    // (CMDIFrameWnd inherits these); stubs on the port.
    void OnContextHelp() {}
    void OnHelp() {}
    void OnHelpIndex() {}
    void OnHelpFinder() {}
    void OnHelpUsing() {}
    void       SetActiveView(CView*) {}
    CView*     GetActiveView() const { return nullptr; }
    CDocument* GetActiveDocument() const { return nullptr; }
    CFrameWnd* GetActiveFrame() { return nullptr; }
    void    ActivateFrame(int=-1) {}
    void    RecalcLayout(BOOL=TRUE) {}
    void    InitialUpdateFrame(CDocument*, BOOL=TRUE) {}
    CWnd*   m_pWndMDIClient = nullptr;
    CStatusBar* m_pStatusBar = nullptr;
    CToolBar*   m_pToolBar   = nullptr;
};

// CControlBar — base class for toolbars and dialog bars
class CControlBar : public CWnd {
public:
    void EnableDocking(DWORD) {}
    DWORD GetBarStyle() const { return 0; }
    void  SetBarStyle(DWORD) {}
};

class CMDIChildWnd : public CFrameWnd {
    SHIM_RUNTIME_CLASS(CMDIChildWnd, CFrameWnd)
public:
    void MDIMaximize() {}
    void MDIRestore() {}
    CMDIChildWnd* MDIGetActive(BOOL* = nullptr) const { return nullptr; }
    void SetHandles(HMENU, HACCEL) {}
    HACCEL GetDefaultAccelerator() { return nullptr; }
    // Windows calls OnUpdateFrameMenu(bActive, pActivateWnd, hMenuAlt)
    virtual void OnUpdateFrameMenu(BOOL, CWnd*, HMENU) {}
    virtual BOOL PreCreateWindow(CREATESTRUCT&) { return TRUE; }
    virtual BOOL OnCreateClient(LPCREATESTRUCT, void*) { return TRUE; }
    virtual void OnClose() {}
    virtual void OnDestroy() {}
    virtual void OnSize(UINT, int, int) {}
    virtual void OnActivate(UINT, CWnd*, BOOL) {}
};

class CMDIFrameWnd : public CFrameWnd {
    SHIM_RUNTIME_CLASS(CMDIFrameWnd, CFrameWnd)
public:
    HWND m_hWndMDIClient = nullptr;

    // LoadFrame creates the real wxMDIParentFrame via the hook, then calls
    // OnCreate() so the subclass (CMainFrame) can populate toolbars etc.
    // Body is in mushapp_stubs.cpp (not inline) to avoid stale-PCH issues.
    BOOL LoadFrame(UINT id,
                   DWORD style = 0,
                   CWnd* pParent = nullptr,
                   void* pContext = nullptr);

    CMDIChildWnd* MDIGetActive(BOOL* = nullptr) const { return nullptr; }
    void MDINext() {}
    void MDIPrev() {}
    void MDIIconArrange() {}
    void MDITile(UINT=0) {}
    void MDICascade(UINT=0) {}
    HMENU GetWindowMenuPopup(HMENU) { return 0; }
    // Docking / control bars
    void EnableDocking(DWORD) {}
    void DockControlBar(CControlBar*, UINT=0, LPCRECT=nullptr) {}
    void FloatControlBar(CControlBar*, CPoint, DWORD=0) {}
    void SaveBarState(LPCTSTR) {}
    void LoadBarState(LPCTSTR) {}
    BOOL ShowControlBar(CControlBar*, BOOL, BOOL) { return FALSE; }
    // MFC overridables that mainfrm.cpp overrides
    virtual BOOL PreTranslateMessage(MSG*) { return FALSE; }
    virtual BOOL PreCreateWindow(CREATESTRUCT&) { return TRUE; }
    virtual LRESULT WindowProc(UINT, WPARAM, LPARAM) { return 0; }
    virtual void OnUpdateFrameMenu(HMENU) {}
    virtual void OnUpdateFrameTitle(BOOL) {}
    // Message handlers (overridable by CMainFrame)
    virtual void OnClose() {}
    virtual void OnDestroy() {}
    virtual void OnSize(UINT, int, int) {}
    virtual void OnActivate(UINT, CWnd*, BOOL) {}
    virtual void OnTimer(UINT) {}
    virtual void OnSysCommand(UINT, LPARAM) {}
    virtual void OnUpdateControlBarMenu(CCmdUI*) {}
    virtual BOOL OnBarCheck(UINT) { return FALSE; }
    // Help handlers
    virtual void OnContextHelp() {}
    virtual void OnHelpFinder() {}
    virtual void OnHelpUsing() {}
    virtual void OnHelpIndex() {}
};

class CToolBarCtrl : public CWnd {
public:
    BOOL  SetButtonInfo(int, void*) { return FALSE; }
    int   GetButtonCount() const { return 0; }
    BOOL  GetItemRect(int, RECT*) const { return FALSE; }
};

class CToolBar : public CControlBar {
public:
    BOOL Create(CWnd*, DWORD=0, UINT=0) { return FALSE; }
    BOOL CreateEx(CWnd*, DWORD=0, DWORD=0, CRect=CRect(), UINT=0) { return FALSE; }
    BOOL LoadToolBar(UINT) { return FALSE; }
    BOOL LoadBitmap(UINT) { return FALSE; }
    BOOL SetButtons(const UINT*, int) { return FALSE; }
    void SetSizes(CSize, CSize) {}
    void SetHeight(int) {}
    void DrawGripper(CDC*, const CRect&) {}
    void DrawBorders(CDC*, CRect&) {}
    void EraseNonClient() {}
    virtual void DoPaint(CDC*) {}
    int  CommandToIndex(UINT) const { return -1; }
    CToolBarCtrl& GetToolBarCtrl() { static CToolBarCtrl s; return s; }
};

class CStatusBar : public CWnd {
public:
    BOOL Create(CWnd*, DWORD=0, UINT=0) { return FALSE; }
    BOOL SetIndicators(const UINT*, int) { return FALSE; }
    void SetPaneText(int, LPCTSTR, BOOL=TRUE) {}
    void SetPaneInfo(int, UINT, UINT, int) {}
    void GetPaneInfo(int, UINT&, UINT&, int&) const {}
    CString GetPaneText(int) const { return {}; }
    CStatusBarCtrl& GetStatusBarCtrl() { static CStatusBarCtrl s; return s; }
    int  CommandToIndex(UINT) const { return -1; }
};

class CReBar : public CWnd {
public:
    BOOL Create(CWnd*, DWORD=0, UINT=0) { return FALSE; }
    BOOL AddBar(CWnd*, LPCTSTR lp=nullptr, CBitmap* bm=nullptr, DWORD dw=0) { return FALSE; }
};

class CDialogBar : public CControlBar {
public:
    BOOL Create(CWnd*, UINT, UINT, UINT) { return FALSE; }
};

// ── CCommandLineInfo stub ─────────────────────────────────────────────────
class CCommandLineInfo {
public:
    BOOL m_bShowSplash = TRUE;
    BOOL m_bRunEmbedded = FALSE;
    BOOL m_bRunAutomated = FALSE;
    int  m_nShellCommand = 0;  // FileNew = 0
    CString m_strFileName;
    CString m_strPrinterName;
    CString m_strDriverName;
    CString m_strPortName;
};

// ── CWinApp ───────────────────────────────────────────────────────────────
class CWinApp : public CCmdTarget {
    SHIM_RUNTIME_CLASS(CWinApp, CCmdTarget)
public:
    // Constructor sets the global g_pWinApp so AfxGetApp() returns this instance.
    CWinApp() { g_pWinApp = this; }

    // Standard MFC app command handlers referenced by message maps (the real
    // new/open is CMUSHclientApp's own; these are the framework defaults — stubs).
    void OnFileNew() {}
    void OnFileOpen() {}
    void OnFilePrintSetup() {}

    LPCTSTR    m_lpszAppName      = nullptr;
    LPCTSTR    m_lpszProfileName  = nullptr;
    HINSTANCE  m_hInstance        = nullptr;
    HINSTANCE  m_hPrevInstance    = nullptr;
    LPCTSTR    m_lpCmdLine        = nullptr;
    int        m_nCmdShow         = 0;
    CWnd*      m_pMainWnd         = nullptr;
    CWnd*      m_pActiveWnd       = nullptr;

    virtual BOOL    InitInstance()    { return TRUE; }
    virtual int     ExitInstance()    { return 0; }
    virtual int     Run()             { return 0; }
    virtual BOOL    OnIdle(LONG)      { return FALSE; }
    virtual void    WinHelp(DWORD_PTR, UINT=0) {}
    virtual int     DoMessageBox(LPCTSTR, UINT, UINT) { return IDOK; }

    // Profile / registry
    CString  GetProfileString(LPCTSTR sec, LPCTSTR key, LPCTSTR def=nullptr) { return def ? def : ""; }
    UINT     GetProfileInt(LPCTSTR, LPCTSTR, int def=0) { return (UINT)def; }
    BOOL     WriteProfileString(LPCTSTR, LPCTSTR, LPCTSTR) { return FALSE; }
    BOOL     WriteProfileInt(LPCTSTR, LPCTSTR, int) { return FALSE; }

    // Document templates
    void     AddDocTemplate(CDocTemplate*) {}
    CDocument* OpenDocumentFile(LPCTSTR) { return nullptr; }

    // MRU
    void     AddToRecentFileList(LPCTSTR) {}
    void     LoadStdProfileSettings(UINT=4) {}

    // Thread/message
    BOOL     PumpMessage() { return FALSE; }

    // File dialog prompt (used by OnSaveDocument path)
    // Ask the user for a file name, seeded with `fileName`.  This used to be
    // `{ return FALSE; }`, and the consequences were bad in both build types:
    // CMUSHclientDoc::DoSave's empty-path arm (doc.cpp:2181-2211) computes a
    // suggested name, calls this to confirm it, and `if (!bResult) return
    // FALSE;` — so saving a world that has never been saved SILENTLY DID
    // NOTHING.  Reachable from `world.Save()` with no argument, and from
    // auto-save-on-close at quit for a File > New World that was never saved,
    // where it meant quiet data loss.  (In a debug build the ASSERT on the line
    // above aborted first; the shipped .app is Release, so it took the silent
    // arm.)  Windows shows a Save As dialog here — so do that.
    //
    // MFC implements this with a CFileDialog too; pTemplate only supplied the
    // filter/extension, which we spell out instead since the port's
    // CDocTemplate carries no strings.
    BOOL DoPromptFileName(CString& fileName, UINT /*nIDSTitle*/, DWORD lFlags,
                          BOOL bOpenFileDialog, CDocTemplate* /*pTemplate*/)
    {
        CFileDialog dlg(bOpenFileDialog,
                        "mcl",
                        fileName.IsEmpty() ? nullptr : (LPCTSTR) fileName,
                        lFlags | (bOpenFileDialog ? 0u : (DWORD) OFN_OVERWRITEPROMPT),
                        "MUSHclient world files (*.mcl)|*.mcl|All files (*.*)|*.*");
        dlg.m_ofn.lpstrTitle = bOpenFileDialog ? "Open World File" : "Save World File";
        if (dlg.DoModal() != IDOK)
            return FALSE;
        CString chosen = dlg.GetPathName();
        if (chosen.IsEmpty())
            return FALSE;
        fileName = chosen;
        return TRUE;
    }

    // Icon / cursor loading
    HICON   LoadIcon(UINT) const   { return nullptr; }
    HICON   LoadIcon(LPCTSTR) const { return nullptr; }
    HCURSOR LoadCursor(UINT) const  { return nullptr; }
    HCURSOR LoadCursor(LPCTSTR) const { return nullptr; }
    HCURSOR LoadStandardCursor(LPCTSTR) const { return nullptr; }
    HBITMAP LoadBitmap(UINT) const  { return nullptr; }

    // Additional methods used in root .cpp files
    virtual BOOL PreTranslateMessage(MSG*) { return FALSE; }
    virtual BOOL SaveAllModified() { return TRUE; }
    void   ParseCommandLine(CCommandLineInfo&) {}
    void   ParseCommandLine(void*) {}
    BOOL   DoPrintDialog(void*) { return FALSE; }
    void   HelpHelper(DWORD_PTR, UINT) {}
    void   RegisterShellFileTypes(BOOL=FALSE) {}
    void   EnableShellOpen() {}
    void   LoadFrame(UINT) {}
    virtual void OnHelp() {}
    CString m_strRecentFiles;
    LPCTSTR m_pszProfileName = nullptr;  // alias used in some code
};

// ── AfxGetMainWnd — defined here so CWinApp::m_pMainWnd is accessible ────
// compat.h forward-declares this; the real body lives here after CWinApp.
inline CWnd* AfxGetMainWnd() { return g_pWinApp ? g_pWinApp->m_pMainWnd : nullptr; }

// ── AfxGetFileTitle ────────────────────────────────────────────────────────
// Declared in doc.cpp as "UINT AFXAPI AfxGetFileTitle(...)".
// Provide the definition here (inline) so the linker is satisfied.
inline UINT AfxGetFileTitle(LPCTSTR lpszPathName, LPTSTR lpszTitle, UINT nMax)
{
    if (!lpszTitle || nMax == 0) {
        // Return length needed
        const char* p = strrchr(lpszPathName, '/');
        return p ? (UINT)strlen(p+1) : (UINT)strlen(lpszPathName);
    }
    const char* p = strrchr(lpszPathName, '/');
    const char* name = p ? p+1 : lpszPathName;
    // Strip extension
    const char* dot = strrchr(name, '.');
    size_t len = dot ? (size_t)(dot - name) : strlen(name);
    if (len >= nMax) len = nMax - 1;
    strncpy(lpszTitle, name, len);
    lpszTitle[len] = '\0';
    return 0;
}

// ── AfxMessageBox ─────────────────────────────────────────────────────────
inline int AfxMessageBox(LPCTSTR msg, UINT = MB_OK, UINT = 0)
{
    fprintf(stderr, "[AfxMessageBox] %s\n", msg ? msg : "");
    return IDOK;
}
// Overload for callers that pass a string-table resource ID (IDP_* / IDS_*).
inline int AfxMessageBox(UINT nIDPrompt, UINT nType = MB_OK, UINT nIDHelp = 0)
{
    fprintf(stderr, "[AfxMessageBox] (resource id %u)\n", nIDPrompt);
    return IDOK;
}

// ── Windows API function stubs ────────────────────────────────────────────
// These are called directly in various source files.
inline BOOL   IsIconic(HWND)          { return FALSE; }
inline BOOL   IsZoomed(HWND)          { return FALSE; }
inline BOOL   IsWindowVisible(HWND)   { return FALSE; }
inline void   PostQuitMessage(int)     {}
inline UINT   RegisterWindowMessage(LPCTSTR) { return 0; }
// LoadIcon / LoadCursor already defined above
inline BOOL   ScreenToClient(HWND, POINT*)   { return FALSE; }
inline BOOL   ClientToScreen(HWND, POINT*)   { return FALSE; }
inline BOOL   ClientToScreen(HWND, CPoint*)  { return FALSE; }  // CPoint* overload
inline BOOL   ScreenToClient(HWND, CPoint*)  { return FALSE; }
inline BOOL   GetCursorPos(POINT*)           { return FALSE; }
inline BOOL   GetCursorPos(CPoint*)          { return FALSE; }
inline BOOL   SetCursorPos(int, int)         { return FALSE; }
inline HCURSOR SetCursor(HCURSOR)            { return nullptr; }
inline HMENU  GetSystemMenu(HWND, BOOL)      { return 0; }
inline BOOL   AppendMenu(HMENU, UINT, UINT_PTR, LPCTSTR) { return FALSE; }
inline BOOL   DeleteMenu(HMENU, UINT, UINT)  { return FALSE; }
inline int    GetMenuItemCount(HMENU)        { return 0; }
inline BOOL   DrawMenuBar(HWND)              { return FALSE; }
inline int    GetSystemMetrics(int idx) {
    if (g_wxhook_get_system_metric) return g_wxhook_get_system_metric(idx);
    return 0;
}
#ifndef SystemParametersInfo
inline BOOL   SystemParametersInfo(UINT, UINT, PVOID, UINT) { return FALSE; }
#endif
// Real monotonic millisecond clock + sleep (POSIX).  Previously no-ops, which
// silently broke timing code and the per-line send delay (Sleep(m_iLineDelay)
// in doc.cpp).  GetTickCount / timeGetTime wrap at 2^32 ms like Win32.
inline DWORD  GetTickCount() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((unsigned long long)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}
inline DWORD  timeGetTime()                  { return GetTickCount(); }
inline void   Sleep(DWORD ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    ::nanosleep(&ts, nullptr);
}
// Single-quote a string for /bin/sh, escaping embedded quotes.
inline std::string ShellQuoteArg(const char* s)
{
    std::string q;
    q += '\'';
    for (const char* p = s; p && *p; ++p) {
        if (*p == '\'') q += "'\\''";
        else            q += *p;
    }
    q += '\'';
    return q;
}

// Build the /bin/sh command line for a ShellExecute request.
//
// Split out of ShellExecute so it can be unit-tested: ShellExecute itself calls
// system() and is side-effectful by contract, so it was on test_mfc_ui.cpp's
// skip list and the argument handling below was never covered.
//
// Returns an empty string when the request cannot be honoured — an empty file,
// or a verb with no macOS/Linux equivalent.  The caller turns that into
// SE_ERR_NOASSOC, which is what Windows returns when the verb is not registered
// for the file type, and which utils.shellexecute already renders as "There is
// no application associated with the given file name extension"
// (scripting/lua_utils.cpp).  Failing loudly beats silently doing "open"
// instead of what the script asked for.
//
// `op` and `dir` used to be accepted and thrown away here, which made
// utils.shellexecute(file, params, defdir, operation) — a documented scripting
// API whose own example is a "print" verb — quietly ignore two of its four
// arguments.
inline std::string BuildShellExecuteCommand(LPCTSTR op, LPCTSTR file,
                                            LPCTSTR params, LPCTSTR dir)
{
    if (!file || !file[0])
        return std::string();

    std::string cmd;

    // Windows' lpDirectory: run in this working directory.
    if (dir && dir[0]) {
        cmd  = "cd ";
        cmd += ShellQuoteArg(dir);
        cmd += " && ";
    }

    if (params && params[0]) {
        // Editor-launch form: ShellExecute(hwnd, "open", editor, "file.lua", …)
        // — `file` is the executable, so run it directly with its arguments.
        // The verb is not meaningful in this form (every in-tree caller passes
        // "open"); doc.cpp's script-editor launch is the reason it exists.
        cmd += ShellQuoteArg(file);
        cmd += ' ';
        cmd += params;          // already shell-quoted by EditFileWithEditor
        cmd += " &";
        return cmd;
    }

    // No parameters: `file` is a document or URL, so dispatch on the verb.
    // Windows verbs are case-insensitive.
    std::string verb = (op && op[0]) ? op : "open";
    for (char& c : verb)
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');

    std::string opener;
#if defined(__APPLE__)
    if      (verb == "open")    opener = "open";
    else if (verb == "edit")    opener = "open -t";   // default text editor
    else if (verb == "explore") opener = "open -R";   // reveal in Finder
    else if (verb == "print")   opener = "lpr";
#else
    if      (verb == "open")    opener = "xdg-open";
    else if (verb == "edit")    opener = "xdg-open";  // no distinct edit verb
    else if (verb == "explore") opener = "xdg-open";
    else if (verb == "print")   opener = "lpr";
#endif

    if (opener.empty())
        return std::string();          // unmapped verb — do NOT silently open

    cmd += opener;
    cmd += ' ';
    cmd += ShellQuoteArg(file);
    cmd += " &";   // don't block the MUD connection
    return cmd;
}

inline HINSTANCE ShellExecute(HWND, LPCTSTR op, LPCTSTR file, LPCTSTR params,
                              LPCTSTR dir, int /*show*/)
{
    const std::string cmd = BuildShellExecuteCommand(op, file, params, dir);
    if (cmd.empty())
        return (HINSTANCE)(intptr_t)(file && file[0] ? SE_ERR_NOASSOC : 0);
    return (::system(cmd.c_str()) == 0) ? (HINSTANCE)33 : nullptr;
}
// MessageBox already defined earlier; avoid redefinition.

// System metrics keys (subset)
#ifndef SM_CXSCREEN
#  define SM_CXSCREEN   0
#  define SM_CYSCREEN   1
#  define SM_CXFULLSCREEN 16
#  define SM_CYFULLSCREEN 17
#  define SM_CYCAPTION  4
#  define SM_CXBORDER   5
#  define SM_CYBORDER   6
#  define SM_CXHSCROLL  21
#  define SM_CYVSCROLL  20
#  define SM_CXVSCROLL  2
#  define SM_CYHSCROLL  3
#endif

// ── Miscellaneous MFC helpers ─────────────────────────────────────────────
#ifndef DECLARE_INTERFACE_MAP
#  define DECLARE_INTERFACE_MAP()
#endif

// IMPLEMENT_OLECREATE / IMPLEMENT_OLECTLTYPE stubs
#define IMPLEMENT_OLECREATE(cls, name, l, w1, w2, b0,b1,b2,b3,b4,b5,b6,b7)
#define IMPLEMENT_OLECREATE_EX(cls, name, l, w1, w2, b0,b1,b2,b3,b4,b5,b6,b7)
#define IMPLEMENT_OLETYPELIB(cls, tlid, major, minor)
#define BEGIN_OLEFACTORY(cls)
#define END_OLEFACTORY(cls)

// CImageList stub (used by tree/list controls)
class CImageList {
public:
    BOOL Create(int,int,UINT,int,int) { return FALSE; }
    BOOL Create(UINT,int,int,COLORREF) { return FALSE; }
    BOOL DeleteImageList() { return FALSE; }
    int  Add(HICON) { return -1; }
    int  Add(CBitmap*, COLORREF) { return -1; }
    int  Add(CBitmap*, CBitmap*) { return -1; }
    BOOL Replace(int, HICON) { return FALSE; }
    BOOL Remove(int) { return FALSE; }
    int  GetImageCount() const { return 0; }
    BOOL SetImageCount(UINT) { return FALSE; }
    BOOL Draw(CDC*, int, CPoint, UINT) const { return FALSE; }
    HIMAGELIST GetSafeHandle() const { return nullptr; }
    HIMAGELIST Detach() { return nullptr; }
    BOOL Attach(HIMAGELIST) { return FALSE; }
    static CImageList* FromHandle(HIMAGELIST) { return nullptr; }
};

// CMenu stub
// Popup-menu hook: implemented in the wx layer (wx_mfc_impl.cpp); null in the
// headless library, where TrackPopupMenu then simply returns 0 (no menu).
extern int (*g_wxhook_popup_menu)(CMenu* menu, int x, int y);

// The real MFC CMenu is a thin wrapper over a Windows HMENU.  Off-Windows we
// record the menu as a flat list of items (a nested submenu is referenced by
// its child CMenu's handle) and render it with a wxWidgets popup through
// g_wxhook_popup_menu.  Menus are built fresh per right-click via GrabAMenu and
// live in a pointer-stable arena that CMiniWindow::Menu() resets at the start of
// each invocation — safe because popup menus are modal, so the previous one has
// already closed.  Item flags (MF_*) are interpreted by the wx layer, so this
// class deliberately stores them verbatim and references no MF_ macros (those
// are #defined further down this header).
class CMenu {
public:
    HMENU m_hMenu = 0;

    struct Item {
        UINT        flags = 0;   // MF_SEPARATOR / MF_STRING / MF_POPUP / MF_CHECKED / MF_GRAYED
        UINT_PTR    arg   = 0;   // command id (MF_STRING) or child menu handle (MF_POPUP)
        std::string text;
    };
    std::vector<Item> m_items;
    // Submenus materialised by GetSubMenu(), parallel to their MF_POPUP entries
    // in m_items.  Arena-owned (stable addresses), never freed individually.
    std::vector<CMenu*> m_subs;

    static std::deque<CMenu>& Arena()  { static std::deque<CMenu> a; return a; }
    static void   ResetArena()         { Arena().clear(); }
    static CMenu* NewPopup() {
        Arena().emplace_back();
        CMenu* p = &Arena().back();
        p->m_hMenu = (HMENU)(intptr_t) Arena().size();   // 1-based handle into the arena
        return p;
    }
    static CMenu* FromMenuHandle(HMENU h) {
        size_t i = (size_t)(intptr_t) h;
        std::deque<CMenu>& a = Arena();
        return (i >= 1 && i <= a.size()) ? &a[i - 1] : nullptr;
    }

    BOOL  CreatePopupMenu()    { m_items.clear(); return TRUE; }
    BOOL  CreateMenu()         { m_items.clear(); return TRUE; }
    BOOL  LoadMenu(UINT)       { return FALSE; }
    BOOL  DestroyMenu()        { m_items.clear(); return TRUE; }
    BOOL  AppendMenu(UINT flags, UINT_PTR idItem = 0, LPCTSTR text = nullptr) {
        Item it; it.flags = flags; it.arg = idItem;
        if (text) it.text = text;
        m_items.push_back(it);
        return TRUE;
    }
    BOOL  AppendMenu(UINT, UINT_PTR, const CBitmap*) { return FALSE; }
    BOOL  InsertMenu(UINT, UINT, UINT_PTR=0, LPCTSTR=nullptr) { return FALSE; }
    BOOL  ModifyMenu(UINT, UINT, UINT_PTR=0, LPCTSTR=nullptr) { return FALSE; }
    BOOL  DeleteMenu(UINT, UINT) { return FALSE; }
    BOOL  CheckMenuItem(UINT, UINT) { return FALSE; }
    BOOL  EnableMenuItem(UINT, UINT) { return FALSE; }
    UINT  GetMenuItemCount() const   { return (UINT) m_items.size(); }
    UINT  GetMenuState(UINT, UINT) const { return 0; }
    int   GetMenuString(UINT, CString&, UINT) const { return 0; }

    // Lazily materialise submenu `i`, creating it (and the MF_POPUP entry that
    // links it into this menu) on first ask.
    //
    // This used to `return nullptr` unconditionally, which CRASHED the app.
    // The canonical idiom throughout the Windows source is
    //     VERIFY(menu.LoadMenu(IDR_SOMETHING));
    //     CMenu* pPopup = menu.GetSubMenu(0);
    //     ASSERT(pPopup != NULL);
    //     pPopup->GetSubMenu(0)->AppendMenu(...);
    // and the port has no menu-resource loader, so LoadMenu returns FALSE and
    // every one of those chains walked into a null CMenu*.  AppendMenu touches
    // m_items, so the null `this` faults — and in a debug build the ASSERT
    // aborts first.  Live repro before this change: Configure ▸ Triggers ▸ Add
    // ▸ the "…" beside Trigger ▸ the "->" button (dialogs/EditDlg.cpp:212-320,
    // six submenus) killed the process outright.
    //
    // Creating on demand is the right answer rather than merely not-crashing:
    // these call sites all populate the menu THEMSELVES with AppendMenu (the
    // resource only supplied a skeleton, whose dummy first item the code then
    // deletes), so once the submenus exist the menu is fully built by canonical
    // code and renders through wx_build_popup_menu.
    //
    // Safe against runaway growth: every caller in the tree indexes a fixed
    // small constant (0..5), never `while (GetSubMenu(i++))`.
    CMenu* GetSubMenu(int i) {
        if (i < 0) return nullptr;
        while ((int) m_subs.size() <= i) {
            CMenu* child = NewPopup();
            m_subs.push_back(child);
            // Link it in with an EMPTY label: the real title lived in the menu
            // resource, which the port does not have.  wx_build_popup_menu
            // inlines an unlabelled popup's items rather than showing a
            // nameless submenu — see platform/wx_mfc_impl.cpp.
            Item it;
            // MF_POPUP is #defined further down this header (after CMenu), so
            // spell the value; it is fixed Win32 API numbering.
            it.flags = 0x00000010 /* MF_POPUP */;
            it.arg   = (UINT_PTR) child->m_hMenu;
            m_items.push_back(it);
        }
        return m_subs[(size_t) i];
    }
    UINT  GetMenuItemID(int) const { return 0; }
    UINT  TrackPopupMenu(UINT /*flags*/, int x, int y, CWnd* /*owner*/, LPCRECT=nullptr) {
        return g_wxhook_popup_menu ? (UINT) g_wxhook_popup_menu(this, x, y) : 0;
    }
    HMENU GetSafeHmenu() const { return m_hMenu; }
    static CMenu* FromHandle(HMENU) { return nullptr; }
    operator HMENU() const { return m_hMenu; }
    HMENU Detach() { HMENU h = m_hMenu; m_hMenu = 0; return h; }
    BOOL  Attach(HMENU h) { m_hMenu = h; return TRUE; }
};

// Drag/drop stub
class COleDataSource {
public:
    void Empty() {}
    void CacheGlobalData(CLIPFORMAT, HGLOBAL, void* = nullptr) {}
    DROPEFFECT DoDragDrop(DWORD=0, LPCRECT=nullptr) { return 0; }
};

// DROPEFFECT constants (the types themselves are in windows_types.h)
#ifndef DROPEFFECT_NONE
#  define DROPEFFECT_NONE   0
#  define DROPEFFECT_COPY   1
#  define DROPEFFECT_MOVE   2
#  define DROPEFFECT_LINK   4
#  define DROPEFFECT_SCROLL 0x80000000L
#endif

// Thread support stubs (CWinThread)
class CWinThread : public CCmdTarget {
    SHIM_RUNTIME_CLASS(CWinThread, CCmdTarget)
public:
    HANDLE m_hThread = nullptr;
    DWORD  m_nThreadID = 0;
    BOOL   m_bAutoDelete = TRUE;
    virtual BOOL InitInstance() { return TRUE; }
    virtual int  ExitInstance() { return 0; }
    virtual BOOL OnIdle(LONG)   { return FALSE; }
    BOOL PumpMessage()          { return FALSE; }
    BOOL IsIdleMessage(MSG*)    { return FALSE; }
    void SetThreadPriority(int) {}
    BOOL PostThreadMessage(UINT, WPARAM, LPARAM) { return FALSE; }
};

// CEvent — already in compat.h

// ── Clipboard format helpers ────────────────────────────────────────────────
inline UINT RegisterClipboardFormat(LPCTSTR) { return 0; }
inline BOOL OpenClipboard(HWND) { return FALSE; }
inline BOOL CloseClipboard() { return FALSE; }
inline BOOL EmptyClipboard() { return FALSE; }
inline HANDLE GetClipboardData(UINT) { return nullptr; }
inline HANDLE SetClipboardData(UINT, HANDLE) { return nullptr; }
inline BOOL IsClipboardFormatAvailable(UINT fmt) {
    // CF_TEXT (1) / CF_UNICODETEXT (13): report whether the clipboard holds text,
    // so the "Paste To World" command-UI (doc.cpp OnUpdateEditPastetomush) enables
    // correctly.  Other formats aren't bridged.
    if ((fmt == 1 || fmt == 13) && g_wxhook_get_clipboard_text) {
        char* t = g_wxhook_get_clipboard_text();
        if (t) { free(t); return TRUE; }
    }
    return FALSE;
}
inline HGLOBAL GlobalAlloc(UINT, size_t n) { return malloc(n); }
inline HGLOBAL GlobalReAlloc(HGLOBAL p, size_t n, UINT) { return realloc(p, n); }
inline HGLOBAL GlobalFree(HGLOBAL p) { free(p); return nullptr; }
inline LPVOID  GlobalLock(HGLOBAL p) { return p; }
inline BOOL    GlobalUnlock(HGLOBAL) { return TRUE; }
inline SIZE_T  GlobalSize(HGLOBAL) { return 0; }

// GMEM flags
#ifndef GMEM_MOVEABLE
#  define GMEM_FIXED    0x0000
#  define GMEM_MOVEABLE 0x0002
#  define GMEM_ZEROINIT 0x0040
#  define GHND (GMEM_MOVEABLE|GMEM_ZEROINIT)
#  define GPTR (GMEM_FIXED|GMEM_ZEROINIT)
#endif

// DragAcceptFiles stub
inline void DragAcceptFiles(HWND, BOOL) {}

// ── COleVariant — full COM VARIANT wrapper stub ────────────────────────────
class COleVariant : public VARIANT {
public:
    COleVariant() { vt = 0; lVal = 0; }
    COleVariant(long v)          { vt = 3; lVal = v; }
    COleVariant(double v)        { vt = 5; dblVal = v; }
    COleVariant(LPCTSTR s)       { vt = 8; bstrVal = (char*)s; }
    COleVariant(const CString& s){ vt = 8; bstrVal = (char*)(LPCTSTR)s; }
    COleVariant(bool b)          { vt = 11; boolVal = b ? -1 : 0; }
    void Clear()                 { vt = 0; lVal = 0; }
    // Coerce in place between the VARIANT types MUSHclient actually uses
    // (VT_I4/R8/BOOL/BSTR).  Previously a no-op, which broke e.g. the non-Lua
    // plugin veto path: CPlugin::ExecutePluginScript packs a script's bool result
    // as VT_BOOL, then does ChangeType(VT_I4) and tests `vt == VT_I4` — with the
    // old no-op that test failed and every callback was treated as "OK'd it", so
    // a Python/Perl/JS/Tcl/Ruby plugin could never suppress a command/line.
    void ChangeType(VARTYPE newvt, const COleVariant* = nullptr) {
        if (vt == newvt) return;
        switch (newvt) {
        case 1: {  // VT_NULL — MFC/OLE: VariantChangeType to VT_NULL yields a
                   // NULL variant (DatabaseColumnValues marks SQL NULLs this way;
                   // pushVariant then answers Lua nil for the element).
            vt = 1; lVal = 0; break;
        }
        case 3: {  // VT_I4
            long v = 0;
            switch (vt) {
                case 5:  v = (long) dblVal;            break;  // VT_R8
                case 11: v = boolVal ? 1 : 0;          break;  // VT_BOOL
                case 8:  v = bstrVal ? atol(bstrVal):0; break;  // VT_BSTR
                default: break;
            }
            lVal = v; vt = 3; break;
        }
        case 5: {  // VT_R8
            double d = 0;
            switch (vt) {
                case 3:  d = (double) lVal;             break;
                case 11: d = boolVal ? 1.0 : 0.0;       break;
                case 8:  d = bstrVal ? atof(bstrVal):0; break;
                default: break;
            }
            dblVal = d; vt = 5; break;
        }
        case 11: {  // VT_BOOL
            bool b = false;
            switch (vt) {
                case 3:  b = (lVal != 0);            break;
                case 5:  b = (dblVal != 0.0);        break;
                case 8:  b = (bstrVal && *bstrVal);  break;
                default: break;
            }
            boolVal = b ? -1 : 0; vt = 11; break;
        }
        case 8: {  // VT_BSTR — format numerics into a thread-local buffer.  Matches
                   // the wrapper's non-owning bstrVal model (it points at borrowed
                   // storage); the sole caller copies it into a CString immediately.
            static thread_local char s_buf[64];
            switch (vt) {
                case 3:  snprintf(s_buf, sizeof s_buf, "%ld", lVal);          break;
                case 5:  snprintf(s_buf, sizeof s_buf, "%g",  dblVal);        break;
                case 11: snprintf(s_buf, sizeof s_buf, "%d",  boolVal ? 1:0); break;
                default: s_buf[0] = '\0'; break;
            }
            bstrVal = s_buf; vt = 8; break;
        }
        default: break;  // other target types: leave as-is
        }
    }
    COleVariant& operator=(long v)          { vt = 3; lVal = v;               return *this; }
    COleVariant& operator=(double v)        { vt = 5; dblVal = v;             return *this; }
    COleVariant& operator=(LPCTSTR s)       { vt = 8; bstrVal = (char*)s;     return *this; }
    COleVariant& operator=(const CString& s){ vt = 8; bstrVal=(char*)(LPCTSTR)s; return *this; }
    COleVariant& operator=(bool b)           { vt = 11; boolVal = b ? -1 : 0; return *this; }
    COleVariant& operator=(const COleSafeArray&) { vt = 8204 /*VT_VARIANT|VT_ARRAY*/; lVal = 0; return *this; }
    COleVariant& operator=(const VARIANT& v)    { *(VARIANT*)this = v; return *this; }
    operator long()   const { return lVal; }
    operator double() const { return dblVal; }
    // Attach/Detach — take/release ownership of a VARIANT
    void Attach(VARIANT& v) { *(VARIANT*)this = v; v.vt = 0; }
    void Detach(VARIANT& v) { v = *(VARIANT*)this; vt = 0; }
    typedef COleVariant VARIANTARG;
};
typedef COleVariant VARIANTARG;

// ── Windows GDI / Rich Edit structs ──────────────────────────────────────────
// (BITMAPINFOHEADER defined earlier, guarded by BITMAPINFOHEADER_DEFINED)
struct RGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; };
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; };
typedef BITMAPINFO* LPBITMAPINFO;
typedef const BITMAPINFO* LPCBITMAPINFO;
struct BITMAPFILEHEADER {
    WORD  bfType;
    DWORD bfSize;
    WORD  bfReserved1, bfReserved2;
    DWORD bfOffBits;
};

// CHARFORMAT / CHARFORMAT2 — Rich edit character formatting
#ifndef CHARFORMAT_DEFINED
#define CHARFORMAT_DEFINED
#define CFM_BOLD       0x00000001
#define CFM_ITALIC     0x00000002
#define CFM_UNDERLINE  0x00000004
#ifndef CFM_STRIKEOUT
#define CFM_STRIKEOUT  0x00000008
#endif
#define CFM_PROTECTED  0x00000010
#define CFM_LINK       0x00000020
#define CFM_SIZE       0x80000000
#define CFM_COLOR      0x40000000
#define CFM_FACE       0x20000000
#define CFM_CHARSET    0x08000000
#define CFM_OFFSET     0x10000000
#define CFM_WEIGHT     0x04000000
#define CFE_BOLD       CFM_BOLD
#define CFE_ITALIC     CFM_ITALIC
#define CFE_UNDERLINE  CFM_UNDERLINE
#define CHARFORMAT_defined
struct CHARFORMAT {
    UINT  cbSize      = sizeof(CHARFORMAT);
    DWORD dwMask      = 0;
    DWORD dwEffects   = 0;
    LONG  yHeight     = 0;
    LONG  yOffset     = 0;
    COLORREF crTextColor = 0;
    BYTE  bCharSet    = 0;
    BYTE  bPitchAndFamily = 0;
    TCHAR szFaceName[32] = {};
};
struct CHARFORMAT2 : CHARFORMAT {
    WORD wWeight = 0;
    SHORT sSpacing = 0;
    COLORREF crBackColor = 0;
    DWORD    lcid = 0;    // LCID is typedef DWORD (defined at top of file)
    DWORD dwReserved = 0;
    SHORT sStyle = 0;
    WORD wKerning = 0;
    BYTE bUnderlineType = 0;
    BYTE bAnimation = 0;
    BYTE bRevAuthor = 0;
    BYTE bReserved1 = 0;
};
typedef CHARFORMAT*  LPCHARFORMAT;
typedef CHARFORMAT2* LPCHARFORMAT2;
// LCID is defined at the top of this file
#endif

// PARAFORMAT for rich edit
#ifndef PARAFORMAT_DEFINED
#define PARAFORMAT_DEFINED
#define PFM_ALIGNMENT 0x0008
#define PFA_LEFT      1
#define PFA_RIGHT     2
#define PFA_CENTER    3
struct PARAFORMAT {
    UINT  cbSize = sizeof(PARAFORMAT);
    DWORD dwMask = 0;
    WORD  wNumbering = 0;
    WORD  wReserved  = 0;
    LONG  dxStartIndent = 0;
    LONG  dxRightIndent = 0;
    LONG  dxOffset      = 0;
    WORD  wAlignment    = 0;
    SHORT cTabCount     = 0;
    LONG  rgxTabs[32]   = {};
};
#endif

// DOCINFO — used by CPrintDialog / printing
struct DOCINFO {
    int     cbSize       = sizeof(DOCINFO);
    LPCTSTR lpszDocName  = nullptr;
    LPCTSTR lpszOutput   = nullptr;
    LPCTSTR lpszDatatype = nullptr;
    DWORD   fwType       = 0;
};

// CControlBar already defined above (with CToolBar/CDialogBar).

// CPrintDialog — Windows print dialog stub
class CPrintDialog : public CDialog {
public:
    PRINTDLG m_pd;  // embedded by value, matching MFC's CCommonDialog::m_pd
    CPrintDialog(BOOL, DWORD = 0, CWnd* = nullptr) {}
    HDC GetPrinterDC() const  { return nullptr; }
    HDC GetHDC()       const  { return nullptr; }
    int GetCopies()    const  { return 1; }
    BOOL PrintAll()    const  { return TRUE; }
    BOOL PrintSelection() const { return FALSE; }
    BOOL PrintRange()     const { return FALSE; }
    int  GetFromPage()    const { return 1; }
    int  GetToPage()      const { return 1; }
    void SetFromPage(int) {}
    void SetToPage(int)   {}
    BOOL GetDefaults()    { return FALSE; }
    CString GetDeviceName() const { return {}; }
    CString GetPortName()   const { return {}; }
};

// ── MUSHCLIENT_FONT_* for non-Windows ─────────────────────────────────────
#ifndef MUSHCLIENT_FONT_CHARSET
#  define MUSHCLIENT_FONT_CHARSET 0   // DEFAULT_CHARSET
#  define MUSHCLIENT_FONT_FAMILY  0   // FF_DONTCARE|DEFAULT_PITCH
#endif

// ── Windows math and system utilities ─────────────────────────────────────
inline int MulDiv(int n, int m, int d) {
    // Win32 MulDiv rounds to nearest (ties away from zero) and returns -1 on
    // divide-by-zero — not truncation. Font-height math (-MulDiv(pts,dpi,72))
    // relies on the rounding, so truncating yielded fonts 1px short of Windows.
    // Computed on magnitudes with the sign reapplied: the previous
    // half-adjustment picked the wrong direction for negative divisors
    // (MulDiv(3, 3, -2) gave -4, not the documented -5).
    if (d == 0) return -1;
    long long num = (long long)n * m;
    long long an  = (num >= 0) ? num : -num;
    long long ad  = (d > 0) ? d : -(long long)d;
    long long q   = (an + ad / 2) / ad;
    return (int)(((num < 0) != (d < 0)) ? -q : q);
}
// High-resolution performance counter using POSIX clock_gettime(CLOCK_MONOTONIC).
// Frequency is 1 GHz (nanoseconds).  Both functions always succeed.
#include <time.h>
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* li) {
    if (li) { li->QuadPart = 1000000000LL; }  // 1 ns resolution
    return TRUE;
}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* li) {
    if (li) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        li->QuadPart = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    }
    return TRUE;
}

// ── WM_SETFONT / more window messages ─────────────────────────────────────
#ifndef WM_SETFONT
#  define WM_SETFONT      0x0030
#  define WM_GETFONT      0x0031
#  define WM_SETTEXT      0x000C
#  define WM_GETTEXT      0x000D
#  define WM_GETTEXTLENGTH 0x000E
#  define WM_ENABLE       0x000A
#  define WM_SETREDRAW    0x000B
#  define WM_NCACTIVATE   0x0086
#  define WM_NCDESTROY    0x0082
#  define WM_NCCREATE     0x0081
#  define WM_MDIACTIVATE  0x0222
#  define WM_MDIDESTROY   0x0221
#  define WM_MDICREATE    0x0220
#  define EM_GETSEL       0x00B0
#  define EM_SETSEL       0x00B1
#  define EM_REPLACESEL   0x00C2
#  define EM_SETREADONLY  0x00CF
#  define EM_LINECOUNT    0x00BA
#  define EM_LINEINDEX    0x00BB
#  define EM_LINELENGTH   0x00C1
#  define EM_GETLINE      0x00C4
#  define EM_LIMITTEXT    0x00C5
#  define CB_ADDSTRING    0x0143
#  define CB_DELETESTRING 0x0144
#  define CB_RESETCONTENT 0x014B
#  define CB_GETCOUNT     0x0146
#  define CB_GETCURSEL    0x0147
#  define CB_SETCURSEL    0x014E
#  define LB_ADDSTRING    0x0180
#  define LB_DELETESTRING 0x0182
#  define LB_RESETCONTENT 0x0184
#  define LB_GETCOUNT     0x018B
#  define LB_GETCURSEL    0x0188
#  define LB_SETCURSEL    0x0186
#  define BM_GETCHECK     0x00F0
#  define BM_SETCHECK     0x00F1
#  define MF_BYCOMMAND    0x00000000
#  define MF_BYPOSITION   0x00000400
#  define MF_CHECKED      0x00000008
#  define MF_UNCHECKED    0x00000000
#  define MF_ENABLED      0x00000000
#  define MF_GRAYED       0x00000001
#  define MF_DISABLED     0x00000002
#  define MF_SEPARATOR    0x00000800
#  define MF_STRING       0x00000000
#  define MF_POPUP        0x00000010
#  define MF_MENUBREAK    0x00000040
#endif

// ── Open File Dialog flags ─────────────────────────────────────────────────
#ifndef OFN_HIDEREADONLY
#  define OFN_HIDEREADONLY      0x00000004
#  define OFN_NOCHANGEDIR       0x00000008
#  define OFN_OVERWRITEPROMPT   0x00000002
#  define OFN_FILEMUSTEXIST     0x00001000
#  define OFN_PATHMUSTEXIST     0x00000800
#  define OFN_ALLOWMULTISELECT  0x00000200
#  define OFN_ENABLESIZING      0x00800000
#  define OFN_LONGNAMES         0x00200000
#  define OFN_EXPLORER          0x00080000
#  define OFN_CREATEPROMPT      0x00002000
#  define OFN_NOREADONLYRETURN  0x00008000
#  define OFN_NOTESTFILECREATE  0x00010000
#endif

// VER_PLATFORM_* (GetVersionEx flags)
#ifndef VER_PLATFORM_WIN32s
#  define VER_PLATFORM_WIN32s      0
#  define VER_PLATFORM_WIN32_WINDOWS 1
#  define VER_PLATFORM_WIN32_NT    2
#endif

// Winsock async / socket-ioctl constants
#ifndef IOC_VENDOR
#  define IOC_VENDOR    0x18000000
#  define _WSAIOW(x,y)  (0x80000000|(x)|(y))
#  define MAXGETHOSTSTRUCT 1024
#  define WSAGETASYNCERROR(x) (HIWORD(x))
#endif
typedef struct in_addr* LPIN_ADDR;

// WSACancelAsyncRequest stub
inline int WSACancelAsyncRequest(HANDLE) { return 0; }
inline int WSAIoctl(unsigned int, DWORD, void*, DWORD, void*, DWORD, DWORD*, void*, void*) { return -1; }

// SSL/X509 stubs come from <openssl/ssl.h> and <openssl/x509.h> (included via
// mfc_ui.h's forward-decl section above).  Nothing extra needed here.

// CertStore stubs (wincrypt)
typedef void* HCERTSTORE;
typedef void* PCCERT_CONTEXT;
inline HCERTSTORE    CertOpenSystemStore(void*, LPCTSTR) { return nullptr; }
inline PCCERT_CONTEXT CertEnumCertificatesInStore(HCERTSTORE, PCCERT_CONTEXT) { return nullptr; }
inline BOOL          CertCloseStore(HCERTSTORE, DWORD) { return FALSE; }
inline BOOL          CertFreeCertificateContext(PCCERT_CONTEXT) { return FALSE; }
typedef struct { DWORD cbData; BYTE* pbData; } CRYPT_OBJID_BLOB;
typedef struct { DWORD cRDNAttr; void* rgRDNAttr; } CERT_RDN;
typedef struct { DWORD cRDN; CERT_RDN* rgRDN; } CERT_NAME_INFO;

// GetClipboardContents / putontoclipboard — declared here so all TUs see
// the prototype; the real (non-Windows) implementation lives in doc.cpp.
// (the real declarations are in stdafx.h #ifdef _WIN32 only)
BOOL putontoclipboard(const CString&, bool = false);
BOOL GetClipboardContents(CString&, bool, bool = true);

// Forward-declare the notepad hook so the inline below compiles without
// pulling in wx_mfc_hooks.h (which would create a circular include chain).
class CMUSHclientDoc;
extern void* (*g_wxhook_create_text_window)(
    const char*, const char*, CMUSHclientDoc*, long long,
    const char*, long, long, unsigned int,
    unsigned long, unsigned long,
    const char*, const char*,
    bool, bool, bool, bool, bool, int, int);

// CreateTextWindow — dispatches to g_wxhook_create_text_window on non-Windows.
inline bool CreateTextWindow(const char* sText, const char* sTitle,
    CMUSHclientDoc* pDoc, long long iUniqueDocNum,
    const CString strFontName, long iFontSize, long iFontWeight,
    DWORD iFontCharset, COLORREF textColour, COLORREF backColour,
    const CString strSearchString, const CString strRecallLinePreamble,
    bool bMatchCase, bool bRegexp, bool bCommands, bool bOutput,
    bool bNotes, int iLines, int iNotepadType)
{
    if (!g_wxhook_create_text_window) return false;
    return g_wxhook_create_text_window(
        sText, sTitle, pDoc, iUniqueDocNum,
        static_cast<const char*>(strFontName), iFontSize, iFontWeight,
        static_cast<unsigned int>(iFontCharset),
        static_cast<unsigned long>(textColour),
        static_cast<unsigned long>(backColour),
        static_cast<const char*>(strSearchString),
        static_cast<const char*>(strRecallLinePreamble),
        bMatchCase, bRegexp, bCommands, bOutput,
        bNotes, iLines, iNotepadType) != nullptr;
}

// LoadLibrary / GetProcAddress stubs
inline HMODULE LoadLibrary(LPCTSTR)               { return nullptr; }
inline HMODULE LoadLibraryEx(LPCTSTR,HANDLE,DWORD){ return nullptr; }
inline void*   GetProcAddress(HMODULE,LPCTSTR)    { return nullptr; }
inline BOOL    FreeLibrary(HMODULE)               { return FALSE; }

// OPENFILENAME defined earlier (before CDialog/CFileDialog).

// AFX_IDD_PRINTDLG (used by CPrintDialog base class)
#ifndef AFX_IDD_PRINTDLG
#  define AFX_IDD_PRINTDLG         0xF200
#  define AFX_IDC_PRINT_DOCNAME    0xF201
#  define AFX_IDC_PRINT_PRINTERNAME 0xF202
#  define AFX_IDC_PRINT_PORTNAME   0xF203
#  define AFX_IDC_PRINT_PAGENUM    0xF204
#  define AFX_IDS_PRINTONPORT      0xF205
#  define AFX_IDS_PRINTPAGENUM     0xF206
#endif

// WCHAR already defined via windows_types.h as wchar_t; no extra needed.

// GetClipboardData / SetClipboardData already stubbed above.

// WSADATA - already defined but may be doubly defined; ensure guard:
// (defined earlier in this file)

// ── CList<TYPE, ARG_TYPE> — generic MFC doubly-linked list template ────────
// Used in ChatListDlg and a few other places.
template<typename TYPE, typename ARG_TYPE = const TYPE&>
class CList {
    std::list<TYPE> m_list;
public:
    bool     IsEmpty()  const { return m_list.empty(); }
    int      GetCount() const { return (int)m_list.size(); }
    void     RemoveAll()      { m_list.clear(); }
    POSITION AddTail(ARG_TYPE v) {
        m_list.push_back(v);
        return (POSITION)(uintptr_t)(m_list.size());
    }
    POSITION AddHead(ARG_TYPE v) {
        m_list.push_front(v);
        return (POSITION)(uintptr_t)1;
    }
    TYPE  RemoveTail() {
        if (m_list.empty()) return TYPE{};
        TYPE v = m_list.back(); m_list.pop_back(); return v;
    }
    TYPE  RemoveHead() {
        if (m_list.empty()) return TYPE{};
        TYPE v = m_list.front(); m_list.pop_front(); return v;
    }
    POSITION GetHeadPosition() const {
        return m_list.empty() ? nullptr : (POSITION)(uintptr_t)1;
    }
    POSITION GetTailPosition() const { return GetHeadPosition(); }
    // POSITION encodes a 1-based element index.  (The previous iterator-cached
    // implementation reset to begin() on every call — pos was always 1 — so
    // iterating any list with 2+ elements spun forever on the first element.)
    TYPE& GetNext(POSITION& pos) const {
        static TYPE dummy{};
        size_t idx = (size_t)(uintptr_t)pos;   // 1-based
        if (idx == 0 || idx > m_list.size()) { pos = nullptr; return dummy; }
        auto it = m_list.begin();
        std::advance(it, idx - 1);
        TYPE& r = const_cast<TYPE&>(*it);
        pos = (idx == m_list.size()) ? nullptr : (POSITION)(uintptr_t)(idx + 1);
        return r;
    }
    TYPE& GetPrev(POSITION& pos) const { return GetNext(pos); }
    TYPE& GetHead() const { static TYPE d{}; return m_list.empty() ? d : const_cast<TYPE&>(m_list.front()); }
    TYPE& GetTail() const { static TYPE d{}; return m_list.empty() ? d : const_cast<TYPE&>(m_list.back()); }
    TYPE& GetAt(POSITION) const { static TYPE d{}; return d; }
    void RemoveAt(POSITION) {}
    POSITION Find(ARG_TYPE, POSITION = nullptr) const { return nullptr; }
    POSITION FindIndex(int n) const {
        if (n < 0 || n >= (int)m_list.size()) return nullptr;
        return (POSITION)(uintptr_t)(n+1);
    }
    POSITION InsertBefore(POSITION, ARG_TYPE v) { return AddHead(v); }
    POSITION InsertAfter(POSITION, ARG_TYPE v)  { return AddTail(v); }
};

// ── CMemFile additional constructors ──────────────────────────────────────
// CMemFile(BYTE* buf, UINT size) — attaches to an existing buffer (no-op stub)
// Reopen the class to add the constructor — we can't reopen, so we use a trick:
// The CMemFile definition already exists above; add a note here.
// Actually we forward the fix to the CMemFile definition above.

// ── COM / OLE stubs ────────────────────────────────────────────────────────
// These are Windows-only COM types referenced in doc.cpp.
// They compile but do nothing on non-Windows.

struct DISPPARAMS {
    VARIANT* rgvarg       = nullptr;
    int*     rgdispidNamedArgs = nullptr;
    UINT     cArgs        = 0;
    UINT     cNamedArgs   = 0;
};

struct EXCEPINFO {
    WORD  wCode = 0;
    WORD  wReserved = 0;
    char* bstrSource = nullptr;
    char* bstrDescription = nullptr;
    char* bstrHelpFile = nullptr;
    DWORD dwHelpContext = 0;
    HRESULT scode = S_OK;
};

// COleDateTime — thin wrapper around CTime for datetime values
class COleDateTime {
public:
    double m_dt = 0.0;
    enum DateTimeStatus { valid=0, invalid=1, null=2 };
    DateTimeStatus m_status = valid;
    COleDateTime() = default;
    COleDateTime(double d) : m_dt(d) {}
    operator double() const { return m_dt; }
    COleDateTime(int y, int m, int d, int h, int min, int s) {}
    static COleDateTime GetCurrentTime() { return {}; }
    bool   SetDateTime(int,int,int,int,int,int) { return false; }
    bool   SetDate(int,int,int) { return false; }
    int    GetYear()   const { return 0; }
    int    GetMonth()  const { return 0; }
    int    GetDay()    const { return 0; }
    int    GetHour()   const { return 0; }
    int    GetMinute() const { return 0; }
    int    GetSecond() const { return 0; }
    CString Format(LPCTSTR) const { return {}; }
    CString Format(UINT)    const { return {}; }
    // Windows: locale-default date+time.  Port rendering decision (locked by
    // the old portable GetLineInfo "timestr"): "%m/%d/%Y %I:%M:%S %p" of the
    // same instant in local time.  Port convention: m_dt stores raw Unix
    // seconds (CTime::GetTime()), not an OLE DATE.
    CString Format(DWORD /*dwFlags*/, LCID /*lcid*/) const {
        time_t t = (time_t) m_dt;
        struct tm tmv = {};
        char buf[40] = "";
        if (localtime_r(&t, &tmv))
            strftime(buf, sizeof buf, "%m/%d/%Y %I:%M:%S %p", &tmv);
        return CString(buf);
    }
    // Fill a local-time SYSTEMTIME from the stored Unix seconds so
    // CTime(SYSTEMTIME) (local-time mktime) round-trips the instant —
    // pushVariant's VT_DATE arm (lua_methods.cpp) depends on this.
    void GetAsSystemTime(SYSTEMTIME& st) const {
        memset(&st, 0, sizeof(st));
        time_t t = (time_t) m_dt;
        struct tm tmv = {};
        if (localtime_r(&t, &tmv)) {
            st.wYear      = (WORD) (tmv.tm_year + 1900);
            st.wMonth     = (WORD) (tmv.tm_mon + 1);
            st.wDayOfWeek = (WORD) tmv.tm_wday;
            st.wDay       = (WORD) tmv.tm_mday;
            st.wHour      = (WORD) tmv.tm_hour;
            st.wMinute    = (WORD) tmv.tm_min;
            st.wSecond    = (WORD) tmv.tm_sec;
        }
    }
    bool   operator==(const COleDateTime& o) const { return m_dt==o.m_dt; }
    bool   operator!=(const COleDateTime& o) const { return m_dt!=o.m_dt; }
    bool   operator< (const COleDateTime& o) const { return m_dt< o.m_dt; }
    bool   operator<=(const COleDateTime& o) const { return m_dt<=o.m_dt; }
    bool   operator> (const COleDateTime& o) const { return m_dt> o.m_dt; }
    bool   operator>=(const COleDateTime& o) const { return m_dt>=o.m_dt; }
    DateTimeStatus GetStatus() const { return m_status; }
    bool IsValid() const { return m_status == valid; }
    bool IsNull()  const { return m_status == null; }
};

class COleDateTimeSpan {
public:
    double m_dblSpan = 0.0;
    COleDateTimeSpan() = default;
    COleDateTimeSpan(double d) : m_dblSpan(d) {}
    COleDateTimeSpan(int days, int hrs, int mins, int secs)
        : m_dblSpan(days + hrs/24.0 + mins/1440.0 + secs/86400.0) {}
    double GetTotalDays()    const { return m_dblSpan; }
    double GetTotalHours()   const { return m_dblSpan * 24; }
    double GetTotalMinutes() const { return m_dblSpan * 1440; }
    double GetTotalSeconds() const { return m_dblSpan * 86400; }
};

// COleSafeArray — Windows-specific safe array; no-op stub
// Real one-dimensional safe array of VARIANTs, backed by the SAFEARRAY/VARIANT
// machinery above.  Used by CMUSHclientDoc::GetPluginList/GetVariableList/etc.
// to return arrays to Lua.  Owns its SAFEARRAY until Detach()/Attach() transfer
// ownership into a VARIANT (which VariantClear then frees).
class COleSafeArray {
public:
    SAFEARRAY* m_psa = nullptr;

    COleSafeArray() = default;
    ~COleSafeArray() { if (m_psa) SafeArrayDestroy(m_psa); }
    COleSafeArray(const COleSafeArray&)            = delete;
    COleSafeArray& operator=(const COleSafeArray&) = delete;

    void CreateOneDim(UINT /*vt*/, UINT count, const void* = nullptr, LONG lbound = 0) {
        if (m_psa) { SafeArrayDestroy(m_psa); m_psa = nullptr; }
        m_psa = new SAFEARRAY();
        m_psa->cDims = 1; m_psa->fFeatures = 0; m_psa->cLocks = 0;
        m_psa->cbElements = sizeof(VARIANT);
        m_psa->rgsabound[0].cElements = count;
        m_psa->rgsabound[0].lLbound   = lbound;
        m_psa->pvData = new VARIANT[count ? count : 1]();   // value-init: vt = 0
    }
    void PutElement(long* index, void* pv) {
        if (!m_psa || !m_psa->pvData || !index || !pv) return;
        long i = index[0] - m_psa->rgsabound[0].lLbound;
        if (i < 0 || (unsigned long) i >= m_psa->rgsabound[0].cElements) return;
        VARIANT* dst = &((VARIANT*) m_psa->pvData)[i];
        VARIANT* src = (VARIANT*) pv;
        if (dst->vt == VT_BSTR && dst->bstrVal) free(dst->bstrVal);
        *dst = *src;
        if (src->vt == VT_BSTR && src->bstrVal) dst->bstrVal = strdup(src->bstrVal); // own it
    }
    void GetElement(long* index, void* pv) { SafeArrayGetElement(m_psa, index, pv); }
    long GetOneDimSize() const { return m_psa ? (long) m_psa->rgsabound[0].cElements : 0; }
    void AccessData(void** pp) { if (pp) *pp = m_psa ? m_psa->pvData : nullptr; }
    void UnaccessData() {}
    // Detach: hand the array to a VARIANT and give up ownership.
    // MFC contract: COleSafeArray IS a VARIANT that starts VT_EMPTY; only
    // Create/CreateOneDim stamp VT_ARRAY.  So a never-created array detaches
    // as VT_EMPTY (→ nil in Lua), NOT as VT_ARRAY with a null parray.
    COleVariant Detach() {
        COleVariant v;
        if (m_psa) {
            v.vt = (unsigned short)(VT_ARRAY | VT_VARIANT);
            v.parray = m_psa; m_psa = nullptr;
        }
        return v;   // default-constructed: VT_EMPTY
    }
    void Detach(VARIANT& out) {
        if (m_psa) {
            out.vt = (unsigned short)(VT_ARRAY | VT_VARIANT);
            out.parray = m_psa; m_psa = nullptr;
        } else {
            out.vt = VT_EMPTY; out.parray = nullptr;
        }
    }
    void Attach(VARIANT& v) {
        if (m_psa) SafeArrayDestroy(m_psa);
        m_psa = v.parray; v.parray = nullptr; v.vt = 0;
    }
    void Clear() { if (m_psa) { SafeArrayDestroy(m_psa); m_psa = nullptr; } }
    // Implicit conversion: shares the array WITHOUT detaching, so use only when
    // the COleSafeArray outlives the result (GetXxxList uses Detach() instead).
    // Same MFC contract as Detach: never-created converts as VT_EMPTY.
    operator COleVariant() const {
        COleVariant v;
        if (m_psa) {
            v.vt = (unsigned short)(VT_ARRAY | VT_VARIANT); v.parray = m_psa;
        }
        return v;
    }
};
// (The former non-full inert COleSafeArray is gone: mushcore uses the real one
// too, so the adopted Get*List/Array* methods return real VT_ARRAY variants
// headless.)

// ── Clipboard format IDs ────────────────────────────────────────────────────
#ifndef CF_TEXT
#  define CF_TEXT        1
#  define CF_BITMAP      2
#  define CF_METAFILEPICT 3
#  define CF_SYLK        4
#  define CF_DIF         5
#  define CF_TIFF        6
#  define CF_OEMTEXT     7
#  define CF_DIB         8
#  define CF_PALETTE     9
#  define CF_PENDATA     10
#  define CF_RIFF        11
#  define CF_WAVE        12
#  define CF_UNICODETEXT 13
#  define CF_ENHMETAFILE 14
#  define CF_HDROP       15
#  define CF_LOCALE      16
#  define CF_DIBV5       17
#  define CF_OWNERDISPLAY 0x0080
#endif

// ── Code page constants ────────────────────────────────────────────────────
#ifndef CP_ACP
#  define CP_ACP       0     // ANSI code page
#  define CP_OEMCP     1     // OEM code page
#  define CP_UTF7      65000
#  define CP_UTF8      65001
#endif

// ── RasterOp / bitblt extra flags ─────────────────────────────────────────
#ifndef FNOINVERT
#  define FNOINVERT     0x0002
#endif

// ── AFX MFC string resource IDs ────────────────────────────────────────────
// On non-Windows these are never loaded; define as 0.
#ifndef AFX_IDS_UNTITLED
#  define AFX_IDS_UNTITLED    0xF000
#  define AFX_IDS_SAVEFILE    0xF001
#  define AFX_IDS_SAVEFILECOPY 0xF002
#  define AFX_IDS_UNNAMED_FILE 0xF003
#  define AFX_IDS_HIDE        0xF004
#  define AFX_IDP_ASK_SAVE    0xF005
#  define AFX_IDP_INVALID_FILENAME 0xF006
#  define AFX_IDP_FAILED_TO_OPEN_DOC 0xF007
#  define AFX_IDP_FAILED_TO_SAVE_DOC 0xF008
#  define AFX_IDP_TITLE_UNTITLED 0xF009
#  define AFX_IDS_IDLEMESSAGE 0xF00A
#  define AFX_IDS_SCSIZE      0xF00B
#  define AFX_IDS_SCMOVE      0xF00C
#  define AFX_IDS_SCMINIMIZE  0xF00D
#  define AFX_IDS_SCMAXIMIZE  0xF00E
#  define AFX_IDS_SCNEXTWINDOW 0xF00F
#  define AFX_IDS_SCPREVWINDOW 0xF010
#  define AFX_IDS_SCCLOSE     0xF011
#  define AFX_IDS_SCRESTORE   0xF012
#  define AFX_IDS_SCTASKLIST  0xF013
#endif

// ── FormatMessage / language constants (used by FormatMessage below) ───────
#ifndef FORMAT_MESSAGE_FROM_SYSTEM
#  define FORMAT_MESSAGE_FROM_SYSTEM       0x00001000
#  define FORMAT_MESSAGE_ALLOCATE_BUFFER   0x00000100
#  define FORMAT_MESSAGE_IGNORE_INSERTS    0x00000200
#  define FORMAT_MESSAGE_FROM_STRING       0x00000400
#  define FORMAT_MESSAGE_FROM_HMODULE      0x00000800
#  define LANG_NEUTRAL    0x00
#  define SUBLANG_DEFAULT 0x01
#  define SUBLANG_SYS_DEFAULT 0x02
#  define MAKELANGID(p,s) ((((WORD)(s))<<10)|(WORD)(p))
#endif

// ── Additional Windows API stubs ───────────────────────────────────────────
inline BOOL MessageBeep(UINT)          { if (g_fn_system_beep) g_fn_system_beep(); return TRUE; }
inline BOOL Beep(DWORD, DWORD)         { if (g_fn_system_beep) g_fn_system_beep(); return TRUE; }
// Real CWD accessors over POSIX getcwd/chdir.  Used by CMUSHclientDoc::
// OnFileSave / OnFileSaveAs, which save the CWD, chdir to the world-files dir
// for the save, then restore it.  (GetTempPath / GetTempFileName had no callers
// and were removed — see the file/handle note further down.)
inline DWORD GetCurrentDirectory(DWORD nBufferLength, LPTSTR lpBuffer) {
    char* cwd = ::getcwd(nullptr, 0);   // BSD/glibc ext: allocates a fitting buffer
    if (!cwd) return 0;
    const size_t len = ::strlen(cwd);
    DWORD ret;
    if (!lpBuffer || nBufferLength < (DWORD)len + 1)
        ret = (DWORD)len + 1;           // Win32: required size, including the NUL
    else { ::memcpy(lpBuffer, cwd, len + 1); ret = (DWORD)len; }  // written, sans NUL
    ::free(cwd);
    return ret;
}
inline BOOL SetCurrentDirectory(LPCTSTR lpPathName) {
    return (lpPathName && ::chdir(lpPathName) == 0) ? TRUE : FALSE;
}
inline DWORD GetModuleFileName(void* /*hModule*/, LPTSTR buf, DWORD n) {
    if (!buf || !n) return 0;
    buf[0] = '\0';
#if defined(__APPLE__)
    // macOS: _NSGetExecutablePath (from <mach-o/dyld.h>, included above)
    // Returns the raw path (may contain symlinks); resolve to canonical form.
    {
        uint32_t size = (uint32_t)n;
        if (_NSGetExecutablePath(buf, &size) == 0) {
            char resolved[4096] = {};
            if (::realpath(buf, resolved)) {
                ::strncpy(buf, resolved, (size_t)(n - 1));
                buf[n - 1] = '\0';
            }
            return (DWORD)::strlen(buf);
        }
    }
#elif defined(__linux__)
    // Linux: read the /proc/self/exe symlink
    {
        ssize_t len = ::readlink("/proc/self/exe", buf, (size_t)(n - 1));
        if (len > 0) { buf[len] = '\0'; return (DWORD)len; }
    }
#endif
    return 0;
}
// POSIX aliases for Windows _fileno / _fstat / _fgetts / _tcstok
#include <sys/stat.h>
#include <stdio.h>
#ifndef _fileno
#  define _fileno  fileno
#endif
#ifndef _fstat
#  define _fstat(fd,buf) fstat(fd,buf)
#endif
#ifndef _fgetts
#  define _fgetts  fgets
#endif
#ifndef _tcstok
#  define _tcstok  strtok
#endif
// struct _stat — Windows uses struct _stat; POSIX uses struct stat
#ifndef _stat
#  define _stat stat
#endif
// Win32 last-error <-> POSIX errno.
inline DWORD GetLastError()        { return (DWORD)errno; }
inline void  SetLastError(DWORD e) { errno = (int)e; }

// The Win32 file/handle/dir APIs — CopyFile, DeleteFile, MoveFile,
// Get/SetFileAttributes, CreateFile/CloseHandle/ReadFile/WriteFile,
// FindFirstFile/FindNextFile/FindClose, CreateDirectory/RemoveDirectory,
// GetTempPath/GetTempFileName — had NO callers on this port (the code uses
// CFile, std::filesystem and POSIX directly), so they are deliberately absent
// rather than left as no-op stubs that silently fail.  If a real caller ever
// appears, add a *working* POSIX-backed shim here (and the build will tell you).

// FormatMessage: render a system error code to text.  Error codes on this port
// flow through GetLastError == errno, so FROM_SYSTEM maps to strerror().  Honours
// the ALLOCATE_BUFFER mode MUSHclient uses (CException::GetErrorMessage,
// CScriptEngine): *lpBuffer then receives a heap string the caller releases with
// LocalFree (== free).  Returns the length written, or 0 on failure.
inline DWORD FormatMessage(DWORD dwFlags, const void* /*lpSource*/,
                           DWORD dwMessageId, DWORD /*dwLanguageId*/,
                           LPTSTR lpBuffer, DWORD nSize, va_list* /*Arguments*/) {
    const char* msg = ::strerror((int)dwMessageId);
    if (!msg || !*msg) msg = "Unknown error";
    const size_t len = ::strlen(msg);
    if (dwFlags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        char* buf = (char*)::malloc(len + 1);            // released by caller via LocalFree
        if (!buf) return 0;
        ::memcpy(buf, msg, len + 1);
        *reinterpret_cast<char**>(lpBuffer) = buf;
        return (DWORD)len;
    }
    if (!lpBuffer || nSize == 0) return 0;
    const size_t n = (len < (size_t)nSize - 1) ? len : (size_t)nSize - 1;
    ::memcpy(lpBuffer, msg, n);
    lpBuffer[n] = '\0';
    return (DWORD)n;
}
// ── MultiByteToWideChar / WideCharToMultiByte — portable UTF-8 implementation
// Treats all code pages as UTF-8 (correct for modern POSIX systems).
// Returns number of wide characters written (or needed if lpWStr == nullptr/cchWStr == 0).
inline int MultiByteToWideChar(UINT /*CodePage*/, DWORD /*dwFlags*/,
                                LPCSTR lpStr, int cbStr, LPWSTR lpWStr, int cchWStr)
{
    if (!lpStr) return 0;
    int srcLen = (cbStr < 0) ? (int)strlen(lpStr) + 1 : cbStr; // include NUL if cbStr<0
    // Count or copy UTF-8 → wchar_t.
    int out = 0;
    for (int i = 0; i < srcLen; ) {
        uint32_t cp = 0;
        unsigned char c = (unsigned char)lpStr[i];
        int charLen = 1;
        if      (c < 0x80)                            { cp = c; charLen = 1; }
        else if ((c & 0xE0) == 0xC0 && i+1 < srcLen) { cp = ((c&0x1F)<<6)|(lpStr[i+1]&0x3F); charLen = 2; }
        else if ((c & 0xF0) == 0xE0 && i+2 < srcLen) { cp = ((c&0x0F)<<12)|((lpStr[i+1]&0x3F)<<6)|(lpStr[i+2]&0x3F); charLen = 3; }
        else if ((c & 0xF8) == 0xF0 && i+3 < srcLen) { cp = ((c&0x07)<<18)|((lpStr[i+1]&0x3F)<<12)|((lpStr[i+2]&0x3F)<<6)|(lpStr[i+3]&0x3F); charLen = 4; }
        i += charLen;
        if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
            cp -= 0x10000;
            if (cchWStr > 0 && lpWStr && out + 1 < cchWStr) {
                lpWStr[out]   = (wchar_t)(0xD800 | (cp >> 10));
                lpWStr[out+1] = (wchar_t)(0xDC00 | (cp & 0x3FF));
            }
            out += 2;
        } else {
            if (cchWStr > 0 && lpWStr && out < cchWStr) lpWStr[out] = (wchar_t)cp;
            out++;
        }
    }
    return out;
}
inline int WideCharToMultiByte(UINT /*CodePage*/, DWORD /*dwFlags*/,
                                LPCWSTR lpWStr, int cchWStr,
                                LPSTR lpStr, int cbStr, LPCSTR /*lpDef*/, BOOL* /*lpUsedDef*/)
{
    if (!lpWStr) return 0;
    int srcLen = (cchWStr < 0) ? (int)wcslen(lpWStr) + 1 : cchWStr;
    int out = 0;
    for (int i = 0; i < srcLen; ++i) {
        uint32_t cp = (uint32_t)lpWStr[i];
        // Handle surrogate pairs on 2-byte wchar_t platforms
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i+1 < srcLen) {
            uint32_t low = (uint32_t)lpWStr[i+1];
            if (low >= 0xDC00 && low <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00); i++; }
        }
        char buf[4]; int n = 0;
        if      (cp < 0x80)    { buf[n++] = (char)cp; }
        else if (cp < 0x800)   { buf[n++] = (char)(0xC0|(cp>>6)); buf[n++] = (char)(0x80|(cp&0x3F)); }
        else if (cp < 0x10000) { buf[n++] = (char)(0xE0|(cp>>12)); buf[n++] = (char)(0x80|((cp>>6)&0x3F)); buf[n++] = (char)(0x80|(cp&0x3F)); }
        else                   { buf[n++] = (char)(0xF0|(cp>>18)); buf[n++] = (char)(0x80|((cp>>12)&0x3F)); buf[n++] = (char)(0x80|((cp>>6)&0x3F)); buf[n++] = (char)(0x80|(cp&0x3F)); }
        if (cbStr > 0 && lpStr) { for (int k = 0; k < n && out < cbStr; ++k) lpStr[out++] = buf[k]; }
        else out += n;
    }
    return out;
}
inline HANDLE CreateMutex(void*,BOOL,LPCTSTR)  { return nullptr; }
inline DWORD WaitForSingleObject(HANDLE,DWORD) { return 0; }
inline BOOL  ReleaseMutex(HANDLE)              { return FALSE; }
inline BOOL  SetEvent(HANDLE)                  { return FALSE; }
inline BOOL  ResetEvent(HANDLE)                { return FALSE; }
inline HANDLE CreateEvent(void*,BOOL,BOOL,LPCTSTR) { return nullptr; }
inline HANDLE CreateThread(void*,SIZE_T,void*,void*,DWORD,LPDWORD) { return nullptr; }
inline BOOL  TerminateThread(HANDLE,DWORD)     { return FALSE; }
inline void  ExitThread(DWORD)                 {}
inline DWORD ResumeThread(HANDLE)              { return 0; }
inline DWORD SuspendThread(HANDLE)             { return 0; }

// INFINITE and WAIT_ constants
#ifndef INFINITE
#  define INFINITE        0xFFFFFFFF
#  define WAIT_OBJECT_0   0x00000000
#  define WAIT_TIMEOUT    0x00000102
#  define WAIT_FAILED     0xFFFFFFFF
#endif

// GENERIC_READ / GENERIC_WRITE / file creation flags
#ifndef GENERIC_READ
#  define GENERIC_READ    0x80000000
#  define GENERIC_WRITE   0x40000000
#  define OPEN_EXISTING   3
#  define CREATE_NEW      1
#  define CREATE_ALWAYS   2
#  define OPEN_ALWAYS     4
#  define TRUNCATE_EXISTING 5
#  define FILE_SHARE_READ   0x00000001
#  define FILE_SHARE_WRITE  0x00000002
#  define FILE_ATTRIBUTE_NORMAL 0x00000080
#  define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#  define FILE_FLAG_OVERLAPPED 0x40000000
#  define INVALID_FILE_ATTRIBUTES 0xFFFFFFFF
#endif

// hostent — defined by <netdb.h> on POSIX; guard against redefinition
#ifndef _NETDB_H
#  ifndef _NETDB_H_
struct hostent {
    char*  h_name;
    char** h_aliases;
    short  h_addrtype;
    short  h_length;
    char** h_addr_list;
};
#  ifndef h_addr
#    define h_addr h_addr_list[0]
#  endif
#  endif
#endif

// WSADATA stub (already guarded in MUSHclient.h but may be needed elsewhere)
struct WSADATA {
    WORD wVersion;
    WORD wHighVersion;
    char szDescription[257];
    char szSystemStatus[129];
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char* lpVendorInfo;
};

// ── Stubs for Windows-only declarations in stdafx.h ──────────────────────────
// These are guarded by #ifdef _WIN32 in stdafx.h, so we provide them here.

// ptrCFont / FixFont used to be stubbed out here as an inline no-op, because
// stdafx.h only declared them under #ifdef _WIN32.  That stub took its CString
// by REFERENCE while the real Utilities.cpp body takes it by VALUE, so the two
// were separate overloads rather than a declaration and its definition: every
// dialog call site bound to the no-op and the real FixFont was dead code on the
// port.  stdafx.h now declares FixFont unconditionally, so there is nothing to
// stub here — do not reintroduce a stub.

// IsArchiveXML is declared in stdafx.h (outside #ifdef _WIN32) and implemented
// in xml/xml_serialize.cpp — no stub needed here.

// ── Control-bar style constants (CBRS_*) ─────────────────────────────────
#ifndef CBRS_TOP
#  define CBRS_TOP            0x0001
#  define CBRS_BOTTOM         0x0002
#  define CBRS_LEFT           0x0004
#  define CBRS_RIGHT          0x0008
#  define CBRS_ORIENT_HORZ    (CBRS_TOP|CBRS_BOTTOM)
#  define CBRS_ORIENT_VERT    (CBRS_LEFT|CBRS_RIGHT)
#  define CBRS_ORIENT_ANY     (CBRS_ORIENT_HORZ|CBRS_ORIENT_VERT)
#  define CBRS_ALIGN_TOP      CBRS_TOP
#  define CBRS_ALIGN_BOTTOM   CBRS_BOTTOM
#  define CBRS_ALIGN_LEFT     CBRS_LEFT
#  define CBRS_ALIGN_RIGHT    CBRS_RIGHT
#  define CBRS_ALIGN_ANY      CBRS_ORIENT_ANY
#  define CBRS_GRIPPER        0x0400
#  define CBRS_TOOLTIPS       0x0010
#  define CBRS_FLYBY          0x0020
#  define CBRS_SIZE_DYNAMIC   0x0800
#  define CBRS_SIZE_FIXED     0x0000
#  define CBRS_FLOATING       0x1000
#  define CBRS_BORDER_3D      0x0200
#endif

// ── Toolbar styles ─────────────────────────────────────────────────────────
#ifndef TBSTYLE_FLAT
#  define TBSTYLE_FLAT        0x0800
#  define TBSTYLE_WRAPABLE    0x0200
#  define TBSTYLE_TOOLTIPS    0x0100
#  define TBSTYLE_ALTDRAG     0x0400
#endif

// ── Window style constants ─────────────────────────────────────────────────
#ifndef WS_CAPTION
#  define WS_OVERLAPPED       0x00000000L
#  define WS_POPUP            0x80000000L
#  define WS_CHILD            0x40000000L
#  define WS_MINIMIZE         0x20000000L
#  define WS_VISIBLE          0x10000000L
#  define WS_DISABLED         0x08000000L
#  define WS_CLIPSIBLINGS     0x04000000L
#  define WS_CLIPCHILDREN     0x02000000L
#  define WS_MAXIMIZE         0x01000000L
#  define WS_CAPTION          0x00C00000L
#  define WS_BORDER           0x00800000L
#  define WS_DLGFRAME         0x00400000L
#  define WS_VSCROLL          0x00200000L
#  define WS_HSCROLL          0x00100000L
#  define WS_SYSMENU          0x00080000L
#  define WS_THICKFRAME       0x00040000L
#  define WS_GROUP            0x00020000L
#  define WS_TABSTOP          0x00010000L
#  define WS_MINIMIZEBOX      0x00020000L
#  define WS_MAXIMIZEBOX      0x00010000L
#  define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX)
#  define WS_POPUPWINDOW      (WS_POPUP|WS_BORDER|WS_SYSMENU)
#  define WS_CHILDWINDOW      WS_CHILD
#endif

// ── WM_ACTIVATE wParam values ─────────────────────────────────────────────
#ifndef WA_INACTIVE
#  define WA_INACTIVE     0
#  define WA_ACTIVE       1
#  define WA_CLICKACTIVE  2
#endif

// ── WM_ message values ────────────────────────────────────────────────────
#ifndef WM_LBUTTONDBLCLK
#  define WM_LBUTTONDBLCLK  0x0203
#  define WM_RBUTTONDBLCLK  0x0206
#  define WM_MBUTTONDBLCLK  0x0209
#  define MM_MCINOTIFY      0x03B9   // multimedia MCI notify
#endif

// ── GetSystemMetrics indices ──────────────────────────────────────────────
#ifndef SM_CXSMICON
#  define SM_CXSMICON  49
#  define SM_CYSMICON  50
#  define SM_CXICON    11
#  define SM_CYICON    12
#endif

// ── AFX resource/ID constants ─────────────────────────────────────────────
#ifndef AFX_IDS_OPENFILE
#  define AFX_IDS_OPENFILE   0xF000
#  define AFX_IDS_SAVEFILE   0xF001
#  define AFX_IDW_DOCKBAR_TOP    0xE81B
#  define AFX_IDW_DOCKBAR_LEFT   0xE81C
#  define AFX_IDW_DOCKBAR_RIGHT  0xE81D
#  define AFX_IDW_DOCKBAR_BOTTOM 0xE81E
#endif

// ── SetWindowPos flags ────────────────────────────────────────────────────
#ifndef SWP_NOOWNERZORDER
#  define SWP_NOOWNERZORDER  0x0200
#  define SWP_FRAMECHANGED   0x0020
#  define SWP_SHOWWINDOW     0x0040
#  define SWP_HIDEWINDOW     0x0080
#endif

// ── LoadImage flags ───────────────────────────────────────────────────────
#ifndef LR_DEFAULTCOLOR
#  define LR_DEFAULTCOLOR  0x0000
#  define LR_DEFAULTSIZE   0x0040
#  define LR_LOADFROMFILE  0x0010
#  define LR_SHARED        0x8000
#endif

// ── System color indices ──────────────────────────────────────────────────
#ifndef COLOR_SCROLLBAR
#  define COLOR_SCROLLBAR    0
#  define COLOR_BACKGROUND   1
#  define COLOR_ACTIVECAPTION 2
#  define COLOR_WINDOW       5
#  define COLOR_BTNFACE      15
#  define COLOR_3DFACE       COLOR_BTNFACE
#  define COLOR_BTNTEXT      18
#  define COLOR_HIGHLIGHT    13
#  define COLOR_HIGHLIGHTTEXT 14
#endif

// ── SND_* sound flags ─────────────────────────────────────────────────────
#ifndef SND_ASYNC
#  define SND_SYNC      0x0000
#  define SND_ASYNC     0x0001
#  define SND_NODEFAULT 0x0002
#  define SND_MEMORY    0x0004
#  define SND_LOOP      0x0008
#  define SND_NOSTOP    0x0010
#  define SND_NOWAIT    0x00002000
#  define SND_FILENAME  0x00020000
#  define SND_RESOURCE  0x00040004
#  define SND_PURGE     0x00000040
#  define SND_APPLICATION 0x00000080
#endif

// ── MCI constants and structs ─────────────────────────────────────────────
#ifndef MCI_OPEN
#  define MCI_OPEN              0x0803
#  define MCI_CLOSE             0x0804
#  define MCI_PLAY              0x0806
#  define MCI_STOP              0x0808
#  define MCI_WAIT              0x00000002L
#  define MCI_NOTIFY            0x00000001L
#  define MCI_OPEN_ELEMENT      0x00000200L
#  define MCI_ALL_DEVICE_ID     ((UINT)-1)
#  define MCI_NOTIFY_SUCCESSFUL 0x0001
#  define MCI_NOTIFY_SUPERSEDED 0x0002
#  define MCI_NOTIFY_ABORTED    0x0004
#  define MCI_NOTIFY_FAILURE    0x0008
struct MCI_OPEN_PARMS {
    DWORD_PTR dwCallback;
    UINT      wDeviceID;
    LPCTSTR   lpstrDeviceType;
    LPCTSTR   lpstrElementName;
    LPCTSTR   lpstrAlias;
};
struct MCI_PLAY_PARMS {
    DWORD_PTR dwCallback;
    DWORD     dwFrom;
    DWORD     dwTo;
};
struct MCI_GENERIC_PARMS {
    DWORD_PTR dwCallback;
};
inline UINT mciSendCommand(UINT, UINT, DWORD_PTR, DWORD_PTR) { return 0; }
inline BOOL mciGetErrorString(DWORD, LPTSTR buf, UINT len)
    { if (buf && len) buf[0] = '\0'; return FALSE; }
#endif

// ── Tooltip constants and structs ─────────────────────────────────────────
#ifndef TTF_IDISHWND
#  define TTF_IDISHWND   0x0001
#  define TTF_CENTERTIP  0x0002
#  define TTF_RTLREADING 0x0004
#  define TTF_SUBCLASS   0x0010
#  define TTN_NEEDTEXT   (0U-520U)
#  define TTN_NEEDTEXTA  TTN_NEEDTEXT
#  define TTN_NEEDTEXTW  (0U-530U)
// TOOLTIPTEXTA is the same layout as the existing TOOLTIPTEXT (char-based)
typedef TOOLTIPTEXT TOOLTIPTEXTA;
struct TOOLTIPTEXTW {
    NMHDR    hdr     = {};
    LPWSTR   lpszText = nullptr;
    wchar_t  szText[80] = {};
    HINSTANCE hinst  = nullptr;
    UINT     uFlags  = 0;
};
#endif

// ── WinHelp command ───────────────────────────────────────────────────────
#ifndef HELP_FINDER
#  define HELP_FINDER  0x000B
#  define HELP_CONTEXT 0x0001
#  define HELP_INDEX   0x0003
#  define HELP_QUIT    0x0002
#endif

// ── CoCreateGuid — generate a random UUID v4 ─────────────────────────────
#ifndef _WIN32
#include <cstdio>
inline HRESULT CoCreateGuid(GUID* pguid) {
    if (!pguid) return E_INVALIDARG;
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f || fread(pguid, sizeof(GUID), 1, f) != 1) {
        if (f) fclose(f);
        memset(pguid, 0, sizeof(GUID));
        return E_FAIL;
    }
    fclose(f);
    // Version 4 (random) and variant bits
    pguid->Data3 = (pguid->Data3 & 0x0FFF) | 0x4000;
    pguid->Data4[0] = (pguid->Data4[0] & 0x3F) | 0x80;
    return S_OK;
}
#endif

// ── Global Win32 function stubs ───────────────────────────────────────────
// GUI build routes to the live desktop theme (wxSystemSettings) via the hook;
// mushcore/CLI (hook unset) keeps the light-grey fallback.
inline DWORD GetSysColor(int idx) {
    if (g_wxhook_get_sys_color) return (DWORD) g_wxhook_get_sys_color(idx);
    return 0xC0C0C0;   // light grey default
}
inline BOOL  DestroyIcon(HICON) { return TRUE; }
inline BOOL  SetMenu(HWND, HMENU) { return FALSE; }
inline HMENU GetMenu(HWND) { return 0; }
inline int   GetDlgCtrlID(HWND) { return 0; }
// GetSystemMetrics and LoadImage already defined earlier in this file
inline BOOL  PlaySound(LPCTSTR, HINSTANCE, DWORD) { return FALSE; }

// ── AfxLoadString / AfxExtractSubString ──────────────────────────────────
inline BOOL AfxLoadString(UINT, LPTSTR buf, UINT n = 256)
    { if (buf && n) buf[0] = '\0'; return FALSE; }
inline BOOL AfxLoadString(UINT, CString& s)
    { s = ""; return FALSE; }
inline BOOL AfxExtractSubString(CString& out, LPCTSTR src, int sub, char sep = '\n') {
    if (!src) return FALSE;
    for (int i = 0; i < sub; ++i) {
        src = strchr(src, sep);
        if (!src) return FALSE;
        ++src;
    }
    const char* end = strchr(src, sep);
    out = end ? CString(src, (int)(end - src)) : CString(src);
    return TRUE;
}

// ── _mbstowcsz / _wcstombsz ──────────────────────────────────────────────
// Non-standard MSVC helpers: like mbstowcs/wcstombs but always NUL-terminate.
inline wchar_t* _mbstowcsz(wchar_t* dst, const char* src, size_t n) {
    if (n == 0) return dst;
    size_t converted = mbstowcs(dst, src, n - 1);
    dst[converted < n ? converted : n - 1] = L'\0';
    return dst;
}
inline char* _wcstombsz(char* dst, const wchar_t* src, size_t n) {
    if (n == 0) return dst;
    size_t converted = wcstombs(dst, src, n - 1);
    dst[converted < n ? converted : n - 1] = '\0';
    return dst;
}

// ── Window painting / border constants ───────────────────────────────────
#ifndef BF_TOP
#  define BF_LEFT      0x0001
#  define BF_TOP       0x0002
#  define BF_RIGHT     0x0004
#  define BF_BOTTOM    0x0008
#  define BF_RECT      (BF_LEFT|BF_TOP|BF_RIGHT|BF_BOTTOM)
#  define BF_ADJUST    0x2000
#  define BF_FLAT      0x4000
#  define BF_SOFT      0x1000
#  define BF_MONO      0x8000
#  define BF_TOPLEFT             (BF_TOP|BF_LEFT)
#  define BF_TOPRIGHT            (BF_TOP|BF_RIGHT)
#  define BF_BOTTOMLEFT          (BF_BOTTOM|BF_LEFT)
#  define BF_BOTTOMRIGHT         (BF_BOTTOM|BF_RIGHT)
#  define BF_DIAGONAL            0x0010
#  define BF_DIAGONAL_ENDTOPLEFT    (BF_DIAGONAL|BF_TOP|BF_LEFT)
#  define BF_DIAGONAL_ENDTOPRIGHT   (BF_DIAGONAL|BF_TOP|BF_RIGHT)
#  define BF_DIAGONAL_ENDBOTTOMLEFT  (BF_DIAGONAL|BF_BOTTOM|BF_LEFT)
#  define BF_DIAGONAL_ENDBOTTOMRIGHT (BF_DIAGONAL|BF_BOTTOM|BF_RIGHT)
#  define BF_MIDDLE    0x0800
#endif
#ifndef CTLCOLOR_STATIC
#  define CTLCOLOR_MSGBOX    0
#  define CTLCOLOR_EDIT      1
#  define CTLCOLOR_LISTBOX   2
#  define CTLCOLOR_BTN       3
#  define CTLCOLOR_DLG       4
#  define CTLCOLOR_SCROLLBAR 5
#  define CTLCOLOR_STATIC    6
#  define CTLCOLOR_MAX       7
#endif

// ── Static control styles ─────────────────────────────────────────────────
#ifndef SS_LEFT
#  define SS_LEFT        0x00000000L
#  define SS_CENTER      0x00000001L
#  define SS_RIGHT       0x00000002L
#  define SS_ICON        0x00000003L
#  define SS_BLACKRECT   0x00000004L
#  define SS_BITMAP      0x0000000EL
#  define SS_NOTIFY      0x00000100L
#  define SS_SUNKEN      0x00001000L
#endif

// ── Class style constants ──────────────────────────────────────────────────
#ifndef CS_VREDRAW
#  define CS_VREDRAW     0x0001
#  define CS_HREDRAW     0x0002
#  define CS_OWNDC       0x0020
#  define CS_CLASSDC     0x0040
#  define CS_PARENTDC    0x0080
#  define CS_NOCLOSE     0x0200
#  define CS_SAVEBITS    0x0800
#  define CS_BYTEALIGNCLIENT 0x1000
#  define CS_BYTEALIGNWINDOW 0x2000
#  define CS_GLOBALCLASS 0x4000
#  define CS_DROPSHADOW  0x00020000
#endif

// ── GetClassLong indices ──────────────────────────────────────────────────
#ifndef GCL_HICONSM
#  define GCL_HICONSM   (-34)
#  define GCL_HICON     (-14)
#  define GCL_HBRBACKGROUND (-10)
#  define GCL_HCURSOR   (-12)
#  define GCL_STYLE     (-26)
#endif

// ── GetWindow / GetNextWindow ─────────────────────────────────────────────
#ifndef GW_HWNDNEXT
#  define GW_HWNDFIRST   0
#  define GW_HWNDLAST    1
#  define GW_HWNDNEXT    2
#  define GW_HWNDPREV    3
#  define GW_OWNER       4
#  define GW_CHILD       5
#endif

// LOGFONT and font constants already defined earlier in this file.
// HDWP — deferred window positioning handle
typedef void* HDWP;

// ── Multimonitor stubs ────────────────────────────────────────────────────
#ifndef HMONITOR_defined
#  define HMONITOR_defined
typedef void* HMONITOR;
#  define MONITOR_DEFAULTTONEAREST  2
#  define MONITOR_DEFAULTTOPRIMARY  1
#  define MONITOR_DEFAULTTONULL     0
struct MONITORINFO {
    DWORD cbSize = sizeof(MONITORINFO);
    RECT  rcMonitor = {};
    RECT  rcWork = {};
    DWORD dwFlags = 0;
};
inline HMONITOR MonitorFromRect(LPCRECT, DWORD) { return nullptr; }
inline HMONITOR MonitorFromWindow(HWND, DWORD)  { return nullptr; }
inline BOOL GetMonitorInfo(HMONITOR, MONITORINFO*) { return FALSE; }
#endif

// ── Print / document stubs ────────────────────────────────────────────────
#ifndef SP_ERROR
#  define SP_ERROR   (-1)
#  define SP_OUTOFDISK  (-4)
#  define SP_OUTOFMEMORY (-5)
#  define SP_USERABORT (-4)
#  define PD_ALLPAGES       0x00000000L
#  define PD_PAGENUMS       0x00000002L
#  define PD_NOPAGENUMS     0x00000008L
#  define PD_RETURNDC       0x00000100L
#  define PHYSICALWIDTH      110
#  define PHYSICALHEIGHT     111
#  define PHYSICALOFFSETX    112
#  define PHYSICALOFFSETY    113
inline int  StartDoc(HDC, const void*) { return 0; }
inline int  EndDoc(HDC) { return 0; }
inline int  StartPage(HDC) { return 0; }
inline int  EndPage(HDC) { return 0; }
// ABORTPROC: printer abort callback type (matches Windows SDK)
typedef BOOL (CALLBACK *ABORTPROC)(HDC, int);
inline int  SetAbortProc(HDC, ABORTPROC) { return 0; }
#endif

// ── OSVERSIONINFO ─────────────────────────────────────────────────────────
#ifndef OSVERSIONINFO_defined
#  define OSVERSIONINFO_defined
struct OSVERSIONINFO {
    DWORD dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    DWORD dwMajorVersion = 0;
    DWORD dwMinorVersion = 0;
    DWORD dwBuildNumber  = 0;
    DWORD dwPlatformId   = 0;
    char  szCSDVersion[128] = {};
};
#  define VER_PLATFORM_WIN32_NT 2
inline BOOL GetVersionEx(OSVERSIONINFO* p) {
    if (p) { p->dwMajorVersion = 10; p->dwPlatformId = VER_PLATFORM_WIN32_NT; }
    return TRUE;
}
#endif
// Also alias OS_VERSION_INFO to OSVERSIONINFO so miniwindow.cpp is happy:
typedef OSVERSIONINFO OS_VERSION_INFO;
// os_version global — defined in MUSHclient.cpp (or here as a weak stub):
// (declared extern in mfc_ui.h already if needed)

// ── Locale / font enumeration stubs ──────────────────────────────────────
#ifndef LOCALE_SABBREVLANGNAME
#  define LOCALE_SABBREVLANGNAME  0x00000003
#  define LOCALE_SLANGUAGE        0x00000002
#  define LOCALE_SCOUNTRY         0x00000006
typedef DWORD LCID;
inline int    GetLocaleInfo(LCID, DWORD, LPTSTR, int) { return 0; }
inline LCID   GetUserDefaultLCID() { return 0; }
#endif
// LOGFONT already defined earlier in this file.
#ifndef ENUMLOGFONTEX_defined
#define ENUMLOGFONTEX_defined
struct ENUMLOGFONTEX { LOGFONT elfLogFont; char elfFullName[64]; char elfStyle[32]; char elfScript[32]; };
#endif
#ifndef FONTENUMPROC_defined
#define FONTENUMPROC_defined
// CALLBACK may be defined via platform macros; ensure it exists
#ifndef CALLBACK
#  define CALLBACK
#endif
typedef int (CALLBACK* FONTENUMPROC)(const LOGFONT*, const void*, DWORD, LPARAM);
typedef int (CALLBACK* FONTENUMEXPROC)(const ENUMLOGFONTEX*, const void*, DWORD, LPARAM);
#endif
inline int EnumFontFamiliesEx(HDC, LOGFONT*, FONTENUMEXPROC, LPARAM, DWORD) { return 1; }
// CALLBACK typedef alias for function pointers
#ifndef CALLBACK
#  define CALLBACK
#endif
typedef void* FARPROC;
typedef void* PROC;

// ── ListView extended styles ───────────────────────────────────────────────
#ifndef LVS_EX_HEADERDRAGDROP
#  define LVS_EX_HEADERDRAGDROP  0x00000010
#  define LVS_EX_FULLROWSELECT   0x00000020
#  define LVS_EX_GRIDLINES       0x00000001
#  define LVS_EX_CHECKBOXES      0x00000004
#  define LVS_EX_TRACKSELECT     0x00000008
#  define LVS_EX_SUBITEMIMAGES   0x00000002
#  define LVS_EX_FLATSB          0x00000100
#  define LVS_EX_INFOTIP         0x00000400
#endif

// ── Memory flags ──────────────────────────────────────────────────────────
#ifndef GMEM_MOVEABLE
#  define GMEM_MOVEABLE   0x0002
#  define GMEM_ZEROINIT   0x0040
#  define GMEM_FIXED      0x0000
#endif
#ifndef LMEM_MOVEABLE
#  define LMEM_MOVEABLE   0x0002
#  define LMEM_ZEROINIT   0x0040
#  define LMEM_FIXED      0x0000
#endif
// Local heap stubs (always available — LocalAlloc/Lock/Unlock are unconditional)
inline HLOCAL  LocalAlloc(UINT, SIZE_T n)    { return malloc(n); }
inline LPVOID  LocalLock(HLOCAL p)           { return p; }
inline BOOL    LocalUnlock(HLOCAL)           { return FALSE; }

// ── Global Win32 function stubs ───────────────────────────────────────────
inline BOOL   BringWindowToTop(HWND)                     { return FALSE; }
inline LONG   GetClassLong(HWND, int)                    { return 0; }
inline int    GetClassName(HWND, LPTSTR, int)            { return 0; }
inline HWND   GetNextWindow(HWND, UINT)                  { return nullptr; }
inline HWND   GetParent(HWND)                            { return nullptr; }
inline HWND   GetTopWindow(HWND)                         { return nullptr; }
inline HDC    GetWindowDC(HWND)                          { return nullptr; }
inline int    ReleaseDC(HWND, HDC)                       { return 0; }
inline BOOL   TrackPopupMenu(HMENU,UINT,int,int,int,HWND,LPCRECT) { return FALSE; }
inline int    GetWindowText(HWND, LPTSTR, int)           { return 0; }
inline BOOL   SetWindowText(HWND, LPCTSTR)               { return FALSE; }
inline int    GetWindowTextLength(HWND)                  { return 0; }
inline BOOL   DrawEdge(HDC, RECT*, UINT, UINT)           { return FALSE; }
inline HWND   GetWindow(HWND, UINT)                      { return nullptr; }
inline BOOL   GetWindowsDirectory(LPTSTR buf, UINT n)    { if(buf&&n) buf[0]='\0'; return FALSE; }
inline HCURSOR CopyCursor(HCURSOR h)                     { return h; }
inline BOOL   InitCommonControls()                       { return TRUE; }
inline HRESULT CoInitialize(void*)                       { return S_OK; }
inline void   CoUninitialize()                           {}
// SetWindowPos already defined earlier in this file.

// ── Afx additional helpers ────────────────────────────────────────────────
// ── _AFX_THREAD_STATE stub ────────────────────────────────────────────────
struct _AFX_THREAD_STATE {
    MSG m_lastSentMsg = {};
    MSG m_lastRecvMsg = {};
};
inline _AFX_THREAD_STATE* AfxGetThreadState() {
    static _AFX_THREAD_STATE s;
    return &s;
}

inline LPCTSTR    AfxGetAppName()        { return "MUSHclient"; }
inline CWinThread* AfxGetThread()        { static CWinThread t; return &t; }
inline void    AfxEnableControlContainer() {}
inline BOOL    AfxInitRichEdit()         { return FALSE; }
inline BOOL    AfxInitRichEdit2()        { return FALSE; }
inline BOOL    AfxOleInit()              { return TRUE; }  // OLE not needed on non-Windows
inline void    AfxSetResourceHandle(HINSTANCE) {}
inline BOOL    AfxSocketInit(LPVOID = nullptr) { return TRUE; } // POSIX sockets need no init
inline void    AfxGetModuleState()       {}
// AfxGetResourceHandle / AfxGetInstanceHandle already defined earlier.

// ── DirectSound stubs ─────────────────────────────────────────────────────
#ifndef DSSCL_NORMAL
#  define DSSCL_NORMAL        1
#  define DSSCL_PRIORITY      2
#  define DSSCL_EXCLUSIVE     3
#  define DSBCAPS_PRIMARYBUFFER 0x00000001
struct DSBUFFERDESC { DWORD dwSize=sizeof(DSBUFFERDESC); DWORD dwFlags=0; DWORD dwBufferBytes=0; DWORD dwReserved=0; void* lpwfxFormat=nullptr; };
inline HRESULT DirectSoundCreate(void*, void**, void*) { return E_FAIL; }
#endif

// ── _vsnprintf / _tcsdup ─────────────────────────────────────────────────
#ifndef _vsnprintf
#  define _vsnprintf vsnprintf
#endif
#ifndef _tcsdup
#  define _tcsdup strdup
#endif
#ifndef _getdcwd
#  define _getdcwd(d,buf,n) getcwd(buf,n)
#endif

// ── AFX_SIZEPARENTPARAMS — used in MDITabs.cpp ────────────────────────────
#ifndef AFX_SIZEPARENTPARAMS
struct AFX_SIZEPARENTPARAMS {
    HDWP hDWP;
    RECT rect;
    SIZE sizeTotal;
    BOOL bStretch;
};
inline HDWP BeginDeferWindowPos(int) { return nullptr; }
inline HDWP DeferWindowPos(HDWP h, HWND, HWND, int,int,int,int, UINT) { return h; }
inline BOOL EndDeferWindowPos(HDWP) { return FALSE; }
inline void AfxRepositionWindow(AFX_SIZEPARENTPARAMS*, HWND, LPCRECT) {}
#define AFX_SIZEPARENTPARAMS AFX_SIZEPARENTPARAMS
#endif

// ── CFont::GetObject ─────────────────────────────────────────────────────
// CFont is defined earlier but missing GetObject; add it via extension macro.
// We can't reopen the class here, so patch via a free helper if needed.
// The CDK approach: just note that CFont already has GetSafeHandle();
// GetObject(LOGFONT*) fills a LOGFONT with zeros.

// ── CListCtrl additional methods ──────────────────────────────────────────
// CListCtrl is defined earlier in this file; it needs GetColumnWidth/SetColumnWidth.
// Cannot reopen the class here, so these are handled inside the class definition.

// ── CWnd::FromHandle / FromHandlePermanent ────────────────────────────────
// These are static members of CWnd; patched in the class definition above.

// ── Forward declaration of the free function defined later in mainfrm.cpp
// (needed so the ::OnNeedText call-site in the same file resolves)
BOOL OnNeedText(UINT id, NMHDR* pNMHDR, LRESULT* pResult);

// ── GDI pen / brush / fill style constants ────────────────────────────────
#ifndef PS_SOLID
#  define PS_SOLID          0
#  define PS_DASH           1
#  define PS_DOT            2
#  define PS_DASHDOT        3
#  define PS_DASHDOTDOT     4
#  define PS_NULL           5
#  define PS_INSIDEFRAME    6
#  define PS_USERSTYLE      7
#  define PS_ALTERNATE      8
#  define PS_STYLE_MASK     0x0000000F
#  define PS_ENDCAP_ROUND   0x00000000
#  define PS_ENDCAP_SQUARE  0x00000100
#  define PS_ENDCAP_FLAT    0x00000200
#  define PS_ENDCAP_MASK    0x00000F00
#  define PS_JOIN_ROUND     0x00000000
#  define PS_JOIN_BEVEL     0x00001000
#  define PS_JOIN_MITER     0x00002000
#  define PS_JOIN_MASK      0x0000F000
#  define PS_COSMETIC       0x00000000
#  define PS_GEOMETRIC      0x00010000
#  define PS_TYPE_MASK      0x000F0000
#endif
#ifndef HS_HORIZONTAL
#  define HS_HORIZONTAL     0
#  define HS_VERTICAL       1
#  define HS_FDIAGONAL      2
#  define HS_BDIAGONAL      3
#  define HS_CROSS          4
#  define HS_DIAGCROSS      5
#endif
#ifndef ALTERNATE
#  define ALTERNATE         1
#  define WINDING           2
#endif
#ifndef TA_LEFT
#  define TA_NOUPDATECP     0
#  define TA_UPDATECP       1
#  define TA_LEFT           0
#  define TA_RIGHT          2
#  define TA_CENTER         6
#  define TA_TOP            0
#  define TA_BOTTOM         8
#  define TA_BASELINE       24
#endif
#ifndef FLOODFILLSURFACE
#  define FLOODFILLSURFACE  1
#  define FLOODFILLBORDER   0
#endif
#ifndef ETO_OPAQUE
#  define ETO_OPAQUE        0x0002
#  define ETO_CLIPPED       0x0004
#endif
#ifndef GM_COMPATIBLE
#  define GM_COMPATIBLE     1
#  define GM_ADVANCED       2
#endif
#ifndef MWT_IDENTITY
#  define MWT_IDENTITY      1
#  define MWT_LEFTMULTIPLY  2
#  define MWT_RIGHTMULTIPLY 3
#endif
#ifndef EDGE_RAISED
#  define EDGE_RAISED       0x05
#  define EDGE_SUNKEN       0x0a
#  define EDGE_ETCHED       0x06
#  define EDGE_BUMP         0x09
#  define BDR_RAISEDOUTER   0x01
#  define BDR_SUNKENOUTER   0x02
#  define BDR_RAISEDINNER   0x04
#  define BDR_SUNKENINNER   0x08
#endif

// ── DIB / bitmap constants ─────────────────────────────────────────────────
#ifndef BI_RGB
#  define BI_RGB            0L
#  define BI_RLE8           1L
#  define BI_RLE4           2L
#  define BI_BITFIELDS      3L
#  define DIB_RGB_COLORS    0
#  define DIB_PAL_COLORS    1
#endif

// ── TPM (TrackPopupMenu) flags ─────────────────────────────────────────────
#ifndef TPM_LEFTALIGN
#  define TPM_LEFTALIGN     0x0000L
#  define TPM_CENTERALIGN   0x0004L
#  define TPM_RIGHTALIGN    0x0008L
#  define TPM_TOPALIGN      0x0000L
#  define TPM_VCENTERALIGN  0x0010L
#  define TPM_BOTTOMALIGN   0x0020L
#  define TPM_NONOTIFY      0x0080L
#  define TPM_RETURNCMD     0x0100L
#endif

// ── GWL / window long field indices ───────────────────────────────────────
#ifndef GWL_STYLE
#  define GWL_WNDPROC    (-4)
#  define GWL_HINSTANCE  (-6)
#  define GWL_HWNDPARENT (-8)
#  define GWL_STYLE      (-16)
#  define GWL_EXSTYLE    (-20)
#  define GWL_USERDATA   (-21)
#  define GWL_ID         (-12)
#endif
#ifndef WS_EX_TOPMOST
#  define WS_EX_TOPMOST       0x00000008L
#  define WS_EX_TRANSPARENT   0x00000020L
#  define WS_EX_LAYERED       0x00080000L
#  define WS_EX_TOOLWINDOW    0x00000080L
#  define WS_EX_APPWINDOW     0x00040000L
#  define WS_EX_CLIENTEDGE    0x00000200L
#  define WS_EX_WINDOWEDGE    0x00000100L
#endif

// ── ListView / ListCtrl constants ──────────────────────────────────────────
#ifndef LVSIL_SMALL
#  define LVSIL_NORMAL  0
#  define LVSIL_SMALL   1
#  define LVSIL_STATE   2
#  define LVM_SETCOLUMNORDERARRAY (0x1000+58)
#  define LVM_GETCOLUMNORDERARRAY (0x1000+59)
typedef struct tagNMHDR2 { HWND hwndFrom; UINT idFrom; UINT code; } NMHDR2;
#endif
// NMLISTVIEW already defined earlier in this file.
// NM_LISTVIEW is the Windows alias for NMLISTVIEW:
#ifndef NM_LISTVIEW
typedef NMLISTVIEW NM_LISTVIEW;
#define NM_LISTVIEW NM_LISTVIEW
#endif

// ── GDI XFORM / world transform ───────────────────────────────────────────
#ifndef XFORM_defined
#define XFORM_defined
struct XFORM {
    float eM11, eM12, eM21, eM22, eDx, eDy;
};
#endif

// ── Gradient fill ─────────────────────────────────────────────────────────
#ifndef GRADIENT_FILL_RECT_H
#  define GRADIENT_FILL_RECT_H  0x00
#  define GRADIENT_FILL_RECT_V  0x01
#  define GRADIENT_FILL_TRIANGLE 0x02
struct TRIVERTEX { LONG x, y; COLORREF Red, Green, Blue, Alpha; };
struct GRADIENT_RECT { ULONG UpperLeft, LowerRight; };
#ifndef MUSHCLIENT_FULL_BUILD
inline BOOL GradientFill(HDC, TRIVERTEX*, ULONG, void*, ULONG, ULONG) { return FALSE; }
#else
// Real implementation in wx_gdi_compat.cpp using wxDC::GradientFillLinear.
BOOL GradientFill(HDC hdc, TRIVERTEX* pVertex, ULONG nVertex,
                  void* pMesh, ULONG nMesh, ULONG ulMode);
#endif
#endif

// ── Shell folder browser ───────────────────────────────────────────────────
#ifndef BROWSEINFO_defined
#define BROWSEINFO_defined
typedef void* LPITEMIDLIST;
// IMalloc stub — code calls pMalloc->Free(pidl), pMalloc->Release()
struct IMalloc_stub {
    void  Free(void*)         {}
    void* Alloc(SIZE_T)       { return nullptr; }
    ULONG Release()           { return 0; }
    ULONG AddRef()            { return 1; }
};
typedef IMalloc_stub* LPMALLOC;
typedef int (CALLBACK *BFFCALLBACK)(HWND, UINT, LPARAM, LPARAM);
struct BROWSEINFO {
    HWND         hwndOwner;
    LPITEMIDLIST pidlRoot;
    LPTSTR       pszDisplayName;
    LPCTSTR      lpszTitle;
    UINT         ulFlags;
    BFFCALLBACK  lpfn;   // browse callback
    LPARAM       lParam;
    int        iImage;
};
#  define BIF_RETURNONLYFSDIRS   0x0001
#  define BIF_DONTGOBELOWDOMAIN  0x0002
#  define BIF_RETURNFSANCESTORS  0x0008
#  define BIF_EDITBOX            0x0010
#  define BIF_VALIDATE           0x0020
#  define BIF_NEWDIALOGSTYLE     0x0040
#  define BIF_USENEWUI           (BIF_NEWDIALOGSTYLE | BIF_EDITBOX)
#  define BFFM_INITIALIZED       1
#  define BFFM_SELCHANGED        2
#  define BFFM_SETSELECTION      (WM_USER + 103)
#  define BFFM_SETSTATUSTEXT     (WM_USER + 104)
inline LPITEMIDLIST SHBrowseForFolder(BROWSEINFO*) { return nullptr; }
inline BOOL SHGetPathFromIDList(LPITEMIDLIST, LPTSTR buf) { if (buf) buf[0]='\0'; return FALSE; }
inline HRESULT SHGetMalloc(LPMALLOC*) { return E_FAIL; }
// Note: BrowseCallbackProc is defined in GlobalPrefs.cpp — no stub here.
#endif

// ── NOERROR / COM helpers ─────────────────────────────────────────────────
#ifndef NOERROR
#  define NOERROR 0
#endif

// ── DirectSound stub ──────────────────────────────────────────────────────
#ifndef DSBSTATUS_PLAYING
#  define DSBSTATUS_PLAYING  0x00000001
#  define DSBSTATUS_LOOPING  0x00000004
#endif

// ── DestroyAcceleratorTable ────────────────────────────────────────────────
inline BOOL DestroyAcceleratorTable(HACCEL) { return TRUE; }

// ── GetWindowLong / SetWindowLong stubs ────────────────────────────────────
inline LONG GetWindowLong(HWND, int) { return 0; }
inline LONG SetWindowLong(HWND, int, LONG) { return 0; }
inline LONG_PTR GetWindowLongPtr(HWND, int) { return 0; }
inline LONG_PTR SetWindowLongPtr(HWND, int, LONG_PTR) { return 0; }

// ── LocalFree ────────────────────────────────────────────────────────────
// GlobalFree/GlobalAlloc/GlobalLock/GlobalUnlock already defined earlier.
inline HLOCAL  LocalFree(HLOCAL p)  { free(p); return nullptr; }
// LocalAlloc/LocalLock/LocalUnlock — stubs defined earlier in this file.

// FormatMessage already defined earlier in this file.

// ── Afx helpers ───────────────────────────────────────────────────────────
inline void AfxFormatString1(CString& out, UINT, LPCTSTR s) { out = s ? s : ""; }
inline void AfxFormatString2(CString& out, UINT, LPCTSTR s1, LPCTSTR s2) { out = ""; }
inline void AfxOleLockApp()   {}
inline void AfxOleUnlockApp() {}

// ── CWaitCursor — RAII scoped wait cursor (no-op on non-Windows) ──────────
class CWaitCursor {
public:
    CWaitCursor()  {}
    ~CWaitCursor() {}
    void Restore() {}
};
inline void AfxThrowResourceException() { throw std::runtime_error("resource exception"); }
// AfxThrowMemoryException already defined in mfc_exceptions.h.
inline BOOL SetRegistryKey(LPCTSTR) { return FALSE; }
inline UINT AfxGetApp_nID() { return 0; }

// ── AFX_IDP constants ─────────────────────────────────────────────────────
#ifndef AFX_IDP_PARSE_STRING_SIZE
#  define AFX_IDP_PARSE_STRING_SIZE  0xF072
#  define AFX_IDP_PARSE_INT          0xF071
#  define AFX_IDP_PARSE_REAL         0xF073
#  define AFX_IDP_PARSE_INT_RANGE    0xF076
#  define AFX_IDP_PARSE_REAL_RANGE   0xF077
#  define AFX_IDP_PARSE_STRING_SIZE_RESTRICT 0xF094
#endif

// ── CFile::status flags ───────────────────────────────────────────────────
#ifndef CFileStatus_defined
#define CFileStatus_defined
struct CFileStatus {
    CTime    m_ctime;
    CTime    m_mtime;
    CTime    m_atime;
    LONG     m_size;
    BYTE     m_attribute;
    BYTE     _m_padding;
    TCHAR    m_szFullName[_MAX_PATH];
};
#endif
// CFile member flag constants
#ifndef CFile_readOnly
namespace CFileAttrib {
    enum { readOnly = 0x01, hidden = 0x02, system = 0x04, volume = 0x08,
           directory = 0x10, archive = 0x20 };
}
// These are also exposed as CFile::readOnly etc on Windows
// provide as standalone constants since CFile is a stub
#  define CFile_readOnly   0x01
#  define CFile_hidden     0x02
#  define CFile_system     0x04
#  define CFile_directory  0x10
#  define CFile_archive    0x20
#endif

// ── PNG loading helpers ────────────────────────────────────────────────────
// LoadPng / LoadPngMemory are declared in stdafx.h and implemented in Utilities.cpp.
struct gRectType { int left, top, right, bottom; };
typedef void* HPNG;
inline int gRect_width(const gRectType& r)  { return r.right - r.left; }
inline int gRect_height(const gRectType& r) { return r.bottom - r.top; }
typedef gRectType* pgRect;
// os_version — used in MUSHclient.cpp / miniwindow.cpp (OSVERSIONINFO == OS_VERSION_INFO)
extern OS_VERSION_INFO os_version;

// ── ExtCreatePen / GDI drawing stubs ─────────────────────────────────────
#ifndef MUSHCLIENT_FULL_BUILD
inline HPEN ExtCreatePen(DWORD,DWORD,const LOGBRUSH*,DWORD,const DWORD*) { return nullptr; }
#else
HPEN ExtCreatePen(DWORD dwPenStyle, DWORD dwWidth, const LOGBRUSH* lplb,
                  DWORD dwStyleCount, const DWORD* lpStyle);
#endif
inline BOOL SetBrushOrgEx(HDC, int, int, POINT*) { return FALSE; }
inline BOOL SetGraphicsMode(HDC, int) { return FALSE; }
inline BOOL SetWorldTransform(HDC, const XFORM*) { return FALSE; }
inline BOOL ModifyWorldTransform(HDC, const XFORM*, DWORD) { return FALSE; }
// GUI build reads the real pixel from the wx DC / DIBSection that owns the HDC
// (so colour-key transparency keys on the true pixel); mushcore returns 0.
inline COLORREF GetPixel(HDC hdc, int x, int y) {
    if (g_wxhook_get_pixel) return (COLORREF) g_wxhook_get_pixel((void*) hdc, x, y);
    return 0;
}
// CreateDIBSection: in the full build this has a real implementation in
// wx_gdi_compat.cpp that allocates a pixel buffer and syncs with CDC::BitBlt.
// SelectObject(HDC, HGDIOBJ) is declared above near the other raw Win32 stubs
// (guarded #ifndef MUSHCLIENT_FULL_BUILD) and in wx_gdi_compat.cpp for the full build.
#ifndef MUSHCLIENT_FULL_BUILD
inline HBITMAP CreateDIBSection(HDC, const BITMAPINFO*, UINT, void**, HANDLE, DWORD) { return nullptr; }
#else
HBITMAP CreateDIBSection(HDC hdc, const BITMAPINFO* pbmi, UINT iUsage,
                          void** ppvBits, HANDLE hSection, DWORD offset);
#endif

// ── CWnd additional methods ───────────────────────────────────────────────
// (methods used in various .cpp files that weren't in the original stub)

// ── Additional COleVariant member ─────────────────────────────────────────
// COleVariant::Attach is used in plugins.cpp
// Add to COleVariant after its definition via a free helper (can't reopen class here)
// — handled inline in the class definition above.

// ── COleSafeArray::Detach ─────────────────────────────────────────────────
// Already defined in COleSafeArray stub above?  Add if missing.

// ── TreeView macros ───────────────────────────────────────────────────────
#ifndef TreeView_SetUnicodeFormat
#  define TreeView_SetUnicodeFormat(hwnd, fUnicode)  ((BOOL)0)
#  define TreeView_GetUnicodeFormat(hwnd)            ((BOOL)0)
#endif

// ── Accelerator table ─────────────────────────────────────────────────────
#ifndef ACCEL_defined
#define ACCEL_defined
struct ACCEL { BYTE fVirt; WORD key; WORD cmd; };
inline HACCEL CreateAcceleratorTable(ACCEL*, int) { return nullptr; }
inline int    CopyAcceleratorTable(HACCEL, ACCEL*, int) { return 0; }
#endif

// ── CUIntArray ────────────────────────────────────────────────────────────
// Simple growable array of UINT (used in genpropertypage, PluginsDlg, etc.)
#ifndef CUIntArray_defined
#define CUIntArray_defined
class CUIntArray {
    std::vector<UINT> m_data;
public:
    int  GetSize() const           { return (int)m_data.size(); }
    int  GetCount() const          { return (int)m_data.size(); }
    UINT GetAt(int i) const        { return m_data[i]; }
    void SetAt(int i, UINT v)      { m_data[i] = v; }
    void Add(UINT v)               { m_data.push_back(v); }
    void RemoveAll()               { m_data.clear(); }
    void SetSize(int n, int =-1)   { m_data.resize(n); }
    UINT operator[](int i) const   { return m_data[i]; }
    UINT& operator[](int i)        { return m_data[i]; }
};
#endif

// ── Network stubs (Winsock replacements) ──────────────────────────────────
#ifndef WSA_stubs_defined
#define WSA_stubs_defined
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#ifndef WSAEWOULDBLOCK
#  define WSAEWOULDBLOCK EWOULDBLOCK
#endif
inline int  WSAGetLastError()   { return errno; }
inline int  closesocket(int fd) { return close(fd); }
// HOSTENT is struct hostent on POSIX
typedef struct hostent  HOSTENT;
typedef struct hostent* LPHOSTENT;
// WSAAsyncGetHostByName — non-blocking DNS; stub returns 0 (failure), code falls back
inline HANDLE WSAAsyncGetHostByName(HWND, UINT, const char*, char*, int) { return nullptr; }
#endif

// ── Code-page stubs ───────────────────────────────────────────────────────
#ifndef GetACP_defined
#define GetACP_defined
inline UINT GetACP()  { return 1252; }  // Western European
inline UINT GetOEMCP() { return 437; }  // DOS US
#endif

// ── TCHAR string helpers ──────────────────────────────────────────────────
#ifndef _tcspbrk
#  define _tcspbrk strpbrk
#endif
#ifndef _tcsncpy
#  define _tcsncpy strncpy
#endif

// ── CMDIChildWnd / CMultiDocTemplate extras ───────────────────────────────
// GetDefaultAccelerator — returns the frame's accelerator table
// m_hAccelTable       — stored on CMultiDocTemplate on Windows
// Both are stubs; we don't do Win32 message translation on macOS.
// These are added inline via template friend below; defined as free stubs.
inline HACCEL MFC_GetDefaultAccelerator() { return nullptr; }

#endif // !_WIN32
