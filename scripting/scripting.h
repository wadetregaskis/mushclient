#pragma once

extern "C"
  {
#ifdef LUA_52
    #include "../../lua52/src/lua.h"
    #include "../../lua52/src/lauxlib.h"
#elif defined(_WIN32)
    #include "../lua.h"
    #include "../lauxlib.h"
#else
    #include "lua.h"
    #include "lauxlib.h"
#endif
  }

#include "paneline.h"

// Opaque per-engine Python state (one module namespace per world/plugin).
// Defined in platform/py_engine.cpp; null unless the world's language is Python.
class CPythonEngine;

#define DOCUMENT_STATE "mushclient.document"
#define WORLD_LIBRARY "world"

void LuaError (lua_State *L, 
               LPCTSTR strEvent = "Run-time error",
               LPCTSTR strProcedure = "",
               LPCTSTR strType = "",
               LPCTSTR strReason = "",
               CMUSHclientDoc * pDoc = NULL);

class CScriptEngine : public CObject
  {

  public:
    CScriptEngine (CMUSHclientDoc * pDoc,
                   const CString strLanguage) // constructor
      {
      m_pDoc = pDoc;
      m_strLanguage = strLanguage;
#ifdef _WIN32
      m_IActiveScript = NULL;
      m_IActiveScriptParse = NULL;
      m_site = NULL;
      m_pDispatch = NULL;
#endif
      L = NULL;
#ifndef _WIN32
      m_pyEngine = NULL;
#endif
      };  // end of constructor

    ~CScriptEngine () // destructor
      {
      DisableScripting ();
      }; // end of destuctor

  bool CreateScriptEngine (void);
  bool Parse (const CString & strCode, const CString & strWhat);

  DISPID GetDispid (const CString & strName);
  DISPID GetLuaDispid (const CString & strName);

  // returns true if script error
  bool Execute (DISPID & dispid,  // dispatch ID, will be set to DISPID_UNKNOWN on an error
                LPCTSTR szProcedure,  // eg. ON_TRIGGER_XYZ
                const unsigned short iReason,  // value for m_iCurrentActionSource
                LPCTSTR szType,   // eg. trigger, alias
                LPCTSTR szReason, // eg. trigger subroutine XXX
                DISPPARAMS & params,  // parameters
                long & nInvocationCount,  // count of invocations
                COleVariant * result    // result of call
                );
  bool ShowError (const HRESULT result, const CString strMsg);
  void DisableScripting (void);

  void OpenLua ();
  void OpenLuaDelayed ();
  void CloseLua ();

//  void RegisterLuaRoutines ();

  bool ParseLua (const CString & strCode, const CString & strWhat);
  // returns true if script error
  bool ExecuteLua (DISPID & dispid,          // dispatch ID, will be set to DISPID_UNKNOWN on an error
                   LPCTSTR szProcedure,      // eg. ON_TRIGGER_XYZ
                   const unsigned short iReason,  // value for m_iCurrentActionSource
                   LPCTSTR szType,           // eg. trigger, alias
                   LPCTSTR szReason,         // eg. trigger subroutine XXX
                   list<double> & nparams,   // list of number parameters
                   list<string> & sparams,   // list of string parameters
                   long & nInvocationCount,  // count of invocations
                   const t_regexp * regexp = NULL,  // regular expression (for triggers, aliases)
                   map<string, string> * table = NULL, // map of other things
                   CPaneLine * paneline = NULL,        // and the line (for triggers)
                   bool * result = NULL);              // where to put result

  // returns true if script error
  bool ExecuteLua (DISPID & dispid,          // dispatch ID, will be set to DISPID_UNKNOWN on an error
                   LPCTSTR szProcedure,      // eg. ON_TRIGGER_XYZ
                   const unsigned short iReason,  // value for m_iCurrentActionSource
                   LPCTSTR szType,           // eg. trigger, alias
                   LPCTSTR szReason,         // eg. trigger subroutine XXX
                   CString strParam,         // string parameter
                   long & nInvocationCount,  // count of invocations
                   CString & result);        // where to put result

  // return value is return from call

  const bool IsLua () const { return L != NULL; }
  lua_State           * L;                  // Lua state

  // IsPython() is defined on every platform (false on Windows, where the COM
  // host — not this embedded engine — runs the alternate languages) so the
  // shared trigger/alias/timer/plugin call sites can write
  // `IsLua() || IsPython()` without per-site #ifdefs.
#ifdef _WIN32
  const bool IsPython () const { return false; }
#else
  // ── Python engine (port) ────────────────────────────────────────────────
  // Real CPython, embedded.  The world API is reached through the portable
  // dispatch-map Invoke (platform/mc_dispatch.cpp), not a hand-written binding.
  // See platform/py_engine.cpp.  m_pyEngine is null for Lua worlds, so
  // IsPython() gates the Python paths without disturbing the others.
  const bool IsPython () const { return m_pyEngine != NULL; }
  void   OpenPython ();
  void   ClosePython ();
  bool   ParsePython (const CString & strCode, const CString & strWhat);
  DISPID GetPythonDispid (const CString & strName);

  // Rich callback — exact mirror of the ExecuteLua overloads above, so the same
  // doc/evaluate/plugins/timers call sites drive it.
  bool ExecutePython (DISPID & dispid, LPCTSTR szProcedure, const unsigned short iReason,
                      LPCTSTR szType, LPCTSTR szReason,
                      list<double> & nparams, list<string> & sparams,
                      long & nInvocationCount, const t_regexp * regexp = NULL,
                      map<string, string> * table = NULL, CPaneLine * paneline = NULL,
                      bool * result = NULL);
  bool ExecutePython (DISPID & dispid, LPCTSTR szProcedure, const unsigned short iReason,
                      LPCTSTR szType, LPCTSTR szReason, CString strParam,
                      long & nInvocationCount, CString & result);

  CPythonEngine * m_pyEngine;               // opaque per-engine Python state
#endif

  private:

#ifdef _WIN32
  IActiveScript       * m_IActiveScript;          // VBscript interface
  IActiveScriptParse  * m_IActiveScriptParse;     // parser
  CActiveScriptSite   * m_site;                   // our local site (world object)
  IDispatch           * m_pDispatch;              // script engine dispatch pointer
#endif

  CMUSHclientDoc      * m_pDoc;                   // related MUSHclient document

  CString               m_strLanguage;        // language, (vbscript, jscript, perlscript)

  };

int RegisterLuaRoutines (lua_State * L);
int DisableDLLs (lua_State * L);
#include "lua_helpers.h"


// ----------- here used for Lua in choosing from combo-box

class CKeyValuePair
  {

  public:
    CKeyValuePair () :
        bNumber_ (false), iKey_ (0.0) { };  // default constructor

    CKeyValuePair (const string sValue) : 
        bNumber_ (false), iKey_ (0.0), sValue_ (sValue) { };  // constructor

    // copy constructor
    CKeyValuePair (const CKeyValuePair & k)
                    : bNumber_ (k.bNumber_), 
                      sKey_ (k.sKey_),
                      iKey_ (k.iKey_),
                      sValue_ (k.sValue_)
                       {};

    // operator =
    const CKeyValuePair & operator= (const CKeyValuePair & rhs)
      {
      bNumber_ = rhs.bNumber_;
      sKey_  = rhs.sKey_;
      iKey_ = rhs.iKey_;
      sValue_ = rhs.sValue_;
      return *this;
      };


  bool   bNumber_; // true if key a number, false if a string

  string sKey_;    // key if string
  double iKey_;    // key if number?

  string sValue_;  // value 

  };   // end of class  CStringValuePair

// call a C function with the Lua environment
void CallLuaCFunction (lua_State * L, lua_CFunction fn);