// platform/headless_app.cpp — the headless CMUSHclientApp landing pad
// (docs/PHASE2-PLAN.md, Stage 3 item 4).
//
// Build: compiled into mushcore ONLY (MUSHCORE_SOURCES), never mushapp/mushgui.
// mushgui keeps platform/mushapp_globals.cpp + the real MUSHclient.cpp: the
// deliberate arrangement is TWO CMUSHclientApp ctor BODIES across DISJOINT link
// targets (mushcore vs mushapp) — legal, because no target links both libraries.
//
// ─── Why this TU is INACTIVE in Stage 3 (the App-ctor link gap) ───────────────
// The plan's Stage-3 goal is the strong-symbol safety net while everything still
// links via the stub.  For the App that safety net cannot be *activated* yet, for
// two hard reasons that are only resolved in Stage 5:
//
//  1. Duplicate global.  `CMUSHclientApp App;` already has a STRONG definition in
//     doc_portable.cpp:151 (a mushcore TU).  A second strong `App` here is a
//     link error (strong-vs-strong), not the tolerated strong-vs-weak-inline.
//     Per the Stage-3 audit rule that symbol is NOT duplicated; it MOVES here in
//     Stage 5 (Stage 5 item 1) when doc_portable.cpp:151 is deleted in lockstep.
//
//  2. Layout incompatibility.  The App global is *constructed* by doc_portable.cpp,
//     which sees the STUB CMUSHclientApp (platform/doc_stub.h:1202 — no base class,
//     no vtable).  The real CMUSHclientApp (MUSHclient.h:50) is `: public CWinApp`
//     with `virtual PreTranslateMessage`, so it carries a vptr at offset 0 and a
//     base subobject — a DIFFERENT object layout.  A strong out-of-line real ctor
//     here would win over the stub's weak inline ctor (doc_stub.h:1343) and then
//     run on the stub-layout global → memory corruption.  Activation is safe only
//     once the App-accessing mushcore TUs (doc_portable.cpp et al.) have flipped
//     to the real MUSHclient.h layout in Stage 4-5.
//
// So in Stage 3 this file is wired into MUSHCORE_SOURCES but compiles to an empty
// TU.  The real-type ctor/dtor/global below are the ready-to-activate Stage-5
// landing (flip the guard + delete doc_portable.cpp:151 + finish the member-
// default parity, then verify).  They are written but UNVERIFIED until activated.

#ifndef _WIN32
#if defined(MUSHCLIENT_HEADLESS_APP)   // ACTIVATED at B-3 (the atomic flip)

#include "stdafx.h"
#include "MUSHclient.h"

#include <string>   // std::string (db_simple_query signature)

// The headless world-document template.  On Windows this is created in
// InitInstance (AddDocTemplate) and the MFC framework registers each open
// CMUSHclientDoc with it; headless there is no framework, so the App owns a
// single template and doc_construct.cpp's ctor/dtor Add/RemoveDocument against
// it (guarded MUSHCLIENT_HEADLESS_APP).  A file-scope object (constructed before
// the App global below, same TU) so App.m_pWorldDocTemplate is valid the instant
// the first world doc is built.  The real CMultiDocTemplate is the mfc_ui.h shim
// (m_docs vector + GetFirstDocPosition/GetNextDoc), which methods_worlds.cpp /
// mainfrm walk.  Was the stub App's `CWorldDocTemplate m_worldDocTemplate` member.
static CMultiDocTemplate s_worldDocTemplate (0, NULL, NULL, NULL);

// Headless ctor — reproduces the retired stub CMUSHclientApp ctor
// (doc_stub.h:1343) plus the stub's in-class global-option defaults, since the
// real MUSHclient.h members have NO in-class initialisers (Windows fills them
// from the registry in InitInstance, which never runs headless).  These are the
// Windows table defaults — the state Windows reaches via LoadGlobalsFromDatabase
// over an empty prefs database.  A static/global App is zero-initialised before
// this runs, so only the NON-zero defaults need setting here.
CMUSHclientApp::CMUSHclientApp()
{
    MXP_LoadElements ();                               // MUSHclient.cpp MXP tables
    MXP_LoadEntities ();
    MXP_LoadColours  ();
    m_whenClientStarted = CTime::GetCurrentTime ();    // MUSHclient.cpp InitInstance
    m_nUniqueNumber     = 0;                           // __int64 GetUniqueNumber()
                                                       // { return m_nUniqueNumber++; }

    m_pWorldDocTemplate = &s_worldDocTemplate;         // world registry (see above)

    // ── Global-option defaults (was doc_stub.h:1359-1453) ────────────────────
    // Boolean/int global prefs whose Windows table default is non-zero.
    m_bAutoConnectWorlds            = 1;
    m_bAutoExpand                  = 1;
    m_bFlatToolbars                = 1;
    m_bColourGradient              = 1;
    m_bConfirmBeforeClosingWorld   = 1;
    m_bConfirmBeforeSavingVariables = 1;
    m_bConfirmLogFileClose         = 1;
    m_bEnableSpellCheck            = 1;
    m_bEnablePackageLibrary        = 1;
    m_bFixedFontForEditing         = 1;
    m_bNotepadWordWrap             = 1;
    m_bRegexpMatchEmpty            = 1;   // read by regexp.cpp (regexec options)
    m_bShowGridLinesInListViews    = 1;
    m_bTriggerRemoveCheck          = 1;
    m_nActivityWindowRefreshInterval = 15;
    m_nActivityWindowRefreshType   = eRefreshBoth;
    m_nParenMatchFlags             = 0x0001 | 0x0020 | 0x0040;   // NEST|BACKSLASH|PERCENT
    m_iNotepadFontSize             = 9;
    m_nPrinterLinesPerPage         = 60;
    m_nPrinterLeftMargin           = 1;
    m_nPrinterTopMargin            = 1;
    m_nPrinterFontSize             = 10;
    m_strNotepadQuoteString        = "> ";
    m_strWordDelimiters            = ".,()[]\"'";
    m_strPluginsDirectory          = ".";   // plugin loader rejects an empty one
                                            // ("No plugins directory specified")
}

// Real dtor is inert off-Windows (MUSHclient.cpp AfxOleUnlockApp is commented
// out); nothing to release.
CMUSHclientApp::~CMUSHclientApp()
{
}

// Message map — real body is MUSHclient.cpp (mushapp-only).  Provides the
// virtual GetMessageMap/GetThisMessageMap; empty map chains to CWinApp.  None of
// its handlers fire headless (no message loop).
BEGIN_MESSAGE_MAP(CMUSHclientApp, CWinApp)
END_MESSAGE_MAP()

// ── CWinApp virtual overrides (real bodies MUSHclient.cpp, mushapp-only) ─────
// Defining these emits `vtable for CMUSHclientApp` and satisfies the CWinApp
// vtable slots CMUSHclientApp declares.  None run headless (no message loop).
BOOL CMUSHclientApp::PreTranslateMessage (MSG * /*pMsg*/) { return FALSE; }
BOOL CMUSHclientApp::InitInstance   ()            { return TRUE; }
BOOL CMUSHclientApp::SaveAllModified()            { return TRUE; }
BOOL CMUSHclientApp::OnIdle (LONG /*lCount*/)     { return FALSE; }
int  CMUSHclientApp::ExitInstance   ()            { return 0; }

// ── App members the stub had inline; real bodies are MUSHclient.cpp-only ─────
// GetUniqueString — used to make trigger/alias/timer names unique.
CString CMUSHclientApp::GetUniqueString ()
{
    static long s_n = 0;
    CString s;
    s.Format ("%ld", ++s_n);
    return s;
}

// App-level notepad helpers — no app-owned notepad windows headless.
bool CMUSHclientApp::AppendToTheNotepad (const CString /*strTitle*/,
                                         const CString /*strText*/,
                                         const bool /*bReplace*/,
                                         const int  /*iNotepadType*/)
{
    return false;
}

BOOL CMUSHclientApp::ActivateNotepad (LPCTSTR /*Title*/)
{
    return FALSE;
}

// SQLite prefs-database no-ops.  Headless there is no prefs database; the App
// members already hold the table defaults (ctor above), so db reads return the
// caller's default and writes are dropped.  Referenced (address-taken) by the
// adopted globalregistryoptions.cpp Populate/Load/SaveGlobalsFromDatabase, which
// never actually run headless.
int CMUSHclientApp::db_simple_query (const char * /*sql*/, std::string & result,
                                     const bool /*bShowError*/,
                                     const char * default_value)
{
    result = default_value ? default_value : "";
    return SQLITE_OK;
}
int CMUSHclientApp::db_execute (const char * /*sql*/, const bool /*bShowError*/)
{
    return SQLITE_OK;
}
int CMUSHclientApp::db_write_string (LPCTSTR /*lpszSection*/, LPCTSTR /*lpszEntry*/,
                                     LPCTSTR /*lpszValue*/)
{
    return SQLITE_OK;
}
int CMUSHclientApp::db_write_int (LPCTSTR /*lpszSection*/, LPCTSTR /*lpszEntry*/,
                                  int /*iValue*/)
{
    return SQLITE_OK;
}

// The one and only headless CMUSHclientApp instance.  Constructed AFTER
// s_worldDocTemplate (above, same TU) so the ctor's m_pWorldDocTemplate is valid.
// Replaces the retired doc_portable.cpp:151 `App` (deleted in the same commit,
// so exactly one strong `App` exists).
CMUSHclientApp App;

#endif // MUSHCLIENT_HEADLESS_APP
#endif // _WIN32
