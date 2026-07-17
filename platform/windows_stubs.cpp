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
#include "MUSHview.h"
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

// ── CAlias / CTrigger / CTimer static CRuntimeClass members ──────────────
// Normally defined by IMPLEMENT_DYNAMIC in doc.cpp (mushapp only).
// xml_load_world_portable.cpp calls GetRuntimeClass() on these types to do
// IsKindOf checks; the static members must exist in mushcore too.
CRuntimeClass CAlias::classCAlias           = { "CAlias" };
CRuntimeClass CTrigger::classCTrigger       = { "CTrigger" };
CRuntimeClass CTimer::classCTimer           = { "CTimer" };

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

// ── CTimerWnd::ChangeTimerRate ────────────────────────────────────────────
// serialize.cpp calls m_pTimerWnd->ChangeTimerRate().
// TimerWnd.h (included via doc.h) declares the real CTimerWnd but
// TimerWnd.cpp is Windows-only; stub here.
void CTimerWnd::ChangeTimerRate(int) {}

// ── CChatSocket method stubs ──────────────────────────────────────────────
// ProcessPreviousLine.cpp calls these.  The real implementations are in
// chatsock.cpp (Windows-only).
void CChatSocket::Process_Snoop   (CString) {}
void CChatSocket::SendChatMessage (int, CString, long) {}

// ── CMUSHclientDoc method stubs (compiled against doc.h) ──────────────────
// All methods listed below are declared in doc.h (the full Windows class)
// but only implemented in doc.cpp / other Windows-only files.  Compiled
// against doc.h their signatures differ from the inline stubs in doc_stub.h,
// which are only visible to TUs that include doc_stub.h.
// Providing out-of-line no-op definitions here satisfies the linker for
// the TUs that include doc.h (evaluate.cpp, ProcessPreviousLine.cpp,
// timers.cpp, serialize.cpp).

// ConnectSocket() is implemented in doc_portable.cpp.

bool CMUSHclientDoc::FindStyle(const CLine* /*pLine*/,
                               int /*iWantedCol*/,
                               int& /*iCol*/,
                               CStyle*& /*pStyle*/,
                               POSITION& /*foundpos*/) const { return false; }

const bool CMUSHclientDoc::CheckScriptingAvailable(const char* /*sWhat*/,
                                                   const DISPID /*dispid*/,
                                                   const char* /*sScript*/)
{ return false; }

// Comparison functions — mirrors doc.cpp exactly (duplicated here because
// doc.cpp is not compiled into mushcore, only into mushapp).
static int CompareTrigger(const void* elem1, const void* elem2)
{
    CTrigger* t1 = *((CTrigger**)elem1);
    CTrigger* t2 = *((CTrigger**)elem2);
    if (t1->iSequence < t2->iSequence) return -1;
    if (t1->iSequence > t2->iSequence) return  1;
    // identical sequence → stable order by match text
    if (t1->trigger < t2->trigger) return -1;
    if (t1->trigger > t2->trigger) return  1;
    return 0;
}

static int CompareAlias(const void* elem1, const void* elem2)
{
    CAlias* a1 = *((CAlias**)elem1);
    CAlias* a2 = *((CAlias**)elem2);
    if (a1->iSequence < a2->iSequence) return -1;
    if (a1->iSequence > a2->iSequence) return  1;
    if (a1->name < a2->name) return -1;
    if (a1->name > a2->name) return  1;
    return 0;
}

// Rebuild the sorted arrays from the maps and update the reverse map.
// Called from methods_triggers.cpp / methods_aliases.cpp after every add/
// delete/change so that evaluate.cpp sees triggers and aliases in the
// correct priority order.
void CMUSHclientDoc::SortTriggers()
{
    int iCount = GetTriggerMap().GetCount();
    GetTriggerArray().SetSize(iCount);
    GetTriggerRevMap().clear();
    CString name;
    CTrigger* pt;
    int i = 0;
    for (POSITION pos = GetTriggerMap().GetStartPosition(); pos; ++i) {
        GetTriggerMap().GetNextAssoc(pos, name, pt);
        GetTriggerArray().SetAt(i, pt);
        GetTriggerRevMap()[pt] = std::string((const char*)name);
    }
    qsort(GetTriggerArray().GetData(), iCount, sizeof(CTrigger*), CompareTrigger);
}

void CMUSHclientDoc::SortAliases()
{
    int iCount = GetAliasMap().GetCount();
    GetAliasArray().SetSize(iCount);
    GetAliasRevMap().clear();
    CString name;
    CAlias* pa;
    int i = 0;
    for (POSITION pos = GetAliasMap().GetStartPosition(); pos; ++i) {
        GetAliasMap().GetNextAssoc(pos, name, pa);
        GetAliasArray().SetAt(i, pa);
        GetAliasRevMap()[pa] = std::string((const char*)name);
    }
    qsort(GetAliasArray().GetData(), iCount, sizeof(CAlias*), CompareAlias);
}

void CMUSHclientDoc::Redraw()       {}

void CMUSHclientDoc::DoFixMenus(CCmdUI* /*pCmdUI*/) {}
void CMUSHclientDoc::ShowStatusLine(bool) {}

// ColourNote(LPCTSTR, LPCTSTR, LPCTSTR) is now implemented in doc_portable.cpp
// with real terminal ANSI colour output (replaces the old no-op stub here).

// OnFileReloaddefaults: REAL body now compiled into mushcore
// (methods_defaults.cpp adoption) — the old empty stub here would be a
// duplicate symbol.

// GetAlphaOptionItem: REAL body now compiled into mushcore (scriptingoptions.cpp,
// option convergence) — the old empty stub here would be a duplicate symbol.

// doc.h: bool SendToAllPluginCallbacks(const string&, const char*, const bool = false)
bool CMUSHclientDoc::SendToAllPluginCallbacks(const string& /*sName*/,
                                              const char*   /*sText*/,
                                              const bool    bStopOnFalse)
{ return bStopOnFalse; }

// doc.h: UINT Load_World_XML(CArchive&, unsigned long, unsigned long, UINT*×8)
UINT CMUSHclientDoc::Load_World_XML(CArchive& /*ar*/,
                                    const unsigned long /*iMask*/,
                                    const unsigned long /*iFlags*/,
                                    UINT* /*piTriggers*/,
                                    UINT* /*piAliases*/,
                                    UINT* /*piTimers*/,
                                    UINT* /*piMacros*/,
                                    UINT* /*piVariables*/,
                                    UINT* /*piColours*/,
                                    UINT* /*piKeypad*/,
                                    UINT* /*piPrinting*/) { return 0; }

// doc.h: void Save_World_XML(CArchive&, unsigned long, CString = "")
void CMUSHclientDoc::Save_World_XML(CArchive& /*ar*/,
                                    const unsigned long /*iMask*/,
                                    CString /*strComment*/) {}

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
void CMainFrame::OnTimer         (UINT)              {}
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
int UMessageBox(const char* sText, UINT nType, const char* caption) {
    if (g_wxhook_message_box)
        return g_wxhook_message_box(sText ? sText : "", caption ? caption : "MUSHclient", nType);
    fprintf(stderr, "MUSHclient [%s]: %s\n", caption ? caption : "MUSHclient", sText ? sText : "");
    return (nType & MB_YESNO) ? IDNO : IDOK;
}
int TMessageBox(const char* sText, UINT nType) {
    return UMessageBox(sText, nType, "MUSHclient");
}

// ── CPlugin destructor ────────────────────────────────────────────────────
// Moved to platform/plugin_portable.cpp — that file includes only doc_stub.h
// and therefore sees the stub CPlugin layout that matches InternalLoadPlugin's
// `new CPlugin(this)` allocation.  Defining it here (which sees the full
// plugins.h CPlugin via doc.h) would cause a struct-size mismatch and crash.

// ── Stubs for lua_utils_portable.cpp ─────────────────────────────────────
// These symbols are defined in mushapp-only files (mainfrm.cpp,
// scripting/methods/methods_info.cpp, scripting/functionlist.cpp, plugins.cpp).
// Provide empty sentinel-terminated tables so mushcore links and the
// corresponding utils.* Lua functions return empty tables rather than crashing.
bool bShowDebugStatus = false;                              // mainfrm.cpp owns the real one

// tInfoTypeMapping and tInternalFunctionsTable are typedef'd in doc.h / stdafx.h
// (both included above via stdafx.h → doc.h).  Sentinel for InfoTypes: iInfoType==0.
// Sentinel for InternalFunctionsTable: sFunction[0]=='\0' (empty string literal).
tInfoTypeMapping           InfoTypes[]              = { {0, nullptr} };
tInternalFunctionsTable    InternalFunctionsTable[] = { {"", ""} };

// ── PluginCallbacksNames — full list of valid plugin callback names ────────
// On Windows this is defined in plugins.cpp.  mushcore needs it so that
// utils.PluginCallbacks() returns the correct set to plugin authors.
// Keep in sync with plugins.cpp.  Sentinel: empty string at end.
std::string PluginCallbacksNames[] = {
    "OnPluginBroadcast",
    "OnPluginChatAccept",
    "OnPluginChatDisplay",
    "OnPluginChatMessage",
    "OnPluginChatMessageOut",
    "OnPluginChatNewUser",
    "OnPluginChatUserDisconnect",
    "OnPluginClose",
    "OnPluginCommand",
    "OnPluginCommandChanged",
    "OnPluginCommandEntered",
    "OnPluginConnect",
    "OnPluginDisable",
    "OnPluginDisconnect",
    "OnPluginDrawOutputWindow",
    "OnPluginEnable",
    "OnPluginGetFocus",
    "OnPlugin_IAC_GA",
    "OnPluginInstall",
    "OnPluginLineReceived",
    "OnPluginListChanged",
    "OnPluginLoseFocus",
    "OnPluginMouseMoved",
    "OnPluginMXPcloseTag",
    "OnPluginMXPerror",
    "OnPluginMXPopenTag",
    "OnPluginMXPsetEntity",
    "OnPluginMXPsetVariable",
    "OnPluginMXPstart",
    "OnPluginMXPstop",
    "OnPluginPacketReceived",
    "OnPluginPartialLine",
    "OnPluginPlaySound",
    "OnPluginSaveState",
    "OnPluginScreendraw",
    "OnPluginSelectionChanged",
    "OnPluginSend",
    "OnPluginSent",
    "OnPluginTabComplete",
    "OnPluginTelnetOption",
    "OnPluginTelnetRequest",
    "OnPluginTelnetSubnegotiation",
    "OnPluginTick",
    "OnPluginTrace",
    "OnPluginPacketDebug",
    "OnPluginWorldOutputResized",
    "OnPluginWorldSave",
    ""   // sentinel
};

#endif // !_WIN32
