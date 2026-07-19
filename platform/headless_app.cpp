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
#if defined(MUSHCLIENT_HEADLESS_APP)   // Stage-5 activation switch (see above)

#include "stdafx.h"
#include "MUSHclient.h"

// Headless ctor — specs the current stub ctor (doc_stub.h:1343) plus the real
// m_nUniqueNumber semantics.  On Windows this work lives in InitInstance()
// (never run headless: no message loop), which is exactly why the stub ctor
// front-loads it into construction.
CMUSHclientApp::CMUSHclientApp()
{
    MXP_LoadElements ();                               // MUSHclient.cpp:743
    MXP_LoadEntities ();                               // MUSHclient.cpp:744
    MXP_LoadColours  ();                               // MUSHclient.cpp:745
    m_whenClientStarted = CTime::GetCurrentTime ();    // MUSHclient.cpp InitInstance
    m_nUniqueNumber     = 0;                           // MUSHclient.cpp:475 — the real
                                                       // __int64 GetUniqueNumber()
                                                       // { return m_nUniqueNumber++; }
                                                       // (MUSHclient.h:261) reads this.
    // TODO(Stage 5 activation): replicate the stub App's remaining in-class
    // member-default initialisers (doc_stub.h:1359-1453 — the global-options
    // family) here, since the real MUSHclient.h members have no in-class
    // initialisers (Windows fills them from the registry in InitInstance).  Then
    // build + ctest to verify this ctor matches the stub's headless behaviour.
}

// Real dtor is inert off-Windows (MUSHclient.cpp:211-215 — AfxOleUnlockApp is
// commented out); nothing to release.
CMUSHclientApp::~CMUSHclientApp()
{
}

// The one and only headless CMUSHclientApp instance.  MOVES here from
// doc_portable.cpp:151 in Stage 5 (that line is deleted in the same commit, so
// exactly one strong `App` exists at all times).
CMUSHclientApp App;

#endif // MUSHCLIENT_HEADLESS_APP
#endif // _WIN32
