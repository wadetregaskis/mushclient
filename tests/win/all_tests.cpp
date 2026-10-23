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
#include "mushview.h"
#include "mainfrm.h"   // MC_TEST_WIN_MAIN_FRAME: a constructed, uncreated frame
#include "pal/gdi_surface.h"   // InstallGdiSurfaceBridge (MC_TEST_WIN_APP_STATE)
#include "TextView.h"
#include "mc_test.h"
#include <dbghelp.h>   // McTestCrashFilter: StackWalk64 + SymFromAddr

// ── A crash names its frame, instead of naming nothing ─────────────────────
//
// An access violation here is not caught by anything: mc_test.h's runner wraps
// each test in catch(...), but this target compiles /EHsc, where a C++ handler
// does not see an SEH exception -- so the process dies, the summary line is
// never printed, and run_tests.ps1 can only report "CRASHED (no summary)".
// The last [PASS] on stdout names the test BEFORE the one that died, which is
// enough to identify the test and nothing at all about where in the engine it
// went.
//
// So walk the stack from the exception's own CONTEXT and print it.  This is
// the Windows counterpart of the in-process backtrace()/backtrace_symbols_fd
// handler the macOS side reached for when lldb hung on the notepad crash
// (CLAUDE.md, Verification debt) -- and for the same reason: the instrument
// that is always installed beats the one that has to be attached to a
// reproduction.
//
// Symbols come from the PDB the Debug build already writes beside the exe, so
// this costs nothing to keep and needs no separate symbol server.  If dbghelp
// cannot name a frame it prints the module and offset, which is still enough
// to place it with a map file.
static void McTestPrintStack (const CONTEXT & context, int maxFrames);

static LONG WINAPI McTestCrashFilter (EXCEPTION_POINTERS * ep)
{
  static bool s_inFilter = false;
  if (s_inFilter)                 // a fault inside the walker: give up quietly
    return EXCEPTION_EXECUTE_HANDLER;
  s_inFilter = true;

  const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
  const char * name = "exception";
  switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:      name = "ACCESS VIOLATION";      break;
    case EXCEPTION_STACK_OVERFLOW:        name = "STACK OVERFLOW";        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    name = "INTEGER DIVIDE BY ZERO"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:   name = "ILLEGAL INSTRUCTION";   break;
    case EXCEPTION_PRIV_INSTRUCTION:      name = "PRIVILEGED INSTRUCTION"; break;
    case EXCEPTION_IN_PAGE_ERROR:         name = "IN-PAGE ERROR";         break;
    }

  fprintf (stderr, "\n*** %s (0x%08lX) at 0x%p", name, (unsigned long) code,
           ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : NULL);
  if (code == EXCEPTION_ACCESS_VIOLATION && ep && ep->ExceptionRecord &&
      ep->ExceptionRecord->NumberParameters >= 2)
    fprintf (stderr, " -- %s 0x%p",
             ep->ExceptionRecord->ExceptionInformation [0] ? "writing" : "reading",
             (void *) ep->ExceptionRecord->ExceptionInformation [1]);
  // Which test was running.  mc_test.h keeps it for exactly this kind of
  // report; stdout is unbuffered here, but the two streams are separate files
  // under the runner and only this line ties them together.
  fprintf (stderr, "\n*** in %s.%s\n",
           mc_test::g ().current_suite ? mc_test::g ().current_suite : "?",
           mc_test::g ().current_name  ? mc_test::g ().current_name  : "?");

  McTestPrintStack (*ep->ContextRecord, 40);
  fflush (stderr);
  return EXCEPTION_EXECUTE_HANDLER;   // die, but having said where
}

// The walker itself, split out because an MFC ASSERT is the OTHER thing on
// this host that reports a file and line with no path to it -- see
// McTestAssertHook below.
static void McTestPrintStack (const CONTEXT & context, int maxFrames)
{
  HANDLE proc = ::GetCurrentProcess (), thread = ::GetCurrentThread ();
  ::SymSetOptions (SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
  ::SymInitialize (proc, NULL, TRUE);

  CONTEXT ctx = context;              // StackWalk64 modifies it
  STACKFRAME64 sf;
  memset (&sf, 0, sizeof sf);
  sf.AddrPC.Offset    = ctx.Eip;   sf.AddrPC.Mode    = AddrModeFlat;
  sf.AddrFrame.Offset = ctx.Ebp;   sf.AddrFrame.Mode = AddrModeFlat;
  sf.AddrStack.Offset = ctx.Esp;   sf.AddrStack.Mode = AddrModeFlat;

  char symbuf [sizeof (SYMBOL_INFO) + 512];
  SYMBOL_INFO * sym = (SYMBOL_INFO *) symbuf;
  memset (symbuf, 0, sizeof symbuf);
  sym->SizeOfStruct = sizeof (SYMBOL_INFO);
  sym->MaxNameLen   = 511;

  for (int frame = 0; frame < maxFrames; ++frame)
    {
    if (!::StackWalk64 (IMAGE_FILE_MACHINE_I386, proc, thread, &sf, &ctx,
                        NULL, ::SymFunctionTableAccess64, ::SymGetModuleBase64, NULL))
      break;
    if (sf.AddrPC.Offset == 0)
      break;

    DWORD64 disp = 0;
    if (::SymFromAddr (proc, sf.AddrPC.Offset, &disp, sym))
      fprintf (stderr, "  %2d  %s + 0x%lX", frame, sym->Name, (unsigned long) disp);
    else
      fprintf (stderr, "  %2d  0x%08lX", frame, (unsigned long) sf.AddrPC.Offset);

    IMAGEHLP_LINE64 line;
    DWORD lineDisp = 0;
    memset (&line, 0, sizeof line);
    line.SizeOfStruct = sizeof line;
    if (::SymGetLineFromAddr64 (proc, sf.AddrPC.Offset, &lineDisp, &line))
      fprintf (stderr, "   [%s:%lu]", line.FileName, (unsigned long) line.LineNumber);
    fprintf (stderr, "\n");
    }
}

// ── an MFC assertion says WHERE IN MFC, which is never the interesting half ──
//
// MC_TEST_WIN_HOST_INIT routes CRT and MFC diagnostics to stderr so nothing
// blocks on a dialog, which turns a failed ASSERT into one line naming a file
// inside MFC -- `winocc.cpp(242) : Assertion failed!`.  That says a CWnd method
// was called on a window that does not exist, and nothing whatever about which
// engine line called it.  Most of the time that is fine, because this host
// provokes assertions by design and they are noise.  When one is the thing you
// are chasing, it is the only clue there is and it is useless on its own.
//
// So: opt in with MC_TEST_ASSERT_STACKS=1 in the environment and each assertion
// grows a stack.  Off by default because a single suite can raise fifty, and
// fifty stacks is not a log anyone reads.  The cap is per run, not per
// assertion, for the same reason.
//
// The COUNT is exported for the same reason McTestDialogsCancelled is: an MFC
// assertion is an observable, and some code paths have no other one.  MFC
// asserts where a Win32 handle is required and absent -- CAsyncSocket::
// AsyncSelect on INVALID_SOCKET is the case that prompted this -- so "did this
// call reach a state MFC considers illegal" is a question a test can now put
// directly, instead of a human reading stderr afterwards and hoping to notice.
static long s_assertionsRaised = 0;

long McTestAssertionsRaised () { return s_assertionsRaised; }

static int  McTestAssertHook (int reportType, char * message, int * returnValue)
{
  static int s_left = -1;             // -1 = not yet decided
  if (s_left < 0)
    {
    const char * v = getenv ("MC_TEST_ASSERT_STACKS");
    s_left = (v && *v && strcmp (v, "0") != 0) ? 12 : 0;
    }

  if (reportType == _CRT_ASSERT)
    ++s_assertionsRaised;

  // Write the report ourselves (the hook replaces the default handling), then
  // decide whether it earns a stack.
  fputs (message ? message : "", stderr);

  if (reportType == _CRT_ASSERT && s_left > 0)
    {
    --s_left;
    fprintf (stderr, "*** assertion in %s.%s\n",
             mc_test::g ().current_suite ? mc_test::g ().current_suite : "?",
             mc_test::g ().current_name  ? mc_test::g ().current_name  : "?");
    CONTEXT ctx;
    memset (&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    ::RtlCaptureContext (&ctx);
    McTestPrintStack (ctx, 24);
    if (s_left == 0)
      fprintf (stderr, "*** (further assertion stacks suppressed)\n");
    }

  fflush (stderr);
  if (returnValue)
    *returnValue = 0;                 // do NOT break into a debugger
  return TRUE;                        // handled; skip the default reporting
}

// ── every dialog this process creates is cancelled, by one mechanism ────────
//
// A modal dialog is the one failure this host cannot report: the run stops, the
// clock kills it, and the summary never prints.  Two answers were already in
// place and neither is general -- McTestMessageBox catches AfxMessageBox and
// UMessageBox, and McTestCancelDialog is an #ifdef at one CFileDialog site.
// A CDialog has no funnel at all: on Windows it is real MFC in a static
// library, and there are 159 DoModal() call sites in this tree.
//
// A WH_CBT hook is the funnel Windows itself provides.  Every window the
// process creates passes through it before it is shown, so a dialog can be told
// to close before it can block anything -- one mechanism instead of 159 edits,
// and no change to shipping source.
//
// WM_CLOSE rather than WM_COMMAND/IDCANCEL: DefDlgProc turns it into IDCANCEL
// for a dialog that has a Cancel button and closes one that does not, so it
// covers property sheets and the common dialogs as well.
//
// The count is exported because "a dialog was raised" is the only observable a
// test has for a code path that ends in one -- see McTestDialogsCancelled.
static HHOOK s_cbtHook = NULL;
static long  s_dialogsCancelled = 0;

long McTestDialogsCancelled () { return s_dialogsCancelled; }

static LRESULT CALLBACK McTestCbtProc (int code, WPARAM wParam, LPARAM lParam)
{
  if (code == HCBT_ACTIVATE)
    {
    char cls [64] = "";
    ::GetClassNameA ((HWND) wParam, cls, sizeof cls);
    if (strcmp (cls, "#32770") == 0)          // the dialog window class
      {
      ++s_dialogsCancelled;
      ::PostMessageA ((HWND) wParam, WM_CLOSE, 0, 0);
      }
    }
  return ::CallNextHookEx (s_cbtHook, code, wParam, lParam);
}

void McTestInstallCrashHandler ()
{
  ::SetUnhandledExceptionFilter (McTestCrashFilter);
  _CrtSetReportHook (McTestAssertHook);
  s_cbtHook = ::SetWindowsHookExA (WH_CBT, McTestCbtProc, NULL,
                                   ::GetCurrentThreadId ());
}

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
