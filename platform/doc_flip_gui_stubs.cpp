// platform/doc_flip_gui_stubs.cpp — Phase 2 Unit B-1 (docs/PHASE2-RECOVERY.md §5/§6).
//
// The FIRST pre-flip additive step of the Option-B big-bang.  A ground-truth
// link-closure probe (§5) measured that after the flip ~78 GUI-only symbols —
// referenced by the real doc.cpp / doc_construct.cpp / Utilities.cpp but defined
// ONLY in mushapp-only GUI TUs (mushview / sendvw / dialogs** / methods_* /
// miniwindow / configuration.cpp / scripting.cpp / MUSHclient.cpp) — would be
// undefined in mushcore.  This TU lands the no-op + tiny-real subset of that set
// NOW, as strong out-of-line definitions, so libmushcore.a already carries them
// before the atomic flip (B-3) — shrinking that commit.
//
// Build: compiled into mushcore ONLY (MUSHCORE_SOURCES), never mushapp/mushgui —
// the real TUs own these symbols there.  Like platform/windows_stubs.cpp and
// platform/doc_headless_stubs.cpp it compiles against the REAL headers (doc.h /
// mainfrm.h / the dialog headers), like platform/windows_stubs.cpp.  These STRONG
// defs are the sole providers of these GUI-only symbols in the headless link; the
// pre-flip build resolved them through the retired headless stub's weak inlines;
// those TUs onto the real headers the weak inlines vanish and these become the
// sole providers.  Everything here is UNREFERENCED in the current stub build (no
// mushcore TU calls a GUI command handler / dialog / DoGlobalPrefs), so the .o is
// not pulled into any test link and ctest is unaffected (30/30) until the flip.
//
// NOT here (deferred to B-2 / the flip):
//   * the ~10 GDI shim entries — CDC::{CreateCompatibleDC,GetTextMetrics,
//     SelectObject}, CFont::{CreateFont,~CFont}, CBitmap::~CBitmap,
//     ::CreateDIBSection, and the CBitmap/CFont/CGdiObject vtables — these need
//     the headless GDI shim over pal::ISurface / cpu_raster (B-2's GDI-shape ODR
//     work), not a trivial stub.
//
// ─── Per-symbol duplicate audit (§ CRITICAL AUDIT) ────────────────────────────
// Every symbol below was checked against the mushcore-compiled strong-def TUs
// (windows_stubs.cpp / doc_portable.cpp / doc_gui_method_stubs.cpp /
// doc_headless_stubs.cpp) and `nm libmushcore.a`.  All were UNDEFINED in mushcore
// (they are the §5 residual, net of the already-staged 116+48 stubs); none carry
// a pre-existing strong def, so no strong-vs-strong collision.  The afx_msg
// command handlers, GamePreferences and CreateScriptEngine/DisableScripting are
// not even declared by the headless CMUSHclientDoc (they are GUI-only members),
// so they are net-new; SetTitle(LPCTSTR) differs in signature from the stub's
// inline SetTitle(const CString&) (distinct mangled name).

#ifndef _WIN32

// Same real-header include order as windows_stubs.cpp / doc_headless_stubs.cpp,
// so CMUSHclientDoc / CMainFrame / the dialog classes are the full doc.h shapes.
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "MUSHview.h"
#include "sendvw.h"
#include "mainfrm.h"
#include "TextDocument.h"
#include "miniwindow.h"
#include "scripting/scripting.h"   // CScriptEngine (CreateScriptEngine body)

// Dialog classes doc.cpp / doc_construct.cpp instantiate but never show headless.
#include "dialogs/ConfirmPreamble.h"
#include "dialogs/DebugWorldInputDlg.h"
#include "dialogs/ImportXMLdlg.h"
#include "dialogs/KeyNameDlg.h"
#include "dialogs/logdlg.h"
#include "dialogs/ProgDlg.h"
#include "dialogs/QuickConnectDlg.h"
#include "dialogs/RecallSearchDlg.h"
#include "dialogs/chat/ChatListDlg.h"
#include "dialogs/plugins/PluginsDlg.h"
// B-3 flip: the extra stragglers the full-lib link surfaced (§5 "+15").
#include "dialogs/DebugLuaDlg.h"
#include "dialogs/ScriptErrorDlg.h"
#include "dialogs/password.h"
#include "dialogs/EditMultiLine.h"   // PluginWizard.cpp opens a CEditMultiLine
#include "winplace.h"

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <string>
#include <sys/stat.h>

// ════════════════════════════════════════════════════════════════════════════
// 1. CMUSHclientDoc afx_msg command / update handlers (world-prefs family).
//    doc.cpp's message map takes their address; they are NEVER invoked headless
//    (there is no menu / accelerator to fire them).  Real bodies open property
//    pages in dialogs/world_prefs/configuration.cpp (mushapp-only).  No-op.
// ════════════════════════════════════════════════════════════════════════════

void CMUSHclientDoc::OnGamePreferences        () {}
// OnGameResetalltimers: REAL body in timers.cpp now (un-gated 2026-08-12) —
// it resets plugin timers too, which the stub (and the GUI's one-liner) lost.
void CMUSHclientDoc::OnGameMapper             () {}
void CMUSHclientDoc::OnGameDomapperspecial    () {}
void CMUSHclientDoc::OnGameDomappercomment    () {}

void CMUSHclientDoc::OnGameConfigureMudaddress      () {}
void CMUSHclientDoc::OnGameConfigureNameAndPassword () {}
void CMUSHclientDoc::OnGameConfigureLogging         () {}
void CMUSHclientDoc::OnGameConfigureInfo            () {}
void CMUSHclientDoc::OnGameConfigureNotes           () {}
void CMUSHclientDoc::OnGameConfigureColours         () {}
void CMUSHclientDoc::OnGameConfigurePrinting        () {}
void CMUSHclientDoc::OnGameConfigureOutput          () {}
void CMUSHclientDoc::OnGameConfigureHighlighting    () {}
void CMUSHclientDoc::OnGameConfigureCommands        () {}
void CMUSHclientDoc::OnGameConfigureKeypad          () {}
void CMUSHclientDoc::OnGameConfigureMacros          () {}
void CMUSHclientDoc::OnGameConfigureAutosay         () {}
void CMUSHclientDoc::OnGameConfigurePastetoworld    () {}
void CMUSHclientDoc::OnGameConfigureSendfile        () {}
void CMUSHclientDoc::OnGameConfigureScripting       () {}
void CMUSHclientDoc::OnGameConfigureVariables       () {}
void CMUSHclientDoc::OnGameConfigureTriggers        () {}
void CMUSHclientDoc::OnGameConfigureAliases         () {}
void CMUSHclientDoc::OnGameConfigureTimers          () {}
void CMUSHclientDoc::OnGameConfigureCustomColours   () {}
void CMUSHclientDoc::OnGameConfigureMxppueblo       () {}
void CMUSHclientDoc::OnGameConfigureChat            () {}

// OnGameImmediate / OnUpdateGameImmediate: real bodies are
// scripting/scripting.cpp, in mushcore since the adoption.
// OnUpdateGameResetalltimers: real body is timers.cpp (mushcore compiles it
// under FULL_BUILD now) — a stub here would be a duplicate symbol.
void CMUSHclientDoc::OnUpdateGameMapper          (CCmdUI* /*pCmdUI*/) {}
void CMUSHclientDoc::OnUpdateGameDomapperspecial (CCmdUI* /*pCmdUI*/) {}
void CMUSHclientDoc::OnUpdateGameDomappercomment (CCmdUI* /*pCmdUI*/) {}

// GamePreferences (configuration.cpp:2003) opens the World-Properties tree
// property sheet; headless there is no sheet, so no page is shown.  Returns
// FALSE = "nothing changed" (the OK/Cancel result the OnGameConfigure* handlers
// above would observe).
bool CMUSHclientDoc::GamePreferences (const int /*iPage*/)
{
  return false;
}

// SetTitle: the REAL body now (methods_output.cpp:714, adopted into mushcore).
// The tiny real copy that stood here was identical to it line for line.


// CreateScriptEngine and DisableScripting are GONE from here: the canonical
// bodies in scripting/scripting.cpp are in mushcore now.  Their twins had
// drifted on six counts, three of them never recorded — see
// docs/STUB-ADOPTION-2026-08.md §7.
// ════════════════════════════════════════════════════════════════════════════
// 3. CMainFrame::DoFileOpen — real body (mainfrm.cpp) shows a file-open dialog
//    then opens the world.  Headless there is no dialog; no-op.
// ════════════════════════════════════════════════════════════════════════════

void CMainFrame::DoFileOpen () {}

// ════════════════════════════════════════════════════════════════════════════
// 4. Miscellaneous GUI view / notepad methods addressed by the doc-side surface
//    but inert headless.  Real bodies are mushapp-only (MUSHview.cpp / sendvw.cpp
//    / Utilities.cpp+notepad).  ~CMiniWindow's members are POINTERS (pdc /
//    m_Bitmap), so an empty dtor references no GDI; miniwindows are never created
//    headless (WindowCreate → eNoSuchWindow), so it is address-only.
// ════════════════════════════════════════════════════════════════════════════

void CMUSHView::sizewindow  (void) {}
void CSendView::UpdateWrap  ()     {}
// CTextDocument::SetTheFont: real body now (TextDocument.cpp, adopted).
// CMiniWindow::~CMiniWindow: the REAL dtor now (miniwindow.cpp, adopted).
// The empty stub that stood here LEAKED every miniwindow: the members it left
// unreleased are pointers (pdc / m_Bitmap), so nothing crashed and nothing
// noticed.  The real dtor is null-safe headless — both pal::g_releaseAdoptedSurface
// and pal::g_surface are guarded.

// ════════════════════════════════════════════════════════════════════════════
// 5. Dialog classes doc.cpp / doc_construct.cpp construct but never DoModal
//    meaningfully headless.  Each needs its out-of-line members defined so the
//    ctor + vtable exist: the ctor, the declared virtual overrides (the vtable
//    key function is the first — DoDataExchange), and the message map (defines
//    the virtual GetMessageMap/GetThisMessageMap).  Bodies are trivial; empty
//    message maps chain to CCmdTarget, the only ancestor whose GetThisMessageMap
//    is defined headless (compat.h), exactly as windows_stubs.cpp does for the
//    frame classes.  Real bodies live in dialogs/**/*.cpp (mushapp-only).
// ════════════════════════════════════════════════════════════════════════════

CConfirmPreamble::CConfirmPreamble (CWnd* pParent)
  : CDialog (CConfirmPreamble::IDD, pParent) {}
void CConfirmPreamble::DoDataExchange (CDataExchange* /*pDX*/) {}
BEGIN_MESSAGE_MAP(CConfirmPreamble, CCmdTarget)
END_MESSAGE_MAP()

CDebugWorldInputDlg::CDebugWorldInputDlg (CWnd* pParent)
  : CDialog (CDebugWorldInputDlg::IDD, pParent) {}
void CDebugWorldInputDlg::DoDataExchange (CDataExchange* /*pDX*/) {}
BOOL CDebugWorldInputDlg::OnInitDialog () { return TRUE; }
BEGIN_MESSAGE_MAP(CDebugWorldInputDlg, CCmdTarget)
END_MESSAGE_MAP()

CImportXMLdlg::CImportXMLdlg (CWnd* pParent)
  : CDialog (CImportXMLdlg::IDD, pParent) {}
void CImportXMLdlg::DoDataExchange (CDataExchange* /*pDX*/) {}
BEGIN_MESSAGE_MAP(CImportXMLdlg, CCmdTarget)
END_MESSAGE_MAP()

CKeyNameDlg::CKeyNameDlg (CWnd* pParent)
  : CDialog (CKeyNameDlg::IDD, pParent) {}
void CKeyNameDlg::DoDataExchange (CDataExchange* /*pDX*/) {}
BEGIN_MESSAGE_MAP(CKeyNameDlg, CCmdTarget)
END_MESSAGE_MAP()

CLogDlg::CLogDlg (CWnd* pParent)
  : CDialog (CLogDlg::IDD, pParent) {}
void CLogDlg::DoDataExchange (CDataExchange* /*pDX*/) {}
BEGIN_MESSAGE_MAP(CLogDlg, CCmdTarget)
END_MESSAGE_MAP()

CQuickConnectDlg::CQuickConnectDlg (CWnd* pParent)
  : CDialog (CQuickConnectDlg::IDD, pParent) {}
void CQuickConnectDlg::DoDataExchange (CDataExchange* /*pDX*/) {}
BEGIN_MESSAGE_MAP(CQuickConnectDlg, CCmdTarget)
END_MESSAGE_MAP()

CRecallSearchDlg::CRecallSearchDlg (CStringList& strFindStringList, CWnd* pParent)
  : CDialog (CRecallSearchDlg::IDD, pParent),
    m_strFindStringList (strFindStringList) {}   // ref member — must be bound
void CRecallSearchDlg::DoDataExchange (CDataExchange* /*pDX*/) {}
BOOL CRecallSearchDlg::OnInitDialog () { return TRUE; }
BEGIN_MESSAGE_MAP(CRecallSearchDlg, CCmdTarget)
END_MESSAGE_MAP()

CChatListDlg::CChatListDlg (CWnd* pParent)
  : CDialog (CChatListDlg::IDD, pParent) {}
void CChatListDlg::DoDataExchange (CDataExchange* /*pDX*/) {}
BOOL CChatListDlg::OnInitDialog () { return TRUE; }
BEGIN_MESSAGE_MAP(CChatListDlg, CCmdTarget)
END_MESSAGE_MAP()

CPluginsDlg::CPluginsDlg (CWnd* pParent)
  : CDialog (CPluginsDlg::IDD, pParent) {}
void CPluginsDlg::DoDataExchange (CDataExchange* /*pDX*/) {}
BOOL CPluginsDlg::OnInitDialog () { return TRUE; }
BEGIN_MESSAGE_MAP(CPluginsDlg, CCmdTarget)
END_MESSAGE_MAP()

// CProgressDlg's fourteen stub bodies are GONE — dialogs/ProgDlg.cpp is in
// CORE_SOURCES now, so mushcore runs the shipping ones.  It was adopted for the
// reason the doc's own rule names: not for coverage (its DoDataExchange is a
// single DDX_Control and OnInitDialog cannot run headless), but because the
// twin was expensive to maintain — it had already drifted once and needed
// patching in a SECOND block after ccd094b.  The drivers it stubbed do carry
// real port logic, which now has its first headless test.

// ════════════════════════════════════════════════════════════════════════════
// 6. Free functions doc.cpp reaches (declared locally / in stdafx.h).  Real
//    bodies are mushapp-only (GlobalPrefsDlg path / the wx front-window raise).
// ════════════════════════════════════════════════════════════════════════════

// DoGlobalPrefs (doc.cpp:2651 forward-decl) opens the global-preferences sheet.
void DoGlobalPrefs (CMUSHclientDoc* /*pCurrentDoc*/) {}

// SendToFront (stdafx.h:819) raises a named external window; no windows headless.
bool SendToFront (const char* /*name*/) { return false; }

// ════════════════════════════════════════════════════════════════════════════
// 7. Data globals owned by MUSHclient.cpp (the app main, mushapp-only) that the
//    real doc.cpp / doc_construct.cpp read.  bWine gates a few Windows-only code
//    paths (always false off-Windows); MUSHCLIENT_VERSION is stamped into the
//    world-open banner (doc.cpp:327) and saved-file headers.  App.cpp sets the
//    real version at startup; headless it stays empty (never displayed).
// ════════════════════════════════════════════════════════════════════════════

bool bWine = false;
// Windows sets this in MUSHclient.cpp InitInstance (= VERSION_STRING); headless
// there is no InitInstance, so seed it at static-init with the same value so
// world.Version() / the world-open banner report the real version (the retired
// headless stub pre-flip defined it as a #define "5.07"; doc.h:21 now makes it a global).
CString MUSHCLIENT_VERSION = VERSION_STRING;

// ════════════════════════════════════════════════════════════════════════════
// 8. B-3 stragglers — the extra GUI/view symbols the FULL-LIBRARY link surfaced
//    (the §5 "+15 full faithful flip").  All address-only / DoModal-via-null-hook
//    headless.  Real bodies live in mushapp-only TUs.
// ════════════════════════════════════════════════════════════════════════════

// ── Free functions doc.cpp reaches (stdafx.h decls) ─────────────────────────
// QuoteForumCodes is NOT stubbed any more, for the same reason StringToKeyCode
// below is not: the stub's justification ("headless never runs the menu
// handler") was false — notepad_convert_shared.cpp:244 is a CORE_SOURCES TU and
// routes eNotepadConvertQuoteForum straight into it.  The real body now lives in
// utilities_shared.cpp, so every build compiles one definition.

// StringToKeyCode is NOT stubbed: it is pure string parsing and now lives in
// utilities_shared.cpp, so the headless engine runs the real one.  The stub
// that used to sit here returned 0/0 for every key, which made the whole
// accelerator API look like it worked while binding nothing.

// ── CMUSHView view methods (MUSHview.cpp, mushapp-only) ─────────────────────
void CMUSHView::addedstuff (void) {}
void CMUSHView::did_jump   (void) {}

// AutoWrapWindowWidth — ADDRESS-ONLY, and the distinction matters.  The adopted
// CMUSHclientDoc::TextRectangle (methods_output.cpp) calls it, so the symbol has
// to exist, but the call sits inside a GetFirstViewPosition walk that yields
// nothing headless (CSplitterWnd::CreateStatic is a FALSE stub, so no CMUSHView
// is ever constructed — the recorded dead-view-family strategy).  So this needs
// a DEFINITION, not an implementation, and giving it a body would be inventing
// behaviour no build has.
void CMUSHView::AutoWrapWindowWidth (CMUSHclientDoc* /*pDoc*/) {}

// ── Missing virtual overrides on the already-stubbed dialogs (§8) ───────────
// OnSize/OnDestroy are virtual in the wx CWnd shim, so the dialog vtables above
// reference them; empty bodies satisfy the vtable (never fire headless).
void CPluginsDlg::OnDestroy () {}
void CPluginsDlg::OnSize (UINT /*nType*/, int /*cx*/, int /*cy*/) {}
void CChatListDlg::OnDestroy () {}

// (The four further CProgressDlg stubs that lived here — SetStatus, StepIt,
// OffsetPos, SetWindowText — are gone with the ten above.  Their existence in a
// SECOND block, added after ccd094b patched the first, is precisely why this
// TU was worth retiring.)

// ── Additional dialog classes the flip instantiates (ctor + vtable) ─────────
// CDebugLuaDlg (lua_methods.cpp our_lua_debug_function): DoModal via the null
// g_run_mfc_dialog hook → IDCANCEL.  The dtor (inline) deletes m_font, so the
// ctor must null it (and L / m_bAbort).
CDebugLuaDlg::CDebugLuaDlg (CWnd* pParent)
  : CDialog (CDebugLuaDlg::IDD, pParent)
{
  L        = nullptr;
  m_bAbort = false;
  m_font   = nullptr;
}
void CDebugLuaDlg::DoDataExchange (CDataExchange* /*pDX*/) {}
void CDebugLuaDlg::OnCancel () {}
BEGIN_MESSAGE_MAP(CDebugLuaDlg, CCmdTarget)
END_MESSAGE_MAP()

// CScriptErrorDlg (scripting/lua_scripting.cpp LuaError): since LuaError became
// one body for every build, a mushcore TU names this class, so the headless link
// needs it.  DoModal goes through the null g_run_mfc_dialog hook → IDCANCEL, and
// m_bUseOutputWindow stays FALSE, so a headless script error reports nothing —
// which is what Windows does too when the world's "note errors" option is off
// and the user just closes the dialog.  A test that wants the four ColourNotes
// sets pDoc->m_bScriptErrorsToOutputWindow first.  The dtor (inline, in
// dialogs/ScriptErrorDlg.h:19) deletes m_font, so the ctor must null it.
CScriptErrorDlg::CScriptErrorDlg (CWnd* pParent)
  : CDialog (CScriptErrorDlg::IDD, pParent)
{
  m_iError           = 0;
  m_bUseOutputWindow = FALSE;
  m_bHaveDoc         = false;
  m_font             = nullptr;
}
void CScriptErrorDlg::DoDataExchange (CDataExchange* /*pDX*/) {}
void CScriptErrorDlg::OnCopy () {}
BOOL CScriptErrorDlg::OnInitDialog () { return TRUE; }
BEGIN_MESSAGE_MAP(CScriptErrorDlg, CCmdTarget)
END_MESSAGE_MAP()

// CPasswordDialog (doc_shared.cpp password prompt): the port declares an
// out-of-line DoModal (#ifndef _WIN32); headless it returns IDCANCEL, which the
// caller reads as "empty password" (the documented headless arm).
CPasswordDialog::CPasswordDialog (CWnd* pParent)
  : CDialog (CPasswordDialog::IDD, pParent) {}
void CPasswordDialog::DoDataExchange (CDataExchange* /*pDX*/) {}
INT_PTR CPasswordDialog::DoModal () { return IDCANCEL; }
BEGIN_MESSAGE_MAP(CPasswordDialog, CCmdTarget)
END_MESSAGE_MAP()

// CPluginWizardSheet is NOT stubbed here: plugins.cpp (OnFilePluginwizard) uses
// the REAL class under FULL_BUILD, and it aggregates 8 CPluginWizardPageN members
// — impractical to hand-stub.  Per "adopt, don't stub", the real
// dialogs/plugins/PluginWizardSheet.cpp + PluginWizard.cpp are compiled into
// mushcore (CMakeLists.txt), providing the sheet + 8 pages headless.

// CPropertySheet::DoModal — the base sheet's runner (real body in
// wx_tree_property_sheet.cpp, mushapp-only).  No sheet headless → IDCANCEL.
INT_PTR CPropertySheet::DoModal () { return IDCANCEL; }

// CWindowPlacement (winplace.cpp, mushapp-only) — zero the WINDOWPLACEMENT so a
// headless read sees a defined (empty) placement.
CWindowPlacement::CWindowPlacement ()  { memset (this, 0, sizeof (*this)); length = sizeof (WINDOWPLACEMENT); }
CWindowPlacement::~CWindowPlacement () {}

// GetText (Utilities.cpp, mushapp-only) — the text of a window/control; no
// windows headless, so empty.  Reached by the adopted PluginWizard.cpp.
CString GetText (const CWnd & /*theWindow*/) { return CString (); }

// CEditMultiLine (EditMultiLine.cpp, mushapp-only) — the multi-line editor the
// adopted PluginWizard.cpp opens for the description/script.  DoModal via the
// null hook → IDCANCEL (CDialog base).  m_font is a raw pointer (no user dtor),
// so null it; the two virtual overrides emit the vtable.
CEditMultiLine::CEditMultiLine (CWnd* pParent)
  : CDialog (CEditMultiLine::IDD, pParent)
{
  m_font    = nullptr;
  m_bScript = false;
  m_bLua    = false;
}
void CEditMultiLine::DoDataExchange (CDataExchange* /*pDX*/) {}
BOOL CEditMultiLine::PreTranslateMessage (MSG* /*pMsg*/) { return FALSE; }
BOOL CEditMultiLine::OnInitDialog () { return TRUE; }
void CEditMultiLine::OnSize (UINT /*nType*/, int /*cx*/, int /*cy*/) {}
void CEditMultiLine::OnDestroy () {}
BEGIN_MESSAGE_MAP(CEditMultiLine, CCmdTarget)
END_MESSAGE_MAP()

#endif // !_WIN32
