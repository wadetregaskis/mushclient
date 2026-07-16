// doc_scripting_shared.cpp
//
// Port-only CMUSHclientDoc scripting callbacks, single-sourced across the two
// non-Windows builds (mushcore + mushapp) so they cannot drift.  They fire the
// on_world_{close,save,get_focus,lose_focus} Lua callbacks and previously
// existed as two identical copies — doc_portable.cpp (mushcore) and
// platform/mushapp_stubs.cpp (mushapp).
//
// These are declared `#ifndef _WIN32` in doc.h, so Windows has no equivalent
// (it fires the same events inline in mushview.cpp / DoSave / OnCloseDocument),
// and this TU is deliberately NOT in MUSHclient.vcxproj — it is compiled only
// into the mushcore and mushapp libraries (both under the `if(NOT WIN32)` block
// in CMakeLists.txt).

#ifndef _WIN32

#if defined(MUSHCLIENT_FULL_BUILD)
  #include "stdafx.h"
  #include "MUSHclient.h"
  #include "doc.h"
#else
  #include "platform/doc_stub.h"
#endif

#include "scripting/scripting.h"   // CScriptEngine

#include <list>
#include <string>
using std::list;
using std::string;

// Fired on world/document teardown so scripts can do final cleanup.
void CMUSHclientDoc::FireWorldClose()
{
    if (m_dispidWorldClose == DISPID_UNKNOWN) return;
    CScriptEngine* engine = GetScriptEngine();
    if (!engine || !engine->IsLua()) return;
    list<double> nparams;
    list<string> sparams;
    long nInvocations = 0;
    engine->ExecuteLua(m_dispidWorldClose, m_strWorldClose,
                       eWorldAction, "world", "close",
                       nparams, sparams, nInvocations);
}

// Called from OnSaveDocument before the file is written (mirrors the
// ExecuteScript(m_dispidWorldSave,...) call in doc.cpp DoSave() on Windows).
void CMUSHclientDoc::FireWorldSave()
{
    if (m_dispidWorldSave == DISPID_UNKNOWN) return;
    CScriptEngine* engine = GetScriptEngine();
    if (!engine || !engine->IsLua()) return;
    list<double> nparams;
    list<string> sparams;
    long nInvocations = 0;
    engine->ExecuteLua(m_dispidWorldSave, m_strWorldSave,
                       eWorldAction, "world", "save",
                       nparams, sparams, nInvocations);
}

// Fire on_world_get_focus / on_world_lose_focus (from wx_world_child OnActivate;
// mirrors mushview.cpp OnSetFocus / OnKillFocus on Windows).
void CMUSHclientDoc::FireWorldGetFocus()
{
    if (m_dispidWorldGetFocus == DISPID_UNKNOWN) return;
    CScriptEngine* engine = GetScriptEngine();
    if (!engine || !engine->IsLua()) return;
    list<double> nparams;
    list<string> sparams;
    long nInvocations = 0;
    engine->ExecuteLua(m_dispidWorldGetFocus, m_strWorldGetFocus,
                       eWorldAction, "world", "get focus",
                       nparams, sparams, nInvocations);
}

void CMUSHclientDoc::FireWorldLoseFocus()
{
    if (m_dispidWorldLoseFocus == DISPID_UNKNOWN) return;
    CScriptEngine* engine = GetScriptEngine();
    if (!engine || !engine->IsLua()) return;
    list<double> nparams;
    list<string> sparams;
    long nInvocations = 0;
    engine->ExecuteLua(m_dispidWorldLoseFocus, m_strWorldLoseFocus,
                       eWorldAction, "world", "lose focus",
                       nparams, sparams, nInvocations);
}

#endif // !_WIN32
