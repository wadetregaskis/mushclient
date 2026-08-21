// tests/win/all_tests.cpp — the single main() for the Windows test target.
//
// On Windows the engine compiles against real MFC and a test binary is ~9 MB,
// so one executable per test file (the macOS/Linux arrangement) would mean 40
// full MFC links per build.  MUSHclient_tests.vcxproj instead compiles every
// enabled tests/test_*.cpp into ONE executable with MC_TEST_COMBINED defined,
// which turns each file's MC_TEST_MAIN() into nothing, and this file supplies
// the one main.  No test file needs any edit: TEST() registers into a global
// registry, so a suite behaves identically whether it is alone or with forty
// others.
//
// The trade is that all the suites share one process.  mc_test.h's runner is
// already written for that — it reports per-test and totals at the end — but
// it does mean one suite that calls abort() takes the rest down with it, and
// that suites are no longer isolated from each other's global state.  Both are
// acceptable for a target whose purpose is to exercise the Windows arms of
// shared code; if a suite ever needs isolation, give it its own vcxproj.

#include "stdafx.h"
#include "MUSHclient.h"   // the App global: MC_TEST_WIN_HOST_INIT primes its state
// The four runtime classes MC_TEST_WIN_DOC_TEMPLATES names.  They cost nothing
// here -- a CMultiDocTemplate stores the CRuntimeClass pointers and creates no
// window -- but the framework needs them to be real, so the headers come in.
#include "doc.h"
#include "TextDocument.h"
#include "childfrm.h"
#include "textchildfrm.h"
#include "MUSHview.h"
#include "TextView.h"
#include "mc_test.h"

MC_TEST_MAIN_ALWAYS()

// ── LoadWorldFromFile, for this host ───────────────────────────────────────
//
// Declared in doc.h under _WIN32 && MUSHCLIENT_TESTS.  It is a driver, not a
// loader: CMUSHclientDoc::Serialize's loading branch (serialize.cpp) IS the
// Windows world loader, and everything below it just hands that branch an
// archive and then does what the framework does afterwards.
//
// Two suites are built on this entry point -- tests/test_world_load.cpp uses it
// 105 times, once per test -- because the framework's own route in,
// CDocument::OnOpenDocument, wants a window this host does not have.
//
// The OpenSession steps at the end are doc.cpp's, run once a world is open: a
// scripting-enabled world gets its engine, and its OnWorldOpen handler runs.
// Without them a loaded world has no script engine, which is most of what the
// suites want to ask about.
BOOL CMUSHclientDoc::LoadWorldFromFile (const char * path)
{
  CFile f;
  if (!f.Open (path, CFile::modeRead | CFile::shareDenyWrite))
    {
    fprintf (stderr, "[world load] cannot open \"%s\"\n", path);
    return FALSE;
    }

  CArchive ar (&f, CArchive::load);

  try
    {
    Serialize (ar);       // the canonical Windows loading branch
    }
  catch (CException * e)
    {
    // Serialize throws a badSchema archive exception for a file with no
    // MUSHclient XML signature, and Load_World_XML throws for a malformed one.
    // Both are "this file did not load", which is what FALSE means here and
    // what the port's arm reports too.
    e->Delete ();
    return FALSE;
    }
  catch (...)
    {
    return FALSE;
    }

  m_bLoaded = true;

  // ── OpenSession scripting steps (doc.cpp) ────────────────────────────────
  if (m_bEnableScripts && !m_ScriptEngine)
    CreateScriptEngine ();

  if (m_ScriptEngine && SeeIfHandlerCanExecute (m_strWorldOpen))
    {
    DISPPARAMS params = { NULL, NULL, 0, 0 };
    long nInvocationCount = 0;
    ExecuteScript (m_dispidWorldOpen,
                   m_strWorldOpen,
                   eWorldAction,
                   "world open",
                   "opening world",
                   params,
                   nInvocationCount);
    }

  return TRUE;
}
