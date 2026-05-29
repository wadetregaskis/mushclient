// platform/plugin_portable.cpp — CPlugin destructor for mushcore (non-Windows).
//
// CRITICAL: this file must include platform/doc_stub.h (and NOT doc.h /
// plugins.h) so that the compiler sees the same stub CPlugin layout that
// InternalLoadPlugin uses when it calls `new CPlugin(this)`.
//
// windows_stubs.cpp includes doc.h → plugins.h and therefore sees the real,
// larger CPlugin.  Putting CPlugin::~CPlugin() there would produce a
// struct-size mismatch (heap-buffer-overflow on m_bEnabled).

#ifndef _WIN32

#include "platform/doc_stub.h"
#include "scripting/scripting.h"

CPlugin::~CPlugin()
{
    // Fire OnPluginClose on this specific plugin before destroying its engine.
    if (m_ScriptEngine && m_bEnabled) {
        CString strName("OnPluginClose");
        DISPID dispid = m_ScriptEngine->GetLuaDispid(strName);
        if (dispid != DISPID_UNKNOWN) {
            list<double> nparams;
            list<string> sparams;
            long nInvocations = 0;
            m_ScriptEngine->ExecuteLua(dispid, strName,
                                       eInputFromServer,
                                       "plugin", "close",
                                       nparams, sparams,
                                       nInvocations);
        }
    }
    delete m_ScriptEngine;
    m_ScriptEngine = nullptr;
    DELETE_MAP(m_TriggerMap,  CTrigger);
    DELETE_MAP(m_AliasMap,    CAlias);
    DELETE_MAP(m_TimerMap,    CTimer);
    DELETE_MAP(m_VariableMap, CVariable);
}

#endif // _WIN32
