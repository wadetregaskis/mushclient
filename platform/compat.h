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
#include "wx_mfc_hooks.h"   // g_wxhook_* (e.g. CWnd::FlashWindow → flash hook)

// ── File path separator normalization ───────────────────────────────────────
// MUSHclient is a Windows application; its source and a great many plugins embed
// Windows-style backslash path separators (e.g. ".\\quow_plugins\\maps\\foo.db").
// POSIX filesystems treat '\\' as an ordinary filename character, so any such
// path must have its separators translated to '/' before being handed to a
// POSIX file API (sqlite3_open, fopen, the image / sound loaders, …).  Windows
// accepts '/' natively, hence this is a non-Windows-only transformation.
#include <string>
inline std::string PortableFilePath (const char * p)
{
  std::string s (p ? p : "");
  for (std::string::size_type i = 0; i < s.size (); ++i)
    if (s [i] == '\\')
      s [i] = '/';
  return s;
}

// ── Per-user writable app-data directories (macOS / POSIX) ───────────────────
// A macOS .app bundle is read-only, so MUSHclient cannot keep its writable files
// next to the executable (nor rely on the launch CWD) the way the Windows build
// does.  These helpers return — and create on first use — the conventional
// per-user locations, so the same canonical code paths work on every platform:
//
//   Settings (preferences DB, MUSHclient.ini) → ~/Library/Preferences/MUSHclient/
//   Data     (plugins, plugin state, logs,    → ~/Library/Application Support/
//             saved worlds, downloaded media)     MUSHclient/
//
// On non-Apple POSIX the XDG base-directory spec is honoured.  Windows is wholly
// unaffected: it keeps its historical working-directory behaviour and never calls
// these.  Both returned paths end in '/'.
#include <cstdlib>      // getenv, free
#include <sys/stat.h>   // mkdir, stat
#include <unordered_map> // CWnd::FromHandle handle map

// mkdir -p: create each component of a (possibly trailing-slash) absolute path.
inline bool PortableMakeDirs (const std::string & path)
{
  if (path.empty ())
    return false;
  std::string acc;
  for (std::string::size_type i = 0; i < path.size (); ++i)
    {
    acc += path [i];
    if (path [i] == '/' && acc.size () > 1)
      ::mkdir (acc.c_str (), 0755);          // intermediate; ignore EEXIST
    }
  ::mkdir (path.c_str (), 0755);             // final component, if no trailing '/'
  struct stat st;
  return ::stat (path.c_str (), &st) == 0 && S_ISDIR (st.st_mode);
}

// ~/Library/Preferences/MUSHclient/  (macOS)  ·  $XDG_CONFIG_HOME or ~/.config (else)
inline std::string PortableUserSettingsDir ()
{
  const char * home = getenv ("HOME");
  if (!home || !*home)
    return std::string ();                   // caller falls back to a relative path
  std::string dir (home);
#if defined (__APPLE__)
  dir += "/Library/Preferences/MUSHclient/";
#else
  const char * xdg = getenv ("XDG_CONFIG_HOME");
  if (xdg && *xdg) { dir = xdg; if (dir [dir.size () - 1] != '/') dir += '/'; dir += "MUSHclient/"; }
  else            { dir += "/.config/MUSHclient/"; }
#endif
  PortableMakeDirs (dir);
  return dir;
}

// ~/Library/Application Support/MUSHclient/  (macOS)  ·  $XDG_DATA_HOME or ~/.local/share (else)
inline std::string PortableUserDataDir ()
{
  const char * home = getenv ("HOME");
  if (!home || !*home)
    return std::string ();
  std::string dir (home);
#if defined (__APPLE__)
  dir += "/Library/Application Support/MUSHclient/";
#else
  const char * xdg = getenv ("XDG_DATA_HOME");
  if (xdg && *xdg) { dir = xdg; if (dir [dir.size () - 1] != '/') dir += '/'; dir += "MUSHclient/"; }
  else            { dir += "/.local/share/MUSHclient/"; }
#endif
  PortableMakeDirs (dir);
  return dir;
}

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

// ── MFC message-map structures (reactivated for the dialog/command bridge) ──
// Real AFX_MSGMAP tables let CDialog route control commands to the canonical
// ON_BN_CLICKED/ON_COMMAND/... handlers — the chosen "reactivate the maps"
// approach.  See the macros in mfc_ui.h and AfxDispatchCommand().
class CCmdTarget;
typedef void (CCmdTarget::*AFX_PMSG)(void);
enum { AfxSig_end = 0, AfxSig_vv, AfxSig_vw, AfxSig_notify, AfxSig_cmdui };  // _vv = void(); _vw = void(UINT) (ranges); _notify = void(NMHDR*, LRESULT*); _cmdui = void(CCmdUI*)
struct AFX_MSGMAP_ENTRY {
    UINT      nMessage, nCode, nID, nLastID;
    UINT_PTR  nSig;
    AFX_PMSG  pfn;
};
struct AFX_MSGMAP {
    const AFX_MSGMAP* (*pfnGetBaseMap)();
    const AFX_MSGMAP_ENTRY* lpEntries;
};

// ── CCmdTarget ───────────────────────────────────────────────────────────
// Defined here (before CWnd) so CWnd can inherit from it.
// mfc_ui.h used to redefine this class — it now defers to this definition.
class CCmdTarget : public CObject {
    SHIM_RUNTIME_CLASS(CCmdTarget, CObject)
public:
    virtual ~CCmdTarget() {}
    void EnableAutomation()  {}
    void BeginWaitCursor()   {}
    void EndWaitCursor()     {}
    void RestoreWaitCursor() {}
    // Root of the message-map chain (terminates here — no base map).
    static const AFX_MSGMAP* GetThisMessageMap();
    virtual const AFX_MSGMAP* GetMessageMap() const { return GetThisMessageMap(); }
};
inline const AFX_MSGMAP* CCmdTarget::GetThisMessageMap() {
    static const AFX_MSGMAP_ENTRY entries[] = { { 0, 0, 0, 0, AfxSig_end, (AFX_PMSG)0 } };
    static const AFX_MSGMAP map = { nullptr, entries };
    return &map;
}

// ── CWnd ──────────────────────────────────────────────────────────────────
// Base class for all GUI windows.  mfc_ui.h derives many classes from CWnd,
// so it must be defined BEFORE mfc_ui.h is included.
class CWnd : public CCmdTarget {
    SHIM_RUNTIME_CLASS(CWnd, CCmdTarget)
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
    void GetWindowText(CString& s) const {
        if (g_wxhook_get_window_text) {
            char* t = g_wxhook_get_window_text(m_hWnd);
            s = t ? t : ""; if (t) free(t);
        } else s = "";
    }
    int  GetWindowTextLength() const { return 0; }
    void ShowWindow(int nCmd) {
        if (g_wxhook_show_window) g_wxhook_show_window(m_hWnd, nCmd);
    }
    void EnableWindow(BOOL bEnable = TRUE) {
        if (g_wxhook_enable_window) g_wxhook_enable_window(m_hWnd, bEnable != FALSE);
    }
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
    void MoveWindow(int x, int y, int w, int h, BOOL repaint = TRUE) {
        if (g_wxhook_move_window)
            g_wxhook_move_window(m_hWnd, x, y, w, h, repaint != FALSE);
    }
    void MoveWindow(const RECT* r, BOOL repaint = TRUE) {   // CRect* derives from RECT
        if (r && g_wxhook_move_window)
            g_wxhook_move_window(m_hWnd, r->left, r->top,
                                 r->right - r->left, r->bottom - r->top,
                                 repaint != FALSE);
    }
    void SetWindowPos(const CWnd*, int, int, int, int, UINT) {}
    void BringWindowToTop() {}
    void SetRedraw(BOOL = TRUE) {}
    void RedrawWindow(const RECT* = nullptr, CRgn* = nullptr, UINT = 0) {
        // Owner-draw controls (e.g. CColourButton swatches) call RedrawWindow()
        // to repaint after their colour changes; route it to a wx Refresh.
        if (g_wxhook_invalidate) g_wxhook_invalidate(m_hWnd, true);
    }
    CWnd* GetParent() const { return nullptr; }
    CWnd* GetOwner()  const { return nullptr; }
    CWnd* SetParent(CWnd*) { return nullptr; }
    void  MapWindowPoints(CWnd*, RECT*) const {}
    void  MapWindowPoints(CWnd*, POINT*, UINT) const {}
    CWnd* GetDlgItem(int id) const {
        return g_wxhook_get_dlg_item ? CWnd::FromHandle(g_wxhook_get_dlg_item(m_hWnd, id)) : nullptr;
    }
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
    // Real: pushes the CFont's wxFont onto the underlying wx control.  FixFont
    // (Utilities.cpp) relies on this to give trigger/alias/script editors the
    // fixed-pitch font; a no-op here silently disables that whole feature.
    // Defined out-of-line after mfc_ui.h (CFont must be complete).
    void  SetFont(CFont* pFont, BOOL = TRUE);
    CFont* GetFont() const { return nullptr; }
    void  DragAcceptFiles(BOOL = TRUE) {}
    BOOL  SetForegroundWindow() { return FALSE; }
    // Defined out-of-line in mfc_ui.h (WINDOWPLACEMENT is incomplete here).
    // Fills rcNormalPosition with the control's size so GetButtonSize() etc.
    // get correct dimensions for the canonical OnSize relayout.
    BOOL  GetWindowPlacement(WINDOWPLACEMENT* wp) const;
    BOOL  SetWindowPlacement(const WINDOWPLACEMENT*) { return FALSE; }
    void  FlashWindow(BOOL bInvert) {
        // No per-window taskbar flash on macOS/Linux.  On "start flash"
        // (bInvert TRUE) bounce the Dock icon / request user attention via the
        // wx hook; there is no separate "stop" (the request is one-shot).  The
        // only callers are the main frame (activity flash, world.FlashIcon).
        if (bInvert && g_wxhook_flash_icon) g_wxhook_flash_icon();
    }
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

    // Subclassing.  MFC makes `this` the C++ wrapper for the dialog's child
    // control.  Resolve the child, let the wx adapter register it (and possibly
    // hand back a substitute widget — see g_wxhook_subclass_dlg_item), then
    // adopt the handle.  Until this had a body every subclassed control kept
    // m_hWnd == nullptr, so GetWindowText / SetWindowText / Invalidate /
    // MoveWindow / ShowWindow on the subclass object all silently no-opped.
    //
    // Returns FALSE for a null parent or an id the dialog does not have, which
    // matches MFC and keeps the headless build (every hook null) inert.
    BOOL SubclassDlgItem(UINT nID, CWnd* pParent) {
        if (!pParent || !pParent->m_hWnd || !g_wxhook_get_dlg_item) return FALSE;
        HWND h = (HWND) g_wxhook_get_dlg_item(pParent->m_hWnd, (int) nID);
        if (!h) return FALSE;
        if (g_wxhook_subclass_dlg_item)
            if (void* sub = g_wxhook_subclass_dlg_item(pParent->m_hWnd, (int) nID, this))
                h = (HWND) sub;
        m_hWnd = h;
        return TRUE;
    }
    // NOTE: SubclassWindow is still inert.  It has one real caller —
    // mainfrm.cpp:294 (m_wndMDIClient), which returns -1 from OnCreate on
    // failure — reached only on Windows because CMainFrame::OnCreate has an
    // `#ifndef _WIN32` early return above it.  Any future implementation must
    // not regress that path.
    BOOL SubclassWindow(HWND)         { return FALSE; }

    // Default message handling (no-op on non-Windows)
    LRESULT Default() { return 0; }

    // OnCreate — called by framework when window is created
    virtual int  OnCreate(LPCREATESTRUCT) { return 0; }
    virtual void OnDestroy() {}
    virtual BOOL OnEraseBkgnd(CDC*) { return FALSE; }

    // Static handle-to-CWnd map (MFC API stubs)
    // Return a stable CWnd wrapper for a wxWindow* handle.  unordered_map
    // guarantees element pointers survive rehash, so &entry is safe to return.
    // Control classes (CButton/CEdit/…) add no data, so callers may cast the
    // result to those types (MFC's GetDlgItem contract).
    static CWnd* FromHandle(HWND h) {
        if (!h) return nullptr;
        static std::unordered_map<void*, CWnd> s_handleMap;
        CWnd& w = s_handleMap[h];
        w.m_hWnd = h;
        return &w;
    }
    static CWnd* FromHandlePermanent(HWND h) { return FromHandle(h); }
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

// ── CWnd methods that need complete CRect/CFont — defined after mfc_ui.h ──
inline void CWnd::SetFont(CFont* pFont, BOOL) {
    if (g_wxhook_set_font && pFont && pFont->m_impl)
        g_wxhook_set_font(m_hWnd, pFont->m_impl);
}
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
