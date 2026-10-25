// platform/extended_selftest.h — the loop-running ("extended") selftest mode.
//
// WHY IT EXISTS
//
// Every other in-app selftest runs inside wxApp::OnInit and std::exit()s before
// OnRun.  That is deliberate and cheap, but it means the event loop never
// starts, so AppKit never issues drawRect: and no modal dialog can ever
// complete.  Measured, not assumed: 200 forced Refresh(false) + Update() cycles
// on a shown, onscreen pane produce ZERO OnPaint calls, and wxYield() is no
// substitute (docs/AUDIT-2026-08.md).
//
// So a whole family of behaviour is unreachable — the property-sheet page
// lifecycle, the bridged-dialog command routing, the update-UI idle pass, and
// anything that only happens while painting.  This mode reaches it: OnInit
// returns true, the harness runs from a CallAfter once the loop is going, and
// it ends by calling ExitMainLoop.
//
// WHY IT IS OPT-IN
//
// The failure mode is different from every other test here.  A harness that
// forgets to leave the loop does not fail — it HANGS, until ctest's timeout,
// and per CLAUDE.md the casualty of a wedged app launch is often a different,
// untouched test that runs next.  So:
//
//   * it is registered only when MUSHCLIENT_EXTENDED_TESTS is ON (default OFF),
//   * a watchdog turns a hang into an ordinary failure with a real exit code,
//   * and the tests hold a ctest RESOURCE_LOCK so two of them never run at once.
//
// WHY THE ISOLATION MATTERS MORE HERE THAN ANYWHERE ELSE
//
// The other selftests exit before InitInstance, so they never reach the code
// that writes preferences.  This one does.  On macOS wxConfig resolves to
// ~/Library/Preferences/mushclient Preferences — the REAL file belonging to the
// user's installed copy — and $HOME does not redirect it.  So the harness
// points OpenAppConfig (platform/app_config.h) at a scratch file first, and
// ASSERTS the redirection took, because the failure it guards against is
// silently rewriting someone's live settings, which nothing downstream notices.

#pragma once

#ifndef _WIN32

#include <wx/string.h>

// True when argv asks for an extended selftest, and `nameOut` receives which.
// The flag is --extended-selftest=<name>.
bool WantsExtendedSelfTest(int argc, wxChar** argv, wxString& nameOut);

// Arm the mode: install the isolated preferences, schedule `name`'s harness to
// run once the loop is up, and start the watchdog.  Call from OnInit and then
// return true so OnRun starts the loop.  The process exits from inside.
// Returns false for an unknown harness name, having said so -- the caller
// should exit rather than start the loop.
//
// IMPORTANT, and the limit of the isolation: this redirects wxConfig ONLY.
// The app has two other persistent stores -- the sqlite preferences database
// and MUSHclient.ini, both named in CMUSHclientApp::InitInstance -- and
// neither is redirected.  They are out of reach today because the extended arm
// returns from OnInit BEFORE g_wxhook_init_instance(), so InitInstance never
// runs and those paths are never opened.  That is a guarantee of the call
// ORDER, not of anything in this file: a harness that ever needs InitInstance
// would start writing the user's real preferences database, and nothing here
// would notice.  ExtendedSelfTestRequiresIsolation() exists to trip on that.
bool ArmExtendedSelfTest(const wxString& name);

// True once an extended selftest is armed.  InitInstance must NOT run in that
// state -- see above.  Checked at the InitInstance call site.
bool ExtendedSelfTestArmed();

// The process exit code once the loop has ended.  Starts at 1 (failure), so a
// loop that ends WITHOUT the harness having finished -- a window closed by
// hand, an unexpected ExitMainLoop -- reports failure rather than success.
int ExtendedSelfTestExitCode();

// Record one assertion: prints its PASS/FAIL line and adds it to the tally the
// watchdog reports if it has to fire.  Harnesses that live in their own TU need
// this; a private counter there would leave the watchdog's "N passed, M failed
// (incomplete)" line reading 0, 0 for exactly the run where it matters most.
// Returns `ok` so the caller can also keep its own failure count.
bool ExtendedSelfTestCheck(bool ok, const char* what);

#endif // !_WIN32
