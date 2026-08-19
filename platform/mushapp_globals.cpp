// platform/mushapp_globals.cpp
// Definitions of global singletons used throughout the MUSHclient codebase
// on non-Windows platforms when building the full (mushapp) library.
//
// In the portable/mushcore build these are provided by doc_portable.cpp.
// In the full/mushapp build doc_portable.cpp is excluded (its function bodies
// would conflict with the real implementations in doc.cpp etc.), so we
// provide ONLY the global variable definitions here.
//
// Included in CMake target: mushapp (not mushcore).

#ifndef _WIN32

#include "stdafx.h"
#include "MUSHclient.h"   // full CMUSHclientApp definition
#include "mainfrm.h"      // full CMainFrame definition

// ── Debug flag ────────────────────────────────────────────────────────────────
// Enables verbose protocol tracing when set to true at runtime.
bool g_debug = false;

// ── Line-ending constant ──────────────────────────────────────────────────────
// Used throughout the source (doc.cpp, Mapping.cpp, mushview.cpp, etc.).
// Originally defined in doc_portable.cpp; provided here for the full build.
extern const char ENDLINE[] = "\r\n";

// ── Application singleton ─────────────────────────────────────────────────────
// Every non-Windows translation unit references App (declared extern in
// stdafx.h).  MUSHclient.cpp defines a
// separate `theApp` (MFC naming convention) that is effectively unused on
// non-Windows; App is the object the rest of the code reaches for.
CMUSHclientApp App;

// ── Main-frame stub ────────────────────────────────────────────────────────────
// stdafx.h (non-Windows block) declares:
//   extern CMainFrame Frame;      ← literal symbol "Frame"
CMainFrame Frame;         // satisfies stdafx.h's extern declaration

#endif // !_WIN32
