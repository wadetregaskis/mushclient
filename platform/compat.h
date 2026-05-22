// platform/compat.h
// Master cross-platform compatibility header.
//
// On Windows this is a no-op (the real MFC headers are used).
// On macOS / Linux this replaces the MFC infrastructure with portable stubs.
//
// Include order matters:
//   windows_types.h  — primitive types (BOOL, COLORREF, HANDLE, …)
//   mfc_object.h     — CObject base class + DECLARE_DYNAMIC macros
//   mfc_string.h     — CString
//   mfc_collections.h — CTypedPtrList / CTypedPtrMap / CTypedPtrArray
//   mfc_exceptions.h — CException hierarchy
//   mfc_time.h       — CTime
//   mfc_ui.h         — MFC GUI class stubs (CDocument, CView, CDialog, CDC, …)

#pragma once

#ifndef _WIN32

#include "windows_types.h"
#include "mfc_object.h"
#include "mfc_string.h"
#include "mfc_collections.h"
#include "mfc_exceptions.h"
#include "mfc_time.h"

// ── CFormat ────────────────────────────────────────────────────────────────
// CFormat is declared in format.h (included via stdafx.h) and defined in
// format.cpp. On non-Windows it inherits from CString just like on Windows.
// We do NOT define it inline here — format.cpp provides the constructors.

// ── Miscellaneous macros missing from the stub headers ────────────────────

// _T() / TEXT() — in MultiByte builds these are no-ops
#ifndef _T
#  define _T(x) x
#endif
#ifndef TEXT
#  define TEXT(x) x
#endif

// String-length helpers
#ifndef lstrlen
#  define lstrlen strlen
#endif
#ifndef lstrcmp
#  define lstrcmp strcmp
#endif
#ifndef lstrcmpi
#  define lstrcmpi strcasecmp
#endif
#ifndef lstrcpy
#  define lstrcpy strcpy
#endif

// UNREFERENCED_PARAMETER — suppress unused-variable warnings
#ifndef UNREFERENCED_PARAMETER
#  define UNREFERENCED_PARAMETER(x) ((void)(x))
#endif

// min / max — avoid conflicts with std::min/max
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef __min
#  define __min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef __max
#  define __max(a,b) ((a)>(b)?(a):(b))
#endif

// ── Global app pointer — set by CWinApp constructor ──────────────────────
// AfxGetApp() returns this.  Defined as an inline variable (C++17) so it
// has exactly one storage location across all TUs without a separate .cpp.
// The CWinApp constructor (in mfc_ui.h) sets this to 'this' on construction.
class CWinApp;
class CWnd;
inline CWinApp* g_pWinApp  = nullptr;
inline CWinApp* AfxGetApp() { return g_pWinApp; }
// AfxGetMainWnd is defined in mfc_ui.h after CWinApp is fully declared,
// so it can access m_pMainWnd.  Forward-declare the stub here.
inline CWnd* AfxGetMainWnd();
// AfxGetInstanceHandle is defined in mfc_ui.h (returns HINSTANCE)

// ── System beep hook ─────────────────────────────────────────────────────
// Called by the MessageBeep() stub below.  The wxWidgets GUI layer sets this
// to wxBell() in wx_mfc_impl.cpp; other builds leave it nullptr (silent).
// Declared as a C++17 inline variable so every TU shares exactly one copy.
inline void (*g_fn_system_beep)() = nullptr;

// ── wx MFC hook function pointers (declared before CWnd uses them) ────────
#include "wx_mfc_hooks.h"

// ── Forward declarations needed by CWnd ──────────────────────────────────
// These classes are fully defined in mfc_ui.h; forward-declare here so
// CWnd can use them as pointer parameters.
struct WINDOWPLACEMENT;   // fully defined in mfc_ui.h
struct MSG;               // fully defined in mfc_ui.h
struct CREATESTRUCT;      // fully defined in mfc_ui.h
typedef CREATESTRUCT* LPCREATESTRUCT;
struct CRuntimeClass;
class CFont;
class CMenu;
class CRgn;
class CPoint;
class CRect;
class CSize;
class CScrollBar;
class CDC;    // fully defined in mfc_ui.h; forward-declare so CWnd can use CDC*

// ── CCmdTarget ───────────────────────────────────────────────────────────
// Defined here (before CWnd) so CWnd can inherit from it.
// mfc_ui.h used to redefine this class — it now defers to this definition.
class CCmdTarget : public CObject {
public:
    virtual ~CCmdTarget() {}
    void EnableAutomation()  {}
    void BeginWaitCursor()   {}
    void EndWaitCursor()     {}
    void RestoreWaitCursor() {}
};

// ── CWnd ──────────────────────────────────────────────────────────────────
// Base class for all GUI windows.  mfc_ui.h derives many classes from CWnd,
// so it must be defined BEFORE mfc_ui.h is included.
class CWnd : public CCmdTarget {
public:
    virtual ~CWnd() {}
    void* GetSafeHwnd()  const { return nullptr; }
    bool  IsWindow()     const { return false; }
    HWND  m_hWnd         = nullptr;

    // Message routing
    LRESULT SendMessage(UINT m, WPARAM w = 0, LPARAM l = 0) {
        if (g_wxhook_send_message) return g_wxhook_send_message(m_hWnd, m, (uintptr_t)w, (intptr_t)l);
        return 0;
    }
    BOOL PostMessage(UINT m, WPARAM w = 0, LPARAM l = 0) {
        if (g_wxhook_post_message) return g_wxhook_post_message(m_hWnd, m, (uintptr_t)w, (intptr_t)l);
        return FALSE;
    }

    // Window state
    void Invalidate(BOOL erase = TRUE) {
        if (g_wxhook_invalidate) g_wxhook_invalidate(m_hWnd, erase != FALSE);
    }
    void InvalidateRect(const RECT* r, BOOL erase = TRUE) {
        (void)r;
        if (g_wxhook_invalidate) g_wxhook_invalidate(m_hWnd, erase != FALSE);
    }
    void UpdateWindow() {
        if (g_wxhook_update_window) g_wxhook_update_window(m_hWnd);
    }
    void SetWindowText(const char* text) {
        if (g_wxhook_set_window_text) g_wxhook_set_window_text(m_hWnd, text);
    }
    void GetWindowText(CString&) const {}
    int  GetWindowTextLength() const { return 0; }
    void ShowWindow(int nCmd) {
        if (g_wxhook_show_window) g_wxhook_show_window(m_hWnd, nCmd);
    }
    void EnableWindow(BOOL = TRUE) {}
    BOOL IsWindowEnabled() const { return TRUE; }
    BOOL IsWindowVisible() const { return m_hWnd != nullptr; }
    void SetFocus() {
        if (g_wxhook_set_focus) g_wxhook_set_focus(m_hWnd);
    }
    void GetClientRect(RECT* r) const {
        if (g_wxhook_get_client_rect) g_wxhook_get_client_rect(m_hWnd, r);
    }
    void GetClientRect(CRect*) const {}
    void GetWindowRect(RECT*) const {}
    void GetWindowRect(CRect*) const {}
    void MoveWindow(int, int, int, int, BOOL = TRUE) {}
    void MoveWindow(const RECT*) {}
    void MoveWindow(const RECT*, BOOL) {}   // CRect* overload (CRect derives from RECT)
    void SetWindowPos(const CWnd*, int, int, int, int, UINT) {}
    void BringWindowToTop() {}
    void SetRedraw(BOOL = TRUE) {}
    void RedrawWindow(const RECT* = nullptr, CRgn* = nullptr, UINT = 0) {}
    CWnd* GetParent() const { return nullptr; }
    CWnd* GetOwner()  const { return nullptr; }
    CWnd* SetParent(CWnd*) { return nullptr; }
    void  MapWindowPoints(CWnd*, RECT*) const {}
    void  MapWindowPoints(CWnd*, POINT*, UINT) const {}
    CWnd* GetDlgItem(int) const { return nullptr; }
    UINT  IsDlgButtonChecked(int) const { return 0; }
    BOOL  CheckDlgButton(int, UINT) { return FALSE; }
    CWnd* SetCapture() { return nullptr; }
    static CWnd* ReleaseCapture() { return nullptr; }
    CWnd* GetFocus() { return nullptr; }
    CWnd* GetWindow(UINT) const { return nullptr; }
    UINT  GetDlgCtrlID() const { return 0; }

    // Timer
    UINT_PTR SetTimer(UINT nIDEvent, UINT nElapse, void* = nullptr) { return 0; }
    BOOL     KillTimer(UINT_PTR) { return FALSE; }

    // Scroll
    void SetScrollPos(int, int, BOOL = TRUE) {}
    int  GetScrollPos(int) const { return 0; }
    void SetScrollRange(int, int, int, BOOL = TRUE) {}
    void EnableScrollBar(int, UINT = 0) {}
    void ShowScrollBar(UINT, BOOL = TRUE) {}
    void ScrollWindow(int, int, const RECT* = nullptr, const RECT* = nullptr) {}

    // DC
    CDC*  GetDC() { return nullptr; }
    void  ReleaseDC(CDC*) {}

    // Menu
    CMenu* GetMenu() const { return nullptr; }
    BOOL   SetMenu(HMENU) { return FALSE; }
    CMenu* GetSystemMenu(BOOL) const { return nullptr; }
    void  DrawMenuBar() {}

    // Cursor / icon
    static HCURSOR SetCursor(HCURSOR) { return nullptr; }

    // Window update locking (no-op on non-Windows)
    BOOL LockWindowUpdate()   { return FALSE; }
    void UnlockWindowUpdate() {}

    // Misc
    void  ModifyStyle(DWORD, DWORD, UINT = 0) {}
    void  ModifyStyleEx(DWORD, DWORD, UINT = 0) {}
    DWORD GetStyle() const { return 0; }
    DWORD GetExStyle() const { return 0; }
    void  ScreenToClient(POINT*) const {}
    void  ClientToScreen(POINT*) const {}
    void  ScreenToClient(RECT*) const {}
    void  ClientToScreen(RECT*) const {}
    void  ScreenToClient(CPoint* p) const { (void)p; }
    void  ClientToScreen(CPoint* p) const { (void)p; }
    void  ScreenToClient(CRect* r) const { (void)r; }
    void  ClientToScreen(CRect* r) const { (void)r; }
    // CRect& overloads — declared here, defined after mfc_ui.h (CRect must be complete)
    void  GetClientRect(CRect& r) const;
    void  ClientToScreen(CRect& r) const;
    void  ScreenToClient(CRect& r) const;
    BOOL  CreateEx(DWORD, LPCTSTR, LPCTSTR, DWORD, int, int, int, int, HWND, HMENU, LPVOID = nullptr) { return FALSE; }
    void  DestroyWindow() {
        if (g_wxhook_destroy_window) g_wxhook_destroy_window(m_hWnd);
        m_hWnd = nullptr;
    }
    void  CenterWindow(CWnd* = nullptr) {}
    void  SetFont(CFont*, BOOL = TRUE) {}
    CFont* GetFont() const { return nullptr; }
    void  DragAcceptFiles(BOOL = TRUE) {}
    BOOL  SetForegroundWindow() { return FALSE; }
    BOOL  GetWindowPlacement(WINDOWPLACEMENT*) const { return FALSE; }
    BOOL  SetWindowPlacement(const WINDOWPLACEMENT*) { return FALSE; }
    void  FlashWindow(BOOL) {}
    void  MessageBox(LPCTSTR, LPCTSTR = nullptr, UINT = MB_OK) {}
    // IsKindOf inherited from CObject (returns int/BOOL, always 0)
    BOOL  IsIconic() const { return FALSE; }
    BOOL  IsZoomed() const { return FALSE; }
    // Implicit conversion to HWND — MFC CWnd supports this.
    operator HWND() const { return m_hWnd; }

    // GetSafeOwner — static variant (used as CWnd::GetSafeOwner(pParent))
    static CWnd* GetSafeOwner(CWnd* pParent, HWND* = nullptr) { return pParent; }

    // IsDialogMessage — member variant (single-arg form used in dialog pump loops)
    BOOL IsDialogMessage(MSG*) { return FALSE; }

    // Subclassing
    BOOL SubclassDlgItem(UINT, CWnd*) { return FALSE; }
    BOOL SubclassWindow(HWND)         { return FALSE; }

    // Default message handling (no-op on non-Windows)
    LRESULT Default() { return 0; }

    // OnCreate — called by framework when window is created
    virtual int  OnCreate(LPCREATESTRUCT) { return 0; }
    virtual void OnDestroy() {}
    virtual BOOL OnEraseBkgnd(CDC*) { return FALSE; }

    // Static handle-to-CWnd map (MFC API stubs)
    static CWnd* FromHandle(HWND)            { return nullptr; }
    static CWnd* FromHandlePermanent(HWND)   { return nullptr; }
    static CWnd* GetDesktopWindow()          { return nullptr; }
    static CWnd* GetForegroundWindow()       { return nullptr; }

    // Static sentinel windows used with SetWindowPos.
    // Declared here, defined inline after the class (type must be complete).
    static const CWnd wndTop;
    static const CWnd wndBottom;
    static const CWnd wndTopMost;
    static const CWnd wndNoTopMost;
};
// Define the CWnd sentinel statics now that the type is complete.
inline const CWnd CWnd::wndTop;
inline const CWnd CWnd::wndBottom;
inline const CWnd CWnd::wndTopMost;
inline const CWnd CWnd::wndNoTopMost;

// ── CEvent stub ────────────────────────────────────────────────────────────
class CEvent {
public:
    CEvent(BOOL = FALSE, BOOL = FALSE) {}
    void SetEvent()   {}
    void ResetEvent() {}
};

// ── MFC precompiled-header sentinel ───────────────────────────────────────
// MUSHclient.h guards itself with #ifndef __AFXWIN_H__ (set by afxwin.h on
// Windows).  Define it here so the guard passes on non-Windows builds.
#define __AFXWIN_H__

// ── CAsyncSocket — full POSIX implementation ──────────────────────────────
#include "platform/posix_socket.h"

// ── mfc_ui.h — MFC GUI class stubs (CDocument, CView, CDialog, CDC, …) ───
// Must come AFTER CWnd (above) because it derives many classes from CWnd.
#include "mfc_ui.h"

// ── CWnd methods that need complete CRect — defined after mfc_ui.h ────────
inline void CWnd::GetClientRect(CRect& r) const { r.left=r.top=r.right=r.bottom=0; }
inline void CWnd::ClientToScreen(CRect& r) const { (void)r; }
inline void CWnd::ScreenToClient(CRect& r) const { (void)r; }

// ── #pragma warning equivalents ───────────────────────────────────────────
// MSVC pragmas that appear in the source — silently ignore on clang/gcc
#define BASED_CODE  /* empty */

// ── POSIX replacements for Windows CRT directory functions ────────────────
// <direct.h> on Windows provides _chdir, _getcwd, _mkdir, _rmdir.
// On POSIX these live in <unistd.h> and <sys/stat.h> without the underscore.
// Define the Windows names as macros so all call sites compile unchanged.
#include <unistd.h>
#include <sys/stat.h>
#ifndef _chdir
#  define _chdir  chdir
#endif
#ifndef _getcwd
#  define _getcwd getcwd
#endif
#ifndef _mkdir
#  define _mkdir(p)  mkdir((p), 0777)
#endif
#ifndef _rmdir
#  define _rmdir  rmdir
#endif

// ── POSIX replacements for Windows CRT file-stat / IO functions ───────────
// <sys/stat.h> on Windows uses _stat / _fstat; POSIX uses stat / fstat.
// _fileno and _isatty are likewise POSIX fileno / isatty.
#ifndef _stat
#  define _stat   stat
#endif
#ifndef _fstat
#  define _fstat  fstat
#endif
#ifndef _fileno
#  define _fileno fileno
#endif
#ifndef _isatty
#  define _isatty isatty
#endif

#endif // !_WIN32
