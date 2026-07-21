// doc_scripting_shared.cpp
//
// Port-only CMUSHclientDoc scripting callbacks: on_world_close,
// on_world_get_focus and on_world_lose_focus.  (on_world_save is NOT here — it
// is fired by the canonical doc.cpp::DoSave, which the port's save paths now go
// through; see the note further down.)
//
// These are declared `#ifndef _WIN32` in doc.h, so Windows has no equivalent —
// it fires the same events inline in mushview.cpp / OnCloseDocument — and this
// TU is deliberately NOT in MUSHclient.vcxproj; it is compiled only into
// mushcore, under the `if(NOT WIN32)` block in CMakeLists.txt.

#ifndef _WIN32

  #include "stdafx.h"
  #include "MUSHclient.h"
  #include "doc.h"

#include "scripting/scripting.h"   // CScriptEngine

// The three helpers mirror the corresponding Windows call sites exactly:
//   close      doc.cpp:2155      (OnCloseDocument)
//   get focus  mushview.cpp:3494 (OnActivateView)
//   lose focus mushview.cpp:3527 (OnActivateView, deactivate arm)
// They used to gate on `engine->IsLua()` and call engine->ExecuteLua directly,
// which made every world-level script handler Lua-only — Python/Perl/JS/Tcl/Ruby
// worlds silently got nothing.  ExecuteScript dispatches through the portable
// mc_dispatch Invoke, so one call covers every language.
//
// Two details that must match Windows:
//  * gate on m_ScriptEngine, NOT GetScriptEngine() — the latter returns the
//    CURRENT PLUGIN's engine when one is executing, and these are world-level
//    handlers.  (ExecuteScript itself still resolves via GetScriptEngine(),
//    exactly as on Windows.)
//  * call SeeIfHandlerCanExecute first and honour a false return.  It is not
//    merely a guard: a handler name beginning with `!` means "run the rest as a
//    command", which it does itself before returning false.  The old
//    `m_dispid... == DISPID_UNKNOWN` early-return ran BEFORE that and so killed
//    the `!command` form outright — a `!` handler never resolves to a dispid.
//    ExecuteScript already ignores DISPID_UNKNOWN (doc_shared.cpp:2342), so no
//    dispid pre-check is needed here.

// Fired on world/document teardown so scripts can do final cleanup.
void CMUSHclientDoc::FireWorldClose()
{
    if (!m_ScriptEngine) return;
    if (!SeeIfHandlerCanExecute (m_strWorldClose)) return;
    DISPPARAMS params = { NULL, NULL, 0, 0 };
    long nInvocationCount = 0;
    ExecuteScript (m_dispidWorldClose, m_strWorldClose, eWorldAction,
                   "world close", "closing world", params, nInvocationCount);
}

// There is deliberately no FireWorldSave here.  It existed, claimed in its own
// comment to be "called from OnSaveDocument", and was in fact called from
// nowhere at all — so on_world_save and OnPluginWorldSave were both dead on the
// port.  The fix was to route the three save entry points (File > Save/Save As
// and world.Save()) through the canonical CMUSHclientDoc::DoSave, which already
// fires both, rather than calling OnSaveDocument directly.  Keeping a helper
// here would just be a decoy that looks wired.

// Fire on_world_get_focus / on_world_lose_focus (from wx_world_child OnActivate;
// mirrors mushview.cpp OnActivateView on Windows).
void CMUSHclientDoc::FireWorldGetFocus()
{
    if (!m_ScriptEngine) return;
    if (!SeeIfHandlerCanExecute (m_strWorldGetFocus)) return;
    DISPPARAMS params = { NULL, NULL, 0, 0 };
    long nInvocationCount = 0;
    ExecuteScript (m_dispidWorldGetFocus, m_strWorldGetFocus, eWorldAction,
                   "world get focus", "getting focus", params, nInvocationCount);
}

void CMUSHclientDoc::FireWorldLoseFocus()
{
    if (!m_ScriptEngine) return;
    if (!SeeIfHandlerCanExecute (m_strWorldLoseFocus)) return;
    DISPPARAMS params = { NULL, NULL, 0, 0 };
    long nInvocationCount = 0;
    ExecuteScript (m_dispidWorldLoseFocus, m_strWorldLoseFocus, eWorldAction,
                   "world lose focus", "losing focus", params, nInvocationCount);
}

#endif // !_WIN32
