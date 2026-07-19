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
// DECLARE_DYNAMIC on these types generates GetRuntimeClass() returning
// &classCXxx (IsKindOf checks), so the statics must exist in mushcore too.
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

// BYTE-COPY of doc.cpp:2662 FindStyle — pure style-list logic, no GUI.  The
// old `return false` stub silently disabled every FindStyle consumer headless
// (notably ProcessPreviousLine's trigger colour-change block, which starts by
// locating the style run at the match column).  Remove when doc.cpp adopts.
bool CMUSHclientDoc::FindStyle (const CLine * pLine,         // which line
                                const int iWantedCol,  // which column we want
                                int & iCol,            // which column style goes up to
                                CStyle * & pStyle,     // return style pointer
                                POSITION & foundpos) const  // and its position
  {
   foundpos = NULL;   // none initially
   iCol = 0;

   // find first style to change
   for (POSITION pos = pLine->styleList.GetHeadPosition(); pos; )
     {
     foundpos = pos;  // position to insert after
     pStyle = pLine->styleList.GetNext (pos);
     iCol += pStyle->iLength;
     if (iCol >= iWantedCol)      // NJG - version 3.73
        break;    // found first style
      }

  return foundpos != NULL;
  }   // end of CMUSHclientDoc::FindStyle

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

// ── CPlugin ───────────────────────────────────────────────────────────────
// doc_stub.h now includes the real plugins.h (CPlugin convergence), so every
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
