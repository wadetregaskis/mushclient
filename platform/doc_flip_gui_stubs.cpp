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
// mainfrm.h / the dialog headers), NOT platform/doc_stub.h.  These STRONG defs
// coexist with, and win over, doc_stub.h's WEAK inline defs in the TUs that still
// include the stub header (the proven windows_stubs.cpp pattern); when B-3 flips
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
// not even declared by doc_stub.h's CMUSHclientDoc (they are GUI-only members),
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
void CMUSHclientDoc::OnGameImmediate          () {}
void CMUSHclientDoc::OnGameResetalltimers     () {}
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

void CMUSHclientDoc::OnUpdateGameImmediate       (CCmdUI* /*pCmdUI*/) {}
void CMUSHclientDoc::OnUpdateGameResetalltimers  (CCmdUI* /*pCmdUI*/) {}
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

// SetTitle (methods_output.cpp:848, mushapp-only) — tiny REAL body: store the
// document title and propagate it to the world's IWorldView.  Kept real (not a
// no-op) so post-flip m_strWindowTitle stays live and title changes still reach
// the view seam, exactly as today via the portable path.  EmitTitle is the
// unguarded IWorldView seam (available in mushcore).
void CMUSHclientDoc::SetTitle (LPCTSTR Title)
{
  m_strWindowTitle = Title;
  EmitTitle (static_cast<const char*> (m_strWindowTitle));
}

// ════════════════════════════════════════════════════════════════════════════
// 2. Tiny REAL scripting bodies (scripting.cpp is mushapp-only; §5's 2 real).
//    These ARE called post-flip (OpenSession doc.cpp:479, doc.cpp:2287-2288,
//    doc_construct.cpp:618), so behaviour matters.  Relocated from the portable
//    twin doc_portable.cpp:PortCreateWorldScriptEngine (still live until B-3):
//    Lua-only (the other five engines are embedded on the mushgui side) and the
//    engine is created only when a script FILE is configured — the deliberate
//    headless deviation where engine-presence == script-presence.
// ════════════════════════════════════════════════════════════════════════════

bool CMUSHclientDoc::CreateScriptEngine ()
{
  // The m_bEnableScripts / !m_ScriptEngine gate stays in the caller
  // (OpenSession, doc.cpp:477-479), matching Windows.
  if (m_strScriptFilename.IsEmpty ())
    return false;                 // nothing to run (headless deviation)

  CString strLanguage = m_strLanguage;
  strLanguage.MakeLower ();
  if (strLanguage.IsEmpty ())
    strLanguage = "lua";          // table default
  if (strLanguage != "lua")
    {
    fprintf (stderr, "[world load] scripting language \"%s\" is not supported "
                     "headless — script not loaded\n",
             static_cast<const char*> (m_strLanguage));
    return false;
    }

  m_bSyntaxErrorOnly = false;
  CScriptEngine * engine = new CScriptEngine (this, "lua");
  if (engine->CreateScriptEngine ())     // true == failed (scripting.cpp:39)
    {
    fprintf (stderr, "[world load] could not initialise Lua engine\n");
    delete engine;
    return true;                         // engine-init failure (scripting.cpp:43)
    }
  m_ScriptEngine = engine;               // scripting.cpp:37 (set before parsing)

  // Read the script file (scripting.cpp:52-73) + remember its mtime so
  // OnScriptFileChanged doesn't immediately see a "change".
  FILE * f = fopen (static_cast<const char*> (m_strScriptFilename), "rb");
  if (!f)
    {
    fprintf (stderr, "[world load] cannot open script file \"%s\": %s\n",
             static_cast<const char*> (m_strScriptFilename), strerror (errno));
    // Windows shows "unable to open" and disables scripting; keep the (empty)
    // engine so the failure is visible but harmless.
    }
  else
    {
    fseek (f, 0, SEEK_END);
    long sz = ftell (f);
    fseek (f, 0, SEEK_SET);
    std::string buf;
    if (sz > 0)
      {
      buf.resize ((size_t) sz);
      size_t got = fread (&buf [0], 1, (size_t) sz, f);
      buf.resize (got);
      }
    fclose (f);
    m_strScript = CString (buf.c_str ());

    struct stat st;
    if (::stat (static_cast<const char*> (m_strScriptFilename), &st) == 0)
      m_scriptFileMtime = st.st_mtime;

    if (engine->Parse (m_strScript, "Script file"))    // scripting.cpp:78
      {
      fprintf (stderr, "[world load] error parsing script \"%s\"\n",
               static_cast<const char*> (m_strScriptFilename));
      m_bSyntaxErrorOnly = true;
      DisableScripting ();                             // scripting.cpp:81
      return true;
      }
    }

  FindAllEntryPoints ();                               // scripting.cpp:102
  return false;                                        // good return
}

void CMUSHclientDoc::DisableScripting (void)
{
  // No script-file monitoring thread headless (the port polls m_scriptFileMtime
  // in OnScriptFileChanged), so scripting.cpp:290's KillThread has no analogue.
  delete m_ScriptEngine;
  m_ScriptEngine = NULL;
}

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
void CTextDocument::SetTheFont (void) {}
CMiniWindow::~CMiniWindow () {}

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

// CProgressDlg (ProgDlg.cpp) — the world-load progress bar.  Besides the dialog
// ctor/vtable it exposes non-virtual drivers the loader pokes (Create / SetPos /
// SetRange / CheckCancelButton); all inert headless (no window, never cancels).
CProgressDlg::CProgressDlg (UINT /*nCaptionID*/) {}
CProgressDlg::~CProgressDlg () {}
void CProgressDlg::DoDataExchange (CDataExchange* /*pDX*/) {}
BOOL CProgressDlg::OnInitDialog () { return TRUE; }
BOOL CProgressDlg::DestroyWindow () { return TRUE; }
void CProgressDlg::OnCancel () {}
BOOL CProgressDlg::Create (CWnd* /*pParent*/) { return TRUE; }
BOOL CProgressDlg::CheckCancelButton () { return FALSE; }   // never cancelled
void CProgressDlg::SetRange (int /*nLower*/, int /*nUpper*/) {}
int  CProgressDlg::SetPos (int /*nPos*/) { return 0; }
BEGIN_MESSAGE_MAP(CProgressDlg, CCmdTarget)
END_MESSAGE_MAP()

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
CString MUSHCLIENT_VERSION;

#endif // !_WIN32
