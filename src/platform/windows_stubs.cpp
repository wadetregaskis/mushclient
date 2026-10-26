// platform/windows_stubs.cpp — non-Windows stub implementations for Windows-
// only classes that are referenced from cross-platform files compiled into
// mushcore (ProcessPreviousLine.cpp, timers.cpp, serialize.cpp, evaluate.cpp).
//
// Build: compiled only on non-Windows (guarded by #ifndef _WIN32).
// Every definition here corresponds to a method or static data member that
// lives in a Windows-only .cpp file (MUSHview.cpp, sendvw.cpp, TimerWnd.cpp,
// chatsock.cpp, paneline.cpp, mainfrm.cpp) and would otherwise produce
// "undefined reference" linker errors.

#ifndef _WIN32

// Pull in the full MFC stub types (CMainFrame, CView, CMDIChildWnd, etc.)
// in the same order that ProcessPreviousLine.cpp uses, so that CMUSHView and
// CSendView are fully defined (including their DECLARE_DYNCREATE static members).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "mushview.h"
#include "sendvw.h"
#include "mainfrm.h"
#include "paneline.h"

// ── CMainFrame global (extern-declared in mainfrm_stub.h) ─────────────────
// timers.cpp and serialize.cpp reference Frame directly; doc_portable.cpp
// redirects its own Frame uses to g_stub_frame via a macro.
CMainFrame Frame;

// ── CMUSHView static CRuntimeClass ────────────────────────────────────────
// Normally defined by IMPLEMENT_DYNCREATE(CMUSHView, CView) in MUSHview.cpp.
// ProcessPreviousLine.cpp uses RUNTIME_CLASS(CMUSHView) for IsKindOf checks
// (which always return FALSE on non-Windows, so the value doesn't matter).
CRuntimeClass CMUSHView::classCMUSHView = { "CMUSHView" };

// ── CSendView static CRuntimeClass + method ───────────────────────────────
// Normally defined in sendvw.cpp.
CRuntimeClass CSendView::classCSendView = { "CSendView" };

// B-3: CAlias/CTrigger/CTimer classCXxx CRuntimeClass members now come from
// doc.cpp's IMPLEMENT_DYNAMIC (adopted into mushcore) — the stub copies here
// would be duplicate symbols, so they are gone.

// ── CMDIClientWnd / CMainFrame static CRuntimeClass members ──────────────
// Normally defined by IMPLEMENT_DYNAMIC in Windows-only MFC source files.
// mainfrm_stub.h / MDIClientWnd.h use DECLARE_DYNAMIC which generates
// GetRuntimeClass() returning &classCXxx, so the statics must be defined.
CRuntimeClass CMDIClientWnd::classCMDIClientWnd = { "CMDIClientWnd" };
CRuntimeClass CMainFrame::classCMainFrame       = { "CMainFrame" };

void CSendView::CheckForSelectionChange()
{
    // no-op: timers.cpp calls this to update the send-view selection state,
    // which has no meaning on non-Windows.
}

// ── CMainFrame ctor / dtor ────────────────────────────────────────────────
// mainfrm_stub.h declares CMainFrame() and ~CMainFrame() but provides no
// inline bodies.  Provide trivial out-of-line definitions here so the Frame
// global (defined above) can be constructed/destructed.
CMainFrame::CMainFrame()  {}
CMainFrame::~CMainFrame() {}

// ── CMainFrame method stubs needed by mushcore ────────────────────────────
// timers.cpp calls Frame.SetStatusMessage(); serialize.cpp calls SetStatusMessageNow().
// On non-Windows, forward to the wxStatusBar via the hook pointer.
#include "platform/wx_mfc_hooks.h"
void CMainFrame::SetStatusMessage    (const CString& msg) {
    if (g_wxhook_set_status)
        g_wxhook_set_status(static_cast<const char*>(msg));
}
void CMainFrame::SetStatusMessageNow (const CString& msg) {
    if (g_wxhook_set_status)
        g_wxhook_set_status(static_cast<const char*>(msg));
}
// Doc-attributed overloads (port-only): a per-world message goes to that
// world's OWN status bar, not whichever window happens to be frontmost.
// Falls back to the plain route when the attributed hook is absent
// (headless, or a null doc).
void CMainFrame::SetStatusMessage    (CMUSHclientDoc* doc, const CString& msg) {
    if (doc && g_wxhook_set_status_for)
        g_wxhook_set_status_for(doc, static_cast<const char*>(msg));
    else if (g_wxhook_set_status)
        g_wxhook_set_status(static_cast<const char*>(msg));
}
void CMainFrame::SetStatusMessageNow (CMUSHclientDoc* doc, const CString& msg) {
    SetStatusMessage(doc, msg);
}

// ── CTimerWnd::ChangeTimerRate ────────────────────────────────────────────
// serialize.cpp calls m_pTimerWnd->ChangeTimerRate().
// TimerWnd.h (included via doc.h) declares the real CTimerWnd but
// TimerWnd.cpp is Windows-only; stub here.
void CTimerWnd::ChangeTimerRate(int) {}

// ── CChatSocket: NO LONGER STUBBED ────────────────────────────────────────
// Eight definitions stood here (ctor, dtor, classCChatSocket, the four
// CAsyncSocket virtual overrides, Process_Snoop and SendChatMessage).
// chatsock.cpp is compiled into mushcore now and supplies all of them —
// classCChatSocket comes from its IMPLEMENT_DYNAMIC.
//
// SendChatMessage being a no-op here is why tests/test_display.cpp's snoop test
// could only ever be a coverage test: its own comment conceded the bytes were
// unobservable "because the port's chat send path does not go through ::send
// here".  It goes through the real one now.

// ── CMUSHclientDoc method stubs (compiled against doc.h) ──────────────────
// All methods listed below are declared in doc.h (the full Windows class)
// but only implemented in doc.cpp / other Windows-only files.  Compiled
// against doc.h their signatures differ from the retired headless stub's inline
// bodies (which were only visible to TUs that included that stub).
// Providing out-of-line no-op definitions here satisfies the linker for
// the TUs that include doc.h (evaluate.cpp, ProcessPreviousLine.cpp,
// timers.cpp, serialize.cpp).

// ConnectSocket() is implemented in doc_port_glue.cpp.

// B-3: FindStyle, CheckScriptingAvailable, SortTriggers, SortAliases (+ their
// CompareTrigger/CompareAlias helpers), DoFixMenus, ShowStatusLine are now the
// REAL bodies in the adopted doc.cpp — the stub copies here were duplicate
// symbols, so they are gone.  Redraw() has joined them: methods_output.cpp:50 is
// in mushcore now, and it is the same one line this twin had been corrected to
// earlier in the adoption (UpdateAllViews (NULL)) — so the twin is redundant
// rather than wrong, and goes.

// ColourNote(LPCTSTR, LPCTSTR, LPCTSTR) is now implemented in doc_portable.cpp
// with real terminal ANSI colour output (replaces the old no-op stub here).

// OnFileReloaddefaults: REAL body now compiled into mushcore
// (methods_defaults.cpp adoption) — the old empty stub here would be a
// duplicate symbol.

// GetAlphaOptionItem: REAL body now compiled into mushcore (scriptingoptions.cpp,
// option convergence) — the old empty stub here would be a duplicate symbol.

// SendToAllPluginCallbacks (all overloads): REAL bodies now compiled into
// mushcore (plugins.cpp, CPlugin convergence) — the old return-the-flag stub
// here would be a duplicate symbol.

// Load_World_XML: REAL loader now compiled into mushcore
// (xml/xml_load_world.cpp adoption) — the old no-op body here would be a
// duplicate symbol.  (Save_World_XML has been REAL since the saver adoption.)

// ── Macro / keypad display-name tables ────────────────────────────────────
// On Windows (and mushapp) these live in dialogs/world_prefs/
// prefspropertypages.cpp:1955/2034 — a GUI TU mushcore doesn't compile.
// Save_Macros_XML / Save_Keypad_XML (xml/xml_save_world.cpp) and ExportXML
// (scripting/methods/methods_xml.cpp) need them headless; the initializers
// are copied verbatim so saved world files carry identical names.
CString strMacroDescriptions [MACRO_COUNT] = {
     "up",
     "down",
     "north",
     "south",
     "east",
     "west",
     "examine",
     "look",
     "page",
     "say",
     "whisper",
     "doing",
     "who",
     "drop",
     "take",
     "F2",
     "F3",
     "F4",
     "F5",
     "F7",
     "F8",
     "F9",
     "F10",
     "F11",
     "F12",
     "F2+Shift",
     "F3+Shift",
     "F4+Shift",
     "F5+Shift",
     "F6+Shift",
     "F7+Shift",
     "F8+Shift",
     "F9+Shift",
     "F10+Shift",
     "F11+Shift",
     "F12+Shift",
     "F2+Ctrl",
     "F3+Ctrl",
     "F5+Ctrl",
     "F7+Ctrl",
     "F8+Ctrl",
     "F9+Ctrl",
     "F10+Ctrl",
     "F11+Ctrl",
     "F12+Ctrl",
     "logout",
     "quit",

     // new ones in v 3.42
     "Alt+A",
     "Alt+B",
     "Alt+J",
     "Alt+K",
     "Alt+L",
     "Alt+M",
     "Alt+N",
     "Alt+O",
     "Alt+P",
     "Alt+Q",
     "Alt+R",
     "Alt+S",
     "Alt+T",
     "Alt+U",
     "Alt+X",
     "Alt+Y",
     "Alt+Z",

     // new ones in v 3.44

     "F1",
     "F1+Ctrl",
     "F1+Shift",
     "F6",
     "F6+Ctrl"

     };

CString strKeypadNames [eKeypad_Max_Items] =
  {
  // normal
  "0",
  "1",
  "2",
  "3",
  "4",
  "5",
  "6",
  "7",
  "8",
  "9",
  ".",
  "/",
  "*",
  "-",
  "+",
  // ctrl
  "Ctrl+0",
  "Ctrl+1",
  "Ctrl+2",
  "Ctrl+3",
  "Ctrl+4",
  "Ctrl+5",
  "Ctrl+6",
  "Ctrl+7",
  "Ctrl+8",
  "Ctrl+9",
  "Ctrl+.",
  "Ctrl+/",
  "Ctrl+*",
  "Ctrl+-",
  "Ctrl++"
  };

// ── CMainFrame virtual-override stubs ─────────────────────────────────────
// mainfrm_stub.h declares these as overrides of CMDIFrameWnd virtuals but
// provides no inline bodies.  Without out-of-line definitions the vtable for
// CMainFrame has dangling entries, causing link errors when windows_stubs.cpp.o
// is pulled into a link that constructs Frame (the global CMainFrame instance).
int  CMainFrame::OnCreate        (CREATESTRUCT*)    { return 0;     }
void CMainFrame::OnDestroy       ()                  {}
void CMainFrame::OnClose         ()                  {}
void CMainFrame::OnSize          (UINT, int, int)    {}
void CMainFrame::OnActivate      (UINT, CWnd*, BOOL) {}
void CMainFrame::OnTimer         (UINT_PTR)          {}
void CMainFrame::OnSysCommand    (UINT, LPARAM)      {}
BOOL CMainFrame::PreTranslateMessage (MSG*)          { return FALSE; }
BOOL CMainFrame::PreCreateWindow     (CREATESTRUCT&) { return TRUE;  }
LRESULT CMainFrame::WindowProc   (UINT, WPARAM, LPARAM) { return 0; }
void CMainFrame::OnUpdateFrameMenu  (HMENU)          {}
void CMainFrame::OnUpdateFrameTitle (BOOL)           {}

// ── CMDIClientWnd ctor / dtor ─────────────────────────────────────────────
// MDIClientWnd.h declares explicit (non-inline) ctor/dtor.  The real
// CMDIClientWnd.cpp is not compiled on non-Windows; stub here.
CMDIClientWnd::CMDIClientWnd()  {}
CMDIClientWnd::~CMDIClientWnd() {}
BOOL CMDIClientWnd::OnEraseBkgnd(CDC*) { return FALSE; }

// ── CMyToolBar::DoPaint ───────────────────────────────────────────────────
// mainfrm_stub.h declares DoPaint() virtual with no body; the vtable entry
// must be satisfied even though no window is ever painted on non-Windows.
void CMyToolBar::DoPaint(CDC*) {}

// ── Stub message maps ─────────────────────────────────────────────────────
// CMainFrame / CMyToolBar / CMDIClientWnd each DECLARE_MESSAGE_MAP(); their real
// maps live in mainfrm.cpp, which is mushapp-only.  Their vtables are emitted in
// this translation unit (it defines the global Frame, the ctors, DoPaint, …), so
// the virtual GetMessageMap() must be defined or mushcore-linked binaries
// (the unit tests) fail to link.  These windows are inert in the
// headless build, so an empty map suffices — chain it to CCmdTarget, the only
// ancestor whose GetThisMessageMap() is defined on the port (compat.h).
BEGIN_MESSAGE_MAP(CMainFrame, CCmdTarget)
END_MESSAGE_MAP()
BEGIN_MESSAGE_MAP(CMyToolBar, CCmdTarget)
END_MESSAGE_MAP()
BEGIN_MESSAGE_MAP(CMDIClientWnd, CCmdTarget)
END_MESSAGE_MAP()

// ── UMessageBox / TMessageBox ─────────────────────────────────────────────
// Route through g_wxhook_message_box when it is set (mushgui at runtime);
// fall back to stderr for CLI / headless test builds.
// The literals in HeadlessMessageBoxAnswer (platform/wx_mfc_hooks.h, which
// cannot see the shim) must be the real ID* values; same for
// ApplyDefaultMessageBoxIcon and the MB_* values (Utilities.cpp's Windows
// UMessageBox default-icon fill, reproduced here -- see that helper's comment).
static_assert (IDOK == 1 && IDCANCEL == 2 && IDABORT == 3 && IDNO == 7,
               "ID* constants moved: fix HeadlessMessageBoxAnswer");
static_assert (MB_OK == 0 && MB_OKCANCEL == 1 && MB_YESNOCANCEL == 3 &&
               MB_YESNO == 4 && MB_ICONEXCLAMATION == 0x30,
               "MB_* constants moved: fix ApplyDefaultMessageBoxIcon");

// The dispatch, with no icon fill -- what AfxMessageBox routes to (see
// mfc_exceptions.h for why AfxMessageBox must NOT go through UMessageBox).
int UMessageBoxRaw(const char* sText, UINT nType, const char* caption) {
    if (g_wxhook_message_box)
        return g_wxhook_message_box(sText ? sText : "", caption ? caption : "MUSHclient", nType);
    fprintf(stderr, "MUSHclient [%s]: %s\n", caption ? caption : "MUSHclient", sText ? sText : "");
    return HeadlessMessageBoxAnswer(nType);
}
int UMessageBox(const char* sText, UINT nType, const char* caption) {
    return UMessageBoxRaw(sText, ApplyDefaultMessageBoxIcon(nType), caption);
}
int TMessageBox(const char* sText, UINT nType) {
    return UMessageBox(sText, nType, "MUSHclient");
}

// ── CPlugin ───────────────────────────────────────────────────────────────
// mushcore includes the real plugins.h (CPlugin convergence), so every
// mushcore TU sees the one true CPlugin layout; the ctor/dtor and the
// ExecutePluginScript family come from plugins.cpp (also in mushcore).
// platform/plugin_portable.cpp (the old stub destructor) is deleted.

// ── Stubs for lua_utils_portable.cpp ─────────────────────────────────────
// These symbols are defined in mushapp-only files (mainfrm.cpp,
// scripting/functionlist.cpp, plugins.cpp).  Provide empty
// sentinel-terminated tables so mushcore links and the corresponding
// utils.* Lua functions return empty tables rather than crashing.
bool bShowDebugStatus = false;                              // mainfrm.cpp owns the real one

// InfoTypes[]: the REAL table now comes from methods_info.cpp (adopted into
// mushcore) — the old {0,nullptr} sentinel here would be a duplicate symbol.

// tInternalFunctionsTable is typedef'd in stdafx.h (included above).
// Sentinel: sFunction[0]=='\0' (empty string literal).  The real table is in
// scripting/functionlist.cpp (mushapp-only: it drives the GUI function-list
// dialog); headless world.Help's function-name lookup just finds nothing.
tInternalFunctionsTable    InternalFunctionsTable[] = { {"", ""} };

// error_descriptions[]: the REAL table now comes from scripting/errors.cpp
// (single-sourced, compiled into every build) — the old {0,NULL} sentinel
// here would be a duplicate symbol.  The adopted methods_info.cpp ErrorDesc
// returns the full Windows descriptions headless.

// ── os_version (stdafx.h:406; MUSHclient.cpp:77 owns the GUI one) ─────────
// Zeroed headless: GetInfo(77) → "", GetInfo(265-268) → 0.
OSVERSIONINFO os_version;

// ── Name generation (stdafx.h:395-396; NameGeneration.cpp owns the real
// bodies — mushapp-only, they read the names files with progress UI).
// Headless: bNamesRead stays false so world.GenerateName returns VT_NULL,
// and ReadNames throws so world.ReadNamesFile returns eCouldNotOpenFile.
bool bNamesRead = false;
void ReadNames (const LPCTSTR /*sName*/, const bool /*bNoDialog*/)
{
    throw new CFileException();   // caught (and Delete()d) by ReadNamesFile
}
CString GenerateName (void)
{
    return "";   // unreachable via world.GenerateName (bNamesRead is false)
}

// PluginCallbacksNames[] now comes from plugins.cpp (compiled into mushcore
// since the CPlugin convergence) — the keep-in-sync duplicate that lived here
// is gone.

#endif // !_WIN32
