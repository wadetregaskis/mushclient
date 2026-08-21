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
#include "mc_test.h"

MC_TEST_MAIN_ALWAYS()
